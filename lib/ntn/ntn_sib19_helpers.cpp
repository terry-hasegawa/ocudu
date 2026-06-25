// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ntn_sib19_helpers.h"

using namespace ocudu;
using namespace ocudu_ntn;

sib19_info ocudu_ntn::generate_sib19_info(const ntn_cell_config&   cell_cfg,
                                          slot_point               epoch_slot,
                                          const ntn_orbital_state& serving_reply,
                                          const ntn_orbital_state* sat_sw_reply)
{
  unsigned   ntn_ul_sync_validity_dur = cell_cfg.assistance_info.ntn_ul_sync_validity_dur.value_or(5);
  sib19_info sib19;
  sib19.ref_location           = cell_cfg.assistance_info.reference_location;
  sib19.distance_thres         = cell_cfg.assistance_info.distance_threshold;
  sib19.t_service              = cell_cfg.assistance_info.t_service;
  sib19.ncells                 = cell_cfg.assistance_info.ncells;
  sib19.moving_ref_location    = cell_cfg.assistance_info.moving_reference_location;
  sib19.sat_switch_with_resync = cell_cfg.assistance_info.sat_switch_with_resync;

  sib19.ntn_cfg.emplace();
  sib19.ntn_cfg->cell_specific_koffset = cell_cfg.assistance_info.cell_specific_koffset;
  sib19.ntn_cfg->polarization          = cell_cfg.assistance_info.polarization;
  sib19.ntn_cfg->ta_report             = cell_cfg.assistance_info.ta_report;
  sib19.ntn_cfg->k_mac                 = cell_cfg.assistance_info.k_mac;
  sib19.ntn_cfg->epoch_time.emplace();
  sib19.ntn_cfg->epoch_time->sfn             = epoch_slot.sfn();
  sib19.ntn_cfg->epoch_time->subframe_number = epoch_slot.subframe_index();
  sib19.ntn_cfg->ephemeris_info              = serving_reply.ephemeris_info;
  if (cell_cfg.assistance_info.feeder_link_info) {
    sib19.ntn_cfg->ta_info = serving_reply.ta_info;
  }
  sib19.ntn_cfg->ntn_ul_sync_validity_dur = ntn_ul_sync_validity_dur;

  if (cell_cfg.assistance_info.ta_info && cell_cfg.assistance_info.ta_info->ta_common_offset != 0.0) {
    if (!sib19.ntn_cfg->ta_info) {
      sib19.ntn_cfg->ta_info.emplace();
    }
    sib19.ntn_cfg->ta_info->ta_common_offset = cell_cfg.assistance_info.ta_info->ta_common_offset;
  }

  // Populate sat-switch target ntn_cfg with propagated ephemeris from the sat-switch OCM.
  if (sat_sw_reply != nullptr && sat_sw_reply->success && sib19.sat_switch_with_resync) {
    auto& sat_sw = *sib19.sat_switch_with_resync;
    sat_sw.ntn_cfg.epoch_time.emplace();
    sat_sw.ntn_cfg.epoch_time->sfn             = epoch_slot.sfn();
    sat_sw.ntn_cfg.epoch_time->subframe_number = epoch_slot.subframe_index();
    sat_sw.ntn_cfg.ephemeris_info              = sat_sw_reply->ephemeris_info;
    sat_sw.ntn_cfg.ta_info                     = sat_sw_reply->ta_info;
    sat_sw.ntn_cfg.ntn_ul_sync_validity_dur =
        sat_sw.ntn_cfg.ntn_ul_sync_validity_dur.value_or(ntn_ul_sync_validity_dur);
  }

  // Propagate serving satellite ephemeris and TA-info to all neighbor NTN cells.
  for (auto& ncell : sib19.ncells) {
    if (!ncell.ntn_cfg) {
      ncell.ntn_cfg.emplace();
    }
    ncell.ntn_cfg->ephemeris_info = sib19.ntn_cfg->ephemeris_info;
    ncell.ntn_cfg->ta_info        = sib19.ntn_cfg->ta_info;
  }

  return sib19;
}
