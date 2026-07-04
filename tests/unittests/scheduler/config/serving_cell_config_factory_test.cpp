// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ocudu/scheduler/config/cell_config_builder_params.h"
#include "ocudu/scheduler/config/ran_cell_config_helper.h"
#include "ocudu/scheduler/config/serving_cell_config_factory.h"
#include <gtest/gtest.h>

using namespace ocudu;

TEST(serving_cell_config_factory_test, pdsch_max_mimo_layers_follows_max_nof_layers)
{
  cell_config_builder_params params{};
  params.dl_carrier.nof_ant = 2;

  const ue_cell_config ue_cfg =
      config_helpers::make_default_ue_cell_config(config_helpers::make_default_ran_cell_config(params));

  ASSERT_TRUE(ue_cfg.serv_cell_cfg.pdsch_serv_cell_cfg.has_value());
  // When not explicitly set, the maximum number of DL layers is derived from the number of DL antenna ports.
  EXPECT_EQ(ue_cfg.serv_cell_cfg.pdsch_serv_cell_cfg->max_mimo_layers, 2U);
}

TEST(serving_cell_config_factory_test, pdsch_max_mimo_layers_capped_by_explicit_max_nof_layers)
{
  cell_config_builder_params params{};
  params.dl_carrier.nof_ant = 2;
  params.max_nof_layers     = 1;

  const ue_cell_config ue_cfg =
      config_helpers::make_default_ue_cell_config(config_helpers::make_default_ran_cell_config(params));

  ASSERT_TRUE(ue_cfg.serv_cell_cfg.pdsch_serv_cell_cfg.has_value());
  EXPECT_EQ(ue_cfg.serv_cell_cfg.pdsch_serv_cell_cfg->max_mimo_layers, 1U);
}
