# Ampere (sm_80) / CUDA-Version / Coherent-Memory Portability Audit

Audit of the CUDA delta (`git diff 092414aac2..ccdf4e681f`) for anything that blocks or degrades
the A100X (GA100, sm_80, discrete PCIe, x86_64). Method: full compile verification with two
toolkits + targeted source audit of arch guards, memory-policy APIs, and device-attribute checks.
Vendored third-party code (`lib/phy/cuda/third_party/vkfft`) is assessed separately.
Each finding is labeled **verified** (build evidence or code read) or **hypothesis**.

## 1. Compile/link verdict — verified

| Check | Result |
|---|---|
| Configure `ENABLE_CUDA=ON`, `CMAKE_CUDA_ARCHITECTURES=80`, x86_64, gcc 13.3 | PASS (CUDA 13.0.88 and 12.9.86) |
| Compile all CUDA TUs (`ocudu_phy_cuda`, `ocudu_ldpc_cuda`) | PASS, zero errors, both toolkits |
| sm_80 SASS present in every object (`cuobjdump --list-elf`) | PASS (17/17 objects) |
| Link all CUDA tests/benchmarks + full `gnb` | PASS |
| CMake arch/version gates | none — plain `check_language(CUDA)` + `find_package(CUDAToolkit)` (CMakeLists.txt:87-104); no minimum CUDA version enforced, no arch allowlist |

Conclusion (verified): **no CUDA-13-only API usage** (12.9 compiles everything) and **no
Hopper/Blackwell-only compile-time constructs**. CUDA 12.9 is a valid floor for A100X boxes whose
driver predates CUDA 13 (r580 series).

## 2. `__CUDA_ARCH__` inventory — verified (3 sites in the delta, excluding vendored VkFFT)

| Site | Guard | On sm_80 (A100X) | Impact |
|---|---|---|---|
| `lib/phy/cuda/src/rate_matching.cu:89` | `>= 700` | fast path taken (native fp16 atomicCAS `atomicAddSaturatingHalf`) | none |
| `lib/phy/cuda/src/ldpc_decoder_flexible.cu:1409` (`box_plus_half2`) | `>= 860` | **fallback taken** | perf degradation, see below |
| `lib/phy/cuda/src/ldpc_decoder_flexible.cu:1432` (`box_plus_half`) | `>= 860` | **fallback taken** | perf degradation, see below |

**Finding (verified guard, hypothesis on magnitude): the LDPC min-sum box-plus fast path excludes
the A100X, and the code comment believes otherwise.** The `>= 860` branch emits a single PTX
`min.xorsign.abs.f16x2` instruction; its comment says "SM86+ (A100/H100/GH200)" — but **A100 is
sm_80**, which does not implement that instruction (it exists on sm_86/sm_89/sm_90+, and GB10
sm_121 gets it). On the A100X every box-plus in the min-sum inner loop — the hottest operation of
the hottest UL kernel — becomes ~4 instructions (`__habs2`+`__hmin2`+sign XOR+OR) instead of 1.
The guard value itself is *correct* for the hardware; what is wrong is the comment's claim that
A100 benefits. Phase 2 should quantify the per-CB decode cost on A100X vs the README's GB10
numbers with this in mind; Phase 3 may explore an sm_80-tuned fallback. This cannot "block" sm_80
— it is a documented-intent vs actual-arch mismatch, not a bug.

Negative results (verified by grep over the delta, excluding vkfft): no `wgmma`, `tcgen05`,
`cp.async.bulk`, thread-block clusters (`__cluster_dims__`, `cudaFuncAttributeClusterDim`),
`setmaxnreg`, or `__nv_fp8*` usage. FP16 (`__half`, used for LLRs) and BF16 (`__nv_bfloat16`,
used for device grid data) are used broadly and are fully supported on sm_80.

## 3. Shared-memory / occupancy assumptions

`lib/phy/cuda/src/ldpc_decoder_flexible.cu:2044` queries
`cudaDevAttrMaxSharedMemoryPerBlockOptin` at runtime and sizes kernels from the query — adapts to
GA100's 164 KB opt-in limit rather than assuming GB10's. **Verified for this site.** A100X (108
SMs, 40 MB L2) vs GB10 occupancy behavior of every kernel remains to be measured on hardware
(**hypothesis** that no kernel regresses catastrophically; nothing found that hard-fails).

## 4. Coherent/unified-memory assumptions — the load-bearing part

The fork's memory policies actively *detect* discrete GPUs and change behavior, rather than
assuming coherence (all verified by code read):

| Policy site | Check | A100X (discrete) behavior |
|---|---|---|
| `lib/phy/upper/resource_grid_cuda_visible_impl.h:658-672` (`should_use_managed_grid_auto`) | `cudaDevAttrIntegrated` | managed (CUDA-visible) grid **disabled**; falls back to `resource_grid_pinned_factory` — host grid pinned via `cudaHostRegister` (`resource_grid_pinned_impl.h:58`), GPU reads it over PCIe or via explicit staging copies |
| `lib/phy/upper/phy_acceleration_resource_grid_factory.cpp:14-32` (`phy_acceleration_cuda_current_device_is_discrete`) | `cudaDevAttrIntegrated` | exported discrete-GPU predicate used by factories |
| `lib/phy/upper/channel_processors/pdsch/pdsch_block_processor_gpu_impl.cpp:41-77` | `cudaDevAttrIntegrated` | GPU RE-mapper (sidecar grid) **disabled**; comment states the discrete path "copies and scans the full resource grid back to host before TX. That is slower than the host mapper" — the fork's authors explicitly concede the discrete-GPU cost |
| `lib/phy/upper/upper_phy_factories.cpp:51-73` | discrete predicate | PDSCH acceleration `auto` → **disabled with warning** on discrete (override: `OCUDU_PDSCH_AUTO_ENABLE_DISCRETE=1`) |
| `lib/phy/upper/phy_acceleration_prach_buffer_factory.cpp:22-34` | `cudaDevAttrManagedMemory` (not `integrated`) | PRACH buffer **is** `cudaMallocManaged` on A100X too; discrete access implemented via UVM page migration + `cudaMemPrefetchAsync` (`prach_buffer_cuda_visible_impl.h:352-374`, prefetch default ON `:385`) |
| `lib/ofh/compression/compression_factory.cpp:54-57` | **no discrete check** | OFH `auto` selects the CUDA compressor whenever the backend exists — per-call H2D/D2H + blocking `cudaStreamSynchronize` on OFH threads on A100X (top `auto` mis-selection suspect; must be measured in Phase 2) |

Comments referencing coherent platforms ("direct NVLink DMA", `pusch_demodulator_gpu_impl.cpp:2312`;
"GH200 … zero-copy", `resource_grid_pinned_impl.h:18`; "GB10/B210 OTA-stable default",
`pdsch_grid_output_strategy.cpp:25-27`) describe the fast platform; the code paths behind them
still execute correct explicit copies on discrete GPUs. **No case found where host code
dereferences device memory (or vice versa) without a policy/copy guard** in the audited
production paths; residual risk on less-traveled paths is a hypothesis to be retired by running
the fork's correctness sweeps on the A100X.

If `OCUDU_CUDA_VISIBLE_GRID=managed` is *forced* on the A100X, host access after GPU writes
becomes a real page migration + blocking `cudaStreamSynchronize` per reader
(`resource_grid_cuda_visible_impl.h:558-570`) — legal, but expected slow; measure before use.

## 5. Runtime-JIT dependency (VkFFT/NVRTC) — verified

All GPU FFTs (six consumers: `pusch_e2e.cu`, `low_phy_puxch_rx.cu`, `low_phy_prach_rx.cu`,
`prach_detector.cu`, `srs_estimator.cu`, `low_phy_tx.cu`) go through vendored VkFFT
(`VKFFT_BACKEND=1`, `lib/phy/cuda/CMakeLists.txt:163`), which JIT-compiles FFT kernels via NVRTC
for the *detected* device at plan-creation time — arch-portable to sm_80 by construction
(hypothesis until executed: NVRTC of both 12.9/13.0 supports sm_80 targets). Consequences:
`libnvrtc` is a runtime dependency of `gnb`, and first-use plan creation has a JIT cost
(init-time, not per-slot).

## 6. Overall verdict

**Expected to compile and run on sm_80/A100X: YES (compile verified here; run pending on the
A100X box).** No blocking arch or API dependency found. The platform-relevant deltas are
performance-policy ones: two sm_86+ fast-path exclusions in the LDPC decoder, PDSCH acceleration
auto-disabled on discrete, pinned-host-grid mode forcing per-slot PCIe grid staging, and an OFH
compression `auto` default that ignores the discrete-GPU cost model.
