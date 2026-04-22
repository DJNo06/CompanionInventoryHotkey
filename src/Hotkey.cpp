#include "Hotkey.h"

#include "Companion.h"
#include "OpenInventory.h"

#include <F4SE/Interfaces.h>

#include <RE/B/BSFixedString.h>
#include <RE/B/BSTEvent.h>
#include <RE/B/ButtonEvent.h>
#include <RE/C/ContainerMenu.h>
#include <RE/M/MenuOpenCloseEvent.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/P/PlayerControls.h>
#include <RE/Q/QuickContainerMode.h>
#include <RE/Q/QuickContainerStateEvent.h>
#include <RE/T/TESBoundObject.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/U/UI.h>

#include <REX/REX.h>

#include <Windows.h>

#include <algorithm>
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
		std::atomic_bool     g_containerMenuHookInstalled{ false };
		std::atomic_bool     g_quickContainerHookInstalled{ false };
		std::atomic_bool     g_menuTraceInstalled{ false };
		std::thread          g_thread;

		constexpr std::uint32_t kBankVk = 0xBA;                    // $
		constexpr std::uint32_t kWorkshopRefID = 0x000250FE;      // Sanctuary workshop
		constexpr std::uint32_t kCapsFormID = 0x0000000F;         // Caps
		constexpr std::uint32_t kPreWarMoneyFormID = 0x00059B02;  // Pre-War Money

		constexpr std::uint32_t kTransferChunk = 60000;           // caps : sécurité
		constexpr std::uint32_t kMoneyConvertChunk = 6000;        // 6000 money -> 60000 caps max

		constexpr std::size_t kContainerMenuProcessMessageSlot = 3;
		constexpr std::size_t kEventSinkProcessEventSlot = 1;
		constexpr std::size_t kPlayerControlsQuickContainerSinkOffset = 0x38;
		constexpr bool        kBlockPlayerControlsQuickContainerOriginal = false;
		constexpr bool        kBlockContainerMenuShowMessage = true;

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

		[[nodiscard]] const char* QuickContainerModeName(RE::QuickContainerMode a_mode) noexcept
		{
			switch (a_mode) {
			case RE::QuickContainerMode::kLoot:
				return "Loot";
			case RE::QuickContainerMode::kTeammate:
				return "Teammate";
			case RE::QuickContainerMode::kPowerArmor:
				return "PowerArmor";
			case RE::QuickContainerMode::kTurret:
				return "Turret";
			case RE::QuickContainerMode::kWorkshop:
				return "Workshop";
			case RE::QuickContainerMode::kCrafting:
				return "Crafting";
			case RE::QuickContainerMode::kStealing:
				return "Stealing";
			case RE::QuickContainerMode::kStealingPowerArmor:
				return "StealingPowerArmor";
			default:
				return "Unknown";
			}
		}

		[[nodiscard]] std::uint32_t ResolveHandleFormID(RE::ObjectRefHandle a_handle) noexcept
		{
			auto ref = a_handle.get();
			return ref ? ref->GetFormID() : 0;
		}

		[[nodiscard]] std::uint32_t HandleValue(RE::ObjectRefHandle a_handle) noexcept
		{
			return a_handle.get_handle();
		}

		[[nodiscard]] const char* UIMessageTypeName(RE::UI_MESSAGE_TYPE a_type) noexcept
		{
			switch (a_type) {
			case RE::UI_MESSAGE_TYPE::kUpdate:
				return "Update";
			case RE::UI_MESSAGE_TYPE::kShow:
				return "Show";
			case RE::UI_MESSAGE_TYPE::kReshow:
				return "Reshow";
			case RE::UI_MESSAGE_TYPE::kHide:
				return "Hide";
			case RE::UI_MESSAGE_TYPE::kForceHide:
				return "ForceHide";
			case RE::UI_MESSAGE_TYPE::kScaleformEvent:
				return "ScaleformEvent";
			case RE::UI_MESSAGE_TYPE::kUserEvent:
				return "UserEvent";
			case RE::UI_MESSAGE_TYPE::kInventoryUpdate:
				return "InventoryUpdate";
			case RE::UI_MESSAGE_TYPE::kUserProfileChange:
				return "UserProfileChange";
			case RE::UI_MESSAGE_TYPE::kMUStatusChange:
				return "MUStatusChange";
			case RE::UI_MESSAGE_TYPE::kResumeCaching:
				return "ResumeCaching";
			case RE::UI_MESSAGE_TYPE::kUpdateController:
				return "UpdateController";
			case RE::UI_MESSAGE_TYPE::kChatterEvent:
				return "ChatterEvent";
			default:
				return "Unknown";
			}
		}

		struct ContainerMenuProcessMessageHook
		{
			using menu_t = RE::ContainerMenu;
			using func_t = RE::UI_MESSAGE_RESULTS(*)(menu_t*, RE::UIMessage&);

			static inline func_t _original{ nullptr };

			static RE::UI_MESSAGE_RESULTS Thunk(menu_t* a_this, RE::UIMessage& a_message)
			{
				const auto type = static_cast<RE::UI_MESSAGE_TYPE>(a_message.type.underlying());
				const bool shouldLog =
					type == RE::UI_MESSAGE_TYPE::kShow ||
					type == RE::UI_MESSAGE_TYPE::kReshow ||
					type == RE::UI_MESSAGE_TYPE::kHide ||
					type == RE::UI_MESSAGE_TYPE::kForceHide;

				if (shouldLog) {
					REX::INFO(
						"ContainerMenu: ProcessMessage type={} ({}) menu='{}' this={:p} containerRef={:08X}/{:08X} menuOpening={} suppressed={}",
						static_cast<std::int32_t>(type),
						UIMessageTypeName(type),
						a_message.menu.c_str(),
						static_cast<void*>(a_this),
						HandleValue(a_this->containerRef),
						ResolveHandleFormID(a_this->containerRef),
						a_this->menuOpening,
						a_this->suppressed);
				}

				if (kBlockContainerMenuShowMessage &&
					(type == RE::UI_MESSAGE_TYPE::kShow || type == RE::UI_MESSAGE_TYPE::kReshow)) {
					REX::INFO("ContainerMenu(test): blocking ProcessMessage {}", UIMessageTypeName(type));
					return RE::UI_MESSAGE_RESULTS::kHandled;
				}

				return _original ? _original(a_this, a_message) : RE::UI_MESSAGE_RESULTS::kPassOn;
			}
		};

		void InstallContainerMenuProcessMessageHook()
		{
			bool expected = false;
			if (!g_containerMenuHookInstalled.compare_exchange_strong(expected, true)) {
				return;
			}

			REL::Relocation<std::uintptr_t> vtblRel{ RE::VTABLE::ContainerMenu[0] };
			auto** vtbl = reinterpret_cast<void**>(vtblRel.address());
			if (!vtbl) {
				REX::WARN("Hotkey: ContainerMenu vtable not resolved");
				g_containerMenuHookInstalled.store(false, std::memory_order_relaxed);
				return;
			}

			ContainerMenuProcessMessageHook::_original =
				reinterpret_cast<ContainerMenuProcessMessageHook::func_t>(vtbl[kContainerMenuProcessMessageSlot]);

			WritePointer(
				std::addressof(vtbl[kContainerMenuProcessMessageSlot]),
				reinterpret_cast<const void*>(ContainerMenuProcessMessageHook::Thunk));

			REX::INFO("Hotkey: ContainerMenu ProcessMessage hook installed");
			REX::INFO("Hotkey:  - handler vtbl       = {:p}", static_cast<void*>(vtbl));
			REX::INFO("Hotkey:  - hooked slot        = {}", kContainerMenuProcessMessageSlot);
			REX::INFO("Hotkey:  - original slot[{}]  = {:p}", kContainerMenuProcessMessageSlot, reinterpret_cast<void*>(ContainerMenuProcessMessageHook::_original));
			REX::INFO("Hotkey:  - block show         = {}", kBlockContainerMenuShowMessage);
		}

		struct PlayerControlsQuickContainerHook
		{
			using event_t = RE::QuickContainerStateEvent;
			using source_t = RE::BSTEventSource<event_t>;
			using func_t = RE::BSEventNotifyControl(*)(void*, const event_t&, source_t*);

			static inline func_t _original{ nullptr };

			static RE::BSEventNotifyControl Thunk(void* a_this, const event_t& a_event, source_t* a_source)
			{
				if (a_event.optionalValue.has_value()) {
					const auto& data = a_event.optionalValue.value();
					const auto  mode = static_cast<RE::QuickContainerMode>(data.mode.underlying());

					REX::INFO(
						"QuickContainer: mode={} ({}) container={:08X}/{:08X} inventory={:08X}/{:08X} items={} activated={} isNew={} locked={} A={} X={}",
						static_cast<std::int32_t>(mode),
						QuickContainerModeName(mode),
						HandleValue(data.containerRef),
						ResolveHandleFormID(data.containerRef),
						HandleValue(data.inventoryRef),
						ResolveHandleFormID(data.inventoryRef),
						data.itemData.size(),
						data.containerActivated,
						data.isNewContainer,
						data.isLocked,
						data.buttonAEnabled,
						data.buttonXEnabled);

					REX::INFO(
						"QuickContainer: containerName='{}' aButton='{}' perkButton='{}'",
						data.containerName.c_str(),
						data.aButtonText.c_str(),
						data.perkButtonText.c_str());
				}
				else {
					REX::INFO("QuickContainer: event received without payload");
				}

				if (kBlockPlayerControlsQuickContainerOriginal) {
					REX::INFO("QuickContainer(test): blocking PlayerControls QuickContainerStateEvent");
					return RE::BSEventNotifyControl::kStop;
				}

				return _original ? _original(a_this, a_event, a_source) : RE::BSEventNotifyControl::kContinue;
			}
		};

		void InstallQuickContainerTraceHook()
		{
			bool expected = false;
			if (!g_quickContainerHookInstalled.compare_exchange_strong(expected, true)) {
				return;
			}

			auto* controls = RE::PlayerControls::GetSingleton();
			if (!controls) {
				REX::WARN("Hotkey: PlayerControls not available");
				g_quickContainerHookInstalled.store(false, std::memory_order_relaxed);
				return;
			}

			auto* sinkBase = reinterpret_cast<void*>(
				reinterpret_cast<std::uintptr_t>(controls) + kPlayerControlsQuickContainerSinkOffset);
			auto** vtbl = *reinterpret_cast<void***>(sinkBase);
			if (!vtbl) {
				REX::WARN("Hotkey: PlayerControls QuickContainer sink vtable not resolved");
				g_quickContainerHookInstalled.store(false, std::memory_order_relaxed);
				return;
			}

			PlayerControlsQuickContainerHook::_original =
				reinterpret_cast<PlayerControlsQuickContainerHook::func_t>(vtbl[kEventSinkProcessEventSlot]);

			WritePointer(
				std::addressof(vtbl[kEventSinkProcessEventSlot]),
				reinterpret_cast<const void*>(PlayerControlsQuickContainerHook::Thunk));

			REX::INFO("Hotkey: PlayerControls QuickContainerStateEvent hook installed");
			REX::INFO("Hotkey:  - playerControls     = {:p}", static_cast<void*>(controls));
			REX::INFO("Hotkey:  - sinkBase           = {:p}", sinkBase);
			REX::INFO("Hotkey:  - handler vtbl       = {:p}", static_cast<void*>(vtbl));
			REX::INFO("Hotkey:  - hooked slot        = {}", kEventSinkProcessEventSlot);
			REX::INFO("Hotkey:  - original slot[{}]  = {:p}", kEventSinkProcessEventSlot, reinterpret_cast<void*>(PlayerControlsQuickContainerHook::_original));
			REX::INFO("Hotkey:  - block original     = {}", kBlockPlayerControlsQuickContainerOriginal);
		}

		struct MenuTraceSink :
			RE::BSTEventSink<RE::MenuOpenCloseEvent>
		{
			static MenuTraceSink& GetSingleton()
			{
				static MenuTraceSink sink;
				return sink;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				static const RE::BSFixedString kContainerMenu("ContainerMenu");
				static const RE::BSFixedString kWorkshopMenu("WorkshopMenu");
				static const RE::BSFixedString kPipboyWorkshopMenu("PipboyWorkshopMenu");

				if (a_event.menuName == kContainerMenu ||
					a_event.menuName == kWorkshopMenu ||
					a_event.menuName == kPipboyWorkshopMenu) {
					REX::INFO(
						"UI: menu '{}' opening={}",
						a_event.menuName.c_str(),
						a_event.opening);
				}

				return RE::BSEventNotifyControl::kContinue;
			}
		};

		void InstallMenuTraceSink()
		{
			bool expected = false;
			if (!g_menuTraceInstalled.compare_exchange_strong(expected, true)) {
				return;
			}

			auto* ui = RE::UI::GetSingleton();
			if (!ui) {
				REX::WARN("Hotkey: UI not available for menu trace");
				g_menuTraceInstalled.store(false, std::memory_order_relaxed);
				return;
			}

			ui->RegisterSink<RE::MenuOpenCloseEvent>(std::addressof(MenuTraceSink::GetSingleton()));
			REX::INFO("Hotkey: MenuOpenCloseEvent trace sink installed");
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
		InstallContainerMenuProcessMessageHook();
		InstallMenuTraceSink();
		InstallQuickContainerTraceHook();

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
