// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief ISAC sensing PoC (Block A) — thread-local PUSCH PDU context bridge.
///
/// The DM-RS estimator interfaces carry no UE identity, so the RNTI is bridged from the PUSCH
/// processor (which sees the PDU) to the estimator tap via a thread-local slot. This is valid
/// because pusch_processor_impl::process() invokes the estimator's estimate() synchronously on
/// the same thread; the scoped guard brackets exactly that window.

#pragma once

#include <cstdint>
#include <optional>

namespace ocudu {
namespace isac {

/// Thread-local slot holding the RNTI of the PUSCH PDU currently being processed on this thread.
inline std::optional<uint16_t>& thread_tap_rnti()
{
  thread_local std::optional<uint16_t> rnti;
  return rnti;
}

/// RAII guard that publishes an RNTI in the thread-local slot for the duration of a scope.
class scoped_tap_rnti
{
public:
  explicit scoped_tap_rnti(uint16_t rnti) : previous(thread_tap_rnti()) { thread_tap_rnti() = rnti; }
  ~scoped_tap_rnti() { thread_tap_rnti() = previous; }

  scoped_tap_rnti(const scoped_tap_rnti&)            = delete;
  scoped_tap_rnti& operator=(const scoped_tap_rnti&) = delete;

private:
  std::optional<uint16_t> previous;
};

} // namespace isac
} // namespace ocudu
