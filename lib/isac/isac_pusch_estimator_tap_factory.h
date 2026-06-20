// SPDX-FileCopyrightText: 2026 OCUDU ISAC sensing PoC
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief ISAC sensing PoC (Block A) — factory wiring entry point.

#pragma once

#include "ocudu/phy/upper/signal_processors/pusch/factories.h"
#include <memory>

namespace ocudu {
namespace isac {

/// \brief Conditionally wraps the PUSCH estimator factory with the ISAC read-only tap.
///
/// Gated at runtime by the environment variable \c OCUDU_ISAC_ZMQ_ENDPOINT:
///   - if unset/empty, returns \c base unchanged (zero behavior change, no socket, no thread);
///   - otherwise binds a process-wide ZeroMQ PUB sink once and returns a wrapping factory whose
///     created estimators tap H read-only and publish one snapshot per PUSCH PDU.
///
/// Mirrors create_pusch_channel_estimator_metric_decorator_factory() in placement and shape.
std::shared_ptr<dmrs_pusch_estimator_factory>
maybe_wrap_pusch_estimator_factory(std::shared_ptr<dmrs_pusch_estimator_factory> base);

} // namespace isac
} // namespace ocudu
