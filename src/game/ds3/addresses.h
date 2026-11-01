#pragma once
// DarkSoulsIII.exe 1.15.2.0 (preferred base 0x140000000, but DllCharacteristics 0x8160 = DYNAMIC_BASE, so the
// module CAN be relocated: every address below is a PREFERRED-BASE address and must go through rebase()).
// Grades: STATIC = read in the exe on disk (ds3vr2/re/, DS3VR2_PLAN.md "RE"), LIVE = seen in a running game,
// HEADSET = JJ confirmed. The adapter refuses to hook anything unless the exe's PE stamp / size match.
#include <cstdint>
#include <windows.h>

namespace bvr::ds3::addr {

// ---- build check (STATIC 2026-10-30) ----
constexpr uint32_t kSizeOfImage = 0x056F4000; // DarkSoulsIII.exe 1.15.2.0, md5 5d2ae4469c02ebdd99f2187bf9562323
constexpr uint32_t kPeTimestamp = 0x639C4DDD;
constexpr uintptr_t kPreferredBase = 0x140000000;

// The live module base; set once at start-up (adapter.cpp). DS3 has ASLR, Sekiro did not - never use a raw
// constant from this file without rebase().
inline uintptr_t module_base()
{
    static uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return b;
}
inline uintptr_t rebase(uintptr_t preferred) { return preferred - kPreferredBase + module_base(); }
template <class T> inline T* ptr(uintptr_t preferred) { return reinterpret_cast<T*>(rebase(preferred)); }

// ---- camera: the ChrCam director's copy-out seam (STATIC 2026-10-30; v1 proved the same seam on 1.15.0.0) ----
// 0x140510440 ChrCam director (its entry is an Arxan `jmp`, so hook the call SITE, not the entry).
// At 0x140510E6D it copies the active camera [rdi+0x78]: fov/aspect/near/far +0x50..+0x5C, then the matrix
// rows into +0x10..+0x4F. The 5-byte call right after is our seam (rdi = ChrCam).
constexpr uintptr_t ChrCamDirector    = 0x140510440;
constexpr uintptr_t ChrCamCopyOutCall = 0x140510EB5;
constexpr const char* kCopyOutPattern = "0F 28 48 30 66 0F 7F 4F 40 E8 ?? ?? ?? ?? 84 C0 74"; // unique; +9 = the call
constexpr uintptr_t CameraMatrixGet   = 0x1404095F0; // (cam, scratch) -> 4 rows, the helper the director uses
constexpr uintptr_t DirectorCallSiteA = 0x1408F1FC3; // the two `call director` sites (v1 re-pointed these)
constexpr uintptr_t DirectorCallSiteB = 0x1408FA2D5;

// Projection builders used for culling: fov +0x50, aspect +0x54, near +0x58, far +0x5C (2 hits, as in v1).
constexpr const char* kCullPattern = "F3 0F 10 5B 54 F3 0F 10 53 50 BA 01 00 00 00"; // 0x140CED499 / 0x140CED529
constexpr uintptr_t CullProjA = 0x140CED430;
constexpr uintptr_t CullProjB = 0x140CED4C0;

// ---- render side: RendMan and the view list (STATIC 2026-10-30, the AC6 / SKVR2 native-stereo route) ----
constexpr uintptr_t RendManSlot   = 0x144796298; // RendManImp* singleton (written by 0x140D59C99)
constexpr uintptr_t RendManVtable = 0x1429797A0; // .?AVRendManImp@NS_SPRJ@@
constexpr uintptr_t RendManCtor   = 0x140DFC850; // allocates the view-list object (0x20 bytes) into RendMan+8
constexpr uintptr_t RendManDraw   = 0x140DFD270; // RendMan::Draw(rendman); calls render-all-views(RendMan+8, ctx)
constexpr uintptr_t RenderAllViews = 0x1400BFEF0; // (viewList, frameCtx): sorts by ~view+0x10, vfunc +0x18 then +0x38
constexpr uintptr_t AddView        = 0x1400C0230; // (viewList, view) -> bool; refuses duplicates, refcount view+8
constexpr uintptr_t RemoveView     = 0x1400C02D0; // (viewList, view) -> bool
constexpr uintptr_t SceneViewCtor  = 0x1400C30D0; // GXSgSceneFactory slot 0: (factory, desc) -> view
constexpr uintptr_t SceneViewCtor2 = 0x1400C3410; // factory slot 1 (second variant)
constexpr uintptr_t SceneDescDefault = 0x1400C2DA0; // returns the default 0x28-byte view desc
constexpr uintptr_t RendManInit    = 0x140DFEEB0; // builds the main view; main view holder at RendMan+0x340
constexpr uintptr_t RendManInitMainRet = 0x140DFF40C; // return address of the main view's create call
constexpr uintptr_t SceneFactoryVtable = 0x14295BA48; // .?AVGXSgSceneFactory@GXSR@@ (slot 0 = SceneViewCtor)
constexpr uintptr_t GxFrameSlot    = 0x144961AB0; // GX frame object; RendMan::Draw takes [slot]+0x20 as the ctx
constexpr uintptr_t ViewHolderAssign = 0x140D64930; // ref-counted holder = view (RendMan init, r14 = RendMan+0x340)
constexpr size_t    kRendManMainViewHolder = 0x340;

// View object (GXSgCompositeScene) layout, from its vtable slots (STATIC):
//   +0x00 vtable  +0x08 refcount  +0x10 priority (sort key)  +0x18..+0x20 child vector
//   +0x38 render-target set   +0x40 scene   +0x48 camera link   +0xE0 extra
// vtable: slot1 (+0x08) set scene, slot3 (+0x18) get RT set, slot4 (+0x20) set RT set,
//         slot5 (+0x28) set camera link (forwards to children), slot6 (+0x30) set +0xE0, slot7 (+0x38) render.
constexpr uintptr_t VtGXSgCompositeScene = 0x143D4A108;
constexpr uintptr_t VtGXSgBaseSceneImpl  = 0x143D4A318;
constexpr uintptr_t VtGXSgDrawScene      = 0x143D4A360;
constexpr uintptr_t VtGXSgShadowScene    = 0x143D4A378;
constexpr uintptr_t VtGXSgReflectScene   = 0x143D4A278;
constexpr uintptr_t VtGXSgEnvMapScene    = 0x143D4A1D8;
constexpr size_t kViewRefCount = 0x08, kViewPriority = 0x10, kViewChildren = 0x18;
constexpr size_t kViewRtSet = 0x38, kViewScene = 0x40, kViewCamLink = 0x48, kViewExtra = 0xE0;
constexpr size_t kViewVfSetScene = 1, kViewVfGetRt = 3, kViewVfSetRt = 4, kViewVfSetCam = 5,
                 kViewVfSetExtra = 6, kViewVfRender = 7;
constexpr size_t kViewBytes = 0x100;      // the view object's own allocation (view ctor 0x1400C30D0)

// The view-list object (RendMan+8, 0x20 bytes): a vector of View*.  NOTE Sekiro had these at +0x08/+0x10.
constexpr size_t kListBegin = 0x00, kListEnd = 0x08, kListCap = 0x10;

// The camera link the main view is given (RendMan init allocates 0x120 at RendMan+0x20 and inits two 0x90 records):
// record 0 = this frame, record 1 at +0x90 = last frame. Record: rows +0x00/+0x10/+0x20, position +0x30 (w=1),
// +0x50 fov (rad), +0x54 near, +0x58 far, +0x64 aspect (confirm LIVE with `ds camdump`).
constexpr size_t kCamLinkBytes = 0x120, kCamLinkRecord = 0x90;

// The per-view render-target / settings object ("RT set"), allocated 0x4170 bytes by 0x1400C3D90 inside the view
// ctor - so every view has its OWN. Fields RendMan init writes for the main view (same five as Sekiro's):
constexpr uint32_t kRtSize       = 0x4170;
constexpr size_t kRtWindowFlag   = 0x78;    // dword = 1: this view composites to the window (Sekiro rt+0xA0)
constexpr size_t kRtFlagA        = 0x415A;  // byte = 1
constexpr size_t kRtFlagB        = 0x415C;  // byte = 1 (a config bool for the main view)
constexpr size_t kRtClearColour  = 0x6C;    // dword, alpha-forced 0xFF......
constexpr size_t kRtCondByte     = 0x50;    // if (rt[0x50] & 0x40) rt[0x3EC1] = 0
constexpr uint8_t kRtCondBit     = 0x40;
constexpr size_t kRtCondTarget   = 0x3EC1;
constexpr size_t kRtSubObject    = 0x1D0;   // [[rt+0x1D0]+0x6C] = 0xFF000000 as well

// ---- render size and the resolution list (STATIC) ----
constexpr uintptr_t RenderWidthGlobal  = 0x1446601E8;
constexpr uintptr_t RenderHeightGlobal = 0x1446601EC;
constexpr uintptr_t RenderCameraBuild  = 0x140CED670; // reads the two globals, aspect -> cam+0x54, Hor+ widening of +0x50
constexpr uintptr_t AspectRef16x9      = 0x142945248; // the 1.7777778 the builder compares against - LEAVE ALONE
constexpr uintptr_t ResolutionList     = 0x144662D70; // 12 entries of {int w, int h}, 800x450 .. 3840x2160
constexpr int       kResolutionCount   = 12;
// The windowed-size list builder drops any entry larger than the desktop (two `jg`): NOP both, as v1 RESLIST did.
constexpr uintptr_t ResListFilterJgA   = 0x140EF9417;
constexpr uintptr_t ResListFilterJgB   = 0x140EF941C;
constexpr const char* kResListPattern  = "39 77 FC 7F ?? 44 39 3F 7F ??"; // unique, plain on disk; hit = JgA - 3

// ---- frame rate (STATIC; the game's own debug-fps path, a data write with no code patch) ----
// The frame-timer object ("SprjFlipper") is at [FlipperGlobal]. Each frame 0x140EED810 sets its period +0x18;
// if byte +0x358 is set it uses 1 / float +0x354 instead. Writing those two every Present caps the game.
constexpr uintptr_t FlipperGlobal   = 0x14489DD10;
constexpr uintptr_t FrameTimerUpdate = 0x140EED810;
constexpr size_t kFlipperPeriod = 0x18, kFlipperFpsOverride = 0x354, kFlipperFpsEnable = 0x358;

// ---- misc (STATIC) ----
constexpr uintptr_t FieldAreaPattern_Site = 0x1404C8000; // `48 8B 0D ?? ?? ?? ?? 48 85 C9 74 26 44 8B` (First Person Souls)
constexpr uintptr_t VtChrCam        = 0x14279D400;
constexpr uintptr_t VtChrExFollowCam = 0x14279DAD0;
constexpr uintptr_t VtChrAimCam     = 0x14279CA00;
constexpr uintptr_t VtGameRend      = 0x1427D9ED0;

} // namespace bvr::ds3::addr
