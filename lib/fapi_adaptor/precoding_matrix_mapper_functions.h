// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/ran/precoding/precoding_codebook_helpers.h"

namespace ocudu {
namespace fapi_adaptor {

/// Returns the precoding matrix index for the SSB codebook.
inline unsigned get_ssb_precoding_matrix_index()
{
  return 0U;
}

/// Returns the precoding matrix index for the CSI-RS codebook.
inline unsigned get_csi_rs_precoding_matrix_index()
{
  return 0U;
}

/// Returns the precoding matrix index for the PDCCH codebook.
inline unsigned get_pdcch_precoding_matrix_index()
{
  return 0U;
}

/// Returns the precoding matrix index for the one-port PDSCH codebook.
inline unsigned get_pdsch_one_port_precoding_matrix_index()
{
  return 0U;
}

/// Returns the precoding matrix index for the PDSCH omnidirectional codebook.
inline unsigned get_pdsch_omnidirectional_precoding_matrix_index()
{
  return 0U;
}

/// Returns the precoding matrix index for the two-port PDSCH codebook using the given PMI.
inline unsigned get_pdsch_two_port_precoding_matrix_index(unsigned pmi)
{
  return pmi;
}

/// Returns the precoding matrix index for a single-panel type 1 PDSCH codebook for the given PMI parameters.
inline unsigned
get_pdsch_single_panel_type1_precoding_matrix_index(const pmi_typeI_single_panel_param_sizes& param_sizes,
                                                    const pmi_typeI_single_panel&             pmi)
{
  unsigned index = pmi.i_1_1;

  if (param_sizes.i_1_2) {
    ocudu_assert(pmi.i_1_2.has_value(), "Parameter i_1_2 is missing.");
    index = (index << param_sizes.i_1_2) + *pmi.i_1_2;
  }

  if (param_sizes.i_1_3) {
    ocudu_assert(pmi.i_1_3.has_value(), "Parameter i_1_3 is missing.");
    index = (index << param_sizes.i_1_3) + *pmi.i_1_3;
  }

  index = (index << param_sizes.i_2) + pmi.i_2;

  return index;
}

} // namespace fapi_adaptor
} // namespace ocudu
