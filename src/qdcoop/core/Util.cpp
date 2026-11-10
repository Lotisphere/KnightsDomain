#include "qdcoop/core/Util.h"

#include <Windows.h>
#include <shellapi.h>

#pragma comment(lib, "shell32.lib")

namespace qd::util {

std::string WideToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

uint64_t NowMs() { return GetTickCount64(); }

bool CommandLineHasFlag(std::wstring_view flag) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    bool found = false;
    for (int i = 1; i < argc && !found; ++i) {
        const std::wstring_view arg(argv[i]);
        found = arg.size() == flag.size() &&
                CompareStringOrdinal(arg.data(), static_cast<int>(arg.size()), flag.data(),
                                     static_cast<int>(flag.size()), TRUE) == CSTR_EQUAL;
    }
    LocalFree(argv);
    return found;
}

}  // namespace qd::util
