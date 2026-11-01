// DS3VR2 TILEFIX - see tilefix.h.
#include "tilefix.h"

#include "settings.h"
#include "core/util/log.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace bvr::ds3::tilefix {
namespace {

constexpr uint64_t kHashes[2] = {0x6a54371b9235b4a4ull, 0xcbc6e4a6512db67cull};
// V12c: a THIRD light-tile culling shader (32x32 tiles, line lights) with the same centred-lens rays - found by a static
// search of the dump (2026-10-31); its per-tile scale sits in r2.x / r2.y instead of r2.y / r2.z.
constexpr uint64_t kHash3 = 0xf08aa13589a07306ull;
// mad r3.zw, cb0[7].xxxy, r0.yyyy, -r2.zzzw
constexpr uint32_t kAfter3[] = {0x0B000032, 0x001000C2, 3, 0x00208406, 0, 7, 0x00100556, 0, 0x80100EA6, 0x41, 2};
// mode 1: mad r3.z, r2.xxxx, cb0[13].zzzz, r3.zzzz / mad r3.w, r2.yyyy, -cb0[14].zzzz, r3.wwww
constexpr uint32_t kNew31[] = {0x0A000032, 0x00100042, 3, 0x00100006, 2, 0x00208AA6, 0, 13, 0x00100AA6, 3,
                               0x0B000032, 0x00100082, 3, 0x00100556, 2, 0x80208AA6, 0x00000041, 0, 14, 0x00100FF6, 3};
constexpr uint32_t kNew32[] = {0x0B000032, 0x00100042, 3, 0x00100006, 2, 0x80208AA6, 0x00000041, 0, 13, 0x00100AA6, 3,
                               0x0A000032, 0x00100082, 3, 0x00100556, 2, 0x00208AA6, 0, 14, 0x00100FF6, 3};
// mad r3.zw, cb0[7].xxxy, r2.xxxx, -r3.xxxy  (read from both dumped shaders)
constexpr uint32_t kAfter[] = {0x0B000032, 0x001000C2, 3, 0x00208406, 0, 7, 0x00100006, 2, 0x80100406, 0x41, 3};
// mode 1: mad r3.z, r2.yyyy, cb0[13].zzzz, r3.zzzz / mad r3.w, r2.zzzz, -cb0[14].zzzz, r3.wwww
constexpr uint32_t kNew1[] = {0x0A000032, 0x00100042, 3, 0x00100556, 2, 0x00208AA6, 0, 13, 0x00100AA6, 3,
                              0x0B000032, 0x00100082, 3, 0x00100AA6, 2, 0x80208AA6, 0x00000041, 0, 14, 0x00100FF6, 3};
// mode 2: the same with the signs flipped (-cb0[13].z, +cb0[14].z)
constexpr uint32_t kNew2[] = {0x0B000032, 0x00100042, 3, 0x00100556, 2, 0x80208AA6, 0x00000041, 0, 13, 0x00100AA6, 3,
                              0x0A000032, 0x00100082, 3, 0x00100AA6, 2, 0x00208AA6, 0, 14, 0x00100FF6, 3};

// ---- DXBC checksum: MD5 rounds with the container's own padding (dxbc_patch.py, matches all 5047 dumped shaders) ----
void md5_block(uint32_t st[4], const uint8_t* p) {
    static const uint32_t S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                   4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    static uint32_t K[64];
    static const bool init = [] { for (int i = 0; i < 64; ++i) K[i] = static_cast<uint32_t>(static_cast<uint64_t>(fabs(sin(i + 1.0)) * 4294967296.0)); return true; }();
    (void)init;
    uint32_t M[16];
    memcpy(M, p, 64);
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f; int g;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7 * i) % 16; }
        f = f + a + K[i] + M[g];
        a = d; d = c; c = b;
        b = b + ((f << S[i]) | (f >> (32 - S[i])));
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}
void dxbc_checksum(std::vector<uint8_t>& blob) {
    const uint8_t* data = blob.data() + 20;
    const uint32_t n = static_cast<uint32_t>(blob.size() - 20), bits = n * 8;
    uint32_t st[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const uint32_t full = n & ~63u;
    for (uint32_t i = 0; i < full; i += 64) md5_block(st, data + i);
    const uint32_t rest = n - full;
    uint8_t blk[64] = {};
    const uint32_t tail = (bits >> 2) | 1;
    if (rest >= 56) {
        memcpy(blk, data + full, rest); blk[rest] = 0x80;
        md5_block(st, blk);
        memset(blk, 0, 64); memcpy(blk, &bits, 4); memcpy(blk + 60, &tail, 4);
        md5_block(st, blk);
    } else {
        memcpy(blk, &bits, 4); memcpy(blk + 4, data + full, rest); blk[4 + rest] = 0x80; memcpy(blk + 60, &tail, 4);
        md5_block(st, blk);
    }
    memcpy(blob.data() + 4, st, 16);
}

uint32_t rd(const std::vector<uint8_t>& b, size_t o) { uint32_t v; memcpy(&v, b.data() + o, 4); return v; }
void wr(std::vector<uint8_t>& b, size_t o, uint32_t v) { memcpy(b.data() + o, &v, 4); }

// Insert `ins` after the first exact `after` sequence of the SHEX/SHDR chunk; every size and the checksum fixed.
bool patch(const void* code, size_t len, const uint32_t* ins, size_t nIns, std::vector<uint8_t>& out,
           const uint32_t* after = kAfter, size_t afterBytes = sizeof(kAfter)) {
    out.assign(static_cast<const uint8_t*>(code), static_cast<const uint8_t*>(code) + len);
    if (len < 32 || memcmp(out.data(), "DXBC", 4) != 0) return false;
    const uint32_t nChunks = rd(out, 28);
    if (32 + 4ull * nChunks > len) return false;
    for (uint32_t ci = 0; ci < nChunks; ++ci) {
        const uint32_t off = rd(out, 32 + 4 * ci);
        if (off + 8 > len || (memcmp(out.data() + off, "SHEX", 4) != 0 && memcmp(out.data() + off, "SHDR", 4) != 0)) continue;
        const uint32_t size = rd(out, off + 4), body = off + 8;
        if (body + size > len) return false;
        const size_t nTok = size / 4, nAfter = afterBytes / 4;
        for (size_t i = 2; i + nAfter <= nTok; ++i) {
            if (memcmp(out.data() + body + 4 * i, after, afterBytes) != 0) continue;
            const size_t at = body + 4 * (i + nAfter), add = 4 * nIns;
            out.insert(out.begin() + at, reinterpret_cast<const uint8_t*>(ins), reinterpret_cast<const uint8_t*>(ins) + add);
            wr(out, off + 4, size + static_cast<uint32_t>(add));
            wr(out, body + 4, rd(out, body + 4) + static_cast<uint32_t>(nIns));
            for (uint32_t cj = ci + 1; cj < nChunks; ++cj) wr(out, 32 + 4 * cj, rd(out, 32 + 4 * cj) + static_cast<uint32_t>(add));
            wr(out, 24, static_cast<uint32_t>(out.size()));
            dxbc_checksum(out);
            return true;
        }
        return false;
    }
    return false;
}

struct Pair { ID3D11ComputeShader* orig; ID3D11ComputeShader* fixed[2]; uint64_t hash; };
std::mutex g_mx;
Pair g_pairs[8]; int g_n = 0;
std::atomic<int> g_mode{1};
std::atomic<uint32_t> g_swapped{0}, g_failed{0};

} // namespace

void on_created(ID3D11Device* d, uint64_t hash, const void* code, size_t len, ID3D11ComputeShader* orig, CreateCSFn create) {
    if (hash != kHashes[0] && hash != kHashes[1] && hash != kHash3) return;
    Pair p{orig, {nullptr, nullptr}, hash};
    const bool third = hash == kHash3;
    const uint32_t* news[2] = {third ? kNew31 : kNew1, third ? kNew32 : kNew2};
    const size_t lens[2] = {third ? sizeof(kNew31) / 4 : sizeof(kNew1) / 4, third ? sizeof(kNew32) / 4 : sizeof(kNew2) / 4};
    for (int k = 0; k < 2; ++k) {
        std::vector<uint8_t> blob;
        HRESULT hr = E_FAIL;
        if (patch(code, len, news[k], lens[k], blob, third ? kAfter3 : kAfter, third ? sizeof(kAfter3) : sizeof(kAfter))) hr = create(d, blob.data(), blob.size(), nullptr, &p.fixed[k]);
        if (FAILED(hr)) { p.fixed[k] = nullptr; g_failed.fetch_add(1); }
        BVR_LOG("tilefix: light-tile culling shader %016llx - corrected copy %d %s (hr 0x%08lX, %zu -> %zu bytes)",
                static_cast<unsigned long long>(hash), k + 1, p.fixed[k] ? "built" : "NOT built", static_cast<unsigned long>(hr), len,
                blob.size());
    }
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_n < 8) g_pairs[g_n++] = p;
}

ID3D11ComputeShader* swap(ID3D11ComputeShader* sh) {
    const int m = g_mode.load(std::memory_order_relaxed);
    if (!sh || m < 1 || m > 2 || !g_n) return sh;
    for (int i = 0; i < g_n; ++i)
        if (g_pairs[i].orig == sh && g_pairs[i].fixed[m - 1]) { g_swapped.fetch_add(1, std::memory_order_relaxed); return g_pairs[i].fixed[m - 1]; }
    return sh;
}

void set_mode(int m) {
    g_mode.store(m < 0 || m > 2 ? 1 : m);
    settings::set_i("tilefix", g_mode.load());
    BVR_LOG("tilefix: mode %d (%s)", g_mode.load(), g_mode.load() == 0 ? "the game's light-tile shaders" :
            g_mode.load() == 1 ? "corrected for each eye's off-centre lens" : "corrected, signs flipped (test)");
}
int mode() { return g_mode.load(); }

const char* status() {
    static char s[300];
    snprintf(s, sizeof(s), "tilefix: mode %d | shaders found %d, copies that failed %u | binds swapped %u", g_mode.load(), g_n,
             g_failed.load(), g_swapped.load());
    return s;
}

} // namespace bvr::ds3::tilefix
