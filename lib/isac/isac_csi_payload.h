// SPDX-FileCopyrightText: 2026 OCUDU ISAC sensing PoC
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief ISAC sensing PoC (Block A) — wire payload for one per-PDU CSI snapshot.
///
/// Disposable prototype: a fixed binary header followed by the raw H body. No protobuf.
/// See lib/isac/README.md for the Block D (Python/numpy) decode recipe.

#pragma once

#include "ocudu/phy/upper/signal_processors/pusch/dmrs_pusch_estimator.h"
#include "ocudu/ran/resource_block.h"
#include <cstdint>
#include <vector>

namespace ocudu {
namespace isac {

/// Magic value identifying an ISAC CSI message ("ISAC" as little-endian uint32).
static constexpr uint32_t ISAC_CSI_MAGIC = 0x43415349U; // 'I','S','A','C'

/// Wire format version. Bump on any layout change.
static constexpr uint16_t ISAC_CSI_VERSION = 1;

/// Maximum number of Rx branches carried (matches PUSCH architectural ceiling).
static constexpr unsigned ISAC_MAX_RX_PORTS = 4;

/// \brief Fixed-size message header (little-endian, byte-packed).
///
/// Followed immediately by the H body: \c nof_rx_ports * \c nof_re complex coefficients,
/// branch-major, each stored as two little-endian float32 (real, imag).
/// Body length in bytes = \c nof_rx_ports * \c nof_re * 2 * sizeof(float).
#pragma pack(push, 1)
struct isac_csi_header {
  uint32_t magic;        ///< ISAC_CSI_MAGIC.
  uint16_t version;      ///< ISAC_CSI_VERSION.
  uint16_t header_bytes; ///< sizeof(isac_csi_header), for forward compatibility.
  uint32_t seq;          ///< Monotonic sequence number (per sink); gaps => drops.
  uint32_t sfn;          ///< System frame number (0..1023).
  uint32_t slot_index;   ///< Slot index within the radio frame.
  uint32_t system_slot;  ///< Absolute slot count (slot_point::system_slot()).
  uint16_t scs_khz;      ///< Subcarrier spacing in kHz (15/30/60/120/240).
  uint8_t  numerology;   ///< Numerology index (0..4).
  uint8_t  rank;         ///< Number of Tx layers (=1 for the PoC).
  uint8_t  nof_rx_ports; ///< Number of Rx branches in the body (<= ISAC_MAX_RX_PORTS).
  uint8_t  dmrs_symbol;  ///< Representative OFDM symbol index the H snapshot was read at.
  uint16_t prb_start;    ///< Lowest allocated PRB (CRB index).
  uint16_t prb_count;    ///< Number of allocated PRBs.
  uint16_t nof_re;       ///< Subcarriers per branch (= prb_count * 12).
  uint8_t  is_contiguous;///< 1 if the PRB allocation is contiguous.
  uint8_t  has_metrics;  ///< 1 if epre/rsrp/snr arrays are valid.
  uint64_t ts_rel_ns;    ///< Steady-clock nanoseconds since sink start.
  float    epre[ISAC_MAX_RX_PORTS]; ///< Per-port EPRE (linear). Valid if has_metrics.
  float    rsrp[ISAC_MAX_RX_PORTS]; ///< Per-port RSRP (linear). Valid if has_metrics.
  float    snr[ISAC_MAX_RX_PORTS];  ///< Per-port SNR  (linear). Valid if has_metrics.
};
#pragma pack(pop)

static_assert(sizeof(isac_csi_header) == 94, "Unexpected ISAC CSI header size; update README/Block D.");

/// \brief Serializes one per-PDU CSI snapshot into \c out (header + H body).
///
/// Reads the channel estimate read-only via \c results.get_symbol_ch_estimate() for each Rx
/// branch (tx_layer 0) at the representative DM-RS symbol, converting cbf16 -> float32 (re,im).
/// Does not mutate \c results or \c config. Never throws (errors yield an empty \c out).
///
/// \param[in]  config      The estimator configuration captured at estimate() time (metadata).
/// \param[in]  results     The completed estimator results (H reader); read-only.
/// \param[in]  seq         Monotonic sequence number to stamp.
/// \param[in]  ts_rel_ns   Relative timestamp (ns) to stamp.
/// \param[out] out         Destination byte buffer (resized to the full message).
void serialize_csi(const dmrs_pusch_estimator::configuration& config,
                   const dmrs_pusch_estimator_results&        results,
                   uint32_t                                   seq,
                   uint64_t                                   ts_rel_ns,
                   std::vector<uint8_t>&                      out) noexcept;

} // namespace isac
} // namespace ocudu
