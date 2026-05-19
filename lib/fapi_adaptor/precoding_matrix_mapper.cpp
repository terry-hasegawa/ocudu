// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/fapi_adaptor/precoding_matrix_mapper.h"
#include "precoding_matrix_mapper_functions.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ran/precoding/precoding_codebook_helpers.h"
#include "ocudu/ran/precoding/precoding_matrix_indicator.h"
#include "ocudu/support/ocudu_assert.h"

using namespace ocudu;
using namespace fapi_adaptor;

/// Table of PMI parameter sizes for single-panel type 1 PDSCH precoding codebook. Indexed by the number of layers.
static const std::array<pmi_typeI_single_panel_param_sizes, 5> pdsch_codebook_param_sizes_sp_type1_4port = {{
    {},
    get_pmi_sizes_typeI_single_panel(get_single_panel_info(pmi_codebook_single_panel_config::two_one), 1),
    get_pmi_sizes_typeI_single_panel(get_single_panel_info(pmi_codebook_single_panel_config::two_one), 2),
    get_pmi_sizes_typeI_single_panel(get_single_panel_info(pmi_codebook_single_panel_config::two_one), 3),
    get_pmi_sizes_typeI_single_panel(get_single_panel_info(pmi_codebook_single_panel_config::two_one), 4),
}};

precoding_matrix_mapper::precoding_matrix_mapper(unsigned sector_id_,
                                                 unsigned nof_ports_,
                                                 const precoding_matrix_mapper_codebook_offset_configuration& config) :
  sector_id(sector_id_),
  logger(ocudulog::fetch_basic_logger("FAPI")),
  nof_ports(nof_ports_),
  pdsch_omni_offset(config.pdsch_omni_offset),
  ssb_codebook_offsets(config.ssb_codebook_offsets),
  pdsch_codebook_offsets(config.pdsch_codebook_offsets),
  pdcch_codebook_offsets(config.pdcch_codebook_offsets),
  csi_rs_codebook_offsets(config.csi_rs_codebook_offsets)
{
  ocudu_assert(!ssb_codebook_offsets.empty(), "Invalid offset configuration");
  ocudu_assert(!pdsch_codebook_offsets.empty(), "Invalid offset configuration");
  ocudu_assert(!pdcch_codebook_offsets.empty(), "Invalid offset configuration");
  ocudu_assert(!csi_rs_codebook_offsets.empty(), "Invalid offset configuration");
}

unsigned precoding_matrix_mapper::map(const mac_csi_rs_precoding_info& precoding_info) const
{
  return csi_rs_codebook_offsets[0] + get_csi_rs_precoding_matrix_index();
}

unsigned precoding_matrix_mapper::map(const mac_ssb_precoding_info& precoding_info) const
{
  return ssb_codebook_offsets[0] + get_ssb_precoding_matrix_index();
}

unsigned precoding_matrix_mapper::map(const mac_pdcch_precoding_info& precoding_info) const
{
  return pdcch_codebook_offsets[0] + get_pdcch_precoding_matrix_index();
}

/// Returns the precoding matrix index for the PDSCH codebook using the given offset, precoding information and number
/// of ports.
static unsigned get_pdsch_precoding_matrix_index(unsigned                          offset,
                                                 const precoding_matrix_indicator& precoding_info,
                                                 unsigned                          nof_ports,
                                                 unsigned                          nof_layers,
                                                 unsigned                          sector_id,
                                                 ocudulog::basic_logger&           logger)
{
  if (nof_ports == 1U) {
    logger.debug("Sector#{}: One port PDSCH precoding matrix, nof_layers={}", sector_id, nof_layers);

    return offset + get_pdsch_one_port_precoding_matrix_index();
  }

  if (nof_ports == 2U) {
    ocudu_assert(std::holds_alternative<pmi_two_antenna_port>(precoding_info), "Expected PMI information");
    unsigned pmi = std::get<pmi_two_antenna_port>(precoding_info).pmi;

    logger.debug("Sector#{}: Two ports PDSCH precoding matrix, pmi={}, nof_layers={}", sector_id, pmi, nof_layers);

    return offset + get_pdsch_two_port_precoding_matrix_index(pmi);
  }

  if (nof_ports == 4U) {
    ocudu_assert(nof_layers < pdsch_codebook_param_sizes_sp_type1_4port.size(),
                 "The number of layers exceeds the supported number of layers.");
    ocudu_assert(std::holds_alternative<pmi_typeI_single_panel>(precoding_info), "Invalid PMI information");
    const auto& report = std::get<pmi_typeI_single_panel>(precoding_info);

    logger.debug("Sector#{}: Four ports PDSCH precoding matrix, i11={}, i13={}, i2={}, nof_layers={}",
                 sector_id,
                 report.i_1_1,
                 (report.i_1_3) ? report.i_1_3.value() : -1,
                 report.i_2,
                 nof_layers);

    return offset + get_pdsch_single_panel_type1_precoding_matrix_index(
                        pdsch_codebook_param_sizes_sp_type1_4port[nof_layers], report);
  }

  return 0;
}

unsigned precoding_matrix_mapper::map(const mac_pdsch_precoding_info& precoding_info, unsigned nof_layers) const
{
  ocudu_assert(nof_layers > 0, "Invalid number of layers={}", nof_layers);

  if (!precoding_info.report) {
    logger.debug("Sector#{}: Omnidirectional PDSCH precoding matrix, nof_layers={}", sector_id, nof_layers);

    return pdsch_omni_offset + get_pdsch_omnidirectional_precoding_matrix_index();
  }

  unsigned layer_index = nof_layers - 1U;
  ocudu_assert(layer_index < pdsch_codebook_offsets.size(), "Invalid layer index value {}", layer_index);

  return get_pdsch_precoding_matrix_index(
      pdsch_codebook_offsets[layer_index], *precoding_info.report, nof_ports, nof_layers, sector_id, logger);
}
