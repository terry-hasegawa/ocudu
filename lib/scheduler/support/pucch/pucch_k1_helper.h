// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/adt/span.h"
#include "ocudu/adt/static_vector.h"
#include "ocudu/ran/pdcch/dci_format.h"
#include "ocudu/ran/pusch/pusch_constants.h"
#include <optional>
#include <vector>

namespace ocudu {

struct tdd_ul_dl_config_common;
struct pusch_time_domain_resource_allocation;

/// \brief Returns the PDSCH-to-HARQ-timing-indicator candidate list to use for scheduling PUCCH HARQ-ACK feedback,
/// as per TS 38.213, clause 9.2.3.
///
/// - For DCI format 1_0, the PDSCH-to-HARQ-timing-indicator field values map to {1, 2, 3, 4, 5, 6, 7, 8}.
/// - For DCI format 1_1 (if present), they map to the values configured via dl-DataToUL-ACK (Table 9.2.3-1).
///
/// Note: tested UEs do not support k1 < 4, so the default list returned for DCI format 1_0 starts at 4 instead of 1.
///
/// \param dci_format         DCI format that will schedule the PDSCH whose HARQ-ACK is being placed.
/// \param dl_data_to_ul_ack  Configured dl-DataToUL-ACK list, used when \p dci_format is not 1_0.
span<const uint8_t> get_k1_candidates(dci_dl_format dci_format, span<const uint8_t> dl_data_to_ul_ack);

/// \brief Computes the list of valid PUCCH k1 values that can be used for a given DL slot.
///
/// For TDD, the returned vector is circularly indexed by slot within the TDD period (size = TDD period length). For
/// FDD, the vector contains a single entry (list) that applies to every DL slot.
///
/// The valid k1 values per slot are derived as follows:
/// - \b FDD: all values in \c dl_data_to_ul_ack are valid for every slot.
/// - \b TDD UL-heavy (more full-UL slots than DL slots): all values in \c dl_data_to_ul_ack are valid for every DL
///   slot; UL slots and non-DL slots get an empty list.
/// - \b TDD DL-heavy (nof DL slots >= full-UL slots): for each DL slot, only k1 values that are >= the slot's
///   assigned k2 (from \c pusch_td_resource_indices_per_slot) are valid, ensuring that a PUSCH cannot be scheduled
///   before any PUCCH carrying the HARQ-ACK scheduled for the same slot. DL slots with no assigned PUSCH carry the full
///   k1 list. UL slots get an empty list.
///
/// \param[in] dl_data_to_ul_ack   List of candidate k1 values (dl-DataToUL-ACK), in ascending order.
/// \param[in] tdd_cfg_common      TDD UL/DL configuration. If absent, the cell operates in FDD mode.
/// \param[in] pusch_td_alloc_list PUSCH time-domain resource allocation table for the cell.
/// \param[in] pusch_td_resource_indices_per_slot Per-slot list of indices into \c pusch_td_alloc_list that are
/// applicable for PUSCH scheduling in that slot, as produced by \c get_pusch_td_resource_indices_per_slot(). Used in
/// the DL-heavy TDD case to determine the minimum k2 assigned to each DL slot and, hence, the minimum valid k1.
/// \return Vector of valid k1 lists, one entry per slot in the TDD period (or one entry for FDD). Empty inner lists
///   indicate slots for which no PUCCH k1 scheduling is applicable (UL or non-DL slots).
std::vector<static_vector<uint8_t, 8>>
get_pucch_k1_list_per_slot(span<const uint8_t>                                       dl_data_to_ul_ack,
                           const std::optional<tdd_ul_dl_config_common>&             tdd_cfg_common,
                           const std::vector<pusch_time_domain_resource_allocation>& pusch_td_alloc_list,
                           const std::vector<static_vector<unsigned, pusch_constants::MAX_NOF_PUSCH_TD_RES_ALLOCS>>&
                               pusch_td_resource_indices_per_slot);

} // namespace ocudu
