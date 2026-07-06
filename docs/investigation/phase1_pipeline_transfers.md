# OCUDU CUDA Fork (cuda_accel.26_04): Per-Slot L1 Pipeline & Host↔Device Transfer/Sync Inventory

Analysis of `/home/user/cuda_accelerated_ocudu` (base 092414aac2 + CUDA commit 9fd4047b43 + docs ccdf4e681f). All file:line references are relative to that repo root. Target platform for classification: **A100X (discrete, sm_80, PCIe, non-coherent)**. Facts are verified-by-reading-code unless marked **[HYPOTHESIS]**.

---

## 0. Platform policy switches that shape both pipelines (read first)

The fork has **two grid/buffer backing strategies**, selected at startup, and the choice changes which transfers exist at all:

| Object | Auto policy | Discrete (A100X) default | Coherent GB10 default | Evidence |
|---|---|---|---|---|
| UL/DL resource grid | managed iff `cudaDevAttrIntegrated != 0` | **pinned host heap** (`cudaHostRegister`) via `resource_grid_pinned_factory` | `cudaMallocManaged` grid, aliased host/device | `lib/phy/upper/resource_grid_cuda_visible_impl.h:658-670` (integrated check), `:674` (pinned fallback), `lib/phy/upper/resource_grid_pinned_impl.h:58` |
| PRACH buffer | managed iff `cudaDevAttrManagedMemory != 0` (**true on A100X too**) | `cudaMallocManaged` + prefetch (default ON) | same, prefetch effectively no-op | `lib/phy/upper/phy_acceleration_prach_buffer_factory.cpp:22-34`, `lib/phy/upper/prach_buffer_cuda_visible_impl.h:90,385` |
| PDSCH acceleration in `auto` mode | disabled on discrete with a warning unless `OCUDU_PDSCH_AUTO_ENABLE_DISCRETE=1` | **OFF by default** | ON | `lib/phy/upper/upper_phy_factories.cpp:51-73` |
| PDSCH GPU grid mapper | disabled iff `integrated == 0` (unless env override) | **OFF** → encoded symbols come back D2H, CPU maps grid | ON (device-side RE mapping) | `lib/phy/upper/channel_processors/pdsch/pdsch_block_processor_gpu_impl.cpp:41-77` |
| OFH IQ compression impl `auto` | resolves to `"cuda"` whenever a CUDA backend exists — **no integrated check** | CUDA compressor with per-call H2D/D2H roundtrip + sync | CUDA compressor reading device grid directly | `lib/ofh/compression/compression_factory.cpp:54-57`, `lib/ofh/compression/iq_compression_cuda.cpp:139,195` |
| Overrides | `OCUDU_CUDA_VISIBLE_GRID` / `OCUDU_{UL,DL}_CUDA_VISIBLE_GRID` = `managed`/`pinned`/`auto`; grid prefetch default **false**, PRACH prefetch default **true** | | | `lib/phy/upper/phy_acceleration_runtime_options.h:73-96`, `resource_grid_cuda_visible_impl.h:586`, `prach_buffer_cuda_visible_impl.h:385` |

Consequence: on A100X in default config, the **UL grid, DL grid, and all lower-PHY/OFH grid interfaces are host memory**; every GPU stage that consumes/produces a grid does explicit PCIe copies. On GB10 the same code paths short-circuit through `supports_device_grid_reading()/mapping()` and the copies disappear.

---

## 1. UL per-slot pipeline (accelerated mode, discrete A100X, OFH split 7.2 default)

```
                 OFH RX threads (ru_ofh workers)                         upper-PHY worker threads
 ┌──────────────────────────────────────────────┐   ┌──────────────────────────────────────────────────────────────┐
 │ eCPRI U-plane decode (CPU)                   │   │ PUSCH task (one task per PDU, pusch executor)                │
 │ BFP decompress:                              │   │  [resident path, no/simple UCI]                              │
 │  - CPU SIMD, or                              │   │  1. RE-index build (CPU, cached)          [CPU]              │
 │  - CUDA roundtrip if impl=cuda(auto!):       │   │  2. ★H2D full grid cbf16 (~0.73 MB/4port) upload_stream      │
 │    H2D 13KB/sym-port + kernel                │   │     pusch_demodulator_gpu_impl.cpp:2331                      │
 │    + D2H + cudaStreamSynchronize PER CALL    │   │  3. ★H2D RE indices 157KB (geometry-cached):2364              │
 │    ofh_compression.cu:239,170-183            │   │  4. event: h2d_complete → stream_ waits :2387-2392            │
 │ write into PINNED HOST UL grid (CPU writer;  │   │  5. E2E kernel chain [GPU stream_]:                           │
 │  device write path OFF on pinned grid:       │   │     DMRS gen→chest→eq(MMSE/ZF)→demap→descramble→FP16 LLR      │
 │  uplink_context_repository.h:96)             │   │     pusch_e2e.cu (:2407-2420 launch)                          │
 └──────────────────────────────────────────────┘   │  6. compact SCH LLRs + UCI demux/decode kernels [GPU]         │
                                                    │     :2553-2653 (+small ★D2H UCI LLR/results, 100s B)          │
 PRACH: OFH → CPU decompress → MANAGED prach buffer │  7. ★D2H SINR/EVM 16B+20B async pusch_e2e.cu:11639-11645       │
 (prach_context_repository / prach_buffer_cuda_     │  8. NO HOST SYNC in resident mode (:2694-2705)                │
  visible_impl.h) — GPU-visible on A100X            │  9. resident handoff (same thread) pusch_processor_impl:680   │
                                                    │ 10. LDPC decoder (same stream): dematch→decode→deseg→TB CRC   │
                                                    │     pusch_codeblock_decoder_cuda_batch.cpp:1182-1404 [GPU]    │
                                                    │ 11. ★D2H TB payload ~0.1-0.15MB :1417/1419                    │
                                                    │     ★D2H CB CRCs (~4B×nof_cbs) :1423  ★TB CRC 4B :1441        │
                                                    │ 12. ●POLL-SYNC completion event :1449-1450 (sched_yield loop) │
                                                    │     ← THE single per-PDU host block                           │
                                                    │ 13. SINR readback from pinned mem (pre-join cb)               │
                                                    │     pusch_processor_impl.cpp:726-745 → report_deferred_sinr   │
                                                    │ 14. notify FAPI (CPU)                                         │
                                                    └──────────────────────────────────────────────────────────────┘
 ★ = PCIe transfer   ● = host blocks (poll)   triple-buffered (bufs 0/1/2) :1763-1769; dedicated h2d/d2h streams :885,899
```

**Non-resident fallback** (E2E handle missing, tiny grants <256 symbols `:572`, unsupported layer/port combos `:1240-1242`): adds ★D2H 2.5 MB FP16 LLRs `:2772` + poll-sync `:2794`, CPU FP16→INT8 LUT conversion `:2812-2814`, then the decoder re-uploads ★H2D ~5 MB FP32 LLRs `pusch_codeblock_decoder_cuda_batch.cpp:877` and downloads decoded bits `:933` + sync `:945`. The LLRs cross PCIe **three times** in that mode.

**PRACH sub-pipeline (upper):** managed buffer → detector prefetches to device (`prach_buffer_cuda_visible_impl.h:359-373`) → zero-copy device read (`prach_detector_cuda_impl.cpp:265-275`) or, if device read unavailable, CPU pack + ★H2D ~27 KB (`prach_detector.cu:502`) → IDFT/correlate kernels → ★D2H RSSI 4 B + 64 candidates ~1.5 KB + **blocking `cudaStreamSynchronize`** (`prach_detector.cu:532-538`). CPU/GPU choice thresholds: `prach_detector_cuda_impl.cpp:211-235`.

**SRS sub-pipeline:** pinned grid not device-readable on A100X → sidecar stager ★H2D **full grid ~0.73 MB per SRS occasion** (`srs_estimator_cuda_impl.cpp:362-369` → `pusch_device_grid_reader_cuda.cpp:101-115`) → kernels (+CUDA-graph capture) → ★D2H result struct + yielding sync (`srs_estimator.cu:1084,1113,1123`).

**Split-8 (SDR) lower-PHY UL variant:** the CUDA OFDM demodulator requires a *device-mappable* grid (`ofdm_demodulator_cuda_impl.cpp:271,294-296`); with the pinned grid on A100X this **fails and falls back to the CPU demodulator per call** (`modulation_factories.cpp:229-252`, fallback at `ofdm_demodulator_cuda_impl.cpp:146-190`). Lower-PHY PRACH GPU demod works (managed buffer): ★H2D ci16 window per occasion (`low_phy_prach_rx.cu:674-739`).

---

## 2. DL per-slot pipeline (accelerated mode, discrete A100X)

Note: PDSCH acceleration is **off by default on discrete** (`upper_phy_factories.cpp:65-70`); the pipeline below is with `pdsch_acceleration_mode=enabled` or `OCUDU_PDSCH_AUTO_ENABLE_DISCRETE=1`.

```
        upper-PHY DL worker threads (l1_dl executors)                          OFH TX threads
 ┌───────────────────────────────────────────────────────────────┐  ┌────────────────────────────────────┐
 │ downlink_processor_multi_executor_impl: configure grid        │  │ per-symbol U-plane build (CPU)      │
 │  → pdsch_grid_output strategy (host writer on discrete        │  │ compression:                        │
 │    default; sidecar writer if accel on)                       │  │  - CPU SIMD, or                     │
 │    pdsch_grid_output_strategy.cpp:50-94                       │  │  - CUDA (auto default!):            │
 │ PDSCH task → pdsch_processor_flexible fork_cb_batches         │  │    ★H2D 13KB/sym-port               │
 │  (cb-batch tasks defer to executor, flexible_impl.cpp:336-411)│  │    ofh_compression.cu:239           │
 │  per lane (pdsch_acceleration_nof_lanes):                     │  │    kernel + ★D2H ~7.6KB             │
 │  1. memcpy TB → pinned; ★H2D TB bytes (≤~150KB)               │  │    + cudaStreamSynchronize          │
 │     pdsch_tb_encoder_cuda.cu:361-362                          │  │    PER CALL :170-183                │
 │  2. GPU fused: CRC+segment+LDPC+RM+interleave+scramble        │  │  → eth TX                            │
 │     +modulate→INT8 symbols :371                               │  └────────────────────────────────────┘
 │  3. ★D2H all symbols int8 ~314KB :383-388                     │
 │     (async, overlaps CPU; completion event :395)              │   Split-8 variant instead of OFH:
 │  4. ●poll-sync event in get_symbols()                         │   pdxch CUDA modulator:
 │     :413 (cudaEventSynchronizeYielding)                       │   ★H2D whole host grid (registered or
 │  5. CPU maps symbols → PINNED HOST grid                       │   staged) low_phy_tx.cu:973-987
 │     (GPU RE mapper disabled on discrete,                      │   → IFFT/CP kernels →
 │      pdsch_block_processor_gpu_impl.cpp:71-75)                │   ★D2H baseband int16 ~0.25MB/port/slot
 │  PDCCH/SSB/CSI-RS: CPU into host grid (unchanged mainline)    │   :779-828 (direct into registered radio
 │  send_resource_grid → before_send_grid (no-op, host path)     │   buffers) ● wait() poll :249,176
 └───────────────────────────────────────────────────────────────┘
```

On GB10/managed-grid: steps 3-5 are replaced by a device-side RE-mapping kernel writing the managed grid (`pdsch_block_processor_gpu_impl.cpp:294-302`, `resource_grid_mapper_impl.cpp:441-460`), and OFH TX reads the device grid directly (`ofh_data_flow_uplane_downlink_data_impl.cpp:203,227,352`).

---

## 3. Transfer/sync inventory (production per-slot paths + init)

Assumptions for size column: 100 MHz / 273 PRB (3276 subc) / 14 symbols / 30 kHz / 4 RX ports / 4 layers / 256QAM; cbf16 RE = 4 B; data REs ≈ 39,312 (2 type-1 DMRS symbols, 2 CDM w/o data); LLRs = 39,312×4×8 ≈ 1.26 M; TBS ≈ 1.2 Mbit ≈ 150 KB. "CP" = on per-slot critical path.

### 3.1 UL PUSCH — demodulator (`pusch_demodulator_gpu_impl.cpp`)

| file:line | API | Dir | Data | Stage | CP | Est. bytes/slot |
|---|---|---|---|---|---|---|
| :2331 | `cudaMemcpyAsync` | H2D | full RX grid cbf16, coalesced ports | E2E input | yes | ~734 KB (0 on managed grid) |
| :2336 | `cudaMemcpyAsync` | H2D | per-port grid (non-sequential ports) | E2E input | alt | ~183 KB×ports |
| :2355 | `cudaMemcpyAsync` | H2D | staged grid (non-contiguous fallback) | E2E input | alt | ~734 KB + host memcpy |
| :2364 | `cudaMemcpyAsync` | H2D | data RE indices | E2E input | geometry-cached (`:2363`, cache `:2365`) | 157 KB on change, else 0 |
| :2497/:2508/:2519 | `cudaMemcpyAsync` | H2D | SCH/HARQ/CSI compaction indices | resident UCI demux | cached per buffer (`:2494,2505,2516`) | ≤157 KB on change |
| :2621/:2648 | `cudaMemcpyAsync` | D2H | HARQ/CSI UCI LLR FP16 | UCI fallback feed | yes (UCI PDUs) | 100s B |
| :2656/:2664 | `cudaMemcpyAsync` | D2H | UCI decode results (pinned) | device UCI decode | yes (UCI PDUs) | ~100 B |
| :2772 | `cudaMemcpyAsync` | D2H | full FP16 LLRs | **non-resident only** | yes (fallback) | ~2.5 MB |
| :1592/:1595 | `cudaMemcpyAsync` | H2D | eq'd symbols cf32 + noise vars | non-E2E batch fallback | fallback only | ~630 KB + 157 KB |
| :1681 | `cudaMemcpyAsync` | D2H | LLRs (batch fallback) | fallback | fallback | ~2.5 MB |
| :1820/:1824/:1839/:2392/:2763 | `cudaStreamWaitEvent` | — | buffer/kernel ordering | triple-buffer | yes (device-side, non-blocking) | — |
| :2711/:2713 | event/stream poll-sync | — | non-resident: wait kernel before SINR notify | demod end | fallback | — |
| :2794/:2797 | event/stream poll-sync | — | non-resident: wait LLR D2H | demod end | fallback | — |
| :2694-2705 | *(none)* | — | **resident: returns with NO host sync** | demod end | yes | — |
| :782,:849-880,:2019 | `cudaHostAlloc`/`cudaMalloc` | — | pinned LLR/staging/result buffers, device buffers | init | **no** (ctor) | 3× ~9 MB device + ~6 MB pinned |
| :2237 (`pusch_e2e_configure`) | implies device sync | — | reconfig (cached, `:2160-2183`) | rare | only on RNTI/shape change | — |

### 3.2 UL PUSCH — E2E kernels (`lib/phy/cuda/src/pusch_e2e.cu`)

| file:line | API | Dir | Data | Stage | CP | Bytes |
|---|---|---|---|---|---|---|
| 11639 | `cudaMemcpyAsync` | D2H | SINR/EVM accum (pinned) | metrics | yes | 16 B |
| 11644 | `cudaMemcpyAsync` | D2H | SINR/EPRE/RSRP/TA/CFO (pinned) | metrics | yes | 20 B |
| 13392 | **`cudaMemcpy` (blocking)** | H2D | DMRS symbol indices, MIMO path per call | chest | yes (MIMO) | ~8-16 B but **synchronous** |
| 11072/11530/12938/13213/13483 | `cudaMemsetAsync` | — | accumulator clears | per call | yes | tiny |
| 10837/10937/11221/12919/12954 | `cudaMemcpy(Async)` | H2D | symbol times, DMRS indices/c_inits | configure-time | no | tiny |
| 10369/10390 | `cudaMallocHost` | — | pinned SINR blocks | init | no | 36 B |
| 11729 | `cudaStreamSynchronize` | — | diagnostics print only | debug | no | — |

### 3.3 UL PUSCH — resident LDPC decoder (`pusch_codeblock_decoder_cuda_batch.cpp`)

| file:line | API | Dir | Data | Stage | CP | Bytes |
|---|---|---|---|---|---|---|
| 1192/1196 | `cudaMemcpyAsync` | H2D | direct-RM per-CB lengths/offsets | rate dematch meta | cached (`:1183-1188`) | ~1.2 KB on change |
| 1417 or 1419 | `cudaMemcpyAsync` | D2H | TB payload (direct or pinned staging) | TB output | yes | ~150 KB max |
| 1423/1435 | `cudaMemcpyAsync` | D2H | per-CB CRC results (pinned) | CRC | yes | 4 B×nof_cbs (~600 B) |
| 1441 | `cudaMemcpyAsync` | D2H | TB CRC result (pinned) | CRC | yes | 4 B |
| 1449-1450 / 1457 | event record + **poll-sync** | — | **the per-PDU UL host block** | join | yes | — |
| 1486 | host `memcpy` | — | pinned→caller TB span | output | yes | ~150 KB |
| 877 | `cudaMemcpyAsync` | H2D | FP32 LLR batch | **non-resident** decode | fallback | ~5 MB |
| 933/995/986 | `cudaMemcpyAsync` | D2H | decoded bit batch | non-resident | fallback | ~160 KB |
| 945/1002 | poll-sync | — | non-resident join | fallback | — | — |
| 233-259 | `cudaMallocHost` | — | pinned offsets/CRC/TB buffers | init | no | ~64 KB + MAX_TB |
| `ldpc_decoder_flexible.cu:6191/6196` | `cudaMemcpyAsync` | D2H | iteration counts | stats | yes (small) | 4 B×nof_cbs |
| `ldpc_decoder_flexible.cu:6353/6438` | `cudaEventSynchronize`/sync | — | stats/CRC readback sync | stats | yes (after main sync → cheap) | — |
| `ldpc_decoder_flexible.cu:5408`, `ldpc_encoder_flexible.cu:2277-2287` | `cudaMemcpy` | H2D | LDPC BG tables | init/config | no | KBs |

### 3.4 UL PRACH

| file:line | API | Dir | Data | Stage | CP | Bytes |
|---|---|---|---|---|---|---|
| `prach_detector.cu:502` | `cudaMemcpyAsync` | H2D | PRACH IQ (host-input path) | detector in | per occasion | ~27 KB (B4, 4 port) / ~13 KB (fmt0, 4 port) |
| `prach_detector.cu:513` | `cudaMemcpyAsync` | H2D | ZC root sequences | detector | root-cached (`:510-518`) | ~0 steady state |
| `prach_detector.cu:532-538` | 2×`cudaMemcpyAsync` D2H + **`cudaStreamSynchronize`** | D2H | RSSI 4 B + 64 candidates ~1.5 KB | detector out | per occasion, **blocking sync** | ~1.6 KB |
| `prach_buffer_cuda_visible_impl.h:359-362` | `cudaMemPrefetchAsync` | H2D (page migration) | managed PRACH buffer | before device read | per occasion (prefetch default ON `:385`) | ~seq×sym×ports×4 B (≈80 KB B4/4port) |
| `prach_buffer_cuda_visible_impl.h:370-372` | prefetch to host + `cudaStreamSynchronize` | D2H | host access after GPU write | fallback host read | when CPU detector used after GPU write | — |
| `prach_detector.cu:299-300` | `cudaMallocHost` | — | pinned RSSI/candidates | init | no | ~1.6 KB |
| `low_phy_prach_rx.cu:674/688/724/738` | `cudaMemcpyAsync` | H2D | PRACH baseband window (split 8) | lower demod | per occasion | ~0.1-1 MB (srate-dependent) |
| `low_phy_prach_rx.cu:339/382` | `cudaHostAlloc` | — | staging slots | init | no | — |

### 3.5 UL SRS

| file:line | API | Dir | Data | Stage | CP | Bytes |
|---|---|---|---|---|---|---|
| `pusch_device_grid_reader_cuda.cpp:101/110` | `cudaMemcpyAsync` | H2D | **full host grid staged** for SRS | SRS input | per SRS occasion | ~734 KB (4 port) |
| `srs_estimator.cu:1023` | `cudaMemcpyAsync` | H2D | SRS sequences | configure | config-cached | KBs on change |
| `srs_estimator.cu:1084/1113` | `cudaMemcpyAsync` | D2H | result struct (pinned) | output | yes | ~100s B |
| `srs_estimator.cu:1123` | poll-sync | — | wait result | output | per occasion | — |
| `srs_estimator.cu:311` | `cudaHostAlloc` | — | pinned result | init | no | — |

### 3.6 DL PDSCH

| file:line | API | Dir | Data | Stage | CP | Bytes |
|---|---|---|---|---|---|---|
| `pdsch_tb_encoder_cuda.cu:361-362` | host memcpy + `cudaMemcpyAsync` | H2D | TB bytes (pinned staging) | encoder in | yes | ≤~150 KB/TB |
| `pdsch_tb_encoder_cuda.cu:383` | `cudaMemcpyAsync` | D2H | all INT8 symbols (pinned) | encoder out | yes (discrete: mapper off) | ~314 KB/TB |
| `pdsch_tb_encoder_cuda.cu:413` | `cudaEventSynchronizeYielding` | — | wait encode+D2H in `get_symbols()` | mapping | yes | — |
| `pdsch_tb_encoder_cuda.cu:425-431` | `cudaMemcpyAsync` D2H + poll-sync | D2H | partial symbols (deferred path) | mapping | alt | slice |
| `pdsch_tb_encoder_cuda.cu:145/151` | `cudaMallocHost` | — | pinned symbol/TB buffers | init | no | ~2 MB |
| `pdsch_block_processor_gpu_impl.cpp:256` | `cudaMemcpyAsync` | H2D | RE offsets (device mapper path) | GPU map | GB10 path, content-cached `:253-262` | ~157 KB on change |
| `pdsch_block_processor_gpu_impl.cpp:264` | `cudaMemsetAsync` | — | zero device grid | GPU map | GB10 path | — |
| `pdsch_device_grid_writer_cuda.cpp:92/136` | `cudaMemsetAsync` | — | sidecar grid clear | GPU map | sidecar path | — |
| `pdsch_device_grid_writer_cuda.cpp:183/187` | event poll-syncs | — | wait mapper done | before send | sidecar path | — |
| `pdsch_device_grid_writer_cuda.cpp:211` | **`cudaMemcpy` (blocking)** | D2H | **entire device grid** on materialize | before send | sidecar path only | ~734 KB×(DL ports)/slot |
| `crc_calculator_cuda.cpp:109/139/145` | H2D data + D2H 4 B + poll-sync | both | standalone GPU CRC (factory-selectable) | fallback component | not on default path | TB-sized |
| `modulation_mapper_cuda.cpp:118-130/158-169`, `demodulation_mapper_cuda.cpp:179-204` | H2D+D2H+sync | both | standalone mapper fallbacks | not on E2E path | fallback | — |

### 3.7 OFH IQ compression (both directions; **`auto` ⇒ CUDA when GPU present**)

| file:line | API | Dir | Data | Stage | CP | Bytes |
|---|---|---|---|---|---|---|
| `ofh_compression.cu:239` (+staging memcpy `:238`) | `cudaMemcpyAsync` | H2D | uncompressed IQ per compress call | OFH TX compress | yes (per symbol×eAxC) | 13.1 KB/sym-port → ~734 KB/slot (4 eAxC) |
| `ofh_compression.cu:170-183/200-210` | `cudaMemcpyAsync` D2H + **`cudaStreamSynchronize`** | D2H | compressed PRBs | OFH TX compress | yes, **blocking sync per call** (≈56/slot) | ~7.6 KB/sym-port → ~428 KB/slot |
| `ofh_compression.cu:220` | `cudaMemcpyAsync` | H2D | compressed input (decompress) | OFH RX | yes | ~428 KB/slot |
| `ofh_compression.cu:3017/3052` | `cudaStreamSynchronize` | — | decompress completion | OFH RX | yes, per call | — |
| `ofh_compression.cu:3790/3838/3900/3955` | `cudaStreamSynchronize` | — | device-grid compress variants | GB10 path | per call | — |
| `ofh_compression.cu:132` | `cudaMallocHost` | — | pinned staging | init | no | — |

### 3.8 Lower PHY split-8 (SDR only)

| file:line | API | Dir | Data | Stage | CP | Bytes |
|---|---|---|---|---|---|---|
| `low_phy_tx.cu:973/984` | `cudaMemcpyAsync` | H2D | DL host grid (registered or staged) | DL mod in | yes | ~183 KB/port/slot |
| `low_phy_tx.cu:779/804/821` | `cudaMemcpyAsync` | D2H | baseband int16 (direct to registered radio buffer or staging) | DL mod out | yes | ~246 KB/port/slot @122.88 Msps |
| `low_phy_tx.cu:249` + `pdxch_baseband_modulator_cuda.cpp:176` | event poll / `wait()` | — | TX completion | radio deadline | yes | — |
| `low_phy_tx.cu:371`, `:540-550` | `cudaHostRegister`/`cudaHostAlloc` | — | radio buffer pinning, staging | init/lazy | no | — |
| `low_phy_puxch_rx.cu:561/584` | `cudaMemcpyAsync` | H2D | UL baseband window per symbol | UL demod in | yes (needs managed grid; else CPU fallback) | ~9 KB/sym/port @61.44 Msps |
| `low_phy_puxch_rx.cu:1120-1123` | sync/event poll | — | demod completion | yes | — | — |
| `ofdm_demodulator_cuda_impl.cpp:298`, `ofdm_prach_demodulator_cuda_impl.cpp:207/314` | `cudaStreamSynchronize` | — | error-path cleanup | no (error only) | — | — |

### 3.9 Managed-grid machinery (GB10/managed mode only)

| file:line | API | Purpose |
|---|---|---|
| `resource_grid_cuda_visible_impl.h:91` | `cudaMallocManaged` | grid allocation (init) |
| `:317/:348` `cudaEventRecord`, `:364` `cudaStreamWaitEvent` | producer/consumer ordering, device-side | per slot, non-blocking |
| `:430-441` event poll + `:558-561` `cudaMemPrefetchAsync` to host + `:570` `cudaStreamSynchronize` | **host access after GPU write** — on A100X-with-managed-override this is a real page migration + blocking sync per host reader | per slot if managed forced on discrete |
| `:481` `cudaMemsetAsync` | device-side grid zeroing | per slot |
| `:586` | prefetch default **false** (grids), | |

---

## 4. Discrete-only vs coherent-no-op transfers — thesis test

**Verified — these PCIe transfers/syncs exist only on the discrete (A100X) default config and vanish (or become coherent-fabric accesses) on GB10:**

1. **UL grid upload ~0.73 MB per PUSCH-bearing slot** — exists because the auto policy picks the pinned host grid on `integrated==0` (`resource_grid_cuda_visible_impl.h:658-674`); on managed grids the demodulator takes the `use_device_grid_reader` branch and does **zero grid copy** (`pusch_demodulator_gpu_impl.cpp:1851-1856, 2307-2309`).
2. **SRS full-grid upload ~0.73 MB per occasion** — sidecar stager only when `!grid.supports_device_grid_reading()` (`srs_estimator_cuda_impl.cpp:356-369`).
3. **DL encoded-symbol download ~314 KB/TB + CPU grid mapping** — GPU RE mapper explicitly disabled when `integrated==0` (`pdsch_block_processor_gpu_impl.cpp:65-75`); on GB10 symbols never leave the device.
4. **PDSCH acceleration disabled altogether in `auto`** on discrete (`upper_phy_factories.cpp:65-70`) — i.e., the fork's own policy concedes the discrete DL path is not profitable as-is.
5. **OFH CUDA compression roundtrip** (~1.2 MB/slot + ~56-112 blocking stream syncs per slot on OFH threads) — the *input* copy exists only when the grid isn't device-readable (`iq_compression_cuda.cpp:139,195` gating; host-span path `ofh_compression.cu:213-247`). Note `auto` still selects the CUDA backend on discrete (`compression_factory.cpp:54-57`) — **this is a per-symbol PCIe+sync cost center on A100X that is largely hidden on GB10**.
6. **Lower-PHY UL GPU OFDM demod unavailable** on discrete pinned grids → silent per-call CPU fallback (`ofdm_demodulator_cuda_impl.cpp:271` + `modulation_factories.cpp:229-252`).
7. **PRACH managed buffer is GPU-visible on A100X too** (gate is `cudaDevAttrManagedMemory`, not `integrated` — `phy_acceleration_prach_buffer_factory.cpp:29-33`), but on discrete the "no-copy" is implemented by **UVM page migration + explicit `cudaMemPrefetchAsync`** (`prach_buffer_cuda_visible_impl.h:352-374`, prefetch default true `:385`); on GB10 those prefetches return `cudaErrorNotSupported`/no-op. CPU writes (OFH PRACH decompress) then GPU reads = real PCIe traffic per occasion on A100X.
8. **GB10 assumptions in comments/defaults**: "OTA-stable default on the GB10/B210 CUDA-visible-grid setup" (`pdsch_grid_output_strategy.cpp:25-27`), "GH200 … zero-copy" (`resource_grid_pinned_impl.h:18`), "direct NVLink DMA" (`pusch_demodulator_gpu_impl.cpp:2312`) — the fast-path framing assumes coherent/NVLink platforms; on A100X the same `cudaMemcpyAsync` is a PCIe DMA.

**Always-present on both platforms (per-slot):** TB payload D2H (~150 KB), CB/TB CRC D2H (~KB), SINR/EVM D2H (36 B), UCI results (~100s B), DL TB H2D (~150 KB), and the per-PDU decoder completion poll-sync.

**Thesis assessment [HYPOTHESIS, grounded in the above]:** on A100X at 273 PRB/4L the *mandatory* per-slot PCIe volume in resident mode is modest (~1 MB UL + ~0.5 MB DL per cell ≈ a few tens of µs of PCIe Gen4 time), so raw bandwidth alone is unlikely to break the slot budget. The bigger discrete-platform risks are (a) the **OFH CUDA compression default**: dozens of small H2D/D2H+`cudaStreamSynchronize` round trips per slot on OFH RX/TX threads; (b) the **non-resident fallback** tripling LLR traffic (~7.5 MB/PDU) whenever the resident path declines a PDU; (c) **SRS full-grid uploads**; and (d) launch/sync latency of the many small copies (UCI, CRC, metrics) each adding ~µs-scale round trips on the single PUSCH task thread. The fork's own guards (items 4, 5, 6) show the authors expected discrete platforms to lose on exactly these paths.

---

## 5. Threading / executor model in accelerated mode

- **No CUDA host callbacks anywhere**: 0 hits for `cudaLaunchHostFunc`/`cudaStreamAddCallback` in the delta. All GPU completion is consumed by **host polling**: `cudaStreamSynchronizeYielding` / `cudaEventSynchronizeYielding` = `cudaStreamQuery`/`cudaEventQuery` + `sched_yield()` loops (`lib/phy/upper/channel_coding/ldpc/cuda/cuda_rt_utils.h:84-109`; duplicated at `pusch_demodulator_gpu_impl.cpp:301-323`), explicitly to avoid RT-priority inversion against normal-priority GPU driver threads (`cuda_rt_utils.h:72-83`). `cudaSetDeviceFlags(cudaDeviceScheduleYield)` at `pusch_demodulator_gpu_impl.cpp:661`. Genuine *blocking* `cudaStreamSynchronize` remains in: OFH compression per call (`ofh_compression.cu:173,183,203,3052`), PRACH detector per occasion (`prach_detector.cu:538`), managed-grid host prefetch (`resource_grid_cuda_visible_impl.h:570`, `prach_buffer_cuda_visible_impl.h:371`), and error/teardown paths.
- **PUSCH**: one upper-PHY task per PDU (enqueued by `lib/phy/upper/uplink_processor_impl.cpp`, tracing at delta lines around `L1.UL.PUSCH.enqueue/task`). That single host thread drives: H2D → E2E kernel enqueue (returns without sync in resident mode, `pusch_demodulator_gpu_impl.cpp:2694-2705`) → `try_decode_resident` **inline on the same thread** (`pusch_processor_impl.cpp:667-717` → `pusch_decoder_impl.cpp:753-848`) → single poll-sync at decoder completion (`pusch_codeblock_decoder_cuda_batch.cpp:1449-1462`) → deferred SINR readback via pre-join callback (`pusch_processor_impl.cpp:726-745`, `pusch_decoder_impl.cpp:834-838`). The decoder executor (`executor->defer`) is used only for the **non-resident** CB/batch paths (`pusch_decoder_impl.cpp:440-443, 573-577`).
- **Streams**: per-demodulator `stream_` + `scr_stream_` (`pusch_demodulator_gpu_impl.cpp:681-691`), dedicated `h2d_stream_` (`:885`) and `d2h_stream_` (`:787-789, 899`), cross-ordered with `h2d_complete/kernel_complete/d2h_complete` events over a 3-deep buffer rotation (`:1763-1769, 1819-1826, 2385-2393, 2679-2681, 2786-2788`). Stream priority selectable via `cudaStreamCreateUpperPhy` (`cuda_rt_utils.h:49-69`). Resident decode reuses the **demodulator's stream** passed as `execution_context` (`pusch_decoder_impl.cpp:816`, `pusch_codeblock_decoder_cuda_batch.cpp:1097-1098`), so demod→decode needs no host or event handoff.
- **Decoder pool**: bounded pool of `pusch_codeblock_decoder_cuda_batch` (~48 MB each, `processor_factories.cpp:272`), sized from `nof_pusch_decoder_threads` (`processor_factories.cpp:207-222`); **blocks the PUSCH thread on a condition variable if exhausted** (`processor_factories.cpp:64-87`).
- **PDSCH**: cb-batch tasks forked onto the DL executor (`pdsch_processor_flexible_impl.cpp:336-411`); each accelerated lane owns a `pdsch_tb_encoder_gpu` with its own stream (`pdsch_tb_encoder_cuda.cu:463`); lane count = `pdsch_acceleration_nof_lanes` (`du_low_config.h` delta; `upper_phy_factories.cpp:1346-1356`). Grid-output strategy object per DL processor (`downlink_processor_multi_executor_impl.cpp:49`), `before_send_grid()` materialization hook on the DL thread before `gateway.send` (delta in `downlink_processor_multi_executor_impl.cpp`).
- **Dedicated PRACH executor (the fork's PRACH routing change, split 8 / SDR)**: `worker_manager.cpp` adds a per-cell `lphy_prach#N` worker + `lower_phy_prach_exec#N` executor at `os_thread_realtime_priority::max()-5` — deliberately below radio (max) and lower-PHY TX/RX (max-1) "so PRACH scanning cannot preempt radio timing" (delta at `apps/services/worker_manager/worker_manager.cpp:537-545` and the two `create_prio_worker(name_prach, …)` blocks). It is plumbed as `cell_executors::prach_exec` into `ru_sdr_executor_mapper` (`include/ocudu/ru/sdr/ru_sdr_executor_mapper.h` delta), which routes PRACH to it and falls back to `rx_exec` when absent (`lib/ru/sdr/ru_sdr_executor_mapper.cpp` delta: `prach_exec = (baseband_exec.prach_exec != nullptr) ? *baseband_exec.prach_exec : *baseband_exec.rx_exec`). `prach_processor_worker` now dispatches the whole PRACH window handoff with `async_task_executor.execute(...)` on that executor — GPU demodulation (`ofdm_prach_demodulator_cuda_impl`) and the wait run there, off the RX slot path (`lib/phy/lower/processors/uplink/prach/prach_processor_worker.cpp` delta: "Production SDR mappings use a dedicated PRACH executor so the GPU demodulation and upper-PHY detection wait do not block the lower RX/TX slot path"). Upper-PHY PRACH *detection* (GPU `prach_detector_cuda_impl`, including its blocking sync) runs on the du-low upper-PHY PRACH executor as in mainline; the fork adds latency-warning instrumentation there (`lib/phy/upper/uplink_processor_impl.cpp` delta, `OCUDU_PRACH_LATENCY_WARN_US`).
- **OFH threads**: CUDA compression/decompression handles are pooled and acquired per call (`iq_compression_cuda.cpp:64-96`), executing synchronous GPU roundtrips directly on OFH transmitter/receiver threads.
- **Managed-grid concurrency** (GB10 mode): producer records `grid_ready` events, consumers `cudaStreamWaitEvent` device-side or poll-sync host-side under a mutex (`resource_grid_cuda_visible_impl.h:307-441`); host access spins with `sched_yield` while a device mapping is active (`:371-378`).