#!/usr/bin/env bash
# Phase 2 measurement runbook for the A100X box (GA100, sm_80, discrete PCIe).
#
# Runs the OCUDU CUDA-fork GPU/CPU baseline battery and collects raw outputs
# into one results directory. Run it from the ROOT OF THE FORK CHECKOUT
# (cuda_accelerated_ocudu, branch cuda_accel.26_04), with the fork already
# built as described in docs/investigation/phase0_bringup.md:
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_CUDA=ON \
#         -DCMAKE_CUDA_ARCHITECTURES=80 [toolkit overrides if needed]
#
# Usage:
#   bash phase2_a100x_runbook.sh [--build-dir build] [--out-dir DIR] [--quick]
#
# Steps (each guarded; a failing step logs and continues):
#   0. environment capture           -> env.txt
#   1. sm_80 SASS + GPU-path gate    -> gate.txt          (aborts only if GPU path falls back)
#   2. PUSCH processor GPU vs CPU    -> pusch_proc_{cpu,gpu}_{4port_nlayer,4port_mimo}.txt
#   3. Type-1 DMRS UL/DL sweeps      -> type1_default/  type1_managed_grid/
#      (second pass forces managed grids on discrete = the memory-policy experiment)
#   4. OFH BFP compression matrix    -> ofh_matrix/       (tests the OFH auto=CUDA suspect)
#   5. SRS latency GPU vs CPU        -> srs_latency.txt
#   6. Nsight Systems capture        -> nsys/             (skipped if nsys absent)
#   7. CUDA correctness subset       -> ctest_cuda.txt

set -uo pipefail

BUILD_DIR="build"
OUT_DIR=""
QUICK=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --out-dir)   OUT_DIR="$2";   shift 2 ;;
    --quick)     QUICK=1;        shift   ;;
    *) echo "unknown arg: $1" >&2; exit 2 ;;
  esac
done
[[ -z "${OUT_DIR}" ]] && OUT_DIR="docs_investigation_results_$(hostname)_$(date +%Y%m%d_%H%M%S)"
mkdir -p "${OUT_DIR}"
log() { echo "[runbook $(date +%H:%M:%S)] $*" | tee -a "${OUT_DIR}/runbook.log"; }
step() { log "=== $1 ==="; }

PUSCH_BENCH="${BUILD_DIR}/tests/benchmarks/phy/upper/channel_processors/pusch/pusch_processor_benchmark"
PDSCH_BENCH="${BUILD_DIR}/tests/benchmarks/phy/upper/channel_processors/pdsch_processor_benchmark"

# --- 0. environment ---------------------------------------------------------
step "0 environment"
{
  nvidia-smi
  echo; nvcc --version 2>/dev/null || true
  echo; nvidia-smi --query-gpu=name,driver_version,compute_cap,pcie.link.gen.current,pcie.link.width.current --format=csv
  echo; lscpu | head -25
  echo; git -C . rev-parse HEAD
  echo; grep -E '^CMAKE_CUDA_ARCHITECTURES|^ENABLE_CUDA|^CMAKE_BUILD_TYPE' "${BUILD_DIR}/CMakeCache.txt"
} > "${OUT_DIR}/env.txt" 2>&1
log "env captured; GPU: $(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1)"

# --- 1. gate: sm_80 SASS present AND GPU path actually taken ----------------
step "1 gate"
{
  cuobjdump --list-elf "${BUILD_DIR}/lib/phy/cuda/libocudu_phy_cuda.a" 2>/dev/null | grep -oE 'sm_[0-9]+' | sort | uniq -c
  echo "--- -G short run (must NOT print 'falling back to CPU'):"
  OCUDU_PUSCH_ACCELERATION_TRACE=1 "${PUSCH_BENCH}" -m latency -B 2 -R 1 -T 1 \
    -P scs30_100MHz_256qam_rvall_1port_1layer -G 2>&1 | head -20
} > "${OUT_DIR}/gate.txt" 2>&1
if grep -q "falling back to CPU" "${OUT_DIR}/gate.txt"; then
  log "FATAL: GPU pipeline fell back to CPU — fix driver/toolkit before measuring. See gate.txt"
  exit 1
fi
log "gate passed: GPU path taken"

# --- 2. PUSCH processor benchmark, GPU vs CPU, our target shapes ------------
step "2 pusch_processor_benchmark"
REPS=$(( QUICK == 1 ? 5 : 20 ))
BATCH=$(( QUICK == 1 ? 20 : 100 ))
for prof in scs30_100MHz_256qam_rv0_4port_nlayer scs30_100MHz_256qam_rv0_4port_mimo; do
  "${PUSCH_BENCH}" -m latency -B "${BATCH}" -R "${REPS}" -T 1 -P "${prof}"    > "${OUT_DIR}/pusch_proc_cpu_${prof}.txt" 2>&1 || log "WARN: cpu ${prof} failed"
  "${PUSCH_BENCH}" -m latency -B "${BATCH}" -R "${REPS}" -T 1 -P "${prof}" -G > "${OUT_DIR}/pusch_proc_gpu_${prof}.txt" 2>&1 || log "WARN: gpu ${prof} failed"
done
"${PDSCH_BENCH}" -m latency -B "${BATCH}" -R "${REPS}" -T 1 -P 4port_4layer_scs30_100MHz_256qam > "${OUT_DIR}/pdsch_proc_cpu_4p4l.txt" 2>&1 || log "WARN: pdsch cpu failed"

# --- 3. README-style type-1 DMRS UL/DL sweeps, incl. 4x4, two grid policies -
step "3 type1 sweeps (default grid policy: pinned-on-discrete)"
T1_COMMON=( --build-dir "${BUILD_DIR}" --no-build --prbs "51,106,273" --topologies "1x1,2x4,4x4,4x8" )
[[ ${QUICK} == 1 ]] && T1_COMMON+=( --quick )
bash scripts/cuda_accel/run_type1_dmrs_ul_dl_gpu_cpu_sweeps.sh "${T1_COMMON[@]}" \
  --out-dir "${OUT_DIR}/type1_default" >> "${OUT_DIR}/runbook.log" 2>&1 || log "WARN: type1 default sweep failed"
step "3b type1 sweeps, forced managed grids (discrete memory-policy experiment)"
bash scripts/cuda_accel/run_type1_dmrs_ul_dl_gpu_cpu_sweeps.sh "${T1_COMMON[@]}" \
  --resource-grid-memory managed --device-grid-memory managed \
  --out-dir "${OUT_DIR}/type1_managed_grid" >> "${OUT_DIR}/runbook.log" 2>&1 || log "WARN: type1 managed sweep failed"

# --- 4. OFH BFP compression matrix (tests the auto=CUDA-on-discrete suspect) -
step "4 OFH compression matrix"
bash tests/benchmarks/ofh/run_ofh_compression_matrix.sh --build-dir "${BUILD_DIR}" \
  --output-dir "${OUT_DIR}/ofh_matrix" > "${OUT_DIR}/ofh_matrix_summary.txt" 2>&1 || log "WARN: ofh matrix failed"

# --- 5. SRS latency ----------------------------------------------------------
step "5 SRS latency"
bash scripts/cuda_accel/run_srs_latency_benchmark.sh --build-dir "${BUILD_DIR}" \
  > "${OUT_DIR}/srs_latency.txt" 2>&1 || log "WARN: srs latency failed"

# --- 6. Nsight Systems (optional) --------------------------------------------
step "6 nsight"
if command -v nsys >/dev/null 2>&1; then
  mkdir -p "${OUT_DIR}/nsys"
  nsys profile -t cuda,nvtx,osrt -o "${OUT_DIR}/nsys/pusch_gpu_4port" --force-overwrite true \
    "${PUSCH_BENCH}" -m latency -B 20 -R 3 -T 1 -P scs30_100MHz_256qam_rv0_4port_mimo -G \
    > "${OUT_DIR}/nsys/pusch_gpu_4port.txt" 2>&1 || log "WARN: nsys capture failed"
  nsys stats --report cuda_gpu_kern_sum,cuda_gpu_mem_time_sum,cuda_api_sum \
    "${OUT_DIR}/nsys/pusch_gpu_4port.nsys-rep" \
    > "${OUT_DIR}/nsys/pusch_gpu_4port_stats.txt" 2>&1 || log "WARN: nsys stats failed"
else
  log "nsys not found — skipping (install nsight-systems for the transfer/kernel split)"
fi

# --- 7. correctness subset ----------------------------------------------------
step "7 ctest CUDA subset"
ctest --test-dir "${BUILD_DIR}" --output-on-failure -j1 -R \
'^(ofdm_demodulator_cuda_test|ofdm_prach_demodulator_cuda_test|pdxch_baseband_modulator_cuda_test|ldpc_encoder_gpu_cpu_test|ldpc_decoder_gpu_cpu_test|prach_detector_cuda_test|pusch_gpu_cpu_comparison_test|pdsch_gpu_e2e_test|pusch_e2e_pipeline_test|pusch_resident_dematch_scramble_test)$' \
  > "${OUT_DIR}/ctest_cuda.txt" 2>&1 || log "WARN: some CUDA tests failed — see ctest_cuda.txt"

step "DONE"
log "Results in ${OUT_DIR} — package with: tar czf ${OUT_DIR}.tar.gz ${OUT_DIR}"
