# OCUDU CUDA Fork — Runtime Infrastructure & Acceleration Configuration/Selection Model

Scope: `lib/phy/cuda/**`, acceleration config plumbing in `apps/`, per-component CUDA/CPU factory selection, and verification of `docs/doxygen/phy_acceleration.dox` against code. All paths relative to `/home/user/cuda_accelerated_ocudu` (branch `cuda_accel.26_04`, delta = `9fd4047b43` + docs `ccdf4e681f` on mainline `092414aac2`). Facts below are verified-by-reading-code unless explicitly tagged **[hypothesis]**.

---

## 1. CUDA Runtime Infrastructure

### 1.1 Build layering (CMake)

- Top-level opt-in: `option(ENABLE_CUDA ... OFF)`; if set and `check_language(CUDA)` succeeds → `CUDA_FOUND=TRUE` (`CMakeLists.txt:88-105`).
- `lib/CMakeLists.txt:8-15`: if `CUDA_FOUND`, adds `lib/phy/cuda` **first**; else force-caches `CUDA_ACCEL_FOUND=FALSE`.
- `lib/phy/cuda/CMakeLists.txt:143` builds one static kernel library `ocudu_phy_cuda` (C-ABI, `extern "C"`, `CUDA_SEPARABLE_COMPILATION OFF` so it links from pure C++, `lib/phy/cuda/CMakeLists.txt:188-193`). It publishes `CUDA_ACCEL_FOUND/INCLUDE_DIRS/LIBRARIES` cache vars (`lib/phy/cuda/CMakeLists.txt:173-175`) consumed by downstream targets.
- Per-target `ENABLE_CUDA` compile definitions are added only where `CUDA_ACCEL_FOUND`: upper PHY (`lib/phy/upper/CMakeLists.txt:21-24,55-57`), PUSCH (`lib/phy/upper/channel_processors/pusch/CMakeLists.txt:11-22,39-43`), LDPC adapter (`lib/phy/upper/channel_coding/ldpc/cuda/CMakeLists.txt:7-60`), lower-PHY modulation (`lib/phy/lower/modulation/CMakeLists.txt:12-18`), PDxCH (`lib/phy/lower/processors/downlink/pdxch/CMakeLists.txt:13-21`), RU SDR (`lib/ru/sdr/CMakeLists.txt:23-24`), OFH compression (`lib/ofh/compression/CMakeLists.txt:33-42`).
- FFTs come from a vendored **VkFFT** (CUDA backend, NVRTC runtime compilation): `lib/phy/cuda/CMakeLists.txt:154-171` (`OCUDU_PHY_CUDA_ENABLE_VKFFT=1`, `VKFFT_BACKEND=1`, links `CUDA::cuda_driver`, `CUDA::nvrtc`), tree at `lib/phy/cuda/third_party/vkfft/`.
- **Tegra/DGX-Spark bias in build system**: elaborate Tegra platform autodetection (checks `/etc/nv_tegra_release`, device-tree, `host1x`, cpuinfo; recognizes "Spark") sets `-DTARGET_TEGRA` (`lib/phy/cuda/CMakeLists.txt:30-105`). `CMAKE_CUDA_ARCHITECTURES` defaults to `native` (`lib/phy/cuda/CMakeLists.txt:108-111`); the fork README documents `-DCMAKE_CUDA_ARCHITECTURES=121` for GB10 (`lib/phy/cuda/README.md:17-22`, `README.md:136`). Building on an A100X box with `native` yields sm_80 — no code prevents that, but nothing is tuned for it either.

### 1.2 Device discovery / selection

There is **no configuration option or env var for CUDA device ID anywhere** — device 0 (or "current device") is implicit throughout (verified by grep; no `OCUDU_CUDA_DEVICE*` exists). Only standard `CUDA_VISIBLE_DEVICES` masking would work. **[hypothesis]**: multi-GPU hosts are unsupported by design.

- Library init is presence-only: `ocudu_phy_cuda_init()` = `cudaGetDeviceCount()>0` (`lib/phy/cuda/src/nr_constants.cu:173-181`); cleanup calls `cudaDeviceReset()` (`nr_constants.cu:183-185`).
- PUSCH GPU demodulator explicitly binds device 0: `cudaSetDevice(0)` "to ensure CUDA context stability across pinned threads" (`lib/phy/upper/channel_processors/pusch/pusch_demodulator_gpu_impl.cpp:637-650`).
- PRACH detector and SRS estimator hard-code `device_id = 0` + `cudaSetDevice` (`lib/phy/upper/channel_processors/prach/prach_detector_cuda_impl.cpp:89-92`; `lib/phy/upper/signal_processors/srs/srs_estimator_cuda_impl.cpp:167-169`).
- Grid/PRACH-buffer implementations capture the *current* device via `cudaGetDevice` (`lib/phy/upper/resource_grid_cuda_visible_impl.h:67-69`, `lib/phy/upper/prach_buffer_cuda_visible_impl.h:69-71`, `lib/phy/upper/phy_acceleration_resource_grid_factory.cpp:19-23`).
- All long-lived CUDA components set `cudaSetDeviceFlags(cudaDeviceScheduleYield)` for RT-thread friendliness (`pusch_demodulator_gpu_impl.cpp:661`, `prach_detector_cuda_impl.cpp:94`, `srs_estimator_cuda_impl.cpp:172`, `pusch_codeblock_decoder_cuda_batch.cpp:153`), and synchronization is done by polling `cudaStreamQuery`/`cudaEventQuery` + `sched_yield()` instead of blocking (`lib/phy/upper/channel_coding/ldpc/cuda/cuda_rt_utils.h:84-108`; same pattern in `resource_grid_cuda_visible_impl.h:496-503`).
- VkFFT plans re-derive the driver device from the runtime's current device (`cuDeviceGet(&h->vkfft_device, cuda_device)`: `lib/phy/cuda/src/pusch_e2e.cu:10440-10445`, `low_phy_tx.cu:478-479`, `low_phy_puxch_rx.cu:485-486`, `low_phy_prach_rx.cu:406-407`, `prach_detector.cu:341-342`, `srs_estimator.cu:328-329`).

### 1.3 Stream model — there is no global stream pool

Streams are **per-component-instance**, and instance counts come from the executor concurrency / pool sizing in the factories, not from a central pool:

| Unit | Streams per instance | Instance count driver | file:line |
|---|---|---|---|
| PUSCH GPU demodulator | 4 (`stream_`, `scr_stream_`, `h2d_stream_`, `d2h_stream_`) + triple-buffer ring `NUM_BUFFERS=3` with per-buffer h2d/kernel/d2h events | one demodulator per PUSCH processor; regular-processor pool = `MAX_PUSCH_PDUS_PER_SLOT` (async decode) or `pusch_executor.max_concurrency` | `pusch_demodulator_gpu_impl.cpp:681,691,885,899`; `pusch_demodulator_gpu_impl.h:291`; pool sizing `lib/phy/upper/upper_phy_factories.cpp:822-825,968-975` |
| PUSCH E2E kernel handle | borrows demodulator stream; own `precompute_stream` for chest precompute | 1 per demodulator | `lib/phy/cuda/src/pusch_e2e.cu:10666,10724,10773` |
| Resident PUSCH CB decoder (batch LDPC) | 1 | pool of `nof_resident_acceleration_decoders` = `nof_regular_pusch_processors` | `pusch_codeblock_decoder_cuda_batch.cpp:155`; `upper_phy_factories.cpp:846`; `lib/phy/upper/channel_processors/pusch/processor_factories.cpp:222` |
| PRACH detector CUDA | 1 | pooled by `prach_executor.max_concurrency` | `prach_detector_cuda_impl.cpp:101-106`; `lib/phy/upper/channel_processors/prach/factories.cpp:244-249` |
| SRS estimator CUDA | 1 | pooled by `srs_executor.max_concurrency` | `srs_estimator_cuda_impl.cpp:179-184`; `srs_estimator_factory.cpp:129-165` |
| PDSCH TB encoder CUDA | 1 | one per "acceleration lane"; lanes = `pdsch_acceleration_nof_lanes` (default `min(4, cb-executor concurrency)`) | `lib/phy/upper/channel_coding/ldpc/cuda/pdsch_tb_encoder_cuda.cu:86`; `upper_phy_factories.cpp:1356-1373` |
| Standalone LDPC/CRC/mod wrappers | 1 each | per created component | `ldpc_decoder_cuda.cpp:34`, `ldpc_encoder_cuda.cpp:128`, `ldpc_encoder_cuda_batch.cpp:103`, `crc_calculator_cuda.cpp:37`, `modulation_mapper_cuda.cpp:52`, `demodulation_mapper_cuda.cpp:33` |
| CUDA-visible resource grid | 1 "maintenance" stream (max-priority, for device-side zeroing) + growable event ring (32 pre-created) | per resource grid | `resource_grid_cuda_visible_impl.h:71-87,505-519` |
| Low-PHY TX handle | 1 owned realtime stream; host staging ring `HOST_GRID_STAGING_SLOTS=8`; CUDA-graph cache (limit 64) | per lower-PHY sector modulator | `lib/phy/cuda/src/low_phy_tx.cu:25-26,188-201,870-875` |
| Low-PHY PUxCH RX handle | 1 owned realtime stream; staging ring 32 slots | per sector | `low_phy_puxch_rx.cu:26,40-47` |
| Low-PHY PRACH RX handle | 1 owned realtime stream; staging ring 8 slots | per sector | `low_phy_prach_rx.cu:25,38-45` |
| OFH IQ compression | 1 stream per handle; **dynamic handle pool** (acquire/release, grows on demand) per `iq_compression_cuda` | per compressor/decompressor object | `lib/phy/cuda/src/ofh_compression.cu:3003`; `lib/ofh/compression/iq_compression_cuda.cpp:64-81` |

Stream priority policy: upper-PHY streams use `cudaStreamCreateUpperPhy` — **normal priority by default**, switchable via env `OCUDU_UPPER_PHY_CUDA_STREAM_PRIORITY` (`high|low|...`) or legacy `OCUDU_UPPER_PHY_CUDA_HIGH_PRIORITY` (`lib/phy/upper/channel_coding/ldpc/cuda/cuda_rt_utils.h:13-70`). Low-PHY and grid maintenance streams always attempt greatest priority via `cudaStreamCreateWithPriority` and fall back to plain non-blocking (`low_phy_tx.cu:46-60`, `resource_grid_cuda_visible_impl.h:505-519`).

CUDA Graphs: low-PHY TX/RX can capture the per-slot pipeline into cached executable graphs, **opt-in** via `OCUDU_LOWPHY_TX_CUDA_GRAPHS` / `OCUDU_LOWPHY_RX_CUDA_GRAPHS` (default off; `low_phy_tx.cu:79-84,663-721`; `low_phy_puxch_rx.cu:69`).

### 1.4 Memory allocators and the policy that picks them

Three tiers, chosen by *platform policy* (integrated vs discrete) plus env overrides:

**(a) Plain device memory (`cudaMalloc`)** — the default for all kernel working sets, pre-allocated at worst case at init: PUSCH demodulator device buffers (`d_symbols_`, `d_noise_vars_`, triple-buffered LLR/unified-input/SCH buffers: `pusch_demodulator_gpu_impl.cpp:807-881`), PDSCH mapper offsets (`pdsch_block_processor_gpu_impl.cpp:110-114`), sidecar device grids in default `device` mode (`pusch_device_grid_reader_cuda.cpp:52`, `pdsch_device_grid_writer_cuda.cpp:51`; enum default `device` at `pusch_device_grid_reader_cuda.h:15-18`, `pdsch_device_grid_writer_cuda.h:19-23`). A default device mem-pool is configured with unlimited release threshold, but **no `cudaMallocAsync` call exists** — `use_mem_pool_` is set and never consumed (`pusch_demodulator_gpu_impl.cpp:798-805`; grep confirms zero `cudaMallocAsync`/`cudaFreeAsync`). **[dead code]**

**(b) Pinned host memory** — two mechanisms:
- `cudaHostAlloc(..., cudaHostAllocDefault)`: D2H LLR staging (`pusch_demodulator_gpu_impl.cpp:782`), per-buffer unified H2D staging and pinned UCI result buffers (`pusch_demodulator_gpu_impl.cpp:849-861`, `:2019`), SRS result staging (`lib/phy/cuda/src/srs_estimator.cu:311-313`), low-PHY staging rings (`low_phy_prach_rx.cu:339-341,382-384`, `low_phy_puxch_rx.cu:417-419`, `low_phy_tx.cu:540-542`). One use of `cudaHostAllocWriteCombined` for the TX grid staging ring (`low_phy_tx.cu:548-550`).
- `cudaHostRegister(..., cudaHostRegisterDefault)` (pin existing heap):
  - `resource_grid_pinned_factory` pins the whole backing tensor of a normally-created host grid after validating `[port][symbol][subcarrier]` contiguity; failure is non-fatal (grid stays unpinned) (`lib/phy/upper/resource_grid_pinned_impl.h:22-67`, unregister at `:79-84`). This is the **fallback/default accelerated grid on discrete GPUs** (see §3).
  - Low-PHY handles maintain a registered-range cache and pin radio/grid buffers on first sight; per-range failure list prevents retry loops (`low_phy_tx.cu:359-385`, `low_phy_puxch_rx.cu:345`, `low_phy_prach_rx.cu:270-295`); unregistered at teardown (`low_phy_tx.cu:895`, `low_phy_puxch_rx.cu:963`, `low_phy_prach_rx.cu:618`). Registration attempts on the hot path are gated by opt-in envs `OCUDU_LOWPHY_{TX,RX,PRACH}_RUNTIME_HOST_REGISTRATION` (default off) (`low_phy_tx.cu:73-77`, `low_phy_puxch_rx.cu:63`, `low_phy_prach_rx.cu:61`).

**(c) Managed memory (`cudaMallocManaged`)** — used only by the "CUDA-visible" shared-grid implementations:
- `cuda_visible_resource_grid` (`resource_grid_cuda_visible_impl.h:89-94`).
- `cuda_visible_prach_buffer` (`prach_buffer_cuda_visible_impl.h:87-93`).
- Sidecar device-grid writer/reader when constructed in `managed` mode (`pdsch_device_grid_writer_cuda.cpp:49-51` uses `cudaMemAttachGlobal`; `pusch_device_grid_reader_cuda.cpp:48-53`) — but default construction mode is `device`.

**Every `cudaMemAdvise` / `cudaMemPrefetchAsync` call and its gating policy:**

| Call | Where | Policy |
|---|---|---|
| `cudaMemAdvise(SetAccessedBy, device)` | `resource_grid_cuda_visible_impl.h:527` | always, best-effort, on managed grid |
| `cudaMemAdvise(SetPreferredLocation, device)` | `resource_grid_cuda_visible_impl.h:528-530` | only if env `OCUDU_CUDA_VISIBLE_GRID_PREFER_DEVICE` (default off) |
| `cudaMemAdvise(SetAccessedBy, device)` | `prach_buffer_cuda_visible_impl.h:328` | always on managed PRACH buffer |
| `cudaMemAdvise(SetPreferredLocation, device)` | `prach_buffer_cuda_visible_impl.h:329-331` | env `OCUDU_CUDA_VISIBLE_PRACH_BUFFER_PREFER_DEVICE` (default off) |
| `cudaMemPrefetchAsync` (grid, to device before mapping / to host after) | `resource_grid_cuda_visible_impl.h:551-573` (call sites `:390,416,435`) | gated by `prefetch_enabled` = env `OCUDU_CUDA_VISIBLE_GRID_PREFETCH`, **default false** (`:586`); `cudaErrorNotSupported` treated as success |
| `cudaMemPrefetchAsync` (PRACH buffer) | `prach_buffer_cuda_visible_impl.h:353-365` | gated by env `OCUDU_CUDA_VISIBLE_PRACH_BUFFER_PREFETCH`, **default TRUE** (`:385`) — inconsistent with the grid default |

Both files carry a CUDART ≥ 13 / < 13 dual path for the `cudaMemLocation` vs `int` prefetch API (`resource_grid_cuda_visible_impl.h:533-549`).

**A100X note**: with `OCUDU_CUDA_VISIBLE_GRID=managed` forced on a discrete sm_80 part, default behavior is managed memory with *no* prefetch and *no* preferred location → per-slot page-fault migration over PCIe on every host-write→device-read transition. The defaults were tuned for coherent GB10 where migration is free. The prefetch/prefer-device envs exist precisely to patch this, but nothing turns them on automatically for discrete parts. **[assessment based on verified defaults]**

---

## 2. Configuration Model

### 2.1 YAML/CLI options (implementation-neutral strings `auto|enabled|disabled`)

| Option | Struct definition | CLI parse | Validation | Translated to | Actually switches |
|---|---|---|---|---|---|
| `expert_phy.pusch_acceleration_mode` | `apps/units/flexible_o_du/o_du_low/du_low_config.h:72-78` | `du_low_config_cli11_schema.cpp:262-267` | `du_low_config_validator.cpp:85-90` | `upper_phy_factory_config.pusch_acceleration_mode` (`du_low_config_translator.cpp:55`; field `include/ocudu/phy/upper/upper_phy_factories.h:356`) | GPU vs SW PUSCH demodulator factory + resident LDPC decode enable (`lib/phy/upper/upper_phy_factories.cpp:807-816,849,895-920`) |
| `expert_phy.srs_acceleration_mode` | `du_low_config.h:79-85` | schema `:268-274` | validator `:92-97` | translator `:56` → `upper_phy_factories.h:358` | `srs_estimator_cuda_impl` vs generic (`upper_phy_factories.cpp:665-686`; `lib/phy/upper/signal_processors/srs/srs_estimator_factory.cpp:96-114`) |
| `expert_phy.pdsch_acceleration_mode` | `du_low_config.h:86-92` | schema `:275-283` | validator `:105-110` | translator `:57` → `upper_phy_factories.h:362` (also DL factory config `:160`) | accelerated PDSCH block processor + DL grid factory + lane policy (`upper_phy_factories.cpp:478-482,1303-1340`) |
| `expert_phy.prach_acceleration_mode` | `du_low_config.h:93-99` | schema `:284-289` | validator `:112-117` | translator `:58` → `upper_phy_factories.h:364` | `prach_detector_factory_cuda` vs SW + accelerated PRACH buffers (`lib/phy/upper/channel_processors/prach/factories.cpp:204-228`; `upper_phy_factories.cpp:502-508,645-656`) |
| `expert_phy.pdsch_acceleration_nof_lanes` | `du_low_config.h:100-104` | schema `:290-295` | (range only via `CLI::Number`) | translator `:59` → `upper_phy_factories.h:366` | number of concurrent GPU PDSCH lanes / processor pool size (`upper_phy_factories.cpp:1361-1373`) |
| `expert_phy.ldpc_decoder_algorithm` (`auto|boxplus|min_sum`) | `du_low_config.h:105-111` | schema `:297-308` | validator `:119-123` | translator `:60` → `upper_phy_factories.h:368` → decoder config (`upper_phy_factories.cpp:850`) → `set_ldpc_decoder_algorithm` (`processor_factories.cpp:119`) | per-batch boxplus vs half2 min-sum kernel choice (`lib/phy/upper/channel_coding/ldpc/cuda/pusch_codeblock_decoder_cuda_batch.cpp:59-80,509`) |
| `ru_sdr.expert_cfg.low_phy_tx_acceleration_mode` | `apps/units/flexible_o_du/split_8/helpers/ru_sdr_config.h:43-49` | `ru_sdr_config_cli11_schema.cpp:68-72` | `ru_sdr_config_validator.cpp:114-117` | `lower_phy_configuration.tx_acceleration_mode` (`ru_sdr_config_translator.cpp:80`; `include/ocudu/phy/lower/lower_phy_configuration.h:98`) | CUDA vs SW PDxCH processor factory (`lib/ru/sdr/lower_phy/lower_phy_factory.cpp:100-122`) |
| `ru_sdr.expert_cfg.low_phy_rx_acceleration_mode` | `ru_sdr_config.h:50-59` | schema `:74-78` | validator `:119-122` | translator `:81` → `lower_phy_configuration.h:108` | CUDA OFDM demodulator factory wrap (`lib/phy/lower/modulation/modulation_factories.cpp:352-368`) |
| `ru_sdr.expert_cfg.low_phy_prach_demodulation_acceleration_mode` | `ru_sdr_config.h:60-68` | schema `:80-84` | validator `:124-127` | translator `:82` → `lower_phy_configuration.h:115` | CUDA PRACH OFDM demodulator factory wrap (`modulation_factories.cpp:393-410`) |
| `ru_ofh.cells[].compression_acceleration_mode` | `apps/units/flexible_o_du/split_7_2/helpers/ru_ofh_config.h:111-112` | `ru_ofh_config_cli11_schema.cpp:164-166` | `ru_ofh_config_validator.cpp:63,155-158` | `ofh_sector_config.compression_acceleration_mode` (`ru_ofh_config_translator.cpp:126`; `include/ocudu/ofh/ofh_sector_config.h:93-94` → receiver/transmitter configs `include/ocudu/ofh/receiver/ofh_receiver_configuration.h:45-46`, `include/ocudu/ofh/transmitter/ofh_transmitter_configuration.h:64-65`; fan-out `lib/ofh/ofh_factories.cpp:48,93`) | passed as `impl_type` to `create_iq_compressor` / `create_iq_decompressor` (`lib/ofh/receiver/ofh_receiver_factories.cpp:26`, `lib/ofh/transmitter/ofh_transmitter_factories.cpp:100`) → CUDA vs AVX512/AVX2/NEON/generic (`lib/ofh/compression/compression_factory.cpp:31-70,83-157`) |

All options round-trip to YAML output (`du_low_config_yaml_writer.cpp:64-69`). **No device-id, stream-count, or buffer-count option exists in the YAML/CLI schema** — those are env-var/compile-time only.

### 2.2 Environment variables (parse site → effect)

Generic parsing helpers: `lib/phy/upper/phy_acceleration_runtime_options.h:18-109` (flag/unsigned parsing, `OCUDU_CUDA_VISIBLE_GRID` mode resolution, `OCUDU_PDSCH_AUTO_ENABLE_DISCRETE`).

**Mode overrides (only consulted when the YAML mode is `auto`):**
- `OCUDU_PRACH_ACCELERATION` → `resolve_prach_acceleration_mode` (`lib/phy/upper/channel_processors/prach/factories.cpp:37-56`).
- `OCUDU_SRS_ACCELERATION` → `resolve_srs_acceleration_mode` (`srs_estimator_factory.cpp:39-61`).
- `OCUDU_LOWPHY_TX_ACCELERATION` (`lib/ru/sdr/lower_phy/lower_phy_factory.cpp:32-48`); `OCUDU_LOWPHY_RX_ACCELERATION` / alias `OCUDU_LOWPHY_PUXCH_DEMODULATION_ACCELERATION` (`modulation_factories.cpp:37-59`); `OCUDU_LOWPHY_PRACH_DEMODULATION_ACCELERATION` / alias `OCUDU_LOWPHY_PRACH_ACCELERATION` (`modulation_factories.cpp:61-83`).
- `OCUDU_OFH_TX_COMPRESSION_IMPL` / `OCUDU_OFH_RX_COMPRESSION_IMPL` / fallback `OCUDU_OFH_COMPRESSION_IMPL` (values incl. `cuda|gpu|avx512|avx2|neon|cpu_auto`; `enabled`→`cuda`, `disabled/cpu/host`→`cpu_auto`) (`lib/ofh/compression/compression_factory.cpp:31-60,77,163`).
- Note: **no** `OCUDU_PUSCH_ACCELERATION` or `OCUDU_PDSCH_ACCELERATION` mode-override env exists; PUSCH/PDSCH modes come from YAML only (PDSCH auto additionally consults `OCUDU_PDSCH_AUTO_ENABLE_DISCRETE`, below).

**Grid residency / memory policy:**
- `OCUDU_CUDA_VISIBLE_GRID`, with direction-specific overrides `OCUDU_DL_CUDA_VISIBLE_GRID` / `OCUDU_UL_CUDA_VISIBLE_GRID` (`managed` | `pinned`/`off`/`disabled` | anything-else→auto): parsed in `phy_acceleration_runtime_options.h:74-96` and `resource_grid_cuda_visible_impl.h:619-656`; switches managed grid vs pinned fallback per direction. Also feeds PDSCH auto-mode (`upper_phy_factories.cpp:61-63`) and the PDSCH GPU mapper policy (`pdsch_block_processor_gpu_impl.cpp:54-56`).
- `OCUDU_UL_CUDA_VISIBLE_PRACH_BUFFER` (falls back to `OCUDU_UL_CUDA_VISIBLE_GRID`/`OCUDU_CUDA_VISIBLE_GRID`): `phy_acceleration_prach_buffer_factory.cpp:36-53` → managed PRACH buffer vs host buffer.
- `OCUDU_CUDA_VISIBLE_GRID_PREFETCH` (default off), `OCUDU_CUDA_VISIBLE_GRID_PREFER_DEVICE` (off), `OCUDU_CUDA_VISIBLE_PRACH_BUFFER_PREFETCH` (**default on**), `OCUDU_CUDA_VISIBLE_PRACH_BUFFER_PREFER_DEVICE` (off) — see §1.4 table.
- `OCUDU_PDSCH_DIRECT_DEVICE_GRID` (**default enabled**): direct CUDA-visible PDSCH grid writer vs sidecar materialization (`lib/phy/upper/pdsch_grid_output_strategy.cpp:23-28,56`; also flips the GPU mapper default: `pdsch_block_processor_gpu_impl.cpp:49-52`).
- `OCUDU_PDSCH_DISABLE_DEVICE_MAP`, `OCUDU_PDSCH_DEFER_SYMBOL_D2H`, `OCUDU_PDSCH_DEVICE_MAP_TIMING`, `OCUDU_PDSCH_DISABLE_ENCODE_CACHE` (`pdsch_block_processor_gpu_impl.cpp:32-95`); `OCUDU_PDSCH_DEVICE_MAP_SKIP_HOST` (`lib/phy/support/resource_grid_mapper_impl.cpp:72`); `OCUDU_PDSCH_TIMING` (`pdsch_tb_encoder_cuda.cu:102`).
- `OCUDU_PDSCH_AUTO_ENABLE_DISCRETE`: opts auto-mode PDSCH GPU on discrete devices (`phy_acceleration_runtime_options.h:99-109`; consumed `upper_phy_factories.cpp:57-59`).
- `OCUDU_LOWPHY_TX_HOST_GRID_STAGING` (default on; `lib/phy/lower/processors/downlink/pdxch/pdxch_baseband_modulator_cuda.cpp:85`), `OCUDU_LOWPHY_{TX,RX,PRACH}_RUNTIME_HOST_REGISTRATION`, `OCUDU_LOWPHY_{TX,RX}_CUDA_GRAPHS` (§1.3/1.4).

**Workload-shape thresholds (auto-mode per-PDU gating):**
- `OCUDU_PUSCH_ACCELERATION_MIN_RB`, `OCUDU_PUSCH_ACCELERATION_MIN_ULSCH_BITS` (defaults **0 = disabled**): `lib/phy/upper/channel_processors/pusch/pusch_acceleration_runtime_options.h:19-47`; applied per grant in `pusch_processor_impl.cpp:53-58` and `pusch_demodulator_gpu_impl.cpp:206-210`.
- `OCUDU_PRACH_ACCELERATION_FORCE`, `OCUDU_PRACH_ACCELERATION_MIN_SHORT_WORK` (128), `OCUDU_PRACH_ACCELERATION_MIN_DEVICE_SHORT_PREAMBLES` (128), `OCUDU_PRACH_ACCELERATION_MIN_LONG_SEQUENCE_WORK` (4): `prach_detector_cuda_impl.cpp:211-235`.
- `OCUDU_SRS_ACCELERATION_FORCE`, `OCUDU_SRS_ACCELERATION_MIN_PILOT_RE` (default 4096), `OCUDU_SRS_ACCELERATION_FULL_CORRELATION` / `OCUDU_SRS_ACCELERATION_WINDOWED_CORRELATION` (windowed default): `srs_estimator_cuda_impl.cpp:57-100`.

**Resident-UCI & misc PUSCH:** `OCUDU_PUSCH_ENABLE_ACCELERATED_UCI` (device-side UCI demux opt-in; `pusch_acceleration_runtime_options.h:49-52`, policy comment about OTA HARQ-miss risk at `pusch_processor_impl.cpp:60-66`), `OCUDU_PUSCH_ACCELERATION_UCI_DEVICE_DECODE` (`pusch_demodulator_gpu_impl.cpp:1938`), `OCUDU_PUSCH_ACCELERATION_TRACE`, `OCUDU_TIME_INTERP`, `OCUDU_NOISE_MODE` (`pusch_demodulator_gpu_impl.cpp:609-628`), `OCUDU_PUSCH_GPU_E2E_TIMING_WARN_US` (`:212-216`).

**LDPC kernel tuning family:** `OCUDU_LDPC_BOXPLUS`, `OCUDU_LDPC_BOXPLUS_MAX_CBS` (default 192, GB10-profiled), plus kernel-internal `OCUDU_LDPC_{SCALE,OFFSET,C2V,SHMEM,LLR_CLAMP,DIRECT_RM,...}` (`pusch_codeblock_decoder_cuda_batch.cpp:59-80`; `lib/phy/cuda/src/ldpc_decoder_flexible.cu`, various).

**Stream priority:** `OCUDU_UPPER_PHY_CUDA_STREAM_PRIORITY` / `OCUDU_UPPER_PHY_CUDA_HIGH_PRIORITY` (`cuda_rt_utils.h:34-47`). **Tracing:** `OCUDU_NVTX_TRACE` (`lib/support/tracing/scoped_trace.cpp:37`).

The docs claim env vars are "reserved for platform policy overrides, focused profiling or sensitivity experiments" (`docs/doxygen/phy_acceleration.dox:134-138`) — **partially accurate**: several env vars are load-bearing production policy (`OCUDU_PDSCH_AUTO_ENABLE_DISCRETE` is the *only* way to get auto-mode PDSCH GPU on an A100X; `OCUDU_PDSCH_DIRECT_DEVICE_GRID` defaults the OTA grid path; threshold defaults define auto behavior).

---

## 3. Auto-Mode Decision Logic — exactly what `auto` checks

Two-stage model: **factory-time availability** (coarse; picks the backend object, which always wraps a CPU fallback) then **runtime per-PDU gating** (fine; workload shape).

### 3.1 Factory-time checks

| Component | `auto` availability check | file:line |
|---|---|---|
| PUSCH demodulator | `cudaGetDeviceCount()>0` (with a retry and a `cudaSetDevice(0)` fallback for sticky `cudaErrorUnknown`) — **device presence only**; no arch/CC/memory check | `demodulator_factories.cpp:236-261`; consumed `upper_phy_factories.cpp:813-815` |
| PUSCH resident LDPC decode | same flag as demodulator (`enable_resident_acceleration_decoder = use_pusch_acceleration`) | `upper_phy_factories.cpp:846-853` |
| SRS estimator | `cudaGetDeviceCount()>0` | `srs_estimator_cuda_impl.cpp:212-220` via `srs_estimator_factory.cpp:181-188`; consumed `upper_phy_factories.cpp:672-674` |
| PRACH detector | `cudaGetDeviceCount()>0` | `prach_detector_cuda_impl.cpp:134-142` via `prach/factories.cpp:230-237`; consumed `factories.cpp:218` |
| PDSCH block processor | `cuInit(0)` + `cuDeviceGetCount()>0` **plus the discrete-device policy below** | `pdsch_tb_encoder_cuda.cu:631-641` via `pdsch/factories.cpp:531-534`; policy `upper_phy_factories.cpp:51-73` |
| Lower-PHY TX (PDxCH) | `is_pdxch_processor_factory_cuda_available()` (device presence) | `lib/ru/sdr/lower_phy/lower_phy_factory.cpp:103-121` |
| Lower-PHY RX / PRACH demod | `cudaGetDeviceCount()>0` (`lowphy_prach_gpu_available`, and per-impl checks) | `modulation_factories.cpp:85-93,352-368,393-410`; `ofdm_demodulator_cuda_impl.cpp:129-135` |
| OFH compression | `cudaGetDevice()` succeeds | `ofh_compression.cu:2987-2991` via `iq_compression_cuda.cpp:31-38`, consumed `compression_factory.cpp:54-58` |

`enabled` mode = same check but `report_fatal_error` on failure (e.g. `upper_phy_factories.cpp:808-811`, `prach/factories.cpp:212-217`, `lower_phy_factory.cpp:104-106`, `compression_factory.cpp:87-88`). CUDA-disabled builds fatal on `enabled` (`srs_estimator_factory.cpp:109-111`, `prach/factories.cpp:223-225`, `lower_phy_factory.cpp:115-118`, `compression_factory.cpp:78-81`). Every selection is logged as an "acceleration manifest" line (`upper_phy_factories.cpp:654-656,683-686,921-927,1409-1415`).

**PDSCH auto is the only component with a platform check** — `use_pdsch_acceleration_in_auto_mode()` (`upper_phy_factories.cpp:51-73`):
1. GPU available? (driver-API count)
2. If `OCUDU_PDSCH_AUTO_ENABLE_DISCRETE` set → obey it.
3. If DL CUDA-visible grid explicitly `managed` → enable.
4. If `phy_acceleration_cuda_current_device_is_discrete()` → **disable with warning** ("Set OCUDU_PDSCH_AUTO_ENABLE_DISCRETE=1 ..."). The discreteness test is `cudaDeviceGetAttribute(cudaDevAttrIntegrated)==0` (`phy_acceleration_resource_grid_factory.cpp:14-35`).
5. Otherwise (integrated) → enable.

### 3.2 Memory-policy auto checks (the ones that diverge between GB10 and A100X)

- **Resource grids** (`resource_grid_cuda_visible_factory::should_use_managed_grid_auto`, `resource_grid_cuda_visible_impl.h:658-670`): managed grid **iff `cudaDevAttrIntegrated != 0`**. On GB10 (integrated=1) → `cudaMallocManaged` grids shared zero-copy. On **A100X (integrated=0) → falls back to `resource_grid_pinned_factory`** (host grid + `cudaHostRegister`, explicit H2D/D2H DMA) — sane for discrete.
- **PRACH buffers** (`should_use_managed_prach_buffer_auto`, `phy_acceleration_prach_buffer_factory.cpp:22-34`): managed buffer **iff `cudaDevAttrManagedMemory != 0`** — this is **true on discrete A100X too**. So under default `auto`, PRACH buffers become `cudaMallocManaged` on A100X while grids stay pinned-host: an inconsistent pair of policies. On A100X the lower-PHY/OFH writer fills the managed buffer from the CPU, then the GPU PRACH detector faults it across PCIe each occasion (prefetch is on by default here — `prach_buffer_cuda_visible_impl.h:385` — which mitigates the device-read side but host-write-back still fault-migrates). **⚠ A100X divergence, likely unintended; the grid policy checks `Integrated`, the PRACH policy checks `ManagedMemory`.**
- **PDSCH sidecar device-map** (`is_gpu_mapper_disabled`, `pdsch_block_processor_gpu_impl.cpp:41-77`): explicitly disabled by default when `cudaDevAttrIntegrated==0` with an in-code rationale ("On discrete GPUs the current sidecar grid path copies and scans the full resource grid back to host before TX. That is slower than the host mapper...").
- **No code anywhere checks** `pageableMemoryAccess`, `concurrentManagedAccess`, compute capability, or SM arch at runtime (verified by grep: only `cudaDevAttrIntegrated` and `cudaDevAttrManagedMemory` are queried). Kernel/arch fitness is entirely a build-time (`CMAKE_CUDA_ARCHITECTURES`) concern.

### 3.3 Runtime per-PDU workload gating

- **PUSCH**: PDU-shape support — layers ∈ {1,2,3,4}, rx ports ∈ {1,2,4,8}, layers ≤ ports, transform precoding only 1-layer (`pusch_processor_impl.cpp:41-51`, duplicated in the demodulator selector `demodulator_factories.cpp:152-161`); small-grant CPU preference via env thresholds, **inactive by default** (`pusch_acceleration_runtime_options.h:30-47`; `pusch_processor_impl.cpp:53-58`). The accelerated factory always builds *both* GPU and SW demodulators and dispatches per-PDU (`demodulator_factories.cpp:196-214`).
- **PRACH**: short formats go GPU when `ports×preambles ≥ 128` or a device-resident PRACH buffer exists with ≥128 preambles; long formats when `ports×active_sequences ≥ 4`; CPU fallback otherwise (`prach_detector_cuda_impl.cpp:211-235`, fallback dispatch `:203-205`).
- **SRS**: GPU when observed pilot REs (`ports×symbols×seq_len`) ≥ 4096; CPU fallback otherwise; runtime `cudaSetDevice` failure also falls back (`srs_estimator_cuda_impl.cpp:86-100,231-236`).
- **LDPC algorithm `auto`**: boxplus when batch `< 192` CBs else min-sum ("Profiling on GB10 shows box-plus is faster for smaller batches" — `pusch_codeblock_decoder_cuda_batch.cpp:59-69`). **⚠ GB10-derived crossover; the 192-CB threshold is unvalidated for sm_80.**
- **PDSCH**: no per-PDU shape gate; instead lane-count concurrency policy (default `min(4, cb-executor-concurrency)` lanes, synchronous CB batching, pool exhaustion blocks — `upper_phy_factories.cpp:1356-1379`).

### 3.4 A100X (discrete sm_80) vs GB10 (integrated sm_121) behavior summary

Default `auto` everything on A100X: PUSCH/SRS/PRACH/lower-PHY/OFH all **go CUDA** (presence-only checks) using pinned-host staging grids; **PDSCH stays on CPU** (discrete check) unless `OCUDU_PDSCH_AUTO_ENABLE_DISCRETE=1`; **PRACH buffers go managed** (ManagedMemory check) — the one memory policy that silently assumes cheap migration. Explicitly GB10-assuming artifacts to audit on A100X:
- "GB10/B210 OTA-stable default" comment for direct device grid (`pdsch_grid_output_strategy.cpp:25-27`).
- "On GH200, this allows the GPU to DMA directly..." (`resource_grid_pinned_impl.h:19-21`) and "full NVLink bandwidth" comments (`pusch_demodulator_gpu_impl.cpp:2105-2107,2311-2312`) — on A100X these DMAs are PCIe, still correct but the latency budget assumptions differ.
- GB10-profiled boxplus threshold (`pusch_codeblock_decoder_cuda_batch.cpp:65-68`).
- Doc statement "The default OTA path on unified CUDA-memory platforms uses CUDA-visible resource grids and direct PDSCH device-grid mapping when available" (`phy_acceleration.dox:140-143`) — verified consistent with code, and correctly scoped to unified-memory platforms.

---

## 4. Enforcement of the "generic PHY does not include CUDA headers" boundary

Mechanisms, all verified:

1. **Opaque-token interface hooks in public headers.** The `include/ocudu/**` interface headers expose optional device paths purely as `void*` tokens + capability booleans with host-safe defaults: `resource_grid_reader` (`supports_device_grid_reading`, `get_device_grid_cbf16`, `get_device_grid_ready_event`, `prepare/enqueue/synchronize` — `include/ocudu/phy/support/resource_grid_reader.h:110-126`), `resource_grid_writer` (`include/ocudu/phy/support/resource_grid_writer.h:92-114`), `prach_buffer` (`include/ocudu/phy/support/prach_buffer.h:67-98`), PUSCH resident mode (`include/ocudu/phy/upper/channel_processors/pusch/pusch_demodulator.h:105-138`, `pusch_decoder.h:84-124`, `pusch_resident_codeblock_decoder.h:61-136`). The implementation-neutral resident-buffer contracts (`resident_softbit_buffer` with `void* execution_context` / `void* completion_token`, `resident_uci_buffer`, `pusch_resident_demodulation_config`) live in `include/ocudu/phy/upper/acceleration/phy_acceleration.h:22-98`. Grep confirms **zero** `cuda*` includes anywhere under `include/ocudu/`.

2. **Factory indirection + `#ifdef ENABLE_CUDA` confined to factory TUs.** Generic factory translation units include CUDA only inside guards: `demodulator_factories.cpp:12-15`, `phy_acceleration_resource_grid_factory.cpp:7-10`, `phy_acceleration_prach_buffer_factory.cpp:10-15`, `pdsch_grid_output_strategy.cpp:9-12`, `modulation_factories.cpp` (include block at top, CUDA classes in `#ifdef` regions), `lower_phy_factory.cpp:102-121`, `compression_factory.cpp:54-58`. Selection helpers exposed to generic code are CUDA-free declarations (`phy_acceleration_resource_grid_factory.h:22-28` — note `phy_acceleration_cuda_current_device_is_discrete()` is *declared* CUDA-free and *implemented* behind the guard).

3. **Generic processors touch only interfaces.** `pusch_processor_impl.cpp` includes no CUDA header (include list at `pusch_processor_impl.cpp:1-24`); its acceleration awareness is limited to env-parsing helpers (`phy_acceleration_runtime_options.h`, `pusch_acceleration_runtime_options.h` — pure `<cstdlib>/<cstring>`) and the opaque resident contracts. Same for `pdsch_processor_flexible_impl`/`downlink_processor_multi_executor_impl` which go through the `pdsch_grid_output_strategy` interface (`lib/phy/upper/pdsch_grid_output_strategy.h`, strategy impls in `.cpp` behind `#ifdef`).

4. **CMake linkage gating** (§1.1): CUDA-including TUs are only compiled with `ENABLE_CUDA` when `CUDA_ACCEL_FOUND`; a CUDA-disabled build compiles the same generic sources with the `#else` stubs returning `nullptr`/`false` (e.g. `demodulator_factories.cpp:263-276`, `pdsch/factories.cpp:536-548`).

**Boundary leak notes (minor, verified):** `lib/phy/upper/resource_grid_pinned_impl.h` and `resource_grid_cuda_visible_impl.h` include `<cuda_runtime.h>` and live in the *generic* `lib/phy/upper/` directory (not `include/`), pulled in only by ENABLE_CUDA-guarded TUs — the doc's layering claim (`phy_acceleration.dox:4-8,53-67`) holds at the public-interface level but these private headers sit outside the `cuda/` subdirectories the doc implies. Also the CPU-side files `pusch_processor_impl.cpp` / `pdsch_block_processor_gpu_impl.cpp` are listed in the same generic dirs as their host peers (the `_gpu_` files are CUDA-including but build-gated). The doc's testing expectation "CUDA-disabled upper-PHY builds to prove concrete accelerator types do not leak" (`phy_acceleration.dox:158-159`) matches this structure.

**Doc-vs-code verification summary for `phy_acceleration.dox`:** configuration-surface claims (§25-39) — verified (§2.1); layering claims (§41-67) — verified with the private-header caveat above; processing-path claims (§69-94) — verified (PUSCH resident path `pusch_processor_impl.cpp:41-99`; PDSCH strategy `pdsch_grid_output_strategy.cpp`; two-stage PRACH `modulation_factories.cpp:393-410` + `prach/factories.cpp:204-228`; OFH direct-to-grid decompress `lib/ofh/receiver/ofh_uplane_rx_symbol_data_flow_writer.cpp:125-140`); contract claims (§96-130) — verified against the default-returning interface hooks; runtime-policy claim that env vars are non-production overrides (§132-138) — **overstated**, see §2.2; "default OTA path on unified CUDA-memory platforms" (§140-143) — verified and correctly excludes discrete A100X, where auto-mode instead produces pinned grids + CPU PDSCH + (inconsistently) managed PRACH buffers.

---

# CUDA Upper-PHY Backends — Architecture Map (WG1 fork `cuda_accel.26_04`)

Scope: CUDA delta between mainline base `092414aac2` and `ccdf4e681f` in `/home/user/cuda_accelerated_ocudu`. All paths relative to repo root. Facts are verified-by-reading unless explicitly tagged **[hypothesis]**.

## 0. Backend layering (verified)

The upper-PHY acceleration is three layers:

1. **Raw CUDA kernel library** `lib/phy/cuda/` — a C API (`lib/phy/cuda/include/ocudu_phy_cuda.h`) with opaque handles (`ldpc_decoder_handle_t`, `rate_matcher_handle_t`, `scrambler_handle_t`, `modulator_handle_t`, `tb_encoder_handle_t`, `pusch_e2e_handle_t`, `polar_handle_t`, `prach_detector_*`, `srs_estimator_*`). Kernels: `lib/phy/cuda/src/ldpc_decoder_flexible.cu` (6,480 LOC), `ldpc_encoder_flexible.cu`, `rate_matching.cu`, `scrambling.cu`, `modulation.cu`, `transport_block.cu`, `crc.cu`, `polar.cu`, `pusch_e2e.cu` (13,674 LOC fused UL pipeline), `prach_detector.cu`, `srs_estimator.cu`, `pdsch_fused.cu`.
2. **C++ adapters** in `lib/phy/upper/**` that implement/extend mainline PHY interfaces (`pusch_demodulator`, `pusch_decoder`, `prach_detector`, `srs_estimator`, `pdsch_block_processor`, `resource_grid_*`).
3. **Factories** selecting CUDA vs host per channel, driven by YAML knobs `pusch/pdsch/prach/srs_acceleration_mode` = auto|enabled|disabled (`apps/units/flexible_o_du/o_du_low/du_low_config_cli11_schema.cpp:256-293`) resolved in `lib/phy/upper/upper_phy_factories.cpp`.

Important negative finding (verified): the generic per-primitive CUDA wrappers in `lib/phy/upper/channel_coding/ldpc/cuda/` (`ldpc_decoder_cuda`, `ldpc_encoder_cuda`, `ldpc_encoder_cuda_batch`, `demodulation_mapper_cuda`, `modulation_mapper_cuda`, `pseudo_random_generator_cuda`, `crc_calculator_cuda`) and their factories (`ldpc_cuda_factories.cpp:94-206`) are **not wired into the production gNB factory graph** — `create_*_factory_cuda()` is referenced only by `ldpc_cuda_factories.*` itself and unit tests (grep over `lib/`; only test caller is `tests/unittests/phy/upper/channel_coding/ldpc/ldpc_encoder_gpu_cpu_test.cpp`). Production acceleration goes exclusively through the fused paths below.

## 1. Component → file table

| Component | CUDA kernels | Adapter/wrapper class | Factory / instantiation | Notes |
|---|---|---|---|---|
| **PUSCH demod (chest+EQ+demap+descramble)** | `lib/phy/cuda/src/pusch_e2e.cu`; entry points `pusch_e2e_process_full_gpu_optimized` (1 layer, `lib/phy/cuda/include/pusch_e2e.h:426`), `..._mimo_half` (2–4 layers, `pusch_e2e.h:449`), `..._with_deprecoding` (transform precoding, `pusch_e2e.h:250`); SCH/UCI compaction+short-UCI decode kernels `lib/phy/upper/channel_coding/ldpc/cuda/pusch_sch_llr_compactor.cu` (API `pusch_sch_llr_compactor.h:12-63`); polar UCI `lib/phy/cuda/src/polar.cu` | `pusch_demodulator_gpu_impl` (`lib/phy/upper/channel_processors/pusch/pusch_demodulator_gpu_impl.h:51`, `.cpp:574-828` ctor/GPU init) wrapped by per-PDU selector `pusch_demodulator_layer_select` (`demodulator_factories.cpp:99-165`) | `pusch_demodulator_factory_accelerated` (`lib/phy/upper/channel_processors/pusch/demodulator_factories.cpp:167-233`); selected at `lib/phy/upper/upper_phy_factories.cpp:895-927` | GPU also does DMRS gen + channel est + noise est + CFO inside `pusch_e2e` (config fields `pusch_demodulator_gpu_impl.cpp:2189-2235`) |
| **PUSCH decode (rate-dematch + LDPC + CRC + TB deseg)** | `ldpc_decoder_flexible.cu`; fused APIs `ldpc_decoder_decode_batch_half` (`lib/phy/cuda/include/ldpc_decoder.h:125`), `ldpc_decoder_decode_batch_half_from_rate_matched` (`ldpc_decoder.h:139`), `rate_matcher_deinterleave_and_dematch_batch_half` (`rate_matching.h:291`), `tb_desegment_and_check_crc_async` (`transport_block.h:637`), scrambler seq gen `scrambling.cu` | `pusch_codeblock_decoder_cuda_batch` (`lib/phy/upper/channel_coding/ldpc/cuda/pusch_codeblock_decoder_cuda_batch.{h:40,cpp}`) implementing `pusch_resident_codeblock_decoder`; consumed by `pusch_decoder_impl` (`lib/phy/upper/channel_processors/pusch/pusch_decoder_impl.h:23`, resident branch `.cpp:753-848`) | Bounded pool `bounded_pusch_batch_gpu_decoder_pool` (`lib/phy/upper/channel_processors/pusch/processor_factories.cpp:31-135`), created when `enable_resident_acceleration_decoder && is_cuda_available()` (`processor_factories.cpp:206-225`); enabled at `upper_phy_factories.cpp:846-852` | Pool sized to # PUSCH processors, cap 32 (`processor_factories.cpp:273`) |
| **LDPC dec (standalone adapter)** | same decoder kernels | `ldpc_decoder_cuda` (`lib/phy/upper/channel_coding/ldpc/cuda/ldpc_decoder_cuda.cpp:27-305`) | `create_ldpc_decoder_factory_cuda` (`ldpc_cuda_factories.cpp:94-97`) | **Test-only wiring** (see §0) |
| **LDPC enc (standalone adapter)** | `ldpc_encoder_flexible.cu` | `ldpc_encoder_cuda_impl` (`ldpc_encoder_cuda.cpp:96-360`, CPU fallback below 1,000 bits `:30,218`); batch variant `ldpc_encoder_cuda_batch.cpp:67-420` | `create_ldpc_encoder_factory_cuda` (`ldpc_cuda_factories.cpp:196-200`) | **Test-only wiring**; production DL encode is inside `pdsch_tb_encoder_cuda` |
| **Descrambler / demod-mapper / CRC / mod-mapper adapters** | `scrambling.cu`, `modulation.cu`, `crc.cu` | `pseudo_random_generator_cuda`, `demodulation_mapper_cuda` (CPU below `MIN_GPU_SYMBOLS`, `demodulation_mapper_cuda.cpp:222`), `crc_calculator_cuda`, `modulation_mapper_cuda` | `ldpc_cuda_factories.cpp:128-206` | **Test-only wiring** |
| **PDSCH encode+scramble+modulate** | `transport_block.cu` fused `tb_encoder_encode_to_symbols_int8` (CRC→segment→LDPC→RM→interleave→scramble→modulate→INT8, `transport_block.h:251`); alt API `pdsch_fused.cu` (tests only) | `pdsch_tb_encoder_gpu_impl` (`lib/phy/upper/channel_coding/ldpc/cuda/pdsch_tb_encoder_cuda.cu:52`, created `:622`); front-end `pdsch_block_processor_gpu_impl` (`lib/phy/upper/channel_processors/pdsch/pdsch_block_processor_gpu_impl.{h:27,cpp:99}`) | `pdsch_block_processor_factory_accelerated` (`lib/phy/upper/channel_processors/pdsch/factories.cpp:504-534`); selected `upper_phy_factories.cpp:1303-1331`; plugged into `pdsch_processor_flexible_impl` (`pdsch_processor_flexible_impl.cpp:231,394-401`) | JIT warm-up of all mod orders at ctor (`pdsch_tb_encoder_cuda.cu:190-268`) |
| **PDSCH map/grid write (device)** | `lib/phy/upper/channel_coding/ldpc/cuda/pdsch_resource_grid_mapper_cuda.cu` kernel `pdsch_map_layers_int8_to_bf16_real_grid` (called from `pdsch_block_processor_gpu_impl.cpp:294-302`) | mapper hook `resource_grid_mapper::symbol_buffer::map_to_device_grid` (`include/ocudu/phy/support/resource_grid_mapper.h:48-72`; driver `lib/phy/support/resource_grid_mapper_impl.cpp:393-467,662-670`); sidecar writer `pdsch_device_grid_writer_cuda` (`lib/phy/upper/channel_coding/ldpc/cuda/pdsch_device_grid_writer_cuda.h:16`) | strategy `create_pdsch_grid_output_strategy` (`lib/phy/upper/pdsch_grid_output_strategy.cpp:42-165`), used by `downlink_processor_multi_executor_impl.cpp:49,113,250,286` | Direct path requires CUDA-visible DL grid; sidecar path materializes to host before send (`pdsch_grid_output_strategy.cpp:87-94`) |
| **PRACH detector** | `lib/phy/cuda/src/prach_detector.cu` (+ vkFFT third-party under `lib/phy/cuda/third_party/vkfft/`) | `prach_detector_cuda_impl` (`lib/phy/upper/channel_processors/prach/prach_detector_cuda_impl.cpp:75-356`); roots generated on CPU via `prach_generator` (`:322-342`) | `prach_detector_factory_cuda` (`lib/phy/upper/channel_processors/prach/factories.cpp:104-142`); `create_prach_detector_factory_accelerated` (`factories.cpp:205-227`); env override `OCUDU_PRACH_ACCELERATION` (`factories.cpp:37-58`) | Per-call GPU/CPU heuristic (`prach_detector_cuda_impl.cpp:211-235`) |
| **SRS estimator** | `lib/phy/cuda/src/srs_estimator.cu` | `srs_estimator_cuda_impl` (`lib/phy/upper/signal_processors/srs/srs_estimator_cuda_impl.cpp:147-401`); host-grid staging via `pusch_device_grid_reader_cuda` (`srs_estimator_cuda_impl.cpp:361-370`; class `lib/phy/upper/channel_coding/ldpc/cuda/pusch_device_grid_reader_cuda.h:12`) | `srs_estimator_factory.cpp:96-111` (enabled/auto), env `OCUDU_SRS_ACCELERATION` (`:39-63`); wired `upper_phy_factories.cpp:665-686` | SRS sequences generated on CPU, uploaded at (re)configure (`srs_estimator_cuda_impl.cpp:332-352`) |

## 2. Resident-buffer / opaque-token contract

### 2.1 Token & handle types (all CUDA-free in `include/`)

| Type | File:line | Purpose |
|---|---|---|
| `resident_softbit_buffer` | `include/ocudu/phy/upper/acceleration/phy_acceleration.h:23-40` | `void* data` (device FP16 LLRs), `nof_softbits`, `void* execution_context` (a `cudaStream_t` in practice), `void* completion_token` (a `cudaEvent_t`), `valid`, `buffer_index` (triple-buffer ring slot) |
| `resident_uci_buffer` | `phy_acceleration.h:43-66` | host-side compact HARQ/CSI1 LLR spans and/or device-decoded payloads + `uci_status` |
| `pusch_resident_sch_compaction` / `pusch_resident_demodulation_config` | `phy_acceleration.h:69-98` | UCI demux geometry for device-side SCH compaction; carried in `pusch_demodulator::configuration::resident` (`include/.../pusch/pusch_demodulator.h:78`) |
| `pusch_resident_codeblock_decoder` (+ timing struct) | `include/.../pusch/pusch_resident_codeblock_decoder.h:62-137, 20-59` | backend-neutral resident decoder interface; `decode_resident_softbits(void* resident_llrs, …, void* execution_context, int buffer_index)` `:115-124` |

### 2.2 Interface extensions in `include/` (virtuals with host-safe defaults; opaque `void*` only)

- `resource_grid_reader`: `supports_device_grid_reading / get_device_grid_cbf16 / get_device_grid_ready_event / prepare_device_grid_reading(void*) / on_device_grid_reading_enqueued(void*) / synchronize_device_grid_reading` — `include/ocudu/phy/support/resource_grid_reader.h:111-126`.
- `resource_grid_writer`: `supports_device_grid_mapping / device_grid_mapping_aliases_host_grid / get_device_grid_bf16 / prepare_device_grid_mapping / on_device_grid_mapping_enqueued / cancel_device_grid_mapping / synchronize_device_grid_mapping` — `include/ocudu/phy/support/resource_grid_writer.h:93-114`.
- `resource_grid_mapper::symbol_buffer`: `supports_device_grid_mapping / map_to_device_grid / get_device_grid_mapping_stream` — `include/ocudu/phy/support/resource_grid_mapper.h:48-72`.
- `prach_buffer`: `supports_device_prach_buffer_{reading,mapping}`, `get_device_prach_buffer_cbf16`, `get_device_prach_symbol_offset`, `prepare_/on_/cancel_/synchronize_device_prach_buffer_*` — `include/ocudu/phy/support/prach_buffer.h:61-108`.
- `pusch_demodulator`: resident mode enable/disable, `get_resident_softbits()`, `get_resident_uci()`, `finalize_resident_uci()`, `used_host_codeword_fallback()`, `report_deferred_sinr()` — `include/.../pusch/pusch_demodulator.h:106-141`.
- `pusch_decoder`: `try_decode_resident(resident_softbit_buffer)`, `supports_resident_decode`, `enable/disable_resident_decode`, `set_demod_gap_timing`, `set_pre_join_callback` — `include/.../pusch/pusch_decoder.h:85-126`.

### 2.3 Device-memory ownership & host↔device synchronization

| Buffer | Owner / allocation | Sync point |
|---|---|---|
| UL resource grid (CUDA-visible) | `cuda_visible_resource_grid` — `cudaMallocManaged` (`lib/phy/upper/resource_grid_cuda_visible_impl.h:89-102`), `cudaMemAdvise` hints `:521-531` | Reader hooks `:173-183`; host `put()` forces `prepare_host_access()` (`:254`); device readiness via per-grid event ring; writer device-mapping records event + clears empty flags (`:286-301`) |
| UL grid fallback (pinned) | `resource_grid_pinned_factory` — heap grid page-locked via `cudaHostRegister` (`lib/phy/upper/resource_grid_pinned_impl.h:22-62`) | No device pointer exposed → consumers do explicit H2D `cudaMemcpyAsync` DMA |
| Grid-selection policy | `resource_grid_cuda_visible_factory` (`resource_grid_cuda_visible_impl.h:597-675`): env `OCUDU_UL/DL_CUDA_VISIBLE_GRID` = managed/pinned/auto; **auto = managed only if `cudaDevAttrIntegrated`** (`:658-670`); wired per-direction in `create_phy_acceleration_resource_grid_factory` (`lib/phy/upper/phy_acceleration_resource_grid_factory.cpp:37-58`, use sites `upper_phy_factories.cpp:478-482,1030-1033`) | — |
| Demod device buffers | `pusch_demodulator_gpu_impl` owns triple-buffered unified input (`h_unified_staging_/d_unified_input_`), FP16 LLR outputs `d_llrs_half_[3]`, compact SCH/UCI buffers, streams `stream_/h2d_stream_/d2h_stream_`, events `h2d_complete_/kernel_complete_/d2h_complete_` (`pusch_demodulator_gpu_impl.h:291-345`) | Resident+no-UCI: **no sync in demodulator** — returns after recording `kernel_complete_` (`pusch_demodulator_gpu_impl.cpp:2679-2705`); non-resident: event sync then FP16→INT8 LUT convert into host codeword buffer (`:2708-2820`) |
| Resident LLR handoff | Token built in `get_resident_softbits()` (`pusch_demodulator_gpu_impl.h:95-108`): `execution_context = stream_`, `completion_token = kernel_complete_[last_buf_]` | Decoder enqueues its kernels on the **demodulator's stream** (`pusch_codeblock_decoder_cuda_batch.cpp:1097-1098`) so ordering is by stream, then does the single blocking wait at `completion_event_` (`:1447-1462`) |
| Decoder device buffers | `pusch_codeblock_decoder_cuda_batch` owns fp32/fp16 LLR batches, output words, TB output, CRC scratch, pinned host mailboxes (`pusch_codeblock_decoder_cuda_batch.h:206-275`) | Single end-of-pipeline sync per TB (above); D2H of TB bytes/CB CRCs/TB CRC queued before it (`.cpp:1410-1441`) |
| Deferred stats | SINR/EPRE/RSRP/TA/CFO/EVM D2H launched async pre-handoff (`pusch_demodulator_gpu_impl.cpp:2676`); read back **after** decoder sync via `set_pre_join_callback` → `report_deferred_sinr` (`pusch_processor_impl.cpp:726-751`; `pusch_decoder_impl.cpp:835-838`; `pusch_demodulator_gpu_impl.cpp:3083-3120`) | — |
| Resident UCI | Compact HARQ/CSI1 LLRs + optional device-decoded payloads D2H into pinned mailboxes (`pusch_demodulator_gpu_impl.cpp:2621-2670`); `finalize_resident_uci()` converts FP16→INT8 or publishes payloads after event wait (`:2951-3081`) | Waits on `kernel_complete_[last_buf_]` only if still pending (`:2961-2971`) |
| PRACH buffer | `cuda_visible_prach_buffer` — `cudaMallocManaged` (`lib/phy/upper/prach_buffer_cuda_visible_impl.h:90`); host `get_symbol` forces `prepare_host_access()` (`:130,137`); device hooks `:177-185` | Factory: `phy_acceleration_prach_buffer_factory.cpp:58-101`; **auto policy = `cudaDevAttrManagedMemory`**, not integrated (`:22-34`) |
| PDSCH symbols | `pdsch_tb_encoder_gpu_impl` owns `d_tb_input_`, `d_coded_bits_`, `d_symbols_int8_`, pinned host mirrors (`pdsch_tb_encoder_cuda.cu:116-155`); block processor owns `d_mapper_re_offsets`, `d_mapper_grid_bf16` scratch (`pdsch_block_processor_gpu_impl.cpp:110-128`) | Host path: D2H of INT8 symbols + event poll in `get_symbols` (`pdsch_tb_encoder_cuda.cu:383-395`); device path: mapping enqueued on encoder stream, grid-ready event recorded via `on_device_grid_mapping_enqueued`; host materialization (sidecar) in `before_send_grid` (`pdsch_grid_output_strategy.cpp:87-94`) |

## 3. UL PUSCH accelerated flow, end to end (resident mode)

Orchestrated by `pusch_processor_impl::process` / `process_data` (`lib/phy/upper/channel_processors/pusch/pusch_processor_impl.cpp`). Resident plan: `make_resident_sch_plan` (`:105-121`), gating predicate `supports_resident_accelerated_pusch_demodulation` (`:41-51`), activation via `scoped_resident_sch_mode` (`:123-151,656`).

| Stage | Where it runs | file:line | H↔D crossing |
|---|---|---|---|
| Resource grid ingest | Grid produced by lower PHY/OFH into CUDA-visible managed grid (if selected) or pinned host grid | grid select `upper_phy_factories.cpp:1030-1033`; managed grid `resource_grid_cuda_visible_impl.h:89-102` | Managed: none explicit (UVM); pinned: full-grid `cudaMemcpyAsync` H2D per PUSCH PDU, coalesced across ports when contiguous (`pusch_demodulator_gpu_impl.cpp:2307-2360`) |
| CPU channel estimator | **Bypassed** when plan allows (`can_bypass_channel_estimator`): stub results injected, `estimator.estimate` skipped | `pusch_processor_impl.cpp:343-395` (stub class `:232-261`); otherwise CPU estimate at `:415-431` | — |
| Channel estimation (DMRS gen, LS, time/freq interp, noise, CFO) | **GPU**, inside fused `pusch_e2e` kernel; configured with DMRS type/mask/scrambling, interp & noise modes | config `pusch_demodulator_gpu_impl.cpp:2144-2280`; comment "E2E path computes channel estimates internally" `:1750-1751` | none |
| Equalization (MMSE/ZF, 1–4 layers × 1/2/4/8 ports) | **GPU** in same kernel | launch `:2403-2421`; algorithm select `:2214-2215` | none |
| Transform de-precoding (MSG3) | **GPU** (`pusch_e2e_process_full_gpu_with_deprecoding`) | `:2403-2414`; preplan `:748-752` | none |
| Soft demod → FP16 LLRs | **GPU**, same kernel; output `d_llrs_half_[buf]` | `:2415-2421` | none |
| Descramble | **GPU**, fused on-the-fly Gold sequence inside E2E kernel | `lib/phy/cuda/src/pusch_e2e.cu:4281+` ("ON-THE-FLY scrambling"); confirmed by decoder passing `nullptr` scrambling with comment "already descrambled by E2E kernel" (`pusch_decoder_impl.cpp:810`) | none |
| UCI/SCH demux (UCI-bearing PDU, opt-in) | **GPU** compaction of SCH LLRs + HARQ/CSI1 extraction; short-block (≤11 bit) and polar (>11 bit) UCI decode on GPU | RE-index build (CPU, cached) `pusch_demodulator_gpu_impl.cpp:385-537,1894-1977`; kernels `:2546-2591` (`pusch_compact_sch_and_decode_uci_short_blocks_half`), `:2596-2645` (polar) | compact UCI LLRs + decode results D2H to pinned mailboxes `:2621-2670` |
| Handoff | Demod returns without sync; processor pulls `resident_softbit_buffer` and calls `decoder->try_decode_resident` | `pusch_processor_impl.cpp:669-671, 715-760`; token `pusch_demodulator_gpu_impl.h:95-108` | none (device pointer + stream passed) |
| Segmentation planning | **CPU** — `ldpc_segmenter_rx::segment` computes per-CB metadata (no data movement) in `set_nof_softbits` | `pusch_decoder_impl.cpp:165-211` (`:192`) | none |
| Rate dematch + deinterleave | **GPU**: uniform-E fused FP16 `rate_matcher_deinterleave_and_dematch_batch_half` (`pusch_codeblock_decoder_cuda_batch.cpp:1258-1268`); "direct-RM" fused into decoder for BG1/Z=384 batches ≥64 CBs (`:1149-1242`); non-uniform E handled per-(E,F)-run (`:1285-1379`) | | none |
| LDPC decode | **GPU** `ldpc_decoder_decode_batch_half[_from_rate_matched]` (`:1275-1276, 1217-1229`); algorithm knob `set_ldpc_decoder_algorithm` (`.h:132`) | | none |
| CB CRC + TB desegment + TB CRC | **GPU** `tb_desegment_and_check_crc_async` (`:1393-1404`); fail-closed CB-CRC fallback `:1428-1439` | | TB payload bytes, per-CB CRC ints, TB CRC int D2H (`:1416-1441`); **single blocking sync** on completion event (`:1447-1462`) — this is where the whole UL pipeline joins the CPU |
| HARQ soft combining | **Not performed in the resident path** — `decode_resident_softbits` receives no prior soft buffers; retransmissions are decoded standalone from fresh LLRs (`pusch_decoder_impl.cpp:807-817` passes only device LLRs + metadata). Host-staged `decode_batch` path does CPU combine for retx (`pusch_codeblock_decoder_cuda_batch.cpp:836-846, 965-984`) | | — |
| CRC result / notify | CPU: `gpu_tb_crc_result_` consumed by `join_and_notify` (`pusch_decoder_impl.cpp:611-618, 820-845`) | | — |
| SINR/CSI report | CPU reads pinned mailbox post-sync (`report_deferred_sinr`, `pusch_demodulator_gpu_impl.cpp:3083-3120`) via pre-join callback (`pusch_processor_impl.cpp:726-751`) | | tiny D2H already queued at `:2676` |
| UCI notify | CPU publishes device payloads or feeds compact LLRs into host UCI decoders (`pusch_processor_impl.cpp:683-713, 746-750`) | | see mailboxes above |

Non-resident accelerated fallbacks within the demodulator: GPU-batch path (CPU equalization + GPU demod/descramble, ≥256 REs, `pusch_demodulator_gpu_impl.cpp:571-572, 1482-1504, 1563-1723`) and full-CPU path (`:1505-1528, 2906+`).

## 4. DL PDSCH accelerated flow (flexible processor + GPU block processor)

| Stage | Where it runs | file:line | H↔D crossing |
|---|---|---|---|
| Segmentation planning | **CPU** `ldpc_segmenter_tx::new_transmission` (metadata only: nof CBs, Z, E_short/long, filler) | `pdsch_processor_flexible_impl.cpp:131-144`; consumed at `pdsch_block_processor_gpu_impl.cpp:350-381` | — |
| TB upload | pinned staging memcpy + `cudaMemcpyAsync` H2D of raw TB bytes | `pdsch_tb_encoder_cuda.cu:361-365` | H2D (TB bytes only) |
| TB CRC (24A/16) + CB segmentation + CB CRC24B | **GPU**, inside `tb_encoder_encode_to_symbols_int8` | call `pdsch_tb_encoder_cuda.cu:371`; API `transport_block.h:251` | none |
| LDPC encode | **GPU** (same fused call; kernels `ldpc_encoder_flexible.cu`) | ibid. | none |
| Rate match + interleave | **GPU** (same fused call) | ibid. | none |
| Scramble + modulate → INT8 IQ | **GPU** (same fused call; standalone fused kernel `modulator_scramble_and_modulate_int8` also exists, `pdsch_tb_encoder_cuda.cu:224-257`) | ibid. | none |
| Whole-TB encode is cached and sliced per CB-batch | CPU bookkeeping | `pdsch_block_processor_gpu_impl.cpp:182-187, 405-441` | — |
| Layer map + precode + grid write — **device path** | **GPU** kernel `pdsch_map_layers_int8_to_bf16_real_grid` maps INT8 symbols → BF16 grid REs; only for identical real precoding weight across ports (`resource_grid_mapper_impl.cpp:399-405`) and layers==1 or layers==ports (`pdsch_block_processor_gpu_impl.cpp:189-193`) | driver `resource_grid_mapper_impl.cpp:393-467` (attempt at `:662-670`); launch `pdsch_block_processor_gpu_impl.cpp:294-302` | RE-offset list H2D (cached, `:253-262`). Destination = writer's device grid (direct CUDA-visible DL grid) or sidecar `pdsch_device_grid_writer_cuda` scratch |
| Grid write — **host path** (default when device map unsupported/disabled) | CPU: `pop_symbols` pulls INT8 symbols (D2H via pinned buffer, `pdsch_tb_encoder_cuda.cu:380-395`; `pdsch_block_processor_gpu_impl.cpp:444-469`) then mainline mapper precodes and `put()`s into host grid | | D2H (all modulated symbols) |
| Before send to lower PHY | Direct CUDA-visible grid: consumers use `synchronize_device_grid_mapping` (`resource_grid_cuda_visible_impl.h:301`); Sidecar: `materialize_nonzero_device_grid_to_host` copies device grid back and scans nonzero REs into host writer (`pdsch_grid_output_strategy.cpp:87-94`; writer `pdsch_device_grid_writer_cuda.h:94`) | `downlink_processor_multi_executor_impl.cpp:283-287` | Sidecar: full-grid D2H + host scan |

## 5. CPU fallbacks that remain in *enabled* mode

**PUSCH:**
- Per-PDU SW demodulator when layers∉{1..4}, ports∉{1,2,4,8}, layers>ports, or transform precoding with >1 layer — `pusch_demodulator_layer_select::demodulate` (`demodulator_factories.cpp:113-121, 152-161`).
- Resident pipeline refused (host demux + host/batch decode used instead) when: no SCH data, CSI Part 2 configured, UCI present without opt-in device UCI (`OCUDU_PUSCH_ENABLE_ACCELERATED_UCI`, `pusch_acceleration_runtime_options.h:49-52`; policy note re OTA HARQ misses `pusch_processor_impl.cpp:60-66`), or small-grant thresholds `OCUDU_PUSCH_ACCELERATION_MIN_RB/_MIN_ULSCH_BITS` (defaults 0 = off, `pusch_acceleration_runtime_options.h:14-47`) — plan predicates `pusch_processor_impl.cpp:79-103`.
- Inside GPU demod: capacity/config/kernel failures → `process_cpu_grid_fallback` (CPU EQ + GPU/CPU demod) at `pusch_demodulator_gpu_impl.cpp:1829-1835, 1882-1889, 1967-1977, 2237-2243, 2431-2440`; CPU-EQ path when E2E handle creation failed (`:753-762`); small accumulations (<256 REs) → full CPU (`:571-572, 1505-1528`).
- Decoder: no GPU decoder available → per-CB CPU LDPC (`pusch_decoder_impl.cpp:472-478`); host-staged batch path does **CPU rate-dematch/HARQ combining** for retransmissions or non-uniform CB configs (`pusch_codeblock_decoder_cuda_batch.cpp:836-846, 965-1003`).
- **No host fallback after resident demod succeeded**: failed resident handoff is fatal (`report_fatal_error`, `pusch_processor_impl.cpp:758-766`); only a demodulator-internal fallback that wrote host softbits (`used_host_codeword_fallback`, `:762-764`) continues on CPU.
- UCI: polar handle creation failure → host UCI decode (`pusch_demodulator_gpu_impl.cpp:764-776`); device short-UCI decode extra-gated by `OCUDU_PUSCH_ACCELERATION_UCI_DEVICE_DECODE` (`:1937-1938`); device-decode CRC failure → host demap of compact LLRs (`:3018-3051`, D2H always queued for this at `:2613-2626`).

**PDSCH:**
- `auto` mode selects **host** backend on discrete GPUs unless `OCUDU_PDSCH_AUTO_ENABLE_DISCRETE=1` (`upper_phy_factories.cpp:51-68`).
- Device grid mapper disabled by default on discrete GPUs (`is_gpu_mapper_disabled`, `pdsch_block_processor_gpu_impl.cpp:41-77`) → host `pop_symbols` mapping with symbol D2H.
- Device map refused for multi-layer partial CB batches (`:206-208`), non-matching port/layer counts (`:202-204`), non-uniform/complex precoding weights (`resource_grid_mapper_impl.cpp:399-405`); any refusal falls back to host mapping loop (`resource_grid_mapper_impl.cpp:662-673`).
- Sidecar writer allocation failure → host grid mapping (`pdsch_grid_output_strategy.cpp:62-68`).

**PRACH:** heuristics keep CPU for small workloads — short formats below `OCUDU_PRACH_ACCELERATION_MIN_SHORT_WORK` (default 128 port×preambles) unless device-readable buffer with ≥128 preambles; long formats below 4 port×sequences (`prach_detector_cuda_impl.cpp:211-235`); GPU detect failure → CPU fallback detector (`:298-301`).

**SRS:** CPU fallback when workload < `OCUDU_SRS_ACCELERATION_MIN_PILOT_RE` (default 4096 observed pilots, `srs_estimator_cuda_impl.cpp:76-100, 265-267`), sequence length/port/DFT caps exceeded (`:255-263`), or configure/estimate failure (`:346-349, 374-377`).

## 6. A100X (discrete sm_80, non-coherent) vs GB10 dev-platform assumptions

1. **UL grid**: auto policy gives managed CUDA-visible grid only on integrated GPUs (`resource_grid_cuda_visible_impl.h:658-670`). On A100X the default is the pinned-host grid → PUSCH E2E always takes the "host grid copy" branch: one full-grid `cudaMemcpyAsync` per PUSCH PDU (`pusch_demodulator_gpu_impl.cpp:2307-2360`, path log `:1873-1878`). Forcing `OCUDU_UL_CUDA_VISIBLE_GRID=managed` uses `cudaMallocManaged` which on x86+PCIe means page migration, not coherence — **[hypothesis]** per-slot host-write/device-read ping-pong will be costly vs GB10.
2. **DL PDSCH**: both the auto-enable gate (`upper_phy_factories.cpp:51-68`) and the device mapper (`pdsch_block_processor_gpu_impl.cpp:65-75`, comment explicitly says the sidecar path is slower than host mapping on discrete GPUs) are wired to prefer host paths on discrete devices; direct-grid default is documented as "OTA-stable default on the GB10/B210 CUDA-visible-grid setup" (`pdsch_grid_output_strategy.cpp:25-27`). On A100X, enabled-mode PDSCH = GPU encode + D2H symbols + CPU grid mapping unless env-forced.
3. **PRACH buffer policy inconsistency**: auto managed-buffer selection keys off `cudaDevAttrManagedMemory` (`phy_acceleration_prach_buffer_factory.cpp:22-34`) — true on A100X — while grids key off `cudaDevAttrIntegrated`. So on A100X the CUDA-visible (managed) PRACH buffer is auto-selected by default when PRACH acceleration is on; host OFH writes + GPU reads over UVM/PCIe — **[hypothesis]** migration overhead; override via `OCUDU_UL_CUDA_VISIBLE_PRACH_BUFFER=pinned/off` (`:38-49`).
4. Pinned-grid rationale comments reference GH200/NVLink (`resource_grid_pinned_impl.h:16-21`); on A100X the same code works but DMA is PCIe-bound — **[hypothesis]** the per-slot full-grid H2D (~ up to 4 ports × 14 × 3276 × 4 B ≈ 2.9 MB/slot for 100 MHz) becomes the dominant handoff cost.
5. **[hypothesis / latent edge case]** In resident mode, if the E2E handle fails to create (`use_gpu_equalization_=false`, `pusch_demodulator_gpu_impl.cpp:756-762`) the demodulator can take the GPU-batch path, which sets `gpu_llrs_valid_=true` but never sets `last_resident_llrs_` (only reset at `:1208`, set only in E2E at `:2685`) and early-returns without host softbits (`:1670-1672`); `get_resident_softbits()` then returns `valid=true,data=nullptr`, `try_decode_resident` rejects it (`pusch_decoder_impl.cpp:763-766`), and the processor hits `report_fatal_error` (`pusch_processor_impl.cpp:758-759`). Worth testing on A100X bring-up since a discrete-platform init failure is the most plausible trigger.

---

# CUDA Lower-PHY and OFH Backend Map (WG1 fork, branch `cuda_accel.26_04`)

All file:line references are relative to `/home/user/cuda_accelerated_ocudu`. Facts below are verified by reading the code unless explicitly marked **[hypothesis]**.

---

## 1. Component → file table (factory entry points)

| Component | Implementation | Factory / entry point |
|---|---|---|
| PDxCH TX accelerator interface | `lib/phy/lower/processors/downlink/pdxch/pdxch_baseband_modulator_accelerator.h:24` | `create_pdxch_baseband_modulator_accelerator_cuda()` decl at `pdxch_baseband_modulator_accelerator.h:55-62`, def at `lib/phy/lower/processors/downlink/pdxch/pdxch_baseband_modulator_cuda.cpp:369-380` |
| PDxCH TX CUDA accelerator | `lib/phy/lower/processors/downlink/pdxch/pdxch_baseband_modulator_cuda.cpp:62` (class `pdxch_baseband_modulator_cuda`) | wired via `pdxch_processor_factory_cuda::create()` at `lib/phy/lower/processors/downlink/pdxch/pdxch_processor_factories.cpp:54-103`; public creators `create_pdxch_processor_factory_cuda()` / `is_pdxch_processor_factory_cuda_available()` at `pdxch_processor_factories.cpp:116-142`, private header `pdxch_processor_factories_cuda.h:13-23` |
| PDxCH TX C/CUDA kernel library | `lib/phy/cuda/src/low_phy_tx.cu` (API `lib/phy/cuda/include/low_phy_tx.h:36-53`) | `ocudu_lowphy_tx_create()` at `low_phy_tx.cu:862` |
| PDxCH slot modulator (host FSM, accelerator-aware) | `lib/phy/lower/processors/downlink/pdxch/pdxch_baseband_modulator.h:46` | constructed in `pdxch_processor_impl` ctor `lib/phy/lower/processors/downlink/pdxch/pdxch_processor_impl.h:48-75` (accelerator factory arg at :53, one accelerator per modulator at :72) |
| PUxCH RX CUDA OFDM demodulator | `lib/phy/lower/modulation/ofdm_demodulator_cuda_impl.{h:16,cpp:102}` | `create_ofdm_demodulator_factory_accelerated()` at `lib/phy/lower/modulation/modulation_factories.cpp` (diff hunk adding it; factory class `ofdm_demodulator_factory_cuda` wraps a CPU fallback factory) |
| PUxCH RX CUDA kernel library | `lib/phy/cuda/src/low_phy_puxch_rx.cu` (API `lib/phy/cuda/include/low_phy_puxch_rx.h:35-59`) | `ocudu_lowphy_puxch_rx_create()` at `low_phy_puxch_rx.cu:929` |
| PRACH CUDA demodulator | `lib/phy/lower/modulation/ofdm_prach_demodulator_cuda_impl.{h:15,cpp:60}` | `create_ofdm_prach_demodulator_factory_accelerated()` (diff in `modulation_factories.cpp`; class `ofdm_prach_demodulator_factory_cuda` wraps CPU fallback) |
| PRACH CUDA kernel library | `lib/phy/cuda/src/low_phy_prach_rx.cu` (API `lib/phy/cuda/include/low_phy_prach_rx.h`) | `ocudu_lowphy_prach_rx_create()` at `low_phy_prach_rx.cu:584` |
| CUDA-visible (managed) resource grid | `lib/phy/upper/resource_grid_cuda_visible_impl.h:61` (`cuda_visible_resource_grid`) | `resource_grid_cuda_visible_factory` at `resource_grid_cuda_visible_impl.h:597-675`; pinned fallback `lib/phy/upper/resource_grid_pinned_impl.h:22` |
| CUDA-visible PRACH buffer | `lib/phy/upper/prach_buffer_cuda_visible_impl.h` (`cuda_visible_prach_buffer`, managed alloc at :90) | `lib/phy/upper/phy_acceleration_prach_buffer_factory.cpp:63-87` |
| Device residency hooks on grid interfaces | reader: `include/ocudu/phy/support/resource_grid_reader.h:110-126`; writer: `include/ocudu/phy/support/resource_grid_writer.h:92-114`; PRACH buffer: `include/ocudu/phy/support/prach_buffer.h:60-107` | virtual defaults = "no device support" |
| Lower-PHY factory selection (SDR split-8) | `lib/ru/sdr/lower_phy/lower_phy_factory.cpp:80-135` | TX CUDA select at :102-121, RX at :81-83, PRACH at :86-89 |
| Dedicated PRACH executor | `apps/services/worker_manager/worker_manager.cpp:542,600-623,646-676`; interface `include/ocudu/ru/sdr/ru_sdr_executor_mapper.h:29,87-88,109-110` | worker `lphy_prach#N` @ RT prio max−5; consumed by `prach_processor_worker.cpp:123` (`execute`, not `defer`) |
| OFH CUDA IQ (de)compressor | `lib/ofh/compression/iq_compression_cuda.{h:20,cpp:40}` | selected in `create_iq_compressor()` / `create_iq_decompressor()`, `lib/ofh/compression/compression_factory.cpp` (CUDA branch inside `none` and `BFP` cases; `resolve_compression_impl_type()` maps `auto`→`cuda` when a CUDA device exists) |
| OFH CUDA compression kernel library | `lib/phy/cuda/src/ofh_compression.cu` (API `lib/phy/cuda/include/ofh_compression.h:19-274`) | `ocudu_ofh_compression_create()` at `ofh_compression.cu:2993` |
| OFH U-plane builder (device-grid batch path) | `lib/ofh/serdes/ofh_uplane_message_builder_impl.cpp:128-159,204-295,297-…` | invoked by DL data flow `lib/ofh/transmitter/ofh_data_flow_uplane_downlink_data_impl.cpp:89-120,220-343,345-480` |
| OFH U-plane decoder (keep-compressed path) | `lib/ofh/serdes/ofh_uplane_message_decoder_impl.cpp` (`decode_with_compressed_iq`, `decompress_to_resource_grid`, `decompress_to_prach_buffer`) | called from `lib/ofh/receiver/ofh_data_flow_uplane_uplink_data_impl.cpp:33-45` and `ofh_data_flow_uplane_uplink_prach_impl.cpp:95-105` |
| OFH resident symbol writers | UL grid: `lib/ofh/receiver/ofh_uplane_rx_symbol_data_flow_writer.cpp:83-155`; PRACH: `lib/ofh/receiver/ofh_uplane_prach_symbol_data_flow_writer.cpp:107-207` | write into repositories via `uplink_context_repository.h:90-105,206-218` (`write_grid_device`) and `prach_context_repository.h:167-196,290-302` (`write_iq_device`) |

Config surface: `--low_phy_tx_acceleration_mode`, `--low_phy_rx_acceleration_mode`, `--low_phy_prach_demodulation_acceleration_mode` (`apps/units/flexible_o_du/split_8/helpers/ru_sdr_config_cli11_schema.cpp:68-84`), OFH `--compression_acceleration_mode` (`apps/units/flexible_o_du/split_7_2/helpers/ru_ofh_config_cli11_schema.cpp:164`); all default `"auto"` (`include/ocudu/phy/lower/lower_phy_configuration.h:98-115`, `include/ocudu/ofh/ofh_sector_config.h` diff).

---

## 2. Lower-PHY TX CUDA path (PDxCH → baseband)

**What is offloaded: the entire slot baseband build** — resource-grid→IFFT-input mapping (frequency shift + cbf16→cf32), batched inverse FFT via bundled VkFFT, CP insertion, per-symbol phase compensation, OFDM scaling, amplitude gain, optional clipping, and cf32→sc16 quantization — for all ports × all symbols of one slot in one submission:

- `pdxch_baseband_modulator::handle_request()` tries `accelerator->enqueue()` for the whole slot before falling back to per-symbol/per-port CPU tasks: `lib/phy/lower/processors/downlink/pdxch/pdxch_baseband_modulator.h:159-172`.
- Kernel 1 `prepare_ifft_input_kernel` (grid cbf16 → dft-order cf32, zero padding, fftshift): `lib/phy/cuda/src/low_phy_tx.cu:108-141`.
- VkFFT batched inverse FFT (batches = ports × symbols, `makeInversePlanOnly`): `low_phy_tx.cu:487-507,619-622`.
- Kernel 2 `postprocess_ifft_to_sc16_kernel` — CP insertion (`src_idx` wrap at :160), phase compensation (:164-167), `ofdm_scale × amplitude_gain` (:166-167), clipping to `clipping_ceiling` (:169-177), int16 quantization ×32767 (:179-181). Amplitude-controller parameters are baked into the kernel config from `amplitude_controller_clipping_config` at `pdxch_baseband_modulator_cuda.cpp:272-275`; phase table from host `phase_compensation_lut` at :283-285.
- Optional CUDA-graph capture/replay of the 3-stage pipeline (env `OCUDU_LOWPHY_TX_CUDA_GRAPHS`): `low_phy_tx.cu:696-731`.

**Grid input, two modes** (`pdxch_baseband_modulator_cuda.cpp:141-173`):
1. **Device-grid path**: if `grid.supports_device_grid_reading()` (i.e. the DL grid is `cuda_visible_resource_grid` managed memory), kernels read the grid pointer directly (`ocudu_lowphy_tx_process`, no grid copy).
2. **Host-grid staging fallback**: requires the grid views to be one contiguous block (`get_contiguous_host_grid()`, :323-341); host `memcpy` into one of 8 write-combined pinned slots then `cudaMemcpyAsync` H2D (`low_phy_tx.cu:977-992`). Can be disabled with `OCUDU_LOWPHY_TX_HOST_GRID_STAGING=0` (:85).

**Where baseband samples return to host** (`copy_output_to_host`, `low_phy_tx.cu:756-841`):
- Preferred: async `cudaMemcpyAsync` D2H **directly into the radio's `baseband_gateway_buffer_dynamic` ci16 buffers**, which were pre-pinned at startup via `cudaHostRegister` (`ocudu_lowphy_tx_register_host_output`, `low_phy_tx.cu:938-944`), triggered from `pdxch_processor_impl::prepare_accelerated_output_buffers()` (`pdxch_processor_impl.h:119-140` → `pdxch_baseband_modulator_cuda.cpp:178-198`). Contiguous multi-port buffers get one copy (:779-791), else one copy per port (:803-818).
- Fallback: D2H into pinned `h_output_staging`, then host `std::memcpy` into the target buffers at flush time (`flush_pending_output`, :284-298).
- Completion: `done_event` recorded after the copy (:786,:812,:828); `pdxch_baseband_modulator::complete_accelerated_task()` (deferred onto the modulation executor, `pdxch_baseband_modulator.h:163-171,275-290`) calls `accelerator->wait()` → `ocudu_lowphy_tx_synchronize` → busy-poll `cudaEventQuery` + `sched_yield` (:243-253,:996-1002). TX metrics (avg/peak power, clipping counters) are then recomputed **on the CPU** from the returned ci16 samples (`pdxch_baseband_modulator_cuda.cpp:205-262`, throttled by `OCUDU_LOWPHY_TX_METRICS_PERIOD`).

**A100X flags**:
- The device-grid input path requires a managed-memory DL grid; the auto policy enables managed grids **only when `cudaDevAttrIntegrated != 0`** (`resource_grid_cuda_visible_impl.h:658-670`) — false on A100X. So on A100X the TX path defaults to the host-grid staging fallback (a full-grid H2D per slot), unless `OCUDU_DL_CUDA_VISIBLE_GRID=managed` forces managed memory over PCIe. This is a DGX-Spark(GB10, integrated/coherent) assumption. **Verified logic; the performance consequence on A100X is [hypothesis].**
- CP source index uses `cfg.dft_size - cp_len + sample` from the IFFT output, which is standard CP prepending — no host CP work remains.

---

## 3. Lower-PHY RX CUDA path (PUxCH OFDM demod + PRACH)

### PUxCH OFDM demodulation

- Entry: `puxch_processor_impl::process_symbol()` first tries single-port `demodulate_ci16` (1 port) or batched `demodulate_ci16_ports` (all ports, one symbol) before the CPU float path: `lib/phy/lower/processors/uplink/puxch/puxch_processor_impl.cpp` diff (batched attempt built at :~60-90 of new file; fallback per-port ci16 → CPU convert+`demodulate`).
- The uplink processor adds a **zero-copy "direct symbol" fast path**: when a radio receive buffer already contains a whole OFDM symbol and CFO is inactive, PRACH/PUxCH process straight from the radio buffer, skipping the temp-buffer copy and the ci16→cf32→ci16 CFO round trip (`lib/phy/lower/processors/uplink/uplink_processor_impl.cpp`, new fast path in `process_collecting` and `process_complete_symbol`; CFO conversion now gated on `cfo_processor.is_active()`).
- CUDA impl gating: requires a CUDA device, env not disabled (`OCUDU_LOWPHY_RX_ACCELERATION` / `OCUDU_LOWPHY_PUXCH_DEMODULATION_ACCELERATION`), and **`grid.supports_device_grid_mapping()`** (`ofdm_demodulator_cuda_impl.cpp:152,178`). With `force_gpu_path` (config mode `enabled`), unavailability is a fatal error (:153,:167).
- Offloaded work (`low_phy_puxch_rx.cu`): FFT-window load ci16→cf32 with 1/scale gain (`load_puxch_fft_input_ci16_kernel`, :103-122), batched forward VkFFT (batches = ports, :494-507), grid extraction with fftshift + phase compensation × dft_scale + optional CP-window phase rotation + **cf32→cbf16 conversion writing directly into the resource grid device pointer** (`extract_puxch_grid_kernel`, :139-167). Output location per RE: `(port*nof_symbols + symbol)*nof_subc + k` into `grid.get_device_grid_bf16()` (:162-166). CP removal/window offset is realized by copying only the `dft_size` window starting at `cp_len − window_offset` (host side, `copy_puxch_fft_window_to_staging`, :591-602). Optional CUDA graphs with output-pointer patching per grid (`OCUDU_LOWPHY_RX_CUDA_GRAPHS`, :646-868).

**Where the resource grid lands**: in **CUDA managed memory owned by `cuda_visible_resource_grid`** (`cudaMallocManaged` at `resource_grid_cuda_visible_impl.h:91`). The demodulator records a grid-ready event on the stream (`on_device_grid_mapping_enqueued` → `record_device_mapping`, :307-332); any host-side reader (`get`, `get_view`, `is_empty`) blocks in `prepare_host_access()` (event busy-poll + optional host prefetch, :423-441) — so the grid stays device-resident until the first host consumer touches it, and the GPU PUSCH chain can consume it directly via `get_device_grid_cbf16()`.

### PRACH demodulation routing

- Lower-PHY PRACH worker: window accumulation unchanged; the complete handoff is dispatched with `execute()` on a **dedicated PRACH executor** so GPU demod + upper-PHY wait cannot block the RX/TX slot path (`prach_processor_worker.cpp:120-123`; comment at :121-122). The workers try `demodulator->demodulate_ci16()` first (packed radio samples, no float convert) and fall back to convert+`demodulate` (:137-147). Dedicated worker `lphy_prach#N` at RT priority max−5 created in `worker_manager.cpp:542,600-623,646-676`, plumbed via `ru_sdr_executor_mapper.h:87-88,109-110` and `ru_sdr_factories.cpp:40`.
- CUDA PRACH demod (`ofdm_prach_demodulator_cuda_impl.cpp`): requires `buffer.supports_device_prach_buffer_mapping()` (:89,:108); computes per-TD-occasion geometry on host (`build_td_occasion_geometry`, :334-409), then per occasion: H2D copy of the occasion samples, `load_prach_fft_input_*_kernel` (CP skip), batched VkFFT forward, `extract_prach_grid_kernel` writes cbf16 REs **directly into the managed PRACH buffer** at `get_device_prach_buffer_cbf16() + get_device_prach_symbol_offset(...)` with symbol/FD strides (:203-210,:310-317; kernel `low_phy_prach_rx.cu:115-138`).
- PRACH buffer residency: `cuda_visible_prach_buffer` uses `cudaMallocManaged` (`prach_buffer_cuda_visible_impl.h:90`); host `get_symbol()` blocks on `prepare_host_access()` (:128-138,258-…), so the upper-PHY PRACH detector (CPU or GPU) synchronizes lazily.
- PRACH buffer "auto" gate differs from grids: it checks `cudaDevAttrManagedMemory` (`phy_acceleration_prach_buffer_factory.cpp:22-33`) which is **true on A100X**, so the managed PRACH buffer is created on discrete GPUs by default, while the UL resource grid is not (integrated-only gate at `resource_grid_cuda_visible_impl.h:658-670`).

**A100X flags**:
- PUxCH GPU demod is dead on A100X by default: pinned fallback grid (`resource_grid_pinned_impl.h`) does **not** implement `supports_device_grid_mapping()`, so `demodulate_ci16*` returns false and the CPU FFTW path runs. Forcing `--low_phy_rx_acceleration_mode enabled` on A100X without `OCUDU_UL_CUDA_VISIBLE_GRID=managed` is a **fatal error** (`ofdm_demodulator_cuda_impl.cpp:153`). Managed grid over PCIe on GA100 = page-migration traffic per slot **[performance hypothesis]**.
- PRACH GPU demod *does* engage on A100X by default (managed-memory gate), with H2D input copies and managed-buffer migration on the first host read.

---

## 4. OFH CUDA compression

**Which compression types are GPU-accelerated**: only `none` and `BFP` (`iq_compression_cuda.cpp:19-29,44-45`; backend enum `ofh_compression.h:14-17`; `valid_request` accepts widths 1–16, `ofh_compression.cu:53-60`). BFP compressed PRB size = 24·width bits + 8-bit exponent (`ofh_compression.cu:44-51`). Specialized kernels exist for widths 8, 9, 10, 12, 14, 16 with a runtime-generic kernel otherwise (`launch_compress_width`, `ofh_compression.cu:2000-2081`): **BFP9** → warp kernel `ofh_compress_9b_warp_kernel_t` (:1048), **BFP14** → `ofh_compress_bfp_warp_kernel_t<14>` (:2056), BFP16 → dedicated kernel (:902), BFP12 → warp kernel (:1210). Pinned staging for host transfers is enabled only for widths 9/12/14 (`use_pinned_host_{output,input}_transfer`, :139-149). `bfp_selective`/`mod_selective`/`mu_law` are never CUDA (factory falls through to CPU impls; `compression_factory.cpp` CUDA branches exist only under the `none` and `BFP` cases). Selection: `impl_type` `"auto"` resolves to `"cuda"` whenever a CUDA device is present, overridable via `OCUDU_OFH_TX_COMPRESSION_IMPL` / `OCUDU_OFH_RX_COMPRESSION_IMPL` / `OCUDU_OFH_COMPRESSION_IMPL` or YAML `compression_acceleration_mode`.

### DL (compression) batched data flow

Call chain: `ofh_downlink_handler_impl` now sends **all eAxCs of a slot in one call** — `data_flow_uplane->enqueue_section_type_1_messages(ctx, 0, all_dl_eaxc, grid)` (`ofh_downlink_handler_impl.cpp:118`). The data flow tries, in order (`ofh_data_flow_uplane_downlink_data_impl.cpp:100-119`):
1. **Slot burst** (`enqueue_section_type_1_messages_slot_burst`, :345-480): requires `reader.supports_device_grid_reading()` and full-bandwidth grid (:352-356). Reserves one Ethernet frame per (symbol × eAxC), writes all OFH/eCPRI headers on host, then a single `up_builder->build_symbol_batch_messages(...)` (:456-462) → `compress_device_symbol_batch_to_buffers` (`ofh_uplane_message_builder_impl.cpp:365-374`) → `ocudu_ofh_compress_device_grid_symbol_batch_to_host_buffers` (`iq_compression_cuda.cpp:423-438` → `ofh_compression.cu:3570-3642`): one batched kernel over ports×symbols reading the device grid, one D2H copy of the packed output into pinned `h_out`, `cudaStreamSynchronize`, then `std::memcpy` fan-out into each frame payload (`copy_device_to_host_buffers`, :186-211). Fallback within the builder: strided single-buffer batch + host `std::copy_n` per port (:378-…, :270-292 for the per-symbol variant).
2. **Symbol burst** (all ports, one symbol; :220-343) → `build_messages` → `compress_device_symbols_to_buffers` → `ocudu_ofh_compress_device_grid_ports_to_host_buffers` (:3363).
3. **Per-port/per-symbol** (:122-218): device-grid single-symbol compress `compress_device_symbol` (`iq_compression_cuda.cpp:130-182`, `ofh_compression.cu:3136-3179`), and finally the plain host path `compressor.compress(span…)` — which for the CUDA impl is still a **synchronous GPU roundtrip** H2D → kernel → D2H (`ofh_compression.cu:3055-3095`).

So **compressed data returns to host** in every DL variant (packet payloads must live in Ethernet frame buffers): the copy-back points are `copy_device_to_host` (`ofh_compression.cu:160-184`) and `copy_device_to_host_buffers` (:186-211), both ending in `cudaStreamSynchronize`. Handles are pooled per-thread-use via acquire/release (`iq_compression_cuda.cpp:64-97`), each handle owning its own non-blocking stream (:3003).

### UL (decompression) resident data flow

- The receiver first tries the keep-compressed decode: `uplane_decoder->decode_with_compressed_iq()` leaves the section payload as a `span<const uint8_t>` view into the packet (`ofh_uplane_message_decoder_impl.cpp`, `decode_iq_data` stores `results.compressed_iq_data` and skips host decompression), gated on `supports_resource_grid_decompression()` (CUDA impl: unconditionally true, `iq_compression_cuda.h:83-85`). Call sites: `ofh_data_flow_uplane_uplink_data_impl.cpp:31-45`, PRACH `ofh_data_flow_uplane_uplink_prach_impl.cpp:93-105`.
- The resident writer then decompresses **directly into the managed UL grid**: `write_compressed_to_resource_grid` (`ofh_uplane_rx_symbol_data_flow_writer.cpp:83-155`) → `uplink_context_repository::write_grid_device` (checks `writer.supports_device_grid_mapping()`, `uplink_context_repository.h:96`) → `decoder.decompress_to_resource_grid` → `iq_compression_cuda::decompress_to_resource_grid` (`iq_compression_cuda.cpp:487-551`): H2D copy of the compressed bytes (pinned slot for BFP9/12/14), decompression kernel writing cbf16 REs into `grid.get_device_grid_bf16()` at the section's PRB offset (`ocudu_ofh_decompress_to_device_grid_async`, `ofh_compression.cu:3209-3254`), then `grid.on_device_grid_mapping_enqueued(stream)` records the grid-ready event — **no D2H**; the grid is consumed on device by the GPU PUSCH chain or synchronized lazily on first host read.
- PRACH: `write_compressed_to_prach_buffer` (`ofh_uplane_prach_symbol_data_flow_writer.cpp:107-207`) → `prach_context_repository::write_iq_device` (`prach_context_repository.h:167-196`) → `decompress_to_prach_buffer` (`iq_compression_cuda.cpp:553-624`) → `ocudu_ofh_decompress_to_device_prach_buffer_async` (`ofh_compression.cu:3256-3306`): decompress into scratch `d_out`, then `launch_copy_decompressed_re` places the RE range into the managed PRACH buffer at `symbol_offset + start_re`. Again no D2H.
- Fallback when the grid/buffer is not device-mappable: the flow re-decodes with host decompression (`decode()`), which for the CUDA decompressor is the synchronous roundtrip `ocudu_ofh_decompress` (`ofh_compression.cu:3097-3133`), then the classic host `write_grid`/`write_iq` copy.

**A100X flags**:
- The resident UL decompress path and the DL device-grid compress paths both require managed (CUDA-visible) grids → auto-disabled on discrete A100X (integrated-only gate). On A100X with defaults, OFH DL falls to `get_host_iq_data()` + `iq_compression_cuda::compress()` and UL falls to `decode()` + `iq_compression_cuda::decompress()` — i.e. **per-packet/per-symbol synchronous GPU roundtrips over PCIe** because `auto` still resolves the compression impl to CUDA whenever a GPU is visible (`compression_factory.cpp`, `resolve_compression_impl_type`). Whether that beats AVX512 on x86 is **[hypothesis — likely not for small sections; measure]**. The PRACH decompress resident path *does* work on A100X (managed-memory-gated PRACH buffer).
- `ocudu_ofh_compression_available()` is merely `cudaGetDevice()==success` (`ofh_compression.cu:2987-2991`) — no integrated/coherence check.

---

## 5. Host↔device copies and synchronization points (explicit list)

### Lower-PHY TX (`low_phy_tx.cu`, `pdxch_baseband_modulator_cuda.cpp`)
| # | Direction/kind | Where |
|---|---|---|
| T1 | Host `memcpy` grid → pinned WC staging slot (host-grid fallback) | `low_phy_tx.cu:983` |
| T2 | `cudaMemcpyAsync` H2D grid staging → `d_grid_stage` | `low_phy_tx.cu:973-975` (registered direct) / `:984-986` (staged) |
| T3 | `cudaEventRecord` staging-slot ready event; producer waits via `cudaEventQuery`+`sched_yield` before slot reuse | `low_phy_tx.cu:988-991`, wait at `:583-595`/`:243-253` |
| T4 | `cudaMemcpyAsync` D2H `d_output` → registered radio buffer(s) (1 or per-port) | `low_phy_tx.cu:779-791, 803-818` |
| T5 | `cudaMemcpyAsync` D2H `d_output` → pinned `h_output_staging` (unregistered fallback) + deferred host `std::memcpy` at flush | `low_phy_tx.cu:821-830`, memcpy `:284-298` |
| T6 | `done_event` record + busy-poll wait in `ocudu_lowphy_tx_synchronize` (called from `complete_accelerated_task`) | record `:786,:812,:828`; wait `:262-268,:996-1002`; caller `pdxch_baseband_modulator.h:277` |
| T7 | `cudaHostRegister` of all baseband output buffers at startup | `low_phy_tx.cu:371`, driver `pdxch_processor_impl.h:119-140` |
| T8 | Device-grid path: `cudaStreamWaitEvent` on grid pending events + optional `cudaMemPrefetchAsync` to device | `resource_grid_cuda_visible_impl.h:358-369, 406-417, 551-573` |
| T9 | Warm-up `cudaMemsetAsync` + `cudaStreamSynchronize` at create/config-change | `low_phy_tx.cu:746-752` |

### Lower-PHY RX PUxCH (`low_phy_puxch_rx.cu`, grid impl)
| # | Direction/kind | Where |
|---|---|---|
| R1 | Host `memcpy` FFT window (per port, CP-skipped) → pinned staging slot | `low_phy_puxch_rx.cu:591-602` |
| R2 | `cudaMemcpyAsync` H2D staging → `d_input_ci16` + slot-ready `cudaEventRecord` (busy-poll on reuse `:517-545`) | `low_phy_puxch_rx.cu:558-566, 547-556` |
| R3 | Optional direct H2D from `cudaHostRegister`ed radio buffers (env `OCUDU_LOWPHY_RX_RUNTIME_HOST_REGISTRATION`) | `low_phy_puxch_rx.cu:568-589, 333-359` |
| R4 | Output: none (kernel writes cbf16 into managed grid) — `extract_puxch_grid_kernel` | `low_phy_puxch_rx.cu:139-167` |
| R5 | Grid ready `cudaEventRecord` on stream (`on_device_grid_mapping_enqueued`) | `ofdm_demodulator_cuda_impl.cpp:310`, `resource_grid_cuda_visible_impl.h:307-332` |
| R6 | Host consumption sync: `cudaEventQuery` busy-poll + optional host `cudaMemPrefetchAsync` (+`cudaStreamSynchronize`) in `prepare_host_access()` | `resource_grid_cuda_visible_impl.h:423-441, 496-503, 551-573` |
| R7 | Error paths: `cudaStreamSynchronize`/`ocudu_lowphy_puxch_rx_synchronize` before cancel | `ofdm_demodulator_cuda_impl.cpp:297-300, 310-314` |
| R8 | Grid zeroing on device: `cudaMemsetAsync` + event on maintenance stream (`set_all_zero`) | `resource_grid_cuda_visible_impl.h:465-494` |
| R9 | Warm-up/prime: `cudaMemsetAsync` + `cudaStreamSynchronize` per (symbol-geometry) at factory init | `low_phy_puxch_rx.cu:870-925`, driver `ofdm_demodulator_cuda_impl.cpp:341-383` |

### Lower-PHY PRACH (`low_phy_prach_rx.cu`, PRACH buffer impl)
| # | Direction/kind | Where |
|---|---|---|
| P1 | Host `memcpy` occasion payload (CP-skipped) → pinned staging; `cudaMemcpyAsync` H2D + ready event (cf32 `:679-694`; ci16 `:729-744`); optional registered direct copy `:672-674, :722-724` | `low_phy_prach_rx.cu` |
| P2 | Output: none — `extract_prach_grid_kernel` writes cbf16 into managed PRACH buffer | `low_phy_prach_rx.cu:115-138`, offsets from `ofdm_prach_demodulator_cuda_impl.cpp:204-205, 311-312` |
| P3 | Buffer-mapping event record (`on_device_prach_buffer_mapping_enqueued`) | `ofdm_prach_demodulator_cuda_impl.cpp:213, 320`; `prach_buffer_cuda_visible_impl.h:181` |
| P4 | Host consumption sync in `get_symbol()` → `prepare_host_access()` (event busy-poll `:317`, prefetch `:359-362`) | `prach_buffer_cuda_visible_impl.h:128-138, 258+` |
| P5 | Error path `cudaStreamSynchronize` / `ocudu_lowphy_prach_rx_synchronize` (busy-poll `:752-762`) | `ofdm_prach_demodulator_cuda_impl.cpp:207, 214, 314, 321` |

### OFH compression (`ofh_compression.cu`, `iq_compression_cuda.cpp`)
| # | Direction/kind | Where |
|---|---|---|
| O1 | Host-span compress/decompress: H2D input (`copy_host_to_device`, pinned-slot staging `memcpy`+`cudaMemcpyAsync`+event for BFP9/12/14 at `:223-246`, plain async otherwise `:219-220`) | `ofh_compression.cu:3078-3081, 3119-3122, 3234-3237, 3287-3290` |
| O2 | Compress output D2H: `cudaMemcpyAsync` + **`cudaStreamSynchronize`** (+ pinned `h_out` staging & host `memcpy` for BFP9/12/14) | `copy_device_to_host` `:160-184`; call sites `:3093, 3133, 3177` |
| O3 | Batched compress output D2H: single `cudaMemcpyAsync` + **`cudaStreamSynchronize`** + per-frame host `memcpy` fan-out | `copy_device_to_host_buffers` `:186-211`; call sites `:3641` (symbol batch), `:3363+` (port batch) |
| O4 | Resident decompress: H2D compressed bytes only (O1 style); kernel writes into managed grid/PRACH buffer; **no D2H**; grid-ready event via `on_device_grid_mapping_enqueued` / `on_device_prach_buffer_mapping_enqueued` | `iq_compression_cuda.cpp:519-537, 590-606`; `ofh_compression.cu:3209-3254, 3256-3306` |
| O5 | Failure sync: `ocudu_ofh_compression_synchronize` (=`cudaStreamSynchronize`) before cancel | `iq_compression_cuda.cpp:535, 604`; `ofh_compression.cu:3047-3053` |
| O6 | Grid prepare before device read: `prepare_device_grid_reading(stream)` → stream-wait on pending grid events (+prefetch) | `iq_compression_cuda.cpp:158, 216, 280, 347, 422`; `resource_grid_cuda_visible_impl.h:406-417` |

---

### Cross-cutting DGX-Spark→A100X caveats (summary)

1. **Managed-grid auto gate is integrated-GPU-only** (`resource_grid_cuda_visible_impl.h:658-670`, duplicated in `phy_acceleration_resource_grid_factory.cpp:25-31`): on A100X, DL/UL grids default to pinned host memory, which silently disables (a) lower-PHY RX GPU demod, (b) lower-PHY TX device-grid input (staging fallback still works), (c) OFH device-grid compress and resident decompress. Overrides: `OCUDU_DL_CUDA_VISIBLE_GRID` / `OCUDU_UL_CUDA_VISIBLE_GRID` = `managed` (`resource_grid_cuda_visible_impl.h:619-655`) — but managed memory on GA100 over PCIe migrates pages on each CPU↔GPU ownership flip; the code's mitigation (`cudaMemAdvise` + optional `cudaMemPrefetchAsync`, `:521-531,551-573`) is best-effort and prefetch is off by default (`OCUDU_CUDA_VISIBLE_GRID_PREFETCH`, `:586`). Performance on A100X is unvalidated **[hypothesis]**.
2. **PRACH managed buffer auto-enables on A100X** (`cudaDevAttrManagedMemory` gate, `phy_acceleration_prach_buffer_factory.cpp:22-33`) — inconsistent with the grid gate; PRACH GPU demod and OFH PRACH resident decompress will engage on discrete GPUs by default.
3. **OFH compression `auto` → CUDA whenever a GPU exists** (`compression_factory.cpp`), regardless of grid residency — on A100X this yields synchronous per-message PCIe roundtrips; set `OCUDU_OFH_COMPRESSION_IMPL=cpu` (or YAML `compression_acceleration_mode: disabled`) to restore AVX paths.
4. All completion waits are busy-poll (`cudaEventQuery` + `sched_yield`/`yield`) rather than blocking sync — consistent RT design, but it pins the waiting core (TX: `low_phy_tx.cu:243-253`; RX: `low_phy_puxch_rx.cu:517-524`; grids: `resource_grid_cuda_visible_impl.h:496-503`; OFH: `ofh_compression.cu:151-158`).