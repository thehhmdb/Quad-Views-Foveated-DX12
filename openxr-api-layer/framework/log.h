// MIT License
//
// Copyright(c) 2022-2023 Matthieu Bucchianeri
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this softwareand associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#pragma once

#include "pch.h"

namespace openxr_api_layer::log {

    TRACELOGGING_DECLARE_PROVIDER(g_traceProvider);

    extern TraceLoggingActivity<g_traceProvider> g_traceActivity;

    // Check if the ETW trace provider is active (i.e., a trace session is attached).
    // Safe to call even if TraceLoggingRegister hasn't been called (returns false).
    inline bool IsTraceEnabled() {
        return TraceLoggingProviderEnabled(g_traceProvider, 0, 0);
    }

#define TraceLocalActivity(activity) TraceLoggingActivity<g_traceProvider> activity;

#define TLArg(var, ...) TraceLoggingValue(var, ##__VA_ARGS__)
#define TLPArg(var, ...) TraceLoggingPointer(var, ##__VA_ARGS__)
#ifdef _M_IX86
#define TLXArg TLArg
#else
#define TLXArg TLPArg
#endif

    // Log levels - messages are logged if their level >= current log level
    enum class LogLevel {
        Verbose = 0,
        Debug = 1,
        Information = 2,
        Warning = 3,
        Error = 4,
        Fatal = 5,
    };

    // Forward declaration of Log (defined below) - needed by template functions
    void Log(const char* fmt, ...);

    // Get/set the current log level (default: Information)
    LogLevel GetLogLevel();
    void SetLogLevel(LogLevel level);

    // Parse log level from string (used by settings parser)
    // Returns true if the string is a valid log level name
    bool ParseLogLevel(const char* value);

    // Core string logging function. Takes an already-formatted message and
    // writes it directly, bypassing the legacy vsnprintf path entirely. This
    // avoids the double-formatting overhead (fmt::format -> c_str -> vsnprintf)
    // that the templated functions previously incurred.
    void LogString(LogLevel level, std::string_view msg);

    // Flush the buffered log stream to disk immediately. Call from error paths
    // that may crash or abort before the atexit handler runs.
    void Flush();

    // Enable/disable flushing after every logged line. Much slower, but
    // guarantees no log tail is lost when the host application crashes.
    // Intended for diagnostics (e.g. bypass mode).
    void SetFlushPerLine(bool enable);

    // Level-gated logging functions.
    // The format string is only evaluated (via fmt::format) if the level is enabled,
    // avoiding allocation overhead for filtered messages.
    template<typename... Args>
    inline void LogVerbose(const char* fmt, const Args&... args) {
        if (GetLogLevel() <= LogLevel::Verbose) {
            LogString(LogLevel::Verbose, fmt::format(fmt, args...));
        }
    }

    template<typename... Args>
    inline void LogDebug(const char* fmt, const Args&... args) {
        if (GetLogLevel() <= LogLevel::Debug) {
            LogString(LogLevel::Debug, fmt::format(fmt, args...));
        }
    }

    template<typename... Args>
    inline void LogInformation(const char* fmt, const Args&... args) {
        if (GetLogLevel() <= LogLevel::Information) {
            LogString(LogLevel::Information, fmt::format(fmt, args...));
        }
    }

    template<typename... Args>
    inline void LogWarning(const char* fmt, const Args&... args) {
        if (GetLogLevel() <= LogLevel::Warning) {
            LogString(LogLevel::Warning, fmt::format(fmt, args...));
        }
    }

    template<typename... Args>
    inline void LogError(const char* fmt, const Args&... args) {
        if (GetLogLevel() <= LogLevel::Error) {
            LogString(LogLevel::Error, fmt::format(fmt, args...));
        }
    }

    template<typename... Args>
    inline void LogFatal(const char* fmt, const Args&... args) {
        if (GetLogLevel() <= LogLevel::Fatal) {
            LogString(LogLevel::Fatal, fmt::format(fmt, args...));
        }
    }

    // Legacy logging functions (kept for backward compatibility).
    // Log() maps to Information level.
    void Log(const char* fmt, ...);
    static inline void Log(const std::string_view& str) {
        LogString(LogLevel::Information, str);
    }

    // DebugLog() - compile-gated to _DEBUG builds, maps to Debug level.
    void DebugLog(const char* fmt, ...);
    static inline void DebugLog(const std::string_view& str) {
        LogString(LogLevel::Debug, str);
    }

    // ErrorLog() - rate-limited, maps to Error level.
    void ErrorLog(const char* fmt, ...);
    static inline void ErrorLog(const std::string_view& str) {
        LogString(LogLevel::Error, str);
    }

} // namespace openxr_api_layer::log

// Safe trace macros: These prevent argument evaluation and ETW API calls
// when no trace session is active. This is critical for unit testing and
// production performance. TraceLoggingProviderEnabled is safe to call even
// if TraceLoggingRegister hasn't been called (returns false in that case).

#define QVF_TRACE(event_name, ...) \
    do { \
        if (::openxr_api_layer::log::IsTraceEnabled()) { \
            TraceLoggingWrite(::openxr_api_layer::log::g_traceProvider, event_name, __VA_ARGS__); \
        } \
    } while (0)

#define QVF_TRACE_START(activity, event_name, ...) \
    do { \
        if (::openxr_api_layer::log::IsTraceEnabled()) { \
            TraceLoggingWriteStart(activity, event_name, __VA_ARGS__); \
        } \
    } while (0)

#define QVF_TRACE_STOP(activity, event_name, ...) \
    do { \
        if (::openxr_api_layer::log::IsTraceEnabled()) { \
            TraceLoggingWriteStop(activity, event_name, __VA_ARGS__); \
        } \
    } while (0)

#define QVF_TRACE_TAGGED(activity, event_name, ...) \
    do { \
        if (::openxr_api_layer::log::IsTraceEnabled()) { \
            TraceLoggingWriteTagged(activity, event_name, __VA_ARGS__); \
        } \
    } while (0)
