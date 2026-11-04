#pragma once

namespace DrainXpV2 {
	// 0 = not running, 1 = running
	int GetRunningStatus();
	int GetRoundsDone();
	int GetRoundsTarget();

	void Start();
	void Stop();

	// Tick hooks (driven from dInnerNetClient_Update and the new
	// dInnerNetClient_HandleMessage hook).
	void Tick();
	void TickGameLoaded();

	// Called from dInnerNetClient_HandleMessage for every incoming message.
	// Returns true when the message should be swallowed (the server's start
	// confirmation on the fast path, so nothing loads).
	bool HandleMessage(InnerNetClient* client, MessageReader* reader);
}