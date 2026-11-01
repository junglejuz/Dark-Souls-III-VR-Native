// BLACKWATCH - see black_watch.h.
#include "core/vr/black_watch.h"

#include "core/gfx/gfx_hud.h"
#include "core/util/log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace bvr::vr::black_watch {
namespace {

constexpr int kRing = 8;   // read back 8 samples later (~45 ms at 180 presents/s) - long done on the card
constexpr int kBox = 16;   // 3 x 3 boxes of 16 x 16 pixels
constexpr int kGrid = 3;
constexpr unsigned kMaxLogs = 300;

struct Stream {
    const char* name;
    D3D11_TEXTURE2D_DESC desc{};
    ID3D11Texture2D* ring[kRing] = {};
    uint64_t present[kRing] = {};
    uint32_t facts[kRing] = {}; // gfx_hud::pass_facts() of the sampled pass
    bool used[kRing] = {};
    unsigned next = 0;
    float hist[6] = {};
    unsigned histN = 0;
    bool inDark = false;
    unsigned darkRun = 0;
    uint64_t darkFrom = 0;
    float darkRef = 0;
    std::atomic<uint32_t> samples{0}, dark{0}, missed{0};
};
Stream g_s[4] = {{"game picture L (before DLSS)"}, {"game picture R (before DLSS)"},
                 {"headset image L (finished)"}, {"headset image R (finished)"}};
std::atomic<bool> g_on{true};
std::atomic<unsigned> g_logs{0};

void facts_text(uint32_t f, char* out, size_t n);

bool readable(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return true;
    default:
        return false;
    }
}
// Brightest channel of one pixel, 0..255 (channel order does not matter for that).
float level(DXGI_FORMAT f, const unsigned char* p) {
    if (f == DXGI_FORMAT_R10G10B10A2_UNORM || f == DXGI_FORMAT_R10G10B10A2_TYPELESS) {
        const uint32_t v = *reinterpret_cast<const uint32_t*>(p);
        return float((std::max)({v & 1023u, (v >> 10) & 1023u, (v >> 20) & 1023u})) * (255.0f / 1023.0f);
    }
    return float((std::max)({p[0], p[1], p[2]}));
}

void release(Stream& s) {
    for (int k = 0; k < kRing; ++k) {
        if (s.ring[k]) s.ring[k]->Release();
        s.ring[k] = nullptr;
        s.used[k] = false;
    }
    s.desc = D3D11_TEXTURE2D_DESC{};
    s.histN = 0;
    s.inDark = false;
}

// FLICKERTRACE (2026-10-29): blackwatch only reports a frame under 35 % of normal - far too coarse for the 20 %
// lighting swing JJ sees. This logs the per-frame brightness of each stream for N frames so the pattern (strict
// alternation = two renders taking turns on one buffer; a slow ring = a feedback loop) can be read off.
std::atomic<int> g_trace{0};

void judge(Stream& s, float v, uint64_t present, uint32_t facts) {
    s.samples.fetch_add(1, std::memory_order_relaxed);
    if (g_trace.load(std::memory_order_relaxed) > 0) {
        BVR_LOG("[flicker] %-28s present %llu brightness %.2f", s.name, (unsigned long long)present, v);
        if (g_trace.fetch_sub(1) == 1) BVR_LOG("[flicker] trace done");
    }
    float ref = 0;
    if (s.histN >= 4) {
        float h[6];
        const unsigned n = (std::min)(s.histN, 6u);
        std::copy(s.hist, s.hist + n, h);
        std::sort(h, h + n);
        ref = h[n / 2];
    }
    const bool dark = ref >= 10.0f && v < 0.35f * ref;
    if (dark) {
        if (!s.inDark) {
            s.inDark = true;
            s.darkRun = 0;
            s.darkFrom = present;
            s.darkRef = ref;
            s.dark.fetch_add(1, std::memory_order_relaxed);
            if (g_logs.fetch_add(1) < kMaxLogs) {
                char ft[160];
                facts_text(facts, ft, sizeof ft);
                BVR_LOG("[blackwatch] DARK %s: brightness %.1f, usual %.1f (present %llu) | that pass: %s", s.name, v, ref,
                        (unsigned long long)present, ft);
            }
        }
        ++s.darkRun;
        return; // dark frames never become the reference
    }
    if (s.inDark) {
        s.inDark = false;
        if (g_logs.fetch_add(1) < kMaxLogs)
            BVR_LOG("[blackwatch] %s back to normal after %u dark frame(s) (presents %llu-%llu, usual %.1f)", s.name,
                    s.darkRun, (unsigned long long)s.darkFrom, (unsigned long long)present, s.darkRef);
    }
    if (s.histN < 6) s.hist[s.histN++] = v;
    else { std::copy(s.hist + 1, s.hist + 6, s.hist); s.hist[5] = v; }
}

void read_slot(ID3D11DeviceContext* ctx, Stream& s, int k) {
    if (!s.used[k]) return;
    s.used[k] = false;
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT hr = ctx->Map(s.ring[k], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (FAILED(hr)) { s.missed.fetch_add(1, std::memory_order_relaxed); return; }
    double sum = 0;
    const int side = kBox * kGrid;
    for (int y = 0; y < side; ++y) {
        const unsigned char* row = static_cast<const unsigned char*>(m.pData) + size_t(y) * m.RowPitch;
        for (int x = 0; x < side; ++x) sum += level(s.desc.Format, row + size_t(x) * 4);
    }
    ctx->Unmap(s.ring[k], 0);
    judge(s, float(sum / double(side * side)), s.present[k], s.facts[k]);
}

void sample(ID3D11DeviceContext* ctx, Stream& s, ID3D11Texture2D* tex, uint64_t present) {
    D3D11_TEXTURE2D_DESC d{};
    tex->GetDesc(&d);
    if (d.Width != s.desc.Width || d.Height != s.desc.Height || d.Format != s.desc.Format) {
        release(s);
        if (!readable(d.Format) || d.SampleDesc.Count != 1 || d.Width < 64 || d.Height < 64) return;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = sd.Height = kBox * kGrid;
        sd.MipLevels = sd.ArraySize = 1;
        sd.Format = d.Format;
        sd.SampleDesc = {1, 0};
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        bool ok = true;
        for (int k = 0; k < kRing && ok; ++k) ok = SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &s.ring[k]));
        dev->Release();
        if (!ok) { release(s); return; }
        s.desc = d;
    }
    const int k = int(s.next++ % kRing);
    read_slot(ctx, s, k); // the sample taken kRing frames ago
    for (int gy = 0; gy < kGrid; ++gy)
        for (int gx = 0; gx < kGrid; ++gx) {
            const UINT x = UINT(d.Width * (2 * gx + 1) / (2 * kGrid)) - kBox / 2;
            const UINT y = UINT(d.Height * (2 * gy + 1) / (2 * kGrid)) - kBox / 2;
            const D3D11_BOX box{x, y, 0, x + kBox, y + kBox, 1};
            ctx->CopySubresourceRegion(s.ring[k], 0, UINT(gx * kBox), UINT(gy * kBox), 0, tex, 0, &box);
        }
    s.present[k] = present;
    s.facts[k] = bvr::gfx_hud::pass_facts();
    s.used[k] = true;
}

// SHEETWATCH - see black_watch.h.
constexpr int kSGrid = 5;
constexpr int kSBoxes = kSGrid * kSGrid;
constexpr unsigned kMaxSheetLogs = 80;
struct Sheet {
    D3D11_TEXTURE2D_DESC desc{};
    ID3D11Texture2D* ring[kRing] = {};
    uint64_t present[kRing] = {};
    uint32_t facts[kRing] = {};
    uint64_t tick[kRing] = {}; // GetTickCount64 % 1e6 when sent (matches the HUD ring's t)
    bool used[kRing] = {};
    unsigned next = 0;
    float ref[kSBoxes * 2] = {}; // per box: brightness (brightest channel), then coverage (alpha)
    bool hasRef = false;
    unsigned off = 0;            // samples in a row unlike the reference
    uint64_t offFrom = 0, offTick = 0;
    uint32_t offFacts = 0;
    unsigned offBoxes = 0;
    float offLum = 0, offA = 0, refLum = 0, refA = 0;
    std::atomic<uint32_t> samples{0}, blinks{0}, changes{0}, missing{0}, kept{0}, missed{0};
};
Sheet g_sheet;
std::atomic<unsigned> g_sheetLogs{0};

void facts_text(uint32_t f, char* out, size_t n) {
    snprintf(out, n, "%u pieces%s%s%s%s%s%s, %s", f & 0xFFFu, (f & bvr::gfx_hud::kFactUndone) ? ", EARLYUI undo" : "",
             (f & bvr::gfx_hud::kFactWiped) ? " left the sheet EMPTY" : "", (f & bvr::gfx_hud::kFactHeld) ? ", last full sheet put back" : "",
             (f & bvr::gfx_hud::kFactRedirect) ? "" : ", UI redirect off",
             (f & bvr::gfx_hud::kFactPausePic) ? ", PAUSEPIC kept a picture off" : "",
             (f & bvr::gfx_hud::kFactNoBlit) ? ", no scene blit (EARLYUI)" : "",
             (f & bvr::gfx_hud::kFactMap) ? "map-type menu" : (f & bvr::gfx_hud::kFactMenu) ? "pause-type menu" : "no menu");
}

void judge_sheet(const float* v, uint64_t present, uint32_t facts, uint64_t tick) {
    Sheet& s = g_sheet;
    s.samples.fetch_add(1, std::memory_order_relaxed);
    float lum = 0, a = 0;
    for (int i = 0; i < kSBoxes; ++i) { lum += v[i]; a += v[kSBoxes + i]; }
    lum /= kSBoxes; a /= kSBoxes;
    if (!s.hasRef) { std::copy(v, v + kSBoxes * 2, s.ref); s.refLum = lum; s.refA = a; s.hasRef = true; return; }
    unsigned boxes = 0;
    float sumD = 0;
    for (int i = 0; i < kSBoxes; ++i) {
        const float dl = fabsf(v[i] - s.ref[i]), da = fabsf(v[kSBoxes + i] - s.ref[kSBoxes + i]);
        if (dl > 24.0f || da > 24.0f) ++boxes;
        sumD += dl + da;
    }
    const bool unlike = boxes >= 3 || sumD / (kSBoxes * 2) > 8.0f;
    if (unlike) {
        if (!s.off) { s.offFrom = present; s.offTick = tick; s.offFacts = facts; s.offBoxes = boxes; s.offLum = lum; s.offA = a; }
        if (++s.off > 6) { // a real change of screen, not a blink
            s.changes.fetch_add(1, std::memory_order_relaxed);
            std::copy(v, v + kSBoxes * 2, s.ref); s.refLum = lum; s.refA = a;
            s.off = 0;
        }
        return;
    }
    if (s.off) {
        s.blinks.fetch_add(1, std::memory_order_relaxed);
        if (g_sheetLogs.fetch_add(1) < kMaxSheetLogs) {
            char f[160];
            facts_text(s.offFacts, f, sizeof f);
            BVR_LOG("[sheetwatch] BLINK: the menu sheet was different for %u frame(s) from present %llu (t%llu), then came "
                    "back - %u of 25 areas changed, brightness %.0f -> %.0f, coverage %.0f -> %.0f | that pass: %s",
                    s.off, (unsigned long long)s.offFrom, (unsigned long long)s.offTick, s.offBoxes, s.refLum, s.offLum,
                    s.refA, s.offA, f);
        }
        // the HUD ring (last ~1500 passes, every piece) for the first 6 big blinks: find the blink pass by its t
        static unsigned s_ringSaves = 0;
        if (s_ringSaves < 6 && s.offBoxes >= 8) { // big changes only (the pause dark box: 20 of 25 areas)
            ++s_ringSaves;
            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%s\\captures", bvr::log::data_dir());
            CreateDirectoryW(path, nullptr);
            swprintf_s(path, L"%s\\captures\\sheetblink_t%llu.txt", bvr::log::data_dir(), (unsigned long long)s.offTick);
            if (bvr::gfx_hud::save_hud_ring(path)) BVR_LOG("[sheetwatch] HUD ring saved: %ls", path);
        }
        s.off = 0;
    }
    std::copy(v, v + kSBoxes * 2, s.ref); s.refLum = lum; s.refA = a; // follow slow changes
}

void read_sheet_slot(ID3D11DeviceContext* ctx, int k) {
    Sheet& s = g_sheet;
    if (!s.used[k]) return;
    s.used[k] = false;
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(s.ring[k], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
        s.missed.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    float v[kSBoxes * 2] = {};
    const bool tenBit = s.desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM || s.desc.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS;
    for (int y = 0; y < kBox * kSGrid; ++y) {
        const unsigned char* row = static_cast<const unsigned char*>(m.pData) + size_t(y) * m.RowPitch;
        for (int x = 0; x < kBox * kSGrid; ++x) {
            const unsigned char* p = row + size_t(x) * 4;
            const int b = (y / kBox) * kSGrid + x / kBox;
            v[b] += level(s.desc.Format, p);
            v[kSBoxes + b] += tenBit ? float(*reinterpret_cast<const uint32_t*>(p) >> 30) * 85.0f : float(p[3]);
        }
    }
    ctx->Unmap(s.ring[k], 0);
    for (float& x : v) x /= float(kBox * kBox);
    judge_sheet(v, s.present[k], s.facts[k], s.tick[k]);
}

} // namespace

void on_sheet(ID3D11DeviceContext* ctx, ID3D11Texture2D* sheet, uint64_t present, uint32_t facts) {
    if (!on() || !ctx || !sheet) return;
    Sheet& s = g_sheet;
    D3D11_TEXTURE2D_DESC d{};
    sheet->GetDesc(&d);
    if (d.Width != s.desc.Width || d.Height != s.desc.Height || d.Format != s.desc.Format) {
        for (int k = 0; k < kRing; ++k) { if (s.ring[k]) s.ring[k]->Release(); s.ring[k] = nullptr; s.used[k] = false; }
        s.desc = D3D11_TEXTURE2D_DESC{};
        s.hasRef = false; s.off = 0;
        if (!readable(d.Format) || d.SampleDesc.Count != 1 || d.Width < 128 || d.Height < 128) return;
        ID3D11Device* dev = nullptr;
        ctx->GetDevice(&dev);
        if (!dev) return;
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = sd.Height = kBox * kSGrid;
        sd.MipLevels = sd.ArraySize = 1;
        sd.Format = d.Format;
        sd.SampleDesc = {1, 0};
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        bool ok = true;
        for (int k = 0; k < kRing && ok; ++k) ok = SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &s.ring[k]));
        dev->Release();
        if (!ok) { for (auto*& t : s.ring) { if (t) t->Release(); t = nullptr; } return; }
        s.desc = d;
    }
    const int k = int(s.next++ % kRing);
    read_sheet_slot(ctx, k);
    for (int gy = 0; gy < kSGrid; ++gy)
        for (int gx = 0; gx < kSGrid; ++gx) {
            const UINT x = UINT(d.Width * (2 * gx + 1) / (2 * kSGrid)) - kBox / 2;
            const UINT y = UINT(d.Height * (2 * gy + 1) / (2 * kSGrid)) - kBox / 2;
            const D3D11_BOX box{x, y, 0, x + kBox, y + kBox, 1};
            ctx->CopySubresourceRegion(s.ring[k], 0, UINT(gx * kBox), UINT(gy * kBox), 0, sheet, 0, &box);
        }
    s.present[k] = present;
    s.facts[k] = facts;
    s.tick[k] = GetTickCount64() % 1000000ULL;
    s.used[k] = true;
}

void on_sheet_missing(uint64_t present, uint32_t facts, const char* why, bool kept) {
    if (!on()) return;
    g_sheet.missing.fetch_add(1, std::memory_order_relaxed);
    if (kept) g_sheet.kept.fetch_add(1, std::memory_order_relaxed);
    // intentional gaps (bats, fades, wipes) are only counted - they filled the log budget in the first run
    const bool notable = kept || (why && (!strncmp(why, "stale", 5) || !strncmp(why, "sheet image", 11)));
    if (notable && g_sheetLogs.fetch_add(1) < kMaxSheetLogs) {
        char f[160];
        facts_text(facts, f, sizeof f);
        BVR_LOG("[sheetwatch] frame without its menu sheet at present %llu (%s) - %s | that pass: %s",
                (unsigned long long)present, why ? why : "-",
                kept ? "SHEETKEEP: the last sheet shown again" : "sent without it", f);
    }
}

void trace(int samples) { g_trace.store(samples); BVR_LOG("[flicker] tracing the next %d brightness samples", samples); }

void set_on(bool on) {
    g_on.store(on);
    BVR_LOG("[blackwatch] %s", on ? "on" : "off");
}
bool on() { return g_on.load(std::memory_order_relaxed); }

void on_game(ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer, int sign, uint64_t present) {
    if (!on() || !ctx || !backbuffer || sign == 0) return;
    sample(ctx, g_s[sign < 0 ? 0 : 1], backbuffer, present);
}

void on_final(ID3D11DeviceContext* ctx, ID3D11Texture2D* image, int eye, uint64_t present) {
    if (!on() || !ctx || !image || eye < 0 || eye > 1) return;
    sample(ctx, g_s[2 + eye], image, present);
}

void status_line(char* out, size_t n) {
    snprintf(out, n, "BLACKWATCH %d: dark events game L/R %u/%u, headset L/R %u/%u | samples %u/%u/%u/%u, not ready %u | "
             "SHEETWATCH: sheets %u, blinks %u, screen changes %u, frames without sheet %u (kept %u), not ready %u",
             on() ? 1 : 0, g_s[0].dark.load(), g_s[1].dark.load(), g_s[2].dark.load(), g_s[3].dark.load(),
             g_s[0].samples.load(), g_s[1].samples.load(), g_s[2].samples.load(), g_s[3].samples.load(),
             g_s[0].missed.load() + g_s[1].missed.load() + g_s[2].missed.load() + g_s[3].missed.load(),
             g_sheet.samples.load(), g_sheet.blinks.load(), g_sheet.changes.load(), g_sheet.missing.load(),
             g_sheet.kept.load(), g_sheet.missed.load());
}

} // namespace bvr::vr::black_watch
