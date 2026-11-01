#pragma once
// DS3VR2 settings: <game>\DS3VR2\settings.ini, section [ds3vr2], plain key = value. Read at start, written whenever a panel
// control changes, re-read on the `ds reload` command. Every test switch is a key here or a command (SESSION_START §2:
// every test is a live switch).

namespace bvr::ds3::settings {

void   load();                                  // after log::init (needs the data folder)
float  f(const char* key, float def);
int    i(const char* key, int def);
void   set_f(const char* key, float v);
void   set_i(const char* key, int v);
const wchar_t* path();

} // namespace bvr::ds3::settings
