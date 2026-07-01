// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief ISAC sensing PoC (Block A) — factory wiring entry points.

#pragma once

#include "ocudu/phy/upper/channel_processors/pusch/factories.h"
#include "ocudu/phy/upper/signal_processors/pusch/factories.h"
#include <memory>

namespace ocudu {
namespace isac {

/// \brief Conditionally wraps the PUSCH estimator factory with the ISAC read-only tap.
///
/// Gated at runtime by the environment variable \c OCUDU_ISAC_ZMQ_ENDPOINT:
///   - if unset/empty, returns \c base unchanged (zero behavior change, no socket, no thread);
///   - otherwise binds a process-wide ZeroMQ PUB sink and returns a wrapping factory whose
///     created estimators tap H read-only and publish one snapshot per PUSCH PDU.
///
/// The sink is created once and intentionally leaked (never destroyed), so no ZMQ teardown runs
/// during post-main static destruction. If the bind fails, nothing is cached and the next call
/// retries, so a transiently busy port does not permanently disable the tap.
///
/// Mirrors create_pusch_channel_estimator_metric_decorator_factory() in placement and shape.
std::shared_ptr<dmrs_pusch_estimator_factory>
maybe_wrap_pusch_estimator_factory(std::shared_ptr<dmrs_pusch_estimator_factory> base);

/// \brief Conditionally wraps a PUSCH processor factory with the RNTI-stamping tap decorator.
///
/// Same gating as maybe_wrap_pusch_estimator_factory(). The wrapper is a pure pass-through that
/// publishes each PDU's RNTI in a thread-local slot (see isac_tap_context.h) so the estimator
/// tap can attach a UE identity to every CSI snapshot.
std::shared_ptr<pusch_processor_factory>
maybe_wrap_pusch_processor_factory(std::shared_ptr<pusch_processor_factory> base);

} // namespace isac
} // namespace ocudu
