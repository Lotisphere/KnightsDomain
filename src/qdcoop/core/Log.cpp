#include "qdcoop/core/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <mutex>
#include <share.h>

namespace qd::log {
namespace {
std::mutex g_mutex;
FILE*      g_file = nullptr;
#if QD_DEV
Level      g_min = Level::Debug;
#else
Level      g_min = Level::Info;    // release: never below info
#endif
std::atomic<void (*)(Level, const char*, const std::string&)> g_sink{nullptr};

const char* LevelName(Level l) {
    switch (l) {
        case Level::Debug: return "DBG";
        case Level::Info:  return "INF";
        case Level::Warn:  return "WRN";
        default:           return "ERR";
    }
}
}  // namespace

void Init(const std::wstring& filePath) {
    std::lock_guard lock(g_mutex);
    if (g_file) return;
    // Binary mode: lines are already UTF-8 bytes. (A "ccs=UTF-8" stream only accepts wide writes; fwrite of
    // narrow bytes into it trips the CRT invalid-parameter handler -> 0xC0000409 fast-fail.)
    // Share for reading so the log can be tailed while the game runs.
    {   // Keep the previous five sessions: X.log -> X.prev1.log -> ... -> X.prev5.log (a new launch used to erase the
        // log of the session a player wanted to report).
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path cur(filePath);
        const auto prev = [&cur](int i) {
            fs::path p = cur;
            p.replace_extension(L".prev" + std::to_wstring(i) + cur.extension().wstring());
            return p;
        };
        fs::remove(prev(5), ec);
        for (int i = 4; i >= 1; --i) fs::rename(prev(i), prev(i + 1), ec);
        if (fs::exists(cur, ec) && fs::file_size(cur, ec) > 0) fs::rename(cur, prev(1), ec);
    }
    g_file = _wfsopen(filePath.c_str(), L"wb", _SH_DENYWR);
}

void Shutdown() {
    std::lock_guard lock(g_mutex);
    if (g_file) { fclose(g_file); g_file = nullptr; }
}

void SetMinLevel(Level level) {
#if !QD_DEV
    if (level < Level::Info) level = Level::Info;
#endif
    g_min = level;
}

void SetSink(void (*sink)(Level, const char*, const std::string&)) { g_sink = sink; }

void Write(Level level, const char* tag, const std::string& message) {
    if (level < g_min) return;
    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char head[64];
    std::snprintf(head, sizeof(head), "[%02d:%02d:%02d.%03lld] [%s] [%s] ", tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<long long>(ms), LevelName(level), tag ? tag : "-");
    std::string line = std::string(head) + message + "\n";

    {
        std::lock_guard lock(g_mutex);
        OutputDebugStringA(line.c_str());
        if (g_file) {
            fwrite(line.data(), 1, line.size(), g_file);
            fflush(g_file);
        }
    }
    if (auto sink = g_sink.load()) sink(level, tag, message);
}

}  // namespace qd::log
