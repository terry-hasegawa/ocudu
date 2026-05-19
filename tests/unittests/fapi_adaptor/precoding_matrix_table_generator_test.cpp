// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/fapi_adaptor/precoding_matrix_table_generator.h"
#include "ocudu/ran/precoding/precoding_codebook_configuration.h"
#include "ocudu/ran/precoding/precoding_codebook_helpers.h"
#include "ocudu/ran/precoding/precoding_codebooks.h"
#include "ocudu/ran/precoding/precoding_weight_matrix_formatters.h"
#include "fmt/ostream.h"
#include "fmt/std.h"
#include <gtest/gtest.h>

using namespace ocudu;
using namespace fapi_adaptor;

namespace ocudu {

std::ostream& operator<<(std::ostream& os, const precoding_weight_matrix& matrix)
{
  fmt::print(os, "{}", matrix);
  return os;
}

} // namespace ocudu

TEST(precoding_matrix_table_generator, one_port)
{
  std::unique_ptr<precoding_matrix_mapper>     mapper;
  std::unique_ptr<precoding_matrix_repository> repository;
  std::tie(mapper, repository) = generate_precoding_matrix_tables(1, 0);

  mac_pdsch_precoding_info info;
  info.report.reset();

  unsigned index = mapper->map(info, 1);

  precoding_weight_matrix matrix = repository->get_precoding_matrix(index);

  precoding_weight_matrix expected_matrix = make_single_port();

  ASSERT_EQ(matrix, expected_matrix);
}

TEST(precoding_matrix_table_generator, two_port_one_layer)
{
  std::unique_ptr<precoding_matrix_mapper>     mapper;
  std::unique_ptr<precoding_matrix_repository> repository;
  std::tie(mapper, repository) = generate_precoding_matrix_tables(2, 0);

  // Iterate over all possible PMI.
  for (unsigned pmi = 0; pmi != 4; ++pmi) {
    mac_pdsch_precoding_info info;
    info.report.emplace(precoding_matrix_indicator{pmi_two_antenna_port{pmi}});

    unsigned index = mapper->map(info, 1);

    precoding_weight_matrix matrix = repository->get_precoding_matrix(index);

    precoding_weight_matrix expected_matrix = make_one_layer_two_ports(pmi);

    ASSERT_EQ(matrix, expected_matrix);
  }
}

TEST(precoding_matrix_table_generator, two_port_two_layer)
{
  std::unique_ptr<precoding_matrix_mapper>     mapper;
  std::unique_ptr<precoding_matrix_repository> repository;
  std::tie(mapper, repository) = generate_precoding_matrix_tables(2, 0);

  // Iterate over all possible PMI.
  for (unsigned pmi = 0; pmi != 2; ++pmi) {
    mac_pdsch_precoding_info info;
    info.report.emplace(precoding_matrix_indicator{pmi_two_antenna_port{pmi}});

    unsigned index = mapper->map(info, 2);

    precoding_weight_matrix matrix = repository->get_precoding_matrix(index);

    precoding_weight_matrix expected_matrix = make_two_layer_two_ports(pmi);

    ASSERT_EQ(matrix, expected_matrix);
  }
}

TEST(precoding_matrix_table_generator, four_port_typeI_single_panel)
{
  static constexpr unsigned                         four_ports   = 4;
  static constexpr pmi_codebook_single_panel_config panel_config = pmi_codebook_single_panel_config::two_one;

  std::unique_ptr<precoding_matrix_mapper>     mapper;
  std::unique_ptr<precoding_matrix_repository> repository;
  std::tie(mapper, repository) = generate_precoding_matrix_tables(four_ports, 0);

  for (unsigned nof_layers = 1; nof_layers <= four_ports; ++nof_layers) {
    pmi_typeI_single_panel_param_sizes param_sizes =
        get_pmi_sizes_typeI_single_panel(get_single_panel_info(panel_config), nof_layers);

    unsigned nof_i_1_1 = pow2(param_sizes.i_1_1);
    unsigned nof_i_1_2 = pow2(param_sizes.i_1_2);
    unsigned nof_i_1_3 = pow2(param_sizes.i_1_3);
    unsigned nof_i_2   = pow2(param_sizes.i_2);

    for (unsigned i_1_1 = 0; i_1_1 != nof_i_1_1; ++i_1_1) {
      for (unsigned i_1_2 = 0; i_1_2 != nof_i_1_2; ++i_1_2) {
        for (unsigned i_1_3 = 0; i_1_3 != nof_i_1_3; ++i_1_3) {
          for (unsigned i_2 = 0; i_2 != nof_i_2; ++i_2) {
            mac_pdsch_precoding_info info;
            pmi_typeI_single_panel   pmi = {.panel_config = panel_config,
                                            .i_1_1        = i_1_1,
                                            .i_1_2        = param_sizes.i_1_2 ? std::optional(i_1_2) : std::nullopt,
                                            .i_1_3        = param_sizes.i_1_3 ? std::optional(i_1_3) : std::nullopt,
                                            .i_2          = i_2};
            info.report.emplace(pmi);

            unsigned index = mapper->map(info, nof_layers);

            precoding_weight_matrix matrix = repository->get_precoding_matrix(index);

            precoding_weight_matrix expected_matrix = make_type1_sp_mode1(pmi, nof_layers);

            ASSERT_EQ(matrix, expected_matrix) << fmt::format("nof_layers={} i_1_1={} i_1_2={} i_1_3={} i_2={}",
                                                              nof_layers,
                                                              pmi.i_1_1,
                                                              pmi.i_1_2,
                                                              pmi.i_1_3,
                                                              pmi.i_2);
          }
        }
      }
    }
  }
}
