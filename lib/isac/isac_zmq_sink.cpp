// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "isac_zmq_sink.h"
#include "isac_csi_payload.h"
#include "fmt/format.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <zmq.h>

using namespace ocudu;
using namespace ocudu::isac;

namespace {

std::int64_t now_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

} // namespace

isac_zmq_sink::isac_zmq_sink(const std::string& endpoint, unsigned queue_depth) :
  max_depth(queue_depth == 0 ? 1U : queue_depth), start_ns(now_ns())
{
  zmq_ctx = zmq_ctx_new();
  if (zmq_ctx == nullptr) {
    fmt::print(stderr, "[isac] failed to create ZMQ context; CSI tap disabled.\n");
    return;
  }

  zmq_sock = zmq_socket(zmq_ctx, ZMQ_PUB);
  if (zmq_sock == nullptr) {
    fmt::print(stderr, "[isac] failed to create ZMQ PUB socket; CSI tap disabled.\n");
    zmq_ctx_term(zmq_ctx);
    zmq_ctx = nullptr;
    return;
  }

  // Bounded send buffer and no lingering on close: we favour dropping over blocking.
  int sndhwm = static_cast<int>(max_depth);
  int linger = 0;
  zmq_setsockopt(zmq_sock, ZMQ_SNDHWM, &sndhwm, sizeof(sndhwm));
  zmq_setsockopt(zmq_sock, ZMQ_LINGER, &linger, sizeof(linger));

  if (zmq_bind(zmq_sock, endpoint.c_str()) != 0) {
    fmt::print(stderr, "[isac] failed to bind ZMQ PUB socket to '{}': {}. CSI tap disabled.\n", endpoint,
               zmq_strerror(zmq_errno()));
    zmq_close(zmq_sock);
    zmq_sock = nullptr;
    zmq_ctx_term(zmq_ctx);
    zmq_ctx = nullptr;
    return;
  }

  ready   = true;
  running = true;
  sender  = std::thread([this]() { run(); });
  fmt::print(stderr, "[isac] CSI tap publishing on '{}' (queue_depth={}).\n", endpoint, max_depth);
}

isac_zmq_sink::~isac_zmq_sink()
{
  {
    std::lock_guard<std::mutex> lk(mtx);
    running = false;
  }
  cv.notify_all();
  if (sender.joinable()) {
    sender.join();
  }
  if (zmq_sock != nullptr) {
    zmq_close(zmq_sock);
  }
  if (zmq_ctx != nullptr) {
    zmq_ctx_term(zmq_ctx);
  }
}

void isac_zmq_sink::publish(const dmrs_pusch_estimator::configuration& config,
                            const dmrs_pusch_estimator_results&        results,
                            std::optional<uint16_t>                    rnti) noexcept
{
  if (!ready) {
    return;
  }

  // Gate BEFORE serializing so a full queue costs nothing but this try_lock. A failed try_lock
  // (momentary contention) also drops; that path cannot bump next_seq, so it is invisible to the
  // subscriber's gap accounting — accepted as rare.
  std::vector<uint8_t> msg;
  {
    std::unique_lock<std::mutex> lk(mtx, std::try_to_lock);
    if (!lk.owns_lock() || !running) {
      return;
    }
    if (queue.size() >= max_depth) {
      // Consume a sequence number for the dropped snapshot so the subscriber sees a gap.
      ++next_seq;
      return;
    }
    if (!free_bufs.empty()) {
      msg = std::move(free_bufs.back());
      free_bufs.pop_back();
    }
  }

  // Serialize outside the lock (bounded copy; reads the estimate read-only). The seq stamped
  // here is provisional; the real one is patched in at enqueue time below.
  serialize_csi(config, results, rnti, /*seq=*/0, static_cast<uint64_t>(now_ns() - start_ns), msg);
  if (msg.empty()) {
    return;
  }

  // Enqueue without ever blocking the PHY thread; assign the sequence number under the same lock
  // so the stream is monotonic in queue order even with concurrent publishers.
  {
    std::unique_lock<std::mutex> lk(mtx, std::try_to_lock);
    if (!lk.owns_lock() || !running) {
      return;
    }
    if (queue.size() >= max_depth) {
      ++next_seq;
      free_bufs.push_back(std::move(msg));
      return;
    }
    const uint32_t seq = next_seq++;
    std::memcpy(msg.data() + ISAC_CSI_SEQ_OFFSET, &seq, sizeof(seq));
    try {
      queue.push_back(std::move(msg));
    } catch (...) {
      // Allocation failure while growing the queue: drop instead of terminating (publish() is
      // noexcept and must never take the gNB down).
      return;
    }
  }
  cv.notify_one();
}

void isac_zmq_sink::run()
{
  for (;;) {
    std::vector<uint8_t> msg;
    {
      std::unique_lock<std::mutex> lk(mtx);
      cv.wait(lk, [this]() { return !running || !queue.empty(); });
      if (!running && queue.empty()) {
        return;
      }
      msg = std::move(queue.front());
      queue.pop_front();
    }
    // Non-blocking send. With ZMQ_PUB this drops if no subscriber / HWM reached.
    (void)zmq_send(zmq_sock, msg.data(), msg.size(), ZMQ_DONTWAIT);

    // Recycle the buffer (capacity retained) so steady state does no allocations.
    std::lock_guard<std::mutex> lk(mtx);
    if (free_bufs.size() < max_depth) {
      free_bufs.push_back(std::move(msg));
    }
  }
}
