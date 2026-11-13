// SPDX-License-Identifier: BSD-2-Clause

// This code is part of the sfizz library and is licensed under a BSD 2-clause
// license. You should have receive a LICENSE.md file along with the code.
// If not, contact the sfizz maintainers at https://github.com/sfztools/sfizz

#pragma once
#include <functional>
#include <memory>
#include <thread>
#if !defined(_WIN32)
#include <pthread.h>
#endif

namespace sfz {

/**
 * @brief A thread with a small stack. A process that locks its memory (mlockall) holds every
 * thread's whole stack resident: 8 MiB each at glibc's default, against what sfizz's background
 * threads use, a few kilobytes.
 */
class SmallStackThread {
public:
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    static constexpr size_t stackBytes = 4 << 20; // sanitizers' frames are much larger
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    static constexpr size_t stackBytes = 4 << 20;
#else
    static constexpr size_t stackBytes = 512 << 10;
#endif
#else
    static constexpr size_t stackBytes = 512 << 10;
#endif

    SmallStackThread() = default;
    SmallStackThread(const SmallStackThread&) = delete;
    SmallStackThread& operator=(const SmallStackThread&) = delete;
    ~SmallStackThread() { join(); }

    void start(std::function<void()> function)
    {
#if defined(_WIN32)
        thread_ = std::thread(std::move(function));
#else
        auto* f = new std::function<void()>(std::move(function));
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, stackBytes);
        started_ = pthread_create(&thread_, &attr, &run, f) == 0;
        pthread_attr_destroy(&attr);
        if (!started_)
            delete f;
#endif
    }

    void join()
    {
#if defined(_WIN32)
        if (thread_.joinable())
            thread_.join();
#else
        if (started_)
            pthread_join(thread_, nullptr);
        started_ = false;
#endif
    }

private:
#if defined(_WIN32)
    std::thread thread_;
#else
    static void* run(void* p)
    {
        std::unique_ptr<std::function<void()>> f { static_cast<std::function<void()>*>(p) };
        (*f)();
        return nullptr;
    }
    pthread_t thread_ {};
    bool started_ { false };
#endif
};

} // namespace sfz
