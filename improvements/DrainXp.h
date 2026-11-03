#pragma once

namespace DrainXp {
	// 0 = not running, 1 = running (draining), 2 = running (30s rejoin window)
	int GetRunningStatus();
	bool GetLevelUpObserved();

	void Start();
	void Stop();
	void StopForHostLeaving();

	// True if this run has queued an end action that should be allowed through.
	bool IsForcedEnd(app::GameOverReason__Enum reason);

	// Tick hooks (driven from dInnerNetClient_Update / dPlayerControl_OnGameStart)
	void Tick();
	void TickLobby();
	void TickGameLoaded();
	void OnGameEnd();
}