#pragma once
// protobuf wire format runtime for the generated message structs

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

namespace pb {

// Heap-boxed optional. Message fields use it so recursive types work and
// "has" presence is just a null check.
template <class T>
class Opt {
public:
    Opt() = default;
    Opt(const Opt& o) : p_(o.p_ ? new T(*o.p_) : nullptr) {}
    Opt(Opt&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    Opt(const T& v) : p_(new T(v)) {}
    Opt(T&& v) : p_(new T(std::move(v))) {}
    ~Opt() { delete p_; }

    Opt& operator=(const Opt& o) {
        if (this != &o) {
            T* n = o.p_ ? new T(*o.p_) : nullptr;
            delete p_;
            p_ = n;
        }
        return *this;
    }
    Opt& operator=(Opt&& o) noexcept {
        if (this != &o) {
            delete p_;
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }
    Opt& operator=(const T& v) {
        if (p_) *p_ = v; else p_ = new T(v);
        return *this;
    }
    Opt& operator=(T&& v) {
        if (p_) *p_ = std::move(v); else p_ = new T(std::move(v));
        return *this;
    }

    explicit operator bool() const { return p_ != nullptr; }
    bool has() const { return p_ != nullptr; }
    T& operator*() { return *p_; }
    const T& operator*() const { return *p_; }
    T* operator->() { return p_; }
    const T* operator->() const { return p_; }
    T* get() { return p_; }
    const T* get() const { return p_; }

    T& emplace() {
        if (!p_) p_ = new T();
        return *p_;
    }
    void reset() {
        delete p_;
        p_ = nullptr;
    }

private:
    T* p_ = nullptr;
};

inline size_t varintSize(uint64_t v) {
    size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        ++n;
    }
    return n;
}

class Writer {
public:
    Writer() = default;
    explicit Writer(size_t reserve) { b_.reserve(reserve); }

    void writeByte(uint8_t c) { b_.push_back(static_cast<char>(c)); }
    void writeRaw(const void* p, size_t n) { b_.append(static_cast<const char*>(p), n); }

    void writeVarint(uint64_t v) {
        while (v >= 0x80) {
            b_.push_back(static_cast<char>((v & 0x7F) | 0x80));
            v >>= 7;
        }
        b_.push_back(static_cast<char>(v));
    }
    void writeTag(uint32_t tag) { writeVarint(tag); }

    void writeUInt32(uint32_t v) { writeVarint(v); }
    void writeUInt64(uint64_t v) { writeVarint(v); }
    void writeInt32(int32_t v) { writeVarint(static_cast<uint64_t>(static_cast<int64_t>(v))); }
    void writeInt64(int64_t v) { writeVarint(static_cast<uint64_t>(v)); }
    void writeBool(bool v) { writeByte(v ? 1 : 0); }
    void writeSInt32(int32_t v) { writeVarint(static_cast<uint32_t>((v << 1) ^ (v >> 31))); }
    void writeSInt64(int64_t v) { writeVarint(static_cast<uint64_t>((v << 1) ^ (v >> 63))); }

    void writeFixed32(uint32_t v) { writeLE32(v); }
    void writeSFixed32(int32_t v) { writeLE32(static_cast<uint32_t>(v)); }
    void writeFixed64(uint64_t v) { writeLE64(v); }
    void writeSFixed64(int64_t v) { writeLE64(static_cast<uint64_t>(v)); }
    void writeFloat(float v) {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        writeLE32(bits);
    }
    void writeDouble(double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, 8);
        writeLE64(bits);
    }

    void writeString(std::string_view s) { writeBytes(s); }
    void writeBytes(std::string_view s) {
        writeVarint(s.size());
        b_.append(s.data(), s.size());
    }

    // Writes the submessage in place, then back-patches its length prefix.
    template <class T>
    void writeMessage(const T& m) {
        size_t at = b_.size();
        b_.push_back('\0');
        m.encode(*this);
        patchLength(at);
    }

    const std::string& data() const { return b_; }
    std::string take() { return std::move(b_); }
    size_t size() const { return b_.size(); }
    void clear() { b_.clear(); }

private:
    void writeLE32(uint32_t v) {
        char t[4] = {static_cast<char>(v), static_cast<char>(v >> 8), static_cast<char>(v >> 16),
                     static_cast<char>(v >> 24)};
        b_.append(t, 4);
    }
    void writeLE64(uint64_t v) {
        char t[8];
        for (int i = 0; i < 8; ++i) t[i] = static_cast<char>(v >> (8 * i));
        b_.append(t, 8);
    }
    void patchLength(size_t at) {
        size_t len = b_.size() - at - 1;
        size_t n = varintSize(len);
        if (n > 1) b_.insert(at, n - 1, '\0');
        for (size_t i = 0; i < n; ++i) {
            uint8_t byte = static_cast<uint8_t>(len & 0x7F);
            len >>= 7;
            if (i + 1 < n) byte |= 0x80;
            b_[at + i] = static_cast<char>(byte);
        }
    }

    std::string b_;
};

class Reader {
public:
    Reader() = default;
    Reader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}
    explicit Reader(std::string_view s)
        : p_(reinterpret_cast<const uint8_t*>(s.data())),
          end_(reinterpret_cast<const uint8_t*>(s.data()) + s.size()) {}

    bool eof() const { return p_ >= end_; }
    size_t remaining() const { return static_cast<size_t>(end_ - p_); }

    bool readVarint(uint64_t& out) {
        uint64_t v = 0;
        int shift = 0;
        while (p_ < end_) {
            uint8_t c = *p_++;
            v |= static_cast<uint64_t>(c & 0x7F) << shift;
            if (!(c & 0x80)) {
                out = v;
                return true;
            }
            shift += 7;
            if (shift > 63) return false;
        }
        return false;
    }
    bool readTag(uint32_t& out) {
        uint64_t v;
        if (!readVarint(v) || v > 0xFFFFFFFFu) return false;
        out = static_cast<uint32_t>(v);
        return true;
    }

    bool readUInt32(uint32_t& o) {
        uint64_t v;
        if (!readVarint(v)) return false;
        o = static_cast<uint32_t>(v);
        return true;
    }
    bool readUInt64(uint64_t& o) { return readVarint(o); }
    bool readInt32(int32_t& o) {
        uint64_t v;
        if (!readVarint(v)) return false;
        o = static_cast<int32_t>(static_cast<uint32_t>(v));
        return true;
    }
    bool readInt64(int64_t& o) {
        uint64_t v;
        if (!readVarint(v)) return false;
        o = static_cast<int64_t>(v);
        return true;
    }
    bool readBool(bool& o) {
        uint64_t v;
        if (!readVarint(v)) return false;
        o = v != 0;
        return true;
    }
    bool readSInt32(int32_t& o) {
        uint64_t v;
        if (!readVarint(v)) return false;
        uint32_t u = static_cast<uint32_t>(v);
        o = static_cast<int32_t>((u >> 1) ^ (~(u & 1) + 1));
        return true;
    }
    bool readSInt64(int64_t& o) {
        uint64_t v;
        if (!readVarint(v)) return false;
        o = static_cast<int64_t>((v >> 1) ^ (~(v & 1) + 1));
        return true;
    }

    bool readFixed32(uint32_t& o) { return readLE32(o); }
    bool readSFixed32(int32_t& o) {
        uint32_t v;
        if (!readLE32(v)) return false;
        o = static_cast<int32_t>(v);
        return true;
    }
    bool readFixed64(uint64_t& o) { return readLE64(o); }
    bool readSFixed64(int64_t& o) {
        uint64_t v;
        if (!readLE64(v)) return false;
        o = static_cast<int64_t>(v);
        return true;
    }
    bool readFloat(float& o) {
        uint32_t v;
        if (!readLE32(v)) return false;
        std::memcpy(&o, &v, 4);
        return true;
    }
    bool readDouble(double& o) {
        uint64_t v;
        if (!readLE64(v)) return false;
        std::memcpy(&o, &v, 8);
        return true;
    }

    bool readString(std::string& o) {
        uint64_t n;
        if (!readVarint(n) || n > remaining()) return false;
        o.assign(reinterpret_cast<const char*>(p_), static_cast<size_t>(n));
        p_ += n;
        return true;
    }
    bool readBytes(std::string& o) { return readString(o); }

    bool readSub(Reader& out) {
        uint64_t n;
        if (!readVarint(n) || n > remaining()) return false;
        out = Reader(p_, static_cast<size_t>(n));
        p_ += n;
        return true;
    }

    template <class T>
    bool readMessage(T& m) {
        Reader sub;
        if (!readSub(sub)) return false;
        return m.decode(sub);
    }

    bool skip(uint32_t tag) {
        switch (tag & 7) {
            case 0: {
                uint64_t v;
                return readVarint(v);
            }
            case 1:
                return advance(8);
            case 2: {
                uint64_t n;
                return readVarint(n) && advance(static_cast<size_t>(n));
            }
            case 5:
                return advance(4);
            default:
                return false;  // groups are not used by this protocol
        }
    }

private:
    bool advance(size_t n) {
        if (n > remaining()) return false;
        p_ += n;
        return true;
    }
    bool readLE32(uint32_t& o) {
        if (remaining() < 4) return false;
        o = static_cast<uint32_t>(p_[0]) | (static_cast<uint32_t>(p_[1]) << 8) |
            (static_cast<uint32_t>(p_[2]) << 16) | (static_cast<uint32_t>(p_[3]) << 24);
        p_ += 4;
        return true;
    }
    bool readLE64(uint64_t& o) {
        if (remaining() < 8) return false;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p_[i]) << (8 * i);
        p_ += 8;
        o = v;
        return true;
    }

    const uint8_t* p_ = nullptr;
    const uint8_t* end_ = nullptr;
};

}  // namespace pb
