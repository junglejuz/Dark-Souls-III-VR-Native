#pragma once
// DS3VR2 native stereo (M3) - the engine's own view list (AC6 route; RE_NOTES section 4).
//
// Build STEREOPROBE-1 (2026-10-28) is READ-ONLY: it proves what the static RE suggests before anything is changed
// (playbook RE-04 frame boundary by ownership, RE-06 consumer census, RE-08 zero-delta control first):
//   - render-all-views (0x1401F7F80) calls per Present and views per call, on which thread, how long;
//   - the list's elements: object, vtable, RTTI class name, the priority dword at +0x10, refcount +8;
//   - every AddView / RemoveView (0x1401F8370 / 0x1401F8410) with its caller;
//   - every scene-view construction (0x1401FB950) with caller, ctx and the first 64 bytes of its desc - the main view's
//     desc is kept for OWNVIEW (M3 step 3).
// Switch: settings stereo_probe (default 1), command `ds views` dumps the list now.
struct IDXGISwapChain;

namespace bvr::ds3::stereo {

bool install();                 // after the build check (adapter.cpp)
void save_pictures(IDXGISwapChain* sc, const wchar_t* dir); // capture: each view's finished picture as BMP
void on_present(IDXGISwapChain* sc);
void draw_ui();                 // TESTS drawer rows
void draw_stereo_ui();          // STEREO RENDERING drawer: true 3D switch (STEREOVIEW-3)
bool handle_command(const char* cmd, const char* args);
void links(void*& mainLink, void*& ownLink);   // OFFAXIS: the game view's (left eye) and ours (right eye)
const char* dlss_status();      // M6: what reached DLSS this run (inputs, upscaled frames per eye)

} // namespace bvr::ds3::stereo
