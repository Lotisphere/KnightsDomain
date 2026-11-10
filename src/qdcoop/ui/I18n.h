// Knights' Domain — player-facing text in English (default), Chinese and Japanese.
// Keys are dotted English ids ("coop.host"). Lookup: current language -> English -> the key itself.
// Every string a player sees (F5 pages, toasts, reject reasons, default name, teleport page, ownership message) goes
// through Tr / TrF; log lines stay English and untranslated. Tables are immutable: Tr is safe on any thread.
#pragma once
#include <cstdint>
#include <format>
#include <string>

namespace qd::ui {

enum class Lang : uint8_t { En, Zh, Ja };

const char* Tr(const char* key);   // UTF-8; never null
void        SetLang(Lang lang);    // thread-safe; takes effect on the next frame
Lang        GetLang();             // thread-safe

// Tr(key) used as a std::format string ("{} joined"). A broken translation falls back to the raw text.
template <class... Args>
std::string TrF(const char* key, const Args&... args) {
    const char* fmt = Tr(key);
    try {
        return std::vformat(fmt, std::make_format_args(args...));
    } catch (const std::exception&) {
        return fmt;
    }
}

// Game thread, after config::Load(). Applies config "language"; when config.json had no language yet (first run /
// upgrade), picks zh for schinese/tchinese, ja for japanese, else en, as soon as Steam answers (retried every
// 2 s for up to 90 s), and writes it to config.json.
void InitI18n();
// Game thread: switch language now and persist it to config.json ("en" / "zh" / "ja").
void SetLangAndSave(Lang lang);

}  // namespace qd::ui
