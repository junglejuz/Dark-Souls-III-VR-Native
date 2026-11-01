#pragma once
// DS3VR2 OFFLINE - see offline.cpp. Refuses the game's outbound connections from the first graphics frame, so a
// modified client never reaches Dark Souls III's servers (its anti-cheat penalises modified clients) and the
// server login fails at once instead of waiting about ten seconds.
//
// This is the same default ModEngine ships for this game (`blockNetworkAccess = 1`). It is a safety net, NOT the
// whole answer: the game's own System > Network > "Play Offline" setting is what stops it trying to log in at all,
// and that is what the README tells players to turn on.
#include <cstdint>

namespace bvr::ds3::offline {

void install();        // first graphics frame (after Steam has started - the game will not run without it)
bool on();
uint32_t refused();    // connections refused since start

} // namespace bvr::ds3::offline
