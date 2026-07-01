// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief ISAC sensing PoC (Block A) — abstract CSI sink.
///
/// A sink receives one per-PDU CSI snapshot from the PHY tap. Implementations MUST be
/// non-blocking and read-only with respect to the estimator results (the call happens on a
/// PHY worker thread inside on_estimation_complete).

#pragma once

#include "ocudu/phy/upper/signal_processors/pusch/dmrs_pusch_estimator.h"
#include <cstdint>
#include <optional>

namespace ocudu {
namespace isac {

/// Abstract destination for tapped UL DMRS channel estimates.
class isac_csi_sink
{
public:
  virtual ~isac_csi_sink() = default;

  /// \brief Publishes one CSI snapshot.
  ///
  /// Called from a PHY worker thread. The implementation must not block the caller and must
  /// only read \c config / \c results. \c results is valid only for the duration of the call.
  /// \c rnti identifies the UE of the tapped PUSCH PDU when available.
  virtual void publish(const dmrs_pusch_estimator::configuration& config,
                       const dmrs_pusch_estimator_results&        results,
                       std::optional<uint16_t>                    rnti) noexcept = 0;
};

} // namespace isac
} // namespace ocudu
