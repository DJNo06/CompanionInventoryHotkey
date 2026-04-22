#include "Hotkey.h"

#include "Companion.h"
#include "OpenInventory.h"

#include <F4SE/Interfaces.h>

#include <RE/B/BSFixedString.h>
#include <RE/B/ButtonEvent.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/P/PlayerControls.h>
#include <RE/R/ReadyWeaponHandler.h>
#include <RE/T/TESBoundObject.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/U/UI.h>

#include <REX/REX.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

namespace Hotkey
{
	namespace
	{
		std::atomic_uint32_t g_vk{ 0x75 };
		std::atomic_bool     g_running{ false };
		std::atomic_bool     g_inputHooksInstalled{ false };
		std::thread          g_thread;

		constexpr std::uint32_t kBankVk = 0xBA;                    // $
		constexpr std::uint32_t kWorkshopRefID = 0x000250FE;      // Sanctuary workshop
		constexpr std::uint32_t kCapsFormID = 0x0000000F;         // Caps
		constexpr std::uint32_t kPreWarMoneyFormID = 0x00059B02;  // Pre-War Money

		constexpr std::uint32_t kTransferChunk = 60000;           // caps : sécurité
		constexpr std::uint32_t kMoneyConvertChunk = 6000;        // 6000 money -> 60000 caps max

		// Empirically, slot 1 is the QCOpenTransferMenu-related button callback
		// for the player input handlers we are inspecting.
		constexpr std::size_t kInputHandlerQCSlot = 1;

		// TEST MODE:
		// Observation only for now.
		constexpr bool kBlockReadyWeaponQCOpenTransferMenuOriginal = false;
		constexpr bool kBlockActivateQCOpenTransferMenuOriginal = false;
		constexpr bool kBlockOtherQCOpenTransferMenuOriginal = false;
		constexpr std::size_t kMaxHookedHandlers = 24;
		constexpr std::size_t kMaxHookedVTables = 24;

		inline bool IsDown(int vk) noexcept
		{
			return (GetAsyncKeyState(vk) & 0x8000) != 0;
		}

		inline bool IsCtrlDown() noexcept
		{
			return IsDown(VK_LCONTROL) || IsDown(VK_RCONTROL);
		}

		inline bool IsShiftDown() noexcept
		{
			return IsDown(VK_LSHIFT) || IsDown(VK_RSHIFT);
		}

		inline bool IsCompanionInputAllowedOnGameThread() noexcept
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return true;
			}

			if (ui->freezeFramePause != 0) {
				return false;
			}

			static const RE::BSFixedString kConsole("Console");
			static const RE::BSFixedString kPipboy("PipboyMenu");
			static const RE::BSFixedString kPause("PauseMenu");

			if (ui->GetMenuOpen(kConsole) ||
				ui->GetMenuOpen(kPipboy) ||
				ui->GetMenuOpen(kPause)) {
				return false;
			}

			return true;
		}

		inline bool IsBankInputAllowedOnGameThread() noexcept
		{
			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				return true;
			}

			if (ui->freezeFramePause != 0) {
				return false;
			}

			static const RE::BSFixedString kConsole("Console");
			static const RE::BSFixedString kPause("PauseMenu");

			if (ui->GetMenuOpen(kConsole) || ui->GetMenuOpen(kPause)) {
				return false;
			}

			return true;
		}

		RE::TESObjectREFR* GetWorkshop() noexcept
		{
			auto* form = RE::TESForm::GetFormByID(kWorkshopRefID);
			if (!form) {
				REX::WARN("Hotkey: workshop form not found");
				return nullptr;
			}

			auto* ref = form->As<RE::TESObjectREFR>();
			if (!ref) {
				REX::WARN("Hotkey: workshop form is not a reference");
				return nullptr;
			}

			return ref;
		}

		RE::TESBoundObject* GetCapsObject() noexcept
		{
			auto* form = RE::TESForm::GetFormByID(kCapsFormID);
			if (!form) {
				REX::WARN("Hotkey: caps form not found");
				return nullptr;
			}

			auto* obj = form->As<RE::TESBoundObject>();
			if (!obj) {
				REX::WARN("Hotkey: caps form is not a bound object");
				return nullptr;
			}

			return obj;
		}

		RE::TESBoundObject* GetPreWarMoneyObject() noexcept
		{
			auto* form = RE::TESForm::GetFormByID(kPreWarMoneyFormID);
			if (!form) {
				REX::WARN("Hotkey: pre-war money form not found");
				return nullptr;
			}

			auto* obj = form->As<RE::TESBoundObject>();
			if (!obj) {
				REX::WARN("Hotkey: pre-war money form is not a bound object");
				return nullptr;
			}

			return obj;
		}

		std::uint32_t GetItemCountSafe(RE::TESObjectREFR* container, RE::TESForm* item) noexcept
		{
			if (!container || !item) {
				return 0;
			}

			std::uint32_t count = 0;
			if (!container->GetItemCount(count, item, false)) {
				return 0;
			}

			return count;
		}

		void WritePointer(void* address, const void* value) noexcept
		{
			DWORD oldProtect = 0;
			if (VirtualProtect(address, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
				std::memcpy(address, &value, sizeof(void*));
				VirtualProtect(address, sizeof(void*), oldProtect, &oldProtect);
			}
		}

		struct HookedHandler
		{
			const char* name{ nullptr };
			void*       instance{ nullptr };
			bool        blockOriginal{ false };
		};

		struct HookedVTable
		{
			void** vtbl{ nullptr };
			using func_t = void(*)(void*, const RE::ButtonEvent*);
			func_t original{ nullptr };
		};

		std::array<HookedHandler, kMaxHookedHandlers> g_hookedHandlers{};
		std::array<HookedVTable, kMaxHookedVTables>   g_hookedVTables{};
		std::size_t                                   g_hookedHandlerCount = 0;
		std::size_t                                   g_hookedVTableCount = 0;

		HookedHandler* FindHookedHandler(void* a_instance) noexcept
		{
			for (std::size_t i = 0; i < g_hookedHandlerCount; ++i) {
				if (g_hookedHandlers[i].instance == a_instance) {
					return std::addressof(g_hookedHandlers[i]);
				}
			}

			return nullptr;
		}

		HookedVTable* FindHookedVTable(void** a_vtbl) noexcept
		{
			for (std::size_t i = 0; i < g_hookedVTableCount; ++i) {
				if (g_hookedVTables[i].vtbl == a_vtbl) {
					return std::addressof(g_hookedVTables[i]);
				}
			}

			return nullptr;
		}

		void RegisterHookedHandler(const char* a_name, void* a_instance, bool a_blockOriginal) noexcept
		{
			if (!a_instance || FindHookedHandler(a_instance)) {
				return;
			}

			if (g_hookedHandlerCount >= g_hookedHandlers.size()) {
				REX::WARN("Hotkey: handler registry full, cannot register {}", a_name ? a_name : "unknown");
				return;
			}

			g_hookedHandlers[g_hookedHandlerCount++] = HookedHandler{ a_name, a_instance, a_blockOriginal };
		}

		void RegisterHookedVTable(void** a_vtbl, HookedVTable::func_t a_original) noexcept
		{
			if (!a_vtbl || FindHookedVTable(a_vtbl)) {
				return;
			}

			if (g_hookedVTableCount >= g_hookedVTables.size()) {
				REX::WARN("Hotkey: vtable registry full, cannot register {:p}", static_cast<void*>(a_vtbl));
				return;
			}

			g_hookedVTables[g_hookedVTableCount++] = HookedVTable{ a_vtbl, a_original };
		}

		struct GenericInputHook
		{
			using event_t = RE::ButtonEvent;

			static void Thunk(void* a_this, const event_t* a_event)
			{
				bool        blockOriginal = false;
				const char* handlerName = "Unknown";

				if (auto* handler = FindHookedHandler(a_this); handler) {
					handlerName = handler->name ? handler->name : handlerName;
					blockOriginal = handler->blockOriginal;
				}

				if (a_event && a_event->QJustPressed()) {
					const auto& action = a_event->QUserEvent();

					if (!action.empty() && action == "QCOpenTransferMenu") {
						REX::INFO(
							"[INPUT][{}] slot={} action='{}' value={} held={} handler={:p}",
							handlerName,
							kInputHandlerQCSlot,
							action.c_str(),
							a_event->value,
							a_event->heldDownSecs,
							a_this);

						if (blockOriginal) {
							REX::INFO("Hotkey(test): blocking {} original QCOpenTransferMenu", handlerName);
						}
					}
				}

				auto** vtbl = a_this ? *reinterpret_cast<void***>(a_this) : nullptr;
				auto*  hook = FindHookedVTable(vtbl);
				if (!blockOriginal && hook && hook->original) {
					hook->original(a_this, a_event);
				}
			}
		};

		void InstallNamedHandlerHook(const char* a_name, void* a_handler, bool a_blockOriginal)
		{
			if (!a_handler) {
				return;
			}

			RegisterHookedHandler(a_name, a_handler, a_blockOriginal);

			auto** vtbl = *reinterpret_cast<void***>(a_handler);
			if (!vtbl) {
				REX::WARN("Hotkey: {} vtable not resolved", a_name ? a_name : "unknown");
				return;
			}

			if (!FindHookedVTable(vtbl)) {
				auto original =
					reinterpret_cast<HookedVTable::func_t>(vtbl[kInputHandlerQCSlot]);
				RegisterHookedVTable(vtbl, original);

				WritePointer(
					std::addressof(vtbl[kInputHandlerQCSlot]),
					reinterpret_cast<const void*>(GenericInputHook::Thunk));
			}

			REX::INFO(
				"Hotkey: hooked {} handler={:p} vtbl={:p} slot={} block={}",
				a_name ? a_name : "unknown",
				a_handler,
				static_cast<void*>(vtbl),
				kInputHandlerQCSlot,
				a_blockOriginal);
		}

		void InstallPlayerControlsInputHooks()
		{
			bool expected = false;
			if (!g_inputHooksInstalled.compare_exchange_strong(expected, true)) {
				return;
			}

			auto* controls = RE::PlayerControls::GetSingleton();
			if (!controls) {
				REX::WARN("Hotkey: PlayerControls not available");
				g_inputHooksInstalled.store(false, std::memory_order_relaxed);
				return;
			}

			REX::INFO(
				"Hotkey: installing PlayerControls QCOpenTransferMenu hooks for {} handlers",
				controls->handlers.size());

			InstallNamedHandlerHook("Movement", controls->movementHandler, kBlockOtherQCOpenTransferMenuOriginal);
			InstallNamedHandlerHook("Look", controls->lookHandler, kBlockOtherQCOpenTransferMenuOriginal);
			InstallNamedHandlerHook("Sprint", controls->sprintHandler, kBlockOtherQCOpenTransferMenuOriginal);
			InstallNamedHandlerHook("ReadyWeapon", controls->readyWeaponHandler, kBlockReadyWeaponQCOpenTransferMenuOriginal);
			InstallNamedHandlerHook("AutoMove", controls->autoMoveHandler, kBlockOtherQCOpenTransferMenuOriginal);
			InstallNamedHandlerHook("ToggleRun", controls->toggleRunHandler, kBlockOtherQCOpenTransferMenuOriginal);
			InstallNamedHandlerHook("Activate", controls->activateHandler, kBlockActivateQCOpenTransferMenuOriginal);
			InstallNamedHandlerHook("Jump", controls->jumpHandler, kBlockOtherQCOpenTransferMenuOriginal);
			InstallNamedHandlerHook("Attack", controls->attackHandler, kBlockOtherQCOpenTransferMenuOriginal);
			InstallNamedHandlerHook("Run", controls->runHandler, kBlockOtherQCOpenTransferMenuOriginal);
			InstallNamedHandlerHook("Sneak", controls->sneakHandler, kBlockOtherQCOpenTransferMenuOriginal);
			InstallNamedHandlerHook("TogglePOV", controls->togglePOVHandler, kBlockOtherQCOpenTransferMenuOriginal);
			InstallNamedHandlerHook("MeleeThrow", controls->meleeThrowHandler, kBlockOtherQCOpenTransferMenuOriginal);
			InstallNamedHandlerHook("GrabRotation", controls->grabRotationHandler, kBlockOtherQCOpenTransferMenuOriginal);

			REX::INFO(
				"Hotkey: installed {} handler hooks across {} unique vtables",
				g_hookedHandlerCount,
				g_hookedVTableCount);
		}

		void QueueOpenInventoryOnGameThread()
		{
			auto* task = F4SE::GetTaskInterface();
			if (!task) {
				REX::WARN("Hotkey: F4SE TaskInterface not available");
				return;
			}

			task->AddTask([]() {
				if (!IsCompanionInputAllowedOnGameThread()) {
					return;
				}

				if (auto* a = Companion::FindActiveCompanionCandidate(); a) {
					OpenInventory::OpenForCompanion(a);
				}
				else {
					REX::WARN("Hotkey(task): no active follower");
				}
				});
		}

		bool ConvertPreWarMoneyToCaps(RE::TESObjectREFR* player, RE::TESBoundObject* moneyObj, RE::TESBoundObject* capsObj) noexcept
		{
			if (!player || !moneyObj || !capsObj) {
				return false;
			}

			auto remainingMoney = GetItemCountSafe(player, moneyObj);
			if (remainingMoney == 0) {
				REX::INFO("Hotkey(task): no pre-war money to convert");
				return true;
			}

			std::uint32_t convertedMoney = 0;
			std::uint32_t createdCaps = 0;

			while (remainingMoney > 0) {
				const auto moneyChunk = (std::min)(remainingMoney, kMoneyConvertChunk);
				const auto capsChunk = moneyChunk * 10;

				{
					RE::TESObjectREFR::RemoveItemData data(moneyObj, static_cast<std::int32_t>(moneyChunk));
					player->RemoveItem(data);
				}

				player->AddObjectToContainer(
					capsObj,
					nullptr,
					static_cast<std::int32_t>(capsChunk),
					nullptr,
					RE::ITEM_REMOVE_REASON::kStoreContainer);

				convertedMoney += moneyChunk;
				createdCaps += capsChunk;
				remainingMoney -= moneyChunk;
			}

			REX::INFO("Hotkey(task): converted {} pre-war money into {} caps", convertedMoney, createdCaps);
			return true;
		}

		bool DepositAllCapsToWorkshop(RE::TESObjectREFR* player, RE::TESObjectREFR* workshop, RE::TESBoundObject* capsObj) noexcept
		{
			if (!player || !workshop || !capsObj) {
				return false;
			}

			auto remainingCaps = GetItemCountSafe(player, capsObj);
			if (remainingCaps == 0) {
				REX::INFO("Hotkey(task): no caps to deposit");
				return true;
			}

			std::uint32_t moved = 0;

			while (remainingCaps > 0) {
				const auto chunk = (std::min)(remainingCaps, kTransferChunk);

				RE::TESObjectREFR::RemoveItemData data(capsObj, static_cast<std::int32_t>(chunk));
				data.reason = RE::ITEM_REMOVE_REASON::kStoreContainer;
				data.a_otherContainer = workshop;

				player->RemoveItem(data);

				moved += chunk;
				remainingCaps -= chunk;
			}

			REX::INFO("Hotkey(task): deposited {} caps to workshop", moved);
			return true;
		}

		void QueueConvertMoneyAndDepositCapsOnGameThread()
		{
			auto* task = F4SE::GetTaskInterface();
			if (!task) {
				REX::WARN("Hotkey: F4SE TaskInterface not available");
				return;
			}

			task->AddTask([]() {
				if (!IsBankInputAllowedOnGameThread()) {
					return;
				}

				auto* player = RE::PlayerCharacter::GetSingleton();
				auto* workshop = GetWorkshop();
				auto* capsObj = GetCapsObject();
				auto* moneyObj = GetPreWarMoneyObject();

				if (!player || !workshop || !capsObj || !moneyObj) {
					REX::WARN("Hotkey(task): convert/deposit aborted");
					return;
				}

				if (!ConvertPreWarMoneyToCaps(player, moneyObj, capsObj)) {
					REX::WARN("Hotkey(task): convert failed");
					return;
				}

				if (!DepositAllCapsToWorkshop(player, workshop, capsObj)) {
					REX::WARN("Hotkey(task): deposit failed");
					return;
				}
				});
		}

		void QueueWithdrawCapsOnGameThread()
		{
			auto* task = F4SE::GetTaskInterface();
			if (!task) {
				REX::WARN("Hotkey: F4SE TaskInterface not available");
				return;
			}

			task->AddTask([]() {
				if (!IsBankInputAllowedOnGameThread()) {
					return;
				}

				auto* player = RE::PlayerCharacter::GetSingleton();
				auto* workshop = GetWorkshop();
				auto* capsObj = GetCapsObject();

				if (!player || !workshop || !capsObj) {
					REX::WARN("Hotkey(task): withdraw aborted");
					return;
				}

				auto remaining = GetItemCountSafe(workshop, capsObj);
				if (remaining == 0) {
					REX::INFO("Hotkey(task): no caps in workshop");
					return;
				}

				std::uint32_t moved = 0;

				while (remaining > 0) {
					const auto chunk = (std::min)(remaining, kTransferChunk);

					RE::TESObjectREFR::RemoveItemData data(capsObj, static_cast<std::int32_t>(chunk));
					data.reason = RE::ITEM_REMOVE_REASON::kStoreContainer;
					data.a_otherContainer = player;

					workshop->RemoveItem(data);

					moved += chunk;
					remaining -= chunk;
				}

				REX::INFO("Hotkey(task): withdrew {} caps from workshop", moved);
				});
		}

		void ThreadMain()
		{
			bool prevCompanionDown = false;
			bool prevBankDown = false;

			auto lastCompanionFire = std::chrono::steady_clock::now() - std::chrono::seconds(10);
			auto lastBankFire = std::chrono::steady_clock::now() - std::chrono::seconds(10);
			const auto cooldown = std::chrono::milliseconds(250);

			while (g_running.load(std::memory_order_relaxed)) {
				const auto vk = static_cast<int>(g_vk.load(std::memory_order_relaxed));
				const bool companionDown = IsDown(vk);

				const bool bankDown = IsDown(static_cast<int>(kBankVk));
				const bool ctrlDown = IsCtrlDown();
				const bool shiftDown = IsShiftDown();

				if (companionDown && !prevCompanionDown) {
					const auto now = std::chrono::steady_clock::now();
					if (now - lastCompanionFire >= cooldown) {
						lastCompanionFire = now;
						QueueOpenInventoryOnGameThread();
					}
				}

				if (bankDown && !prevBankDown) {
					const auto now = std::chrono::steady_clock::now();
					if (now - lastBankFire >= cooldown) {
						lastBankFire = now;

						if (ctrlDown) {
							QueueWithdrawCapsOnGameThread();
						}
						else if (shiftDown) {
							REX::INFO("Hotkey(task): Shift+$ reserved for QCOpenTransferMenu investigation");
						}
						else {
							QueueConvertMoneyAndDepositCapsOnGameThread();
						}
					}
				}

				prevCompanionDown = companionDown;
				prevBankDown = bankDown;

				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}
		}
	}

	void SetVK(std::uint32_t vk)
	{
		g_vk.store(vk, std::memory_order_relaxed);
	}

	void Start()
	{
		InstallPlayerControlsInputHooks();

		bool expected = false;
		if (!g_running.compare_exchange_strong(expected, true)) {
			return;
		}

		g_thread = std::thread(ThreadMain);
		g_thread.detach();
	}

	void Stop()
	{
		g_running.store(false, std::memory_order_relaxed);
	}
}
