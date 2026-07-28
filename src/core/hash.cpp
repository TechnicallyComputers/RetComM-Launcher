#include "retcomm/hash.hpp"

#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace retcomm {
namespace {

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            const uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

// Compact public-domain SHA-1 (Steve Reid / others), adapted for file streaming.
struct Sha1Ctx {
    uint32_t state[5]{};
    uint64_t count = 0;
    uint8_t buffer[64]{};
};

uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

void sha1_transform(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const uint32_t temp = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = temp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void sha1_init(Sha1Ctx& ctx) {
    ctx.state[0] = 0x67452301u;
    ctx.state[1] = 0xEFCDAB89u;
    ctx.state[2] = 0x98BADCFEu;
    ctx.state[3] = 0x10325476u;
    ctx.state[4] = 0xC3D2E1F0u;
    ctx.count = 0;
}

void sha1_update(Sha1Ctx& ctx, const uint8_t* data, size_t len) {
    size_t i = 0;
    const size_t idx = size_t(ctx.count & 63ull);
    ctx.count += len;
    if (idx) {
        const size_t fill = 64 - idx;
        if (len < fill) {
            std::memcpy(ctx.buffer + idx, data, len);
            return;
        }
        std::memcpy(ctx.buffer + idx, data, fill);
        sha1_transform(ctx.state, ctx.buffer);
        i = fill;
    }
    for (; i + 64 <= len; i += 64) sha1_transform(ctx.state, data + i);
    if (i < len) std::memcpy(ctx.buffer, data + i, len - i);
}

void sha1_final(Sha1Ctx& ctx, uint8_t digest[20]) {
    uint8_t finalcount[8];
    for (int i = 0; i < 8; ++i)
        finalcount[i] = uint8_t((ctx.count * 8) >> ((7 - i) * 8));

    const uint8_t pad = 0x80;
    sha1_update(ctx, &pad, 1);
    const uint8_t zero = 0;
    while ((ctx.count & 63ull) != 56ull) sha1_update(ctx, &zero, 1);
    sha1_update(ctx, finalcount, 8);
    for (int i = 0; i < 20; ++i)
        digest[i] = uint8_t((ctx.state[i >> 2] >> ((3 - (i & 3)) * 8)) & 0xff);
}

} // namespace

std::string crc32_hex(uint32_t crc) {
    std::ostringstream oss;
    oss << std::hex << std::nouppercase << std::setw(8) << std::setfill('0') << crc;
    return oss.str();
}

std::string to_lower_hex(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(char(std::tolower(c)));
    if (out.size() >= 2 && out[0] == '0' && out[1] == 'x') out.erase(0, 2);
    return out;
}

std::string file_crc32_hex(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    uint32_t crc = 0;
    std::array<uint8_t, 1 << 16> buf{};
    while (in) {
        in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
        const auto n = size_t(in.gcount());
        if (n) crc = crc32_update(crc, buf.data(), n);
    }
    return crc32_hex(crc);
}

std::string file_sha1_hex(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    Sha1Ctx ctx;
    sha1_init(ctx);
    std::array<uint8_t, 1 << 16> buf{};
    while (in) {
        in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
        const auto n = size_t(in.gcount());
        if (n) sha1_update(ctx, buf.data(), n);
    }
    uint8_t digest[20];
    sha1_final(ctx, digest);
    std::ostringstream oss;
    for (uint8_t b : digest)
        oss << std::hex << std::nouppercase << std::setw(2) << std::setfill('0') << int(b);
    return oss.str();
}

} // namespace retcomm
