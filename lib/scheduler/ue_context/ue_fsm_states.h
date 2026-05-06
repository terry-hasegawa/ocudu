// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include <cstdint>

namespace ocudu {

/// States of the Finite State Machine (FSM) representing the UE stage of configuration.
enum class ue_fsm_states : uint8_t {
  /// RACH-created UE: waiting for ConRes MAC CE to be ACKed by the UE.
  pending_conres_ce,
  /// F1AP-created UE: waiting for C-RNTI MAC CE to be received from the UE.
  pending_crnti_ce,
  /// ConRes CE was ACKed but the confirmation of the RRC Setup/Reestablishment Complete not yet confirmed.
  pending_setup,
  /// Awaiting RRC Reconfiguration Complete for RRC Reconfiguration right after RRC Reestablishment.
  pending_reest_reconf,
  /// Awaiting RRC Reconfiguration Complete for RRC Reconfiguration.
  pending_reconf,
  /// The UE is not in fallback mode.
  normal
};

/// Whether the UE is in fallback mode.
inline bool is_in_fallback(ue_fsm_states state)
{
  return state != ue_fsm_states::normal;
}

inline const char* to_string(ue_fsm_states state)
{
  switch (state) {
    case ue_fsm_states::pending_conres_ce:
      return "pending_conres_ce";
    case ue_fsm_states::pending_crnti_ce:
      return "pending_crnti_ce";
    case ue_fsm_states::pending_setup:
      return "pending_setup";
    case ue_fsm_states::pending_reest_reconf:
      return "pending_reest_reconf";
    case ue_fsm_states::pending_reconf:
      return "pending_reconf";
    case ue_fsm_states::normal:
      return "normal";
  }
  return "unknown";
}

/// Type of events that trigger UE FSM updates.
enum class ue_fsm_config_event : uint8_t {
  conres_ce_acked,
  conres_ce_timeout,
  crnti_ce_received,
  reest_reconf_initiated,
  reconf_initiated,
  config_applied
};

inline const char* to_string(ue_fsm_config_event ev)
{
  switch (ev) {
    case ue_fsm_config_event::conres_ce_acked:
      return "conres_ce_acked";
    case ue_fsm_config_event::conres_ce_timeout:
      return "conres_ce_timeout";
    case ue_fsm_config_event::crnti_ce_received:
      return "crnti_ce_received";
    case ue_fsm_config_event::reest_reconf_initiated:
      return "reest_reconf_initiated";
    case ue_fsm_config_event::reconf_initiated:
      return "reconf_initiated";
    case ue_fsm_config_event::config_applied:
      return "config_applied";
  }
  return "unknown";
}

} // namespace ocudu
