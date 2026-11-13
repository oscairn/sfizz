// SPDX-License-Identifier: BSD-2-Clause

// Copyright (c) 2019-2020, Paul Ferrand, Andrea Zanellato
// All rights reserved.

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:

// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.

// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "FilePool.h"
#include "AudioReader.h"
#include "Buffer.h"
#include "AudioBuffer.h"
#include "AudioSpan.h"
#include "Config.h"
#include "utility/SwapAndPop.h"
#include "utility/Debug.h"
#include <absl/types/span.h>
#include <absl/strings/match.h>
#include <absl/memory/memory.h>
#include <algorithm>
#include <memory>
#include <thread>
#include <system_error>
#include <atomic_queue/defs.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif
using namespace std::placeholders;

// The chunks of streamed files, for the whole process
static std::atomic<size_t> streamCacheCapacity { sfz::config::streamCacheBytes };
static std::atomic<size_t> streamCacheUsage { 0 };
static constexpr size_t chunkBytes(unsigned channels)
{
    return sizeof(sfz::StreamChunk) + channels * sfz::config::streamChunkFrames * sizeof(float);
}
static int64_t steadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

sfz::StreamState::StreamState(fs::path path, uint32_t frames, unsigned channels)
    : path(std::move(path)), frames(frames), channels(channels),
      numSlots((frames + config::streamChunkFrames - 1) / config::streamChunkFrames),
      slots(new StreamSlot[numSlots])
{
}

sfz::StreamState::~StreamState()
{
    for (size_t i = 0; i < numSlots; ++i)
        retired.push_back(slots[i].chunk.exchange(nullptr));
    for (StreamChunk* chunk : retired) {
        if (chunk) {
            streamCacheUsage -= chunkBytes(channels);
            delete chunk;
        }
    }
}

void readBaseFile(sfz::AudioReader& reader, sfz::FileAudioBuffer& output, uint32_t numFrames)
{
    output.reset();
    output.resize(numFrames);

    const unsigned channels = reader.channels();

    if (channels == 1) {
        output.addChannel();
        output.clear();
        reader.readNextBlock(output.channelWriter(0), numFrames);
    } else if (channels == 2) {
        output.addChannel();
        output.addChannel();
        output.clear();
        sfz::Buffer<float> tempReadBuffer { 2 * numFrames };
        reader.readNextBlock(tempReadBuffer.data(), numFrames);
        sfz::readInterleaved(tempReadBuffer, output.getSpan(0), output.getSpan(1));
    }
}

sfz::FileAudioBuffer readFromFile(sfz::AudioReader& reader, uint32_t numFrames)
{
    sfz::FileAudioBuffer baseBuffer;
    readBaseFile(reader, baseBuffer, numFrames);
    return baseBuffer;
}

void streamFromFile(sfz::AudioReader& reader, sfz::FileAudioBuffer& output, std::atomic<size_t>* filledFrames = nullptr)
{
    const auto numFrames = static_cast<size_t>(reader.frames());
    const auto numChannels = reader.channels();
    const auto chunkSize = static_cast<size_t>(sfz::config::fileChunkSize);

    output.reset();
    output.addChannels(reader.channels());
    output.resize(numFrames);
    output.clear();

    sfz::Buffer<float> fileBlock { chunkSize * numChannels };
    size_t inputFrameCounter { 0 };
    size_t outputFrameCounter { 0 };
    bool inputEof = false;

    while (!inputEof && inputFrameCounter < numFrames)
    {
        auto thisChunkSize = std::min(chunkSize, numFrames - inputFrameCounter);
        const auto numFramesRead = static_cast<size_t>(
            reader.readNextBlock(fileBlock.data(), thisChunkSize));
        if (numFramesRead == 0)
            break;

        if (numFramesRead < thisChunkSize) {
            inputEof = true;
            thisChunkSize = numFramesRead;
        }
        const auto outputChunkSize = thisChunkSize;

        for (size_t chanIdx = 0; chanIdx < numChannels; chanIdx++) {
            const auto outputChunk = output.getSpan(chanIdx).subspan(outputFrameCounter, outputChunkSize);
            for (size_t i = 0; i < thisChunkSize; ++i)
                outputChunk[i] = fileBlock[i * numChannels + chanIdx];
        }
        inputFrameCounter += thisChunkSize;
        outputFrameCounter += outputChunkSize;

        if (filledFrames != nullptr)
            filledFrames->fetch_add(outputChunkSize);
    }
}

sfz::FilePool::FilePool()
    : filesToLoad(alignedNew<FileQueue>()),
      chunksToLoad(alignedNew<ChunkQueue>())
{
    lastUsedFiles.reserve(config::maxVoices);
    garbageToCollect.reserve(config::maxVoices);
    gatherBuffer.addChannels(2);
    gatherBuffer.resize(getGatherFrames());
    dispatchThread.start([this]() { dispatchingJob(); });
    garbageThread.start([this]() { garbageJob(); });
}

sfz::FilePool::~FilePool()
{
    std::error_code ec;

    garbageFlag = false;
    semGarbageBarrier.post(ec);
    garbageThread.join();

    dispatchFlag = false;
    dispatchBarrier.post(ec);
    dispatchThread.join();
}

bool sfz::FilePool::checkSample(std::string& filename) const noexcept
{
    fs::path path { rootDirectory / filename };
    std::error_code ec;
    if (fs::exists(path, ec))
        return true;

#if defined(_WIN32)
    return false;
#else
    fs::path oldPath = std::move(path);
    path = oldPath.root_path();

    static const fs::path dot { "." };
    static const fs::path dotdot { ".." };

    for (const fs::path& part : oldPath.relative_path()) {
        if (part == dot || part == dotdot) {
            path /= part;
            continue;
        }

        if (fs::exists(path / part, ec)) {
            path /= part;
            continue;
        }

        auto it = path.empty() ? fs::directory_iterator { dot, ec } : fs::directory_iterator { path, ec };
        if (ec) {
            DBG("Error creating a directory iterator for " << filename << " (Error code: " << ec.message() << ")");
            return false;
        }

        auto searchPredicate = [&part](const fs::directory_entry &ent) -> bool {
#if !defined(GHC_USE_WCHAR_T)
            return absl::EqualsIgnoreCase(
                ent.path().filename().native(), part.native());
#else
            return absl::EqualsIgnoreCase(
                ent.path().filename().u8string(), part.u8string());
#endif
        };

        while (it != fs::directory_iterator {} && !searchPredicate(*it))
            it.increment(ec);

        if (it == fs::directory_iterator {}) {
            DBG("File not found, could not resolve " << filename);
            return false;
        }

        path /= it->path().filename();
    }

    const auto newPath = fs::relative(path, rootDirectory, ec);
    if (ec) {
        DBG("Error extracting the new relative path for " << filename << " (Error code: " << ec.message() << ")");
        return false;
    }
    DBG("Updating " << filename << " to " << newPath);
    filename = newPath.string();
    return true;
#endif
}

bool sfz::FilePool::checkSampleId(FileId& fileId) const noexcept
{
    if (loadedFiles.contains(fileId))
        return true;

    std::string filename = fileId.filename();
    bool result = checkSample(filename);
    if (result)
        fileId = FileId(std::move(filename), fileId.isReverse());
    return result;
}

absl::optional<sfz::FileInformation> getReaderInformation(sfz::AudioReader* reader) noexcept
{
    const unsigned channels = reader->channels();
    if (channels != 1 && channels != 2)
        return {};

    sfz::FileInformation returnedValue;
    returnedValue.end = static_cast<uint32_t>(reader->frames()) - 1;
    returnedValue.sampleRate = static_cast<double>(reader->sampleRate());
    returnedValue.numChannels = static_cast<int>(channels);

    // Check for instrument info
    sfz::InstrumentInfo instrumentInfo {};
    if (reader->getInstrumentInfo(instrumentInfo)) {
        returnedValue.rootKey = clamp<uint8_t>(instrumentInfo.basenote, 0, 127);
        if (reader->type() == sfz::AudioReaderType::Forward) {
            if (instrumentInfo.loop_count > 0) {
                returnedValue.hasLoop = true;
                returnedValue.loopStart = instrumentInfo.loops[0].start;
                returnedValue.loopEnd =
                    min(returnedValue.end, static_cast<int64_t>(instrumentInfo.loops[0].end - 1));
            }
        } else {
            // TODO loops ignored when reversed
            //   prehaps it can make use of SF_LOOP_BACKWARD?
        }
    }

    // Check for wavetable info
    sfz::WavetableInfo wt {};
    if (reader->getWavetableInfo(wt))
        returnedValue.wavetable = wt;

    return returnedValue;
}

absl::optional<sfz::FileInformation> sfz::FilePool::checkExistingFileInformation(const FileId& fileId) noexcept
{
    const auto loadedFile = loadedFiles.find(fileId);
    if (loadedFile != loadedFiles.end())
        return loadedFile->second.information;

    const auto preloadedFile = preloadedFiles.find(fileId);
    if (preloadedFile != preloadedFiles.end())
        return preloadedFile->second.information;

    return {};
}

absl::optional<sfz::FileInformation> sfz::FilePool::getFileInformation(const FileId& fileId) noexcept
{
    auto existingInformation = checkExistingFileInformation(fileId);
    if (existingInformation)
        return existingInformation;

    if (sharedPreloads) {
        std::lock_guard<std::mutex> lock { sharedPreloads->mutex };
        const auto shared = sharedPreloads->entries.find(sharedKey(fileId));
        if (shared != sharedPreloads->entries.end() && !shared->second.data.expired())
            return shared->second.information;
    }

    const fs::path file { rootDirectory / fileId.filename() };

    if (!fs::exists(file))
        return {};

    ++fileOpens;
    AudioReaderPtr reader = createAudioReader(file, fileId.isReverse());
    return getReaderInformation(reader.get());
}

std::string sfz::FilePool::sharedKey(const FileId& fileId) const
{
    return (rootDirectory / fileId.filename()).string() + (fileId.isReverse() ? "|r" : "");
}

bool sfz::FilePool::preloadFile(const FileId& fileId, uint32_t maxOffset) noexcept
{
    std::lock_guard<std::mutex> files { filesMutex };
    ++generation;
    const auto loadedFile = loadedFiles.find(fileId);
    if (loadedFile != loadedFiles.end()) {
        loadedFile->second.preloadCallCount++;
        return true;
    }

    auto fileInformation = getFileInformation(fileId);
    if (!fileInformation)
        return false;

    fileInformation->maxOffset = maxOffset;
    const fs::path file { rootDirectory / fileId.filename() };

    // Another pool's data for this file, if it holds at least what this one needs
    std::shared_ptr<FileAudioBuffer> sharedData;
    uint32_t frames = 0;
    double sampleRate = 0.0;
    std::string key;
    if (sharedPreloads) {
        key = sharedKey(fileId);
        std::lock_guard<std::mutex> lock { sharedPreloads->mutex };
        const auto shared = sharedPreloads->entries.find(key);
        if (shared != sharedPreloads->entries.end()) {
            frames = shared->second.frames;
            const uint32_t wanted = loadInRam ? frames : min(frames, maxOffset + preloadSize);
            sharedData = shared->second.data.lock();
            if (sharedData && sharedData->getNumFrames() >= wanted)
                sampleRate = shared->second.information.sampleRate;
            else
                sharedData.reset();
        }
    }
    AudioReaderPtr reader;
    if (!sharedData) {
        ++fileOpens;
        reader = createAudioReader(file, fileId.isReverse());
        frames = static_cast<uint32_t>(reader->frames());
        sampleRate = static_cast<double>(reader->sampleRate());
    }

    const auto framesToLoad = [&]() {
        if (loadInRam)
            return frames;
        else
            return min(frames, maxOffset + preloadSize);
    }();
    // Reads the file, unless the shared data covers it; offers what it read to the other pools
    auto load = [&]() -> std::shared_ptr<FileAudioBuffer> {
        if (sharedData)
            return sharedData;
        auto data = std::make_shared<FileAudioBuffer>(readFromFile(*reader, framesToLoad));
        if (sharedPreloads) {
            FileInformation information = *fileInformation;
            information.sampleRate = sampleRate;
            std::lock_guard<std::mutex> lock { sharedPreloads->mutex };
            auto& entry = sharedPreloads->entries[key];
            const auto held = entry.data.lock();
            if (!held || held->getNumFrames() < data->getNumFrames())
                entry = { data, information, frames };
        }
        return data;
    };

    const auto existingFile = preloadedFiles.find(fileId);
    if (existingFile != preloadedFiles.end()) {
        auto& fileData = existingFile->second;
        if (framesToLoad > fileData.preloadedData->getNumFrames()) {
            fileData.information.maxOffset = maxOffset;
            fileData.preloadedData = load();
            fileData.fullyLoaded = frames == framesToLoad;
            makeStream(fileId, fileData, frames);
        }
        fileData.preloadCallCount++;
    } else {
        fileInformation->sampleRate = sampleRate;
        auto insertedPair = preloadedFiles.insert_or_assign(fileId, {
            load(),
            *fileInformation
        });

        insertedPair.first->second.preloadCallCount++;
        insertedPair.first->second.status = FileData::Status::Preloaded;
        insertedPair.first->second.fullyLoaded = framesToLoad == frames;
        makeStream(fileId, insertedPair.first->second, frames);
    }

    return true;
}

void sfz::FilePool::makeStream(const FileId& fileId, FileData& data, uint32_t frames)
{
    // Reversed files are read whole (the reader reverses them in memory)
    if (data.fullyLoaded || loadInRam || fileId.isReverse())
        data.stream.reset();
    else
        data.stream.reset(new StreamState(rootDirectory / fileId.filename(), frames,
                                          data.preloadedData->getNumChannels()));
}

void sfz::FilePool::resetPreloadCallCounts() noexcept
{
    for (auto& preloadedFile: preloadedFiles)
        preloadedFile.second.preloadCallCount = 0;

    for (auto& loadedFile: loadedFiles)
        loadedFile.second.preloadCallCount = 0;
}

void sfz::FilePool::removeUnusedPreloadedData() noexcept
{
    std::lock_guard<std::mutex> files { filesMutex };
    ++generation;
    for (auto it = preloadedFiles.begin(), end = preloadedFiles.end(); it != end; ) {
        auto copyIt = it++;
        if (copyIt->second.preloadCallCount == 0) {
            DBG("[sfizz] Removing unused preloaded data: " << copyIt->first.filename());
            preloadedFiles.erase(copyIt);
        }
    }

    for (auto it = loadedFiles.begin(), end = loadedFiles.end(); it != end; ) {
        auto copyIt = it++;
        if (copyIt->second.preloadCallCount == 0) {
            DBG("[sfizz] Removing unused loaded data: " << copyIt->first.filename());
            loadedFiles.erase(copyIt);
        }
    }
}

sfz::FileDataHolder sfz::FilePool::loadFile(const FileId& fileId) noexcept
{
    auto fileInformation = getFileInformation(fileId);
    if (!fileInformation)
        return {};

    const auto existingFile = loadedFiles.find(fileId);
    if (existingFile != loadedFiles.end()) {
        existingFile->second.preloadCallCount++;
        return { &existingFile->second };
    }

    const fs::path file { rootDirectory / fileId.filename() };
    ++fileOpens;
    AudioReaderPtr reader = createAudioReader(file, fileId.isReverse());

    std::lock_guard<std::mutex> files { filesMutex };
    ++generation;
    const auto frames = static_cast<uint32_t>(reader->frames());
    auto insertedPair = loadedFiles.insert_or_assign(fileId, {
        std::make_shared<FileAudioBuffer>(readFromFile(*reader, frames)),
        *fileInformation
    });
    insertedPair.first->second.preloadCallCount++;
    insertedPair.first->second.status = FileData::Status::Preloaded;
    insertedPair.first->second.fullyLoaded = true;
    return { &insertedPair.first->second };
}

sfz::FileDataHolder sfz::FilePool::loadFromRam(const FileId& fileId, const std::vector<char>& data) noexcept
{
    const auto loaded = loadedFiles.find(fileId);
    if (loaded != loadedFiles.end())
        return { &loaded->second };

    auto reader = createAudioReaderFromMemory(data.data(), data.size(), fileId.isReverse());
    auto fileInformation = getReaderInformation(reader.get());
    std::lock_guard<std::mutex> files { filesMutex };
    ++generation;
    const auto frames = static_cast<uint32_t>(reader->frames());
    auto insertedPair = loadedFiles.insert_or_assign(fileId, {
        std::make_shared<FileAudioBuffer>(readFromFile(*reader, frames)),
        *fileInformation
    });
    insertedPair.first->second.preloadCallCount++;
    insertedPair.first->second.status = FileData::Status::Preloaded;
    insertedPair.first->second.fullyLoaded = true;
    DBG("Added a file " << fileId.filename());
    return { &insertedPair.first->second };
}

sfz::FileDataHolder sfz::FilePool::getFilePromise(const std::shared_ptr<FileId>& fileId) noexcept
{
    const auto loaded = loadedFiles.find(*fileId);
    if (loaded != loadedFiles.end())
        return { &loaded->second };

    const auto preloaded = preloadedFiles.find(*fileId);
    if (preloaded == preloadedFiles.end()) {
        DBG("[sfizz] File not found in the preloaded files: " << fileId->filename());
        return {};
    }

    auto& fileData = preloaded->second;
    if (!fileData.fullyLoaded && !fileData.stream) {
        QueuedFileData queuedData { fileId, &fileData };
        if (!filesToLoad->try_push(queuedData)) {
            DBG("[sfizz] Could not enqueue the file to load for " << fileId << " (queue capacity " << filesToLoad->capacity() << ")");
            return {};
        }

        std::error_code ec;
        dispatchBarrier.post(ec);
        ASSERT(!ec);
    }

    return { &preloaded->second };
}

void sfz::FilePool::setPreloadSize(uint32_t preloadSize) noexcept
{
    std::lock_guard<std::mutex> files { filesMutex };
    ++generation;
    this->preloadSize = preloadSize;
    if (loadInRam)
        return;

    // Update all the preloaded sizes
    for (auto& preloadedFile : preloadedFiles) {
        auto& fileId = preloadedFile.first;
        auto& fileData = preloadedFile.second;
        const auto maxOffset = fileData.information.maxOffset;
        fs::path file { rootDirectory / fileId.filename() };
        AudioReaderPtr reader = createAudioReader(file, fileId.isReverse());
        const auto frames = reader->frames();
        const auto framesToLoad = min(frames, maxOffset + preloadSize);
        fileData.preloadedData = std::make_shared<FileAudioBuffer>(readFromFile(*reader, static_cast<uint32_t>(framesToLoad)));
        fileData.fullyLoaded = frames == framesToLoad;
        makeStream(fileId, fileData, static_cast<uint32_t>(frames));
    }
}

void sfz::FilePool::loadingJob(const QueuedFileData& data) noexcept
{
    raiseCurrentThreadPriority();

    std::shared_ptr<FileId> id = data.id.lock();
    if (!id) {
        // file ID was nulled, it means the region was deleted, ignore
        return;
    }

    const fs::path file { rootDirectory / id->filename() };
    std::error_code readError;
    AudioReaderPtr reader = createAudioReader(file, id->isReverse(), &readError);

    if (readError) {
        DBG("[sfizz] reading the file errored for " << *id << " with code " << readError << ": " << readError.message());
        return;
    }

    FileData::Status currentStatus;

    unsigned spinCounter { 0 };

    while (1) {
        currentStatus = data.data->status.load();
        while (currentStatus == FileData::Status::Invalid) {
            // Spin until the state changes
            if (spinCounter > 1024) {
                DBG("[sfizz] " << *id << " is stuck on Invalid? Leaving the load");
                return;
            }

            std::this_thread::sleep_for(std::chrono::microseconds(100));
            currentStatus = data.data->status.load();
            spinCounter += 1;
        }
        // wait for garbage collection
        if (currentStatus == FileData::Status::GarbageCollecting) {
            atomic_queue::spin_loop_pause();
            atomic_queue::spin_loop_pause();
            atomic_queue::spin_loop_pause();
            atomic_queue::spin_loop_pause();
            continue;
        }
        // Already loading or loaded
        if (currentStatus != FileData::Status::Preloaded)
            return;

        // go outside loop if this gets token
        if (data.data->status.compare_exchange_strong(currentStatus, FileData::Status::Streaming))
            break;
    }

    streamFromFile(*reader, data.data->fileData, &data.data->availableFrames);

    data.data->status = FileData::Status::Done;

    std::lock_guard<SpinMutex> guard { garbageAndLastUsedMutex };
    if (absl::c_find(lastUsedFiles, *id) == lastUsedFiles.end())
        lastUsedFiles.push_back(*id);
}

void sfz::FilePool::clear()
{
    std::lock_guard<std::mutex> files { filesMutex };
    ++generation;
    std::lock_guard<SpinMutex> guard { garbageAndLastUsedMutex };
    emptyFileLoadingQueues();
    garbageToCollect.clear();
    lastUsedFiles.clear();
    preloadedFiles.clear();
    loadedFiles.clear();
}

uint32_t sfz::FilePool::getPreloadSize() const noexcept
{
    return preloadSize;
}

// One loader thread per pool: whole files (reversed ones), then the chunks voices asked for,
// and, at least every 100 ms, the chunks no voice has wanted for a while are freed
void sfz::FilePool::dispatchingJob() noexcept
{
    raiseCurrentThreadPriority();
    int64_t lastSweep = steadyMs();
    while (true) {
        std::error_code ec;
        dispatchBarrier.timed_wait(100, ec);
        if (!dispatchFlag)
            break;
        std::lock_guard<std::mutex> files { filesMutex };
        {
            std::lock_guard<std::mutex> loading { loadingMutex };
            QueuedFileData queuedData;
            while (filesToLoad->try_pop(queuedData))
                loadingJob(queuedData);
        }
        ChunkRequest request;
        while (chunksToLoad->try_pop(request))
            if (request.generation == generation.load())
                loadRequestedChunk(*request.stream, request.slot);
        const int64_t now = steadyMs();
        if (now - lastSweep >= 100) {
            freeUnwantedChunks(now - config::streamUnwantedMs);
            lastSweep = now;
        }
    }
}

void sfz::FilePool::loadRequestedChunk(StreamState& stream, size_t slot) noexcept
{
    StreamSlot& s = stream.slots[slot];
    int requested = 1;
    if (!s.state.compare_exchange_strong(requested, 2))
        return; // a freewheeling render took it
    if (s.chunk.load()) {
        s.state = 0;
        return;
    }
    const size_t bytes = chunkBytes(stream.channels);
    if (streamCacheUsage + bytes > streamCacheCapacity) {
        freeUnwantedChunks(steadyMs() - config::streamPressureMs);
        if (streamCacheUsage + bytes > streamCacheCapacity) {
            s.state = 0; // full of chunks voices play: asked again by the voice's next block
            return;
        }
    }
    {
        std::lock_guard<std::mutex> decoding { stream.decodeMutex };
        decodeChunk(stream, slot);
    }
    s.state = 0;
}

// A freewheeling render's: decodes the chunk on the calling thread unless the loader is at it
void sfz::FilePool::loadChunkNow(StreamState& stream, size_t slot) noexcept
{
    StreamSlot& s = stream.slots[slot];
    while (!s.chunk.load()) {
        int state = s.state.load();
        if (state == 2 || !s.state.compare_exchange_strong(state, 2)) {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
            continue;
        }
        {
            std::lock_guard<std::mutex> decoding { stream.decodeMutex };
            decodeChunk(stream, slot);
        }
        s.state = 0;
    }
}

void sfz::FilePool::decodeChunk(StreamState& stream, size_t slot) noexcept
{
    const size_t K = config::streamChunkFrames;
    const uint64_t start = slot * K;
    const size_t frames = std::min<size_t>(K, stream.frames - start);
    const unsigned channels = stream.channels;
    std::unique_ptr<StreamChunk> chunk { new StreamChunk };
    chunk->data.reset(new float[channels * K]());

    if (stream.reader && stream.readerPosition != start) {
        if (stream.reader->seek(start))
            stream.readerPosition = start;
        else if (stream.readerPosition > start)
            stream.reader.reset();
    }
    if (!stream.reader) {
        std::error_code ec;
        stream.reader = createAudioReader(stream.path, false, &ec);
        stream.readerPosition = 0;
        if (ec || !stream.reader || stream.reader->channels() != channels)
            stream.reader.reset(); // a file gone or changed plays as silence
        else if (start > 0 && stream.reader->seek(start))
            stream.readerPosition = start;
    }
    const size_t block = config::fileChunkSize;
    stream.decodeBuffer.resize(block * channels);
    float* buffer = stream.decodeBuffer.data();
    while (stream.reader && stream.readerPosition < start) { // formats without an exact seek
        const size_t n = stream.reader->readNextBlock(buffer, std::min<uint64_t>(block, start - stream.readerPosition));
        if (n == 0)
            stream.reader.reset();
        else
            stream.readerPosition += n;
    }
    for (size_t done = 0; stream.reader && done < frames;) {
        const size_t n = stream.reader->readNextBlock(buffer, std::min(block, frames - done));
        if (n == 0) {
            stream.reader.reset();
            break;
        }
        for (unsigned c = 0; c < channels; ++c)
            for (size_t i = 0; i < n; ++i)
                chunk->data[c * K + done + i] = buffer[i * channels + c];
        done += n;
        stream.readerPosition += n;
    }
    streamCacheUsage += chunkBytes(channels);
    stream.slots[slot].chunk.store(chunk.release());
}

void sfz::FilePool::freeUnwantedChunks(int64_t wantedBefore) noexcept
{
    for (auto& file : preloadedFiles) {
        StreamState* stream = file.second.stream.get();
        if (!stream)
            continue;
        bool wanted = false;
        for (size_t i = 0; i < stream->numSlots; ++i) {
            StreamSlot& s = stream->slots[i];
            if (s.wantedAt.load() >= wantedBefore) {
                wanted = true;
                continue;
            }
            if (s.state.load() == 0 && s.chunk.load())
                stream->retired.push_back(s.chunk.exchange(nullptr));
        }
        // freed once no audio thread copies from them (gather checks the slot after counting itself)
        swapAndPopAll(stream->retired, [stream](StreamChunk* chunk) {
            if (chunk && chunk->readers.load() != 0)
                return false;
            if (chunk) {
                streamCacheUsage -= chunkBytes(stream->channels);
                delete chunk;
            }
            return true;
        });
        if (!wanted && stream->reader && stream->decodeMutex.try_lock()) {
            stream->reader.reset(); // no file stays open for a file nobody plays
            stream->decodeMutex.unlock();
        }
    }
}

void sfz::FilePool::want(FileData& data, int64_t first, int64_t last) noexcept
{
    StreamState* stream = data.stream.get();
    if (!stream)
        return;
    first = std::max<int64_t>(first, data.preloadedData->getNumFrames());
    last = std::min<int64_t>(last, int64_t(stream->frames) - 1);
    if (first > last)
        return;
    const int64_t now = steadyMs();
    const uint32_t currentGeneration = generation.load();
    bool queued = false;
    for (int64_t c = first / config::streamChunkFrames; c <= last / config::streamChunkFrames; ++c) {
        StreamSlot& s = stream->slots[c];
        s.wantedAt.store(now, std::memory_order_relaxed);
        int idle = 0;
        if (s.chunk.load(std::memory_order_acquire) || !s.state.compare_exchange_strong(idle, 1))
            continue;
        if (chunksToLoad->try_push(ChunkRequest { stream, static_cast<uint32_t>(c), currentGeneration }))
            queued = true;
        else
            s.state = 0;
    }
    if (queued) {
        std::error_code ec;
        dispatchBarrier.post(ec);
    }
}

sfz::AudioSpan<const float> sfz::FilePool::gather(FileData& data, int64_t first, size_t count, bool wait) noexcept
{
    StreamState& stream = *data.stream;
    const FileAudioBuffer& preload = *data.preloadedData;
    const int64_t preloaded = preload.getNumFrames();
    const int64_t frames = stream.frames;
    const unsigned channels = stream.channels;
    const size_t K = config::streamChunkFrames;
    count = std::min(count, getGatherFrames());
    for (size_t i = 0; i < count;) {
        const int64_t frame = first + static_cast<int64_t>(i);
        size_t run;
        if (frame < 0 || frame >= frames) {
            run = frame < 0 ? std::min<size_t>(count - i, static_cast<size_t>(-frame)) : count - i;
            for (unsigned c = 0; c < channels; ++c)
                std::fill_n(gatherBuffer.channelWriter(c) + i, run, 0.0f);
        } else if (frame < preloaded) {
            run = std::min<size_t>(count - i, static_cast<size_t>(preloaded - frame));
            for (unsigned c = 0; c < channels; ++c)
                std::copy_n(preload.channelReader(c) + frame, run, gatherBuffer.channelWriter(c) + i);
        } else {
            const size_t slot = static_cast<size_t>(frame) / K;
            const size_t offset = static_cast<size_t>(frame) - slot * K;
            run = std::min<size_t>({ count - i, K - offset, static_cast<size_t>(frames - frame) });
            StreamSlot& s = stream.slots[slot];
            StreamChunk* chunk = s.chunk.load(std::memory_order_acquire);
            if (!chunk && wait) {
                loadChunkNow(stream, slot);
                chunk = s.chunk.load(std::memory_order_acquire);
            }
            if (chunk) {
                chunk->readers.fetch_add(1);
                if (s.chunk.load() != chunk) { // freed meanwhile
                    chunk->readers.fetch_sub(1);
                    chunk = nullptr;
                }
            }
            for (unsigned c = 0; c < channels; ++c) {
                if (chunk)
                    std::copy_n(chunk->data.get() + c * K + offset, run, gatherBuffer.channelWriter(c) + i);
                else
                    std::fill_n(gatherBuffer.channelWriter(c) + i, run, 0.0f); // not there in time
            }
            if (chunk)
                chunk->readers.fetch_sub(1);
        }
        i += run;
    }
    if (channels == 1)
        return AudioSpan<const float>({ gatherBuffer.channelReader(0) }, count);
    return AudioSpan<const float>({ gatherBuffer.channelReader(0), gatherBuffer.channelReader(1) }, count);
}

void sfz::FilePool::setStreamCacheCapacity(size_t bytes) noexcept { streamCacheCapacity = bytes; }
size_t sfz::FilePool::getStreamCacheCapacity() noexcept { return streamCacheCapacity; }
size_t sfz::FilePool::getStreamCacheUsage() noexcept { return streamCacheUsage; }

size_t sfz::FilePool::getPreloadedBytes(std::set<const void*>& counted) const noexcept
{
    std::lock_guard<std::mutex> files { filesMutex };
    size_t bytes = 0;
    auto add = [&bytes, &counted](const FileAudioBuffer& b) {
        if (b.getNumFrames() > 0 && counted.insert(&b).second)
            bytes += b.getNumChannels() * (b.getNumFrames() + 2 * config::excessFileFrames) * sizeof(float);
    };
    for (auto* map : { &preloadedFiles, &loadedFiles })
        for (auto& file : *map) {
            add(*file.second.preloadedData);
            add(file.second.fileData);
        }
    return bytes;
}

size_t sfz::FilePool::getFixedBytes() noexcept
{
    // the gather buffer and the two threads' stacks
    return 2 * (getGatherFrames() + 2 * config::excessFileFrames) * sizeof(float) + 2 * SmallStackThread::stackBytes;
}

void sfz::FilePool::garbageJob() noexcept
{
    while (semGarbageBarrier.wait(), garbageFlag) {
        std::lock_guard<SpinMutex> guard { garbageAndLastUsedMutex };
        garbageToCollect.clear();
    }
}

void sfz::FilePool::waitForBackgroundLoading() noexcept
{
    // Whole files the notes queued load here, or have loaded once the loader lets go
    std::lock_guard<std::mutex> loading { loadingMutex };
    QueuedFileData queuedData;
    while (filesToLoad->try_pop(queuedData))
        loadingJob(queuedData);
}

void sfz::FilePool::raiseCurrentThreadPriority() noexcept
{
#if defined(_WIN32)
    HANDLE thread = GetCurrentThread();
    const int priority = THREAD_PRIORITY_ABOVE_NORMAL; /*THREAD_PRIORITY_HIGHEST*/
    if (!SetThreadPriority(thread, priority)) {
        std::system_error error(GetLastError(), std::system_category());
        DBG("[sfizz] Cannot set current thread priority: " << error.what());
    }
#else
    pthread_t thread = pthread_self();
    int policy;
    sched_param param;

    if (pthread_getschedparam(thread, &policy, &param) != 0) {
        DBG("[sfizz] Cannot get current thread scheduling parameters");
        return;
    }

    policy = SCHED_RR;
    const int minprio = sched_get_priority_min(policy);
    const int maxprio = sched_get_priority_max(policy);
    param.sched_priority = minprio + config::backgroundLoaderPthreadPriority * (maxprio - minprio) / 100;

    if (pthread_setschedparam(thread, policy, &param) != 0) {
        DBG("[sfizz] Cannot set current thread scheduling parameters");
        return;
    }
#endif
}

void sfz::FilePool::setRamLoading(bool loadInRam) noexcept
{
    if (loadInRam == this->loadInRam)
        return;

    this->loadInRam = loadInRam;

    if (loadInRam) {
        for (auto& preloadedFile : preloadedFiles) {
            fs::path file { rootDirectory / preloadedFile.first.filename() };
            AudioReaderPtr reader = createAudioReader(file, preloadedFile.first.isReverse());
            auto& fileData = preloadedFile.second;
            fileData.preloadedData = std::make_shared<FileAudioBuffer>(readFromFile(
                *reader,
                fileData.information.end
            ));
            fileData.fullyLoaded = true;
        }
    } else {
        setPreloadSize(preloadSize);
    }
}

void sfz::FilePool::triggerGarbageCollection() noexcept
{
    const std::unique_lock<SpinMutex> guard { garbageAndLastUsedMutex, std::try_to_lock };
    if (!guard.owns_lock())
        return;

    const auto now = std::chrono::high_resolution_clock::now();
    swapAndPopAll(lastUsedFiles, [&](const FileId& id) {
        if (garbageToCollect.size() == garbageToCollect.capacity())
           return false;

        auto it = preloadedFiles.find(id);
        if (it == preloadedFiles.end()) {
            // Getting here means that the preloadedFiles got changed (probably cleared)
            // while the lastUsedFiles were untouched.
            return true;
        }

        sfz::FileData& data = it->second;

        if (data.readerCount != 0)
            return false;

        const auto secondsIdle = std::chrono::duration_cast<std::chrono::seconds>(now - data.lastViewerLeftAt).count();
        if (secondsIdle < config::fileClearingPeriod)
            return false;

        auto status = data.status.load();
        if (status == FileData::Status::Preloaded) {
            // do the garbage collection when availableFrames != 0
            if (data.availableFrames == 0) {
                return true;
            }
        }
        else if (status != FileData::Status::Done) {
            return false;
        }

        // do garbage collection when changing the status is success
        if (data.status.compare_exchange_strong(status, FileData::Status::GarbageCollecting)) {
            // recheck readerCount
            auto readerCount = data.readerCount.load();
            if (readerCount == 0) {
                data.availableFrames = 0;
                garbageToCollect.push_back(std::move(data.fileData));
                data.status = FileData::Status::Preloaded;
                return true;
            }
            data.status = status;
        }
        return false;
    });

    std::error_code ec;
    semGarbageBarrier.post(ec);
    ASSERT(!ec);
}
