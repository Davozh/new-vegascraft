#include "compositor.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <reshade.hpp>

using namespace reshade::api;

namespace
{
	// Minecraft runs natively on Linux and writes /dev/shm/VegasCraftFrame; Wine sees it on drive Z:
	constexpr const wchar_t *kFramePath = L"Z:\\dev\\shm\\VegasCraftFrame";
	constexpr uint32_t kMagic = 0x5450434D; // "MCPT"
	constexpr int kHeader = 4096;
	constexpr int kSlotDesc = 256;
	constexpr int kSlotDescBytes = 128;
	constexpr const char *kEffect = "VegasCraft.fx";

	std::atomic<bool> g_registered{false};
	std::atomic<bool> g_active{false};
	std::atomic<float> g_hostNear{0.1f};
	std::atomic<float> g_hostFar{5000.0f};
	std::atomic<uint32_t> g_bbWidth{0}, g_bbHeight{0};
	std::atomic<bool> g_cameraLocked{false};
	std::atomic<int> g_debugView{-1}; // -1: leave the preset's value
	std::atomic<int> g_captureLeft{0}, g_captureEvery{5}, g_captureDelay{0};
	int g_captureFrame = 0, g_captureId = 0;
	struct UsedPoses
	{
		float hy = 0, hp = 0, hf = 0, my = 0, mp = 0, mf = 0;
		double hx = 0, hyy = 0, hz = 0, mx = 0, myy = 0, mz = 0;
		bool valid = false;
	} g_used; // the poses the last composite used
	std::atomic<float> g_lookLight{-1.0f}, g_lookBias{-1.0f}, g_lookSlope{-1.0f};
	std::atomic<float> g_shakeX{0.0f}, g_shakeY{0.0f}, g_shakeRoll{0.0f}, g_portalWarp{0.0f};
	float g_savedLight = -1.0f, g_savedBias = -1.0f, g_savedSlope = -1.0f;

	struct Pose
	{
		float yaw = 0, pitch = 0, roll = 0, fov = 70;
		double x = 0, y = 0, z = 0;
		bool valid = false;
	};
	std::mutex g_poseLock;
	// FNV's recent camera poses, newest last. The script reads the camera FNV is about to render (the next frame),
	// while the picture being presented was rendered with the one before: re-project to that (g_poseLag back).
	Pose g_hostPoses[4];
	unsigned g_hostPoseCount = 0;
	std::atomic<int> g_poseLag{0}; // FNV: set live with Insert while measuring
	Pose g_mcPose;

	HANDLE g_file = INVALID_HANDLE_VALUE;
	HANDLE g_mapping = nullptr;
	const uint8_t *g_view = nullptr; // the header page only: FNV is 32-bit, so slots are mapped one at a time
	int32_t g_slots = 0;
	int64_t g_stride = 0;
	int64_t g_lastPublish = -1;
	DWORD g_nextOpenAttempt = 0;
	DWORD g_allocGranularity = 65536;

	struct Layer
	{
		resource tex = {0};
		resource_view srv = {0};
	};
	Layer g_world, g_depth, g_overlay;
	uint32_t g_width = 0, g_height = 0;
	bool g_hasFrame = false;
	float g_mcNear = 0.05f, g_mcFar = 2048.0f;
	int32_t g_mcFlags = 7;

	template <typename T>
	T read(const uint8_t *p)
	{
		T v;
		std::memcpy(&v, p, sizeof(T));
		return v;
	}

	/// effect_runtime::find_uniform_variable, called the way ReShade (built with MSVC) expects. MSVC returns a struct
	/// from a member function through a hidden pointer (the first stack argument, popped by the callee); GCC returns
	/// an 8-byte struct in EDX:EAX, so a plain virtual call from mingw would corrupt the stack.
	effect_uniform_variable find_uniform(effect_runtime *runtime, const char *effect, const char *name)
	{
#if defined(__GNUC__) && defined(__i386__)
		using Fn = effect_uniform_variable *(__attribute__((thiscall)) *)(const effect_runtime *, effect_uniform_variable *, const char *, const char *);
		// GCC's pointer to a virtual member: 1 + the byte offset of its vtable slot (same layout as MSVC's here:
		// single inheritance, no virtual destructors, no overloaded virtuals)
		auto member = &effect_runtime::find_uniform_variable;
		struct { uintptr_t ptr, adj; } raw;
		std::memcpy(&raw, &member, sizeof(raw));
		void **vtable = *reinterpret_cast<void ***>(runtime);
		const Fn fn = reinterpret_cast<Fn>(vtable[(raw.ptr - 1) / sizeof(void *)]);
		effect_uniform_variable result = {0};
		fn(runtime, &result, effect, name);
		return result;
#else
		return runtime->find_uniform_variable(effect, name);
#endif
	}

	void close_mapping()
	{
		if (g_view != nullptr)
			UnmapViewOfFile(g_view);
		if (g_mapping != nullptr)
			CloseHandle(g_mapping);
		if (g_file != INVALID_HANDLE_VALUE)
			CloseHandle(g_file);
		g_view = nullptr;
		g_mapping = nullptr;
		g_file = INVALID_HANDLE_VALUE;
	}

	bool open_mapping()
	{
		if (g_view != nullptr)
			return true;
		if (GetTickCount() < g_nextOpenAttempt)
			return false;
		g_nextOpenAttempt = GetTickCount() + 1000;
		SYSTEM_INFO si;
		GetSystemInfo(&si);
		g_allocGranularity = si.dwAllocationGranularity;
		g_file = CreateFileW(kFramePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (g_file == INVALID_HANDLE_VALUE)
			return false;
		g_mapping = CreateFileMappingW(g_file, nullptr, PAGE_READONLY, 0, 0, nullptr);
		if (g_mapping == nullptr)
		{
			close_mapping();
			return false;
		}
		g_view = static_cast<const uint8_t *>(MapViewOfFile(g_mapping, FILE_MAP_READ, 0, 0, kHeader));
		if (g_view == nullptr || read<uint32_t>(g_view) != kMagic)
		{
			close_mapping();
			return false;
		}
		g_slots = read<int32_t>(g_view + 12);
		g_stride = read<int64_t>(g_view + 16);
		reshade::log::message(reshade::log::level::info, "VegasCraft: connected to Minecraft's frame export");
		return true;
	}

	/// Maps [offset, offset + size) of the frame file; *base points at offset. Unmap the returned view when done.
	const uint8_t *map_range(int64_t offset, size_t size, const uint8_t **base)
	{
		const int64_t aligned = offset - offset % g_allocGranularity;
		const size_t delta = size_t(offset - aligned);
		auto *view = static_cast<const uint8_t *>(MapViewOfFile(g_mapping, FILE_MAP_READ, DWORD(uint64_t(aligned) >> 32), DWORD(aligned), delta + size));
		*base = view != nullptr ? view + delta : nullptr;
		return view;
	}

	void destroy_layers(device *dev)
	{
		for (Layer *layer : {&g_world, &g_depth, &g_overlay})
		{
			if (layer->srv.handle != 0)
				dev->destroy_resource_view(layer->srv);
			if (layer->tex.handle != 0)
				dev->destroy_resource(layer->tex);
			*layer = Layer();
		}
		g_width = g_height = 0;
		g_hasFrame = false;
	}

	/// Dynamic textures, written by locking them (see write_layer). The colour layers are created BGRA (D3D9's native
	/// 8-bit order) and filled with Minecraft's RGBA bytes as they are; the effect swaps red and blue back when it
	/// samples them. ReShade's update_texture_region would instead create a staging texture on every call and swap
	/// each pixel on the CPU: ~19 ms per Minecraft frame at 2536x1384, on FNV's main thread.
	bool create_layer(device *dev, Layer &layer, uint32_t w, uint32_t h, format fmt)
	{
		if (!dev->create_resource(
				resource_desc(w, h, 1, 1, fmt, 1, memory_heap::default_, resource_usage::shader_resource, resource_flags::dynamic),
				nullptr, resource_usage::shader_resource, &layer.tex))
			return false;
		return dev->create_resource_view(layer.tex, resource_usage::shader_resource, resource_view_desc(fmt), &layer.srv);
	}

	/// Copies a w x h layer (4 bytes a pixel, rows packed) into a dynamic texture, discarding its old contents.
	bool write_layer(device *dev, const Layer &layer, const uint8_t *src, uint32_t w, uint32_t h)
	{
		subresource_data mapped;
		if (!dev->map_texture_region(layer.tex, 0, nullptr, map_access::write_discard, &mapped))
			return false;
		const size_t row = size_t(w) * 4;
		auto *dst = static_cast<uint8_t *>(mapped.data);
		if (mapped.row_pitch == row)
			std::memcpy(dst, src, row * h);
		else
			for (uint32_t y = 0; y < h; ++y)
				std::memcpy(dst + size_t(y) * mapped.row_pitch, src + y * row, row);
		dev->unmap_texture_region(layer.tex, 0);
		return true;
	}

	void bind(effect_runtime *runtime)
	{
		runtime->update_texture_bindings("MCWORLD", g_world.srv, g_world.srv);
		runtime->update_texture_bindings("MCDEPTH", g_depth.srv, g_depth.srv);
		runtime->update_texture_bindings("MCOVERLAY", g_overlay.srv, g_overlay.srv);
	}

	/// Upload the newest published Minecraft frame, if there is one we haven't shown yet.
	/// Where a frame's time goes (milliseconds, summed until the next report in the ReShade log).
	struct Timing
	{
		double map = 0, upload = 0, effects = 0, present = 0;
		int frames = 0, uploads = 0;
		LARGE_INTEGER lastPresent = {};
	} g_timing;

	double ms_since(const LARGE_INTEGER &start)
	{
		static LARGE_INTEGER freq = {};
		if (freq.QuadPart == 0)
			QueryPerformanceFrequency(&freq);
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		return double(now.QuadPart - start.QuadPart) * 1000.0 / double(freq.QuadPart);
	}

	void upload(effect_runtime *runtime)
	{
		const int64_t published = read<int64_t>(g_view + 32);
		if (published == g_lastPublish)
			return;
		const int32_t slot = read<int32_t>(g_view + 40);
		if (slot < 0 || slot >= g_slots)
			return;
		const uint8_t *desc = g_view + kSlotDesc + kSlotDescBytes * slot;
		const int64_t seq = read<int64_t>(desc);
		if (seq & 1)
			return;
		const uint32_t w = read<uint32_t>(desc + 24), h = read<uint32_t>(desc + 28);
		if (w == 0 || h == 0)
			return;
		device *dev = runtime->get_device();
		if (w != g_width || h != g_height)
		{
			destroy_layers(dev);
			if (!create_layer(dev, g_world, w, h, format::b8g8r8a8_unorm) ||
				!create_layer(dev, g_depth, w, h, format::r32_float) ||
				!create_layer(dev, g_overlay, w, h, format::b8g8r8a8_unorm))
			{
				destroy_layers(dev);
				return;
			}
			g_width = w;
			g_height = h;
			bind(runtime);
		}
		const size_t layer = size_t(w) * h * 4;
		const uint8_t *base = nullptr;
		LARGE_INTEGER t0;
		QueryPerformanceCounter(&t0);
		const uint8_t *view = map_range(kHeader + g_stride * slot, layer * 3, &base);
		g_timing.map += ms_since(t0);
		QueryPerformanceCounter(&t0);
		if (view == nullptr)
			return;
		write_layer(dev, g_world, base, w, h);
		write_layer(dev, g_depth, base + layer, w, h);
		write_layer(dev, g_overlay, base + 2 * layer, w, h);
		g_timing.upload += ms_since(t0);
		++g_timing.uploads;
		QueryPerformanceCounter(&t0);
		UnmapViewOfFile(view);
		g_timing.map += ms_since(t0);
		if (read<int64_t>(desc) != seq)
			return; // Minecraft rewrote the slot mid-copy: show the next one instead
		g_lastPublish = published;
		g_mcNear = read<float>(desc + 32);
		g_mcFar = read<float>(desc + 36);
		g_mcFlags = read<int32_t>(desc + 44);
		g_mcPose.fov = read<float>(desc + 40);
		g_mcPose.yaw = read<float>(desc + 72);
		g_mcPose.pitch = read<float>(desc + 76);
		g_mcPose.roll = read<float>(desc + 80);
		g_mcPose.x = read<double>(desc + 48);
		g_mcPose.y = read<double>(desc + 56);
		g_mcPose.z = read<double>(desc + 64);
		g_mcPose.valid = true;
		g_hasFrame = true;
	}

	/// Camera-to-world rotation, as Minecraft builds it: rotationYXZ(pi - yaw, -pitch, roll) (camera looks down -z).
	void camera_rotation(const Pose &p, float m[3][3])
	{
		const float d2r = 3.14159265f / 180.0f;
		const float a = 3.14159265f - p.yaw * d2r, b = -p.pitch * d2r, c = p.roll * d2r;
		const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cc = std::cos(c), sc = std::sin(c);
		// Ry(a) * Rx(b) * Rz(c)
		const float ry[3][3] = {{ca, 0, sa}, {0, 1, 0}, {-sa, 0, ca}};
		const float rx[3][3] = {{1, 0, 0}, {0, cb, -sb}, {0, sb, cb}};
		const float rz[3][3] = {{cc, -sc, 0}, {sc, cc, 0}, {0, 0, 1}};
		float t[3][3] = {};
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
				for (int k = 0; k < 3; ++k)
					t[i][j] += ry[i][k] * rx[k][j];
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
			{
				m[i][j] = 0;
				for (int k = 0; k < 3; ++k)
					m[i][j] += t[i][k] * rz[k][j];
			}
	}

	/// Rows of R_mc^T * R_host: turns a ray in FNV's camera space into Minecraft's camera space.
	void warp_matrix(const Pose &host, const Pose &mc, float out[3][3])
	{
		float rh[3][3], rm[3][3];
		camera_rotation(host, rh);
		camera_rotation(mc, rm);
		for (int i = 0; i < 3; ++i)
			for (int j = 0; j < 3; ++j)
			{
				out[i][j] = 0;
				for (int k = 0; k < 3; ++k)
					out[i][j] += rm[k][i] * rh[k][j];
			}
	}

	/// Reload VegasCraft.fx when the file changes (ReShade doesn't watch it), so tweaks don't need a FNV restart.
	void watch_effect_file(effect_runtime *runtime)
	{
		static DWORD next = 0;
		static FILETIME last = {};
		static wchar_t path[MAX_PATH] = {};
		if (GetTickCount() < next)
			return;
		next = GetTickCount() + 1000;
		if (path[0] == 0)
		{
			GetModuleFileNameW(nullptr, path, MAX_PATH);
			wchar_t *slash = wcsrchr(path, L'\\');
			if (slash)
				wcscpy(slash + 1, L"reshade-shaders\\Shaders\\VegasCraft.fx");
		}
		WIN32_FILE_ATTRIBUTE_DATA info;
		if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info))
			return;
		if (last.dwLowDateTime != 0 && CompareFileTime(&info.ftLastWriteTime, &last) != 0)
			runtime->reload_effect_next_frame(kEffect);
		last = info.ftLastWriteTime;
	}

	void on_present(effect_runtime *runtime)
	{
		watch_effect_file(runtime); // every frame, even when the effect failed to compile
		if (g_captureLeft.load() > 0 && g_captureDelay.load() > 0)
			--g_captureDelay;
		else if (g_captureLeft.load() > 0 && (g_captureFrame++ % std::max(g_captureEvery.load(), 1)) == 0)
		{
			char tag[32], line[320];
			snprintf(tag, sizeof(tag), "vc%03d", ++g_captureId);
			runtime->save_screenshot(tag);
			const UsedPoses &u = g_used;
			snprintf(line, sizeof(line), "VegasCraft capture %s host %.3f %.3f %.3f pos %.4f %.4f %.4f | minecraft %.3f %.3f %.3f pos %.4f %.4f %.4f | warp %d",
				tag, u.hy, u.hp, u.hf, u.hx, u.hyy, u.hz, u.my, u.mp, u.mf, u.mx, u.myy, u.mz, u.valid ? 1 : 0);
			reshade::log::message(reshade::log::level::info, line);
			{
				// the host poses of this frame and the two before: which one the picture really matches tells the pose lag
				std::lock_guard<std::mutex> lock(g_poseLock);
				for (unsigned back = 0; back < 3 && back < g_hostPoseCount; ++back)
				{
					const Pose &h = g_hostPoses[(g_hostPoseCount - 1 - back) & 3];
					snprintf(line, sizeof(line), "VegasCraft capture %s back %u host %.3f %.3f %.3f pos %.4f %.4f %.4f lag %d", tag, back, h.yaw, h.pitch, h.fov, h.x, h.y, h.z, g_poseLag.load());
					reshade::log::message(reshade::log::level::info, line);
				}
			}
			--g_captureLeft;
		}
		if (g_timing.lastPresent.QuadPart != 0)
			g_timing.present += ms_since(g_timing.lastPresent);
		QueryPerformanceCounter(&g_timing.lastPresent);
		if (++g_timing.frames >= 300)
		{
			const double n = g_timing.frames, u = std::max(g_timing.uploads, 1);
			char line[200];
			snprintf(line, sizeof(line), "VegasCraft timing: %.1f ms/frame (%.0f fps), effects %.2f ms/frame, upload %.2f ms + map %.2f ms per Minecraft frame (%d uploads), %ux%u",
				g_timing.present / n, 1000.0 * n / std::max(g_timing.present, 1.0), g_timing.effects / n, g_timing.upload / u, g_timing.map / u, g_timing.uploads, g_width, g_height);
			reshade::log::message(reshade::log::level::info, line);
			const LARGE_INTEGER keep = g_timing.lastPresent;
			g_timing = Timing();
			g_timing.lastPresent = keep;
		}
	}

	void on_begin_effects(effect_runtime *runtime, command_list *, resource_view, resource_view)
	{
		LARGE_INTEGER started;
		QueryPerformanceCounter(&started);
		struct AddTime
		{
			LARGE_INTEGER &from;
			~AddTime() { g_timing.effects += ms_since(from); }
		} addTime{started};
		uint32_t bw = 0, bh = 0;
		runtime->get_screenshot_width_and_height(&bw, &bh);
		g_bbWidth = bw;
		g_bbHeight = bh;
		bool on = g_active && open_mapping();
		if (on)
			upload(runtime);
		on = on && g_hasFrame;
		// The technique stays enabled (preset); McActive gates it, so FNV passes through untouched until a
		// Minecraft frame is here. (Toggling techniques from inside this callback crashes ReShade.)
		if (const effect_uniform_variable v = find_uniform(runtime, kEffect, "McActive"); v.handle != 0)
			runtime->set_uniform_value_bool(v, on);
		if (!on)
			return;
		if (const int view = g_debugView.load(); view >= 0)
			if (const effect_uniform_variable v = find_uniform(runtime, kEffect, "DebugView"); v.handle != 0)
				runtime->set_uniform_value_int(v, view);
		if (const effect_uniform_variable v = find_uniform(runtime, kEffect, "McPlanes"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_mcNear, g_mcFar, float(g_mcFlags));
		// a scene's look overrides the preset's light matching and depth bias; the preset's values come back after
		auto look = [&](const char *name, float want, float &saved) {
			const effect_uniform_variable v = find_uniform(runtime, kEffect, name);
			if (v.handle == 0)
				return;
			if (want >= 0.0f)
			{
				if (saved < 0.0f)
					runtime->get_uniform_value_float(v, &saved, 1);
				runtime->set_uniform_value_float(v, want);
			}
			else if (saved >= 0.0f)
			{
				runtime->set_uniform_value_float(v, saved);
				saved = -1.0f;
			}
		};
		look("LightMatch", g_lookLight.load(), g_savedLight);
		look("DepthBias", g_lookBias.load(), g_savedBias);
		look("SlopeBias", g_lookSlope.load(), g_savedSlope);
		if (const effect_uniform_variable v = find_uniform(runtime, kEffect, "Shake"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_shakeX.load(), g_shakeY.load(), g_shakeRoll.load());
		if (const effect_uniform_variable v = find_uniform(runtime, kEffect, "PortalWarp"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_portalWarp.load());
		if (const effect_uniform_variable v = find_uniform(runtime, kEffect, "HostPlanes"); v.handle != 0)
			runtime->set_uniform_value_float(v, g_hostNear.load(), g_hostFar.load());

		// Re-projection from Minecraft's pose to FNV's latest (extrapolated by the effect's PosePrediction frames).
		Pose host, prev;
		{
			std::lock_guard<std::mutex> lock(g_poseLock);
			const unsigned lag = unsigned(std::clamp(g_poseLag.load(), 0, 2));
			if (g_hostPoseCount > lag)
				host = g_hostPoses[(g_hostPoseCount - 1 - lag) & 3];
			if (g_hostPoseCount > lag + 1)
				prev = g_hostPoses[(g_hostPoseCount - 2 - lag) & 3];
		}
		float predict = 0.0f;
		if (const effect_uniform_variable v = find_uniform(runtime, kEffect, "PosePrediction"); v.handle != 0)
			runtime->get_uniform_value_float(v, &predict, 1);
		const bool warp = host.valid && g_mcPose.valid && !g_cameraLocked;
		g_used = {host.yaw, host.pitch, host.fov, g_mcPose.yaw, g_mcPose.pitch, g_mcPose.fov, host.x, host.y, host.z, g_mcPose.x, g_mcPose.y, g_mcPose.z, warp};
		if (warp && prev.valid && predict != 0.0f)
		{
			auto delta = [](float a, float b) { float d = std::fmod(a - b + 540.0f, 360.0f) - 180.0f; return d; };
			host.yaw += delta(host.yaw, prev.yaw) * predict;
			host.pitch += (host.pitch - prev.pitch) * predict;
			host.roll += (host.roll - prev.roll) * predict;
			host.x += (host.x - prev.x) * predict;
			host.y += (host.y - prev.y) * predict;
			host.z += (host.z - prev.z) * predict;
		}
		float m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
		float t[3] = {0, 0, 0};
		if (warp)
		{
			warp_matrix(host, g_mcPose, m);
			// T = R_mc^T (host position - Minecraft's camera position), in Minecraft's camera space
			float rm[3][3];
			camera_rotation(g_mcPose, rm);
			const float d[3] = {float(host.x - g_mcPose.x), float(host.y - g_mcPose.y), float(host.z - g_mcPose.z)};
			for (int i = 0; i < 3; ++i)
				t[i] = rm[0][i] * d[0] + rm[1][i] * d[1] + rm[2][i] * d[2];
		}
		const char *rows[3] = {"WarpRow0", "WarpRow1", "WarpRow2"};
		for (int i = 0; i < 3; ++i)
			if (const effect_uniform_variable v = find_uniform(runtime, kEffect, rows[i]); v.handle != 0)
				runtime->set_uniform_value_float(v, m[i][0], m[i][1], m[i][2]);
		if (const effect_uniform_variable v = find_uniform(runtime, kEffect, "WarpT"); v.handle != 0)
			runtime->set_uniform_value_float(v, t[0], t[1], t[2]);
		const float d2r = 3.14159265f / 180.0f;
		const float tanHost = std::tan((warp ? host.fov : g_mcPose.fov) * d2r * 0.5f), tanMc = std::tan(g_mcPose.fov * d2r * 0.5f);
		if (const effect_uniform_variable v = find_uniform(runtime, kEffect, "WarpTan"); v.handle != 0)
			runtime->set_uniform_value_float(v, tanHost, tanMc, float(g_width) / float(g_height));
	}

	void on_reloaded_effects(effect_runtime *runtime)
	{
		if (g_width != 0)
			bind(runtime);
	}

	void on_destroy_effect_runtime(effect_runtime *runtime)
	{
		destroy_layers(runtime->get_device());
		close_mapping();
	}
}

namespace compositor
{
	bool try_register(void *module)
	{
		if (g_registered)
			return true;
		if (!reshade::register_addon(module))
			return false;
		reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
		reshade::register_event<reshade::addon_event::reshade_present>(on_present);
		reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(on_reloaded_effects);
		reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy_effect_runtime);
		g_registered = true;
		reshade::log::message(reshade::log::level::info, "VegasCraft: registered");
		return true;
	}

	void unregister(void *module)
	{
		if (g_registered.exchange(false))
			reshade::unregister_addon(module);
	}

	void set_active(bool active)
	{
		g_active = active;
	}

	void set_host_planes(float near_clip, float far_clip)
	{
		g_hostNear = near_clip;
		g_hostFar = far_clip;
	}

	void backbuffer_size(int &width, int &height)
	{
		width = int(g_bbWidth.load());
		height = int(g_bbHeight.load());
	}

	void set_camera_locked(bool locked)
	{
		g_cameraLocked = locked;
	}

	void set_screen_fx(float shake_x, float shake_y, float shake_roll, float portal_warp)
	{
		g_shakeX = shake_x;
		g_shakeY = shake_y;
		g_shakeRoll = shake_roll;
		g_portalWarp = portal_warp;
	}

	void set_look(float light_match, float depth_bias, float slope_bias)
	{
		g_lookLight = light_match;
		g_lookBias = depth_bias;
		g_lookSlope = slope_bias;
	}

	void set_host_pose(float yaw, float pitch, float roll, float fov, double x, double y, double z)
	{
		std::lock_guard<std::mutex> lock(g_poseLock);
		g_hostPoses[g_hostPoseCount & 3] = {yaw, pitch, roll, fov, x, y, z, true};
		++g_hostPoseCount;
	}

	void request_capture(int count, int every, int delay)
	{
		g_captureEvery = every;
		g_captureDelay = delay;
		g_captureFrame = 0;
		g_captureLeft = count;
	}

	int pose_lag()
	{
		return g_poseLag.load();
	}

	void cycle_debug_view()
	{
		g_debugView = (g_debugView.load() + 1) % 4;
	}

	void set_pose_lag(int frames)
	{
		g_poseLag = frames;
	}
}
