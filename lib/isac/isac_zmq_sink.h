// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief ISAC sensing PoC (Block A) — ZeroMQ PUB sink.
///
/// Serializes each CSI snapshot on the calling PHY thread (a bounded copy into a recycled
/// buffer), then hands the bytes to a dedicated sender thread via a bounded queue. The PHY
/// thread never performs ZMQ I/O and never blocks: the queue depth is checked BEFORE
/// serializing, and if the queue is full or momentarily contended the snapshot is dropped.
/// Sequence numbers are assigned under the queue lock at enqueue time, so the stream is
/// monotonic in queue order and a gap always means a dropped snapshot. The sender thread owns
/// the (non-thread-safe) ZMQ socket exclusively and returns spent buffers to a free list.

#pragma once

#include "isac_csi_sink.h"
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ocudu {
namespace isac {

/// ZeroMQ PUB sink. Publishes per-PDU CSI snapshots; drops when no subscriber / queue full.
class isac_zmq_sink : public isac_csi_sink
{
public:
  /// \brief Creates the sink and binds a PUB socket to \c endpoint (e.g. "tcp://*:5599").
  /// \param[in] endpoint   ZeroMQ endpoint to bind.
  /// \param[in] queue_depth Maximum number of pending messages before producers drop.
  explicit isac_zmq_sink(const std::string& endpoint, unsigned queue_depth = 16);

  ~isac_zmq_sink() override;

  isac_zmq_sink(const isac_zmq_sink&)            = delete;
  isac_zmq_sink& operator=(const isac_zmq_sink&) = delete;

  /// Returns true if the PUB socket bound successfully and the sender thread is running.
  bool is_ready() const { return ready; }

  // See isac_csi_sink for documentation.
  void publish(const dmrs_pusch_estimator::configuration& config,
               const dmrs_pusch_estimator_results&        results,
               std::optional<uint16_t>                    rnti) noexcept override;

private:
  /// Sender-thread main loop: pops serialized messages and sends them (non-blocking).
  void run();

  void*              zmq_ctx  = nullptr;
  void*              zmq_sock = nullptr;
  bool               ready    = false;
  const unsigned     max_depth;
  const std::int64_t start_ns;

  std::mutex                       mtx;
  std::condition_variable          cv;
  std::deque<std::vector<uint8_t>> queue;     ///< Pending serialized messages (guarded by mtx).
  std::deque<std::vector<uint8_t>> free_bufs; ///< Recycled buffers (guarded by mtx).
  uint32_t                         next_seq = 0; ///< Next sequence number (guarded by mtx).
  bool                             running  = false;
  std::thread                      sender;
};

} // namespace isac
} // namespace ocudu
