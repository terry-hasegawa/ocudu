// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ru_sdr_ntn_doppler_compensation_handler_impl.h"
#include "fmt/chrono.h"

using namespace ocudu;

ru_sdr_ntn_doppler_compensation_handler_impl::ru_sdr_ntn_doppler_compensation_handler_impl(ru_controller& ru_ctrl_) :
  logger(ocudulog::fetch_basic_logger("RU")), ru_ctrl(ru_ctrl_)
{
}

bool ru_sdr_ntn_doppler_compensation_handler_impl::handle_dl_doppler_compensation(
    const ocudu_ntn::doppler_compensation_request& request)
{
  if (not ru_ctrl.get_cfo_controller()) {
    logger.warning("NTN: CFO controller not available, cannot apply DL Doppler compensation");
    return false;
  }

  cfo_compensation_request cfo_reqs;
  cfo_reqs.cfo_hz          = request.cfo_hz;
  cfo_reqs.cfo_drift_hz_s  = request.cfo_drift_hz_s;
  cfo_reqs.start_timestamp = request.start_timestamp;

  // Apply the pre-calculated DL Doppler compensation values to TX.
  ru_ctrl.get_cfo_controller()->set_tx_cfo(request.sector_id, cfo_reqs);

  logger.debug("NTN: Apply DL Doppler compensation: {:.1f} Hz (drift: {:.1f} Hz/s) at {:%T}",
               cfo_reqs.cfo_hz,
               cfo_reqs.cfo_drift_hz_s,
               cfo_reqs.start_timestamp.value());

  return true;
}

bool ru_sdr_ntn_doppler_compensation_handler_impl::handle_ul_doppler_compensation(
    const ocudu_ntn::doppler_compensation_request& request)
{
  if (not ru_ctrl.get_cfo_controller()) {
    logger.warning("NTN: CFO controller not available, cannot apply UL Doppler compensation");
    return false;
  }

  cfo_compensation_request cfo_reqs;
  cfo_reqs.cfo_hz          = request.cfo_hz;
  cfo_reqs.cfo_drift_hz_s  = request.cfo_drift_hz_s;
  cfo_reqs.start_timestamp = request.start_timestamp;

  // Apply the pre-calculated UL Doppler compensation values to RX.
  ru_ctrl.get_cfo_controller()->set_rx_cfo(request.sector_id, cfo_reqs);

  logger.debug("NTN: Apply UL Doppler compensation: {:.1f} Hz (drift: {:.1f} Hz/s) at {:%T}",
               cfo_reqs.cfo_hz,
               cfo_reqs.cfo_drift_hz_s,
               cfo_reqs.start_timestamp.value());

  return true;
}
