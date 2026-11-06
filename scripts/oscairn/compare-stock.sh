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
#
# Every patch must load in both, with the same regions, groups, masters, curves, preloaded
# files and unknown opcodes, and must sound: a silent render fails, unless the patch is
# listed below as silent on purpose.
#
# Streaming: with a preload shorter than the files, the rest of each file streams in on
# background threads. Rendering freewheels, so each block first waits for those loads, and
# the output must then be the fully preloaded output. The fork's streaming renders must be
# bit-identical to stock's fully preloaded renders at the same settings. Stock's own
# streaming renders are made too and only counted: stock's wait can miss a load that a note
# has just queued (fixed in the fork, ca69309e), so they may differ between runs or from
# the preloaded output. This shows that native streaming in a freewheeling render gives the
# preloaded output; it does not cover a voice catching up with the loader in real time (the
# fork's ab0e878c), which no deterministic render reaches.
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
# differed from the rest on 2026-11-01, starting at its fifth note). Left out because they
# load no region on their own: Includes/subdir/included_subdir.sfz (a fragment that
# root_subdir.sfz includes and the gate renders) and table_embedded.sfz (base64 data split
# over lines, which sfizz does not parse).
patches=()
while IFS= read -r p; do patches+=("$p"); done < <(
    find "$fork/tests/TestFiles" "$here/patches" -name '*.sfz' \
        ! -name 'included_loop*.sfz' ! -name 'root_loop.sfz' ! -name 'newdrums_flat.sfz' \
        ! -name 'included_subdir.sfz' ! -name 'table_embedded.sfz' | LC_ALL=C sort)

# Silent on purpose, so only loaded and compared: the hierarchy fixtures' group delays (36 to
# 67 s) outlast the render; scripts/oscairn/patches/hierarchy.sfz plays the same hierarchy.
silent_ok=(tests/TestFiles/basic_hierarchy.sfz tests/TestFiles/basic_hierarchy_antislash.sfz)

# block size, sample rate, sample quality, preload frames ("all": whole files in memory)
configs=("256 48000 2 all" "37 44100 1 all" "512 96000 10 all" "256 48000 2 1024" "512 44100 1 64")
compared=0 streamed=0 unstable=0 failed=0 stock_stream_differs=0 stock_stream_unstable=0
mkdir -p "$work/out"
render() { # <binary> <patch> <out> <config>: prints the exit code, or the output's hash
    local r=0 preload
    read -r block rate quality preload <<< "$4"
    [[ $preload == all ]] && preload=1073741824
    "${run[@]}" "$1" "$2" "$3" "$block" "$rate" "$quality" "$preload" > "$3.load" 2> /dev/null || r=$?
    if [[ $r -ne 0 ]]; then echo "exit $r"; else cksum < "$3" | cut -d' ' -f1-2; fi
}
silent() { cmp -s -n "$(($(wc -c < "$1")))" "$1" /dev/zero; }
fail() { echo "FAIL $name: $1"; failed=$((failed + 1)); }
for p in "${patches[@]}"; do
    rel="${p#"$fork"/}"
    for c in "${configs[@]}"; do
        name="$rel [$c]"
        stream=false; [[ $c != *" all" ]] && stream=true
        if $stream; then
            # the reference: stock with whole files, at the same block size, rate and quality
            s1=$(render "$work/render-stock" "$p" "$work/out/s1.f32" "${c% *} all")
            s2=$(render "$work/render-stock" "$p" "$work/out/s2.f32" "${c% *} all")
            t1=$(render "$work/render-stock" "$p" "$work/out/t1.f32" "$c")
            t2=$(render "$work/render-stock" "$p" "$work/out/t2.f32" "$c")
            if [[ "$t1" != "$t2" ]]; then
                echo "stock streaming, differs between runs: $name"; stock_stream_unstable=$((stock_stream_unstable + 1))
            elif [[ "$t1" != "$s1" ]]; then
                echo "stock streaming, differs from preloaded: $name"; stock_stream_differs=$((stock_stream_differs + 1))
            fi
        else
            s1=$(render "$work/render-stock" "$p" "$work/out/s1.f32" "$c")
            s2=$(render "$work/render-stock" "$p" "$work/out/s2.f32" "$c")
        fi
        f1=$(render "$work/render-fork" "$p" "$work/out/f1.f32" "$c")
        f2=$(render "$work/render-fork" "$p" "$work/out/f2.f32" "$c")
        if [[ "$s1" != "$s2" ]]; then
            # Stock gives different results from run to run: no reference to compare with
            echo "UNSTABLE IN STOCK $name"; unstable=$((unstable + 1)); continue
        fi
        if [[ "$f1" != "$f2" ]]; then fail "fork differs between runs, stock's reference does not"; continue; fi
        if [[ "$s1" == exit* || "$f1" == exit* ]]; then fail "does not load: stock $s1, fork $f1"; continue; fi
        if grep -q '^regions 0 ' "$work/out/s1.f32.load"; then fail "loads no region"; continue; fi
        if ! cmp -s "$work/out/s1.f32.load" "$work/out/f1.f32.load"; then
            fail "loads differently: stock '$(cat "$work/out/s1.f32.load")', fork '$(cat "$work/out/f1.f32.load")'"; continue
        fi
        if ! cmp -s "$work/out/s1.f32" "$work/out/f1.f32"; then fail "output differs"; continue; fi
        if silent "$work/out/s1.f32" && [[ " ${silent_ok[*]-} " != *" $rel "* ]]; then fail "silent"; continue; fi
        if $stream; then streamed=$((streamed + 1)); else compared=$((compared + 1)); fi
    done
done
echo "stock $STOCK_COMMIT vs fork $(git -C "$fork" rev-parse --short HEAD)$( [[ -n $(git -C "$fork" status --porcelain --untracked-files=no) ]] && echo ' (modified)'):" \
     "$compared renders bit-identical, $streamed streaming renders bit-identical to stock's preloaded ones," \
     "$unstable unstable in stock, $failed failed;" \
     "stock's own streaming renders: $stock_stream_differs differ from its preloaded ones, $stock_stream_unstable differ between runs"
[[ $failed -eq 0 && $compared -gt 0 && $streamed -gt 0 ]]
