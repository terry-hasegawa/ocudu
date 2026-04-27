// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/adt/bounded_integer.h"
#include "ocudu/ran/precoding/precoding_matrix_indicator.h"
#include <optional>
#include <variant>

namespace ocudu {

/// Channel Quality Indicator value.
using cqi_value = bounded_integer<uint8_t, 0, 15>;

/// Channel Quality Indicator type.
using csi_report_wideband_cqi_type = cqi_value;

/// Channel State Information (CSI) report fields.
struct csi_report_data {
  /// Rank Indicator (RI) data type.
  using ri_type = bounded_integer<uint8_t, 1, 8>;
  /// Layer Indicator (LI) data type.
  using li_type = bounded_integer<uint8_t, 0, 7>;
  /// Wideband Channel Quality Indicator (CQI) data type.
  using wideband_cqi_type = bounded_integer<uint8_t, 0, 15>;

  /// CSI-RS Resource Indicator (CRI) if reported.
  std::optional<uint8_t> cri;
  /// Rank Indicator (RI) if reported. The range is {1, ..., 8}.
  std::optional<ri_type> ri;
  /// Layer Indicator (LI) if reported.
  std::optional<li_type> li;
  /// PMI wideband information fields if reported.
  std::optional<precoding_matrix_indicator> pmi;
  /// Wideband CQI for the first TB.
  std::optional<wideband_cqi_type> first_tb_wideband_cqi;
  /// Wideband CQI for the second TB.
  std::optional<wideband_cqi_type> second_tb_wideband_cqi;
  /// Flag indicating if the CSI was detected correctly.
  bool valid;
};

} // namespace ocudu
