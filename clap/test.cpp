// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// Tests of the CLAP instrument through its entry point, with a minimal host.

#include "plugin.h"
#include <oscairn-clap/assets.h>
#include <oscairn-clap/sound-contexts.h>
#include <dlfcn.h>
#include <atomic>
#include <cstdlib>
#include <new>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

int callbacks = 0;
// Allocation guard: counts allocations the processing thread makes inside process() (sfizz's
// own loader threads may allocate meanwhile)
thread_local bool guarded = false;
std::atomic<int> guardedAllocations { 0 };

// Assets: the test library "lib" maps to the sfizz test files folder; "missing" fails
bool resolveAsset(const clap_host_t*, oscairn_asset_ref_t* ref, char* path, uint32_t capacity, char* message, uint32_t messageCapacity)
{
    if (std::strcmp(ref->library, "lib") != 0) {
        std::snprintf(message, messageCapacity, "%s", "no such library");
        return false;
    }
    std::string p = ref->path;
    if (p.compare(0, 4, "sub/") == 0)
        p = p.substr(4);
    std::snprintf(path, capacity, "%s/%s", OSCAIRN_TEST_FILES, p.c_str());
    std::snprintf(ref->sha256, sizeof(ref->sha256), "%s", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    return true;
}
const oscairn_host_assets_t hostAssets { resolveAsset };
bool offerAssets = false;
const void* hostExtension(const clap_host_t*, const char* id)
{
    return offerAssets && !std::strcmp(id, OSCAIRN_EXT_ASSETS) ? &hostAssets : nullptr;
}
void hostRequest(const clap_host_t*) {}
void hostCallback(const clap_host_t*) { ++callbacks; }
const clap_host_t host { CLAP_VERSION_INIT, nullptr, "test", "", "", "1", hostExtension, hostRequest, hostRequest, hostCallback };

struct Events {
    std::vector<std::vector<uint8_t>> in, out;
    clap_input_events_t input { this, size, get };
    clap_output_events_t output { this, push };
    static uint32_t size(const clap_input_events_t* l) { return static_cast<uint32_t>(static_cast<Events*>(l->ctx)->in.size()); }
    static const clap_event_header_t* get(const clap_input_events_t* l, uint32_t i)
    {
        return reinterpret_cast<const clap_event_header_t*>(static_cast<Events*>(l->ctx)->in[i].data());
    }
    static bool push(const clap_output_events_t* l, const clap_event_header_t* h)
    {
        auto* b = reinterpret_cast<const uint8_t*>(h);
        const bool was = guarded; // the host's own storage is not the plugin's
        guarded = false;
        static_cast<Events*>(l->ctx)->out.emplace_back(b, b + h->size);
        guarded = was;
        return true;
    }
    template <class T> void add(const T& e)
    {
        auto* b = reinterpret_cast<const uint8_t*>(&e);
        in.emplace_back(b, b + sizeof(T));
    }
    void note(uint16_t type, uint32_t time, int32_t id, int16_t key, double velocity, int16_t channel = 0)
    {
        clap_event_note_t n {};
        n.header = { sizeof(n), time, CLAP_CORE_EVENT_SPACE_ID, type, 0 };
        n.note_id = id; n.port_index = 0; n.channel = channel; n.key = key; n.velocity = velocity;
        add(n);
    }
    void volume(uint32_t time, int32_t id, double value)
    {
        clap_event_note_expression_t x {};
        x.header = { sizeof(x), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0 };
        x.expression_id = CLAP_NOTE_EXPRESSION_VOLUME; x.note_id = id; x.port_index = 0; x.channel = -1; x.key = -1; x.value = value;
        add(x);
    }
    int ends(int32_t id) const
    {
        int count = 0;
        for (auto& e : out) {
            auto* n = reinterpret_cast<const clap_event_note_t*>(e.data());
            count += n->header.type == CLAP_EVENT_NOTE_END && n->note_id == id;
        }
        return count;
    }
};

struct Run {
    const clap_plugin_t* plugin;
    std::vector<float> left, right;
    clap_process_status status = CLAP_PROCESS_CONTINUE;
    float peak = 0;
    void block(Events& events, uint32_t frames)
    {
        left.assign(frames, 1.0f); right.assign(frames, 1.0f);
        float* channels[2] = { left.data(), right.data() };
        clap_audio_buffer_t out {};
        out.data32 = channels; out.channel_count = 2;
        clap_process_t p {};
        p.frames_count = frames; p.audio_outputs = &out; p.audio_outputs_count = 1;
        p.in_events = &events.input; p.out_events = &events.output; p.steady_time = -1;
        guarded = true;
        status = plugin->process(plugin, &p);
        guarded = false;
        peak = 0;
        for (uint32_t i = 0; i < frames; ++i)
            peak = std::fmax(peak, std::fmax(std::fabs(left[i]), std::fabs(right[i])));
        events.in.clear();
    }
};

struct Buffer {
    std::string data; size_t at = 0;
    clap_ostream_t out { this, write };
    clap_istream_t in { this, read };
    static int64_t write(const clap_ostream_t* s, const void* b, uint64_t n)
    {
        static_cast<Buffer*>(s->ctx)->data.append(static_cast<const char*>(b), n); return static_cast<int64_t>(n);
    }
    static int64_t read(const clap_istream_t* s, void* b, uint64_t n)
    {
        auto* self = static_cast<Buffer*>(s->ctx);
        n = std::min<uint64_t>(n, self->data.size() - self->at);
        std::memcpy(b, self->data.data() + self->at, n); self->at += n; return static_cast<int64_t>(n);
    }
};

// State version 2: directory, base library, base path, patch text
std::string stateFor(const std::string& text, const std::string& library = "", const std::string& path = "", const std::string& directory = "")
{
    std::string s = "oscairn-sfizz-2\n";
    for (const std::string* f : { &directory, &library, &path, &text })
        s += std::to_string(f->size()) + "\n" + *f + "\n";
    return s;
}

bool load(const clap_plugin_t* plugin, const std::string& state)
{
    auto* ext = static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
    Buffer b; b.data = state;
    return ext->load(plugin, &b.in);
}

std::string save(const clap_plugin_t* plugin)
{
    auto* ext = static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
    Buffer b;
    return ext->save(plugin, &b.out) ? b.data : std::string("save failed");
}

const clap_plugin_t* create()
{
    CHECK(clap_entry.init(""));
    auto* factory = static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    CHECK(factory && factory->get_plugin_count(factory) == 1);
    auto* desc = factory->get_plugin_descriptor(factory, 0);
    auto* plugin = factory->create_plugin(factory, &host, desc->id);
    CHECK(plugin && plugin->init(plugin));
    return plugin;
}

const std::string sine = "<region> sample=*sine ampeg_release=0.01\n";

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1) { // the built module exports a working entry
        void* module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        CHECK(module != nullptr);
        auto* entry = module ? static_cast<const clap_plugin_entry_t*>(dlsym(module, "clap_entry")) : nullptr;
        CHECK(entry && entry->init(argv[1]));
        if (entry) {
            auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
            CHECK(factory && factory->get_plugin_count(factory) == 1);
            entry->deinit();
        }
    }
    { // interface, ports
        auto* plugin = create();
        CHECK(plugin->init && plugin->destroy && plugin->activate && plugin->deactivate && plugin->start_processing
              && plugin->stop_processing && plugin->reset && plugin->process && plugin->get_extension && plugin->on_main_thread);
        auto* audio = static_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
        auto* notes = static_cast<const clap_plugin_note_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS));
        CHECK(audio && audio->count(plugin, true) == 0 && audio->count(plugin, false) == 1);
        clap_audio_port_info_t a {};
        CHECK(audio->get(plugin, 0, false, &a) && a.channel_count == 2);
        CHECK(notes && notes->count(plugin, true) == 1 && notes->count(plugin, false) == 0);
        clap_note_port_info_t n {};
        CHECK(notes->get(plugin, 0, true, &n) && (n.supported_dialects & CLAP_NOTE_DIALECT_MIDI));
        plugin->destroy(plugin);
    }
    { // a note sounds, ends with NOTE_END after its release, and the plugin sleeps
        auto* plugin = create();
        CHECK(load(plugin, stateFor(sine)));
        CHECK(plugin->activate(plugin, 48000, 1, 512));
        Run run { plugin };
        Events events;
        run.block(events, 512);
        CHECK(run.peak == 0.0f && run.status == CLAP_PROCESS_SLEEP);
        events.note(CLAP_EVENT_NOTE_ON, 100, 7, 69, 0.8);
        run.block(events, 512);
        CHECK(run.peak > 0.1f && run.status == CLAP_PROCESS_CONTINUE);
        CHECK(run.left[99] == 0.0f && run.left[200] != 0.0f); // starts at its event time
        events.note(CLAP_EVENT_NOTE_OFF, 0, 7, 69, 0.0);
        run.block(events, 512);
        for (int i = 0; i < 20 && run.status != CLAP_PROCESS_SLEEP; ++i)
            run.block(events, 512);
        CHECK(run.status == CLAP_PROCESS_SLEEP);
        run.block(events, 512);
        CHECK(run.status == CLAP_PROCESS_SLEEP && run.peak == 0.0f);
        CHECK(events.ends(7) == 1);
        // velocity 0 stays a note-on; MIDI notes play too
        events.note(CLAP_EVENT_NOTE_ON, 0, 8, 60, 0.0);
        run.block(events, 512);
        CHECK(run.peak > 0.0f);
        clap_event_midi_t midi {};
        midi.header = { sizeof(midi), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
        midi.data[0] = 0x90; midi.data[1] = 72; midi.data[2] = 100;
        events.add(midi);
        run.block(events, 512);
        CHECK(run.peak > 0.1f);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    { // state round trip, rejection of bad state, a patch swapped while active
        auto* plugin = create();
        CHECK(load(plugin, stateFor(sine)));
        CHECK(save(plugin) == stateFor(sine));
        // version 1 (no base asset reference) still loads
        CHECK(load(plugin, "oscairn-sfizz-1\n0\n\n" + std::to_string(sine.size()) + "\n" + sine + "\n"));
        CHECK(save(plugin) == stateFor(sine));
        CHECK(!load(plugin, "oscairn-sfizz-1\n0\n\n999\nshort\n"));
        CHECK(!load(plugin, "something else\n"));
        CHECK(save(plugin) == stateFor(sine));
        CHECK(plugin->activate(plugin, 44100, 1, 256));
        Run run { plugin };
        Events events;
        const std::string silent = "<region> sample=*silence\n";
        CHECK(load(plugin, stateFor(silent)));
        CHECK(save(plugin) == stateFor(silent));
        run.block(events, 256); // takes the new patch
        plugin->on_main_thread(plugin); // frees the old one
        events.note(CLAP_EVENT_NOTE_ON, 0, 1, 69, 1.0);
        run.block(events, 256);
        CHECK(run.peak == 0.0f);
        CHECK(callbacks > 0);
        plugin->deactivate(plugin);
        CHECK(save(plugin) == stateFor(silent));
        plugin->destroy(plugin);
    }
    { // per-note NOTE_END: a released note ends while another keeps sounding
        auto* plugin = create();
        CHECK(load(plugin, stateFor("<region> sample=*sine ampeg_release=0.005\n")));
        CHECK(plugin->activate(plugin, 48000, 1, 256));
        Run run { plugin };
        Events events;
        events.note(CLAP_EVENT_NOTE_ON, 0, 1, 60, 1.0);
        events.note(CLAP_EVENT_NOTE_ON, 0, 2, 64, 1.0);
        run.block(events, 256);
        events.note(CLAP_EVENT_NOTE_OFF, 0, 1, 60, 0.0);
        for (int i = 0; i < 40 && !events.ends(1); ++i)
            run.block(events, 256);
        CHECK(events.ends(1) == 1);
        CHECK(events.ends(2) == 0);
        CHECK(run.peak > 0.1f && run.status == CLAP_PROCESS_CONTINUE);
        // a choke fades note 2 over 3 ms (144 frames) and ends it, although its release is long
        events.note(CLAP_EVENT_NOTE_CHOKE, 10, 2, 64, 0.0);
        run.block(events, 256);
        CHECK(run.left[5] != 0.0f);
        CHECK(run.left[10 + 144 + 2] == 0.0f && run.left[255] == 0.0f);
        CHECK(events.ends(2) == 1);
        CHECK(run.status == CLAP_PROCESS_SLEEP);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    { // per-note volume scales only its note
        auto* plugin = create();
        CHECK(load(plugin, stateFor("<region> sample=*sine amp_veltrack=0\n")));
        CHECK(plugin->activate(plugin, 48000, 1, 512));
        Run run { plugin };
        Events events;
        events.note(CLAP_EVENT_NOTE_ON, 0, 5, 69, 1.0);
        for (int i = 0; i < 4; ++i)
            run.block(events, 512);
        const float full = run.peak;
        events.volume(0, 5, 0.25);
        for (int i = 0; i < 8; ++i)
            run.block(events, 512);
        CHECK(std::fabs(run.peak - 0.25f * full) < 0.01f * full);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    { // sound contexts: prepare, select per channel, held notes keep theirs, discard rules
        auto* plugin = create();
        CHECK(load(plugin, stateFor("<region> sample=*silence\n")));
        auto* ctx = static_cast<const oscairn_plugin_sound_contexts_t*>(plugin->get_extension(plugin, OSCAIRN_EXT_SOUND_CONTEXTS));
        CHECK(ctx && ctx->capacity(plugin) == 8);
        // the draft Oscairn recognises; no presets to prepare
        CHECK(plugin->get_extension(plugin, "oscairn.sound-contexts/draft-5") == ctx && !ctx->prepare_preset);
        char message[256] {};
        const uint32_t sineId = ctx->prepare(plugin, "Sine", nullptr, 0, "<region> sample=*sine ampeg_release=0.005", nullptr, message, sizeof(message));
        CHECK(sineId != CLAP_INVALID_ID && sineId != 0);
        CHECK(ctx->prepare(plugin, "Bad", nullptr, 1, "<region> sample=*sine", nullptr, message, sizeof(message)) == CLAP_INVALID_ID);
        CHECK(plugin->activate(plugin, 48000, 1, 256));
        Run run { plugin };
        Events events;
        CHECK(ctx->select(plugin, sineId, 1));
        events.note(CLAP_EVENT_NOTE_ON, 0, 10, 69, 1.0, 0); // default (silent) context
        run.block(events, 256);
        CHECK(run.peak == 0.0f);
        events.note(CLAP_EVENT_NOTE_ON, 0, 11, 69, 1.0, 1); // sine on channel 1
        run.block(events, 256);
        CHECK(run.peak > 0.1f);
        oscairn_sound_context_info_t info {};
        CHECK(ctx->get(plugin, 1, &info) && info.id == sineId && info.selected_channels == 2u);
        // switching channel 1 back to the default keeps note 11 on the sine context
        CHECK(ctx->select(plugin, 0, 1));
        run.block(events, 256);
        CHECK(run.peak > 0.1f);
        CHECK(!ctx->discard(plugin, sineId)); // still sounding
        events.note(CLAP_EVENT_NOTE_OFF, 0, 11, 69, 0.0, 1);
        for (int i = 0; i < 40 && !events.ends(11); ++i)
            run.block(events, 256);
        run.block(events, 256);
        CHECK(events.ends(11) == 1);
        CHECK(ctx->get(plugin, 1, &info) && info.idle);
        CHECK(ctx->discard(plugin, sineId));
        CHECK(!ctx->get(plugin, 1, &info));
        CHECK(!ctx->discard(plugin, 0));
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    { // assets: samples resolved through the host and listed as dependencies
        offerAssets = true;
        auto* plugin = create();
        auto* ctx = static_cast<const oscairn_plugin_sound_contexts_t*>(plugin->get_extension(plugin, OSCAIRN_EXT_SOUND_CONTEXTS));
        auto* deps = static_cast<const oscairn_plugin_asset_dependencies_t*>(plugin->get_extension(plugin, OSCAIRN_EXT_ASSET_DEPENDENCIES));
        oscairn_asset_ref_t base {};
        std::snprintf(base.library, sizeof(base.library), "%s", "lib");
        std::snprintf(base.path, sizeof(base.path), "%s", "sub");
        char message[256] {};
        const uint32_t id = ctx->prepare(plugin, "Kick", nullptr, 0, "<region> sample=kick.wav", &base, message, sizeof(message));
        CHECK(id != CLAP_INVALID_ID);
        CHECK(deps && deps->count(plugin) == 1);
        oscairn_asset_ref_t ref {};
        CHECK(deps->get(plugin, 0, &ref) && !std::strcmp(ref.path, "sub/kick.wav") && !std::strcmp(ref.library, "lib") && std::strlen(ref.sha256) == 64);
        std::snprintf(base.library, sizeof(base.library), "%s", "missing");
        CHECK(ctx->prepare(plugin, "Kick", nullptr, 0, "<region> sample=kick.wav", &base, message, sizeof(message)) == CLAP_INVALID_ID);
        CHECK(std::strstr(message, "kick.wav") != nullptr);
        CHECK(ctx->prepare(plugin, "Up", nullptr, 0, "<region> sample=../kick.wav", &base, message, sizeof(message)) == CLAP_INVALID_ID);
        // the kick plays
        CHECK(plugin->activate(plugin, 48000, 1, 256));
        Run run { plugin };
        Events events;
        CHECK(ctx->select(plugin, id, -1));
        events.note(CLAP_EVENT_NOTE_ON, 0, 1, 60, 1.0);
        run.block(events, 256);
        run.block(events, 256);
        CHECK(run.peak > 0.01f);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
        // the state carries a base asset reference: its samples resolve through the host, are
        // listed as dependencies, play, and round-trip
        plugin = create();
        deps = static_cast<const oscairn_plugin_asset_dependencies_t*>(plugin->get_extension(plugin, OSCAIRN_EXT_ASSET_DEPENDENCIES));
        const std::string kick = "<region> sample=kick.wav";
        CHECK(load(plugin, stateFor(kick, "lib", "sub")));
        CHECK(deps->count(plugin) == 1);
        ref = {};
        CHECK(deps->get(plugin, 0, &ref) && !std::strcmp(ref.path, "sub/kick.wav") && !std::strcmp(ref.library, "lib") && std::strlen(ref.sha256) == 64);
        CHECK(save(plugin) == stateFor(kick, "lib", "sub"));
        CHECK(!load(plugin, stateFor(kick, "missing", "sub"))); // the host cannot resolve it
        CHECK(!load(plugin, stateFor("<region> sample=../kick.wav", "lib", "sub")));
        CHECK(save(plugin) == stateFor(kick, "lib", "sub")); // a failed load keeps the state
        CHECK(plugin->activate(plugin, 48000, 1, 256));
        Run kickRun { plugin };
        Events kickEvents;
        kickEvents.note(CLAP_EVENT_NOTE_ON, 0, 1, 60, 1.0);
        kickRun.block(kickEvents, 256);
        kickRun.block(kickEvents, 256);
        CHECK(kickRun.peak > 0.01f);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
        offerAssets = false;
        // without the host's assets, a base needs a local directory to fall back to
        plugin = create();
        CHECK(!load(plugin, stateFor(kick, "lib", "sub")));
        plugin->destroy(plugin);
    }
    { // a key-off element: a release region alone keeps its note until the note-off, plays then,
      // and ends on its element envelope
        auto* plugin = create();
        CHECK(load(plugin, stateFor("<control> oscairn_profile=1 <region> sample=*sine trigger=release rt_dead=1 amp_veltrack=0 "
                                    "oscairn_ampeg_decay1=40 oscairn_ampeg_decay1_level=0 oscairn_ampeg_decay2_level=0\n")));
        CHECK(plugin->activate(plugin, 48000, 1, 256));
        Run run { plugin };
        Events events;
        events.note(CLAP_EVENT_NOTE_ON, 0, 7, 60, 1.0);
        for (int i = 0; i < 20; ++i)
            run.block(events, 256);
        CHECK(run.peak == 0.0f);
        CHECK(events.ends(7) == 0); // no voice yet, but the note stays for its note-off
        events.note(CLAP_EVENT_NOTE_OFF, 0, 7, 60, 0.0);
        run.block(events, 256);
        CHECK(run.peak > 0.1f);
        CHECK(events.ends(7) == 0);
        for (int i = 0; i < 400 && !events.ends(7); ++i)
            run.block(events, 256);
        CHECK(events.ends(7) == 1);
        CHECK(run.peak < 1e-3f); // the -90 dB floor
        // a key without release regions still ends its note when its voices end
        CHECK(load(plugin, stateFor("<region> sample=*sine key=60 loop_mode=one_shot end=100\n")));
        events.note(CLAP_EVENT_NOTE_ON, 0, 8, 60, 1.0);
        for (int i = 0; i < 20 && !events.ends(8); ++i)
            run.block(events, 256);
        CHECK(events.ends(8) == 1);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    { // snapshots prepared beside: every sound a new context, nothing selected, all or nothing,
      // samples another context holds not read again
        auto* plugin = create();
        auto* ctx = static_cast<const oscairn_plugin_sound_contexts_t*>(plugin->get_extension(plugin, OSCAIRN_EXT_SOUND_CONTEXTS));
        auto* snaps = static_cast<const oscairn_plugin_sound_snapshots_t*>(plugin->get_extension(plugin, OSCAIRN_EXT_SOUND_SNAPSHOTS));
        CHECK(snaps && snaps->prepare);
        const std::string kick = "<region> sample=kick.wav loop_mode=one_shot";
        CHECK(load(plugin, stateFor(kick, "", "", OSCAIRN_TEST_FILES)));
        const size_t opened = oscairnSfizzFileOpens(plugin);
        CHECK(opened > 0);
        Buffer saved;
        CHECK(ctx->state->save(plugin, &saved.out));
        Buffer in; in.data = saved.data;
        uint32_t selected[16] {};
        char message[256] {};
        CHECK(snaps->prepare(plugin, &in.in, selected, message, sizeof(message)));
        CHECK(oscairnSfizzFileOpens(plugin) == opened); // nothing read from disk
        oscairn_sound_context_info_t info {};
        CHECK(ctx->get(plugin, 1, &info) && info.id != 0 && !std::strcmp(info.name, "Default") && info.selected_channels == 0);
        for (uint32_t id : selected)
            CHECK(id == info.id);
        CHECK(ctx->get(plugin, 0, &info) && info.selected_channels == 0xffffu); // still playing the old one
        // it plays as the old one did
        auto play = [&](uint32_t id) {
            CHECK(ctx->select(plugin, id, 0));
            Run run { plugin };
            Events events;
            events.note(CLAP_EVENT_NOTE_ON, 0, 1, 60, 1.0);
            std::vector<float> out;
            for (int i = 0; i < 8; ++i) {
                run.block(events, 256);
                out.insert(out.end(), run.left.begin(), run.left.end());
            }
            events.note(CLAP_EVENT_NOTE_OFF, 0, 1, 60, 0.0);
            for (int i = 0; i < 100; ++i)
                run.block(events, 256); // the one-shot kick ends
            return out;
        };
        // the reference: a fresh instance with the same state, first note
        auto* fresh = create();
        CHECK(load(fresh, stateFor(kick, "", "", OSCAIRN_TEST_FILES)));
        CHECK(fresh->activate(fresh, 48000, 1, 256));
        std::vector<float> before;
        {
            Run run { fresh };
            Events events;
            events.note(CLAP_EVENT_NOTE_ON, 0, 1, 60, 1.0);
            for (int i = 0; i < 8; ++i) {
                run.block(events, 256);
                before.insert(before.end(), run.left.begin(), run.left.end());
            }
        }
        fresh->deactivate(fresh);
        fresh->destroy(fresh);
        CHECK(plugin->activate(plugin, 48000, 1, 256));
        CHECK(ctx->get(plugin, 1, &info));
        const auto after = play(info.id);
        CHECK(before == after);
        CHECK(*std::max_element(after.begin(), after.end()) > 0.01f);
        plugin->deactivate(plugin);
        // all or nothing: six more fit (slots 2-7), a seventh does not, and changes nothing
        for (int i = 0; i < 6; ++i) {
            Buffer again; again.data = saved.data;
            CHECK(snaps->prepare(plugin, &again.in, selected, message, sizeof(message)));
        }
        Buffer full; full.data = saved.data;
        CHECK(!snaps->prepare(plugin, &full.in, selected, message, sizeof(message)));
        CHECK(std::strstr(message, "free") != nullptr);
        Buffer bad; bad.data = "oscairn-sfizz-contexts-1\nnonsense";
        CHECK(!snaps->prepare(plugin, &bad.in, selected, message, sizeof(message)));
        plugin->destroy(plugin);
    }
    { // ring-out: a retiring context (selected by no channel) plays into channels 0 and 1 of the
      // host's buffer during the next process() only; choke_retiring fades it into 2 and 3
        auto* plugin = create();
        auto* ctx = static_cast<const oscairn_plugin_sound_contexts_t*>(plugin->get_extension(plugin, OSCAIRN_EXT_SOUND_CONTEXTS));
        auto* snaps = static_cast<const oscairn_plugin_sound_snapshots_t*>(plugin->get_extension(plugin, OSCAIRN_EXT_SOUND_SNAPSHOTS));
        CHECK(snaps && snaps->ring_out && snaps->choke_retiring);
        CHECK(load(plugin, stateFor(sine)));
        Buffer saved;
        CHECK(ctx->state->save(plugin, &saved.out));
        uint32_t selected[16] {};
        char message[256] {};
        CHECK(snaps->prepare(plugin, &saved.in, selected, message, sizeof(message)));
        CHECK(plugin->activate(plugin, 48000, 1, 256));
        Run run { plugin };
        Events events;
        events.note(CLAP_EVENT_NOTE_ON, 0, 1, 69, 1.0); // held on the default context
        run.block(events, 256);
        CHECK(run.peak > 0.1f);
        CHECK(ctx->select(plugin, selected[0], -1)); // every channel to the new sound
        std::vector<std::vector<float>> ringData(4, std::vector<float>(256, 0.0f));
        float* ringChannels[4] = { ringData[0].data(), ringData[1].data(), ringData[2].data(), ringData[3].data() };
        clap_audio_buffer_t ring {};
        ring.data32 = ringChannels;
        ring.channel_count = 4;
        auto peakOf = [](const std::vector<float>& v) { float m = 0; for (float x : v) m = std::max(m, std::fabs(x)); return m; };
        auto ringBlock = [&]() {
            for (auto& c : ringData)
                std::fill(c.begin(), c.end(), 0.0f);
            snaps->ring_out(plugin, &ring);
            run.block(events, 256);
        };
        ringBlock();
        CHECK(run.peak == 0.0f);
        CHECK(peakOf(ringData[0]) > 0.1f);
        CHECK(peakOf(ringData[2]) == 0.0f);
        run.block(events, 256); // no ring_out before this one: the main port again
        CHECK(run.peak > 0.1f);
        // a second change: the retiring context is choked, its fade in channels 2 and 3
        CHECK(snaps->choke_retiring(plugin));
        ringBlock();
        CHECK(peakOf(ringData[0]) == 0.0f);
        CHECK(peakOf(ringData[2]) > 0.01f);
        for (int i = 0; i < 4; ++i)
            ringBlock();
        CHECK(peakOf(ringData[2]) == 0.0f); // ended (3 ms fade)
        oscairn_sound_context_info_t info {};
        CHECK(ctx->get(plugin, 0, &info) && info.idle);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    { // a fresh instance plays a sine
        auto* plugin = create();
        CHECK(plugin->activate(plugin, 48000, 1, 256));
        Run run { plugin };
        Events events;
        events.note(CLAP_EVENT_NOTE_ON, 0, 1, 69, 1.0);
        run.block(events, 256);
        CHECK(run.peak > 0.1f);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    { // sound-context snapshot: every context, queued selections applied, fresh IDs on load
        auto contexts = [](const clap_plugin_t* p) {
            return static_cast<const oscairn_plugin_sound_contexts_t*>(p->get_extension(p, OSCAIRN_EXT_SOUND_CONTEXTS));
        };
        auto* plugin = create();
        auto* ctx = contexts(plugin);
        CHECK(ctx->state && ctx->state->save && ctx->state->load);
        CHECK(load(plugin, stateFor("<region> sample=*silence\n")));
        char message[256] {};
        const uint32_t sineId = ctx->prepare(plugin, "Sine", nullptr, 0, sine.c_str(), nullptr, message, sizeof(message));
        CHECK(sineId != CLAP_INVALID_ID && ctx->select(plugin, sineId, 3)); // queued, not yet applied
        Buffer saved;
        CHECK(ctx->state->save(plugin, &saved.out));
        Buffer in; in.data = saved.data;
        CHECK(ctx->state->load(plugin, &in.in));
        oscairn_sound_context_info_t info {};
        CHECK(ctx->get(plugin, 1, &info) && info.id != sineId && info.id != 0 && !std::strcmp(info.name, "Sine")
              && info.selected_channels == 8u && info.idle == false);
        CHECK(ctx->get(plugin, 0, &info) && info.id == 0 && info.selected_channels == 0xfff7u);
        CHECK(!ctx->select(plugin, sineId, 0)); // the old ID is gone
        Buffer again;
        CHECK(ctx->state->save(plugin, &again.out) && again.data == saved.data);
        CHECK(save(plugin) == stateFor("<region> sample=*silence\n"));
        CHECK(plugin->activate(plugin, 48000, 1, 256));
        Run run { plugin };
        Events events;
        events.note(CLAP_EVENT_NOTE_ON, 0, 1, 69, 1.0, 0);
        run.block(events, 256);
        CHECK(run.peak == 0.0f);
        events.note(CLAP_EVENT_NOTE_ON, 0, 2, 69, 1.0, 3);
        run.block(events, 256);
        CHECK(run.peak > 0.1f);
        plugin->deactivate(plugin);
        // a version 2 state loads as the default context alone
        Buffer v2; v2.data = stateFor(sine);
        CHECK(ctx->state->load(plugin, &v2.in));
        CHECK(!ctx->get(plugin, 1, &info));
        CHECK(ctx->get(plugin, 0, &info) && info.selected_channels == 0xffffu);
        CHECK(save(plugin) == stateFor(sine));
        // an invalid snapshot (channel 15 on a free slot) changes nothing
        Buffer bad; bad.data = saved.data;
        bad.data[bad.data.size() - 2] = '5';
        CHECK(!ctx->state->load(plugin, &bad.in));
        CHECK(!ctx->get(plugin, 1, &info) && save(plugin) == stateFor(sine));
        plugin->destroy(plugin);
    }
    // No allocation in any process call above
    CHECK(guardedAllocations.load() == 0);
    if (guardedAllocations.load())
        std::printf("%d allocations while processing\n", guardedAllocations.load());

    clap_entry.deinit();
    std::printf(failures ? "%d failures\n" : "All CLAP tests passed\n", failures);
    return failures ? 1 : 0;
}

// Allocation guard (see `guarded`)
void* operator new(std::size_t size)
{
    if (guarded)
        guardedAllocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
