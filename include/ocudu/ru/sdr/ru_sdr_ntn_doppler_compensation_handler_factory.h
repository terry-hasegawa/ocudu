// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/ntn/ntn_doppler_compensation_handler.h"
#include <memory>

namespace ocudu {

class ru_controller;

/// \brief Create RU SDR NTN Doppler Compensation handler.
///
/// \param ru_ctrl RU controller interface.
/// \return NTN Doppler compensation handler.
std::unique_ptr<ocudu_ntn::ntn_doppler_compensation_handler>
create_ru_sdr_ntn_doppler_compensation_handler(ru_controller& ru_ctrl);

} // namespace ocudu
