// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// A CLAP instrument around sfz::Synth (doc/oscairn-plan.md, section 5).
//
// Sounds: up to eight prepared sound contexts (Oscairn's oscairn.sound-contexts), each a
// synth with its own SFZ patch. Context 0 is the plugin state's patch. Each MIDI channel
// plays one context; a note keeps the context it started in. Patches are loaded on the main
// thread; the audio thread only renders synths that are selected or still sounding, so the
// main thread may free one once nothing uses it.
//
// Notes: CLAP and MIDI notes on port 0. sfizz's voices know their key, not a note ID, so a
// note ends (NOTE_END) once its context has no voice left on its key; notes sharing a key in
// one context end together. A choke fades the key's voices over 3 ms; a note's volume
// expression scales the key's voices.
//
// Voice budget (oscairn.voice-budget): every sfizz voice (one region of a note, an element of a
// layered sound) is one voice to the host. Past the host's limits, voices fade out over the
// choke time in the SDK's order (voice-budget-plugin.hpp), at the start of a block and when a
// note starts voices; sfizz's own polyphony limits act as before. Without limits nothing changes.
//
// Buses: Main and a sound bus for each of the eight sounds (Oscairn's parts sharing one instance,
// RFC 0021). One parameter, Output bus (oscairn.ui's output bus role), is each sound's: an event on
// a channel sets the bus of the sound that channel selects, one without a channel every selected
// sound's, from the start of the block it arrives in. A voice keeps the bus it started on. While a
// sound rings out (oscairn.sound-snapshots), only what it plays on Main goes to the host's buffer.
// Draft 4 saves one sound alone (on Main) and reads an unselected sound's bus, which Oscairn needs
// to form shared instances on its own.
//
// Rendering: CLAP's render extension; offline, every block waits for the samples its notes
// stream (sfizz's freewheeling), so a render does not depend on how far the loader has got.
//
// Files: with the host's oscairn.assets, every sample a patch uses is resolved through the
// host, and oscairn.asset-dependencies lists them; without it, paths resolve from the
// patch's directory.
//
// State (version 4): UTF-8 text,
//   oscairn-sampler-4\n
// then five fields, each its byte length in decimal, \n, the bytes, \n:
//   directory   - local folder relative sample paths resolve from when no base is given
//                 (or when the host offers no oscairn.assets); may be empty
//   library     - base asset reference: library name (under 64 bytes); empty for none
//   path        - base asset reference: folder inside the library (under 1024 bytes,
//                 may be empty for the library's root)
//   patch       - the SFZ text
//   bus         - the output bus, one digit (0 for Main)
// With a base, every sample= path (relative, no "..", after default_path) is resolved through
// the host as the asset <library>/<path>/<sample path> and listed in
// oscairn.asset-dependencies; a sample the host cannot resolve fails the state load.

#include "plugin.h"
#include "budget.h"
#include "sfizz/Synth.h"
#include "sfizz/Config.h"
#include "sfizz/Voice.h"
#include "sfizz/Region.h"
#include "sfizz/AudioSpan.h"
#include "sfizz/FilePool.h"
#include "sfizz/Resources.h"
#include "sfizz/parser/Parser.h"
#include "sfizz/parser/ParserListener.h"
#include <oscairn-clap/assets.h>
#include <oscairn-clap/sound-contexts.h>
#include <oscairn-clap/ui.h>

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr const char* stateVersion = "oscairn-sampler-4";
constexpr size_t maxStateBytes = 64 << 20;
constexpr size_t maxNotes = 256;
constexpr uint32_t contextCount = 8;
constexpr uint32_t busCount = contextCount + 1; // Main and a sound bus per sound
constexpr clap_id busParam = 1;
constexpr const char* freshPatch = "<region> sample=*sine\n"; // a fresh instance plays something
constexpr uint32_t selectionCapacity = 32;
constexpr int16_t chokeMark = -2; // a queued choke_retiring, in place of a channel
constexpr size_t maxSlots = sfz::config::maxVoices; // voice slots a synth has at most
static_assert(busCount <= 10, "a bus is saved as one digit");
static_assert(contextCount * maxSlots <= oscairn_sampler::maxBudgetVoices && maxNotes <= oscairn_sampler::maxBudgetNotes, "budget.h");

struct Patch {
    std::string directory; // where relative sample and include paths resolve
    std::string library;   // base asset reference for host-resolved samples (empty: none)
    std::string basePath;
    std::string text;      // SFZ
    uint32_t bus { 0 };    // as read from a state or snapshot; the playing one is Context::bus
};

struct Engine {
    Patch patch;
    sfz::Synth synth;
    std::vector<oscairn_asset_ref_t> assets; // resolved through the host, for dependencies
    std::bitset<128> releaseKeys; // keys a release region may still play on at note-off
    uint32_t id { 0 };            // its context's ID, set before the audio thread sees it
};

struct Note {
    int32_t id;
    int16_t channel, key;
    uint32_t context;
    bool released; // note-off or choke seen
};

struct Context {
    // Main thread
    bool prepared { false };
    uint32_t id { 0 };
    char name[64] {};
    std::unique_ptr<Engine> engine; // owned here; set through setEngine
    // Shared: the audio thread renders a context while any of these is set; the main thread
    // frees it only when all are clear. It reads the engine through `live`, never `engine`:
    // the main thread writes the pointer while it checks contexts it does not own.
    std::atomic<Engine*> live { nullptr };
    std::atomic<uint32_t> selectedChannels { 0 }; // written by the audio thread
    std::atomic<uint32_t> queued { 0 };           // selections not yet applied
    std::atomic<uint32_t> sounding { 0 };         // notes or voices left, audio thread
    std::atomic<uint32_t> bus { 0 };              // the output bus new voices take
};

struct Selection {
    uint32_t context;
    int16_t channel;
};

// Where a voice budget candidate plays
struct BudgetVoice {
    Engine* engine;
    uint32_t context; // slot
    int voice;
};

struct Plugin {
    clap_plugin_t plugin {};
    const clap_host_t* host { nullptr };
    const oscairn_host_assets_t* assets { nullptr };
    double sampleRate { 48000.0 };
    uint32_t maxFrames { 1024 };
    bool active { false };
    uint32_t chokeFrames { 144 };
    std::atomic<bool> offline { false }; // main thread -> audio: CLAP_RENDER_OFFLINE

    // main thread: preloaded samples the contexts share, so a sound prepared beside another
    // that uses the same files does not read them again
    std::shared_ptr<sfz::SharedPreloads> sharedPreloads { std::make_shared<sfz::SharedPreloads>() };
    Patch patch; // main thread: context 0's patch, as saved
    Context contexts[contextCount];
    uint32_t nextId { 1 };

    // Main thread -> audio: a new engine for context 0 (from a state load while active)
    std::atomic<Engine*> pendingDefault { nullptr };
    std::atomic<Engine*> retired { nullptr };

    Selection selections[selectionCapacity];
    std::atomic<uint32_t> selectionHead { 0 }, selectionTail { 0 };

    // Audio thread
    const clap_audio_buffer_t* ringOut { nullptr }; // for the next process() only
    bool fading[contextCount] {}; // choked by choke_retiring: rings out into channels 2 and 3
    uint32_t channelContext[16] {};
    uint32_t busesInUse[contextCount] {}; // buses a context's voices may be on: its own, and older ones until it is silent
    Note notes[maxNotes];
    size_t noteCount { 0 };
    std::vector<float> left, right, mix[2 * busCount];
    oscairn_sampler::VoiceBudget budget; // the host's limits, as set_limits last gave them
    // Contexts a reset found idle, which the main thread may be replacing: each is cleared when
    // next rendered, at the start of a process call, before any event
    uint32_t stale { 0 };
    BudgetVoice budgetVoices[contextCount * maxSlots];
};

Plugin& self(const clap_plugin_t* p) { return *static_cast<Plugin*>(p->plugin_data); }

void configure(const Plugin& s, sfz::Synth& synth)
{
    synth.setSampleRate(static_cast<float>(s.sampleRate));
    synth.setSamplesPerBlock(static_cast<int>(s.maxFrames));
}

// The sample files a patch names, relative to its directory (default_path applied)
struct SampleCollector : sfz::Parser::Listener {
    std::string defaultPath, header;
    std::vector<std::string> samples;
    void onParseHeader(const sfz::SourceRange&, const std::string& h) override { header = h; }
    void onParseOpcode(const sfz::SourceRange&, const sfz::SourceRange&, const std::string& name, const std::string& value) override
    {
        std::string v = value;
        std::replace(v.begin(), v.end(), '\\', '/');
        if (header == "control" && name == "default_path")
            defaultPath = v;
        else if (name == "sample" && !v.empty() && v[0] != '*')
            samples.push_back(defaultPath + v);
    }
};

bool loadPatch(const Plugin& s, Engine& e, const oscairn_asset_ref_t* base, std::string& message)
{
    configure(s, e.synth);
    e.synth.getResources().getFilePool().setSharedPreloads(s.sharedPreloads);
    if (e.patch.text.empty())
        return true;
    // sfizz resolves relative paths from the directory of the virtual file name
    std::string directory = e.patch.directory;
    if (s.assets && base) {
        // Resolve every sample through the host. A first parse finds them; the files must
        // then lie in one folder tree, which becomes the patch's directory.
        // shortcut: samples resolving to several folders are refused, and #include files are
        // not resolved; per-file resolution needs a path-mapping hook in sfizz's file pool.
        SampleCollector scan;
        sfz::Parser parser;
        parser.setListener(&scan);
        parser.parseString("/nonexistent-oscairn-base/patch.sfz", e.patch.text);
        if (e.patch.text.find("#include") != std::string::npos) {
            message = "#include is not supported with host-resolved samples yet";
            return false;
        }
        std::string root;
        for (const std::string& relative : scan.samples) {
            if (relative.empty() || relative[0] == '/' || relative.find("..") != std::string::npos) {
                message = "sample path not allowed: " + relative;
                return false;
            }
            oscairn_asset_ref_t ref {};
            std::snprintf(ref.library, sizeof(ref.library), "%s", base->library);
            const std::string path = (std::strlen(base->path) ? std::string(base->path) + "/" : std::string()) + relative;
            if (path.size() >= sizeof(ref.path)) {
                message = "sample path too long: " + relative;
                return false;
            }
            std::snprintf(ref.path, sizeof(ref.path), "%s", path.c_str());
            char local[4096] {}, why[256] {};
            if (!s.assets->resolve(s.host, &ref, local, sizeof(local), why, sizeof(why))) {
                message = "cannot resolve " + relative + ": " + why;
                return false;
            }
            const std::string resolved = local;
            if (resolved.size() <= relative.size() || resolved.compare(resolved.size() - relative.size(), relative.size(), relative) != 0) {
                message = "a resolved sample is not in the patch's folder tree: " + relative;
                return false;
            }
            const std::string thisRoot = resolved.substr(0, resolved.size() - relative.size());
            if (!root.empty() && root != thisRoot) {
                message = "samples resolve to more than one folder";
                return false;
            }
            root = thisRoot;
            if (std::none_of(e.assets.begin(), e.assets.end(), [&](const oscairn_asset_ref_t& a) { return !std::strcmp(a.path, ref.path); }))
                e.assets.push_back(ref);
        }
        if (!root.empty())
            directory = root;
    }
    if (!e.synth.loadSfzString(directory + "/patch.sfz", e.patch.text)) {
        message = "the patch could not be loaded";
        return false;
    }
    for (int i = 0, n = e.synth.getNumRegions(); i < n; ++i) {
        const sfz::Region* r = e.synth.getRegionView(i);
        if (r && r->isRelease())
            for (int k = std::max(0, int(r->keyRange.getStart())); k <= std::min(127, int(r->keyRange.getEnd())); ++k)
                e.releaseKeys.set(static_cast<size_t>(k));
    }
    return true;
}

std::unique_ptr<Engine> makeEngine(const Plugin& s, const Patch& patch, const oscairn_asset_ref_t* base, std::string& message)
{
    std::unique_ptr<Engine> e { new Engine };
    e->patch = patch;
    if (!loadPatch(s, *e, base, message))
        return nullptr;
    return e;
}

void collectRetired(Plugin& s) { delete s.retired.exchange(nullptr); }

// Gives a context its engine (null for none); the old one is freed here
void setEngine(Context& c, std::unique_ptr<Engine> e)
{
    c.live.store(e.get(), std::memory_order_release);
    c.engine = std::move(e);
}

// Audio thread

void endNote(const clap_output_events_t* out, const Note& n, uint32_t time)
{
    clap_event_note_t end {};
    end.header = { sizeof(end), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_END, 0 };
    end.note_id = n.id;
    end.port_index = 0;
    end.channel = n.channel;
    end.key = n.key;
    out->try_push(out, &end.header);
}

void endFinishedNotes(Plugin& s, const clap_output_events_t* out, uint32_t time)
{
    size_t kept = 0;
    for (size_t i = 0; i < s.noteCount; ++i) {
        const Note& n = s.notes[i];
        Engine* e = s.contexts[n.context].live.load(std::memory_order_relaxed);
        // a held key with a release region keeps its note, so its note-off still arrives
        if (!e || (e->synth.getNumActiveVoicesForNote(n.key) == 0 && (n.released || !e->releaseKeys[static_cast<size_t>(n.key)])))
            endNote(out, n, time);
        else
            s.notes[kept++] = n;
    }
    s.noteCount = kept;
}

void applySelections(Plugin& s)
{
    uint32_t tail = s.selectionTail.load(std::memory_order_relaxed);
    const uint32_t head = s.selectionHead.load(std::memory_order_acquire);
    for (; tail != head; ++tail) {
        const Selection sel = s.selections[tail % selectionCapacity];
        if (sel.channel == chokeMark) {
            // choke every retiring context (selected by no channel, still sounding) with the
            // choke fade; its notes no longer wait for a note-off
            for (uint32_t i = 0; i < contextCount; ++i) {
                Context& c = s.contexts[i];
                Engine* e = c.live.load(std::memory_order_relaxed);
                if (!e || c.selectedChannels.load(std::memory_order_relaxed) || !c.sounding.load(std::memory_order_relaxed))
                    continue;
                s.fading[i] = true;
                for (int key = 0; key < 128; ++key)
                    e->synth.fadeOutNote(0, key, static_cast<int>(s.chokeFrames));
                for (size_t n = 0; n < s.noteCount; ++n)
                    if (s.notes[n].context == i)
                        s.notes[n].released = true;
            }
            continue;
        }
        s.fading[sel.context] = false;
        for (int ch = 0; ch < 16; ++ch) {
            if (sel.channel >= 0 && sel.channel != ch)
                continue;
            const uint32_t old = s.channelContext[ch];
            s.contexts[old].selectedChannels.fetch_and(~(1u << ch), std::memory_order_release);
            s.contexts[sel.context].selectedChannels.fetch_or(1u << ch, std::memory_order_release);
            s.channelContext[ch] = sel.context;
        }
        s.contexts[sel.context].queued.fetch_sub(1, std::memory_order_release);
    }
    s.selectionTail.store(tail, std::memory_order_release);
}

// Voice budget (audio thread)

// The engine of a context the audio thread renders now, or null
Engine* rendered(Plugin& s, uint32_t i)
{
    Context& c = s.contexts[i];
    return (c.selectedChannels.load(std::memory_order_acquire) || c.sounding.load(std::memory_order_relaxed))
        ? c.live.load(std::memory_order_relaxed) : nullptr;
}

// Fades voices out until every context keeps within its own limit and the instance within its
// limit: in the SDK's order, for a new voice of context slot `incoming` (contextCount: none),
// from `delay` frames into the block.
void keepWithinLimits(Plugin& s, uint32_t incoming, int delay)
{
    using oscairn_sampler::VoiceBudget;
    VoiceBudget& budget = s.budget;
    if (!budget.limited())
        return;
    budget.clearCandidates();
    size_t count = 0;
    uint32_t kept[contextCount] {}, total = 0;
    uint32_t ids[contextCount + 1];
    ids[contextCount] = VoiceBudget::none;
    for (uint32_t i = 0; i < contextCount; ++i) {
        Engine* e = rendered(s, i);
        ids[i] = e ? e->id : VoiceBudget::none;
        if (!e)
            continue;
        const bool selected = s.contexts[i].selectedChannels.load(std::memory_order_relaxed) != 0;
        const sfz::MidiState& midi = e->synth.getResources().getMidiState();
        for (int k = 0, slots = e->synth.getNumVoiceSlots(); k < slots; ++k) {
            const sfz::Voice& v = *e->synth.getVoiceSlot(k);
            if (v.isFree())
                continue;
            const sfz::TriggerEvent& trigger = v.getTriggerEvent();
            if (count == oscairn_sampler::maxBudgetVoices)
                break;
            s.budgetVoices[count++] = { e, i, k };
            oscairn_sampler::BudgetCandidate c;
            c.context = e->id;
            c.selected = selected;
            // a key up is released, pedal or not
            c.held = !v.released() && (trigger.type != sfz::TriggerEventType::NoteOn || midi.isNotePressed(trigger.number));
            c.ending = v.offedOrFree() || v.isFadingOut(); // sfizz's own steal, or a fade
            c.loudness = v.getLastGain(); // a voice this block's events started: the level it starts at
            c.age = static_cast<uint64_t>(std::numeric_limits<int>::max() - v.getAge());
            budget.addCandidate(c);
            if (!c.ending) {
                ++kept[i];
                ++total;
            }
        }
    }
    auto giveUp = [&](uint32_t only) {
        const int i = budget.victim(ids[incoming], ids[only]);
        if (i < 0)
            return false;
        const BudgetVoice& b = s.budgetVoices[i];
        b.engine->synth.getVoiceSlot(b.voice)->fadeOut(delay, static_cast<int>(s.chokeFrames));
        budget.setEnding(static_cast<size_t>(i));
        --kept[b.context];
        --total;
        return true;
    };
    for (uint32_t i = 0; i < contextCount; ++i) {
        const uint32_t own = ids[i] == VoiceBudget::none ? 0
            : budget.limitOf(ids[i], s.contexts[i].selectedChannels.load(std::memory_order_relaxed) != 0);
        while (own && kept[i] > own && giveUp(i)) {
        }
    }
    while (budget.limit() && total > budget.limit() && giveUp(contextCount)) {
    }
}

// A context as in a fresh instance (activation, reset): its synth as loaded, nothing sounding;
// allocation-free
void clearContext(Plugin& s, uint32_t i, Engine& e)
{
    e.synth.restart();
    s.contexts[i].sounding.store(0, std::memory_order_release);
}

// An Output bus event: the bus of the sound its channel selects, or of every selected sound
void applyParam(Plugin& s, const clap_event_header_t* h)
{
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE)
        return;
    const auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
    if (e->param_id != busParam || e->note_id >= 0 || e->key >= 0 || e->port_index > 0 || e->channel < -1 || e->channel > 15
        || !std::isfinite(e->value))
        return;
    const auto bus = static_cast<uint32_t>(std::lround(std::min(std::max(e->value, 0.0), double(busCount - 1))));
    for (int ch = 0; ch < 16; ++ch)
        if (e->channel < 0 || e->channel == ch)
            s.contexts[s.channelContext[ch]].bus.store(bus, std::memory_order_relaxed);
}

bool matches(const Note& note, int32_t id, int16_t channel, int16_t key)
{
    return (id < 0 || note.id == id) && (channel < 0 || note.channel == channel) && (key < 0 || note.key == key);
}

void applyEvent(Plugin& s, const clap_event_header_t* h, int delay)
{
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return;
    switch (h->type) {
    case CLAP_EVENT_NOTE_ON:
    case CLAP_EVENT_NOTE_OFF:
    case CLAP_EVENT_NOTE_CHOKE: {
        const auto* n = reinterpret_cast<const clap_event_note_t*>(h);
        if (n->port_index > 0 || n->key > 127)
            return;
        const float velocity = static_cast<float>(std::min(1.0, std::max(n->velocity, 0.0)));
        if (h->type == CLAP_EVENT_NOTE_ON) {
            if (n->channel < 0 || n->channel > 15 || n->key < 0)
                return;
            const uint32_t context = s.channelContext[n->channel];
            // A velocity-0 note-on stays a note-on (Oscairn RFC 0005); sfizz needs a velocity
            // above 0 to start a voice, so use the smallest one it distinguishes.
            if (Engine* e = s.contexts[context].live.load(std::memory_order_relaxed)) {
                e->synth.hdNoteOn(delay, n->key, std::max(velocity, 1.0f / 127.0f));
                keepWithinLimits(s, context, delay);
            }
            if (s.noteCount < maxNotes)
                s.notes[s.noteCount++] = { n->note_id, n->channel, n->key, context, false };
            return;
        }
        // Note-off and choke address the notes they match, in the context each started in
        for (size_t i = 0; i < s.noteCount; ++i) {
            Note& note = s.notes[i];
            Engine* e = s.contexts[note.context].live.load(std::memory_order_relaxed);
            if (!e || note.released || !matches(note, n->note_id, n->channel, n->key))
                continue;
            note.released = true;
            if (h->type == CLAP_EVENT_NOTE_OFF) {
                e->synth.hdNoteOff(delay, note.key, velocity); // may start release regions
                keepWithinLimits(s, note.context, delay);
            } else
                e->synth.fadeOutNote(delay, note.key, static_cast<int>(s.chokeFrames));
        }
        break;
    }
    case CLAP_EVENT_NOTE_EXPRESSION: {
        const auto* x = reinterpret_cast<const clap_event_note_expression_t*>(h);
        if (x->port_index > 0 || x->expression_id != CLAP_NOTE_EXPRESSION_VOLUME)
            return;
        const float gain = static_cast<float>(std::min(4.0, std::max(x->value, 0.0)));
        for (size_t i = 0; i < s.noteCount; ++i) {
            const Note& note = s.notes[i];
            if (Engine* e = s.contexts[note.context].live.load(std::memory_order_relaxed))
                if (matches(note, x->note_id, x->channel, x->key))
                    e->synth.setNoteGain(note.key, gain);
        }
        break;
    }
    case CLAP_EVENT_MIDI: {
        const auto* m = reinterpret_cast<const clap_event_midi_t*>(h);
        if (m->port_index > 0)
            return;
        const int channel = m->data[0] & 0x0f;
        const uint8_t kind = m->data[0] & 0xf0, a = m->data[1] & 0x7f, b = m->data[2] & 0x7f;
        if (kind == 0x90 || kind == 0x80) {
            clap_event_note_t n {};
            n.header = { sizeof(n), h->time, CLAP_CORE_EVENT_SPACE_ID,
                         static_cast<uint16_t>(kind == 0x90 && b ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0 };
            n.note_id = -1;
            n.channel = static_cast<int16_t>(channel);
            n.key = a;
            n.velocity = b / 127.0;
            applyEvent(s, &n.header, delay);
            return;
        }
        Engine* e = s.contexts[s.channelContext[channel]].live.load(std::memory_order_relaxed);
        if (!e)
            return;
        switch (kind) {
        case 0xb0: e->synth.hdcc(delay, a, b / 127.0f); break;
        case 0xe0: e->synth.pitchWheel(delay, (b << 7 | a) - 8192); break;
        case 0xd0: e->synth.channelAftertouch(delay, a); break;
        case 0xa0: e->synth.polyAftertouch(delay, a, b); break;
        default: break;
        }
        break;
    }
    default:
        break;
    }
}

// Plugin interface

bool init(const clap_plugin_t* p)
{
    auto& s = self(p);
    if (s.host->get_extension)
        s.assets = static_cast<const oscairn_host_assets_t*>(s.host->get_extension(s.host, OSCAIRN_EXT_ASSETS));
    std::string message;
    Context& c = s.contexts[0];
    s.patch.text = freshPatch;
    setEngine(c, makeEngine(s, s.patch, nullptr, message));
    c.prepared = true;
    std::snprintf(c.name, sizeof(c.name), "%s", "Default");
    c.selectedChannels = 0xffff;
    return c.engine != nullptr;
}

void destroy(const clap_plugin_t* p)
{
    auto& s = self(p);
    delete s.pendingDefault.exchange(nullptr);
    collectRetired(s);
    delete &s;
}

bool activate(const clap_plugin_t* p, double rate, uint32_t, uint32_t maxFrames)
{
    auto& s = self(p);
    s.sampleRate = rate;
    s.maxFrames = std::max<uint32_t>(1, maxFrames);
    // A choke fades over 3 ms (Oscairn RFC 0018)
    s.chokeFrames = std::max<uint32_t>(1, static_cast<uint32_t>(rate * 0.003 + 0.5));
    s.left.assign(s.maxFrames, 0.0f);
    s.right.assign(s.maxFrames, 0.0f);
    for (auto& v : s.mix)
        v.assign(s.maxFrames, 0.0f);
    for (uint32_t i = 0; i < contextCount; ++i)
        if (Engine* e = s.contexts[i].engine.get()) {
            configure(s, e->synth);
            clearContext(s, i, *e);
        }
    s.stale = 0;
    s.active = true;
    return true;
}

void deactivate(const clap_plugin_t* p)
{
    auto& s = self(p);
    s.active = false;
    if (Engine* e = s.pendingDefault.exchange(nullptr)) {
        collectRetired(s);
        s.retired.store(s.contexts[0].engine.release());
        setEngine(s.contexts[0], std::unique_ptr<Engine>(e));
    }
    collectRetired(s);
    for (auto& c : s.contexts) {
        c.sounding = 0;
        s.fading[&c - s.contexts] = false;
        if (c.engine)
            c.engine->synth.allSoundOff();
    }
    s.noteCount = 0;
}

bool startProcessing(const clap_plugin_t*) { return true; }
void stopProcessing(const clap_plugin_t*) {}

void reset(const clap_plugin_t* p)
{
    auto& s = self(p);
    // At once: the contexts the audio thread renders now; the others, which the main thread may
    // be replacing, are marked stale
    s.stale = 0;
    for (uint32_t i = 0; i < contextCount; ++i) {
        if (Engine* e = rendered(s, i))
            clearContext(s, i, *e);
        else
            s.stale |= 1u << i;
    }
    s.noteCount = 0; // its notes end without NOTE_END, as in an instance that never played them
}

clap_process_status process(const clap_plugin_t* p, const clap_process_t* process)
{
    auto& s = self(p);
    // valid for this process() only
    const clap_audio_buffer_t* ringOut = s.ringOut;
    s.ringOut = nullptr;
    if (ringOut && (ringOut->channel_count < 4 || !ringOut->data32))
        ringOut = nullptr;
    if (process->audio_outputs_count < 1 || process->audio_outputs[0].channel_count < 2)
        return CLAP_PROCESS_ERROR;
    const clap_output_events_t* out = process->out_events;

    // A new default patch: only once the main thread has collected the last replaced one
    if (!s.retired.load(std::memory_order_acquire)) {
        if (Engine* next = s.pendingDefault.exchange(nullptr, std::memory_order_acq_rel)) {
            size_t kept = 0; // the old synth's voices stop with it; its notes end now
            for (size_t i = 0; i < s.noteCount; ++i) {
                if (s.notes[i].context == 0)
                    endNote(out, s.notes[i], 0);
                else
                    s.notes[kept++] = s.notes[i];
            }
            s.noteCount = kept;
            s.retired.store(s.contexts[0].engine.release(), std::memory_order_release);
            setEngine(s.contexts[0], std::unique_ptr<Engine>(next));
            if (s.host->request_callback)
                s.host->request_callback(s.host);
        }
    }
    applySelections(s);
    for (uint32_t i = 0; s.stale && i < contextCount; ++i)
        if (Engine* e = (s.stale >> i & 1) ? rendered(s, i) : nullptr) {
            clearContext(s, i, *e); // selected since a reset, so nothing of it sounds
            s.stale &= ~(1u << i);
        }
    const uint32_t frames = process->frames_count;
    const uint32_t count = process->in_events->size(process->in_events);
    // Output buses change at the block's start: the voices started from then on take the new one
    for (uint32_t i = 0; i < count; ++i)
        applyParam(s, process->in_events->get(process->in_events, i));
    uint32_t hostBus[contextCount] {}, used = 1; // buses something may play on; Main always written
    for (uint32_t i = 0; i < contextCount; ++i)
        if (Engine* e = rendered(s, i)) {
            hostBus[i] = s.contexts[i].bus.load(std::memory_order_relaxed);
            e->synth.setHostBus(static_cast<int>(hostBus[i]));
            used |= s.busesInUse[i] |= 1u << hostBus[i];
        }
    keepWithinLimits(s, contextCount, 0); // past limits the host lowered

    const uint32_t ports = std::min(busCount, process->audio_outputs_count);
    auto port = [&](uint32_t b) {
        const clap_audio_buffer_t& o = process->audio_outputs[b];
        return o.channel_count >= 2 && o.data32 && o.data32[0] && o.data32[1];
    };
    for (uint32_t b = 1; b < ports; ++b)
        if (!(used >> b & 1) && port(b)) { // silent, and nothing rendered for it
            std::fill(process->audio_outputs[b].data32[0], process->audio_outputs[b].data32[0] + frames, 0.0f);
            std::fill(process->audio_outputs[b].data32[1], process->audio_outputs[b].data32[1] + frames, 0.0f);
        }
    uint32_t e = 0;
    for (uint32_t start = 0; start < frames; start += s.maxFrames) {
        const uint32_t n = std::min(s.maxFrames, frames - start);
        for (; e < count; ++e) {
            const auto* h = process->in_events->get(process->in_events, e);
            if (h->time >= start + n)
                break;
            applyEvent(s, h, static_cast<int>(h->time >= start ? h->time - start : 0));
        }
        for (uint32_t b = 0; b < busCount; ++b)
            if (used >> b & 1) {
                std::fill(s.mix[2 * b].begin(), s.mix[2 * b].begin() + n, 0.0f);
                std::fill(s.mix[2 * b + 1].begin(), s.mix[2 * b + 1].begin() + n, 0.0f);
            }
        for (uint32_t i = 0; i < contextCount; ++i) {
            Context& c = s.contexts[i];
            Engine* e = (c.selectedChannels.load(std::memory_order_acquire) || c.sounding.load(std::memory_order_relaxed))
                ? c.live.load(std::memory_order_relaxed) : nullptr;
            if (!e)
                continue;
            float* to[2 * busCount];
            for (uint32_t k = 0; k < 2 * busCount; ++k)
                to[k] = s.mix[k].data();
            // a retiring context (selected by no channel) rings out what it plays on Main into
            // the host's buffer: channels 0 and 1, or 2 and 3 while a choke fades it
            if (ringOut && !c.selectedChannels.load(std::memory_order_acquire)) {
                const uint32_t pair = s.fading[i] ? 2 : 0;
                to[0] = ringOut->data32[pair] + start;
                to[1] = ringOut->data32[pair + 1] + start;
            }
            float* buffers[2] = { s.left.data(), s.right.data() };
            if (s.offline.load(std::memory_order_relaxed))
                e->synth.enableFreeWheeling();
            else
                e->synth.disableFreeWheeling();
            // voices on an older bus than the context's play into it (only buses in busesInUse)
            e->synth.renderBlock(sfz::AudioSpan<float>(buffers, 2, 0, n), to, static_cast<int>(busCount));
            float* toLeft = to[2 * hostBus[i]];
            float* toRight = to[2 * hostBus[i] + 1];
            for (uint32_t k = 0; k < n; ++k) {
                toLeft[k] += s.left[k];
                toRight[k] += s.right[k];
            }
        }
        for (uint32_t b = 0; b < ports; ++b)
            if ((used >> b & 1) && port(b)) {
                std::copy(s.mix[2 * b].begin(), s.mix[2 * b].begin() + n, process->audio_outputs[b].data32[0] + start);
                std::copy(s.mix[2 * b + 1].begin(), s.mix[2 * b + 1].begin() + n, process->audio_outputs[b].data32[1] + start);
            }
    }
    for (; e < count; ++e)
        applyEvent(s, process->in_events->get(process->in_events, e), 0);

    endFinishedNotes(s, out, frames ? frames - 1 : 0);
    uint32_t notesPerContext[contextCount] {};
    for (size_t i = 0; i < s.noteCount; ++i)
        ++notesPerContext[s.notes[i].context];
    bool anySound = false;
    for (uint32_t i = 0; i < contextCount; ++i) {
        Context& c = s.contexts[i];
        const bool rendered = c.selectedChannels.load(std::memory_order_acquire) || c.sounding.load(std::memory_order_relaxed);
        if (!rendered && !notesPerContext[i])
            continue;
        Engine* e = c.live.load(std::memory_order_relaxed);
        const bool sounding = notesPerContext[i] > 0 || (e && e->synth.getNumActiveVoices() > 0);
        c.sounding.store(sounding ? 1 : 0, std::memory_order_release);
        if (!sounding) {
            s.fading[i] = false;
            s.busesInUse[i] = 0; // no voice left on an older bus
        }
        anySound = anySound || sounding;
    }
    return anySound ? CLAP_PROCESS_CONTINUE : CLAP_PROCESS_SLEEP;
}

void onMainThread(const clap_plugin_t* p) { collectRetired(self(p)); }

// Audio ports

uint32_t audioPortCount(const clap_plugin_t*, bool isInput) { return isInput ? 0 : busCount; }

bool audioPortGet(const clap_plugin_t*, uint32_t index, bool isInput, clap_audio_port_info_t* info)
{
    if (isInput || index >= busCount)
        return false;
    *info = {};
    info->id = index;
    if (index == 0)
        std::snprintf(info->name, sizeof(info->name), "%s", "Main");
    else
        std::snprintf(info->name, sizeof(info->name), "Sound bus %u", index);
    info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

const clap_plugin_audio_ports_t audioPorts { audioPortCount, audioPortGet };

// Note ports

uint32_t notePortCount(const clap_plugin_t*, bool isInput) { return isInput ? 1 : 0; }

bool notePortGet(const clap_plugin_t*, uint32_t index, bool isInput, clap_note_port_info_t* info)
{
    if (!isInput || index != 0)
        return false;
    *info = {};
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::snprintf(info->name, sizeof(info->name), "%s", "Notes");
    return true;
}

const clap_plugin_note_ports_t notePorts { notePortCount, notePortGet };

// State: a version line, then context 0's fields, each as a length line and the bytes.

bool writeAll(const clap_ostream_t* stream, const std::string& data)
{
    for (size_t at = 0; at < data.size();) {
        const int64_t n = stream->write(stream, data.data() + at, data.size() - at);
        if (n <= 0)
            return false;
        at += static_cast<size_t>(n);
    }
    return true;
}

std::string busField(uint32_t bus) { return std::string(1, static_cast<char>('0' + bus)); }

bool parseBus(const std::string& field, uint32_t& bus)
{
    if (field.size() != 1 || field[0] < '0' || field[0] >= static_cast<char>('0' + busCount))
        return false;
    bus = static_cast<uint32_t>(field[0] - '0');
    return true;
}

bool stateSave(const clap_plugin_t* p, const clap_ostream_t* stream)
{
    auto& s = self(p);
    const Patch& patch = s.patch;
    const std::string bus = busField(s.contexts[0].bus.load(std::memory_order_relaxed));
    std::string data = std::string(stateVersion) + "\n";
    for (const std::string* f : { &patch.directory, &patch.library, &patch.basePath, &patch.text, &bus })
        data += std::to_string(f->size()) + "\n" + *f + "\n";
    return writeAll(stream, data);
}

bool readField(const std::string& data, size_t& at, std::string& field)
{
    const size_t eol = data.find('\n', at);
    if (eol == std::string::npos || eol == at || eol - at > 20)
        return false;
    size_t length = 0;
    for (size_t i = at; i < eol; ++i) {
        if (data[i] < '0' || data[i] > '9')
            return false;
        length = length * 10 + static_cast<size_t>(data[i] - '0');
    }
    at = eol + 1;
    if (length > data.size() - at || data.size() - at - length < 1 || data[at + length] != '\n')
        return false;
    field.assign(data, at, length);
    at += length + 1;
    return true;
}

bool readAll(const clap_istream_t* stream, std::string& data)
{
    char buffer[4096];
    for (;;) {
        const int64_t n = stream->read(stream, buffer, sizeof(buffer));
        if (n < 0 || data.size() + static_cast<size_t>(n) > maxStateBytes)
            return false;
        if (n == 0)
            return true;
        data.append(buffer, static_cast<size_t>(n));
    }
}

// A state: context 0's patch
bool parsePatchState(const std::string& data, Patch& patch)
{
    const std::string header = std::string(stateVersion) + "\n";
    if (data.compare(0, header.size(), header) != 0)
        return false;
    size_t at = header.size();
    std::string bus;
    if (!readField(data, at, patch.directory) || !readField(data, at, patch.library) || !readField(data, at, patch.basePath)
        || !readField(data, at, patch.text) || !readField(data, at, bus) || !parseBus(bus, patch.bus))
        return false;
    return at == data.size();
}

// An engine for a saved patch, its base (if any) taken from the patch's fields
std::unique_ptr<Engine> engineFor(const Plugin& s, const Patch& patch, std::string& message)
{
    oscairn_asset_ref_t base {};
    const bool hasBase = !patch.library.empty();
    if (hasBase) {
        if (patch.library.size() >= sizeof(base.library) || patch.basePath.size() >= sizeof(base.path))
            return nullptr;
        std::snprintf(base.library, sizeof(base.library), "%s", patch.library.c_str());
        std::snprintf(base.path, sizeof(base.path), "%s", patch.basePath.c_str());
        // without the host's assets a base can only fall back to a local directory
        if (!s.assets && patch.directory.empty())
            return nullptr;
    }
    return makeEngine(s, patch, hasBase ? &base : nullptr, message);
}

bool stateLoad(const clap_plugin_t* p, const clap_istream_t* stream)
{
    auto& s = self(p);
    std::string data, message;
    Patch patch;
    if (!readAll(stream, data) || !parsePatchState(data, patch))
        return false;
    auto e = engineFor(s, patch, message);
    if (!e)
        return false;
    collectRetired(s);
    s.patch = patch;
    s.contexts[0].bus.store(patch.bus, std::memory_order_relaxed);
    if (s.active)
        delete s.pendingDefault.exchange(e.release()); // replaces one the audio thread never took
    else
        setEngine(s.contexts[0], std::move(e));
    return true;
}

const clap_plugin_state_t state { stateSave, stateLoad };

// Sound contexts (main thread, concurrently with process)

uint32_t contextCapacity(const clap_plugin_t*) { return contextCount; }

bool contextGet(const clap_plugin_t* p, uint32_t slot, oscairn_sound_context_info_t* info)
{
    auto& s = self(p);
    if (slot >= contextCount || !info || !s.contexts[slot].prepared)
        return false;
    const Context& c = s.contexts[slot];
    *info = {};
    info->id = c.id;
    std::memcpy(info->name, c.name, sizeof(c.name));
    info->selected_channels = c.selectedChannels.load(std::memory_order_acquire);
    // shortcut: 1 while the context sounds, not a voice count
    info->active_voices = c.sounding.load(std::memory_order_acquire);
    info->idle = !info->selected_channels && !c.queued.load(std::memory_order_acquire) && !info->active_voices;
    return true;
}

// A sound from its patch (none: a fresh instance's sine), with its output bus as its one parameter
uint32_t contextPrepare(const clap_plugin_t* p, const char* name, const oscairn_sound_parameter_t* parameters, uint32_t count,
                        const char* patchText, const oscairn_asset_ref_t* base, char* message, uint32_t capacity)
{
    auto& s = self(p);
    auto fail = [&](const std::string& why) {
        if (message && capacity)
            std::snprintf(message, capacity, "%s", why.c_str());
        return CLAP_INVALID_ID;
    };
    uint32_t bus = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!parameters || parameters[i].id != busParam || !(parameters[i].value >= 0 && parameters[i].value < busCount)
            || parameters[i].value != std::floor(parameters[i].value))
            return fail("the only parameter is the output bus, 0 to 8");
        bus = static_cast<uint32_t>(parameters[i].value);
    }
    if (!patchText)
        base = nullptr;
    Context* slot = nullptr;
    for (uint32_t i = 1; i < contextCount && !slot; ++i)
        if (!s.contexts[i].prepared)
            slot = &s.contexts[i];
    if (!slot || s.nextId == CLAP_INVALID_ID)
        return fail("no free sound context");
    Patch patch;
    patch.text = patchText ? patchText : freshPatch;
    if (base) { // kept for the snapshot
        patch.library.assign(base->library, strnlen(base->library, sizeof(base->library)));
        patch.basePath.assign(base->path, strnlen(base->path, sizeof(base->path)));
    }
    std::string why;
    auto e = makeEngine(s, patch, base, why);
    if (!e)
        return fail(why);
    e->id = s.nextId;
    slot->bus.store(bus, std::memory_order_relaxed);
    setEngine(*slot, std::move(e));
    slot->id = s.nextId++;
    std::snprintf(slot->name, sizeof(slot->name), "%s", name ? name : "");
    slot->prepared = true;
    return slot->id;
}

bool contextSelect(const clap_plugin_t* p, uint32_t id, int16_t channel)
{
    auto& s = self(p);
    if (channel < -1 || channel > 15)
        return false;
    const uint32_t head = s.selectionHead.load(std::memory_order_relaxed);
    if (head - s.selectionTail.load(std::memory_order_acquire) == selectionCapacity)
        return false;
    for (uint32_t i = 0; i < contextCount; ++i) {
        Context& c = s.contexts[i];
        if (!c.prepared || c.id != id)
            continue;
        c.queued.fetch_add(1, std::memory_order_relaxed);
        s.selections[head % selectionCapacity] = { i, channel };
        s.selectionHead.store(head + 1, std::memory_order_release);
        return true;
    }
    return false;
}

bool contextDiscard(const clap_plugin_t* p, uint32_t id)
{
    auto& s = self(p);
    if (id == 0)
        return false;
    for (uint32_t i = 1; i < contextCount; ++i) {
        Context& c = s.contexts[i];
        if (!c.prepared || c.id != id)
            continue;
        // Only the main thread queues selections, and notes only start on selected channels,
        // so once all three are clear the audio thread no longer touches this synth.
        if (c.selectedChannels.load(std::memory_order_acquire) || c.queued.load(std::memory_order_acquire)
            || c.sounding.load(std::memory_order_acquire))
            return false;
        c.prepared = false;
        setEngine(c, nullptr);
        return true;
    }
    return false;
}

// Sound-context snapshot (the host stops processing first): every prepared context's name, patch
// and bus, and each channel's context with the queued selections applied. Text, like the state:
//   oscairn-sampler-contexts-3\n
// then for each of the eight slots seven fields (as in the state: decimal length, \n, bytes, \n):
// "1" or empty for a free slot, name, directory, library, path, patch, output bus (one digit; 0
// for a free slot); then one field of 16 digits, the slot each MIDI channel plays. Slot 0 is the
// default context (the state's patch). Loading builds every engine before replacing any; contexts
// other than 0 get fresh IDs. A state loads too, as context 0 alone.
constexpr const char* snapshotVersion = "oscairn-sampler-contexts-3";

// The snapshot of every context, or (alone < contextCount) of that slot alone as the default on
// every channel and on Main (save_sound)
std::string snapshotText(const Plugin& s, uint32_t alone)
{
    uint32_t channels[16] {};
    if (alone == contextCount) {
        std::copy(std::begin(s.channelContext), std::end(s.channelContext), channels);
        const uint32_t head = s.selectionHead.load(std::memory_order_acquire);
        for (uint32_t tail = s.selectionTail.load(std::memory_order_acquire); tail != head; ++tail) {
            const Selection sel = s.selections[tail % selectionCapacity];
            for (int ch = 0; ch < 16; ++ch)
                if (sel.channel < 0 || sel.channel == ch)
                    channels[ch] = sel.context;
        }
    }
    std::string data = std::string(snapshotVersion) + "\n";
    auto field = [&](const std::string& f) { data += std::to_string(f.size()) + "\n" + f + "\n"; };
    for (uint32_t i = 0; i < contextCount; ++i) {
        const uint32_t from = alone == contextCount ? i : i == 0 ? alone : contextCount;
        const Context* c = from < contextCount ? &s.contexts[from] : nullptr;
        const bool used = c && c->prepared && c->engine;
        const Patch none {}, &patch = from == 0 ? s.patch : used ? c->engine->patch : none;
        field(used ? "1" : "");
        field(used ? std::string(c->name, strnlen(c->name, sizeof(c->name))) : "");
        for (const std::string* f : { &patch.directory, &patch.library, &patch.basePath, &patch.text })
            field(*f);
        field(busField(used && alone == contextCount ? c->bus.load(std::memory_order_relaxed) : 0));
    }
    std::string digits;
    for (uint32_t ch : channels)
        digits += static_cast<char>('0' + ch);
    field(digits);
    return data;
}

bool snapshotSave(const clap_plugin_t* p, const clap_ostream_t* stream) { return writeAll(stream, snapshotText(self(p), contextCount)); }

struct ParsedSnapshot {
    bool used[contextCount] {};
    std::string names[contextCount];
    Patch patches[contextCount];
    uint32_t channels[16] {};
};

// A snapshot, or a state as the default context alone
bool parseSnapshot(const std::string& data, ParsedSnapshot& out)
{
    out.used[0] = true;
    out.names[0] = "Default";
    const std::string header = std::string(snapshotVersion) + "\n";
    if (data.compare(0, header.size(), header) != 0)
        return parsePatchState(data, out.patches[0]);
    size_t at = header.size();
    std::string flag, digits, bus;
    for (uint32_t i = 0; i < contextCount; ++i) {
        Patch& patch = out.patches[i];
        if (!readField(data, at, flag) || !readField(data, at, out.names[i]) || !readField(data, at, patch.directory)
            || !readField(data, at, patch.library) || !readField(data, at, patch.basePath) || !readField(data, at, patch.text)
            || !readField(data, at, bus) || !parseBus(bus, patch.bus))
            return false;
        if ((flag != "1" && !flag.empty()) || (i == 0 && flag.empty()) || out.names[i].size() >= sizeof(Context::name))
            return false;
        out.used[i] = flag == "1";
    }
    if (!readField(data, at, digits) || at != data.size() || digits.size() != 16)
        return false;
    for (int ch = 0; ch < 16; ++ch) {
        out.channels[ch] = static_cast<uint32_t>(digits[ch] - '0');
        if (out.channels[ch] >= contextCount || !out.used[out.channels[ch]])
            return false;
    }
    return true;
}

bool snapshotLoad(const clap_plugin_t* p, const clap_istream_t* stream)
{
    auto& s = self(p);
    std::string data, message;
    ParsedSnapshot parsed;
    if (!readAll(stream, data) || !parseSnapshot(data, parsed))
        return false;
    const bool* used = parsed.used;
    const std::string* names = parsed.names;
    const Patch* patches = parsed.patches;
    const uint32_t* channels = parsed.channels;
    if (s.nextId > CLAP_INVALID_ID - contextCount)
        return false;
    std::unique_ptr<Engine> engines[contextCount];
    for (uint32_t i = 0; i < contextCount; ++i)
        if (used[i] && !(engines[i] = engineFor(s, patches[i], message)))
            return false;
    // Replace everything: no voices, notes or queued selections survive
    delete s.pendingDefault.exchange(nullptr);
    collectRetired(s);
    s.selectionTail.store(s.selectionHead.load());
    s.noteCount = 0;
    s.patch = patches[0];
    for (uint32_t i = 0; i < contextCount; ++i) {
        Context& c = s.contexts[i];
        c.id = !used[i] || i == 0 ? 0 : s.nextId++;
        if (engines[i])
            engines[i]->id = c.id;
        c.bus.store(used[i] ? patches[i].bus : 0, std::memory_order_relaxed);
        setEngine(c, std::move(engines[i]));
        c.prepared = used[i];
        std::snprintf(c.name, sizeof(c.name), "%s", used[i] ? names[i].c_str() : "");
        c.queued = 0;
        c.sounding = 0;
        s.fading[&c - s.contexts] = false;
        c.selectedChannels = 0;
    }
    for (uint32_t ch = 0; ch < 16; ++ch) {
        s.channelContext[ch] = channels[ch];
        s.contexts[channels[ch]].selectedChannels.fetch_or(1u << ch);
    }
    return true;
}

const clap_plugin_state_t snapshot { snapshotSave, snapshotLoad };

// Snapshots prepared beside (oscairn.sound-snapshots): every sound of a snapshot, its default
// included, becomes a new context in a free slot, all or nothing, nothing selected. Samples
// that another context already holds are taken from it (SharedPreloads), not read again.
bool snapshotPrepare(const clap_plugin_t* p, const clap_istream_t* stream, uint32_t* selected, char* message, uint32_t capacity)
{
    auto& s = self(p);
    auto fail = [&](const std::string& why) {
        if (message && capacity)
            std::snprintf(message, capacity, "%s", why.c_str());
        return false;
    };
    std::string data, why;
    ParsedSnapshot parsed;
    if (!stream || !readAll(stream, data) || !parseSnapshot(data, parsed))
        return fail("not a snapshot of this plugin");
    uint32_t slotFor[contextCount] {};
    uint32_t next = 1;
    for (uint32_t i = 0; i < contextCount; ++i) {
        if (!parsed.used[i])
            continue;
        while (next < contextCount && s.contexts[next].prepared)
            ++next;
        if (next == contextCount)
            return fail("not enough free sound contexts");
        slotFor[i] = next++;
    }
    if (s.nextId > CLAP_INVALID_ID - contextCount)
        return fail("no free sound context IDs");
    std::unique_ptr<Engine> engines[contextCount];
    for (uint32_t i = 0; i < contextCount; ++i)
        if (parsed.used[i] && !(engines[i] = engineFor(s, parsed.patches[i], why)))
            return fail(why.empty() ? "a patch could not be loaded" : why);
    for (uint32_t i = 0; i < contextCount; ++i) {
        if (!parsed.used[i])
            continue;
        Context& c = s.contexts[slotFor[i]];
        engines[i]->id = s.nextId;
        c.bus.store(parsed.patches[i].bus, std::memory_order_relaxed);
        setEngine(c, std::move(engines[i]));
        c.id = s.nextId++;
        std::snprintf(c.name, sizeof(c.name), "%s", parsed.names[i].c_str());
        c.prepared = true;
    }
    if (selected)
        for (int ch = 0; ch < 16; ++ch)
            selected[ch] = s.contexts[slotFor[parsed.channels[ch]]].id;
    return true;
}

// Called on the audio thread just before a process(): where retiring contexts ring out
void snapshotRingOut(const clap_plugin_t* p, const clap_audio_buffer_t* buffer) { self(p).ringOut = buffer; }

// Main thread: queued like a selection, so it applies before the selections queued after it
bool snapshotChokeRetiring(const clap_plugin_t* p)
{
    auto& s = self(p);
    const uint32_t head = s.selectionHead.load(std::memory_order_relaxed);
    if (head - s.selectionTail.load(std::memory_order_acquire) == selectionCapacity)
        return false;
    s.selections[head % selectionCapacity] = { 0, chokeMark };
    s.selectionHead.store(head + 1, std::memory_order_release);
    return true;
}

// Draft 4 (main thread): one sound as a whole snapshot of its own, its default on every channel and
// on Main; rendering excluded, as for snapshotSave
bool snapshotSaveSound(const clap_plugin_t* p, uint32_t id, const clap_ostream_t* stream)
{
    auto& s = self(p);
    for (uint32_t i = 0; i < contextCount; ++i)
        if (s.contexts[i].prepared && s.contexts[i].engine && s.contexts[i].id == id)
            return writeAll(stream, snapshotText(s, i));
    return false;
}

// Draft 4 (main thread, beside process()): a sound's values while no channel selects it, so the
// audio thread does not change them
uint32_t snapshotSoundValues(const clap_plugin_t* p, uint32_t id, oscairn_sound_parameter_t* values, uint32_t capacity)
{
    auto& s = self(p);
    for (const Context& c : s.contexts)
        if (c.prepared && c.id == id) {
            if (c.selectedChannels.load(std::memory_order_acquire))
                return 0;
            if (values && capacity)
                values[0] = { busParam, double(c.bus.load(std::memory_order_relaxed)) };
            return 1;
        }
    return 0;
}

const oscairn_plugin_sound_snapshots_t soundSnapshots { snapshotPrepare, snapshotRingOut, snapshotChokeRetiring, snapshotSaveSound, snapshotSoundValues };

const oscairn_plugin_sound_contexts_t soundContexts { contextCapacity, contextGet, contextPrepare, contextSelect, contextDiscard, &snapshot, nullptr };

// Asset dependencies: every context's resolved samples

uint32_t dependencyCount(const clap_plugin_t* p)
{
    uint32_t n = 0;
    for (auto& c : self(p).contexts)
        if (c.prepared && c.engine)
            n += static_cast<uint32_t>(c.engine->assets.size());
    return n;
}

bool dependencyGet(const clap_plugin_t* p, uint32_t index, oscairn_asset_ref_t* ref)
{
    for (auto& c : self(p).contexts) {
        if (!c.prepared || !c.engine)
            continue;
        if (index < c.engine->assets.size()) {
            *ref = c.engine->assets[index];
            return true;
        }
        index -= static_cast<uint32_t>(c.engine->assets.size());
    }
    return false;
}

const oscairn_plugin_asset_dependencies_t dependencies { dependencyCount, dependencyGet, nullptr };

// Voice budget (oscairn.voice-budget/draft-1)

// Every voice follows CLAP_NOTE_EXPRESSION_VOLUME (per key, as notes are)
bool budgetHonorsVolume(const clap_plugin_t*) { return true; }

void budgetSetLimits(const clap_plugin_t* p, uint32_t limit, const oscairn_voice_budget_context_t* contexts, uint32_t count)
{
    self(p).budget.setLimits(limit, contexts, count);
}

uint32_t budgetActiveVoices(const clap_plugin_t* p, double* voiceCost)
{
    auto& s = self(p);
    if (voiceCost)
        *voiceCost = 0; // no estimate: the host measures the instance
    uint32_t n = 0;
    for (uint32_t i = 0; i < contextCount; ++i)
        if (Engine* e = rendered(s, i))
            for (int k = 0, slots = e->synth.getNumVoiceSlots(); k < slots; ++k)
                n += !e->synth.getVoiceSlot(k)->isFree();
    return n;
}

uint32_t budgetNotes(const clap_plugin_t* p, oscairn_voice_budget_note_t* out, uint32_t capacity)
{
    auto& s = self(p);
    oscairn_sampler::VoiceBudget& report = s.budget;
    report.clearReport();
    for (uint32_t i = 0; i < contextCount; ++i) {
        Engine* e = rendered(s, i);
        if (!e)
            continue;
        uint32_t voices[128] {};
        float gain[128] {};
        for (int k = 0, slots = e->synth.getNumVoiceSlots(); k < slots; ++k) {
            const sfz::Voice& v = *e->synth.getVoiceSlot(k);
            const int key = v.getTriggerEvent().number;
            if (v.isFree() || v.getTriggerEvent().type == sfz::TriggerEventType::CC || key < 0 || key > 127)
                continue;
            ++voices[key];
            gain[key] = std::max(gain[key], v.getLastGain());
        }
        // notes sharing a key in one context share its voices (sfizz's voices know their key)
        for (size_t n = 0; n < s.noteCount; ++n) {
            const Note& note = s.notes[n];
            if (note.context == i)
                for (uint32_t k = 0; k < voices[note.key]; ++k)
                    report.addToReport(note.id, note.channel, note.key, gain[note.key]);
        }
    }
    return report.writeReport(out, capacity);
}

const oscairn_plugin_voice_budget_t voiceBudget { budgetHonorsVolume, budgetSetLimits, budgetActiveVoices, budgetNotes };

// Parameters: Output bus, each sound's own. get_value shows the default context's.

uint32_t paramCount(const clap_plugin_t*) { return 1; }

bool paramInfo(const clap_plugin_t*, uint32_t index, clap_param_info_t* info)
{
    if (index != 0)
        return false;
    *info = {};
    info->id = busParam;
    info->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM;
    std::snprintf(info->name, sizeof(info->name), "%s", "Output bus");
    info->min_value = 0;
    info->max_value = busCount - 1;
    info->default_value = 0;
    return true;
}

bool paramValue(const clap_plugin_t* p, clap_id id, double* value)
{
    if (id != busParam || !value)
        return false;
    *value = self(p).contexts[0].bus.load(std::memory_order_relaxed);
    return true;
}

bool paramToText(const clap_plugin_t*, clap_id id, double value, char* text, uint32_t capacity)
{
    if (id != busParam || !text || !capacity || !(value >= 0 && value <= busCount - 1))
        return false;
    const int bus = static_cast<int>(std::lround(value));
    if (bus == 0)
        std::snprintf(text, capacity, "%s", "Main");
    else
        std::snprintf(text, capacity, "Sound bus %d", bus);
    return true;
}

bool paramFromText(const clap_plugin_t*, clap_id id, const char* text, double* value)
{
    if (id != busParam || !text || !value)
        return false;
    if (!std::strcmp(text, "Main")) {
        *value = 0;
        return true;
    }
    unsigned bus = 0;
    char rest = 0;
    if ((std::sscanf(text, "Sound bus %u%c", &bus, &rest) != 1 && std::sscanf(text, "%u%c", &bus, &rest) != 1) || bus >= busCount)
        return false;
    *value = bus;
    return true;
}

// While not processing (the main thread when inactive, else the audio thread)
void paramFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*)
{
    for (uint32_t i = 0, n = in ? in->size(in) : 0; i < n; ++i)
        applyParam(self(p), in->get(in, i));
}

const clap_plugin_params_t params { paramCount, paramInfo, paramValue, paramToText, paramFromText, paramFlush };

bool paramHint(const clap_plugin_t*, clap_id id, oscairn_param_hint_t* hint)
{
    if (id != busParam || !hint)
        return false;
    *hint = {};
    hint->widget = OSCAIRN_WIDGET_LIST;
    hint->range_high = CLAP_INVALID_ID;
    hint->role = OSCAIRN_ROLE_OUTPUT_BUS;
    return true;
}

const oscairn_plugin_ui_t ui { paramHint };

bool renderSet(const clap_plugin_t* p, clap_plugin_render_mode mode)
{
    if (mode != CLAP_RENDER_REALTIME && mode != CLAP_RENDER_OFFLINE)
        return false;
    self(p).offline = mode == CLAP_RENDER_OFFLINE;
    return true;
}

const clap_plugin_render_t render { [](const clap_plugin_t*) { return false; }, renderSet };

const void* getExtension(const clap_plugin_t*, const char* id)
{
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &audioPorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS))
        return &notePorts;
    if (!std::strcmp(id, CLAP_EXT_STATE))
        return &state;
    if (!std::strcmp(id, OSCAIRN_EXT_SOUND_CONTEXTS))
        return &soundContexts;
    if (!std::strcmp(id, OSCAIRN_EXT_ASSET_DEPENDENCIES))
        return &dependencies;
    if (!std::strcmp(id, OSCAIRN_EXT_SOUND_SNAPSHOTS))
        return &soundSnapshots;
    if (!std::strcmp(id, OSCAIRN_EXT_VOICE_BUDGET))
        return &voiceBudget;
    if (!std::strcmp(id, CLAP_EXT_PARAMS))
        return &params;
    if (!std::strcmp(id, OSCAIRN_EXT_UI))
        return &ui;
    if (!std::strcmp(id, CLAP_EXT_RENDER))
        return &render;
    return nullptr;
}

} // namespace

size_t oscairnSamplerFileOpens(const clap_plugin_t* p)
{
    size_t n = 0;
    for (auto& c : self(p).contexts)
        if (c.engine)
            n += c.engine->synth.getResources().getFilePool().getNumFileOpens();
    return n;
}

namespace {

const char* features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SAMPLER, CLAP_PLUGIN_FEATURE_STEREO, nullptr };

const clap_plugin_descriptor_t descriptor {
    CLAP_VERSION_INIT,
    "org.oscairn.sampler",
    "Oscairn Sampler",
    "Oscairn",
    "https://github.com/oscairn/sfizz",
    "",
    "",
    "0.5.0",
    "SFZ and AWM2-class sample player, based on sfizz",
    features,
};

const clap_plugin_t* createPlugin(const clap_plugin_factory_t*, const clap_host_t* host, const char* id)
{
    if (!host || !id || std::strcmp(id, descriptor.id))
        return nullptr;
    auto* s = new Plugin;
    s->host = host;
    s->plugin.desc = &descriptor;
    s->plugin.plugin_data = s;
    s->plugin.init = init;
    s->plugin.destroy = destroy;
    s->plugin.activate = activate;
    s->plugin.deactivate = deactivate;
    s->plugin.start_processing = startProcessing;
    s->plugin.stop_processing = stopProcessing;
    s->plugin.reset = reset;
    s->plugin.process = process;
    s->plugin.get_extension = getExtension;
    s->plugin.on_main_thread = onMainThread;
    return &s->plugin;
}

uint32_t pluginCount(const clap_plugin_factory_t*) { return 1; }
const clap_plugin_descriptor_t* pluginDescriptor(const clap_plugin_factory_t*, uint32_t index)
{
    return index == 0 ? &descriptor : nullptr;
}

const clap_plugin_factory_t factory { pluginCount, pluginDescriptor, createPlugin };

bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* entryFactory(const char* id)
{
    return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &factory : nullptr;
}

} // namespace

extern "C" {
OSCAIRN_SAMPLER_EXPORT const clap_plugin_entry_t clap_entry { CLAP_VERSION_INIT, entryInit, entryDeinit, entryFactory };
}
