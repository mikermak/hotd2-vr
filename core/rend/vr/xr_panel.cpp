/*
	A panel with text in the headset (hotd2-vr). See xr_panel.h.

	The text is drawn on the CPU with stb_truetype (the copy that comes with Dear ImGui) in
	Flycast's own Roboto, into a picture that goes on a quad.

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "xr_panel.h"
#include "rend/gles/gles.h"
#include "rend/gles/glcache.h"
#include "oslib/resources.h"
#include "log/Log.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "imgui/imstb_truetype.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <memory>

namespace vr::xr
{
namespace
{

constexpr int Width = 1024, Height = 448;
constexpr float WidthMetres = 1.3f, Distance = 1.6f;

std::unique_ptr<u8[]> fontData;
stbtt_fontinfo font;
bool fontTried, fontLoaded;

GLuint program, texture;
GLuint quadVao, quadVbo;
GLint uMvp = -1;
std::vector<u8> pixels;
std::string shown;			// what the picture says now

void logGlErrors()
{
	// (hotd2-vr: the first few seconds only, at most 10 lines: checked after every draw it
	// costs a GPU round trip and filled the log at headset rate)
	static int checks, logged;
	if (checks >= 1000 || logged >= 10)
		return;
	checks++;
	for (GLenum error; (error = glGetError()) != GL_NO_ERROR && logged < 10; logged++)
		ERROR_LOG(RENDERER, "VR panel OpenGL error 0x%04x", error);
}

bool loadFont()
{
	if (fontTried)
		return fontLoaded;
	fontTried = true;
	size_t size = 0;
	fontData = resource::load("fonts/Roboto-Bold.ttf", size);
	fontLoaded = fontData != nullptr && stbtt_InitFont(&font, fontData.get(), stbtt_GetFontOffsetForIndex(fontData.get(), 0)) != 0;
	if (!fontLoaded)
		WARN_LOG(RENDERER, "XR: no font for the panel");
	return fontLoaded;
}

void blend(int x, int y, const glm::vec3& colour, float alpha)
{
	if (x < 0 || y < 0 || x >= Width || y >= Height || alpha <= 0.f)
		return;
	u8 *p = &pixels[((size_t)y * Width + x) * 4];
	for (int c = 0; c < 3; c++)
		p[c] = (u8)std::lround(p[c] * (1.f - alpha) + colour[c] * 255.f * alpha);
}

void fill(int x0, int y0, int x1, int y1, const glm::vec3& colour, float alpha = 1.f)
{
	for (int y = std::max(y0, 0); y < std::min(y1, Height); y++)
		for (int x = std::max(x0, 0); x < std::min(x1, Width); x++)
			blend(x, y, colour, alpha);
}

// One line, centred, its baseline at y.
void text(const std::string& s, float px, int y, const glm::vec3& colour)
{
	const float scale = stbtt_ScaleForPixelHeight(&font, px);
	float width = 0.f;
	for (size_t i = 0; i < s.size(); i++)
	{
		int advance, bearing;
		stbtt_GetCodepointHMetrics(&font, (u8)s[i], &advance, &bearing);
		width += advance * scale;
		if (i + 1 < s.size())
			width += scale * stbtt_GetCodepointKernAdvance(&font, (u8)s[i], (u8)s[i + 1]);
	}
	float x = (Width - width) * 0.5f;
	std::vector<u8> glyph;
	for (size_t i = 0; i < s.size(); i++)
	{
		const int c = (u8)s[i];
		int advance, bearing, x0, y0, x1, y1;
		stbtt_GetCodepointHMetrics(&font, c, &advance, &bearing);
		stbtt_GetCodepointBitmapBoxSubpixel(&font, c, scale, scale, x - std::floor(x), 0.f, &x0, &y0, &x1, &y1);
		const int w = x1 - x0, h = y1 - y0;
		if (w > 0 && h > 0)
		{
			glyph.assign((size_t)w * h, 0);
			stbtt_MakeCodepointBitmapSubpixel(&font, glyph.data(), w, h, w, scale, scale, x - std::floor(x), 0.f, c);
			for (int gy = 0; gy < h; gy++)
				for (int gx = 0; gx < w; gx++)
					blend((int)std::floor(x) + x0 + gx, y + y0 + gy, colour, glyph[(size_t)gy * w + gx] / 255.f);
		}
		x += advance * scale;
		if (i + 1 < s.size())
			x += scale * stbtt_GetCodepointKernAdvance(&font, c, (u8)s[i + 1]);
	}
}

void paint(const Panel& panel)
{
	pixels.assign((size_t)Width * Height * 4, 0);
	for (size_t i = 0; i < pixels.size(); i += 4)
	{
		pixels[i] = 16;
		pixels[i + 1] = 15;
		pixels[i + 2] = 17;
		pixels[i + 3] = 235;
	}
	// a thin red frame, like the game's own
	const glm::vec3 red(0.62f, 0.06f, 0.06f);
	fill(0, 0, Width, 5, red);
	fill(0, Height - 5, Width, Height, red);
	fill(0, 0, 5, Height, red);
	fill(Width - 5, 0, Width, Height, red);

	float total = 0.f;
	for (const PanelLine& line : panel.lines)
		total += line.size * Height * 1.45f;
	const int bar = panel.progress >= 0.f ? 46 : 0;
	float y = (Height - total - bar) * 0.5f;
	for (const PanelLine& line : panel.lines)
	{
		const float px = line.size * Height;
		y += px * 1.45f;
		if (!line.text.empty())
			text(line.text, px, (int)(y - px * 0.38f), line.colour);
	}
	if (panel.progress >= 0.f)
	{
		const int x0 = Width * 18 / 100, x1 = Width - x0, top = (int)y + 24, bottom = top + 14;
		fill(x0, top, x1, bottom, glm::vec3(0.24f, 0.24f, 0.26f));
		fill(x0, top, x0 + (int)((x1 - x0) * std::clamp(panel.progress, 0.f, 1.f)), bottom, glm::vec3(0.8f, 0.1f, 0.1f));
	}
	if (!panel.footer.empty())
		text(panel.footer, Height * 0.042f, Height - 28, glm::vec3(0.55f));
}

bool initGl()
{
	if (program != 0)
		return true;
	OpenGlSource vertex;
	vertex.addSource(VertexCompatShader).addSource(R"(
in highp vec4 in_pos;
in highp vec2 in_uv;
uniform highp mat4 mvp;
out highp vec2 vtx_uv;
void main()
{
	vtx_uv = in_uv;
	gl_Position = mvp * vec4(in_pos.xyz, 1.0);
}
)");
	OpenGlSource fragment;
	fragment.addSource(PixelCompatShader).addSource(R"(
uniform sampler2D tex;
in highp vec2 vtx_uv;
void main()
{
	gl_FragColor = texture(tex, vtx_uv);
}
)");
	program = gl_CompileAndLink(vertex.generate().c_str(), fragment.generate().c_str());
	if (program == 0)
		return false;
	uMvp = glGetUniformLocation(program, "mvp");
	glcache.UseProgram(program);
	glUniform1i(glGetUniformLocation(program, "tex"), 0);
	glGenTextures(1, &texture);
	glcache.BindTexture(GL_TEXTURE_2D, texture);
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glcache.BindTexture(GL_TEXTURE_2D, 0);
	quadVao = createVertexArray();
	bindVertexArray(quadVao);
	glGenBuffers(1, &quadVbo);
	glBindBuffer(GL_ARRAY_BUFFER, quadVbo);
	glBufferData(GL_ARRAY_BUFFER, 20 * sizeof(float), nullptr, GL_STREAM_DRAW);
	glVertexAttribPointer(VERTEX_POS_ARRAY, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (const void *)0);
	glVertexAttribPointer(VERTEX_UV_ARRAY, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (const void *)(3 * sizeof(float)));
	glEnableVertexAttribArray(VERTEX_POS_ARRAY);
	glEnableVertexAttribArray(VERTEX_UV_ARRAY);
	bindVertexArray(0);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	shown.clear();
	return true;
}

}	// namespace

void drawPanel(const glm::mat4& viewProj, const Panel& panel)
{
	if (!loadFont() || !initGl())
		return;
	// what it says, the bar in steps of half a percent
	std::string says;
	for (const PanelLine& line : panel.lines)
		says += line.text + '\n';
	says += panel.footer + '\n' + std::to_string(panel.progress < 0.f ? -1 : (int)(panel.progress * 200.f));
	glActiveTexture(GL_TEXTURE0);
	glcache.BindTexture(GL_TEXTURE_2D, texture);
	if (says != shown)
	{
		shown = says;
		paint(panel);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, Width, Height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	}
	const float hx = WidthMetres * 0.5f, hy = hx * Height / Width;
	const float quad[] {
		-hx,  hy, -Distance,  0.f, 0.f,
		 hx,  hy, -Distance,  1.f, 0.f,
		-hx, -hy, -Distance,  0.f, 1.f,
		 hx, -hy, -Distance,  1.f, 1.f,
	};
	glBindBuffer(GL_ARRAY_BUFFER, quadVbo);
	glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
	glcache.UseProgram(program);
	glUniformMatrix4fv(uMvp, 1, GL_FALSE, &viewProj[0][0]);
	glcache.Disable(GL_SCISSOR_TEST);
	glcache.Disable(GL_STENCIL_TEST);
	glcache.Disable(GL_DEPTH_TEST);
	glcache.Disable(GL_CULL_FACE);
	glcache.Enable(GL_BLEND);
	glcache.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);	// (through the cache: the game's next draw relies on it)
	bindVertexArray(quadVao);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	bindVertexArray(0);
	glcache.Disable(GL_BLEND);
	glcache.BindTexture(GL_TEXTURE_2D, 0);
	logGlErrors();
}

void termPanel()
{
	if (program != 0)
		glcache.DeleteProgram(program);
	if (texture != 0)
		glcache.DeleteTextures(1, &texture);
	if (quadVbo != 0) glDeleteBuffers(1, &quadVbo);
	deleteVertexArray(quadVao);
	program = texture = 0;
	quadVao = quadVbo = 0;
	uMvp = -1;
	shown.clear();
}

}
