// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "isac_pusch_estimator_tap_factory.h"
#include "isac_pusch_estimator_tap_decorator.h"
#include "isac_pusch_processor_tap_decorator.h"
#include "isac_zmq_sink.h"
#include <cstdlib>
#include <mutex>
#include <string>

using namespace ocudu;
using namespace ocudu::isac;

namespace {

/// Returns the configured tap endpoint, or nullptr when the tap is disabled.
const char* tap_endpoint()
{
  const char* endpoint = std::getenv("OCUDU_ISAC_ZMQ_ENDPOINT");
  return (endpoint != nullptr && endpoint[0] != '\0') ? endpoint : nullptr;
}

/// \brief Returns the process-wide sink, creating it on first success.
///
/// The sink holds a live thread and a ZMQ context, so it is intentionally leaked: destroying it
/// during post-main static destruction would join the sender thread and terminate the ZMQ
/// context at a point PHY teardown cannot sequence. A failed bind is NOT cached — the next call
/// retries, so a transiently busy port does not permanently disable the tap.
std::shared_ptr<isac_csi_sink> acquire_sink(const char* endpoint)
{
  static std::mutex mtx;
  // Never destroyed by design (see above); the OS reclaims it at process exit.
  static std::shared_ptr<isac_zmq_sink>* slot = new std::shared_ptr<isac_zmq_sink>();

  std::lock_guard<std::mutex> lock(mtx);
  if (!*slot) {
    auto sink = std::make_shared<isac_zmq_sink>(std::string(endpoint));
    if (!sink->is_ready()) {
      return nullptr;
    }
    *slot = std::move(sink);
  }
  return *slot;
}

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

/// Factory decorator: each created PUSCH processor is wrapped in the RNTI-stamping tap.
class isac_tap_processor_factory : public pusch_processor_factory
{
public:
  explicit isac_tap_processor_factory(std::shared_ptr<pusch_processor_factory> base_) : base(std::move(base_)) {}

  std::unique_ptr<pusch_processor> create() override
  {
    return std::make_unique<isac_pusch_processor_tap_decorator>(base->create());
  }

  std::unique_ptr<pusch_processor> create(ocudulog::basic_logger& logger) override
  {
    return std::make_unique<isac_pusch_processor_tap_decorator>(base->create(logger));
  }

  std::unique_ptr<pusch_pdu_validator> create_validator() override { return base->create_validator(); }

private:
  std::shared_ptr<pusch_processor_factory> base;
};

} // namespace

std::shared_ptr<dmrs_pusch_estimator_factory>
ocudu::isac::maybe_wrap_pusch_estimator_factory(std::shared_ptr<dmrs_pusch_estimator_factory> base)
{
  const char* endpoint = tap_endpoint();
  if (endpoint == nullptr) {
    // Tap disabled: return the factory unchanged. No socket, no thread, no behavior change.
    return base;
  }

  std::shared_ptr<isac_csi_sink> sink = acquire_sink(endpoint);
  if (!sink) {
    // Bind failed (logged by the sink); leave the pipeline untouched. A later init may succeed.
    return base;
  }

  return std::make_shared<isac_tap_estimator_factory>(std::move(base), std::move(sink));
}

std::shared_ptr<pusch_processor_factory>
ocudu::isac::maybe_wrap_pusch_processor_factory(std::shared_ptr<pusch_processor_factory> base)
{
  if (tap_endpoint() == nullptr) {
    return base;
  }
  return std::make_shared<isac_tap_processor_factory>(std::move(base));
}
