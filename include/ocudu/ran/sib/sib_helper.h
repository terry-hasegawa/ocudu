// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/ran/nr_band.h"
#include "ocudu/ran/pdcch/search_space.h"
#include "ocudu/ran/ssb/ssb_configuration.h"
#include "ocudu/ran/subcarrier_spacing.h"
#include <cstdint>
#include <vector>

namespace ocudu::sib_helper {

/// \brief Returns the slot offsets (within one SIB1 retransmission period) where SIB1 Type0-CSS PDCCH monitoring
/// is active.
///
/// Slot offsets are expressed in \c scs_common slot units. For each active SSB beam, the two consecutive Type0-CSS
/// monitoring slots n0 and n0+1 are included per TS 38.213 Section 13. Duplicate values across beams are reported
/// once. Output order is not guaranteed.
///
/// \param ssb_cfg      SSB configuration carrying the active-SSB bitmap, SSB SCS, and k_SSB.
/// \param band         NR band, used to determine FR1/FR2 and the CORESET#0 table (TS 38.213 Tables 13-1 to 13-10).
/// \param scs_common   Reference SCS used for slot counting (i.e., the common SCS).
/// \param ss0_idx      SearchSpace#0 index from PDCCH-ConfigSIB1 (parameter \c searchSpaceZero, TS 38.331).
/// \param coreset0_idx CORESET#0 index from PDCCH-ConfigSIB1 (parameter \c controlResourceSetZero, TS 38.331).
/// \return             List of unique slot offsets within one Type0-CSS window (two consecutive frames) where
///                     SIB1 PDCCH monitoring is active.
std::vector<unsigned> get_occupied_slot_offsets(const ssb_configuration& ssb_cfg,
                                                nr_band                  band,
                                                subcarrier_spacing       scs_common,
                                                search_space0_index      ss0_idx,
                                                uint8_t                  coreset0_idx);

} // namespace ocudu::sib_helper
