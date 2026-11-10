// QD Coop — small internal helpers shared by the core. Not part of the public contract.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace qd::util {

std::string  WideToUtf8(std::wstring_view w);
std::wstring Utf8ToWide(std::string_view s);

// Monotonic milliseconds (GetTickCount64). Thread-safe.
uint64_t NowMs();

// True if the process command line contains `flag` as a whole argument (case-insensitive), e.g. L"-qdcoop-noplsf".
bool CommandLineHasFlag(std::wstring_view flag);

}  // namespace qd::util
