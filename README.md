<p align="center">
  <img src="https://cdn.cloudflare.steamstatic.com/steam/apps/374320/capsule_616x353.jpg" alt="Dark Souls III" width="616">
</p>

# Dark Souls III VR (native stereo)

Play Dark Souls III in PC VR, in full 3D with head tracking. This mod was built from the ground up for picture
quality and speed: the game itself draws both eyes, each for its own lens, and NVIDIA DLSS rebuilds each eye at your
headset's resolution.

## What you get

- **Native synchronised stereo.** The game draws both eyes in the same frame from the same head position, so depth is
  correct and the two eyes never show different moments.
- **Each eye drawn for its own lens.** VR lenses are off-centre (each eye sees more to the outside than to the inside).
  The game draws each eye for that exact shape, so no pixels are drawn that the lens cannot show. Scenery at the edges
  of each eye stays in place (no pop-in at the outer edge).
- **NVIDIA DLSS on each eye.** The game renders each eye smaller and DLSS rebuilds it at your headset's resolution, with
  a separate DLSS for each eye (its own history and motion), so the eyes stay matched. Balanced is the default; DLAA,
  Quality and Performance are in the VR menu. Needs an NVIDIA RTX graphics card.
- **AFW, an alternative for slower PCs.** One eye is drawn per frame and the other is built from that same frame's
  picture and depth, so both eyes still show the same moment. Choose it in the VR menu (Rendering).
- **Comfortable screens and HUD.** The HUD sits on a floating panel; menus and loading screens are shown on a screen in
  the room, with fades between the loading screen and the game; the game's own fade to black covers your whole view.
- **The game's graphics options set for VR**, for VR starts only. Starting the game from Steam as usual plays the
  normal game with your own settings.

## Compared with the other Dark Souls III VR mod

Measured on the same PC (RTX 4070 Ti, Quest 3 through Virtual Desktop at 90 Hz), at the same spot in the game, with
the same game graphics settings (everything at maximum), 60 seconds each (30 still, 30 turning the head). Frame rate =
the game's frames per second, read by Intel PresentMon (median frame), which works the same for both mods.

| Mod | DLSS | Each eye drawn at | Frames per second | Graphics card time per frame |
|---|---|---|---|---|
| DS3 VR 2.0.1 (geo-11 based, by scubasteve2365) | off | 2482 x 2448 | 43 | 23.0 ms |
| DS3 VR 2.0.1 (geo-11 based, by scubasteve2365) | Balanced | 58 % of 2482 x 2448 | 65 | 15.4 ms |
| **This mod** | Balanced | 1328 x 1416, rebuilt to 2288 x 2448 | **78** | **12.7 ms** |
| **This mod** | off | 2288 x 2448 | not measured yet | |

With DLSS Balanced in both, this mod ran about 20 % faster and needed about 17 % less graphics card time per frame.
The tester also found the other mod's DLSS Balanced picture clearly lower in resolution than this mod's. One PC and one
scene: your numbers will differ, the comparison should not.

## What you need

- Dark Souls III on Steam (the current version, App ver. 1.15.2).
- A PC VR headset and the software you use for PC VR games (for example Virtual Desktop, SteamVR or the Meta Quest
  Link app), set as the PC's OpenXR runtime. Tested with a Quest 3 and Virtual Desktop.
- An NVIDIA RTX graphics card for DLSS (tested on an RTX 4070 Ti). On other cards, turn DLSS off in the VR menu and restart the game.
- A gamepad.

## Install

1. Start Dark Souls III once from Steam as usual, wait for the title screen and quit (the game makes its settings file).
2. Download `DS3VR-Native-DarkSoulsIII.zip` from [Releases](../../releases) and unpack it (if Windows refuses the zip,
   use 7-Zip).
3. Double-click `Install-DS3VR-Native.bat`. If Windows SmartScreen appears: More info, then Run anyway. The installer puts
   everything into the game folder; you copy nothing yourself. If another 3D or VR setup is in the game folder (geo-11,
   a 3D fix or another VR mod), it offers to move it aside; the uninstaller puts it back.
4. Make your VR software the PC's OpenXR runtime (Virtual Desktop does this itself; in SteamVR: Settings, OpenXR).
5. Start your VR software, then the game with the **Dark Souls III VR** shortcut on your desktop.

The first start with a new headset uses a standard picture size and reads your headset's own; from the second start
the picture is sized for your headset.

## In the game

| Key / button | Does |
|---|---|
| F10, or click both thumbsticks | VR menu (also blocks the game's input while open) |
| F12 | recenter |
| F11 | head tracking on / off |

## Known issues

- With DLSS on, one eye can occasionally turn black or show coloured noise. Restarting the game clears it; turning DLSS
  off in the VR menu avoids it. The mod checks both eyes' DLSS while you play and records it in its log when it happens.
- On the test PC the headset picture sometimes froze (the graphics driver reported an error) after taking the headset
  off and putting it back on. The game keeps running; restart it.

## Remove

Start menu, Dark Souls III VR, **Uninstall Dark Souls III VR**. It removes the mod, puts your own graphics settings back
and restores anything the installer moved aside.

## Building from source

Visual Studio 2022, CMake 3.21 or newer:
`cmake -S . -B build -G "Visual Studio 17 2022" -A x64` then `cmake --build build --config Release`. The project
expects MinHook and the OpenXR SDK in a sibling `../references` folder and NVIDIA's NGX SDK in `third_party/ngx`;
`python tools/make_package.py` builds the release zip.

## Credits

Built on other people's work; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Game image: Dark Souls III,
© Bandai Namco Entertainment Inc. / © FromSoftware, Inc., shown from Steam.
