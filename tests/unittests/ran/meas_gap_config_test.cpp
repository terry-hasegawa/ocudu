// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ocudu/ran/meas_gap_config.h"
#include <gtest/gtest.h>

using namespace ocudu;

TEST(supported_meas_gap_patterns_test, default_only_supports_mandatory_patterns_0_and_1)
{
  const supported_meas_gap_patterns default_patterns;

  // Gap patterns 0 and 1 are mandatory and always supported.
  EXPECT_TRUE(default_patterns.is_supported(0));
  EXPECT_TRUE(default_patterns.is_supported(1));
  // No other gap pattern is supported when no UE capabilities are available.
  for (unsigned pattern_id = 2; pattern_id != nof_meas_gap_patterns; ++pattern_id) {
    EXPECT_FALSE(default_patterns.is_supported(pattern_id)) << "pattern " << pattern_id;
  }
}

TEST(supported_meas_gap_patterns_test, mandatory_patterns_are_supported_even_if_not_marked)
{
  // Even if patterns 0 and 1 are never explicitly marked, they must remain supported.
  supported_meas_gap_patterns patterns;
  patterns.mark_supported(4);

  EXPECT_TRUE(patterns.is_supported(0));
  EXPECT_TRUE(patterns.is_supported(1));
  EXPECT_TRUE(patterns.is_supported(4));
}
