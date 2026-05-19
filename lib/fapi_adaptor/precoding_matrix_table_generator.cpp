// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/fapi_adaptor/precoding_matrix_table_generator.h"
#include "precoding_matrix_mapper_functions.h"
#include "precoding_matrix_repository_builder.h"
#include "ocudu/fapi_adaptor/precoding_matrix_mapper.h"
#include "ocudu/ran/precoding/precoding_codebook_helpers.h"
#include "ocudu/ran/precoding/precoding_codebooks.h"

using namespace ocudu;
using namespace fapi_adaptor;

/// Maximum number of codebooks for 1-port, 2-port and 4-port configurations.
static constexpr std::array<unsigned, 4> max_num_codebooks = {{6, 11, 0, 261}};

/// Generates SSB codebooks and precoding matrices for the given number of ports.
static unsigned generate_ssb(unsigned offset, unsigned nof_ports, precoding_matrix_repository_builder& repo_builder)
{
  precoding_weight_matrix precoding = make_one_layer_one_port(nof_ports, 0);
  unsigned                pm_index  = offset + get_ssb_precoding_matrix_index();
  repo_builder.add(pm_index, precoding);

  return ++offset;
}

/// Generates PDCCH codebooks and precoding matrices for the given number of ports.
static unsigned generate_pdcch(unsigned offset, unsigned nof_ports, precoding_matrix_repository_builder& repo_builder)
{
  precoding_weight_matrix precoding = make_one_layer_one_port(nof_ports, 0);
  unsigned                pm_index  = offset + get_pdcch_precoding_matrix_index();
  repo_builder.add(pm_index, precoding);

  return ++offset;
}

/// Generates CSI-RS codebooks and precoding matrices for the given number of ports.
static unsigned generate_csi_rs(unsigned offset, unsigned nof_ports, precoding_matrix_repository_builder& repo_builder)
{
  precoding_weight_matrix precoding = make_identity(nof_ports);
  unsigned                pm_index  = offset + get_csi_rs_precoding_matrix_index();
  repo_builder.add(pm_index, precoding);

  return ++offset;
}

/// Generates PDSCH omnidirectional codebook and precoding matrices for the given number of ports.
static unsigned
generate_pdsch_omnidirectional(unsigned offset, unsigned nof_ports, precoding_matrix_repository_builder& repo_builder)
{
  precoding_weight_matrix precoding = make_one_layer_one_port(nof_ports, 0);
  unsigned                pm_index  = offset + get_pdsch_omnidirectional_precoding_matrix_index();
  repo_builder.add(pm_index, precoding);

  return ++offset;
}

/// Generates one-port PDSCH codebooks and precoding matrices.
static unsigned generate_pdsch_one_port(unsigned offset, precoding_matrix_repository_builder& repo_builder)
{
  precoding_weight_matrix precoding = make_single_port();
  unsigned                pm_index  = offset + get_pdsch_one_port_precoding_matrix_index();
  repo_builder.add(pm_index, precoding);

  return ++offset;
}

/// Generates the identity matrix.
static unsigned
generate_identity_matrix(unsigned offset, precoding_matrix_repository_builder& repo_builder, unsigned nof_layers)
{
  precoding_weight_matrix precoding = make_identity(nof_layers);
  repo_builder.add(0, precoding);

  return ++offset;
}

/// Generates one-port codebooks and precoding matrices.
static void generate_single_port_table(precoding_matrix_mapper_codebook_offset_configuration& mapper_offsets,
                                       precoding_matrix_repository_builder&                   repo_builder)
{
  unsigned                  offset    = 0U;
  static constexpr unsigned nof_ports = 1U;

  offset = generate_identity_matrix(offset, repo_builder, nof_ports);
  mapper_offsets.ssb_codebook_offsets.push_back(offset);
  offset                           = generate_ssb(offset, nof_ports, repo_builder);
  mapper_offsets.pdsch_omni_offset = offset;
  offset                           = generate_pdsch_omnidirectional(offset, nof_ports, repo_builder);
  mapper_offsets.pdsch_codebook_offsets.push_back(offset);
  offset = generate_pdsch_one_port(offset, repo_builder);
  mapper_offsets.pdcch_codebook_offsets.push_back(offset);
  offset = generate_pdcch(offset, nof_ports, repo_builder);
  mapper_offsets.csi_rs_codebook_offsets.push_back(offset);
  generate_csi_rs(offset, nof_ports, repo_builder);
}

/// Generates two-port PDSCH codebooks and precoding matrices for one layer.
static unsigned generate_pdsch_2_ports_1_layer(unsigned offset, precoding_matrix_repository_builder& repo_builder)
{
  unsigned base_offset = offset;
  for (unsigned i = 0, e = 4; i != e; ++i) {
    precoding_weight_matrix precoding = make_one_layer_two_ports(i);
    unsigned                pm_index  = base_offset + get_pdsch_two_port_precoding_matrix_index(i);
    repo_builder.add(pm_index, precoding);
    offset = pm_index;
  }
  return ++offset;
}

/// Generates two-port PDSCH codebooks and precoding matrices for two layers.
static unsigned generate_pdsch_2_ports_2_layers(unsigned offset, precoding_matrix_repository_builder& repo_builder)
{
  unsigned base_offset = offset;
  for (unsigned i = 0, e = 2; i != e; ++i) {
    precoding_weight_matrix precoding = make_two_layer_two_ports(i);
    unsigned                pm_index  = base_offset + get_pdsch_two_port_precoding_matrix_index(i);
    repo_builder.add(pm_index, precoding);
    offset = pm_index;
  }

  return ++offset;
}

/// Generates two-port codebooks and precoding matrices.
static void generate_2_ports_table(precoding_matrix_mapper_codebook_offset_configuration& mapper_offsets,
                                   precoding_matrix_repository_builder&                   repo_builder)
{
  unsigned                  offset    = 0U;
  static constexpr unsigned nof_ports = 2U;

  offset = generate_identity_matrix(offset, repo_builder, nof_ports);
  mapper_offsets.ssb_codebook_offsets.push_back(offset);
  offset = generate_ssb(offset, nof_ports, repo_builder);
  mapper_offsets.pdcch_codebook_offsets.push_back(offset);
  offset                           = generate_pdcch(offset, nof_ports, repo_builder);
  mapper_offsets.pdsch_omni_offset = offset;
  offset                           = generate_pdsch_omnidirectional(offset, nof_ports, repo_builder);
  mapper_offsets.pdsch_codebook_offsets.push_back(offset);
  offset = generate_pdsch_2_ports_1_layer(offset, repo_builder);
  mapper_offsets.pdsch_codebook_offsets.push_back(offset);
  offset = generate_pdsch_2_ports_2_layers(offset, repo_builder);
  mapper_offsets.csi_rs_codebook_offsets.push_back(offset);
  generate_csi_rs(offset, nof_ports, repo_builder);
}

/// Generates PDSCH single-panel type 1 precoding matrices codebook for a number of layers.
static unsigned generate_pdsch_sp_type1(unsigned                             offset,
                                        const pmi_codebook_single_panel_info panel_info,
                                        unsigned                             nof_layers,
                                        precoding_matrix_repository_builder& repo_builder)
{
  unsigned base_offset = offset;

  // Get parameter sizes
  pmi_typeI_single_panel_param_sizes param_sizes = get_pmi_sizes_typeI_single_panel(panel_info, nof_layers);

  unsigned nof_i_1_1 = pow2(param_sizes.i_1_1);
  unsigned nof_i_1_2 = pow2(param_sizes.i_1_2);
  unsigned nof_i_1_3 = pow2(param_sizes.i_1_3);
  unsigned nof_i_2   = pow2(param_sizes.i_2);

  for (unsigned i_1_1 = 0; i_1_1 != nof_i_1_1; ++i_1_1) {
    for (unsigned i_1_2 = 0; i_1_2 != nof_i_1_2; ++i_1_2) {
      for (unsigned i_1_3 = 0; i_1_3 != nof_i_1_3; ++i_1_3) {
        for (unsigned i_2 = 0; i_2 != nof_i_2; ++i_2) {
          pmi_typeI_single_panel  pmi       = {.panel_config = pmi_codebook_single_panel_config::two_one,
                                               .i_1_1        = i_1_1,
                                               .i_1_2        = param_sizes.i_1_2 ? std::optional(i_1_2) : std::nullopt,
                                               .i_1_3        = param_sizes.i_1_3 ? std::optional(i_1_3) : std::nullopt,
                                               .i_2          = i_2};
          precoding_weight_matrix precoding = make_type1_sp_mode1(pmi, nof_layers);
          unsigned pm_index = base_offset + get_pdsch_single_panel_type1_precoding_matrix_index(param_sizes, pmi);
          repo_builder.add(pm_index, precoding);

          offset = pm_index;
        }
      }
    }
  }

  return ++offset;
}

/// Generates four-port codebooks and precoding matrices.
static void generate_4_ports_table(precoding_matrix_mapper_codebook_offset_configuration& mapper_offsets,
                                   precoding_matrix_repository_builder&                   repo_builder)
{
  static constexpr pmi_codebook_single_panel_config panel_config = pmi_codebook_single_panel_config::two_one;
  static constexpr unsigned                         nof_ports    = 4U;

  unsigned offset = 0U;

  // Get panel parameters.
  pmi_codebook_single_panel_info panel_info = get_single_panel_info(panel_config);

  offset = generate_identity_matrix(offset, repo_builder, nof_ports);
  mapper_offsets.ssb_codebook_offsets.push_back(offset);
  offset = generate_ssb(offset, nof_ports, repo_builder);
  mapper_offsets.pdcch_codebook_offsets.push_back(offset);
  offset                           = generate_pdcch(offset, nof_ports, repo_builder);
  mapper_offsets.pdsch_omni_offset = offset;
  offset                           = generate_pdsch_omnidirectional(offset, nof_ports, repo_builder);
  for (unsigned nof_layers = 1; nof_layers <= nof_ports; ++nof_layers) {
    mapper_offsets.pdsch_codebook_offsets.push_back(offset);
    offset = generate_pdsch_sp_type1(offset, panel_info, nof_layers, repo_builder);
  }
  mapper_offsets.csi_rs_codebook_offsets.push_back(offset);
  generate_csi_rs(offset, nof_ports, repo_builder);
}

std::pair<std::unique_ptr<precoding_matrix_mapper>, std::unique_ptr<precoding_matrix_repository>>
ocudu::fapi_adaptor::generate_precoding_matrix_tables(unsigned nof_antenna_ports, unsigned sector_id)
{
  ocudu_assert(nof_antenna_ports > 0, "Invalid number of antenna ports={}", nof_antenna_ports);
  ocudu_assert(nof_antenna_ports != 3, "Unsupported number of antenna ports={}", nof_antenna_ports);

  unsigned                                              nof_ports_index = nof_antenna_ports - 1U;
  precoding_matrix_mapper_codebook_offset_configuration mapper_offsets;
  precoding_matrix_repository_builder                   repo_builder(max_num_codebooks[nof_ports_index]);

  if (nof_antenna_ports == 1U) {
    generate_single_port_table(mapper_offsets, repo_builder);

    return {std::make_unique<precoding_matrix_mapper>(sector_id, nof_antenna_ports, mapper_offsets),
            repo_builder.build()};
  }

  if (nof_antenna_ports == 2U) {
    generate_2_ports_table(mapper_offsets, repo_builder);

    return {std::make_unique<precoding_matrix_mapper>(sector_id, nof_antenna_ports, mapper_offsets),
            repo_builder.build()};
  }

  if (nof_antenna_ports == 4U) {
    generate_4_ports_table(mapper_offsets, repo_builder);

    return {std::make_unique<precoding_matrix_mapper>(sector_id, nof_antenna_ports, mapper_offsets),
            repo_builder.build()};
  }

  ocudu_assert(false, "Unsupported number of ports", nof_antenna_ports);
  return {};
}
