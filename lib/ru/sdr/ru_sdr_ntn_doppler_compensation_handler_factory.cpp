// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ocudu/ru/sdr/ru_sdr_ntn_doppler_compensation_handler_factory.h"
#include "ru_sdr_ntn_doppler_compensation_handler_impl.h"

using namespace ocudu;

std::unique_ptr<ocudu::ocudu_ntn::ntn_doppler_compensation_handler>
ocudu::create_ru_sdr_ntn_doppler_compensation_handler(ru_controller& ru_ctrl)
{
  auto doppler_handler = std::make_unique<ru_sdr_ntn_doppler_compensation_handler_impl>(ru_ctrl);
  return doppler_handler;
}
