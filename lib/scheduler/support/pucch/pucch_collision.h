// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/ran/pucch/pucch_configuration.h"

namespace ocudu {

/// Checks whether two PUCCH resources collide, i.e., they interfere with each other if allocated in the same slot.
bool pucch_resources_collide(const pucch_resource& res1, const pucch_resource& res2);

} // namespace ocudu
