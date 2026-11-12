#!/bin/sh
# Run this repo's Oscairn test set against a release build (the Test step of
# .github/workflows/oscairn.yml: sfizz's tests, the CLAP plugin's tests, the privacy check).
#
#   test.sh <platform> <outdir>       e.g. test.sh macos-arm64 dist
#
# Reads the build tree build.sh left under <outdir>; runs from the repository root so
# the tests find their files at the paths they read them from.
set -eu
cd "$(dirname "$0")/../../.."
. "$(dirname "$0")/release.conf"

platform=${1:?usage: test.sh <platform> <outdir>}
out=${2:?usage: test.sh <platform> <outdir>}
build=$out/build-$platform
bin=$build/library/bin
[ -x "$bin/sfizz_tests" ] || { echo "test.sh: no $bin/sfizz_tests; run build.sh first" >&2; exit 1; }

"$bin/sfizz_tests"
"$bin/oscairn_sampler_clap_tests" "$build/$CLAP_FILE"
clap/tests/privacy-check.sh "$bin/oscairn_sampler_privacy_host" "$build/$CLAP_FILE" \
    org.oscairn.sampler --library org.oscairn.privacy-test="$PWD/tests/TestFiles" --state clap/tests/privacy-state.txt
echo "test.sh: $platform tests pass"
