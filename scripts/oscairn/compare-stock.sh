#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause
#
# Stock-vs-fork gate (doc/oscairn-plan.md, section 3): renders SFZ patches through stock
# sfizz and through this tree with the same event script, and requires bit-identical
# 32-bit float output.
#
# usage: scripts/oscairn/compare-stock.sh <work dir> [fork source dir]
#
# The stock tree is upstream sfizz at STOCK_COMMIT, unmodified. Both trees are built as
# shared libraries with the same compiler and flags; the flag below only lets current
# compilers accept the stock tree's vendored atomic_queue. Each render runs in its own
# process (sfizz's random generators restart with the process). Every render runs twice: a
# patch whose stock renders differ between runs has no reference and is reported, not
# compared; a fork that differs between runs where stock does not fails.
set -euo pipefail

STOCK_COMMIT=f5c6e29f23b8057867c08e88f5f6ac6738baa30b
STOCK_URL=https://github.com/sfztools/sfizz.git

work=$(mkdir -p "$1" && cd "$1" && pwd)
fork=$(cd "${2:-$(dirname "$0")/../..}" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
cxx=${CXX:-c++}
flags="-Wno-unknown-warning-option -Wno-missing-template-arg-list-after-template-kw"
[[ $("$cxx" --version) == *GCC* || $("$cxx" --version) == *"Free Software"* ]] && flags=""

if [[ ! -d "$work/stock-src/.git" ]]; then
    git clone --quiet "$STOCK_URL" "$work/stock-src"
fi
if [[ $(git -C "$work/stock-src" rev-parse HEAD) != "$STOCK_COMMIT" ]]; then
    git -C "$work/stock-src" fetch --quiet origin "$STOCK_COMMIT" || true
    git -C "$work/stock-src" checkout --quiet "$STOCK_COMMIT"
fi
git -C "$work/stock-src" submodule --quiet update --init --recursive
if [[ -n $(git -C "$work/stock-src" status --porcelain --untracked-files=no) ]]; then
    echo "stock tree has local changes" >&2; exit 1
fi

build() { # <source> <build dir> <render binary>
    cmake -S "$1" -B "$2" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSFIZZ_SHARED=ON \
        -DSFIZZ_TESTS=OFF -DSFIZZ_RENDER=OFF -DSFIZZ_JACK=OFF -DSFIZZ_DEMOS=OFF -DSFIZZ_DEVTOOLS=OFF \
        -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" > "$2.configure.log"
    cmake --build "$2" --target sfizz_shared > "$2.build.log"
    local lib
    lib=$(dirname "$(find "$2" -name 'libsfizz.so' -o -name 'libsfizz.dylib' | head -1)")
    "$cxx" -O2 -std=c++14 -I"$1/src" "$here/render.cpp" -L"$lib" -lsfizz -Wl,-rpath,"$lib" -o "$3"
}
build "$work/stock-src" "$work/stock-build" "$work/render-stock"
build "$fork" "$work/fork-build" "$work/render-fork"

# abseil seeds its hash tables from an address, and sfizz's output depends on their iteration
# order for some patches (newdrums_flat.sfz), so renders run with ASLR off.
if [[ $(uname) == Darwin ]]; then
    "$cxx" -O2 "$here/no-aslr.cpp" -o "$work/no-aslr"
    run=("$work/no-aslr")
else
    run=(setarch "$(uname -m)" -R)
fi

# The fork's copies of the patches are the inputs for both builds. Left out because stock
# itself does not render them reproducibly, even with ASLR off: the include-loop parser
# fixtures (crashes, or different output on every run) and newdrums_flat.sfz (2 of 12 runs
# differed from the rest on 2026-11-01, starting at its fifth note).
patches=()
while IFS= read -r p; do patches+=("$p"); done < <(
    find "$fork/tests/TestFiles" "$here/patches" -name '*.sfz' \
        ! -name 'included_loop*.sfz' ! -name 'root_loop.sfz' ! -name 'newdrums_flat.sfz' | LC_ALL=C sort)

configs=("256 48000 2" "37 44100 1" "512 96000 10")
compared=0 skipped=0 unstable=0 failed=0
mkdir -p "$work/out"
render() { # <binary> <patch> <out> <config>: prints the exit code, or the output's hash
    local r=0
    read -r block rate quality <<< "$4"
    "${run[@]}" "$1" "$2" "$3" "$block" "$rate" "$quality" > /dev/null 2>&1 || r=$?
    if [[ $r -ne 0 ]]; then echo "exit $r"; else cksum < "$3" | cut -d' ' -f1-2; fi
}
for p in "${patches[@]}"; do
    for c in "${configs[@]}"; do
        name="${p#"$fork"/} [$c]"
        s1=$(render "$work/render-stock" "$p" "$work/out/s1.f32" "$c")
        s2=$(render "$work/render-stock" "$p" "$work/out/s2.f32" "$c")
        f1=$(render "$work/render-fork" "$p" "$work/out/f1.f32" "$c")
        f2=$(render "$work/render-fork" "$p" "$work/out/f2.f32" "$c")
        if [[ "$s1" != "$s2" ]]; then
            # Stock gives different results from run to run: no reference to compare with
            echo "UNSTABLE IN STOCK $name"; unstable=$((unstable + 1)); continue
        fi
        if [[ "$f1" != "$f2" ]]; then
            echo "FAIL $name: fork differs between runs, stock does not"; failed=$((failed + 1)); continue
        fi
        if [[ "$s1" == exit* || "$f1" == exit* ]]; then
            if [[ "$s1" == "$f1" ]]; then skipped=$((skipped + 1)); else
                echo "FAIL $name: stock $s1, fork $f1"; failed=$((failed + 1)); fi
            continue
        fi
        if cmp -s "$work/out/s1.f32" "$work/out/f1.f32"; then
            compared=$((compared + 1))
        else
            echo "FAIL $name: output differs"; failed=$((failed + 1))
        fi
    done
done
echo "stock $STOCK_COMMIT vs fork $(git -C "$fork" rev-parse --short HEAD)$( [[ -n $(git -C "$fork" status --porcelain --untracked-files=no) ]] && echo ' (modified)'):" \
     "$compared renders bit-identical, $skipped not loadable by either, $unstable unstable in stock, $failed failed"
[[ $failed -eq 0 && $compared -gt 0 ]]
