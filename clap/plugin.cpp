// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// A CLAP instrument around sfz::Sfizz (doc/oscairn-plan.md, section 5).
//
// The patch is SFZ text plus the directory its relative paths resolve from, both stored in
// the plugin state. A new patch is loaded into a new synth on the main thread and handed to
// the audio thread through an atomic pointer at the next process call; the replaced synth
// goes back the same way and is destroyed on the main thread.
//
// Notes: CLAP note events and MIDI note events on port 0. A note ends (NOTE_END) once the
// synth has no active voice left; sfizz does not report voices per note yet.

#include "plugin.h"
#include <sfizz.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr const char* stateVersion = "oscairn-sfizz-1";
constexpr size_t maxStateBytes = 64 << 20;
constexpr size_t maxNotes = 256;

struct Patch {
    std::string directory; // where relative sample and include paths resolve
    std::string text;      // SFZ
};

struct Engine {
    Patch patch;
    sfz::Sfizz synth;
};

struct Note {
    int32_t id;
    int16_t channel, key;
};

struct Plugin {
    clap_plugin_t plugin {};
    const clap_host_t* host { nullptr };
    double sampleRate { 48000.0 };
    uint32_t maxFrames { 1024 };
    bool active { false };

    Patch patch; // main thread: the latest patch, as saved
    std::unique_ptr<Engine> prepared; // main thread: built while inactive, used by activate
    std::atomic<Engine*> pending { nullptr }; // main -> audio
    std::atomic<Engine*> retired { nullptr }; // audio -> main
    Engine* audio { nullptr }; // audio thread while active

    Note notes[maxNotes];
    size_t noteCount { 0 };
    std::vector<float> left, right;
};

Plugin& self(const clap_plugin_t* p) { return *static_cast<Plugin*>(p->plugin_data); }

void collectRetired(Plugin& s)
{
    delete s.retired.exchange(nullptr);
}

std::unique_ptr<Engine> makeEngine(const Plugin& s, const Patch& patch)
{
    std::unique_ptr<Engine> e { new Engine };
    e->patch = patch;
    e->synth.setSampleRate(static_cast<float>(s.sampleRate));
    e->synth.setSamplesPerBlock(static_cast<int>(s.maxFrames));
    if (!patch.text.empty()) {
        // sfizz resolves relative paths from the directory of the virtual file name
        const std::string virtualPath = patch.directory + "/patch.sfz";
        if (!e->synth.loadSfzString(virtualPath, patch.text))
            return nullptr;
    }
    return e;
}

// Main thread: makes `patch` the playing patch.
bool setPatch(Plugin& s, const Patch& patch)
{
    auto e = makeEngine(s, patch);
    if (!e)
        return false;
    collectRetired(s);
    s.patch = patch;
    if (s.active)
        delete s.pending.exchange(e.release()); // replaces one the audio thread never took
    else
        s.prepared = std::move(e);
    return true;
}

void endNotes(Plugin& s, const clap_output_events_t* out, uint32_t time)
{
    for (size_t i = 0; i < s.noteCount; ++i) {
        clap_event_note_t end {};
        end.header = { sizeof(end), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_END, 0 };
        end.note_id = s.notes[i].id;
        end.port_index = 0;
        end.channel = s.notes[i].channel;
        end.key = s.notes[i].key;
        out->try_push(out, &end.header);
    }
    s.noteCount = 0;
}

void startNote(Plugin& s, int32_t id, int16_t channel, int16_t key)
{
    if (s.noteCount < maxNotes)
        s.notes[s.noteCount++] = { id, channel, key };
}

void applyEvent(Plugin& s, sfz::Sfizz& synth, const clap_event_header_t* h, int delay)
{
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return;
    switch (h->type) {
    case CLAP_EVENT_NOTE_ON:
    case CLAP_EVENT_NOTE_OFF:
    case CLAP_EVENT_NOTE_CHOKE: {
        const auto* n = reinterpret_cast<const clap_event_note_t*>(h);
        if (n->port_index > 0 || n->key < 0 || n->key > 127)
            return;
        // A velocity-0 note-on stays a note-on (Oscairn RFC 0005); sfizz needs a velocity
        // above 0 to start a voice, so use the smallest one it distinguishes.
        const float velocity = static_cast<float>(std::min(1.0, std::max(n->velocity, 0.0)));
        if (h->type == CLAP_EVENT_NOTE_ON) {
            synth.hdNoteOn(delay, n->key, std::max(velocity, 1.0f / 127.0f));
            startNote(s, n->note_id, n->channel, n->key);
        } else {
            synth.hdNoteOff(delay, n->key, velocity);
        }
        break;
    }
    case CLAP_EVENT_MIDI: {
        const auto* m = reinterpret_cast<const clap_event_midi_t*>(h);
        if (m->port_index > 0)
            return;
        const uint8_t kind = m->data[0] & 0xf0, a = m->data[1] & 0x7f, b = m->data[2] & 0x7f;
        switch (kind) {
        case 0x90:
            if (b) {
                synth.noteOn(delay, a, b);
                startNote(s, -1, m->data[0] & 0x0f, a);
                break;
            }
            synth.noteOff(delay, a, 0);
            break;
        case 0x80: synth.noteOff(delay, a, b); break;
        case 0xb0: synth.cc(delay, a, b); break;
        case 0xe0: synth.pitchWheel(delay, (b << 7 | a) - 8192); break;
        case 0xd0: synth.channelAftertouch(delay, a); break;
        case 0xa0: synth.polyAftertouch(delay, a, b); break;
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
    s.prepared = makeEngine(s, Patch {});
    return s.prepared != nullptr;
}

void destroy(const clap_plugin_t* p)
{
    auto& s = self(p);
    delete s.pending.exchange(nullptr);
    collectRetired(s);
    delete s.audio;
    delete &s;
}

bool activate(const clap_plugin_t* p, double rate, uint32_t, uint32_t maxFrames)
{
    auto& s = self(p);
    s.sampleRate = rate;
    s.maxFrames = std::max<uint32_t>(1, maxFrames);
    s.left.assign(s.maxFrames, 0.0f);
    s.right.assign(s.maxFrames, 0.0f);
    auto e = std::move(s.prepared);
    if (e) {
        e->synth.setSampleRate(static_cast<float>(rate));
        e->synth.setSamplesPerBlock(static_cast<int>(s.maxFrames));
    } else if (!(e = makeEngine(s, s.patch))) {
        return false;
    }
    s.audio = e.release();
    s.active = true;
    return true;
}

void deactivate(const clap_plugin_t* p)
{
    auto& s = self(p);
    s.active = false;
    delete s.pending.exchange(nullptr);
    collectRetired(s);
    s.prepared.reset(s.audio); // s.patch is the newest; activate rebuilds if they differ
    if (s.prepared && (s.prepared->patch.text != s.patch.text || s.prepared->patch.directory != s.patch.directory))
        s.prepared.reset();
    s.audio = nullptr;
    s.noteCount = 0;
}

bool startProcessing(const clap_plugin_t*) { return true; }
void stopProcessing(const clap_plugin_t*) {}

void reset(const clap_plugin_t* p)
{
    auto& s = self(p);
    if (s.audio)
        s.audio->synth.allSoundOff();
}

clap_process_status process(const clap_plugin_t* p, const clap_process_t* process)
{
    auto& s = self(p);
    if (process->audio_outputs_count < 1 || process->audio_outputs[0].channel_count < 2)
        return CLAP_PROCESS_ERROR;
    // Take a new patch only once the main thread has collected the last replaced synth
    if (!s.retired.load(std::memory_order_acquire)) {
        if (Engine* next = s.pending.exchange(nullptr, std::memory_order_acq_rel)) {
            s.retired.store(s.audio, std::memory_order_release);
            s.audio = next;
            endNotes(s, process->out_events, 0); // the old synth's voices stop with it
            if (s.host->request_callback)
                s.host->request_callback(s.host);
        }
    }
    sfz::Sfizz& synth = s.audio->synth;
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
            applyEvent(s, synth, h, static_cast<int>(h->time >= start ? h->time - start : 0));
        }
        float* buffers[2] = { s.left.data(), s.right.data() };
        synth.renderBlock(buffers, n, 1);
        std::copy(s.left.begin(), s.left.begin() + n, outL + start);
        std::copy(s.right.begin(), s.right.begin() + n, outR + start);
    }
    for (; e < count; ++e)
        applyEvent(s, synth, process->in_events->get(process->in_events, e), 0);
    if (synth.getNumActiveVoices() == 0) {
        endNotes(s, process->out_events, frames ? frames - 1 : 0);
        return CLAP_PROCESS_SLEEP;
    }
    return CLAP_PROCESS_CONTINUE;
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

// State: a version line, then the directory and the SFZ text, each as a length line and
// the bytes.

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
    data += std::to_string(patch.directory.size()) + "\n" + patch.directory + "\n";
    data += std::to_string(patch.text.size()) + "\n" + patch.text + "\n";
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

bool stateLoad(const clap_plugin_t* p, const clap_istream_t* stream)
{
    std::string data;
    char buffer[4096];
    for (;;) {
        const int64_t n = stream->read(stream, buffer, sizeof(buffer));
        if (n < 0 || data.size() + static_cast<size_t>(n) > maxStateBytes)
            return false;
        if (n == 0)
            break;
        data.append(buffer, static_cast<size_t>(n));
    }
    const std::string header = std::string(stateVersion) + "\n";
    if (data.compare(0, header.size(), header) != 0)
        return false;
    size_t at = header.size();
    Patch patch;
    if (!readField(data, at, patch.directory) || !readField(data, at, patch.text) || at != data.size())
        return false;
    return setPatch(self(p), patch);
}

const clap_plugin_state_t state { stateSave, stateLoad };

const void* getExtension(const clap_plugin_t*, const char* id)
{
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &audioPorts;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS))
        return &notePorts;
    if (!std::strcmp(id, CLAP_EXT_STATE))
        return &state;
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
