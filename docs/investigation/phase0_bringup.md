# OCUDU L1 Investigation — Phase 0: CUDA Fork Bring-Up for A100X (sm_80/x86_64)

## Executive summary

- The CUDA fork (`cuda_accel.26_04`, HEAD `ccdf4e681f`) **configures and compiles cleanly for
  `CMAKE_CUDA_ARCHITECTURES=80` on x86_64** with the exact CUDA toolkit version the fork was
  developed with (nvcc 13.0.88). All 17 CUDA kernel translation units in `lib/phy/cuda/src/` plus
  the LDPC/PDSCH adapter kernels compiled with zero errors, and `cuobjdump --list-elf` confirms
  every object carries **sm_80 SASS** (real Ampere machine code, not PTX-only).
- IMPORTANT CAVEAT: this session's remote container has **no GPU** (no driver, no `/dev/nvidia*`).
  The compile/link gate for sm_80 is fully answered here; the **runtime gate (item 3, end-to-end
  benchmark execution) requires the A100X box** and is handed over as an exact command list below.
  Linking without a GPU was made possible by the CUDA driver *stub* library.
- No Hopper/Blackwell-only, CUDA-13-only, or coherent-memory constructs blocked the sm_80 build.
  GPU FFTs are done by vendored **VkFFT** (`lib/phy/cuda/third_party/vkfft`, `VKFFT_BACKEND=1`),
  which JIT-compiles FFT kernels **at runtime via NVRTC for the detected device arch** — arch-
  portable by design, but it means GPU FFT plan creation costs a runtime compilation on first use.
- **CUDA 12.9 compile-compat check: PASSED.** A second build tree configured with nvcc 12.9.86
  (`build-cuda129/`, same flags, `CMAKE_CUDA_ARCHITECTURES=80`) compiled `ocudu_phy_cuda` and
  `ocudu_ldpc_cuda` with zero errors. Conclusion: the fork does NOT require CUDA-13-only APIs;
  a CUDA 12.9 toolchain is sufficient. Practical consequence for the A100X box: if its driver
  predates the r580 series (CUDA 13 requirement), build with CUDA 12.x instead — no source
  changes needed.

## Environment (this container — NOT the A100X box)

| Item | Value |
|---|---|
| OS / kernel | Ubuntu 24.04.4 LTS, Linux 6.18.5, x86_64 |
| CPU | 4 vCPU, AVX2 + AVX512F present |
| RAM / disk | 15 GB / ~30 GB free |
| GPU | **none** (no driver, no device nodes) |
| Host compiler | gcc/g++ 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1) — same GCC line as the fork's GB10 toolchain |
| CMake | 3.28.3 |
| CUDA toolkit | 13.0.88 (nvcc `V13.0.88`), assembled from conda `nvidia` channel into `/opt/cuda13`; matches README's documented dev toolkit "CUDA toolkit 13.0.88" exactly |
| FFTW | fftw3f 3.3.10 (FFT backend; MKL absent/pending, FFTZ absent) |
| DPDK | 23.11.4 (fork requires >= 22.11 → satisfied, `ENABLE_DPDK=ON`) |
| ZeroMQ | system libzmq found, `ENABLE_ZEROMQ=ON` |
| UHD | OFF |
| GTest | 1.14.0 |

Network policy notes: NVIDIA's apt repo (developer.download.nvidia.com) is blocked in this
container; the toolkit was assembled from the `nvidia` conda channel (reachable), using
`cuda-nvcc-impl`, `cuda-cudart-dev`, `cuda-cccl_linux-64`, `libcurand-dev`, `cuda-nvrtc-dev`,
`cuda-driver-dev` (driver link stub) at version 13.0. The PyPI `nvidia-cuda-nvcc-cu12` wheels are
NOT a full compiler (they ship only `ptxas` + nvvm) — dead end if you try that route.

## Exact configure/build commands used

```bash
git clone --branch cuda_accel.26_04 --single-branch \
  https://gitlab.com/ocudu/work_groups/wg1_hw_accel/cuda_accelerated_ocudu.git

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_CUDA=ON \
  -DCMAKE_CUDA_ARCHITECTURES=80 \
  -DCMAKE_CUDA_COMPILER=/opt/cuda13/bin/nvcc \
  -DCUDAToolkit_ROOT=/opt/cuda13 \
  -DCMAKE_CUDA_HOST_COMPILER=/usr/bin/g++ \
  -DENABLE_DPDK=ON -DENABLE_ZEROMQ=ON -DENABLE_UHD=OFF -DENABLE_EXPORT=OFF
```

Configure output (key lines):

```
-- The CUDA compiler identification is NVIDIA 13.0.88
-- Found CUDAToolkit: /opt/cuda13/targets/x86_64-linux/include (found version "13.0.88")
-- CUDA support enabled
-- Found fftw3f, version 3.3.10
-- Found libdpdk, version 23.11.4
-- Building OCUDU version 26.04.0
-- The build type is Release
```

Per the platform rules: `-DMCPU=neoverse-v2` was **not** passed (that is the GB10/aarch64
workaround; on x86 the default `-march=native` path applies). `CMAKE_CUDA_ARCHITECTURES=80`, not
121. CUDA enablement in the fork is plain `check_language(CUDA)` + `find_package(CUDAToolkit)`
(CMakeLists.txt:87-104) — no vendor lock, no arch allowlist, no minimum-version gate beyond what
the code itself needs.

## Build results

1. **Core CUDA kernel libraries — clean.** `make ocudu_phy_cuda ocudu_ldpc_cuda`:
   all CUDA TUs compiled with zero errors/warnings:
   `ldpc_decoder, modulation, rate_matching, crc, transport_block, nr_constants, scrambling,
   pusch_e2e, srs_estimator, pdsch_fused, polar, low_phy_prach_rx, low_phy_puxch_rx, low_phy_tx,
   ofh_compression, prach_detector` (lib/phy/cuda/src/*.cu) and
   `pusch_sch_llr_compactor, pdsch_resource_grid_mapper_cuda, pdsch_tb_encoder_cuda`
   (lib/phy/upper/channel_coding/ldpc/cuda/*.cu).
2. **sm_80 SASS verified**: `cuobjdump --list-elf` over every object in `ocudu_phy_cuda` reports
   `sm_80` (17/17 objects).
3. **Benchmarks/tests/gnb link — clean.** All targets built and linked with zero errors:
   `pusch_processor_benchmark`, `pdsch_processor_benchmark`, `ldpc_decoder_gpu_cpu_test`,
   `ldpc_encoder_gpu_cpu_test`, `pusch_gpu_cpu_comparison_test`, `pdsch_gpu_e2e_test`,
   `prach_detector_cuda_test`, `srs_estimator_cuda_test`, `ofdm_demodulator_cuda_test`,
   `ofdm_prach_demodulator_cuda_test`, `pdxch_baseband_modulator_cuda_test`,
   `ofh_iq_compression_cuda_test`, and the full **`gnb`** application (72 MB, links
   `libcudart.so.13` + `libnvrtc.so.13`; `libcuda.so.1` resolves from the driver on a real
   GPU host). Note: `pdsch_processor_benchmark` lands at
   `build/tests/benchmarks/phy/upper/channel_processors/pdsch_processor_benchmark` (not in a
   `pdsch/` subdir).
4. **Smoke execution (CPU mode, this container).** `pusch_processor_benchmark -m latency -B 5
   -R 2 -T 1 -P scs30_100MHz_256qam_rv0_4port_nlayer` runs end-to-end (auto-selected `avx512`
   LDPC decoder; 273 PRB, 256QAM, layers 1-4). Absolute numbers from this 4-vCPU container are
   NOT meaningful and are not reported as measurements. With `-G` and no GPU the binary prints
   an explicit `GPU pipeline not available, falling back to CPU` — so a silent-fallback
   misreading on the A100X is detectable by absence of that line plus
   `OCUDU_PUSCH_ACCELERATION_TRACE=1` trace output.

## Ampere/discrete-GPU compatibility findings (build-level)

- Full audit (arch guards, memory policies, coherence assumptions) in
  `phase1_sm80_portability.md`. Headline: no blocker; two `__CUDA_ARCH__ >= 860` fast-path
  exclusions in the LDPC decoder (A100X takes the slower fallback; the code comment wrongly
  claims A100 is covered).
- VkFFT/NVRTC: all six GPU FFT consumers (`pusch_e2e.cu`, `low_phy_puxch_rx.cu`,
  `low_phy_prach_rx.cu`, `prach_detector.cu`, `srs_estimator.cu`, `low_phy_tx.cu`) use vendored
  VkFFT with the CUDA backend (`lib/phy/cuda/CMakeLists.txt:163` sets `VKFFT_BACKEND=1`), which
  links `CUDA::nvrtc` and `CUDA::cuda_driver` (lib/phy/cuda/CMakeLists.txt:169-170) and compiles
  FFT kernels at runtime for the device it finds — expected to work on sm_80, verify at runtime.

## Runtime gate — to execute on the A100X box (blocked here: no GPU)

Prerequisites on the A100X machine: NVIDIA driver new enough for the chosen toolkit
(CUDA 13.x needs r580+; if the box runs an r535/r550/r560 driver, use a CUDA 12.x toolkit —
see the 12.9 compile-compat result above), plus the same apt deps
(`libfftw3-dev libmbedtls-dev libsctp-dev libyaml-cpp-dev libzmq3-dev libdpdk-dev libgtest-dev libgmock-dev`).

```bash
# 1. Confirm platform
nvidia-smi                        # expect A100X, note driver version
nvcc --version                    # note toolkit version

# 2. Build (same flags as above; drop the two /opt/cuda13 overrides if a system
#    CUDA toolkit is installed)
cmake --build build -j $(nproc) --target \
  pusch_processor_benchmark pdsch_processor_benchmark gnb

# 3. Prove the GPU path executes and is actually taken (not silent CPU fallback):
#    -G enables the full GPU pipeline (demodulator + resident LDPC decoder)
#    (flag verified at tests/benchmarks/phy/upper/channel_processors/pusch/
#     pusch_processor_benchmark.cpp:310,366-424; CUDA availability check + explicit
#     "Falling back to CPU" warning at :575-580 — if you see that line, the GPU
#     path was NOT taken)
OCUDU_PUSCH_ACCELERATION_TRACE=1 \
  ./build/tests/benchmarks/phy/upper/channel_processors/pusch/pusch_processor_benchmark \
  -m latency -B 100 -T 4 -P scs30_100MHz_256qam_rv0_4port_nlayer -G

# CPU reference, same shape:
  ./build/tests/benchmarks/phy/upper/channel_processors/pusch/pusch_processor_benchmark \
  -m latency -B 100 -T 4 -P scs30_100MHz_256qam_rv0_4port_nlayer

# 4. First A100X data point for the README comparison (Phase 2 will do the full sweep):
bash scripts/cuda_accel/gpu_benchmark_sweep.sh   # defaults to the 4-port nlayer profile
```

Profiles available for our target shape (pusch_processor_benchmark.cpp:196-244):
`scs30_100MHz_256qam_rv0_4port_nlayer` (default in sweeps), `scs30_100MHz_256qam_rv0_4port_mimo`
(4 RX ports — matches the 4T4R n77 OTA reality), `..._8port_mimo`, `..._2port_mimo`,
`..._rvall_1port_1layer`.

## Fork delta shape (context for Phase 1)

The CUDA work is essentially ONE feature commit on the 26.04 base:
`9fd4047b43 "feat(cuda): add resident CUDA PHY acceleration"` — 403 files, +152,052/−674 lines —
plus a docs/README commit (`ccdf4e681f`). `git diff 092414aac2..ccdf4e681f` is the authoritative
delta. Note also from the README (lines 108-110): on GB10 itself the GPU is SLOWER than the CPU
at small shapes (20 MHz 1-layer PUSCH 225.8 µs CPU vs 262.3 µs GPU; PDSCH 29.4 µs CPU vs 79.9 µs
GPU) — the headline ~21x PUSCH number is shape-dependent (large aggregate batches), which supports
measuring OUR target shape on OUR platform before believing any transfer of those numbers.
