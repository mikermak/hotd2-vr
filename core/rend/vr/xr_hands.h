/*
	The agent's own hands and pistol in the headset (hotd2-vr), taken from the player's copy
	of the game: hands.bin in the data directory, made by the app (hands_rip.h) or on the PC
	by hotd2-vr/assets/rip_hands.py from a rip of the game over scene. In The Maze of the
	Kings the hero's staff in his fist, and his other glove, from staff-mok.bin. Each game
	has its own file; nothing of the game is in this source; without the file the arcade gun
	of xr_gun.h is drawn instead.

	Gun space is that of xr_gun.h (barrel along -z, y up, metres) with its origin in the fist
	around the grip. Hand space holds the open left hand: fingers along -z, palm towards +x,
	thumb up, origin just off the palm, where the controller's grip goes. Mirrored in x, both
	serve the other side. (The staff: its head along -z, origin on its axis in the fist; hand
	space is staff space mirrored, with the left glove a fist like the right one.)

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include <glm/glm.hpp>

namespace vr::xr
{

struct HandsModel
{
	glm::vec3 muzzle;		// gun space
	glm::vec3 grab;			// gun space: the back of the slide, where the other hand takes it
	float travel;			// metres the slide goes back (+z); 0: no slide (the staff)
	glm::mat4 onSlide;		// hand space -> gun space: the open hand racking the slide (at rest)
};

// The running game's model, loaded on first use (again once another game starts); nullptr
// without one.
const HandsModel *handsModel();

struct HandsView
{
	glm::mat4 gunPose;		// gun space -> room space
	float slide = 0.f;		// metres the slide is back
	bool otherHand = false;	// draw the open hand...
	glm::mat4 handPose;		// ...hand space -> room space
	glm::vec4 muzzleLight { 0.f };	// room position, strength (the muzzle flash)
};

// Draws the pistol in its hand, and the other hand, into the bound eye framebuffer.
void drawHands(const glm::mat4& viewProj, const glm::vec3& eyePos, const HandsView& view);
// The GL context is going away.
void termHands();

}
