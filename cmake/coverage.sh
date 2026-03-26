#!/bin/sh
# Run lcov filter + genhtml, tolerating Apple LLVM gcov compatibility warnings.
# Usage: coverage.sh <lcov> <genhtml> <build_dir> <src_dir> <coverage_dir>
set -e

LCOV=$1
GENHTML=$2
BUILD_DIR=$3
SRC_DIR=$4
COV_DIR=$5

IGNORE="source,inconsistent,unsupported,format,count,unused,category"

# Filter out noise (tests, deps, system headers)
"$LCOV" \
    --remove "$COV_DIR/coverage.info" \
    "$SRC_DIR/tests/*" \
    "$BUILD_DIR/_deps/*" \
    "/usr/*" "/opt/*" \
    --output-file "$COV_DIR/coverage_filtered.info" \
    --ignore-errors "$IGNORE" || true

# Generate HTML
"$GENHTML" \
    "$COV_DIR/coverage_filtered.info" \
    --output-directory "$COV_DIR/html" \
    --title "PeerCore Coverage" \
    --legend --demangle-cpp \
    --ignore-errors "$IGNORE"

# Print summary
"$LCOV" \
    --summary "$COV_DIR/coverage_filtered.info" \
    --ignore-errors "$IGNORE" || true
