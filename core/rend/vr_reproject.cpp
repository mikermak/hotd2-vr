/*
	VR reprojection prototype (hotd2-vr). See vr_reproject.h.

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "vr_reproject.h"
#include "vr/xr_host.h"
#include "vr/hands_rip.h"
#include "transform_matrix.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/sh4/sh4_mem.h"
#include "stdclass.h"
#include "hw/maple/maple_cfg.h"
#include "cfg/option.h"
#include "emulator.h"
#include "log/Log.h"
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iterator>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vr
{

static ReprojectParams params;
static u64 frameCount;

// Near plane as a fraction of the closest vertex depth: vertices are never clipped by it
// in the identity reprojection, but anything behind the free camera is.
constexpr float NearFraction = 0.01f;

// Offsets of the x and y scale terms in a 4x4 float matrix (diagonal, either order).
constexpr u32 ScaleX = 0;
constexpr u32 ScaleY = 5 * 4;

static float readFloat(u32 ramOffset)
{
	u32 raw = ReadMem32_nommu(0x8C000000u + (ramOffset & 0x01FFFFFFu));
	float f;
	memcpy(&f, &raw, sizeof(f));
	return f;
}

static void writeFloat(u32 ramOffset, float f)
{
	u32 raw;
	memcpy(&raw, &f, sizeof(raw));
	WriteMem32_nommu(0x8C000000u + (ramOffset & 0x01FFFFFFu), raw);
}

static bool plausible(float v, float lo, float hi) {
	return std::isfinite(v) && std::abs(v) > lo && std::abs(v) < hi;
}

// The game's focal length in DC pixels, per axis: viewport scale times projection scale.
static glm::vec2 readGameFocal()
{
	if (config::VrProjAddr > 0 && config::VrViewportAddr > 0)
	{
		const glm::vec2 focal(
				std::abs(readFloat(config::VrViewportAddr + ScaleX) * readFloat(config::VrProjAddr + ScaleX)),
				std::abs(readFloat(config::VrViewportAddr + ScaleY) * readFloat(config::VrProjAddr + ScaleY)));
		if (plausible(focal.x, 10.f, 20000.f) && plausible(focal.y, 10.f, 20000.f))
			return focal;
	}
	return glm::vec2(config::VrFocal);
}

//
// Widening the game's view, two ways.
//
// Best: the field of view the game builds its projection from. HOTD2 passes it as a
// 16-bit angle (65536 = a full turn) from a few literals in its code; raised there, the
// game itself draws and culls with the wider view (its light gun hit test keeps the stock
// view, measured on the headset). Checked every vblank, since the code is loaded after the
// game starts. See GameProfile::fovLiterals.
//
// Otherwise: scale down the viewport matrix's x/y terms by vr.FovScale. The game then
// maps (and culls against the screen) a wider cone into the same 640x480, but anything
// it computes for the screen on its own, like the light gun hit test, keeps the stock
// view. The projection itself can't be scaled: the game rebuilds it every frame. The
// viewport is set once per scene, so it is checked every vblank: a value differing from
// what we last wrote is fresh from the game and gets scaled; our own value is left alone.
//
static glm::vec2 lastWritten;
static u32 fovLiterals[4];		// RAM offsets of the game's field of view angle (0: none)
static u16 fovStock;			// the angle the game ships with

// The angle (65536 = full turn) that widens stock by `scale` (as tan of the half angle).
static u16 widenedAngle(u16 stock, float scale)
{
	const float half = stock * (3.14159265f / 65536.f);
	const float wide = 2.f * std::atan(std::tan(half) * scale);
	return (u16)std::lround(wide * (65536.f / 6.2831853f));
}

static bool widensByCode();

bool widensGameFov() {
	return fovLiterals[0] != 0 || widensByCode();
}

static void widenGameView(Event event, void *)
{
	if (event == Event::Start)
	{
		lastWritten = glm::vec2(0.f);
		return;
	}
	const float scale = config::VrFovScale;
	if (!config::VrReproject || scale <= 1.f)
		return;
	if (widensGameFov())
	{
		const u16 wide = widenedAngle(fovStock, scale);
		for (u32 offset : fovLiterals)
		{
			if (offset == 0)
				break;
			const u32 addr = 0x8C000000u + offset;
			// only the stock value: anything else isn't the code we know (not loaded yet)
			if (ReadMem16_nommu(addr) == fovStock)
			{
				WriteMem16_nommu(addr, wide);
				NOTICE_LOG(RENDERER, "VR: field of view %.1f -> %.1f degrees at %08x", fovStock * 360.f / 65536.f, wide * 360.f / 65536.f, addr);
			}
		}
		return;
	}
	if (config::VrViewportAddr <= 0 || widensByCode())
		return;
	const u32 addr = config::VrViewportAddr;
	const glm::vec2 cur(readFloat(addr + ScaleX), readFloat(addr + ScaleY));
	if (cur == lastWritten || !plausible(cur.x, 10.f, 5000.f) || !plausible(cur.y, 10.f, 5000.f))
		return;
	lastWritten = cur / scale;
	writeFloat(addr + ScaleX, lastWritten.x);
	writeFloat(addr + ScaleY, lastWritten.y);
}

static struct WidenGameViewRegistration
{
	WidenGameViewRegistration() {
		EventManager::listen(Event::Start, widenGameView);
		EventManager::listen(Event::VBlank, widenGameView);
	}
} widenGameViewRegistration;

static void logSceneStats(const rend_context& ctx, glm::vec2 focal)
{
	std::vector<float> invW;
	invW.reserve(ctx.verts.size());
	size_t overlay = 0;
	for (const Vertex& v : ctx.verts)
		if (std::isfinite(v.z) && v.z > 0.f)
		{
			invW.push_back(v.z);
			if (std::abs(1.f / v.z - config::VrHudW) < 0.002f)
				overlay++;
		}
	if (invW.empty())
		return;
	std::sort(invW.begin(), invW.end());
	auto w = [&](float q) { return 1.f / invW[(size_t)(q * (invW.size() - 1))]; };
	NOTICE_LOG(RENDERER, "VR: focal %.1f x %.1f  verts %zu overlay %zu  W near %.3f p10 %.3f median %.3f p90 %.3f far %.3f",
			focal.x, focal.y, invW.size(), overlay,
			w(1.f), w(0.9f), w(0.5f), w(0.1f), w(0.f));
}

//
// Per-game profiles, applied when the game starts so the Quest build needs no manual
// config. Values already set in the config file win.
//
struct CodePatch
{
	u32 offset;			// RAM offset of a 16-bit word of code
	u16 stock, patched;
};
struct Poke
{
	u32 offset;			// RAM offset of a 16-bit word, written every frame
	u16 value;
};
struct ScreenBox
{
	float x0, y0, x1, y1;	// DC pixels
};
struct GameProfile
{
	const char *gameId = "";
	// The projection and viewport matrices in RAM: the game's live focal length.
	int projAddr = 0, viewportAddr = 0;
	float fovScale = 1.f;
	// Widening the game's own view: either the 16-bit angle literals its code builds its
	// projection from (scaled by fovScale, see widenGameView)...
	u32 fovLiterals[4] {};
	u16 fovStock = 0;
	// ...or code changed (every word stock, else none of them).
	CodePatch fovPatch[4] {};
	// The focal length the game's light-gun hit test uses (its stock view): a float in RAM
	// (it may change from scene to scene), else vr.Focal. And the stock framing of the 2D
	// plane (0: vr.Focal).
	u32 stockFocalAddr = 0;
	float focal = 0.f;
	// Written every frame once the game's code is there (its fovPatch words stock or patched).
	Poke pokes[12] {};
	// The textures of the game's own hit marker (VRAM addresses) and the W it is drawn at:
	// see dropShotMarker.
	u32 markerTextures[4] {};
	float markerMinW = 0.85f, markerMaxW = 1.15f;
	// Player 2's prompt pictures and where they are: see hidePlayer2Prompt.
	u32 p2PromptTextures[4] {};
	ScreenBox p2PromptBox { 352.f, 384.f, 640.f, 480.f };
	// 2D layers near the 2D plane (see snapNear2D): up to this W, at W in multiples of
	// this step; the W of the letterbox bars (0: the 2D plane's).
	float snapMaxW = 1.05f, snapStep = 0.01f;
	float letterboxW = 0.f;
	// The W of its 2D plane (0: vr.HudW, HOTD2's 1.0). Never a W its 3D can have: the
	// headset's vertex shader pins every vertex near it onto the 2D plane.
	float hudW = 0.f;
	// Screens without any 3D (menus drawn in layers of depth): all on the 2D plane.
	bool flatScreens = false;
	// The size of its game units next to HOTD2's: vr.WorldScale (metres per HOTD2 unit, the
	// player's own setting) times this.
	float unitScale = 1.f;
	// The player's own model from the game, in a file of its own (hands_rip.h): the agent's
	// hands and pistol, the hero's staff, or none (the arcade gun).
	hands::Source model {};
};

static std::vector<GameProfile> makeProfiles()
{
	std::vector<GameProfile> list;
	GameProfile p;
	// The House of the Dead 2 (PAL). Its four perspective setups (0x8C029B48, 0x8C02B2C6,
	// 0x8C02B30A, 0x8C02B34E) load 41.1 degrees from two literals and call 0x8C0383C0.
	// Its hit marker: a glow (0x59bb80, ~62 px) on the shot and rays (0x587380) flying out
	// ~100 px, drawn at W 0.9..1.01 (measured on the PC, chapter 1 and the attract demo).
	// Player 2's prompt in the bottom right corner: 0x57b700 and 0x56bb00 (chapter 1).
	p.gameId = "MK-5100250";
	p.projAddr = 0x4C65E0;
	p.viewportAddr = 0x4C6708;
	p.fovScale = 2.f;
	p.fovLiterals[0] = 0x029C12;
	p.fovLiterals[1] = 0x02B370;
	p.fovStock = 7484;
	p.markerTextures[0] = 0x59bb80;
	p.markerTextures[1] = 0x587380;
	p.p2PromptTextures[0] = 0x57b700;
	p.p2PromptTextures[1] = 0x56bb00;
	// The agent's parts in the game over scene: his hands, the cuff, the pistol.
	p.model.kind = hands::Source::AgentsPistol;
	p.model.file = "hands.bin";
	p.model.pistol = { { 0x5fa380, 0x5fc380 }, 0x601380, 0x5fe380 };
	list.push_back(p);
	// The House of the Dead 2 (USA): the same code and data as PAL, elsewhere (found by
	// comparing RAM and VRAM dumps of both at the same moments: code 0x280 lower, the matrices
	// 0x680 lower, the setups at 0x8C0298C4, 0x8C02B042, 0x8C02B086 and 0x8C02B0CA calling
	// perspective() at 0x8C038140; every texture an exact byte match of PAL's).
	p.gameId = "MK-51002";
	p.projAddr = 0x4C5F60;
	p.viewportAddr = 0x4C6088;
	p.fovLiterals[0] = 0x02998E;
	p.fovLiterals[1] = 0x02B0EC;
	p.markerTextures[0] = 0x59e000;
	p.markerTextures[1] = 0x589000;
	p.p2PromptTextures[0] = 0x53c400;
	p.p2PromptTextures[1] = 0x572000;
	p.model.pistol = { { 0x5fc800, 0x5fe800 }, 0x603800, 0x600800 };	// (the same hands.bin as PAL's)
	list.push_back(p);
	// The Maze of the Kings (NAOMI, Hitmaker 2002; worked out from rips, RAM dumps and its
	// code, see hotd2-vr/README). Its field of view changes from scene to scene (60 degrees
	// in the maze, 40 in the story, 50 in menus, 80 in the attract) and comes from camera
	// data, not code: one angle A, set through setPerspective 0x8C074630 / setFovAngle
	// 0x8C0746B0, which keep the stock focal 240/tan(A/2) at 0x0C0E7248 (what the light-gun
	// hit test unprojects with) and call the projection builder 0x8C08D8D0. Its prologue
	// is changed to take 0.75A as the half angle instead of A/2 (mov r4,r13; shlr2 r4;
	// sub r4,r13 in place of a dead stack store, its reload and shar r13): the projection,
	// culling planes and screen matrices all 1.5 times as wide (the maze 90 degrees). The
	// matrices: projection 0x0C0E7A60 (f/320, f/240), viewport 0x0C0F24D0 (320, -240).
	// It culls models by their bounding spheres against its own frame (0x8C084BB0 and the
	// node loops at 0x8C0912A0, 0x8C0923C2, 0x8C09916E), with a margin both builders make
	// from a 0.5 at 0x8C08DC4C (0x0C0F2550/54, read only there): made 4.0, a sphere counts
	// eight times its size, so the people and things beside the camera in the story scenes,
	// in view when you turn your head, are drawn too (the game's own frame stays the same;
	// about a third more polygons in the story).
	p = GameProfile();
	p.gameId = "THE MAZE OF THE KINGS";
	p.projAddr = 0x0E7A60;
	p.viewportAddr = 0x0F24D0;
	p.fovScale = 2.f;	// (only says "widen": the patch's own factor is fixed)
	p.fovPatch[0] = { 0x08D8EE, 0x1F41, 0x6D43 };
	p.fovPatch[1] = { 0x08D8F2, 0x5DF1, 0x4409 };
	p.fovPatch[2] = { 0x08D8F6, 0x4D21, 0x3D48 };
	p.fovPatch[3] = { 0x08DC4E, 0x3F00, 0x4080 };
	p.stockFocalAddr = 0x0E7248;
	p.focal = 415.72f;	// the maze's
	// The gun calibration (ROD CONTROLLER SETTINGS; factory 64, 128, 500, 0, 128, 500 for
	// each player at 0x0C0E2058 and 0x0C0E2064) stretches the gun's position outward from
	// the centre, up to 30 px off. Kept at 0, 0, 512, 0, 0, 512, the game's aim is where the
	// gun points (within a pixel).
	const u16 calibration[6] { 0, 0, 512, 0, 0, 512 };
	for (int player = 0; player < 2; player++)
		for (int k = 0; k < 6; k++)
			p.pokes[player * 6 + k] = { (u32)(0x0E2058 + player * 12 + k * 2), calibration[k] };
	// Its shot flash: a starburst (0x50b800) and red stars (0x4f6800) at W 1.5.
	p.markerTextures[0] = 0x50b800;
	p.markerTextures[1] = 0x4f6800;
	p.markerMinW = 1.45f;
	p.markerMaxW = 1.55f;
	// "PRESS 2P START" at the bottom centre (the same sheets as FREE PLAY under it and
	// player 1's prompt, which are wider and lower).
	p.p2PromptTextures[0] = 0x68d800;
	p.p2PromptTextures[1] = 0x48c000;
	p.p2PromptBox = { 250.f, 440.f, 390.f, 462.f };
	// The HUD at W 1.082, subtitles 1.040, area names 1.052-1.054, CONTINUE 1.08-1.085, the
	// damage flash 1.4 and its shot effects 1.5, in steps of 0.001; letterbox bars at 1.090.
	p.snapMaxW = 1.6f;
	p.snapStep = 0.001f;
	p.letterboxW = 1.090f;
	// Its near plane is at W 1.0 exactly (the floor under the camera, the walls beside it,
	// the desert's ground and sky domes are clipped there, 20-340 corners a frame): with the
	// 2D plane at 1.0 those corners were pinned onto it, streaks over the floor. Its HUD's W
	// instead; FREE PLAY at 1.000 is snapped like the other layers.
	p.hudW = 1.082f;
	p.flatScreens = true;
	// The camera rides about 14.5 units over the floor in the maze: eye height at 0.11 m a
	// unit, 4.4 times HOTD2's 0.025.
	p.unitScale = 4.4f;
	// The hero's staff, in his right fist, as the attract demo shows it from 32 s on (and
	// the story's intro): the rod's texture and the hero's body's (his gloves are on it), in
	// VRAM where each of the two scenes puts them. (The heroine's body is another texture.)
	p.model.kind = hands::Source::HerosStaff;
	p.model.file = "staff-mok.bin";
	p.model.staff = { { { 0xC1C000, 0xCB0800 }, { 0xD87000, 0xDA9800 } } };
	list.push_back(p);
	return list;
}
static const std::vector<GameProfile> profiles = makeProfiles();
static const GameProfile *activeProfile;	// the game being played (or none)

const hands::Source *gameModelSource()
{
	for (const GameProfile& prof : profiles)
		if (settings.content.gameId == prof.gameId && prof.model.kind != hands::Source::None)
			return &prof.model;
	return nullptr;
}

static u32 markerTextures[4];	// the active profile's GameProfile::markerTextures
static u32 p2PromptTextures[4];	// ...and GameProfile::p2PromptTextures

static void applyGameProfile(Event, void *)
{
	std::fill(std::begin(fovLiterals), std::end(fovLiterals), 0u);
	std::fill(std::begin(markerTextures), std::end(markerTextures), 0u);
	std::fill(std::begin(p2PromptTextures), std::end(p2PromptTextures), 0u);
	activeProfile = nullptr;
	if (xr::enabled())
	{
		// Everything the headset view depends on.
		config::VrReproject.override(true);
		config::NativeDepthInterpolation.override(true);
	}
	for (const GameProfile& prof : profiles)
	{
		if (settings.content.gameId != prof.gameId)
			continue;
		activeProfile = &prof;
		if (config::VrProjAddr == 0)
			config::VrProjAddr.override(prof.projAddr);
		if (config::VrViewportAddr == 0)
			config::VrViewportAddr.override(prof.viewportAddr);
		if (prof.focal > 0.f)
			config::VrFocal.override(prof.focal);
		if (prof.hudW > 0.f && config::VrHudW == 1.f)	// (unless the player set one)
			config::VrHudW.override(prof.hudW);
		if (xr::enabled() && config::VrWiden && config::VrFovScale <= 1.f)
			config::VrFovScale.override(prof.fovScale);
		if (config::VrWidenFov)
		{
			std::copy(std::begin(prof.fovLiterals), std::end(prof.fovLiterals), std::begin(fovLiterals));
			fovStock = prof.fovStock;
		}
		std::copy(std::begin(prof.markerTextures), std::end(prof.markerTextures), std::begin(markerTextures));
		std::copy(std::begin(prof.p2PromptTextures), std::end(prof.p2PromptTextures), std::begin(p2PromptTextures));
		const bool gunB = config::VrDualWield;
		if (xr::enabled() && config::VrXrGun && settings.platform.isConsole()
				&& (config::MapleMainDevices[0] != MDT_LightGun || (gunB && config::MapleMainDevices[1] != MDT_LightGun)))
		{
			// The right controller is a light gun: plug one into port A (a pad ignores
			// where it points), and player 2's into port B for dual wielding (vr.DualWield).
			// The devices were made just before this event, before the game runs, so they
			// can still be swapped. (An arcade game's guns are on its I/O board already.)
			config::MapleMainDevices[0].override(MDT_LightGun);
			if (gunB)
				config::MapleMainDevices[1].override(MDT_LightGun);
			mcfg_DestroyDevices();
			mcfg_CreateDevices();
			NOTICE_LOG(RENDERER, "VR: light gun in port A%s", config::VrDualWield ? " and B" : "");
		}
		NOTICE_LOG(RENDERER, "VR: camera profile for %s", prof.gameId);
	}
}

static struct GameProfileRegistration
{
	GameProfileRegistration() {
		EventManager::listen(Event::Start, applyGameProfile);
	}
} gameProfileRegistration;

static bool widensByCode() {
	return activeProfile != nullptr && activeProfile->fovPatch[0].offset != 0 && config::VrWidenFov;
}

// The code a profile's patch and pokes are for is in RAM: every patched word stock or patched.
static bool profileCodeThere(const GameProfile& prof)
{
	if (prof.fovPatch[0].offset == 0)
		return true;
	for (const CodePatch& c : prof.fovPatch)
	{
		if (c.offset == 0)
			break;
		const u16 now = ReadMem16_nommu(0x8C000000u + c.offset);
		if (now != c.stock && now != c.patched)
			return false;
	}
	return true;
}

// Each frame: the profile's code patch (all of it, once the stock code is there) and pokes.
static void applyProfileWrites(Event, void *)
{
	const GameProfile *prof = activeProfile;
	if (prof == nullptr || !config::VrReproject || !profileCodeThere(*prof))
		return;
	if (prof->fovPatch[0].offset != 0 && config::VrWidenFov && config::VrFovScale > 1.f)
	{
		bool stock = true;
		for (const CodePatch& c : prof->fovPatch)
			stock = stock && (c.offset == 0 || ReadMem16_nommu(0x8C000000u + c.offset) == c.stock);
		if (stock)
		{
			// (the dynarec drops what it compiled from these pages when they're written)
			for (const CodePatch& c : prof->fovPatch)
				if (c.offset != 0)
					WriteMem16_nommu(0x8C000000u + c.offset, c.patched);
			NOTICE_LOG(RENDERER, "VR: the game's field of view widened (code at %08x)", 0x8C000000u + prof->fovPatch[0].offset);
		}
	}
	for (const Poke& poke : prof->pokes)
		if (poke.offset != 0 && ReadMem16_nommu(0x8C000000u + poke.offset) != poke.value)
			WriteMem16_nommu(0x8C000000u + poke.offset, poke.value);
}

static struct ProfileWritesRegistration
{
	ProfileWritesRegistration() {
		EventManager::listen(Event::VBlank, applyProfileWrites);
	}
} profileWritesRegistration;

float worldScale()
{
	const GameProfile *prof = activeProfile;
	return config::VrWorldScale * (prof != nullptr ? prof->unitScale : 1.f);
}

// The stock focal length the game's light-gun hit test uses, in DC pixels.
static float stockFocal()
{
	const GameProfile *prof = activeProfile;
	if (prof != nullptr && prof->stockFocalAddr != 0)
	{
		const float f = readFloat(prof->stockFocalAddr);
		if (plausible(f, 10.f, 20000.f))
			return f;
	}
	return config::VrFocal;
}

GameCamera gameCamera(const rend_context& ctx)
{
	int dcWidth, dcHeight;
	getPvrFramebufferSize(ctx, dcWidth, dcHeight);
	GameCamera cam;
	cam.dcSize = glm::vec2(dcWidth, dcHeight);
	cam.tanHalf = cam.dcSize * 0.5f / readGameFocal();
	cam.stockTan = cam.dcSize * 0.5f / stockFocal();
	cam.overlayW = config::VrHudW;
	return cam;
}

static glm::mat4 projection(glm::vec2 tanHalf, float zNear)
{
	glm::mat4 proj(0.f);
	proj[0][0] = 1.f / tanHalf.x;
	proj[1][1] = 1.f / tanHalf.y;
	proj[2][2] = -1.f;
	proj[2][3] = -1.f;
	proj[3][2] = -2.f * zNear;
	return proj;
}

glm::vec3 comfortParams()
{
	const float start = config::VrComfortStart;
	if (start <= 0.f)
		return glm::vec3(0.f);
	// at most half the start distance: the curve stays monotonic
	const float closest = std::clamp((float)config::VrComfortMin, 0.f, start * 0.5f);
	return glm::vec3(worldScale(), start, closest);
}

//
// The game's own 2D shot marker: the little flash it draws on its 640x480 screen where the
// light gun was when it fired. In the headset that screen is a plane at the stock framing,
// so after a shot into the (wider) 3D scene the marker shows up somewhere else, and at
// another depth, than the hit, next to our own muzzle flash and aim dot: it goes. After a
// shot at the 2D plane itself (menus, text) it stays, since there it is where the shot
// landed. It is taken out while the TA's polygons are still whole (before indexing merges
// and sorts them): NaN x/y keeps its vertices out of every index, sorted or not, like the
// PVR's own invalid vertices (is_vertex_inf). z stays finite for the sorters.
//
constexpr float MarkerMaxSize = 96.f;	// DC pixels, width and height (HOTD2's glow is 62)
constexpr float MarkerMaxOffset = 120.f;	// DC pixels, from its centre to the gun (its rays fly out ~100)
constexpr int64_t MarkerMs = 500;		// after the trigger pull
// A bright full-screen flash right after a shot (light guns on a CRT need one to find the
// beam), should the game draw it: vr.DropShotFlash.
constexpr int64_t FlashMs = 200;

// The last few shots: in a fight they come ~0.2 s apart, so with only the newest one the
// marker of the one before would come back for the rest of its life.
struct GunShot
{
	std::atomic<int64_t> ms { std::numeric_limits<int64_t>::min() / 2 };
	std::atomic<u32> pos { 0 };			// (y << 16) | x, DC pixels
	std::atomic<bool> into3D { false };
};
static GunShot gunShots[4];
static std::atomic<u32> nextShot;

static int64_t steadyMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();
}

void noteGunShot(int x, int y, bool into3D)
{
	if (x < 0 || y < 0 || x >= 640 || y >= 480)
		return;
	GunShot& s = gunShots[nextShot.fetch_add(1) % std::size(gunShots)];
	s.ms = std::numeric_limits<int64_t>::min() / 2;	// invalid while it changes
	s.pos = ((u32)y << 16) | (u32)x;
	s.into3D = into3D;
	s.ms = steadyMs();	// last: whoever sees the new time sees the rest
}

static bool onOverlay(const Vertex& v, float hudW) {
	return v.z > 0.f && std::abs(1.f / v.z - hudW) < 0.002f;
}

static bool opaqueBlack(const Vertex& v) {
	return v.col[3] > 250 && v.col[0] < 3 && v.col[1] < 3 && v.col[2] < 3;
}

static void hideVertices(rend_context& ctx, const PolyParam& pp)
{
	for (u32 j = pp.first; j < pp.first + pp.count; j++)
		ctx.verts[j].x = ctx.verts[j].y = std::numeric_limits<float>::quiet_NaN();
}

// 2D layers the game draws just in front of or behind its 2D plane (HOTD2: text at W 0.98
// and 0.99 over boxes at W 1, other layers at 0.80 and 1.02): screen-aligned polygons, every
// vertex at the same W. Off the plane they went through the 3D path, ~2 cm in front of the
// eyes, and the comfort zone pushed them back bent ("B to cancel" sucked into a vortex).
// They are pulled onto the plane, their order kept. Only what looks like such a layer:
// one W, a round one (a multiple of 0.01: sprites and debris flying past the camera pass
// through every W, see the headset logs), near the plane, and on the screen.
constexpr float SnapMinW = 0.75f;

static void snapNear2D(rend_context& ctx, const std::vector<PolyParam>& list, u32 from, u32 to, float hudW,
		int fbWidth, int fbHeight)
{
	const GameProfile *prof = activeProfile;
	const float SnapMaxW = prof != nullptr ? prof->snapMaxW : 1.05f;
	const float step = prof != nullptr ? prof->snapStep : 0.01f;
	for (u32 n = from; n < to && n < list.size(); n++)
	{
		const PolyParam& pp = list[n];
		if (pp.count < 3 || pp.first + pp.count > ctx.verts.size())
			continue;
		const float z = ctx.verts[pp.first].z;
		if (!(z > 0.f) || !std::isfinite(z))
			continue;
		const float w = 1.f / z;
		if (w < SnapMinW || w > SnapMaxW || w == hudW || std::abs(w / step - std::round(w / step)) > 0.02f)
			continue;
		u32 i = pp.first;
		for (; i < pp.first + pp.count; i++)
		{
			const Vertex& v = ctx.verts[i];
			if (std::abs(v.z - z) > z * 1e-5f || !(v.x >= -1.f && v.x <= fbWidth + 1.f && v.y >= -1.f && v.y <= fbHeight + 1.f))
				break;
		}
		if (i != pp.first + pp.count)
			continue;	// not flat, or not on the screen: 3D
		// one linear scale per side over the whole band, inside the plane's +-0.002
		const float t = w < hudW ? (w - hudW) / (hudW - SnapMinW) : (w - hudW) / (SnapMaxW - hudW);
		const float snapped = hudW + 0.0015f * std::clamp(t, -1.f, 1.f);
		for (u32 j = pp.first; j < pp.first + pp.count; j++)
			ctx.verts[j].z = 1.f / snapped;
		// what gets snapped, once per W (in 0.01 steps), to check nothing 3D is caught
		static u32 seen[202];
		const int bucket = (int)std::lround(w * 100.f);
		if (bucket >= 0 && bucket < (int)std::size(seen) && seen[bucket]++ == 0)
			NOTICE_LOG(RENDERER, "VR: 2D layer at W %.3f (%u verts at %.0f,%.0f) put on the 2D plane",
					w, pp.count, ctx.verts[pp.first].x, ctx.verts[pp.first].y);
	}
}

// A screen without any 3D (a menu the game draws in layers of depth: W 2..10 in The Maze of
// the Kings) when the profile says so (flatScreens): all of it on the 2D plane, its layers
// kept in their order. True when it was one.
static bool snapFlatScreen(rend_context& ctx, const RenderPass& pass, const RenderPass& prev, float hudW)
{
	if (activeProfile == nullptr || !activeProfile->flatScreens)
		return false;
	bool flat = true;
	float wMax = 0.f;
	u32 polys = 0;
	auto each = [&](auto&& fn) {
		auto list = [&](const std::vector<PolyParam>& polys, u32 from, u32 to) {
			for (u32 n = from; n < to && n < polys.size(); n++)
			{
				const PolyParam& pp = polys[n];
				if (pp.count >= 3 && pp.first + pp.count <= ctx.verts.size())
					fn(pp);
			}
		};
		list(ctx.global_param_op, std::max(prev.op_count, 1u), pass.op_count);
		list(ctx.global_param_pt, prev.pt_count, pass.pt_count);
		list(ctx.global_param_tr, prev.tr_count, pass.tr_count);
	};
	each([&](const PolyParam& pp) {
		const float z = ctx.verts[pp.first].z;
		if (!(z > 0.f) || !std::isfinite(z))
			return;
		for (u32 i = pp.first; i < pp.first + pp.count; i++)
			if (std::abs(ctx.verts[i].z - z) > z * 1e-5f)
				flat = false;
		wMax = std::max(wMax, 1.f / z);
		polys++;
	});
	if (!flat || polys == 0 || wMax <= hudW + 0.01f)
		return false;
	each([&](const PolyParam& pp) {
		const float z = ctx.verts[pp.first].z;
		if (!(z > 0.f) || !std::isfinite(z))
			return;
		const float t = std::clamp((1.f / z - hudW) / (wMax - hudW), -1.f, 1.f);
		const float snapped = 1.f / (hudW + 0.0015f * t);
		for (u32 i = pp.first; i < pp.first + pp.count; i++)
			ctx.verts[i].z = snapped;
	});
	static u32 logged;
	if (logged++ % 600 == 0)
		NOTICE_LOG(RENDERER, "VR: a screen without 3D (%u polygons, W up to %.1f) put on the 2D plane", polys, wMax);
	return true;
}

// The cinematic letterbox: opaque black polygons on the 2D plane. Whole polygons only, so
// one with just some black corners (a text box, a fade) is never torn apart, as the old
// per-vertex hiding in the vertex shader did.
static void hideLetterbox(rend_context& ctx, const std::vector<PolyParam>& list, u32 from, u32 to, float hudW)
{
	if (activeProfile != nullptr && activeProfile->letterboxW > 0.f)
		hudW = activeProfile->letterboxW;
	for (u32 n = from; n < to && n < list.size(); n++)
	{
		const PolyParam& pp = list[n];
		if (pp.count < 3 || pp.first + pp.count > ctx.verts.size())
			continue;
		u32 i = pp.first;
		while (i < pp.first + pp.count && onOverlay(ctx.verts[i], hudW) && opaqueBlack(ctx.verts[i]))
			i++;
		if (i == pp.first + pp.count)
			hideVertices(ctx, pp);
	}
}

// Player 2's "PRESS START BUTTON" with "CREDIT(S)" and its number under it, in the bottom
// right corner all through a one-player game: in the headset it floats in the room for
// nothing (asked for on Reddit, twice). Its pictures go when the game profile knows them
// (vr.HideP2Prompt), and so does what's on the line right after them (the number). Player
// 1's own, bottom left, stays, and so do the story's subtitles.
static std::atomic<int64_t> p2PromptAt { -1000000 };

static void hidePlayer2Prompt(rend_context& ctx, const RenderPass& pass, const RenderPass& prev, float hudW,
		int fbWidth, int fbHeight)
{
	if (p2PromptTextures[0] == 0)
		return;
	const bool hide = config::VrHideP2Prompt;
	struct Box { float x0, y0, x1, y1; };
	auto flat = [&](const PolyParam& pp, Box& b) {
		if (pp.count < 3 || pp.first + pp.count > ctx.verts.size())
			return false;
		b = { 1e9f, 1e9f, -1e9f, -1e9f };
		for (u32 i = pp.first; i < pp.first + pp.count; i++)
		{
			const Vertex& v = ctx.verts[i];
			if (!(v.z > 0.f) || std::abs(1.f / v.z - hudW) > 0.05f || !std::isfinite(v.x) || !std::isfinite(v.y))
				return false;
			b = { std::min(b.x0, v.x), std::min(b.y0, v.y), std::max(b.x1, v.x), std::max(b.y1, v.y) };
		}
		return true;
	};
	auto each = [&](auto&& fn) {
		auto list = [&](const std::vector<PolyParam>& polys, u32 from, u32 to) {
			for (u32 n = from; n < to && n < polys.size(); n++)
				fn(polys[n]);
		};
		list(ctx.global_param_op, std::max(prev.op_count, 1u), pass.op_count);
		list(ctx.global_param_pt, prev.pt_count, pass.pt_count);
		list(ctx.global_param_tr, prev.tr_count, pass.tr_count);
	};
	Box gone[4];
	int count = 0;
	each([&](const PolyParam& pp) {
		Box b;
		const u32 tex = pp.pcw.Texture ? pp.tcw.TexAddr << 3 : 0;
		const ScreenBox& at = activeProfile != nullptr ? activeProfile->p2PromptBox : ScreenBox { 352.f, 384.f, 640.f, 480.f };
		const float sx = fbWidth / 640.f, sy = fbHeight / 480.f;
		if (tex == 0 || std::find(std::begin(p2PromptTextures), std::end(p2PromptTextures), tex) == std::end(p2PromptTextures)
				|| !flat(pp, b) || b.x0 < at.x0 * sx || b.y0 < at.y0 * sy || b.x1 > at.x1 * sx || b.y1 > at.y1 * sy)
			return;
		if (hide)
			hideVertices(ctx, pp);
		if (count < (int)std::size(gone))
			gone[count++] = b;
	});
	if (count == 0)
		return;
	// (seen: player 2 isn't playing, see xr_host.cpp's dual wielding)
	p2PromptAt = steadyMs();
	if (!hide)
		return;
	each([&](const PolyParam& pp) {
		Box b;
		if (!flat(pp, b))
			return;
		for (int k = 0; k < count; k++)
			if (b.x0 >= gone[k].x0 && b.x1 <= gone[k].x1 + fbWidth * 0.12f && b.y0 >= gone[k].y0 - 4.f && b.y1 <= gone[k].y1 + 4.f)
			{
				hideVertices(ctx, pp);
				break;
			}
	});
}

bool player2Prompting() {
	return steadyMs() - p2PromptAt < 500;
}

bool player2PromptKnown() {
	return p2PromptTextures[0] != 0;
}

// A 3D polygon with some of its corners at the 2D plane's W (a game whose near plane is
// there) would have just those corners pinned onto the 2D plane by the headset's vertex
// shader, stretched across the scene. They're moved a hair off it along their rays (0.25%)
// instead, and stay 3D.
static void keepMixedIn3D(rend_context& ctx, const std::vector<PolyParam>& list, u32 from, u32 to, float hudW)
{
	for (u32 n = from; n < to && n < list.size(); n++)
	{
		const PolyParam& pp = list[n];
		if (pp.count < 3 || pp.first + pp.count > ctx.verts.size())
			continue;
		u32 on = 0;
		for (u32 i = pp.first; i < pp.first + pp.count; i++)
			on += onOverlay(ctx.verts[i], hudW);
		if (on == 0 || on == pp.count)
			continue;
		for (u32 i = pp.first; i < pp.first + pp.count; i++)
		{
			Vertex& v = ctx.verts[i];
			if (onOverlay(v, hudW))
				v.z = 1.f / (1.f / v.z < hudW ? hudW - 0.0025f : hudW + 0.0025f);
		}
		static u32 logged;
		if (logged++ % 600 == 0)
			NOTICE_LOG(RENDERER, "VR: kept a 3D polygon with %u of %u corners at the 2D plane's W in 3D", on, pp.count);
	}
}

// PC: overlay-like polygons the headset's vertex shader could bend (mixed overlay and 3D
// W, or 3D right next to the overlay W) or that only partly are letterbox black.
static void logOverlayCensus(const rend_context& ctx, const RenderPass& pass, const RenderPass& prev, float hudW)
{
	int lines = 0;
	auto each = [&](const std::vector<PolyParam>& list, u32 from, u32 to) {
		for (u32 n = from; n < to && n < list.size() && lines < 20; n++)
		{
			const PolyParam& pp = list[n];
			if (pp.count < 3 || pp.first + pp.count > ctx.verts.size())
				continue;
			u32 hud = 0, nearW = 0, black = 0;
			float wMin = 1e30f, wMax = 0.f;
			for (u32 i = pp.first; i < pp.first + pp.count; i++)
			{
				const Vertex& v = ctx.verts[i];
				const float w = v.z > 0.f ? 1.f / v.z : 0.f;
				wMin = std::min(wMin, w);
				wMax = std::max(wMax, w);
				const bool overlay = onOverlay(v, hudW);
				hud += overlay;
				nearW += !overlay && w >= 0.5f && w < 4.f;
				black += opaqueBlack(v);
			}
			const char *what = hud > 0 && hud < pp.count ? "mixed 2D/3D W"
					: nearW == pp.count ? "3D right next to the 2D W"
					: hud == pp.count && black > 0 && black < pp.count ? "2D with some black corners" : nullptr;
			if (what == nullptr)
				continue;
			lines++;
			NOTICE_LOG(RENDERER, "VR census: %s, at %.0f,%.0f, %u verts, W %.3f..%.3f, black %u",
					what, ctx.verts[pp.first].x, ctx.verts[pp.first].y, pp.count, wMin, wMax, black);
		}
	};
	each(ctx.global_param_op, std::max(prev.op_count, 1u), pass.op_count);
	each(ctx.global_param_pt, prev.pt_count, pass.pt_count);
	each(ctx.global_param_tr, prev.tr_count, pass.tr_count);
}

// PC diagnostics: textures that show up within half a second of a shot and weren't drawn
// in the second before it - the game's hit effects (sparks, blood, its hit marker) - once
// per texture per shot, with where and how they're drawn.
static void logShotTextures(const rend_context& ctx, const RenderPass& pass, const RenderPass& prev, int fbWidth, int fbHeight)
{
	static std::unordered_map<u32, u32> lastSeen;	// texture -> parse number
	static std::unordered_set<u32> before, reported;
	static u32 parse;
	static int64_t snapFor = std::numeric_limits<int64_t>::min();
	parse++;
	// the newest shot
	int64_t at = std::numeric_limits<int64_t>::min();
	glm::vec2 gun(0.f);
	for (GunShot& s : gunShots)
	{
		const int64_t ms = s.ms;
		const u32 packed = s.pos;
		if (s.ms == ms && ms > at)
		{
			at = ms;
			gun = glm::vec2((packed & 0xffff) * fbWidth / 640.f, (packed >> 16) * fbHeight / 480.f);
		}
	}
	const int64_t since = steadyMs() - at;
	const bool window = since >= 0 && since <= 500;
	if (window && snapFor != at)
	{
		snapFor = at;
		before.clear();
		reported.clear();
		for (const auto& [tex, seen] : lastSeen)
			if (parse - seen <= 60)
				before.insert(tex);
	}
	auto each = [&](const std::vector<PolyParam>& list, u32 from, u32 to, const char *name) {
		for (u32 n = from; n < to && n < list.size(); n++)
		{
			const PolyParam& pp = list[n];
			const u32 tex = pp.pcw.Texture ? pp.tcw.TexAddr << 3 : 0xffffffffu;
			if (window && !before.count(tex) && !reported.count(tex) && pp.count > 0 && pp.first + pp.count <= ctx.verts.size())
			{
				reported.insert(tex);
				glm::vec2 lo(1e30f), hi(-1e30f);
				float wMin = 1e30f, wMax = 0.f;
				for (u32 i = pp.first; i < pp.first + pp.count; i++)
				{
					const Vertex& v = ctx.verts[i];
					if (!std::isfinite(v.x) || !std::isfinite(v.y))
						continue;
					lo = glm::min(lo, glm::vec2(v.x, v.y));
					hi = glm::max(hi, glm::vec2(v.x, v.y));
					const float w = v.z > 0.f ? 1.f / v.z : 0.f;
					wMin = std::min(wMin, w);
					wMax = std::max(wMax, w);
				}
				const u8 *c = ctx.verts[pp.first].col;
				NOTICE_LOG(RENDERER, "VR new after shot +%d ms gun %.0f,%.0f: %s %u tex %06x tsp %08x isp %08x tcw %08x, %u verts at %.0f,%.0f..%.0f,%.0f (centre %.0f px from the gun) W %.2f..%.2f col %02x%02x%02x%02x",
						(int)since, gun.x, gun.y, name, n, tex, pp.tsp.full, pp.isp.full, pp.tcw.full, pp.count, lo.x, lo.y, hi.x, hi.y,
						glm::distance((lo + hi) * 0.5f, gun), wMin, wMax, c[0], c[1], c[2], c[3]);
			}
			lastSeen[tex] = parse;
		}
	};
	each(ctx.global_param_op, std::max(prev.op_count, 1u), pass.op_count, "OP");
	each(ctx.global_param_pt, prev.pt_count, pass.pt_count, "PT");
	each(ctx.global_param_tr, prev.tr_count, pass.tr_count, "TR");
}

//
// PC: "rip.request" in the working directory dumps the next render pass as the TA got it,
// for hotd2-vr/assets/rip_hands.py to lift into 3D models (models from your own copy of the
// game, never shipped): rip/passNNNN.bin with every polygon and vertex, plus the VRAM and
// palette when they changed. The request file may hold "count interval ram texture polys":
// that many passes, that many passes apart, with ram 1 also the main RAM; with a texture
// (VRAM address, hex) only from the first pass with at least that many polygons using it
// (hotd2-vr/rip-hands.ps1 waits for the agent's pistol that way).
//
static void ripRequested(const rend_context& ctx, const RenderPass& pass, const RenderPass& prev)
{
	static u32 left, every, wait, serial, withRam, waitTexture, waitPolys;
	static u64 vramHash, ramHash;
	if (left == 0)
	{
		static u32 polls;
		if (polls++ % 15 != 0)
			return;
		FILE *req = fopen("rip.request", "r");
		if (req == nullptr)
			return;
		left = 1;
		every = 1;
		withRam = 0;
		waitTexture = 0;
		waitPolys = 1;
		if (fscanf(req, "%u %u %u %x %u", &left, &every, &withRam, &waitTexture, &waitPolys) < 1)
			left = 1;
		fclose(req);
		remove("rip.request");
		left = std::clamp(left, 1u, 1000u);
		every = std::max(every, 1u);
		wait = 0;
		make_directory("rip");
		if (waitTexture != 0)
			NOTICE_LOG(RENDERER, "VR: rip waiting for texture %06x", waitTexture);
	}
	if (waitTexture != 0)
	{
		u32 found = 0;
		auto count = [&](const std::vector<PolyParam>& polys, u32 from, u32 to) {
			for (u32 k = from; k < to && k < polys.size(); k++)
				found += polys[k].pcw.Texture && (polys[k].tcw.TexAddr << 3) == waitTexture;
		};
		count(ctx.global_param_op, std::max(prev.op_count, 1u), pass.op_count);
		count(ctx.global_param_pt, prev.pt_count, pass.pt_count);
		count(ctx.global_param_tr, prev.tr_count, pass.tr_count);
		if (found < waitPolys)
			return;
		waitTexture = 0;
	}
	if (wait > 0)
	{
		wait--;
		return;
	}
	left--;
	wait = every - 1;
	const u32 n = serial++;
	auto hash = [](RamRegion& mem, size_t size) {
		u64 h = 1469598103934665603ull;
		for (size_t i = 0; i < size; i += 8)
		{
			u64 w;
			memcpy(&w, &mem[i], 8);
			h = (h ^ w) * 1099511628211ull;
		}
		return h;
	};
	auto dump = [](RamRegion& mem, size_t size, const char *path) {
		if (FILE *f = fopen(path, "wb"))
		{
			fwrite(&mem[0], 1, size, f);
			fclose(f);
		}
	};
	char path[64];
	const u64 vh = hash(vram, VRAM_SIZE);
	if (vh != vramHash)
	{
		vramHash = vh;
		snprintf(path, sizeof(path), "rip/vram%04u.bin", n);
		dump(vram, VRAM_SIZE, path);
		snprintf(path, sizeof(path), "rip/pal%04u.bin", n);
		if (FILE *f = fopen(path, "wb"))
		{
			const u32 ctrl = PAL_RAM_CTRL;
			fwrite(&ctrl, 4, 1, f);
			fwrite(PALETTE_RAM, 4, 1024, f);
			fclose(f);
		}
	}
	const u64 rh = withRam ? hash(mem_b, RAM_SIZE) : ramHash;
	if (rh != ramHash)
	{
		ramHash = rh;
		snprintf(path, sizeof(path), "rip/ram%04u.bin", n);
		dump(mem_b, RAM_SIZE, path);
	}
	snprintf(path, sizeof(path), "rip/pass%04u.bin", n);
	FILE *f = fopen(path, "wb");
	if (f == nullptr)
		return;
	int fbWidth, fbHeight;
	getPvrFramebufferSize(ctx, fbWidth, fbHeight);
	const glm::vec2 focal = readGameFocal();
	struct { char magic[4]; u32 fbWidth, fbHeight; float focalX, focalY, hudW, stockFocal; u32 lists; } head = {
		{ 'R', 'I', 'P', '1' }, (u32)fbWidth, (u32)fbHeight, focal.x, focal.y, (float)config::VrHudW, (float)config::VrFocal, 3 };
	fwrite(&head, sizeof(head), 1, f);
	auto list = [&](const std::vector<PolyParam>& polys, u32 from, u32 to) {
		to = std::min<u32>(to, (u32)polys.size());
		const u32 count = to > from ? to - from : 0;
		fwrite(&count, 4, 1, f);
		for (u32 k = from; k < to; k++)
		{
			const PolyParam& pp = polys[k];
			const u32 nverts = pp.first + pp.count <= ctx.verts.size() ? pp.count : 0;
			const u32 words[5] = { pp.isp.full, pp.tsp.full, pp.tcw.full, pp.pcw.full, nverts };
			fwrite(words, 4, 5, f);
			for (u32 i = pp.first; i < pp.first + nverts; i++)
			{
				const Vertex& v = ctx.verts[i];
				const float xyzuv[5] = { v.x, v.y, v.z, v.u, v.v };
				fwrite(xyzuv, 4, 5, f);
				fwrite(v.col, 1, 4, f);
				fwrite(v.spc, 1, 4, f);
			}
		}
	};
	list(ctx.global_param_op, std::max(prev.op_count, 1u), pass.op_count);
	list(ctx.global_param_pt, prev.pt_count, pass.pt_count);
	list(ctx.global_param_tr, prev.tr_count, pass.tr_count);
	fclose(f);
	NOTICE_LOG(RENDERER, "VR: ripped pass %u (%u to go)", n, left);
}

void dropShotMarker(rend_context& ctx, const RenderPass& pass, const RenderPass& previousPass)
{
	// (Naomi 2 polygons are still in model space here, and aren't reprojected anyway)
	if (!config::VrReproject || ctx.isRTT || settings.platform.isNaomi2())
		return;
	const bool onPC = !xr::enabled();
	if (onPC)
		ripRequested(ctx, pass, previousPass);
	hands::observePass(ctx, pass, previousPass);
	const float hudW = config::VrHudW;
	static u32 parses;
	if (onPC && parses++ % 240 == 0)
		logOverlayCensus(ctx, pass, previousPass, hudW);
	int fbWidth, fbHeight;
	getPvrFramebufferSize(ctx, fbWidth, fbHeight);
	// The letterbox first: only what the game drew at the plane's W, as the shader used to
	// (a black text shadow on a snapped layer must stay). The first opaque polygon is the
	// background plane.
	if (config::VrHideLetterbox)
	{
		hideLetterbox(ctx, ctx.global_param_op, std::max(previousPass.op_count, 1u), pass.op_count, hudW);
		hideLetterbox(ctx, ctx.global_param_pt, previousPass.pt_count, pass.pt_count, hudW);
		hideLetterbox(ctx, ctx.global_param_tr, previousPass.tr_count, pass.tr_count, hudW);
	}
	if (!snapFlatScreen(ctx, pass, previousPass, hudW))
	{
		snapNear2D(ctx, ctx.global_param_op, std::max(previousPass.op_count, 1u), pass.op_count, hudW, fbWidth, fbHeight);
		snapNear2D(ctx, ctx.global_param_pt, previousPass.pt_count, pass.pt_count, hudW, fbWidth, fbHeight);
		snapNear2D(ctx, ctx.global_param_tr, previousPass.tr_count, pass.tr_count, hudW, fbWidth, fbHeight);
	}
	hidePlayer2Prompt(ctx, pass, previousPass, hudW, fbWidth, fbHeight);
	keepMixedIn3D(ctx, ctx.global_param_op, std::max(previousPass.op_count, 1u), pass.op_count, hudW);
	keepMixedIn3D(ctx, ctx.global_param_pt, previousPass.pt_count, pass.pt_count, hudW);
	keepMixedIn3D(ctx, ctx.global_param_tr, previousPass.tr_count, pass.tr_count, hudW);

	// the recent shots, in DC framebuffer pixels
	struct Recent { glm::vec2 gun; int64_t since, at; bool into3D; };
	Recent recent[std::size(gunShots)];
	u32 count = 0;
	const int64_t now = steadyMs();
	for (GunShot& s : gunShots)
	{
		const int64_t at = s.ms;
		const u32 packed = s.pos;
		const bool into3D = s.into3D;
		if (s.ms != at || now - at < 0 || now - at > MarkerMs)
			continue;
		recent[count++] = { glm::vec2((packed & 0xffff) * fbWidth / 640.f, (packed >> 16) * fbHeight / 480.f),
				now - at, at, into3D };
	}
	if (onPC)
		logShotTextures(ctx, pass, previousPass, fbWidth, fbHeight);
	if (count == 0)
		return;
	const bool dropMarkers = config::VrDropShotMarker;
	const bool dropFlash = config::VrDropShotFlash;
	// Diagnostics on the headset too, for the first few shots: what the game draws right
	// after a shot (to pin down its marker / flash). The PC logs every shot.
	static u32 diagShots;
	static int64_t diagFor;
	const Recent *latest = &recent[0];
	for (u32 k = 1; k < count; k++)
		if (recent[k].at > latest->at)
			latest = &recent[k];
	if (latest->at != diagFor && latest->since <= 200)
	{
		diagFor = latest->at;
		diagShots++;
	}
	const bool diag = onPC || (diagShots <= 8 && latest->at == diagFor && latest->since <= 200);
	if (diag && pass.op_count > 0 && !ctx.global_param_op.empty() && ctx.global_param_op[0].first < ctx.verts.size())
	{
		const u8 *bg = ctx.verts[ctx.global_param_op[0].first].col;
		NOTICE_LOG(RENDERER, "VR diag: shot +%d ms gun %.0f,%.0f%s, fb %dx%d, polygons op %u pt %u tr %u, background %02x%02x%02x%02x",
				(int)latest->since, latest->gun.x, latest->gun.y, latest->into3D ? "" : " (2D)", fbWidth, fbHeight,
				pass.op_count, pass.pt_count, pass.tr_count, bg[0], bg[1], bg[2], bg[3]);
	}
	if (!dropMarkers && !dropFlash && !diag)
		return;
	u32 dropped = 0;
	const Recent *newest = nullptr;
	auto scan = [&](const std::vector<PolyParam>& list, u32 from, u32 to, const char *name) {
		for (u32 n = from; n < to && n < list.size(); n++)
		{
			const PolyParam& pp = list[n];
			if (pp.count < 3 || pp.first + pp.count > ctx.verts.size())
				continue;
			glm::vec2 lo(1e30f), hi(-1e30f);
			float wMin = 1e30f, wMax = 0.f;
			bool all2D = true;
			for (u32 i = pp.first; i < pp.first + pp.count; i++)
			{
				const Vertex& v = ctx.verts[i];
				if (!std::isfinite(v.x) || !std::isfinite(v.y))
				{
					all2D = false;	// hidden already, or the game's own invalid vertex
					continue;
				}
				lo = glm::min(lo, glm::vec2(v.x, v.y));
				hi = glm::max(hi, glm::vec2(v.x, v.y));
				const float w = v.z > 0.f ? 1.f / v.z : 0.f;
				wMin = std::min(wMin, w);
				wMax = std::max(wMax, w);
				all2D = all2D && onOverlay(v, hudW);
			}
			if (lo.x > hi.x)
				continue;
			const glm::vec2 size = hi - lo;
			// The hit marker: one of the profile's marker textures, or (any game) a small
			// polygon by the 2D plane that isn't flat on it - the game's flash quads come at
			// W 0.9..1.01, while its HUD and text sit at one W.
			const u32 tex = pp.pcw.Texture ? pp.tcw.TexAddr << 3 : 0u;
			const bool markerTex = tex != 0 && std::find(std::begin(markerTextures), std::end(markerTextures), tex) != std::end(markerTextures);
			const bool nearPlane = (wMin >= 0.85f && wMax <= 1.15f)
					|| (activeProfile != nullptr && wMin >= activeProfile->markerMinW && wMax <= activeProfile->markerMaxW);
			const bool markerLike = nearPlane && (markerTex || wMax - wMin > 0.004f)
					&& size.x <= MarkerMaxSize && size.y <= MarkerMaxSize;
			const glm::vec2 centre = (lo + hi) * 0.5f;
			const bool fullScreen = size.x >= fbWidth * 0.9f && size.y >= fbHeight * 0.9f;
			const u8 *c = ctx.verts[pp.first].col;
			const bool bright = c[0] >= 0xc0 && c[1] >= 0xc0 && c[2] >= 0xc0;
			// the nearest recent shot, for the log
			const Recent *near = &recent[0];
			for (u32 k = 1; k < count; k++)
				if (glm::distance(centre, recent[k].gun) < glm::distance(centre, near->gun))
					near = &recent[k];
			const float offset = glm::distance(centre, near->gun);
			bool hide = false;
			for (u32 k = 0; k < count; k++)
			{
				const Recent& r = recent[k];
				hide = hide || (markerLike && dropMarkers && r.into3D && glm::distance(centre, r.gun) <= MarkerMaxOffset);
				hide = hide || (all2D && dropFlash && fullScreen && bright && r.since <= FlashMs);
			}
			const bool big = size.x >= fbWidth * 0.5f && size.y >= fbHeight * 0.5f && size.x < fbWidth * 4.f && size.y < fbHeight * 4.f;
			if (diag && (((all2D || markerLike) && offset <= MarkerMaxOffset) || (big && (all2D || bright))))
				NOTICE_LOG(RENDERER, "VR: shot +%d ms gun %.0f,%.0f%s: %s %u (%u verts) %.0fx%.0f, %.0f px off, W %.3f..%.3f%s, tex %06x tsp %08x col %02x%02x%02x%02x%s",
						(int)near->since, near->gun.x, near->gun.y, near->into3D ? "" : " (2D)", name, n, pp.count,
						size.x, size.y, offset, wMin, wMax, all2D ? " all-2D" : "",
						(unsigned)(pp.tcw.TexAddr << 3), pp.tsp.full, c[0], c[1], c[2], c[3], hide ? " DROPPED" : "");
			if (!hide)
				continue;
			hideVertices(ctx, pp);
			dropped++;
			if (newest == nullptr || near->at > newest->at)
				newest = near;
		}
	};
	scan(ctx.global_param_op, std::max(previousPass.op_count, 1u), pass.op_count, "OP");
	scan(ctx.global_param_pt, previousPass.pt_count, pass.pt_count, "PT");
	scan(ctx.global_param_tr, previousPass.tr_count, pass.tr_count, "TR");
	static int64_t loggedFor;
	if (dropped > 0 && newest != nullptr && loggedFor != newest->at)
	{
		loggedFor = newest->at;
		NOTICE_LOG(RENDERER, "VR: hid the game's shot marker (%u polygons, %d ms after the shot)", dropped, (int)newest->since);
	}
}

const ReprojectParams& update(const rend_context& ctx)
{
	int dcWidth, dcHeight;
	getPvrFramebufferSize(ctx, dcWidth, dcHeight);
	const glm::vec2 halfSize(dcWidth * 0.5f, dcHeight * 0.5f);
	const bool active = config::VrReproject && !ctx.isRTT;
	params.comfort = glm::vec3(0.f);

	const float zMax = ctx.fZ_max > 0.f && std::isfinite(ctx.fZ_max) ? ctx.fZ_max : 1.f;
	const float zNear = NearFraction / zMax;

	if (!active)
	{
		params.tanHalf = halfSize / (float)config::VrFocal;
		params.overlay = glm::vec4(params.tanHalf, 1.f, -1.f);	// no vertex counts as overlay
		params.viewProj = projection(params.tanHalf, zNear);
		return params;
	}

	// The game may run with a widened view: rebuild with its live focal length, but
	// present with the stock one so the framing and HUD size stay as designed.
	const glm::vec2 gameFocal = readGameFocal();
	const glm::vec2 stockTan = halfSize / (float)config::VrFocal;
	params.tanHalf = halfSize / gameFocal;
	params.overlay = glm::vec4(stockTan, (float)config::VrHudDepth, (float)config::VrHudW);

	// depth statistics now and then, for tuning a new game on the PC (not in the headset)
	if (frameCount++ % 120 == 0 && !xr::enabled())
		logSceneStats(ctx, gameFocal);

	if (const xr::Eye *eye = xr::currentEye())
	{
		// In the headset: game camera at the player's head, viewed through this eye.
		params.viewProj = eye->viewProj;
		params.comfort = comfortParams();
		return params;
	}

	float yaw = config::VrYaw;
	float pitch = config::VrPitch;
	glm::vec3 offset(config::VrOffsetX, config::VrOffsetY, config::VrOffsetZ);
	if (config::VrAnimate)
	{
		// Sway around the game camera so stills from consecutive seconds show parallax.
		const float t = frameCount / 60.f;
		yaw *= std::sin(t * 0.9f);
		pitch *= std::sin(t * 0.6f);
		offset *= glm::vec3(std::sin(t * 0.7f), std::cos(t * 0.5f), std::sin(t * 0.4f));
	}
	glm::mat4 pose = glm::translate(glm::mat4(1.f), offset)
			* glm::rotate(glm::mat4(1.f), glm::radians(yaw), glm::vec3(0, 1, 0))
			* glm::rotate(glm::mat4(1.f), glm::radians(pitch), glm::vec3(1, 0, 0));
	// The renderer's ndc y points down the image; flip into y-up space for the pose.
	const glm::mat4 flipY = glm::scale(glm::mat4(1.f), glm::vec3(1, -1, 1));
	params.viewProj = projection(stockTan, zNear) * flipY * glm::inverse(pose) * flipY;

	return params;
}

}
