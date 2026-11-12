#!/bin/sh
# Build one platform's single-file .clap from a clean Release configure.
#
#   build.sh <platform> <outdir>      e.g. build.sh macos-arm64 dist
#
# Writes <outdir>/<NAME>-<platform>.clap; the CMake tree goes under <outdir>/build-<platform>.
# Must run on a machine of the platform's OS (each release job runs on its own runner).
set -eu
cd "$(dirname "$0")/../../.."
. "$(dirname "$0")/release.conf"

platform=${1:?usage: build.sh <platform> <outdir>}
out=${2:?usage: build.sh <platform> <outdir>}

case $platform in
  macos-*) want=Darwin ;;
  linux-*) want=Linux ;;
  *) echo "build.sh: unknown platform '$platform' (macos-* or linux-*)" >&2; exit 2 ;;
esac
[ "$(uname -s)" = "$want" ] || {
    echo "build.sh: platform '$platform' needs $(uname -s != $want); build each platform on its own runner" >&2
    exit 1; }

# Extra configure options, generator and environment; word splitting is intended, the
# conf keys hold flag lists. All optional keys default to empty, so a conf without them
# (an older fork) still builds. macOS-only flags/env only apply on macOS (empty elsewhere).
# EXTRA_CMAKE_ARGS lets a local run add its own (CI leaves it unset).
cmake_args="${CMAKE_ARGS:-} ${EXTRA_CMAKE_ARGS:-}"
gen=
if [ -n "${CMAKE_GENERATOR:-}" ]; then gen="-G $CMAKE_GENERATOR"; fi
env_prefix=
if [ "$(uname -s)" = Darwin ]; then
    cmake_args="$cmake_args ${MACOS_CMAKE_ARGS:-}"
    env_prefix=${MACOS_ENV:-}
fi

mkdir -p "$out"
build=$out/build-$platform
rm -rf "$build"
env $env_prefix cmake -S "$CMAKE_SOURCE_DIR" -B "$build" $gen \
    -DCMAKE_BUILD_TYPE=Release $cmake_args
cmake --build "$build" --parallel ${BUILD_TARGETS:+--target} ${BUILD_TARGETS:-}
cp "$build/$CLAP_FILE" "$out/$NAME-$platform.clap"
echo "build.sh: built $out/$NAME-$platform.clap"
