// New VegasCraft: the xNVSE plugin half of a Minecraft passthrough for Fallout: New Vegas.
//
// Every game loop it sends FNV's camera to the Minecraft mod (WebSocket 127.0.0.1:25599) and hands the same pose to
// the ReShade add-on (compositor.cpp), which composites Minecraft's frame into FNV's picture.
//
// Coordinates: 70 FNV units = 1 m = 1 block. FNV (x east, y north, z up) -> Minecraft (x, z + yOffset, -y).
// yOffset puts the ground where the player stands at Minecraft y = 64.
// Minecraft yaw: 0 faces +z (south), 180 faces north; pitch is positive looking down.
//
// Keys: F7 passthrough off/on, F8 re-level, F9 write the camera and player state to vegascraft.log.
#include "compositor.h"
#include "fnv_game.h"
#include "nvse_min.h"
#include "ws.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace
{
	constexpr int kPort = 25599;
	// Minecraft's window (and so its readback) is capped at about this many pixels; the effect scales it up
	constexpr double kMaxMinecraftPixels = 1920.0 * 1080.0;
	constexpr float kRad2Deg = 57.29577951f;

	HMODULE g_module = nullptr;
	PluginHandle g_handle = 0;
	NVSEMessagingInterface *g_messaging = nullptr;
	FILE *g_log = nullptr;

	WsClient g_ws;
	bool g_started = false;
	bool g_enabled = true;
	int g_generation = -1;
	int g_viewSent = 0;
	bool g_haveOffset = false;
	float g_yOffset = 0.0f;
	unsigned g_frame = 0;
	bool g_keyDown[256] = {};

	void log(const char *fmt, ...)
	{
		if (g_log == nullptr)
			return;
		va_list args;
		va_start(args, fmt);
		vfprintf(g_log, fmt, args);
		va_end(args);
		fputc('\n', g_log);
		fflush(g_log);
	}

	void sendf(const char *fmt, ...)
	{
		char buffer[1024];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buffer, sizeof(buffer), fmt, args);
		va_end(args);
		g_ws.send(buffer);
	}

	/// True once per press (FNV keeps the focus; GetAsyncKeyState also works under Wine).
	bool pressed(int vk)
	{
		const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
		const bool edge = down && !g_keyDown[vk];
		g_keyDown[vk] = down;
		return edge;
	}

	float wrap_degrees(float a)
	{
		a = std::fmod(a + 180.0f, 360.0f);
		return (a < 0.0f ? a + 360.0f : a) - 180.0f;
	}

	/// Minecraft x, y, z for an FNV position (game units).
	void to_mc(const fnv::Vec3 &p, double &x, double &y, double &z)
	{
		x = p.x / fnv::kUnitsPerMetre;
		y = p.z / fnv::kUnitsPerMetre + g_yOffset;
		z = -p.y / fnv::kUnitsPerMetre;
	}

	/// The camera's view direction in FNV's world: NiCamera looks down its local +X, i.e. the first column of its
	/// world rotation (Gamebryo's NiMatrix3 maps local to world as R * v).
	fnv::Vec3 camera_forward(const fnv::Camera &c)
	{
		return {c.rot[0][0], c.rot[1][0], c.rot[2][0]};
	}

	/// A test pillar 4 blocks in front of the player, so the first link shows something to line up against FNV.
	void place_test_pillar(const fnv::Vec3 &feet, float heading)
	{
		double x, y, z;
		to_mc(feet, x, y, z);
		const int bx = int(std::floor(x + std::sin(heading) * 4.0));
		const int bz = int(std::floor(z - std::cos(heading) * 4.0));
		const int by = int(std::floor(y + 0.01));
		sendf("{\"t\":\"cmd\",\"c\":\"fill %d %d %d %d %d %d minecraft:diamond_block\"}", bx, by, bz, bx, by + 2, bz);
		log("test pillar at Minecraft %d %d %d (player at %.2f %.2f %.2f)", bx, by, bz, x, y, z);
	}

	void dump_state(const fnv::Camera &c, const fnv::Vec3 &feet, const fnv::Vec3 &rot, bool third)
	{
		log("camera pos %.1f %.1f %.1f", c.pos.x, c.pos.y, c.pos.z);
		for (int i = 0; i < 3; ++i)
			log("camera rot row %d: % .4f % .4f % .4f", i, c.rot[i][0], c.rot[i][1], c.rot[i][2]);
		log("frustum l %.4f r %.4f t %.4f b %.4f near %.2f far %.1f", c.left, c.right, c.top, c.bottom, c.nearPlane, c.farPlane);
		log("player feet %.1f %.1f %.1f, rot %.3f %.3f %.3f (heading %.1f deg), third person %d", feet.x, feet.y, feet.z,
			rot.x, rot.y, rot.z, rot.z * kRad2Deg, third ? 1 : 0);
		int bw = 0, bh = 0;
		compositor::backbuffer_size(bw, bh);
		log("backbuffer %dx%d, link %s, yOffset %.3f", bw, bh, g_ws.connected() ? "up" : "down", g_yOffset);
	}

	void tick()
	{
		if (!g_started)
		{
			g_started = true;
			g_ws.start("127.0.0.1", kPort);
			log("connecting to Minecraft on 127.0.0.1:%d", kPort);
		}
		compositor::try_register(g_module);
		++g_frame;

		if (pressed(VK_F7))
		{
			g_enabled = !g_enabled;
			log("passthrough %s", g_enabled ? "on" : "off");
		}
		const bool relevel = pressed(VK_F8);
		const bool dump = pressed(VK_F9);

		fnv::Camera cam;
		fnv::Vec3 feet, rot;
		bool third = false;
		if (!fnv::readCamera(cam) || !fnv::readPlayer(feet, rot, third))
		{
			compositor::set_active(false);
			return;
		}
		if (dump)
			dump_state(cam, feet, rot, third);

		const bool linked = g_ws.connected();
		compositor::set_active(g_enabled && linked);
		if (!linked)
			return;

		if (g_ws.generation() != g_generation)
		{
			// a new connection: Minecraft may have restarted, send everything again
			g_generation = g_ws.generation();
			g_viewSent = 0;
			g_haveOffset = false;
			log("linked to Minecraft (connection %d)", g_generation);
		}

		int bw = 0, bh = 0;
		compositor::backbuffer_size(bw, bh);
		if (bw > 0 && bh > 0 && (bw * 65536 + bh) != g_viewSent)
		{
			g_viewSent = bw * 65536 + bh;
			const double scale = std::min(1.0, std::sqrt(kMaxMinecraftPixels / (double(bw) * bh)));
			sendf("{\"t\":\"view\",\"w\":%d,\"h\":%d}", int(bw * scale + 0.5), int(bh * scale + 0.5));
		}

		if (!g_haveOffset || relevel)
		{
			// no ground probe yet: the player's feet stand for the ground
			g_yOffset = 64.0f - feet.z / fnv::kUnitsPerMetre;
			g_haveOffset = true;
			g_ws.send("{\"t\":\"clear\"}");
			place_test_pillar(feet, rot.z);
		}

		// Minecraft's camera from FNV's: position, view direction -> yaw/pitch, vertical fov from the frustum
		double cx, cy, cz;
		to_mc(cam.pos, cx, cy, cz);
		const fnv::Vec3 f = camera_forward(cam);
		const float dx = f.x, dy = f.z, dz = -f.y; // in Minecraft's axes
		const float mcYaw = wrap_degrees(std::atan2(-dx, dz) * kRad2Deg);
		const float mcPitch = std::asin(std::clamp(-dy, -1.0f, 1.0f)) * kRad2Deg;
		const float fov = (std::atan(cam.top) - std::atan(cam.bottom)) * kRad2Deg;
		compositor::set_host_planes(cam.nearPlane / fnv::kUnitsPerMetre, cam.farPlane / fnv::kUnitsPerMetre);
		compositor::set_host_pose(mcYaw, mcPitch, 0.0f, fov, cx, cy, cz);

		double px, py, pz;
		to_mc(feet, px, py, pz);
		// FNV heading: clockwise from north. Minecraft: 180 = north, 270 = east
		const float bodyYaw = wrap_degrees(180.0f + rot.z * kRad2Deg);
		sendf("{\"t\":\"cam\",\"f\":%u,\"p\":[%.4f,%.4f,%.4f],\"r\":[%.3f,%.3f,0],\"fov\":%.3f,\"fp\":%s,\"pl\":[%.4f,%.4f,%.4f],\"h\":%.3f}",
			g_frame, cx, cy, cz, mcYaw, mcPitch, fov, third ? "false" : "true", px, py, pz, bodyYaw);

		std::string message;
		while (g_ws.poll(message))
			if (message.find("\"hello\"") != std::string::npos)
				log("Minecraft: %s", message.c_str());
	}

	void on_message(NVSEMessagingInterface::Message *msg)
	{
		switch (msg->type)
		{
		case NVSEMessagingInterface::kMessage_MainGameLoop:
			tick();
			break;
		case NVSEMessagingInterface::kMessage_ExitGame:
			g_ws.stop();
			break;
		default:
			break;
		}
	}
}

extern "C"
{
	__declspec(dllexport) bool NVSEPlugin_Query(const NVSEInterface *nvse, PluginInfo *info)
	{
		info->infoVersion = PluginInfo::kInfoVersion;
		info->name = "VegasCraft";
		info->version = 1;
		return !nvse->isEditor && nvse->runtimeVersion == kRuntime_1_4_0_525;
	}

	__declspec(dllexport) bool NVSEPlugin_Load(const NVSEInterface *nvse)
	{
		g_log = std::fopen("vegascraft.log", "w");
		log("New VegasCraft loaded (xNVSE %08X, runtime %08X)", nvse->nvseVersion, nvse->runtimeVersion);
		g_handle = nvse->GetPluginHandle();
		g_messaging = static_cast<NVSEMessagingInterface *>(nvse->QueryInterface(kInterface_Messaging));
		if (g_messaging == nullptr || !g_messaging->RegisterListener(g_handle, "NVSE", on_message))
		{
			log("no xNVSE messaging interface");
			return false;
		}
		return true;
	}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
		g_module = module;
	else if (reason == DLL_PROCESS_DETACH)
		compositor::unregister(module);
	return TRUE;
}
