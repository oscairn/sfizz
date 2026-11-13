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

#pragma once
#include "Config.h"
#include "Defaults.h"
#include "RTSemaphore.h"
#include "AudioBuffer.h"
#include "AudioSpan.h"
#include "FileId.h"
#include "FileMetadata.h"
#include "SIMDHelpers.h"
#include "SpinMutex.h"
#include "utility/Timing.h"
#include "utility/LeakDetector.h"
#include "utility/MemoryHelpers.h"
#include <ghc/fs_std.hpp>
#include <absl/container/flat_hash_map.h>
#include <absl/types/optional.h>
#include <absl/strings/string_view.h>
#include <atomic_queue/atomic_queue.h>
#include <chrono>
#include <thread>
#include <future>
#include <mutex>
#include <memory>
#include <set>
#include "utility/SmallStackThread.h"

namespace sfz {
using FileAudioBuffer = AudioBuffer<float, 2, config::defaultAlignment,
                                    sfz::config::excessFileFrames, sfz::config::excessFileFrames>;
using FileAudioBufferPtr = std::shared_ptr<FileAudioBuffer>;

struct FileInformation {
    int64_t end { Default::sampleEnd };
    int64_t maxOffset { 0 };
    int64_t loopStart { Default::loopStart };
    int64_t loopEnd { Default::loopEnd };
    bool hasLoop { false };
    double sampleRate { config::defaultSampleRate };
    int numChannels { 0 };
    int rootKey { 0 };
    absl::optional<WavetableInfo> wavetable;
};

class AudioReader;

/**
 * @brief The streamed part of a file longer than its preload: decoded in chunks of
 * config::streamChunkFrames, only near where voices play (FilePool::want), into a cache whose
 * total size, for the whole process, is capped (FilePool::setStreamCacheCapacity). A chunk no
 * voice has wanted for a while is freed again.
 */
struct StreamChunk {
    std::atomic<int> readers { 0 }; // audio threads copying from it now
    std::unique_ptr<float[]> data; // planar: channels x config::streamChunkFrames
};
struct StreamSlot {
    std::atomic<StreamChunk*> chunk { nullptr }; // published once decoded
    std::atomic<int64_t> wantedAt { 0 }; // steady clock, ms
    std::atomic<int> state { 0 }; // 0 idle, 1 requested, 2 decoding
};
struct StreamState {
    StreamState(fs::path path, uint32_t frames, unsigned channels);
    ~StreamState();
    fs::path path;
    uint32_t frames;
    unsigned channels;
    size_t numSlots;
    std::unique_ptr<StreamSlot[]> slots;
    // the loader's (or a freewheeling render's) decoder, read on from where it stopped
    std::mutex decodeMutex;
    std::unique_ptr<AudioReader> reader;
    uint64_t readerPosition { 0 };
    std::vector<float> decodeBuffer;
    std::vector<StreamChunk*> retired; // unpublished, freed once nobody copies from them
};

// Strict C++11 disallows member initialization if aggregate initialization is to be used...
struct FileData
{
    enum class Status { Invalid, Preloaded, Streaming, Done, GarbageCollecting };
    FileData() = default;
    FileData(std::shared_ptr<FileAudioBuffer> preloaded, FileInformation info)
    : preloadedData(std::move(preloaded)), information(std::move(info))
    {

    }
    AudioSpan<const float> getData()
    {
        ASSERT(readerCount > 0);
        if (status != Status::GarbageCollecting && availableFrames > preloadedData->getNumFrames())
            return AudioSpan<const float>(fileData).first(availableFrames);
        else
            return AudioSpan<const float>(*preloadedData);
    }

    FileData(const FileData& other) = delete;
    FileData& operator=(const FileData& other) = delete;
    FileData(FileData&& other)
    {
        ASSERT(other.readerCount == 0); // Probably should not be moving this...
        information = std::move(other.information);
        preloadedData = std::move(other.preloadedData);
        fileData = std::move(other.fileData);
        stream = std::move(other.stream);
        preloadCallCount = other.preloadCallCount;
        availableFrames = other.availableFrames.load();
        lastViewerLeftAt = other.lastViewerLeftAt;
        status = other.status.load();
    }
    FileData& operator=(FileData&& other)
    {
        ASSERT(other.readerCount == 0); // Probably should not be moving this...
        information = std::move(other.information);
        preloadedData = std::move(other.preloadedData);
        fileData = std::move(other.fileData);
        stream = std::move(other.stream);
        preloadCallCount = other.preloadCallCount;
        availableFrames = other.availableFrames.load();
        lastViewerLeftAt = other.lastViewerLeftAt;
        status = other.status.load();
        return *this;
    }

    // never written once loaded, so file pools may share it (SharedPreloads)
    std::shared_ptr<FileAudioBuffer> preloadedData { std::make_shared<FileAudioBuffer>() };
    FileInformation information;
    FileAudioBuffer fileData {};
    // set when the rest of the file streams in chunks; fileData then stays empty
    std::unique_ptr<StreamState> stream;
    int preloadCallCount { 0 };
    std::atomic<Status> status { Status::Invalid };
    bool fullyLoaded { false };
    std::atomic<size_t> availableFrames { 0 };
    std::atomic<int> readerCount { 0 };
    std::chrono::time_point<std::chrono::high_resolution_clock> lastViewerLeftAt;

    LEAK_DETECTOR(FileData);
};


class FileDataHolder {
public:
    FileDataHolder() = default;
    FileDataHolder(const FileDataHolder&) = delete;
    FileDataHolder& operator=(const FileDataHolder&) = delete;
    FileDataHolder(FileDataHolder&& other)
    {
        this->data = other.data;
        other.data = nullptr;
    }
    FileDataHolder& operator=(FileDataHolder&& other)
    {
        this->data = other.data;
        other.data = nullptr;
        return *this;
    }
    FileDataHolder(FileData* data) : data(data)
    {
        if (!data)
            return;

        data->readerCount += 1;
    }
    void reset()
    {
        if (!data)
            return;

        data->readerCount -= 1;
        data->lastViewerLeftAt = highResNow();
        data = nullptr;
    }
    ~FileDataHolder()
    {
        ASSERT(!data || data->readerCount > 0);
        reset();
    }
    FileData& operator*() { return *data; }
    FileData* operator->() { return data; }
    explicit operator bool() const { return data != nullptr; }
private:
    FileData* data { nullptr };
    LEAK_DETECTOR(FileDataHolder);
};

/**
 * @brief This is a singleton-designed class that holds all the preloaded data
 * as well as functions to request new file data and collect the file handles to
 * close after they are read.
 *
 * This object caches the file data that was already preloaded in case it is
 * asked again by a region using the same sample. In this situation, both
 * regions have a handle on the same preloaded data.
 *
 * The file request is immediately served using the preloaded data. A promise is
 * then provided to the voice that requested the file, and the file loading
 * happens in the background. File reads happen on whole samples but
 * oversampling is done in chunks, and the promise contains a counter for the
 * frames that are loaded. When the voice dies it releases its handle on the
 * promise, which should decrease the  reference count to 1. A garbage
 * collection thread then runs regularly to clear the memory of all file handles
 * with a reference count of 1.
 */


/**
 * @brief Preloaded file data that several file pools (synths) share: a file another pool has
 * already read is used from memory instead of read again. Entries hold the data weakly, so it
 * lives only while a pool uses it.
 */
struct SharedPreloads {
    struct Entry {
        std::weak_ptr<FileAudioBuffer> data;
        FileInformation information;
        uint32_t frames { 0 }; // the file's length
    };
    std::mutex mutex;
    absl::flat_hash_map<std::string, Entry> entries; // by path, "|r" appended when reversed
};

class FilePool {
public:
    /**
     * @brief Construct a new File Pool object.
     *
     * This creates the background threads based on config::numBackgroundThreads
     * as well as the garbage collection thread.
     */
    FilePool();

    ~FilePool();
    /**
     * @brief Set the root directory from which to search for files to load
     *
     * @param directory
     */
    void setRootDirectory(const fs::path& directory) noexcept { rootDirectory = directory; }
    /**
     * @brief Get the number of preloaded sample files
     *
     * @return size_t
     */
    size_t getNumPreloadedSamples() const noexcept { return preloadedFiles.size() + loadedFiles.size(); }
    /**
     * @brief Share preloaded data with other pools (null: none, the default). Set it before
     * loading; it applies to files preloaded afterwards.
     */
    void setSharedPreloads(std::shared_ptr<SharedPreloads> shared) noexcept { sharedPreloads = std::move(shared); }
    /**
     * @brief How many times this pool opened a sample file to preload it or read its information
     */
    size_t getNumFileOpens() const noexcept { return fileOpens; }

    /**
     * @brief Get metadata information about a file.
     *
     * @param fileId
     * @return absl::optional<FileInformation>
     */
    absl::optional<FileInformation> getFileInformation(const FileId& fileId) noexcept;

    /**
     * @brief Preload a file with the proper offset bounds
     *
     * @param fileId
     * @param maxOffset the maximum offset to consider for preloading. The total preloaded
     *                  size will be preloadSize + offset
     * @return true if the preloading went fine
     * @return false if something went wrong ()
     */
    bool preloadFile(const FileId& fileId, uint32_t maxOffset) noexcept;

    /**
     * @brief Load a file and return its information. The file pool will store this
     * data for future requests so use this function responsibly.
     *
     * @param fileId
     * @return A handle on the file data
     */
    FileDataHolder loadFile(const FileId& fileId) noexcept;

    /**
     * @brief Load a file from RAM and return its information. The file pool will store\
     * this data for future requests so use this function responsibly.
     *
     * @param fileId
     * @param data
     * @return A handle on the file data
     */
    FileDataHolder loadFromRam(const FileId& fileId, const std::vector<char>& data) noexcept;

    /**
     * @brief Check that the sample exists. If not, try to find it in a case insensitive way.
     *
     * @param filename the sample filename; may be updated by the method
     * @return true if the sample exists or was updated properly
     * @return false if no sample was found even with a case insensitive search
     */
    bool checkSample(std::string& filename) const noexcept;

    /**
     * @brief Check that the sample exists. If not, try to find it in a case insensitive way.
     *
     * @param fileId the sample file identifier; may be updated by the method
     * @return true if the sample exists or was updated properly
     * @return false if no sample was found even with a case insensitive search
     */
    bool checkSampleId(FileId& fileId) const noexcept;

    /**
     * @brief Clear all preloaded files.
     *
     */
    void clear();

    /**
     * @brief Reset the number of preloadFile counts for each sample.
     */
    void resetPreloadCallCounts() noexcept;

    /**
     * @brief Clear all files with a preload call count of 0.
     */
    void removeUnusedPreloadedData() noexcept;

    /**
     * @brief Get a handle on a file, which triggers background loading
     *
     * @param fileId the file to preload
     * @return FileDataHolder a file data handle
     */
    FileDataHolder getFilePromise(const std::shared_ptr<FileId>& fileId) noexcept;
    /**
     * @brief Change the preloading size. This will trigger a full
     * reload of all samples, so don't call it on the audio thread.
     *
     * @param preloadSize
     */
    void setPreloadSize(uint32_t preloadSize) noexcept;
    /**
     * @brief Get the current preload size.
     *
     * @return uint32_t
     */
    uint32_t getPreloadSize() const noexcept;
    /**
     * @brief Empty the file loading queues without actually loading
     * the files. All promises will be unfulfilled. Don't call this
     * method on the audio thread as it will spinlock.
     *
     */
    void emptyFileLoadingQueues() noexcept
    {
        // nothing to do in this implementation,
        // deleting the region and its sample ID take care of it
    }
    /**
     * @brief Wait for the background loading to finish for all promises
     * in the queue.
     */
    void waitForBackgroundLoading() noexcept;
    /**
     * @brief Ask for the chunks of a streamed file covering frames first to last (audio
     * thread). Frames in the preload, and files that do not stream in chunks, need nothing.
     */
    void want(FileData& data, int64_t first, int64_t last) noexcept;
    /**
     * @brief Copy frames first to first + count - 1 of a file streamed in chunks into this
     * pool's gather buffer and return them (audio thread; count at most getGatherFrames()).
     * Frames outside the file read as zeros, as a fully loaded file's padding does; a chunk not
     * decoded yet reads as zeros too, unless `wait` (a freewheeling render), which decodes it.
     */
    AudioSpan<const float> gather(FileData& data, int64_t first, size_t count, bool wait) noexcept;
    static constexpr size_t getGatherFrames() noexcept { return 2 * config::streamChunkFrames; }
    /**
     * @brief The most memory the chunks of streamed files may take, for the whole process
     * (config::streamCacheBytes by default), and what they take now.
     */
    static void setStreamCacheCapacity(size_t bytes) noexcept;
    static size_t getStreamCacheCapacity() noexcept;
    static size_t getStreamCacheUsage() noexcept;
    /**
     * @brief Bytes this pool holds in preloaded and fully loaded sample data (not streamed
     * chunks). Buffers already in `counted` (shared with another pool) are skipped, and the
     * others added to it.
     */
    size_t getPreloadedBytes(std::set<const void*>& counted) const noexcept;
    static size_t getFixedBytes() noexcept;
    /**
     * @brief Assign the current thread a priority which is appropriate
     * for background sample file processing.
     */
    static void raiseCurrentThreadPriority() noexcept;
    /**
     * @brief Change whether all samples are loaded in ram.
     * This will trigger a purge and reloading.
     *
     * @param loadInRam
     */
    void setRamLoading(bool loadInRam) noexcept;
    /**
     * @brief Prepares unused data to be freed on a background thread.
     * This should be called regularly by the Synth, otherwise memory
     * risk building up.
     */
    void triggerGarbageCollection() noexcept;
private:

    absl::optional<sfz::FileInformation> checkExistingFileInformation(const FileId& fileId) noexcept;
    fs::path rootDirectory;

    bool loadInRam { config::loadInRam };
    uint32_t preloadSize { config::preloadSize };

    // Signals
    std::atomic<bool> dispatchFlag { true };
    std::atomic<bool> garbageFlag { true };
    RTSemaphore dispatchBarrier;
    RTSemaphore semGarbageBarrier;

    // Structures for the background loaders
    struct QueuedFileData
    {
        QueuedFileData() noexcept {}
        QueuedFileData(std::weak_ptr<FileId> id, FileData* data) noexcept
        : id(id), data(data) {}
        std::weak_ptr<FileId> id;
        FileData* data { nullptr };
    };

    using FileQueue = atomic_queue::AtomicQueue2<QueuedFileData, config::maxVoices>;
    aligned_unique_ptr<FileQueue> filesToLoad;

    // Requests for chunks of streamed files, from the audio thread
    struct ChunkRequest {
        StreamState* stream { nullptr };
        uint32_t slot { 0 };
        uint32_t generation { 0 };
    };
    using ChunkQueue = atomic_queue::AtomicQueue2<ChunkRequest, 4 * config::maxVoices>;
    aligned_unique_ptr<ChunkQueue> chunksToLoad;
    // Changed with the files, under filesMutex: a request from before belongs to no file now
    std::atomic<uint32_t> generation { 0 };
    // Held by the loader while it works on the files, and by changes to the set of files
    mutable std::mutex filesMutex;
    void decodeChunk(StreamState& stream, size_t slot) noexcept; // with decodeMutex held
    void loadRequestedChunk(StreamState& stream, size_t slot) noexcept;
    void loadChunkNow(StreamState& stream, size_t slot) noexcept;
    void freeUnwantedChunks(int64_t wantedBefore) noexcept; // with filesMutex held
    void makeStream(const FileId& fileId, FileData& data, uint32_t frames);

    void dispatchingJob() noexcept;
    void garbageJob() noexcept;
    void loadingJob(const QueuedFileData& data) noexcept;
    std::mutex loadingMutex; // held while a whole file loads
    FileAudioBuffer gatherBuffer;
    // Started at the end of the constructor, once every member they use exists
    SmallStackThread dispatchThread;
    SmallStackThread garbageThread;

    SpinMutex garbageAndLastUsedMutex;
    std::vector<FileId> lastUsedFiles;
    std::vector<FileAudioBuffer> garbageToCollect;

    std::shared_ptr<SharedPreloads> sharedPreloads;
    size_t fileOpens { 0 };
    std::string sharedKey(const FileId& fileId) const;

    // Preloaded data
    absl::flat_hash_map<FileId, FileData> preloadedFiles;
    absl::flat_hash_map<FileId, FileData> loadedFiles;
    LEAK_DETECTOR(FilePool);
};
}
