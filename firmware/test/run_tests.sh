#!/usr/bin/env bash
# Host unit tests for Soft Decoder V3 (Linux/macOS/CI).
# Usage: ./run_tests.sh [--sanitize] [suite ...]
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="${TMPDIR:-/tmp}/dcc_soft_decoder_tests"
CC="${DCC_TEST_CC:-gcc}"
SANITIZE=0
ONLY=()

for arg in "$@"; do
    case "$arg" in
        --sanitize) SANITIZE=1 ;;
        -h|--help)
            echo "Usage: $0 [--sanitize] [suite ...]"
            echo "Suites: test_dcc test_settings test_motor test_auxio test_web_util"
            echo "        test_track test_audio test_pinmap test_track_manifest"
            echo "        test_storage test_track_recover test_provision test_selftest"
            echo "        test_web test_sound_graph test_sound_graph_store"
            echo "        test_audio_pack test_main test_concurrency test_motor_concurrency"
            exit 0
            ;;
        *) ONLY+=("$arg") ;;
    esac
done

SUITES=(test_dcc test_settings test_motor test_auxio test_web_util test_track
        test_audio test_pinmap test_track_manifest test_storage test_track_recover
        test_provision test_selftest test_web test_sound_graph test_sound_graph_store
        test_audio_pack test_main test_concurrency test_motor_concurrency)
if [ ${#ONLY[@]} -gt 0 ]; then
    for s in "${ONLY[@]}"; do
        if [[ ! " ${SUITES[*]} " =~ " ${s} " ]]; then
            echo "Unknown test suite: $s" >&2
            exit 2
        fi
    done
    SUITES=("${ONLY[@]}")
fi

mkdir -p "$TMP"
UNITY_SRC="$TMP/unity_clone/src"
if [ ! -f "$UNITY_SRC/unity.c" ]; then
    echo "Fetching Unity test framework..."
    rm -rf "$TMP/unity_clone"
    git clone --depth 1 https://github.com/ThrowTheSwitch/Unity.git "$TMP/unity_clone" || exit 1
fi

INC=(
    -I"$UNITY_SRC"
    -I"$ROOT/test_libs/teststubs/include"
    -I"$ROOT/components/dcc/include"
    -I"$ROOT/components/pinmap/include"
    -I"$ROOT/components/settings/include"
    -I"$ROOT/components/motor/include"
    -I"$ROOT/components/auxio/include"
    -I"$ROOT/components/audio/include"
    -I"$ROOT/components/sound/include"
    -I"$ROOT/components/track/include"
    -I"$ROOT/components/web/include"
    -I"$ROOT/components/storage/include"
    -I"$ROOT/components/provision/include"
    -I"$ROOT/components/selftest/include"
    -I"$ROOT/main"
)
EXTRA=()
if [ "$SANITIZE" -eq 1 ]; then
    EXTRA=(-fsanitize=address,undefined -fno-sanitize-recover=all
           -fno-omit-frame-pointer -g -O1)
    export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1"
    export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
fi

echo "Compiler: $CC"
failed=0
for suite in "${SUITES[@]}"; do
    shopt -s nullglob
    srcs=("$ROOT/test/$suite"/*.c)
    shopt -u nullglob
    if [ ${#srcs[@]} -eq 0 ]; then
        echo "SKIP $suite (no sources)"
        continue
    fi
    exe="$TMP/$suite"
    rm -f "$exe"
    echo "=== $suite ==="
    SUITE_EXTRA=()
    if [ "$suite" = "test_concurrency" ] || [ "$suite" = "test_motor_concurrency" ]; then
        SUITE_EXTRA=(-DMOCK_FREERTOS_REAL_SYNC -pthread)
    fi
    if [ "$suite" = "test_motor_concurrency" ]; then
        SUITE_EXTRA+=("-I$ROOT/test/test_motor")
    fi
    if ! "$CC" "${INC[@]}" "${srcs[@]}" "$UNITY_SRC/unity.c" "${EXTRA[@]}" "${SUITE_EXTRA[@]}" -o "$exe"; then
        echo "BUILD FAILED: $suite" >&2
        failed=$((failed + 1))
        continue
    fi
    if ! ( cd "$ROOT" && "$exe" ); then
        failed=$((failed + 1))
    fi
done

echo
if [ "$failed" -eq 0 ]; then
    echo "ALL TEST SUITES PASSED"
else
    echo "$failed suite(s) FAILED" >&2
    exit 1
fi
