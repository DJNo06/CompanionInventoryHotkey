#pragma once

#include <atomic>
#include <cstdint>

namespace Settings
{
	inline std::atomic_uint32_t HotkeyVK{ 0x75 };

	void Load();

	void StartWatcher();
	void StopWatcher();
}