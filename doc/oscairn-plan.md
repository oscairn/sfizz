# Oscairn plan: a sample engine with element voices, on sfizz

Status: proposal, 2026-11-01. Progress is logged in [section 11](#11-progress).

This branch (`oscairn`) of the [oscairn/sfizz](https://github.com/oscairn/sfizz) fork turns
sfizz into the sample engine of Oscairn RFC 0013: a sampler that can do everything a
MONTAGE M / MODX M AWM2 Part can do as a sound engine, so a converted Part plays as on the
reference, while every ordinary SFZ patch keeps playing exactly as it does in stock sfizz.

The sfizz facts below were read from the fork at `f5c6e29f` ("skip over spaces and tabs to
find a command fed to it..."), the head of `sfztools/sfizz` `develop` on 2026-11-01
(committed 2025-03-17). Paths are relative to the repository root. The target's parameter
lists come from the parameter tables transcribed from the published MONTAGE M Data List
(in Jacob's research repository: `docs/params/awm2-element.md`, `performance-part.md`,
`drum-key.md`). Units, curves and selection rules the documents do not publish are measured
from rendered audio (section 6).

## Contents

1. [Goal, scope and relation to RFC 0013](#1-goal-scope-and-relation-to-rfc-0013)
2. [How sfizz is built](#2-how-sfizz-is-built)
3. [The rule: native SFZ stays stock](#3-the-rule-native-sfz-stays-stock)
4. [Gap table](#4-gap-table)
5. [Oscairn integration](#5-oscairn-integration)
6. [Measurement](#6-measurement)
7. [Milestones](#7-milestones)
8. [Upstreamability](#8-upstreamability)
9. [Licence and naming](#9-licence-and-naming)
10. [Decisions for Jacob to confirm, risks, open questions](#10-decisions-for-jacob-to-confirm-risks-open-questions)
11. [Progress](#11-progress)

---

## 1. Goal, scope and relation to RFC 0013

**Goal.** One CLAP instrument, built from this fork, that

- plays any AWM2 Part (Normal or Drum) converted from a MONTAGE M / MODX M Performance, with
  every element and drum-key engine parameter represented with the same range, unit and
  behaviour, matching the reference within the per-feature tolerances of RFC 0013;
- plays every SFZ patch exactly as stock sfizz does (section 3);
- meets Oscairn's host requirements and recommendations (`docs/plugin-porting.md`): native
  sound contexts (RFC 0005), sound assets (RFC 0009), CLAP notes with `NOTE_END`, chokes and
  per-note volume (RFC 0018), deterministic state, a Linux ARM64 build.

**Out of scope for the engine** (RFC 0013 and RFC 0008): insertion, system and master
effects, Part EQ, mixing, sends, zones, arpeggios, scenes, motion sequencer, Super Knob and
controller assignment. They are Performance-level in Oscairn. The engine exposes the
destinations those systems drive (element switches, assignable switches, the Part-level
offsets).

**What RFC 0013 said.** RFC 0013 describes the engine as a new implementation written for
Oscairn, growing the Oscairn Sampler (one-region SFZ, AGPL, `oscairn/sampler`) through RFC
0008's step 3 (multisample) and then the element model, filters, envelopes and drum Parts.

**What forking sfizz changes.**

- **RFC 0013's step 1 is mostly there on day one.** sfizz already plays multisample SFZ:
  regions, groups and masters, loops with crossfades, FLAC/WAV/AIFF/WavPack, preload plus
  background streaming, pan/width/position, key, velocity and CC crossfades, release and
  legato triggers, round robin (`seq_*`), random (`lorand`/`hirand`), key switches,
  `off_by`/polyphony groups, delays, ARIA flex EGs, LFOs and up to N filters and EQs per
  region. The step 1 gate becomes "loads the imported factory waveform library and the
  Salamander piano" rather than writing those features.
- **The engine stays a separate plugin under sfizz's licence.** Oscairn loads it through
  CLAP. Everything this fork adds is BSD-2-Clause like sfizz, so any of it can go upstream
  (section 9). The CLAP wrapper is new code in this fork (sfizz's own plugins live in
  `sfztools/sfizz-ui`, which has LV2 and VST3 but no CLAP).
- **We inherit sfizz's shape.** One `sfz::Synth` holds one patch; voices are per region
  (`src/sfizz/Voice.cpp`), each with its own envelopes, LFOs, filters and EQs. That is the
  per-voice element state RFC 0013 asks for. An AWM2 element becomes a `<group>` of regions,
  one region per key bank; element settings sit on the group and every voice of that element
  gets its own copy of the DSP state.
- **The Oscairn Sampler and this engine.** The Sampler stays as it is (AGPL, Oscairn's own
  baseline profile and test harness). Whether Oscairn keeps both or replaces the Sampler's
  engine with this one is a decision for Jacob (section 10).

**Recommendation: keep sfizz's engine, add an opt-in element layer.** Native opcodes keep
their sfizz meaning in every patch. The reference behaviour arrives through new opcodes with
an `oscairn_` prefix that only act when the patch declares the Oscairn profile (section 3).
They are parsed in new files and implemented as new modulation sources, filter types and
voice stages behind small hooks in sfizz's files.

---

## 2. How sfizz is built

All read from the code at `f5c6e29f`.

### Build

- CMake; the library is `src/` (targets `sfizz::static`, `sfizz::shared`, internal
  `sfizz_internal`); clients in `clients/` (`sfizz_render` renders a MIDI file through an
  SFZ to 16-bit WAV; `sfizz_jack` on Linux); tests in `tests/` (Catch2, `sfizz_tests`);
  options in `CMakeLists.txt` (`SFIZZ_TESTS`, `SFIZZ_RENDER`, `SFIZZ_SHARED`, `SFIZZ_ASAN`,
  `SFIZZ_USE_SYSTEM_*`). C++ standard gnu++14 (`cmake/SfizzConfig.cmake`), `-ffast-math`
  on the library and tests (`sfizz_enable_fast_math`).
- Dependencies are submodules or vendored in `external/` and `src/external/`: abseil,
  ghc::filesystem, simde, st_audiofile (dr_wav, dr_flac, libaiff, WavPack), atomic_queue,
  hiir, kiss_fft, pugixml, cpuid, Surge's tuning library, threadpool, cxxopts, fmidi.
- At `f5c6e29f` the library does not compile with Apple clang 17: the vendored
  `atomic_queue` uses `Base::template do_pop_any(...)` without a template argument list,
  which clang 19 and later reject (`-Wmissing-template-arg-list-after-template-kw`, an error
  by default). One test, `[FloatMath] Nan/Inf checker`, fails in Release because
  `-ffast-math` lets the compiler fold the NaN checks. Both are fixed in milestone 0.
- `.github/workflows/build.yml` runs on `develop`, `master`, tags and pull requests:
  clang-tidy, ASan (Ubuntu 22.04, Debug and RelWithDebInfo), Linux, a MinGW cross build,
  Windows, macOS and a source tarball. Nothing for Linux ARM64, TSan or UBSan. It does not
  run on other branches except by hand.

### The synth (`src/sfizz/Synth.*`, `SynthPrivate.h`)

- `sfz::Synth` loads a file or string (`loadSfzFile`, `loadSfzString`) on the calling
  thread: parse (`src/sfizz/parser/`), build `Region`s, then `finalizeSfzLoad` prepares
  voices, modulation and preloads samples. Loading and rendering must not overlap; plugins
  serialize them with a lock (sfizz-ui) or a fresh instance.
- Events (`noteOn`, `noteOff`, `cc`, `hdcc`, `pitchWheel`, aftertouch, tempo) carry a frame
  delay inside the next block. `renderBlock` renders voices into per-output buses and
  effect buses (`effect` opcodes), then the bus effects.
- Region activation is in `Layer` (`src/sfizz/Layer.cpp`): key, velocity, CC, key switch,
  sequence and random conditions; `trigger` attack, release, release_key, first, legato.
  One random value is drawn per note event and shared by every region. **Sequence counters
  are per region**, counted only on notes inside that region's key range.
- Voices (`VoiceManager`, `VoiceStealing`, `PolyphonyGroup`): a fixed pool (`setNumVoices`,
  default 64), stealing by age or envelope, `group`/`off_by`/`polyphony`/`note_polyphony`.
  Sister voices (one note's voices) are linked in a ring.

### The voice (`src/sfizz/Voice.cpp`)

`Voice::renderBlock`: initial delay, then sample data (`fillWithData`: interpolation by
sample quality 0-10: nearest, linear, Hermite, B-spline, windowed sinc of growing length;
loops; streaming reads) or a generator (`fillWithGenerator`), then the amplitude stage
(base gain, velocity curve, amp EG through the modulation matrix, crossfades, gain
smoothing), pan stage (pan, width, position), and the filter stage (filters in series,
then EQs). Mono regions are filtered before panning, stereo regions after.

### Envelopes, LFOs, filters

- `ADSREnvelope` (SFZv1 DAHDSR, `ampeg_*`, `pitcheg_*`, `fileg_*`): attack linear, decay and
  release exponential (`exp(-9 / (t * rate))` per sample); no shape opcodes.
- `FlexEnvelope` (ARIA `egN_timeX`, `egN_levelX`, `egN_shapeX`, `egN_sustain`; `egN_ampeg`
  replaces the amp EG). Times and levels can follow CCs, not velocity or key.
- `LFO` (ARIA `lfoN_*`: wave, freq, beats, phase, delay, fade, count, steps, sub-oscillators
  with ratio, scale and offset), routed by `lfoN_<target>` and the modulation matrix.
- `SfzFilter` (Faust-generated, `src/sfizz/gen/`): `lpf_1p/2p/4p/6p`, `hpf_*`, `bpf_*`,
  `brf_*`, `apf_1p`, `pink`, `lpf_2p_sv`..., `lsh`, `hsh`, `peq`; per filter cutoff,
  resonance, gain, keytrack, veltrack, random. `filN_` filters run in series. EQs are RBJ
  peaks and shelves (`eqN_*`).
- Modulation (`src/sfizz/modulations/`): `ModId` lists sources (controllers, envelopes,
  LFOs, aftertouch) and targets (amplitude, pan, pitch, volume, cutoff, resonance, EQ...);
  `ModMatrix` connects them per voice per block.

### Parsing

`Region::parseOpcode` (`src/sfizz/Region.cpp`) switches on a hash of the opcode name with
`&` wildcards for numbers; an unknown opcode returns false and the synth reports it as
unknown. `<control>` opcodes are handled in `Synth::Impl::handleControlOpcodes`.

### Randomness and determinism

`Random::randomGenerator` is a `static fast_rand` in `MathHelpers.h` with seed 0: each
translation unit has its own copy, and the sequence restarts with the process. A fresh
process rendering the same events gives the same output; two synths in one process share
the generators. That is good enough for the stock-vs-fork comparison (one process per
render) but not for Oscairn's per-context determinism (section 5).

---

## 3. The rule: native SFZ stays stock

**Rule (Jacob, 2026-11-01).** Ordinary SFZ patches sound exactly as in stock sfizz.
Reference-matching behaviour sits behind a per-patch setting, off by default, never global.

**Design.**

- A patch opts in with `<control> oscairn_profile=1`. It applies to the regions after that
  header (sfizz builds each region as it parses it), so the converter writes it first.
  (Implemented this way on 2026-11-01; the first draft said "whole patch".)
- Without it, every `oscairn_*` opcode is reported as unknown and ignored, exactly as stock
  sfizz ignores it. With it, `oscairn_*` opcodes take effect.
- **The profile never changes the meaning of an existing opcode.** Where the reference
  behaves differently from an SFZ opcode with a similar name (velocity curve, EG segment
  shapes, crossfade law, pan law, cycle counting), the difference is a new `oscairn_*`
  opcode the converter writes, not a different reading of the old one. So a patch that
  declares the profile but uses no `oscairn_*` opcode also renders as stock.
- Everything the profile enables lives in new files under `src/sfizz/oscairn/`. Hooks in
  sfizz's files stay small and are listed in section 8.

**Gate (every milestone, every pushed commit): stock vs fork.** `scripts/oscairn/` builds
the public-API render tool against the stock library (the merge base with
`upstream/develop`, unmodified except for the compiler flag that keeps its vendored
atomic_queue compiling) and against the fork, renders a set of SFZ patches with a fixed
event script (notes, velocities, CCs, pitch bend, sustain, overlapping and repeated notes)
to 32-bit float, and compares the bytes. The patches are sfizz's own test patches
(`tests/TestFiles/`, synthetic samples) plus patches written for the features the fork
touches. The comparison is **bit-identical**; a single differing sample fails. Each render
runs in its own process (section 2, randomness), at a fixed block size and sample rate.
Both trees are built with the same compiler and flags.

What it took to make stock renders reproducible, found while building the gate (2026-11-01):

- **Whole samples preloaded.** With sfizz's default preload size, the rest of a file streams
  in on background threads, and when it arrives decides what a voice plays; renders
  differed from run to run (`looped_regions.sfz`: four different outputs in six runs). The render tool sets the preload size to 2^30 frames, so the
  gate does not cover sfizz's streaming path.
- **ASLR off.** abseil seeds its hash tables from an address, and some patches render
  differently depending on the iteration order (with ASLR on, `newdrums_flat.sfz` gave two
  different outputs in six runs, split 4 and 2; with ASLR off, all six agreed). Renders run with ASLR off (`no-aslr` on macOS,
  `setarch -R` on Linux).
- **Every render twice.** A patch whose stock renders still differ between runs has no
  reference and is reported as unstable; a fork that differs between runs where stock does
  not fails.
- **Patches left out:** the include-loop parser fixtures (`included_loop*.sfz`,
  `root_loop.sfz`: stock crashes on them or renders them differently every run, even with
  ASLR off) and `newdrums_flat.sfz` (2 of 12 stock runs differed from the rest with ASLR
  off, from its fifth note; cause not found). That leaves 53 patches at three settings
  (256 frames, 48 kHz, quality 2; 37 frames, 44.1 kHz, quality 1; 512 frames, 96 kHz,
  quality 10): 159 renders.
- A one-character change in the amp EG's decay coefficient made 125 of the 159 renders
  fail, so the gate does see small changes.

---

## 4. Gap table

Columns: **sfizz** says what stock sfizz already does with standard opcodes. **Disposition**:
*opcode* (an existing opcode means the same; the converter writes it), *mapping* (the
converter translates units through a table or formula from section 6, existing opcode),
*engine* (new behaviour, an `oscairn_*` opcode under the profile), *Performance* (outside
the engine). "Measure" means the unit or rule is not published and is fitted from renders
before the row is done. Factory usage counts are from RFC 0013.

### Waveforms and key banks

| Parameter | sfizz | Disposition |
| --- | --- | --- |
| Key bank note/velocity range, root note | `lokey`/`hikey`/`lovel`/`hivel`/`pitch_keycenter` | opcode (extractor output) |
| Key bank level, fine/coarse tune, pan, mono/stereo | `volume`, `tune`, `transpose`, `pan`, stereo files | mapping, verified on renders by the extractor (`volume = -(255 - level) * 0.375` dB, `tune = fine * 100/64` c, `transpose = coarse`); pan law between the extremes: measure |
| Loop start/end, one-shot, own sample rate | `loop_mode`, `loop_start`/`loop_end`, any rate | opcode; loop continuity over long notes is RFC 0013's step 1 gate |
| Very large pitch offsets, rates up to 192 kHz | sample quality 0-10 (sinc up to 10) | opcode; aliasing compared with the reference (measure); quality per patch is `sample_quality` |
| Wave Select (preset, user, library), wave number | – | converter (which include file) |

### Element (`2p 00`/`2p 01` blocks)

| Parameter | sfizz | Disposition |
| --- | --- | --- |
| Element switch, note limit, velocity limit | `lokey`... on the group | opcode |
| Up to 128 elements, per-voice state | one `<group>` per element; voices per region | opcode; voice budget measured (RFC 0018) |
| Element level | `volume` / `amplitude` | mapping; level curve: measure |
| Element pan | `pan` | mapping; pan law: measure |
| Random pan depth | – (no random pan opcode) | engine (`oscairn_pan_random`), measure the distribution |
| Alternate pan depth | – | engine (`oscairn_pan_alternate`): alternates left/right per note; counter scope: measure |
| Scaling pan depth | – | engine (`oscairn_pan_keyscale`, centre note fixed): measure |
| Velocity cross fade | `xfin_lovel`/`xfout_hivel`, `xf_velcurve` gain or power | engine curve (`oscairn_xfvel`) unless the reference matches power or gain (measure) |
| Key On Delay time / tempo-synced length | `delay` (seconds); no `delay_beats` | mapping for time (table: measure); engine for tempo sync (`oscairn_delay_beats`) |
| XA Normal | plain region | opcode |
| XA Legato | `trigger=legato` | opcode; selection in mono and poly, and what a legato element does with the first note: measure |
| XA Key Off | `trigger=release` (+ `rt_decay`) | opcode; level and envelope at release: measure |
| XA Cycle | `seq_length`/`seq_position`, counted **per region within its key range** | engine: one counter per element group shared by all its regions (`oscairn_cycle`) |
| XA Random | `lorand`/`hirand` with one value per note | opcode if the reference picks uniformly among the group's random elements; measure |
| XA A.SW Off / 1 On / 2 On | `loccN`/`hiccN` on a controller | mapping onto two reserved switch controllers, set by the Performance's assignable switches (decision) |
| Element Group number (1-8) | – | engine: scope of Cycle/Random (`oscairn_element_group`); measure the rules |
| Element connection Thru/InsA/InsB | `output` (multi-output) | mapping to outputs 0/1/2 of the CLAP plugin; routing is RFC 0007 |
| Half Damper switch and time | sustain is a switch | engine (`oscairn_half_damper`, `oscairn_damper_time`): measure |
| Controller sets 1-32, key controller sets 1-4 (switches) | `_onccN` modifiers | Performance (assignments) plus engine destinations; the switches decide which element a controller set reaches |
| LFO box switches 1-3 | – | engine: whether the Part LFO's boxes reach this element |

### Amplitude (`2p 02`)

| Parameter | sfizz | Disposition |
| --- | --- | --- |
| AEG initial, attack, decay 1, decay 2 levels; attack, decay 1, decay 2, release times (0-127) | DAHDSR or flex EG in seconds; no initial level on the amp EG except `ampeg_start` | engine: `oscairn_ampeg_*` with raw 0-127 values and **fitted time and level tables**, segment shapes as measured |
| AEG time velocity segment and sensitivity | `ampeg_vel2*` (seconds added) | engine (scaling factor per segment set): measure |
| AEG time key follow, centre note, release adjustment | – | engine: measure |
| Level velocity sensitivity, offset, key curve 0-4 | `amp_veltrack`, `amp_velcurve_N` | engine (`oscairn_amp_velsens`, `_veloffset`, `_velcurve`): measure the five curves |
| Level scaling: 4 break points, 4 offsets | `amp_keytrack` (linear dB per key) | engine, built: `oscairn_amp_scaling_key1`-`4`, `oscairn_amp_scaling_offset1`-`4`, with `oscairn_amp_level` (K40) |
| Level key follow sensitivity | `amp_keytrack` | engine, built: `oscairn_amp_keyfollow`, an offset of sensitivity x (key - 60) / 32 steps (K40) |

### Pitch (`2p 03`)

| Parameter | sfizz | Disposition |
| --- | --- | --- |
| Coarse, fine tune | `transpose`, `tune` | mapping (fine step: measure) |
| Pitch velocity sensitivity | `pitch_veltrack` (cents at velocity 127) | mapping if linear: measure |
| Random pitch depth | `pitch_random` (cents, uniform) | mapping if uniform: measure distribution and scale |
| Pitch key follow (−200..+200 %) and centre note | `pitch_keytrack`, but around `pitch_keycenter` of each region | engine: key follow around its own centre note independent of key banks (`oscairn_pitch_keytrack`, `_keycenter`) |
| Fine tune key follow scaling | – | engine: measure |
| PEG hold/attack/decay1/decay2/release times and hold/attack/decay1/decay2/release levels (±4800 c), depth, depth velocity sensitivity and curve, time velocity segment and sensitivity, time key follow and centre | `pitcheg_*` DAHDSR, `pitcheg_depth`, `pitcheg_veltodepth` | engine: the same multi-point EG as the AEG, routed to pitch (`oscairn_pitcheg_*`): tables and curves measured; sensitivities `oscairn_pitcheg_veltime_segments`, `_veltime`, `_keytime`, `_keytime_center`, `_veldepth`, `_veldepth_curve` (K13) |

### Filter (`2p 04`)

| Parameter | sfizz | Disposition |
| --- | --- | --- |
| Type LPF24D, LPF24A, LPF18, LPF18s, LPF12+HPF12, LPF6+HPF12, HPF24D, HPF12, BPF12D, BPFw, BPF6, BEF12, BEF6, DualLPF/HPF/BPF/BEF, LPF12+BPF6, Thru (and factory codes 19, 21) | `fil_type` lpf/hpf/bpf/brf 1-6 pole, state variable; two filters in series | engine: one `oscairn_fil_type` per reference type, built from sfizz's filter blocks where the response matches and new blocks where not (24 dB "analog", 18 dB, Dual distance). LPF12+HPF12 first (50 % of elements). Measure every type |
| Cutoff 0-1023, resonance/width 0-127, gain 0-255, HPF cutoff, distance | `cutoff` Hz, `resonance` dB, `fil_gain` | engine (raw values through fitted tables) |
| Cutoff and resonance velocity sensitivity | `fil_veltrack` (cents) | engine: measure |
| Cutoff key follow (−200..+200 %), HPF key follow | `fil_keytrack` (cents per key) | mapping if linear in cents; centre: measure |
| Cutoff scaling: 4 break points, 4 offsets | – | engine (breakpoint curve, shared with level scaling) |
| FEG hold/attack/decay1/decay2/release times, five levels (±9600 c), depth, depth velocity sensitivity and curve, time velocity segment and sensitivity, time key follow and centre | `fileg_*` DAHDSR | engine: the multi-point EG routed to cutoff |
| EQ type 2-band, P.EQ, Boost6/12/18, Thru; frequencies, gains, Q | `eqN_*` peak and shelves | mapping for 2-band and P.EQ if the shapes match (measure); engine for Boost |
| Element LFO: wave saw/triangle/square, key-on reset, delay, speed 0-63 or extended 0-415, fade in, amplitude/pitch/filter depth | `lfoN_wave`, `_freq`, `_delay`, `_fade`, `_phase`, `lfoN_volume/pitch/cutoff` | mapping for shapes; speed, delay and fade tables measured; depth units measured; free-running when reset is off is engine work (sfizz LFOs restart per voice) |
| Part LFO phase offset, destination 1-3 depth ratio | – | engine with the Part LFO (below) |

### Part level an AWM2 Part uses

| Parameter | sfizz | Disposition |
| --- | --- | --- |
| Mono/poly, key assign single/multi | `polyphony`, `note_polyphony`, `group` | mapping where it matches; mono legato behaviour: measure, likely engine (`oscairn_mono`) |
| Portamento switch, time, mode (fingered/full-time), time mode (rate/time 1-2), legato slope | – | engine (`oscairn_portamento_*`): measure |
| Pitch bend range up/down, detune (Hz), note shift | `bend_up`/`bend_down`, `transpose` | mapping; detune in Hz is engine |
| Part AEG/FEG/filter offsets (−64..+63), velocity sensitivity depth/offset | – | engine destinations the Performance drives (parameters of the CLAP plugin) |
| Part LFO: wave (13 incl. user steps), speed or tempo, delay, fade in, hold, fade out, key-on reset off/each/first, 3 destinations with depth, user cycle, slope, 16 steps, random speed | ARIA LFOs are per voice | engine: one LFO per sound context feeding all voices, per-element depth ratio and phase offset |
| Element count, extended element switch | – | converter |
| Micro tuning | sfizz `Tuning` (Scala) | mapping (scales as `.scl`), or host tuning (RFC 0017) |

### Drum key (`2p 10`)

| Parameter | sfizz | Disposition |
| --- | --- | --- |
| Switch, wave, level, velocity sensitivity, pan, coarse/fine, pitch velocity sensitivity | region opcodes | as for elements |
| Random pan, alternate pan | – | engine, shared with elements |
| Receive note off | `loop_mode=one_shot` when off | mapping |
| Key assign single/multi | `note_polyphony=1` / unlimited | mapping: measure retrigger behaviour |
| Alternate group 1-127 | `group` + `off_by` | mapping; choke time `off_time`: measure |
| AEG attack, decay 1, decay 2 (or hold), decay 1 level | – | engine (the element AEG with fixed release) |
| LPF cutoff/resonance/velocity sensitivity, HPF cutoff | – | engine (filter types above) |
| 2-band EQ, Q | `eqN_*` | as for elements |
| Reverb and variation sends, connection Thru/InsA/InsB, output select | – | Performance (sends) and plugin outputs |

### Summary of engine work

Ordered by RFC 0013's build order and factory usage:

1. Profile switch and the stock-vs-fork gate (section 3).
2. A multi-point element envelope (initial/hold, attack, decay 1, decay 2, release) with
   raw-index times and levels through fitted tables, segment shapes as measured, velocity
   and key scaling of times; used three times: amplitude, pitch, filter.
3. Level and velocity curves; breakpoint scaling (level and cutoff).
4. Element-scoped cycle and random selection, element groups, assignable switches, key-off
   and legato rules; key-on delay with tempo sync.
5. The reference filter types and their tables.
6. Element LFO tables and free-running reset; the Part LFO per sound context.
7. Pan modes, random pitch, pitch key follow with centre, EQ Boost types, half damper.
8. Drum-key AEG and filters; element outputs.
9. Mono, portamento, Part offsets as CLAP parameters.

---

## 5. Oscairn integration

Checked against `oscairn/docs/plugin-porting.md` at Oscairn `9de5aad`.

### CLAP target

sfizz has no CLAP build in either repository. The fork adds one in `clap/`
(`oscairn-sfizz.clap`, target `sfizz_clap`, option `SFIZZ_CLAP`, off by default so upstream
builds are unchanged). It vendors the CLAP headers (MIT) and Oscairn's plugin SDK headers
(MIT: `sound-contexts.h`, `assets.h`, and from 0.3.0 `voice-budget.h` and the helper
`voice-budget-plugin.hpp`) with their commits. The SDK's
`sound-contexts-plugin.hpp` owns voices itself, which does not fit sfizz's voice manager, so
the wrapper implements the extension directly.

| Requirement | Plan |
| --- | --- |
| Factory, complete plugin interface | Written in `clap/`; a test loads the bundle through `clap_entry` and checks every function pointer |
| Audio/note ports | One stereo output (two more for element outputs A/B later), no input; one note port, CLAP preferred, MIDI accepted |
| Parameters | None in milestone 0; later the Part-level offsets and switches, with real units and module paths |
| State | Version 2 (`69fe1f5f`): text `oscairn-sfizz-2\n`, then four length-prefixed fields (decimal byte length, `\n`, bytes, `\n`): local directory, base asset library, base asset path, SFZ text. With a base, every `sample=` path (relative, no `..`) resolves through `oscairn.assets` as `<library>/<path>/<sample>` and is listed in `oscairn.asset-dependencies`; an unresolvable sample fails the load; without the host extension a base falls back to the directory and fails if it is empty. Version 1 (directory and text only) still loads. Documented at the top of `clap/plugin.cpp` |
| Activation | Any rate and maximum block; `sfz::Synth::setSampleRate`/`setSamplesPerBlock` on activate |
| Linux ARM64 | CI job on `ubuntu-24.04-arm` |

### Recommended behaviour

- **Real-time safety.** sfizz preallocates voices and buffers and loads samples on its own
  threads; `loadSfzString` allocates and must stay off the audio thread. Preparation builds
  a new `sfz::Synth` on the main thread and the audio thread swaps a pointer at a block
  boundary; old synths are destroyed on the main thread. Allocation in `renderBlock` is
  checked with an allocation guard test.
- **Notes and `NOTE_END`.** sfizz voices know their note number and delay but not a CLAP
  note ID. The wrapper keeps a note table (ID, key, channel, context) and ends a note when
  the synth reports no active voice for its key in that context; that needs a small query
  hook (`Synth::getNumActiveVoicesForKey` or a voice-end callback). Velocity-0 note-on stays a
  note-on (RFC 0005).
- **Chokes and volume expression.** `NOTE_CHOKE` fades the note's voices over 3 ms
  (sfizz's `off_mode=time` path); per-note volume scales the note's voices (a per-voice gain
  hook).
- **Sleep.** `CLAP_PROCESS_SLEEP` when no voice is active in any context.
- **Deterministic rendering.** Seed sfizz's random generators per instance and per context
  (a hook replacing the TU-static generators by one owned by `Resources`; upstreamable).

### Native sound contexts (RFC 0005, `oscairn.sound-contexts/draft-5`)

A context is one `sfz::Synth` with its patch. Up to 8 per instance, each channel selecting
one; held voices finish in the synth they started in; all synths are mixed into the output.
Preparation runs `loadSfzString` with the patch text on the main thread; selection is
queued and applied at the next process boundary; discard waits until the synth is silent.
Contexts that use the same sample files should share decoded audio; sfizz's `FilePool` is
per synth, so sharing needs a pool shared between synths (measured first: memory per
context with and without sharing).

Draft 5 (sixth session, `fd859853`): the plugin lists no CLAP presets, so `prepare_preset` is
null. Oscairn saves a Performance through the contexts' own snapshot interface and refuses one
without it, so the plugin has one (`a5e93e55`, format at `snapshotSave` in `clap/plugin.cpp`):
every prepared context's name and patch and each channel's context, queued selections applied;
loading builds every engine first and gives contexts other than the default fresh IDs; a
version 1 or 2 plugin state (what converted libraries hold) loads as the default context. A
fresh instance plays a sine (`fae08c7e`), so a new part sounds. Checked with Oscairn's
`oscairn-plugin-check` at Oscairn `66a16e5`, built outside the repository, in the engine and
with `OSCAIRN_PLUGIN_WORKER`: PASSED both ways (plays, saves, reopens identically, exports,
installs and plays from the store).

Snapshots prepared beside (`oscairn.sound-snapshots`, draft 1 at Oscairn `eecdee4` in
`84a07d4c`, draft 2 at `2d1424a` with ring-out into the host's buffer for retiring contexts,
draft 3 at `b9e90c7` with `choke_retiring`, which fades the older tail into channels 2 and 3):
`prepare` builds every sound of a snapshot, its default included, as new contexts in free slots
(all or nothing, nothing selected). The instance's synths share preloaded sample data through
the file pool (`f0604e59`, `FilePool::setSharedPreloads`; the data is held weakly, so it lives
while any context uses it), so a sound whose files another context holds opens no file; the
streamed remainder of long samples is still per synth. Measured with `scratchpad
switchbench.cpp`: Rd 73 and Cant Sleep prepared beside in about 10 ms, against 33-59 ms for a
state load into a fresh instance (98-144 ms the first time). With one Default plus seven slots,
a one-sound snapshot needs one free slot.

### Voice budget (RFC 0018 step 4, `oscairn.voice-budget/draft-1`)

From 0.3.0 (`3c629261`). Every sfizz voice is a voice to the host: one region of a note, so one
element of a layered sound, with its own loudness and age, across every context the audio thread
renders (one pool for the instance, as sfizz's synths are per context but the host's limit is
not). The plugin keeps the host's settings in the SDK's `voices::Limits` and chooses with
`voices::victim()`; `notes` uses `voices::Report`. Because the helper needs C++17 and sfizz builds
as C++14 (its headers change ABI under C++17: absl's `string_view`), the helper is compiled alone
in `clap/budget.cpp` behind `clap/budget.h`.

- **When.** At the start of each block, and after each note-on and note-off (which may start
  release regions): while the instance, or a context with a limit of its own, keeps more voices
  than its limit, the next victim fades out over the choke time, 3 ms. A voice already fading,
  or offed by sfizz's own `polyphony`, `note_polyphony`, `off_by` or engine voice limit, counts
  as ending (the SDK's "a choke or a steal"): it is neither counted against the limit nor chosen.
  Without limits from the host (none set, or limit 0 and no entries) nothing is given up.
- **Candidates.** Context: the context's ID (kept on its engine for the audio thread). Selected:
  a channel selects it. Held: not released and, for a note-on voice, its key still down (a key up
  is released, pedal or not). Loudness: the gain on the voice's last rendered frame (element or
  amp envelope, level, velocity, crossfades, per-note volume, fade); before its first block, its
  level and velocity gain, so a new note's quiet elements can go before an older note's louder
  ones. Age: frames since it started. Mono: never (sfizz has no mono mode of its own; the host's
  mono and legato flags still protect held voices through the SDK).
- **sfizz's own limits** (`polyphony`, `note_polyphony`, `off_by`, the engine's voice count, and
  the element envelope releasing at the off time, `2aac320e`) act first and as before; the budget
  is a further limit on top. A full pool inside one synth still steals by sfizz's own order
  (register, K43).
- **Reports.** `active_voices`: every voice not free, fading and offed ones included; no cost
  estimate (0). `notes`: each note by its host address, with its context's voices on its key and
  the largest of their gains (notes sharing a key in one context share them, as for K14).
  `honors_volume`: true (per-note volume scales the key's voices). `NOTE_END` comes once a note's
  last voice has ended, as before.

### Sound assets (RFC 0009)

All file access goes through the host's `oscairn.assets` resolve: before loading, the wrapper
lists the patch's `sample=` and `#include` paths (sfizz's parser reports them), resolves
each relative to the base library reference, and loads the synth with resolved paths
(sfizz's `default_path` and a path-rewriting hook). `oscairn.asset-dependencies` lists them.
Absolute paths and escapes from the library are rejected, as in the Sampler.

### Testing

| Test | Plan |
| --- | --- |
| sfizz's tests | `sfizz_tests` on every job |
| Stock vs fork | Section 3, on every job that builds both |
| Fork tests | `tests/OscairnT.cpp` (Catch2, in `sfizz_tests`, flat like sfizz's other tests): profile switch, every `oscairn_*` opcode, fitted tables, against generated tones |
| CLAP | Load, activate, render, notes with IDs and `NOTE_END`, state round trip, allocation guard |
| Sound contexts, workers | Oscairn's host tests, run against this checkout (as the Sampler's CI does) |
| Sanitizers | ASan+UBSan and TSan builds of `sfizz_tests` and the fork tests |

---

## 6. Measurement

Following RFC 0013 ("How behavior is specified"): a probe Performance with one element,
one parameter swept per render, rendered by the reference recorder; analysis and fitted
tables live in the measurement repository (`oscairn/measurement`, folder `awm2/`, MIT,
no audio). Renders stay on the measuring machine. Fitted tables enter this repository as
generated C++ arrays with the fitting script and its inputs' description, never as audio.

Order (RFC 0013 step 2, then the rest): AEG times (attack, decay 1, decay 2, release) and
levels; level and velocity curves; filter cutoff and resonance per type; PEG; element LFO
speed. Per table: the fitting indices, check indices not used for the fit, the
tolerance (proposed until Jacob confirms), and the result. Tables checked only against
their own fitting renders are labelled so.

**Sample rates (Jacob, 2026-11-01).** Oscairn runs mainly at 48 kHz but supports other
rates. The reference is measured at 44.1 kHz, its hardware runs at 44.1 kHz internally, and
its waveforms keep their own rates. So every fitted table is expressed in time (seconds),
frequency (Hz) or pitch (cents), never in 44.1 kHz samples, and the engine turns it into
per-sample coefficients at the rate it is running at. Every reference-matching feature has a
**cross-rate check**: the same test patch rendered at 44.1, 48 and 96 kHz gives the same
measured quantity (segment time, level, cutoff, LFO rate) within the feature's tolerance at
each rate. It is part of each feature's gate (section 7).

Probe source: a sustained, steady waveform with a clear fundamental, filter Thru, effects
off, so the amplitude envelope is the render's level envelope. Times are measured between
level crossings (attack: onset to within 1 dB of the attack level; decay and release: dB
slope or crossing times), relative to the note-on/note-off markers found in the render.

---

## 7. Milestones

Each milestone meets RFC 0008's runtime gates (no allocation while rendering, held-note
survival, sanitizers) and the stock-vs-fork gate. Every reference-matching feature also
passes its cross-rate check at 44.1, 48 and 96 kHz (section 6).

| # | Deliverable | Gate |
| --- | --- | --- |
| 0 | Builds on macOS and Linux (x86_64, ARM64); toolchain fixes; CLAP entry with state and notes; CI on `oscairn` (Linux x86_64, Linux ARM64, macOS, ASan/UBSan, TSan); stock-vs-fork gate | sfizz's tests pass on every job; the CLAP plugin loads, renders a synthetic patch and round-trips state; stock-vs-fork bit-identical |
| 1 | RFC 0013 step 1 check: the imported factory waveform library and the Salamander piano | All 7,647 waveform files load without errors; loops continuous over 30 s notes (no discontinuity above a proposed threshold at loop points) |
| 2 | Measurement harness in `measurement/awm2/` and the first fitted tables: AEG times and levels, then level/velocity curves; profile switch and `oscairn_ampeg_*` | Repeatable renders (two renders of one probe agree within the proposed tolerance); AEG checks within proposed tolerances at indices not used for fitting; the same checks at 44.1, 48 and 96 kHz; profile tests fail with the change undone |
| 3 | Element model: element-scoped cycle and random, element groups, assignable switches, key-off and legato rules, key-on delay with tempo sync | Articulation fixtures (synthetic) select the same elements as the reference does on matching probes |
| 4 | Filters: LPF12+HPF12 and the LPF family, then the rest; FEG; scaling breakpoints | Frequency-response checks per type within the accepted tolerances (decision 19) |
| 5 | PEG, element LFO, Part LFO per context, pan modes, random pitch, element EQ, half damper | Per-feature measurement checks |
| 6 | Drum keys, element outputs A/B; native sound contexts and assets in the CLAP plugin | Drum fixtures; routing tests; Oscairn's sound-context and asset tests against this checkout |
| 7 | Converter coverage support: every element and drum-key parameter has an opcode | The converter's report shows no engine-parameter loss over the factory set |

---

## 8. Upstreamability

- **New files only, small hooks.** Oscairn code is in `src/sfizz/oscairn/`, `clap/`,
  `tests/OscairnT.cpp` and `scripts/oscairn/`. Hooks in sfizz files, as of `23630bee`:
  - `Synth.cpp`, `buildRegion`: an `oscairn_*` opcode is parsed by `oscairn::parseRegionOpcode`
    when the profile is on (and not skipped for having been unknown in an earlier region);
    `handleControlOpcodes`: `oscairn::parseControlOpcode`; `clear()` resets the profile.
  - `SynthPrivate.h`: the profile flag. `Region.h`: one member, `oscairn::RegionData oscairn`.
  - `Voice.cpp`: the element envelope as a member; started in `startVoice`, applied after the
    amp EG in `amplitudeEnvelope`, released in `release`, and asked in `renderBlock` (end of
    the voice) and `released()`. Each is an `if (region->oscairn.ampegEnabled)` branch. The
    same pattern for the pitch envelope (`pitchEnvelope`), the filter and its envelope
    (`oscairnFilterStage`, called from the two filter stages; started in `startVoice`, released
    in `release`) and the element LFO (`beginBlock` at the top of `renderBlock`, pitch in
    `pitchEnvelope`, gain in `amplitudeEnvelope`, cutoff in `oscairnFilterStage`).
  - `CMakeLists.txt` (option and `add_subdirectory(clap)`), `src/CMakeLists.txt` (sources),
    `tests/CMakeLists.txt` (one test file).
  - For the CLAP plugin (not the profile): `Synth::getNumActiveVoicesForNote`, `setNoteGain`,
    `fadeOutNote`, and for the voice budget `getNumVoiceSlots` and `getVoiceSlot`; in `Voice`,
    `setHostGain`, `fadeOut`, `isFadingOut` and `getLastGain` (the gain stored after the
    amplitude stage, and the level and velocity gain at `startVoice`).
  Upstream-style fixes that are not Oscairn-specific are separate commits (section 11).
- **Candidates for upstream as they are:** the streaming fix `ab0e878c` (interpolator taps
  kept within the frames streamed in so far; `Voice.cpp`, `tests/SynthT.cpp`), the toolchain
  fixes (atomic_queue, the NaN test),
  the CLAP target, per-instance seeded random generators, the active-voices-per-key query,
  multi-point envelopes with velocity/key time scaling, breakpoint curves, element-scoped
  sequence counters, `pan_random`-style opcodes. Upstream would want them as ordinary SFZ
  extensions without the profile; the profile is the Oscairn wrapper around them.
- **Merging upstream.** Before each report, a test merge of `upstream/develop` into
  `oscairn` (not pushed) records conflicts here.

## 9. Licence and naming

- sfizz is BSD-2-Clause (`LICENSE`, "Copyright (c) 2021-2023, sfizz contributors"); its
  vendored dependencies keep their licences (`external/`, `src/external/`). Everything this
  fork adds is BSD-2-Clause with sfizz's header, so it can go upstream. The CLAP headers and
  Oscairn's SDK headers are MIT, vendored with their commits.
- Oscairn loads the plugin through CLAP as a separate work (RFC 0001), so the BSD licence
  has no effect on Oscairn's AGPL and vice versa.
- RFC 0013 IP rules: no manufacturer code, tables or audio; fixtures are generated; renders
  stay on the measuring machine. Names: no manufacturer or product names (or their engine and
  feature names) in identifiers, opcodes, parameters, UI or committed code; this plan names
  the target product as Oscairn's RFCs do.

## 10. Decisions for Jacob to confirm, risks, open questions

Decisions taken (for Jacob to confirm):

1. **Licence:** everything added in this fork is BSD-2-Clause, like sfizz, so it can go
   upstream.
2. **Profile switch:** `<control> oscairn_profile=1`, for the regions after it, off by default;
   `oscairn_*` opcodes do nothing without it; it never changes an existing opcode's
   meaning.
3. **Engine opcodes take the reference's raw values** (0-127 times, 0-1023 cutoff) and the
   engine holds the fitted tables, so the converter does not need them and the "same range,
   unit and behaviour" rule of RFC 0013 holds. The alternative (converter writes seconds and
   Hz into SFZ opcodes) would keep tables out of the engine but cannot express key and
   velocity scaling of times.
4. **Stock baseline for the gate** is the merge base with `upstream/develop`, compiled with
   `-Wno-missing-template-arg-list-after-template-kw` (no source change) so current
   compilers build it.
5. **Sound contexts** are one `sfz::Synth` each; sample sharing between contexts is a later,
   measured change.
6. **Assignable switches** map to two reserved switch controllers (CC numbers above 127,
   which sfizz supports as extended CCs) that the CLAP plugin exposes as parameters.
7. **The Oscairn Sampler** stays unchanged; whether Oscairn retires it in favour of this
   engine is open.
8. **CLAP identity:** ID `org.oscairn.sfizz`, name "Oscairn sfizz", vendor "Oscairn",
   version 0.1.0.
9. **Profile scope:** the profile applies to the regions after its `<control>` header, not to
   the whole patch (sfizz builds regions while parsing).
10. **Envelope tables in seconds**, generated into `ElementTables.cpp` by the measurement
    repository's `fit_eg.py`; the 2.5 % tolerance for segment times is proposed.
11. **Level 0 of a decay or release is -90 dB**, then silence, fitted from the renders
    above; the attack keeps a constant rate for partial attacks until the rule is found.
12. **Gate exclusions:** the include-loop fixtures and `newdrums_flat.sfz` are left out of
    the stock-vs-fork gate because stock does not render them reproducibly (section 3).
13. **Measurement folder name:** the measurement repository folder is `awm2/` as asked; it
    is a product term, which the measurement repository's README otherwise keeps out of
    identifiers. Renaming it (for example to `sample/`) is Jacob's call.

Decided by Jacob (2026-11-01):

14. **Level tolerance 0.4 dB** (velocity, level and pan checks above -24 dB).
15. **Pitch envelope hold above index 80** is extrapolated from the 64-80 slope.
16. **No pitch ceiling.** The reference stops pitch shifts near +4573 cents in one measured
    case; Oscairn does not clamp output pitch at all. Patches below the ceiling sound the
    same; above it Oscairn goes higher. Parameter value ranges stay as documented.
17. **No limit on note shift, transpose, coarse tune or bend range.** The converter maps
    them to the native `transpose`, `tune`, `bend_up` and `bend_down`, which stock sfizz
    already accepts beyond the reference's documented ranges (transpose to +-127 semitones,
    bend to +-120 semitones, bounds permissive); the profile adds no narrower opcode and
    playback clamps no pitch ratio. Documented values map exactly. A test plays +-127
    semitones with bends of 24 and 120 semitones on looped and one-shot samples.
18. **Every sample rate sounds like the reference at 44.1 kHz.** Profile features are checked
    at 44.1, 48, 88.2, 96 and 192 kHz; what still drifts is in the register (K22).

Decided by Jacob (2026-11-01, fourth session):

19. **Tolerances accepted:** the filter against the reference 1 dB; the filter across rates
    0.75 dB where the 44.1 kHz response is within 20 dB of its peak and 1.5 dB from 20 to
    40 dB below it; a whole voice across rates 0.1 dB and 10 ms.
20. **The filter matches its 44.1 kHz response at every rate** by its design, not by internal
    resampling, near the top of the range included; checked with the tolerances of 19 at 48,
    88.2, 96 and 192 kHz, CPU measured against the earlier filter; coefficients computed
    without allocation while processing. Built in `f4b72515` (section 11).
21. **Element LFO in two modes,** chosen per region by `oscairn_elfo_mode`: 0, reference (the
    default; what the converter writes): only the controller-set destinations act, at the
    measured 4.848 Hz falling triangle with the K19 pitch law and the K30 amplitude points,
    the element's own LFO settings ignored; 1, settings: speed, wave, delay, fade-in, reset
    and depths act through documented definitions (`ElementLfo.h`). Built in `b1946dca`.

Taken in the fourth session; Jacob accepted 22-24 and 26 and decided 25 (2026-11-02):

22. **Filter type codes** follow the reference's codes, not the Data List's positions: 0-8 as
    listed, 9 and 20 do not load on the reference and pass the signal through, 10-17 are the
    Data List's BPFw to DualBEF, 18 passes untouched (but see K27), 19 is LPF12+BPF6 (fitted
    as low-pass minus band-pass), 21 applies the filter gain only (measured flat). The
    converter writes the raw code.
23. **`oscairn_fil_distance`** takes the Dual types' distance as displayed, -128..+127; the
    second section's cutoff index moves by 7 per step.
24. **The filter envelope's release gets the whole key-follow shift** (no adjustment parameter
    exists for it; not measured).
25. **Output staging (K28) is an option:** the reference plays a full-scale sample 1.84 dB below
    sfizz at the same Part Volume and CC 7. `oscairn_ref_level=1` (per patch or region, under
    the profile) applies that; it is off by default, so sfizz's own level stays the default
    (Jacob sometimes finds the reference too quiet).
26. **Filter code 18's late low-pass (K27)** is not modelled: factory elements use code 21.

Taken in the voice budget session (2026-11-06), for Jacob to confirm:

27. **Voice budget** (section 5, K43): one pool across the instance's contexts, a voice per
    region; the SDK's order only while the host has set limits, at block start and after note
    events, with sfizz's own polyphony limits acting first and unchanged; a voice offed by those
    counts as ending; loudness is the last rendered gain (the level and velocity gain before the
    first block); no cost estimate; `honors_volume` true.

Risks and open questions:

- sfizz's upstream is slow (last commit 2025-03-17); upstreaming may not happen, and the
  fork must be able to live alone.
- Voice cost: up to 128 elements per note in one patch. sfizz's per-region voices cost a full
  voice each; budget measured on the target (RFC 0018).
- Filter types 19 and 21 in factory content are not in the Data List (RFC 0013 open
  question).
- Stock sfizz: when playback reaches the end of the streamed part of a file, the interpolator's
  neighbour taps could read frames the loader thread is still writing (ThreadSanitizer data
  race in `Voice::Impl::fillInterpolatedWithQuality`). Fixed in `ab0e878c` as a separate,
  upstreamable commit: the new `[Synth] Streamed playback far up...` test raced in 5 of 5
  TSan runs without the fix and in 0 of 5 with it.
- Stock sfizz: `ScopedFTZ`'s destructor read the ARM control register (FPCR, FPSCR) instead of
  writing the saved value back, so each render left flush-to-zero on for the rest of the host's
  audio thread, and other plugins on that thread had their denormals flushed (an Oscairn
  arpeggiator hung on it). Fixed in `8900c80c` as a separate, upstreamable commit; its test fails
  on AArch64 without the fix.
- `-ffast-math` makes the gate compiler-dependent: bit identity is only claimed between the
  two builds made with one compiler on one machine.

## 11. Progress

### Milestone 0 (2026-11-01): done except sound contexts and assets

Commits on `oscairn` after the plan (`75c32654`):

| Commit | What | Upstreamable |
| --- | --- | --- |
| `0488b7b5` | atomic_queue: drop `template` without an argument list (clang 19+ build) | yes |
| `1ee7c5bb` | NaN/Inf checker test without finite-math assumptions (failed in Release on Apple clang 17) | yes |
| `c5b780f5` | sinc interpolator: no misaligned vector load, no wrapped unsigned index (UBSan) | yes |
| `c450b1ac` | WavPack: widen samples with an unsigned shift (UBSan) | yes |
| `f650d314` | FilePool: start its threads after the members they use; atomic stop flags (TSan) | yes |
| `30cbb52c` | stock-vs-fork comparison (`scripts/oscairn/`) | Oscairn |
| `e9aa3371` | CI for `oscairn` (`.github/workflows/oscairn.yml`) | Oscairn |
| `65182e7e` | no null pointers to memcpy/memset in Buffer and OSCWriter (GCC UBSan, found by the first CI run) | yes |
| `6d8f056c` | CLAP plugin `oscairn-sfizz.clap` and its tests | yes, as a generic CLAP target |
| `23630bee` | Oscairn profile and the element amplitude envelope (milestone 2, below) | the envelope, without the profile |

Local checks on the tree of every commit before pushing it (macOS arm64, Apple clang 17):
Release build and `sfizz_tests`; ASan+UBSan and TSan builds (both Release, see below) of
`sfizz_tests`, and of `sfizz_clap_tests` from `6d8f056c` on; the stock-vs-fork gate.
Commits before the fixes they depend on fail the sanitizer runs with the findings those
fixes address (`0488b7b5`: UBSan in the sinc interpolator, TSan in FilePool; and so on);
from `f650d314` every run passes. The gate passed on every commit: 159 renders
bit-identical up to `30cbb52c`'s tree, 162 once the profile patch was added (54 patches at
three settings).

CI runs only on the pushed head. Run 41356239682 (head `e9aa3371`): Linux x86_64, Linux
ARM64, macOS arm64 and TSan passed; ASan+UBSan failed on a GCC-only finding (fixed in
`65182e7e`). Run 41361467597 (head `23630bee`): all five jobs passed; `sfizz_tests` and
`sfizz_clap_tests` passed in each, and the stock-vs-fork gate reported 162 renders
bit-identical on Linux x86_64, Linux ARM64 and macOS.

Sanitized builds use `CMAKE_BUILD_TYPE=Release`: at RelWithDebInfo (-O2) with UBSan, nine of
sfizz's exact float comparisons in `RegionValuesT`/`RegionValuesSetT` fail on Apple clang
(`-10.0f == -10.0f` after a percent round trip), a test fragility not related to the
sanitizers' findings.

Not done in milestone 0: native sound contexts, `oscairn.assets`, the allocation guard test
and per-note `NOTE_END`/choke/volume (the plugin ends all notes once the synth is silent).
They move to milestone 6 with the context work.

**RFC 0013 step 1 (partial).** 300 waveform SFZ files picked at random (seed 1) from the
7,647 of the imported factory library loaded in the fork without errors or unknown opcodes,
and each sounded on a held note 60. All 7,647 and loop continuity over 30 s notes are not
checked yet; the Salamander piano is not here.

### Milestone 2 (2026-11-01): AEG times, in progress

Reference renders: 44.1 kHz, a looped single-cycle sine, filter Thru, effects off, velocity
127, note 69, one AEG parameter swept per render (batches `eg1`-`eg3`, about 250 renders).
Dropouts (runs of exact zeros) were checked in every render; two renders had one and were
not used. Analysis: `awm2/` in the measurement repository (`eg.py`, `segments.py`,
`fit_eg.py`, `compare_eg.py`).

What the renders show (reference behaviour):

- **Levels** 0-127 are `(level/127)^2` in amplitude: 16 levels from 8 to 127 within 0.25 dB
  (median 0.08 dB). Not yet a gated table (the engine uses the formula).
- **Attack** is a straight line in amplitude (at indices 96 and 112, sampled at 12 points
  from 5 % to 100 % of the time, within 0.003 of the line). Its time grows about 2x per 8 indices above 64 and has a floor of
  about 2.7 ms below index 16.
- **Decays and the release** are straight lines in dB. A segment takes the same time whatever
  distance it covers: decays from level 127 to 96, 64 and 32 at index 80 all took 3.17-3.20 s,
  and the decay to level 0 fits the same time if level 0 sits at -90 dB. A release from level
  64 (index 80) agrees with a -90 dB floor within 3 %.
- **Decay times double exactly every 8 indices** with the same eight steps in each octave:
  measured 56-103 agree with the octave measured at 64-71 within 0.21 %. Release times do
  the same at 56-76 and 92-121, but 80-88 are 3-4 % shorter (two renders of index 80 made
  in different sessions agree to 0.0001 %, so this is the reference, not noise).
- Repeatability: attack index 96 in two sessions 0.16 % apart; release 64 and 80 identical.

Engine (`23630bee`): `oscairn_ampeg_*` with those shapes and tables in seconds
(`src/sfizz/oscairn/ElementTables.cpp`, generated by `fit_eg.py`). Attack: interpolated in log
time between measured indices. Decay: the octave rule from 56 (measured values below).
Release: the octave rule from 56 except 77-91, interpolated between measured indices.

Gate (tolerance 2.5 %, **proposed**): the engine rendered with the same probes and measured
with the same code as the reference, at 44.1, 48 and 96 kHz.

| Table | Valid span | Checks (indices not used for fitting) | Largest error | Outside the span |
| --- | --- | --- | --- | --- |
| Attack | 40-111 | 7 (50, 62, 74, 86, 98, 103, 111) | 2.39 % (111, at 96 kHz) | 119: -4.9 % |
| Decay | 56-103 | 15 | 0.21 % | 50: -4.4 %; 105-119: +2 to +10.5 % |
| Release | 56-121 | 25 | 0.81 % | 50: +2.6 % |

All checks pass at all three rates; the engine's own measurements differ by at most 0.034 %
between 44.1, 48 and 96 kHz. Below the valid spans the tables hold measured values that
are not checked (attack 2.7-7.8 ms; decay and release below 56, where the measured falls
are not straight lines in dB).

The open items above were taken up in the second session (below).

### Second session (2026-11-01): amp envelope finished, velocity and pan, pitch envelope, Oscairn integration

Reference batches `lv1`, `lv2`, `eg4`-`eg6`, `pf1`, `pf2` (about 640 renders, 44.1 kHz,
same probe set-up as above).

**Amp envelope** (`781b0147`):

- *Attacks between levels.* On a grid of 25 (initial, attack) level pairs at one index the
  attack is still a straight line in amplitude, and its time is the index's full-scale time
  times the larger level's amplitude times a factor that depends only on the ratio of the
  smaller to the larger level (in level steps), the same up and down: 2.0 at 1/16, 3.8 at
  1/4, 6.1 at 1/2, 9.0 at 7/8. From or to level 0 it is 1.29, 1.17, 1.08 and 1 for the
  other level at 32, 64, 96, 127. 27 renders compared (index 80, and 96 where the render
  held long enough): all within 2.5 % at 44.1, 48 and 96 kHz, largest 2.3 %; 16 of them
  were not used for the fit. Ratios between 0 and 1/16 are not measured.
- *Velocity and key scaling of times.* Each shifts the segment's index: velocity by
  sensitivity * velocity / 128 steps, the key by sensitivity * semitones from the centre / 64
  steps, both truncated toward zero with +63 counting as 64 (a 16-velocity fine grid at
  +32 and a 9-point grid at +63 pinned the rounding). The velocity shift applies to the
  segments the segment switch selects (attack; attack + decay; decay; attack + release;
  all: seen on the reference at all five; the engine comparison covers "all"). The release gets the key shift times 2^((adjustment-127)/32),
  truncated. 52 renders compared at three rates: 50 within 2.5 %; the two outside shift the
  decay below index 56, outside its valid span.
- *Tables.* Every attack index from 112 to 127 measured (re-measured indices agree within
  0.3 %); decays above 104 scaled to three renders that held 40 s (the earlier 16 s renders
  at 113 and 115 read 5-7 % shorter); releases at 77-95 measured, 0.6-4.1 % below the octave
  rule. Valid spans: attack 40-127, decay 56-103 (105-117 fitted only), release 56-121.
  47 checks, all within 2.5 % at 44.1, 48 and 96 kHz (attack 2.25 %, decay 0.21 %, release
  0.68 %).

**Velocity, level and pan** (`0509c480`):

- Element level is the level law, `(level/127)^2`: the converter writes it as `volume`
  (40 log10(level/127) dB); no engine change.
- Velocity: velocity plus offset (at most 127), then one of five curves (curve 3 changes
  nothing; 0, 1, 2 and 4 are measured at 12 velocities and interpolated in dB), then the
  sensitivity: positive values scale the distance below 127 by k, negative ones the distance
  above 1, with k = s/32 up to 32 and 31.5/(63.5 - s) above; gain (result/127)^2.
  `oscairn_amp_velsens`, `_veloffset`, `_velcurve`; the converter sets `amp_veltrack=0`.
- Element pan is a balance: the far side falls as sin^2 of its distance from the edge, the
  near side keeps its level (centre: both full). `oscairn_pan`.
- Compared with 451 reference levels at three rates (all of them used to choose the rules,
  so these are checks against the fitting renders): above -24 dB, 375 of 381 within
  0.4 dB, largest 0.70 dB; below -24 dB, largest 1.27 dB. The reference's levels fall on a
  grid of about 0.375 dB steps whose rounding rule is not identified; a 0.2 dB tolerance is
  not met. Proposed: 0.4 dB above -24 dB.
- Part level: Part volume is 40 log10(v/127) and Part pan equal power,
  θ = (π/2)(P-1)/126, for sample Parts too (5 volume and 6 pan renders, within 0.05 dB and
  0.2 dB). They stay in the converter.

**Pitch envelope** (`adc846f3`, `ed7b70df`): levels in steps of 37.5 cents times depth/64
(255 and +63 count as the full end), moves straight in cents over the amp decay table's
full-scale time, a measured hold table. 15 static offsets within 1 cent except +4800 at full
depth, where the reference stopped at 4573 cents (a pitch ceiling, not modelled); attack
glides within 0.2 % and releases within 2.1 %, at three rates. Depth and time sensitivity
to velocity and key: not measured.

**Not done in this session:**

- *Filters.* LPF12+HPF12 renders with white noise (22 cutoffs, 4 resonances, 3 HPF cutoffs)
  and three other types at 3 cutoffs are recorded. Up to cutoff 512 the response is a
  two-pole low-pass with Q about 0.5 (fits within 0.4 dB rms), corner 89 Hz at 256 to 480 Hz
  at 512; above that a two-pole model no longer fits. Resonance at cutoff 512 peaks 6, 18, 27
  and 35 dB at 32, 64, 96 and 127. No filter in the engine yet.
- *FEG*, *element LFO*: with the element LFO's pitch or amplitude depth at 16-127, any speed,
  delay and fade-in (including 19 and 19, as factory sounds use), the reference rendered no
  modulation at all (under 0.1 cent and 0.02 dB). Something else enables it; not found.

**Oscairn integration** (`dd73d7aa`, `0a245ce1`): per-note voice count, gain and fade-out in
`sfz::Synth` (upstreamable); the CLAP plugin ends each note (NOTE_END) when its context has
no voice left on its key, fades a choked key over 3 ms, applies per-note volume, offers
eight native sound contexts and resolves samples through `oscairn.assets` with
`oscairn.asset-dependencies` (patches with `#include`, or whose samples resolve to more than
one folder, are refused). Tests cover each, and count allocations by the processing thread
inside `process()`: none (a deliberate allocation makes the test fail). Oscairn's
`kick-clap-scan` lists the plugin, and `oscairn-render` loads and runs it (silent: it cannot
set a patch). Oscairn's host tests were not run against it. Notes sharing a key in one
context end and are choked together; per-note volume acts per key; the snapshot (exclusive
state) of contexts is not implemented; contexts do not share decoded samples.

**Factory waveforms** (RFC 0013 step 1): all 7,647 waveform SFZ files of the imported library
load in the fork without errors or unknown opcodes (86,676 regions) and sound on their root
note or on 36, 60 or 84. Loops: 40 looped waveforms picked at random (seed 2), rendered for
30 s at their root note: no click larger than 1.62 times the strongest 3rd difference in its
50 ms neighbourhood (Clarinet3 ff St at 8.05 s; proposed criterion, not listened to). The
Salamander piano is not here.

### Third session (2026-11-01): level grid, filter, element LFO findings, sample rates

- **Level grid.** Element level, velocity gain and balance-pan gains are rounded to whole
  0.375 dB steps, the rule found from the renders (register K1).
- **Element filter** (`oscairn_fil_type`, `_cutoff`, `_resonance`, `_hpf_cutoff`,
  `_keyfollow`, `_gain`): types 0, 4, 6, 7 as two-integrator state-variable sections matching
  the reference at 44.1 kHz, cutoff knots every 8 indices, measured top of range, damping limit,
  key follow and gain (K20-K25, K29). Types 1, 2, 8 measured, not built; type 9 renders need
  re-recording (K23).
- **Sample rates (decision 18).** Envelope, pitch envelope and filter tests run at 44.1, 48,
  88.2, 96 and 192 kHz; a voice using all profile features keeps its level within 0.1 dB and its
  release timing within 10 ms across those rates. The filter is carried to other rates by an
  allpass frequency transform; the residual is K22.
- **Pitch range (decision 17).** No profile limit on note shift or bend; a test plays +-127
  semitones with 120-semitone bends (K26).
- **Element LFO.** Through controller sets ("E.LFO PMD"/"AMD") the reference's element LFO runs
  at 4.848 Hz as a triangle starting at zero and falling, whatever the element's own speed,
  wave, delay and fade say (K18); pitch depth law fitted (K19); amplitude depth is a downward
  triangle in dB (K30). The Part LFO's settings do not set the rate either. Not built this
  round.
- **Filter envelope.** Level law measured (75 cents per step at full depth); times and
  velocity sensitivities not resolved; not built (K17).
- **Listening pairs** in `measurements/listen-awm2/` (not committed) with a README; they
  brought out K2 (curve 4 between knots), K27 (held-waveform level steps) and K28 (overall level
  of the sine probes).
- Not done this round: velocity curves between knots, pitch envelope sensitivities.

### Fourth session (2026-11-01): filter at every rate, all filter types, filter envelope, element LFO

Reference batches `fl3` (450 renders: filter types, filter envelope, velocity curves), `ls2`
and `ls3` (held notes). Dropout checks: none in any render (34 renders of `fl3` are flagged
silent by `check.py`: low velocities on steep curves, really quiet, not dropouts).

| Commit | What | Upstreamable |
| --- | --- | --- |
| `f4b72515` | Filter: the 44.1 kHz response at every rate (decision 20) | Oscairn |
| `b1946dca` | Element LFO, reference and settings modes (decision 21) | Oscairn |
| `978c9f25` | Velocity curves: knots every four velocities (K2) | Oscairn |
| `531e2214` | Filter envelope, cutoff velocity sensitivity | Oscairn; the envelope without the profile |
| `704d872d` | The remaining filter types (decision 22) | Oscairn |
| `ab0e878c` | Interpolator kept within the frames streamed in so far (TSan race) | yes |
| `cd32975b` | Tests: the filter's 1 dB tolerance is accepted | Oscairn |
| `69fe1f5f` | CLAP plugin: asset references in the state (version 2) | yes, as a generic CLAP target |

- **Filter at every rate (decision 20, K22).** The two-integrator section now runs as itself
  with each unit delay replaced by a "reference-rate delay": a pure delay of 2 or 4 samples at
  88.2 and 176.4 kHz, otherwise z^-M times a second-order allpass whose phase equals one
  44.1 kHz sample's at the section's cutoff and at 18 kHz (M = floor(rate / 44.1 kHz) - 2,
  at least 0; when M is 0 the allpass's direct path closes the loop within the sample and the
  section is solved for it). A one-pole section uses the same delay. The allpass coefficients
  come from a 2x2 linear solve per coefficient update (every 16 samples under modulation), no
  allocation. Engine against its own 44.1 kHz response (types 0, 4, 6, 7, cutoffs 0-992 by
  32, resonance 0-127, 41 tones 20 Hz-20 kHz), largest error within 20 dB of the peak /
  from 20 to 40 dB below, before -> after: 48 kHz 1.64 / 1.23 -> 0.27 / 0.31 dB; 88.2 kHz
  5.45 / 6.13 -> 0.00 / 0.00; 96 kHz 5.74 / 6.44 -> 0.06 / 0.06; 192 kHz 12.36 / 9.00 ->
  0.00 / 0.00. The test now covers every type (the first four on the full grid, the others on
  a coarser one) with the accepted tolerances and fails on the earlier filter. CPU (two
  sections, stereo, Apple M3 Max, Apple clang 17, -O3, ns per frame, unmodulated / modulated): before 9.0-9.4
  / 12.8-14.8 at every rate; after 15.9 / 19.1 (44.1), 20.4 / 23.0 (48), 13.4 / 16.7 (88.2),
  20.5 / 23.3 (96), 16.4 / 19.3 (192); at most 0.37 % of a core per voice at 192 kHz.
- **Element LFO (decision 21).** Both modes, `oscairn_elfo_*`; reference mode is the default.
  Tests: the reference mode's rate, falling start, depth laws and controller dependence at all
  five rates, its indifference to the element's settings, a voice's zero-crossing spread
  (1118 cents at CC 127, under 1 cent at CC 0); the settings mode's speed law, waves, delay,
  fade-in, reset and added controller depth.
- **Filter envelope** (`oscairn_fileg_*`, K33, K34) and **cutoff velocity sensitivity**
  (`oscairn_fil_velsens`). Measured: 75 cents per level step at full depth (from `fe1`), the
  pitch envelope's times, the amplitude envelope's velocity and key time shifts, depth velocity
  sensitivity on five curves, cutoff index moved by 0.115 x sensitivity x (velocity - 115).
- **Filter types** (decision 22, K23, K31, K32): all codes the reference loads.
- **K2** resolved at the knots (curve 4 now within 0.16 dB in the listening sweep); **K27**:
  filter code 18 turns into a fixed 1190 Hz low-pass about 10.5 s into a held note-60 note;
  **K28**: 1.84 dB of output staging (decision 25).
- **Converted patches.** All 9,993 patches of the converter's `coverage-2` output loaded in the
  fork at `69fe1f5f`'s source (public API, preload 8192 frames; notes 36, 48, 60, 72, 84 at
  velocity 100 for 0.43 s; `scripts/s4/loadcheck.cpp`, `loadall.log`): no unknown opcodes, no
  non-finite output; 356 patches have no regions at all (the converter wrote the profile header
  and nothing else) and load empty; 50 are silent on those notes (key-off parts, drum and
  key-ranged parts among them; not looked into). Not listened to.
- **Listening pairs** refreshed (curve 4, old files kept as `-before`) and added (filter at 96
  and 48 kHz, five new types, filter envelope, element LFO): `measurements/listen-awm2/`.
- Not done: pitch envelope sensitivities (K13), cutoff below index 144 and between knots (K20),
  HPF key follow, velocity curves between the new knots.

### Fifth session (2026-11-02): CPU

A converted Performance ("Cant Sleep", six elements per note, all filter type 4) took 1,152 us
average and 2,517 us worst of a 2,667 us block (48 kHz, 128 frames) in Oscairn. Offline
benchmark: the part's SFZ with its samples preloaded, 4-note chords changing every 2 s plus a
melody note every 0.5 s, 20 s, about 45 voices (61 at most); "Rd 73" (from Rd 73 Chill) the
same way, about 8 voices. us per block, average / p99 / worst, before (`ccac8b08`) -> after:

| | Cant Sleep | Rd 73 |
| --- | --- | --- |
| macOS arm64 (M3 Max, Apple clang 17) | 253 / 365 / 489 -> 134 / 213 / 356 | 42 / 84 / 115 -> 25 / 53 / 145 |
| Linux ARM64 (Debian trixie on the same machine, GCC 14) | 251 / 382 / 578 -> 158 / 251 / 465 | 45 / 95 / 802 -> 30 / 64 / 127 |

Per voice: Cant Sleep about 5.6 -> 3.0 us per block on macOS (0.21 -> 0.11 % of the budget),
5.6 -> 3.5 us on Linux. Worst-case figures are noisy (the machine was loaded). Changes, each
with the arithmetic per sample unchanged except the filter loop's reassociation:

- Filter coefficients are redesigned only when the cutoff shift changes (they were redesigned
  every 16 samples whenever the filter envelope or LFO was on, also while it held still).
- At 48 and 96 kHz (delay 0) the filter runs both channels as a two-float vector with the
  states in registers and both sections in one loop, the allpass output substituted into the
  state updates (four multiply-adds per state instead of about nine) and no per-sample
  branches. Other rates and compilers without GCC vector extensions keep `run()`.
- The pitch and filter envelopes add runs of samples between events; the amplitude envelope
  fills held levels as runs and skips its per-sample checks where a segment cannot end.

Already in place: FTZ on the audio thread (`ScopedFTZ` in `Synth::renderBlock`); voices end
when the amplitude envelope reaches its -90 dB floor, which stops their filter and envelopes.
All gates unchanged. What is left (K39): the filter is still about a third of a voice and
bound by its recurrence latency; about half of a voice is stock sfizz (interpolation, gains,
pan and width, the stock amplitude EG that still runs under the element envelope, voice
timing, a per-sample `exp2` for the pitch).

### Sixth session (2026-11-05): level scaling, Calling After Me 2, CPU, draft 5, key-off elements

- **Element level scaling** (`93d8e6eb`, K40): break points, offsets, level key follow, measured
  with sine probes (`ks1`). Rd 73's third break point is note 84 with offset 0, so at the played
  note 84 its scaling is 0 dB on the reference too: it is not the cause of the Part's level.
- **Calling After Me 2, Parts 2 and 3 at note 84** (K41): isolated on the reference (only that
  Part sounding, Part Volume 127, effects off; `ks1` `p2only`, `p3only`; `ks3` one element at a
  time) against the converted SFZ at CC 7 127. After the 1.84 dB of output staging (K28), Part 3
  (element 2) plays about 12 dB lower in the engine and Part 2 (element 2) about 4.5 dB higher,
  in both cases already with velocity sensitivity 0, a flat amplitude envelope and filter code
  18 on both sides; the amplitude envelope (sine probe with Part 3's settings, `p3eg`: within
  0.1 dB of level after staging, slopes within 0.1 dB per 100 ms) and the velocity response
  (within 0.4 dB) agree. Part 3's difference is the same on every harmonic (same sample, wrong
  level); Part 2's is not (-5.6 dB on the fundamental, -2.7 and -3.1 on the next two), which
  suggests another key bank. So the level is decided before the element's processing: the
  region the converter writes for those key banks (sample and `volume`). Part 3 is "nearly
  silent" because that deficit adds to its Part Volume of 30 (-25 dB). Converter note in K41.
  Caveat: on the reference the filter-code-18 variant of each element read exactly the level
  of the unchanged element (to 0.01 dB), so that override may not have taken effect; if so the
  reference's "flat" renders still carry the element filter and its gain, and the sizes above
  move by up to the filter gain's 5.45 dB (Part 3 larger, Part 2 smaller), not the signs.
- **Converter notes.** (1) Level scaling: write `oscairn_amp_scaling_key1`-`4` (break points,
  MIDI notes), `oscairn_amp_scaling_offset1`-`4` (offsets as displayed, raw - 128),
  `oscairn_amp_keyfollow` (Level Key Follow Sensitivity, raw - 64) and `oscairn_amp_level`
  (Element Level, raw) on each element's group, whenever an offset or the key follow is not 0;
  keep the element level in `volume` as now. (2) Pitch envelope: write the six
  `oscairn_pitcheg_*` sensitivity opcodes (raw - 64 for the sensitivities, the segment and curve
  codes raw, the centre note as a MIDI note), as for the filter envelope. (3) Key-off elements
  (XA Control Key Off): `trigger=release rt_dead=1 oscairn_keyoff=1` on the group. (4) Calling
  After Me 2 (K41): compare the key banks written for waves 338 and 348 at note 84, velocity 100
  (sample and `volume`) with the reference; the `ks3` probes isolate one element.
- **Pitch envelope sensitivities** (`8f92e7df`, K13): time velocity and key follow follow the
  amplitude envelope's law; depth velocity sensitivity the filter envelope's curves (`ks2`).
- **CPU** (`466bc961`, `b22746d4`): regions with the element envelope no longer generate the
  stock amplitude EG (the connection is not made); element voices compute one pitch ratio per
  block while the pitch holds (stock voices keep the per-sample loop, so stock output stays
  bit-identical whatever the compiler vectorizes). Benchmark (section 11, fifth session; M3 Max,
  best of three runs of three passes, us per 128-frame block, average / p99): Cant Sleep
  137.5 / 212 -> 130 / 199 (EG) -> 124 / 191 (pitch); Rd 73 25.1 / 51.8 -> 23.9 / 49 -> 23.3 /
  46.6. Profile renders (Rd 73 and Cant Sleep, notes 36/60/84, 44.1/48/96 kHz) are
  bit-identical before and after.
- **Draft 5, snapshot, default sound** (section 5): `fd859853`, `a5e93e55`, `fae08c7e`.
- **Key-off elements** (`a2c16d82`, `ad43ddac`, K42): the CLAP wrapper keeps a note while its
  key has a release region, so the note-off reaches the synth; an element envelope that holds
  level 0 ends its voice (a release-triggered voice is never released). Stock sfizz plays a
  release region only while an attack voice of that key plays, unless `rt_dead=1`: the converter
  writes `rt_dead=1` on key-off groups (an existing opcode with the meaning needed). The
  reference releases a key-off element 2.0 s after it starts (`ks4`); `oscairn_keyoff=1`
  (`4c38842e`) does that.

### Review session (2026-11-06): six fixes from a code review of the branch

- **Freewheeling** (`ca69309e`, stock bug): `waitForBackgroundLoading` only waited for the
  loads the dispatch thread had already taken; a note's file queued just before could be
  missed and the voice cut where the preload ended. Cause of the `[Files] Embedded sample
  data` flake under load; deterministic with a block longer than the preload (new test).
- **Reset and activate** (`b2f650ce`, plugin 0.2.1): `reset` only stopped voices, so a held
  sustain pedal, bend or key carried into the next notes; an instance reset and played again
  did not sound as a fresh one (`docs/plugin-porting.md`, "Expect a second life"). Both now
  clear voices, controllers (to the patch's defaults) and note states.
- **SIMD dispatch tables** (`7418cf38`, stock bug): every `Synth` constructor rewrote the
  global function-pointer tables, a data race against a rendering synth (ThreadSanitizer,
  new CLAP test that prepares and discards contexts beside a playing one). Once-only now.
- **Chokes under the element envelope** (`2aac320e`): `off()` shortened only the stock amp
  EG, so `off_by`, `polyphony`, `note_polyphony` and a one-shot sample's end left an element
  voice releasing at its own index (minutes at high indices; drum alternate groups never
  cut). The element envelope now releases at the off time.
- **Element filter below 44.1 kHz** (`dee42b60`, K38): at 22.05 kHz the allpass went
  unstable (NaN with full resonance); such rates run the plain sections.
- **Context engine pointer** (`ca08e6c6`): the audio thread checked every context's
  `unique_ptr` while the main thread set or cleared it (prepare, discard): a data race
  ThreadSanitizer caught in one gate run of the new concurrency test. The audio thread now
  reads an atomic mirror of the pointer.
- Found, not changed (for Jacob): a decay segment that rises from level 0 (`decay1_level=0
  decay2_level=127`) holds 0 and jumps to the target at the segment's end instead of rising
  (`ElementEnvelope.cpp` runs decays as a gain ratio, so 0 stays 0); the shape the reference
  gives is not measured. The stock-vs-fork gate counts silent renders as passes (7 of the
  test patches render silence on both sides) and never renders the streaming path, the one
  fork change that deliberately differs from stock. A misspelt `oscairn_*` opcode is listed
  as unknown once per region, not once. Key-off voices report `released()` from their first
  block (`loop_mode=loop_sustain` on one stops looping at once).

### Voice budget session (2026-11-06): `oscairn.voice-budget/draft-1`, plugin 0.3.0

Oscairn read at `0d6a3e1` (local main; it contains `2f05937`, "Add oscairn.voice-budget/draft-1"):
`voice-budget.h`, the SDK's `voice-budget-plugin.hpp`, the porting guide's voice budget section and
RFC 0018 step 4. Both headers vendored unchanged (`dc39393a`); `sound-contexts.h` and `assets.h`
are the same at `0d6a3e1` as at `b9e90c7`. The extension (`3c629261`; section 5, decision 27, K43),
then the version and this entry.

**Tests** (`clap/test.cpp`, 48 kHz, 64-frame blocks):

- The RFC 0018 step 4 gate: four elements a note under the profile (element envelope, element
  velocity, levels 0, -6, -12, -18 dB), a limit of 16 with one `OSCAIRN_VOICE_BUDGET_SELECTED`
  entry (what Oscairn sends), eight notes: every note keeps its two loudest elements and its
  loudness; at a limit of 8, one element each; no `NOTE_END` while a note has a voice, one each
  after the release.
- A lowered limit: six notes, the limit lowered to four; from the next block the two oldest
  fade (3 ms), counted by `active_voices` and listed by `notes` (more quietly) while they fade,
  `NOTE_END` in the block the fade ends, and the render from then on equal (within 1e-6) to an
  instance playing only the four kept notes; largest step during the fade no larger than the
  notes' own steps plus their level / 64 (Octet's threshold); before the limit, the render is
  the unlimited one.
- The order: a released note before a quieter, older held one; the quietest held note, not the
  oldest; a new note's own sound before another sound's quieter, older note.
- The concurrency test (prepare, select, discard beside a playing context, under TSan) runs with
  a limit set, so the budget reads every context.
- Every budget call on the audio thread under the allocation guard.

Each was checked to fail with its part undone (mutations applied one at a time in a scratch
build): every candidate's loudness 0 (oldest whole notes go: the gate's checks and the quietest
case), giving up the victim's whole key instead of one voice (the gate, including `NOTE_END`
while other elements remained), no block-start check (the lowered limit and the order cases), a
cut instead of the fade (the step check, the still-fading check and the counts), every voice
taken as held (released before held), the incoming context not passed (own sound first), fading
voices left out of `active_voices` or of `notes` (the counts), an allocation in the limit path
(the guard: 488 allocations), the `get_extension` line removed, and no level estimate before a
voice's first block (a new note's quiet elements then never go first: the gate fails).

A converted sound outside the repository (`Evens and Odds`, 13 elements a note, scratch harness
through the built plugin): eight notes, a limit of 45 against 52 counted voices: elements went
from the quietest notes first (13 to 6 and 13 to 12), no note lost to the budget; sfizz's own
64-voice limit had already offed the first notes' elements, which ring out their off-time release
for about 100 ms (K43). A converted 18-element piano plays one region a note, so it gives up
whole notes, as it must.

**Gates** (`gate.sh`: Release, ASan/UBSan and TSan builds of `sfizz_tests` and the CLAP tests, and
stock vs fork, each on the committed tree): `dc39393a` and `3c629261` passed all four (sfizz 518
test cases, the CLAP tests, 162 renders bit-identical). `oscairn-plugin-check` (Oscairn `0d6a3e1`,
built from an export outside the repository, in-process) PASSED on `3c629261`'s plugin; it does
not exercise the voice budget. Oscairn's own voice-budget host tests were not run against this
plugin, and the worker protocol does not carry the extension yet.

### Upstream merge test (2026-11-01)

`upstream/develop` has no commits after `f5c6e29f`: merging it into `oscairn` is a no-op
(checked again at `69fe1f5f`). Of upstream's other branches, `paulfd/test-refactor` merges
cleanly into `develop` but conflicts with `oscairn` in `CMakeLists.txt` (the `SFIZZ_CLAP`
option line); `essej-onpolyaft` conflicts with `oscairn` (`Synth.cpp`, two tests) and with
`develop` too.

## 12. Known differences from the reference

Every difference found so far, so that a converted sound that sounds wrong can be traced.
"Reproduce" names the reference batch under `measurements/awm2/` (renders stay on the
measuring machine) and the analysis script (`measurement/awm2/` in the measurement
repository, or the local scripts beside the renders).

| # | What differs | Size and where | Cause | Status | Reproduce | Code |
| --- | --- | --- | --- | --- | --- | --- |
| K1 | Element level, velocity and pan levels | 381 levels above -24 dB: largest 0.77 dB, 371 within 0.4 dB, 349 within 0.2 dB, 341 within 0.1 dB; below -24 dB up to 1.26 dB. The largest sit where a steep velocity sensitivity maps to a small effective velocity | Levels are rounded to whole 0.375 dB steps (level L: round(40 log10(127/L) / 0.375); velocity: the same on a 2048-step scale; pan far side: the sine law on the grid), now modelled; the rest is the velocity-spread fit | Accepted tolerance 0.4 dB above -24 dB (decision 14); below -24 dB and the spread residual unresolved | `lv1`, `lv2`; `scripts/engine_lv.py` | `oscairn/ElementLevel.cpp`, `ElementEnvelope.cpp` `levelGain` |
| K2 | Velocity curves 0, 1, 2, 4 between measured velocities | Now measured at velocity 1 and every fourth velocity (`fl3` `vk_*`, sensitivity +32) and interpolated linearly in dB between those; curve 4 rises 14 dB per 4 velocities between 60 and 68. The listening sweep (curve 4, sensitivity +32, velocities 8-127 in steps of 8) is within 0.16 dB per note (was +5.9 dB at 56 and -6.2 dB at 72). Those checks are against fitting renders where the sweep hits a knot; 56 and 72 are knots now | Linear in dB between knots 4 velocities apart | Resolved at the knots; between knots unverified | `fl3` (`vk_*`), `ls1` (`vsw_*`); `scripts/s4/vk.py`, `listen4.py` | `ElementLevel.cpp` `curveDb` |
| K3 | Velocity response for curves other than 3 combined with offsets or negative sensitivity | Not measured | Order of offset, curve and sensitivity assumed | Unverified | none yet | `ElementLevel.cpp` |
| K4 | Velocity sensitivity -64 | Reference gives a velocity of 1 about 86 dB down and silence otherwise; engine: velocity 1 at -84 dB, otherwise silent | Edge case approximated | Accepted approximation | `lv1` (`vs_0_*`) | `ElementLevel.cpp` |
| K5 | Attack time between levels, level ratios 0 < smaller/larger < 1/16 | Not measured (larger levels with smaller level 1-7) | Factor interpolated between 1 (level 0) and 2.02 (ratio 1/16) | Unverified | `eg5`; `scripts/engine_att.py` | `ElementEnvelope.cpp` `attackDurationFactor` |
| K6 | Attack times below index 40 | 2.7-7.8 ms in the table, unchecked | Measured knots only | Unverified | `eg2` | `ElementTables.cpp` |
| K7 | Decay times above 103 | 105-117 fitted from three 40 s renders (ratios to the octave rule 0.974-0.979); 119-127 extrapolated with the 117 ratio; 16 s renders at 113/115 read 5-7 % shorter. Those renders use filter code 18, whose late low-pass (K27) lowers a 440 Hz sine by about 1 dB from about 10.5 s on: the long-decay fits may carry that bias (not re-measured) | Long decays need long holds | Fitted only, no checks | `eg4` (`decl_*`), `eg3`; `collect.py` | `ElementTables.cpp` |
| K8 | Decay and release below index 56 | Measured knots, and the falls are not straight lines in dB; engine renders measured the same way differ from 4 % (index 50) to 3.9 times (release index 4) | Fast segments not modelled | Unresolved | `eg2`; `compare_eg.py` | `ElementTables.cpp` |
| K9 | Release 77-95 | Measured directly (0.6-4.1 % below the octave rule); the curve deviates from a straight dB line by 0.3-0.8 dB here | Unknown | Table follows the measurements; shape difference unresolved | `eg2`, `eg4` (`rel_*`) | `ElementTables.cpp` |
| K10 | Envelope velocity/key shifts that move an index below 56 | Up to 10 % (decay index 55) | Outside the decay table's valid span | Unresolved | `eg6` (`tvd_127_66/67`) | `ElementEnvelope.cpp` |
| K11 | Pitch ceiling | Reference: +4800 cents at full depth gave 4573 cents (note 60, sine waveform); Oscairn: 4800 | Reference clamps pitch shift | Deliberate (decision 16) | `pf1` (`pl_255_127`); `scripts/engine_peg.py` | none (no clamp) |
| K12 | Pitch envelope hold times | Measured at 16, 32, 64, 80 only; between knots log-interpolated, above 80 extrapolated | Four knots | Above 80 accepted (decision 15); between knots unverified | `pf2` (`ph_*`) | `oscairn/ElementPitch.cpp` |
| K13 | Pitch envelope depth and time sensitivity to velocity and key | Built (`8f92e7df`). Time velocity sensitivity and key follow use the amplitude envelope's law: 14 glides (sensitivities +-32, +63; velocities 32, 64, 127; notes 36, 60, 84) within one 10 ms analysis step at 44.1, 48 and 96 kHz, except a shift to index 0 (+63 at velocity 127): the reference glides within 15 ms, the engine takes 65 ms (the table's time at index 0). Depth velocity sensitivity uses the filter envelope's curves (K34): 17 static probes on a 1200-cent depth within 61 cents (curves 0, 1 and 4 at velocity 100 and 32 the largest). The release adjustment is not a parameter here; the release gets the whole key shift (not measured) | Shared laws | Built; checks against fitting renders | `ks2`; `scratchpad eng_peg.py` | `ElementPitch.cpp` `start`, `filterDepthVelocityFactor` |
| K14 | Note ends, chokes and per-note volume in the CLAP plugin | Per key, not per note: notes sharing a key in one context end, choke and scale together | sfizz voices know their key, not a note ID | Accepted limitation | `clap/test.cpp` | `clap/plugin.cpp` |
| K15 | Loop continuity | 40 waveforms checked; largest click metric 1.62 (Clarinet3 ff St at 8.05 s), criterion proposed, not listened to | - | Unverified | `loops.py` (scratchpad) | - |
| K16 | Factory waveform playback | Loads and sounds; level and timbre against the reference not compared per waveform | - | Unverified | `loadcheck` | - |
| K17 | Element EQ, Part LFO | Not in the engine. (The filter envelope and the element LFO are, from `531e2214` and `b1946dca`: K33-K36) | Not built | Open | none yet | - |
| K18 | Element LFO parameters | In the reference renders the element's LFO speed (0-63), wave, delay, fade-in, key-on reset and pitch depth change nothing; the LFO driven through a controller set ("E.LFO PMD"/"AMD") runs at 4.848 Hz as a triangle from note-on, and the extended speed range gives no modulation at all; the Part LFO's settings do not set the rate either (`lf4`) | Not known | Modelled as the reference mode (decision 21); the settings mode makes them act by definition (K35) | `lf2`, `lf3`, `lf4`; `scripts/lfo.py` | `oscairn/ElementLfo.cpp` |
| K19 | Element LFO pitch depth through a controller set | Model: peak-to-peak cents = 1118 x 2^((min(2 \|ratio\|, 127) + cc - 253) / 16), nothing at controller value 0 (`lf2`: 0.04 cents at CC 0); 17 probes within -73..+54 cents of depth (4.3 %), the CC series reads up to 4 % low at CC 80-112; ratios below 8 and negative ratios not measured (negative ratios invert the wave, assumed). Listening pair 15 (ratio 40): envelope within 0.08 / 0.36 dB | Fitted law | In the engine (reference mode) | `lf3` (`dr_*`, `dc_*`); `scripts/lfo.py` | `ElementLfo.cpp` `lfoPitchDepthCents` |
| K20 | Filter cutoff table | Knots measured every 8 indices from 144 to 912; between knots linear in cents, unverified. The measured curve rises fast after roughly every 104 indices (one octave) and slower after, up to 190 cents in 8 indices; below 144 (cutoffs under 30 Hz, not measurable) the table continues at 11.54 cents per index; from 928 up the reference stays at 7290 Hz | Knots from `fl1` and `fl2` | Measured; between knots and below 144 unverified | `fl1`, `fl2`; `scripts/fl2.py knots` | `oscairn/ElementFilter.cpp` `kCutoffKnots` |
| K21 | Filter gain, key follow | Gain: passband levels measured at 64, 128, 192, 230 (default) and 255 (-68.34, -44.38, -20.18, -5.45, +3.25 dB), linear in dB between; 0 is silent; below 64 0.375 dB per step assumed. Key follow: (sensitivity - 64) * 3.125 cents per key from note 60 leaves errors of -15 to +94 cents at notes 36 and 84; the largest at the default 74, where both notes read about 90 cents high (a note-60 render at 74 would show whether it is an offset). HPF key follow not measured or modelled | Fitted laws | Gain measured; key follow residual unresolved | `fl2` (`g4_*`, `k4_*`); `scripts/fl2.py gain`, `fl2.py key` | `ElementFilter.cpp` `filterGain`, `start` |
| K22 | Filter at other sample rates | Resolved by design (decision 20, `f4b72515`): each section is the 44.1 kHz section with its unit delays replaced by a pure delay (88.2, 176.4 kHz) or z^-M times a second-order allpass pinned to one 44.1 kHz sample at the section's cutoff and at 18 kHz. Engine against its own 44.1 kHz response, types 0, 4, 6, 7, cutoffs 0-992 in steps of 32, resonance 0-127, 41 tones 20 Hz-20 kHz: largest error 0.27 dB at 48 kHz, 0.00 at 88.2, 0.06 at 96, 0.00 at 192 kHz (was 1.64, 5.45, 5.74, 12.36 dB with the allpass frequency transform). The test covers every type. Above 22.05 kHz at 88.2 kHz and up the response repeats the 44.1 kHz one mirrored (ultrasonic, K37). CPU, two sections stereo: 1.3-2.2 times the earlier filter (section 11) | - | Resolved within the accepted tolerances | `OscairnT.cpp` cross-rate test; `scripts/s4/xrate.cpp`, `scripts/s4/xr*.py` | `ElementFilter.cpp` `pin`, `run` |
| K23 | Filter types | All codes the reference loads are built (`704d872d`, decision 22). Fitted on `fl1`-`fl3` renders with the engine's cutoff and damping laws, rms over each type's renders: 1 (resonant section + damping-1 section) 0.39 dB, 2 (section + one-pole) 0.36, 3 (one-pole + section 4097 cents higher) 4.2 (K31), 5 (one-pole + HPF section) 0.31, 8 (two band-pass) 0.30 below resonance 96 (K24), 10 (low-pass and high-pass with damping 1, 38.2 cents apart per resonance step above 7.5) 0.37, 11 (band-pass) 0.33, 12 (two notch) 0.75, 13 (notch) 0.61, 14-16 (two sections summed) 0.28, 0.23, 0.33, 17 (two notch in series) 0.76, 19 (low-pass minus band-pass) 0.83 over 5 renders. Test points outside 1 dB, left out of the test: 2 at 512/64 at 223 Hz +1.08, 10 at 512/32 at 223 Hz +1.20, 12 at 512/0 at 223 Hz +1.13, 14 at 512/64 at 223 Hz +1.38, 17 at 512/64 at 223 Hz +1.37 (all half the cutoff, at index 512). Codes 9 and 20 do not load on the reference (the recorder keeps the previous performance; re-recorded in `fl3` with the same result); 21 is flat at the filter gain | Fitted models | Built; checks are against fitting renders | `fl1`, `fl2`, `fl3` (`ty*`); `scripts/s4/types2.py`, `tfit*.py`, `t1.py`, `t10.py`, `t19.py` | `ElementFilter.cpp` `start`, `update` |
| K29 | Filter damping near the top of the range | At resonance 0 the reference's damping falls from 2 at index 816 to 1 at about 912 (0.0104 per index), measured; the engine follows that through the knots. How it combines with resonance there is not measured (the engine takes the smaller) | - | Partly measured | `fl2` (`t4_816`-`t4_912`) | `ElementFilter.cpp` `dampingLimit` |
| K30 | Element LFO amplitude depth through a controller set | A downward triangle in dB from 0 at note-on; peak-to-peak 2.2, 4.8, 8.1, 11.8, 16.9, 23.9 dB at ratios 8, 16, 24, 32, 40, 48 (CC 127) and about 83 dB at 63; at ratio 63 CC 32, 64, 96 gives the ratio 16, 32, 48 values, so ratio and CC multiply (unlike the pitch depth, K19). The engine interpolates those points linearly at \|ratio\| x cc / 127; values above 24 dB need a measurement without the analysis's -26 dB floor. Listening pair 16 (ratio 48): envelope within 0.22 / 0.61 dB | Measured points | In the engine; no law | `lf3` (`amd_*`), `lf4` (`pa_*`, `pac_*`); `scripts/lfo.py` | `ElementLfo.cpp` `lfoAmpDepthDb` |
| K24 | Two-section filters at resonance 96 and above | The reference's response peaks at about 50-55 dB instead of the 60-80 dB that two undamped sections would give; type 8 at 96 and 127 is far from any linear model (rms 8-10 dB); the engine follows the linear model | Probably internal limiting; not measured with a level sweep | Unresolved | `fl1` (`t0r_512_*`, `t6r_512_*`), `fl3` (`ty8_512_96`, `_127`) | `ElementFilter.cpp` |
| K25 | Filter under cutoff modulation | The engine runs the state-variable structure itself (since `f4b72515`) and redesigns it every 16 samples from the filter envelope and LFO cents; the reference's update rate and structure under fast cutoff changes are not compared | - | Unverified | none yet | `ElementFilter.cpp` `process` |
| K26 | Note shift, transpose, coarse tune and bend range | The reference's documented ranges are narrower (Part and Performance note shift and element coarse tune +-48 semitones, bend range -48 to +24 semitones); Oscairn takes transpose to +-127 semitones and bends to +-120 semitones, unclamped | - | Deliberate (decision 17) | `OscairnT.cpp` "Note shift and bend range" | stock `Defaults.cpp` (permissive bounds) |
| K27 | Level of held looped waveforms | The steps are a low-pass: the probes use filter code 18, which the reference passes untouched at first and then, about 10.5 s after a note-60 note-on, turns into a fixed two-pole low-pass at about 1190 Hz with damping 2 (fit rms 0.09 dB), fading in over about 1.5 s, whatever the element's cutoff (384 or 640) and resonance (0 or 64); at note 84 nothing within 16 s; code 21 shows nothing within 16 s. Held noise shows it directly; repeated Oboe2 render agrees | Reference behaviour of code 18; factory elements do not use code 18 | Not modelled (decision 26); reproduces | `ls1`, `ls2` (`noise_hold`), `ls3` | - |
| K28 | Overall level of the sine probes | The reference plays a full-scale sample at Part Volume 127, pan centre, at -9.19 dB per channel (`lv1` `pv_127`); sfizz at CC 7 127 at -7.35 dB (its fixed global volume; its default CC 7 of 100 is -4.15 dB, exactly the probes' Part Volume 100). So the sine pairs' +1.7 to +1.9 dB is 1.84 dB of output staging (the equal-power centre -3.01 dB and a further -6.18 dB on the reference side) | Output staging | Optional: `oscairn_ref_level=1` applies it, off by default (decision 25) | `lv1`; `scripts/s4/k28` | stock `Defaults.h` `globalVolume` |
| K31 | Filter type 3 where its section passes the top of the range | The section sits 4097 cents above the cutoff and is held at 7290 Hz; with resonance there the reference shows a weak peak (fit damping 1.36 at cutoff 768, resonance 64) where the engine has the full one (15 dB rms off); low cutoffs (64-448 at resonance 0) 1.2-3.7 dB rms | Not measured near the top | Unresolved | `fl1`, `fl2` (`t3*`, `ty3*`) | `ElementFilter.cpp` `update` case 3 |
| K32 | Dual types with a distance that pushes the second section past the top | At distance +127 (cutoff 512, resonance 32) the reference's second section is near 16 kHz, above 7290 Hz; the engine holds it at 7290 Hz (rms 6.7-7.8 dB for that render); distances -128, -64, +64 fit within 0.13-0.96 dB with 7 index steps per distance step | Above-top frequencies not modelled | Unresolved | `fl3` (`ty14d_*`-`ty16d_*`); `scripts/s4/tdist.py` | `ElementFilter.cpp` `update` cases 14-17 |
| K33 | Filter envelope times | The engine uses the pitch envelope's law (the decay table's full-scale time, hold table); glides measured from noise renders agree within the analysis's spread: attack 72 -2 to -7 %, decay 80 -3 to +5 %, time velocity (+63 at velocity 32) -2 to -11 %, key follow (+63 at note 84) -12 to +6 %, at 44.1, 48 and 96 kHz; attack 48 and release 48 read 20-25 % shorter on the reference than the table (too fast for the analysis window) | Shared law, measured only at these points | Partly checked | `fl3` (`fa_*`, `fd_*`, `fr_*`, `ftv_*`, `ftk_*`); `scripts/s4/eng_feg.py` | `ElementPitch.cpp` |
| K34 | Filter envelope depth and cutoff velocity sensitivity | Cutoff velocity sensitivity: the index moves by 0.115 x sensitivity x (velocity - 115), negative sensitivities mirrored; engine against 80 renders at 44.1, 48, 96 kHz: rms 44-46 cents, largest 138-158. Depth velocity sensitivity: the depth at +63 measured per curve, other sensitivities scale the distance from 1, negative mirrored; 72 renders: rms 36-38 cents, largest 74-82. Both are checks against the fitting renders. Curves 0, 1, 3, 4 measured at 5 velocities and assumed 0 at velocity 1 | Fitted laws | Built | `fl3` (`cv_*`, `dv_*`, `dc_*`); `scripts/s4/cv.py`, `cvfit.py`, `eng_feg.py` | `ElementFilter.cpp` `cutoffVelocityCents`; `ElementPitch.cpp` `filterDepthVelocityFactor` |
| K35 | Element LFO settings mode | Speed, extended speed, wave, delay, fade-in, reset and depth mappings are definitions (decision 21), not measurements: the reference ignores those parameters | By definition | Deliberate | `OscairnT.cpp` "Element LFO, settings mode" | `ElementLfo.h` |
| K36 | Element LFO in reference mode: what is not built | The filter destination through a controller set (E.LFO FMD), the controller sets' polarity and curve, and ratios below 8 or negative are not measured; the engine uses the ratio and controller value only | Not measured | Open | none yet | `ElementLfo.cpp` |
| K37 | Filter above 22.05 kHz at 88.2 kHz and up | With the reference-rate delay the response above 22.05 kHz repeats the 44.1 kHz response mirrored (a low-pass passes again toward 44.1 kHz); inaudible, but sample content up there is not attenuated as an analog-style filter would | Consequence of decision 20's design | Accepted by Jacob (inaudible; no ultrasonic low-pass) | none | `ElementFilter.cpp` |
| K38 | Filter at rates other than the five, or above 705.6 kHz | Rates between 44.1 and 48 kHz put the allpass's pole near Nyquist (an impulse sweep at 32, 45, 46, 47, 50, 64 kHz stayed finite and bounded, response not checked); below 44.1 kHz the allpass went unstable, so those rates run the plain 44.1 kHz sections (`dee42b60`, response off by the rate ratio); above 16 x 44.1 kHz the delay line is capped (comment in the code) | Design limits | Stable everywhere; response unverified off the five rates | `OscairnT.cpp` "finite below the reference rate" | `ElementFilter.cpp` `setSampleRate` |
| K39 | CPU per voice | Cant Sleep: about 3.0 us per voice per 128-frame block at 48 kHz on an M3 Max (3.5 us under Linux ARM64), half of it stock sfizz; not measured on a small ARM board | The filter's recurrence latency (one section pair per voice, about 6 ns per stereo frame); stock voice stages | Open. Done in the sixth session: the stock amplitude EG is not computed under the element envelope, one pitch ratio per block while the pitch holds (about 10 % of a Cant Sleep block). Next: the two sections' four recurrences as one four-float vector (the second section a sample behind), filter coefficients without the double-precision pin per update | Benchmark described in section 11 (fifth session), not in the repository (it needs the converted samples) | `ElementFilter.cpp` `runDirect`; `Voice.cpp` |
| K40 | Element level scaling | Offset law (128 + offset) / 128 on the element level, at most level 127, 0.375 dB grid; offsets linear in key between break points, rounded; key follow sensitivity x (key - 60) / 32 steps, summed with the offset (the sum is assumed, not measured with both set). 50 sine probes: 44 within 0.2 dB; at notes 12, 30, 100, 105 and 110 the reference's sine waveform is itself up to 0.95 dB off its note-60 level (`ks3` `sine_*`); after removing that, 105 and 110 still read 0.45 dB below the engine. How it combines with velocity is assumed multiplicative | Fitted law | Built (`93d8e6eb`) | `ks1`, `ks3`; `scratchpad eng_ls.py` | `ElementLevel.cpp` `levelScalingGain` |
| K41 | Calling After Me 2, Parts 2 and 3 at note 84 | Element 2 of each, isolated, flat (velocity sensitivity 0, flat envelope, filter code 18): Part 3 about 12 dB lower in the engine than the reference (after the 1.84 dB staging), the same on every harmonic; Part 2 about 4.5 dB higher, different by harmonic. Envelope, velocity and level scaling agree. The converted regions for those key banks (sample and `volume`) are the suspects: converter side | Converter (key bank choice or level) | Open, for the converter | `ks1` (`p2only`, `p3only`), `ks3`; `scratchpad eng_ks3.py`, `spec.py` | converter |
| K42 | Key-off elements | The reference plays a key-off element from the note-off without any other voice of the key, and releases it 2.0 s after it starts (measured once, release time 40, sustain held); a decay to level 0 ends it. Engine: `rt_dead=1` (stock opcode) plus `oscairn_keyoff=1` (release 2.0 s after the start); the CLAP wrapper keeps the note until its note-off; an envelope holding level 0 ends its voice. Whether the 2.0 s depends on any parameter is not measured | Measured at one setting | Built (`a2c16d82`, `ad43ddac`, `4c38842e`) | `ks4` | `Voice.cpp` `startVoice`; `clap/plugin.cpp` `endFinishedNotes` |
| K43 | Voice stealing under a voice budget | While Oscairn sets voice limits (`oscairn.voice-budget`), voices past them go in the SDK's order (released before held, the new voice's own sound, lower priority, quieter, older; never a held voice within its sound's reserve or of a part marked mono or legato), a region at a time, at a block's start and after note events, faded over 3 ms. sfizz's own limits (the engine's voices, 64 a context by default; `polyphony`; `note_polyphony`; `off_by`) still steal first, in sfizz's order, and a voice they off counts as ending though it still renders its off-time release (a converted 13-element sound, eight notes: up to 39 such voices for about 100 ms). A voice in its attack counts as quiet as it is then. The reference's own stealing order is not measured | Oscairn RFC 0018's order, so the host and the plugin agree on what is least audible | Deliberate (decision 27, proposed) | `clap/test.cpp` voice budget cases | `clap/plugin.cpp` `keepWithinLimits`; `clap/budget.cpp` |
