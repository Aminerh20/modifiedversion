#include "pch-il2cpp.h"
#include "UiUpdateQueue.hpp"
#include "../hooks/_hooks.h"
#include "../user/game.h"
#include "../user/utility.h"
#include "logger.h"
#include <deque>
#include <mutex>
#include <utility>

namespace {
	struct PendingUiUpdates {
		std::mutex mutex;
		struct LobbyChatUpdate {
			uint8_t sourcePlayerId;
			std::string message;
			bool censor;
		};
		std::deque<LobbyChatUpdate> lobbyChat;
	};

	PendingUiUpdates& GetPendingUiUpdates() {
		static PendingUiUpdates updates;
		return updates;
	}

	constexpr size_t maxQueuedLobbyMessages = 256;
}

namespace UiUpdateQueue {
	void EnqueueLobbyChat(uint8_t sourcePlayerId, std::string message, bool censor) {
		auto& updates = GetPendingUiUpdates();
		std::lock_guard lock(updates.mutex);
		if (updates.lobbyChat.size() >= maxQueuedLobbyMessages) {
			updates.lobbyChat.pop_front();
			Log.Info("UI UPDATE QUEUE", "Dropped the oldest queued lobby chat message because the queue was full.");
		}
		updates.lobbyChat.push_back({ sourcePlayerId, std::move(message), censor });
	}

	void ProcessPending() {
		if (!IsInLobby()) return;

		auto hudManager = Game::HudManager.GetInstance();
		auto chat = hudManager != nullptr ? hudManager->fields.Chat : nullptr;
		if (chat == nullptr) return;

		std::deque<PendingUiUpdates::LobbyChatUpdate> lobbyChat;
		auto& updates = GetPendingUiUpdates();
		{
			std::lock_guard lock(updates.mutex);
			lobbyChat.swap(updates.lobbyChat);
		}

		for (const auto& update : lobbyChat) {
			auto sourcePlayer = GetPlayerControlById(update.sourcePlayerId);
			if (sourcePlayer == nullptr) {
				Log.Info("UI UPDATE QUEUE", "Dropped a queued lobby chat message because its sender disconnected.");
				continue;
			}
			ProcessQueuedLobbyChatMessage(
				chat, sourcePlayer, convert_to_string(update.message), update.censor, nullptr);
		}
	}
}
