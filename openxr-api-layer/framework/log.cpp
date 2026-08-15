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

#include "pch.h"
#include "log.h"

#include <cstdlib>

namespace openxr_api_layer::log {
    extern std::ofstream logStream;
}

namespace {
    constexpr uint32_t k_maxLoggedErrors = 100;
    uint32_t g_globalErrorCount = 0;

    // Mutex to serialize log writes from multiple threads. Without this,
    // concurrent LogString calls interleave their output, producing
    // half-written lines.
    std::mutex g_logMutex;

    // Flush the log stream on process exit. Without this, the OS buffer is
    // lost when the process crashes or is killed, and the last few lines
    // are never written to disk.
    void FlushLogOnExit() {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (openxr_api_layer::log::logStream.is_open()) {
            openxr_api_layer::log::logStream.flush();
        }
    }

    // Register the exit handler once. On normal process termination, flush
    // the log stream so the buffered tail of the log is not lost.
    struct LogExitHandler {
        LogExitHandler() {
            std::atexit(FlushLogOnExit);
        }
    };
    LogExitHandler g_logExitHandler;
} // namespace

namespace openxr_api_layer::log {

    // {cbf3adcd-42b1-4c38-830c-91980af201f8}
    TRACELOGGING_DEFINE_PROVIDER(g_traceProvider,
                                 "QuadViewsFoveated",
                                 (0xcbf3adcd, 0x42b1, 0x4c38, 0x83, 0x0c, 0x98, 0x98, 0x0a, 0xf2, 0x01, 0xf8));

    TraceLoggingActivity<g_traceProvider> g_traceActivity;

    // Global log level (default: Information)
    LogLevel g_logLevel = LogLevel::Information;

    LogLevel GetLogLevel() {
        return g_logLevel;
    }

    void SetLogLevel(LogLevel level) {
        g_logLevel = level;
    }

    bool ParseLogLevel(const char* value) {
        if (!value) return false;
        // Case-insensitive comparison
        std::string lower(value);
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower == "verbose") { g_logLevel = LogLevel::Verbose; return true; }
        if (lower == "debug") { g_logLevel = LogLevel::Debug; return true; }
        if (lower == "information" || lower == "info") { g_logLevel = LogLevel::Information; return true; }
        if (lower == "warning" || lower == "warn") { g_logLevel = LogLevel::Warning; return true; }
        if (lower == "error") { g_logLevel = LogLevel::Error; return true; }
        if (lower == "fatal") { g_logLevel = LogLevel::Fatal; return true; }
        return false;
    }

    // Core string logging function. Writes an already-formatted message with a
    // timestamp and level prefix. Performance notes:
    //  - localtime_s is thread-safe (std::localtime takes a CRT lock).
    //  - OutputDebugStringA is only called when a debugger is attached; it is a
    //    slow OS call that would otherwise run for every message.
    //  - The file stream is NOT flushed per-message; the OS buffers writes and
    //    flushes periodically or on close. Per-message flush() was the single
    //    largest logging cost (a synchronous disk write per line).
    void LogString(LogLevel level, std::string_view msg) {
        const std::time_t now = std::time(nullptr);
        struct tm timeinfo;
        localtime_s(&timeinfo, &now);

        char timeBuf[64];
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S %z ", &timeinfo);

        const char* levelStr = "";
        switch (level) {
            case LogLevel::Verbose:     levelStr = "[V] "; break;
            case LogLevel::Debug:       levelStr = "[D] "; break;
            case LogLevel::Information: levelStr = "[I] "; break;
            case LogLevel::Warning:     levelStr = "[W] "; break;
            case LogLevel::Error:       levelStr = "[E] "; break;
            case LogLevel::Fatal:       levelStr = "[F] "; break;
            default: break;
        }

        if (IsDebuggerPresent()) {
            // msg is a string_view with no null-termination guarantee, so copy
            // into a std::string before calling the C-string OS API.
            const std::string line = std::string(timeBuf) + levelStr + std::string(msg);
            OutputDebugStringA(line.c_str());
        }

        if (logStream.is_open()) {
            std::lock_guard<std::mutex> lock(g_logMutex);
            // Callers embed their own trailing newline in the message.
            logStream << timeBuf << levelStr << msg;
            // Guarantee newline termination: if the caller forgot the trailing
            // newline, or the message was truncated by the 2048-byte buffer in
            // Log()/ErrorLog()/DebugLog(), the next line would otherwise start
            // on the same physical line.
            if (msg.empty() || msg.back() != '\n') {
                logStream << '\n';
            }
        }
    }

    void Log(const char* fmt, ...) {
        va_list va;
        va_start(va, fmt);
        char buf[2048];
        vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, va);
        va_end(va);
        LogString(LogLevel::Information, buf);
    }

    void ErrorLog(const char* fmt, ...) {
        if (g_globalErrorCount++ < k_maxLoggedErrors) {
            va_list va;
            va_start(va, fmt);
            char buf[2048];
            vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, va);
            va_end(va);
            LogString(LogLevel::Error, buf);
            if (g_globalErrorCount == k_maxLoggedErrors) {
                LogString(LogLevel::Information, "Maximum number of errors logged. Going silent.");
            }
        }
    }

    void DebugLog(const char* fmt, ...) {
#ifdef _DEBUG
        va_list va;
        va_start(va, fmt);
        char buf[2048];
        vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, va);
        va_end(va);
        LogString(LogLevel::Debug, buf);
#endif
    }

} // namespace openxr_api_layer::log
