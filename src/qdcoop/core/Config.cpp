#include "qdcoop/core/Config.h"

#include "qdcoop/core/Log.h"
#include "qdcoop/core/Util.h"

#include <Windows.h>
#include <shellapi.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include "nlohmann/json.hpp"

#pragma comment(lib, "shell32.lib")

namespace qd::config {
namespace {
constexpr char kTag[] = "config";

Config g_config;        // effective values (file + dev command-line overrides)
Config g_file;          // what config.json holds (defaults for missing keys); never contains command-line overrides
bool   g_fileUsable = true;   // false: config.json exists but could not be parsed -> never rewrite it

std::wstring ConfigPath() { return paths::ModDir() + L"\\config.json"; }

nlohmann::json ToJson(const Config& c) {
    nlohmann::json j = nlohmann::json::object();
#if QD_DEV
    j["transport"] = c.transport;
    j["udp_port"] = c.udpPort;
    j["udp_host_ip"] = c.udpHostIp;
    j["profile"] = c.profile;
#endif
    j["menu_key"] = c.menuKey;
    j["log_level"] = c.logLevel;
    j["language"] = c.language;
    j["hp_scale"] = {{"normal", c.hpScale.normal}, {"elite", c.hpScale.elite}, {"boss", c.hpScale.boss}};
    return j;
}

// Reads `key` from `obj` into `out` if present and of the right type.
// missing -> true when the key is absent; typeError -> true when present but unusable (value left at default).
template <class T>
void ReadKey(const nlohmann::json& obj, const char* key, T& out, bool& missing, bool& typeError) {
    auto it = obj.find(key);
    if (it == obj.end()) {
        missing = true;
        return;
    }
    try {
        out = it->template get<T>();
    } catch (const std::exception& e) {
        typeError = true;
        QD_WARN(kTag, "config.json: key '{}' has the wrong type ({}); keeping default", key, e.what());
    }
}

bool IsKnownLanguage(const std::string& l) { return l == "en" || l == "zh" || l == "ja"; }

void FromJson(const nlohmann::json& j, Config& c, bool& missing, bool& typeError) {
#if QD_DEV
    ReadKey(j, "transport", c.transport, missing, typeError);
    ReadKey(j, "udp_port", c.udpPort, missing, typeError);
    ReadKey(j, "udp_host_ip", c.udpHostIp, missing, typeError);
    ReadKey(j, "profile", c.profile, missing, typeError);
#else
    // Release: transport / udp_* / profile are dev keys; a file that still has them is accepted, they are ignored.
    for (const char* devKey : {"transport", "udp_port", "udp_host_ip", "profile"})
        if (j.contains(devKey)) QD_INFO(kTag, "config.json: dev-only key '{}' ignored in the release build", devKey);
#endif
    ReadKey(j, "menu_key", c.menuKey, missing, typeError);
    ReadKey(j, "log_level", c.logLevel, missing, typeError);
    {
        bool        langMissing = false, langTypeError = false;
        std::string lang = c.language;
        ReadKey(j, "language", lang, langMissing, langTypeError);
        if (langMissing) {
            missing = true;   // upgrade the file; the UI language is detected once (ui::InitI18n)
        } else if (langTypeError) {
            typeError = true;
        } else if (IsKnownLanguage(lang)) {
            c.language = lang;
            c.languageFromFile = true;
        } else {
            QD_WARN(kTag, "config.json: unknown language '{}' (use \"en\" or \"zh\"); keeping '{}'", lang, c.language);
        }
    }
    auto hp = j.find("hp_scale");
    if (hp == j.end()) {
        missing = true;
    } else if (!hp->is_object()) {
        typeError = true;
        QD_WARN(kTag, "config.json: 'hp_scale' is not an object; keeping defaults");
    } else {
        ReadKey(*hp, "normal", c.hpScale.normal, missing, typeError);
        ReadKey(*hp, "elite", c.hpScale.elite, missing, typeError);
        ReadKey(*hp, "boss", c.hpScale.boss, missing, typeError);
    }
}

void Sanitize(Config& c) {
#if QD_DEV
    if (c.transport != "steam" && c.transport != "udp") {
        QD_WARN(kTag, "unknown transport '{}', using 'steam'", c.transport);
        c.transport = "steam";
    }
    if (c.udpPort < 1 || c.udpPort > 65535) {
        QD_WARN(kTag, "udp_port {} out of range, using 27777", c.udpPort);
        c.udpPort = 27777;
    }
    if (c.profile < 1) c.profile = 1;
    if (c.profile > 4) c.profile = 4;
#else
    // Release: Steam only, one profile.
    c.transport = "steam";
    c.profile = 1;
#endif
    if (!IsKnownLanguage(c.language)) c.language = "en";
}

#if QD_DEV
// -qdcoop-transport=udp  -qdcoop-profile=2  -qdcoop-udp-port=27777  -qdcoop-host=1.2.3.4   (dev flavour only)
void ApplyCommandLine(Config& c) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = util::WideToUtf8(argv[i]);
        auto value = [&](std::string_view prefix, std::string& out) {
            if (arg.size() > prefix.size() && _strnicmp(arg.c_str(), prefix.data(), prefix.size()) == 0) {
                out = arg.substr(prefix.size());
                return true;
            }
            return false;
        };
        std::string v;
        try {
            if (value("-qdcoop-transport=", v)) {
                c.transport = v;
            } else if (value("-qdcoop-profile=", v)) {
                c.profile = std::stoi(v);
            } else if (value("-qdcoop-udp-port=", v)) {
                c.udpPort = std::stoi(v);
            } else if (value("-qdcoop-host=", v)) {
                c.udpHostIp = v;
            }
        } catch (const std::exception&) {
            QD_WARN(kTag, "ignoring malformed command-line argument '{}'", arg);
        }
    }
    LocalFree(argv);
}
#endif

bool WriteFileAtomic(const std::wstring& path, const std::string& text) {
    const std::wstring tmp = path + L".tmp";
    {
        std::ofstream f(std::filesystem::path(tmp), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f) return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool CreateDirectoriesW(const std::wstring& dir) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dir), ec);
    return !ec || std::filesystem::is_directory(std::filesystem::path(dir), ec);
}
}  // namespace

bool Load() {
    Config c;  // defaults
    bool ok = true;
    const std::wstring path = ConfigPath();
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(path), ec)) {
        // First run: write the defaults (before command-line overrides, so the file holds plain defaults).
        // languageFromFile stays false: the UI language is detected once from the Steam game language.
        g_config = c;
        if (!Save()) QD_WARN(kTag, "could not write default config.json");
    } else {
        std::ifstream f(std::filesystem::path(path), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        nlohmann::json j = nlohmann::json::parse(ss.str(), nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
        if (j.is_discarded() || !j.is_object()) {
            // Parse failure: defaults, leave the user's file untouched.
            QD_WARN(kTag, "config.json could not be parsed; using defaults (file left untouched)");
            ok = false;
            g_fileUsable = false;
            c.languageFromFile = true;   // no first-run language detection over a broken file (it could not be saved)
        } else {
            bool missing = false, typeError = false;
            FromJson(j, c, missing, typeError);
            g_config = c;
            if (missing && !typeError) {
                // Upgrade the file with any keys it lacks; never rewrite when a value was unusable.
                if (!Save()) QD_WARN(kTag, "could not rewrite config.json with missing keys");
            }
        }
    }
    Sanitize(c);
    g_file = c;   // on-disk view: no command-line overrides
#if QD_DEV
    ApplyCommandLine(c);
    Sanitize(c);
#endif
    g_config = c;
    return ok;
}

const Config& Get() { return g_config; }
Config& Mutable() { return g_config; }

bool Save() {
    const std::wstring dir = paths::ModDir();
    if (dir.empty() || !g_fileUsable) return false;
    return WriteFileAtomic(ConfigPath(), ToJson(g_config).dump(2) + "\n");
}

bool SetLanguage(const std::string& lang) {
    const std::string l = IsKnownLanguage(lang) ? lang : std::string("en");
    g_config.language = l;
    g_config.languageFromFile = true;
    g_file.language = l;
    g_file.languageFromFile = true;
    if (paths::ModDir().empty()) return false;
    if (!g_fileUsable) {
        QD_WARN(kTag, "config.json could not be parsed at start-up; language '{}' not saved (file left untouched)", l);
        return false;
    }
    if (!WriteFileAtomic(ConfigPath(), ToJson(g_file).dump(2) + "\n")) {
        QD_WARN(kTag, "could not write language '{}' to config.json", l);
        return false;
    }
    QD_INFO(kTag, "language '{}' saved to config.json", l);
    return true;
}

}  // namespace qd::config

namespace qd::paths {
namespace {
std::wstring ComputeModDir() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ComputeModDir), &self)) {
        return {};
    }
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(self, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    const size_t slash = buf.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : buf.substr(0, slash);
}

std::wstring ComputeGameSaved() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, static_cast<DWORD>(std::size(buf)));
    std::wstring base;
    if (n > 0 && n < std::size(buf)) {
        base.assign(buf, n);
    } else {
        // Fallback: %USERPROFILE%\AppData\Local
        const DWORD m = GetEnvironmentVariableW(L"USERPROFILE", buf, static_cast<DWORD>(std::size(buf)));
        if (m > 0 && m < std::size(buf)) base = std::wstring(buf, m) + L"\\AppData\\Local";
    }
    return base + L"\\QueensDomain\\Saved";
}
}  // namespace

const std::wstring& ModDir() {
    static const std::wstring dir = ComputeModDir();
    return dir;
}

const std::wstring& GameSaved() {
    static const std::wstring dir = ComputeGameSaved();
    return dir;
}

// Computed on first use: callers must run after config::Load() (core::Start guarantees this ordering).
const std::wstring& ModSaved() {
    static const std::wstring dir = [] {
        std::wstring d = GameSaved() + L"\\KnightsDomain";
        const int profile = config::Get().profile;   // always 1 in the release flavour
        if (profile >= 2) d += L"\\DevProfile" + std::to_wstring(profile);
        if (!qd::config::CreateDirectoriesW(d)) {
            OutputDebugStringW((L"[KnightsDomain] could not create " + d + L"\n").c_str());
            return d;
        }
        // Rename: carry the per-save-slot names over from the pre-1.0 folder once (never overwrites).
        std::wstring old = GameSaved() + L"\\QDCoop";
        if (profile >= 2) old += L"\\DevProfile" + std::to_wstring(profile);
        CopyFileW((old + L"\\profiles.json").c_str(), (d + L"\\profiles.json").c_str(), /*bFailIfExists=*/TRUE);
        return d;
    }();
    return dir;
}

std::wstring LogFile() { return ModSaved() + L"\\KnightsDomain.log"; }

}  // namespace qd::paths
