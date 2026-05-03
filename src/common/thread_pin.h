#pragma once

#include <cstdint>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef ERROR  // Windows.h defines ERROR macro, conflicts with LogLevel::ERROR
#elif defined(__linux__)
#include <sched.h>
#include <pthread.h>
#endif

namespace lt {

// Pin the calling thread to a specific CPU core.
// Returns true on success, false on failure or unsupported platform.
// core = -1 means "don't pin" (no-op, returns true).
inline bool pin_thread_to_core(int core) {
    if (core < 0) return true;  // -1 = don't pin

#ifdef _WIN32
    DWORD_PTR mask = static_cast<DWORD_PTR>(1) << core;
    DWORD_PTR prev = SetThreadAffinityMask(GetCurrentThread(), mask);
    return prev != 0;
#elif defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#else
    (void)core;
    return false;  // unsupported platform
#endif
}

// Set the calling thread to high priority (for hot-path threads).
// Returns true on success.
inline bool set_thread_high_priority() {
#ifdef _WIN32
    return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != 0;
#elif defined(__linux__)
    // Use SCHED_FIFO with moderate priority (requires CAP_SYS_NICE or root)
    struct sched_param param;
    param.sched_priority = 50;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;
#else
    return false;
#endif
}

}  // namespace lt
