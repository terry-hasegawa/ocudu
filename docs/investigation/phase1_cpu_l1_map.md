# OCUDU L1 — CPU (non-CUDA) Upper PHY Map

Scope: `lib/phy/upper/**` (plus `lib/phy/generic_functions/precoding/**`, which the upper-PHY resource-grid mapper depends on), excluding `**/cuda/**` and `*_cuda_*`/`*_gpu_*`/`*_hw_*` accelerator paths. All references are relative to repo root `/home/user/cuda_accelerated_ocudu`. Facts are verified by reading the cited code unless prefixed **[HYP]**.

Note on repo delta: the CPU kernel bodies (LDPC, CRC, modulation, equalizer, precoder, polar, short-block) are inherited unchanged from the 26.04 mainline base; the CUDA commit `9fd4047b43` only touched the *factory/CMake* glue and the flexible/decoder plumbing in this subtree (confirmed via `git diff --name-only 092414aac2..ccdf4e681f`). So the CPU track described here is the shared mainline L1.

---

## 1. Component → implementation-file → factory entry point

| Component | Main implementation file(s) | Factory entry point (file:line) |
|---|---|---|
| **CRC calculator** | `channel_coding/crc_calculator_generic_impl.cpp`, `crc_calculator_lut_impl.cpp`, `crc_calculator_clmul_impl.cpp` (x86), `crc_calculator_neon_impl.cpp` (ARM) | `ocudu::create_crc_calculator_factory_sw` — `channel_coding/channel_coding_factories.cpp:333`; dispatch in `crc_calculator_factory_sw_impl::create` `:50` |
| **LDPC encoder** | `channel_coding/ldpc/ldpc_encoder_impl.cpp` (base), `ldpc_encoder_generic.cpp`, `ldpc_encoder_avx2.cpp`, `ldpc_encoder_neon.cpp` | `create_ldpc_encoder_factory_sw` — `channel_coding_factories.cpp:306`; dispatch `ldpc_encoder_factory_sw::create` `:158` |
| **LDPC decoder** | `ldpc/ldpc_decoder_impl.cpp` (base), `ldpc_decoder_generic.cpp`, `ldpc_decoder_avx2.cpp`, `ldpc_decoder_avx512.cpp`, `ldpc_decoder_neon.cpp` | `create_ldpc_decoder_factory_sw` — `channel_coding_factories.cpp:293`; dispatch `ldpc_decoder_factory_sw::create` `:96` |
| **LDPC rate matcher (Tx)** | `ldpc/ldpc_rate_matcher_impl.cpp` | `create_ldpc_rate_matcher_factory_sw` — `channel_coding_factories.cpp:317` (generic-only) |
| **LDPC rate dematcher (Rx)** | `ldpc/ldpc_rate_dematcher_impl.cpp` (base/generic), `ldpc_rate_dematcher_avx2_impl.cpp`, `ldpc_rate_dematcher_avx512_impl.cpp`, `ldpc_rate_dematcher_neon_impl.cpp` | `create_ldpc_rate_dematcher_factory_sw` — `channel_coding_factories.cpp:311`; dispatch `ldpc_rate_dematcher_factory_sw::create` `:189` |
| **LDPC segmenter Tx / Rx** | `ldpc/ldpc_segmenter_tx_impl.cpp`, `ldpc_segmenter_rx_impl.cpp` | `create_ldpc_segmenter_tx_factory_sw` `:322`, `create_ldpc_segmenter_rx_factory_sw` `:328` |
| **Polar (code/enc/dec/rate match/interleave/alloc)** | `channel_coding/polar/polar_*_impl.cpp` | `create_polar_factory_sw` — `channel_coding_factories.cpp:338`; `polar_factory_sw` `:255` |
| **Short-block enc/detect** | `channel_coding/short/short_block_encoder_impl.cpp`, `short_block_detector_impl.cpp` | `create_short_block_detector_factory_sw` — `channel_coding_factories.cpp:343` |
| **Modulation mapper** | `channel_modulation/modulation_mapper_lut_impl.cpp`, `modulation_mapper_avx512_impl.cpp` (x86), `modulation_mapper_neon_impl.cpp` (ARM) | `create_modulation_mapper_factory` — `channel_modulation/channel_modulation_factories.cpp:69`; dispatch `modulation_mapper_factory_impl::create` `:25` |
| **Demodulation mapper (soft LLR)** | `channel_modulation/demodulation_mapper_impl.cpp` + per-order `demodulation_mapper_qpsk/qam16/qam64/qam256.cpp` | `create_demodulation_mapper_factory` — `channel_modulation_factories.cpp:74` (single impl; SIMD internal, see §2) |
| **EVM calculator** | `channel_modulation/evm_calculator_generic_impl.cpp` | `create_evm_calculator_factory` — `channel_modulation_factories.cpp:79` (generic-only) |
| **Channel equalizer (ZF/MMSE)** | `equalization/channel_equalizer_generic_impl.cpp` + headers `equalize_zf_1xn.h`, `equalize_zf_2xn.h`, `equalize_zf_mxn_simd.h`, `equalize_mmse_mxn_simd.h`, `gram_matrix.h`, `matrix_inverse.h` | `create_channel_equalizer_generic_factory` — `equalization/equalization_factories.cpp:28` (single impl; SIMD internal) |
| **Channel precoder** | `generic_functions/precoding/channel_precoder_generic.cpp`, `channel_precoder_avx2.cpp`, `channel_precoder_avx512.cpp`, `channel_precoder_neon.cpp` | `create_channel_precoder_factory` — `precoding/precoding_factories.cpp:59`; dispatch `channel_precoder_factory_impl::create` `:26` |
| **Port channel estimator (DMRS avg)** | `signal_processors/channel_estimator/port_channel_estimator_average_impl.cpp`, `port_channel_estimator_helpers.cpp` | `create_port_channel_estimator_factory_sw` — `channel_estimator/factories.cpp:41` (single impl) |
| **PUSCH channel estimator (DMRS)** | `signal_processors/pusch/dmrs_pusch_estimator_impl.cpp` | `signal_processors/pusch/factories.cpp` |
| **PUSCH demodulator** | `channel_processors/pusch/pusch_demodulator_impl.cpp` | `create_pusch_demodulator_factory_sw` — `pusch/demodulator_factories.cpp` (class `pusch_demodulator_factory_generic:20`) |
| **PUSCH codeblock decoder** | `pusch/pusch_codeblock_decoder.cpp` | wired inside `create_pusch_decoder_factory_sw` |
| **PUSCH decoder (TB)** | `pusch/pusch_decoder_impl.cpp` | `create_pusch_decoder_factory_sw` (CPU) |
| **PUSCH processor** | `pusch/pusch_processor_impl.cpp` | `create_pusch_processor_factory_sw` — `pusch/processor_factories.cpp` (`pusch_processor_factory_generic:316`); pool `create_pusch_processor_pool:482` |
| **ULSCH demultiplex** | `pusch/ulsch_demultiplex_impl.cpp` | `create_ulsch_demultiplex_factory_sw` |
| **UCI decoder** | `channel_processors/uci/uci_decoder_impl.cpp` | `create_uci_decoder_factory_generic` — `uci/factories.cpp:46` |
| **PDSCH encoder** | `pdsch/pdsch_encoder_impl.cpp` (segmenter→encoder→rate-matcher, `pdsch_encoder_impl.h:28`) | `create_pdsch_encoder_factory_sw` — `pdsch/factories.cpp` |
| **PDSCH block processor** | `pdsch/pdsch_block_processor_impl.cpp` (SW) | `create_pdsch_block_processor_factory_sw` — `pdsch/factories.cpp:444` |
| **PDSCH modulator** | `pdsch/pdsch_modulator_impl.cpp` | `create_pdsch_modulator_factory_sw` — `pdsch/factories.cpp:426` |
| **PDSCH processor (generic / flexible)** | `pdsch/pdsch_processor_impl.cpp`, `pdsch_processor_flexible_impl.cpp` | `create_pdsch_processor_factory_sw:434`, `create_pdsch_flexible_processor_factory_sw:465`, pool `create_pdsch_processor_pool:485` |
| **PDCCH processor / enc / mod** | `pdcch/pdcch_processor_impl.cpp`, `pdcch_encoder_impl.cpp`, `pdcch_modulator_impl.cpp` | `pdcch/factories.cpp` (`create_pdcch_processor_factory_sw`) |
| **PUCCH processor / detectors / demods** | `pucch/pucch_processor_impl.cpp`; `pucch_detector_format0/1.cpp`; `pucch_demodulator_format2/3/4.cpp` | `pucch/factories.cpp` (`create_pucch_processor_factory_sw:209`, demod `:228`, detector `:240`) |
| **PRACH detector / generator** | `prach/prach_detector_generic_impl.cpp`, `prach_generator_impl.cpp` | `create_prach_detector_factory_sw` — `prach/factories.cpp:197` (`prach_detector_factory_sw:64`) |
| **SRS estimator** | `signal_processors/srs/srs_estimator_generic_impl.cpp` | `srs/srs_estimator_factory.cpp` |
| **SSB processor (PBCH enc/mod, PSS/SSS/DMRS)** | `ssb/pbch_encoder_impl.cpp`, `pbch_modulator_impl.cpp`, `signal_processors/ssb/*` | `ssb/factories.cpp` |
| **Pseudo-random (scrambling) generator** | `sequence_generators/pseudo_random_generator_impl.cpp` | `sequence_generators/sequence_generator_factories.cpp` |
| **Rx softbuffer pool** | `rx_buffer_pool_impl.cpp`, `rx_buffer_impl.h`, `rx_buffer_codeblock_pool.h` | `create_rx_buffer_pool` — `upper_phy_factories.cpp:490` |
| **Uplink processor / pool** | `uplink_processor_impl.cpp`, `uplink_processor_pool_impl.cpp` | `uplink_processor_base_factory` — `upper_phy_factories.cpp:87`; pool `create_uplink_processor_pool:1489` |
| **Downlink processor / pool** | `downlink_processor_multi_executor_impl.cpp`, `downlink_processor_pool_impl.cpp` | `downlink_processor_single_executor_factory` — `upper_phy_factories.cpp:206`; pool `create_dl_processor_pool:1502` |
| **Upper PHY assembly** | `upper_phy_impl.cpp` | `create_upper_phy_factory` — `upper_phy_factories.cpp:1514` |

---

## 2. Per-kernel implementation variants and runtime selection

There are **two distinct selection mechanisms** in this codebase — this distinction matters for A100X:

- **(A) Runtime CPUID dispatch** — a factory queries `cpu_supports_feature(...)` (which is `__builtin_cpu_supports("avx2"/"avx512f"/...)`, `include/ocudu/support/cpu_features.h:94`) and picks a concrete subclass. The specialized `.cpp` files are compiled with explicit per-file `-mavx2`/`-mavx512*` flags (CMake `set_source_files_properties`), so they can be safely selected even if the general build isn't AVX512. Optional string arg (`"auto"|"generic"|"avx2"|"avx512"|"neon"`) can force a choice.
- **(B) Compile-time ISA gating** — a *single* impl whose SIMD is chosen at compile time via `#ifdef __AVX2__ / __AVX512F__ / __ARM_NEON`, driven by the global `-march=native` (`CMakeLists.txt:583-588`, x86) / `-mcpu=native` (`:580`, ARM). No runtime dispatch: the build host's ISA is baked in. The shared SIMD width comes from `include/ocudu/ocuduvec/simd.h` (`OCUDU_SIMD_*_SIZE`, `:75-133`).

### LDPC encoder — mechanism (A)
| Variant | File:line | Runtime condition |
|---|---|---|
| generic | `ldpc/ldpc_encoder_generic.h:15` | `enc_type ∈ {generic, auto}` fallback — `channel_coding_factories.cpp:174` |
| AVX2 (x86) | `ldpc_encoder_avx2.h:16` | `avx2` && `auto|avx2` — `:163` |
| NEON (ARM) | `ldpc_encoder_neon.h:16` | `neon` && `auto|neon` — `:170` |

No AVX512 encoder variant exists (encoder tops out at AVX2 on x86).

### LDPC decoder — mechanism (A)
| Variant | File:line | Runtime condition |
|---|---|---|
| generic | `ldpc/ldpc_decoder_generic.h:15` | fallback — `channel_coding_factories.cpp:116` |
| AVX2 | `ldpc_decoder_avx2.h:15` | `avx2` — `:105` |
| AVX512 (needs `avx512f`+`avx512bw`) | `ldpc_decoder_avx512.h:15` | `:102`; preferred over AVX2 in `auto` |
| NEON | `ldpc_decoder_neon.h:15` | `:112` |

`get_selected_type()` mirrors the same priority (`:123`).

### LDPC rate dematcher — mechanism (A)
| Variant | File:line | Runtime condition |
|---|---|---|
| generic | `ldpc_rate_dematcher_impl.h:18` | fallback — `channel_coding_factories.cpp:210` |
| AVX2 | `ldpc_rate_dematcher_avx2_impl.h:14` | `avx2` — `:199` |
| AVX512 (needs `avx512f`+`avx512bw`+**`avx512vbmi`**) | `ldpc_rate_dematcher_avx512_impl.h:14` | `:196` |
| NEON | `ldpc_rate_dematcher_neon_impl.h:14` | `:206` |

Rate **matcher** (Tx) is generic-only (`create` at `:223`).

### CRC calculator — mechanism (A)
| Variant | File:line | Runtime condition |
|---|---|---|
| generic (bit-serial) | `crc_calculator_generic_impl.h:12` | forced for CRC6 or `type=="generic"` — `:53` |
| LUT | `crc_calculator_lut_impl.h:15` | `auto|lut` fallback — `:73` |
| CLMUL (x86, needs `pclmul`+`sse4_1`) | `crc_calculator_clmul_impl.h:19` | `:60` |
| NEON PMULL (ARM, needs `neon`+`pmull`) | `crc_calculator_neon_impl.h:19` | `:68` |

### Modulation mapper — mechanism (A)
| Variant | File:line | Runtime condition |
|---|---|---|
| LUT (scalar) | `modulation_mapper_lut_impl.h:14` | fallback — `channel_modulation_factories.cpp:39` |
| AVX512 (needs `avx512f`+`avx512bw`+**`avx512vbmi`**) | `modulation_mapper_avx512_impl.h:17` | `:28` |
| NEON | `modulation_mapper_neon_impl.h:15` | `:34` |

No AVX2 modulation-mapper variant: on an AVX2-only host the mapper falls back to the scalar **LUT** path.

### Demodulation mapper (soft-LLR) — mechanism (B)
Single class `demodulation_mapper_impl` (`demodulation_mapper_impl.cpp:58`), dispatching by modulation order to per-order kernels, each internally `#ifdef`-gated:
- QPSK — `demodulation_mapper_qpsk.cpp`: AVX2 `:20`, NEON `:64`
- QAM16 — `demodulation_mapper_qam16.cpp`: AVX2 `:24`, NEON `:105`
- QAM64 — `demodulation_mapper_qam64.cpp`: `HAVE_AVX512` `:62`, AVX2 `:167`, NEON `:264`
- QAM256 — `demodulation_mapper_qam256.cpp`: `HAVE_AVX512` `:145`, AVX2 `:193`, NEON `:245`

`HAVE_AVX512` is defined only when the compiler already targets `__AVX512F__ && __AVX512BW__` (`qam64.cpp:9`, `qam256.cpp:10`) — i.e. purely compile-time, **no runtime CPUID dispatch and no per-file AVX512 flag**. On a non-AVX512 build host the AVX512 demod paths are compiled out entirely.

### Channel estimator — mechanism (B), single impl
`port_channel_estimator_average_impl.cpp` is the only estimator. Its hot interpolation kernel `simd_vector_interpolate` (`:957`) uses `ocudu_simd_f_*` intrinsics from `ocuduvec/simd.h` (compile-time width). No AVX2/AVX512/NEON-named variant files; no runtime dispatch.

### Channel equalizer (ZF/MMSE) — mechanism (B), single impl
Only `channel_equalizer_generic_impl` exists (`equalization_factories.cpp:12`). Despite the "generic" name it is heavily vectorized through templated headers `equalize_zf_mxn_simd.h` / `equalize_mmse_mxn_simd.h` using `simd_cf_t`/`simd_f_t` (compile-time width from `ocuduvec/simd.h`). Selection between ZF and MMSE is a `channel_equalizer_algorithm_type` factory arg (`:15`), **not** an ISA choice. No AVX/NEON variant files.

### Channel precoder — mechanism (A)
| Variant | File:line | Runtime condition |
|---|---|---|
| generic | `channel_precoder_generic.h:15` | fallback — `precoding_factories.cpp:46` |
| AVX2 (needs `avx2`+`fma`) | `channel_precoder_avx2.h:15` | `:35` |
| AVX512 (needs `avx512f`+`avx512bw`) | `channel_precoder_avx512.h:15` | `:32` |
| NEON | `channel_precoder_neon.h:15` | `:41` |

Called with `"auto"` from `upper_phy_factories.cpp:1151`.

### PUSCH demodulation chain — mechanism (B) at the orchestration level
`pusch_demodulator_impl` (`pusch_demodulator_impl.cpp`) owns: `channel_equalizer` (B), `transform_precoder`, `demodulation_mapper` (B), `evm_calculator` (generic), `pseudo_random_generator` (B). The `process()` loop per OFDM symbol: get channel estimates → `equalizer->equalize(...)` (`:338`) → optional transform-precoding → `demapper->demodulate_soft(...)` (`:380`) → optional EVM (`:385`) → `descrambler->generate(...)` + XOR (`:391`). It also `#include`s `immintrin.h`/`arm_neon.h` directly (`:15`,`:17`) for compile-time-gated inline scrambling. So the whole PUSCH demod chain is dominated by **compile-time-gated** SIMD, except the equalizer's inner width and the demapper — both also compile-time. There is no runtime ISA dispatch anywhere in the PUSCH demod chain.

### Pseudo-random generator (scrambling) — mechanism (B)
`pseudo_random_generator_impl.cpp` uses `nof_bits_per_simd` blocks (`:142`,`:308`,`:416`) via compile-time SIMD width. Single impl.

### Runtime-config plumbing
The type strings (`ldpc_encoder_type`, `ldpc_decoder_type`, `ldpc_rate_dematcher_type`, `crc_calculator_type`) live in `downlink_processor_factory_sw_config` / uplink config (`upper_phy_factories.h:143,299-324`) and are hard-set to `"auto"` by the app translator (`apps/units/flexible_o_du/o_du_low/du_low_config_translator.cpp:42,44,65`). So in production everything under mechanism (A) resolves via CPUID to the best available ISA; mechanism (B) resolves via `-march=native` on the build host.

---

## 3. Generic-only kernels (no SIMD specialization) — CPU-track optimization candidates

Verified generic-only (no AVX2/AVX512/NEON path, and not internally SIMD-vectorized through `ocuduvec/simd.h`):

1. **LDPC rate matcher (Tx)** — `ldpc/ldpc_rate_matcher_impl.cpp`; factory generic-only (`channel_coding_factories.cpp:223`). Uses `ocuduvec::bit_pack`/`copy` (`:158`,`:241`) which are themselves SIMD internally, but the matcher logic has no specialization. (DL-only; lower ROI on an UL-heavy accel target.)
2. **Polar code / encoder / decoder / rate matcher / rate dematcher / interleaver / allocator** — all of `channel_coding/polar/*` are single-impl; grep for AVX/NEON/intrinsics returns nothing. `polar_decoder_impl.h:18` is the only decoder. Used by PBCH and **UCI on PUSCH/PUCCH** (small blocks, moderate ROI).
3. **Short-block encoder / detector** — `channel_coding/short/short_block_*_impl.cpp`; no SIMD. Used for small UCI payloads.
4. **EVM calculator** — `channel_modulation/evm_calculator_generic_impl.cpp`; generic-only. Only active when EVM reporting is enabled.
5. **PRACH detector / generator** — `prach/prach_detector_generic_impl.cpp`, `prach_generator_impl.cpp`. The detector's heavy lifting is delegated to `dft_processor` (correlations via FFT), so the DFT backend (FFTW/AVX2-ci16) carries the SIMD, but the detector's own combining/threshold logic (`prach_detector_generic_thresholds.cpp`) is scalar.
6. **SRS estimator (generic)** — `srs/srs_estimator_generic_impl.cpp`; single impl (CUDA variant is the only alternative). Sequence correlation is scalar-ish here.
7. **UCI decoder** — `uci/uci_decoder_impl.cpp`; wraps polar + short-block (both generic) + CRC.

Kernels that are single-factory-impl but **already SIMD-vectorized** (via `ocuduvec/simd.h`, compile-time) and therefore *not* pure optimization candidates in the "add SIMD" sense: **channel equalizer**, **port channel estimator**, **demodulation mapper**, **pseudo-random generator**. For these the CPU-track lever is different: raising the compile-time ISA ceiling (see §5).

---

## 4. Processor structure notes

### PUSCH processor (`pusch/pusch_processor_impl.{h,cpp}`)
- **Reusable dependency bundle**: `concurrent_dependencies` (`pusch_processor_impl.h:30`) groups one `dmrs_pusch_estimator`, `pusch_demodulator`, `ulsch_demultiplex`, `uci_decoder`, plus three `pusch_uci_decoder_wrapper`s (HARQ-ACK / CSI-part1 / CSI-part2, `:73-79`). These are pooled via `bounded_unique_object_pool<concurrent_dependencies>` (`:82`) so each processing thread grabs a private set — this is the concurrency model for PUSCH.
- **Chain in `process()`**: acquire dependencies from pool → `estimator.estimate(...)` (`pusch_processor_impl.cpp:430`) writes channel estimates → configure UL-SCH demux (`:498`) → `demodulator.demodulate(...)` producing scrambled soft bits (`demodulator` obtained at `:513`) → `demultiplex.demultiplex(...)` separates SCH data vs UCI (`:589`) → `decoder->new_data(...)`/`set_nof_softbits(...)` feeds the TB decoder (`:559`,`:565`). Post-EQ SINR / EVM are gathered for CSI.
- **Decoder** (`pusch/pusch_decoder_impl.{h,cpp}`): holds `ldpc_segmenter_rx`, a `bounded_unique_object_pool<pusch_codeblock_decoder>` (`pusch_decoder_impl.h:26`), CRC set {CRC16, CRC24A, CRC24B}, and a `task_executor*` for **asynchronous per-codeblock decode**. Each `pusch_codeblock_decoder` (`pusch_codeblock_decoder.h:41`) chains `ldpc_rate_dematcher → ldpc_decoder → crc_calculator`. A `softbits_buffer` sized to the max codeword is held per decoder (`:63`). (`#ifdef ENABLE_CUDA` adds optional batch-GPU decoder ctors at `:97`/`:130` — not part of the CPU track.)
- **Softbuffer / HARQ handling**: `rx_buffer_pool_impl` (`rx_buffer_pool_impl.h:21`) owns a `rx_buffer_codeblock_pool` and a `std::vector<rx_buffer_impl>`; `reserve(slot, id, nof_codeblocks, new_data)` (`:46`) maps a `trx_buffer_identifier` to a buffer with an expiration timeout (`run_slot` ages them out). Each `rx_buffer_impl` (`rx_buffer_impl.h:24`) is a 3-state FSM (`available → reserved → locked`, `:27-46`), stores per-CB CRC flags and codeblock IDs (`crc`, `codeblock_ids`, `:53-55`), and returns codeblocks to the pool on `free()`/`release()`. This is what carries soft bits across HARQ retransmissions; on CRC pass the buffer is released, on fail it stays `reserved` for combining.

### PDSCH processor variants
Two configurations selected via `std::variant<pdsch_processor_generic_configuration, pdsch_processor_flexible_configuration>` (`upper_phy_factories.h:156,380`, structs `:114`/`:122`) — chosen in `upper_phy_factories.cpp:1347` (generic) vs `:1352` (flexible). There is **no separate "concurrent" or "lite" class**; "lite/flexible/concurrent" behavior is all folded into the *flexible* processor plus its wrapping pool.

- **Generic** — `pdsch/pdsch_processor_impl.{h,cpp}`. Straight-line: `encode()` (segmenter→`ldpc_encoder`→`ldpc_rate_matcher`, via `pdsch_encoder_impl.h:28`) → `modulate()` (`pdsch_processor_impl.h:54`) → DMRS + PTRS. One TB at a time, synchronous. Factory `create_pdsch_processor_factory_sw` (`pdsch/factories.cpp:434`; class `pdsch_processor_factory_sw:111`).
- **Flexible** — `pdsch/pdsch_processor_flexible_impl.{h,cpp}`. Parameterizable concurrency + memory footprint (`pdsch_processor_flexible_impl.h:22`). Holds three `bounded_unique_object_pool`s — block-processor, DMRS-generator, PTRS-generator (`:27-31`) — an LDPC `segmenter`, a `resource_grid_mapper`, a `task_executor&`, and `max_nof_codeblocks_per_batch`. Codeblocks are batched and dispatched to the executor (`sync_pdsch_cb_processing()` for synchronous mode). `cb_batch_length == synchronous_cb_batch_length` (`= UINT_MAX`, header `:124`) forces a single synchronous batch. Factory `create_pdsch_flexible_processor_factory_sw` (`pdsch/factories.cpp:465`; class `pdsch_processor_flexible_factory_sw:305`). The block processor comes from `create_pdsch_block_processor_factory_sw` (`:444`, CPU LDPC path) — vs the CUDA `..._accelerated` / HW `..._hw` alternates chosen at `upper_phy_factories.cpp:1327/1332/1336`.
- **Async pool wrapper** — `pdsch_processor_asynchronous_pool_factory` (`pdsch/factories.cpp:363`), applied via `create_pdsch_processor_pool` (`:485`) when `pool_size > 1` (`upper_phy_factories.cpp:1438`). For the non-accelerated flexible-async case the pool is sized to `MAX_PDSCH_PDUS_PER_SLOT` (`:1398`).

---

## 5. A100X-specific flags for the CPU track

**[HYP]** The fork's dev platform was DGX Spark GB10 (ARM/`aarch64`, `sm_121`, coherent). On the CPU side that means everything went through the **NEON** paths (mechanism A) and `-mcpu=native` (mechanism B). On the A100X target the host is **x86_64**, so:

- Mechanism-(A) kernels will CPUID-dispatch into the x86 AVX2/AVX512/CLMUL paths — these are exercised on x86 but were *not* the fork's daily-driven code path, so treat the x86 dispatch results (esp. the AVX512+VBMI predicates for LDPC rate-dematcher `channel_coding_factories.cpp:196` and modulation mapper `channel_modulation_factories.cpp:28`) as needing validation on the actual A100X host.
- **[HYP]** DGX/HGX A100 hosts are commonly AMD EPYC (Zen2/Zen3), which support **AVX2 but not AVX512** (Zen2) or a slower double-pumped AVX512 (Zen4). If the A100X host lacks AVX512: (a) mechanism-(A) LDPC decoder, LDPC rate-dematcher, precoder, and modulation mapper silently fall back to AVX2 (or, for the modulation mapper, to the scalar **LUT** — no AVX2 mapper exists); and (b) all mechanism-(B) kernels compiled with `-march=native` on that host (demod mapper, LLR `log_likelihood_ratio.cpp:258`, channel estimator, equalizer, scrambler) get only AVX2 width (`OCUDU_SIMD_*_SIZE` = 8-float, `simd.h:87-96`) rather than AVX512 width (16-float, `:75-84`). This roughly halves SIMD throughput on those hot kernels versus an AVX512 host. Confirm the host CPU's AVX512 support before assuming the AVX512 paths are live.
- Because mechanism-(B) is baked in at compile time from `-march=native` (`CMakeLists.txt:583`), building on one A100X host and deploying on another with a lower ISA would be unsafe for the demod/LLR/estimator/equalizer paths — unlike the mechanism-(A) kernels which are per-file-flagged and CPUID-guarded. This is a build-vs-deploy portability caveat specific to the compile-time-gated group.

---

# OCUDU L1 — CPU Lower PHY and Generic Kernel Layers

All file:line references are relative to the mainline checkout root (`/home/user/ocudu`) unless prefixed `fork:` (= `/home/user/cuda_accelerated_ocudu`, branch `cuda_accel.26_04`). All claims are verified by reading the code except where explicitly labeled **[hypothesis]**. Note: the vector library historically named `srsvec` is renamed `ocuduvec` in OCUDU (`lib/ocuduvec/`, headers in `include/ocudu/ocuduvec/`).

---

## 1. Lower PHY structure (`lib/phy/lower/**`)

### 1.1 Top-level composition

`lower_phy_impl` aggregates a DL processor, an UL processor, a baseband controller, a handler adaptor (upper→lower requests) and a notifier adaptor (lower→upper events) — `lib/phy/lower/lower_phy_impl.h:31-107`, wired in `lib/phy/lower/lower_phy_impl.cpp:10-28`. The SW factory `lower_phy_factory_sw::create()` builds all three and computes rx buffer sizing policy, tx time offset (N_TA,offset − calibration) and rx-to-tx max delay — `lib/phy/lower/lower_phy_factory.cpp:52-174` (offset math at :41-48, buffer policy switch at :80-94, `rx_to_tx_max_delay = srate.to_kHz() + tx_time_offset` at :100).

### 1.2 Baseband engine (controller)

`lower_phy_baseband_processor` is the real-time core driving both directions with two self-reposting task loops (`lib/phy/lower/lower_phy_baseband_processor.h:73-205`):

- `start()` seeds one `ul_process()` task on the **rx executor** and one `dl_process(init_time + rx_to_tx_max_delay)` task on the **tx executor** — `lib/phy/lower/lower_phy_baseband_processor.cpp:48-60`.
- **DL loop** `dl_process(timestamp)`: throttles against the last rx timestamp (busy-sleep loop, `.cpp:79-93`) and optional system-time throttling (`.cpp:96-111`); calls `downlink_processor.process(ts)` to obtain one slot (or slot-remainder) of samples; calls `transmitter.transmit()`; re-defers itself with `timestamp += buffer size` — `.cpp:114-137`.
- **UL loop** `ul_process()`: pops a buffer from a `blocking_queue` of `baseband_gateway_buffer_dynamic` (pool created at `.cpp:42-46`), calls `receiver.receive()`, publishes `last_rx_timestamp`, then **defers the demodulation work onto the ul executor** (`uplink_processor.process(...)` then returns buffer to pool) and immediately re-defers itself on the rx executor — `.cpp:139-173`.
- Stop is coordinated by a lock-free FSM (`internal_fsm`, `.h:88-160`) that counts `stop_nof_slots = 2 * max_processing_delay_slots` extra iterations (`lower_phy_factory.cpp:141`) before releasing a promise.

### 1.3 Downlink chain (per slot)

1. `downlink_processor_baseband_impl::process(timestamp)` converts the timestamp into subframe/slot/symbol indices (`lib/phy/lower/processors/downlink/downlink_processor_baseband_impl.cpp:83-124`); on a new slot it notifies the **TTI boundary** `nof_slot_tti_in_advance` slots ahead (`on_tti_boundary`, `.cpp:130-137` — this is the event that triggers the upper PHY to start producing the DL grid) and pulls that slot's pre-modulated baseband from the PDxCH processor via `process_slot()` (`.cpp:141`).
2. CFO is applied slot-wide on the result — samples are ci16, so they are converted ci16→cf, `cfo_processor.process()`, cf→ci16 (`.cpp:144-158`; CFO rotation itself is `ocuduvec::prod_cexp`, `lib/phy/lower/processors/baseband_cfo_processor.h:88`).
3. If timing is not slot-aligned or no PDxCH data exists, a pool buffer is filled with zeros / tail samples instead (`.cpp:167-188`, `fill_zeros` at :47-67).
4. **PDxCH processor** (`pdxch_processor_impl`, `lib/phy/lower/processors/downlink/pdxch/pdxch_processor_impl.h:30-142`): `handle_request(grid, context)` (upper PHY push) grabs a baseband buffer from a pool and hands it to one of **4 `pdxch_baseband_modulator` instances** selected by `slot % 4` (`.h:97`, `.h:109-112`, `pdxch_processor_impl.cpp:31-61`). Completed slots are exchanged into a lock-free `resource_request_pool<slot_result>` (`on_modulation_completion`, `.cpp:63-76`); `process_slot()` swaps them out and reports lates on slot mismatch (`.cpp:84-102`).
5. **`pdxch_baseband_modulator::handle_request`** fans out **one deferred task per (OFDM symbol × port)** on the modulation executor (`lib/phy/lower/processors/downlink/pdxch/pdxch_baseband_modulator.h:100-194`, defer at :144). Each task: OFDM-modulate symbol → amplitude control → power/clipping metrics → cf→ci16 conversion (`.h:156-168`). A per-modulator atomic counter (`state_modulate_mask + nsymb*nports`, `.h:113-114`, completion at :198-236) detects the last task, aggregates metrics and notifies.
6. **OFDM modulation** (`ofdm_symbol_modulator_impl::modulate`, `lib/phy/lower/modulation/ofdm_modulator_impl.cpp:39-90`): reads the grid halves into the IDFT input (negative frequencies mapped to upper bins, `.cpp:74-77`), runs the DFT processor, applies TS 38.211 §5.4 phase compensation + scaling via `ocuduvec::sc_prod` (`.cpp:83-86`), copies the CP (`.cpp:89`). Phase compensation LUT in `lib/phy/lower/modulation/phase_compensation_lut.h`; center-frequency retune re-derives it lazily (`.cpp:44-49`). Because 4 modulators × N symbol-tasks may run concurrently, the shared symbol modulator is wrapped by a lock-free **instance pool** `ofdm_symbol_modulator_pool` (`lib/phy/lower/modulation/ofdm_modulator_pool.h:16-64`), sized `MAX_NSYMB_PER_SLOT * nof_tx_ports` (`lib/ru/sdr/lower_phy/lower_phy_factory.cpp:28-29`).
7. **Amplitude control**: two implementations — scaling only (`ocuduvec::sc_prod`, `lib/phy/lower/amplitude_controller/amplitude_controller_scaling_impl.cpp:10-19`) and gain+clipping with PAPR/clipping metrics (`ocuduvec::clip_iq`, `lib/phy/lower/amplitude_controller/amplitude_controller_clipping_impl.cpp:13-50`); chosen by `amplitude_config.enable_clipping` (`lib/ru/sdr/lower_phy/lower_phy_factory.cpp:43-48`).

### 1.4 Uplink chain (per received buffer)

`lower_phy_uplink_processor_impl` is a two-state FSM (`alignment`/`collecting`) that reassembles OFDM symbols from arbitrarily-sized radio buffers (`lib/phy/lower/processors/uplink/uplink_processor_impl.h:77-84`, `.cpp:83-118`). Per completed symbol (`process_collecting`, `.cpp:166-272`):

1. concatenate ci16 samples into `temp_buffer` (`ocuduvec::copy`, `.cpp:186-195`);
2. CFO compensation with ci16→cf→ci16 round trip (`.cpp:206-220`);
3. feed the symbol to the **PRACH processor** (`prach_proc->get_baseband().process_symbol`, `.cpp:222-225`) and the **PUxCH processor** (`.cpp:228-230`);
4. power/clipping metrics if PUxCH consumed the symbol (`.cpp:232-255`);
5. notify **half-slot** and **full-slot** timing boundaries (`.cpp:258-267`) — these drive upper-PHY UL slot pacing.

**PUxCH** (`puxch_processor_impl::process_symbol`, `lib/phy/lower/processors/uplink/puxch/puxch_processor_impl.cpp:13-73`): on slot change, exchanges the slot's grid request out of a `resource_request_pool<shared_resource_grid>` (`.cpp:24`, late detection `.cpp:30-34`); skips the symbol entirely if no grid was requested (`.cpp:42-44`); otherwise per port converts ci16→cf and OFDM-demodulates directly **into the upper-PHY resource grid writer** (`.cpp:55-62`), then notifies `on_rx_symbol` (`.cpp:65`). OFDM demodulation (`ofdm_symbol_demodulator_impl::demodulate`, `lib/phy/lower/modulation/ofdm_demodulator_impl.cpp:77-129`) skips the CP with a configurable DFT window offset (default 0.5 CP, `lib/phy/lower/processors/uplink/uplink_processor_factories.cpp:46`), runs the forward DFT, applies phase compensation (`sc_prod`) plus per-bin window-offset phase ramp (`prod`, `.cpp:114-120`).

**PRACH** (`prach_processor_impl`, `lib/phy/lower/processors/uplink/prach/prach_processor_impl.h:19-98`) is a pool of `prach_processor_worker`s (one per concurrent occasion, count = `max_nof_prach_concurrent_requests`, `prach_processor_factories.cpp:39-49`). Each worker is a 4-state FSM idle→wait→collecting→processing (`prach_processor_worker.h:35-50`): it accumulates the PRACH window samples in ci16 (`prach_processor_worker.cpp:66-90`), then **defers the demodulation onto the dedicated PRACH async executor** (`.cpp:100-133`): ci16→cf conversion, `ofdm_prach_demodulator_impl::demodulate` per port, `on_rx_prach_window` notification. The PRACH demodulator holds one DFT per candidate RA SCS (`lib/phy/lower/modulation/modulation_factories.cpp:163-189`) and handles the 16κ CP corrections for short preambles (`ofdm_prach_demodulator_impl.cpp:80-108`).

### 1.5 Resource grid handoff upper ↔ lower PHY

- **Downwards (requests)**: `processor_handler_adaptor` exposes `lower_phy_downlink_handler::handle_resource_grid` → `pdxch_processor_request_handler::handle_request`, and `lower_phy_uplink_request_handler::{request_prach_window, request_uplink_slot}` → PRACH/PUxCH handlers (`lib/phy/lower/processors/adaptors/processor_handler_adaptor.h:18-79`). Grids are reference-counted `shared_resource_grid` objects; the lower PHY copies the reference (`grid.copy()`, `pdxch_baseband_modulator.h:120`; `puxch_processor_impl.cpp:85`) and releases it when modulation completes (`pdxch_baseband_modulator.h:210`) or the UL slot's last symbol is written (`puxch_processor_impl.cpp:68-70`).
- **Slot matching** uses the lock-free `resource_request_pool` (16 entries indexed by `slot.system_slot() % 16`, exchange semantics with late-slot reporting) — `lib/phy/lower/processors/resource_request_pool.h:33-88`.
- **Upwards (notifications)**: `processor_notifier_adaptor` maps processor events to `lower_phy_timing_notifier` (TTI/half/full slot), `lower_phy_rx_symbol_notifier` (UL grid symbols, PRACH windows), error and metrics notifiers — `lib/phy/lower/processors/adaptors/processor_notifier_adaptor.h:22-123`.

### 1.6 Component → file map

| Component | File (mainline) | Key lines |
|---|---|---|
| Lower PHY facade | `lib/phy/lower/lower_phy_impl.h` / `.cpp` | h:31-107, cpp:10-28 |
| SW factory | `lib/phy/lower/lower_phy_factory.cpp` | 52-183 |
| Baseband RT engine / controller | `lib/phy/lower/lower_phy_baseband_processor.{h,cpp}` | h:73-205, cpp:48-173 |
| DL processor wrapper | `lib/phy/lower/processors/downlink/downlink_processor_impl.h` | 15-50 |
| DL baseband (slot timing, TTI, CFO, zero-fill) | `lib/phy/lower/processors/downlink/downlink_processor_baseband_impl.{h,cpp}` | cpp:83-193 |
| PDxCH processor (request/exchange) | `lib/phy/lower/processors/downlink/pdxch/pdxch_processor_impl.{h,cpp}` | h:30-142, cpp:31-102 |
| PDxCH async slot modulator | `lib/phy/lower/processors/downlink/pdxch/pdxch_baseband_modulator.h` | 44-281 |
| OFDM modulator | `lib/phy/lower/modulation/ofdm_modulator_impl.{h,cpp}` | cpp:39-90 |
| OFDM modulator concurrency pool | `lib/phy/lower/modulation/ofdm_modulator_pool.h` | 16-64 |
| Amplitude controllers | `lib/phy/lower/amplitude_controller/amplitude_controller_{clipping,scaling}_impl.cpp` | 13-50 / 10-19 |
| UL processor (symbol FSM) | `lib/phy/lower/processors/uplink/uplink_processor_impl.{h,cpp}` | cpp:83-272 |
| PUxCH processor | `lib/phy/lower/processors/uplink/puxch/puxch_processor_impl.{h,cpp}` | cpp:13-103 |
| OFDM demodulator | `lib/phy/lower/modulation/ofdm_demodulator_impl.cpp` | 77-129 |
| PRACH worker pool | `lib/phy/lower/processors/uplink/prach/prach_processor_impl.h` | 19-98 |
| PRACH worker FSM | `lib/phy/lower/processors/uplink/prach/prach_processor_worker.{h,cpp}` | cpp:14-194 |
| PRACH OFDM demodulator | `lib/phy/lower/modulation/ofdm_prach_demodulator_impl.cpp` | 14-120+ |
| Request pool (grid handoff) | `lib/phy/lower/processors/resource_request_pool.h` | 33-88 |
| Handler / notifier adaptors | `lib/phy/lower/processors/adaptors/processor_{handler,notifier}_adaptor.h` | 18-79 / 22-123 |
| CFO processor | `lib/phy/lower/processors/baseband_cfo_processor.h` | 22-95 |
| RU-SDR lower PHY assembly | `lib/ru/sdr/lower_phy/lower_phy_factory.cpp` | 9-111 |

### 1.7 CUDA hooks in the WG1 fork (for reference; CPU code above is shared)

Verified from `git diff 092414aac2..ccdf4e681f`:

- New config knobs `tx_acceleration_mode` / `rx_acceleration_mode` / `prach_demodulation_acceleration_mode` (`"auto"|"enabled"|"disabled"`) in `fork: include/ocudu/phy/lower/lower_phy_configuration.h` (diff hunk at struct `lower_phy_configuration`).
- New factory entry points `create_ofdm_demodulator_factory_accelerated` and `create_ofdm_prach_demodulator_factory_accelerated` in `fork: include/ocudu/phy/lower/modulation/modulation_factories.h`, implemented in `fork: lib/phy/lower/modulation/modulation_factories.cpp` with CUDA impls `ofdm_demodulator_cuda_impl.{h,cpp}` and `ofdm_prach_demodulator_cuda_impl.{h,cpp}`.
- DL: `pdxch_processor_impl` gains an accelerator-factory constructor argument; `create_pdxch_processor_factory_cuda` + `is_pdxch_processor_factory_cuda_available()` (calls `cudaGetDeviceCount`) in `fork: lib/phy/lower/processors/downlink/pdxch/pdxch_processor_factories.cpp`; the GPU slot modulator is `pdxch_baseband_modulator_cuda.cpp` behind the interface `pdxch_baseband_modulator_accelerator.h`. This replaces the per-symbol×port CPU task fan-out with a whole-slot GPU path.
- UL: `uplink_processor_impl` refactored with `process_complete_symbol()` (`fork: lib/phy/lower/processors/uplink/uplink_processor_impl.h` diff), and `puxch_processor_impl.cpp` / `prach_processor_worker.cpp` modified to feed the CUDA demodulators.
- **[hypothesis / platform flag]** The hook *locations* are platform-neutral, but the CUDA implementations were developed on GB10 (coherent memory); on A100X the ci16 host buffers used throughout this chain (`baseband_gateway_buffer_dynamic`) would require explicit H2D/D2H staging — that concern lives in the CUDA-side files, not in the shared CPU code mapped here.

---

## 2. FFT/DFT backend selection (`lib/phy/generic_functions/**`)

### 2.1 Backends present

| Backend | File | Notes |
|---|---|---|
| **Generic** (always built) | `lib/phy/generic_functions/dft_processor_generic_impl.cpp` | Template radix decimation-in-time; arbitrary-N fallback via full table DFT (`:15-61`), radix-2 specialization (`:70-80`+); may not support all sizes (`generic_functions_factories.cpp:31-35` returns nullptr if `!is_valid()`). |
| **FFTW-API** (FFTW3f, Intel MKL, or ARM PL) | `lib/phy/generic_functions/dft_processor_fftw_impl.cpp` | One plan per instance, global init mutex (`:104-110`), wisdom persisted to `~/.ocudu_fftwisdom` (`:74`). MKL and ARMPL are consumed *through the FFTW3 API* — same source file, different link library. |
| **AOCL-FFTZ** (AMD Zen) | `lib/phy/generic_functions/dft_processor_fftz_impl.{h,cpp}` | `#include <aoclfftz.h>`; opt_level 0-3 selects non-SIMD…AVX512 (`dft_processor_fftz_impl.h:18-29`); factory hardcodes opt_level 3, no bit-reproducibility (`generic_functions_factories.cpp:90-95`). |
| **ci16 AVX2 DFT** | `lib/phy/generic_functions/dft_processor_ci16_avx2.cpp` | Fixed-point 16-bit complex DFT, power-of-two sizes ≤ 8192 (template recursion, `:284-308`), x86-only, compiled with `-mavx2` (`CMakeLists.txt:46-49`), gated at runtime by `cpu_supports_feature(cpu_feature::avx2)` (`generic_functions_factories.cpp:134-143`). **Not used by the production lower/upper PHY factories** — only referenced by tests/benchmarks (grep across repo: `tests/unittests/...`, `tests/benchmarks/...`). |

### 2.2 Build-time selection (CMake)

`lib/phy/generic_functions/CMakeLists.txt`:
- Priority ladder for the FFTW-API library: **MKL** (`:16-20`) → **ARMPL** (`:21-25`) → **FFTW3f** (`:26-30`); any of them adds `dft_processor_fftw_impl.cpp` and defines `HAVE_FFTW` (`:41-44`).
- FFTZ independently adds `dft_processor_fftz_impl.cpp` + `HAVE_FFTZ` (`:33-39`).
- Options default ON at top level: `ENABLE_FFTW`, `ENABLE_MKL`, `ENABLE_FFTZ`, `ENABLE_ARMPL` — `CMakeLists.txt:67-70`.

### 2.3 Run-time selection (factories)

- `create_dft_processor_factory()` (used by the RU-SDR lower PHY at `lib/ru/sdr/lower_phy/lower_phy_factory.cpp:19` and by the upper PHY at `lib/phy/upper/upper_phy_factories.cpp:586`) tries **FFTW → FFTZ → generic** in that order — `include/ocudu/phy/generic_functions/generic_functions_factories.h:88-101`.
- The no-arg FFTW factory picks the plan profile by build type: `estimate/1 s` ("slow") for non-release, non-x86_64, or sanitizer builds; `measure/5 s` ("fast") otherwise — `lib/phy/generic_functions/generic_functions_factories.cpp:145-152`, profiles defined at `generic_functions_factories.h:57-72`. Optimization flag strings parsed at `generic_functions_factories.cpp:65-74`.
- Modulator/demodulator factories receive the DFT factory by injection (`lib/phy/lower/modulation/modulation_factories.cpp:27-39, 124-138`), so the OFDM chain is backend-agnostic.

### 2.4 Precoding / transform precoding (also in `generic_functions`)

- **Channel precoder** (used by upper PHY, listed here as it lives in this directory): implementations `channel_precoder_generic/avx2/avx512/neon` (`lib/phy/generic_functions/precoding/`), per-file ISA flags `-mavx2;-mfma` and `-mavx512f;-mavx512bw` (`precoding/CMakeLists.txt:11-17`), **runtime** dispatch on `cpu_supports_feature(avx512f&avx512bw | avx2&fma | neon)` with string override `"auto"|"avx512"|"avx2"|"neon"|"generic"` — `precoding/precoding_factories.cpp:26-51`.
- **Transform precoder** (PUSCH DFT-s-OFDM): `transform_precoder_dft_impl::deprecode_ofdm_symbol` runs one pre-created DFT per valid PRB count and scales by 1/√M_sc (`transform_precoding/transform_precoder_dft_impl.cpp:13-42`); factory pre-instantiates inverse DFTs for every valid PRB size (`transform_precoding/transform_precoding_factories.cpp:24-40`).

---

## 3. `ocuduvec` SIMD coverage (`lib/ocuduvec/**`, `include/ocudu/ocuduvec/**`)

### 3.1 Abstraction model

`include/ocudu/ocuduvec/simd.h` defines a portable wrapper ISA (`ocudu_simd_f_*`, `ocudu_simd_cf_*`, `ocudu_simd_s_*`, `ocudu_simd_b_*`, `ocudu_simd_c16_*`) with a compile-time ladder **AVX512F → AVX2 → SSE4.1 → NEON → none** in every wrapper (e.g. `ocudu_simd_f_load`, simd.h:166-183; `ocudu_simd_cf_prod`, simd.h:973; int16 `ocudu_simd_s_add`, simd.h:1881; int8 `ocudu_simd_b_add`, simd.h:2756; float→int16/bf16 converters simd.h:2188-2617). Vector widths: AVX512 16 floats / AVX2 8 / SSE4.1 and NEON 4 (simd.h:75-133). Selection is purely **compile-time** via `-march` (default `-march=native` on x86_64, `-mcpu=native` on aarch64 — top-level `CMakeLists.txt:573-596`); there is **no runtime dispatch inside ocuduvec** (unlike the channel precoder). The `c16` fixed-point wrapper set is dead code by default: `OCUDU_SIMD_C16_SIZE` is forced to 0 unless `ENABLE_C16` is defined (simd.h:135-138), and `ENABLE_C16` is defined nowhere in the build (repo-wide grep hits only simd.h).

Ops written against the wrappers automatically get AVX512/AVX2/SSE/NEON paths; a few ops instead use **raw intrinsics** and therefore have narrower coverage.

### 3.2 Per-operation coverage

| Operation | File | SIMD path | Evidence |
|---|---|---|---|
| `add` (float, cf, int16, int8) | `lib/ocuduvec/add.cpp` | wrappers: all ISAs (F at :14-45, S at :65-85, B at :96-116) + scalar tail | `#if OCUDU_SIMD_F_SIZE` / `OCUDU_SIMD_S_SIZE` |
| `subtract` (float, cf, int16, int8) | `lib/ocuduvec/subtract.cpp` | wrappers: all ISAs (:14, :45, :76) | same pattern |
| `prod`, `prod_conj`, `prod_cexp` (CFO rotator) | `lib/ocuduvec/prod.cpp` | wrappers: all ISAs (F :16, S :47, CF :79-148) | `#if OCUDU_SIMD_*` |
| `sc_prod`, `sc_prod_and_add` (cf/float/cbf16) | `lib/ocuduvec/sc_prod.cpp` | wrappers: all ISAs (:14-122) | `#if OCUDU_SIMD_F/CF_SIZE` |
| `dot_prod` (cf×cf, cbf16 variants), `average_power` | `lib/ocuduvec/dot_prod.cpp` | wrappers: all ISAs (:18-32, :49-63, :87-100, :119-132) | `#if OCUDU_SIMD_CF_SIZE` |
| `modulus_square`, `modulus_square_and_add` | `lib/ocuduvec/modulus_square.cpp` | wrappers: all ISAs (:14, :43, :69, :101) | `#if OCUDU_SIMD_CF_SIZE` |
| `accumulate` (float sum) | `lib/ocuduvec/accumulate.cpp` | wrappers: all ISAs (:17) | `#if OCUDU_SIMD_F_SIZE` |
| `divide` (float) | `lib/ocuduvec/division.cpp` | wrappers: all ISAs (:14) | `#if OCUDU_SIMD_F_SIZE` |
| `convolution` (`detail::multiply_and_accumulate`) | `lib/ocuduvec/convolution.cpp` | wrappers: all ISAs (:38) | `#if OCUDU_SIMD_F_SIZE` |
| `convert` (cf↔ci16, cf↔int16, bf16/cbf16 families, ~20 overloads) | `lib/ocuduvec/conversion.cpp` | **raw intrinsics**: AVX512F(+VL/BW masked tails) at :53-88, :215-271, :324-387; AVX2 at :90-106, :274-293, :390-410; NEON at :296-314, :413-432; NEON-BF16 at :132-139; scalar tail always | this is the hot ci16↔cf path of the lower PHY |
| `max_abs_element`, `max_element` | `lib/ocuduvec/compare.cpp` | AVX2 helper (:18-32) + NEON helper (:34-59) + wrapper path (`OCUDU_SIMD_CF_SIZE`, :80, :144); scalar fallback | mixed raw+wrapper |
| `count_if_part_abs_greater_than` (clipping metric) | `lib/ocuduvec/compare.cpp:192-241` | **AVX2 only** (:196-236) + scalar; **no AVX512, no NEON path** | raw intrinsics |
| bit pack/unpack, `copy_offset` | `lib/ocuduvec/bit.cpp` | **AVX512F+BW** (:68-96 unpack, :221-247 pack) and **AVX2** (:98-139, :313-325); **no NEON path** — scalar on ARM | raw intrinsics |
| `clip`, `clip_iq`, `clip_magnitude` | `lib/ocuduvec/clip.cpp:9-58` | **scalar only** (no SIMD refs in file) | grep: zero SIMD guards |
| `unwrap_arguments` (phase unwrap) | `lib/ocuduvec/unwrap.cpp:44` | **scalar only** | zero SIMD guards |
| `copy` | `include/ocudu/ocuduvec/copy.h:21-29` | `std::copy` (header-only; relies on compiler autovectorization) | — |
| `zero` / `fill` | `include/ocudu/ocuduvec/zero.h:16-21`, `fill.h` | `std::fill` (header-only) | — |
| `find` (char) | `lib/ocuduvec/compare.cpp:12` | AVX512BW/AVX2 helpers per file guards (:68-139 region of bit-style guards in compare/bit) | partially raw |

**Platform note for A100X (x86_64 host):** with the default `-march=native` on an x86 host all wrapper-based ops compile to AVX512 or AVX2 depending on the host CPU; the fork's dev platform (GB10, aarch64) would instead exercise the NEON wrapper branch and *lose* the raw-intrinsic AVX paths in `conversion.cpp`'s AVX512-tail variants, `bit.cpp` and `count_if_part_abs_greater_than` (scalar on ARM). Anything benchmarked on GB10 for these three ops does not transfer to x86 and vice-versa — verified from the ifdef structure cited above.

---

## 4. Threading / executor model of the lower PHY

### 4.1 Executor roles (defined by the lower PHY itself)

Five executors enter the lower PHY (`lower_phy_dependencies`, consumed at `lib/phy/lower/lower_phy_factory.cpp:144-152` and `lib/ru/sdr/lower_phy/lower_phy_factory.cpp:87-97`):

| Executor | Work executed | Where enqueued |
|---|---|---|
| **rx** | `receiver.receive()` blocking radio reads; self-reposting loop | `lower_phy_baseband_processor.cpp:55, 172` |
| **tx** | `dl_process`: DL slot assembly + `transmitter.transmit()`; self-reposting loop (includes the rx-throttle busy-wait, :79-93) | `lower_phy_baseband_processor.cpp:58, 133-136` |
| **ul** | `uplink_processor.process()` per rx buffer: symbol reassembly, CFO, PUxCH OFDM demod into grids, PRACH sample collection, timing notifications | `lower_phy_baseband_processor.cpp:158-169` |
| **dl (modulation)** | PDxCH per-(symbol×port) tasks: OFDM modulation, amplitude control, metrics, cf→ci16 | `pdxch_baseband_modulator.h:144-174`; executor plumbed via `downlink_proc_factory->create(cfg, dependencies.dl_task_executor)` (`lower_phy_factory.cpp:113-114`) |
| **prach_async** | PRACH window demodulation (deferred once the full window is captured) | `prach_processor_worker.cpp:100-133` |

Per-slot flow on the wire: the **tx thread** performs the TTI-boundary notification (`downlink_processor_baseband_impl.cpp:130-137` runs inside `dl_process`), swaps in the pre-modulated slot, applies CFO and transmits; the actual IFFT work happened earlier, asynchronously, on the **dl executor** (up to 4 slots in flight, `pdxch_processor_impl.h:97`). The **rx thread** only reads samples and republishes; all demodulation happens on the **ul executor**; PUxCH OFDM demod runs inline on the ul thread (single `ofdm_symbol_demodulator`, no pool in the SW PUxCH factory — `puxch_processor_factories.cpp:45-46`), while PRACH FFTs are pushed off to prach_async.

### 4.2 Thread provisioning (application layer, gnb `worker_manager`)

`apps/services/worker_manager/worker_manager.cpp:536-665` creates the actual threads per profile; mapping to lower-PHY roles in `lib/ru/sdr/ru_sdr_executor_mapper.cpp:40-121`:

| Profile | Threads per cell | Executor mapping (dl / ul / prach / tx / rx) |
|---|---|---|
| `sequential` | none dedicated (shared `phy_exec`) | dl=ul=prach=**inline executor**, tx=rx=`phy_exec` (`ru_sdr_executor_mapper.cpp:43-58`) |
| `single` | `lower_phy#N` (RT prio max, `worker_manager.cpp:561-583`) | dl=prach=`rt_prio_exec` pool queue, ul=**inline** (demod on the rx thread), tx=rx=`lower_phy_exec#N` (`ru_sdr_executor_mapper.cpp:60-78`) |
| `dual` | `lower_phy_tx#N` (prio max), `lower_phy_rx#N` (prio max−1) (`worker_manager.cpp:585-616`) | dl=prach=`rt_prio_exec`, ul=**inline** on rx thread, tx/rx=dedicated (`ru_sdr_executor_mapper.cpp:80-99`) |
| `triple` | `lower_phy_tx#N` (max), `lower_phy_rx#N` (max−2, queue depth 1), `lower_phy_ul#N` (max−1) (`worker_manager.cpp:618-658`) | dl=prach=`rt_prio_exec`, ul=`lower_phy_ul_exec#N`, tx/rx=dedicated (`ru_sdr_executor_mapper.cpp:101-121`) |

`rt_prio_exec` is the real-time-priority queue of the general worker pool (`worker_manager.cpp:338, 357`); the radio itself gets a separate `radio` thread (`worker_manager.cpp:541-546`). "Inline executor" means the task runs synchronously in the caller's thread (`include/ocudu/support/executors/inline_task_executor.h`, instantiated at `ru_sdr_executor_mapper.cpp:130`).

### 4.3 Synchronization primitives (no locks on the fast path)

- rx-buffer recycling: `blocking_queue` (`lower_phy_baseband_processor.h:196`).
- DL slot exchange & UL grid requests: lock-free `resource_request_pool` try-lock exchange (`resource_request_pool.h:65-77`).
- PDxCH modulator lifecycle: atomic task counter FSM (`pdxch_baseband_modulator.h:111-114, 198-236`); static assert ties pool size to modulator count to guarantee single-writer per pool entry (`pdxch_processor_impl.h:105-106`).
- start/stop: `internal_fsm` atomic + promise (`lower_phy_baseband_processor.h:88-160`); PDxCH stop spin-waits on modulator completion (`pdxch_processor_impl.h:76-84`); PRACH uses `rt_stop_event_source` tokens (`prach_processor_worker.h:80, .cpp:143-171`).
- Cross-thread pacing: `last_rx_timestamp` atomic acquire/release (`lower_phy_baseband_processor.cpp:89, 155`) plus 10 µs sleep polling — the only "blocking" on the tx path.

**Fork note [verified diff, relevant to threading]:** the CUDA commit rewrites `lower_phy_baseband_processor.cpp` (+259 lines in the diff stat) and `pdxch_baseband_modulator.h` (+72), i.e. the fork changes both the RT loop and the DL fan-out policy when acceleration is active; the CPU model described above is the `092414aac2` mainline behavior that remains the fallback (`acceleration_mode = "disabled"`).