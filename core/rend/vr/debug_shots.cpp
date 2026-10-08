/*
	A PC test aid (hotd2-vr): with vr.DebugShots = N, what the game shows is written every N
	seconds of game time to the data directory (shot_<seconds>.png), to work out a new game's
	screens and timings without watching it (the window can stay minimized).

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "cfg/option.h"
#include "emulator.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/sh4/sh4_sched.h"
#include "ui/gui.h"
#include "stdclass.h"
#include <stb_image_write.h>

#include <cstdio>
#include <vector>

namespace vr
{
namespace
{

double startedAt;
int lastShot = -1;

double gameSeconds() {
	return sh4_sched_now64() / (double)SH4_MAIN_CLOCK;
}

void onEvent(Event event, void *)
{
	if (event == Event::Start)
	{
		startedAt = gameSeconds();
		lastShot = -1;
		return;
	}
	const int every = config::VrDebugShots;
	if (every <= 0)
		return;
	const int n = (int)((gameSeconds() - startedAt) / every);
	if (n == lastShot)
		return;
	lastShot = n;
	const int seconds = n * every;
	gui_runOnUiThread([seconds]() {
		std::vector<u8> rgb;
		int w = 0, h = 0;
		if (renderer == nullptr || !renderer->GetLastFrame(rgb, w, h) || w <= 0 || h <= 0)
			return;
		char name[32];
		snprintf(name, sizeof(name), "shot_%04d.png", seconds);
		stbi_write_png(get_writable_data_path(name).c_str(), w, h, 3, rgb.data(), w * 3);
	});
}

struct Registration
{
	Registration() {
		EventManager::listen(Event::Start, onEvent);
		EventManager::listen(Event::VBlank, onEvent);
	}
} registration;

}
}
