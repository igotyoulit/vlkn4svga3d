#!/usr/bin/env bash
# ==============================================================================
# harness_loop.sh -- tight feedback-loop driver for the vlkn4svga3d test harnesses
#
# Repeatedly:  build -> run ICD-free translator suite -> run lavapipe suites
#              -> summarize pass/fail -> stop on NEW regressions.
#
# Fails fast with clear logs; never silently skips a suite.
#
# Usage:
#   ./scripts/harness_loop.sh [--iterations N] [--fail-fast] [--skip-build]
#   make harness-loop [HARNESS_LOOP_ARGS="--iterations 3 --fail-fast"]
#
# Environment:
#   HARNESS_ICD      Path to a Vulkan ICD JSON (default: the svga-vlkn-harness
#                    lavapipe prefix used by local development). Missing ICD is
#                    a hard error, not a skip.
#   SPIRV_TOOLS_DIR  Directory holding spirv-val / spirv-dis. Prepended to PATH
#                    so translator tests validate instead of SKIP-ing.
#   HARNESS_TIMEOUT  Per-suite timeout in seconds (default 900).
# ==============================================================================
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

ITERATIONS=1
FAIL_FAST=0
SKIP_BUILD=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --iterations)
            if [[ $# -lt 2 || ! "$2" =~ ^[1-9][0-9]*$ || ${#2} -gt 9 ]]; then
                echo "--iterations requires a positive integer (1..999999999)" >&2
                exit 2
            fi
            ITERATIONS="$2"; shift 2 ;;
        --fail-fast)  FAIL_FAST=1; shift ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        *) echo "Unknown argument: $1" >&2; exit 2 ;;
    esac
done

RESULTS_DIR="$ROOT_DIR/.harness-loop"
mkdir -p "$RESULTS_DIR"

# ---------------------------------------------------------------- env: ICD ---
DEFAULT_ICD="$HOME/workspace/svga-vlkn-harness/prefix/share/vulkan/icd.d/lvp_icd.x86_64.json"
HARNESS_ICD="${HARNESS_ICD:-$DEFAULT_ICD}"
if [[ ! -f "$HARNESS_ICD" ]]; then
    echo "[harness-loop] FATAL: Vulkan ICD not found: $HARNESS_ICD" >&2
    echo "[harness-loop] Set HARNESS_ICD to a valid ICD JSON (e.g. a lavapipe icd.d file)." >&2
    exit 1
fi
export VK_ICD_FILENAMES="$HARNESS_ICD"
echo "[harness-loop] Using Vulkan ICD: $VK_ICD_FILENAMES"

# ------------------------------------------------------- env: SPIRV-Tools ---
DEFAULT_SPIRV_DIR="$HOME/workspace/spirv-tools/build/tools"
SPIRV_TOOLS_DIR="${SPIRV_TOOLS_DIR:-$DEFAULT_SPIRV_DIR}"
if [[ -x "$SPIRV_TOOLS_DIR/spirv-val" ]]; then
    export PATH="$SPIRV_TOOLS_DIR:$PATH"
    echo "[harness-loop] spirv-val enabled: $SPIRV_TOOLS_DIR/spirv-val"
else
    echo "[harness-loop] WARNING: spirv-val not found in $SPIRV_TOOLS_DIR;" >&2
    echo "[harness-loop] WARNING: translator SPIR-V validation checks will SKIP, not validate." >&2
    echo "[harness-loop] WARNING: set SPIRV_TOOLS_DIR to a dir containing spirv-val." >&2
fi

HARNESS_TIMEOUT="${HARNESS_TIMEOUT:-900}"

# Suites: name -> needs-Vulkan-ICD (all lavapipe suites do; the translator
# suite is ICD-free but harmless to run with the ICD exported).
ICD_FREE_SUITES=( test_translator_novulkan )
LAVAPIPE_SUITES=(
    test_buffer_ordering
    test_shader_translation
    test_real_vulkan
    test_svga3_vlkn
    test_shader_execution
    test_guest_memory
    test_malformed_inputs
    test_verified_rendering
    test_presentation
    test_qemu_integration
)

run_suite() {
    local suite="$1" log="$2"
    echo "[harness-loop] --- running $suite ---"
    if timeout "$HARNESS_TIMEOUT" "./bin/$suite" >"$log" 2>&1; then
        echo "[harness-loop] $suite: PASS"
        return 0
    else
        local code=$?
        if [[ $code -eq 124 ]]; then
            echo "[harness-loop] $suite: FAIL (timeout after ${HARNESS_TIMEOUT}s) -- log: $log"
        else
            echo "[harness-loop] $suite: FAIL (exit $code) -- log: $log"
        fi
        echo "[harness-loop] tail of $log:"
        tail -15 "$log" | sed 's/^/[harness-loop]   /'
        return 1
    fi
}

overall_rc=0
prev_results=""

for (( iter=1; iter<=ITERATIONS; iter++ )); do
    iter_tag=$(printf "%02d" "$iter")
    iter_dir="$RESULTS_DIR/iter-$iter_tag"
    mkdir -p "$iter_dir"
    echo "=================================================================="
    echo "[harness-loop] iteration $iter/$ITERATIONS"
    echo "=================================================================="

    # ------------------------------------------------------------- build ---
    if [[ $SKIP_BUILD -eq 0 ]]; then
        echo "[harness-loop] building (make -j$(nproc) all) ..."
        if ! make -j"$(nproc)" all >"$iter_dir/build.log" 2>&1; then
            echo "[harness-loop] FATAL: build failed -- log: $iter_dir/build.log" >&2
            tail -25 "$iter_dir/build.log" | sed 's/^/[harness-loop]   /' >&2
            exit 1
        fi
        echo "[harness-loop] build: OK"
    else
        echo "[harness-loop] build: skipped (--skip-build)"
    fi

    # ------------------------------------------------------------- suites --
    results_tsv="$iter_dir/results.tsv"
    : > "$results_tsv"
    iter_failed=0
    for suite in "${ICD_FREE_SUITES[@]}" "${LAVAPIPE_SUITES[@]}"; do
        if [[ ! -x "bin/$suite" ]]; then
            echo "[harness-loop] FATAL: missing test binary bin/$suite (build produced nothing?)" >&2
            exit 1
        fi
        start=$(date +%s)
        if run_suite "$suite" "$iter_dir/$suite.log"; then
            status="PASS"
        else
            status="FAIL"
            iter_failed=1
        fi
        elapsed=$(( $(date +%s) - start ))
        printf "%s\t%s\t%d\n" "$suite" "$status" "$elapsed" >> "$results_tsv"
        if [[ $FAIL_FAST -eq 1 && $iter_failed -eq 1 ]]; then
            echo "[harness-loop] --fail-fast: stopping iteration at first failure."
            break
        fi
    done

    # ----------------------------------------------------------- summary ---
    echo "------------------------------------------------------------------"
    echo "[harness-loop] iteration $iter summary:"
    printf "[harness-loop]   %-28s %-6s %s\n" "SUITE" "STATUS" "SECS"
    while IFS=$'\t' read -r suite status secs; do
        printf "[harness-loop]   %-28s %-6s %s\n" "$suite" "$status" "$secs"
    done < "$results_tsv"
    echo "------------------------------------------------------------------"

    # --------------------------------------- regression vs previous iter ---
    if [[ -n "$prev_results" && -f "$prev_results" ]]; then
        new_regressions=0
        while IFS=$'\t' read -r suite status _secs; do
            prev_status=$(awk -F'\t' -v s="$suite" '$1==s{print $2}' "$prev_results")
            if [[ "$prev_status" == "PASS" && "$status" == "FAIL" ]]; then
                echo "[harness-loop] NEW REGRESSION: $suite passed in previous iteration, FAILS now."
                new_regressions=1
            fi
        done < "$results_tsv"
        if [[ $new_regressions -eq 1 ]]; then
            echo "[harness-loop] FATAL: stopping loop on new regression(s)." >&2
            exit 1
        fi
        echo "[harness-loop] no new regressions vs previous iteration."
    fi
    prev_results="$results_tsv"

    if [[ $iter_failed -eq 1 ]]; then
        echo "[harness-loop] iteration $iter finished with failures (no new regressions vs prior iteration)."
        overall_rc=1
    else
        echo "[harness-loop] iteration $iter: ALL SUITES PASS."
    fi
done

echo "[harness-loop] done. results in $RESULTS_DIR/"
exit $overall_rc
