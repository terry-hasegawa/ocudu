// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "fmt/format.h"

namespace ocudu {

/// PDCP NR RLC mode information.
enum class pdcp_rlc_mode { um, am };

} // namespace ocudu

// Formatters.
namespace fmt {

template <>
struct formatter<ocudu::pdcp_rlc_mode> {
  template <typename ParseContext>
  auto parse(ParseContext& ctx)
  {
    return ctx.begin();
  }

  template <typename FormatContext>
  auto format(ocudu::pdcp_rlc_mode mode, FormatContext& ctx) const
  {
    static constexpr const char* options[] = {"UM", "AM"};
    return format_to(ctx.out(), "{}", options[static_cast<unsigned>(mode)]);
  }
};
} // namespace fmt
