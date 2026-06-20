# SPDX-FileCopyrightText: 2026 OCUDU ISAC sensing PoC
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
#
# ISAC sensing PoC (Block A) — read-only UL DMRS channel-estimate PHY TAP.
# Disposable prototype: this whole directory can be deleted to fully remove the feature.
# Only built when ENABLE_ISAC_TAP=ON (see top-level CMakeLists.txt).

if (NOT ZEROMQ_FOUND)
    message(FATAL_ERROR
            "ENABLE_ISAC_TAP requires ZeroMQ. Set ENABLE_ZEROMQ=ON and install libzmq (e.g. libzmq3-dev).")
endif ()

add_library(ocudu_isac STATIC
        isac_csi_payload.cpp
        isac_zmq_sink.cpp
        isac_pusch_estimator_tap_factory.cpp)

target_link_libraries(ocudu_isac ${ZEROMQ_LIBRARIES} fmt ocudulog ocuduvec ocudu_support)
