// QD Coop — per-save-slot display names. See Profiles.h.
#include "qdcoop/sync/Profiles.h"

#include "qdcoop/core/Config.h"
#include "qdcoop/core/Log.h"
#include "qdcoop/net/Protocol.h"

#include <nlohmann/json.hpp>

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace qd::profiles {
namespace {

constexpr const char* kTag = "profiles";

std::mutex     g_mx;
bool           g_loaded = false;
nlohmann::json g_doc = nlohmann::json::object();

std::filesystem::path FilePath() {
    return std::filesystem::path(paths::ModSaved()) / L"profiles.json";
}

std::string SlotKey(int saveSlot) {
    return "slot" + std::to_string(saveSlot < 0 ? 0 : saveSlot);
}

void LoadLocked() {
    if (g_loaded) return;
    g_loaded = true;

    std::error_code ec;
    const auto path = FilePath();
    if (!std::filesystem::exists(path, ec)) return;

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        QD_WARN(kTag, "cannot open profiles.json for reading");
        return;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(f);
        if (j.is_object()) {
            g_doc = std::move(j);
        } else {
            QD_WARN(kTag, "profiles.json is not a JSON object; ignoring it");
        }
    } catch (const std::exception& e) {
        QD_WARN(kTag, "profiles.json parse error: {}", e.what());
    }
}

bool SaveLocked() {
    const auto path = FilePath();
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    auto tmp = path;
    tmp += L".tmp";
    try {
        const std::string text = g_doc.dump(2);
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            QD_WARN(kTag, "cannot open profiles.json.tmp for writing");
            return false;
        }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.close();
        if (!f) {
            QD_WARN(kTag, "write to profiles.json.tmp failed");
            return false;
        }
    } catch (const std::exception& e) {
        QD_WARN(kTag, "profiles.json serialize error: {}", e.what());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        QD_WARN(kTag, "replace profiles.json failed (GetLastError={})", GetLastError());
        return false;
    }
    return true;
}

bool IsAsciiSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

}  // namespace

size_t CountCodepoints(std::string_view s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}

std::string Sanitize(std::string_view in) {
    std::string out;
    size_t cps = 0;
    size_t i = 0;
    while (i < in.size() && cps < kMaxNameChars) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        size_t len = 0;
        uint32_t cp = 0;
        if (c < 0x80)              { len = 1; cp = c; }
        else if ((c >> 5) == 0x6)  { len = 2; cp = c & 0x1F; }
        else if ((c >> 4) == 0xE)  { len = 3; cp = c & 0x0F; }
        else if ((c >> 3) == 0x1E) { len = 4; cp = c & 0x07; }
        else { ++i; continue; }                       // stray continuation / invalid lead byte
        if (i + len > in.size()) break;               // truncated tail

        bool ok = true;
        for (size_t k = 1; k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(in[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { ++i; continue; }
        const bool overlong = (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000);
        if (overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) || cp < 0x20 || cp == 0x7F) {
            i += len;
            continue;
        }
        if (out.size() + len > net::kMaxNameBytes) break;
        out.append(in.substr(i, len));
        ++cps;
        i += len;
    }
    size_t b = 0, e = out.size();
    while (b < e && IsAsciiSpace(out[b])) ++b;
    while (e > b && IsAsciiSpace(out[e - 1])) --e;
    return out.substr(b, e - b);
}

std::optional<std::string> GetName(const std::string& profileKey, int saveSlot) {
    std::lock_guard lk(g_mx);
    LoadLocked();
    try {
        auto it = g_doc.find(profileKey);
        if (it == g_doc.end() || !it->is_object()) return std::nullopt;
        auto jt = it->find(SlotKey(saveSlot));
        if (jt == it->end() || !jt->is_string()) return std::nullopt;
        std::string name = Sanitize(jt->get<std::string>());
        if (name.empty()) return std::nullopt;
        return name;
    } catch (const std::exception& e) {
        QD_WARN(kTag, "GetName: {}", e.what());
        return std::nullopt;
    }
}

bool SetName(const std::string& profileKey, int saveSlot, const std::string& rawName) {
    const std::string name = Sanitize(rawName);
    std::lock_guard lk(g_mx);
    LoadLocked();
    try {
        auto& entry = g_doc[profileKey];
        if (!entry.is_object()) entry = nlohmann::json::object();
        if (name.empty()) {
            entry.erase(SlotKey(saveSlot));
            if (entry.empty()) g_doc.erase(profileKey);
        } else {
            entry[SlotKey(saveSlot)] = name;
        }
    } catch (const std::exception& e) {
        QD_WARN(kTag, "SetName: {}", e.what());
        return false;
    }
    const bool ok = SaveLocked();
    QD_INFO(kTag, "name for {} {} -> \"{}\" ({})", profileKey, SlotKey(saveSlot), name, ok ? "saved" : "NOT saved");
    return ok;
}

std::string SteamPersonaName() {
    using SteamFriendsFn   = void*(__cdecl*)();
    using PersonaNameFn    = const char*(__cdecl*)(void*);

    HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
    if (!steam) return {};
    auto getFriends = reinterpret_cast<SteamFriendsFn>(GetProcAddress(steam, "SteamAPI_SteamFriends_v018"));
    auto getName    = reinterpret_cast<PersonaNameFn>(GetProcAddress(steam, "SteamAPI_ISteamFriends_GetPersonaName"));
    if (!getFriends || !getName) {
        QD_DEBUG(kTag, "steam flat exports for persona name not found");
        return {};
    }
    void* friends = getFriends();
    if (!friends) return {};
    const char* n = getName(friends);
    if (!n) return {};
    return Sanitize(n);
}

}  // namespace qd::profiles
