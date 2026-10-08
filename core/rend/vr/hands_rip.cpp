/*
	The agent's hands and pistol, or the hero's staff, from the player's own game (hotd2-vr).
	See hands_rip.h.

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "hands_rip.h"
#include "hands_build.h"
#include "xr_host.h"
#include "rend/vr_reproject.h"
#include "rend/transform_matrix.h"
#include "hw/pvr/ta_ctx.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/sh4/sh4_sched.h"
#include "input/udp_lightgun.h"
#include "cfg/option.h"
#include "emulator.h"
#include "stdclass.h"
#include "log/Log.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <mutex>

namespace vr::hands
{
namespace
{

// the pistol in the game over scene: this many of its polygons and it's all there
constexpr u32 MinGunPolys = 20;

// The run to HOTD2's game over scene. Start gets it through the notices at boot ("PRESS START
// BUTTON", with saved data or without, which then makes it), the intro, the title and its
// menu (Arcade mode comes first). It is only pressed while one of those is on: a screen
// drawn every frame with little 3D in it (the title draws ~450 polygons a frame, the menu
// 86, the notices none), for the story and the game draw 900 and more, and Start would
// pause them. From the story on, nothing: the agent loses, and kneels with his pistol at
// about 115 s of game (measured on the PC, both with saved data and without).
constexpr u32 Story3D = 600;		// 3D polygons a frame
constexpr double PressEvery = 2.0, PressHold = 0.2;
constexpr int Window = 30;			// vblanks looked back over (half a second)
constexpr double TypicalRun = 115.0;
constexpr double GiveUpAfter = 330.0;

// The run for the hero's staff (The Maze of the Kings): its attract demo, from boot, with no
// input at all. Its logos end at about 31 s of game; from 32 s both heroes walk towards the
// camera, staff in hand (the first frame of that is enough), at 53 s they raise them, and the
// demo comes round again every two and a half to three minutes (measured on the PC).
constexpr double StaffRun = 33.0;
constexpr double StaffGiveUpAfter = 150.0;

enum State { Off, Watching, Preparing };
std::atomic<int> state { Off };
std::atomic<bool> newModel, skipRequested;
std::atomic<float> progress;
std::atomic<bool> staffRun;	// the run is for the hero's staff
double startedAt;			// game seconds at the start
bool fastForwardWas;
// why the last try at the staff didn't make it (for when the run gives up)
std::mutex failureMutex;
std::string lastFailure;
// what the passes showed since the last vblank
std::atomic<u32> seen3D, seenPasses;
// ...and over the last vblanks
u32 recent3D[Window], recentPasses[Window];
int recentAt;
bool storyBegun;
double lastPress;
u32 passes, failedAt;

constexpr int OffScreen = -10000;

double gameSeconds() {
	return sh4_sched_now64() / (double)SH4_MAIN_CLOCK;
}

bool haveModel() {
	return file_exists(modelPath()) || (!legacyModelPath().empty() && file_exists(legacyModelPath()));
}

void stopPrep(const char *why)
{
	if (state != Preparing)
		return;
	state = Watching;
	settings.input.fastForwardMode = fastForwardWas;
	lightgunSet(0, OffScreen, OffScreen, 0);
	NOTICE_LOG(RENDERER, "VR hands: %s after %.0f s of game", why, gameSeconds() - startedAt);
}

void onStart()
{
	state = Off;
	skipRequested = false;
	progress = 0.f;
	// a new game: whatever model the renderer has is another game's
	newModel = true;
	const Source *source = gameModelSource();
	if (source == nullptr || !config::VrGameHands)
		return;
	if (haveModel())
	{
		NOTICE_LOG(RENDERER, "VR hands: %s, made before: %s", source->name(),
				(file_exists(modelPath()) ? modelPath() : legacyModelPath()).c_str());
		return;
	}
	state = Watching;
	passes = failedAt = 0;
	staffRun = source->kind == Source::HerosStaff;
	{
		std::lock_guard<std::mutex> lock(failureMutex);
		lastFailure.clear();
	}
	// The run: in the headset, or on the PC when asked (vr.HandsPrep, for testing)
	if (!xr::enabled() && !config::VrHandsPrep)
		return;
	std::fill(std::begin(recent3D), std::end(recent3D), 0u);
	std::fill(std::begin(recentPasses), std::end(recentPasses), 0u);
	recentAt = 0;
	seen3D = seenPasses = 0;
	storyBegun = false;
	lastPress = -1e9;
	startedAt = gameSeconds();
	fastForwardWas = settings.input.fastForwardMode;
	settings.input.fastForwardMode = true;
	state = Preparing;
	if (staffRun)
		NOTICE_LOG(RENDERER, "VR hands: no model yet, running the attract demo for the hero's staff (%s)", modelPath().c_str());
	else
		NOTICE_LOG(RENDERER, "VR hands: no model yet, running the game to its game over scene for it");
}

void onVBlank()
{
	if (state != Preparing)
		return;
	if (skipRequested)
	{
		stopPrep("skipped");
		return;
	}
	const double t = gameSeconds() - startedAt;
	if (staffRun)
	{
		// Hands off: the attract demo plays by itself.
		progress = (float)std::min(t / StaffRun, 0.97);
		if (t > StaffGiveUpAfter)
		{
			std::string why;
			{
				std::lock_guard<std::mutex> lock(failureMutex);
				why = lastFailure.empty() ? "the hero's staff never came up whole (gave up)"
						: "no whole staff (gave up; last time " + lastFailure + ")";
			}
			stopPrep(why.c_str());
			return;
		}
		lightgunSet(0, OffScreen, OffScreen, 0);
		return;
	}
	progress = (float)std::min(t / TypicalRun, 0.97);
	if (t > GiveUpAfter)
	{
		stopPrep("no game over scene (gave up)");
		return;
	}
	recent3D[recentAt] = seen3D.exchange(0);
	recentPasses[recentAt] = seenPasses.exchange(0);
	recentAt = (recentAt + 1) % Window;
	u32 polys = 0, drawn = 0;
	for (int i = 0; i < Window; i++)
	{
		polys += recent3D[i];
		drawn += recentPasses[i];
	}
	const bool drawing = drawn >= Window * 2 / 3;
	if (!storyBegun && drawing && polys >= Story3D * drawn)
	{
		storyBegun = true;
		NOTICE_LOG(RENDERER, "VR hands: the story has begun at %.0f s, hands off", t);
	}
	bool start = false;
	if (!storyBegun)
	{
		if (t - lastPress < PressHold)
			start = true;
		else if (drawing && t - lastPress >= PressEvery)
		{
			lastPress = t;
			start = true;
			INFO_LOG(RENDERER, "VR hands: Start at %.1f s (%u 3D polygons a frame)", t, polys / std::max(drawn, 1u));
		}
	}
	lightgunSet(0, OffScreen, OffScreen, start ? 4 : 0);
}

void onEvent(Event event, void *)
{
	switch (event)
	{
	case Event::Start:
		onStart();
		break;
	case Event::VBlank:
		onVBlank();
		break;
	case Event::Terminate:
		stopPrep("stopped");
		state = Off;
		break;
	default:
		break;
	}
}

struct Registration
{
	Registration() {
		EventManager::listen(Event::Start, onEvent);
		EventManager::listen(Event::VBlank, onEvent);
		EventManager::listen(Event::Terminate, onEvent);
	}
} registration;

bool save(const std::vector<u8>& data)
{
	const std::string path = modelPath();
	if (path.empty())
		return false;
	const std::string temp = path + ".part";
	FILE *f = nowide::fopen(temp.c_str(), "wb");
	if (f == nullptr)
		return false;
	const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
	if (fclose(f) != 0 || !ok)
	{
		nowide::remove(temp.c_str());
		return false;
	}
	nowide::remove(path.c_str());
	return nowide::rename(temp.c_str(), path.c_str()) == 0;
}

}	// namespace

std::string modelPath()
{
	// each game its own (HOTD2's, either version: hands.bin)
	const Source *source = gameModelSource();
	return source == nullptr ? std::string() : get_writable_data_path(source->file);
}

std::string legacyModelPath()
{
	// the app's internal files directory, from the game path (files/games/...); only ever
	// the agent's hands
	const Source *source = gameModelSource();
	if (source == nullptr || source->kind != Source::AgentsPistol)
		return std::string();
	const std::string& game = settings.content.path;
	const size_t at = game.find("/files/");
	return at == std::string::npos ? std::string() : game.substr(0, at + 6) + "/hands.bin";
}

void observePass(const rend_context& ctx, const RenderPass& pass, const RenderPass& prev)
{
	const Source *source = gameModelSource();
	if (state == Off || source == nullptr)
		return;
	const bool staff = source->kind == Source::HerosStaff;
	passes++;
	auto each = [&](auto&& fn) {
		auto list = [&](const std::vector<PolyParam>& polys, u32 from, u32 to) {
			for (u32 k = from; k < to && k < polys.size(); k++)
				fn(polys[k]);
		};
		list(ctx.global_param_op, std::max(prev.op_count, 1u), pass.op_count);
		list(ctx.global_param_pt, prev.pt_count, pass.pt_count);
		list(ctx.global_param_tr, prev.tr_count, pass.tr_count);
	};
	auto texture = [](const PolyParam& pp) -> u32 {
		return pp.pcw.Texture ? pp.tcw.TexAddr << 3 : 0;
	};
	// how much 3D is in view (for the run), and how much of the pistol (or of the rods)
	const float overlayW = config::VrHudW;
	u32 polys3D = 0, gunPolys = 0;
	each([&](const PolyParam& pp) {
		gunPolys += staff ? source->staff.isRod(texture(pp)) : texture(pp) == source->pistol.gun;
		for (u32 i = pp.first; i < pp.first + pp.count && i < ctx.verts.size(); i++)
		{
			const float z = ctx.verts[i].z;
			if (z > 0.f && std::isfinite(z) && 1.f / z >= 0.5f && std::abs(1.f / z - overlayW) > 0.05f)
			{
				polys3D++;
				break;
			}
		}
	});
	seen3D += polys3D;
	seenPasses++;
	// a failed try (not all of it in view yet) isn't repeated every frame
	if (gunPolys < (staff ? StaffRodPolys : MinGunPolys) || (failedAt != 0 && passes - failedAt < 30))
		return;

	Rip rip;
	int fbWidth, fbHeight;
	getPvrFramebufferSize(ctx, fbWidth, fbHeight);
	rip.fbWidth = fbWidth;
	rip.fbHeight = fbHeight;
	const GameCamera cam = gameCamera(ctx);
	rip.focalX = cam.dcSize.x * 0.5f / cam.tanHalf.x;
	rip.focalY = cam.dcSize.y * 0.5f / cam.tanHalf.y;
	each([&](const PolyParam& pp) {
		if (!(staff ? source->staff.has(texture(pp)) : source->pistol.has(texture(pp))) || pp.first + pp.count > ctx.verts.size())
			return;
		RipPoly& p = rip.polys.emplace_back();
		p.isp = pp.isp.full;
		p.tsp = pp.tsp.full;
		p.tcw = pp.tcw.full;
		p.pcw = pp.pcw.full;
		p.v.reserve(pp.count);
		for (u32 i = pp.first; i < pp.first + pp.count; i++)
		{
			const Vertex& v = ctx.verts[i];
			p.v.push_back({ v.x, v.y, v.z, v.u, v.v, { v.col[0], v.col[1], v.col[2], v.col[3] } });
		}
	});
	std::vector<u8> model;
	std::string error;
	const TextureSource textures = [](u32 tcw, u32 tsp, Image& image) {
		return decodeTexture(&vram[0], VRAM_SIZE, PALETTE_RAM, PAL_RAM_CTRL, tcw, tsp, image);
	};
	const bool built = staff ? buildStaff(rip, source->staff, textures, model, error)
			: build(rip, source->pistol, textures, model, error);
	if (!built)
	{
		failedAt = passes;
		if (staff)
		{
			WARN_LOG(RENDERER, "VR hands: the hero's staff, but %s", error.c_str());
			std::lock_guard<std::mutex> lock(failureMutex);
			lastFailure = error;
		}
		else
			WARN_LOG(RENDERER, "VR hands: the game over scene, but %s", error.c_str());
		return;
	}
	if (!save(model))
	{
		failedAt = passes;
		WARN_LOG(RENDERER, "VR hands: can't write %s", modelPath().c_str());
		return;
	}
	NOTICE_LOG(RENDERER, "VR hands: %s from this game, %zu bytes to %s", source->name(), model.size(), modelPath().c_str());
	progress = 1.f;
	stopPrep("done");
	state = Off;
	newModel = true;
}

bool takeNewModel() {
	return newModel.exchange(false);
}

bool preparing() {
	return state == Preparing;
}

float prepProgress() {
	return progress;
}

void skipPrep() {
	skipRequested = true;
}

}
