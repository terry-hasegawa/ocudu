// SPDX-FileCopyrightText: 2026 OCUDU ISAC sensing PoC
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "isac_pusch_estimator_tap_factory.h"
#include "isac_pusch_estimator_tap_decorator.h"
#include "isac_zmq_sink.h"
#include <cstdlib>
#include <string>

using namespace ocudu;
using namespace ocudu::isac;

namespace {

/// Factory decorator: each created estimator is wrapped in the ISAC read-only tap.
/// Same shape as metric_decorator_factory in phy_metrics_factories.cpp.
class isac_tap_estimator_factory : public dmrs_pusch_estimator_factory
{
public:
  isac_tap_estimator_factory(std::shared_ptr<dmrs_pusch_estimator_factory> base_,
                             std::shared_ptr<isac_csi_sink>                sink_) :
    base(std::move(base_)), sink(std::move(sink_))
  {
  }

  std::unique_ptr<dmrs_pusch_estimator> create() override
  {
    return std::make_unique<isac_pusch_estimator_tap_decorator>(base->create(), sink);
  }

private:
  std::shared_ptr<dmrs_pusch_estimator_factory> base;
  std::shared_ptr<isac_csi_sink>                sink;
};

} // namespace

std::shared_ptr<dmrs_pusch_estimator_factory>
ocudu::isac::maybe_wrap_pusch_estimator_factory(std::shared_ptr<dmrs_pusch_estimator_factory> base)
{
  const char* endpoint = std::getenv("OCUDU_ISAC_ZMQ_ENDPOINT");
  if (endpoint == nullptr || endpoint[0] == '\0') {
    // Tap disabled: return the factory unchanged. No socket, no thread, no behavior change.
    return base;
  }

  // Process-wide sink, created exactly once on the first enabled call (thread-safe init).
  static std::shared_ptr<isac_csi_sink> sink = std::make_shared<isac_zmq_sink>(std::string(endpoint));

  return std::make_shared<isac_tap_estimator_factory>(std::move(base), sink);
}
