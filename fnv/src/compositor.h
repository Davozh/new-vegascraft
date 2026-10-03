// ReShade add-on half: uploads Minecraft's latest frame (Z:\dev\shm\VegasCraftFrame, the file a native Minecraft
// writes in /dev/shm) into textures that VegasCraft.fx composites into FNV's picture against FNV's depth buffer.
// Adapted from universal-modder's minecraft-gta5-passthrough (MIT).
#pragma once

namespace compositor
{
	/// Registers with ReShade once it is loaded; call until it returns true.
	bool try_register(void *module);
	void unregister(void *module);
	/// Whether to composite this frame (the script turns it off in menus and cutscenes).
	void set_active(bool active);
	/// FNV's camera clip planes in metres, to turn its depth buffer into metres.
	void set_host_planes(float near_clip, float far_clip);
	/// FNV's current camera in Minecraft's convention (rotation in degrees, vertical fov, position in Minecraft
	/// coordinates): Minecraft's frame is re-projected (with its depth) from the pose it was rendered with to this
	/// one, hiding the link's latency on camera turns and moves.
	void set_host_pose(float yaw, float pitch, float roll, float fov, double x, double y, double z);
	/// How many poses back the presented picture is (1: the script reads the camera of the frame being prepared).
	void set_pose_lag(int frames);
	/// Minecraft's picture moves with the camera (the flight chase cam frames Steve): composite it as rendered,
	/// without re-projecting it to FNV's newer pose, so Steve stays exactly where the camera framed him.
	void set_camera_locked(bool locked);
	/// A scene's look: the effect's "Match FNV lighting", depth bias and slope bias (negative: the preset's own values).
	void set_look(float light_match, float depth_bias, float slope_bias);
	/// The camera shake, applied to the finished picture (x, y: fraction of the screen height; roll: radians), and the
	/// nether portal's warp (0..1).
	void set_screen_fx(float shake_x, float shake_y, float shake_roll, float portal_warp);
	/// FNV's backbuffer size as ReShade sees it (0 until the first frame).
	void backbuffer_size(int &width, int &height);
}
