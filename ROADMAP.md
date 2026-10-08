# Roadmap

What's next for The House of the Dead 2 VR, in order. Most of it comes straight from the
comments on the first video. Thanks for all of them.

## 1. Download and play (built, being tested)

Until now you had to build the app yourself, copy the game in with `adb` and rip the hands
on a PC. That's too much. Now: download one file, drop your game on the headset, play.

- **A ready-made APK** on the [Releases](https://github.com/mikermak/hotd2-vr/releases) page,
  installable with SideQuest (no command line). Signed with one fixed key, so updates install
  over the old version and keep your settings and saves. It contains nothing of the game.
- **Your game from the Download folder.** Copy your own `.chd`, `.gdi`, `.cue` or `.cdi` to the
  headset's Download folder (SideQuest, or USB from Windows Explorer). The app asks once for
  file access and finds it, whatever the file is called.
- **A setup screen in the headset** that says what it found and what's still missing, instead
  of a black screen.
- **The agent's hands without a PC.** The first time you start the game, the app takes the
  hands and pistol from your own copy, on the headset, once: it plays itself to the game over
  scene, fast, behind a panel. No Python, no script. B skips it (the red arcade gun then).
- **Quest 2 and Quest Pro.** The app was locked to Quest 3 and 3S. It doesn't use anything
  those headsets lack, so now it runs on them too. Quest 2 owners: tell me how it runs. If
  needed, there'll be a lighter preset.
- **No more floating "Player 2" text** in one-player games.

## 2. PCVR (built, being tested)

The same mod on a PC: SteamVR, Quest Link / Air Link, Virtual Desktop. Same renderer,
same hands and slide reload. On the PC the game can also render at a higher resolution.

- The OpenXR part has a Windows version (desktop OpenGL instead of Android's GLES).
- Controller bindings for Quest Touch, Index, Vive and Windows Mixed Reality.
- A zip with the program: unpack, point it at your game, play.

## 3. Two guns (built, being tested)

One player, a gun in each hand: the left gun is player 2. Both health bars, both scores.
This is also the first step towards real two-player games.

## 4. Two players, two headsets

Two Quests (or a Quest and a PC) in the same game, each player looking around from their own
head, standing side by side like at the cabinet.

- Flycast already has rollback netplay (GGPO) that works with Dreamcast light gun games:
  both headsets run the same game and only send each other their gun inputs.
- First on the same Wi-Fi, finding each other by themselves (no typing IP addresses).
- You see the other player's hands and gun next to you.
- Then over the internet, with a room code.

This is the biggest step, so it comes last. It needs both headsets to stay exactly in sync,
which has to be tested well before it goes out.

## Later, maybe

- **The Maze of the Kings** (NAOMI): built, being tested.
- Other Dreamcast and NAOMI light gun games (Confidential Mission, Death Crimson OX, Ninja
  Assault, Gun Survivor 2) run on the same Flycast base, so they're the most likely next games.
- The House of the Dead 1 is an arcade (Model 2) and Saturn game, not a Dreamcast one, so it
  would be a separate project. [port0r](https://github.com/jacquesdupontd/port0r) is
  already working on it.
- Hold the trigger for auto fire, for the trigger fingers.

No dates: this is a hobby project. Requests and Quest 2 test reports are welcome in the
[issues](https://github.com/mikermak/hotd2-vr/issues).
