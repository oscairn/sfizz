// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// Tests of the CLAP instrument through its entry point, with a minimal host.

#include "plugin.h"
#include <dlfcn.h>
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
const void* hostExtension(const clap_host_t*, const char*) { return nullptr; }
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
        static_cast<Events*>(l->ctx)->out.emplace_back(b, b + h->size);
        return true;
    }
    template <class T> void add(const T& e)
    {
        auto* b = reinterpret_cast<const uint8_t*>(&e);
        in.emplace_back(b, b + sizeof(T));
    }
    void note(uint16_t type, uint32_t time, int32_t id, int16_t key, double velocity)
    {
        clap_event_note_t n {};
        n.header = { sizeof(n), time, CLAP_CORE_EVENT_SPACE_ID, type, 0 };
        n.note_id = id; n.port_index = 0; n.channel = 0; n.key = key; n.velocity = velocity;
        add(n);
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
        status = plugin->process(plugin, &p);
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

std::string stateFor(const std::string& text)
{
    return "oscairn-sfizz-1\n0\n\n" + std::to_string(text.size()) + "\n" + text + "\n";
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
    clap_entry.deinit();
    std::printf(failures ? "%d failures\n" : "All CLAP tests passed\n", failures);
    return failures ? 1 : 0;
}
