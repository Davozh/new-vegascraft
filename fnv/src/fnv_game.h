// Fallout: New Vegas 1.4.0.525 (Steam) engine layouts this plugin reads. Addresses and offsets as documented by the
// community (JIP LN NVSE's headers); nothing is called, only read.
#pragma once
#include <cstdint>

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
		return true;
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
