// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ocudu/ran/precoding/precoding_codebook_configuration.h"
#include "ocudu/adt/to_array.h"
#include "ocudu/ran/precoding/precoding_codebook_helpers.h"
#include "ocudu/support/ocudu_assert.h"

using namespace ocudu;

/// List of PMI codebook configurations indexed by \c precoding_codebook_identifier.
static constexpr auto codebook_configurations = to_array<pmi_codebook_config>(
    {pmi_codebook_one_port{},
     pmi_codebook_two_port{},
     pmi_codebook_typeI_single_panel{pmi_codebook_single_panel_config::two_one, pmi_codebook_typeI_mode::one},
     pmi_codebook_typeI_single_panel{pmi_codebook_single_panel_config::two_two, pmi_codebook_typeI_mode::one},
     pmi_codebook_typeI_single_panel{pmi_codebook_single_panel_config::four_one, pmi_codebook_typeI_mode::one},
     pmi_codebook_typeI_single_panel{pmi_codebook_single_panel_config::three_two, pmi_codebook_typeI_mode::one},
     pmi_codebook_typeI_single_panel{pmi_codebook_single_panel_config::six_one, pmi_codebook_typeI_mode::one},
     pmi_codebook_typeI_single_panel{pmi_codebook_single_panel_config::four_two, pmi_codebook_typeI_mode::one},
     pmi_codebook_typeI_single_panel{pmi_codebook_single_panel_config::eight_one, pmi_codebook_typeI_mode::one},
     pmi_codebook_typeI_single_panel{pmi_codebook_single_panel_config::four_three, pmi_codebook_typeI_mode::one},
     pmi_codebook_typeI_single_panel{pmi_codebook_single_panel_config::six_two, pmi_codebook_typeI_mode::one},
     pmi_codebook_typeI_single_panel{pmi_codebook_single_panel_config::twelve_one, pmi_codebook_typeI_mode::one},
     pmi_codebook_typeI_single_panel{pmi_codebook_single_panel_config::four_four, pmi_codebook_typeI_mode::one},
     pmi_codebook_typeI_single_panel{pmi_codebook_single_panel_config::eight_two, pmi_codebook_typeI_mode::one},
     pmi_codebook_typeI_single_panel{pmi_codebook_single_panel_config::sixteen_one, pmi_codebook_typeI_mode::one}});
static_assert(codebook_configurations.size() == pmi_codebook_id::max() + 1,
              "The number of codebook configurations does not match the number of identifiers.");

/// List of PMI codebook descriptions as strings indexed by \c precoding_codebook_identifier.
static constexpr auto codebook_configurations_string =
    to_array<const char*>({"one port",
                           "two port",
                           "Type I mode 1 single-panel 4-port 2x1",
                           "Type I mode 1 single-panel 8-port 2x2",
                           "Type I mode 1 single-panel 8-port 4x1",
                           "Type I mode 1 single-panel 12-port 3x2",
                           "Type I mode 1 single-panel 12-port 6x1",
                           "Type I mode 1 single-panel 16-port 4x2",
                           "Type I mode 1 single-panel 16-port 8x1",
                           "Type I mode 1 single-panel 24-port 4x3",
                           "Type I mode 1 single-panel 24-port 6x2",
                           "Type I mode 1 single-panel 24-port 12x1",
                           "Type I mode 1 single-panel 32-port 4x4",
                           "Type I mode 1 single-panel 32-port 8x2",
                           "Type I mode 1 single-panel 32-port 16x1"});
static_assert(codebook_configurations.size() == pmi_codebook_id::max() + 1,
              "The number of codebook strings does not match the number of identifiers.");

static pmi_codebook_id to_id(std::monostate)
{
  return 0;
}

static pmi_codebook_id to_id(pmi_codebook_one_port)
{
  return 0;
}

static pmi_codebook_id to_id(pmi_codebook_two_port)
{
  return 1;
}

static pmi_codebook_id to_id(const pmi_codebook_typeI_single_panel& codebook)
{
  ocudu_assert(codebook.mode == pmi_codebook_typeI_mode::one, "Unsupported mode.");
  return 2 + static_cast<unsigned>(codebook.n1_n2);
}

pmi_codebook_id ocudu::to_pmi_codebook_identifier(const pmi_codebook_config& codebook)
{
  return std::visit([](const auto& item) { return to_id(item); }, codebook);
}

const pmi_codebook_config& ocudu::to_pmi_codebook_config(pmi_codebook_id identifier)
{
  return codebook_configurations[identifier.value()];
}

const char* ocudu::to_string(const pmi_codebook_config& codebook)
{
  pmi_codebook_id id = to_pmi_codebook_identifier(codebook);
  return codebook_configurations_string[id.value()];
}

unsigned ocudu::get_precoding_codebook_antenna_ports(const pmi_codebook_config& pmi_codebook)
{
  struct overloaded {
    unsigned operator()(std::monostate) const { return 0; }
    unsigned operator()(pmi_codebook_one_port) const { return 1; }
    unsigned operator()(pmi_codebook_two_port) const { return 2; }
    unsigned operator()(const pmi_codebook_typeI_single_panel& codebook) const
    {
      pmi_codebook_single_panel_info panel_config = get_single_panel_info(codebook.n1_n2);
      return 2 * panel_config.n1 * panel_config.n2;
    }
  };

  return std::visit(overloaded{}, pmi_codebook);
}
