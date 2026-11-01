#pragma once
// DS3VR2 DLSS modes and presets - ONE table, read by the panel, by eyesize (which sizes the game's render in
// DllMain, before any of the DLSS code may run) and by dlss.cpp (which creates the NGX feature). They used to keep
// separate tables and the panel's names had drifted from what the numbers meant.
//
// Modes are NVIDIA's conventional ones, in the order every game's menu shows them (JJ 2026-10-30: "a pill slider
// with conventional DLSS settings"). Scales are NVIDIA's published render-scale per axis.
// Presets are the ones the SDK in third_party/ngx still recommends (nvsdk_ngx_defs.h): A-D were removed, E and F
// are deprecated, G / N / O "do not use" - so only Default, J, K, L and M are offered.

namespace bvr::ds3::dlss_modes {

struct Mode { const char* name; const char* hint; float scale; };
constexpr int kModeCount = 6;
inline const Mode kModes[kModeCount] = {
    {"Off",               "the game draws every pixel itself",                                1.000f},
    {"DLAA",              "full size, cleanest edges - the slowest",                          1.000f},
    {"Quality",           "renders at two thirds of the size, rebuilt to full",               0.667f},
    {"Balanced",          "renders at a bit over half the size",                              0.580f},
    {"Performance",       "renders at half the size - the usual choice for VR",               0.500f},
    {"Ultra Performance", "renders at a third of the size - fastest, softest",                0.333f},
};

struct Preset { const char* name; const char* hint; int ngx; };
constexpr int kPresetCount = 5;
inline const Preset kPresets[kPresetCount] = {
    {"Default", "NVIDIA picks per mode (K for DLAA / Quality / Balanced, M for Performance, L for Ultra Performance)", 0},
    {"J",       "transformer model - a little less ghosting, a little more shimmer than K",                           10},
    {"K",       "transformer model - NVIDIA's best image quality, costs a little more",                               11},
    {"L",       "NVIDIA's model for Ultra Performance",                                                                12},
    {"M",       "NVIDIA's model for Performance",                                                                      13},
};

inline float scale_of(int mode) { return mode >= 0 && mode < kModeCount ? kModes[mode].scale : 1.0f; }
inline int preset_index(int ngx) { for (int i = 0; i < kPresetCount; ++i) if (kPresets[i].ngx == ngx) return i; return 0; }

// The old `dlss` key (before 2026-10-30) meant 1 DLAA, 2 an "ultra quality" 0.77, 3 Quality 0.667, 4 Balanced
// 0.58, 5 Performance 0.5. Mapped to the conventional mode of the same or nearest scale, once, into `dlss_mode`.
inline int from_old_key(int old) {
    static const int kMap[6] = {0, 1, 2, 2, 3, 4};
    return old >= 0 && old < 6 ? kMap[old] : 0;
}

} // namespace bvr::ds3::dlss_modes
