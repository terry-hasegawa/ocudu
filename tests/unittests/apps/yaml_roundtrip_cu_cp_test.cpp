// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "apps/cu_cp/cu_cp_appconfig.h"
#include "apps/cu_cp/cu_cp_appconfig_cli11_schema.h"
#include "apps/cu_cp/cu_cp_appconfig_yaml_writer.h"
#include "apps/units/o_cu_cp/o_cu_cp_application_unit.h"
#include "yaml_roundtrip_test_helpers.h"
#include "ocudu/support/config_parsers.h"
#include "CLI/CLI11.hpp"

#ifndef CONFIGS_DIR
#error "CONFIGS_DIR must be defined"
#endif

using namespace ocudu;
using namespace ocudu::yaml_roundtrip_test;

namespace {

YAML::Node load_and_emit(const std::string& yaml_text)
{
  temp_yaml_file tmp(yaml_text);

  CLI::App app("cu_cp yaml-roundtrip-test");
  app.config_formatter(create_yaml_config_parser());
  app.allow_config_extras(CLI::config_extras_mode::error);
  std::string cfg_path;
  app.set_config("-c,", cfg_path, "Read config from file", false);

  cu_cp_appconfig cu_cp_cfg;
  configure_cli11_with_cu_cp_appconfig_schema(app, cu_cp_cfg);

  auto o_cu_cp = create_o_cu_cp_application_unit("cucp");
  o_cu_cp->on_parsing_configuration_registration(app);

  app.callback([&]() { o_cu_cp->on_configuration_parameters_autoderivation(app); });

  std::vector<const char*> argv = {"cu_cp", "-c", tmp.path().c_str()};
  app.parse(static_cast<int>(argv.size()), argv.data());

  YAML::Node node;
  fill_cu_cp_appconfig_in_yaml_schema(node, cu_cp_cfg);
  o_cu_cp->dump_config(node);
  return node;
}

YAML::Node emit_defaults()
{
  cu_cp_appconfig cu_cp_cfg;
  auto            o_cu_cp = create_o_cu_cp_application_unit("cucp");
  YAML::Node      node;
  fill_cu_cp_appconfig_in_yaml_schema(node, cu_cp_cfg);
  o_cu_cp->dump_config(node);
  return node;
}

const std::string CONFIGS = CONFIGS_DIR;

class cu_cp_example_config_test : public ::testing::TestWithParam<std::string>
{};

TEST_P(cu_cp_example_config_test, roundtrip)
{
  const std::string& name = GetParam();
  assert_roundtrip(read_file(CONFIGS + "/" + name), &load_and_emit, name);
}

INSTANTIATE_TEST_SUITE_P(, cu_cp_example_config_test, ::testing::Values("cu_cp.yml"));

TEST(cu_cp_default_config_test, roundtrip)
{
  YAML::Node a = emit_defaults();
  assert_roundtrip(YAML::Dump(a), &load_and_emit, "cu_cp defaults");
}

} // namespace
