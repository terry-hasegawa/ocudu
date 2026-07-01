// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief ISAC sensing PoC (Block A) — read-only PHY tap decorator for the PUSCH channel estimator.
///
/// External shape mirrors phy_metrics_pusch_channel_estimator_decorator, but adds the net-new
/// piece that the metrics decorator lacks: a notifier shim that intercepts on_estimation_complete
/// to read the channel estimate (H), forwards it unchanged to the real notifier, and never alters
/// the estimation path. Existing classes are untouched. The UE identity (RNTI) is read from the
/// thread-local tap context stamped by isac_pusch_processor_tap_decorator.

#pragma once

#include "isac_csi_sink.h"
#include "isac_tap_context.h"
#include "ocudu/phy/upper/signal_processors/pusch/dmrs_pusch_estimator.h"
#include <memory>
#include <utility>

namespace ocudu {
namespace isac {

/// Decorator around a dmrs_pusch_estimator that taps the channel estimate read-only.
class isac_pusch_estimator_tap_decorator : public dmrs_pusch_estimator
{
  /// \brief Self-owning notifier shim.
  ///
  /// The base estimator runs per-port estimation asynchronously (task executor) and invokes
  /// on_estimation_complete exactly once, possibly after estimate() has returned. The shim is
  /// therefore heap-allocated and deletes itself when the completion fires, so it survives the
  /// async path while remaining safe for the synchronous (inline) path.
  /// Known limitation (accepted for the PoC): if the executor discards a queued estimation task
  /// during teardown, the completion never fires and the shim leaks.
  class notifier_shim : public dmrs_pusch_estimator_notifier
  {
  public:
    notifier_shim(dmrs_pusch_estimator_notifier&      real_notifier_,
                  dmrs_pusch_estimator::configuration config_,
                  std::optional<uint16_t>             rnti_,
                  isac_csi_sink&                      sink_) :
      real_notifier(real_notifier_), config(std::move(config_)), rnti(rnti_), sink(sink_)
    {
    }

    void on_estimation_complete(const dmrs_pusch_estimator_results& results) override
    {
      // Take ownership so the shim is freed even if forwarding throws.
      std::unique_ptr<notifier_shim> self(this);

      // Read-only tap BEFORE forwarding: the results object is only guaranteed valid for the
      // duration of this callback. publish() is non-blocking and noexcept.
      sink.publish(config, results, rnti);

      // Always forward to preserve the original estimation -> demodulation -> decode path.
      real_notifier.on_estimation_complete(results);
    }

  private:
    dmrs_pusch_estimator_notifier&      real_notifier;
    dmrs_pusch_estimator::configuration config; ///< Copied: metadata must outlive async completion.
    std::optional<uint16_t>             rnti;   ///< UE identity captured at estimate() time.
    isac_csi_sink&                      sink;
  };

public:
  isac_pusch_estimator_tap_decorator(std::unique_ptr<dmrs_pusch_estimator> base_,
                                     std::shared_ptr<isac_csi_sink>        sink_) :
    base(std::move(base_)), sink(std::move(sink_))
  {
  }

  // See dmrs_pusch_estimator for documentation.
  void estimate(dmrs_pusch_estimator_notifier& notifier,
                const resource_grid_reader&    grid,
                const configuration&           config) override
  {
    // estimate() runs synchronously inside pusch_processor_impl::process(), so the thread-local
    // RNTI stamped by the processor tap decorator identifies this PDU's UE.
    // The shim outlives this call (async completion) and self-deletes on completion.
    auto* shim = new notifier_shim(notifier, config, thread_tap_rnti(), *sink);
    base->estimate(*shim, grid, config);
  }

private:
  std::unique_ptr<dmrs_pusch_estimator> base;
  std::shared_ptr<isac_csi_sink>        sink;
};

} // namespace isac
} // namespace ocudu
