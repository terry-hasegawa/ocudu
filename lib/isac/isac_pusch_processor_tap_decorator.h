// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief ISAC sensing PoC (Block A) — PUSCH processor decorator that stamps the PDU RNTI.
///
/// Pure pass-through except for publishing pdu.rnti in the thread-local tap context while the
/// base process() runs. The estimator tap decorator reads it there (estimate() is invoked
/// synchronously inside process() on the same thread), so CSI snapshots can carry a UE identity
/// without modifying any existing class.

#pragma once

#include "isac_tap_context.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_processor.h"
#include "ocudu/phy/upper/unique_rx_buffer.h"
#include <memory>

namespace ocudu {
namespace isac {

/// Decorator around a pusch_processor that exposes the current PDU RNTI to the estimator tap.
class isac_pusch_processor_tap_decorator : public pusch_processor
{
public:
  explicit isac_pusch_processor_tap_decorator(std::unique_ptr<pusch_processor> base_) : base(std::move(base_)) {}

  // See pusch_processor for documentation.
  void process(span<uint8_t>                    data,
               unique_rx_buffer                 rm_buffer,
               pusch_processor_result_notifier& notifier,
               const resource_grid_reader&      grid,
               const pdu_t&                     pdu) override
  {
    // The guard covers the synchronous part of process(), which includes the call into the
    // (tap-decorated) estimator where the RNTI is captured.
    scoped_tap_rnti guard(pdu.rnti);
    base->process(data, std::move(rm_buffer), notifier, grid, pdu);
  }

private:
  std::unique_ptr<pusch_processor> base;
};

} // namespace isac
} // namespace ocudu
