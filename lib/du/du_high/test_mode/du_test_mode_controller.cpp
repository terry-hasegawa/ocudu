// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "du_test_mode_controller.h"
#include "ocudu/adt/byte_buffer.h"
#include "ocudu/asn1/f1ap/common.h"
#include "ocudu/asn1/f1ap/f1ap.h"
#include "ocudu/asn1/f1ap/f1ap_pdu_contents_ue.h"
#include "ocudu/f1ap/du/f1ap_du.h"
#include "ocudu/f1ap/f1ap_message.h"
#include "ocudu/mac/mac_pdu_handler.h"
#include "ocudu/ran/logical_channel/lcid.h"
#include "ocudu/scheduler/result/sched_result.h"
#include "ocudu/support/executors/task_executor.h"
#include <atomic>

using namespace ocudu;
using namespace odu;

static constexpr std::chrono::milliseconds GUARD_PERIOD_MSEC{1000};

// ---- f1c_wrapper_impl ----

/// Intercepts outgoing F1AP PDUs (DU → CU) to capture gnb_du_ue_f1ap_id for test mode UEs.
class du_test_mode_controller::f1c_wrapper_impl : public f1c_connection_client
{
public:
  explicit f1c_wrapper_impl(du_test_mode_controller& parent_) : parent(parent_) {}

  void set_upstream(f1c_connection_client& upstream_) { upstream = &upstream_; }

  std::unique_ptr<f1ap_message_notifier>
  handle_du_connection_request(std::unique_ptr<f1ap_message_notifier> du_rx_pdu_notifier) override
  {
    ocudu_assert(upstream != nullptr, "f1c upstream not set");
    auto upstream_tx = upstream->handle_du_connection_request(std::move(du_rx_pdu_notifier));
    if (upstream_tx == nullptr) {
      return nullptr;
    }
    return std::make_unique<tx_interceptor>(parent, std::move(upstream_tx));
  }

private:
  class tx_interceptor : public f1ap_message_notifier
  {
  public:
    tx_interceptor(du_test_mode_controller& parent_, std::unique_ptr<f1ap_message_notifier> upstream_tx_) :
      parent(parent_), upstream_tx(std::move(upstream_tx_))
    {
    }

    void on_new_message(const f1ap_message& msg) override
    {
      using namespace asn1::f1ap;

      if (msg.pdu.type().value == f1ap_pdu_c::types_opts::init_msg and
          msg.pdu.init_msg().value.type().value ==
              f1ap_elem_procs_o::init_msg_c::types_opts::init_ul_rrc_msg_transfer) {
        const auto& ie   = msg.pdu.init_msg().value.init_ul_rrc_msg_transfer();
        rnti_t      rnti = to_rnti(ie->c_rnti);

        if (ie->du_to_cu_rrc_container_present) {
          for (unsigned c = 0; c < parent.cells.size(); ++c) {
            if (parent.is_test_ue_in_cell(static_cast<du_cell_index_t>(c), rnti)) {
              parent.on_ue_f1ap_id_captured(rnti, int_to_gnb_du_ue_f1ap_id(ie->gnb_du_ue_f1ap_id));
              break;
            }
          }
        }
      }

      upstream_tx->on_new_message(msg);
    }

  private:
    du_test_mode_controller&               parent;
    std::unique_ptr<f1ap_message_notifier> upstream_tx;
  };

  du_test_mode_controller& parent;
  f1c_connection_client*   upstream = nullptr;
};

// ---- cell_notifier_impl ----

/// Wraps a mac_cell_result_notifier to intercept Msg4 events and slot timing for the cycling controller.
class du_test_mode_controller::cell_notifier_impl : public mac_cell_result_notifier
{
public:
  cell_notifier_impl(du_test_mode_controller&  parent_,
                     du_cell_index_t           cell_index_,
                     mac_cell_result_notifier& real_phy_,
                     unsigned                  nof_ues_) :
    parent(parent_), cell_index(cell_index_), real_phy(real_phy_), msg4_dispatched(nof_ues_, false)
  {
  }

  void on_new_downlink_scheduler_results(const mac_dl_sched_result& dl_res) override
  {
    // Check if a reset was requested by the controller (new cycle starting).
    if (reset_requested.exchange(false)) {
      std::fill(msg4_dispatched.begin(), msg4_dispatched.end(), false);
    }

    // Scan for first SRB grant per test UE RNTI — signals Msg4 reception.
    for (const auto& grant : dl_res.dl_res->ue_grants) {
      const rnti_t crnti = grant.pdsch_cfg.rnti;
      if (not parent.is_test_ue_in_cell(cell_index, crnti)) {
        continue;
      }
      const unsigned base      = to_value(parent.cfg.rnti) + static_cast<unsigned>(cell_index) * parent.cfg.nof_ues;
      const unsigned ue_offset = to_value(crnti) - base;
      if (msg4_dispatched[ue_offset]) {
        continue;
      }
      const auto& lchs = grant.tb_list[0].lc_chs_to_sched;
      if (std::any_of(lchs.begin(), lchs.end(), [](const auto& lc) {
            return lc.lcid.is_sdu() and is_srb(lc.lcid.to_lcid());
          })) {
        msg4_dispatched[ue_offset] = true;
        parent.on_msg4_received(cell_index, crnti);
      }
    }

    real_phy.on_new_downlink_scheduler_results(dl_res);
  }

  void on_new_downlink_data(const mac_dl_data_result& dl_data) override { real_phy.on_new_downlink_data(dl_data); }

  void on_new_uplink_scheduler_results(const mac_ul_sched_result& ul_res) override
  {
    real_phy.on_new_uplink_scheduler_results(ul_res);
  }

  void on_cell_results_completion(slot_point slot) override
  {
    real_phy.on_cell_results_completion(slot);
    parent.on_slot_completed(cell_index, slot);
  }

  void request_msg4_reset() { reset_requested.store(true); }

private:
  du_test_mode_controller&  parent;
  du_cell_index_t           cell_index;
  mac_cell_result_notifier& real_phy;
  std::vector<bool>         msg4_dispatched; // on cell thread; guards redundant ctrl_exec dispatches
  std::atomic<bool>         reset_requested{false};
};

// ---- du_test_mode_controller ----

du_test_mode_controller::du_test_mode_controller(const du_test_mode_config::test_mode_ue_config& cfg_,
                                                 timer_manager&                                  timers_,
                                                 task_executor&                                  ctrl_exec_,
                                                 unsigned                                        nof_cells_) :
  cfg(cfg_),
  ctrl_exec(ctrl_exec_),
  logger(ocudulog::fetch_basic_logger("DU")),
  cells(nof_cells_),
  f1c_wrapper(std::make_unique<f1c_wrapper_impl>(*this))
{
  for (unsigned i = 0, sz = nof_cells_; i != sz; ++i) {
    auto& cell = cells[i];
    if (cfg.attach_detach_duration.has_value()) {
      // Setup attach-detach cycle timer.
      cell.attach_detach_timer = timers_.create_unique_timer(ctrl_exec_);
      cell.attach_detach_timer.set(*cfg.attach_detach_duration, [this, i](timer_id_t /* unused */) {
        this->handle_attach_detach_timer(to_du_cell_index(i));
      });
    }
    cell.msg4_counted.assign(cfg.nof_ues, false);
  }
}

du_test_mode_controller::~du_test_mode_controller() = default;

void du_test_mode_controller::set_f1c_upstream(f1c_connection_client& upstream)
{
  f1c_wrapper->set_upstream(upstream);
}

f1c_connection_client& du_test_mode_controller::get_f1c_wrapper()
{
  return *f1c_wrapper;
}

mac_cell_result_notifier& du_test_mode_controller::add_cell_notifier(du_cell_index_t           cell_index,
                                                                     mac_cell_result_notifier& real_phy_notifier)
{
  if (cell_notifiers.size() <= static_cast<unsigned>(cell_index)) {
    cell_notifiers.resize(static_cast<unsigned>(cell_index) + 1);
  }
  cell_notifiers[cell_index] = std::make_unique<cell_notifier_impl>(*this, cell_index, real_phy_notifier, cfg.nof_ues);
  return *cell_notifiers[cell_index];
}

void du_test_mode_controller::connect(mac_pdu_handler& pdu_handler_, f1ap_du& f1ap_)
{
  pdu_handler  = &pdu_handler_;
  f1ap_handler = &f1ap_;
}

void du_test_mode_controller::on_msg4_received(du_cell_index_t cell_index, rnti_t rnti)
{
  if (not ctrl_exec.defer([this, cell_index, rnti]() { on_msg4_received_on_ctrl(cell_index, rnti); })) {
    logger.warning("TEST_MODE: Failed to dispatch Msg4 notification for rnti={}", rnti);
  }
}

void du_test_mode_controller::on_slot_completed(du_cell_index_t cell_index, slot_point slot)
{
  if (not ctrl_exec.defer([this, cell_index, slot]() { on_slot_completed_on_ctrl(cell_index, slot); })) {
    logger.warning("TEST_MODE: Failed to dispatch slot notification for cell={}", fmt::underlying(cell_index));
  }
}

void du_test_mode_controller::on_ue_f1ap_id_captured(rnti_t rnti, gnb_du_ue_f1ap_id_t gnb_du_ue_id)
{
  if (not ctrl_exec.defer(
          [this, rnti, gnb_du_ue_id]() { ue_id_table.push_back({.rnti = rnti, .gnb_du_ue_id = gnb_du_ue_id}); })) {
    logger.warning("TEST_MODE: Failed to dispatch F1AP ID capture for rnti={}", rnti);
  }
}

void du_test_mode_controller::on_ue_removed(rnti_t rnti)
{
  if (not cfg.attach_detach_duration.has_value()) {
    // Do nothing if not in attach-detach mode.
    return;
  }

  // Find which cell this UE belongs to and decrement the pending remove counter.
  for (unsigned c = 0; c < cells.size(); ++c) {
    if (is_test_ue_in_cell(static_cast<du_cell_index_t>(c), rnti)) {
      auto& cell = cells[c];
      if (cell.cycle == cell_cycle_state::releasing and cell.nof_ues_pending_remove > 0) {
        --cell.nof_ues_pending_remove;
        if (cell.nof_ues_pending_remove == 0) {
          start_guard_period(to_du_cell_index(c));
        }
      }
      return;
    }
  }
}

void du_test_mode_controller::on_msg4_received_on_ctrl(du_cell_index_t cell_index, rnti_t rnti)
{
  auto& cell = cells[cell_index];
  if (cell.cycle != cell_cycle_state::creating) {
    logger.warning("TEST_MODE cell={} tc-rnti={}: Unexpected Msg4 detected", cell_index, rnti);
    return;
  }

  const unsigned base      = to_value(cfg.rnti) + static_cast<unsigned>(cell_index) * cfg.nof_ues;
  const unsigned ue_offset = to_value(rnti) - base;
  if (ue_offset >= cfg.nof_ues or cell.msg4_counted[ue_offset]) {
    return;
  }

  cell.msg4_counted[ue_offset] = true;
  ++cell.nof_ues_estab;

  if (cell.nof_ues_estab == cfg.nof_ues) {
    // All test mode UEs have been established.
    cell.cycle = cell_cycle_state::running;
    if (cell.attach_detach_timer.is_valid()) {
      // Start attach-detach timer if set.
      cell.attach_detach_timer.run();
      logger.info("TEST_MODE cell={}: All {} UE(s) established. Running for {} ms.",
                  cell_index,
                  cfg.nof_ues,
                  cfg.attach_detach_duration->count());
    } else {
      logger.info("TEST_MODE cell={}: All {} UE(s) established.", cell_index, cfg.nof_ues);
    }
  }
}

void du_test_mode_controller::on_slot_completed_on_ctrl(du_cell_index_t cell_index, slot_point slot)
{
  auto& cell     = cells[cell_index];
  cell.last_slot = slot;
  try_create_ue(cell_index, slot);
}

bool du_test_mode_controller::release_ue(rnti_t rnti)
{
  auto it = std::find_if(ue_id_table.begin(), ue_id_table.end(), [rnti](const ue_entry& e) { return e.rnti == rnti; });
  if (it == ue_id_table.end()) {
    return false;
  }

  auto gnb_cu_ue_id = f1ap_handler->get_gnb_cu_ue_f1ap_id(it->gnb_du_ue_id);
  if (not gnb_cu_ue_id.has_value()) {
    logger.warning("TEST_MODE: Cannot release rnti={}: gnb_cu_ue_f1ap_id not found", rnti);
    return false;
  }

  f1ap_message rel_cmd;
  rel_cmd.pdu.set_init_msg().load_info_obj(ASN1_F1AP_ID_UE_CONTEXT_RELEASE);
  auto& cmd                            = rel_cmd.pdu.init_msg().value.ue_context_release_cmd();
  cmd->gnb_du_ue_f1ap_id               = gnb_du_ue_f1ap_id_to_uint(it->gnb_du_ue_id);
  cmd->gnb_cu_ue_f1ap_id               = gnb_cu_ue_f1ap_id_to_uint(*gnb_cu_ue_id);
  cmd->cause.set_radio_network().value = asn1::f1ap::cause_radio_network_opts::options::normal_release;

  logger.info("TEST_MODE rnti={}: Injecting UE Context Release Command", rnti);
  f1ap_handler->handle_message(rel_cmd);
  return true;
}

void du_test_mode_controller::try_create_ue(du_cell_index_t cell_index, slot_point slot)
{
  auto& cell = cells[cell_index];
  if (cell.cycle != cell_cycle_state::creating) {
    return;
  }
  if (cell.nof_ues_created >= cfg.nof_ues) {
    return;
  }
  if (pdu_handler == nullptr) {
    return;
  }

  const unsigned cell_offset_mod = cfg.ue_creation_stagger_slots * static_cast<unsigned>(cell_index) / MAX_NOF_DU_CELLS;
  if (slot.count() % cfg.ue_creation_stagger_slots != cell_offset_mod) {
    return;
  }

  auto ulcch_buf = byte_buffer::create({0x34, 0x1e, 0x4f, 0xc0, 0x4f, 0xa6, 0x06, 0x3f, 0x00, 0x00, 0x00});
  if (not ulcch_buf.has_value()) {
    logger.warning("TEST_MODE: Postponing creation of test mode ue={}. Cause: Unable to allocate byte_buffer",
                   cell.nof_ues_created);
    return;
  }

  const rnti_t test_ue_rnti =
      to_rnti(to_value(cfg.rnti) + (static_cast<unsigned>(cell_index) * cfg.nof_ues) + cell.nof_ues_created);

  pdu_handler->handle_rx_data_indication(
      mac_rx_data_indication{slot, cell_index, {mac_rx_pdu{test_ue_rnti, 0, ulcch_buf.value().copy()}}});

  logger.info("TEST_MODE cell={} rnti={}: Starting UE creation. There still {}/{} left to be created.",
              test_ue_rnti,
              cell_index,
              cfg.nof_ues - (cell.nof_ues_created + 1),
              cfg.nof_ues);
  ++cell.nof_ues_created;
}

void du_test_mode_controller::handle_attach_detach_timer(du_cell_index_t cell_index)
{
  auto& cell = cells[cell_index];

  switch (cell.cycle) {
    case cell_cycle_state::running: {
      start_release_all_ues_in_cell(cell_index);
    } break;
    case cell_cycle_state::guard: {
      start_reset_cell_for_next_cycle(cell_index);
    } break;
    default:
      break;
  }
}

void du_test_mode_controller::start_guard_period(du_cell_index_t cell_index)
{
  auto& cell = cells[cell_index];

  logger.info("TEST_MODE cell={}: All UE(s) released. Entering guard period.", cell_index);
  cell.cycle = cell_cycle_state::guard;

  // Clean up ID table entries for this cell's UEs.
  ue_id_table.erase(std::remove_if(ue_id_table.begin(),
                                   ue_id_table.end(),
                                   [&](const ue_entry& e) { return is_test_ue_in_cell(cell_index, e.rnti); }),
                    ue_id_table.end());

  // Start GUARD period timer.
  cell.attach_detach_timer.set(GUARD_PERIOD_MSEC);
  cell.attach_detach_timer.run();
}

void du_test_mode_controller::start_release_all_ues_in_cell(du_cell_index_t cell_index)
{
  logger.info("TEST_MODE cell={}: Attach/detach duration elapsed. Releasing {} UE(s).", cell_index, cfg.nof_ues);

  auto& cell            = cells[cell_index];
  cell.cycle            = cell_cycle_state::releasing;
  unsigned nof_released = 0;

  for (unsigned u = 0; u < cfg.nof_ues; ++u) {
    const rnti_t rnti = to_rnti(to_value(cfg.rnti) + static_cast<unsigned>(cell_index) * cfg.nof_ues + u);
    if (release_ue(rnti)) {
      ++nof_released;
    }
  }
  cell.nof_ues_pending_remove = nof_released;

  if (nof_released == 0) {
    start_guard_period(cell_index);
  }
}

void du_test_mode_controller::start_reset_cell_for_next_cycle(du_cell_index_t cell_index)
{
  auto& cell                  = cells[cell_index];
  cell.cycle                  = cell_cycle_state::creating;
  cell.nof_ues_estab          = 0;
  cell.nof_ues_created        = 0;
  cell.nof_ues_pending_remove = 0;
  std::fill(cell.msg4_counted.begin(), cell.msg4_counted.end(), false);

  if (cell_index < cell_notifiers.size() and cell_notifiers[cell_index] != nullptr) {
    cell_notifiers[cell_index]->request_msg4_reset();
  }

  logger.info("TEST_MODE cell={}: Guard period elapsed. Starting new creation cycle.", cell_index);
}
