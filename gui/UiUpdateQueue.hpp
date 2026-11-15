#pragma once

#include <cstdint>
#include <string>

struct MethodInfo;

namespace app {
	struct ChatController;
	struct PlayerControl;
	struct String;
}

namespace UiUpdateQueue {
	void EnqueueLobbyChat(uint8_t sourcePlayerId, std::string message, bool censor);
	void ProcessPending();
}

void ProcessQueuedLobbyChatMessage(
	app::ChatController* chat,
	app::PlayerControl* sourcePlayer,
	app::String* message,
	bool censor,
	::MethodInfo* method);
