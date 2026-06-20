// SPDX-FileCopyrightText: 2026 OCUDU ISAC sensing PoC
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "isac_csi_payload.h"
#include "ocudu/adt/complex.h"
#include "ocudu/adt/span.h"
#include "ocudu/ran/subcarrier_spacing.h"
#include <array>
#include <cstring>

using namespace ocudu;
using namespace ocudu::isac;

void ocudu::isac::serialize_csi(const dmrs_pusch_estimator::configuration& config,
                                const dmrs_pusch_estimator_results&        results,
                                uint32_t                                   seq,
                                uint64_t                                   ts_rel_ns,
                                std::vector<uint8_t>&                      out) noexcept
{
  out.clear();

  // Number of Rx branches actually estimated (rank-1 => single Tx layer, index 0).
  const unsigned nof_rx_ports = config.rx_ports.size();
  if (nof_rx_ports == 0) {
    return;
  }

  // Allocation in the frequency domain. The estimator exposes H over all REs of the allocated RBs.
  const int lowest_prb  = config.rb_mask.find_lowest();
  const int highest_prb = config.rb_mask.find_highest();
  const unsigned rb_count = static_cast<unsigned>(config.rb_mask.count());
  if (rb_count == 0 || lowest_prb < 0 || highest_prb < 0) {
    return;
  }
  const unsigned nof_re = rb_count * static_cast<unsigned>(NOF_SUBCARRIERS_PER_RB);
  if (nof_re > static_cast<unsigned>(MAX_NOF_SUBCARRIERS)) {
    return;
  }

  // Representative OFDM symbol: the first DM-RS-carrying symbol of the allocation.
  int dmrs_symbol = config.symbols_mask.find_lowest();
  if (dmrs_symbol < 0) {
    dmrs_symbol = static_cast<int>(config.first_symbol);
  }

  // Assemble the header.
  const subcarrier_spacing scs = config.slot.scs();

  isac_csi_header hdr{};
  hdr.magic         = ISAC_CSI_MAGIC;
  hdr.version       = ISAC_CSI_VERSION;
  hdr.header_bytes  = static_cast<uint16_t>(sizeof(isac_csi_header));
  hdr.seq           = seq;
  hdr.sfn           = config.slot.sfn();
  hdr.slot_index    = config.slot.slot_index();
  hdr.system_slot   = config.slot.system_slot();
  hdr.scs_khz       = static_cast<uint16_t>(scs_to_khz(scs));
  hdr.numerology    = static_cast<uint8_t>(config.slot.numerology());
  hdr.rank          = static_cast<uint8_t>(config.get_nof_tx_layers());
  hdr.nof_rx_ports  = static_cast<uint8_t>(nof_rx_ports);
  hdr.dmrs_symbol   = static_cast<uint8_t>(dmrs_symbol);
  hdr.prb_start     = static_cast<uint16_t>(lowest_prb);
  hdr.prb_count     = static_cast<uint16_t>(rb_count);
  hdr.nof_re        = static_cast<uint16_t>(nof_re);
  hdr.is_contiguous = (static_cast<unsigned>(highest_prb + 1 - lowest_prb) == rb_count) ? 1U : 0U;
  hdr.has_metrics   = 1U;
  hdr.ts_rel_ns     = ts_rel_ns;

  // Per-port signal metrics (linear scale). Read-only getters on the estimator results.
  for (unsigned i_port = 0; i_port != nof_rx_ports && i_port != ISAC_MAX_RX_PORTS; ++i_port) {
    hdr.epre[i_port] = results.get_epre(i_port);
    hdr.rsrp[i_port] = results.get_rsrp(i_port, /*tx_layer=*/0);
    hdr.snr[i_port]  = results.get_snr(i_port);
  }

  // Lay out: [header][port0 re,im ...][port1 ...]... as float32.
  const size_t body_floats = static_cast<size_t>(nof_rx_ports) * nof_re * 2U;
  const size_t total_bytes = sizeof(isac_csi_header) + body_floats * sizeof(float);

  try {
    out.resize(total_bytes);
  } catch (...) {
    out.clear();
    return;
  }

  std::memcpy(out.data(), &hdr, sizeof(isac_csi_header));
  auto* body = reinterpret_cast<float*>(out.data() + sizeof(isac_csi_header));

  // Read-only copy of the channel estimate for each Rx branch at the representative symbol.
  std::array<cbf16_t, MAX_NOF_SUBCARRIERS> tmp;
  span<cbf16_t>                            tmp_view(tmp.data(), nof_re);
  for (unsigned i_port = 0; i_port != nof_rx_ports; ++i_port) {
    results.get_symbol_ch_estimate(tmp_view, static_cast<unsigned>(dmrs_symbol), i_port, /*tx_layer=*/0);

    float* dst = body + static_cast<size_t>(i_port) * nof_re * 2U;
    for (unsigned i_re = 0; i_re != nof_re; ++i_re) {
      const cf_t h          = to_cf(tmp_view[i_re]);
      dst[2U * i_re]        = h.real();
      dst[2U * i_re + 1U]   = h.imag();
    }
  }
}
