#pragma once

namespace Menu {
	enum Tabs {
		Settings,
		Game,
		Self,
		Animations,
		Radar,
		Replay,
		Esp,
		Players,
		Tasks,
		Sabotage,
		Doors,
		Host,
		Debug
	};
	void CloseAllOtherTabs(Tabs openTab);
	void Init();
	void Render();
}
