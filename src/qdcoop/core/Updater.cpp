// Knights' Domain — update check and in-place update. See Updater.h.
#include "qdcoop/core/Updater.h"

#include "qdcoop/core/Config.h"
#include "qdcoop/core/Core.h"
#include "qdcoop/core/Log.h"
#include "qdcoop/ui/I18n.h"

#include "nlohmann/json.hpp"

#include <Windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace qd::updater {
namespace {

constexpr const char* kTag = "update";
constexpr const char* kGithubManifest = "https://raw.githubusercontent.com/Lotisphere/KnightsDomain/main/update/manifest.json";
// The mirror is the Cloudflare download site that mainland China can reach (tools/mirror, release/site-dl).
constexpr const char* kMirrorManifest = "https://kd.mangolotis.com/update/manifest.json";
constexpr size_t kMaxManifestBytes = 64 * 1024;
constexpr size_t kMaxFileBytes = 32 * 1024 * 1024;

// The only files an update may write, relative to Binaries\Win64 (the parent of the mod folder).
struct Target { const char* manifestPath; const wchar_t* relative; };
constexpr Target kTargets[] = {
    {"KnightsDomain/KnightsDomain.dll", L"KnightsDomain\\KnightsDomain.dll"},
    {"xinput1_3.dll", L"xinput1_3.dll"},
};

struct FileEntry {
    int         target = -1;
    std::string sha256;       // lower-case hex
    std::string github, mirror;
};
struct Manifest {
    std::string version;
    std::vector<FileEntry> files;
};

std::mutex        g_mutex;
Status            g_status;
Manifest          g_manifest;
bool              g_mirrorFirst = false;
std::atomic<bool> g_started{false}, g_busy{false};

void Set(State st, const std::string& err = {}) {
    std::lock_guard l(g_mutex);
    g_status.state = st;
    g_status.error = err;
    if (st != State::Downloading) g_status.percent = st == State::Installed ? 100 : 0;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool AllowedManifestHost(const std::wstring& host) {
#if QD_DEV
    if (host == L"127.0.0.1" || host == L"localhost") return true;   // local test server
#endif
    return host == L"raw.githubusercontent.com" || host == L"kd.mangolotis.com";
}
bool AllowedFileHost(const std::wstring& host) {
#if QD_DEV
    if (host == L"127.0.0.1" || host == L"localhost") return true;
#endif
    return host == L"github.com" || host == L"kd.mangolotis.com" || host == L"raw.githubusercontent.com";
}

struct Handle {
    HINTERNET h = nullptr;
    ~Handle() { if (h) WinHttpCloseHandle(h); }
};

// GET over HTTPS (plain HTTP only for the dev test server). Follows redirects (GitHub release downloads redirect to
// its file storage); what arrives is verified by SHA-256 afterwards. progress: 0..100 or nullptr.
bool HttpGet(const std::string& url, bool isManifest, size_t maxBytes, std::vector<uint8_t>& out, std::string& err,
             void (*progress)(int) = nullptr) {
    out.clear();
    const std::wstring wurl = Widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2047;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { err = "bad url"; return false; }
    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
#if QD_DEV
    const bool localHttp = !https && (std::wstring(host) == L"127.0.0.1" || std::wstring(host) == L"localhost");
#else
    const bool localHttp = false;
#endif
    if (!https && !localHttp) { err = "not https"; return false; }
    if (!(isManifest ? AllowedManifestHost(host) : AllowedFileHost(host))) { err = "host not allowed"; return false; }

    Handle session, connect, request;
    session.h = WinHttpOpen(L"KnightsDomain-Updater", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) session.h = WinHttpOpen(L"KnightsDomain-Updater", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) { err = "WinHttpOpen"; return false; }
    WinHttpSetTimeouts(session.h, 8000, 8000, 15000, 30000);
    connect.h = WinHttpConnect(session.h, host, uc.nPort, 0);
    if (!connect.h) { err = "connect"; return false; }
    request.h = WinHttpOpenRequest(connect.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                   https ? WINHTTP_FLAG_SECURE : 0);
    if (!request.h) { err = "open request"; return false; }
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        err = "request failed (" + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status != 200) { err = "http " + std::to_string(status); return false; }
    DWORD total = 0;
    len = sizeof(total);
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &total, &len, WINHTTP_NO_HEADER_INDEX);
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(request.h, &avail)) { err = "read failed"; return false; }
        if (avail == 0) break;
        if (out.size() + avail > maxBytes) { err = "too large"; return false; }
        const size_t old = out.size();
        out.resize(old + avail);
        DWORD got = 0;
        if (!WinHttpReadData(request.h, out.data() + old, avail, &got)) { err = "read failed"; return false; }
        out.resize(old + got);
        if (progress && total > 0) progress(static_cast<int>(out.size() * 100ull / total));
    }
    return true;
}

bool Sha256Hex(const uint8_t* data, size_t size, std::string& hex) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    uint8_t digest[32] = {};
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            ok = BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) == 0 &&
                 BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    if (!ok) return false;
    static const char* d = "0123456789abcdef";
    hex.clear();
    for (uint8_t b : digest) { hex.push_back(d[b >> 4]); hex.push_back(d[b & 15]); }
    return true;
}

bool ReadFileBytes(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

std::filesystem::path Win64Dir() { return std::filesystem::path(paths::ModDir()).parent_path(); }

bool ParseManifest(const std::vector<uint8_t>& body, Manifest& m, std::string& err) {
    try {
        const auto j = nlohmann::json::parse(body.begin(), body.end());
        m.version = j.at("version").get<std::string>();
        if (m.version.empty() || m.version.size() > 32) { err = "bad version"; return false; }
        for (const auto& f : j.at("files")) {
            FileEntry e;
            const std::string path = f.at("path").get<std::string>();
            for (int i = 0; i < static_cast<int>(std::size(kTargets)); ++i)
                if (path == kTargets[i].manifestPath) e.target = i;
            if (e.target < 0) { err = "unexpected file in manifest: " + path; return false; }
            e.sha256 = f.at("sha256").get<std::string>();
            for (char& c : e.sha256) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (e.sha256.size() != 64) { err = "bad sha256"; return false; }
            e.github = f.value("github", std::string());
            e.mirror = f.value("mirror", std::string());
            if (e.github.empty() && e.mirror.empty()) { err = "file without url"; return false; }
            m.files.push_back(std::move(e));
        }
        if (m.files.empty()) { err = "no files"; return false; }
        return true;
    } catch (const std::exception& ex) {
        err = std::string("manifest parse: ") + ex.what();
        return false;
    }
}

void CleanLeftovers() {
    std::error_code ec;
    for (const Target& t : kTargets) {
        const auto p = Win64Dir() / t.relative;
        std::filesystem::remove(std::filesystem::path(p).concat(L".old"), ec);
        std::filesystem::remove(std::filesystem::path(p).concat(L".new"), ec);
    }
}

void CheckThread() {
    CleanLeftovers();
    std::vector<std::string> urls;
#if QD_DEV
    char over[512] = {};
    if (GetEnvironmentVariableA("QD_UPDATE_MANIFEST", over, sizeof(over)) > 0) urls.push_back(over);
#endif
    if (urls.empty()) {
        urls.push_back(g_mirrorFirst ? kMirrorManifest : kGithubManifest);
        urls.push_back(g_mirrorFirst ? kGithubManifest : kMirrorManifest);
    }
    std::string err;
    for (const std::string& url : urls) {
        std::vector<uint8_t> body;
        Manifest m;
        if (!HttpGet(url, true, kMaxManifestBytes, body, err) || !ParseManifest(body, m, err)) {
            QD_INFO(kTag, "manifest {}: {}", url, err);
            continue;
        }
        const bool newer = VersionLess(core::kModVersion, m.version);
        QD_INFO(kTag, "latest version {} (installed {}), from {}", m.version, core::kModVersion, url);
        {
            std::lock_guard l(g_mutex);
            g_manifest = std::move(m);
            g_status.latest = g_manifest.version;
        }
        Set(newer ? State::Available : State::UpToDate);
        g_busy = false;
        return;
    }
    Set(State::Failed, "check: " + err);
    g_busy = false;
}

void UpdateThread() {
    Manifest m;
    {
        std::lock_guard l(g_mutex);
        m = g_manifest;
    }
    const auto dir = Win64Dir();
    std::error_code ec;
    struct Pending { std::filesystem::path target, fresh; };
    std::vector<Pending> pending;
    int index = 0;
    for (const FileEntry& f : m.files) {
        const auto target = dir / kTargets[f.target].relative;
        std::vector<uint8_t> cur;
        std::string hex;
        if (ReadFileBytes(target, cur) && Sha256Hex(cur.data(), cur.size(), hex) && hex == f.sha256) {
            ++index;
            continue;   // already this version
        }
        static std::atomic<int> s_base{0}, s_span{100};
        s_base = index * 100 / static_cast<int>(m.files.size());
        s_span = 100 / static_cast<int>(m.files.size());
        auto progress = [](int p) {
            std::lock_guard l(g_mutex);
            g_status.percent = s_base + p * s_span / 100;
        };
        std::vector<std::string> urls;
        if (g_mirrorFirst) { urls.push_back(f.mirror); urls.push_back(f.github); }
        else               { urls.push_back(f.github); urls.push_back(f.mirror); }
        std::vector<uint8_t> body;
        std::string err = "no url";
        bool ok = false;
        for (const std::string& url : urls) {
            if (url.empty()) continue;
            if (!HttpGet(url, false, kMaxFileBytes, body, err, progress)) {
                QD_WARN(kTag, "download {}: {}", url, err);
                continue;
            }
            if (!Sha256Hex(body.data(), body.size(), hex) || hex != f.sha256) {
                err = "checksum mismatch";
                QD_WARN(kTag, "download {}: checksum {} != manifest {}", url, hex, f.sha256);
                continue;
            }
            ok = true;
            break;
        }
        if (!ok) {
            for (const Pending& p : pending) std::filesystem::remove(p.fresh, ec);
            Set(State::Failed, "download: " + err);
            g_busy = false;
            return;
        }
        const auto fresh = std::filesystem::path(target).concat(L".new");
        {
            std::ofstream out(fresh, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
            if (!out) {
                for (const Pending& p : pending) std::filesystem::remove(p.fresh, ec);
                std::filesystem::remove(fresh, ec);
                Set(State::Failed, "cannot write into the game folder");
                g_busy = false;
                return;
            }
        }
        pending.push_back({target, fresh});
        ++index;
    }
    // Every file is downloaded and verified: swap them in. A loaded DLL cannot be overwritten but can be renamed.
    for (const Pending& p : pending) {
        const auto old = std::filesystem::path(p.target).concat(L".old");
        std::filesystem::remove(old, ec);
        const bool had = std::filesystem::exists(p.target, ec);
        if (had && !MoveFileExW(p.target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            Set(State::Failed, "cannot replace " + p.target.filename().string());
            g_busy = false;
            return;
        }
        if (!MoveFileExW(p.fresh.c_str(), p.target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            if (had) MoveFileExW(old.c_str(), p.target.c_str(), MOVEFILE_REPLACE_EXISTING);   // put the old one back
            Set(State::Failed, "cannot install " + p.target.filename().string());
            g_busy = false;
            return;
        }
        QD_INFO(kTag, "installed {}", p.target.filename().string());
    }
    QD_INFO(kTag, "updated to {}; restart the game to use it", m.version);
    Set(State::Installed);
    g_busy = false;
}

std::filesystem::path NoticeMarker() {
    return std::filesystem::path(paths::ModSaved()) / (std::string("notice_") + core::kModVersion + ".seen");
}

}  // namespace

bool VersionLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        unsigned long x = 0, y = 0;
        while (i < a.size() && a[i] != '.') { if (a[i] >= '0' && a[i] <= '9') x = x * 10 + static_cast<unsigned long>(a[i] - '0'); ++i; }
        while (j < b.size() && b[j] != '.') { if (b[j] >= '0' && b[j] <= '9') y = y * 10 + static_cast<unsigned long>(b[j] - '0'); ++j; }
        if (x != y) return x < y;
        if (i < a.size()) ++i;
        if (j < b.size()) ++j;
    }
    return false;
}

void Start() {
    if (g_started.exchange(true)) return;
    if (paths::ModDir().empty()) return;
    g_mirrorFirst = ui::GetLang() == ui::Lang::Zh;
    g_busy = true;
    Set(State::Checking);
    std::thread(CheckThread).detach();
}

Status Get() {
    std::lock_guard l(g_mutex);
    return g_status;
}

void BeginUpdate() {
    {
        std::lock_guard l(g_mutex);
        if (g_manifest.files.empty()) return;
        if (g_status.state != State::Available && g_status.state != State::Failed) return;
    }
    if (g_busy.exchange(true)) return;
    g_mirrorFirst = ui::GetLang() == ui::Lang::Zh;
    Set(State::Downloading);
    std::thread(UpdateThread).detach();
}

bool NoticeSeen() {
    std::error_code ec;
    return std::filesystem::exists(NoticeMarker(), ec);
}

void MarkNoticeSeen() {
    std::ofstream f(NoticeMarker(), std::ios::trunc);
    f << "seen\n";
}

}  // namespace qd::updater
