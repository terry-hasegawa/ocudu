// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "precoding_matrix_indicator.h"
#include "ocudu/ran/precoding/precoding_weight_matrix.h"

namespace ocudu {

/// Constructs a precoder configuration for a single transmitter port.
precoding_weight_matrix make_single_port();

/// \brief Constructs a precoding weight matrix for one layer mapped into one transmit port.
///
/// The matrix dimensions correspond to one layer and a given number of ports. All weights are zero except for the
/// indicated port.
///
/// \param[in] nof_ports Number of ports available for transmitting.
/// \param[in] i_port    Port identifier {0, ..., \c nof_ports - 1}.
/// \return A precoding weight matrix for one layer and \c nof_ports ports.
/// \remark An assertion is triggered if \c i_port is equal to or greater than \c nof_ports.
precoding_weight_matrix make_one_layer_one_port(unsigned nof_ports, unsigned i_port);

/// \brief Constructs a precoding weight matrix for the one layer&ndash;one port mapping.
///
/// The precoding maps each layer to the port with the same identifier.
///
/// \param[in] nof_streams Number of layers and ports.
/// \return A scaled identity precoding weight matrix for \c nof_streams layers and ports. The constant value of the
/// entries in the main diagonal is computed to satisfy power-allocation requirements. All other entries are equal to
/// zero.
/// \remark An assertion is triggered if \c nof_streams is out of the range {1, ..., \ref
/// precoding_constants::MAX_NOF_LAYERS}.
precoding_weight_matrix make_identity(unsigned nof_streams);

/// \brief Constructs a precoding weight matrix for one layer mapped into all transmit ports.
///
/// All weights are set so that an identical signal is generated for each antenna port.
///
/// \param[in] nof_ports Number of ports available for transmitting.
/// \return A precoding weight matrix (actually, a column vector) for one layer and \c nof_ports.
/// \remark This precoding matrix is intended for testing purposes only.
precoding_weight_matrix make_one_layer_all_ports(unsigned nof_ports);

/// \brief Constructs a precoding weight matrix for one layer mapped into two transmit ports.
///
/// All weights are derived from TS38.214 Table 5.2.2.2.1-1 for 1-layer CSI reporting.
///
/// \param[in] i_codebook Codebook identifier.
/// \return A precoding weight matrix for one layer and two ports.
precoding_weight_matrix make_one_layer_two_ports(unsigned i_codebook);

/// \brief Constructs a precoding weight matrix for two layers mapped into two transmit ports.
///
/// All weights are derived from TS38.214 Table 5.2.2.2.1-1 for 2-layer CSI reporting.
///
/// \param[in] i_codebook Codebook identifier.
/// \return A precoding weight matrix for two layers and two ports.
precoding_weight_matrix make_two_layer_two_ports(unsigned i_codebook);

/// \brief Constructs a precoding weight matrix for a given number of layers for a Type I Single-Panel antenna
/// configuration.
///
/// All weights are derived from TS38.214 Table 5.2.2.2.1-5 to 5.2.2.2.1-8, which describe CSI reporting using Type I
/// Single-Panel codebook for one to four layers. The generated precoding weights for the first half of ports
/// corresponds to the first polarization, while the second half of ports corresponds to the second polarization.
///
/// \param[in] pmi The Precoding Matrix Indicator (PMI) codebook parameters.
/// \param[in] nof_layers The number of layers used for the transmission.
/// \return A precoding weight matrix for the given number of layers and the given antenna panel distribution.
precoding_weight_matrix make_type1_sp_mode1(const precoding_matrix_indicator& pmi, unsigned nof_layers);

} // namespace ocudu
