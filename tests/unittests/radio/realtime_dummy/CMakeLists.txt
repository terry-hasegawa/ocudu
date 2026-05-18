# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI


add_executable(radio_realtime_dummy_test radio_realtime_dummy_test.cpp)
target_link_libraries(radio_realtime_dummy_test ocudu_radio ocudu_support GTest::gtest GTest::gtest_main)
add_test(radio_realtime_dummy_test radio_realtime_dummy_test)

add_executable(radio_realtime_dummy_validator_test radio_realtime_dummy_validator_test.cpp)
target_link_libraries(radio_realtime_dummy_validator_test ocudu_radio ocudu_support GTest::gtest GTest::gtest_main)
add_test(radio_realtime_dummy_validator_test radio_realtime_dummy_validator_test)

