# OCUDU L1 Investigation — Phase 0 (A100X Bring-Up) + Phase 1 (Architecture Map)

Investigation of OCUDU L1 (CPU + CUDA) targeting an NVIDIA A100X (GA100, sm_80, discrete PCIe,
x86_64) host, based on the WG1 CUDA fork branch `cuda_accel.26_04`
(HEAD `ccdf4e681f` = 26.04 base `092414aac2` + CUDA feature commit `9fd4047b43`).
Phases 0-1 only; no source modified. All file:line references are relative to the fork checkout.

## TL;DR

1. **Phase 0 gate: the CUDA fork builds for sm_80 on x86_64 — verified.** Clean compile with both
   CUDA 13.0.88 (the fork's own dev toolkit version) and CUDA 12.9.86 (⇒ no CUDA-13-only APIs);
   sm_80 SASS confirmed in every kernel object; all CUDA tests/benchmarks and the full `gnb` app
   link; the PUSCH benchmark executes (CPU mode). The **runtime** half of the gate (GPU execution)
   could not run in this session's container — it has no GPU — and is handed over as an exact,
   source-verified command list for the A100X machine (see `phase0_bringup.md`).
2. **No sm_80 blocker found.** Only three `__CUDA_ARCH__` guards exist in the whole delta; the two
   `>= 860` ones exclude the A100X from the LDPC min-sum single-instruction box-plus fast path
   (PTX `min.xorsign.abs.f16x2`) — and their comment wrongly claims A100 is covered ("SM86+
   (A100/H100/GH200)", `ldpc_decoder_flexible.cu:1410`). A100X takes the ~4-instruction fallback
   in the hottest UL decode loop. Not a correctness issue; a marked Phase-2 measurement target.
3. **The discrete-PCIe thesis is confirmed at code level (measurement still pending).** The fork
   itself treats discrete GPUs as a second-class memory platform:
   - resource grids: `auto` gives **pinned host grids** on discrete (managed/CUDA-visible grids
     require `cudaDevAttrIntegrated != 0`), so every GPU consumer stages the grid over PCIe
     (~0.73 MB per PUSCH slot H2D at 273 PRB/4 port; same again per SRS occasion);
   - PDSCH acceleration: `auto` **disables it on discrete GPUs with a warning**
     (`upper_phy_factories.cpp:51-73`), and the GPU RE-mapper is off on discrete because the
     sidecar grid "copies and scans the full resource grid back to host before TX"
     (`pdsch_block_processor_gpu_impl.cpp:66-75` — the authors' own words);
   - OFH IQ compression: `auto` **selects the CUDA backend even on discrete** (no integrated
     check, `compression_factory.cpp:54-57`) at a cost of per-symbol H2D/D2H + blocking
     `cudaStreamSynchronize` on OFH threads (~56+ blocking syncs/slot) — the top candidate for
     an `auto` mis-selection on A100X;
   - lower-PHY GPU OFDM demod (split 8): requires a device-mappable grid and **silently falls
     back to CPU per call** on discrete pinned grids (`ofdm_demodulator_cuda_impl.cpp:271`).
   In resident PUSCH mode the *mandatory* per-slot PCIe volume is modest (~1 MB UL + ~0.5 MB DL
   per cell); the code-level risks are sync-per-call patterns (OFH), the non-resident PUSCH
   fallback (LLRs cross PCIe three times, ~7.5 MB/PDU), and SRS full-grid staging — all
   quantified per-transfer in `phase1_pipeline_transfers.md` §3-4. GB10's headline numbers rely
   on paths (device-readable grids, GPU RE mapping) that are **switched off** on A100X.
4. **CPU L1 remains the shared 26.04 base** — the CUDA commit only touches factory glue in the
   CPU subtree. SIMD coverage is strong (AVX2/AVX512 LDPC enc/dec/dematcher, CLMUL CRC, AVX512
   modulation mapper, SIMD-templated equalizer/demapper/precoder) with identified generic-only
   gaps listed in `phase1_cpu_l1_map.md` (CPU-track optimization candidates).

## Report index

| File | Contents |
|---|---|
| `phase0_bringup.md` | Full Phase 0: environment, exact commands, build results, smoke runs, A100X runtime handover |
| `phase1_cpu_l1_map.md` | CPU L1: upper PHY map + SIMD variant/dispatch table; lower PHY, FFT backends, srsvec SIMD coverage, threading |
| `phase1_cuda_map.md` | CUDA delta: runtime infra (streams/memory/device), config + auto-mode selection model, upper-PHY backends + resident-buffer contract, lower-PHY + OFH backends |
| `phase1_pipeline_transfers.md` | Per-slot UL/DL pipelines in accelerated mode + complete host↔device transfer/sync inventory with sizes, and the discrete-vs-coherent delta |
| `phase1_sm80_portability.md` | sm_80 / CUDA-version / coherent-memory portability audit (arch guards, memory policies, VkFFT/NVRTC) |

## What `auto` resolves to on the A100X (summary table)

| Component | `auto` on A100X (discrete sm_80) | Evidence |
|---|---|---|
| PUSCH (demod+decode, resident) | **ON** — grid staged H2D ~0.73 MB/slot; one poll-sync per PDU | `pusch_demodulator_gpu_impl.cpp:2331`, `pusch_codeblock_decoder_cuda_batch.cpp:1449` |
| PDSCH block processor | **OFF** (discrete guard + warning; env `OCUDU_PDSCH_AUTO_ENABLE_DISCRETE=1` to force) | `upper_phy_factories.cpp:51-73` |
| PDSCH GPU RE mapper / sidecar grid | **OFF** on discrete even when PDSCH accel forced | `pdsch_block_processor_gpu_impl.cpp:41-77` |
| UL/DL CUDA-visible (managed) grid | **OFF** → pinned host grid (`cudaHostRegister`) | `resource_grid_cuda_visible_impl.h:658-674` |
| PRACH detection (upper) | **ON** — managed buffer + UVM prefetch works on discrete | `phy_acceleration_prach_buffer_factory.cpp:22-34` |
| SRS estimator | **ON** — full-grid H2D per occasion (no device-readable grid) | `srs_estimator_cuda_impl.cpp:356-369` |
| Lower-PHY TX baseband (split 8) | ON — H2D grid, D2H baseband per slot | `low_phy_tx.cu:973-987, 779-828` |
| Lower-PHY RX OFDM demod (split 8) | **CPU fallback per call** (needs device-mappable grid) | `ofdm_demodulator_cuda_impl.cpp:271`, `modulation_factories.cpp:229-252` |
| Lower-PHY PRACH OFDM demod | ON (managed PRACH buffer) | `low_phy_prach_rx.cu:674-739` |
| OFH IQ compression/decompression | **ON — no discrete check** (blocking sync per call on OFH threads) | `compression_factory.cpp:54-57`, `ofh_compression.cu:170-183` |

## Proposed Phase 2 focus (pending review — nothing started)

1. On the A100X: run the runtime gate (`phase0_bringup.md` §Runtime gate), then the README
   PUSCH/PDSCH sweeps at 273 PRB with **4 RX ports** and the OFH BFP compression matrix
   (`auto`-selected CUDA vs forced CPU) — the latter directly tests the top mis-selection suspect.
2. Nsight Systems on the PUSCH resident path: split per-slot GPU time into kernel / H2D / D2H /
   sync / launch, and quantify how often the non-resident fallback (3× LLR PCIe traffic) triggers
   at our target shape.
3. Quantify the sm_80 box-plus fallback cost in `ldpc_decoder_flexible` (A100X per-CB decode
   latency vs GB10 sample, iso-shape).
4. CPU-track hotspot profile at the same shape for a fair GPU/CPU comparison and for the
   generic-only srsvec/upper-PHY kernels listed in `phase1_cpu_l1_map.md`.
