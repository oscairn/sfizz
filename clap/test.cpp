// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

// Tests of the CLAP instrument through its entry point, with a minimal host.

#include "plugin.h"
#include <oscairn-clap/assets.h>
#include <oscairn-clap/memory.h>
#include <oscairn-clap/sound-contexts.h>
#include <oscairn-clap/ui.h>
#include <oscairn-clap/voice-budget.h>
#include "sfizz/FilePool.h"
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <dlfcn.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

int callbacks = 0;
// Allocation guard: counts allocations the processing thread makes inside process() (sfizz's
// own loader threads may allocate meanwhile)
thread_local bool guarded = false;
std::atomic<int> guardedAllocations { 0 };
bool offlineRenders = false; // set while the plugins process offline, which may allocate

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
    // The Output bus of the sound `channel` selects (-1: every selected sound)
    void bus(uint32_t time, int16_t channel, double value)
    {
        clap_event_param_value_t e {};
        e.header = { sizeof(e), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
        e.param_id = 1; e.note_id = -1; e.port_index = -1; e.channel = channel; e.key = -1; e.value = value;
        add(e);
    }
    // Assign Switch 1 or 2 of the sound `channel` selects (-1: every selected sound)
    void assignSwitch(uint32_t time, int which, int16_t channel, double value)
    {
        clap_event_param_value_t e {};
        e.header = { sizeof(e), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
        e.param_id = static_cast<clap_id>(1 + which); e.note_id = -1; e.port_index = -1; e.channel = channel; e.key = -1; e.value = value;
        add(e);
    }
    void midi(uint32_t time, uint8_t status, uint8_t a, uint8_t b)
    {
        clap_event_midi_t m {};
        m.header = { sizeof(m), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
        m.data[0] = status; m.data[1] = a; m.data[2] = b;
        add(m);
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
    const clap_event_transport_t* transport = nullptr;
    void block(Events& events, uint32_t frames)
    {
        left.assign(frames, 1.0f); right.assign(frames, 1.0f);
        float* channels[2] = { left.data(), right.data() };
        clap_audio_buffer_t out {};
        out.data32 = channels; out.channel_count = 2;
        clap_process_t p {};
        p.frames_count = frames; p.audio_outputs = &out; p.audio_outputs_count = 1;
        p.in_events = &events.input; p.out_events = &events.output; p.steady_time = -1; p.transport = transport;
        guarded = !offlineRenders;
        status = plugin->process(plugin, &p);
        guarded = false;
        peak = 0;
        for (uint32_t i = 0; i < frames; ++i)
            peak = std::fmax(peak, std::fmax(std::fabs(left[i]), std::fabs(right[i])));
        events.in.clear();
    }
};

// A process() with Main and the eight sound buses, each filled with 1 first (the plugin must write
// every one), keeping every bus's left channel over the blocks
struct BusRun {
    const clap_plugin_t* plugin;
    std::vector<std::vector<float>> data = std::vector<std::vector<float>>(18);
    std::vector<std::vector<float>> kept = std::vector<std::vector<float>>(9);
    void block(Events& events, uint32_t frames)
    {
        float* channels[18];
        clap_audio_buffer_t out[9] {};
        for (int c = 0; c < 18; ++c) {
            data[c].assign(frames, 1.0f);
            channels[c] = data[c].data();
        }
        for (int b = 0; b < 9; ++b) {
            out[b].data32 = channels + 2 * b;
            out[b].channel_count = 2;
        }
        clap_process_t p {};
        p.frames_count = frames; p.audio_outputs = out; p.audio_outputs_count = 9;
        p.in_events = &events.input; p.out_events = &events.output; p.steady_time = -1;
        guarded = true;
        plugin->process(plugin, &p);
        guarded = false;
        for (int b = 0; b < 9; ++b)
            kept[b].insert(kept[b].end(), data[2 * b].begin(), data[2 * b].end());
        events.in.clear();
    }
    float peak(int bus, size_t from = 0, size_t to = SIZE_MAX) const
    {
        float m = 0;
        for (size_t i = from; i < std::min(to, kept[bus].size()); ++i)
            m = std::max(m, std::fabs(kept[bus][i]));
        return m;
    }
};
float largestStep(const std::vector<float>& v, size_t from = 0)
{
    float m = 0;
    for (size_t i = std::max<size_t>(from, 1); i < v.size(); ++i)
        m = std::max(m, std::fabs(v[i] - v[i - 1]));
    return m;
}
float largestDifference(const std::vector<float>& a, const std::vector<float>& b)
{
    float m = a.size() == b.size() ? 0.0f : 1e9f;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        m = std::max(m, std::fabs(a[i] - b[i]));
    return m;
}

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

// State version 4: directory, base library, base path, patch text, output bus
std::string stateFor(const std::string& text, const std::string& library = "", const std::string& path = "", const std::string& directory = "",
                     const std::string& bus = "0")
{
    std::string s = "oscairn-sampler-4\n";
    for (const std::string* f : { &directory, &library, &path, &text, &bus })
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

// Events at frames from the start, played in blocks of `block` frames: Main's output, left and
// right interleaved; the plugin's output events are added to `out`
std::vector<float> play(const clap_plugin_t* plugin, const Events& all, uint32_t frames, Events* out = nullptr, uint32_t block = 256)
{
    std::vector<std::vector<uint8_t>> in = all.in;
    auto time = [](const std::vector<uint8_t>& e) { return reinterpret_cast<const clap_event_header_t*>(e.data())->time; };
    // in time order, events at one time as given (not std::stable_sort: its buffer comes from the
    // nothrow operator new, which the allocation guard below does not replace)
    std::vector<size_t> order(in.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return time(in[a]) != time(in[b]) ? time(in[a]) < time(in[b]) : a < b; });
    std::vector<std::vector<uint8_t>> sorted;
    for (size_t i : order)
        sorted.push_back(in[i]);
    in.swap(sorted);
    Run run { plugin };
    Events events;
    std::vector<float> lr;
    lr.reserve(2 * frames);
    size_t next = 0;
    for (uint32_t start = 0; start < frames; start += block) {
        const uint32_t n = std::min(block, frames - start);
        for (; next < in.size() && time(in[next]) < start + n; ++next) {
            events.in.push_back(in[next]);
            reinterpret_cast<clap_event_header_t*>(events.in.back().data())->time -= std::min(start, time(in[next]));
        }
        run.block(events, n);
        for (uint32_t i = 0; i < n; ++i) {
            lr.push_back(run.left[i]);
            lr.push_back(run.right[i]);
        }
    }
    if (out)
        out->out.insert(out->out.end(), events.out.begin(), events.out.end());
    return lr;
}

double rms(const std::vector<float>& lr, size_t from, size_t to)
{
    double e = 0;
    for (size_t i = 2 * from; i < 2 * to && i < lr.size(); ++i)
        e += double(lr[i]) * lr[i];
    return std::sqrt(e / double(2 * (to - from)));
}

std::string readFile(const std::string& path)
{
    std::ifstream f(path);
    std::stringstream b;
    b << f.rdbuf();
    return b.str();
}

// The .sfz files directly in a folder, sorted
std::vector<std::string> sfzFiles(const std::string& dir)
{
    std::vector<std::string> r;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".sfz") == 0)
                r.push_back(dir + "/" + name);
        }
        closedir(d);
    }
    std::sort(r.begin(), r.end());
    return r;
}

const std::string sine = "<region> sample=*sine ampeg_release=0.01\n";

// Voice budget calls, on the audio thread (guarded like process)
const oscairn_plugin_voice_budget_t* budgetOf(const clap_plugin_t* p)
{
    return static_cast<const oscairn_plugin_voice_budget_t*>(p->get_extension(p, OSCAIRN_EXT_VOICE_BUDGET));
}
void setLimits(const clap_plugin_t* p, uint32_t limit, std::vector<oscairn_voice_budget_context_t> contexts = {})
{
    CHECK(budgetOf(p));
    guarded = true;
    if (budgetOf(p))
        budgetOf(p)->set_limits(p, limit, contexts.data(), static_cast<uint32_t>(contexts.size()));
    guarded = false;
}
double lastCost = -1; // the voice cost the last activeVoices() call reported
uint32_t activeVoices(const clap_plugin_t* p)
{
    if (!budgetOf(p))
        return 0;
    double cost = -1;
    guarded = true;
    const uint32_t n = budgetOf(p)->active_voices(p, &cost);
    guarded = false;
    lastCost = cost;
    return n;
}
struct BudgetNotes {
    oscairn_voice_budget_note_t notes[64] {};
    uint32_t count = 0;
    explicit BudgetNotes(const clap_plugin_t* p)
    {
        guarded = true;
        count = budgetOf(p) ? budgetOf(p)->notes(p, notes, 64) : 0;
        guarded = false;
    }
    const oscairn_voice_budget_note_t* find(int32_t id) const
    {
        for (uint32_t i = 0; i < count; ++i)
            if (notes[i].note_id == id)
                return &notes[i];
        return nullptr;
    }
    uint32_t voices(int32_t id) const { return find(id) ? find(id)->voices : 0; }
    float loudness(int32_t id) const { return find(id) ? find(id)->loudness : -1.0f; }
};
oscairn_voice_budget_context_t selectedEntry(uint32_t limit, uint32_t reserve, uint8_t priority)
{
    oscairn_voice_budget_context_t c {};
    c.context = OSCAIRN_VOICE_BUDGET_SELECTED;
    c.limit = limit;
    c.reserve = reserve;
    c.priority = priority;
    return c;
}
// Four elements a note under the profile, each with the element envelope and velocity, at
// element levels 0, -6, -12 and -18 dB (the converter keeps an element's level in volume)
const std::string layered =
    "<control> oscairn_profile=1\n<group> sample=*sine oscairn_ampeg_attack=0 oscairn_ampeg_release=60 oscairn_amp_velsens=32\n"
    "<region> volume=0\n<region> volume=-6\n<region> volume=-12\n<region> volume=-18\n";

// Bytes malloc has handed out and not had back; 0 where unknown. Sanitizers replace malloc, so
// their builds skip the checks that use it.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool heapKnown = false;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool heapKnown = false;
#else
constexpr bool heapKnown = true;
#endif
#else
constexpr bool heapKnown = true;
#endif
double heapMB()
{
#if defined(__APPLE__)
    malloc_statistics_t st;
    malloc_zone_statistics(nullptr, &st);
    return st.size_in_use / 1048576.0;
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || __GLIBC_MINOR__ >= 33)
    const struct mallinfo2 m = mallinfo2();
    return (m.uordblks + m.hblkhd) / 1048576.0;
#else
    return 0;
#endif
}

// A folder of long mono 16-bit sine files at 44.1 kHz, one a key from 48, and a patch playing
// them (written at run time; no audio is kept in the repository)
std::string longFiles(int count, double seconds)
{
    char dir[] = "/tmp/oscairn-sampler-test-XXXXXX";
    if (!mkdtemp(dir))
        return "";
    const uint32_t frames = static_cast<uint32_t>(seconds * 44100);
    std::vector<int16_t> pcm(frames);
    std::string patch;
    for (int i = 0; i < count; ++i) {
        for (uint32_t n = 0; n < frames; ++n)
            pcm[n] = static_cast<int16_t>(30000 * std::sin(2 * M_PI * (110.0 + 10 * i) * n / 44100.0));
        const std::string name = "long" + std::to_string(i) + ".wav";
        std::ofstream f(std::string(dir) + "/" + name, std::ios::binary);
        auto u32 = [&f](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
        auto u16 = [&f](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
        f.write("RIFF", 4); u32(36 + 2 * frames); f.write("WAVEfmt ", 8); u32(16); u16(1); u16(1);
        u32(44100); u32(88200); u16(2); u16(16); f.write("data", 4); u32(2 * frames);
        f.write(reinterpret_cast<const char*>(pcm.data()), 2 * frames);
        patch += "<region> key=" + std::to_string(48 + i) + " amp_veltrack=0 sample=" + name + "\n";
    }
    std::ofstream(std::string(dir) + "/patch.sfz") << patch;
    return dir;
}
void removeFiles(const std::string& dir)
{
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d))
            if (e->d_name[0] != '.')
                std::remove((dir + "/" + e->d_name).c_str());
        closedir(d);
    }
    rmdir(dir.c_str());
}
// The process's threads
int threadCount()
{
#if defined(__APPLE__)
    thread_act_array_t threads;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
        return -1;
    for (mach_msg_type_number_t i = 0; i < count; ++i)
        mach_port_deallocate(mach_task_self(), threads[i]);
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), count * sizeof(thread_act_t));
    return static_cast<int>(count);
#else
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
        if (line.compare(0, 8, "Threads:") == 0)
            return std::atoi(line.c_str() + 8);
    return -1;
#endif
}
const oscairn_plugin_memory_t* memoryOf(const clap_plugin_t* p)
{
    return static_cast<const oscairn_plugin_memory_t*>(p->get_extension(p, OSCAIRN_EXT_MEMORY));
}
double totalMB(const oscairn_memory_t& m, bool withCache)
{
    const double cache = withCache ? 0 : sfz::FilePool::getStreamCacheCapacity();
    return (m.shared_bytes - cache + m.context_bytes + m.asset_bytes) / 1048576.0;
}

// Note lifetimes: a sound, 128-frame blocks at 48 kHz (375 a second), and what the plugin reports
struct Probe {
    const clap_plugin_t* plugin;
    Run run;
    Events events;
    explicit Probe(const std::string& sfz, uint32_t frames = 128) : plugin(create()), run { plugin }
    {
        CHECK(load(plugin, stateFor(sfz, "", "", OSCAIRN_TEST_FILES)));
        CHECK(plugin->activate(plugin, 48000, 1, frames));
    }
    ~Probe()
    {
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    void on(int32_t id, int16_t key, int16_t channel = 0, double velocity = 1.0, uint32_t time = 0)
    {
        events.note(CLAP_EVENT_NOTE_ON, time, id, key, velocity, channel);
    }
    void off(int32_t id, int16_t key, int16_t channel = 0, uint32_t time = 0)
    {
        events.note(CLAP_EVENT_NOTE_OFF, time, id, key, 0.0, channel);
    }
    void choke(int32_t id, int16_t key, int16_t channel = 0) { events.note(CLAP_EVENT_NOTE_CHOKE, 0, id, key, 0.0, channel); }
    void cc(uint8_t number, uint8_t value, int channel = 0) { events.midi(0, static_cast<uint8_t>(0xb0 | channel), number, value); }
    void blocks(int n)
    {
        for (int i = 0; i < n; ++i)
            run.block(events, 128);
    }
    void seconds(double s) { blocks(static_cast<int>(s * 375)); }
    uint32_t voices() { return activeVoices(plugin); }
    // Blocks until no voice sounds, at most `limit` seconds: seconds taken, or -1
    double settle(double limit = 3.0)
    {
        for (int b = 0; b < static_cast<int>(limit * 375); ++b) {
            if (!voices())
                return b / 375.0;
            blocks(1);
        }
        return voices() ? -1.0 : limit;
    }
};

// Sounds the lifetimes are tried on: a looped sample (loop_continuous and loop_sustain), a plain
// endless one, a one-shot, and a converted sound's elements (two sustained, one key-off element)
const std::string loopContinuous = "<region> sample=looped_flute.wav loop_mode=loop_continuous ampeg_release=0.05\n";
const std::string loopSustain = "<region> sample=looped_flute.wav loop_mode=loop_sustain ampeg_release=0.05\n";
const std::string endless = "<region> sample=*sine ampeg_release=0.05\n";
const std::string oneShot = "<region> sample=looped_flute.wav loop_mode=one_shot ampeg_release=0.05\n";
const std::string keyed = "<region> sample=*sine key=60 ampeg_release=0.05\n";
const std::string elements =
    "<control> oscairn_profile=1\n<global> amp_veltrack=0 sample=*sine oscairn_ampeg_attack=0 oscairn_ampeg_release=40\n"
    "<region> oscairn_element=1\n<region> oscairn_element=2 volume=-6\n"
    "<region> oscairn_element=3 trigger=release rt_dead=1 oscairn_keyoff=1\n";

struct Scenario {
    const char* name;
    const std::string* sound;
    void (*play)(Probe&); // the sequence after the note (id 1, key 60, 0.5 s on); the first block is not yet run
    bool quiet;           // every voice ends within 5 s
    int ends;             // NOTE_END for id 1 over the scenario
    int other = -1;       // another note id the sequence starts, which has the same fate
};

// A sequence that leaves voices sounding is correct when SFZ says so (a sustain pedal left down, a
// sostenuto that caught the note, a note never released): the host's all-notes-off, a choke or a
// reset ends those
const Scenario scenarios[] = {
    { "note-on, never a note-off", &loopContinuous, [](Probe&) {}, false, 0 },
    { "matching note-off", &loopContinuous, [](Probe& p) { p.off(1, 60); }, true, 1 },
    { "note-off, id differs, key and channel match", &loopContinuous, [](Probe& p) { p.off(99, 60); }, true, 1 },
    { "note-off, id -1", &loopContinuous, [](Probe& p) { p.off(-1, 60); }, true, 1 },
    { "note-off, id matches, key differs", &loopContinuous, [](Probe& p) { p.off(1, 61); }, true, 1 },
    { "note-off, id matches, channel differs", &loopContinuous, [](Probe& p) { p.off(1, 60, 1); }, true, 1 },
    { "note-off, id, key and channel all differ", &loopContinuous, [](Probe& p) { p.off(99, 61, 1); }, false, 0 },
    { "two note-ons on a key (ids 1, 2), note-off 1", &loopContinuous, [](Probe& p) { p.on(2, 60); p.off(1, 60); }, true, 1, 2 },
    { "two note-ons on a key (ids 1, 2), note-off 2", &loopContinuous, [](Probe& p) { p.on(2, 60); p.off(2, 60); }, true, 1, 2 },
    { "two note-ons on a key, ids -1, note-off -1", &loopContinuous, [](Probe& p) { p.on(-1, 60); p.off(-1, 60); }, true, 1 },
    { "the same note-on twice (a retrigger), one note-off", &loopContinuous, [](Probe& p) { p.on(1, 60); p.off(1, 60); }, true, 1 },
    { "note-off before its note-on, one block", &loopContinuous, [](Probe& p) { p.off(1, 60); p.off(5, 62, 0, 10); p.on(5, 62, 0, 1.0, 20); }, false, 1, 5 },
    { "note-on and note-off at the same sample", &loopContinuous, [](Probe& p) { p.off(1, 60); p.on(5, 62, 0, 1.0, 20); p.off(5, 62, 0, 20); }, true, 1, 5 },
    { "note-off in an earlier block than its note-on", &loopContinuous, [](Probe& p) { p.off(1, 60); p.off(5, 62); p.blocks(1); p.on(5, 62); }, false, 1, 5 },
    { "MIDI note-on of velocity 0 is a note-off", &loopContinuous, [](Probe& p) { p.events.midi(0, 0x90, 60, 0); }, true, 1 },
    { "CLAP note-on of velocity 0 is a note-on", &loopContinuous, [](Probe& p) { p.off(1, 60); p.on(7, 62, 0, 0.0); }, false, 1, 7 },
    { "sustain down, note-off, sustain up", &loopContinuous, [](Probe& p) { p.cc(64, 127); p.blocks(1); p.off(1, 60); p.blocks(40); p.cc(64, 0); }, true, 1 },
    { "sustain left down after the note-off", &loopContinuous, [](Probe& p) { p.cc(64, 127); p.blocks(1); p.off(1, 60); }, false, 0 },
    { "sustain left down, then CC 120", &loopContinuous, [](Probe& p) { p.cc(64, 127); p.blocks(1); p.off(1, 60); p.blocks(40); p.cc(120, 0); }, true, 1 },
    { "sustain left down, then CC 123", &loopContinuous, [](Probe& p) { p.cc(64, 127); p.blocks(1); p.off(1, 60); p.blocks(40); p.cc(123, 0); }, true, 1 },
    { "note held, CC 120", &loopContinuous, [](Probe& p) { p.cc(120, 0); }, true, 1 },
    { "note held, CC 123", &loopContinuous, [](Probe& p) { p.cc(123, 0); }, true, 1 },
    { "sostenuto down over a held note, note-off", &loopContinuous, [](Probe& p) { p.cc(66, 127); p.blocks(1); p.off(1, 60); }, false, 0 },
    { "sostenuto down over a held note, note-off, sostenuto up", &loopContinuous, [](Probe& p) { p.cc(66, 127); p.blocks(1); p.off(1, 60); p.blocks(40); p.cc(66, 0); }, true, 1 },
    { "note held, reset() (no NOTE_END: the host ends it)", &loopContinuous, [](Probe& p) { p.plugin->reset(p.plugin); }, true, 0 },
    { "note held, deactivate and activate (no NOTE_END)", &loopContinuous, [](Probe& p) { p.plugin->deactivate(p.plugin); p.plugin->activate(p.plugin, 48000, 1, 128); }, true, 0 },
    { "note held, choke with id -1", &loopContinuous, [](Probe& p) { p.choke(-1, 60); }, true, 1 },
    { "note held, choke with its id", &loopContinuous, [](Probe& p) { p.choke(1, 60); }, true, 1 },
    { "pitch bend, then note-off", &loopContinuous, [](Probe& p) { p.events.midi(0, 0xe0, 0, 127); p.off(1, 60); }, true, 1 },
    { "note-on with a channel that is not one (never starts)", &loopContinuous, [](Probe& p) { p.off(1, 60); p.on(9, 62, -1); }, true, 1, 9 },
    { "note-on with a key that is not one (never starts)", &loopContinuous, [](Probe& p) { p.off(1, 60); p.on(9, 200); }, true, 1, 9 },
    { "loop_sustain, note-off", &loopSustain, [](Probe& p) { p.off(1, 60); }, true, 1 },
    { "loop_sustain, never released", &loopSustain, [](Probe&) {}, false, 0 },
    { "endless sample, note-off", &endless, [](Probe& p) { p.off(1, 60); }, true, 1 },
    { "one-shot, never released: the sample ends", &oneShot, [](Probe&) {}, true, 1 },
    { "one-shot, note-off", &oneShot, [](Probe& p) { p.off(1, 60); }, true, 1 },
    { "a key no region covers", &keyed, [](Probe& p) { p.off(1, 60); p.on(9, 61); }, true, 1, 9 },
    { "elements, note-off (the key-off element rings 2 s)", &elements, [](Probe& p) { p.off(1, 60); }, true, 1 },
    { "elements, never released", &elements, [](Probe&) {}, false, 0 },
    { "elements, sustain down, note-off, sustain up", &elements, [](Probe& p) { p.cc(64, 127); p.blocks(1); p.off(1, 60); p.blocks(40); p.cc(64, 0); }, true, 1 },
    { "elements, sustain left down", &elements, [](Probe& p) { p.cc(64, 127); p.blocks(1); p.off(1, 60); }, false, 0 },
    { "elements, note-off with another id", &elements, [](Probe& p) { p.off(99, 60); }, true, 1 },
    { "elements, choke", &elements, [](Probe& p) { p.choke(1, 60); }, true, 1 },
    { "elements, CC 123", &elements, [](Probe& p) { p.cc(123, 0); }, true, 1 },
};

void noteLifetimes()
{
    for (const Scenario& sc : scenarios) {
        Probe p(*sc.sound);
        p.on(1, 60);
        p.seconds(0.5);
        CHECK(p.voices() > 0);
        sc.play(p);
        const double took = p.settle(5.0);
        const bool quiet = took >= 0;
        if (quiet != sc.quiet || p.events.ends(1) != sc.ends || (sc.other >= 0 && p.events.ends(sc.other) != (sc.quiet ? 1 : 0)) ) {
            std::printf("scenario \"%s\": %s after %.2f s, NOTE_END %d, other %d\n", sc.name, quiet ? "quiet" : "sounding", took, p.events.ends(1),
                        sc.other >= 0 ? p.events.ends(sc.other) : -1);
            CHECK(!"note lifetime scenario");
        }
        if (!sc.quiet) { // what ends it: the host's all-sound-off
            p.cc(120, 0);
            p.blocks(8);
            CHECK(p.voices() == 0);
        }
    }
}

// Random note traffic (stacked notes past the voice limit, retriggers, note-offs with wrong ids,
// chokes, pedals, all-notes-off, a lowered voice budget), then everything released: every note-on
// of a distinct id has had exactly one NOTE_END, and nothing sounds
void noteBalance(const std::string& sound, bool budget, uint32_t seed)
{
    Probe p(sound, 128);
    uint32_t rng = seed;
    auto next = [&](uint32_t n) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) % n; };
    std::vector<int32_t> ids;
    std::vector<int16_t> keys, channels;
    int32_t id = 100;
    uint32_t peak = 0;
    for (int b = 0; b < 6000; ++b) {
        if (budget && b == 2000)
            setLimits(p.plugin, 48);
        const uint32_t events = next(4);
        for (uint32_t e = 0; e < events; ++e) {
            const uint32_t what = next(100);
            int16_t key = static_cast<int16_t>(40 + next(50)), channel = static_cast<int16_t>(next(3));
            const uint32_t time = next(128);
            if (b < 1500 && what >= 70)
                continue; // the first part only piles notes up
            if (b < 1500 ? what < 70 : what < 20) { // more notes than the voices at first
                // one in ten repeats a live note (its id, key and channel): a retrigger; a used id
                // is not reused otherwise (the host's reuse after NOTE_END would count twice)
                int32_t use = id;
                if (next(10) == 0 && !ids.empty()) {
                    const size_t at = next(static_cast<uint32_t>(ids.size()));
                    if (p.events.ends(ids[at]) == 0)
                        use = ids[at], key = keys[at], channel = channels[at];
                }
                if (use == id) {
                    ++id;
                    ids.push_back(use), keys.push_back(key), channels.push_back(channel);
                }
                p.on(use, key, channel, 0.2 + next(80) / 100.0, time);
            } else if (what < 75) {
                const uint32_t how = next(4);
                const int32_t use = ids.empty() ? -1 : ids[next(static_cast<uint32_t>(ids.size()))];
                p.off(how == 0 ? -1 : how == 1 ? use + 7 : use, key, channel, time); // a wrong key or id is the sender's
            } else if (what < 80)
                p.choke(ids.empty() ? -1 : ids[next(static_cast<uint32_t>(ids.size()))], -1, -1);
            else if (what < 83)
                p.cc(64, next(2) ? 127 : 0, static_cast<int>(next(3)));
            else if (what < 84)
                p.cc(123, 0, static_cast<int>(next(3)));
        }
        std::stable_sort(p.events.in.begin(), p.events.in.end(), [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
            return reinterpret_cast<const clap_event_header_t*>(a.data())->time < reinterpret_cast<const clap_event_header_t*>(b.data())->time;
        }); // the host's events come in time order
        p.blocks(1);
        peak = std::max(peak, p.voices());
        CHECK(p.voices() <= 512);
    }
    CHECK(peak >= 200); // the voice limit and the note table were reached
    for (int16_t c = 0; c < 3; ++c) {
        p.cc(64, 0, c);
        for (int16_t k = 40; k < 90; ++k)
            p.off(-1, k, c);
    }
    p.cc(123, 0); // what a stuck pedal or a lost note-off needs
    p.seconds(6);
    CHECK(p.voices() == 0);
    int wrong = 0;
    for (int32_t i : ids)
        wrong += p.events.ends(i) != 1;
    if (wrong) {
        std::printf("note balance: %d of %zu note ids without exactly one NOTE_END\n", wrong, ids.size());
        int shown = 0;
        for (int32_t i : ids)
            if (p.events.ends(i) != 1 && shown++ < 5)
                std::printf("  id %d: %d\n", i, p.events.ends(i));
    }
    CHECK(wrong == 0);
}

// A full note table: the 257th note is tracked (the oldest held note, most likely one whose
// note-off was lost, ends with its NOTE_END), so its own note-off reaches its voices
void fullNoteTable()
{
    Probe p(endless);
    for (int i = 0; i < 256; ++i)
        p.on(i, 40, static_cast<int16_t>(i % 16));
    p.blocks(2);
    p.on(1000, 60);
    p.blocks(2);
    CHECK(p.events.ends(0) == 1);
    CHECK(p.events.ends(1000) == 0);
    p.off(1000, 60);
    p.seconds(0.3);
    CHECK(p.events.ends(1000) == 1);
    CHECK(p.events.ends(0) == 1);
}

// What the host reads: the voice budget's active_voices is every voice not free in the sounds the
// plugin renders (releasing ones too); a sound context's own active_voices is its share of that
void voiceCounts()
{
    Probe p(elements);
    auto* ctx = static_cast<const oscairn_plugin_sound_contexts_t*>(p.plugin->get_extension(p.plugin, OSCAIRN_EXT_SOUND_CONTEXTS));
    oscairn_sound_context_info_t info {};
    for (int i = 0; i < 5; ++i)
        p.on(i, static_cast<int16_t>(60 + i));
    p.seconds(0.2);
    CHECK(p.voices() == 10); // two sustained elements a note
    CHECK(ctx->get(p.plugin, 0, &info) && info.active_voices == 10 && !info.idle);
    p.off(0, 60);
    p.off(1, 61);
    p.blocks(1);
    CHECK(p.voices() == 10 + 2); // releasing voices count, and a key-off element starts with each note-off
    CHECK(ctx->get(p.plugin, 0, &info) && info.active_voices == p.voices());
    p.seconds(3);
    CHECK(p.voices() == 6 && ctx->get(p.plugin, 0, &info) && info.active_voices == 6);
    p.cc(123, 0);
    p.seconds(0.1);
    CHECK(p.voices() == 0 && ctx->get(p.plugin, 0, &info) && info.active_voices == 0);
}

// The voices and slots a pile of notes without note-offs can take, and what the host's limit does
void stackedNotes(const std::string& sound, int notes, bool oneBlock)
{
    Probe p(sound);
    uint32_t peak = 0;
    for (int i = 0; i < notes; ++i) {
        p.on(1000 + i, static_cast<int16_t>(36 + i % 60), static_cast<int16_t>(i % 16));
        if (!oneBlock || i == notes - 1) {
            p.blocks(1);
            peak = std::max(peak, p.voices());
        }
    }
    p.seconds(0.05); // a stolen voice fades over its off time (6 ms) and gives its slot back
    CHECK(peak <= 512);
    CHECK(p.voices() <= 256);
    setLimits(p.plugin, 32);
    p.seconds(0.05);
    CHECK(p.voices() <= 32);
    for (int i = 0; i < notes; ++i)
        p.off(1000 + i, static_cast<int16_t>(36 + i % 60), static_cast<int16_t>(i % 16));
    p.seconds(5);
    CHECK(p.voices() == 0);
}

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
            auto* desc = factory ? factory->get_plugin_descriptor(factory, 0) : nullptr;
            CHECK(desc && !std::strcmp(desc->id, "org.oscairn.sampler") && !std::strcmp(desc->name, "Oscairn Sampler"));
            entry->deinit();
        }
    }
    noteLifetimes();
    for (const std::string* sound : { &endless, &loopSustain, &elements })
        for (bool budget : { false, true })
            noteBalance(*sound, budget, 12345 + (budget ? 1 : 0));
    fullNoteTable();
    voiceCounts();
    stackedNotes(loopContinuous, 600, false);
    stackedNotes(loopContinuous, 600, true);
    stackedNotes(elements, 400, true);
    { // interface, ports
        auto* plugin = create();
        CHECK(plugin->init && plugin->destroy && plugin->activate && plugin->deactivate && plugin->start_processing
              && plugin->stop_processing && plugin->reset && plugin->process && plugin->get_extension && plugin->on_main_thread);
        auto* audio = static_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
        auto* notes = static_cast<const clap_plugin_note_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS));
        CHECK(audio && audio->count(plugin, true) == 0 && audio->count(plugin, false) == 9); // Main and eight sound buses
        clap_audio_port_info_t a {};
        CHECK(audio->get(plugin, 0, false, &a) && a.channel_count == 2 && a.id == 0 && (a.flags & CLAP_AUDIO_PORT_IS_MAIN));
        CHECK(audio->get(plugin, 8, false, &a) && a.channel_count == 2 && a.id == 8 && !(a.flags & CLAP_AUDIO_PORT_IS_MAIN)
              && !std::strcmp(a.name, "Sound bus 8"));
        CHECK(!audio->get(plugin, 9, false, &a));
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
        // the states the plugin wrote as Oscairn sfizz are not read
        std::string old = stateFor(sine);
        CHECK(!load(plugin, old.replace(0, std::strlen("oscairn-sampler-4"), "oscairn-sfizz-3")));
        CHECK(!load(plugin, "oscairn-sfizz-2\n0\n\n0\n\n0\n\n" + std::to_string(sine.size()) + "\n" + sine + "\n"));
        CHECK(!load(plugin, stateFor(sine, "", "", "", "9"))); // no bus 9
        CHECK(!load(plugin, stateFor(sine, "", "", "", "")));
        CHECK(!load(plugin, "oscairn-sampler-4\n0\n\n999\nshort\n"));
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
        // draft 4, which replaced draft 3 outright
        CHECK(plugin->get_extension(plugin, "oscairn.sound-snapshots/draft-4") == snaps);
        CHECK(!plugin->get_extension(plugin, "oscairn.sound-snapshots/draft-3"));
        const std::string kick = "<region> sample=kick.wav loop_mode=one_shot";
        CHECK(load(plugin, stateFor(kick, "", "", OSCAIRN_TEST_FILES)));
        const size_t opened = oscairnSamplerFileOpens(plugin);
        CHECK(opened > 0);
        Buffer saved;
        CHECK(ctx->state->save(plugin, &saved.out));
        CHECK(saved.data.compare(0, 27, "oscairn-sampler-contexts-3\n") == 0);
        Buffer in; in.data = saved.data;
        uint32_t selected[16] {};
        char message[256] {};
        CHECK(snaps->prepare(plugin, &in.in, selected, message, sizeof(message)));
        CHECK(oscairnSamplerFileOpens(plugin) == opened); // nothing read from disk
        Buffer old; old.data = "oscairn-sfizz-contexts-2" + saved.data.substr(26); // as Oscairn sfizz wrote it
        uint32_t unused[16] {};
        CHECK(!snaps->prepare(plugin, &old.in, unused, message, sizeof(message)));
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
        Buffer bad; bad.data = "oscairn-sampler-contexts-3\nnonsense";
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
    { // offline (clap.render): each block waits for the samples its notes stream, so a note in a
      // block longer than sfizz's preload (8192 frames) plays past it
        auto* plugin = create();
        auto* r = static_cast<const clap_plugin_render_t*>(plugin->get_extension(plugin, CLAP_EXT_RENDER));
        CHECK(r && !r->has_hard_realtime_requirement(plugin) && !r->set(plugin, 2) && r->set(plugin, CLAP_RENDER_OFFLINE));
        CHECK(load(plugin, stateFor("<region> sample=stereo_sample.wav", "", "", OSCAIRN_TEST_FILES)));
        CHECK(plugin->activate(plugin, 48000, 1, 32768));
        Events in;
        in.note(CLAP_EVENT_NOTE_ON, 0, 1, 60, 1.0);
        offlineRenders = true;
        const auto out = play(plugin, in, 32768, nullptr, 32768);
        offlineRenders = false;
        CHECK(rms(out, 0, 8192) > 1e-3 && rms(out, 9000, 32768) > 1e-4);
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
        CHECK(saved.data.compare(0, 27, "oscairn-sampler-contexts-3\n") == 0);
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
    { // reset clears the controllers too: a sustain pedal held before it does not hold the next note
        auto* plugin = create();
        CHECK(load(plugin, stateFor("<region> sample=*sine ampeg_release=0.005\n")));
        CHECK(plugin->activate(plugin, 48000, 1, 256));
        Run run { plugin };
        Events events;
        clap_event_midi_t pedal {};
        pedal.header = { sizeof(pedal), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
        pedal.data[0] = 0xb0; pedal.data[1] = 64; pedal.data[2] = 127;
        events.add(pedal);
        events.note(CLAP_EVENT_NOTE_ON, 0, 1, 69, 1.0);
        run.block(events, 256);
        events.note(CLAP_EVENT_NOTE_OFF, 0, 1, 69, 0.0);
        for (int i = 0; i < 40; ++i)
            run.block(events, 256);
        CHECK(run.peak > 0.1f && run.status == CLAP_PROCESS_CONTINUE); // the pedal holds it
        guarded = true; // reset runs on the audio thread
        plugin->reset(plugin);
        guarded = false;
        run.block(events, 256);
        CHECK(run.peak == 0.0f);
        events.note(CLAP_EVENT_NOTE_ON, 0, 2, 69, 1.0);
        run.block(events, 256);
        CHECK(run.peak > 0.1f);
        events.note(CLAP_EVENT_NOTE_OFF, 0, 2, 69, 0.0);
        for (int i = 0; i < 40 && run.status != CLAP_PROCESS_SLEEP; ++i)
            run.block(events, 256);
        CHECK(run.status == CLAP_PROCESS_SLEEP); // released as in a fresh instance
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    { // CLAP reset, as Oscairn sends it to an instance it takes over, before giving it a state:
      // it takes effect at once, so a note in the next block sounds at full level; the instance
      // then plays exactly as a fresh one with the state it gets, whatever it played before; every
      // prepared sound with it, an idle one too; and nothing is allocated (reset is guarded)
        auto reset = [](const clap_plugin_t* p) {
            guarded = true;
            p->reset(p);
            guarded = false;
        };
        // What reset clears would show here: the keyswitches (sw_default, a range, the previous
        // note), round robins, lorand and every random generator, the notes held (trigger=first),
        // channel and poly aftertouch, a controller, the bend, the program and the tempo as
        // switches, a release region under the pedal, a random LFO and controller, and the volume
        // controller's smoothing
        const std::string routed = R"(
            <control> set_cc20=10
            <global> ampeg_release=0.03
            <group> sw_lokey=24 sw_hikey=25 sw_default=24
            <region> sw_last=24 lokey=60 hikey=72 sample=*sine
            <region> sw_last=25 lokey=60 hikey=72 sample=*saw
            <group>
            <region> key=48 seq_length=3 seq_position=1 sample=*sine
            <region> key=48 seq_length=3 seq_position=2 sample=*triangle
            <region> key=48 seq_length=3 seq_position=3 sample=*square
            <region> key=50 hirand=0.5 sample=*noise pitch_random=50 amp_random=3 delay_random=0.002
            <region> key=50 lorand=0.5 sample=*gnoise fil_type=lpf_2p cutoff=2000 fil_random=1000
            <region> key=52 lochanaft=64 sample=*saw
            <region> key=53 lopolyaft=64 sample=*square
            <region> key=54 locc20=64 sample=*triangle
            <region> key=55 lobend=4000 sample=*saw
            <region> key=56 loprog=1 hiprog=127 sample=*sine
            <region> key=45 lobpm=200 sample=*triangle
            <region> key=46 sw_lolast=23 sw_hilast=24 sample=*saw
            <region> key=58 sw_previous=84 sample=*triangle
            <region> key=58 trigger=first sample=*square
            <region> key=57 sample=*sine
            <region> key=57 trigger=release sample=*saw
            <region> key=59 sample=*saw oscillator_phase=-1 lfo1_wave=12 lfo1_freq=20 lfo1_pitch=100 pitch_oncc135=200
        )";
        // What the outgoing Performance played on channel `ch`: notes held, under the sustain and
        // sostenuto pedals, the keyswitch, round robins, aftertouch, controllers and the bend;
        // ids from 1000 * (ch + 1)
        auto busy = [](Events& e, int16_t ch) {
            const int32_t id = 1000 * (ch + 1);
            const uint8_t cc = static_cast<uint8_t>(0xb0 | ch);
            e.midi(0, cc, 66, 127);
            e.note(CLAP_EVENT_NOTE_ON, 10, id, 25, 0.8, ch); // keyswitch
            e.note(CLAP_EVENT_NOTE_OFF, 20, id, 25, 0.0, ch);
            int k = 1;
            for (int16_t key : { 48, 50, 48, 50, 57, 59, 60, 64, 36, 67, 72, 84 })
                e.note(CLAP_EVENT_NOTE_ON, 100 * static_cast<uint32_t>(k), id + k, key, 0.4 + 0.04 * k, ch), ++k;
            e.midi(1500, cc, 64, 127);
            e.note(CLAP_EVENT_NOTE_OFF, 1600, id + 5, 57, 0.0, ch); // its release waits for the pedal
            e.midi(1700, static_cast<uint8_t>(0xe0 | ch), 0x7f, 0x7f);
            e.midi(1800, static_cast<uint8_t>(0xd0 | ch), 127, 0);
            e.midi(1900, static_cast<uint8_t>(0xa0 | ch), 53, 127);
            e.midi(1910, static_cast<uint8_t>(0xa0 | ch), 60, 90);
            for (uint8_t n : { 1, 7, 10, 11, 20, 74 })
                e.midi(2000 + n, cc, n, 127 - n);
        };
        // what the incoming one plays
        auto incoming = [](Events& e, int16_t ch, uint32_t at) {
            const int32_t id = 100 * (ch + 1);
            int k = 0;
            for (int16_t key : { 58, 46, 60, 48, 50, 52, 53, 54, 55, 56, 45, 57, 59, 50, 48, 64 })
                e.note(CLAP_EVENT_NOTE_ON, at + 200 * static_cast<uint32_t>(k), id + k, key, 0.8, ch), ++k;
            e.note(CLAP_EVENT_NOTE_OFF, at + 3000, id + 11, 57, 0.0, ch);
            e.midi(at + 3100, static_cast<uint8_t>(0xb0 | ch), 64, 127);
            e.note(CLAP_EVENT_NOTE_OFF, at + 3200, id + 3, 48, 0.0, ch);
            e.midi(at + 3300, static_cast<uint8_t>(0xb0 | ch), 64, 0); // releases what the pedal holds
            e.note(CLAP_EVENT_NOTE_OFF, at + 20000, -1, -1, 0.0, ch);
        };
        const uint32_t frames = 30000;

        // At once: notes in the first block after a reset sound at full level, and no NOTE_END
        // comes for the notes the reset ended
        {
            auto* a = create();
            auto* b = create();
            CHECK(load(a, stateFor(routed)) && load(b, stateFor(routed)));
            CHECK(a->activate(a, 48000, 1, 256) && b->activate(b, 48000, 1, 256));
            Events before, after, fresh;
            busy(before, 0);
            play(a, before, 25600);
            reset(a);
            incoming(after, 0, 0);
            Events ends;
            const auto x = play(a, after, frames, &ends);
            incoming(fresh, 0, 0);
            const auto y = play(b, fresh, frames);
            CHECK(rms(y, 0, 4800) > 1e-3 && rms(x, 0, 4800) >= 0.99 * rms(y, 0, 4800));
            int stale = 0;
            for (int32_t id = 1000; id < 1100; ++id)
                stale += ends.ends(id);
            CHECK(stale == 0 && ends.ends(100) == 1);
            a->destroy(a);
            b->destroy(b);
        }

        // As a fresh instance: reset and the same state loaded again, or another state after this
        // one played; reset with no load (its state unchanged); and the host's takeover (reset,
        // deactivate, load, activate). The patch above, the stock gate's own patches and sfizz's
        // test files.
        std::vector<std::pair<std::string, std::string>> patches { { "routed", stateFor(routed) } }; // name, state
        const std::string files = OSCAIRN_TEST_FILES;
        std::vector<std::string> paths = sfzFiles(files + "/../../scripts/oscairn/patches");
        for (const std::string& path : sfzFiles(files))
            if (path.find("newdrums_flat") == std::string::npos) // not reproducible in stock either (compare-stock.sh)
                paths.push_back(path);
        for (const std::string& path : paths)
            patches.push_back({ path, stateFor(readFile(path), "", "", path.substr(0, path.rfind('/'))) });
        int compared = 0, failed = 0;
        // Rendered offline: in real time, a patch whose samples stream renders what the loader has
        // read so far (section 3 of the plan), and two fresh instances differ on a loaded machine
        auto offline = [](const clap_plugin_t* p) {
            auto* r = static_cast<const clap_plugin_render_t*>(p->get_extension(p, CLAP_EXT_RENDER));
            CHECK(r && !r->has_hard_realtime_requirement(p) && r->set(p, CLAP_RENDER_OFFLINE));
            return p;
        };
        offlineRenders = true; // sfizz queues a block's loads itself when freewheeling
        for (size_t i = 0; i < patches.size(); ++i) {
            const std::string& state = patches[i].second;
            const std::string& other = patches[(i * 7 + 3) % patches.size()].second;
            Events in;
            incoming(in, 0, 0);
            auto renderFresh = [&](std::vector<float>& out) {
                auto* f = offline(create());
                const bool ok = load(f, state) && f->activate(f, 48000, 1, 256);
                if (ok)
                    out = play(f, in, frames);
                f->destroy(f);
                return ok;
            };
            std::vector<float> fresh;
            if (!renderFresh(fresh))
                continue;
            for (int way = 0; way < 4; ++way) {
                auto* a = offline(create());
                CHECK(load(a, way == 1 ? other : state) || load(a, state));
                CHECK(a->activate(a, 48000, 1, 256));
                Events before;
                busy(before, 0);
                play(a, before, 25600);
                const std::string saved = save(a);
                reset(a);
                if (way == 3)
                    a->deactivate(a);
                if (way != 2)
                    CHECK(load(a, state));
                if (way == 3)
                    CHECK(a->activate(a, 48000, 1, 256));
                if (way == 2)
                    CHECK(save(a) == saved);
                const bool same = play(a, in, frames) == fresh;
                a->on_main_thread(a);
                a->destroy(a);
                if (!same)
                    std::printf("after a reset (way %d) it renders differently from a fresh instance: %s\n", way, patches[i].first.c_str());
                failed += !same;
            }
            ++compared;
        }
        offlineRenders = false;
        std::printf("reset: %d patches compared, %d renders differ from a fresh instance's\n", compared, failed);
        CHECK(compared > 30 && failed == 0);

        // Every sound: three prepared sounds on channels 2 to 4. The one on channel 2 is taken off
        // it while it still sounds (cleared at once); the one on channel 4 is taken off and rings
        // out, idle at the reset (cleared when next selected). After the reset and the selections
        // the instance plays as one that never played.
        {
            const std::string random = "<region> sample=*noise hirand=0.5 ampeg_release=0.005\n<region> sample=*gnoise lorand=0.5 ampeg_release=0.005\n";
            auto prepare = [&](const clap_plugin_t* p, uint32_t* ids) {
                auto* ctx = static_cast<const oscairn_plugin_sound_contexts_t*>(p->get_extension(p, OSCAIRN_EXT_SOUND_CONTEXTS));
                char message[256] {};
                ids[0] = ctx->prepare(p, "Routed", nullptr, 0, routed.c_str(), nullptr, message, sizeof(message));
                ids[1] = ctx->prepare(p, "Saw", nullptr, 0, "<region> sample=*saw ampeg_release=0.02 lfo1_wave=12 lfo1_freq=9 lfo1_pitch=50", nullptr, message, sizeof(message));
                ids[2] = ctx->prepare(p, "Random", nullptr, 0, random.c_str(), nullptr, message, sizeof(message));
                CHECK(ids[0] != CLAP_INVALID_ID && ids[1] != CLAP_INVALID_ID && ids[2] != CLAP_INVALID_ID);
                CHECK(ctx->select(p, ids[0], 1) && ctx->select(p, ids[1], 2) && ctx->select(p, ids[2], 3));
                return ctx;
            };
            auto* a = create();
            auto* b = create();
            CHECK(load(a, stateFor(routed)) && load(b, stateFor(routed)));
            uint32_t ia[3], ib[3];
            auto* ca = prepare(a, ia);
            prepare(b, ib);
            CHECK(a->activate(a, 48000, 1, 256) && b->activate(b, 48000, 1, 256));
            Events before;
            for (int16_t ch : { 0, 1, 2 })
                busy(before, ch);
            for (int k = 0; k < 6; ++k) // the random sound draws, then is released
                before.note(CLAP_EVENT_NOTE_ON, 100 * static_cast<uint32_t>(k), 4000 + k, 60 + k, 0.8, 3);
            before.note(CLAP_EVENT_NOTE_OFF, 1000, -1, -1, 0.0, 3);
            play(a, before, 12800);
            CHECK(ca->select(a, 0, 1) && ca->select(a, 0, 3));
            play(a, {}, 12800);
            oscairn_sound_context_info_t info {};
            CHECK(ca->get(a, 1, &info) && info.active_voices && ca->get(a, 3, &info) && info.idle); // ringing, and idle
            reset(a);
            for (uint32_t slot = 0; slot < 4; ++slot) // nothing sounds, at once
                CHECK(ca->get(a, slot, &info) && !info.active_voices);
            CHECK(ca->select(a, ia[0], 1) && ca->select(a, ia[2], 3));
            Events in;
            for (int16_t ch : { 0, 1, 2, 3 })
                incoming(in, ch, 0);
            CHECK(play(a, in, frames) == play(b, in, frames));
            a->destroy(a);
            b->destroy(b);
        }
    }
    { // context calls on the main thread beside process(): prepare, select, choke_retiring and
      // discard while the audio thread plays another context (ThreadSanitizer checks the sharing)
        auto* plugin = create();
        auto* ctx = static_cast<const oscairn_plugin_sound_contexts_t*>(plugin->get_extension(plugin, OSCAIRN_EXT_SOUND_CONTEXTS));
        auto* snaps = static_cast<const oscairn_plugin_sound_snapshots_t*>(plugin->get_extension(plugin, OSCAIRN_EXT_SOUND_SNAPSHOTS));
        CHECK(load(plugin, stateFor(sine)));
        CHECK(plugin->activate(plugin, 48000, 1, 256));
        std::atomic<bool> done { false };
        std::thread audio([&] {
            Run run { plugin };
            Events events;
            setLimits(plugin, 1); // the voice budget reads every context too
            for (int n = 0; !done.load(); ++n) {
                if (n % 8 == 0)
                    events.note(CLAP_EVENT_NOTE_ON, 0, n, 60, 1.0);
                if (n % 8 == 4)
                    events.note(CLAP_EVENT_NOTE_OFF, 0, n - 4, 60, 0.0);
                run.block(events, 256);
            }
        });
        for (int i = 0; i < 100; ++i) {
            snaps->choke_retiring(plugin); // may be refused while the queue is full
            const uint32_t id = ctx->prepare(plugin, "Beside", nullptr, 0, "<region> sample=*silence", nullptr, nullptr, 0);
            CHECK(id != CLAP_INVALID_ID);
            CHECK(ctx->select(plugin, id, 5));
            CHECK(ctx->select(plugin, 0, 5));
            while (!ctx->discard(plugin, id)) // once the audio thread has applied both selections
                std::this_thread::yield();
        }
        done = true;
        audio.join();
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    { // voice budget: the extension, every entry
        auto* plugin = create();
        auto* vb = budgetOf(plugin);
        CHECK(vb && vb->honors_volume && vb->set_limits && vb->active_voices && vb->notes);
        CHECK(vb && vb->honors_volume(plugin));
        plugin->destroy(plugin);
    }
    { // voice budget: the per-voice cost estimate: 0 before any voice, positive and plausible while
      // voices sound, kept (not 0, not a jump) once they stop
        auto* plugin = create();
        CHECK(load(plugin, stateFor(sine)));
        CHECK(plugin->activate(plugin, 48000, 1, 256));
        Run run { plugin };
        Events events;
        for (int b = 0; b < 4; ++b)
            run.block(events, 256);
        CHECK(activeVoices(plugin) == 0 && lastCost == 0);
        events.note(CLAP_EVENT_NOTE_ON, 0, 1, 60, 1.0);
        events.note(CLAP_EVENT_NOTE_ON, 0, 2, 64, 1.0);
        for (int b = 0; b < 40; ++b)
            run.block(events, 256);
        CHECK(activeVoices(plugin) == 2);
        const double sounding = lastCost;
        CHECK(sounding > 0.01 && sounding < 100000); // microseconds a block for one voice
        events.note(CLAP_EVENT_NOTE_OFF, 0, 1, 60, 0.0);
        events.note(CLAP_EVENT_NOTE_OFF, 0, 2, 64, 0.0);
        for (int b = 0; b < 2000 && run.status != CLAP_PROCESS_SLEEP; ++b)
            run.block(events, 256);
        for (int b = 0; b < 8; ++b)
            run.block(events, 256);
        CHECK(activeVoices(plugin) == 0);
        CHECK(lastCost > 0.01 && lastCost < 100000 && lastCost < 20 * sounding && lastCost > sounding / 20);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    { // the sample loader thread is an ordinary thread by default; OSCAIRN_SAMPLER_LOADER_PRIORITY
      // asks for a real-time one (which only a process allowed to can have)
        const auto loaderPolicy = [](const char* setting, int wanted, int tries) {
            if (setting) setenv("OSCAIRN_SAMPLER_LOADER_PRIORITY", setting, 1); else unsetenv("OSCAIRN_SAMPLER_LOADER_PRIORITY");
            auto* plugin = create();
            CHECK(load(plugin, stateFor(sine)));
            int policy = -1, priority = -1;
            for (int i = 0; i < tries; ++i) { // the loader thread starts with the synth
                sfz::FilePool::lastLoaderScheduling(policy, priority);
                if (policy == wanted)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            plugin->destroy(plugin);
            unsetenv("OSCAIRN_SAMPLER_LOADER_PRIORITY");
            return policy;
        };
        CHECK(loaderPolicy(nullptr, SCHED_OTHER, 100) == SCHED_OTHER);
        CHECK(loaderPolicy("normal", SCHED_OTHER, 100) == SCHED_OTHER);
        CHECK(loaderPolicy("rr:", SCHED_OTHER, 100) == SCHED_OTHER); // not a policy: normal
        { // whether this process may have a real-time thread at all
            bool allowed = false;
            std::thread([&] {
                sched_param param {};
                param.sched_priority = sched_get_priority_min(SCHED_RR);
                allowed = pthread_setschedparam(pthread_self(), SCHED_RR, &param) == 0;
            }).join();
            if (allowed) {
                CHECK(loaderPolicy("rr:1", SCHED_RR, 200) == SCHED_RR);
                CHECK(loaderPolicy("fifo:1", SCHED_FIFO, 200) == SCHED_FIFO);
                CHECK(loaderPolicy("stock", SCHED_RR, 200) == SCHED_RR);
                CHECK(loaderPolicy(nullptr, SCHED_OTHER, 200) == SCHED_OTHER); // and back
            } else
                std::printf("loader priority: real-time policies not permitted here, rr/fifo/stock unchecked\n");
        }
#ifdef __linux__
        { // no thread of the process but those of the host is real-time by default
            auto* plugin = create();
            CHECK(load(plugin, stateFor(sine)));
            CHECK(plugin->activate(plugin, 48000, 1, 256));
            Run run { plugin };
            Events events;
            events.note(CLAP_EVENT_NOTE_ON, 0, 1, 60, 1.0);
            for (int b = 0; b < 20; ++b)
                run.block(events, 256);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            int threads = 0;
            if (DIR* d = opendir("/proc/self/task")) {
                while (const dirent* e = readdir(d)) {
                    const int tid = std::atoi(e->d_name);
                    if (tid <= 0)
                        continue;
                    ++threads;
                    const int policy = sched_getscheduler(tid);
                    CHECK(policy == SCHED_OTHER || policy == SCHED_BATCH || policy == SCHED_IDLE);
                }
                closedir(d);
            }
            CHECK(threads >= 2);
            plugin->deactivate(plugin);
            plugin->destroy(plugin);
        }
#endif
    }
    { // voice budget, the gate of Oscairn RFC 0018 step 4: a layered sound under pressure gives up
      // quiet elements before whole notes; a note ends only once its last voice has
        auto* plugin = create();
        CHECK(load(plugin, stateFor(layered)));
        CHECK(plugin->activate(plugin, 48000, 1, 64));
        Run run { plugin };
        Events events;
        setLimits(plugin, 16, { selectedEntry(0, 0, 5) }); // what Oscairn sends a part's instance
        for (int n = 0; n < 8; ++n) {
            events.note(CLAP_EVENT_NOTE_ON, 0, n, static_cast<int16_t>(60 + n), 1.0);
            for (int b = 0; b < 8; ++b) // past each note's attack
                run.block(events, 64);
        }
        for (int b = 0; b < 4; ++b) // past the last fades
            run.block(events, 64);
        CHECK(activeVoices(plugin) == 16);
        BudgetNotes notes(plugin);
        CHECK(notes.count == 8);
        for (int n = 0; n < 8; ++n) { // every note keeps its two loudest elements, and its level
            CHECK(notes.voices(n) == 2);
            CHECK(std::fabs(notes.loudness(n) - notes.loudness(0)) < 1e-3f && notes.loudness(n) > 0.5f);
            CHECK(events.ends(n) == 0);
        }
        // the limit lowered to 8: each note's second element goes, every note still sounds
        setLimits(plugin, 8, { selectedEntry(0, 0, 5) });
        for (int b = 0; b < 4; ++b)
            run.block(events, 64);
        CHECK(activeVoices(plugin) == 8);
        BudgetNotes fewer(plugin);
        CHECK(fewer.count == 8);
        for (int n = 0; n < 8; ++n)
            CHECK(fewer.voices(n) == 1 && events.ends(n) == 0);
        for (int n = 0; n < 8; ++n)
            events.note(CLAP_EVENT_NOTE_OFF, 0, n, static_cast<int16_t>(60 + n), 0.0);
        for (int b = 0; b < 2000 && run.status != CLAP_PROCESS_SLEEP; ++b)
            run.block(events, 64);
        for (int n = 0; n < 8; ++n)
            CHECK(events.ends(n) == 1);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    { // voice budget: a lowered limit fades the excess out from the next block, over 3 ms, without
      // a click; fading voices are counted and listed until they end
        const std::string flat = "<region> sample=*sine amp_veltrack=0 ampeg_release=0.01\n";
        auto render = [&](bool all, bool limit, std::vector<float>& out, std::vector<uint32_t>* counts) {
            auto* plugin = create();
            CHECK(load(plugin, stateFor(flat)));
            CHECK(plugin->activate(plugin, 48000, 1, 64));
            Run run { plugin };
            Events events;
            for (int n = 0; n < 6; ++n) { // a note a block
                if (all || n >= 2)
                    events.note(CLAP_EVENT_NOTE_ON, 0, n, static_cast<int16_t>(60 + 3 * n), 1.0);
                run.block(events, 64);
                out.insert(out.end(), run.left.begin(), run.left.end());
            }
            if (limit)
                setLimits(plugin, 4);
            for (int b = 0; b < 6; ++b) {
                run.block(events, 64);
                out.insert(out.end(), run.left.begin(), run.left.end());
                if (counts) {
                    BudgetNotes notes(plugin);
                    counts->push_back(activeVoices(plugin));
                    counts->push_back(notes.count);
                    counts->push_back(static_cast<uint32_t>(events.ends(0) + events.ends(1)));
                    counts->push_back(static_cast<uint32_t>(events.ends(2) + events.ends(3) + events.ends(4) + events.ends(5)));
                    if (b == 0)
                        CHECK(notes.loudness(0) < notes.loudness(5) && notes.loudness(0) > 0.0f);
                }
            }
            plugin->deactivate(plugin);
            plugin->destroy(plugin);
        };
        std::vector<float> limited, kept, unlimited;
        std::vector<uint32_t> counts;
        render(true, true, limited, &counts);
        render(false, false, kept, nullptr);
        render(true, false, unlimited, nullptr);
        const size_t at = 6 * 64; // the limit's block
        // blocks 0 to 2 fade (144 frames): six voices counted and listed, no NOTE_END before the
        // fade's block ends; then four, the two oldest ended once, the others not
        const std::vector<uint32_t> expected { 6, 6, 0, 0, 6, 6, 0, 0, 4, 4, 2, 0, 4, 4, 2, 0, 4, 4, 2, 0, 4, 4, 2, 0 };
        CHECK(counts == expected);
        float diff = 0, level = 0, step = 0, ownStep = 0;
        for (size_t i = at + 145; i < limited.size(); ++i) // gone after the fade
            diff = std::max(diff, std::fabs(limited[i] - kept[i]));
        CHECK(diff < 1e-6f);
        for (size_t i = at; i < at + 145; ++i) {
            level = std::max(level, std::fabs(unlimited[i]));
            step = std::max(step, std::fabs(limited[i] - limited[i - 1]));
            ownStep = std::max(ownStep, std::fabs(unlimited[i] - unlimited[i - 1]));
        }
        CHECK(step <= ownStep + level / 64); // no step beyond the notes' own
        CHECK(std::fabs(limited[at + 10] - kept[at + 10]) > 1e-3f); // still fading, not cut
        // until the limit, the render is the unlimited one
        CHECK(limited.size() == unlimited.size() && std::equal(unlimited.begin(), unlimited.begin() + at, limited.begin()));
    }
    { // voice budget: the order: released before held, the quietest, the new note's own sound
        auto* plugin = create();
        auto* ctx = static_cast<const oscairn_plugin_sound_contexts_t*>(plugin->get_extension(plugin, OSCAIRN_EXT_SOUND_CONTEXTS));
        CHECK(load(plugin, stateFor("<region> sample=*sine ampeg_release=2\n")));
        CHECK(plugin->activate(plugin, 48000, 1, 64));
        Run run { plugin };
        Events events;
        auto blocks = [&](int n) { for (int b = 0; b < n; ++b) run.block(events, 64); };
        // a quiet held note, then a loud newer one released (its long release still sounding)
        events.note(CLAP_EVENT_NOTE_ON, 0, 1, 60, 0.3);
        blocks(2);
        events.note(CLAP_EVENT_NOTE_ON, 0, 2, 64, 1.0);
        blocks(2);
        events.note(CLAP_EVENT_NOTE_OFF, 0, 2, 64, 0.0);
        blocks(2);
        setLimits(plugin, 1, { selectedEntry(0, 0, 5) });
        blocks(4);
        CHECK(events.ends(2) == 1 && events.ends(1) == 0);
        CHECK(activeVoices(plugin) == 1);
        // held notes: the quietest goes, not the oldest
        setLimits(plugin, 0);
        events.note(CLAP_EVENT_NOTE_CHOKE, 0, 1, 60, 0.0);
        events.note(CLAP_EVENT_NOTE_ON, 0, 3, 67, 1.0);
        blocks(2);
        events.note(CLAP_EVENT_NOTE_ON, 0, 4, 71, 0.3);
        blocks(2);
        events.note(CLAP_EVENT_NOTE_ON, 0, 5, 74, 1.0);
        blocks(2);
        CHECK(events.ends(1) == 1 && activeVoices(plugin) == 3); // no limit: all three
        setLimits(plugin, 2, { selectedEntry(0, 0, 5) });
        blocks(4);
        CHECK(events.ends(4) == 1 && events.ends(3) == 0 && events.ends(5) == 0);
        // a new note gives up a voice of its own sound before another sound's quieter, older one
        setLimits(plugin, 0);
        char message[256] {};
        const uint32_t other = ctx->prepare(plugin, "Other", nullptr, 0, "<region> sample=*sine ampeg_release=2", nullptr, message, sizeof(message));
        CHECK(other != CLAP_INVALID_ID && ctx->select(plugin, other, 1));
        events.note(CLAP_EVENT_NOTE_ON, 0, 6, 60, 0.3, 1); // the other sound
        blocks(2);
        setLimits(plugin, 3, { selectedEntry(0, 0, 5) });
        events.note(CLAP_EVENT_NOTE_ON, 0, 7, 76, 1.0, 0); // the default sound
        blocks(4);
        CHECK(events.ends(3) == 1 && events.ends(5) == 0 && events.ends(6) == 0 && events.ends(7) == 0);
        CHECK(activeVoices(plugin) == 3);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    { // parts sharing one instance (Oscairn RFC 0021): the Output bus parameter and its hint
        auto* plugin = create();
        auto* params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
        auto* ui = static_cast<const oscairn_plugin_ui_t*>(plugin->get_extension(plugin, OSCAIRN_EXT_UI));
        CHECK(params && params->count(plugin) == 3);
        clap_param_info_t info {};
        CHECK(params->get_info(plugin, 0, &info) && info.id == 1 && (info.flags & CLAP_PARAM_IS_STEPPED)
              && !(info.flags & CLAP_PARAM_IS_READONLY) && info.min_value == 0 && info.max_value == 8 && info.default_value == 0);
        CHECK(!params->get_info(plugin, 3, &info));
        oscairn_param_hint_t hint {};
        CHECK(ui && ui->param_hint(plugin, 1, &hint) && hint.role == OSCAIRN_ROLE_OUTPUT_BUS && !ui->param_hint(plugin, 2, &hint));
        // oscairn.ui draft 5, not draft 4 or 3 (their smaller tables); no parameter is in a rich view, and no times
        CHECK(plugin->get_extension(plugin, "oscairn.ui/draft-5") && !plugin->get_extension(plugin, "oscairn.ui/draft-4")
              && !plugin->get_extension(plugin, "oscairn.ui/draft-3"));
        CHECK(ui && !ui->algorithm && !ui->seconds && ui->param_hint(plugin, 1, &hint) && hint.view == 0 && hint.field == OSCAIRN_FIELD_NONE && hint.index == 0);
        CHECK(ui && !ui->param_hint(plugin, 3, &hint));
        char text[64] {};
        double value = -1;
        CHECK(params->value_to_text(plugin, 1, 0, text, sizeof(text)) && !std::strcmp(text, "Main"));
        CHECK(params->value_to_text(plugin, 1, 3, text, sizeof(text)) && !std::strcmp(text, "Sound bus 3"));
        CHECK(params->text_to_value(plugin, 1, "Sound bus 3", &value) && value == 3);
        CHECK(params->text_to_value(plugin, 1, "Main", &value) && value == 0);
        CHECK(!params->text_to_value(plugin, 1, "Sound bus 9", &value));
        // set while inactive through flush, saved in the state, shown as the value
        Events events;
        events.bus(0, -1, 5);
        params->flush(plugin, &events.input, &events.output);
        CHECK(params->get_value(plugin, 1, &value) && value == 5);
        CHECK(save(plugin) == stateFor("<region> sample=*sine\n", "", "", "", "5"));
        auto* other = create();
        CHECK(load(other, save(plugin)));
        CHECK(params->get_value(other, 1, &value) && value == 5);
        CHECK(other->activate(other, 48000, 1, 256));
        BusRun run { other };
        events.in.clear();
        events.note(CLAP_EVENT_NOTE_ON, 0, 1, 69, 1.0);
        run.block(events, 256);
        CHECK(run.peak(5) > 0.1f && run.peak(0) == 0.0f && run.peak(1) == 0.0f && run.peak(8) == 0.0f);
        other->deactivate(other);
        other->destroy(other);
        // a sound as Oscairn shares one: the plugin's own patch, the bus as its parameter
        auto* ctx = static_cast<const oscairn_plugin_sound_contexts_t*>(plugin->get_extension(plugin, OSCAIRN_EXT_SOUND_CONTEXTS));
        char message[256] {};
        const oscairn_sound_parameter_t bus4 { 1, 4 }, bad { 1, 9 }, other2 { 4, 0 };
        CHECK(ctx->prepare(plugin, "Bad", &bad, 1, nullptr, nullptr, message, sizeof(message)) == CLAP_INVALID_ID);
        CHECK(ctx->prepare(plugin, "Bad", &other2, 1, nullptr, nullptr, message, sizeof(message)) == CLAP_INVALID_ID);
        const uint32_t id = ctx->prepare(plugin, "Shared", &bus4, 1, nullptr, nullptr, message, sizeof(message));
        CHECK(id != CLAP_INVALID_ID && ctx->select(plugin, id, 6));
        CHECK(plugin->activate(plugin, 48000, 1, 256));
        BusRun shared { plugin };
        events.note(CLAP_EVENT_NOTE_ON, 0, 2, 69, 1.0, 6);
        shared.block(events, 256);
        CHECK(shared.peak(4) > 0.1f && shared.peak(5) == 0.0f && shared.peak(0) == 0.0f);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    { // two parts on one instance, each its own sound, channel and bus, render sample for sample
      // what two instances render, with a pitch bend on one part's channel only
        const std::string a = "<region> sample=*sine ampeg_attack=0.002 ampeg_release=0.05\n";
        const std::string b = "<region> sample=*saw ampeg_attack=0.01 ampeg_release=0.1 bend_up=700\n";
        auto events = [](int block, Events& e, int16_t only) { // only: the channel kept, -1 for all
            auto note = [&](uint16_t type, uint32_t time, int32_t id, int16_t key, double velocity, int16_t channel) {
                if (only < 0 || only == channel)
                    e.note(type, time, id, key, velocity, channel);
            };
            if (block == 0)
                note(CLAP_EVENT_NOTE_ON, 10, 1, 60, 0.8, 0);
            if (block == 1)
                note(CLAP_EVENT_NOTE_ON, 30, 2, 64, 0.7, 1);
            if (block == 5 && (only < 0 || only == 1))
                e.midi(100, 0xe1, 0, 96); // a bend up on channel 1
            if (block == 10)
                note(CLAP_EVENT_NOTE_ON, 0, 3, 67, 1.0, 0);
            if (block == 15) {
                note(CLAP_EVENT_NOTE_OFF, 5, 1, 60, 0.0, 0);
                note(CLAP_EVENT_NOTE_OFF, 200, 2, 64, 0.0, 1);
            }
            if (block == 20)
                note(CLAP_EVENT_NOTE_OFF, 0, 3, 67, 0.0, 0);
        };
        constexpr int blocks = 40;
        std::vector<float> separate[2];
        for (int part = 0; part < 2; ++part) {
            auto* plugin = create();
            CHECK(load(plugin, stateFor(part ? b : a)));
            CHECK(plugin->activate(plugin, 48000, 1, 256));
            BusRun run { plugin };
            Events e;
            for (int i = 0; i < blocks; ++i) {
                events(i, e, static_cast<int16_t>(part));
                run.block(e, 256);
            }
            separate[part] = run.kept[0];
            plugin->deactivate(plugin);
            plugin->destroy(plugin);
        }
        auto* plugin = create();
        auto* ctx = static_cast<const oscairn_plugin_sound_contexts_t*>(plugin->get_extension(plugin, OSCAIRN_EXT_SOUND_CONTEXTS));
        char message[256] {};
        const oscairn_sound_parameter_t bus2 { 1, 2 };
        const uint32_t ida = ctx->prepare(plugin, "A", nullptr, 0, a.c_str(), nullptr, message, sizeof(message));
        const uint32_t idb = ctx->prepare(plugin, "B", &bus2, 1, b.c_str(), nullptr, message, sizeof(message));
        CHECK(ida != CLAP_INVALID_ID && idb != CLAP_INVALID_ID && ctx->select(plugin, ida, 0) && ctx->select(plugin, idb, 1));
        CHECK(plugin->activate(plugin, 48000, 1, 256));
        BusRun run { plugin };
        Events e;
        e.bus(0, 0, 1); // A's bus, set as Oscairn does: on its channel, with the selection
        for (int i = 0; i < blocks; ++i) {
            events(i, e, -1);
            run.block(e, 256);
        }
        const float da = largestDifference(run.kept[1], separate[0]), db = largestDifference(run.kept[2], separate[1]);
        std::printf("shared instance: largest difference from two instances %g, %g\n", da, db);
        CHECK(da == 0.0f && db == 0.0f);
        CHECK(*std::max_element(separate[0].begin(), separate[0].end()) > 0.1f && *std::max_element(separate[1].begin(), separate[1].end()) > 0.1f);
        for (int bus : { 0, 3, 4, 5, 6, 7, 8 })
            CHECK(run.peak(bus) == 0.0f);
        // the routing is saved with the sounds and restored: a fresh instance plays them on their buses
        Buffer saved;
        CHECK(ctx->state->save(plugin, &saved.out));
        plugin->deactivate(plugin);
        auto* restored = create();
        auto* rctx = static_cast<const oscairn_plugin_sound_contexts_t*>(restored->get_extension(restored, OSCAIRN_EXT_SOUND_CONTEXTS));
        Buffer in; in.data = saved.data;
        CHECK(rctx->state->load(restored, &in.in));
        Buffer again;
        CHECK(rctx->state->save(restored, &again.out) && again.data == saved.data);
        CHECK(restored->activate(restored, 48000, 1, 256));
        {
            BusRun r { restored };
            Events re;
            for (int i = 0; i < blocks; ++i) {
                events(i, re, -1);
                r.block(re, 256);
            }
            CHECK(largestDifference(r.kept[1], separate[0]) == 0.0f && largestDifference(r.kept[2], separate[1]) == 0.0f);
            CHECK(r.peak(0) == 0.0f);
        }
        restored->deactivate(restored);
        restored->destroy(restored);
        // draft 4: a sound's values while no channel selects it, and one sound saved on its own
        auto* own = static_cast<const oscairn_plugin_sound_snapshots_t*>(plugin->get_extension(plugin, OSCAIRN_EXT_SOUND_SNAPSHOTS));
        CHECK(own->save_sound && own->sound_values);
        const oscairn_sound_parameter_t bus3 { 1, 3 };
        const uint32_t idc = ctx->prepare(plugin, "C", &bus3, 1, a.c_str(), nullptr, message, sizeof(message));
        oscairn_sound_parameter_t values[4] {};
        CHECK(own->sound_values(plugin, idc, values, 4) == 3 && values[0].id == 1 && values[0].value == 3);
        CHECK(own->sound_values(plugin, idb, values, 4) == 0); // selected on channel 1
        CHECK(own->sound_values(plugin, 999, values, 4) == 0);
        Buffer alone;
        CHECK(own->save_sound(plugin, idb, &alone.out) && !own->save_sound(plugin, 999, &alone.out));
        auto* single = create();
        auto* sctx = static_cast<const oscairn_plugin_sound_contexts_t*>(single->get_extension(single, OSCAIRN_EXT_SOUND_CONTEXTS));
        CHECK(sctx->state->load(single, &alone.in));
        oscairn_sound_context_info_t sinfo {};
        CHECK(sctx->get(single, 0, &sinfo) && sinfo.selected_channels == 0xffffu && !std::strcmp(sinfo.name, "B"));
        for (uint32_t slot = 1; slot < 8; ++slot)
            CHECK(!sctx->get(single, slot, &sinfo));
        auto* reference = create();
        CHECK(load(reference, stateFor(b)));
        std::vector<float> outs[2];
        for (auto* q : { single, reference }) {
            CHECK(q->activate(q, 48000, 1, 256));
            BusRun r { q };
            Events qe;
            for (int i = 0; i < blocks; ++i) {
                events(i, qe, 1);
                r.block(qe, 256);
            }
            for (int bus = 1; bus < 9; ++bus)
                CHECK(r.peak(bus) == 0.0f); // on Main
            outs[q == reference] = r.kept[0];
            q->deactivate(q);
            q->destroy(q);
        }
        CHECK(largestDifference(outs[0], outs[1]) == 0.0f && *std::max_element(outs[0].begin(), outs[0].end()) > 0.1f);
        plugin->destroy(plugin);
        // and prepared beside, in another instance: the snapshot's sounds keep their buses
        auto* beside = create();
        auto* bctx = static_cast<const oscairn_plugin_sound_contexts_t*>(beside->get_extension(beside, OSCAIRN_EXT_SOUND_CONTEXTS));
        auto* snaps = static_cast<const oscairn_plugin_sound_snapshots_t*>(beside->get_extension(beside, OSCAIRN_EXT_SOUND_SNAPSHOTS));
        Buffer prepared; prepared.data = saved.data;
        uint32_t selected[16] {};
        CHECK(snaps->prepare(beside, &prepared.in, selected, message, sizeof(message)));
        CHECK(bctx->select(beside, selected[0], 0) && bctx->select(beside, selected[1], 1));
        CHECK(beside->activate(beside, 48000, 1, 256));
        {
            BusRun r { beside };
            Events re;
            re.note(CLAP_EVENT_NOTE_ON, 0, 1, 60, 0.8, 0);
            re.note(CLAP_EVENT_NOTE_ON, 0, 2, 64, 0.8, 1);
            r.block(re, 256);
            r.block(re, 256);
            CHECK(r.peak(1) > 0.1f && r.peak(2) > 0.1f && r.peak(0) == 0.0f);
        }
        beside->deactivate(beside);
        beside->destroy(beside);
    }
    { // a change of bus at a block's start: the held note stays on its bus without a click, the
      // next note plays on the new one, and nothing is lost or doubled
        auto render = [](bool change) {
            auto* plugin = create();
            CHECK(load(plugin, stateFor("<region> sample=*sine amp_veltrack=0 ampeg_release=0.2\n")));
            CHECK(plugin->activate(plugin, 48000, 1, 256));
            BusRun run { plugin };
            Events e;
            for (int i = 0; i < 40; ++i) {
                if (i == 0) {
                    e.bus(0, -1, 1);
                    e.note(CLAP_EVENT_NOTE_ON, 20, 1, 69, 1.0);
                }
                if (i == 10 && change)
                    e.bus(100, -1, 2); // takes effect from the block's start
                if (i == 12)
                    e.note(CLAP_EVENT_NOTE_ON, 50, 2, 76, 1.0);
                if (i == 20)
                    e.note(CLAP_EVENT_NOTE_OFF, 0, 1, 69, 0.0);
                if (i == 30)
                    e.note(CLAP_EVENT_NOTE_OFF, 0, 2, 76, 0.0);
                run.block(e, 256);
            }
            plugin->deactivate(plugin);
            plugin->destroy(plugin);
            return run.kept;
        };
        const auto same = render(false), changed = render(true);
        std::vector<float> sum(changed[1].size());
        for (size_t i = 0; i < sum.size(); ++i)
            sum[i] = changed[1][i] + changed[2][i];
        CHECK(largestDifference(sum, same[1]) < 1e-6f); // nothing lost or doubled
        const size_t secondStarts = 12 * 256 + 50;
        CHECK(*std::max_element(changed[2].begin(), changed[2].begin() + secondStarts) == 0.0f); // the held note stayed
        CHECK(*std::max_element(changed[2].begin() + secondStarts, changed[2].end()) > 0.1f);
        CHECK(*std::max_element(changed[1].begin() + secondStarts, changed[1].end()) > 0.1f);
        // no click: no step on either bus larger than the unchanged render's largest
        const float limit = largestStep(same[1]) * 1.001f;
        std::printf("bus change: largest steps %g, %g against %g\n", largestStep(changed[1]), largestStep(changed[2]), limit);
        CHECK(largestStep(changed[1]) <= limit && largestStep(changed[2]) <= limit);
        CHECK(*std::max_element(same[2].begin(), same[2].end()) == 0.0f && *std::max_element(changed[0].begin(), changed[0].end()) == 0.0f);
    }
    { // ring-out carries only Main: a retiring sound on a sound bus keeps playing on that bus
        auto* plugin = create();
        auto* ctx = static_cast<const oscairn_plugin_sound_contexts_t*>(plugin->get_extension(plugin, OSCAIRN_EXT_SOUND_CONTEXTS));
        auto* snaps = static_cast<const oscairn_plugin_sound_snapshots_t*>(plugin->get_extension(plugin, OSCAIRN_EXT_SOUND_SNAPSHOTS));
        CHECK(load(plugin, stateFor(sine, "", "", "", "3")));
        Buffer saved;
        CHECK(ctx->state->save(plugin, &saved.out));
        uint32_t selected[16] {};
        char message[256] {};
        CHECK(snaps->prepare(plugin, &saved.in, selected, message, sizeof(message)));
        CHECK(plugin->activate(plugin, 48000, 1, 256));
        BusRun run { plugin };
        Events events;
        events.note(CLAP_EVENT_NOTE_ON, 0, 1, 69, 1.0);
        run.block(events, 256);
        CHECK(run.peak(3) > 0.1f);
        CHECK(ctx->select(plugin, selected[0], -1));
        std::vector<std::vector<float>> ringData(4, std::vector<float>(256, 0.0f));
        float* ringChannels[4] = { ringData[0].data(), ringData[1].data(), ringData[2].data(), ringData[3].data() };
        clap_audio_buffer_t ring {};
        ring.data32 = ringChannels;
        ring.channel_count = 4;
        snaps->ring_out(plugin, &ring);
        run.block(events, 256);
        CHECK(run.peak(3, 256) > 0.1f && run.peak(0, 256) == 0.0f);
        CHECK(*std::max_element(ringData[0].begin(), ringData[0].end()) == 0.0f);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    { // voice budget on a shared instance: an entry per part (Oscairn RFC 0021), each its sound's
      // context with its part's reserve and priority and no limit of its own
        auto* plugin = create();
        auto* ctx = static_cast<const oscairn_plugin_sound_contexts_t*>(plugin->get_extension(plugin, OSCAIRN_EXT_SOUND_CONTEXTS));
        char message[256] {};
        const oscairn_sound_parameter_t bus1 { 1, 1 }, bus2 { 1, 2 };
        const char* patch = "<region> sample=*sine amp_veltrack=0 ampeg_release=2";
        const uint32_t a = ctx->prepare(plugin, "A", &bus1, 1, patch, nullptr, message, sizeof(message));
        const uint32_t b = ctx->prepare(plugin, "B", &bus2, 1, patch, nullptr, message, sizeof(message));
        CHECK(a != CLAP_INVALID_ID && b != CLAP_INVALID_ID && ctx->select(plugin, a, 0) && ctx->select(plugin, b, 1));
        auto entry = [](uint32_t context, uint32_t reserve, uint8_t priority) {
            oscairn_voice_budget_context_t c {};
            c.context = context;
            c.reserve = reserve;
            c.priority = priority;
            return c;
        };
        CHECK(plugin->activate(plugin, 48000, 1, 64));
        BusRun run { plugin };
        Events events;
        auto blocks = [&](int n) { for (int i = 0; i < n; ++i) run.block(events, 64); };
        // a part's reserve: A's two notes (the oldest) are within its reserve of 2, so B, over
        // its reserve of 1, gives up its older note, although B's priority is the higher
        events.note(CLAP_EVENT_NOTE_ON, 0, 1, 60, 1.0, 0);
        events.note(CLAP_EVENT_NOTE_ON, 0, 2, 62, 1.0, 0);
        blocks(2);
        events.note(CLAP_EVENT_NOTE_ON, 0, 3, 64, 1.0, 1);
        blocks(2);
        events.note(CLAP_EVENT_NOTE_ON, 0, 4, 65, 1.0, 1);
        blocks(2);
        setLimits(plugin, 3, { entry(a, 2, 3), entry(b, 1, 7) });
        blocks(4);
        CHECK(events.ends(3) == 1 && events.ends(1) == 0 && events.ends(2) == 0 && events.ends(4) == 0);
        CHECK(activeVoices(plugin) == 3);
        // a part's priority: with no reserves, the lower-priority part's note goes, not the oldest
        setLimits(plugin, 0);
        for (int id : { 1, 2, 4 })
            events.note(CLAP_EVENT_NOTE_CHOKE, 0, id, -1, 0.0, -1);
        blocks(4);
        CHECK(activeVoices(plugin) == 0);
        events.note(CLAP_EVENT_NOTE_ON, 0, 5, 60, 1.0, 1); // B first: the oldest
        events.note(CLAP_EVENT_NOTE_ON, 0, 6, 62, 1.0, 1);
        blocks(2);
        events.note(CLAP_EVENT_NOTE_ON, 0, 7, 64, 1.0, 0);
        blocks(2);
        events.note(CLAP_EVENT_NOTE_ON, 0, 8, 65, 1.0, 0);
        blocks(2);
        setLimits(plugin, 3, { entry(a, 0, 3), entry(b, 0, 7) });
        blocks(4);
        CHECK(events.ends(7) == 1 && events.ends(5) == 0 && events.ends(6) == 0 && events.ends(8) == 0);
        CHECK(run.peak(1) > 0.1f && run.peak(2) > 0.1f && run.peak(0) == 0.0f);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    { // the assignable switches (A.SW elements) as parameters, each sound's own
        const std::string patch = "<control> oscairn_profile=1 <region> sample=*sine amp_veltrack=0 oscairn_element=1 oscairn_xa=6\n";
        auto* plugin = create();
        auto* params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
        clap_param_info_t info {};
        for (uint32_t k = 1; k <= 2; ++k)
            CHECK(params->get_info(plugin, k, &info) && info.id == 1 + k && (info.flags & CLAP_PARAM_IS_STEPPED)
                  && info.min_value == 0 && info.max_value == 1 && info.default_value == 0
                  && std::string(info.name) == "Assign Switch " + std::to_string(k));
        char text[64] {};
        double value = -1;
        CHECK(params->value_to_text(plugin, 2, 1, text, sizeof(text)) && !std::strcmp(text, "On"));
        CHECK(params->value_to_text(plugin, 3, 0, text, sizeof(text)) && !std::strcmp(text, "Off"));
        CHECK(params->text_to_value(plugin, 3, "On", &value) && value == 1);
        CHECK(load(plugin, stateFor(patch)));
        CHECK(plugin->activate(plugin, 48000, 1, 256));
        Run run { plugin };
        Events events;
        events.note(CLAP_EVENT_NOTE_ON, 0, 1, 69, 1.0);
        run.block(events, 256);
        CHECK(run.peak == 0.0f); // switch 1 off: the element does not play
        events.note(CLAP_EVENT_NOTE_OFF, 0, 1, 69, 0.0);
        events.assignSwitch(100, 1, -1, 1.0);
        events.note(CLAP_EVENT_NOTE_ON, 100, 2, 69, 1.0); // at the switch's frame
        run.block(events, 256);
        CHECK(run.peak > 0.1f && run.left[99] == 0.0f);
        CHECK(params->get_value(plugin, 2, &value) && value == 1 && params->get_value(plugin, 3, &value) && value == 0);
        events.note(CLAP_EVENT_NOTE_OFF, 0, 2, 69, 0.0);
        events.assignSwitch(0, 1, -1, 0.0);
        run.block(events, 256);
        events.note(CLAP_EVENT_NOTE_ON, 0, 3, 69, 1.0);
        run.block(events, 256);
        run.block(events, 256);
        CHECK(run.peak < 1e-3f); // note 2's release tail only
        // a reset keeps the parameter's value (CLAP), as does a new patch
        events.note(CLAP_EVENT_NOTE_OFF, 0, 3, 69, 0.0);
        events.assignSwitch(0, 1, -1, 1.0);
        run.block(events, 256);
        plugin->reset(plugin);
        CHECK(params->get_value(plugin, 2, &value) && value == 1);
        events.note(CLAP_EVENT_NOTE_ON, 0, 4, 69, 1.0);
        run.block(events, 256);
        CHECK(run.peak > 0.1f);
        events.note(CLAP_EVENT_NOTE_OFF, 0, 4, 69, 0.0);
        run.block(events, 256);
        CHECK(load(plugin, stateFor(patch + " ")));
        run.block(events, 256); // the new patch takes over
        events.note(CLAP_EVENT_NOTE_ON, 0, 5, 69, 1.0);
        run.block(events, 256);
        CHECK(run.peak > 0.1f);
        events.note(CLAP_EVENT_NOTE_OFF, 0, 5, 69, 0.0);
        events.assignSwitch(0, 1, -1, 0.0);
        run.block(events, 256);
        // a shared sound: prepared with its switch on, addressed by its channel, reported as a value
        auto* ctx = static_cast<const oscairn_plugin_sound_contexts_t*>(plugin->get_extension(plugin, OSCAIRN_EXT_SOUND_CONTEXTS));
        auto* own = static_cast<const oscairn_plugin_sound_snapshots_t*>(plugin->get_extension(plugin, OSCAIRN_EXT_SOUND_SNAPSHOTS));
        char message[256] {};
        const oscairn_sound_parameter_t on[2] = { { 1, 2 }, { 2, 1 } }, bad { 2, 2 };
        CHECK(ctx->prepare(plugin, "Bad", &bad, 1, patch.c_str(), nullptr, message, sizeof(message)) == CLAP_INVALID_ID);
        const uint32_t id = ctx->prepare(plugin, "Switched", on, 2, patch.c_str(), nullptr, message, sizeof(message));
        oscairn_sound_parameter_t values[3] {};
        CHECK(own->sound_values(plugin, id, values, 3) == 3 && values[0].value == 2 && values[1].id == 2 && values[1].value == 1
              && values[2].id == 3 && values[2].value == 0);
        CHECK(ctx->select(plugin, id, 3));
        BusRun shared { plugin };
        events.note(CLAP_EVENT_NOTE_ON, 0, 5, 69, 1.0, 3);
        shared.block(events, 256);
        CHECK(shared.peak(2) > 0.1f);
        events.note(CLAP_EVENT_NOTE_OFF, 0, 5, 69, 0.0, 3);
        events.assignSwitch(0, 1, 0, 0.0); // channel 0's sound only
        shared.block(events, 256);
        events.note(CLAP_EVENT_NOTE_ON, 0, 6, 69, 1.0, 3);
        shared.block(events, 256);
        CHECK(shared.peak(2, 512) > 0.1f);
        events.note(CLAP_EVENT_NOTE_OFF, 0, 6, 69, 0.0, 3);
        events.assignSwitch(0, 1, 3, 0.0);
        shared.block(events, 256);
        shared.block(events, 256);
        events.note(CLAP_EVENT_NOTE_ON, 0, 7, 69, 1.0, 3);
        shared.block(events, 256);
        CHECK(shared.peak(2, 1280) < 1e-3f); // note 6's release tail only
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    { // a tempo-synced key-on delay follows the host's tempo
        auto* plugin = create();
        CHECK(load(plugin, stateFor("<control> oscairn_profile=1 <region> sample=*sine oscairn_delay_note=11\n")));
        CHECK(plugin->activate(plugin, 48000, 1, 480));
        clap_event_transport_t transport {};
        transport.header = { sizeof(transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0 };
        transport.flags = CLAP_TRANSPORT_HAS_TEMPO;
        transport.tempo = 240.0; // a quarter: 0.25 s
        Run run { plugin };
        run.transport = &transport;
        Events events;
        int first = -1;
        events.note(CLAP_EVENT_NOTE_ON, 0, 1, 69, 1.0);
        for (int b = 0; b < 40 && first < 0; ++b) {
            run.block(events, 480);
            for (int i = 0; i < 480 && first < 0; ++i)
                if (run.left[i] != 0.0f)
                    first = b * 480 + i;
        }
        CHECK(std::abs(first - 12000) <= 2);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    { // a mono part keeps a held note whose voices a newer note cut: it plays again at that note's end
        auto* plugin = create();
        CHECK(load(plugin, stateFor("<control> oscairn_profile=1 <region> sample=*sine oscairn_element=1 oscairn_mono=1\n")));
        CHECK(plugin->activate(plugin, 48000, 1, 256));
        Run run { plugin };
        Events events;
        auto blocks = [&](int n) { for (int b = 0; b < n; ++b) run.block(events, 256); };
        events.note(CLAP_EVENT_NOTE_ON, 0, 1, 60, 1.0);
        blocks(2);
        events.note(CLAP_EVENT_NOTE_ON, 0, 2, 67, 1.0);
        blocks(20);
        CHECK(events.ends(1) == 0 && run.peak > 0.1f);
        events.note(CLAP_EVENT_NOTE_OFF, 0, 2, 67, 0.0);
        blocks(20);
        CHECK(events.ends(2) == 1 && events.ends(1) == 0 && run.peak > 0.1f); // note 1 again
        events.note(CLAP_EVENT_NOTE_OFF, 0, 1, 60, 0.0);
        blocks(20);
        CHECK(events.ends(1) == 1 && run.peak == 0.0f);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    { // threads: a sound's synth adds its loader and its garbage collector, whatever the machine's
      // cores (a process that locks its memory holds every thread's stack)
        const int before = threadCount();
        auto* plugin = create();
        CHECK(load(plugin, stateFor(sine)));
        CHECK(threadCount() - before == 2);
        plugin->destroy(plugin);
        CHECK(threadCount() == before);
    }
    { // memory while notes are held on long files is bounded: a file streams in chunks near where
      // its voices play, not whole (sixteen 20 s files would be 56 MB decoded)
        const std::string dir = longFiles(16, 20.0);
        CHECK(!dir.empty());
        const double before = heapMB();
        auto* plugin = create();
        CHECK(load(plugin, stateFor(readFile(dir + "/patch.sfz"), "", "", dir)));
        CHECK(plugin->activate(plugin, 48000, 1, 128));
        const double loaded = heapMB() - before;
        Run run { plugin };
        Events events;
        for (int k = 0; k < 16; ++k)
            events.note(CLAP_EVENT_NOTE_ON, 0, k, static_cast<int16_t>(48 + k), 1.0);
        double held = 0;
        for (int b = 0; b < 700; ++b) { // 1.9 s of sound, at a little over real time
            run.block(events, 128);
            std::this_thread::sleep_for(std::chrono::microseconds(1500));
            if (b % 50 == 49)
                held = std::max(held, heapMB() - before);
        }
        CHECK(run.peak > 0.05f && activeVoices(plugin) == 16);
        std::printf("held notes: %.1f MB after loading, %.1f MB at most while held\n", loaded, held);
        CHECK(held < loaded + 12);
        CHECK(sfz::FilePool::getStreamCacheUsage() <= sfz::FilePool::getStreamCacheCapacity());
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
        removeFiles(dir);
    }
    { // a voice whose next chunk is late plays silence and goes on, rather than ending: with no
      // room for chunks at all it holds past its preload, and sounds again once there is room
        const std::string dir = longFiles(1, 10.0);
        setenv("OSCAIRN_SAMPLER_MEMORY", "minimal", 1); // 2048 frames: the voice is past them below
        auto* plugin = create(); // the entry sets the capacity from the machine's memory
        const size_t capacity = sfz::FilePool::getStreamCacheCapacity();
        sfz::FilePool::setStreamCacheCapacity(0);
        CHECK(load(plugin, stateFor(readFile(dir + "/patch.sfz"), "", "", dir)));
        CHECK(plugin->activate(plugin, 48000, 1, 128));
        Run run { plugin };
        Events events;
        events.note(CLAP_EVENT_NOTE_ON, 0, 1, 48, 1.0);
        run.block(events, 128);
        CHECK(run.peak > 0.05f); // the preload
        for (int b = 0; b < 100; ++b) { // 0.27 s: past its preload
            run.block(events, 128);
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        CHECK(run.peak == 0.0f && activeVoices(plugin) == 1 && events.ends(1) == 0);
        sfz::FilePool::setStreamCacheCapacity(capacity);
        float peak = 0;
        for (int b = 0; b < 100; ++b) {
            run.block(events, 128);
            peak = std::max(peak, run.peak);
            std::this_thread::sleep_for(std::chrono::microseconds(2000));
        }
        CHECK(peak > 0.05f && activeVoices(plugin) == 1);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
        unsetenv("OSCAIRN_SAMPLER_MEMORY");
        removeFiles(dir);
    }
    { // voices: a sound plays up to 256 at once (sfizz's own default is 64), and the host's
      // voice budget is the limit below that
        const std::string two = "<region> sample=*sine amp_veltrack=0 ampeg_release=0.01\n<region> sample=*saw amp_veltrack=0 ampeg_release=0.01\n";
        auto* plugin = create();
        CHECK(load(plugin, stateFor(two)));
        CHECK(plugin->activate(plugin, 48000, 1, 128));
        Run run { plugin };
        Events events;
        for (int k = 0; k < 120; ++k)
            events.note(CLAP_EVENT_NOTE_ON, 0, k, static_cast<int16_t>(4 + k), 1.0);
        for (int b = 0; b < 20; ++b) // past any steal's fade
            run.block(events, 128);
        CHECK(activeVoices(plugin) == 240);
        setLimits(plugin, 100, { selectedEntry(0, 0, 5) });
        for (int b = 0; b < 20; ++b)
            run.block(events, 128);
        CHECK(activeVoices(plugin) == 100);
        setLimits(plugin, 0);
        for (int k = 0; k < 120; ++k)
            events.note(CLAP_EVENT_NOTE_OFF, 0, k, static_cast<int16_t>(4 + k), 0.0);
        for (int b = 0; b < 200 && run.status != CLAP_PROCESS_SLEEP; ++b)
            run.block(events, 128);
        CHECK(activeVoices(plugin) == 0);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    { // a host's large maximum block costs no more than 512 frames' worth: the synths render at
      // most that many at a time (Oscairn's workers offer 8192)
        auto* plugin = create();
        CHECK(load(plugin, stateFor(sine)));
        const double before = heapMB();
        CHECK(plugin->activate(plugin, 48000, 1, 8192));
        CHECK(heapMB() - before < 8);
        Run run { plugin };
        Events events;
        events.note(CLAP_EVENT_NOTE_ON, 0, 1, 60, 1.0);
        run.block(events, 8192);
        CHECK(run.peak > 0.1f && run.left[100] != 0.0f && run.left[8000] != 0.0f);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
    { // the memory policy: a sound preloads 2048 frames a file when told "minimal", 16384 when
      // told "generous", and its declaration follows; activating only ever lowers what it holds
        const std::string dir = longFiles(4, 2.0);
        uint64_t assets[2] {};
        int i = 0;
        for (const char* policy : { "minimal", "generous" }) {
            setenv("OSCAIRN_SAMPLER_MEMORY", policy, 1);
            auto* plugin = create();
            CHECK(load(plugin, stateFor(readFile(dir + "/patch.sfz"), "", "", dir)));
            oscairn_memory_t estimate {};
            CHECK(memoryOf(plugin) && memoryOf(plugin)->estimate(plugin, 48000, 128, &estimate));
            assets[i++] = estimate.asset_bytes;
            if (!std::strcmp(policy, "generous")) {
                setenv("OSCAIRN_SAMPLER_MEMORY", "minimal", 1);
                CHECK(plugin->activate(plugin, 48000, 1, 128));
                CHECK(memoryOf(plugin)->estimate(plugin, 48000, 128, &estimate));
                CHECK(estimate.asset_bytes == assets[0]);
                plugin->deactivate(plugin);
                setenv("OSCAIRN_SAMPLER_MEMORY", "generous", 1);
                CHECK(plugin->activate(plugin, 48000, 1, 128));
                CHECK(memoryOf(plugin)->estimate(plugin, 48000, 128, &estimate));
                CHECK(estimate.asset_bytes == assets[0]); // not raised again
                plugin->deactivate(plugin);
            }
            plugin->destroy(plugin);
        }
        unsetenv("OSCAIRN_SAMPLER_MEMORY");
        CHECK(assets[0] >= 4 * 2048 * sizeof(float));
        CHECK(assets[0] < 4 * 4096 * sizeof(float));
        CHECK(assets[1] >= 4 * 16384 * sizeof(float));
        removeFiles(dir);
    }
    { // oscairn.memory: the declaration before activating covers what loading and activating
      // take, without counting it twice, and what the plugin says it holds while notes are held
      // covers what it holds
        const std::string dir = longFiles(8, 10.0);
        const double before = heapMB();
        auto* plugin = create();
        CHECK(memoryOf(plugin) && memoryOf(plugin)->estimate && memoryOf(plugin)->current);
        CHECK(load(plugin, stateFor(readFile(dir + "/patch.sfz"), "", "", dir)));
        oscairn_memory_t estimate {}, now {};
        CHECK(memoryOf(plugin) && memoryOf(plugin)->estimate(plugin, 48000, 8192, &estimate));
        CHECK(plugin->activate(plugin, 48000, 1, 8192));
        const double loaded = heapMB() - before;
        CHECK(totalMB(estimate, false) >= loaded);
        CHECK(!heapKnown || totalMB(estimate, false) <= 2 * loaded + 4);
        CHECK(estimate.shared_bytes >= sfz::FilePool::getStreamCacheCapacity()); // the cache, at its capacity
        CHECK(estimate.asset_bytes > 8 * 4096 * sizeof(float)); // the preloads
        Run run { plugin };
        Events events;
        for (int k = 0; k < 8; ++k)
            events.note(CLAP_EVENT_NOTE_ON, 0, k, static_cast<int16_t>(48 + k), 1.0);
        for (int b = 0; b < 400; ++b) {
            run.block(events, 128);
            std::this_thread::sleep_for(std::chrono::microseconds(1500));
            if (b % 50 == 49) {
                CHECK(memoryOf(plugin) && memoryOf(plugin)->current(plugin, &now));
                CHECK(totalMB(now, true) >= heapMB() - before); // the chunks it holds included
            }
        }
        std::printf("memory: %.1f MB after loading, declared %.1f MB and the %.0f MB cache; %.1f MB held at the end, %.1f MB said\n",
                    loaded, totalMB(estimate, false), sfz::FilePool::getStreamCacheCapacity() / 1048576.0, heapMB() - before, totalMB(now, true));
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
        removeFiles(dir);
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
