# The House of the Dead 2 VR

Stand inside **The House of the Dead 2** (Sega Dreamcast) on a **Meta Quest**, standalone, or
with a **PC VR** headset.
The game's camera becomes your rail cart, the 3D scenes are rebuilt around you in stereo,
and your controller is the light gun, a red Namco-style arcade gun in your hand.

It is a fork of the [Flycast](https://github.com/flyinghead/flycast) Dreamcast emulator,
branch `hotd2-vr`. There is no remake here: the original game runs, and the emulator turns
what it draws into a VR view.

> **No game included.** You need your own dump of the game. This project contains no
> Sega code, data, graphics or sound. Not affiliated with or endorsed by Sega, Namco,
> Meta or the Flycast project.

Status: experimental, built for and tested with the European release (PAL, product ID
`MK-5100250`) on a Quest 3. The US release (`MK-51002`) has its own profile now (checked on
the PC: the same view, aim and hands as PAL). Quest 2, Quest Pro and PCVR are new and not
tested yet. Other versions need their own profile (see *How it works*).
Shooting lands on the aim dot, the B button skips story scenes and the game's own hit flash
is gone, and the agent's hands and racking the slide work (all tested on the headset).

What's next (two players, among others): see the [roadmap](ROADMAP.md).

## What you get

- **Immersive 3D.** The Dreamcast's GPU only sees vertices already projected to the
  screen (x, y, 1/w). Knowing the game's lens, each one is lifted back into 3D and drawn
  per eye from your head, so you look around and lean into the scene at the headset's
  refresh rate, while the game runs at its own 60 Hz.
- **A wider view than the original.** The game's own field of view is raised from 41° to
  74° (vertical), inside the game: it draws and culls with it. Its light gun still only
  counts shots inside the original view, so that is where you can hit things; around it
  the aim dot turns into a grey ring and the trigger does nothing.
- **2D on a screen in front of you.** Menus, text and the HUD sit on a plane at a fixed
  distance, framed as in the original. The game's own hit flash (a glow and rays drawn
  just off its 2D plane, which in the headset came right at your face) is left out after
  shots into the 3D scene: the aim dot, blood and the hit show where it went.
- **The light gun.** Either controller. Shots are traced into the rebuilt scene from the
  barrel and handed to the game at its own screen framing, so they land on the dot; a
  laser line and dot show where (optional). Recoil, muzzle flash, smoke and a haptic
  knock on every shot.
- **The agent's own hands.** Made from your copy of the game, by the app itself: his right hand
  around his pistol in place of the arcade gun, his open left hand on the other controller.
  Reload like with a real pistol: take the slide with the other hand (grip button) and pull
  it back.
- **Comfort.** Things that come right into your face (a zombie grabbing you) are pulled
  back a little, so your eyes don't have to cross. Recentering, adjustable world size.
- Starts straight into the game from the headset's app library. No floating "PRESS START
  BUTTON" for player 2 in a one-player game.

## Controls

| | Left controller | Right controller |
|---|---|---|
| Shoot | trigger | trigger |
| Reload | shoot away from the view (past the grey ring), or rack the slide (below); with two guns: point the gun at the floor | the same |
| Skip a story scene, back in menus (the gun's B) | Y | B |
| Start, pause | | A |
| Player 2 joins: a gun in each hand | menu button ≡ | |
| Menus (the gun's D-pad) | thumbstick | thumbstick |
| Recenter | X or thumbstick click | thumbstick click |
| World size (kept) | hold grip + thumbstick up/down | hold grip + thumbstick up/down |
| Gun size (kept) | hold grip + thumbstick left/right | hold grip + thumbstick left/right |
| Rack the slide (the agent's pistol) | grip, near the back of the pistol, and pull | the same |

The gun goes to the hand you last fired with; B and Y both work, whichever hand holds it.

**Two guns.** Press ≡ on the left controller and player 2 joins, as at the cabinet: the
right hand holds player 1's gun (red aim), the left player 2's (blue aim), both lives and
scores on screen. With no hand free for the slide, point a gun at the floor to reload it
(or shoot past the view). When player 2 is out of the game (the game asks for player 2's
Start again), it's one gun again. `vr.DualWield = no` makes ≡ player 1's Start as before.

With the agent's pistol, bring the other hand to the back of the pistol, over the gun
hand, squeeze its grip and pull towards you: the hand takes the slide, and all the way back
it reloads (a knock in both hands). Let go and it springs home.
Holding the Meta button recenters too.
Quit with the Meta button, then *Quit*.

## Install on the Quest

For Quest 3, 3S, 2 and Pro (Quest 2 and Pro: not tested yet, tell me how it runs).

1. **Developer mode.** Make a (free) developer account at
   [developers.meta.com](https://developers.meta.com/horizon/) with your Meta account, then in
   the Meta Horizon app on your phone: Devices > your headset > Headset settings > Developer
   mode. Restart the headset.
2. **The app.** Download `hotd2-vr-quest.apk` from
   [Releases](https://github.com/mikermak/hotd2-vr/releases) and install it with
   [SideQuest](https://sidequestvr.com/setup-howto) (*Install APK file from folder*), or
   `adb install -r hotd2-vr-quest.apk`.
3. **Your game.** Copy your own copy of the game to the headset's **Download** folder: a
   `.chd`, a `.gdi`, a `.cue` with its `.bin` files, or a `.cdi`. With SideQuest (*Manage
   files*), or connect the headset to a PC with USB, allow access in the headset, and open
   *Quest > Internal shared storage > Download*. Unpack a `.zip` or `.7z` first.
4. **Play.** Start *HOTD2 VR* from your Library (*Unknown sources*). The first time, a
   panel asks for *All files access* (to find the game in Download) and tells you what's
   still missing, if anything.

The first time the game starts, the app makes the agent's hands from your game: it plays
itself to the game over scene, fast, behind a panel that says so (a minute or two). Once only;
B skips it (the red arcade gun then, and another go next time).

Updates install over the old version; settings, saves and the hands stay.

No Dreamcast BIOS is needed: Flycast's built-in replacement (HLE) works.

## PCVR

The same, on a PC with a VR headset: SteamVR, Quest Link / Air Link or Virtual Desktop
(anything with an OpenXR runtime that does OpenGL). Not tested yet on every one of them:
tell me how it goes.

1. Download `hotd2-vr-pcvr.zip` from [Releases](https://github.com/mikermak/hotd2-vr/releases)
   and unpack it anywhere.
2. Connect the headset (start SteamVR, Quest Link or Virtual Desktop) so it is the active
   OpenXR runtime.
3. Start `flycast.exe`. Its window is Flycast's own: add the folder with your game (a `.chd`,
   `.gdi`, `.cue` or `.cdi`) and start it. It plays in the headset; the window stays as it was.

Controllers: Quest Touch as on the Quest. Valve Index: A is Start (the left A lets player 2
join), B the gun's B, a thumbstick click recenters. Vive wands and Windows Mixed Reality: the
menu button is Start (the left one lets player 2 join), a click on the pad the gun's B; the pad
or stick is the D-pad (Windows Mixed Reality: a thumbstick click recenters, Vive wands: the
runtime's own recenter). With `vr.DualWield = no` the left Start is player 1's. The first start makes the
agent's hands, as on the Quest. Settings: `emu.cfg` next to `flycast.exe` (`vr.Xr = yes`
turns VR on; without a headset the game plays in the window).

## The agent's hands (from your game)

The hands and pistol in the headset are the game's own: the agent's, as the game draws him
at the game over scene, kneeling with his pistol in his hand. Nothing of them is in this
repository or in the app: the app takes them from your copy of the game the first time
(above), into `hands.bin` in its data folder. Keep that file to yourself.

How: the game is run to that scene (Start through the notices, the title and its menu
while there's little 3D on screen, then nothing, so the agent loses), the frame with his
pistol is taken apart into the pistol, the hand around it and the open hand, lifted back
into 3D, the pistol stood upright and its slide cut free along the line painted on its
sides (`core/rend/vr/hands_rip.cpp`, `hands_build.cpp`).

On the PC the same can be done by hand, from a rip of the game over scene: build the
Windows version, install Python with numpy and pillow (`pip install -r
hotd2-vr\assets\requirements.txt`), and run `powershell -File hotd2-vr\rip-hands.ps1`
(`-NoPush` keeps it on the PC, `-Manual` lets you play to the game over yourself). It
gives the same `hands.bin`, with a preview.

## Build

On Windows, with:

- JDK 17 (a Microsoft JDK 17 in Program Files is picked up, otherwise set `JAVA_HOME`)
- the Android SDK with NDK 29.0.14206865 and CMake 3.22.1 (`ANDROID_HOME`, default
  `%LOCALAPPDATA%\Android\Sdk`)
- the git submodules: `git submodule update --init --recursive`

On Windows, two paths in the `core/deps/gamesdk` submodule are symlinks
(`games-frame-pacing/include/common` and `include/swappy`). Without symlink support
(Developer Mode plus `git config --global core.symlinks true` before cloning) they
check out as small text files and the build fails: replace them with copies of
`core/deps/gamesdk/include/common` and `core/deps/gamesdk/include/swappy`.

```bat
hotd2-vr\build-quest.cmd
```

This builds the `vr` build type (arm64, optimised, OpenXR) to
`shell\android-studio\flycast\build\outputs\apk\vr\flycast-vr.apk`. It installs
as its own app, *HOTD2 VR* (`com.flycast.emulator.vr`), next to a normal Flycast.

`hotd2-vr\build-win.cmd` builds the Windows version (Visual Studio 2022 Build Tools) with
PCVR (`-DUSE_OPENXR=ON`: CMake fetches the Khronos OpenXR loader and builds it in), also used
for the webcam finger-gun mode and for testing. `hotd2-vr\package-release.ps1` builds both and
puts the APK and the PCVR zip in `dist\`.

By hand on the Quest (without the setup panel): the app also boots the first `.cue`,
`.gdi`, `.chd` or `.cdi` in its own `files/games`:
```bat
adb shell run-as com.flycast.emulator.vr mkdir -p files/games
adb exec-in run-as com.flycast.emulator.vr sh -c "cat > files/games/hotd2.chd" < hotd2.chd
```

## The Maze of the Kings (new, experimental)

Sega's NAOMI light-gun game from 2002 (Hitmaker, the HOTD team) works too, with its own
profile: its view is widened in the game itself (the maze from 60 to 90 degrees, the story
from 40 to 60), the gun's calibration is kept neutral so shots land on the dot, the HUD and
subtitles go on the panel, the menus (drawn in layers of depth) flat on it, and the game's own
shot flash and the floating "PRESS 2P START" are gone. Two guns work as in HOTD2. First played
in the headset, then again with the staff and the two fixes below; only `vr.StaffHand` is
new and not tried there yet.

Put your own MAME set in the headset's Download folder: `mok.zip` (with its key
`317-0333-com.pic`, and the NAOMI BIOS in it or as `naomi.zip` next to it) and the folder `mok`
with the disc, `gds-0022.chd` (a `.cue` or `.gdi` of it works too). With more than one game on
the headset, the setup panel asks which one to play.

**The hero's staff.** The heroes carry a staff, not a gun, and so do you: the hero's own, head
forward, with his left glove on your other hand. Like the agent's hands in
HOTD2, the app makes it from your game the first time it starts: the game plays its attract
demo by itself, fast and without input, up to the first frame with the hero and his staff whole
in it (about half a minute of the game's time, under 20 seconds on the PC), behind a panel that
says so. B skips it (the arcade gun then, and another go next time). It goes into `staff-mok.bin`
next to `hands.bin`; each game has its own and never shows the other's. It is shortened to
about 1 m (the game's is 1.4 m: the shaft behind the fist is cut down to a hand's width, its
gold tail put back on there); grip + thumbstick left/right makes it bigger or smaller
(`vr.HandScale`). There's no slide to rack: reload by pointing away from the screen and
pulling the trigger. His right glove around it is left out: the game made that fist for a staff
held upright like a walking stick, so with the staff pointing forward its wrist pointed at the
floor (`vr.StaffHand` brings it back).

Fixed since the first go in the headset:
- Streaks over the floor. The game clips its 3D at W 1.0 (the floor under the camera, the walls
  beside it, the desert's ground and sky), right where the 2D plane was, so the corners it cut
  there were pinned onto the plane. Its 2D plane is now at its HUD's W, 1.082.
- People beside the camera in the story scenes went missing when you turned your head: the
  game culls models against its own frame, with a margin of half a bounding sphere (the `0.5`
  at `0x8C08DC4C`). Made 4, they are drawn too (about a third more polygons in the story; the
  game's own frame stays the same).

How it was worked out (PC: `vr.DebugShots`, rips and RAM dumps): its field of view comes from
camera data, not code, as one angle the projection builder halves; three instructions of the
builder (`0x8C08D8EE`) make that three quarters instead. Its light-gun hit test keeps the stock
focal (`0x0C0E7248`), which the aim follows. The staff is one model with one texture (a head
piece and a shaft, 66 polygons), the gloves part of the hero's body texture; the app takes the
rod his right glove is around, whole (its 66 polygons with all their 514 corners, and nothing of
it or the gloves on the near plane, which keeps the polygons it cuts), stands it along its axis
with the head forward and the ears of the animal head on it up, and fits his left glove onto
the mirror image of the right one (`hands_build.cpp`, `buildStaff`).

## Options

Settings live in `/sdcard/Android/data/com.flycast.emulator.vr/files/emu.cfg` (PC: `emu.cfg`
next to `flycast.exe`), under `[config]`. The useful ones:

| Option | Default | |
|---|---|---|
| `vr.WorldScale` | 0.025 | metres per game unit (also set with grip + thumbstick) |
| `vr.FovScale` | 2 (profile) | how much wider than the original (tan of the half angle) |
| `vr.WidenFov` | yes | widen through the game's own field of view (the profile knows where) |
| `vr.Laser` | yes | laser line and dot |
| `vr.ShowGun` | yes | the gun model |
| `vr.GunScale` | 0.68 | size of the gun, times the 25 cm arcade gun it is modelled on (also set with grip + thumbstick) |
| `vr.GameHands` | yes | the agent's hands and pistol, or the hero's staff (made from the game at the first start) |
| `vr.HandScale` | 1 | their size: 1 is a 20 cm pistol, a 1 m staff (also set with grip + thumbstick) |
| `vr.SlideReload` | yes | rack the slide with the other hand to reload |
| `vr.StaffHand` | no | the hero's glove around his staff (The Maze of the Kings) |
| `vr.DropShotMarker` | yes | hide the game's own 2D shot flash after shots into the 3D scene (with `no` it shows at the stock-framed spot, not where the shot landed) |
| `vr.DualWield` | yes | the left controller's ≡ lets player 2 join with a gun in your left hand |
| `vr.HideP2Prompt` | yes | no "PRESS START BUTTON / CREDIT(S)" for player 2 in a one-player game |
| `vr.DropShotFlash` | yes | hide a bright full-screen flash right after a shot, should the game draw one |
| `vr.ComfortStart` / `vr.ComfortMin` | 0.8 / 0.4 | comfort zone, metres (0: off) |
| `vr.HudDepth` | 40 | distance of the 2D plane, game units |
| `vr.XrRefreshRate` | 72 | Hz |
| `vr.XrResolution` | 1 | eye buffer size, times the recommended size |
| `vr.Xr` | yes (PC: no) | VR; the PCVR zip's `emu.cfg` turns it on |

## How it works

- `core/rend/vr_reproject.*`: rebuilds eye space from the projected vertices with the
  game's live focal length (read from its projection and viewport matrices in RAM), and
  the per-game profile: matrix addresses and the field-of-view literals to patch.
  HOTD2's four perspective setups load their angle (`0x1D3C`, 41.1°) from two literals
  (`0x8C029C12`, `0x8C02B370`) for the routine at `0x8C0383C0`.
- `core/rend/gles/gles.cpp`: the vertex shader does the lifting and reprojection; only
  frames the game actually presents reach the headset.
- `core/hw/pvr/ta_vtx.cpp`: hands each parsed render pass to `vr::dropShotMarker` before
  its index is built, which hides the game's 2D shot marker after shots into 3D.
- `core/rend/vr/xr_host.*`: the OpenXR session (bound to Flycast's GL context: EGL and
  GLES on the Quest, WGL and desktop OpenGL on the PC), headset-paced frames, controllers
  and the light gun ray cast.
- `core/rend/vr/xr_gun.*`, `gun_model.h`: the gun, its effects and shaders.
- `core/rend/vr/xr_hands.*`: the agent's hands and pistol from `hands.bin`, with the
  slide that moves, or the hero's staff from `staff-mok.bin` (only the running game's file).
  The other hand's grip and the rack are in `xr_host.cpp`.
- `core/rend/vr/hands_rip.*`, `hands_build.*`: the run to the game over scene and the
  model made from it (the C++ twin of `hotd2-vr/assets/rip_hands.py`); in The Maze of the
  Kings the run through its attract demo and the staff.
- `core/rend/vr/xr_panel.*`: a text panel in the headset (the hands run).
- `shell/android-studio/.../VrSetupActivity.java`, `VrGames.java`: the setup panel and
  finding the game on the headset.
- `core/input/udp_lightgun.*`: the light gun can also be driven over UDP on 127.0.0.1
  (used by the finger-gun tracker).

## Tools

- `hotd2-vr/fingergun`: play on the PC with **finger guns** in front of a webcam
  (MediaPipe hand tracking). Set up once:
  ```bat
  py -m venv hotd2-vr\fingergun\.venv
  hotd2-vr\fingergun\.venv\Scripts\pip install -r hotd2-vr\fingergun\requirements.txt
  ```
  and put MediaPipe's [hand landmarker model](https://ai.google.dev/edge/mediapipe/solutions/vision/hand_landmarker)
  (`hand_landmarker.task`) in `hotd2-vr\fingergun\`. Then `hotd2-vr\play-fingergun.ps1`
  (after `build-win.cmd`). It starts with a guided calibration; its on-screen text is in Dutch.
  Aim with your index finger, fire by dropping your thumb, reload by opening your hand for
  a moment (`--reload down` brings back the old point-the-gun-down reload). The
  calibration (`K`) moves the game's crosshair to nine targets in turn; aim at each. Aiming
  then follows where your hand is: a webcam in front of you can't tell which way a finger
  points straight at it, so the finger's direction is left out. Press `C` while aiming at
  the centre whenever you sit differently.
  Two players: `play-fingergun.ps1 -Players 2`, two people side by side in front of one
  webcam, one gun hand each (left in the preview is player 1, red crosshair; right is
  player 2, light blue). `K` calibrates both in turn, each with their own settings. A
  thumbs-up (a fist, thumb up) held for half a second presses the game's Start: that is how
  player 2 joins in (and it pauses for a player already in the game).
- `hotd2-vr/gun_model`: bakes a `.glb` gun model into `gun_model.h`.
- `hotd2-vr/rip-hands.ps1`, `hotd2-vr/assets`: the agent's hands from your game on the PC
  (above). The PC build rips a frame on request (`rip.request` in its working folder: every polygon as
  the GPU got it, with the VRAM), `assets/riplib.py` reads that and decodes the textures,
  and `assets/rip_hands.py` lifts the hands and pistol back into 3D, stands the pistol
  upright, cuts its slide free along the line painted on its sides and writes `hands.bin`.
- `hotd2-vr/re/sh4dis.py`: disassembles game code from a Flycast RAM dump (Capstone),
  which is how the field-of-view literals were found.
- `hotd2-vr/run-test.ps1`: PC test runs with screenshots.

The scripts look for your game in a `game` folder next to the repository, or in
`%HOTD2VR_DATA%\game`.

## Credits and licences

- [Flycast](https://github.com/flyinghead/flycast) by flyinghead and contributors, GPL-2.0
  or later. This fork's code is too (see `LICENSE`); changed Flycast files carry a note,
  and `git log master..hotd2-vr` shows every change. Flycast's own readme:
  [README.flycast.md](README.flycast.md).
- The gun: ["Namco Arcade Gun"](https://sketchfab.com/3d-models/namco-arcade-gun-15fbd5b9add94a34b5c21746e3dd32be)
  by [Martoscar](https://sketchfab.com/Martoscar), licensed
  [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). Changed: re-oriented and
  moved into the controller's aim space, quantised, textures and materials dropped,
  recoloured, converted to a C++ header (`core/rend/vr/gun_model.h`, CC BY 4.0). Since CC
  BY 4.0 goes together with GPL-3.0 but not GPL-2.0, builds that include it are
  distributed under GPL-3.0, which Flycast's "or later" allows.
- [OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK) by Khronos, Apache-2.0.
- Roboto (Flycast's own copy, on the headset's panel) by Google, Apache-2.0.
- [MediaPipe](https://github.com/google-ai-edge/mediapipe) by Google, Apache-2.0
  (finger-gun tracker); [Capstone](https://www.capstone-engine.org/), BSD.
- Inspired by [DR-89/time-crisis-vr](https://github.com/DR-89/time-crisis-vr).
- The House of the Dead is a trademark of Sega. Namco is a trademark of Bandai Namco.
