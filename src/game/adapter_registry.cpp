#include "adapter_registry.h"
#include "igame_adapter.h"
#include "core/util/log.h"
#include <windows.h>
namespace bvr::ds3 { bvr::game::IGameAdapter* create_adapter(); }
namespace bvr::game {
namespace { IGameAdapter* current = nullptr; }
HostGame detect_host_game() {
    wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr,path,MAX_PATH);
    auto* p=wcsrchr(path,L'\\');
    return _wcsicmp(p ? p+1 : path,L"DarkSoulsIII.exe") == 0 ? HostGame::DarkSouls3 : HostGame::Unknown;
}
const wchar_t* host_data_subdir() { return L""; }
void init_adapter() {
    if(detect_host_game()==HostGame::Unknown) { BVR_LOG("adapter: host exe is not DarkSoulsIII.exe - no adapter"); return; }
    current=bvr::ds3::create_adapter();
    pattern_scan::ProcessImage image{};
    if(pattern_scan::capture_main_module(image)) current->init(image);
    else BVR_LOG("adapter: could not read the game image - adapter not initialised");
}
IGameAdapter* adapter() { return current; }
}
