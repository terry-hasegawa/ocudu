// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "lib/du/du_high/du_manager/ran_resource_management/du_meas_config_manager.h"
#include "ocudu/asn1/rrc_nr/sys_info.h"
#include "ocudu/ran/ssb/ssb_properties.h"
#include "fmt/format.h"
#include <gtest/gtest.h>

using namespace ocudu;
using namespace odu;
using namespace asn1::rrc_nr;

namespace {

using smtc_duration = ssb_mtc_s::dur_opts::options;
using offset_range  = std::pair<uint8_t, uint8_t>;

struct meas_gap_test_params {
  subcarrier_spacing         pcell_scs;
  ssb_periodicity            smtc_period;
  offset_range               smtc_offsets; // half-open offset range [first, second)
  smtc_duration              smtc_dur;
  meas_gap_length            expected_mgl;
  meas_gap_repetition_period expected_mgrp;
};

ssb_mtc_s make_smtc(ssb_periodicity period, uint8_t offset, smtc_duration dur)
{
  ssb_mtc_s smtc;
  smtc.dur.value = dur;
  switch (period) {
    case ssb_periodicity::ms5:
      smtc.periodicity_and_offset.set_sf5() = offset;
      break;
    case ssb_periodicity::ms10:
      smtc.periodicity_and_offset.set_sf10() = offset;
      break;
    case ssb_periodicity::ms20:
      smtc.periodicity_and_offset.set_sf20() = offset;
      break;
    case ssb_periodicity::ms40:
      smtc.periodicity_and_offset.set_sf40() = offset;
      break;
    case ssb_periodicity::ms80:
      smtc.periodicity_and_offset.set_sf80() = offset;
      break;
    case ssb_periodicity::ms160:
      smtc.periodicity_and_offset.set_sf160() = offset;
      break;
  }
  return smtc;
}

std::string param_name(const ::testing::TestParamInfo<meas_gap_test_params>& info)
{
  const auto& p = info.param;
  return fmt::format("scs_{:_>3}kHz_period_{:_>3}sf_offsets_{:_>3}_to_{:_>3}_duration_{:01}sf",
                     scs_to_khz(p.pcell_scs),
                     fmt::underlying(p.smtc_period),
                     p.smtc_offsets.first,
                     p.smtc_offsets.second,
                     static_cast<unsigned>(p.smtc_dur) + 1);
}

class du_meas_config_manager_create_meas_gap_test : public ::testing::TestWithParam<meas_gap_test_params>
{};

TEST_P(du_meas_config_manager_create_meas_gap_test, gap_matches_expected_mgl_mgrp_and_offset)
{
  const meas_gap_test_params& p = GetParam();
  for (uint8_t off = p.smtc_offsets.first; off < p.smtc_offsets.second; ++off) {
    SCOPED_TRACE(fmt::format("smtc_offset={}", off));
    const ssb_mtc_s       smtc = make_smtc(p.smtc_period, off, p.smtc_dur);
    const meas_gap_config gap  = create_meas_gap(p.pcell_scs, smtc);

    EXPECT_EQ(gap.offset, off);
    EXPECT_EQ(gap.mgl, p.expected_mgl);
    EXPECT_EQ(gap.mgrp, p.expected_mgrp);
  }
}

// SMTC Duration to Measurement Gap Length test
INSTANTIATE_TEST_SUITE_P(duration_to_mgl,
                         du_meas_config_manager_create_meas_gap_test,
                         ::testing::Values(
                             // sf1 + 15 kHz -> ms3.
                             meas_gap_test_params{subcarrier_spacing::kHz15,
                                                  ssb_periodicity::ms20,
                                                  {0, 20},
                                                  smtc_duration::sf1,
                                                  meas_gap_length::ms3,
                                                  meas_gap_repetition_period::ms20},
                             // sf1 + 30 kHz -> ms1.5.
                             meas_gap_test_params{subcarrier_spacing::kHz30,
                                                  ssb_periodicity::ms20,
                                                  {0, 20},
                                                  smtc_duration::sf1,
                                                  meas_gap_length::ms1dot5,
                                                  meas_gap_repetition_period::ms20},
                             // sf1 + 60 kHz -> ms1.5.
                             meas_gap_test_params{subcarrier_spacing::kHz60,
                                                  ssb_periodicity::ms20,
                                                  {0, 20},
                                                  smtc_duration::sf1,
                                                  meas_gap_length::ms1dot5,
                                                  meas_gap_repetition_period::ms20},
                             // sf2 -> ms3.
                             meas_gap_test_params{subcarrier_spacing::kHz30,
                                                  ssb_periodicity::ms20,
                                                  {0, 20},
                                                  smtc_duration::sf2,
                                                  meas_gap_length::ms3,
                                                  meas_gap_repetition_period::ms20},
                             // sf3 -> ms4.
                             meas_gap_test_params{subcarrier_spacing::kHz30,
                                                  ssb_periodicity::ms20,
                                                  {0, 20},
                                                  smtc_duration::sf3,
                                                  meas_gap_length::ms4,
                                                  meas_gap_repetition_period::ms20},
                             // sf4 -> ms6.
                             meas_gap_test_params{subcarrier_spacing::kHz30,
                                                  ssb_periodicity::ms20,
                                                  {0, 20},
                                                  smtc_duration::sf4,
                                                  meas_gap_length::ms6,
                                                  meas_gap_repetition_period::ms20},
                             // sf5 -> ms6.
                             meas_gap_test_params{subcarrier_spacing::kHz30,
                                                  ssb_periodicity::ms20,
                                                  {0, 20},
                                                  smtc_duration::sf5,
                                                  meas_gap_length::ms6,
                                                  meas_gap_repetition_period::ms20}),
                         param_name);

// SMTC periodicity to Measurement Gap Repetition Period test
INSTANTIATE_TEST_SUITE_P(periodicity_to_mgrp,
                         du_meas_config_manager_create_meas_gap_test,
                         ::testing::Values(meas_gap_test_params{subcarrier_spacing::kHz30,
                                                                ssb_periodicity::ms5,
                                                                {0, 5},
                                                                smtc_duration::sf5,
                                                                meas_gap_length::ms6,
                                                                meas_gap_repetition_period::ms20},
                                           meas_gap_test_params{subcarrier_spacing::kHz30,
                                                                ssb_periodicity::ms10,
                                                                {0, 10},
                                                                smtc_duration::sf5,
                                                                meas_gap_length::ms6,
                                                                meas_gap_repetition_period::ms20},
                                           meas_gap_test_params{subcarrier_spacing::kHz30,
                                                                ssb_periodicity::ms20,
                                                                {0, 20},
                                                                smtc_duration::sf5,
                                                                meas_gap_length::ms6,
                                                                meas_gap_repetition_period::ms20},
                                           meas_gap_test_params{subcarrier_spacing::kHz30,
                                                                ssb_periodicity::ms40,
                                                                {0, 40},
                                                                smtc_duration::sf5,
                                                                meas_gap_length::ms6,
                                                                meas_gap_repetition_period::ms40},
                                           meas_gap_test_params{subcarrier_spacing::kHz30,
                                                                ssb_periodicity::ms80,
                                                                {0, 80},
                                                                smtc_duration::sf5,
                                                                meas_gap_length::ms6,
                                                                meas_gap_repetition_period::ms80},
                                           meas_gap_test_params{subcarrier_spacing::kHz30,
                                                                ssb_periodicity::ms160,
                                                                {0, 160},
                                                                smtc_duration::sf5,
                                                                meas_gap_length::ms6,
                                                                meas_gap_repetition_period::ms160}),
                         param_name);

} // namespace
