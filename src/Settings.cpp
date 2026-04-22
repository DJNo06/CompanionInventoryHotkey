#include "Settings.h"
#include "Hotkey.h"

#include <Windows.h>

#include <REX/REX.h>

#include <filesystem>
#include <string_view>
#include <cstdint>
#include <thread>
#include <sstream>
#include <iomanip>

namespace
{
	std::filesystem::path GetGameDataDir()
	{
		wchar_t buf[MAX_PATH]{};
		const DWORD n = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
		if (n == 0 || n >= MAX_PATH) {
			return {};
		}

		const std::filesystem::path gameDir = std::filesystem::path(buf).parent_path();
		return gameDir / L"Data";
	}

	std::uint32_t ReadUIntW(const wchar_t* section, const wchar_t* key, std::uint32_t def, const wchar_t* path)
	{
		return static_cast<std::uint32_t>(::GetPrivateProfileIntW(section, key, static_cast<int>(def), path));
	}

	std::uint32_t MapMcmIndexToVK(std::uint32_t idx, std::uint32_t defVK)
	{
		switch (idx) {
		case 0:  return 0x70; // F1
		case 1:  return 0x71; // F2
		case 2:  return 0x72; // F3
		case 3:  return 0x73; // F4
		case 4:  return 0x75; // F6
		case 5:  return 0x76; // F7
		case 6:  return 0x77; // F8
		case 7:  return 0x79; // F10
		case 8:  return 0x7A; // F11
		case 9:  return 0x7B; // F12
		case 10: return 0x54; // T
		case 11: return 0x59; // Y
		case 12: return 0x55; // U
		case 13: return 0x50; // P
		case 14: return 0x47; // G
		case 15: return 0x48; // H
		case 16: return 0x4C; // L
		case 17: return 0x56; // V
		case 18: return 0x42; // B
		case 19: return 0x4E; // N
		default: return defVK;
		}
	}

	std::filesystem::path GetIniPath()
	{
		const auto dataDir = GetGameDataDir();
		if (dataDir.empty()) {
			return {};
		}
		return dataDir / L"MCM" / L"Settings" / L"CompanionInventoryHotkey.ini";
	}

	std::uint32_t LoadHotkeyVK(bool logIfMissing)
	{
		const std::uint32_t defVK = 0x75; // F6

		const auto ini = GetIniPath();
		if (ini.empty()) {
			REX::WARN("Settings: cannot resolve MCM ini path -> using default VK=0x{:02X}", defVK);
			return defVK;
		}

		if (!std::filesystem::exists(ini)) {
			if (logIfMissing) {
				REX::INFO("Settings: MCM ini not found -> using default VK=0x{:02X}", defVK);
			}
			return defVK;
		}

		const auto iniW = ini.c_str();
		const std::uint32_t idx = ReadUIntW(L"Main", L"iHotkey", 4, iniW); // défaut index=4 (F6)
		return MapMcmIndexToVK(idx, defVK);
	}

	std::string VKToName(std::uint32_t vk)
	{
		UINT scan = ::MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);

		switch (vk) {
		case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
		case VK_PRIOR: case VK_NEXT:
		case VK_END: case VK_HOME:
		case VK_INSERT: case VK_DELETE:
		case VK_DIVIDE: case VK_NUMLOCK:
		case VK_RCONTROL: case VK_RMENU:
			scan |= 0xE000;
			break;
		default:
			break;
		}

		char name[128]{};
		const LONG lparam = static_cast<LONG>(scan << 16);

		if (::GetKeyNameTextA(lparam, name, static_cast<int>(sizeof(name))) > 0) {
			return std::string(name);
		}

		if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
			return std::string(1, static_cast<char>(vk));
		}

		return "Unknown";
	}
}

namespace Settings
{
	namespace
	{
		std::atomic_bool g_watchRunning{ false };
		std::thread      g_watchThread;

		inline bool IEqualsFileName(std::wstring_view a, std::wstring_view b)
		{
			if (a.size() != b.size()) {
				return false;
			}
			for (std::size_t i = 0; i < a.size(); ++i) {
				wchar_t ca = a[i];
				wchar_t cb = b[i];
				if (ca >= L'A' && ca <= L'Z') ca = static_cast<wchar_t>(ca - L'A' + L'a');
				if (cb >= L'A' && cb <= L'Z') cb = static_cast<wchar_t>(cb - L'A' + L'a');
				if (ca != cb) {
					return false;
				}
			}
			return true;
		}

		void ApplyVK(std::uint32_t vk, bool logChange)
		{
			const auto current = HotkeyVK.load(std::memory_order_relaxed);
			if (vk == current) {
				return;
			}

			HotkeyVK.store(vk, std::memory_order_relaxed);
			Hotkey::SetVK(vk);

			if (logChange) {
				REX::INFO("Settings: hotkey updated -> {} (VK=0x{:02X})", VKToName(vk), vk);
			}
		}

		void WatchThreadMain(std::filesystem::path dirPath)
		{
			const HANDLE hDir = ::CreateFileW(
				dirPath.c_str(),
				FILE_LIST_DIRECTORY,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
				nullptr,
				OPEN_EXISTING,
				FILE_FLAG_BACKUP_SEMANTICS,
				nullptr);

			if (hDir == INVALID_HANDLE_VALUE) {
				REX::WARN("Settings watcher: CreateFileW failed (err={})", ::GetLastError());
				return;
			}

			BYTE buffer[8 * 1024]{};
			DWORD bytesReturned = 0;

			constexpr std::wstring_view target = L"CompanionInventoryHotkey.ini";

			while (g_watchRunning.load(std::memory_order_relaxed)) {
				const BOOL ok = ::ReadDirectoryChangesW(
					hDir,
					buffer,
					static_cast<DWORD>(sizeof(buffer)),
					FALSE,
					FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME,
					&bytesReturned,
					nullptr,
					nullptr);

				if (!ok) {
					REX::WARN("Settings watcher: ReadDirectoryChangesW failed (err={})", ::GetLastError());
					break;
				}

				auto* p = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer);
				for (;;) {
					const std::wstring_view name(p->FileName, p->FileNameLength / sizeof(wchar_t));

					if (IEqualsFileName(name, target)) {
						switch (p->Action) {
						case FILE_ACTION_MODIFIED:
						case FILE_ACTION_ADDED:
						case FILE_ACTION_RENAMED_NEW_NAME:
							::Sleep(50);
							ApplyVK(LoadHotkeyVK(false), true);
							break;
						default:
							break;
						}
					}

					if (p->NextEntryOffset == 0) {
						break;
					}
					p = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
						reinterpret_cast<BYTE*>(p) + p->NextEntryOffset);
				}
			}

			::CloseHandle(hDir);
		}
	}

	void Load()
	{
		ApplyVK(LoadHotkeyVK(true), true);
	}

	void StartWatcher()
	{
		bool expected = false;
		if (!g_watchRunning.compare_exchange_strong(expected, true)) {
			return;
		}

		const auto ini = GetIniPath();
		if (ini.empty()) {
			REX::WARN("Settings watcher: MCM ini path not resolved -> disabled");
			g_watchRunning.store(false, std::memory_order_relaxed);
			return;
		}

		const auto dir = ini.parent_path();

		g_watchThread = std::thread([dir]() {
			WatchThreadMain(dir);
			g_watchRunning.store(false, std::memory_order_relaxed);
			});
		g_watchThread.detach();
	}

	void StopWatcher()
	{
		g_watchRunning.store(false, std::memory_order_relaxed);
	}
}