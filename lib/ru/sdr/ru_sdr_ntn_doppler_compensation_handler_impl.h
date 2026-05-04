// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/ntn/ntn_doppler_compensation_handler.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ru/ru_controller.h"

namespace ocudu {

/// RU-side implementation of NTN Doppler compensation handler.
/// This class handles NTN Doppler compensation requests by applying them to the RU controller.
class ru_sdr_ntn_doppler_compensation_handler_impl : public ocudu_ntn::ntn_doppler_compensation_handler
{
public:
  explicit ru_sdr_ntn_doppler_compensation_handler_impl(ru_controller& ru_ctrl);

  /// \brief Handle DL Doppler compensation request.
  ///
  /// \param dl_request Pre-calculated DL Doppler compensation request containing time, frequency, and drift values.
  /// \return True if the request was successfully handled; false otherwise.
  bool handle_dl_doppler_compensation(const ocudu_ntn::doppler_compensation_request& dl_request) override;

  /// \brief Handle UL Doppler compensation request.
  ///
  /// \param ul_request Pre-calculated UL Doppler compensation request containing time, frequency, and drift values.
  /// \return True if the request was successfully handled; false otherwise.
  bool handle_ul_doppler_compensation(const ocudu_ntn::doppler_compensation_request& ul_request) override;

private:
  ocudulog::basic_logger& logger;
  ru_controller&          ru_ctrl;
};

} // namespace ocudu
