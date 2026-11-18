// Knights' Domain — engine addresses found at run time. See EngineOffsets.h.
//
// Include order: SDK headers FIRST (see GameQueries.cpp for why).
#include "SDK/Basic.hpp"

#include "qdcoop/core/EngineOffsets.h"

#include "qdcoop/core/Log.h"

#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <iterator>

namespace qd::core {
namespace {

constexpr const char* kTag = "offsets";

// -1 = any byte. Every pattern was checked to match exactly once in the code section of both game builds known so
// far (exe 81,374,720 and 81,378,304 bytes) and to give the address in the SDK headers of that build.
//
// Code symbols: the pattern is the start of the function.
constexpr int16_t kAppendString[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC,
                                     0x20, 0x8B, 0x01, 0x48, 0x8B, 0xDA, 0x8B, 0xF8, 0x44, 0x0F, 0xB7, 0xC0, 0xC1, 0xEF,
                                     0x10, 0x48, 0x8B, 0xF1, 0x80, 0x3D, -1,   -1,   -1,   -1,   0x00, 0x89};
constexpr int16_t kFindFunctionChecked[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10,
                                            0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B, 0xF9, 0x41, 0xB8,
                                            0x01, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x49, 0x10, 0xE8, -1,
                                            -1,   -1,   -1,   0x48, 0x8B, 0xD8, 0x48, 0x85, 0xC0, 0x0F};
constexpr int16_t kProcessEvent[] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81,
                                     0xEC, 0xF0, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x6C, 0x24, 0x30, 0x48, 0x89, 0x9D, 0x18,
                                     0x01, 0x00, 0x00, 0x48, 0x8B, 0x05, -1,   -1,   -1,   -1,   0x48, 0x33};
// Data symbols: an instruction that loads the symbol RIP-relative; the 4 wildcard bytes are its displacement.
constexpr int kDispAt = 13;
constexpr int16_t kGObjectsA[] = {0xC1, 0xF9, 0x10, 0x48, 0x63, 0xC9, 0x48, 0x8D, 0x14, 0x40, 0x48, 0x8B, 0x05, -1,
                                  -1,   -1,   -1,   0x48, 0x8B, 0x0C, 0xC8, 0x4C, 0x8D, 0x34, 0xD1, 0x41, 0xF7};
constexpr int16_t kGObjectsB[] = {0xC1, 0xF9, 0x10, 0x48, 0x63, 0xC9, 0x48, 0x8D, 0x14, 0x40, 0x48, 0x8B, 0x05, -1,
                                  -1,   -1,   -1,   0x48, 0x8B, 0x0C, 0xC8, 0x48, 0x8D, 0x04, 0xD1, 0xEB, 0x07};
constexpr int16_t kGObjectsC[] = {0xC1, 0xF9, 0x10, 0x48, 0x63, 0xC9, 0x48, 0x8D, 0x14, 0x40, 0x48, 0x8B, 0x05, -1,
                                  -1,   -1,   -1,   0x48, 0x8B, 0x0C, 0xC8, 0x4C, 0x8D, 0x0C, 0xD1, 0x41, 0x8B};
constexpr int16_t kGNamesA[] = {0x20, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0xC1, 0xEA, 0x03, 0x48, 0x8D, 0x2D, -1,
                                -1,   -1,   -1,   0xFF, 0xCA, 0x8B, 0xD9, 0x48, 0xBF, 0xCD, 0xCC, 0xCC, 0xCC};
constexpr int16_t kGWorldA[] = {0x43, 0x04, 0x0F, 0x57, 0xC9, 0x0F, 0x2E, 0xC1, 0x74, 0x4D, 0x48, 0x8B, 0x1D, -1,
                                -1,   -1,   -1,   0x48, 0x85, 0xDB, 0x74, 0x3B, 0x41, 0xB0, 0x01, 0x33, 0xD2};
constexpr int16_t kGWorldB[] = {0x00, 0x49, 0x63, 0x7C, 0x24, 0x08, 0x3B, 0xFE, 0x7F, 0x30, 0x48, 0x8B, 0x05, -1,
                                -1,   -1,   -1,   0x48, 0x8D, 0x95, 0x90, 0x04, 0x00, 0x00, 0x48, 0x8B, 0x48};
constexpr int16_t kGWorldC[] = {0x09, 0x48, 0x8B, 0x40, 0x30, 0x48, 0x85, 0xC0, 0x75, 0x07, 0x48, 0x8B, 0x05, -1,
                                -1,   -1,   -1,   0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x48, 0x89, 0x5C};

struct Pattern {
    const int16_t* bytes = nullptr;
    size_t         size = 0;
    bool           data = false;   // true: the value is the target of the RIP-relative displacement at kDispAt
};
template <size_t N>
constexpr Pattern Code(const int16_t (&b)[N]) { return {b, N, false}; }
template <size_t N>
constexpr Pattern Data(const int16_t (&b)[N]) { return {b, N, true}; }

struct Symbol {
    const char* name;
    SDK::int32* slot;
    Pattern     patterns[3];
};

struct Section {
    const uint8_t* begin = nullptr;
    const uint8_t* end = nullptr;
};

Section CodeSection(const uint8_t* base) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return {};
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return {};
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (std::memcmp(sec->Name, ".text", 6) != 0) continue;
        const uint8_t* b = base + sec->VirtualAddress;
        return {b, b + sec->Misc.VirtualSize};
    }
    return {};
}

// Number of matches (counting stops at 2); `first` is the first one.
int Find(const Section& s, const Pattern& p, const uint8_t*& first) {
    int n = 0;
    const auto b0 = static_cast<uint8_t>(p.bytes[0]);   // every pattern starts with a fixed byte
    const uint8_t* cur = s.begin;
    const uint8_t* last = s.end - p.size;
    while (cur <= last) {
        cur = static_cast<const uint8_t*>(std::memchr(cur, b0, static_cast<size_t>(last - cur) + 1));
        if (!cur) break;
        size_t i = 1;
        while (i < p.size && (p.bytes[i] < 0 || cur[i] == static_cast<uint8_t>(p.bytes[i]))) ++i;
        if (i == p.size) {
            if (n == 0) first = cur;
            if (++n >= 2) break;
        }
        ++cur;
    }
    return n;
}

}  // namespace

bool ResolveEngineOffsets() {
    const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const Section text = base ? CodeSection(base) : Section{};
    if (!text.begin || text.end <= text.begin) {
        QD_WARN(kTag, "no code section found in the game image; keeping the SDK addresses");
        return false;
    }
    const Symbol symbols[] = {
        {"GObjects", &SDK::Offsets::GObjects, {Data(kGObjectsA), Data(kGObjectsB), Data(kGObjectsC)}},
        {"AppendString", &SDK::Offsets::AppendString, {Code(kAppendString), {}, {}}},
        {"GNames", &SDK::Offsets::GNames, {Data(kGNamesA), {}, {}}},
        {"GWorld", &SDK::Offsets::GWorld, {Data(kGWorldA), Data(kGWorldB), Data(kGWorldC)}},
        {"ProcessEvent", &SDK::Offsets::ProcessEvent, {Code(kProcessEvent), {}, {}}},
        {"FindFunctionChecked", &SDK::Offsets::FindFunctionChecked, {Code(kFindFunctionChecked), {}, {}}},
    };
    int found = 0, moved = 0;
    for (const Symbol& sym : symbols) {
        bool ok = false;
        for (const Pattern& p : sym.patterns) {
            if (!p.bytes) continue;
            const uint8_t* at = nullptr;
            if (Find(text, p, at) != 1) continue;   // absent or ambiguous: try the next pattern
            const uint8_t* target = at;
            if (p.data) {
                int32_t disp = 0;
                std::memcpy(&disp, at + kDispAt, sizeof(disp));
                target = at + kDispAt + 4 + disp;
            }
            const int64_t rva = target - base;
            if (rva <= 0 || rva > 0x7FFFFFFF) continue;
            if (*sym.slot != static_cast<SDK::int32>(rva)) {
                QD_INFO(kTag, "{}: 0x{:X} (SDK headers: 0x{:X})", sym.name, rva, *sym.slot);
                *sym.slot = static_cast<SDK::int32>(rva);
                ++moved;
            }
            ok = true;
            break;
        }
        if (ok) {
            ++found;
        } else {
            QD_WARN(kTag, "{}: not found by pattern, keeping 0x{:X} from the SDK headers", sym.name, *sym.slot);
        }
    }
    const int total = static_cast<int>(std::size(symbols));
    QD_INFO(kTag, "{}/{} engine addresses found by pattern, {} differ from the SDK headers", found, total, moved);
    return found == total;
}

}  // namespace qd::core
