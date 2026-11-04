#include "pch-il2cpp.h"
#include "DrainXp.h"
#include "_hooks.h"
#include "_rpc.h"
#include "state.hpp"
#include "game.h"
#include "utility.h"
#include "toasts.hpp"
#include "logger.h"

#include <string>
#include <chrono>
#include <unordered_map>

// Drain XP automation.
//
// One button (Game tab -> General) repeatedly force-starts the game and ends it
// right after the loading bar completes (never in the middle of loading, which
// can crash the lobby). Observed level-ups are announced through the local
// player, with a 30-second notice to rejoin before the next round.
//
// ---------------------------------------------------------------------------
// XP detection is necessarily approximate: per-player XP stats are local-only,
// while the displayed level is replicated. A level increase is evidence of an
// XP gain, but no increase does not prove that the player has no XP remaining.
// The loop therefore stops after a round with no observed level increases.
//
// ---------------------------------------------------------------------------
// Signal choices (all verified against this codebase):
//  * Loading bar done  -> PlayerControl_OnGameStart (il2cpp-functions.h:159).
//                         The mod's dPlayerControl_OnGameStart hook already
//                         fires on every client at that exact moment.
//  * Force start       -> InnerNetClient_SendStartGame (same as the Host tab
//                         "Force Start of Game", host_tab.cpp:436).
//  * Start countdown   -> follows the game's existing countdown behavior and
//                         the Host-tab Modify Start Countdown setting.
//  * End game          -> RpcEndGame(0) -> GameManager_RpcEndGame
//                         (rpc/RpcMurderPlayer.cpp:497, same as /end).
//  * Chat xp announce  -> PlayerControl_RpcSendChat(*Game::pLocalPlayer, ...)
//                         (same call as the warn/mute/reset chat messages).
//  * Level read        -> GetPlayerData(p)->fields.PlayerLevel (+1 is display).

namespace DrainXp {

    // Heads-up so the Game tab can paint the button's live state.
    static bool s_running = false;
    static bool s_levelUpObserved = false;
    static bool s_waitingRejoin = false; // 30s rejoin window in progress
    static bool s_finalWindow = false;   // after the window: stop (not restart)
    static bool s_forcedEndPending = false; // the drain's own forced end is pending
    static bool s_meetingEndPending = false;
    static bool s_endDelayPending = false;
    static bool s_endQueued = false;     // the end has been pushed; OnGameStart may fire per-player
    static bool s_roundEnded = false;    // a round ended; drain-check on next lobby tick
    static std::chrono::steady_clock::time_point s_windowStart;
    static std::chrono::steady_clock::time_point s_endReadyAt;
    static std::unordered_map<uint8_t, uint32_t> s_levelBefore; // PlayerId -> PlayerLevel snapshot

    static bool HasHostAuthority() {
        return Game::pAmongUsClient != nullptr
            && *Game::pAmongUsClient != nullptr
            && IsHost();
    }

    int GetRunningStatus() { return s_running ? (s_waitingRejoin ? 2 : 1) : 0; }
    bool GetLevelUpObserved() { return s_levelUpObserved; }

    // Let an end through only after this run has queued its own end action.
    bool IsForcedEnd(app::GameOverReason__Enum) {
        return s_running && (s_forcedEndPending || s_meetingEndPending);
    }

    // ------------------------------------------------------------------
    // Internals
    // ------------------------------------------------------------------

    // Safe to talk to the game yet? (pLocalPlayer + HudManager chat exist.)
    static bool CanChat() {
        return Game::pLocalPlayer != nullptr
            && *Game::pLocalPlayer != nullptr
            && Game::HudManager.GetInstance() != nullptr
            && Game::HudManager.GetInstance()->fields.Chat != nullptr;
    }

    static void ChatMessage(const std::string& msg) {
        if (!State.DrainXpChat) return;
        if (!CanChat()) return;
        PlayerControl_RpcSendChat(*Game::pLocalPlayer, convert_to_string(msg), NULL);
    }

    static std::string PlayerDisplayName(PlayerControl* player) {
        if (player == nullptr) return "";
        auto outfit = GetPlayerOutfit(GetPlayerData(player));
        if (outfit == nullptr) return "";
        std::string name = RemoveHtmlTags(convert_from_string(outfit->fields.PlayerName));
        return name.empty() ? "Someone" : name;
    }

    static uint32_t CurrentLevel(PlayerControl* player) {
        auto data = GetPlayerData(player);
        if (data == nullptr) return 0;
        return data->fields.PlayerLevel;
    }

    // Use the same start calls as the Host tab's "Force Start of Game" button.
    // The countdown still follows the game's normal setting.
    static bool StartRound() {
        if (!s_running || !HasHostAuthority() || !IsInLobby()) {
            return false;
        }
        app::AmongUsClient_KickNotJoinedPlayers(*Game::pAmongUsClient, NULL);
        app::InnerNetClient_SendStartGame((InnerNetClient*)(*Game::pAmongUsClient), NULL);
        return true;
    }

    // ------------------------------------------------------------------
    // Round completion: find who drained and react
    // ------------------------------------------------------------------

    // Called (from the lobby tick) once we are back in a lobby after a round.
    // Compares each player's level against the pre-round snapshot; anyone who
    // leveled up just drained their XP.
    static void HandleRoundEnd() {
        if (!s_roundEnded) return;
        s_roundEnded = false;

        std::vector<std::string> drained;
        for (auto p : GetAllPlayerControl()) {
            if (p == nullptr) continue;
            auto data = GetPlayerData(p);
            if (data == nullptr || data->fields.Disconnected) continue;

            auto it = s_levelBefore.find(p->fields.PlayerId);
            if (it != s_levelBefore.end() && CurrentLevel(p) > it->second) {
                drained.push_back(PlayerDisplayName(p));
            }
        }

        // Refresh the snapshot with the current lobby state.
        s_levelBefore.clear();
        for (auto p : GetAllPlayerControl()) {
            if (p == nullptr) continue;
            s_levelBefore[p->fields.PlayerId] = CurrentLevel(p);
        }

        if (!drained.empty()) {
            for (auto& name : drained) {
                ChatMessage(name + " leveled up during the XP drain. You have 30 seconds to rejoin.");
                Toasts::AddToast("Drain XP", name + " leveled up!", ImVec4(0.f, 1.f, 0.5f, 1.f));
            }
            s_levelUpObserved = true;
            s_finalWindow = false;

            // 30-second window so the drained players can rejoin before the
            // next round. After it: force-start the next round.
            s_waitingRejoin = true;
            s_windowStart = std::chrono::steady_clock::now();
            return;
        }

        // Silent round - no level increases were observed.
        if (!s_levelUpObserved) {
            s_running = false;
            s_waitingRejoin = false;
            s_levelBefore.clear();
            Toasts::AddToast("Drain XP", "No level-ups detected; XP drains were not confirmed.", ImVec4(0.3f, 0.8f, 1.f, 1.f));
            return;
        }

        // No further level-ups were observed. Hold a final rejoin window, then
        // stop without claiming that all players have no XP remaining.
        s_finalWindow = true;
        s_waitingRejoin = true;
        s_windowStart = std::chrono::steady_clock::now();
        Toasts::AddToast("Drain XP", "No further level-ups detected. Final 30s rejoin window.", ImVec4(0.f, 1.f, 0.5f, 1.f));
    }

    // ------------------------------------------------------------------
    // Update entry points (hooked from dInnerNetClient_Update and
    // dPlayerControl_OnGameStart)
    // ------------------------------------------------------------------

    void Tick() {
        if (!s_running) return;
        if (State.PanicMode) {
            Stop();
            return;
        }
        if (!HasHostAuthority()) {
            StopForHostLeaving();
            return;
        }
        if (s_endDelayPending
            && std::chrono::steady_clock::now() >= s_endReadyAt
            && IsInGame()
            && State.GameLoaded) {
            s_endDelayPending = false;
            s_endQueued = true;
            if (State.DrainXpEndViaMeeting) {
                s_meetingEndPending = true;
                State.rpcQueue.push(new RpcReportBody({}));
            }
            else {
                s_forcedEndPending = true;
                State.rpcQueue.push(new RpcEndGame(GameOverReason__Enum(0)));
            }
        }
        if (IsInLobby()) TickLobby();
    }

    void TickLobby() {
        if (!s_running) {
            s_waitingRejoin = false;
            return;
        }

        HandleRoundEnd();

        if (s_waitingRejoin) {
            float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - s_windowStart).count();
            if (elapsed < 30.f) return;

            s_waitingRejoin = false;
            if (s_finalWindow) {
                s_running = false;
                s_finalWindow = false;
                s_levelBefore.clear();
                return;
            }
            // Rejoin window over - next round.
            if (!StartRound()) StopForHostLeaving();
        }
    }

    // First in-game tick after the loading bar finishes.
    void TickGameLoaded() {
        if (!s_running || s_endDelayPending || s_endQueued) return;
        if (State.PanicMode) {
            Stop();
            return;
        }
        if (!HasHostAuthority()) {
            StopForHostLeaving();
            return;
        }

        // Let the original OnGameStart handler and game initialization finish
        // before scheduling an end. This hook can fire once per player control.
        s_endDelayPending = true;
        s_endReadyAt = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    }

    static void StopWithMessage(const std::string& message) {
        if (!s_running) return;
        s_running = false;
        s_waitingRejoin = false;
        s_finalWindow = false;
        s_forcedEndPending = false;
        s_meetingEndPending = false;
        s_endDelayPending = false;
        s_endQueued = false;
        s_roundEnded = false;
        s_levelBefore.clear();
        Toasts::AddToast("Drain XP", message, ImVec4(1.f, 0.6f, 0.2f, 1.f));
    }

    // ------------------------------------------------------------------
    // Public API used by the Game tab
    // ------------------------------------------------------------------

    void Start() {
        if (s_running) return;
        if (!IsInLobby()) return;
        if (!HasHostAuthority()) {
            Toasts::AddToast("Drain XP", "Host only!", ImVec4(1.f, 0.3f, 0.3f, 1.f));
            return;
        }

        s_running = true;
        s_levelUpObserved = false;
        s_waitingRejoin = false;
        s_finalWindow = false;
        s_forcedEndPending = false;
        s_meetingEndPending = false;
        s_endDelayPending = false;
        s_endQueued = false;
        s_roundEnded = false;

        // Snapshot every lobby player's current level before the first round.
        s_levelBefore.clear();
        for (auto p : GetAllPlayerControl()) {
            if (p == nullptr) continue;
            s_levelBefore[p->fields.PlayerId] = CurrentLevel(p);
        }

        Toasts::AddToast("Drain XP", "Starting XP drain...", ImVec4(0.f, 1.f, 0.5f, 1.f));
        ChatMessage("Starting XP drain - please do not disconnect!");

        if (!StartRound()) StopForHostLeaving();
    }

    void Stop() {
        StopWithMessage("Stopped.");
    }

    void StopForHostLeaving() {
        StopWithMessage("Stopped because host authority was lost.");
    }

    // Called on game end from the dGameManager_RpcEndGame hook so the run
    // survives the end-game screen and the round is counted.
    void OnGameEnd() {
        if (!s_running) return;
        s_forcedEndPending = false;
        s_meetingEndPending = false;
        s_endDelayPending = false;
        s_endQueued = false;         // next round is allowed to enqueue its end
        s_roundEnded = true;         // drain-check runs when we reach the lobby again
    }
}