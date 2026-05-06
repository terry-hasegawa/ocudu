// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "fmt/format.h"
#include <cstdint>
#include <type_traits>

namespace ocudu {

/// \brief Ue_index internally used to identify the UE CU-CP-wide.
enum class cu_cp_ue_index_t : uint64_t {
  min     = 0,
  max     = std::numeric_limits<uint64_t>::max() - 1,
  invalid = std::numeric_limits<uint64_t>::max()
};

/// Convert ue_index type to integer.
inline uint64_t cu_cp_ue_index_to_uint(cu_cp_ue_index_t index)
{
  return static_cast<uint64_t>(index);
}

/// Convert integer to ue_index type.
inline cu_cp_ue_index_t uint_to_ue_index(std::underlying_type_t<cu_cp_ue_index_t> index)
{
  return static_cast<cu_cp_ue_index_t>(index);
}

} // namespace ocudu

namespace fmt {

template <>
struct formatter<ocudu::cu_cp_ue_index_t> {
  template <typename ParseContext>
  auto parse(ParseContext& ctx)
  {
    return ctx.begin();
  }

  template <typename FormatContext>
  auto format(ocudu::cu_cp_ue_index_t ue_idx, FormatContext& ctx) const
  {
    if (ue_idx == ocudu::cu_cp_ue_index_t::invalid) {
      return format_to(ctx.out(), "invalid");
    }
    return format_to(ctx.out(), "{}", static_cast<uint64_t>(ue_idx));
  }
};

} // namespace fmt
