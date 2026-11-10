// QD Coop — logging (shared by every module). Thread-safe, UTF-8, file + OutputDebugString.
#pragma once
#include <cstdint>
#include <format>
#include <string>

namespace qd::log {

enum class Level : uint8_t { Debug, Info, Warn, Error };

void Init(const std::wstring& filePath);   // safe to call once; before Init, lines go to OutputDebugString only
void Shutdown();
void SetMinLevel(Level level);
void Write(Level level, const char* tag, const std::string& message);
// Optional mirror of every written line (e.g. the ~ debug console). Called outside the log lock; must be thread-safe.
void SetSink(void (*sink)(Level level, const char* tag, const std::string& message));

}  // namespace qd::log

#define QD_LOG_(lvl, tag, ...) ::qd::log::Write(::qd::log::Level::lvl, tag, std::format(__VA_ARGS__))
#if QD_DEV
#define QD_DEBUG(tag, ...) QD_LOG_(Debug, tag, __VA_ARGS__)
#else
// Release flavour: the lowest log level is info, so debug lines are compiled out (arguments not evaluated).
#define QD_DEBUG(tag, ...) ((void)0)
#endif
#define QD_INFO(tag, ...)  QD_LOG_(Info,  tag, __VA_ARGS__)
#define QD_WARN(tag, ...)  QD_LOG_(Warn,  tag, __VA_ARGS__)
#define QD_ERROR(tag, ...) QD_LOG_(Error, tag, __VA_ARGS__)
