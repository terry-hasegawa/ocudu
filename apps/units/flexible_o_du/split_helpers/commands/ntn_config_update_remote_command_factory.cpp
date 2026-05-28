// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ntn_config_update_remote_command_factory.h"
#include "apps/units/flexible_o_du/o_du_unit.h"
#include "ntn_config_update_remote_command.h"

using namespace ocudu;
using namespace ocudu_ntn;

void ocudu::add_ntn_config_update_remote_command(application_unit_commands& commands,
                                                 ntn_configuration_manager& ntn_manager)
{
  commands.remote.push_back(std::make_unique<ntn_config_update_remote_command>(ntn_manager));
}
