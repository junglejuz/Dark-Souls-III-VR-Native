#pragma once
// LOADSCREEN (ported from SKVR2 2026-11-01): is the game's own loading screen (a Scaleform movie) being DRAWN? (loadscreen.cpp)
#include <windows.h>

namespace bvr::ds3::loadscreen {
bool install();                  // queues the two movie-draw hooks (before MH_ApplyQueued)
bool ok();
void retry();                    // V13z3: call each Present - hooks once the game's code is unpacked
ULONGLONG ms_since_loading();    // ms since the loading screen was last drawn (~0 = never)
bool drawn_this_frame();         // TRAVELCLEAN2: the loading screen was drawn since the previous call (call once per Present)
}
