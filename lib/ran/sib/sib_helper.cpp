// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ocudu/ran/sib/sib_helper.h"
#include "ocudu/ran/band_helper.h"
#include "ocudu/ran/pdcch/pdcch_type0_css_coreset_config.h"
#include "ocudu/ran/pdcch/pdcch_type0_css_occasions.h"
#include <algorithm>

using namespace ocudu;

std::vector<unsigned> sib_helper::get_occupied_slot_offsets(const ssb_configuration& ssb_cfg,
                                                            nr_band                  band,
                                                            subcarrier_spacing       scs_common,
                                                            search_space0_index      ss0_idx,
                                                            uint8_t                  coreset0_idx)
{
  const bool is_fr2 = band_helper::get_freq_range(band) == frequency_range::FR2;

  const pdcch_type0_css_coreset_description coreset0_params =
      pdcch_type0_css_coreset_get(band, ssb_cfg.scs, scs_common, coreset0_idx, ssb_cfg.k_ssb.value());
  ocudu_assert(coreset0_params.pattern == ssb_coreset0_mplex_pattern::mplx_pattern1,
               "Only SS/PBCH and CORESET multiplexing pattern 1 is supported.");

  const pdcch_type0_css_occasion_pattern1_description occ = pdcch_type0_css_occasions_get_pattern1(
      {.is_fr2 = is_fr2, .ss0_index = ss0_idx, .nof_symb_coreset = coreset0_params.nof_symb_coreset});

  std::vector<unsigned> slots;
  for (unsigned ssb_idx = 0; ssb_idx < ssb_cfg.ssb_bitmap.size(); ++ssb_idx) {
    if (!ssb_cfg.ssb_bitmap.test(ssb_idx)) {
      continue;
    }
    const unsigned n0 = get_type0_pdcch_css_n0(occ.offset, occ.M, scs_common, ssb_idx).to_uint();
    // Type0-CSS PDCCH monitoring spans two consecutive slots starting at n0 per TS 38.213 Section 13.
    for (unsigned slot : {n0, n0 + 1}) {
      if (std::find(slots.begin(), slots.end(), slot) == slots.end()) {
        slots.push_back(slot);
      }
    }
  }
  return slots;
}
