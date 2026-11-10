// QD Coop — little-endian byte writer/reader used by the wire protocol.
// Strings: u16 byte length + UTF-8 bytes. Arrays: u16 element count + elements.
// ByteReader is sticky-failing: once a read runs past the end (or a value is out of range) every later read fails.
#pragma once
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace qd::net {

// Longest prefix of `s` that is <= maxBytes and does not end in the middle of a UTF-8 sequence.
inline std::string_view Utf8Truncate(std::string_view s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t cut = maxBytes;
    // s[cut] is the first byte we drop; while it is a continuation byte (10xxxxxx) the character straddles the cut.
    while (cut > 0 && (static_cast<uint8_t>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut);
}

class ByteWriter {
public:
    ByteWriter() = default;
    explicit ByteWriter(size_t reserve) { buf_.reserve(reserve); }

    void U8(uint8_t v) { buf_.push_back(v); }
    void U16(uint16_t v) { U8(static_cast<uint8_t>(v)); U8(static_cast<uint8_t>(v >> 8)); }
    void U32(uint32_t v) { U16(static_cast<uint16_t>(v)); U16(static_cast<uint16_t>(v >> 16)); }
    void U64(uint64_t v) { U32(static_cast<uint32_t>(v)); U32(static_cast<uint32_t>(v >> 32)); }
    void I32(int32_t v) { U32(static_cast<uint32_t>(v)); }
    void F32(float v) { U32(std::bit_cast<uint32_t>(v)); }
    void Bool(bool v) { U8(v ? 1 : 0); }

    // u16 length + bytes; cut at a UTF-8 boundary so it fits in min(maxBytes, 65535).
    void Str(std::string_view s, size_t maxBytes = 0xFFFF) {
        if (maxBytes > 0xFFFF) maxBytes = 0xFFFF;
        const std::string_view t = Utf8Truncate(s, maxBytes);
        U16(static_cast<uint16_t>(t.size()));
        Bytes(t.data(), t.size());
    }

    // Writes a u16 element count, clamped to 65535. Returns the count written: callers must emit exactly that many.
    uint16_t Count(size_t n) {
        const uint16_t c = n > 0xFFFF ? uint16_t{0xFFFF} : static_cast<uint16_t>(n);
        U16(c);
        return c;
    }

    void Bytes(const void* data, size_t n) {
        if (n == 0) return;
        const auto* p = static_cast<const uint8_t*>(data);
        buf_.insert(buf_.end(), p, p + n);
    }

    size_t Size() const { return buf_.size(); }
    std::vector<uint8_t>& Buffer() { return buf_; }
    std::vector<uint8_t> Take() { return std::move(buf_); }

private:
    std::vector<uint8_t> buf_;
};

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size) : p_(data), size_(data ? size : 0) {}

    bool U8(uint8_t& v) { return Take(&v, 1); }
    bool U16(uint16_t& v) {
        uint8_t b[2];
        if (!Take(b, 2)) return false;
        v = static_cast<uint16_t>(b[0] | (b[1] << 8));
        return true;
    }
    bool U32(uint32_t& v) {
        uint8_t b[4];
        if (!Take(b, 4)) return false;
        v = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) | (static_cast<uint32_t>(b[2]) << 16) |
            (static_cast<uint32_t>(b[3]) << 24);
        return true;
    }
    bool U64(uint64_t& v) {
        uint32_t lo = 0, hi = 0;
        if (!U32(lo) || !U32(hi)) return false;
        v = (static_cast<uint64_t>(hi) << 32) | lo;
        return true;
    }
    bool I32(int32_t& v) {
        uint32_t u = 0;
        if (!U32(u)) return false;
        v = static_cast<int32_t>(u);
        return true;
    }
    // Rejects NaN / infinity (treated as an out-of-range value).
    bool F32(float& v) {
        uint32_t u = 0;
        if (!U32(u)) return false;
        const float f = std::bit_cast<float>(u);
        if (!std::isfinite(f)) return Fail();
        v = f;
        return true;
    }
    // Only 0 / 1 are valid.
    bool Bool(bool& v) {
        uint8_t b = 0;
        if (!U8(b)) return false;
        if (b > 1) return Fail();
        v = b != 0;
        return true;
    }
    // Fails if the declared length exceeds maxBytes or the remaining data.
    bool Str(std::string& s, size_t maxBytes = 0xFFFF) {
        uint16_t n = 0;
        if (!U16(n)) return false;
        if (n > maxBytes || n > Remaining()) return Fail();
        s.assign(reinterpret_cast<const char*>(p_ + pos_), n);
        pos_ += n;
        return true;
    }
    // Reads a u16 element count and sanity-checks it against the remaining bytes (each element needs >= minElemBytes).
    bool Count(uint16_t& n, size_t minElemBytes) {
        if (!U16(n)) return false;
        if (minElemBytes > 0 && static_cast<size_t>(n) * minElemBytes > Remaining()) return Fail();
        return true;
    }

    size_t Remaining() const { return ok_ ? size_ - pos_ : 0; }
    bool   AtEnd() const { return ok_ && pos_ == size_; }
    bool   Ok() const { return ok_; }

private:
    bool Fail() {
        ok_ = false;
        return false;
    }
    bool Take(void* dst, size_t n) {
        if (!ok_ || size_ - pos_ < n) return Fail();
        std::memcpy(dst, p_ + pos_, n);
        pos_ += n;
        return true;
    }

    const uint8_t* p_ = nullptr;
    size_t         size_ = 0;
    size_t         pos_ = 0;
    bool           ok_ = true;
};

}  // namespace qd::net
