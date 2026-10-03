// Fallout: New Vegas 1.4.0.525 (Steam) engine layouts and functions this plugin uses. Addresses and offsets as
// documented by the community (JIP LN NVSE's and xNVSE's headers). Mostly reads; it also calls two game functions
// (terrain height, the physics ray query) and sets the player's disabled-controls flags.
#pragma once
#include <cstdint>
#include <initializer_list>
#include <new>

namespace fnv
{
	// 1 game unit = 1/70 m (Bethesda's scale: a 128-unit player is ~1.83 m)
	constexpr float kUnitsPerMetre = 70.0f;

	struct Vec3
	{
		float x, y, z;
	};

	/// NiAVObject world transform: rotation (row-major 3x3) at 0x68, translation at 0x8C, scale at 0x98.
	/// NiCamera looks down its local +X, with +Y up and +Z right; its NiFrustum (left, right, top, bottom, near, far,
	/// ortho) is at 0xDC, left/right/top/bottom being tangents (the frustum at distance 1).
	struct Camera
	{
		float rot[3][3];
		Vec3 pos;
		float left, right, top, bottom, nearPlane, farPlane;
		float worldToCam[4][4]; // 0x9C: what the engine projects with (WorldToScreen)
	};

	inline uintptr_t sceneGraph() { return *reinterpret_cast<uintptr_t *>(0x11DEB7C); }
	inline uintptr_t player() { return *reinterpret_cast<uintptr_t *>(0x11DEA3C); }

	inline bool readCamera(Camera &out)
	{
		const uintptr_t sg = sceneGraph();
		if (sg == 0)
			return false;
		const uintptr_t cam = *reinterpret_cast<uintptr_t *>(sg + 0xAC);
		if (cam == 0)
			return false;
		const float *r = reinterpret_cast<const float *>(cam + 0x68);
		for (int i = 0; i < 9; ++i)
			out.rot[i / 3][i % 3] = r[i];
		out.pos = *reinterpret_cast<const Vec3 *>(cam + 0x8C);
		const float *f = reinterpret_cast<const float *>(cam + 0xDC);
		out.left = f[0];
		out.right = f[1];
		out.top = f[2];
		out.bottom = f[3];
		out.nearPlane = f[4];
		out.farPlane = f[5];
		const float *w = reinterpret_cast<const float *>(cam + 0x9C);
		for (int i = 0; i < 16; ++i)
			out.worldToCam[i / 4][i % 4] = w[i];
		return true;
	}

	inline uintptr_t tes() { return *reinterpret_cast<uintptr_t *>(0x11DEA10); }

	/// TES::currentInterior (0x34): null outdoors.
	inline bool inInterior()
	{
		const uintptr_t t = tes();
		return t != 0 && *reinterpret_cast<const uintptr_t *>(t + 0x34) != 0;
	}

	/// TES::GetTerrainHeight (0x4572E0): the landscape's height at x, y (exteriors, loaded cells only).
	inline bool terrainHeight(float x, float y, float &z)
	{
		using Fn = bool(__attribute__((thiscall)) *)(uintptr_t, const float *, float *);
		const uintptr_t t = tes();
		if (t == 0)
			return false;
		const float xy[2] = {x, y};
		return reinterpret_cast<Fn>(0x4572E0)(t, xy, &z);
	}

	/// Havok's ray query as the game makes it (TES::PickObject, 0x458440), set up the way JIP LN's GetRayCastPos does.
	/// Havok works in game units / 7.
	struct alignas(16) RayCastData
	{
		float from[4];       // 00
		float to[4];         // 10
		uint8_t byte20;      // 20
		uint8_t pad21[3];
		uint32_t filter;     // 24: collision layer (low byte) and the caster's group (high half)
		uint32_t unk28[6];
		float hitFraction;   // 40: 1 = nothing hit
		uint32_t unk44[15];
		void *cdBody;        // 80
		uint32_t unk84[3];
		float vector90[4];   // 90
		uint32_t unkA0[3];
		uint8_t byteAC;
		uint8_t padAD[3];
	};
	static_assert(sizeof(RayCastData) == 0xB0, "RayCastData layout");

	constexpr float kHavokScale = 1.0f / 7.0f;
	constexpr uint32_t kLayerProjectile = 6; // what a projectile hits: terrain, statics, actors

	/// The player's collision group, so a ray from inside the player's capsule doesn't hit the player.
	inline uint32_t playerCollisionGroup()
	{
		uintptr_t p = player();
		for (uintptr_t offset : {0x68u, 0x138u, 0x594u, 0x8u})
		{
			if (p == 0)
				return 0;
			p = *reinterpret_cast<const uintptr_t *>(p + offset);
		}
		return p != 0 ? *reinterpret_cast<const uint32_t *>(p + 0x2C) & 0xFFFF0000u : 0;
	}

	/// Casts a ray from `from` along `dir` (unit vector) up to `range` game units. Main thread only.
	inline bool rayCast(const Vec3 &from, const Vec3 &dir, float range, Vec3 &hit, uint32_t layer = kLayerProjectile)
	{
		const uintptr_t t = tes();
		if (t == 0)
			return false;
		// Havok reads it with aligned SSE loads, and FNV calls the plugin with a stack only 4-byte aligned: align by hand
		alignas(16) uint8_t storage[sizeof(RayCastData) + 16];
		RayCastData &rc = *new (reinterpret_cast<void *>((reinterpret_cast<uintptr_t>(storage) + 15) & ~uintptr_t(15))) RayCastData();
		rc.from[0] = from.x * kHavokScale;
		rc.from[1] = from.y * kHavokScale;
		rc.from[2] = from.z * kHavokScale;
		rc.to[0] = (from.x + dir.x * range) * kHavokScale;
		rc.to[1] = (from.y + dir.y * range) * kHavokScale;
		rc.to[2] = (from.z + dir.z * range) * kHavokScale;
		rc.hitFraction = 1.0f;
		rc.unk44[0] = 0xFFFFFFFFu; // 44
		rc.unk44[3] = 0xFFFFFFFFu; // 50
		rc.filter = playerCollisionGroup() | (layer & 0x7F);
		using Fn = void *(__attribute__((thiscall)) *)(uintptr_t, RayCastData *, int);
		reinterpret_cast<Fn>(0x458440)(t, &rc, 1);
		if (!(rc.hitFraction < 1.0f))
			return false;
		hit = {from.x + dir.x * range * rc.hitFraction, from.y + dir.y * range * rc.hitFraction, from.z + dir.z * range * rc.hitFraction};
		return true;
	}

	/// PlayerCharacter::disabledControlFlags (0x680), the flags DisablePlayerControls sets.
	constexpr uint8_t kControlFight = 1 << 3;
	inline uint8_t &disabledControls() { return *reinterpret_cast<uint8_t *>(player() + 0x680); }

	/// InterfaceManager::currentMode (0x0C): 1 = game, 2 = a menu is open (Pip-Boy, dialogue, messages, pause).
	inline bool inMenu()
	{
		const uintptr_t im = *reinterpret_cast<uintptr_t *>(0x11D8A80);
		return im != 0 && *reinterpret_cast<const uint32_t *>(im + 0x0C) > 1;
	}

	/// TESObjectREFR::parentCell (0x40): null until the player is placed in the world (main menu, loading).
	inline bool playerInWorld()
	{
		const uintptr_t p = player();
		return p != 0 && *reinterpret_cast<const uintptr_t *>(p + 0x40) != 0;
	}

	/// TESObjectREFR: rotation (radians; z = heading, clockwise from north) at 0x24, position at 0x30.
	inline bool readPlayer(Vec3 &pos, Vec3 &rot, bool &thirdPerson)
	{
		const uintptr_t p = player();
		if (p == 0)
			return false;
		rot = *reinterpret_cast<const Vec3 *>(p + 0x24);
		pos = *reinterpret_cast<const Vec3 *>(p + 0x30);
		thirdPerson = *reinterpret_cast<const uint8_t *>(p + 0x64A) != 0;
		return true;
	}
}
