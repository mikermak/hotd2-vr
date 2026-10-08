/*
	Making the agent's hands and pistol (hands.bin) from one render pass of the game itself
	(hotd2-vr): the C++ twin of hotd2-vr/assets/rip_hands.py, so the headset makes them from
	the player's own copy of the game, with no PC and nothing of the game in the app. The
	same for the hero's staff in The Maze of the Kings (staff-mok.bin), in the same format.

	Standalone (glm and the standard library only), so it can be checked against the Python
	tool on the PC with the same rip.

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace vr::hands
{

// The game over scene: the textures (VRAM addresses) of the agent's parts, per version of
// the game (vr_reproject.cpp's profiles). The pistol is in his right hand, the left one is open.
struct Parts
{
	uint32_t hands[2];
	uint32_t cuff;
	uint32_t gun;
	bool has(uint32_t tex) const { return tex != 0 && (tex == hands[0] || tex == hands[1] || tex == cuff || tex == gun); }
};

// The hero's staff in The Maze of the Kings: the textures (VRAM addresses) of the rod and of
// the hero's body, which his gloves are part of, per scene he is seen in (the attract demo,
// the story's intro). He holds the rod in his right fist.
struct StaffParts
{
	struct Scene { uint32_t rod, body; } scenes[2];
	bool has(uint32_t tex) const {
		for (const Scene& s : scenes)
			if (tex != 0 && (tex == s.rod || tex == s.body))
				return true;
		return false;
	}
	bool isRod(uint32_t tex) const { return tex != 0 && (tex == scenes[0].rod || tex == scenes[1].rod); }
};
// A whole rod: its head piece (42 polygons) and its shaft (24)
constexpr uint32_t StaffRodPolys = 66;

// One polygon (a triangle strip) of a render pass, as the TA got it: vertices in framebuffer
// pixels, z = 1/W.
struct RipVertex
{
	float x, y, z, u, v;
	uint8_t col[4];
};
struct RipPoly
{
	uint32_t isp, tsp, tcw, pcw;
	std::vector<RipVertex> v;
	// VRAM address of the texture, 0 for none
	uint32_t texture() const { return (pcw >> 3) & 1 ? (tcw & 0x1FFFFF) << 3 : 0; }
};
struct Rip
{
	uint32_t fbWidth, fbHeight;
	float focalX, focalY;		// the game's focal length, framebuffer pixels
	std::vector<RipPoly> polys;
};
struct Image
{
	uint32_t width = 0, height = 0;
	std::vector<uint8_t> rgba;
};

// The top mip level of a PowerVR texture, from VRAM (linear, as Flycast keeps it) and the
// palette RAM. False for formats not handled (YUV, bump, planar VQ) or out of VRAM.
bool decodeTexture(const uint8_t *vram, size_t vramSize, const uint32_t *palette, uint32_t palCtrl,
		uint32_t tcw, uint32_t tsp, Image& image);

// Where the textures come from: tcw and the tsp's size bits.
using TextureSource = std::function<bool(uint32_t tcw, uint32_t tsp, Image& image)>;

// The pistol, the hand around its grip and the open hand, as the bytes of hands.bin (the
// format is in xr_hands.cpp). False with a reason when the pass doesn't have them.
bool build(const Rip& rip, const Parts& parts, const TextureSource& textures, std::vector<uint8_t>& out, std::string& error);

// The hero's staff, his right fist around it and his left glove as the open hand, in the
// same format: the rod is the frame, there is no slide (it travels 0), the muzzle is the tip
// of its head. Shortened (the default), the shaft behind the fist is cut down to a hand's
// width and its tail piece put back on there: about 1 m in all instead of 1.4. False with a
// reason when the pass doesn't show all of it.
bool buildStaff(const Rip& rip, const StaffParts& parts, const TextureSource& textures, std::vector<uint8_t>& out,
		std::string& error, bool shortened = true);

}
