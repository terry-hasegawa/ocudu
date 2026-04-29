// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/du/du_high/du_test_mode_config.h"
#include "ocudu/mac/mac.h"
#include "ocudu/mac/mac_config.h"

namespace ocudu::odu {

class du_test_mode_controller;

/// \brief Create a MAC instance for DU-high. In case test mode is enabled, the MAC messages will be intercepted.
/// When \p ctrl is non-null and attach/detach cycling is configured, the controller is wired for UE lifecycle.
std::unique_ptr<mac_interface> create_du_high_mac(const mac_config&               mac_cfg,
                                                  const odu::du_test_mode_config& test_cfg,
                                                  unsigned                        nof_cells,
                                                  du_test_mode_controller*        ctrl = nullptr);

} // namespace ocudu::odu
