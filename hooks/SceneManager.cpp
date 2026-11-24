#include "pch-il2cpp.h"
#include "_hooks.h"
#include "state.hpp"
#include "../gui/tabs/players_tab.h"

void dSceneManager_Internal_ActiveSceneChanged(Scene previousActiveScene, Scene newActiveScene, MethodInfo* method) {
	//if (State.ShowHookLogs) Log.HookDebug("Hook dSceneManager_Internal_ActiveSceneChanged executed", false);
	const std::string previousScene =
		convert_from_string(app::Scene_GetNameInternal(previousActiveScene.m_Handle, NULL));
	State.CurrentScene = convert_from_string(app::Scene_GetNameInternal(newActiveScene.m_Handle, NULL));
	PlayersTab::RecordXpLevelLifecycleEvent(
		"SceneManager.Internal_ActiveSceneChanged",
		nullptr,
		std::format("previous_scene={} current_scene={}", previousScene, State.CurrentScene));
	LOG_DEBUG(("Scene changed to " + State.CurrentScene).c_str());
	if (State.CurrentScene == "MainMenu") {
		State.MainMenuLoaded = true;
		State.IsFreePlay = false;
	}
	if (State.CurrentScene == "Tutorial")
		ResetOriginalAppearance(); // since we don't join a lobby beforehand
	app::SceneManager_Internal_ActiveSceneChanged(previousActiveScene, newActiveScene, method);
}