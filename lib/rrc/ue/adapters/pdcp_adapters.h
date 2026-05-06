// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/pdcp/pdcp_rx.h"
#include "ocudu/pdcp/pdcp_tx.h"
#include "ocudu/ran/cause/ngap_cause.h"

namespace ocudu {
namespace ocucp {

/// Helper structure used to store and pop RRC PDUs received from PDCP.
struct rrc_ue_rx_pdu_info {
  /// The PDU buffer.
  byte_buffer rrc_pdu;
  /// Indicates whether the integrity of \c rrc_pdu is verified (true) or unverified/unchecked (false).
  bool integrity_verified = false;
};

/// Adapter between PDCP Rx data and RRC in UL direction (Rx)
class pdcp_rrc_ue_rx_adapter : public pdcp_rx_upper_data_notifier, public pdcp_rx_upper_control_notifier
{
public:
  pdcp_rrc_ue_rx_adapter() = default;

  void on_new_sdu(byte_buffer sdu, bool integrity_verified) override
  {
    rrc_pdus.push_back(rrc_ue_rx_pdu_info{.rrc_pdu = std::move(sdu), .integrity_verified = integrity_verified});
  }

  void on_protocol_failure() override
  {
    ocudulog::fetch_basic_logger("PDCP").warning(
        "Requesting UE release. Cause: Received protocol failure from PDCP Rx");
    cause = cause_protocol_t::unspecified;
  }

  void on_integrity_failure() override
  {
    ocudulog::fetch_basic_logger("PDCP").warning(
        "Requesting UE release. Cause: Received integrity failure from PDCP Rx");
    cause = cause_protocol_t::unspecified;
  }

  void on_max_count_reached() override
  {
    ocudulog::fetch_basic_logger("PDCP").warning("Requesting UE release. Cause: Max count reached from PDCP Rx");
    cause = cause_protocol_t::unspecified;
  }

  void on_resume_required() override
  {
    ocudulog::fetch_basic_logger("PDCP").error("Unsupported request for SRB resume from PDCP Rx");
  }

  std::variant<std::vector<rrc_ue_rx_pdu_info>, ngap_cause_t> pop_result()
  {
    if (cause.has_value()) {
      auto ret = *cause;
      cause.reset();
      return ret;
    }
    return std::move(rrc_pdus);
  }

private:
  std::vector<rrc_ue_rx_pdu_info> rrc_pdus;
  std::optional<ngap_cause_t>     cause;
};

/// Adapter between PDCP and RRC UE for DL PDUs
class pdcp_rrc_ue_tx_adapter : public pdcp_tx_lower_notifier
{
public:
  pdcp_rrc_ue_tx_adapter() = default;

  void on_new_pdu(byte_buffer pdu, bool is_retx) override
  {
    pdcp_pdu         = std::move(pdu);
    pdcp_pdu_is_retx = is_retx;
  }

  void on_discard_pdu(uint32_t pdcp_sn) override
  {
    // not implemented
  }

  byte_buffer get_pdcp_pdu() { return std::move(pdcp_pdu); }

private:
  byte_buffer pdcp_pdu;
  bool        pdcp_pdu_is_retx;
};

/// Adapter between PDCP Tx control and RRC
class pdcp_tx_control_rrc_ue_adapter : public pdcp_tx_upper_control_notifier
{
public:
  pdcp_tx_control_rrc_ue_adapter() = default;

  void on_protocol_failure() override
  {
    ocudulog::fetch_basic_logger("PDCP").warning(
        "Requesting UE release. Cause: Received protocol failure from PDCP Tx");
    cause = cause_protocol_t::unspecified;
  }

  void on_max_count_reached() override
  {
    ocudulog::fetch_basic_logger("PDCP").warning("Requesting UE release. Cause: Max count reached from PDCP Tx");
    cause = cause_protocol_t::unspecified;
  }

  void on_resume_required() override
  {
    ocudulog::fetch_basic_logger("PDCP").error("Unsupported request for SRB resume from PDCP Tx");
  }

  ngap_cause_t get_failure_cause() { return cause; }

private:
  ngap_cause_t cause;
};

} // namespace ocucp
} // namespace ocudu
