// The few pieces of xNVSE's plugin API this plugin uses (layouts from xNVSE's PluginAPI.h, interface version 4).
#pragma once
#include <cstdint>

using PluginHandle = uint32_t;

struct PluginInfo
{
	enum { kInfoVersion = 1 };
	uint32_t infoVersion;
	const char *name;
	uint32_t version;
};

struct NVSEInterface
{
	uint32_t nvseVersion;
	uint32_t runtimeVersion;
	uint32_t editorVersion;
	uint32_t isEditor;
	void *RegisterCommand;
	void *SetOpcodeBase;
	void *(*QueryInterface)(uint32_t id);
	PluginHandle (*GetPluginHandle)();
};

struct NVSEMessagingInterface
{
	struct Message
	{
		const char *sender;
		uint32_t type;
		uint32_t dataLen;
		void *data;
	};
	using EventCallback = void (*)(Message *msg);

	enum
	{
		kMessage_PostLoad = 0,
		kMessage_ExitGame = 1,
		kMessage_PostLoadGame = 8,
		kMessage_NewGame = 14,
		kMessage_DeferredInit = 18,
		kMessage_MainGameLoop = 20,
		kMessage_OnFramePresent = 24, // data: int*, nonzero on a loading screen
	};

	uint32_t version;
	bool (*RegisterListener)(PluginHandle listener, const char *sender, EventCallback handler);
	bool (*Dispatch)(PluginHandle sender, uint32_t messageType, void *data, uint32_t dataLen, const char *receiver);
};

constexpr uint32_t kInterface_Messaging = 2;
constexpr uint32_t kRuntime_1_4_0_525 = 0x040020D0;
