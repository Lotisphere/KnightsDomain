// Knights' Domain — Steam launch + ownership check. See Ownership.h.
#include "qdcoop/core/Ownership.h"

#include "qdcoop/core/Core.h"
#include "qdcoop/core/Log.h"
#include "qdcoop/core/Util.h"
#include "qdcoop/net/SteamFlat.h"

#include <Windows.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

#include <atomic>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace qd::core {
namespace {

constexpr char    kTag[] = "ownership";
constexpr wchar_t kSteamDll[] = L"steam_api64.dll";

std::atomic<Ownership> g_state{Ownership::NotChecked};
std::mutex             g_reasonMx;
std::string            g_reason;

// Resolves one flat export from the loaded steam_api64.dll with the signature declared in net/SteamFlat.h.
#define QD_OWN_RESOLVE(mod, name) \
    reinterpret_cast<decltype(&::qd::net::steam::decl::name)>(GetProcAddress((mod), #name))

void SetFailed(const std::string& why) {
    {
        std::lock_guard lk(g_reasonMx);
        g_reason = why;
    }
    g_state.store(Ownership::Failed);
    QD_ERROR(kTag, "ownership check FAILED: {}", why);
}

std::wstring ModulePath(HMODULE m) {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(m, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

// Checks 2-5. "" = passed, otherwise the reason.
std::string SteamChecks() {
    HMODULE mod = GetModuleHandleW(kSteamDll);
    if (!mod) return "steam_api64.dll is not loaded (game not started through Steam?)";

    auto userFn = QD_OWN_RESOLVE(mod, SteamAPI_SteamUser_v023);
    auto appsFn = QD_OWN_RESOLVE(mod, SteamAPI_SteamApps_v008);
    auto utilsFn = QD_OWN_RESOLVE(mod, SteamAPI_SteamUtils_v010);
    auto getAppId = QD_OWN_RESOLVE(mod, SteamAPI_ISteamUtils_GetAppID);
    auto loggedOn = QD_OWN_RESOLVE(mod, SteamAPI_ISteamUser_BLoggedOn);
    auto subscribed = QD_OWN_RESOLVE(mod, SteamAPI_ISteamApps_BIsSubscribedApp);
    if (!userFn || !appsFn || !utilsFn || !getAppId || !loggedOn || !subscribed)
        return std::format("steam_api64.dll lacks a required export (user={} apps={} utils={} GetAppID={} BLoggedOn={} "
                           "BIsSubscribedApp={})",
                           userFn != nullptr, appsFn != nullptr, utilsFn != nullptr, getAppId != nullptr,
                           loggedOn != nullptr, subscribed != nullptr);

    net::steam::ISteamUser*  user = userFn();
    net::steam::ISteamApps*  apps = appsFn();
    net::steam::ISteamUtils* utils = utilsFn();
    if (!user || !apps || !utils)
        return std::format("Steam interfaces unavailable (user={} apps={} utils={}); Steam API not initialised",
                           user != nullptr, apps != nullptr, utils != nullptr);

    const uint32_t appId = getAppId(utils);
    if (appId != kSteamAppId) return std::format("running as Steam app {} (expected {})", appId, kSteamAppId);
    if (!loggedOn(user)) return "the Steam user is not logged on";
    if (!subscribed(apps, kSteamAppId)) return std::format("this Steam account does not own app {}", kSteamAppId);
    return {};
}

// Check 1 (worker thread). "" = passed, otherwise the reason.
std::string VerifySignature(const std::wstring& path) {
    // 1a. Authenticode chain valid.
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &file;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_REVOCATION_CHECK_NONE;
    const LONG trust = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &wd);
    if (trust != ERROR_SUCCESS)
        return std::format("steam_api64.dll Authenticode signature invalid (WinVerifyTrust 0x{:08X})",
                           static_cast<uint32_t>(trust));

    // 1b. Signer is Valve.
    DWORD      encoding = 0, contentType = 0, formatType = 0;
    HCERTSTORE store = nullptr;
    HCRYPTMSG  msg = nullptr;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(), CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                          CERT_QUERY_FORMAT_FLAG_BINARY, 0, &encoding, &contentType, &formatType, &store, &msg,
                          nullptr))
        return std::format("CryptQueryObject failed ({})", GetLastError());

    std::string result;
    std::wstring signer;
    DWORD size = 0;
    if (!CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &size) || size == 0) {
        result = std::format("CMSG_SIGNER_INFO_PARAM size query failed ({})", GetLastError());
    } else {
        std::vector<BYTE> buf(size);
        if (!CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, buf.data(), &size)) {
            result = std::format("CMSG_SIGNER_INFO_PARAM failed ({})", GetLastError());
        } else {
            const auto* si = reinterpret_cast<const CMSG_SIGNER_INFO*>(buf.data());
            CERT_INFO ci{};
            ci.Issuer = si->Issuer;
            ci.SerialNumber = si->SerialNumber;
            PCCERT_CONTEXT cert = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                                                             CERT_FIND_SUBJECT_CERT, &ci, nullptr);
            if (!cert) {
                result = std::format("signer certificate not found in the signature ({})", GetLastError());
            } else {
                wchar_t name[512] = {};
                CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name,
                                   static_cast<DWORD>(std::size(name)));
                signer = name;
                CertFreeCertificateContext(cert);
            }
        }
    }
    if (store) CertCloseStore(store, 0);
    if (msg) CryptMsgClose(msg);
    if (!result.empty()) return result;

    // Valve's code-signing certificate subject: "Valve Corp." (also accept the long form "Valve Corporation").
    if (signer.find(L"Valve Corp.") == std::wstring::npos && signer.find(L"Valve Corporation") == std::wstring::npos)
        return std::format("steam_api64.dll is signed by '{}', not by Valve", util::WideToUtf8(signer));
    QD_INFO(kTag, "steam_api64.dll signature OK (signer '{}')", util::WideToUtf8(signer));
    return {};
}

}  // namespace

void OwnershipCheck() {
    Ownership expected = Ownership::NotChecked;
    if (!g_state.compare_exchange_strong(expected, Ownership::Pending)) return;   // already started / decided

#if QD_DEV
    QD_INFO(kTag, "dev build: Steam ownership check skipped");
    g_state.store(Ownership::Passed);
#else
    if (std::string why = SteamChecks(); !why.empty()) {
        SetFailed(why);
        return;
    }
    QD_INFO(kTag, "Steam checks passed (app {}, logged on, owned); verifying steam_api64.dll signature", kSteamAppId);
    const std::wstring path = ModulePath(GetModuleHandleW(kSteamDll));
    if (path.empty()) {
        SetFailed("cannot get the path of steam_api64.dll");
        return;
    }
    QD_INFO(kTag, "steam_api64.dll: {}", util::WideToUtf8(path));
    try {
        std::thread([path] {
            const std::string why = VerifySignature(path);
            if (!why.empty()) {
                SetFailed(why);
                return;
            }
            Ownership pending = Ownership::Pending;   // never turn a Failed (OwnershipFail) back into Passed
            if (g_state.compare_exchange_strong(pending, Ownership::Passed)) QD_INFO(kTag, "ownership check passed");
        }).detach();
    } catch (const std::exception& e) {
        SetFailed(std::format("could not start the signature check thread ({})", e.what()));
    }
#endif
}

void OwnershipFail(const std::string& why) {
    Ownership s = g_state.load();
    if (s == Ownership::Passed || s == Ownership::Failed) return;
#if QD_DEV
    QD_WARN(kTag, "dev build: would fail the ownership check: {}", why);
    return;
#else
    SetFailed(why);
#endif
}

Ownership OwnershipState() { return g_state.load(); }

std::string OwnershipFailReason() {
    if (g_state.load() != Ownership::Failed) return {};
    std::lock_guard lk(g_reasonMx);
    return g_reason;
}

std::string SteamGameLanguage() {
    HMODULE mod = GetModuleHandleW(kSteamDll);
    if (!mod) return {};
    auto appsFn = QD_OWN_RESOLVE(mod, SteamAPI_SteamApps_v008);
    auto langFn = QD_OWN_RESOLVE(mod, SteamAPI_ISteamApps_GetCurrentGameLanguage);
    if (!appsFn || !langFn) return {};
    net::steam::ISteamApps* apps = appsFn();
    if (!apps) return {};
    const char* lang = langFn(apps);
    return lang ? std::string(lang) : std::string();
}

}  // namespace qd::core
