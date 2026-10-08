/*
	VR reprojection prototype (hotd2-vr).

	The PVR2 receives vertices already projected by the game: screen x, y and z = 1/w.
	With the game's focal length known, each vertex can be lifted back to eye space:
		W = 1/z,  X = ndc.x * W * tanHalf.x,  Y = ndc.y * W * tanHalf.y,  Z = -W
	and re-projected from another camera (a debug free camera now, the HMD eyes later).

	2D overlays (HUD, text, letterbox bars) are drawn by the game at one fixed W (1.0 in
	HOTD2). Lifting them back would put them in your face, so they go on a plane at a
	fixed depth in front of the game camera instead, keeping their original on-screen
	size. Opaque black overlay is the cinematic letterbox and can be hidden.

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include "vr/hands_rip.h"
#include <glm/glm.hpp>

struct rend_context;
struct RenderPass;

namespace vr
{

struct ReprojectParams
{
	// Maps the rebuilt eye-space position (renderer y convention) to clip space.
	glm::mat4 viewProj { 1.f };
	// tan of the game camera's half field of view, per axis, from its live focal length.
	glm::vec2 tanHalf { 1.f, 0.75f };
	// Overlay plane: tan of the half field of view at the original focal length (x, y),
	// plane depth (z), and the W the game draws overlay at (w).
	glm::vec4 overlay { 1.f, 0.75f, 40.f, 1.f };
	// Comfort zone in the headset (see comfortScale): metres per game unit, distance the
	// pull-back starts at, closest distance (metres). All 0 when off.
	glm::vec3 comfort { 0.f };
};

// The game's own field of view is widened (its code patched, see vr_reproject.cpp): it
// then draws and culls with the wide view. (Its light gun hit test still uses the stock
// view: see aimAtScene in vr/xr_host.cpp.)
bool widensGameFov();

// The comfort zone's parameters for the headset (vr.WorldScale, vr.ComfortStart,
// vr.ComfortMin), or zero when it is switched off.
glm::vec3 comfortParams();

// The light gun fired at (x, y), DC pixels (640x480); into3D: into the 3D scene, not at
// the 2D plane (menus, text). From lightgunSet, any thread.
void noteGunShot(int x, int y, bool into3D);
// After a shot into 3D, takes the game's own 2D shot marker out of a pass the TA just
// parsed, before its index is built (ta_vtx.cpp parseRenderPass); vr.DropShotMarker.
// On the PC it also logs what it sees around shots and overlay that could bend, for tuning.
void dropShotMarker(rend_context& ctx, const RenderPass& pass, const RenderPass& previousPass);

// Things that come closer to the game camera than the comfort start, straight ahead (a
// zombie grabbing you), are pulled back along the line of sight, smoothly, to no closer
// than the comfort minimum, so the eyes don't have to cross. Off to the side or below
// (the floor at your feet) nothing moves: the pull fades out between about 32 and 57
// degrees from the view axis. Returns the factor for a position p in rebuilt game eye
// space. The vertex shader (gles.cpp) does the same.
inline float comfortScale(const glm::vec3& p, const glm::vec3& comfort)
{
	const float len = glm::length(p);
	const float d = len * comfort.x;
	if (!(d > 0.f) || d >= comfort.y)
		return 1.f;
	const float t = d / comfort.y;
	const float c = comfort.z / comfort.y;
	const float pull = comfort.y * (t + c * (1.f - t) * (1.f - t)) / d;
	const float a = glm::clamp((-p.z / len - 0.55f) / 0.30f, 0.f, 1.f);
	const float ahead = a * a * (3.f - 2.f * a);	// smoothstep
	return 1.f + (pull - 1.f) * ahead;
}

// The game camera for a frame: tan of its half field of view, the DC framebuffer size
// and the W its 2D overlay is drawn at. Shared with the headset's light-gun raycast.
struct GameCamera
{
	glm::vec2 tanHalf;
	glm::vec2 dcSize;
	float overlayW;
	// tan of the half field of view the game's light-gun hit test uses (its stock view)
	glm::vec2 stockTan;
};
GameCamera gameCamera(const rend_context& ctx);

// What the game being played makes its own model from (hands_rip.h: the agent's hands and
// pistol, the hero's staff), or nullptr when its profile has none.
const hands::Source *gameModelSource();

// The game asks for player 2 in its corner ("PRESS START BUTTON"): player 2 isn't playing.
// Seen in the last half second (when the game's profile knows the picture).
bool player2Prompting();
// Metres per game unit: vr.WorldScale, times the game profile's unit size next to HOTD2's.
float worldScale();
// ...the profile knows that picture (else player 2 can't be told out again).
bool player2PromptKnown();

// Computes this frame's reprojection. Passes that must not move (render-to-texture,
// or the feature switched off) get the identity reprojection, which reproduces the
// original image exactly.
const ReprojectParams& update(const rend_context& ctx);

}
