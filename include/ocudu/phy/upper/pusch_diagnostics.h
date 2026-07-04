// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/adt/static_vector.h"
#include "ocudu/phy/antenna_ports.h"
#include <optional>

namespace ocudu {

/// \brief Optional PUSCH diagnostic measurements.
///
/// Collected by the PUSCH demodulator only when the PUSCH diagnostics are enabled (see \c
/// pusch_diagnostics_enabled in the Distributed Unit low expert configuration). All quantities are cheap to compute
/// and are intended for offline link-quality troubleshooting (e.g., separating reported SNR from effective SINR).
struct pusch_diagnostics {
  /// Post-equalization SINR for each transmission layer, in decibels.
  static_vector<float, MAX_PORTS> sinr_layer_dB;
  /// Channel estimator noise variance for each receive port, in decibels (relative to the resource grid full scale).
  static_vector<float, MAX_PORTS> port_noise_var_dB;
  /// \brief Channel Gram matrix condition number in decibels.
  ///
  /// Median over a subsampled set of resource elements of the first processed OFDM symbol. Only computed for
  /// two-layer transmissions, as it quantifies the layer separability (ratio of the largest to the smallest
  /// eigenvalue of \f$H^H H\f$).
  std::optional<float> ch_cond_dB;
  /// Ratio of soft bits saturated at the maximum LLR amplitude, in range [0, 1].
  float llr_sat_ratio = 0.0F;
  /// Average absolute LLR amplitude (maximum is 120).
  float llr_abs_mean = 0.0F;
};

} // namespace ocudu
