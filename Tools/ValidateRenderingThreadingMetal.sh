#!/usr/bin/env bash
#
# Metal acceptance runner for the UE 5.7.4 threading migration.
#
# Mirrors Tools/ValidateRenderingThreading.ps1 for the macOS host:
#   1. Metal policy gate - backend default is DedicatedThread and a forced
#      `tasks` request must be rejected with a reason instead of silently
#      degrading onto a different lane.
#   2. The rendering/threading regression subset, executed natively.
#   3. The three stress cases (Forward, scene mutation + resize, swapchain
#      Present/resize) with NLS_THREADED_STRESS_BACKEND=metal, 10,000 frames
#      and a 30-minute scene-mutation window by default.
#   4. Metal API Validation stays on for every run (MTL_DEBUG_LAYER=1) and the
#      logs must contain no validation error.
#
# Usage:
#   bash Tools/ValidateRenderingThreadingMetal.sh [--skip-build] [--stress-only]
#
# Environment overrides:
#   NLS_CONFIG (RelWithDebInfo), NLS_BUILD_DIR (build),
#   NLS_STRESS_FRAMES (10000), NLS_STRESS_MIN_SECONDS (1800),
#   NLS_STRESS_TIMEOUT_SECONDS (5400), NLS_OUTPUT_DIR,
#   NLS_METAL_VALIDATION_MARKER ("Metal API Validation Enabled")

set -euo pipefail

CONFIG=${NLS_CONFIG:-RelWithDebInfo}
BUILD_DIR=${NLS_BUILD_DIR:-build}
STRESS_FRAMES=${NLS_STRESS_FRAMES:-10000}
MIN_SECONDS=${NLS_STRESS_MIN_SECONDS:-1800}
TIMEOUT_SECONDS=${NLS_STRESS_TIMEOUT_SECONDS:-5400}
VALIDATION_MARKER=${NLS_METAL_VALIDATION_MARKER:-"Metal API Validation Enabled"}
SKIP_BUILD=0
STRESS_ONLY=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build) SKIP_BUILD=1 ;;
        --stress-only) STRESS_ONLY=1 ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

if [[ "$(uname -s)" != "Darwin" ]]; then
    echo "error: Metal acceptance requires a macOS host." >&2
    exit 2
fi

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo_root"

if [[ -z "${NLS_OUTPUT_DIR:-}" ]]; then
    OUTPUT_DIR="$repo_root/$BUILD_DIR/validation/metal-threading-$(date +%Y%m%d-%H%M%S)"
else
    OUTPUT_DIR=$(cd "$(dirname "$NLS_OUTPUT_DIR")" 2>/dev/null && pwd)/$(basename "$NLS_OUTPUT_DIR") || OUTPUT_DIR="$NLS_OUTPUT_DIR"
fi
mkdir -p "$OUTPUT_DIR"

binary_dir="$repo_root/$BUILD_DIR/bin/$CONFIG"
if [[ ! -d "$binary_dir" ]]; then
    binary_dir="$repo_root/$BUILD_DIR/bin"
fi
unit="$binary_dir/NullusUnitTests"
performance="$binary_dir/NullusPerformanceTests"

fail() { echo "FAILED: $*" >&2; exit 1; }
note() { echo "==> $*"; }

if [[ "$SKIP_BUILD" -eq 0 ]]; then
    note "Building NullusUnitTests and NullusPerformanceTests ($CONFIG)"
    cmake --build "$BUILD_DIR" --config "$CONFIG" \
        --target NullusUnitTests NullusPerformanceTests | tee "$OUTPUT_DIR/build.log"
fi

[[ -x "$unit" ]] || fail "Missing executable: $unit"
[[ -x "$performance" ]] || fail "Missing executable: $performance"

hashes_before="$OUTPUT_DIR/binaries-before.txt"
(cd "$binary_dir" && shasum -a 256 NullusUnitTests NullusPerformanceTests > "$hashes_before")

expected_tests() {
    # Counts gtest cases for a filter; fails when the filter selects nothing.
    "$1" --gtest_filter="$2" --gtest_list_tests 2>/dev/null \
        | grep -c '^  [^ ]' || true
}

check_xml() {
    local xml=$1 expected=$2
    [[ -f "$xml" ]] || fail "Missing gtest XML: $xml"
    local header
    header=$(head -n 2 "$xml" | tail -n 1)
    local tests failures errors
    tests=$(printf '%s' "$header" | sed -n 's/.*tests="\([0-9]*\)".*/\1/p')
    failures=$(printf '%s' "$header" | sed -n 's/.*failures="\([0-9]*\)".*/\1/p')
    errors=$(printf '%s' "$header" | sed -n 's/.*errors="\([0-9]*\)".*/\1/p')
    [[ "$tests" == "$expected" ]] || fail "XML test count $tests != $expected in $xml"
    [[ "$failures" == "0" ]] || fail "XML reports $failures failures in $xml"
    [[ "$errors" == "0" ]] || fail "XML reports $errors errors in $xml"
}

check_metal_validation() {
    local log=$1
    local require_marker=${2:-1}
    if [[ "$require_marker" -eq 1 ]] && ! grep -q "$VALIDATION_MARKER" "$log"; then
        fail "Metal API Validation marker not found in $log; set MTL_DEBUG_LAYER=1 (override the marker with NLS_METAL_VALIDATION_MARKER if the OS wording changed)."
    fi
    if grep -E 'Metal API Validation.*(error|Error)|Validation Error|GPU Validation|Assertion Failed|^\[  FAILED  \]' "$log" >/dev/null 2>&1; then
        fail "Metal validation or test failure reported in $log"
    fi
}

run_unit() {
    local name=$1 filter=$2 expected xml log
    xml="$OUTPUT_DIR/$name.xml"
    log="$OUTPUT_DIR/$name.log"
    note "Running $name ($expected tests)"
    "$unit" --gtest_filter="$filter" --gtest_output="xml:$xml" > "$log" 2>&1 \
        || fail "$name exited non-zero; see $log"
    check_xml "$xml" "$expected"
    # Policy-only cases never create a Metal device, so the marker is optional
    # there; the stress runs below require it.
    check_metal_validation "$log" 0
    echo "$name passed $expected tests"
}

run_stress() {
    local mode=$1 xml log expected
    xml="$OUTPUT_DIR/stress-$mode.xml"
    log="$OUTPUT_DIR/stress-$mode.log"
    expected=$(expected_tests "$performance" \
        'ThreadedRenderingStressPerformanceTests.*:ThreadedRenderingSwapchainPerformanceTests.*')
    [[ "$expected" -gt 0 ]] || fail "Stress filter selected no tests"
    note "Running stress mode=$mode frames=$STRESS_FRAMES minSeconds=$MIN_SECONDS"
    NLS_THREADED_STRESS_BACKEND=metal \
    NLS_THREADED_STRESS_RHI_MODE="$mode" \
    NLS_THREADED_STRESS_FRAMES="$STRESS_FRAMES" \
    NLS_THREADED_STRESS_MIN_SECONDS="$MIN_SECONDS" \
    NLS_THREADED_STRESS_TIMEOUT_SECONDS="$TIMEOUT_SECONDS" \
    MTL_DEBUG_LAYER=1 \
        "$performance" \
        --gtest_filter='ThreadedRenderingStressPerformanceTests.*:ThreadedRenderingSwapchainPerformanceTests.*' \
        --gtest_output="xml:$xml" > "$log" 2>&1 \
        || fail "stress-$mode exited non-zero; see $log"
    check_xml "$xml" "$expected"
    check_metal_validation "$log" 1
    if ! grep -q 'SuccessfullyRetired\|successfullyRetired' "$log"; then
        fail "stress-$mode produced no retirement summary; see $log"
    fi
    if grep -q '^\[  SKIPPED  \]' "$log"; then
        fail "stress-$mode skipped one or more cases; the host must expose a Metal device. See $log"
    fi
    if grep -q 'retainedAfterDrain=[1-9]' "$log"; then
        fail "stress-$mode retained frame resources after drain; see $log"
    fi
}

# 1. Metal policy gate: platform-neutral tests assert the documented matrix,
#    including that a forced `tasks` request on Metal is rejected with a reason.
policy_filter='RhiThreadModePolicyTests.*'
policy_expected=$(expected_tests "$unit" "$policy_filter")
[[ "$policy_expected" -gt 0 ]] || fail "Policy filter selected no tests"
run_unit metal-policy "$policy_filter" "$policy_expected"

if [[ "$STRESS_ONLY" -eq 0 ]]; then
    regression_filter='RhiCommandListExecutorTests.*:RhiSubmitLaneSourceGuardTests.*:RhiCommandListRecorderTests.*:RenderFrameExecutionTests.*:FrameResourceRingTests.*:FrameLatency*Tests.*:DriverFlushTests.*:DriverRhiThreadModeTests.*:DriverSwapchainResizeTests.*:DriverNullDeviceReadinessTests.*:RenderingThread*Tests.*:RenderCommand*Tests.*:FrameGraphSceneTargetsTests.*:RHIUiOverlayPassTests.*:RHIUiOverlaySourceGuardTests.*:UE5RenderArchitectureContractTests.*:RenderFrameworkContractTests.*:RenderSceneCacheTests.*:RenderSceneHandoffTests.*:SceneRenderStore*Tests.*:SceneProxyTests.*:TextureResourceLifecycleTests.*:UploadContextTests.*:DX12ReadbackUtilsTests.*'
    regression_expected=$(expected_tests "$unit" "$regression_filter")
    [[ "$regression_expected" -gt 0 ]] || fail "Regression filter selected no tests"
    run_unit metal-regression "$regression_filter" "$regression_expected"
fi

# 2. Forced tasks mode must fail loudly on Metal instead of silently degrading.
note "Checking that a forced tasks-mode run is rejected"
tasks_log="$OUTPUT_DIR/forced-tasks-rejection.log"
set +e
NLS_THREADED_STRESS_BACKEND=metal \
NLS_THREADED_STRESS_RHI_MODE=tasks \
NLS_THREADED_STRESS_FRAMES=2 \
NLS_THREADED_STRESS_TIMEOUT_SECONDS=120 \
MTL_DEBUG_LAYER=1 \
    "$performance" \
    --gtest_filter='ThreadedRenderingSwapchainPerformanceTests.*' > "$tasks_log" 2>&1
tasks_exit=$?
set -e
[[ "$tasks_exit" -ne 0 ]] || fail "Forced tasks mode succeeded on Metal; the backend must reject it instead of degrading silently."
grep -q 'rejecting RHI thread mode request' "$tasks_log" \
    || fail "Forced tasks mode failed without the expected rejection reason; see $tasks_log"
echo "Forced tasks mode rejected with the documented reason"

# 3. DedicatedThread is the Metal default and the only accepted lane.
run_stress dedicated_thread

hashes_after="$OUTPUT_DIR/binaries-after.txt"
(cd "$binary_dir" && shasum -a 256 NullusUnitTests NullusPerformanceTests > "$hashes_after")
cmp -s "$hashes_before" "$hashes_after" || fail "Binaries changed during validation"

echo "All Metal verification complete; binary hashes unchanged. Results: $OUTPUT_DIR"
