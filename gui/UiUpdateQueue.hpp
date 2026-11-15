#pragma once

#include <cstdint>
#include <string>

struct ChatController;
struct PlayerControl;
struct String;
struct MethodInfo;

namespace UiUpdateQueue {
	void EnqueueLobbyChat(uint8_t sourcePlayerId, std::string message, bool censor);
	void ProcessPending();
}

void ProcessQueuedLobbyChatMessage(
	ChatController* chat,
	PlayerControl* sourcePlayer,
	String* message,
	bool censor,
	MethodInfo* method);
