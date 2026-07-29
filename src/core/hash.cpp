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

std::string file_crc32_hex(const fs::path& path, std::uint64_t skip_bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    if (skip_bytes) in.seekg(static_cast<std::streamoff>(skip_bytes), std::ios::beg);
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

namespace {

// Compact public-domain MD5 (RFC 1321), adapted for file streaming.
struct Md5Ctx {
    uint32_t state[4]{};
    uint64_t count = 0;
    uint8_t buffer[64]{};
};

uint32_t md5_rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

void md5_transform(uint32_t state[4], const uint8_t block[64]) {
    static constexpr uint32_t K[64] = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au,
        0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
        0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u,
        0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
        0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
        0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
        0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
        0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
        0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u,
        0xffeff47du, 0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
        0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};
    static constexpr int S[64] = {7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22,
                                  5,  9,  14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 5,  9,  14, 20,
                                  4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
                                  6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21};

    uint32_t M[16];
    for (int i = 0; i < 16; ++i) {
        M[i] = uint32_t(block[i * 4]) | (uint32_t(block[i * 4 + 1]) << 8) |
               (uint32_t(block[i * 4 + 2]) << 16) | (uint32_t(block[i * 4 + 3]) << 24);
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f, g;
        if (i < 16) {
            f = (b & c) | ((~b) & d);
            g = uint32_t(i);
        } else if (i < 32) {
            f = (d & b) | ((~d) & c);
            g = uint32_t((5 * i + 1) % 16);
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = uint32_t((3 * i + 5) % 16);
        } else {
            f = c ^ (b | (~d));
            g = uint32_t((7 * i) % 16);
        }
        const uint32_t tmp = d;
        d = c;
        c = b;
        b = b + md5_rol(a + f + K[i] + M[g], S[i]);
        a = tmp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

void md5_init(Md5Ctx& ctx) {
    ctx.state[0] = 0x67452301u;
    ctx.state[1] = 0xefcdab89u;
    ctx.state[2] = 0x98badcfeu;
    ctx.state[3] = 0x10325476u;
    ctx.count = 0;
}

void md5_update(Md5Ctx& ctx, const uint8_t* data, size_t len) {
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
        md5_transform(ctx.state, ctx.buffer);
        i = fill;
    }
    for (; i + 64 <= len; i += 64) md5_transform(ctx.state, data + i);
    if (i < len) std::memcpy(ctx.buffer, data + i, len - i);
}

void md5_final(Md5Ctx& ctx, uint8_t digest[16]) {
    uint8_t bits[8];
    const uint64_t bitcount = ctx.count * 8;
    for (int i = 0; i < 8; ++i) bits[i] = uint8_t((bitcount >> (8 * i)) & 0xff);

    const uint8_t pad = 0x80;
    md5_update(ctx, &pad, 1);
    const uint8_t zero = 0;
    while ((ctx.count & 63ull) != 56ull) md5_update(ctx, &zero, 1);
    md5_update(ctx, bits, 8);
    for (int i = 0; i < 4; ++i) {
        digest[i * 4] = uint8_t(ctx.state[i] & 0xff);
        digest[i * 4 + 1] = uint8_t((ctx.state[i] >> 8) & 0xff);
        digest[i * 4 + 2] = uint8_t((ctx.state[i] >> 16) & 0xff);
        digest[i * 4 + 3] = uint8_t((ctx.state[i] >> 24) & 0xff);
    }
}

} // namespace

std::string file_md5_hex(const fs::path& path, std::uint64_t skip_bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    if (skip_bytes) in.seekg(static_cast<std::streamoff>(skip_bytes), std::ios::beg);
    if (!in) return {};
    Md5Ctx ctx;
    md5_init(ctx);
    std::array<uint8_t, 1 << 16> buf{};
    while (in) {
        in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
        const auto n = size_t(in.gcount());
        if (n) md5_update(ctx, buf.data(), n);
    }
    uint8_t digest[16];
    md5_final(ctx, digest);
    std::ostringstream oss;
    for (uint8_t b : digest)
        oss << std::hex << std::nouppercase << std::setw(2) << std::setfill('0') << int(b);
    return oss.str();
}

std::string file_sha1_hex(const fs::path& path, std::uint64_t skip_bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    if (skip_bytes) in.seekg(static_cast<std::streamoff>(skip_bytes), std::ios::beg);
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

namespace {

// Compact public-domain SHA-256 (FIPS 180-4), adapted for file streaming.
struct Sha256Ctx {
    uint32_t state[8]{};
    uint64_t count = 0;
    uint8_t buffer[64]{};
};

uint32_t ror32(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    static constexpr uint32_t K[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
        0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
        0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
        0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
        0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
        0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = h + S1 + ch + K[i] + w[i];
        const uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void sha256_init(Sha256Ctx& ctx) {
    ctx.state[0] = 0x6a09e667u;
    ctx.state[1] = 0xbb67ae85u;
    ctx.state[2] = 0x3c6ef372u;
    ctx.state[3] = 0xa54ff53au;
    ctx.state[4] = 0x510e527fu;
    ctx.state[5] = 0x9b05688cu;
    ctx.state[6] = 0x1f83d9abu;
    ctx.state[7] = 0x5be0cd19u;
    ctx.count = 0;
}

void sha256_update(Sha256Ctx& ctx, const uint8_t* data, size_t len) {
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
        sha256_transform(ctx.state, ctx.buffer);
        i = fill;
    }
    for (; i + 64 <= len; i += 64) sha256_transform(ctx.state, data + i);
    if (i < len) std::memcpy(ctx.buffer, data + i, len - i);
}

void sha256_final(Sha256Ctx& ctx, uint8_t digest[32]) {
    uint8_t finalcount[8];
    for (int i = 0; i < 8; ++i)
        finalcount[i] = uint8_t((ctx.count * 8) >> ((7 - i) * 8));

    const uint8_t pad = 0x80;
    sha256_update(ctx, &pad, 1);
    const uint8_t zero = 0;
    while ((ctx.count & 63ull) != 56ull) sha256_update(ctx, &zero, 1);
    sha256_update(ctx, finalcount, 8);
    for (int i = 0; i < 32; ++i)
        digest[i] = uint8_t((ctx.state[i >> 2] >> ((3 - (i & 3)) * 8)) & 0xff);
}

} // namespace

std::string file_sha256_hex(const fs::path& path, std::uint64_t skip_bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    if (skip_bytes) in.seekg(static_cast<std::streamoff>(skip_bytes), std::ios::beg);
    if (!in) return {};
    Sha256Ctx ctx;
    sha256_init(ctx);
    std::array<uint8_t, 1 << 16> buf{};
    while (in) {
        in.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
        const auto n = size_t(in.gcount());
        if (n) sha256_update(ctx, buf.data(), n);
    }
    uint8_t digest[32];
    sha256_final(ctx, digest);
    std::ostringstream oss;
    for (uint8_t b : digest)
        oss << std::hex << std::nouppercase << std::setw(2) << std::setfill('0') << int(b);
    return oss.str();
}

} // namespace retcomm
