// Host-side stand-in for the NDK's <android/log.h>, used only by the
// desktop GoogleTest build in this directory. Routes log calls to stderr.
#pragma once

#include <cstdarg>
#include <cstdio>

enum
{
    ANDROID_LOG_UNKNOWN = 0,
    ANDROID_LOG_DEFAULT,
    ANDROID_LOG_VERBOSE,
    ANDROID_LOG_DEBUG,
    ANDROID_LOG_INFO,
    ANDROID_LOG_WARN,
    ANDROID_LOG_ERROR,
    ANDROID_LOG_FATAL,
    ANDROID_LOG_SILENT,
};

inline int __android_log_vprint(int /*prio*/, const char* tag, const char* fmt, va_list ap)
{
    std::fprintf(stderr, "[%s] ", tag);
    int n = std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    return n;
}

inline int __android_log_print(int prio, const char* tag, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = __android_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return n;
}
