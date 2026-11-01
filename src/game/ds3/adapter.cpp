// DS3VR2 game adapter for DarkSoulsIII.exe 1.6.0.0: build check, engine hooks (camera, stereo probe), the in-headset panel in
// the Batman layout (scene.cpp draw_ui: status row, plain-word drawers, one TESTS drawer at the bottom - memory
// panel-labels-plain-no-bad-switches), commands (`ds ...` in DS3VR2\command.txt), captures (F2 / panel).
#include "game/igame_adapter.h"

#include "addresses.h"
#include "camera.h"
#include "dlss.h"
#include "picsplit.h"
#include "shadowres.h"
#include "contact.h"
#include "eyesize.h"
#include "dlss_modes.h"
#include "framerate.h"
#include "core/vr/eye_burst.h"
#include "sunrays.h"
#include "offaxis.h"
#include "lightgrid.h"
#include "tilefix.h"
#include "afw.h"
#include "focus.h"
#include "core/gfx/hud_capture.h"
#include "offline.h"
#include "markers.h"
#include "settings.h"
#include "stereo.h"

#include "core/framework/command.h"
#include "core/input/dinput_block.h"
#include "core/input/xinput_bridge.h"
#include "core/ui/overlay.h"
#include "core/util/log.h"
#include "core/vr/openxr_runtime.h"

#include <windows.h>
#include <dxgi.h>
#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bvr::ds3 {
namespace {

constexpr float kPi = 3.14159265f;
std::atomic<bool> g_buildOk{false}, g_captureReq{false};
int g_poseDelay = 1;           // presented picture = camera update this many updates back (re-measure under native stereo)
float g_resScale = 1.0f;
// TEST: the head-tracked game camera with the same picture in both eyes (`mono = 1`). 3D is the default since
// 2026-10-30 (stereo.cpp), and when 3D is on it overrides this.
bool g_mono = false;

bool build_ok() {
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<uint8_t*>(dos) + dos->e_lfanew);
    const bool ok = nt->FileHeader.TimeDateStamp == addr::kPeTimestamp && nt->OptionalHeader.SizeOfImage == addr::kSizeOfImage;
    BVR_LOG("adapter: DarkSoulsIII.exe pe-timestamp 0x%08X size 0x%08X, module base %p - %s", nt->FileHeader.TimeDateStamp,
            nt->OptionalHeader.SizeOfImage, reinterpret_cast<void*>(addr::module_base()),
            ok ? "1.15.2.0, supported" : "NOT the supported 1.15.2.0 build: engine hooks off, flat screen only");
    return ok;
}

// The game reads its pad through the import slot XINPUT1_3 ordinal 2 (XInputGetState; v1). Point it at the bridge, so
// the panel can hide the pad from the game while it is open (x360ce's xinput1_3.dll stays in the chain).
void hook_game_pad() {
    HMODULE exe = GetModuleHandleW(nullptr);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(exe);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<BYTE*>(exe) + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(reinterpret_cast<BYTE*>(exe) + dir.VirtualAddress);
         dir.VirtualAddress && imp->Name; ++imp) {
        const char* dll = reinterpret_cast<const char*>(exe) + imp->Name;
        if (_strnicmp(dll, "xinput", 6) != 0 || !imp->OriginalFirstThunk) continue;
        auto ft = reinterpret_cast<IMAGE_THUNK_DATA64*>(reinterpret_cast<BYTE*>(exe) + imp->FirstThunk);
        auto oft = reinterpret_cast<IMAGE_THUNK_DATA64*>(reinterpret_cast<BYTE*>(exe) + imp->OriginalFirstThunk);
        for (int i = 0; ft[i].u1.Function; ++i) {
            bool getState = false;
            if (IMAGE_SNAP_BY_ORDINAL64(oft[i].u1.Ordinal)) getState = IMAGE_ORDINAL64(oft[i].u1.Ordinal) == 2;
            else getState = strcmp(reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(reinterpret_cast<BYTE*>(exe) + oft[i].u1.AddressOfData)->Name,
                                   "XInputGetState") == 0;
            if (getState) {
                const bool ok = bvr::input::hijack_import_slot(reinterpret_cast<void**>(&ft[i].u1.Function));
                BVR_LOG("adapter: game pad import (%s XInputGetState) %s", dll, ok ? "routed through the bridge" : "NOT routed");
                return;
            }
        }
    }
    BVR_LOG("adapter: no XInputGetState import found - the panel cannot hide the pad from the game");
}

// CULLFIX (V13u, 2026-10-31): POPSWEEP2 found the trim of the right eye's instanced copies - widening ONLY the pass-setup
// caller 0x14006EB33 (0x14006E9E0, the render camera's culling copy) to both eyes' window x1.10 restored them: right kept
// 63 % (control) -> 101 %, every other caller ~76 %. Applied with the off-centre lens only (settings cull_fix, `ds cullfix`).
void cullfix_apply() {
    const bool on = settings::i("cull_fix", 1) != 0 && settings::i("lens", 1) == 1;
    // V13w: margin cull_fix_k (default 1.30): with the head still x1.10 restored every copy (95-102 %), yet JJ still saw pops -
    // most likely the culling camera lags the head by a frame or two while turning, so the edge needs room to spare.
    const float k = settings::f("cull_fix_k", 1.30f);
    offaxis::union_only(on ? 0x14006EB33ull : 0, k);
    // V13x: the second half (JJ HEADSET 00:04-00:05, A/B on / off / on: pops gone / back): the scenery occlusion queries are
    // drawn from the LEFT eye a frame late, so objects entering the right eye's outer edge read "hidden" while turning.
    // BOXVIS (the playbook's FAIL-STR-025 override) turns those zero answers into "visible", with the off-centre lens only.
    picsplit::boxvis(on ? 1 : 0);
    BVR_LOG("cullfix %d: the culling camera at 14006EB33 %s x%.2f", on ? 1 : 0, on ? "covers both eyes' window" : "is the eye's own window", k);
}

// POPNAME sequence (panel TESTS button / `ds popname`): symmetric lens, settle 45 frames, record 20 frames; off-centre lens,
// settle, record; the launch's lens back; report (picsplit.cpp POPNAME). The player holds still for the ~3 s it takes.
int g_popStep = 0, g_popWait = 0, g_popLens = 0;
char g_popStatus[96] = "not run";
// POPSWEEP (2026-10-31): POPNAME showed the vanishing scenery is INSTANCED and both views draw the same copy count (27/27 ->
// 21/21) = one copy list trimmed with the left window, shared. Which lens build feeds that trim? The sweep repeats the
// sequence once per candidate view-setup caller (offcallers list), widening ONLY that caller to both eyes' window x1.10
// while the off-centre phase records, and logs the copies the right view drew - the candidate that brings the right view's
// copies back to the symmetric count is the trimmer. Candidate 0 = nothing widened (control). Picture may look wrong for a
// moment while a render-projection caller is widened (0x140203EE7 last).
// POPSWEEP2 (JJ 2026-10-31 "go ahead with searching for all the cameras"): the list is every caller of either lens setup
// (view 0x14004B070 / pass 0x14004B3B0) that the mod has seen with an eye camera this session, plus the control (0), and a
// READBACK log runs during the first candidate.
uintptr_t kSweep[66] = {};
int g_nSweep = 0;
int g_sweepAt = -1;   // -1 = single POPNAME run
void popname_start(bool sweep) {
    if (g_popStep) return;
    g_popLens = settings::i("lens", 1);
    g_sweepAt = sweep ? 0 : -1;
    if (sweep) {
        kSweep[0] = 0; g_nSweep = 1;   // the control first; its off-centre phase fills the caller record, the list is taken after it
        picsplit::readback(10);
    }
    g_popStep = 1;
    strcpy_s(g_popStatus, sweep ? "sweep running - hold still about a minute" : "running - hold still");
    BVR_LOG("popname: %s started (the launch's lens %d is put back afterwards)", sweep ? "SWEEP" : "sequence", g_popLens);
}
void popname_tick() {
    switch (g_popStep) {
    case 1: offaxis::union_only(0, 0.0f); camera::handle_command("lens", "0"); g_popWait = 45; g_popStep = 2; break;
    case 2: if (--g_popWait <= 0) { picsplit::popname_record(0, 20); g_popStep = 3; } break;
    case 3:
        if (!picsplit::popname_busy()) {
            camera::handle_command("lens", "1");
            {   // POPSWEEP3: CULLFIX stays on during the off-centre phase (it is the base); a sweep adds one candidate on top
                const uintptr_t base = settings::i("cull_fix", 1) ? 0x14006EB33ull : 0;
                offaxis::union_two(base, g_sweepAt > 0 ? kSweep[g_sweepAt] : 0, settings::f("cull_fix_k", 1.30f));
            }
            g_popWait = 45; g_popStep = 4;
        }
        break;
    case 4: if (--g_popWait <= 0) { picsplit::popname_record(1, 20); g_popStep = 5; } break;
    case 5:
        if (!picsplit::popname_busy()) {
            offaxis::union_only(0, 0.0f);
            if (g_sweepAt < 0) picsplit::popname_report();
            else {
                uint32_t c[2][2] = {}, m[2][2] = {};
                picsplit::popname_copies(c);
                picsplit::popname_meshes(m);
                BVR_LOG("popsweep: widened %llX%s -> instanced copies symmetric L%u R%u | off-centre L%u R%u (right kept %.0f %%) | meshes "
                        "symmetric L%u R%u | off-centre L%u R%u (right kept %.0f %%)",
                        (unsigned long long)kSweep[g_sweepAt], settings::i("cull_fix", 1) ? " (+ cullfix)" : "", c[0][0], c[0][1], c[1][0],
                        c[1][1], c[0][1] ? 100.0 * c[1][1] / c[0][1] : 0.0, m[0][0], m[0][1], m[1][0], m[1][1],
                        m[0][1] ? 100.0 * m[1][1] / m[0][1] : 0.0);
                if (g_sweepAt == 0) {
                    g_nSweep = 1 + offaxis::callers_list(kSweep + 1, 64);
                    char l[700]; int k = sprintf_s(l, "popsweep: %d candidates:", g_nSweep - 1);
                    for (int i = 1; i < g_nSweep && k < 680; ++i) k += sprintf_s(l + k, sizeof(l) - k, " %llX", (unsigned long long)kSweep[i]);
                    BVR_LOG("%s", l);
                }
                if (++g_sweepAt < g_nSweep) { g_popStep = 1; break; }
            }
            char b[8]; sprintf_s(b, "%d", g_popLens);
            camera::handle_command("lens", b);
            g_popStep = 0; g_sweepAt = -1;
            cullfix_apply();   // the sweep cleared it
            strcpy_s(g_popStatus, "done - the result is in the log");
        }
        break;
    default: break;
    }
}

void apply_view_mode() {
    // Until native stereo runs (M3) the game is shown on a screen in the room; the mono test puts the head-tracked game
    // camera into both eyes (never a release mode: one picture in both eyes is not 3D).
    vr::set_flat_screen(!g_mono);
    vr::set_camera_mode(g_mono);
    // The runtime hides untagged (mono) presents in projection mode (MONOSKIP, a Batman stereo safety rule); the mono
    // test sends exactly those, the same picture to both eyes.
    vr::set_mono_skip(!g_mono);
}

void load_runtime_settings() {
    g_mono = settings::i("mono", 0) != 0;   // 3D is the default now; mono = 1 is the head-tracked single picture
    g_poseDelay = settings::i("pose_delay", 1);
    g_resScale = settings::f("resolution", 1.0f);
    const float hd = settings::f("hud_dist", 2.8f), hw = settings::f("hud_width", 3.2f), hu = settings::f("hud_up", 0.0f);
    vr::set_hud_quad(hd, hw, hu);
    const float md = settings::f("menu_dist", 1.6f), mw = settings::f("menu_width", 1.9f);
    vr::set_hud_menu_quad(md, mw);
    const float sd = settings::f("screen_dist", 2.5f), sw = settings::f("screen_width", 3.0f);
    vr::set_screen_quad(sd, sw);
    vr::set_asym_fov(true);
    cullfix_apply();
    // PACEDETACH (JJ 2026-10-31: the desktop mirror crawls whenever he leaves VR; the Xid 13 freezes cluster there too):
    // once the session has been FOCUSED and loses focus, the game stops waiting on the headset; a keep-alive frame every
    // pace_keepalive_ms (on the present thread - never the off-thread "feed", VR_ESSENTIALS §5) lets the runtime see a
    // rendering app so focus comes back. `vrpace detach off` / `vrpace keepalive N` live.
    vr::set_pace_detach(settings::i("pace_detach", 1) != 0);
    char ka[32];
    snprintf(ka, sizeof(ka), "keepalive %d", settings::i("pace_keepalive_ms", 250));
    vr::handle_pace_command(ka);
    apply_view_mode();
}

void save_quads() {
    float d, w, u; vr::get_hud_quad(&d, &w, &u);
    settings::set_f("hud_dist", d); settings::set_f("hud_width", w); settings::set_f("hud_up", u);
    vr::get_hud_menu_quad(&d, &w); settings::set_f("menu_dist", d); settings::set_f("menu_width", w);
    vr::get_screen_quad(&d, &w); settings::set_f("screen_dist", d); settings::set_f("screen_width", w);
}

void capture(IDXGISwapChain* sc) {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t dir[MAX_PATH];
    swprintf_s(dir, L"%s\\captures", log::data_dir());
    CreateDirectoryW(dir, nullptr);
    swprintf_s(dir, L"%s\\captures\\%04u%02u%02u_%02u%02u%02u", log::data_dir(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    CreateDirectoryW(dir, nullptr);
    vr::save_eye_pictures(dir);
    if (g_buildOk) stereo::save_pictures(sc, dir);
    wchar_t txt[MAX_PATH]; swprintf_s(txt, L"%s\\status.txt", dir);
    FILE* f = nullptr;
    if (_wfopen_s(&f, txt, L"w") == 0 && f) {
        camera::Cam h{}, g{};
        uint32_t gen = 0;
        camera::latest(-1, h, &gen); camera::game_camera(g);
        fprintf(f, "DS3VR2 capture\n%s\nupdates %llu, generation %u\nhead cam pos %.3f %.3f %.3f fwd %.3f %.3f %.3f fov %.4f\n"
                   "game cam pos %.3f %.3f %.3f fwd %.3f %.3f %.3f\n%s\n",
                camera::status(), static_cast<unsigned long long>(camera::updates()), gen, h.r[3][0], h.r[3][1], h.r[3][2],
                h.r[2][0], h.r[2][1], h.r[2][2], h.fov, g.r[3][0], g.r[3][1], g.r[3][2], g.r[2][0], g.r[2][1], g.r[2][2], dlss::diag());
        fclose(f);
    }
    char d8[MAX_PATH * 2]; WideCharToMultiByte(CP_UTF8, 0, dir, -1, d8, sizeof(d8), nullptr, nullptr);
    BVR_LOG("capture: both eyes + status -> %s", d8);
    stereo::handle_command("views", "");
}

void keys() {
    static bool f2 = false, f11 = false, f12 = false;
    const bool k2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0, k11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0,
               k12 = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    if (k2 && !f2) g_captureReq = true;
    if (k11 && !f11) camera::set_head(!camera::head_on());
    if (k12 && !f12) { camera::recenter(); vr::recenter_panel(); }
    f2 = k2; f11 = k11; f12 = k12;
}

class Ds3Adapter final : public bvr::game::IGameAdapter {
public:
    uint32_t capabilities() const override { return camera::installed() ? bvr::game::CAP_CAMERA_OVERRIDE : 0; }

    bool init(const bvr::pattern_scan::ProcessImage&) override {
        settings::load();
        command::enable_present_pump();
        hook_game_pad();
        input::dinput_block::install();
        load_runtime_settings();
        eyesize::install(); // M4: what DllMain / the first graphics call did about the eye size, and the resize filter
        {   // DLSS (M6): the game's frame is then SMALLER than the eye picture, so the headset images must be made at
            // the full eye size - otherwise the reconstruction is thrown away on the copy (run M6-1: both were 1632x1768).
            unsigned fw = 0, fh = 0;
            if (eyesize::dlss_mode() > 0 && eyesize::full_size(fw, fh)) {
                vr::set_output_size(fw, fh);
                BVR_LOG("adapter: DLSS on - headset images at the full eye size %ux%u", fw, fh);
            }
        }
        offline::install();   // before anything else can reach the servers
        g_buildOk = build_ok();
        if (!g_buildOk) return false;
        framerate::install();
        sunrays::install();   // SUNRAYS: the god-ray focal length (probe + live scale)
        offaxis::install();   // OFFAXIS: the true off-centre lens (lens mode 1)
        lightgrid::install(); // LIGHTGRID: the light tiles follow the eye's own window (V4)
        tilefix::set_mode(settings::i("tilefix", 1));   // TILEFIX: the GPU light-tile culling too (V6)
        focus::install();      // KEEPFOCUS (V9): full speed when the window is not in front
        bvr::hud::lock_disabled();   // V9: the core's Arkham-era HUD capture first ran in DS3 at a panel open and the game hung
        afw::load_settings();  // AFW (V7): native stereo (default) or one eye drawn per frame, the other warped
        bvr::hud::set_engine_scene_veto(settings::i("enginescene", 1) != 0);   // ENGINESCENE (V5): lock-on stays 3D
        const bool cam = camera::install();
        const bool st = stereo::install();
        BVR_LOG("adapter: camera %s, stereo probe %s", cam ? "on" : "FAILED", st ? "on" : "FAILED");
        return cam;
    }

    void setFov(float) override {}

    void onPresent(IDXGISwapChain* sc) override {
        keys();
        framerate::tick();   // DS3's cap is two data writes on the frame timer - re-asserted every frame
        eyesize::watch();
        {   // DLSS: NGX's own start takes seconds - begin it in the background at the first frame (VR_ESSENTIALS section 4)
            static bool ngxStarted = false;
            if (!ngxStarted && dlss::mode() > 0) {
                ID3D11Device* dev = nullptr;
                if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) && dev) {
                    dlss::start(dev); dev->Release(); ngxStarted = true;
                }
            }
        }
        // What DLSS reconstructs to: the eye picture this launch was sized for (eyesize already applied the headset's
        // recommendation, the lens shape and the sharpness slider). Without the eye-size route, the runtime's own
        // recommendation x the slider (FAIL-PERF-020).
        unsigned fw = 0, fh = 0;
        if (eyesize::full_size(fw, fh)) {
            dlss::set_out_size(static_cast<UINT>(fw), static_cast<UINT>(fh));
        } else {
            uint32_t rw = 0, rh = 0;
            if (vr::recommended_eye_size(&rw, &rh) && rw && rh)
                dlss::set_out_size(static_cast<UINT>(rw * g_resScale), static_cast<UINT>(rh * g_resScale));
        }
        // PERF-007 / pose-timing contract: the picture of this Present was rendered from the camera update g_poseDelay
        // updates back - tag it with that update's locate generation, so the runtime declares the pose it was drawn with.
        uint32_t gen = 0;
        if (camera::generation_back(g_poseDelay, gen)) {
            vr::sr_set_present_gen(gen);
            vr::sr_set_mono_gen(gen); // MONOPOSE: mono frames are declared with this exact pose too
        }
        if (g_buildOk) stereo::on_present(sc);
        popname_tick();   // POPNAME
        if (g_captureReq.exchange(false)) capture(sc);
    }

    void drawDebugUi() override {
        const ImVec4 kGreen(0.3f, 1.0f, 0.55f, 1.0f), kAmber(1.0f, 0.7f, 0.25f, 1.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "Dark Souls III VR 2.0");
        ImGui::SameLine();
        ImGui::TextDisabled("   build: " __DATE__ " " __TIME__);
        if (!g_buildOk) {
            ImGui::TextColored(kAmber, "This game version is not supported: the game is shown on a screen in the room.");
            return;
        }
        const bool live = vr::session_live();
        ImGui::TextColored(live ? kGreen : kAmber, "%s", live ? "VR connected" : "VR not connected");
        ImGui::SameLine();
        ImGui::TextDisabled("|  %s  |  head tracking %s", g_mono ? "head tracking, one picture in both eyes (not 3D yet)" : "screen in the room",
                            camera::head_on() ? "on" : "OFF (F11)");
        if (ImGui::Button("Recenter  (F12)")) { camera::recenter(); vr::recenter_panel(); }
        ImGui::SameLine();
        if (ImGui::Button("Save capture  (F2)")) g_captureReq = true;
        ImGui::SameLine();
        if (ImGui::Button(camera::head_on() ? "Head tracking: ON  (F11)" : "Head tracking: OFF  (F11)")) camera::set_head(!camera::head_on());
        ImGui::TextDisabled("controller: left stick / d-pad = move, A = select / change, B = back, both stick clicks = close");
        ImGui::Separator();

        // STEREO RENDERING drawer removed (JJ 2026-10-30: "True 3D can be removed from the panel"): 3D is simply on.
        // Its status lines are with the other test read-outs (SHADOWS drawer, "show test options").
        ImGui::SetNextItemOpen(false, ImGuiCond_Once);
        if (ImGui::CollapsingHeader("VIEW")) {
            camera::draw_ui();
            // POSEDELAY (JJ 2026-10-28: "the world also jitters when I move my head ... that should be an option in the
            // panel"): which head position the picture is said to be drawn from, in game frames back. Wrong = the world
            // jitters or swims as the head moves (v1 needed 3 under geo-11).
            if (overlay::SliderStepInt("picture timing - raise or lower if the world jitters when you move your head",
                                       &g_poseDelay, 0, 4, "%d frames"))
                settings::set_i("pose_delay", g_poseDelay);
            if (overlay::show_tests()) {
                uint32_t h = 0, m = 0; vr::mono_gen_stats(&h, &m);
                ImGui::TextDisabled("   exact head position found for %u frames, missed %u", h, m);
            }
        }
        ImGui::SetNextItemOpen(false, ImGuiCond_Once);
        if (ImGui::CollapsingHeader("HUD")) {
            float q[3]; vr::get_hud_quad(&q[0], &q[1], &q[2]);
            float deg = 2.0f * std::atan(0.5f * q[1] / q[0]) * 180.0f / kPi;
            const float d0 = q[0];
            bool ch = overlay::SliderStep("HUD distance (m)", &q[0], 0.5f, 8.0f, "%.2f");
            ch |= overlay::SliderStep("HUD size (degrees of your view)", &deg, 30.0f, 120.0f, "%.0f");
            float upDeg = std::atan(q[2] / d0) * 180.0f / kPi;
            ch |= overlay::SliderStep("HUD height (degrees, + up)", &upDeg, -30.0f, 30.0f, "%.0f");
            if (ch) {
                q[1] = 2.0f * q[0] * std::tan(0.5f * deg * kPi / 180.0f);
                q[2] = q[0] * std::tan(upDeg * kPi / 180.0f);
                vr::set_hud_quad(q[0], q[1], q[2]); vr::recenter_panel(); save_quads();
            }
            ImGui::Separator();
            // M8 world markers. Plain words: what JJ will SEE, not how it is done (memory
            // panel-labels-plain-no-bad-switches).
            bool mk = settings::i("markers", 1) != 0;
            if (ImGui::Checkbox("put enemy bars, grapple points and lock-on on the thing they belong to", &mk)) {
                settings::set_i("markers", mk ? 1 : 0);
                stereo::handle_command("markers", mk ? "1" : "0");
            }
            float ms = settings::f("marker_smooth", 1.0f);
            if (overlay::SliderStep("steady enemy bars (0 = follow exactly)", &ms, 0.0f, 2.0f, "%.2f")) {
                char b[32]; snprintf(b, sizeof(b), "%.3f", ms);
                stereo::handle_command("mksmooth", b);
            }
            ImGui::TextDisabled("   They stand at their target's own distance, so each eye sees them where the target is,\n"
                                "   and they turn only when the game's camera turns - not with your head.");
            if (overlay::show_tests()) {
                bool fp = settings::i("marker_face_pos", 1) != 0;
                if (ImGui::Checkbox("test: card tilted towards the camera (off = upright)", &fp))
                    stereo::handle_command("mkface", fp ? "1" : "0");
                bool bs = settings::i("marker_base", 1) != 0;
                if (ImGui::Checkbox("test: lay markers out with the head-free camera", &bs))
                    stereo::handle_command("mkbase", bs ? "1" : "0");
                float rr = settings::f("marker_reach", 0.12f);
                if (overlay::SliderStep("test: how far a piece may sit from its marker", &rr, 0.02f, 0.30f, "%.3f")) {
                    char b[32]; snprintf(b, sizeof(b), "%.3f", rr);
                    stereo::handle_command("mkreach", b);
                }
                ImGui::TextDisabled("   %s", markers::status());
            }
        }
        ImGui::SetNextItemOpen(false, ImGuiCond_Once);
        if (ImGui::CollapsingHeader("MENUS AND SCREENS")) {
            float md = 0, mw = 0; vr::get_hud_menu_quad(&md, &mw);
            float mdeg = 2.0f * std::atan(0.5f * mw / md) * 180.0f / kPi;
            bool mc = overlay::SliderStep("pause / shop window distance (m)", &md, 0.5f, 8.0f, "%.2f");
            mc |= overlay::SliderStep("pause / shop window size (degrees)", &mdeg, 30.0f, 120.0f, "%.0f");
            if (mc) { vr::set_hud_menu_quad(md, 2.0f * md * std::tan(0.5f * mdeg * kPi / 180.0f)); save_quads(); }
            float d = 0, w = 0; vr::get_screen_quad(&d, &w);
            bool sc = overlay::SliderStep("game screen / loading screen distance (m)", &d, 0.8f, 6.0f, "%.2f");
            sc |= overlay::SliderStep("game screen / loading screen size (m)", &w, 0.8f, 6.0f, "%.2f");
            if (sc) { vr::set_screen_quad(d, w); save_quads(); }
            float pw = 0, pd = 0; vr::get_panel_size(&pw, &pd);
            bool pc = overlay::SliderStep("this menu: distance (m)", &pd, 0.6f, 3.0f, "%.2f");
            pc |= overlay::SliderStep("this menu: size (m)", &pw, 0.5f, 2.0f, "%.2f");
            if (pc) vr::set_panel_size(pw, pd);
        }
        ImGui::SetNextItemOpen(false, ImGuiCond_Once);
        if (ImGui::CollapsingHeader("FRAME RATE")) { framerate::draw_ui(); if (overlay::show_tests()) ImGui::TextDisabled("   %s", framerate::status()); }
        ImGui::SetNextItemOpen(false, ImGuiCond_Once);
        if (ImGui::CollapsingHeader("RENDERING")) {
            // AFW (V7, JJ 2026-10-30): the name first, the description in brackets (Batman City's wording)
            const bool afwOn = afw::on();
            if (ImGui::RadioButton("Native synchronised (both eyes drawn every frame)", !afwOn)) afw::set_on(false);
            if (ImGui::RadioButton("AFW (one eye drawn per frame, the other warped)", afwOn)) afw::set_on(true);
            if (afwOn) {
                const int m = afw::mode();
                ImGui::TextUnformatted("warp from:");
                ImGui::SameLine();
                if (ImGui::RadioButton("this frame", m == 1)) afw::command("mode 1");
                ImGui::SameLine();
                if (ImGui::RadioButton("previous frame", m == 2)) afw::command("mode 2");
                ImGui::SameLine();
                if (ImGui::RadioButton("both (best)", m == 3)) afw::command("mode 3");
                ImGui::TextDisabled("   smooth only when the game keeps up with the headset's rate (one eye per frame)");
            }
        }
        ImGui::SetNextItemOpen(false, ImGuiCond_Once);
        if (ImGui::CollapsingHeader("VR RESOLUTION")) {
            unsigned ew = 0, eh = 0, iw = 0, ih = 0;
            const bool on = eyesize::wanted(ew, eh);
            eyesize::internal(iw, ih);
            // PANELWORDS (JJ 2026-11-01: "not sure what those options mean or whether we really even need them"): one plain
            // slider here; the lens choice and the scenery A/B went to TESTS (lens 1 + cull_fix are simply the better setup).
            if (overlay::SliderStep("picture sharpness", &g_resScale, 0.6f, 1.8f, "%.2f"))
                settings::set_f("resolution", g_resScale);
            ImGui::TextDisabled("   higher = sharper but slower; takes effect the next time the game starts");
            if (overlay::show_tests()) {
                if (on) ImGui::TextDisabled("   each eye is drawn at %ux%u now (the game reports %ux%u)", ew, eh, iw, ih);
                else ImGui::TextDisabled("   each eye is drawn at the game window's size (eye size switched off)");
            }
            if (overlay::show_tests()) ImGui::TextDisabled("   %s", eyesize::status());
        }
        ImGui::SetNextItemOpen(false, ImGuiCond_Once);
        if (ImGui::CollapsingHeader("SHADOWS")) {
            // SHADOWRES (JJ 2026-10-30: "an option for higher resolution shadows"). Plain words: what he will SEE.
            static const char* kDetail[] = {"the game's own", "sharper (1.5 x the game's finest)", "sharpest (2 x the game's finest)"};
            const float cur = settings::f("shadow_res", shadowres::kDefaultFactor);
            int di = cur >= 1.99f ? 2 : cur >= 1.49f ? 1 : 0;
            if (ImGui::Combo("shadow detail", &di, kDetail, 3)) settings::set_f("shadow_res", di == 2 ? 2.0f : di == 1 ? 1.5f : 1.0f);
            ImGui::TextDisabled("   Draws the sun's shadows on a finer grid: sharper, steadier shadow edges, no stair steps\n"
                                "   on leaves and fences. Set the game's own Shadow Quality to MAX for the full effect.\n"
                                "   Takes effect the next time the game starts. Measured on the test headset: no change in\n"
                                "   frames a second; \"sharpest\" uses about 1 GB more video memory.");
            float edge = picsplit::shadow_sharp_now();
            if (overlay::SliderStep("shadow edges (lower = crisper, higher = softer)", &edge, 0.25f, 2.0f, "%.2f")) {
                char b[32]; snprintf(b, sizeof(b), "%.3f", edge);
                stereo::handle_command("shadowsharp", b);
            }
            // Contact shadows removed from the panel (JJ 2026-10-30). The module is Sekiro's and never found DS3's
            // screen-shadow pass ("0 passes drawn" all session), so it is off by default too (contact.cpp).
            if (overlay::show_tests()) {
                ImGui::TextDisabled("   %s", shadowres::status());
                ImGui::TextDisabled("   %s", contact::status());
                stereo::draw_stereo_ui();
            }
        }
        ImGui::SetNextItemOpen(false, ImGuiCond_Once);
        if (ImGui::CollapsingHeader("DLSS")) {
            // JJ 2026-10-30: "rename the smoother picture drawer to DLSS and change the drop-down to a pill slider
            // with conventional DLSS settings ... Preset". Modes and presets come from dlss_modes.h, the same table
            // the render size and the NGX feature are built from, so the names cannot drift from what they do.
            static const char* kModeShort[dlss_modes::kModeCount] = {"Off", "DLAA", "Quality", "Balanced", "Perf", "Ultra Perf"};
            int m = settings::i("dlss_mode", 0);
            if (m < 0 || m >= dlss_modes::kModeCount) m = 0;
            ImGui::TextUnformatted("Mode");
            if (overlay::PillSlider("dlss_mode", &m, kModeShort, dlss_modes::kModeCount)) settings::set_i("dlss_mode", m);
            ImGui::TextDisabled("   %s - %s", dlss_modes::kModes[m].name, dlss_modes::kModes[m].hint);

            static const char* kPresetShort[dlss_modes::kPresetCount] = {"Default", "J", "K", "L", "M"};
            int p = dlss_modes::preset_index(settings::i("dlss_preset", 0));
            ImGui::Spacing();
            ImGui::TextUnformatted("Preset");
            if (overlay::PillSlider("dlss_preset", &p, kPresetShort, dlss_modes::kPresetCount))
                settings::set_i("dlss_preset", dlss_modes::kPresets[p].ngx);
            ImGui::TextDisabled("   %s", dlss_modes::kPresets[p].hint);

            ImGui::Spacing();
            if (m != eyesize::dlss_mode())
                ImGui::TextColored(kAmber, "   restart the game to use %s (this session renders for %s)", dlss_modes::kModes[m].name,
                                   dlss_modes::kModes[eyesize::dlss_mode()].name);
            else
                ImGui::TextDisabled("   both are applied when the game starts");
            ImGui::TextDisabled("   Needs an NVIDIA RTX card. Measured here in 3D, each eye 2448x2648 on an RTX 4070 Ti:\n"
                                "   Quality 72 frames a second, about 3 ms of DLSS work per eye.");
            if (overlay::show_tests()) {
                ImGui::TextDisabled("   %s", dlss::diag());
                ImGui::TextDisabled("   %s", stereo::dlss_status());
            }
        }
        ImGui::SetNextItemOpen(false, ImGuiCond_Once);
        if (ImGui::CollapsingHeader("Keyboard shortcuts")) {
            ImGui::TextDisabled("F10 or both stick clicks - this menu\nF12 - recenter\nF11 - head tracking on/off\n"
                                "F2 - save a capture (both eyes and a status file)");
        }
        ImGui::SetNextItemOpen(false, ImGuiCond_Once);
        if (ImGui::CollapsingHeader("TESTS  (only when asked - leave alone)")) {
            ImGui::TextDisabled("For finding bugs. Leave these as they are unless a test asks for them.");
            // N2 tidy (JJ 2026-11-01: "remove any tests we don't need anymore"): the smoke recorder, the edge-popping finders,
            // the scenery-edge switch, the mono view and the second-view / lighting-learn / log buttons left the panel - their
            // problems are solved, and the second-view switches broke 3D when touched. All still work as `ds` commands
            // (smokerec, popname, popsweep, cullfix, mono, view, rtclass, eyemap, camdump; the second-view camera tests are code-only).
            {   // moved from VR RESOLUTION (PANELWORDS)
                static const char* kLens[] = {"one wide picture for both eyes", "each eye its own view (sharper, faster)"};
                int li = settings::i("lens", 1) == 0 ? 0 : 1;
                if (ImGui::Combo("eye shape (next start)", &li, kLens, 2)) settings::set_i("lens", li ? 1 : 0);
            }
            bool st = overlay::show_tests();
            if (ImGui::Checkbox("show test numbers in the other drawers", &st)) overlay::set_show_tests(st);
            stereo::draw_ui();
            ImGui::TextDisabled("%s", camera::status());
            ImGui::TextDisabled("%s", dlss::diag());
            ImGui::TextDisabled("%s", markers::status());
            ImGui::TextDisabled("captures and logs: <game>\\DS3VR2");
        }
    }

    bool handleCommand(const char* cmd, const char* args) override {
        if (_stricmp(cmd, "ds") != 0) return false;
        // "ds <sub> <args>"
        char sub[32] = {};
        const char* rest = args ? args : "";
        while (*rest == ' ') ++rest;
        size_t n = 0;
        while (rest[n] && rest[n] != ' ' && n < sizeof(sub) - 1) { sub[n] = rest[n]; ++n; }
        const char* a = rest + n;
        while (*a == ' ') ++a;
        if (!_stricmp(sub, "capture")) { g_captureReq = true; return true; }
        if (!_stricmp(sub, "popname")) { popname_start(false); return true; }
        if (!_stricmp(sub, "popsweep")) { popname_start(true); return true; }
        if (!_stricmp(sub, "smokerec")) { picsplit::smokerec(800); bvr::vr::eye_burst::start(800); return true; }
        if (!_stricmp(sub, "cullfix")) { settings::set_i("cull_fix", atoi(a) ? 1 : 0); cullfix_apply(); return true; }
        if (!_stricmp(sub, "fps")) { framerate::set_cap(atoi(a)); return true; }
        if (!_stricmp(sub, "mono")) { g_mono = atoi(a) != 0; settings::set_i("mono", g_mono ? 1 : 0); apply_view_mode(); BVR_LOG("ds: head-tracked single picture %d", g_mono ? 1 : 0); return true; }
        if (!_stricmp(sub, "posedelay")) { g_poseDelay = atoi(a); settings::set_i("pose_delay", g_poseDelay); BVR_LOG("sk: pose delay %d", g_poseDelay); return true; }
        if (!_stricmp(sub, "dlss")) { char w[16] = {}; int v = 0; sscanf_s(a, "%15s %d", w, (unsigned)sizeof(w), &v); dlss::command(w, v); return true; }
        if (!_stricmp(sub, "lens")) { camera::handle_command(sub, a); cullfix_apply(); return true; }   // CULLFIX follows the lens
        if (camera::handle_command(sub, a)) return true;
        if (stereo::handle_command(sub, a)) return true;
        BVR_LOG("sk: unknown command '%s'", sub);
        return true;
    }
};

} // namespace

bvr::game::IGameAdapter* create_adapter() {
    static Ds3Adapter adapter;
    return &adapter;
}

} // namespace bvr::ds3
