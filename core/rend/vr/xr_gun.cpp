/*
	The player's pistol in the headset (hotd2-vr). See xr_gun.h.

	The model is a Namco arcade light gun in glossy red plastic: "Namco Arcade Gun" by
	Martoscar (https://sketchfab.com/3d-models/namco-arcade-gun-15fbd5b9add94a34b5c21746e3dd32be),
	CC BY 4.0, baked into gun_model.h by hotd2-vr/gun_model/convert_gun.py. It is lit per pixel
	by a fixed key light and a dim fill from below, with a plastic highlight and rim, so it
	reads as a solid object in the game's dark scenes.

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "xr_gun.h"
#include "xr_hands.h"
#include "gun_model.h"
#include "rend/gles/gles.h"
#include "rend/gles/glcache.h"
#include "cfg/option.h"
#include "log/Log.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

namespace vr::xr
{
namespace
{

void logGlErrors(const char *pass)
{
	// (hotd2-vr: the first few seconds only, at most 10 lines: checked after every draw it
	// costs a GPU round trip and filled the log at headset rate)
	static int checks, logged;
	if (checks >= 1000 || logged >= 10)
		return;
	checks++;
	for (GLenum error; (error = glGetError()) != GL_NO_ERROR && logged < 10; logged++)
		ERROR_LOG(RENDERER, "VR %s OpenGL error 0x%04x", pass, error);
}

static_assert(GunMuzzle.x == gunmodel::Muzzle[0] && GunMuzzle.y == gunmodel::Muzzle[1] && GunMuzzle.z == gunmodel::Muzzle[2],
		"GunMuzzle (xr_gun.h) must match the baked model");

struct Material
{
	glm::vec3 color;
	float specular;		// highlight strength
	float shininess;	// highlight tightness
	glm::vec3 emissive;
};
// per model part, by name
struct PartMaterial { const char *name; Material material; };
const PartMaterial Materials[] = {
	{ "hardsurfaces", { { 0.80f, 0.05f, 0.04f }, 0.70f, 60.f, { 0.f, 0.f, 0.f } } },		// candy red shell
	{ "softsurfaces", { { 0.56f, 0.03f, 0.03f }, 0.35f, 28.f, { 0.f, 0.f, 0.f } } },		// deeper red grip and panels
	{ "screws", { { 0.16f, 0.16f, 0.18f }, 0.8f, 80.f, { 0.f, 0.f, 0.f } } },			// dark metal
	{ "lens", { { 0.06f, 0.01f, 0.01f }, 1.0f, 140.f, { 0.12f, 0.f, 0.f } } },			// smoked lens, faint glow
};

// The model, on the GPU
GLuint modelVbo, modelIbo, modelVao;
GLuint glowVbo, glowVao;
GLuint modelProgram;
GLint mMvp = -1, mModel = -1, mEye = -1, mColor = -1, mSpecular = -1, mShininess = -1, mEmissive = -1, mLight = -1;

// Glowing bits (aim line, dot, muzzle flash): client-side, a few quads
struct GlowVertex
{
	glm::vec3 pos;
	glm::vec4 color;
	glm::vec2 corner;	// spots: -1..1 across, for the round falloff
};
GLuint glowProgram;
GLint gMvp = -1, gShape = -1, gStarAngle = -1;
enum Shape { Line, Dot, Star, Flame, Smoke };

// Gun smoke: a few puffs left in the room at each shot, drifting up and fading.
struct Puff
{
	glm::vec3 pos;
	glm::vec3 vel;
	double born;
	float size;
};
std::vector<Puff> smoke;
unsigned lastShot[2];		// per player

// A cheap deterministic random number in 0..1 for shot n, stream k.
float shotRandom(unsigned n, unsigned k)
{
	unsigned h = n * 747796405u + k * 2891336453u + 0x9E3779B9u;
	h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
	return (h & 0xffffff) / float(0x1000000);
}

bool initModel()
{
	if (modelProgram != 0)
		return true;
	OpenGlSource vertex;
	vertex.addSource(VertexCompatShader).addSource(R"(
in highp vec3 in_pos;
in highp vec3 in_normal;
uniform highp mat4 mvp;
uniform highp mat4 model;
out highp vec3 vtx_pos;
out highp vec3 vtx_normal;
void main()
{
	vtx_pos = (model * vec4(in_pos, 1.0)).xyz;
	vtx_normal = mat3(model) * in_normal;
	gl_Position = mvp * vec4(in_pos, 1.0);
}
)");
	OpenGlSource fragment;
	fragment.addSource(PixelCompatShader).addSource(R"(
uniform highp vec3 eyePos;
uniform mediump vec3 baseColor;
uniform mediump float specular;
uniform mediump float shininess;
uniform mediump vec3 emissive;
uniform highp vec4 muzzleLight;	// room position, strength
in highp vec3 vtx_pos;
in highp vec3 vtx_normal;
void main()
{
	highp vec3 n = normalize(vtx_normal);
	highp vec3 v = normalize(eyePos - vtx_pos);
	if (dot(n, v) < 0.0)
		n = -n;		// inside faces seen through gaps
	const highp vec3 key = vec3(0.32, 0.86, 0.40);		// normalised: up, front, a bit right
	const highp vec3 fill = vec3(-0.45, -0.75, -0.48);
	mediump float diffuse = max(dot(n, key), 0.0);
	mediump float back = max(dot(n, fill), 0.0);
	highp vec3 h = normalize(key + v);
	mediump float highlight = pow(max(dot(n, h), 0.0), shininess) * specular;
	// (clamped: a dot a hair over 1 would make the base negative, and pow() NaN)
	highp float edge = 1.0 - clamp(dot(n, v), 0.0, 1.0);
	mediump float rim = edge * edge * edge;
	mediump vec3 color = baseColor * (0.30 + 0.80 * diffuse + 0.25 * back)
			+ vec3(highlight)
			+ rim * (0.18 * baseColor + 0.10 * specular)
			+ emissive;
	// the muzzle flash lights up the front of the gun
	if (muzzleLight.w > 0.0)
	{
		highp vec3 toFlash = muzzleLight.xyz - vtx_pos;
		highp float fd = length(toFlash);
		mediump float lit = muzzleLight.w * max(dot(n, toFlash / fd), 0.15) / (1.0 + fd * fd * 600.0);
		color += (baseColor * 2.2 + 0.25) * vec3(1.0, 0.62, 0.28) * lit;
	}
	gl_FragColor = vec4(color, 1.0);
}
)");
	modelProgram = gl_CompileAndLink(vertex.generate().c_str(), fragment.generate().c_str());
	if (modelProgram == 0)
		return false;
	mMvp = glGetUniformLocation(modelProgram, "mvp");
	mModel = glGetUniformLocation(modelProgram, "model");
	mEye = glGetUniformLocation(modelProgram, "eyePos");
	mColor = glGetUniformLocation(modelProgram, "baseColor");
	mSpecular = glGetUniformLocation(modelProgram, "specular");
	mShininess = glGetUniformLocation(modelProgram, "shininess");
	mEmissive = glGetUniformLocation(modelProgram, "emissive");
	mLight = glGetUniformLocation(modelProgram, "muzzleLight");

	// interleaved position, normal
	std::vector<float> verts(gunmodel::VertexCount * 6);
	for (unsigned i = 0; i < gunmodel::VertexCount; i++)
	{
		for (int k = 0; k < 3; k++)
		{
			verts[i * 6 + k] = gunmodel::Positions[i * 3 + k] * gunmodel::PositionScale;
			verts[i * 6 + 3 + k] = gunmodel::Normals[i * 3 + k] / 127.f;
		}
	}
	modelVao = createVertexArray();
	bindVertexArray(modelVao);
	glGenBuffers(1, &modelVbo);
	glBindBuffer(GL_ARRAY_BUFFER, modelVbo);
	glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float), verts.data(), GL_STATIC_DRAW);
	glGenBuffers(1, &modelIbo);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, modelIbo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(gunmodel::Indices), gunmodel::Indices, GL_STATIC_DRAW);
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (const void *)0);
	glVertexAttribPointer(VERTEX_NORM_ARRAY, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (const void *)(3 * sizeof(float)));
	glEnableVertexAttribArray(VERTEX_POS_ARRAY);
	glEnableVertexAttribArray(VERTEX_NORM_ARRAY);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	bindVertexArray(0);
	return true;
}

bool initGlow()
{
	if (glowProgram != 0)
		return true;
	OpenGlSource vertex;
	vertex.addSource(VertexCompatShader).addSource(R"(
in highp vec3 in_pos;
in lowp vec4 in_base;
in highp vec2 in_uv;
uniform highp mat4 mvp;
out lowp vec4 vtx_color;
out highp vec2 vtx_corner;
void main()
{
	vtx_color = in_base;
	vtx_corner = in_uv;
	gl_Position = mvp * vec4(in_pos, 1.0);
}
)");
	OpenGlSource fragment;
	fragment.addSource(PixelCompatShader).addSource(R"(
uniform lowp float shape;		// 0 line, 1 dot, 2 star burst, 3 flame, 4 smoke
uniform highp float starAngle;
in lowp vec4 vtx_color;
in highp vec2 vtx_corner;
void main()
{
	mediump float a = vtx_color.a;
	highp float r = length(vtx_corner);
	if (shape > 3.5)
		a *= 1.0 - smoothstep(0.0, 1.0, r);			// soft round puff
	else if (shape > 2.5)
	{
		// along the barrel: x 0..1 from the muzzle out, y -1..1 across
		highp float along = clamp(vtx_corner.x, 0.0, 1.0);
		a *= pow(1.0 - along, 1.6) * (1.0 - vtx_corner.y * vtx_corner.y);
	}
	else if (shape > 1.5)
	{
		// six rays around a white-hot core
		highp float ang = atan(vtx_corner.y, vtx_corner.x) + starAngle;
		highp float rays = pow(abs(cos(ang * 3.0)), 10.0);
		a *= clamp((1.0 - r) * (0.25 + 1.6 * rays) + (1.0 - smoothstep(0.0, 0.4, r)), 0.0, 1.0);
	}
	else if (shape > 0.5)
		a *= 1.0 - smoothstep(0.25, 1.0, r);
	gl_FragColor = vec4(vtx_color.rgb, a);
}
)");
	glowProgram = gl_CompileAndLink(vertex.generate().c_str(), fragment.generate().c_str());
	if (glowProgram == 0)
		return false;
	gMvp = glGetUniformLocation(glowProgram, "mvp");
	gShape = glGetUniformLocation(glowProgram, "shape");
	gStarAngle = glGetUniformLocation(glowProgram, "starAngle");
	glowVao = createVertexArray();
	bindVertexArray(glowVao);
	glGenBuffers(1, &glowVbo);
	glBindBuffer(GL_ARRAY_BUFFER, glowVbo);
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, sizeof(GlowVertex), (const void *)offsetof(GlowVertex, pos));
	glVertexAttribPointer(VERTEX_COL_BASE_ARRAY, 4, GL_FLOAT, GL_FALSE, sizeof(GlowVertex), (const void *)offsetof(GlowVertex, color));
	glVertexAttribPointer(VERTEX_UV_ARRAY, 2, GL_FLOAT, GL_FALSE, sizeof(GlowVertex), (const void *)offsetof(GlowVertex, corner));
	glEnableVertexAttribArray(VERTEX_POS_ARRAY);
	glEnableVertexAttribArray(VERTEX_COL_BASE_ARRAY);
	glEnableVertexAttribArray(VERTEX_UV_ARRAY);
	bindVertexArray(0);
	return true;
}

void drawModel(const glm::mat4& viewProj, const glm::vec3& eyePos, const glm::mat4& pose, const glm::vec4& muzzleLight)
{
	glcache.UseProgram(modelProgram);
	const glm::mat4 mvp = viewProj * pose;
	glUniformMatrix4fv(mMvp, 1, GL_FALSE, &mvp[0][0]);
	glUniformMatrix4fv(mModel, 1, GL_FALSE, &pose[0][0]);
	glUniform3f(mEye, eyePos.x, eyePos.y, eyePos.z);
	glUniform4f(mLight, muzzleLight.x, muzzleLight.y, muzzleLight.z, muzzleLight.w);
	bindVertexArray(modelVao);
	glEnableVertexAttribArray(VERTEX_POS_ARRAY);
	glEnableVertexAttribArray(VERTEX_NORM_ARRAY);
	for (const gunmodel::Part& part : gunmodel::Parts)
	{
		const Material *m = &Materials[0].material;
		for (const PartMaterial& pm : Materials)
			if (!strcmp(pm.name, part.name))
				m = &pm.material;
		glUniform3f(mColor, m->color.r, m->color.g, m->color.b);
		glUniform1f(mSpecular, m->specular);
		glUniform1f(mShininess, m->shininess);
		glUniform3f(mEmissive, m->emissive.r, m->emissive.g, m->emissive.b);
		glDrawElements(GL_TRIANGLES, (GLsizei)part.count, GL_UNSIGNED_SHORT, (const void *)(part.first * sizeof(unsigned short)));
	}
	bindVertexArray(0);
}

void drawGlow(const std::vector<GlowVertex>& verts)
{
	if (verts.empty())
		return;
	bindVertexArray(glowVao);
	glBindBuffer(GL_ARRAY_BUFFER, glowVbo);
	glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(GlowVertex), verts.data(), GL_STREAM_DRAW);
	glDrawArrays(GL_TRIANGLES, 0, (GLsizei)verts.size());
	bindVertexArray(0);
}

// A quad facing the eye, around a point in room space.
void billboard(std::vector<GlowVertex>& out, const glm::vec3& center, const glm::vec3& eyePos, float size, const glm::vec4& color)
{
	const glm::vec3 toEye = glm::normalize(eyePos - center);
	glm::vec3 right = glm::cross(glm::vec3(0, 1, 0), toEye);
	right = glm::length(right) < 1e-4f ? glm::vec3(1, 0, 0) : glm::normalize(right);
	const glm::vec3 up = glm::cross(toEye, right);
	const glm::vec3 r = right * size, u = up * size;
	const GlowVertex a { center - r - u, color, { -1, -1 } }, b { center + r - u, color, { 1, -1 } };
	const GlowVertex c { center + r + u, color, { 1, 1 } }, d { center - r + u, color, { -1, 1 } };
	for (const GlowVertex& v : { a, b, c, a, c, d })
		out.push_back(v);
}


// A quad along the barrel from the muzzle forward, turned to face the eye: corner x runs
// 0..1 from the muzzle out, y -1..1 across.
void flame(std::vector<GlowVertex>& out, const glm::vec3& from, const glm::vec3& fwd, float length, float width,
		const glm::vec3& eyePos, const glm::vec4& color)
{
	glm::vec3 side = glm::cross(fwd, eyePos - from);
	side = glm::length(side) < 1e-6f ? glm::vec3(width, 0, 0) : glm::normalize(side) * width;
	const glm::vec3 to = from + fwd * length;
	const GlowVertex a { from - side, color, { 0, -1 } }, b { from + side, color, { 0, 1 } };
	const GlowVertex c { to + side, color, { 1, 1 } }, d { to - side, color, { 1, -1 } };
	for (const GlowVertex& v : { a, b, c, a, c, d })
		out.push_back(v);
}

void drawGlowShape(Shape shape, const std::vector<GlowVertex>& verts)
{
	if (verts.empty())
		return;
	glUniform1f(gShape, (float)shape);
	drawGlow(verts);
}

}	// namespace

bool gameGun()
{
	return config::VrGameHands && handsModel() != nullptr;
}

glm::vec3 gunMuzzle()
{
	return gameGun() ? handsModel()->muzzle : GunMuzzle;
}

float gunKick(float sinceShot)
{
	if (sinceShot < 0.f || sinceShot > 0.4f)
		return 0.f;
	// a damped spring: up to full kick at once, a small bounce back past rest at about
	// 0.11 s, settled by 0.4 s
	return std::exp(-sinceShot / 0.06f) * std::cos(sinceShot * (6.2831853f / 0.22f));
}

// Where the muzzle is as drawn (with the recoil), which way it points, and how bright its
// flash is now: full for two headset frames, then gone in another 50 ms.
static void muzzleNow(const GunView& gun, glm::vec3& muzzle, glm::vec3& forward, float& flash)
{
	muzzle = glm::vec3(gun.pose * glm::vec4(gunMuzzle(), 1.f));
	forward = glm::normalize(glm::vec3(gun.pose * glm::vec4(0.f, 0.f, -1.f, 0.f)));
	flash = gun.sinceShot < 0.f ? 0.f
			: gun.sinceShot < 0.02f ? 1.f
			: std::max(0.f, 1.f - (gun.sinceShot - 0.02f) / 0.05f);
}

void drawGunModel(const glm::mat4& viewProj, const glm::vec3& eyePos, const GunView& gun, bool clearDepth)
{
	const bool game = gameGun();
	if (!game && !initModel())
		return;
	glm::vec3 muzzle, forward;
	float flash;
	muzzleNow(gun, muzzle, forward, flash);
	// sizes that go with the model (flash, flame, smoke) follow its scale
	const float g = gun.scale;

	// The gun is in the player's hand, in front of anything in the game: draw it over the
	// image with its own depth test only.
	glcache.Disable(GL_SCISSOR_TEST);
	glcache.Disable(GL_STENCIL_TEST);
	glcache.Disable(GL_CULL_FACE);
	glcache.Disable(GL_BLEND);
	glcache.Enable(GL_DEPTH_TEST);
	glcache.DepthMask(GL_TRUE);
	glcache.DepthFunc(GL_LESS);
	if (clearDepth)
	{
		glClearDepthf(1.f);
		glClear(GL_DEPTH_BUFFER_BIT);
	}
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	GlVertexArray::unbind();

	const glm::vec4 muzzleLight(muzzle + forward * (0.02f * g), flash * 1.4f);
	if (game)
	{
		HandsView hands;
		hands.gunPose = gun.pose;
		hands.slide = gun.slide;
		hands.otherHand = gun.otherHand;
		hands.handPose = gun.handPose;
		hands.muzzleLight = muzzleLight;
		drawHands(viewProj, eyePos, hands);
	}
	else
		drawModel(viewProj, eyePos, gun.pose, muzzleLight);
	logGlErrors("gun");
}

void drawGunGlow(const glm::mat4& viewProj, const glm::vec3& eyePos, const GunView& gun, bool withSmoke)
{
	if (!initGlow())
		return;
	glm::vec3 muzzle, forward;
	float flash;
	muzzleNow(gun, muzzle, forward, flash);
	const float g = gun.scale;

	// A new shot leaves smoke at the muzzle, drifting out of the barrel and upward.
	unsigned& shotWas = lastShot[gun.player & 1];
	if (gun.shot != shotWas)
	{
		shotWas = gun.shot;
		// out of the barrel as it settles (the drawn gun is still kicked up now)
		const glm::vec3 muzzle = gun.restMuzzle, forward = gun.restForward;
		for (unsigned k = 0; gun.sinceShot < 0.1f && k < 3; k++)
		{
			const float spread = shotRandom(gun.shot, k) - 0.5f;
			const glm::vec3 up(0.f, 1.f, 0.f);
			smoke.push_back({ muzzle + forward * ((0.01f + 0.02f * k) * g),
					forward * (0.18f + 0.1f * k) + up * (0.05f + 0.04f * spread) + glm::vec3(spread * 0.04f, 0.f, 0.f),
					gun.now, (0.018f + 0.006f * k) * g });
		}
		if (smoke.size() > 36)
			smoke.erase(smoke.begin(), smoke.begin() + (smoke.size() - 36));
	}
	if (withSmoke)
		smoke.erase(std::remove_if(smoke.begin(), smoke.end(), [&](const Puff& p) { return gun.now - p.born > 0.9; }), smoke.end());

	// Glowing bits, in room space, on top. Smoke and the aim line are hidden by the guns
	// where they pass behind them; the muzzle flash and the aim dot always show.
	static std::vector<GlowVertex> line, smokeVerts, flames, stars, dots;
	line.clear();
	smokeVerts.clear();
	flames.clear();
	stars.clear();
	dots.clear();
	if (gun.aimLine)
	{
		const glm::vec3 dir = gun.aimPoint - muzzle;
		const float len = glm::length(dir);
		if (len > 0.05f)
		{
			const glm::vec3 fwd = dir / len;
			glm::vec3 side = glm::cross(fwd, eyePos - muzzle);
			side = glm::length(side) < 1e-6f ? glm::vec3(0.0008f, 0, 0) : glm::normalize(side) * 0.0008f;
			// fades out along the way: it guides without painting over the scene
			const glm::vec3 colour = gun.player == 1 ? glm::vec3(0.15f, 0.45f, 1.f) : glm::vec3(1.f, 0.08f, 0.05f);
			const glm::vec4 start { colour, 0.55f }, end { colour, 0.f };
			const glm::vec3 to = muzzle + fwd * std::min(len, 3.f);
			const GlowVertex a { muzzle - side, start, {} }, b { muzzle + side, start, {} };
			const GlowVertex c { to + side, end, {} }, d { to - side, end, {} };
			for (const GlowVertex& v : { a, b, c, a, c, d })
				line.push_back(v);
		}
	}
	// (the smoke of both guns, once)
	if (withSmoke)
		for (const Puff& p : smoke)
		{
			const float age = (float)(gun.now - p.born);
			const float t = age / 0.9f;
			// slows down as it spreads; grows, then thins out
			const glm::vec3 at = p.pos + p.vel * (0.35f * (1.f - std::exp(-age / 0.35f))) + glm::vec3(0.f, 0.03f * age, 0.f);
			const float alpha = 0.22f * std::min(1.f, age / 0.06f) * (1.f - t) * (1.f - t);
			billboard(smokeVerts, at, eyePos, p.size * (1.f + 3.5f * t), glm::vec4(0.62f, 0.6f, 0.58f, alpha));
		}
	if (flash > 0.f)
	{
		// a tongue of fire out of the barrel, and a star burst with a white-hot core
		const float reach = (0.07f + 0.06f * shotRandom(gun.shot, 7)) * g;
		const glm::vec3 tip = muzzle + forward * (0.006f * g);	// the lens sits a little inside the nose
		flame(flames, tip, forward, reach * (0.6f + 0.4f * flash), 0.016f * g, eyePos, glm::vec4(1.f, 0.55f, 0.15f, flash));
		flame(flames, tip, forward, reach * 0.55f, 0.007f * g, eyePos, glm::vec4(1.f, 0.95f, 0.75f, flash));
		const glm::vec3 at = muzzle + forward * (0.012f * g);
		billboard(stars, at, eyePos, (0.035f + 0.02f * flash) * g, glm::vec4(1.f, 0.7f, 0.25f, flash));
		billboard(stars, at, eyePos, 0.016f * g, glm::vec4(1.f, 1.f, 0.9f, flash));
	}
	if (gun.aimDot)
	{
		// about 0.6 degrees across wherever it lands
		const float size = glm::length(gun.aimPoint - eyePos) * 0.0055f;
		if (gun.aimOutside)
			// outside the game's view: a dim grey ring, nothing to hit here
			billboard(dots, gun.aimPoint, eyePos, size * 0.8f, glm::vec4(0.55f, 0.55f, 0.6f, 0.6f));
		else
		{
			const bool blue = gun.player == 1;
			billboard(dots, gun.aimPoint, eyePos, size, blue ? glm::vec4(0.2f, 0.5f, 1.f, 0.95f) : glm::vec4(1.f, 0.12f, 0.06f, 0.95f));
			billboard(dots, gun.aimPoint, eyePos, size * 0.4f, blue ? glm::vec4(0.75f, 0.9f, 1.f, 1.f) : glm::vec4(1.f, 0.85f, 0.7f, 1.f));
		}
	}

	glcache.UseProgram(glowProgram);
	glUniformMatrix4fv(gMvp, 1, GL_FALSE, &viewProj[0][0]);
	glUniform1f(gStarAngle, 6.2831853f * shotRandom(gun.shot, 3));
	glcache.Enable(GL_BLEND);
	// smoke and the line against the guns' depth (the guns' models are all drawn by now)
	glcache.Enable(GL_DEPTH_TEST);
	glcache.DepthFunc(GL_LESS);
	glcache.DepthMask(GL_FALSE);
	// smoke: see-through grey
	glcache.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	drawGlowShape(Smoke, smokeVerts);
	// light: added on top
	glcache.BlendFunc(GL_SRC_ALPHA, GL_ONE);
	drawGlowShape(Line, line);
	// the flash is in front of the nose, which would hide most of it from behind the gun
	glcache.Disable(GL_DEPTH_TEST);
	drawGlowShape(Flame, flames);
	drawGlowShape(Star, stars);
	drawGlowShape(Dot, dots);
	glcache.DepthMask(GL_TRUE);
	glcache.Disable(GL_BLEND);
	logGlErrors("gun glow");
}

void termGun()
{
	if (modelProgram != 0)
		glcache.DeleteProgram(modelProgram);
	if (glowProgram != 0)
		glcache.DeleteProgram(glowProgram);
	if (modelVbo != 0)
		glDeleteBuffers(1, &modelVbo);
	if (modelIbo != 0)
		glDeleteBuffers(1, &modelIbo);
	deleteVertexArray(modelVao);
	if (glowVbo != 0) glDeleteBuffers(1, &glowVbo);
	deleteVertexArray(glowVao);
	modelProgram = glowProgram = 0;
	modelVbo = modelIbo = 0;
	modelVao = glowVao = glowVbo = 0;
	mMvp = mModel = mEye = mColor = mSpecular = mShininess = mEmissive = mLight = -1;
	gMvp = gShape = gStarAngle = -1;
	smoke.clear();
	lastShot[0] = lastShot[1] = 0;
	termHands();
}

}
