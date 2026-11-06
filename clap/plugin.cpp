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
// Files: with the host's oscairn.assets, every sample a patch uses is resolved through the
// host, and oscairn.asset-dependencies lists them; without it, paths resolve from the
// patch's directory.
//
// State (version 2; version 1, without the two base fields, still loads): UTF-8 text,
//   oscairn-sfizz-2\n
// then four fields, each its byte length in decimal, \n, the bytes, \n:
//   directory   - local folder relative sample paths resolve from when no base is given
//                 (or when the host offers no oscairn.assets); may be empty
//   library     - base asset reference: library name (under 64 bytes); empty for none
//   path        - base asset reference: folder inside the library (under 1024 bytes,
//                 may be empty for the library's root)
//   patch       - the SFZ text
// With a base, every sample= path (relative, no "..", after default_path) is resolved through
// the host as the asset <library>/<path>/<sample path> and listed in
// oscairn.asset-dependencies; a sample the host cannot resolve fails the state load.

#include "plugin.h"
#include "sfizz/Synth.h"
#include "sfizz/Region.h"
#include "sfizz/AudioSpan.h"
#include "sfizz/parser/Parser.h"
#include "sfizz/parser/ParserListener.h"
#include <oscairn-clap/assets.h>
#include <oscairn-clap/sound-contexts.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr const char* stateVersion = "oscairn-sfizz-2";
constexpr const char* stateVersion1 = "oscairn-sfizz-1";
constexpr size_t maxStateBytes = 64 << 20;
constexpr size_t maxNotes = 256;
constexpr uint32_t contextCount = 8;
constexpr uint32_t selectionCapacity = 32;

struct Patch {
    std::string directory; // where relative sample and include paths resolve
    std::string library;   // base asset reference for host-resolved samples (empty: none)
    std::string basePath;
    std::string text;      // SFZ
};

struct Engine {
    Patch patch;
    sfz::Synth synth;
    std::vector<oscairn_asset_ref_t> assets; // resolved through the host, for dependencies
};

struct Note {
    int32_t id;
    int16_t channel, key;
    uint32_t context;
};

struct Context {
    // Main thread
    bool prepared { false };
    uint32_t id { 0 };
    char name[64] {};
    std::unique_ptr<Engine> engine;
    // Shared: the audio thread renders a context while any of these is set; the main thread
    // frees it only when all are clear.
    std::atomic<uint32_t> selectedChannels { 0 }; // written by the audio thread
    std::atomic<uint32_t> queued { 0 };           // selections not yet applied
    std::atomic<uint32_t> sounding { 0 };         // notes or voices left, audio thread
};

struct Selection {
    uint32_t context;
    int16_t channel;
};

struct Plugin {
    clap_plugin_t plugin {};
    const clap_host_t* host { nullptr };
    const oscairn_host_assets_t* assets { nullptr };
    double sampleRate { 48000.0 };
    uint32_t maxFrames { 1024 };
    bool active { false };
    uint32_t chokeFrames { 144 };

    Patch patch; // main thread: context 0's patch, as saved
    Context contexts[contextCount];
    uint32_t nextId { 1 };

    // Main thread -> audio: a new engine for context 0 (from a state load while active)
    std::atomic<Engine*> pendingDefault { nullptr };
    std::atomic<Engine*> retired { nullptr };

    Selection selections[selectionCapacity];
    std::atomic<uint32_t> selectionHead { 0 }, selectionTail { 0 };

    // Audio thread
    uint32_t channelContext[16] {};
    Note notes[maxNotes];
    size_t noteCount { 0 };
    std::vector<float> left, right, mixLeft, mixRight;
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
        Engine* e = s.contexts[n.context].engine.get();
        if (!e || e->synth.getNumActiveVoicesForNote(n.key) == 0)
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
            if (Engine* e = s.contexts[context].engine.get())
                e->synth.hdNoteOn(delay, n->key, std::max(velocity, 1.0f / 127.0f));
            if (s.noteCount < maxNotes)
                s.notes[s.noteCount++] = { n->note_id, n->channel, n->key, context };
            return;
        }
        // Note-off and choke address the notes they match, in the context each started in
        for (size_t i = 0; i < s.noteCount; ++i) {
            const Note& note = s.notes[i];
            Engine* e = s.contexts[note.context].engine.get();
            if (!e || !matches(note, n->note_id, n->channel, n->key))
                continue;
            if (h->type == CLAP_EVENT_NOTE_OFF)
                e->synth.hdNoteOff(delay, note.key, velocity);
            else
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
            if (Engine* e = s.contexts[note.context].engine.get())
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
        Engine* e = s.contexts[s.channelContext[channel]].engine.get();
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
    c.engine = makeEngine(s, Patch {}, nullptr, message);
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
    for (auto* v : { &s.left, &s.right, &s.mixLeft, &s.mixRight })
        v->assign(s.maxFrames, 0.0f);
    for (auto& c : s.contexts)
        if (c.engine)
            configure(s, c.engine->synth);
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
        s.contexts[0].engine.reset(e);
    }
    collectRetired(s);
    for (auto& c : s.contexts) {
        c.sounding = 0;
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
    for (auto& c : s.contexts)
        if (c.sounding.load() && c.engine)
            c.engine->synth.allSoundOff();
}

clap_process_status process(const clap_plugin_t* p, const clap_process_t* process)
{
    auto& s = self(p);
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
            s.contexts[0].engine.reset(next);
            if (s.host->request_callback)
                s.host->request_callback(s.host);
        }
    }
    applySelections(s);

    float* outL = process->audio_outputs[0].data32[0];
    float* outR = process->audio_outputs[0].data32[1];
    const uint32_t frames = process->frames_count;
    const uint32_t count = process->in_events->size(process->in_events);
    uint32_t e = 0;
    for (uint32_t start = 0; start < frames; start += s.maxFrames) {
        const uint32_t n = std::min(s.maxFrames, frames - start);
        for (; e < count; ++e) {
            const auto* h = process->in_events->get(process->in_events, e);
            if (h->time >= start + n)
                break;
            applyEvent(s, h, static_cast<int>(h->time >= start ? h->time - start : 0));
        }
        std::fill(s.mixLeft.begin(), s.mixLeft.begin() + n, 0.0f);
        std::fill(s.mixRight.begin(), s.mixRight.begin() + n, 0.0f);
        for (uint32_t i = 0; i < contextCount; ++i) {
            Context& c = s.contexts[i];
            // Check the flags before the engine: an unused context's engine may be freed
            if ((!c.selectedChannels.load(std::memory_order_acquire) && !c.sounding.load(std::memory_order_relaxed)) || !c.engine)
                continue;
            float* buffers[2] = { s.left.data(), s.right.data() };
            c.engine->synth.renderBlock(sfz::AudioSpan<float>(buffers, 2, 0, n));
            for (uint32_t k = 0; k < n; ++k) {
                s.mixLeft[k] += s.left[k];
                s.mixRight[k] += s.right[k];
            }
        }
        std::copy(s.mixLeft.begin(), s.mixLeft.begin() + n, outL + start);
        std::copy(s.mixRight.begin(), s.mixRight.begin() + n, outR + start);
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
        const bool sounding = notesPerContext[i] > 0 || (c.engine && c.engine->synth.getNumActiveVoices() > 0);
        c.sounding.store(sounding ? 1 : 0, std::memory_order_release);
        anySound = anySound || sounding;
    }
    return anySound ? CLAP_PROCESS_CONTINUE : CLAP_PROCESS_SLEEP;
}

void onMainThread(const clap_plugin_t* p) { collectRetired(self(p)); }

// Audio ports

uint32_t audioPortCount(const clap_plugin_t*, bool isInput) { return isInput ? 0 : 1; }

bool audioPortGet(const clap_plugin_t*, uint32_t index, bool isInput, clap_audio_port_info_t* info)
{
    if (isInput || index != 0)
        return false;
    *info = {};
    info->id = 0;
    std::snprintf(info->name, sizeof(info->name), "%s", "Main");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
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

// State: a version line, then the directory and the SFZ text of context 0, each as a length
// line and the bytes.

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

bool stateSave(const clap_plugin_t* p, const clap_ostream_t* stream)
{
    const Patch& patch = self(p).patch;
    std::string data = std::string(stateVersion) + "\n";
    for (const std::string* f : { &patch.directory, &patch.library, &patch.basePath, &patch.text })
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

// A version 1 or 2 state: context 0's patch
bool parsePatchState(const std::string& data, Patch& patch)
{
    const std::string header = std::string(stateVersion) + "\n";
    const std::string header1 = std::string(stateVersion1) + "\n";
    const bool v1 = data.compare(0, header1.size(), header1) == 0;
    if (!v1 && data.compare(0, header.size(), header) != 0)
        return false;
    size_t at = v1 ? header1.size() : header.size();
    if (!readField(data, at, patch.directory))
        return false;
    if (!v1 && (!readField(data, at, patch.library) || !readField(data, at, patch.basePath)))
        return false;
    return readField(data, at, patch.text) && at == data.size();
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
    if (s.active)
        delete s.pendingDefault.exchange(e.release()); // replaces one the audio thread never took
    else
        s.contexts[0].engine = std::move(e);
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

uint32_t contextPrepare(const clap_plugin_t* p, const char* name, const oscairn_sound_parameter_t*, uint32_t count,
                        const char* patchText, const oscairn_asset_ref_t* base, char* message, uint32_t capacity)
{
    auto& s = self(p);
    auto fail = [&](const std::string& why) {
        if (message && capacity)
            std::snprintf(message, capacity, "%s", why.c_str());
        return CLAP_INVALID_ID;
    };
    if (count)
        return fail("this plugin has no parameters");
    if (!patchText)
        return fail("a patch is required");
    Context* slot = nullptr;
    for (uint32_t i = 1; i < contextCount && !slot; ++i)
        if (!s.contexts[i].prepared)
            slot = &s.contexts[i];
    if (!slot || s.nextId == CLAP_INVALID_ID)
        return fail("no free sound context");
    Patch patch;
    patch.text = patchText;
    if (base) { // kept for the snapshot
        patch.library.assign(base->library, strnlen(base->library, sizeof(base->library)));
        patch.basePath.assign(base->path, strnlen(base->path, sizeof(base->path)));
    }
    std::string why;
    auto e = makeEngine(s, patch, base, why);
    if (!e)
        return fail(why);
    slot->engine = std::move(e);
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
        c.engine.reset();
        return true;
    }
    return false;
}

// Sound-context snapshot (the host stops processing first): every prepared context's name and
// patch, and each channel's context with the queued selections applied. Text, like the state:
//   oscairn-sfizz-contexts-1\n
// then for each of the eight slots six fields (as in the state: decimal length, \n, bytes, \n):
// "1" or empty for a free slot, name, directory, library, path, patch; then one field of 16
// digits, the slot each MIDI channel plays. Slot 0 is the default context (the state's patch).
// Loading builds every engine before replacing any; contexts other than 0 get fresh IDs. A
// version 1 or 2 state loads too, as context 0 alone.
constexpr const char* snapshotVersion = "oscairn-sfizz-contexts-1";

bool snapshotSave(const clap_plugin_t* p, const clap_ostream_t* stream)
{
    auto& s = self(p);
    uint32_t channels[16];
    std::copy(std::begin(s.channelContext), std::end(s.channelContext), channels);
    const uint32_t head = s.selectionHead.load(std::memory_order_acquire);
    for (uint32_t tail = s.selectionTail.load(std::memory_order_acquire); tail != head; ++tail) {
        const Selection sel = s.selections[tail % selectionCapacity];
        for (int ch = 0; ch < 16; ++ch)
            if (sel.channel < 0 || sel.channel == ch)
                channels[ch] = sel.context;
    }
    std::string data = std::string(snapshotVersion) + "\n";
    auto field = [&](const std::string& f) { data += std::to_string(f.size()) + "\n" + f + "\n"; };
    for (uint32_t i = 0; i < contextCount; ++i) {
        const Context& c = s.contexts[i];
        const bool used = c.prepared && c.engine;
        const Patch none {}, &patch = i == 0 ? s.patch : used ? c.engine->patch : none;
        field(used ? "1" : "");
        field(used ? std::string(c.name, strnlen(c.name, sizeof(c.name))) : "");
        for (const std::string* f : { &patch.directory, &patch.library, &patch.basePath, &patch.text })
            field(*f);
    }
    std::string digits;
    for (uint32_t ch : channels)
        digits += static_cast<char>('0' + ch);
    field(digits);
    return writeAll(stream, data);
}

bool snapshotLoad(const clap_plugin_t* p, const clap_istream_t* stream)
{
    auto& s = self(p);
    std::string data, message;
    if (!readAll(stream, data))
        return false;
    bool used[contextCount] {};
    std::string names[contextCount];
    Patch patches[contextCount];
    uint32_t channels[16] {};
    used[0] = true;
    names[0] = "Default";
    const std::string header = std::string(snapshotVersion) + "\n";
    if (data.compare(0, header.size(), header) == 0) {
        size_t at = header.size();
        std::string flag, digits;
        for (uint32_t i = 0; i < contextCount; ++i) {
            Patch& patch = patches[i];
            if (!readField(data, at, flag) || !readField(data, at, names[i]) || !readField(data, at, patch.directory)
                || !readField(data, at, patch.library) || !readField(data, at, patch.basePath) || !readField(data, at, patch.text))
                return false;
            if ((flag != "1" && !flag.empty()) || (i == 0 && flag.empty()) || names[i].size() >= sizeof(Context::name))
                return false;
            used[i] = flag == "1";
        }
        if (!readField(data, at, digits) || at != data.size() || digits.size() != 16)
            return false;
        for (int ch = 0; ch < 16; ++ch) {
            channels[ch] = static_cast<uint32_t>(digits[ch] - '0');
            if (channels[ch] >= contextCount || !used[channels[ch]])
                return false;
        }
    } else if (!parsePatchState(data, patches[0]))
        return false;
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
        c.engine = std::move(engines[i]);
        c.prepared = used[i];
        c.id = !used[i] || i == 0 ? 0 : s.nextId++;
        std::snprintf(c.name, sizeof(c.name), "%s", used[i] ? names[i].c_str() : "");
        c.queued = 0;
        c.sounding = 0;
        c.selectedChannels = 0;
    }
    for (uint32_t ch = 0; ch < 16; ++ch) {
        s.channelContext[ch] = channels[ch];
        s.contexts[channels[ch]].selectedChannels.fetch_or(1u << ch);
    }
    return true;
}

const clap_plugin_state_t snapshot { snapshotSave, snapshotLoad };

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
    return nullptr;
}

const char* features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SAMPLER, CLAP_PLUGIN_FEATURE_STEREO, nullptr };

const clap_plugin_descriptor_t descriptor {
    CLAP_VERSION_INIT,
    "org.oscairn.sfizz",
    "Oscairn sfizz",
    "Oscairn",
    "https://github.com/oscairn/sfizz",
    "",
    "",
    "0.1.0",
    "SFZ sample player, sfizz with Oscairn's extensions",
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
OSCAIRN_SFIZZ_EXPORT const clap_plugin_entry_t clap_entry { CLAP_VERSION_INIT, entryInit, entryDeinit, entryFactory };
}
