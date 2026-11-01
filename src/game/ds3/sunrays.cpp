// DS3VR2 SUNRAYS - see sunrays.h.
#include "sunrays.h"

#include "addresses.h"
#include "eyesize.h"
#include "settings.h"
#include "core/util/log.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>
#include <cstdint>

namespace bvr::ds3::sunrays {
namespace {

constexpr uintptr_t kFocal = 0x141751CB0;   // float __fastcall (PostEffect* self, float scale)
using FocalFn = float(__fastcall*)(void*, float);
FocalFn oFocal = nullptr;
// sun_rays: -1 = automatic (default), otherwise a fixed multiplier (1 = the game's own value, i.e. off).
// Automatic = (16/9) / the eye picture's width-to-height: the god rays are then built as for a 16:9 picture of the same
// height and field of view - the shape the game was tuned for (LIVE sim 2026-10-30: x1 at the VR shape = 1 blur round,
// no boost, no beams; x1.15-x2 = 2 rounds + boost; x4 = 3 rounds and the boost gone again).
std::atomic<float> g_scale{-1.0f};
float auto_scale() {
    unsigned w = 0, h = 0;
    if (!eyesize::full_size(w, h) || !w || !h) return 1.0f;
    const float k = (16.0f / 9.0f) / (static_cast<float>(w) / static_cast<float>(h));
    return k < 1.0f ? 1.0f : k > 3.0f ? 3.0f : k;   // a picture already 16:9 or wider keeps the game's value
}
std::atomic<uint32_t> g_calls{0};
ULONGLONG g_nextLog = 0;

float __fastcall hkFocal(void* self, float arg) {
    const float r = oFocal(self, arg);
    g_calls.fetch_add(1, std::memory_order_relaxed);
    const float set = g_scale.load(std::memory_order_relaxed);
    const float k = set < 0.0f ? auto_scale() : set;
    const ULONGLONG now = GetTickCount64();
    if (now >= g_nextLog && self) {
        g_nextLog = now + 5000;
        const auto* b = static_cast<const uint8_t*>(self);
        __try {
            BVR_LOG("sunrays: focal(%p, %.3f) = %.3f | +B0 %d, +138 %.4f, +1B4 %.4f | x%.2f, %u calls so far", self, arg, r,
                    *reinterpret_cast<const int*>(b + 0xB0), *reinterpret_cast<const float*>(b + 0x138),
                    *reinterpret_cast<const float*>(b + 0x1B4), k, g_calls.load());
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (set >= 0.0f) return r * k;   // a fixed multiplier: exactly what was asked
    // Automatic: also keep the result inside the band where the game builds two blur rounds + the boost (measured
    // 2026-10-30: 0.94 = one round, no beams; 1.08..1.88 = two rounds + boost; 3.77 = three rounds, no boost). With DLSS
    // the picture is smaller and the value lands at the band's edge (1.04), so it is held at 1.3..1.8.
    const float v = r * k;
    return v < 1.3f ? 1.3f : v > 1.8f ? 1.8f : v;
}

} // namespace

void install() {
    g_scale.store(settings::f("sun_rays", -1.0f));
    void* fn = reinterpret_cast<void*>(addr::rebase(kFocal));
    const MH_STATUS s = MH_CreateHook(fn, reinterpret_cast<void*>(&hkFocal), reinterpret_cast<void**>(&oFocal));
    const MH_STATUS e = s == MH_OK ? MH_EnableHook(fn) : s;
    BVR_LOG("sunrays: god-ray focal length at %p hooked: %s / %s", fn, MH_StatusToString(s), MH_StatusToString(e));
}
void set_scale(float k) {
    g_scale.store(k > 0.0f ? k : -1.0f);
    settings::set_f("sun_rays", g_scale.load());
    if (g_scale.load() < 0.0f) BVR_LOG("sunrays: automatic - focal length x%.2f (16:9 over the eye's shape)", auto_scale());
    else BVR_LOG("sunrays: focal length x%.2f", g_scale.load());
}
float scale() { return g_scale.load(); }

} // namespace bvr::ds3::sunrays
