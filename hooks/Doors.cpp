#include "pch-il2cpp.h"
#include "_hooks.h"
#include "state.hpp"
#include "_rpc.h"
#include "../gui/tabs/players_tab.h"
#include <chrono>
#include <unordered_map>

static void QueueDoorClose(SystemTypes__Enum room) {
	static std::unordered_map<SystemTypes__Enum, std::chrono::steady_clock::time_point> lastQueued;
	const auto now = std::chrono::steady_clock::now();
	const auto it = lastQueued.find(room);
	if (it != lastQueued.end()
		&& now - it->second < std::chrono::milliseconds(400))
		return;
	lastQueued[room] = now;
	State.rpcQueue.push(new RpcCloseDoorsOfType(room, false));
}

void dPlainDoor_SetDoorway(PlainDoor* __this, bool open, MethodInfo* method) {
	if (State.ShowHookLogs) Log.HookDebug("Hook dPlainDoor_SetDoorway executed", false);
	if (open && (PlayersTab::IsXpFarmingRound()
		|| std::find(State.pinnedDoors.begin(), State.pinnedDoors.end(), __this->fields._.Room) != State.pinnedDoors.end())) {
		QueueDoorClose(__this->fields._.Room);
	}
	app::PlainDoor_SetDoorway(__this, open, method);
}

void dMushroomWallDoor_SetDoorway(MushroomWallDoor* __this, bool open, MethodInfo* method) {
	if (State.ShowHookLogs) Log.HookDebug("Hook dMushroomWallDoor_SetDoorway executed", false);
	if (open && (PlayersTab::IsXpFarmingRound()
		|| std::find(State.pinnedDoors.begin(), State.pinnedDoors.end(), __this->fields._.Room) != State.pinnedDoors.end())) {
		QueueDoorClose(__this->fields._.Room);
	}
	app::MushroomWallDoor_SetDoorway(__this, open, method);
}

bool dAutoOpenDoor_DoUpdate(AutoOpenDoor* __this, float dt, MethodInfo* method) {
	if (State.ShowHookLogs) Log.HookDebug("Hook dAutoOpenDoor_DoUpdate executed", false);
	if ((PlayersTab::IsXpFarmingRound()
		|| std::find(State.pinnedDoors.begin(), State.pinnedDoors.end(), __this->fields._._.Room) != State.pinnedDoors.end())
		&& __this->fields.ClosedTimer < 1.5f) {
		QueueDoorClose(__this->fields._._.Room);
	}
	return app::AutoOpenDoor_DoUpdate(__this, dt, method);
}