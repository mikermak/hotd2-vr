/*
	OpenXR host (hotd2-vr). See xr_host.h.

	Everything here runs on Flycast's render thread, which owns the EGL context the
	OpenXR session is bound to. Initialisation happens on the first frame.

	World layout: the game camera sits at the player's head as it was when the session
	started (or at the last recenter), looking where the player looked. Rebuilt game
	eye-space positions (game units, renderer y convention: y down) map to the headset's
	local space (metres, y up) by a uniform scale (vr.WorldScale) and a y flip.

	Copyright 2026 mikermak. This file is part of Flycast and is distributed under the GNU GPL v2 or later.
*/
#include "xr_host.h"

#ifdef USE_OPENXR
#include "xr_gun.h"
#include "xr_hands.h"
#include "xr_panel.h"
#include "hands_rip.h"
#include "rend/gles/gles.h"
#ifdef __ANDROID__
// the Quest: OpenGL ES through EGL
#include <jni.h>
#include <glad/egl.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#else
// PCVR: desktop OpenGL through WGL
#include <windows.h>
#include <unknwn.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "rend/vr_reproject.h"
#include "rend/TexCache.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/ta_ctx.h"
#include "input/udp_lightgun.h"
#include "cfg/option.h"
#include "cfg/cfg.h"
#include "stdclass.h"
#include "emulator.h"
#include "log/Log.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iterator>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef __ANDROID__
extern JavaVM *g_jvm;
extern jobject g_activity;
#endif

namespace vr::xr
{
namespace
{

#ifdef __ANDROID__
using SwapchainImage = XrSwapchainImageOpenGLESKHR;
constexpr XrStructureType SwapchainImageType = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
// Plain RGBA8: the game's colours were made for a gamma display, like the eye buffers.
constexpr int64_t SwapchainFormats[] { GL_RGBA8 };
#else
using SwapchainImage = XrSwapchainImageOpenGLKHR;
constexpr XrStructureType SwapchainImageType = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
// PC runtimes take RGBA8 as linear and brighten it. An sRGB image written to as it is (no
// GL_FRAMEBUFFER_SRGB) shows the colours as on the Quest.
constexpr int64_t SwapchainFormats[] { GL_SRGB8_ALPHA8, GL_RGBA8 };
#endif

constexpr float NearMetres = 0.05f;
constexpr u32 RestartIndex = 0xFFFFFFFFu;

XrInstance instance = XR_NULL_HANDLE;
XrSystemId systemId = XR_NULL_SYSTEM_ID;
XrSession session = XR_NULL_HANDLE;
XrSpace localSpace = XR_NULL_HANDLE;
constexpr int Left = 0, Right = 1;
XrPath handPaths[2];
XrSpace aimSpaces[2];
XrSpace gripSpaces[2];		// the hand on the handle (grip pose): where the gun's grip goes
XrActionSet actionSet = XR_NULL_HANDLE;
XrAction aimAction, handAction, triggerAction, startAction, skipAction, recenterAction, hapticAction, stickAction, gripAction;
PFN_xrRequestDisplayRefreshRateFB requestRefreshRate;
PFN_xrPerfSettingsSetPerformanceLevelEXT setPerformanceLevel;

bool initTried;
bool sessionRunning;
bool focused;
std::chrono::steady_clock::time_point focusLost;
// The runtime lost the session (start a new one) or wants the app to quit (don't).
bool sessionLost;
bool exiting;

struct EyeChain
{
	XrSwapchain chain = XR_NULL_HANDLE;
	int width = 0, height = 0;
	std::vector<SwapchainImage> images;
	std::vector<GLuint> fbos;
	GLuint depthStencil = 0;
};
EyeChain chains[2];
XrView views[2] { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };

Eye eye;
bool drawingEye;

// Where the game camera is: the head pose at start / recenter (yaw only).
bool originSet;
bool recenterRequested;
glm::vec3 originPos;
glm::quat originRot { 1, 0, 0, 0 };

// The light guns. Player 1's is in the hand whose trigger was pulled last; once player 2
// joins (vr.DualWield: the left controller's Start), player 1's is in the right hand and
// player 2's in the left: dual wielding.
struct Gun
{
	int player;
	int hand = Right;
	bool triggerWas;
	// The current pull started outside the game's view (aimOutside): it does nothing until
	// the trigger is let go, even when the aim moves into view.
	bool pullOutside;
	XrTime lastShot;
	unsigned shotCount;
	// When the last reload started (racked slide, gun pointed down; 0: done), and a
	// trigger held through it.
	XrTime reloadAt;
	bool triggerWait;
	bool pointedDown;
	// as drawn (no pose: hidden), with the last shot for recoil and flash
	GunView view;
	bool visible;
	int palmLogged = -1;	// hand * 2 + where the palm came from, as last logged
};
Gun guns[2] { { 0 }, { 1, Left } };
bool dualWield;
XrTime dualSince;
bool leftStartWas, recenterWas;
// A game (re)started: one gun again (EventManager calls on the emulator's thread).
std::atomic<bool> newGame;
struct NewGameRegistration
{
	NewGameRegistration() {
		EventManager::listen(Event::Start, [](Event, void *) { newGame = true; });
	}
} newGameRegistration;
// for capture-shot.request: trigger pulls so far, and where the last one went (DC pixels)
std::atomic<u32> shotSerial;
glm::vec2 lastShotScreen(-1.f);

bool check(XrResult result, const char *what)
{
	if (XR_SUCCEEDED(result))
		return true;
	char text[XR_MAX_RESULT_STRING_SIZE] = "";
	if (instance != XR_NULL_HANDLE)
		xrResultToString(instance, result, text);
	ERROR_LOG(RENDERER, "XR: %s failed: %d %s", what, result, text);
	return false;
}
#define XRCHECK(call) check((call), #call)

XrPath path(const char *s)
{
	XrPath p = XR_NULL_PATH;
	xrStringToPath(instance, s, &p);
	return p;
}

bool createAction(XrAction& action, const char *name, const char *label, XrActionType type, bool perHand = false)
{
	XrActionCreateInfo info { XR_TYPE_ACTION_CREATE_INFO };
	info.actionType = type;
	strncpy(info.actionName, name, sizeof(info.actionName) - 1);
	strncpy(info.localizedActionName, label, sizeof(info.localizedActionName) - 1);
	if (perHand)
	{
		info.countSubactionPaths = 2;
		info.subactionPaths = handPaths;
	}
	return XRCHECK(xrCreateAction(actionSet, &info, &action));
}

bool initActions()
{
	handPaths[Left] = path("/user/hand/left");
	handPaths[Right] = path("/user/hand/right");
	XrActionSetCreateInfo setInfo { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy(setInfo.actionSetName, "lightgun");
	strcpy(setInfo.localizedActionSetName, "Light gun");
	if (!XRCHECK(xrCreateActionSet(instance, &setInfo, &actionSet)))
		return false;
	if (!createAction(aimAction, "aim", "Aim", XR_ACTION_TYPE_POSE_INPUT, true)
			|| !createAction(handAction, "hand", "Hand (holds the gun)", XR_ACTION_TYPE_POSE_INPUT, true)
			|| !createAction(triggerAction, "trigger", "Fire", XR_ACTION_TYPE_FLOAT_INPUT, true)
			|| !createAction(startAction, "start", "Start (left: player 2 joins)", XR_ACTION_TYPE_BOOLEAN_INPUT, true)
			|| !createAction(skipAction, "skip", "Skip, back (the gun's B)", XR_ACTION_TYPE_BOOLEAN_INPUT, true)
			|| !createAction(recenterAction, "recenter", "Recenter", XR_ACTION_TYPE_BOOLEAN_INPUT)
			|| !createAction(hapticAction, "recoil", "Recoil", XR_ACTION_TYPE_VIBRATION_OUTPUT, true)
			|| !createAction(stickAction, "stick", "Menus (D-pad), world size", XR_ACTION_TYPE_VECTOR2F_INPUT)
			|| !createAction(gripAction, "grip", "Rack the slide; hold for world size", XR_ACTION_TYPE_FLOAT_INPUT, true))
		return false;
	// Either trigger fires; shooting away from the screen reloads. Start (pauses): A. The
	// menu button on the left lets player 2 join (vr.DualWield), else it is player 1's Start
	// too. The gun's B (skips a story scene, backs out of menus): B or Y, under the thumb of
	// whichever hand holds the gun (with two guns: each its player's). Recenter: X, or a thumbstick click.
	// Thumbstick: the gun's D-pad (menus); with a grip held, up/down sizes the world. The
	// other hand's grip racks the game pistol's slide.
	auto suggest = [](const char *profile, std::initializer_list<std::pair<XrAction, const char *>> list) {
		std::vector<XrActionSuggestedBinding> bindings;
		for (const auto& [action, where] : list)
			bindings.push_back({ action, path(where) });
		XrInteractionProfileSuggestedBinding suggested { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		suggested.interactionProfile = path(profile);
		suggested.countSuggestedBindings = (u32)bindings.size();
		suggested.suggestedBindings = bindings.data();
		return check(xrSuggestInteractionProfileBindings(instance, &suggested), profile);
	};
	bool any = suggest("/interaction_profiles/oculus/touch_controller", {
		{ stickAction, "/user/hand/left/input/thumbstick" },
		{ stickAction, "/user/hand/right/input/thumbstick" },
		{ gripAction, "/user/hand/left/input/squeeze/value" },
		{ gripAction, "/user/hand/right/input/squeeze/value" },
		{ aimAction, "/user/hand/left/input/aim/pose" },
		{ aimAction, "/user/hand/right/input/aim/pose" },
		{ handAction, "/user/hand/left/input/grip/pose" },
		{ handAction, "/user/hand/right/input/grip/pose" },
		{ triggerAction, "/user/hand/left/input/trigger/value" },
		{ triggerAction, "/user/hand/right/input/trigger/value" },
		{ startAction, "/user/hand/right/input/a/click" },
		{ startAction, "/user/hand/left/input/menu/click" },
		{ skipAction, "/user/hand/right/input/b/click" },
		{ skipAction, "/user/hand/left/input/y/click" },
		{ recenterAction, "/user/hand/left/input/x/click" },
		{ recenterAction, "/user/hand/left/input/thumbstick/click" },
		{ recenterAction, "/user/hand/right/input/thumbstick/click" },
		{ hapticAction, "/user/hand/left/output/haptic" },
		{ hapticAction, "/user/hand/right/output/haptic" },
	});
#ifndef __ANDROID__
	// PCVR: the other controllers, as near as they come: a Start on each hand (the left one
	// lets player 2 join) and the gun's B. Index: A and B, recenter with a stick click. Vive
	// wands and Windows Mixed Reality: the menu button is Start, a click on the pad the gun's
	// B; the pad or stick is the D-pad.
	any |= suggest("/interaction_profiles/valve/index_controller", {
		{ stickAction, "/user/hand/left/input/thumbstick" },
		{ stickAction, "/user/hand/right/input/thumbstick" },
		{ gripAction, "/user/hand/left/input/squeeze/value" },
		{ gripAction, "/user/hand/right/input/squeeze/value" },
		{ aimAction, "/user/hand/left/input/aim/pose" },
		{ aimAction, "/user/hand/right/input/aim/pose" },
		{ handAction, "/user/hand/left/input/grip/pose" },
		{ handAction, "/user/hand/right/input/grip/pose" },
		{ triggerAction, "/user/hand/left/input/trigger/value" },
		{ triggerAction, "/user/hand/right/input/trigger/value" },
		{ startAction, "/user/hand/right/input/a/click" },
		{ startAction, "/user/hand/left/input/a/click" },
		{ skipAction, "/user/hand/right/input/b/click" },
		{ skipAction, "/user/hand/left/input/b/click" },
		{ recenterAction, "/user/hand/left/input/thumbstick/click" },
		{ recenterAction, "/user/hand/right/input/thumbstick/click" },
		{ hapticAction, "/user/hand/left/output/haptic" },
		{ hapticAction, "/user/hand/right/output/haptic" },
	});
	any |= suggest("/interaction_profiles/htc/vive_controller", {
		{ stickAction, "/user/hand/left/input/trackpad" },
		{ stickAction, "/user/hand/right/input/trackpad" },
		{ gripAction, "/user/hand/left/input/squeeze/click" },
		{ gripAction, "/user/hand/right/input/squeeze/click" },
		{ aimAction, "/user/hand/left/input/aim/pose" },
		{ aimAction, "/user/hand/right/input/aim/pose" },
		{ handAction, "/user/hand/left/input/grip/pose" },
		{ handAction, "/user/hand/right/input/grip/pose" },
		{ triggerAction, "/user/hand/left/input/trigger/value" },
		{ triggerAction, "/user/hand/right/input/trigger/value" },
		{ startAction, "/user/hand/right/input/menu/click" },
		{ startAction, "/user/hand/left/input/menu/click" },
		{ skipAction, "/user/hand/right/input/trackpad/click" },
		{ skipAction, "/user/hand/left/input/trackpad/click" },
		{ hapticAction, "/user/hand/left/output/haptic" },
		{ hapticAction, "/user/hand/right/output/haptic" },
	});
	any |= suggest("/interaction_profiles/microsoft/motion_controller", {
		{ stickAction, "/user/hand/left/input/thumbstick" },
		{ stickAction, "/user/hand/right/input/thumbstick" },
		{ gripAction, "/user/hand/left/input/squeeze/click" },
		{ gripAction, "/user/hand/right/input/squeeze/click" },
		{ aimAction, "/user/hand/left/input/aim/pose" },
		{ aimAction, "/user/hand/right/input/aim/pose" },
		{ handAction, "/user/hand/left/input/grip/pose" },
		{ handAction, "/user/hand/right/input/grip/pose" },
		{ triggerAction, "/user/hand/left/input/trigger/value" },
		{ triggerAction, "/user/hand/right/input/trigger/value" },
		{ startAction, "/user/hand/right/input/menu/click" },
		{ startAction, "/user/hand/left/input/menu/click" },
		{ skipAction, "/user/hand/right/input/trackpad/click" },
		{ skipAction, "/user/hand/left/input/trackpad/click" },
		{ recenterAction, "/user/hand/left/input/thumbstick/click" },
		{ recenterAction, "/user/hand/right/input/thumbstick/click" },
		{ hapticAction, "/user/hand/left/output/haptic" },
		{ hapticAction, "/user/hand/right/output/haptic" },
	});
#endif
	if (!any)
		return false;
	XrSessionActionSetsAttachInfo attach { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attach.countActionSets = 1;
	attach.actionSets = &actionSet;
	if (!XRCHECK(xrAttachSessionActionSets(session, &attach)))
		return false;
	for (int hand : { Left, Right })
	{
		XrActionSpaceCreateInfo spaceInfo { XR_TYPE_ACTION_SPACE_CREATE_INFO };
		spaceInfo.action = aimAction;
		spaceInfo.subactionPath = handPaths[hand];
		spaceInfo.poseInActionSpace.orientation.w = 1.f;
		if (!XRCHECK(xrCreateActionSpace(session, &spaceInfo, &aimSpaces[hand])))
			return false;
		spaceInfo.action = handAction;
		if (!XRCHECK(xrCreateActionSpace(session, &spaceInfo, &gripSpaces[hand])))
			return false;
	}
	return true;
}

bool initSwapchains()
{
	u32 formatCount = 0;
	if (!XRCHECK(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr)))
		return false;
	std::vector<int64_t> formats(formatCount);
	if (!XRCHECK(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data())))
		return false;
	int64_t format = 0;
	for (int64_t wanted : SwapchainFormats)
		if (format == 0 && std::find(formats.begin(), formats.end(), wanted) != formats.end())
			format = wanted;
	if (format == 0)
	{
		ERROR_LOG(RENDERER, "XR: no RGBA8 swapchain format");
		return false;
	}
	u32 viewCount = 0;
	if (!XRCHECK(xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr))
			|| viewCount != 2)
		return false;
	XrViewConfigurationView configViews[2] { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
	if (!XRCHECK(xrEnumerateViewConfigurationViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &viewCount, configViews)))
		return false;
	for (int i = 0; i < 2; i++)
	{
		EyeChain& c = chains[i];
		const float scale = std::clamp((float)config::VrXrResolution, 0.3f, 1.5f);
		c.width = (int)(configViews[i].recommendedImageRectWidth * scale);
		c.height = (int)(configViews[i].recommendedImageRectHeight * scale);
		XrSwapchainCreateInfo info { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		info.format = format;
		info.sampleCount = 1;
		info.width = c.width;
		info.height = c.height;
		info.faceCount = 1;
		info.arraySize = 1;
		info.mipCount = 1;
		if (!XRCHECK(xrCreateSwapchain(session, &info, &c.chain)))
			return false;
		u32 imageCount = 0;
		if (!XRCHECK(xrEnumerateSwapchainImages(c.chain, 0, &imageCount, nullptr)))
			return false;
		c.images.assign(imageCount, { SwapchainImageType });
		if (!XRCHECK(xrEnumerateSwapchainImages(c.chain, imageCount, &imageCount, (XrSwapchainImageBaseHeader *)c.images.data())))
			return false;
		// Depth with stencil: the renderer uses stencil for modifier volumes (shadows).
		glGenRenderbuffers(1, &c.depthStencil);
		glBindRenderbuffer(GL_RENDERBUFFER, c.depthStencil);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, c.width, c.height);
		c.fbos.resize(imageCount);
		glGenFramebuffers(imageCount, c.fbos.data());
		for (u32 j = 0; j < imageCount; j++)
		{
			glBindFramebuffer(GL_FRAMEBUFFER, c.fbos[j]);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c.images[j].image, 0);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, c.depthStencil);
			if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
			{
				ERROR_LOG(RENDERER, "XR: eye framebuffer incomplete");
				return false;
			}
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	}
	NOTICE_LOG(RENDERER, "XR: eye buffers %dx%d", chains[0].width, chains[0].height);
	return true;
}

bool init()
{
	initTried = true;
#ifdef __ANDROID__
	if (g_jvm == nullptr || g_activity == nullptr)
	{
		ERROR_LOG(RENDERER, "XR: no Java VM / activity");
		return false;
	}
	PFN_xrInitializeLoaderKHR initLoader = nullptr;
	if (!XRCHECK(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&initLoader)))
		return false;
	XrLoaderInitInfoAndroidKHR loaderInfo { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
	loaderInfo.applicationVM = g_jvm;
	loaderInfo.applicationContext = g_activity;
	if (!XRCHECK(initLoader((const XrLoaderInitInfoBaseHeaderKHR *)&loaderInfo)))
		return false;
	const char *graphicsExtension = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
	std::vector<const char *> extensions { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, graphicsExtension };
#else
	const char *graphicsExtension = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
	std::vector<const char *> extensions { graphicsExtension };
#endif

	u32 available = 0;
	if (!XRCHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &available, nullptr)))
		return false;	// no OpenXR runtime (PC: no SteamVR, Quest Link or Virtual Desktop running)
	std::vector<XrExtensionProperties> props(available, { XR_TYPE_EXTENSION_PROPERTIES });
	XRCHECK(xrEnumerateInstanceExtensionProperties(nullptr, available, &available, props.data()));
	auto has = [&](const char *name) {
		for (const auto& p : props)
			if (!strcmp(p.extensionName, name))
				return true;
		return false;
	};
	if (!has(graphicsExtension))
	{
		ERROR_LOG(RENDERER, "XR: the OpenXR runtime has no %s", graphicsExtension);
		return false;
	}
	const bool refreshRates = has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	const bool perfSettings = has(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
	if (refreshRates)
		extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	if (perfSettings)
		extensions.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);

	XrInstanceCreateInfo createInfo { XR_TYPE_INSTANCE_CREATE_INFO };
#ifdef __ANDROID__
	XrInstanceCreateInfoAndroidKHR androidInfo { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	androidInfo.applicationVM = g_jvm;
	androidInfo.applicationActivity = g_activity;
	createInfo.next = &androidInfo;
#endif
	createInfo.enabledExtensionCount = (u32)extensions.size();
	createInfo.enabledExtensionNames = extensions.data();
	strcpy(createInfo.applicationInfo.applicationName, "HOTD2 VR");
	createInfo.applicationInfo.applicationVersion = 1;
	strcpy(createInfo.applicationInfo.engineName, "Flycast");
	createInfo.applicationInfo.engineVersion = 1;
	createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
	if (!XRCHECK(xrCreateInstance(&createInfo, &instance)))
		return false;
	if (refreshRates)
		xrGetInstanceProcAddr(instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&requestRefreshRate);
	if (perfSettings)
		xrGetInstanceProcAddr(instance, "xrPerfSettingsSetPerformanceLevelEXT", (PFN_xrVoidFunction *)&setPerformanceLevel);
	XrInstanceProperties instanceProps { XR_TYPE_INSTANCE_PROPERTIES };
	if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &instanceProps)))
		NOTICE_LOG(RENDERER, "XR: runtime %s", instanceProps.runtimeName);

	XrSystemGetInfo systemInfo { XR_TYPE_SYSTEM_GET_INFO };
	systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (!XRCHECK(xrGetSystem(instance, &systemInfo, &systemId)))
		return false;	// (PC: the headset isn't connected)

	// Bind the session to Flycast's own GL context, current on this thread.
#ifdef __ANDROID__
	PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = nullptr;
	if (!XRCHECK(xrGetInstanceProcAddr(instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&getRequirements)))
		return false;
	XrGraphicsRequirementsOpenGLESKHR requirements { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
	if (!XRCHECK(getRequirements(instance, systemId, &requirements)))
		return false;

	EGLDisplay display = eglGetCurrentDisplay();
	EGLContext context = eglGetCurrentContext();
	EGLint configId = 0, configCount = 0;
	EGLConfig eglConfig = nullptr;
	if (context == EGL_NO_CONTEXT || !eglQueryContext(display, context, EGL_CONFIG_ID, &configId))
	{
		ERROR_LOG(RENDERER, "XR: no current EGL context");
		return false;
	}
	const EGLint configAttribs[] { EGL_CONFIG_ID, configId, EGL_NONE };
	if (!eglChooseConfig(display, configAttribs, &eglConfig, 1, &configCount) || configCount == 0)
	{
		ERROR_LOG(RENDERER, "XR: EGL config not found");
		return false;
	}
	XrGraphicsBindingOpenGLESAndroidKHR binding { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
	binding.display = display;
	binding.config = eglConfig;
	binding.context = context;
#else
	PFN_xrGetOpenGLGraphicsRequirementsKHR getRequirements = nullptr;
	if (!XRCHECK(xrGetInstanceProcAddr(instance, "xrGetOpenGLGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&getRequirements)))
		return false;
	XrGraphicsRequirementsOpenGLKHR requirements { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
	if (!XRCHECK(getRequirements(instance, systemId, &requirements)))
		return false;

	XrGraphicsBindingOpenGLWin32KHR binding { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
	binding.hDC = wglGetCurrentDC();
	binding.hGLRC = wglGetCurrentContext();
	if (binding.hDC == nullptr || binding.hGLRC == nullptr)
	{
		ERROR_LOG(RENDERER, "XR: no current OpenGL context (the PC version needs the OpenGL renderer)");
		return false;
	}
	// what goes into the sRGB eye images goes in as it is (see SwapchainFormats)
	glDisable(GL_FRAMEBUFFER_SRGB);
#endif
	XrSessionCreateInfo sessionInfo { XR_TYPE_SESSION_CREATE_INFO };
	sessionInfo.next = &binding;
	sessionInfo.systemId = systemId;
	if (!XRCHECK(xrCreateSession(instance, &sessionInfo, &session)))
		return false;

	XrReferenceSpaceCreateInfo spaceInfo { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	spaceInfo.poseInReferenceSpace.orientation.w = 1.f;
	if (!XRCHECK(xrCreateReferenceSpace(session, &spaceInfo, &localSpace)))
		return false;
	if (!initActions() || !initSwapchains())
		return false;
	NOTICE_LOG(RENDERER, "XR: session created");
	return true;
}

void pollEvents()
{
	XrEventDataBuffer event { XR_TYPE_EVENT_DATA_BUFFER };
	while (xrPollEvent(instance, &event) == XR_SUCCESS)
	{
		if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
		{
			const auto& change = *reinterpret_cast<XrEventDataSessionStateChanged *>(&event);
			NOTICE_LOG(RENDERER, "XR: session state %d", change.state);
			if (change.state == XR_SESSION_STATE_READY)
			{
				XrSessionBeginInfo begin { XR_TYPE_SESSION_BEGIN_INFO };
				begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				if (XRCHECK(xrBeginSession(session, &begin)))
				{
					sessionRunning = true;
					if (requestRefreshRate)
						XRCHECK(requestRefreshRate(session, (float)config::VrXrRefreshRate));
					if (setPerformanceLevel)
					{
						XRCHECK(setPerformanceLevel(session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT));
						XRCHECK(setPerformanceLevel(session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT));
					}
				}
			}
			else if (change.state == XR_SESSION_STATE_STOPPING)
			{
				xrEndSession(session);
				sessionRunning = false;
			}
			else if (change.state == XR_SESSION_STATE_LOSS_PENDING)
				sessionLost = true;
			else if (change.state == XR_SESSION_STATE_EXITING)
				exiting = true;
			const bool nowFocused = change.state == XR_SESSION_STATE_FOCUSED;
			if (nowFocused && !focused && originSet
					&& std::chrono::steady_clock::now() - focusLost > std::chrono::seconds(3))
				// back after a while (headset put on again, long menu visit): the player
				// may face elsewhere now
				recenterRequested = true;
			if (focused && !nowFocused)
				focusLost = std::chrono::steady_clock::now();
			focused = nowFocused;
		}
		else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
		{
			sessionLost = true;
		}
		else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING)
		{
			recenterRequested = true;
		}
		event = { XR_TYPE_EVENT_DATA_BUFFER };
	}
}

glm::vec3 toVec(const XrVector3f& v) { return { v.x, v.y, v.z }; }
glm::quat toQuat(const XrQuaternionf& q) { return glm::quat(q.w, q.x, q.y, q.z); }

glm::quat yawOnly(const glm::quat& q)
{
	const glm::vec3 forward = q * glm::vec3(0, 0, -1);
	return glm::angleAxis(std::atan2(-forward.x, -forward.z), glm::vec3(0, 1, 0));
}

// Pose relative to the game camera's place in the room.
glm::vec3 relativePosition(const XrVector3f& p) {
	return glm::conjugate(originRot) * (toVec(p) - originPos);
}
glm::quat relativeRotation(const XrQuaternionf& q) {
	return glm::conjugate(originRot) * toQuat(q);
}

glm::mat4 eyeProjection(const XrFovf& fov)
{
	const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
	const float u = std::tan(fov.angleUp), d = std::tan(fov.angleDown);
	glm::mat4 p(0.f);
	p[0][0] = 2.f / (r - l);
	p[1][1] = 2.f / (u - d);
	p[2][0] = (r + l) / (r - l);
	p[2][1] = (u + d) / (u - d);
	p[2][2] = -1.f;
	p[2][3] = -1.f;
	p[3][2] = -2.f * NearMetres;	// infinite far plane; depth comes from the game's own W
	return p;
}

// Rebuilt game eye space -> headset local space, relative to the origin.
glm::mat4 gameToWorld()
{
	const float s = worldScale();
	return glm::scale(glm::mat4(1.f), glm::vec3(s, -s, s));
}

void updateOrigin(bool tracked)
{
	if (!tracked || (originSet && !recenterRequested))
		return;
	const glm::vec3 center = (toVec(views[0].pose.position) + toVec(views[1].pose.position)) * 0.5f;
	originPos = center;
	originRot = yawOnly(toQuat(views[0].pose.orientation));
	originSet = true;
	recenterRequested = false;
	NOTICE_LOG(RENDERER, "XR: game camera placed at the head");
}

// Möller-Trumbore; returns the distance along the ray, or a negative value for a miss.
float rayTriangle(const glm::vec3& o, const glm::vec3& d, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
{
	const glm::vec3 e1 = b - a, e2 = c - a;
	const glm::vec3 p = glm::cross(d, e2);
	const float det = glm::dot(e1, p);
	if (std::abs(det) < 1e-9f)
		return -1.f;
	const float inv = 1.f / det;
	const glm::vec3 t = o - a;
	const float u = glm::dot(t, p) * inv;
	if (u < 0.f || u > 1.f)
		return -1.f;
	const glm::vec3 q = glm::cross(t, e1);
	const float v = glm::dot(d, q) * inv;
	if (v < 0.f || u + v > 1.f)
		return -1.f;
	return glm::dot(e2, q) * inv;
}

// Closer than this (game units) is the game's clamped near plane, not real geometry.
constexpr float MinSceneW = 0.5f;

// How the last aim was placed, for the shot log.
const char *aimHow = "";
// The last aim is on something the player sees but the game can't hit: outside its stock
// view (see aimAtScene). The dot greys and the trigger does nothing there.
bool aimOutside;
// ...and for 3D aims, the triangle that was hit (diagnostics: which game geometry the ray
// meets, at what original W).
struct AimHit
{
	const char *list = "";
	u32 poly = 0;
	float w[3] {};
	glm::vec2 xy[3] {};
	u32 tex = 0, tsp = 0, isp = 0;
	u8 col[4] {};
} aimHit;

bool onGameScreen(const glm::vec2& screen) {
	return screen.x >= 0.f && screen.x <= 1.f && screen.y >= 0.f && screen.y <= 1.f;
}

// Where the ray (game eye space) meets the plane the 2D overlay and flat screens are
// shown on, at their stock framing (see the VR_REPROJECT vertex shader and drawVrScreen):
// the position in screen ndc (-1..1, y down) and the distance along the ray.
bool hitOverlayPlane(const glm::vec3& o, const glm::vec3& d, const glm::vec2& dcSize, glm::vec2& ndc, float& dist)
{
	const float depth = config::VrHudDepth;
	if (d.z > -1e-3f)
		return false;
	dist = (-depth - o.z) / d.z;
	if (dist <= 0.f)
		return false;
	const glm::vec3 hit = o + d * dist;
	const glm::vec2 halfSize = dcSize * 0.5f / (float)config::VrFocal * depth;
	ndc = glm::vec2(hit.x, hit.y) / halfSize;
	return true;
}

bool inTriangle(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b, const glm::vec2& c)
{
	auto side = [](const glm::vec2& p, const glm::vec2& a, const glm::vec2& b) {
		return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
	};
	const float s1 = side(p, a, b), s2 = side(p, b, c), s3 = side(p, c, a);
	return (s1 >= 0.f && s2 >= 0.f && s3 >= 0.f) || (s1 <= 0.f && s2 <= 0.f && s3 <= 0.f);
}

// Where on the game screen (0..1) the gun points, as the game's own hit test will see it,
// and how far along the ray the aim point is (game units).
//  - 2D overlay (menu items, text) is on its own plane at the stock framing: when the ray
//    passes through a piece of it, the shot goes there. So do all shots on a screen with
//    nothing but overlay (menus).
//  - Otherwise the nearest 3D surface, placed on the screen at the game's stock framing,
//    which its hit test uses even when its view is widened; in the wider band around
//    it: aimOutside.
//  - Nothing hit: the point far along the ray.
bool aimAtScene(const rend_context& ctx, const glm::vec3& o, const glm::vec3& d, glm::vec2& screen, float& dist)
{
	aimOutside = false;
	const GameCamera cam = gameCamera(ctx);
	const glm::vec3 comfort = comfortParams();
	static std::vector<glm::vec3> pos;
	static std::vector<glm::vec2> overlayNdc;
	enum Kind : u8 { Unused, Scene, Overlay };
	static std::vector<Kind> kind;
	pos.resize(ctx.verts.size());
	overlayNdc.resize(ctx.verts.size());
	kind.assign(ctx.verts.size(), Unused);
	for (size_t i = 0; i < ctx.verts.size(); i++)
	{
		const Vertex& v = ctx.verts[i];
		if (!(v.z > 0.f) || !std::isfinite(v.z))
			continue;
		const float w = 1.f / v.z;
		const glm::vec2 ndc(v.x * 2.f / cam.dcSize.x - 1.f, v.y * 2.f / cam.dcSize.y - 1.f);
		if (std::abs(w - cam.overlayW) < 0.002f)
		{
			// (hidden letterbox bars have NaN x/y and are in no triangle)
			overlayNdc[i] = ndc;
			kind[i] = Overlay;
		}
		else if (w >= MinSceneW)
		{
			// where it is seen: the comfort zone pulls close things back (along the line of
			// sight from the game camera, so the screen position stays the same)
			const glm::vec3 p(ndc.x * w * cam.tanHalf.x, ndc.y * w * cam.tanHalf.y, -w);
			pos[i] = p * comfortScale(p, comfort);
			kind[i] = Scene;
		}
		// else: a vertex the game clamped to its near plane (W 0.001, huge x/y). Its
		// triangles turn into long invisible slivers across the view that would catch the
		// aim; the visible part of such a polygon is hit through its other triangles.
	}

	glm::vec2 planeNdc;
	float planeDist = 0.f;
	const bool onPlane = hitOverlayPlane(o, d, cam.dcSize, planeNdc, planeDist);
	bool overlayHit = false;
	bool hasScene = false;
	float nearest = 1e30f;
	const char *listName = "";
	u32 polyIndex = 0;
	const PolyParam *poly = nullptr;
	auto triangle = [&](u32 a, u32 b, u32 c, bool solid, bool background) {
		if (a == RestartIndex || b == RestartIndex || c == RestartIndex
				|| a >= kind.size() || b >= kind.size() || c >= kind.size())
			return;
		if (kind[a] == Overlay && kind[b] == Overlay && kind[c] == Overlay)
		{
			// skip full-screen fades and backdrops: only buttons and text count
			const glm::vec2 e1 = overlayNdc[b] - overlayNdc[a], e2 = overlayNdc[c] - overlayNdc[a];
			if (onPlane && !overlayHit && std::abs(e1.x * e2.y - e1.y * e2.x) < 1.f
					&& inTriangle(planeNdc, overlayNdc[a], overlayNdc[b], overlayNdc[c]))
				overlayHit = true;
		}
		else if (solid && kind[a] == Scene && kind[b] == Scene && kind[c] == Scene)
		{
			if (!background)
				hasScene = true;
			const float t = rayTriangle(o, d, pos[a], pos[b], pos[c]);
			if (t > 0.f && t < nearest)
			{
				nearest = t;
				aimHit.list = listName;
				aimHit.poly = polyIndex;
				const u32 abc[3] { a, b, c };
				for (int k = 0; k < 3; k++)
				{
					const Vertex& v = ctx.verts[abc[k]];
					aimHit.w[k] = 1.f / v.z;
					aimHit.xy[k] = glm::vec2(v.x, v.y);
				}
				if (poly != nullptr)
				{
					aimHit.tex = poly->tcw.TexAddr << 3;
					aimHit.tsp = poly->tsp.full;
					aimHit.isp = poly->isp.full;
				}
				memcpy(aimHit.col, ctx.verts[a].col, 4);
			}
		}
	};
	// strips: first/count are ranges of the index buffer
	auto strips = [&](const std::vector<PolyParam>& list, size_t from, size_t to, bool solid, bool opaque) {
		listName = &list == &ctx.global_param_op ? "OP" : &list == &ctx.global_param_pt ? "PT" : "TR";
		for (size_t n = from; n < to && n < list.size(); n++)
		{
			const PolyParam& pp = list[n];
			polyIndex = (u32)n;
			poly = &pp;
			for (u32 j = pp.first; j + 2 < pp.first + pp.count && j + 2 < ctx.idx.size(); j++)
				// the first opaque polygon is the background plane
				triangle(ctx.idx[j], ctx.idx[j + 1], ctx.idx[j + 2], solid, opaque && n == 0);
		}
	};
	strips(ctx.global_param_op, 0, ctx.global_param_op.size(), true, true);
	strips(ctx.global_param_pt, 0, ctx.global_param_pt.size(), true, false);
	// Translucent polygons (most 2D text) of auto-sorted passes were turned into a list of
	// sorted triangles; their own first/count still point at vertices, not indices.
	RenderPass prev {};
	for (const RenderPass& pass : ctx.render_passes)
	{
		if (pass.sorted_tr_count != prev.sorted_tr_count)
		{
			for (u32 s = prev.sorted_tr_count; s < pass.sorted_tr_count && s < ctx.sortedTriangles.size(); s++)
			{
				const SortedTriangle& st = ctx.sortedTriangles[s];
				for (u32 j = st.first; j + 2 < st.first + st.count && j + 2 < ctx.idx.size(); j += 3)
					triangle(ctx.idx[j], ctx.idx[j + 1], ctx.idx[j + 2], false, false);
			}
		}
		else
			strips(ctx.global_param_tr, prev.tr_count, pass.tr_count, false, false);
		prev = pass;
	}

	if (onPlane && (overlayHit || !hasScene))
	{
		screen = planeNdc * 0.5f + 0.5f;
		dist = planeDist;
		aimHow = overlayHit ? "2D" : "menu";
		return onGameScreen(screen);
	}
	aimHow = nearest < 1e30f ? "3D" : "far";
	constexpr float Far = 10000.f;
	dist = nearest < Far ? nearest : Far;
	const glm::vec3 hit = o + d * dist;
	if (hit.z > -1e-3f)
		return false;
	// The game tests gun hits with its stock view, not the widened one it draws with. With
	// viewport widening that showed as shots drifting towards the centre, and it holds with
	// the game's own field of view widened too (vr.WidenFov): on the headset, a shot sent
	// at x 410 (of 640) had the game's hit effect ~8 degrees nearer the centre than the dot,
	// exactly where the stock framing puts it. So the hit point goes on the screen at the
	// stock framing: the shot lands where the dot is. The wider view around it is seen but
	// can't be hit (as in the original game, where it wasn't on screen): aimOutside.
	const glm::vec2 tan = config::VrAimWidened ? cam.tanHalf : cam.stockTan;
	screen = glm::vec2(hit.x / (-hit.z * tan.x), hit.y / (-hit.z * tan.y)) * 0.5f + 0.5f;
	if (onGameScreen(screen))
		return true;
	const glm::vec2 seen = glm::vec2(hit.x / (-hit.z * cam.tanHalf.x), hit.y / (-hit.z * cam.tanHalf.y)) * 0.5f + 0.5f;
	aimOutside = onGameScreen(seen);
	return false;
}

// The flat screen menus and notices are shown on (see drawVrScreen in gles.cpp).
bool aimAtFlatScreen(const glm::vec3& o, const glm::vec3& d, glm::vec2& screen, float& dist)
{
	aimOutside = false;
	glm::vec2 ndc;
	if (!hitOverlayPlane(o, d, glm::vec2(640.f, 480.f), ndc, dist))
		return false;
	screen = ndc * 0.5f + 0.5f;
	aimHow = "screen";
	return onGameScreen(screen);
}

// A button, of either hand or (for per-hand actions) of one
bool boolAction(XrAction action, int hand = -1)
{
	XrActionStateGetInfo info { XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = action;
	if (hand >= 0)
		info.subactionPath = handPaths[hand];
	XrActionStateBoolean state { XR_TYPE_ACTION_STATE_BOOLEAN };
	return XR_SUCCEEDED(xrGetActionStateBoolean(session, &info, &state)) && state.isActive && state.currentState;
}

float floatAction(XrAction action, int hand)
{
	XrActionStateGetInfo info { XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = action;
	info.subactionPath = handPaths[hand];
	XrActionStateFloat state { XR_TYPE_ACTION_STATE_FLOAT };
	return XR_SUCCEEDED(xrGetActionStateFloat(session, &info, &state)) && state.isActive ? state.currentState : 0.f;
}

void haptic(int hand, float seconds, float amplitude)
{
	XrHapticActionInfo info { XR_TYPE_HAPTIC_ACTION_INFO };
	info.action = hapticAction;
	info.subactionPath = handPaths[hand];
	XrHapticVibration vibration { XR_TYPE_HAPTIC_VIBRATION };
	vibration.duration = (XrDuration)(seconds * 1e9f);
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
	vibration.amplitude = amplitude;
	xrApplyHapticFeedback(session, &info, (const XrHapticBaseHeader *)&vibration);
}

void recoil(int hand)
{
	haptic(hand, 0.03f, 1.f);	// a sharp knock
}

glm::vec2 stickAction2()
{
	XrActionStateGetInfo info { XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = stickAction;
	XrActionStateVector2f stick { XR_TYPE_ACTION_STATE_VECTOR2F };
	if (!XR_SUCCEEDED(xrGetActionStateVector2f(session, &info, &stick)) || !stick.isActive)
		return glm::vec2(0.f);
	return glm::vec2(stick.currentState.x, stick.currentState.y);
}

// The light gun's D-pad from a thumbstick, with some hysteresis: pushed past 60%,
// let go below 40%. Returns the lightgunSet bits (up 8, down 16, left 32, right 64).
u32 dpadFromStick(const glm::vec2& stick)
{
	static u32 held;
	auto axis = [&](float v, u32 negBit, u32 posBit) {
		const u32 was = held & (negBit | posBit);
		if (was == posBit ? v > 0.4f : v > 0.6f)
			return posBit;
		if (was == negBit ? v < -0.4f : v < -0.6f)
			return negBit;
		return 0u;
	};
	held = axis(stick.y, 16, 8) | axis(stick.x, 32, 64);
	return held;
}

// With a grip held, thumbstick up makes the world bigger around the player, down smaller
// (about 1.8x per second held), and right makes the gun bigger, left smaller (about 1.6x
// per second). Both are kept in emu.cfg when the stick is let go.
// Returns true while gripping (the stick then isn't the D-pad).
bool adjustWorldSize(XrTime time, const glm::vec2& stick)
{
	static XrTime lastTime;
	static bool worldChanged, gunChanged;
	const float dt = lastTime != 0 ? std::clamp((time - lastTime) * 1e-9f, 0.f, 0.1f) : 0.f;
	lastTime = time;
	XrActionStateGetInfo info { XR_TYPE_ACTION_STATE_GET_INFO };
	info.action = gripAction;
	XrActionStateFloat grip { XR_TYPE_ACTION_STATE_FLOAT };
	const bool gripped = XR_SUCCEEDED(xrGetActionStateFloat(session, &info, &grip)) && grip.isActive && grip.currentState > 0.7f;
	const glm::vec2 s = gripped ? stick : glm::vec2(0.f);
	// the stronger direction only, so a diagonal doesn't change both
	const bool vertical = std::abs(s.y) >= std::abs(s.x);
	if (vertical && std::abs(s.y) > 0.5f)
	{
		const float scale = std::clamp(config::VrWorldScale * std::exp((s.y > 0.f ? 0.6f : -0.6f) * dt), 0.005f, 0.2f);
		config::VrWorldScale.set(scale);
		worldChanged = true;
	}
	else if (worldChanged)
	{
		worldChanged = false;
		config::saveFloat("config", "vr.WorldScale", config::VrWorldScale);
		NOTICE_LOG(RENDERER, "XR: world size %.4f m per game unit", (float)config::VrWorldScale);
	}
	// the size of whichever pistol is drawn
	const bool game = gameGun();
	config::Option<float>& gunSize = game ? config::VrHandScale : config::VrGunScale;
	const char *gunKey = game ? "vr.HandScale" : "vr.GunScale";
	if (!vertical && std::abs(s.x) > 0.5f)
	{
		gunSize.set(std::clamp(gunSize * std::exp((s.x > 0.f ? 0.5f : -0.5f) * dt), 0.4f, 1.6f));
		gunChanged = true;
	}
	else if (gunChanged)
	{
		gunChanged = false;
		config::saveFloat("config", gunKey, gunSize);
		NOTICE_LOG(RENDERER, "XR: gun size %.2f (%s)", (float)gunSize, gunKey);
	}
	return gripped;
}

// Off the game screen, as a light gun sees it (any negative position).
constexpr int OffScreen = -10000;

// The other hand, with the game's pistol (xr_hands.h): the open hand follows the other
// controller. Its grip squeezed near the back of the slide takes the slide, and pulling it
// back all the way reloads, as the game knows it: a shot off the screen. Let go, the slide
// springs home. (The hero's staff has no slide: the other glove only follows the hand.)
struct Slide
{
	bool held;
	bool racked;		// this pull reloaded already
	bool squeezeWas;
	bool clack;			// pulled well back: it hits home with a knock
	float from;			// the hand along the barrel when it took the slide (model metres)
	float back;			// how far back the slide is (model metres)
	XrTime time;
} slide;
// How far the hand (the controller's grip) may be from the back of the slide to take it:
// the controllers can't overlap, and the pistol's back sits over the gun hand's ring.
constexpr float GrabReach = 0.11f;

void updateOtherHand(XrTime time, int hand, bool tracked, const XrSpaceLocation& location,
		const glm::mat4& restGun, float scale, bool mirror)
{
	const HandsModel *model = handsModel();
	const float dt = slide.time != 0 ? std::clamp((time - slide.time) * 1e-9f, 0.f, 0.1f) : 0.f;
	slide.time = time;
	GunView& gunView = guns[0].view;
	gunView.otherHand = false;
	if (!tracked || model == nullptr)
		slide.held = false;
	else
	{
		const glm::quat rot = relativeRotation(location.pose.orientation);
		const glm::mat4 aim = glm::translate(glm::mat4(1.f), relativePosition(location.pose.position)) * glm::mat4_cast(rot);
		glm::vec3 palm = HandPalm;
		XrSpaceLocation grip { XR_TYPE_SPACE_LOCATION };
		if (XR_SUCCEEDED(xrLocateSpace(gripSpaces[hand], aimSpaces[hand], time, &grip))
				&& (grip.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
				&& glm::length(toVec(grip.pose.position)) < 0.25f)
			palm = toVec(grip.pose.position);
		const glm::vec3 handAt = glm::vec3(aim * glm::vec4(palm, 1.f));
		// the hand in the pistol's model space (without recoil)
		const glm::vec3 inGun = glm::vec3(glm::inverse(restGun) * glm::vec4(handAt, 1.f));
		const float squeeze = floatAction(gripAction, hand);
		const bool squeezed = slide.squeezeWas ? squeeze > 0.35f : squeeze > 0.6f;
		if (config::VrSlideReload && model->travel > 0.f && squeezed && !slide.squeezeWas && !slide.held)
		{
			const glm::vec3 grabAt = glm::vec3(restGun * glm::vec4(model->grab + glm::vec3(0.f, 0.f, slide.back), 1.f));
			const float reach = glm::distance(grabAt, handAt);
			if (reach < GrabReach)
			{
				slide.held = true;
				slide.racked = false;
				slide.from = inGun.z - slide.back;
				haptic(hand, 0.012f, 0.35f);
			}
			NOTICE_LOG(INPUT, "XR: grip %d cm from the slide%s", (int)(reach * 100.f), slide.held ? ": took it" : "");
		}
		slide.squeezeWas = squeezed;
		if (slide.held && !squeezed)
		{
			slide.held = false;
			slide.clack = slide.back > model->travel * 0.5f;
		}
		if (slide.held)
		{
			slide.back = std::clamp(inGun.z - slide.from, 0.f, model->travel);
			if (!slide.racked && slide.back > model->travel * 0.85f)
			{
				slide.racked = true;
				guns[0].reloadAt = time;
				haptic(hand, 0.025f, 0.8f);
				haptic(guns[0].hand, 0.025f, 0.6f);
				NOTICE_LOG(INPUT, "XR: slide racked (reload)");
			}
		}
		gunView.otherHand = true;
		gunView.handPose = slide.held
				// on the slide, going along with it
				? gunView.pose * glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, slide.back)) * model->onSlide
				// the model is a left hand: mirrored for the right
				: aim * glm::translate(glm::mat4(1.f), palm) * glm::scale(glm::mat4(1.f), glm::vec3(mirror ? -scale : scale, scale, scale));
	}
	if (!slide.held && slide.back > 0.f)
	{
		// home it goes, fast
		slide.back = std::max(0.f, slide.back - dt * 1.5f);
		if (slide.back == 0.f && slide.clack)
			haptic(guns[0].hand, 0.02f, 0.7f);
	}
	if (slide.back == 0.f)
		slide.clack = false;
	gunView.slide = slide.back;
}

// One gun for a headset frame: where it points (handed to the game's hit test at its stock
// framing), how it's drawn, and the buttons that go to its player's light gun. `extra` are
// the buttons that aren't the trigger's (Start, B, the D-pad). With `slide`, the other hand
// (`other`) is free to rack the game pistol's slide.
void updateGun(Gun& g, XrTime time, const XrSpaceLocation *locations, const bool *tracked, const float *pulls,
		u32 extra, bool slide)
{
	// A clean click: pulled past 60%, let go below 35%, so a resting finger or a half
	// release doesn't fire twice.
	const float pull = pulls[g.hand];
	const bool trigger = g.triggerWas ? pull > 0.35f : pull > 0.6f;

	glm::vec2 screen(-1.f);
	bool onScreen = false;
	aimOutside = false;
	const XrSpaceLocation& location = locations[g.hand];
	if (originSet && tracked[g.hand])
	{
		const glm::quat rot = relativeRotation(location.pose.orientation);
		const glm::mat4 pose = glm::translate(glm::mat4(1.f), relativePosition(location.pose.position)) * glm::mat4_cast(rot);
		// The drawn gun: the middle of its grip where the hand is (the controller's grip
		// pose, seen from the aim pose), at vr.GunScale times the model's size.
		glm::vec3 palm = HandPalm;
		XrSpaceLocation grip { XR_TYPE_SPACE_LOCATION };
		const bool fromRuntime = XR_SUCCEEDED(xrLocateSpace(gripSpaces[g.hand], aimSpaces[g.hand], time, &grip))
				&& (grip.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
				&& glm::length(toVec(grip.pose.position)) < 0.25f;
		if (fromRuntime)
			palm = toVec(grip.pose.position);
		if (g.palmLogged != g.hand * 2 + fromRuntime)
		{
			g.palmLogged = g.hand * 2 + fromRuntime;
			NOTICE_LOG(INPUT, "XR: %s hand at %.3f %.3f %.3f from the aim pose (%s)", g.hand == Right ? "right" : "left",
					palm.x, palm.y, palm.z, fromRuntime ? "runtime" : "fallback");
		}
		const bool game = gameGun();
		const float gunScale = std::clamp(game ? (float)config::VrHandScale : (float)config::VrGunScale, 0.4f, 1.6f);
		// the game's pistol is in a right hand: mirrored for the left
		const bool mirror = game && g.hand == Left;
		const glm::mat4 placement = gunPlacement(game, gunScale, palm, mirror);
		// Shots leave the muzzle along the barrel, so the aim line and the hit agree. Into
		// rebuilt game eye space for the hit test.
		const glm::vec3 o = glm::vec3(pose * placement * glm::vec4(gunMuzzle(), 1.f));
		const glm::vec3 d = rot * glm::vec3(0, 0, -1);
		const float s = worldScale();
		const glm::vec3 og = glm::vec3(o.x, -o.y, o.z) / s;
		const glm::vec3 dg = glm::normalize(glm::vec3(d.x, -d.y, d.z));
		float dist = -1.f;
		const rend_context *ctx = gles_vr_frame();
		if (gles_vr_showing_framebuffer())
			onScreen = aimAtFlatScreen(og, dg, screen, dist);
		else if (ctx != nullptr)
			onScreen = aimAtScene(*ctx, og, dg, screen, dist);

		// Recoil: back and muzzle up around the hand, with a small bounce (gunKick) and a
		// little sideways twist that differs per shot. A shot fired now shows its flash
		// and kick in this very frame.
		if (trigger && !g.triggerWas && onScreen)
		{
			g.lastShot = time;
			g.shotCount++;
		}
		const float sinceShot = g.lastShot != 0 ? (time - g.lastShot) * 1e-9f : 1e9f;
		const float kick = gunKick(sinceShot);
		const float twist = ((g.shotCount * 2654435761u) >> 24) / 255.f - 0.5f;	// -0.5..0.5 per shot
		const glm::vec3 hand = palm;
		// A long one (the hero's staff, its head 75 cm out) tips up less: its muzzle rises no
		// more than one 40 cm from the hand would. Every pistol here is shorter.
		const float reach = glm::distance(glm::vec3(placement * glm::vec4(gunMuzzle(), 1.f)), hand);
		const float tip = std::min(1.f, 0.4f / std::max(reach, 0.01f));
		glm::mat4 recoilMat = glm::translate(glm::mat4(1.f), glm::vec3(0.f, 0.f, 0.035f * kick) + hand);
		recoilMat = glm::rotate(recoilMat, glm::radians(18.f * tip * kick), glm::vec3(1, 0, 0));
		recoilMat = glm::rotate(recoilMat, glm::radians(6.f * tip * twist * std::max(kick, 0.f)), glm::vec3(0, 1, 0));
		recoilMat = glm::translate(recoilMat, -hand);
		GunView& view = g.view;
		view.pose = pose * recoilMat * placement;
		// (the game's pistol has a smaller muzzle than the arcade gun's lens)
		view.scale = game ? gunScale * 0.8f : gunScale;
		view.restMuzzle = o;
		view.restForward = d;
		view.trigger = std::clamp(pull, 0.f, 1.f);
		view.now = time * 1e-9;
		view.sinceShot = sinceShot;
		view.shot = g.shotCount;
		view.player = g.player;
		view.aimLine = config::VrLaser;
		view.aimDot = config::VrLaser && (onScreen || aimOutside);
		view.aimOutside = aimOutside && !onScreen;
		view.aimPoint = o + d * (dist > 0.f ? dist * s : 5.f);
		g.visible = config::VrShowGun;
		if (game && slide)
			updateOtherHand(time, 1 - g.hand, tracked[1 - g.hand], locations[1 - g.hand], pose * placement, gunScale, mirror);
		else
		{
			view.otherHand = false;
			view.slide = 0.f;
		}
		// A gun in each hand leaves no hand for the slide: pointing a gun at the floor
		// reloads it instead.
		if (!slide)
		{
			if (!g.pointedDown && d.y < -0.75f)
			{
				g.pointedDown = true;
				g.reloadAt = time;
				haptic(g.hand, 0.02f, 0.5f);
				NOTICE_LOG(INPUT, "XR: player %d pointed down (reload)", g.player + 1);
			}
			else if (g.pointedDown && d.y > -0.5f)
				g.pointedDown = false;
		}
	}

	// Seen but outside the game's view: the trigger does nothing (no shot, no reload).
	const bool outside = !onScreen && aimOutside;
	// What a pull does is decided when it starts. One started in the grey margin never
	// fires. One started on screen (a shot) or off it (a reload) keeps the game's A held
	// without a break until it is let go, also while crossing the margin: the reload bit is
	// A held too (maple_lightgun::transform_kcode), so no new press reaches the game and a
	// held trigger never fires again by sweeping.
	if (trigger && !g.triggerWas)
		g.pullOutside = outside;
	u32 buttons = 0;
	if (trigger && !g.pullOutside)
		// pointing away from the screen fires off-screen: that's how you reload
		buttons |= onScreen ? 1 : 2;
	// A shot at the 2D plane (menus, text, a flat screen): the game's own shot marker lands
	// right where it hit, so it stays (vr::dropShotMarker).
	if (trigger && onScreen && (!strcmp(aimHow, "2D") || !strcmp(aimHow, "menu") || !strcmp(aimHow, "screen")))
		buttons |= 256;
	buttons |= extra;
	// A reload (the slide racked, the gun pointed down): the trigger let go for a moment,
	// then pulled off the screen. A trigger held through it waits to be let go, so it
	// doesn't fire right after.
	bool reloading = false;
	if (g.reloadAt != 0)
	{
		const float since = (time - g.reloadAt) * 1e-9f;
		if (since < 0.15f)
		{
			buttons &= ~3u;
			if (since >= 0.05f)
			{
				buttons |= 2;
				reloading = true;
			}
			g.triggerWait = g.triggerWait || trigger;
		}
		else
			g.reloadAt = 0;
	}
	if (g.triggerWait && !trigger)
		g.triggerWait = false;
	if (g.triggerWait && g.reloadAt == 0)
		buttons &= ~3u;
	// Trigger + B + Start is the game's own reset. A and B sit under one thumb: not by accident.
	if ((buttons & 128) && (buttons & 3))
		buttons &= ~4u;
	if (trigger && !g.triggerWas && outside)
		NOTICE_LOG(INPUT, "XR: player %d's trigger outside the game's view (no shot)", g.player + 1);
	else if (trigger && !g.triggerWas)
	{
		recoil(g.hand);
		lastShotScreen = onScreen ? glm::vec2(screen.x * 640.f, screen.y * 480.f) : glm::vec2(-1.f);
		shotSerial++;
		// diagnostics: where shots go, to compare with what the game makes of them
		if (onScreen && !strcmp(aimHow, "3D"))
			NOTICE_LOG(INPUT, "XR: player %d shot at %d,%d (3D, %d cm) hit %s %u W %.1f/%.1f/%.1f at %.0f,%.0f %.0f,%.0f %.0f,%.0f tex %06x tsp %08x isp %08x col %02x%02x%02x%02x",
					g.player + 1, (int)(screen.x * 640.f), (int)(screen.y * 480.f), (int)(glm::length(g.view.aimPoint - g.view.restMuzzle) * 100.f),
					aimHit.list, aimHit.poly, aimHit.w[0], aimHit.w[1], aimHit.w[2], aimHit.xy[0].x, aimHit.xy[0].y,
					aimHit.xy[1].x, aimHit.xy[1].y, aimHit.xy[2].x, aimHit.xy[2].y, aimHit.tex, aimHit.tsp, aimHit.isp,
					aimHit.col[0], aimHit.col[1], aimHit.col[2], aimHit.col[3]);
		else if (onScreen)
			NOTICE_LOG(INPUT, "XR: player %d shot at %d,%d (%s, %d cm)", g.player + 1, (int)(screen.x * 640.f), (int)(screen.y * 480.f), aimHow,
					(int)(glm::length(g.view.aimPoint - g.view.restMuzzle) * 100.f));
		else
			NOTICE_LOG(INPUT, "XR: player %d shot off screen (reload)", g.player + 1);
	}
	g.triggerWas = trigger;
	const glm::ivec2 pos = onScreen && !reloading ? glm::ivec2(screen * 10000.f) : glm::ivec2(OffScreen);
	lightgunSet(g.player, pos.x, pos.y, buttons);
}

void updateLightgun(XrTime time)
{
	for (Gun& g : guns)
		g.visible = false;
	if (!config::VrXrGun)
		return;
	if (newGame.exchange(false))
	{
		if (dualWield)
			lightgunSet(1, OffScreen, OffScreen, 0);
		dualWield = false;
		guns[1] = Gun { 1, Left };
		guns[0].triggerWas = true;
		slide.held = false;
		leftStartWas = true;	// a left Start still held doesn't join at once
	}
	// The game runs itself to its game over scene for the agent's hands, or its attract demo
	// for the hero's staff (hands_rip.h): its buttons are its own then.
	const bool preparing = hands::preparing();
	if (!focused)
	{
		// headset menu or app in the background: let go of everything
		if (!preparing)
			for (const Gun& g : guns)
				lightgunSet(g.player, OffScreen, OffScreen, 0);
		for (Gun& g : guns)
			g.triggerWas = false;
		return;
	}
	const XrActiveActionSet active { actionSet, XR_NULL_PATH };
	XrActionsSyncInfo sync { XR_TYPE_ACTIONS_SYNC_INFO };
	sync.countActiveActionSets = 1;
	sync.activeActionSets = &active;
	if (!XR_SUCCEEDED(xrSyncActions(session, &sync)))
		return;
	if (preparing)
	{
		// B (or Y): not now, the arcade gun this time
		if (boolAction(skipAction))
			hands::skipPrep();
		for (Gun& g : guns)
		{
			// a trigger held through it doesn't fire when it's over: it waits to be let go
			g.triggerWas = true;
			g.triggerWait = true;
		}
		return;
	}

	const bool recenter = boolAction(recenterAction);
	if (recenter && !recenterWas)
		recenterRequested = true;
	recenterWas = recenter;
	const glm::vec2 stick = stickAction2();
	const u32 dpad = adjustWorldSize(time, stick) ? dpadFromStick(glm::vec2(0.f)) : dpadFromStick(stick);

	const XrSpaceLocationFlags valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	XrSpaceLocation locations[2] { { XR_TYPE_SPACE_LOCATION }, { XR_TYPE_SPACE_LOCATION } };
	bool tracked[2];
	float pulls[2];
	for (int hand : { Left, Right })
	{
		tracked[hand] = XR_SUCCEEDED(xrLocateSpace(aimSpaces[hand], localSpace, time, &locations[hand]))
				&& (locations[hand].locationFlags & valid) == valid;
		pulls[hand] = tracked[hand] ? floatAction(triggerAction, hand) : 0.f;
	}
	const bool leftStart = boolAction(startAction, Left);
	const bool rightStart = boolAction(startAction, Right);
	// Joining takes both controllers (with one, the left Start stays player 1's) and a game
	// that shows when player 2 is out again (its profile knows the prompt).
	const bool canJoin = config::VrDualWield && player2PromptKnown() && tracked[Left] && tracked[Right];
	if (canJoin && leftStart && !leftStartWas && !dualWield)
	{
		dualWield = true;
		dualSince = time;
		guns[0].hand = Right;
		guns[1] = Gun { 1, Left };
		// a trigger already held doesn't fire: it waits to be let go
		guns[1].triggerWas = true;
		guns[1].triggerWait = true;
		slide.held = false;
		haptic(Left, 0.05f, 0.6f);
		NOTICE_LOG(INPUT, "XR: player 2 joins: a gun in each hand");
	}
	leftStartWas = leftStart;
	if (dualWield && (!config::VrDualWield || !player2PromptKnown()
			|| ((time - dualSince) > 4'000'000'000LL && player2Prompting())))
	{
		dualWield = false;
		lightgunSet(1, OffScreen, OffScreen, 0);
		NOTICE_LOG(INPUT, "XR: player 2 is out: one gun again");
	}
	// Player 2 joins as at the cabinet: with the left controller's Start (vr.DualWield), and
	// the left hand gets player 2's gun (above). Out again once the game asks for player 2 in
	// its corner again (lost, or never got in), after a few seconds to let the join land.
	Gun& gun = guns[0];
	if (!dualWield)
	{
		// One gun: in the hand whose trigger is pulled while the gun hand's isn't, or the
		// other one when the gun hand's controller is gone (asleep, battery).
		const int otherHand = 1 - gun.hand;
		if (!gun.triggerWas && ((tracked[otherHand] && !tracked[gun.hand]) || (pulls[otherHand] > 0.6f && pulls[gun.hand] < 0.35f)))
		{
			gun.hand = otherHand;
			NOTICE_LOG(INPUT, "XR: the gun is in the %s hand", gun.hand == Right ? "right" : "left");
		}
	}

	// The buttons that aren't the trigger. One gun: either Start is player 1's (unless the
	// left one lets player 2 join), either B too (the gun's B: HOTD2 skips a story scene and
	// backs out of menus with it). Two guns: each hand's are its player's. The D-pad (menus)
	// is player 1's.
	u32 p1 = dpad, p2 = 0;
	if (dualWield)
	{
		p1 |= (rightStart ? 4 : 0) | (boolAction(skipAction, Right) ? 128 : 0);
		p2 |= (leftStart ? 4 : 0) | (boolAction(skipAction, Left) ? 128 : 0);
	}
	else
		p1 |= (rightStart || (leftStart && !canJoin) ? 4 : 0) | (boolAction(skipAction) ? 128 : 0);

	updateGun(guns[0], time, locations, tracked, pulls, p1, !dualWield);
	if (dualWield)
		updateGun(guns[1], time, locations, tracked, pulls, p2, false);
}

// Left-eye image, every `step`th pixel, to a PPM file. Buffers are kept and the file goes
// out in one write: a capture runs on the render thread, frame after frame.
static bool writeEye(GLuint fbo, int width, int height, const std::string& path, int step)
{
	static std::vector<u8> pixels, rgb;
	pixels.resize((size_t)width * height * 4);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	const int w = width / step, h = height / step;
	rgb.resize((size_t)w * h * 3);
	u8 *out = rgb.data();
	for (int row = 0; row < h; row++)
	{
		const int y = height - 1 - row * step;	// GL rows go up
		for (int col = 0; col < w; col++, out += 3)
			memcpy(out, &pixels[((size_t)y * width + (size_t)col * step) * 4], 3);
	}
	FILE *f = fopen(path.c_str(), "wb");
	if (f == nullptr)
		return false;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	fwrite(rgb.data(), 1, rgb.size(), f);
	fclose(f);
	return true;
}

// The shown frame's polygons on the 2D plane, for finding what the game draws right after
// a shot. The frame has been parsed: opaque and punch-through polygons, and translucent ones
// of passes that weren't auto-sorted, are ranges of the index buffer by now (strips merged
// into one, the others left empty); only auto-sorted translucent ones still point at vertices.
// Hidden vertices (NaN) were left out of the indices, so they can't be counted here.
static void writeOverlayList(const std::string& path, const glm::vec2& gun)
{
	const rend_context *ctx = gles_vr_frame();
	FILE *s = ctx != nullptr ? fopen(path.c_str(), "w") : nullptr;
	if (s == nullptr)
		return;
	const GameCamera cam = gameCamera(*ctx);
	fprintf(s, "gun %.0f %.0f  dc %.0fx%.0f  overlayW %.4f\n", gun.x, gun.y, cam.dcSize.x, cam.dcSize.y, cam.overlayW);
	// which translucent polygons still point at vertices
	std::vector<bool> trByVertex(ctx->global_param_tr.size(), false);
	RenderPass prev {};
	for (const RenderPass& pass : ctx->render_passes)
	{
		if (pass.sorted_tr_count != prev.sorted_tr_count)
			for (u32 n = prev.tr_count; n < pass.tr_count && n < trByVertex.size(); n++)
				trByVertex[n] = true;
		prev = pass;
	}
	std::vector<u32> vis;
	int lines = 0, mixedDumped = 0;
	for (const auto& [list, name] : { std::pair(&ctx->global_param_op, "OP"), std::pair(&ctx->global_param_pt, "PT"),
			std::pair(&ctx->global_param_tr, "TR") })
		for (size_t n = 0; n < list->size() && lines < 400; n++)
		{
			const PolyParam& pp = (*list)[n];
			vis.clear();
			if (list == &ctx->global_param_tr && trByVertex[n])
			{
				for (u32 i = pp.first; i < pp.first + pp.count && i < ctx->verts.size(); i++)
					vis.push_back(i);
			}
			else
			{
				for (u32 j = pp.first; j < pp.first + pp.count && j < ctx->idx.size(); j++)
				{
					const u32 vi = ctx->idx[j];
					if (vi != RestartIndex && vi < ctx->verts.size() && (vis.empty() || vis.back() != vi))
						vis.push_back(vi);
				}
			}
			if (vis.empty())
				continue;
			glm::vec2 lo(1e30f), hi(-1e30f);
			u32 overlay = 0, finite = 0;
			float wMin = 1e30f, wMax = 0.f;
			for (u32 vi : vis)
			{
				const Vertex& v = ctx->verts[vi];
				if (!std::isfinite(v.x) || !std::isfinite(v.y))
					continue;
				finite++;
				const float w = v.z > 0.f ? 1.f / v.z : 0.f;
				overlay += std::abs(w - cam.overlayW) < 0.002f;
				wMin = std::min(wMin, w);
				wMax = std::max(wMax, w);
				lo = glm::min(lo, glm::vec2(v.x, v.y));
				hi = glm::max(hi, glm::vec2(v.x, v.y));
			}
			const u8 *c = ctx->verts[vis[0]].col;
			const bool bigBright = lo.x <= hi.x && hi.x - lo.x >= cam.dcSize.x * 0.5f && hi.y - lo.y >= cam.dcSize.y * 0.5f
					&& c[0] >= 0xc0 && c[1] >= 0xc0;
			if (overlay == 0 && !bigBright)
				continue;
			fprintf(s, "%s %zu: %zu verts (%u on 2D) at %.0f,%.0f..%.0f,%.0f W %.3f..%.3f tex %06x tsp %08x isp %08x col %02x%02x%02x%02x\n",
					name, n, vis.size(), overlay, lo.x, lo.y, hi.x, hi.y, wMin, wMax,
					(unsigned)(pp.tcw.TexAddr << 3), pp.tsp.full, pp.isp.full, c[0], c[1], c[2], c[3]);
			lines++;
			// strips with both 2D-plane and 3D vertices: every vertex
			if (overlay > 0 && overlay < finite && mixedDumped < 12)
			{
				mixedDumped++;
				for (size_t k = 0; k < vis.size() && k < 64; k++)
				{
					const Vertex& v = ctx->verts[vis[k]];
					fprintf(s, "    v %zu: %.1f,%.1f W %.3f col %02x%02x%02x%02x uv %.3f,%.3f\n", k, v.x, v.y,
							v.z > 0.f ? 1.f / v.z : 0.f, v.col[0], v.col[1], v.col[2], v.col[3], v.u, v.v);
				}
			}
		}
	fclose(s);
}

// Debug aids (run-as):
//  "touch files/capture.request": the next left-eye image to files/eye0.ppm, plus fb0.ppm
//    (the console's video output in VRAM) and scene.txt (3D triangles);
//  "touch files/capture-shot.request": at the next trigger pull, 8 frames in a row to
//    files/shot1..8.ppm (half size) with the 2D polygons of each in shot1..8.txt.
void captureIfRequested(GLuint fbo, int width, int height)
{
	static std::string dir;
	if (dir.empty())
	{
		// the app's internal files directory, from the game path we were started with (when
		// it's in there), else the data directory
		const std::string& game = settings.content.path;
		const size_t at = game.find("/files/");
		dir = at != std::string::npos ? game.substr(0, at + 6) : get_writable_data_path("");
		if (!dir.empty() && (dir.back() == '/' || dir.back() == '\\'))
			dir.pop_back();
	}
	static bool shotArmed;
	static u32 armedSerial;
	static int shotFrame = -1;
	const std::string shotRequest = dir + "/capture-shot.request";
	if (!shotArmed && shotFrame < 0 && file_exists(shotRequest))
	{
		nowide::remove(shotRequest.c_str());
		shotArmed = true;
		armedSerial = shotSerial;
		NOTICE_LOG(RENDERER, "XR: capturing at the next shot");
	}
	if (shotArmed && shotSerial != armedSerial)
	{
		shotArmed = false;
		shotFrame = 0;
	}
	if (shotFrame >= 0)
	{
		shotFrame++;
		const std::string name = dir + "/shot" + std::to_string(shotFrame);
		writeEye(fbo, width, height, name + ".ppm", 2);
		writeOverlayList(name + ".txt", lastShotScreen);
		if (shotFrame >= 8)
		{
			shotFrame = -1;
			NOTICE_LOG(RENDERER, "XR: captured 8 frames after the shot to %s/shot1..8", dir.c_str());
		}
	}
	const std::string request = dir + "/capture.request";
	if (!file_exists(request))
		return;
	nowide::remove(request.c_str());
	if (!writeEye(fbo, width, height, dir + "/eye0.ppm", 1))
		return;
	NOTICE_LOG(RENDERER, "XR: captured the left eye to %s/eye0.ppm", dir.c_str());

	// ...and the picture the console's video output reads from VRAM right now, which is
	// what a TV would show when the game draws with the CPU.
	FramebufferInfo info;
	info.update();
	PixelBuffer<u32> pb;
	int fbWidth = 0, fbHeight = 0;
	ReadFramebuffer<RGBAPacker>(info, pb, fbWidth, fbHeight);
	FILE *f = fbWidth > 0 && fbHeight > 0 ? fopen((dir + "/fb0.ppm").c_str(), "wb") : nullptr;
	if (f == nullptr)
		return;
	fprintf(f, "P6\n%d %d\n255\n", fbWidth, fbHeight);
	const u32 *px = pb.data();
	for (int i = 0; i < fbWidth * fbHeight; i++)
		fwrite(&px[i], 1, 3, f);
	fclose(f);
	NOTICE_LOG(RENDERER, "XR: video output %dx%d from %06x to %s/fb0.ppm", fbWidth, fbHeight, info.fb_r_sof1, dir.c_str());

	// ...and the shown frame's solid 3D triangles, rebuilt in game eye space (y down, game
	// units, integers in 1/1000), for measuring the scene (floor height, sizes) offline.
	const rend_context *ctx = gles_vr_frame();
	FILE *s = ctx != nullptr ? fopen((dir + "/scene.txt").c_str(), "w") : nullptr;
	if (s == nullptr)
		return;
	const GameCamera cam = gameCamera(*ctx);
	fprintf(s, "tan %d %d scale %d\n", (int)(cam.tanHalf.x * 1e6f), (int)(cam.tanHalf.y * 1e6f), (int)(worldScale() * 1e6f));
	auto vertex = [&](u32 i, glm::vec3& p) {
		if (i == RestartIndex || i >= ctx->verts.size())
			return false;
		const Vertex& v = ctx->verts[i];
		if (!(v.z > 0.f) || !std::isfinite(v.z))
			return false;
		const float w = 1.f / v.z;
		if (w < MinSceneW || std::abs(w - cam.overlayW) < 0.002f)
			return false;
		const glm::vec2 ndc(v.x * 2.f / cam.dcSize.x - 1.f, v.y * 2.f / cam.dcSize.y - 1.f);
		p = glm::vec3(ndc.x * w * cam.tanHalf.x, ndc.y * w * cam.tanHalf.y, -w);
		return true;
	};
	int count = 0;
	for (const std::vector<PolyParam> *list : { &ctx->global_param_op, &ctx->global_param_pt })
		for (size_t n = list == &ctx->global_param_op ? 1 : 0; n < list->size(); n++)
		{
			const PolyParam& pp = (*list)[n];
			for (u32 j = pp.first; j + 2 < pp.first + pp.count && j + 2 < ctx->idx.size(); j++)
			{
				glm::vec3 a, b, c;
				if (!vertex(ctx->idx[j], a) || !vertex(ctx->idx[j + 1], b) || !vertex(ctx->idx[j + 2], c))
					continue;
				fprintf(s, "%d %d %d %d %d %d %d %d %d\n",
						(int)(a.x * 1000), (int)(a.y * 1000), (int)(a.z * 1000),
						(int)(b.x * 1000), (int)(b.y * 1000), (int)(b.z * 1000),
						(int)(c.x * 1000), (int)(c.y * 1000), (int)(c.z * 1000));
				count++;
			}
		}
	fclose(s);
	NOTICE_LOG(RENDERER, "XR: %d scene triangles to %s/scene.txt", count, dir.c_str());
}

}	// namespace

bool enabled()
{
	static const bool on = config::VrXr;
	return on && !(initTried && session == XR_NULL_HANDLE);
}

const Eye *currentEye()
{
	return drawingEye ? &eye : nullptr;
}

void term()
{
	if (instance != XR_NULL_HANDLE)
	{
		NOTICE_LOG(RENDERER, "XR: render context going away, closing the session");
		// still on the render thread with the context current: our own GL objects first
		termGun();
		termPanel();
		for (EyeChain& c : chains)
		{
			if (!c.fbos.empty())
				glDeleteFramebuffers((GLsizei)c.fbos.size(), c.fbos.data());
			if (c.depthStencil != 0)
				glDeleteRenderbuffers(1, &c.depthStencil);
		}
		if (session != XR_NULL_HANDLE)
			xrDestroySession(session);	// with its spaces and swapchains
		xrDestroyInstance(instance);	// with the actions
		// let go of the guns
		if (config::VrXrGun)
			for (const Gun& g : guns)
				lightgunSet(g.player, OffScreen, OffScreen, 0);
	}
	for (EyeChain& c : chains)
		c = EyeChain();
	instance = XR_NULL_HANDLE;
	systemId = XR_NULL_SYSTEM_ID;
	session = XR_NULL_HANDLE;
	localSpace = aimSpaces[Left] = aimSpaces[Right] = gripSpaces[Left] = gripSpaces[Right] = XR_NULL_HANDLE;
	actionSet = XR_NULL_HANDLE;
	aimAction = handAction = triggerAction = startAction = skipAction = recenterAction = hapticAction = stickAction = gripAction = XR_NULL_HANDLE;
	requestRefreshRate = nullptr;
	setPerformanceLevel = nullptr;
	sessionRunning = focused = drawingEye = false;
	sessionLost = exiting = false;
	originSet = recenterRequested = false;
	guns[0] = Gun { 0 };
	guns[1] = Gun { 1, Left };
	dualWield = leftStartWas = recenterWas = false;
	initTried = false;
}

bool frame()
{
	if (!initTried && !init())
	{
		ERROR_LOG(RENDERER, "XR: initialisation failed, showing the flat window instead");
		// no headset: no panel to show the run for the agent's hands on either
		hands::skipPrep();
		return rend_single_frame(true);
	}
	if (session != XR_NULL_HANDLE)
		pollEvents();
	if (sessionLost || exiting)
	{
		// Lost: a new session next frame. Exiting (quit from the headset menu): none.
		const bool quit = exiting;
		NOTICE_LOG(RENDERER, "XR: session %s", quit ? "exiting" : "lost");
		term();
		initTried = quit;
		exiting = quit;
	}
	if (!sessionRunning)
	{
		// Not shown (yet): keep the emulator's frames moving without spinning.
		rend_vr_drain(10);
		return false;
	}

	XrFrameWaitInfo waitInfo { XR_TYPE_FRAME_WAIT_INFO };
	XrFrameState frameState { XR_TYPE_FRAME_STATE };
	XrResult result = xrWaitFrame(session, &waitInfo, &frameState);
	if (XR_SUCCEEDED(result))
	{
		XrFrameBeginInfo beginInfo { XR_TYPE_FRAME_BEGIN_INFO };
		result = xrBeginFrame(session, &beginInfo);
	}
	if (!check(result, "xrWaitFrame/xrBeginFrame"))
	{
		sessionLost = result == XR_ERROR_SESSION_LOST || result == XR_ERROR_INSTANCE_LOST;
		rend_vr_drain(10);
		return false;
	}

	// Let through whatever the emulator produced since the last headset frame. While it runs
	// the game to its game over scene for the agent's hands (hands_rip.h), or its attract demo
	// for the hero's staff, it goes as fast as it can: it waits for each of its frames to be
	// taken, so keep taking them for most of the headset's frame.
	const bool preparing = hands::preparing();
	if (preparing)
	{
		const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(9);
		while (std::chrono::steady_clock::now() < until && hands::preparing())
			rend_vr_drain(1);
	}
	else
		rend_vr_drain(0);
	// What the panel says: while that runs, and for a moment when it's done.
	static bool wasPreparing;
	static std::chrono::steady_clock::time_point doneUntil;
	if (wasPreparing && !preparing && hands::prepProgress() >= 1.f)
		doneUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
	wasPreparing = preparing;
	Panel panel;
	const glm::vec3 red(0.86f, 0.13f, 0.1f), white(0.92f), grey(0.66f);
	const hands::Source *source = gameModelSource();
	const bool staff = source != nullptr && source->kind == hands::Source::HerosStaff;
	const char *title = staff ? "THE MAZE OF THE KINGS VR" : "THE HOUSE OF THE DEAD 2 VR";
	if (preparing)
	{
		panel.lines = {
			{ title, 0.085f, red },
			{ "", 0.03f, white },
			{ staff ? "Making the hero's staff from your game" : "Taking the agent's hands and pistol from your game", 0.064f, white },
			{ staff ? "Only this once: the game plays its attract demo by itself, fast."
					: "Only this once: the game plays itself to its game over, fast.", 0.047f, grey },
		};
		panel.progress = hands::prepProgress();
		panel.footer = "B: skip it for now (the arcade gun this time)";
	}
	else if (std::chrono::steady_clock::now() < doneUntil)
		panel.lines = {
			{ title, 0.085f, red },
			{ "", 0.03f, white },
			{ staff ? "Got it. The hero's staff is yours." : "Got them. The agent's hands are yours.", 0.064f, white },
			{ staff ? "Reload: point it away from the screen and pull the trigger."
					: "Reload: grab the back of the pistol with your other hand and pull.", 0.047f, grey },
		};
	const bool showPanel = !panel.lines.empty();

	XrViewState viewState { XR_TYPE_VIEW_STATE };
	XrViewLocateInfo locateInfo { XR_TYPE_VIEW_LOCATE_INFO };
	locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locateInfo.displayTime = frameState.predictedDisplayTime;
	locateInfo.space = localSpace;
	u32 viewCount = 0;
	const XrViewStateFlags tracked = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
	const bool located = XR_SUCCEEDED(xrLocateViews(session, &locateInfo, &viewState, 2, &viewCount, views))
			&& viewCount == 2 && (viewState.viewStateFlags & tracked) == tracked;
	updateOrigin(located);
	updateLightgun(frameState.predictedDisplayTime);

	XrCompositionLayerProjectionView projViews[2] { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
	const bool render = frameState.shouldRender && located;
	bool ok = true;
	for (int i = 0; render && i < 2; i++)
	{
		EyeChain& c = chains[i];
		u32 index = 0;
		XrSwapchainImageAcquireInfo acquire { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		XrSwapchainImageWaitInfo wait { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		wait.timeout = XR_INFINITE_DURATION;
		if (!XRCHECK(xrAcquireSwapchainImage(c.chain, &acquire, &index)) || !XRCHECK(xrWaitSwapchainImage(c.chain, &wait)))
		{
			ok = false;
			break;
		}
		const glm::mat4 view = glm::inverse(glm::translate(glm::mat4(1.f), relativePosition(views[i].pose.position))
				* glm::mat4_cast(relativeRotation(views[i].pose.orientation)));
		eye = { c.fbos[index], c.width, c.height, eyeProjection(views[i].fov) * view * gameToWorld() };
		if (showPanel)
		{
			// a dark room with the panel in it
			glBindFramebuffer(GL_FRAMEBUFFER, c.fbos[index]);
			glViewport(0, 0, c.width, c.height);
			glcache.Disable(GL_SCISSOR_TEST);
			glcache.ClearColor(0.02f, 0.02f, 0.025f, 1.f);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
			drawPanel(eyeProjection(views[i].fov) * view, panel);
		}
		else if (gles_vr_have_frame())
		{
			drawingEye = true;
			gles_vr_draw_eye(c.width, c.height);
			drawingEye = false;
		}
		else
		{
			// nothing to show yet: black, not whatever the image held before
			glBindFramebuffer(GL_FRAMEBUFFER, c.fbos[index]);
			glViewport(0, 0, c.width, c.height);
			glcache.Disable(GL_SCISSOR_TEST);
			glcache.ClearColor(0.f, 0.f, 0.f, 1.f);
			glClear(GL_COLOR_BUFFER_BIT);
		}
		if (!showPanel && (guns[0].visible || guns[1].visible))
		{
			// every gun's model, then every gun's glow (dots and flashes over both models)
			glBindFramebuffer(GL_FRAMEBUFFER, c.fbos[index]);
			glViewport(0, 0, c.width, c.height);
			const glm::mat4 viewProj = eyeProjection(views[i].fov) * view;
			const glm::vec3 eyePos = relativePosition(views[i].pose.position);
			bool first = true;
			for (const Gun& g : guns)
				if (g.visible)
				{
					drawGunModel(viewProj, eyePos, g.view, first);
					first = false;
				}
			first = true;
			for (const Gun& g : guns)
				if (g.visible)
				{
					drawGunGlow(viewProj, eyePos, g.view, first);
					first = false;
				}
		}
		if (i == 0)
			captureIfRequested(c.fbos[index], c.width, c.height);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		XrSwapchainImageReleaseInfo release { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		if (!XRCHECK(xrReleaseSwapchainImage(c.chain, &release)))
		{
			ok = false;
			break;
		}
		projViews[i].pose = views[i].pose;
		projViews[i].fov = views[i].fov;
		projViews[i].subImage.swapchain = c.chain;
		projViews[i].subImage.imageRect.extent = { c.width, c.height };
	}

	XrCompositionLayerProjection layer { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	layer.space = localSpace;
	layer.viewCount = 2;
	layer.views = projViews;
	const XrCompositionLayerBaseHeader *layers[] { (const XrCompositionLayerBaseHeader *)&layer };
	XrFrameEndInfo endInfo { XR_TYPE_FRAME_END_INFO };
	endInfo.displayTime = frameState.predictedDisplayTime;
	endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	endInfo.layerCount = render && ok ? 1 : 0;
	endInfo.layers = layers;
	XRCHECK(xrEndFrame(session, &endInfo));
	return render && ok;
}

}	// namespace vr::xr
#endif
