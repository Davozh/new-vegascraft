// New VegasCraft: the xNVSE plugin half of a Minecraft passthrough for Fallout: New Vegas.
//
// Every game loop it sends FNV's camera to the Minecraft mod (WebSocket 127.0.0.1:25599) and hands the same pose to
// the ReShade add-on (compositor.cpp), which composites Minecraft's frame into FNV's picture.
//
// Coordinates: 70 FNV units = 1 m = 1 block. FNV (x east, y north, z up) -> Minecraft (x, z + yOffset, -y).
// yOffset is fixed outdoors (kExteriorYOffset); indoors it puts the ground under the player at Minecraft y = 64.
// Minecraft yaw: 0 faces +z (south), 180 faces north; pitch is positive looking down.
//
// Ground: the plugin casts rays down through FNV's physics world in the columns around the player and sends them as
// barrier columns, so Minecraft's blocks, mobs and items rest on FNV's terrain, rocks and buildings.
//
// Building: B toggles build mode. In it the mouse belongs to Minecraft (left: break/attack, right: place/use, 1-9:
// hotbar) and FNV's own fighting is disabled; out of it FNV fights as usual.
//
// Keys: F7 passthrough off/on, F8 re-level, F10 write the camera and player state to vegascraft.log, F11 the effect's
// next debug view, F12 a probe pillar where the crosshair hits, Shift+F12 a
// motion capture (synchronised screenshots + poses).
// (Not F9: that is FNV's quick load.)
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
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{
	constexpr int kPort = 25599;
	// Minecraft's window (and so its readback) is capped at about this many pixels; the effect scales it up
	constexpr double kMaxMinecraftPixels = 1920.0 * 1080.0;
	constexpr float kRad2Deg = 57.29577951f;
	constexpr int kGroundRadius = 32;      // blocks around the player that get collision
	constexpr int kGroundProbesPerTick = 48;
	constexpr int kGroundDepth = 2;        // barrier layers under each surface
	constexpr float kProbeAbove = 175.0f;  // probes start 2.5 m above the feet: indoors that finds the floor, not the roof
	constexpr float kProbeRange = 7000.0f; // and look 100 m down
	constexpr float kTeleport = 30.0f * fnv::kUnitsPerMetre; // a jump this far in one frame: fast travel, a door
	// Mojave heights (~0-300 m) land at Minecraft y ~ -34..266, inside its -64..320 build range; Nipton's ground ~64
	constexpr float kExteriorYOffset = -34.0f;

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
	std::vector<std::pair<int, int>> g_spiral;
	std::unordered_set<int64_t> g_sampled;
	fnv::Vec3 g_lastFeet = {0, 0, 0};
	bool g_build = false;
	bool g_fightDisabledByUs = false;
	bool g_mouseDown[2] = {};
	fnv::Camera g_presentCam = {};  // the camera as it was when the last frame was presented
	bool g_havePresentCam = false;

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

	struct McPose
	{
		double x, y, z;
		float yaw, pitch, fov, nearM, farM;
	};

	/// Minecraft's camera from FNV's: position, view direction -> yaw/pitch, vertical fov from the frustum.
	McPose mc_pose(const fnv::Camera &cam)
	{
		McPose p;
		to_mc(cam.pos, p.x, p.y, p.z);
		const fnv::Vec3 f = camera_forward(cam);
		const float dx = f.x, dy = f.z, dz = -f.y; // in Minecraft's axes
		p.yaw = wrap_degrees(std::atan2(-dx, dz) * kRad2Deg);
		p.pitch = std::asin(std::clamp(-dy, -1.0f, 1.0f)) * kRad2Deg;
		const float halfTan = (cam.top - cam.bottom) * 0.5f;
		p.fov = 2.0f * std::atan(halfTan) * kRad2Deg;
		p.nearM = cam.nearPlane / fnv::kUnitsPerMetre;
		p.farM = cam.farPlane / fnv::kUnitsPerMetre;
		return p;
	}

	/// Just before FNV presents a frame its camera is the one that frame was drawn with: hand that pose to the
	/// compositor, so it re-projects Minecraft's picture onto exactly the picture on screen. (Read in the game loop,
	/// the camera is the previous frame's: blocks shook by a frame's worth of movement whenever the player moved.)
	void on_present(bool loadingScreen)
	{
		fnv::Camera cam;
		if (loadingScreen || !g_haveOffset || !fnv::playerInWorld() || !fnv::readCamera(cam))
			return;
		g_presentCam = cam;
		g_havePresentCam = true;
		const McPose p = mc_pose(cam);
		compositor::set_host_planes(p.nearM, p.farM);
		compositor::set_host_pose(p.yaw, p.pitch, 0.0f, p.fov, p.x, p.y, p.z);
		if (!g_ws.connected())
			return;
		// and Minecraft renders from it too: its next frame then needs the least re-projection
		fnv::Vec3 feet, rot;
		bool third = false;
		if (!fnv::readPlayer(feet, rot, third))
			return;
		double px, py, pz;
		to_mc(feet, px, py, pz);
		// FNV heading: clockwise from north. Minecraft: 180 = north, 270 = east
		const float bodyYaw = wrap_degrees(180.0f + rot.z * kRad2Deg);
		sendf("{\"t\":\"cam\",\"f\":%u,\"p\":[%.4f,%.4f,%.4f],\"r\":[%.3f,%.3f,0],\"fov\":%.3f,\"fp\":%s,\"pl\":[%.4f,%.4f,%.4f],\"h\":%.3f}",
			g_frame, p.x, p.y, p.z, p.yaw, p.pitch, p.fov, third ? "false" : "true", px, py, pz, bodyYaw);
	}

	int64_t column_key(int x, int z)
	{
		return (int64_t(x) << 32) ^ uint32_t(z);
	}

	/// Probes FNV's ground in the not yet sampled Minecraft columns nearest the player and sends them as barriers.
	void sample_ground(const fnv::Vec3 &feet)
	{
		if (g_spiral.empty())
		{
			for (int dx = -kGroundRadius; dx <= kGroundRadius; ++dx)
				for (int dz = -kGroundRadius; dz <= kGroundRadius; ++dz)
					if (dx * dx + dz * dz <= kGroundRadius * kGroundRadius)
						g_spiral.emplace_back(dx, dz);
			std::sort(g_spiral.begin(), g_spiral.end(), [](auto &a, auto &b) {
				return a.first * a.first + a.second * a.second < b.first * b.first + b.second * b.second;
			});
		}
		const int px = int(std::floor(feet.x / fnv::kUnitsPerMetre)), pz = int(std::floor(-feet.y / fnv::kUnitsPerMetre));
		const bool outside = !fnv::inInterior();
		std::string columns;
		int probes = 0;
		for (const auto &[dx, dz] : g_spiral)
		{
			const int x = px + dx, z = pz + dz;
			if (g_sampled.count(column_key(x, z)))
				continue;
			if (++probes > kGroundProbesPerTick)
				break;
			// Minecraft column (x, z) covers FNV x in [x, x+1) m and y in (-z-1, -z] m: probe its centre
			const float fx = (x + 0.5f) * fnv::kUnitsPerMetre, fy = -(z + 0.5f) * fnv::kUnitsPerMetre;
			float start = feet.z + kProbeAbove;
			float land = 0.0f;
			const bool haveLand = outside && fnv::terrainHeight(fx, fy, land);
			if (haveLand && land > start - 50.0f)
				start = land + 50.0f; // a hillside above the player: a ray from inside the hill would miss it
			fnv::Vec3 hit;
			float groundZ;
			if (fnv::rayCast({fx, fy, start}, {0.0f, 0.0f, -1.0f}, kProbeRange, hit))
				groundZ = hit.z;
			else if (haveLand)
				groundZ = land;
			else
				continue; // collision not loaded yet: try again later
			g_sampled.insert(column_key(x, z));
			const int top = int(std::floor(groundZ / fnv::kUnitsPerMetre + g_yOffset + 0.5f)) - 1;
			char entry[64];
			snprintf(entry, sizeof(entry), "%s%d,%d,%d,%d", columns.empty() ? "" : ",", x, z, top - kGroundDepth + 1, top);
			columns += entry;
		}
		if (!columns.empty())
			g_ws.send("{\"t\":\"ground\",\"c\":[" + columns + "]}");
	}

	/// The game window has the focus (the keys and buttons are meant for FNV, not another program).
	bool focused()
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &pid);
		return pid == GetCurrentProcessId();
	}

	void set_fight_disabled(bool disabled)
	{
		uint8_t &flags = fnv::disabledControls();
		if (disabled)
		{
			if (!(flags & fnv::kControlFight))
				g_fightDisabledByUs = true;
			flags |= fnv::kControlFight;
		}
		else if (g_fightDisabledByUs)
		{
			flags &= ~fnv::kControlFight;
			g_fightDisabledByUs = false;
		}
	}

	void forward_mouse(int index, int vk, const char *key, bool allowed)
	{
		const bool down = allowed && (GetAsyncKeyState(vk) & 0x8000) != 0;
		if (down != g_mouseDown[index])
		{
			g_mouseDown[index] = down;
			sendf("{\"t\":\"key\",\"k\":\"%s\",\"down\":%s}", key, down ? "true" : "false");
		}
	}

	/// Build mode: Minecraft gets the mouse and the hotbar keys, FNV doesn't fight.
	void build_tick(bool active)
	{
		set_fight_disabled(active);
		forward_mouse(0, VK_LBUTTON, "attack", active);
		forward_mouse(1, VK_RBUTTON, "use", active);
		if (!active)
			return;
		for (int i = 0; i < 9; ++i)
			if (pressed('1' + i))
				sendf("{\"t\":\"slot\",\"n\":%d}", i);
	}

	/// F12: where FNV's crosshair ray hits its world, a 1x1x2 diamond pillar in Minecraft. It must stand exactly under
	/// the crosshair and stay there as the player moves: a calibration of the whole FNV -> Minecraft mapping.
	void place_probe(const fnv::Camera &cam)
	{
		const fnv::Vec3 f = camera_forward(cam);
		fnv::Vec3 hit;
		if (!fnv::rayCast(cam.pos, f, 20000.0f, hit))
		{
			log("probe: the crosshair ray hit nothing");
			return;
		}
		double x, y, z;
		to_mc(hit, x, y, z);
		const int bx = int(std::floor(x)), by = int(std::floor(y + 0.01)), bz = int(std::floor(z));
		sendf("{\"t\":\"cmd\",\"c\":\"fill %d %d %d %d %d %d minecraft:diamond_block\"}", bx, by, bz, bx, by + 1, bz);
		log("probe: FNV hit %.1f %.1f %.1f (%.1f m away) -> Minecraft %.3f %.3f %.3f -> pillar at block %d %d %d", hit.x, hit.y, hit.z,
			std::sqrt((hit.x - cam.pos.x) * (hit.x - cam.pos.x) + (hit.y - cam.pos.y) * (hit.y - cam.pos.y) + (hit.z - cam.pos.z) * (hit.z - cam.pos.z)) / fnv::kUnitsPerMetre,
			x, y, z, bx, by, bz);
	}

	void dump_state(const fnv::Camera &c, const fnv::Vec3 &feet, const fnv::Vec3 &rot, bool third)
	{
		log("camera pos %.1f %.1f %.1f", c.pos.x, c.pos.y, c.pos.z);
		for (int i = 0; i < 3; ++i)
			log("camera rot row %d: % .4f % .4f % .4f", i, c.rot[i][0], c.rot[i][1], c.rot[i][2]);
		log("frustum l %.4f r %.4f t %.4f b %.4f near %.2f far %.1f -> fov %.2f x %.2f deg; SceneGraph cameraFOV (0xBC) %.2f",
			c.left, c.right, c.top, c.bottom, c.nearPlane, c.farPlane, 2 * std::atan(c.right) * kRad2Deg, 2 * std::atan(c.top) * kRad2Deg,
			*reinterpret_cast<const float *>(fnv::sceneGraph() + 0xBC));
		log("player feet %.1f %.1f %.1f, rot %.3f %.3f %.3f (heading %.1f deg), third person %d (0x64A) / %d (0x64C)", feet.x, feet.y, feet.z,
			rot.x, rot.y, rot.z, rot.z * kRad2Deg, third ? 1 : 0, *reinterpret_cast<const uint8_t *>(fnv::player() + 0x64C));
		float land = 0.0f;
		fnv::Vec3 hit = {0, 0, 0};
		const bool haveLand = fnv::terrainHeight(feet.x, feet.y, land);
		const bool haveHit = fnv::rayCast({feet.x, feet.y, feet.z + kProbeAbove}, {0, 0, -1}, kProbeRange, hit);
		log("ground under the player: terrain %s %.1f, ray %s %.1f; interior %d, menu %d, build %d", haveLand ? "yes" : "no", land,
			haveHit ? "hit" : "miss", hit.z, fnv::inInterior() ? 1 : 0, fnv::inMenu() ? 1 : 0, g_build ? 1 : 0);
		int bw = 0, bh = 0;
		compositor::backbuffer_size(bw, bh);
		log("backbuffer %dx%d, link %s, yOffset %.3f", bw, bh, g_ws.connected() ? "up" : "down", g_yOffset);
		if (g_havePresentCam)
		{
			const fnv::Camera &p = g_presentCam;
			log("at present: camera pos %.1f %.1f %.1f, forward %.4f %.4f %.4f, frustum t %.4f r %.4f", p.pos.x, p.pos.y, p.pos.z,
				p.rot[0][0], p.rot[1][0], p.rot[2][0], p.top, p.right);
			for (int i = 0; i < 4; ++i)
				log("at present: worldToCam row %d: % .5f % .5f % .5f % .2f", i, p.worldToCam[i][0], p.worldToCam[i][1], p.worldToCam[i][2], p.worldToCam[i][3]);
		}
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
		const bool dump = pressed(VK_F10);
		const bool f12 = pressed(VK_F12) && focused();
		const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
		const bool probe = f12 && !shift;
		if (f12 && shift)
		{
			compositor::request_capture(12, 5);
			log("motion capture: 12 screenshots, one every 5 frames (yOffset %.3f)", g_yOffset);
		}
		if (pressed(VK_F11) && focused())
		{
			compositor::cycle_debug_view();
			log("debug view changed");
		}
		if (pressed('B') && focused())
		{
			g_build = !g_build;
			log("build mode %s", g_build ? "on" : "off");
		}

		fnv::Camera cam;
		fnv::Vec3 feet, rot;
		bool third = false;
		// in the main menu the player exists but stands nowhere (no cell): nothing to show, nothing to level on
		if (!fnv::readCamera(cam) || !fnv::readPlayer(feet, rot, third) || !fnv::playerInWorld())
		{
			compositor::set_active(false);
			return;
		}
		if (dump)
			dump_state(cam, feet, rot, third);

		const bool linked = g_ws.connected();
		const bool menu = fnv::inMenu();
		// menus (Pip-Boy, dialogue, messages) are drawn before ReShade composites: hide Minecraft while one is open
		compositor::set_active(g_enabled && linked && !menu);
		build_tick(g_build && g_enabled && linked && !menu && focused());
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

		const float jumpX = feet.x - g_lastFeet.x, jumpY = feet.y - g_lastFeet.y, jumpZ = feet.z - g_lastFeet.z;
		if (jumpX * jumpX + jumpY * jumpY + jumpZ * jumpZ > kTeleport * kTeleport)
			g_haveOffset = false; // fast travel or a door: level the new place
		g_lastFeet = feet;
		if (!g_haveOffset || relevel)
		{
			// The Mojave keeps one fixed offset, so what was built stays at the same height after a reload or fast
			// travel (levelling to the ground underfoot each time moved every build up or down by up to a block).
			// Interiors have coordinates of their own: there the ground under the player goes to Minecraft y = 64.
			const bool indoors = fnv::inInterior();
			float groundZ = feet.z;
			fnv::Vec3 hit;
			if (fnv::rayCast({feet.x, feet.y, feet.z + kProbeAbove}, {0, 0, -1}, kProbeRange, hit))
				groundZ = hit.z;
			g_yOffset = indoors ? 64.0f - groundZ / fnv::kUnitsPerMetre : kExteriorYOffset;
			g_haveOffset = true;
			g_sampled.clear();
			g_ws.send("{\"t\":\"clear\"}");
			log("levelled: ground %.1f -> Minecraft y %.2f (yOffset %.3f)%s", groundZ, groundZ / fnv::kUnitsPerMetre + g_yOffset, g_yOffset,
				indoors ? " indoors" : "");
		}
		sample_ground(feet);
		if (probe)
			place_probe(cam);

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
		case NVSEMessagingInterface::kMessage_OnFramePresent:
			on_present(msg->data != nullptr && *static_cast<const int *>(msg->data) != 0);
			break;
		case NVSEMessagingInterface::kMessage_PostLoadGame:
		case NVSEMessagingInterface::kMessage_NewGame:
			// another place: level the ground there
			g_haveOffset = false;
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
		// ReShade (d3d9.dll) is loaded with the game: register now, before FNV creates its device
		log("ReShade add-on %s at load", compositor::try_register(g_module) ? "registered" : "not registered yet");
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
