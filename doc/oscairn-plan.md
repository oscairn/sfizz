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

- A patch opts in with `<control> oscairn_profile=1`. The profile is a property of the
  whole patch, whichever `<control>` header (including an included file's) carries it.
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
| Level scaling: 4 break points, 4 offsets | `amp_keytrack` (linear dB per key) | engine (`oscairn_amp_keyscale_*` breakpoint curve) |
| Level key follow sensitivity | `amp_keytrack` | mapping if it is dB per key around a fixed centre: measure |

### Pitch (`2p 03`)

| Parameter | sfizz | Disposition |
| --- | --- | --- |
| Coarse, fine tune | `transpose`, `tune` | mapping (fine step: measure) |
| Pitch velocity sensitivity | `pitch_veltrack` (cents at velocity 127) | mapping if linear: measure |
| Random pitch depth | `pitch_random` (cents, uniform) | mapping if uniform: measure distribution and scale |
| Pitch key follow (−200..+200 %) and centre note | `pitch_keytrack`, but around `pitch_keycenter` of each region | engine: key follow around its own centre note independent of key banks (`oscairn_pitch_keytrack`, `_keycenter`) |
| Fine tune key follow scaling | – | engine: measure |
| PEG hold/attack/decay1/decay2/release times and hold/attack/decay1/decay2/release levels (±4800 c), depth, depth velocity sensitivity and curve, time velocity segment and sensitivity, time key follow and centre | `pitcheg_*` DAHDSR, `pitcheg_depth`, `pitcheg_veltodepth` | engine: the same multi-point EG as the AEG, routed to pitch (`oscairn_pitcheg_*`): tables and curves measured |

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
(MIT: `sound-contexts.h`, `assets.h`) with their commits. The SDK's
`sound-contexts-plugin.hpp` owns voices itself, which does not fit sfizz's voice manager, so
the wrapper implements the extension directly.

| Requirement | Plan |
| --- | --- |
| Factory, complete plugin interface | Written in `clap/`; a test loads the bundle through `clap_entry` and checks every function pointer |
| Audio/note ports | One stereo output (two more for element outputs A/B later), no input; one note port, CLAP preferred, MIDI accepted |
| Parameters | None in milestone 0; later the Part-level offsets and switches, with real units and module paths |
| State | Patch text, its base asset reference and the sample hashes; deterministic, locale-independent |
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

### Native sound contexts (RFC 0005, `oscairn.sound-contexts/draft-4`)

A context is one `sfz::Synth` with its patch. Up to 8 per instance, each channel selecting
one; held voices finish in the synth they started in; all synths are mixed into the output.
Preparation runs `loadSfzString` with the patch text on the main thread; selection is
queued and applied at the next process boundary; discard waits until the synth is silent.
Contexts that use the same sample files should share decoded audio; sfizz's `FilePool` is
per synth, so sharing needs a pool shared between synths (measured first: memory per
context with and without sharing).

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
| Fork tests | `tests/oscairn/` (Catch2, in `sfizz_tests`): profile switch, every `oscairn_*` opcode, fitted tables, against synthetic samples |
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

Probe source: a sustained, steady waveform with a clear fundamental, filter Thru, effects
off, so the amplitude envelope is the render's level envelope. Times are measured between
level crossings (attack: onset to within 1 dB of the attack level; decay and release: dB
slope or crossing times), relative to the note-on/note-off markers found in the render.

---

## 7. Milestones

Each milestone meets RFC 0008's runtime gates (no allocation while rendering, held-note
survival, sanitizers) and the stock-vs-fork gate.

| # | Deliverable | Gate |
| --- | --- | --- |
| 0 | Builds on macOS and Linux (x86_64, ARM64); toolchain fixes; CLAP entry with state and notes; CI on `oscairn` (Linux x86_64, Linux ARM64, macOS, ASan/UBSan, TSan); stock-vs-fork gate | sfizz's tests pass on every job; the CLAP plugin loads, renders a synthetic patch and round-trips state; stock-vs-fork bit-identical |
| 1 | RFC 0013 step 1 check: the imported factory waveform library and the Salamander piano | All 7,647 waveform files load without errors; loops continuous over 30 s notes (no discontinuity above a proposed threshold at loop points) |
| 2 | Measurement harness in `measurement/awm2/` and the first fitted tables: AEG times and levels, then level/velocity curves; profile switch and `oscairn_ampeg_*` | Repeatable renders (two renders of one probe agree within the proposed tolerance); AEG checks within proposed tolerances at indices not used for fitting; profile tests fail with the change undone |
| 3 | Element model: element-scoped cycle and random, element groups, assignable switches, key-off and legato rules, key-on delay with tempo sync | Articulation fixtures (synthetic) select the same elements as the reference does on matching probes |
| 4 | Filters: LPF12+HPF12 and the LPF family, then the rest; FEG; scaling breakpoints | Frequency-response checks per type within proposed tolerances |
| 5 | PEG, element LFO, Part LFO per context, pan modes, random pitch, element EQ, half damper | Per-feature measurement checks |
| 6 | Drum keys, element outputs A/B; native sound contexts and assets in the CLAP plugin | Drum fixtures; routing tests; Oscairn's sound-context and asset tests against this checkout |
| 7 | Converter coverage support: every element and drum-key parameter has an opcode | The converter's report shows no engine-parameter loss over the factory set |

---

## 8. Upstreamability

- **New files only, small hooks.** Oscairn code is in `src/sfizz/oscairn/`, `clap/`,
  `tests/oscairn/` and `scripts/oscairn/`. Hooks in sfizz files are listed here as they are
  added, each a few lines: the opcode fallback in `Region::parseOpcode`, the control opcode
  in `Synth::Impl::handleControlOpcodes`, new `ModId` entries and their generators, the voice
  stage calls.
- **Candidates for upstream as they are:** the toolchain fixes (atomic_queue, the NaN test),
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
2. **Profile switch:** `<control> oscairn_profile=1`, whole patch, off by default;
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

Risks and open questions:

- sfizz's upstream is slow (last commit 2025-03-17); upstreaming may not happen, and the
  fork must be able to live alone.
- Voice cost: up to 128 elements per note in one patch. sfizz's per-region voices cost a full
  voice each; budget measured on the target (RFC 0018).
- Filter types 19 and 21 in factory content are not in the Data List (RFC 0013 open
  question).
- `-ffast-math` makes the gate compiler-dependent: bit identity is only claimed between the
  two builds made with one compiler on one machine.

## 11. Progress

(Updated as milestones land.)
