/*
	The agent's hands and pistol from the game itself (hotd2-vr), or the hero's staff. See
	xr_hands.h.

	hands.bin in the data directory (little endian, made from the player's own game by
	hands_rip.cpp, or on the PC by hotd2-vr/assets/rip_hands.py), and in the same format the
	hero's staff of The Maze of the Kings, staff-mok.bin (only the running game's file is
	read, again when another game starts):
		"HND1", u32 version 1
		f32 muzzle[3], grab[3], travel, onSlide[16] (column major)
		u32 textures, each: u32 width, height, flags (1 clamp u, 2 clamp v, 4 mirror u,
			8 mirror v, 16 alpha test), then width * height RGBA bytes
		u32 meshes (frame, slide, gun hand, open hand), each: u32 parts, each: u32 texture,
			u32 vertices, then the vertices: f32 position[3], normal[3], uv[2], u8 rgba[4]
	A model without a slide (the staff) has travel 0 and no parts in the slide mesh.

	Lit like the arcade gun (xr_gun.cpp): a fixed key light and a dim fill, so the hands
	read as solid in the game's dark scenes, rather than with the light baked in that one
	frame of the game.

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "xr_hands.h"
#include "hands_rip.h"
#include "rend/vr_reproject.h"
#include "rend/gles/gles.h"
#include "rend/gles/glcache.h"
#include "cfg/option.h"
#include "log/Log.h"
#include "types.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace vr::xr
{
namespace
{

enum { MeshFrame, MeshSlide, MeshGunHand, MeshOpenHand, MeshCount };

struct Texture
{
	u32 width, height, flags;
	std::vector<u8> rgba;
	GLuint id = 0;
};
struct Part
{
	u32 texture;
	u32 first, count;	// vertices
};
constexpr size_t VertexSize = 36;

struct Model
{
	HandsModel info;
	std::vector<Texture> textures;
	std::vector<Part> meshes[MeshCount];
	std::vector<u8> vertices;
	bool staff = false;	// the hero's staff (its fist off unless vr.StaffHand)
};
Model model;
bool tried, loaded;

GLuint program, vbo;
GLint uMvp = -1, uModel = -1, uEye = -1, uSpecular = -1, uShininess = -1, uAlphaTest = -1, uLight = -1, uTex = -1;

bool load()
{
	std::string path = hands::modelPath();
	if (path.empty())
	{
		INFO_LOG(RENDERER, "XR: no model of its own for this game, the arcade gun it is");
		return false;
	}
	// where older builds had it: the app's internal files
	FILE *f = nowide::fopen(path.c_str(), "rb");
	if (f == nullptr && !hands::legacyModelPath().empty())
	{
		path = hands::legacyModelPath();
		f = nowide::fopen(path.c_str(), "rb");
	}
	if (f == nullptr)
	{
		INFO_LOG(RENDERER, "XR: no %s, the arcade gun it is", hands::modelPath().c_str());
		return false;
	}
	bool ok = true;
	auto read = [&](void *to, size_t size) {
		ok = ok && fread(to, 1, size, f) == size;
		return ok;
	};
	auto u32At = [&]() {
		u32 v = 0;
		read(&v, 4);
		return v;
	};
	char magic[4] {};
	read(magic, 4);
	const u32 version = u32At();
	if (!ok || memcmp(magic, "HND1", 4) != 0 || version != 1)
	{
		fclose(f);
		WARN_LOG(RENDERER, "XR: %s is not a hands model this build reads", path.c_str());
		return false;
	}
	float head[3 + 3 + 1 + 16];
	read(head, sizeof(head));
	model.info.muzzle = glm::make_vec3(&head[0]);
	model.info.grab = glm::make_vec3(&head[3]);
	model.info.travel = head[6];
	model.info.onSlide = glm::make_mat4(&head[7]);
	const u32 textures = u32At();
	for (u32 i = 0; ok && i < textures && i < 64; i++)
	{
		Texture t;
		t.width = u32At();
		t.height = u32At();
		t.flags = u32At();
		if (!ok || t.width == 0 || t.height == 0 || t.width > 1024 || t.height > 1024)
		{
			ok = false;
			break;
		}
		t.rgba.resize((size_t)t.width * t.height * 4);
		read(t.rgba.data(), t.rgba.size());
		model.textures.push_back(std::move(t));
	}
	const u32 meshes = u32At();
	for (u32 m = 0; ok && m < meshes && m < MeshCount; m++)
	{
		const u32 parts = u32At();
		for (u32 p = 0; ok && p < parts && p < 64; p++)
		{
			Part part;
			part.texture = u32At();
			part.count = u32At();
			part.first = (u32)(model.vertices.size() / VertexSize);
			if (!ok || part.texture >= model.textures.size() || part.count > 65536)
			{
				ok = false;
				break;
			}
			const size_t at = model.vertices.size();
			model.vertices.resize(at + part.count * VertexSize);
			read(&model.vertices[at], part.count * VertexSize);
			model.meshes[m].push_back(part);
		}
	}
	fclose(f);
	if (!ok || meshes < MeshCount)
	{
		WARN_LOG(RENDERER, "XR: %s is cut short or damaged", path.c_str());
		model = Model();
		return false;
	}
	const hands::Source *source = gameModelSource();
	model.staff = source != nullptr && source->kind == hands::Source::HerosStaff;
	NOTICE_LOG(RENDERER, "XR: the game's own model from %s (%zu textures, %zu vertices%s)", path.c_str(),
			model.textures.size(), model.vertices.size() / VertexSize, model.info.travel > 0.f ? "" : ", no slide");
	return true;
}

bool initGl()
{
	if (program != 0)
		return true;
	OpenGlSource vertex;
	vertex.addSource(VertexCompatShader).addSource(R"(
in highp vec3 in_pos;
in highp vec3 in_normal;
in highp vec2 in_uv;
in lowp vec4 in_base;
uniform highp mat4 mvp;
uniform highp mat4 model;
out highp vec3 vtx_pos;
out highp vec3 vtx_normal;
out highp vec2 vtx_uv;
out lowp vec4 vtx_color;
void main()
{
	vtx_pos = (model * vec4(in_pos, 1.0)).xyz;
	vtx_normal = mat3(model) * in_normal;
	vtx_uv = in_uv;
	vtx_color = in_base;
	gl_Position = mvp * vec4(in_pos, 1.0);
}
)");
	OpenGlSource fragment;
	fragment.addSource(PixelCompatShader).addSource(R"(
uniform sampler2D tex;
uniform highp vec3 eyePos;
uniform mediump float specular;
uniform mediump float shininess;
uniform lowp float alphaTest;
uniform highp vec4 muzzleLight;	// room position, strength
in highp vec3 vtx_pos;
in highp vec3 vtx_normal;
in highp vec2 vtx_uv;
in lowp vec4 vtx_color;
void main()
{
	lowp vec4 base = texture(tex, vtx_uv) * vtx_color;
	if (base.a < alphaTest)
		discard;
	highp vec3 n = normalize(vtx_normal);
	highp vec3 v = normalize(eyePos - vtx_pos);
	if (dot(n, v) < 0.0)
		n = -n;		// a mirrored model, or inside faces seen through gaps
	const highp vec3 key = vec3(0.32, 0.86, 0.40);
	const highp vec3 fill = vec3(-0.45, -0.75, -0.48);
	mediump float diffuse = max(dot(n, key), 0.0);
	mediump float back = max(dot(n, fill), 0.0);
	highp vec3 h = normalize(key + v);
	mediump float highlight = pow(max(dot(n, h), 0.0), shininess) * specular;
	highp float edge = 1.0 - clamp(dot(n, v), 0.0, 1.0);
	mediump vec3 color = base.rgb * (0.32 + 0.72 * diffuse + 0.22 * back + 0.12 * edge * edge)
			+ vec3(highlight);
	if (muzzleLight.w > 0.0)
	{
		highp vec3 toFlash = muzzleLight.xyz - vtx_pos;
		highp float fd = length(toFlash);
		mediump float lit = muzzleLight.w * max(dot(n, toFlash / fd), 0.15) / (1.0 + fd * fd * 600.0);
		color += (base.rgb * 2.2 + 0.2) * vec3(1.0, 0.62, 0.28) * lit;
	}
	gl_FragColor = vec4(color, 1.0);
}
)");
	program = gl_CompileAndLink(vertex.generate().c_str(), fragment.generate().c_str());
	if (program == 0)
		return false;
	uMvp = glGetUniformLocation(program, "mvp");
	uModel = glGetUniformLocation(program, "model");
	uEye = glGetUniformLocation(program, "eyePos");
	uSpecular = glGetUniformLocation(program, "specular");
	uShininess = glGetUniformLocation(program, "shininess");
	uAlphaTest = glGetUniformLocation(program, "alphaTest");
	uLight = glGetUniformLocation(program, "muzzleLight");
	uTex = glGetUniformLocation(program, "tex");

	GlVertexArray::unbind();
	glGenBuffers(1, &vbo);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER, model.vertices.size(), model.vertices.data(), GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glActiveTexture(GL_TEXTURE0);
	for (Texture& t : model.textures)
	{
		glGenTextures(1, &t.id);
		glcache.BindTexture(GL_TEXTURE_2D, t.id);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t.width, t.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.rgba.data());
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, t.flags & 1 ? GL_CLAMP_TO_EDGE : t.flags & 4 ? GL_MIRRORED_REPEAT : GL_REPEAT);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, t.flags & 2 ? GL_CLAMP_TO_EDGE : t.flags & 8 ? GL_MIRRORED_REPEAT : GL_REPEAT);
	}
	glcache.BindTexture(GL_TEXTURE_2D, 0);
	return true;
}

void drawMesh(int mesh, const glm::mat4& viewProj, const glm::mat4& pose, float specular, float shininess)
{
	const glm::mat4 mvp = viewProj * pose;
	glUniformMatrix4fv(uMvp, 1, GL_FALSE, &mvp[0][0]);
	glUniformMatrix4fv(uModel, 1, GL_FALSE, &pose[0][0]);
	glUniform1f(uSpecular, specular);
	glUniform1f(uShininess, shininess);
	for (const Part& part : model.meshes[mesh])
	{
		const Texture& t = model.textures[part.texture];
		glcache.BindTexture(GL_TEXTURE_2D, t.id);
		glUniform1f(uAlphaTest, t.flags & 16 ? 0.5f : -1.f);
		glDrawArrays(GL_TRIANGLES, (GLint)part.first, (GLsizei)part.count);
	}
}

}	// namespace

const HandsModel *handsModel()
{
	if (hands::takeNewModel())
	{
		// just made from the game, or another game's: in with it
		termHands();
		model = Model();
		tried = false;
	}
	if (!tried)
	{
		tried = true;
		loaded = load();
	}
	return loaded ? &model.info : nullptr;
}

void drawHands(const glm::mat4& viewProj, const glm::vec3& eyePos, const HandsView& view)
{
	if (handsModel() == nullptr || !initGl())
		return;
	glcache.UseProgram(program);
	glUniform3f(uEye, eyePos.x, eyePos.y, eyePos.z);
	glUniform4f(uLight, view.muzzleLight.x, view.muzzleLight.y, view.muzzleLight.z, view.muzzleLight.w);
	glUniform1i(uTex, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, VertexSize, (const void *)0);
	glVertexAttribPointer(VERTEX_NORM_ARRAY, 3, GL_FLOAT, GL_FALSE, VertexSize, (const void *)12);
	glVertexAttribPointer(VERTEX_UV_ARRAY, 2, GL_FLOAT, GL_FALSE, VertexSize, (const void *)24);
	glVertexAttribPointer(VERTEX_COL_BASE_ARRAY, 4, GL_UNSIGNED_BYTE, GL_TRUE, VertexSize, (const void *)32);
	glEnableVertexAttribArray(VERTEX_POS_ARRAY);
	glEnableVertexAttribArray(VERTEX_NORM_ARRAY);
	glEnableVertexAttribArray(VERTEX_UV_ARRAY);
	glEnableVertexAttribArray(VERTEX_COL_BASE_ARRAY);

	// metal with a sheen, skin and cotton matt
	drawMesh(MeshFrame, viewProj, view.gunPose, 0.35f, 40.f);
	drawMesh(MeshSlide, viewProj, glm::translate(view.gunPose, glm::vec3(0.f, 0.f, view.slide)), 0.45f, 50.f);
	// (the hero's fist sits where the player's own hand is, but made for a staff held
	// upright: it looked better left out)
	if (!model.staff || config::VrStaffHand)
		drawMesh(MeshGunHand, viewProj, view.gunPose, 0.06f, 10.f);
	if (view.otherHand)
		drawMesh(MeshOpenHand, viewProj, view.handPose, 0.06f, 10.f);

	glDisableVertexAttribArray(VERTEX_POS_ARRAY);
	glDisableVertexAttribArray(VERTEX_NORM_ARRAY);
	glDisableVertexAttribArray(VERTEX_UV_ARRAY);
	glDisableVertexAttribArray(VERTEX_COL_BASE_ARRAY);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glcache.BindTexture(GL_TEXTURE_2D, 0);
}

void termHands()
{
	if (program != 0)
		glcache.DeleteProgram(program);
	if (vbo != 0)
		glDeleteBuffers(1, &vbo);
	for (Texture& t : model.textures)
		if (t.id != 0)
		{
			glcache.DeleteTextures(1, &t.id);
			t.id = 0;
		}
	program = vbo = 0;
	uMvp = uModel = uEye = uSpecular = uShininess = uAlphaTest = uLight = uTex = -1;
}

}
