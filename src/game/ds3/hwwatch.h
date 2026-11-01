#pragma once
// DS3VR2 HWWATCH: log the game code that writes an address (hardware write watch on every thread). Test tool.
// Up to FOUR addresses at once (DR0-DR3), so one game launch answers several questions (test rule: many tests per launch).
#include <cstdint>

namespace bvr::ds3::hwwatch {

void arm(uintptr_t addr, const char* what);              // slot 0, clears the others
void arm_slot(int slot, uintptr_t addr, const char* what); // slot 0..3; addr 0 = clear that slot
void rearm();                                            // put the armed slots on threads started since (call every few s)
void off();
void set_read(bool on);   // V12z CULLHUNT: watch READS as well as writes (DR7 RW=11)

} // namespace bvr::ds3::hwwatch
