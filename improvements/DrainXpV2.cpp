#include "pch-il2cpp.h"
#include "DrainXpV2.h"
#include "state.hpp"
#include "game.h"
#include "utility.h"
#include "toasts.hpp"
#include "logger.h"
#include "DrainXp.h"

// Drain XP v2 - fast drain, N rounds (user picks the count, no level detection).
//
// Ports SkyMenuLink's "Fast drain" step 3 loop to native hooks:
//   * the server's StartGame confirmation (InnerNetClient message tag 2) is
//     swallowed so no map ever loads, and the end is sent the moment the
//     start is confirmed - each round is start -> end -> rejoin with nothing
//     drawn;
//   * the XP grant rides the server's end-of-game message (tag 8) and is
//     applied by the game's own OnGameEnd, exactly as it is for a real round;
//   * the rejoin is the same JoinGame the end screen's "Play Again" button
//     uses, and the lobby's natural return is also accepted when it happens.
//
// Safety rules (measured by SkyMenuLink - 1700 rounds overnight without a
// kick at a 0.2s lobby gap; restarting a fresh lobby fast, or 0ms gaps, gets
// "Hacking" within ~50 rounds):
//   * the first start in a lobby is never fast - it runs one normal loaded
//     round so the server has accepted a start from this lobby;
//   * a minimum 0.2s gap between a round completing and the next start;
//   * the end message (tag 8) is NEVER swallowed - the game's own OnGameEnd
//     applies the XP grant, and with no map loaded the end screen simply never
//     spawns (SkyMenuLink's Fast.cs runs the same way: "an end screen that was
//     not shown" - verified across whole farming sessions in their XpGrantPatch
//     comment, the progression screen never draws and the grant still lands);
//   * a stuck rejoin heals itself - one second after JoinGame, if a map is
//     standing but the lobby has not come back, the lobby scene ("OnlineGame")
//     is loaded by hand, exactly like Fast.cs's Rejoin;
//   * step 3 watches itself - a start or end that goes unconfirmed degrades
//     the run to loaded rounds (the old drain's proven path) instead of
//     spinning; if even loaded rounds refuse, the run stops and leaves.
//
// Unlike the old drain: no level snapshots, no 30s rejoin windows, no
// "drained" detection. Exactly N rounds, then stop.

namespace DrainXpV2 {

    // InnerNetClient message tags (Hazel), same as SkyMenuLink/Fast.cs.
    static constexpr uint8_t s_TagStartGame = 2;
    static constexpr uint8_t s_TagEndGame = 8;

    // Stage machine, mirrors Fast.cs's Idle/Starting/Ending/Rejoining.
    enum class Stage : uint8_t {
        Idle = 0,
        Starting = 1,   // start sent, waiting for the server's confirmation
        Ending = 2,     // swallowed start, end sent, waiting for the end
        Rejoining = 3,  // end confirmed, waiting for the lobby to come back
    };

    static bool s_running = false;
    static int  s_roundsDone = 0;
    static int  s_roundsTarget = 10;
    static Stage s_stage = Stage::Idle;

    // The lobby we are cycling in (GameId), and how many rounds it has
    // completed in this run. The first start of a lobby is never fast.
    static int  s_lobby = 0;
    static int  s_roundsHere = 0;

    // When the current stage began, and the last time the lobby was seen
    // ready for the next start (both Time.time).
    static float s_stageAt = 0.f;
    static float s_readySince = -1.f;

    // Whether an end for the current round has gone out.
    static bool s_endSent = false;
    // Whether the rejoin has been asked for (JoinGame was already called).
    static bool s_joinSent = false;
    // When JoinGame went out (Time.time), and whether the lobby-scene fallback
    // has already been asked for in this round (see Stage::Rejoining).
    static float s_joinAt = -1.f;
    static bool s_sceneReloadSent = false;

    // Step 3 refused / broke this run: the lobby has other players, or the
    // fast loop went unconfirmed. Play loaded rounds instead of nothing.
    static bool s_broken = false;

    // Time limit per stage. Matches Fast.cs (3s for a round trip, 8s to come
    // back from the game) plus a stall guard for the idle lobby.
    static constexpr float s_StageTimeout = 3.f;
    static constexpr float s_RejoinTimeout = 8.f;
    static constexpr float s_IdleTimeout = 6.f;

    // Minimum gap between the lobby being ready and the next start. Fast.cs:
    // "at 0ms a tester was kicked for Hacking twice, about fifty rounds into
    // a drain"; 200ms is what ran overnight without a kick.
    static constexpr float s_Gap = 0.2f;

    // A lobby that just appeared gets 1.5s before its first start, fast or not.
    static constexpr float s_FirstStartSettle = 1.5f;

    // Three unanswered stages in a row switches step 3 off (Fast.cs _broken).
    static constexpr int s_MaxFails = 3;
    static int s_fails = 0;

    static bool HasHostAuthority() {
        return Game::pAmongUsClient != nullptr
            && *Game::pAmongUsClient != nullptr
            && IsHost();
    }

    // Are we alone in the lobby? Step 3 (no round) must not run with other
    // players - they would be "left in a game their client never saw start"
    // (Fast.cs:29-30).
    static bool Alone() {
        try {
            if (Game::pGameData != nullptr && *Game::pGameData != nullptr)
                return (int)il2cpp::List((*Game::pGameData)->fields.AllPlayers).size() <= 1;
        }
        catch (...) {
        }
        // No GameData (or it failed): cannot prove we are alone, so run a
        // loaded round instead of risking a phantom round with other players.
        return false;
    }

    // The lobby is there and usable for the next round: joined, and the scene
    // says lobby (GameState == Ended means we are still catching the rejoin).
    static bool LobbyReady(InnerNetClient* client) {
        return client != nullptr
            && client->fields.GameState == InnerNetClient_GameStates__Enum::Joined
            && IsInLobby();
    }

    static void Toast(const std::string& msg) {
        Toasts::AddToast("Drain XP v2", msg, ImVec4(0.f, 1.f, 0.5f, 1.f));
    }

    static void StopWithMessage(const std::string& message) {
        if (!s_running) return;
        s_running = false;
        s_stage = Stage::Idle;
        s_endSent = false;
        s_joinSent = false;
        s_joinAt = -1.f;
        s_sceneReloadSent = false;
        s_readySince = -1.f;
        Toast(message);
    }

    // Send the end through the game's own RPC, reaching the original directly
    // (the RpcEndGame::Process path that the /end command uses -
    // rpc/RpcMurderPlayer.cpp:497). This bypasses the mod's NoGameEnd /
    // GameMode gate on purpose: a drain's end must always go out. The dXp
    // round-trip is completed when the server's end message (tag 8) comes back,
    // which also re-arms the next round's end.
    static void SendEnd() {
        if (s_endSent) return;
        s_endSent = true;
        auto* gm = GameManager__TypeInfo->static_fields->_Instance_k__BackingField;
        if (gm != nullptr)
            app::GameManager_RpcEndGame(gm, GameOverReason__Enum(0), false, NULL);
    }

    // Start the next round. Same start calls as the Host tab's "Force Start of
    // Game" (host_tab.cpp:436) and the old drain's.
    static bool StartRound() {
        if (!s_running || !HasHostAuthority() || !IsInLobby()) return false;
        app::AmongUsClient_KickNotJoinedPlayers(*Game::pAmongUsClient, NULL);
        app::InnerNetClient_SendStartGame((InnerNetClient*)(*Game::pAmongUsClient), NULL);
        return true;
    }

    // One round finished: count it, and either stop at the user's target or
    // arm the next round. The lobby that just ran a round is now settled and
    // may use the fast path.
    static void CompleteRound() {
        s_joinSent = false;
        s_sceneReloadSent = false;
        if (++s_roundsDone >= s_roundsTarget) {
            StopWithMessage("Done: " + std::to_string(s_roundsDone) + " round(s).");
            return;
        }
        s_roundsHere++;
        s_readySince = -1.f;
        s_stageAt = Time_get_time(NULL);
        s_stage = Stage::Idle;
    }

    // Leave the lobby cleanly when giving up, so the next lap starts from the
    // menu. Fast.cs makes this exact move when it aborts.
    static void LeaveLobby() {
        try {
            if (Game::pAmongUsClient != nullptr && *Game::pAmongUsClient != nullptr)
                app::AmongUsClient_ExitGame(*Game::pAmongUsClient, DisconnectReasons__Enum::ExitGame, NULL);
        }
        catch (...) {
        }
    }

    static void Fail(const std::string& why) {
        s_fails++;
        s_readySince = -1.f;
        s_endSent = false;
        s_joinSent = false;
        s_joinAt = -1.f;
        s_sceneReloadSent = false;
        s_stageAt = Time_get_time(NULL);

        LOG_ERROR(std::format("Drain XP v2: {} - failure {} of {}", why, s_fails, s_MaxFails));

        if (s_fails >= s_MaxFails) {
            if (s_broken) {
                // Even loaded rounds are refusing - nothing left to try.
                s_running = false;
                s_stage = Stage::Idle;
                LeaveLobby();
                Toast("Stopped (" + why + ").");
                return;
            }
            // Step 3 off: the fast loop is being refused. Keep the run going on
            // loaded rounds (the old drain's proven path) for the remaining
            // rounds, and let the loop try the fast path again after a rejoin.
            s_broken = true;
            s_fails = 0;
            s_stage = Stage::Idle;
            Toast("Fast loop off (" + why + ") - loaded rounds for the rest of this run.");
            return;
        }
        s_stage = Stage::Idle;
    }

    // ------------------------------------------------------------------
    // Ticks
    // ------------------------------------------------------------------

    void Tick() {
        if (!s_running) return;
        if (State.PanicMode) {
            StopWithMessage("Stopped (panic mode).");
            return;
        }
        if (!HasHostAuthority()) {
            if (s_stage != Stage::Idle) {
                s_stage = Stage::Idle;
                s_readySince = -1.f;
            }
            return;
        }

        auto* client = (InnerNetClient*)(*Game::pAmongUsClient);
        if (client == nullptr) return;

        const float now = Time_get_time(NULL);

        switch (s_stage) {
        case Stage::Starting:
            // Waiting for the server's start confirmation (handled in HandleMessage).
            if (now - s_stageAt > s_StageTimeout) {
                Fail("the server never confirmed the start");
                return;
            }
            break;

        case Stage::Ending:
            // Waiting for the server's end confirmation (handled in HandleMessage).
            if (now - s_stageAt > s_StageTimeout) {
                Fail("the server never confirmed the end");
                return;
            }
            break;

        case Stage::Rejoining: {
            // The lobby came back on its own (a loaded round's normal return),
            // or JoinGame below flipped GameState back - either way we are ready.
            if (LobbyReady(client)) {
                CompleteRound();
                break;
            }
            // End confirmed, no auto-return: rejoin the same lobby the way the
            // end screen's "Play Again" button does.
            if (!s_joinSent && client->fields.GameState == InnerNetClient_GameStates__Enum::Ended) {
                app::InnerNetClient_JoinGame((InnerNetClient*)(*Game::pAmongUsClient), true, NULL);
                s_joinSent = true;
                s_joinAt = now;
            }
            // SkyMenuLink's rejoin safety net (Fast.cs "Rejoin"): give the join
            // one second, then if a map is standing but the lobby did not come
            // back, the join loaded the wrong scene (a half-loaded round state)
            // - pull the lobby scene in by hand. On the fast rounds no map ever
            // loads, so ShipStatus stays null here and nothing is forced; the
            // rejoin timeout below covers that branch, the same way Fast.cs
            // simply waits on it.
            if (s_joinSent && !s_sceneReloadSent && now - s_joinAt >= 1.f
                && ShipStatus__TypeInfo->static_fields->Instance != nullptr) {
                s_sceneReloadSent = true;
                LOG_ERROR(std::format("Drain XP v2: rejoin landed on a map scene - loading the lobby by hand."));
                app::SceneManager_LoadScene(convert_to_string("OnlineGame"), NULL);
            }
            if (now - s_stageAt > s_RejoinTimeout) {
                Fail("the lobby did not come back after the end");
                return;
            }
            break;
        }

        case Stage::Idle:
        default: {
            if (!LobbyReady(client)) {
                if (s_readySince >= 0.f) s_readySince = -1.f;
                // Never became usable again (e.g. stuck in GameState Ended).
                if (now - s_stageAt > s_IdleTimeout) {
                    Fail("the lobby never became ready");
                    return;
                }
                return;
            }
            if (s_readySince < 0.f) s_readySince = now;

            if (client->fields.GameId != s_lobby) {
                s_lobby = client->fields.GameId;
                s_roundsHere = 0;
            }

            if (s_roundsHere == 0) {
                // First start in this lobby: never fast, and it needs the
                // lobby to have been there a moment.
                if (now - s_readySince < s_FirstStartSettle) return;
            }
            else if (now - s_readySince < s_Gap) {
                return;
            }

            s_readySince = -1.f;
            if (!StartRound()) return;
            s_stage = Stage::Starting;
            s_stageAt = now;
            break;
        }
        }
    }

    // Called from dInnerNetClient_HandleMessage for every incoming message.
    // Returns true when the message must be swallowed (the server's start
    // confirmation on the fast path, so nothing loads).
    bool HandleMessage(InnerNetClient* client, MessageReader* reader) {
        if (!s_running || s_stage == Stage::Idle) return false;
        if (client == nullptr || reader == nullptr) return false;

        const uint8_t tag = reader->fields.Tag;

        // Step 3: the server confirmed the start. If step 3 is on, the lobby
        // is settled (at least one round done here) and we are alone, swallow
        // it - no map, no ShipStatus, nothing drawn - and end the round
        // immediately. Otherwise let it through and play a normal round.
        if (tag == s_TagStartGame && s_stage == Stage::Starting) {
            if (!s_broken && s_roundsHere > 0 && Alone()) {
                s_stage = Stage::Ending;
                s_stageAt = Time_get_time(NULL);
                SendEnd();
                return true; // swallow: no round materializes
            }
            if (!s_broken && s_roundsHere > 0) {
                // Somebody else is in the lobby - a phantom round would drop
                // them. Play this and the rest loaded.
                s_broken = true;
            }
            return false; // loaded round; TickGameLoaded ends it once it spawns
        }

        // The server ended the game. Let the game handle it (OnGameEnd is what
        // writes the XP grant down) and move on to the rejoin. The end going
        // out is what re-arms the next round's own end.
        if (tag == s_TagEndGame && s_stage == Stage::Ending) {
            s_endSent = false;
            s_stage = Stage::Rejoining;
            s_stageAt = Time_get_time(NULL);
        }

        return false;
    }

    // Loaded-round fallback: the round really loaded. End it once it is safe,
    // exactly like the old drain (never mid-load).
    void TickGameLoaded() {
        if (!s_running || s_stage != Stage::Starting) return;
        if (s_endSent) return;
        s_stage = Stage::Ending;
        s_stageAt = Time_get_time(NULL);
        SendEnd();
    }

    // ------------------------------------------------------------------
    // Public API (Game tab)
    // ------------------------------------------------------------------

    int GetRunningStatus() { return s_running ? 1 : 0; }
    int GetRoundsDone() { return s_roundsDone; }
    int GetRoundsTarget() { return s_roundsTarget; }

    void Start() {
        if (s_running) return;
        if (!IsInLobby()) {
            Toast("Start from the lobby!");
            return;
        }
        if (!HasHostAuthority()) {
            Toast("Host only!");
            return;
        }

        auto* client = (InnerNetClient*)(*Game::pAmongUsClient);
        if (client == nullptr) return;

        // The two drains must not run at the same time - both would end rounds.
        DrainXp::Stop();

        s_running = true;
        s_roundsDone = 0;
        s_roundsTarget = std::clamp(State.DrainXpV2Rounds, 1, 1000);
        s_stage = Stage::Idle;
        s_endSent = false;
        s_joinSent = false;
        s_joinAt = -1.f;
        s_sceneReloadSent = false;
        s_broken = false;
        s_fails = 0;
        s_readySince = -1.f;
        s_lobby = client->fields.GameId;
        s_roundsHere = 0;
        s_stageAt = Time_get_time(NULL);

        Toast("Starting: " + std::to_string(s_roundsTarget) + " round(s).");
    }

    void Stop() {
        StopWithMessage("Stopped after " + std::to_string(s_roundsDone) + " round(s).");
    }
}