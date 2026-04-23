#include "Hotkey.h"

#include "Companion.h"
#include "OpenInventory.h"

#include <F4SE/Interfaces.h>

#include <RE/B/BSFixedString.h>
#include <RE/B/BSSpinLock.h>
#include <RE/C/ContainerMenu.h>
#include <RE/M/MemoryManager.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESBoundObject.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/U/UI.h>
#include <RE/U/UIMessageQueue.h>

#include <REX/REX.h>

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace Hotkey
{
	namespace
	{
		std::atomic_uint32_t g_vk{ 0x75 };
		std::atomic_bool     g_running{ false };
		std::thread          g_thread;

		constexpr std::uint32_t kBankVk = 0xBA;
		constexpr std::uint32_t kWorkshopRefID = 0x000250FE;
		constexpr std::uint32_t kCapsFormID = 0x0000000F;
		constexpr std::uint32_t kPreWarMoneyFormID = 0x00059B02;

		constexpr std::uint32_t kTransferChunk = 60000;
		constexpr std::uint32_t kMoneyConvertChunk = 6000;
		constexpr std::uint64_t kWorkshopOpenContainerTag = 0x0000000600000000ULL;

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

			return !(ui->GetMenuOpen(kConsole) ||
				ui->GetMenuOpen(kPipboy) ||
				ui->GetMenuOpen(kPause));
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

			return !(ui->GetMenuOpen(kConsole) || ui->GetMenuOpen(kPause));
		}

		inline bool IsWorkshopTransferInputAllowedOnGameThread() noexcept
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
			static const RE::BSFixedString kPipboy("PipboyMenu");
			static const RE::BSFixedString kContainerMenu("ContainerMenu");
			static const RE::BSFixedString kWorkshopMenu("WorkshopMenu");
			static const RE::BSFixedString kPipboyWorkshopMenu("PipboyWorkshopMenu");

			return !(ui->GetMenuOpen(kConsole) ||
				ui->GetMenuOpen(kPause) ||
				ui->GetMenuOpen(kPipboy) ||
				ui->GetMenuOpen(kContainerMenu) ||
				ui->GetMenuOpen(kWorkshopMenu) ||
				ui->GetMenuOpen(kPipboyWorkshopMenu));
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

		struct OpenContainerMenuMessageOverlay
		{
			std::uintptr_t                                    vtbl{ 0x0 };
			RE::BSFixedString                                 menu;
			REX::TEnumSet<RE::UI_MESSAGE_TYPE, std::int32_t> type{ RE::UI_MESSAGE_TYPE::kTotal };
			std::uint64_t                                    packedHandle{ 0x0 };
			std::uint64_t                                    unk20{ 0x0 };
			std::uint64_t                                    unk28{ 0x0 };
			std::uint64_t                                    unk30{ 0x0 };
			std::uint64_t                                    unk38{ 0x0 };
			std::uint64_t                                    unk40{ 0x0 };
			std::uint64_t                                    unk48{ 0x0 };
		};
		static_assert(sizeof(OpenContainerMenuMessageOverlay) == 0x50);

		[[nodiscard]] std::unique_ptr<RE::UIMessage> BuildWorkshopOpenContainerMessage(RE::TESObjectREFR* a_workshop) noexcept
		{
			if (!a_workshop) {
				return nullptr;
			}

			constexpr std::size_t kMessageStorageSize = 0x100;
			auto* storage = static_cast<std::byte*>(RE::calloc(1, kMessageStorageSize));
			if (!storage) {
				REX::WARN("Hotkey: failed to allocate OpenContainerMenuMessage storage");
				return nullptr;
			}

			auto* message = reinterpret_cast<OpenContainerMenuMessageOverlay*>(storage);
			std::construct_at(std::addressof(message->menu), RE::BSFixedString(RE::ContainerMenu::MENU_NAME));
			message->type = RE::UI_MESSAGE_TYPE::kShow;

			REL::Relocation<std::uintptr_t> openContainerVtblRel{ RE::VTABLE::OpenContainerMenuMessage[0] };
			message->vtbl = openContainerVtblRel.address();
			message->packedHandle =
				kWorkshopOpenContainerTag | static_cast<std::uint64_t>(a_workshop->GetHandle().get_handle());

			return std::unique_ptr<RE::UIMessage>(reinterpret_cast<RE::UIMessage*>(storage));
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

				if (auto* companion = Companion::FindActiveCompanionCandidate(); companion) {
					OpenInventory::OpenForCompanion(companion);
				}
			});
		}

		bool ConvertPreWarMoneyToCaps(RE::TESObjectREFR* player, RE::TESBoundObject* moneyObj, RE::TESBoundObject* capsObj) noexcept
		{
			if (!player || !moneyObj || !capsObj) {
				return false;
			}

			auto remainingMoney = GetItemCountSafe(player, moneyObj);
			while (remainingMoney > 0) {
				const auto moneyChunk = (std::min)(remainingMoney, kMoneyConvertChunk);
				const auto capsChunk = moneyChunk * 10;

				RE::TESObjectREFR::RemoveItemData removeMoney(moneyObj, static_cast<std::int32_t>(moneyChunk));
				player->RemoveItem(removeMoney);

				player->AddObjectToContainer(
					capsObj,
					nullptr,
					static_cast<std::int32_t>(capsChunk),
					nullptr,
					RE::ITEM_REMOVE_REASON::kStoreContainer);

				remainingMoney -= moneyChunk;
			}

			return true;
		}

		bool DepositAllCapsToWorkshop(RE::TESObjectREFR* player, RE::TESObjectREFR* workshop, RE::TESBoundObject* capsObj) noexcept
		{
			if (!player || !workshop || !capsObj) {
				return false;
			}

			auto remainingCaps = GetItemCountSafe(player, capsObj);
			while (remainingCaps > 0) {
				const auto chunk = (std::min)(remainingCaps, kTransferChunk);

				RE::TESObjectREFR::RemoveItemData removeCaps(capsObj, static_cast<std::int32_t>(chunk));
				removeCaps.reason = RE::ITEM_REMOVE_REASON::kStoreContainer;
				removeCaps.a_otherContainer = workshop;
				player->RemoveItem(removeCaps);

				remainingCaps -= chunk;
			}

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
					return;
				}

				if (!ConvertPreWarMoneyToCaps(player, moneyObj, capsObj)) {
					return;
				}

				DepositAllCapsToWorkshop(player, workshop, capsObj);
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
					return;
				}

				auto remainingCaps = GetItemCountSafe(workshop, capsObj);
				while (remainingCaps > 0) {
					const auto chunk = (std::min)(remainingCaps, kTransferChunk);

					RE::TESObjectREFR::RemoveItemData removeCaps(capsObj, static_cast<std::int32_t>(chunk));
					removeCaps.reason = RE::ITEM_REMOVE_REASON::kStoreContainer;
					removeCaps.a_otherContainer = player;
					workshop->RemoveItem(removeCaps);

					remainingCaps -= chunk;
				}
			});
		}

		void QueueOpenSanctuaryWorkshopTransferMenuOnGameThread()
		{
			auto* task = F4SE::GetTaskInterface();
			if (!task) {
				REX::WARN("Hotkey: F4SE TaskInterface not available");
				return;
			}

			task->AddTask([]() {
				if (!IsWorkshopTransferInputAllowedOnGameThread()) {
					return;
				}

				auto* workshop = GetWorkshop();
				auto* queue = RE::UIMessageQueue::GetSingleton();
				if (!workshop || !queue) {
					return;
				}

				auto message = BuildWorkshopOpenContainerMessage(workshop);
				if (!message) {
					return;
				}

				RE::BSAutoLock locker(queue->uiMessagesLock);
				queue->messages.push_back(std::move(message));
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
							QueueOpenSanctuaryWorkshopTransferMenuOnGameThread();
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
