# Phase 2 Preparation: Benchmark Infra Enumeration + Container CPU Baseline + A100X Runbook

Phase 2 proper (GPU baselines, Nsight profiles) requires the A100X box — this session's container
has no GPU. This document covers the parts executable now: (1) the fork's benchmark/profiling
infrastructure enumerated, (2) a CPU-track kernel baseline measured in the container (validity
caveats below), and (3) the one-command A100X runbook (`phase2_a100x_runbook.sh`).

## 1. Benchmark / profiling infrastructure of the fork (what runs standalone)

All scripts under `scripts/cuda_accel/` unless noted. "GPU req." = needs a CUDA device.

| Script | What it measures | Main binaries | GPU req. |
|---|---|---|---|
| `gpu_benchmark_sweep.sh` | PUSCH GPU-vs-CPU across threads (1-32) × batch (10-500), latency + throughput modes | `pusch_processor_benchmark` | yes (CPU legs run anywhere) |
| `run_type1_dmrs_ul_dl_gpu_cpu_sweeps.sh` | The README headline sweep: PUSCH sensitivity + latency, PDSCH latency, CPU vs GPU, per PRB (51/106/273) × topology (`LxP` list) | `pusch_e2e_sensitivity_sweep`, `pusch_e2e_pipeline_test`, `pdsch_gpu_latency_benchmark` | yes |
| `run_latency_profile.sh` | PRB/MCS latency matrix with **GPU phase breakdown** | `pusch_64qam_stress_test` | yes |
| `run_sensitivity_sweep.sh` | 10% BLER threshold SNR per PRB/MCS, CPU vs GPU | `pusch_e2e_sensitivity_sweep` | yes |
| `run_e2e_correctness_sweep.sh` | all MCS/TBS/BG correctness, GPU pipeline | e2e tests | yes |
| `run_multiport_sweep.sh` | correctness sweep at 1/2/4/8 RX ports | e2e tests | yes |
| `multi_sector_stress.sh` | multi-cell GPU scaling (`-C` cells sharing one GPU) | `pusch_processor_benchmark` | yes |
| `run_gpu_stream_load_test.sh` | concurrent UL/DL/SRS stream-ordering stress gate | correctness/thread-safety binaries | yes |
| `run_regression_test.sh` | A/B build comparison over all test binaries, resumable | all | per-leg |
| `run_srs_latency_benchmark.sh` / `run_srs_config_sweep.sh` / `run_srs_sensitivity_sweep.sh` | SRS estimator CPU-vs-GPU latency / config coverage / accuracy-vs-SNR | `srs_estimator_gpu_latency_benchmark`, sweeps | yes |
| `test_sinr_sweep.sh` | GPU-vs-CPU BLER across MCS × PRB × SINR | e2e | yes |
| `tests/benchmarks/ofh/run_ofh_compression_matrix.sh` | OFH BFP compression matrix: CPU/GPU TX/RX p50 + speedups, per type × BW × ports; CSV output | `ofh_compression_benchmark` | yes (CPU legs standalone) |
| CPU micro-benchmarks (`tests/benchmarks/phy/...`) | per-kernel CPU throughput: LDPC enc/dec, equalizer, precoder, mod/demod chain, DFT, DMRS, PUCCH, PRACH, SRS | `*_benchmark` | no |

Key sweep knobs verified in `run_type1_dmrs_ul_dl_gpu_cpu_sweeps.sh`: `--topologies "4x4"` (our
4T4R shape — the fork's defaults are 1x1/2x4/**4x8** only), `--resource-grid-memory
host|managed|auto` and `--device-grid-memory` (the discrete-GPU memory-policy experiment),
`--rx-device-grid` (UL device-grid mode). The runbook uses these to produce a pinned-vs-managed
comparison on the A100X.

## 2. Container CPU-track baseline (indicative only — read the caveat)

**Caveat: measured on this session's 4-vCPU shared cloud container (AVX512-capable, unknown host
CPU, no isolation, no perf counters). Absolute numbers are NOT production numbers and must be
re-measured on the A100X host CPU. They are recorded to (a) prove the harnesses run, (b) give
rough relative rankings on an AVX512 machine, (c) serve as command templates.** p50 figures below.

PUSCH processor, CPU path, 273 PRB / 256QAM / MMSE / AVX512 LDPC (2 iter), single thread
(`pusch_processor_benchmark -m latency -B 20 -R 5 -T 1 -P scs30_100MHz_256qam_rv0_4port_mimo`):

| Shape | p50 per-slot latency | implied CPU throughput |
|---|---|---|
| 4 port, 1 layer | 2.19 ms | 590 Mbps needs ~4.4 slot-times → >4 cores/cell UL |
| 4 port, 2 layers | 4.94 ms | |
| 4 port, 3 layers | 7.34 ms | |
| 4 port, 4 layers | 9.81 ms | ~2.36 Gbps offered; ~20 slot-times single-thread |

PDSCH processor, CPU, 4 port 4 layer 270 PRB 256QAM: **p50 0.91 ms/slot** single-thread
(DL is ~10x cheaper than UL at iso-shape — consistent with UL being dominated by
chest/equalize/demod/LDPC-decode).

Kernel micro-benchmarks (p50, units as printed by each harness):

| Kernel | Result (container) | Note |
|---|---|---|
| LDPC dec AVX512, BG2 LS384 cb=4608 R=0.833, 6 iter | 113 Mbps/cb-chain | AVX512 > AVX2 (88 Mbps) at iso-config |
| LDPC dec AVX512, BG1-style long cb, R=0.2 | ~15-16 Mbps | rate-dependent; full tables in raw logs |
| LDPC enc AVX2, BG2 LS384 cb=19200 | 4.65 Gbps | encode is ~40x decode |
| Equalizer MMSE 4x4 | 127 MREps | scales badly with rank: 1x4 = 627 MREps |
| Modulation 256QAM mapper (ci8) | 13.6 Gbps | demapper 3.4 Gbps — soft demap is the expensive side |
| DFT 4096 (FFTW vs generic) | 468 vs 322 Ms/s fwd; 627 vs 335 inverse | FFTW backend worth +45-90% at 100 MHz size |
| Precoder auto cf_t 4 ports × 4 layers | 225-299 MREps | |

Raw outputs: kept out of the repo; regenerate with the commands above (or see the session log).
On the A100X host CPU these should be re-run with `-R >= 1000` on isolated cores.

## 3. A100X runbook

`phase2_a100x_runbook.sh` (this directory) — run from the fork checkout root on the A100X box
after building. One command, ~30-60 min (or `--quick`), writes one results directory:

```bash
bash /path/to/ocudu/docs/investigation/phase2_a100x_runbook.sh --build-dir build
# then send back: tar czf results.tar.gz <printed results dir>
```

What it produces and why:

| Output | Answers |
|---|---|
| `env.txt`, `gate.txt` | Phase-0 runtime gate: GPU path actually taken on sm_80 (aborts if it falls back) |
| `pusch_proc_{cpu,gpu}_*.txt` | GPU-vs-CPU per-slot latency at OUR shapes (4 port nlayer + 4x4 MIMO) — the direct GB10-vs-A100X comparison point |
| `type1_default/` vs `type1_managed_grid/` | README-style sweep at 51/106/273 PRB × {1x1,2x4,**4x4**,4x8}, then the same with managed grids forced — quantifies the pinned-vs-managed (UVM) policy cost on discrete PCIe |
| `ofh_matrix_summary.txt`, `ofh_matrix/` | CPU vs GPU BFP compression — tests the top `auto` mis-selection suspect (OFH auto=CUDA with per-call blocking syncs) |
| `srs_latency.txt` | SRS full-grid-staging cost on discrete |
| `nsys/pusch_gpu_4port_stats.txt` | per-slot split: kernel time vs H2D/D2H (`cuda_gpu_mem_time_sum`) vs API/sync overhead (`cuda_api_sum`) — the direct test of the PCIe-transfer thesis |
| `ctest_cuda.txt` | correctness on sm_80 (catches any runtime-only Ampere issue the compile audit could not) |

## 4. What Phase 2 will conclude from the returned data

1. Confirm/refute the transfer thesis with numbers: share of per-slot GPU time in memcpy+sync vs
   kernels on the UL resident path (prediction from the Phase-1 inventory: modest volume
   ~1.2 MB/slot but sync-bound patterns in OFH/PRACH/SRS).
2. GB10-vs-A100X divergence table for the README sweep rows (same shapes, both platforms).
3. Whether `auto` policies mis-select on A100X (OFH compression is the candidate; the PDSCH
   auto-off-on-discrete guard can be validated by comparing `type1_default` DL rows with
   `OCUDU_PDSCH_AUTO_ENABLE_DISCRETE=1` runs later).
4. The sm_80 LDPC box-plus fallback cost (compare per-CB decode latency against the GB10 sample
   at iso-shape, knowing A100X lacks `min.xorsign.abs.f16x2`).
5. A ranked, measured hotspot list for both tracks → Phase 3 proposals.
