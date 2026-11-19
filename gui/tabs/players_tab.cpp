#include "pch-il2cpp.h"
#include "players_tab.h"
#include "game.h"
#include "state.hpp"
#include "toasts.hpp"
#include "logger.h"
#include "utility.h"
#include "gui-helpers.hpp"
#include "../UiUpdateQueue.hpp"
#include <future>
#include <_hooks.h>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <deque>
#include <string>
#include <utility>
#include "../../improvements/DrainXp.h"
#include "../../improvements/DrainXpV2.h"

namespace PlayersTab {
    int framesPassed = -1;
    Vector2 previousPlayerPosition;
    static std::string forcedName = "";
    static int forcedColor = 0;
    static bool protectionToggleUiEnabled = false;
    static bool protectionCrewmatesOnly = false;
    static int protectionIntervalMs = 50;
    static std::vector<Game::PlayerId> protectionTargets = {};
    static size_t protectionTargetIndex = 0;
    static std::chrono::steady_clock::time_point lastProtectionTime = std::chrono::steady_clock::now();
    static bool matchCycleProtectionOwnsSettings = false;
    static bool matchCyclePreviousProtectionEnabled = false;
    static bool matchCyclePreviousCrewmatesOnly = false;
    static int matchCyclePreviousProtectionIntervalMs = 50;
    static std::vector<Game::PlayerId> matchCyclePreviousProtectionTargets;
    static constexpr int matchCycleProtectionIntervalMs = 70;
    static bool killToggleUiEnabled = false;
    static int killIntervalMs = 500;
    static std::chrono::steady_clock::time_point lastKillTime = std::chrono::steady_clock::now();

    enum class XpControllerPhase {
        STOPPED,
        FARMING_ROUND,
        LOBBY_EVAL,
        ROLLOVER_LOOP,
    };

    enum class XpControllerStage {
        PreparingRound,
        AwaitingMatch,
        MatchTimer,
        AwaitingLobby,
        AwaitingRejoin,
        LobbyDelay,
        WaitingForParticipants,
        EvaluateLobby,
    };

    struct XpTrackedPlayer {
        bool has_earned_xp = false;
        bool has_completed_objective = false;
        bool is_host = false;
        bool is_impostor = false;
        bool is_shielded = false;
        bool needs_rejoin = false;
        bool has_farming_attempt = false;
        bool connected = false;
        bool disconnected_after_rejoin_request = false;
        bool was_impostor_this_round = false;
        bool farmed_this_round = false;
        std::string name;
    };

    struct XpControllerChatMessage {
        std::string text;
        std::chrono::seconds delayAfterSend{0};
    };

    static XpControllerPhase xpControllerPhase = XpControllerPhase::STOPPED;
    static XpControllerStage xpControllerStage = XpControllerStage::PreparingRound;
    static std::unordered_map<Game::PlayerId, XpTrackedPlayer> xpTrackedPlayers;
    static std::vector<Game::PlayerId> xpRoundImpostorIds;
    static std::chrono::steady_clock::time_point xpControllerDeadline;
    static std::chrono::steady_clock::time_point xpLastRosterCheck;
    static std::chrono::steady_clock::time_point xpTransitionGraceDeadline;
    static std::chrono::steady_clock::time_point xpParticipantWaitStarted;
    static std::chrono::steady_clock::time_point xpHostAuthorityLostAt;
    static std::chrono::steady_clock::time_point xpPostChatDeadline;
    static std::chrono::steady_clock::time_point xpNextChatSendAt;
    static std::chrono::steady_clock::time_point xpRoundChatBatchStartedAt;
    static std::chrono::milliseconds xpRoundChatWait{ 0 };
    static std::deque<XpControllerChatMessage> xpPendingChatMessages;
    static size_t xpRoundChatMessageCount = 0;
    static int64_t xpRoundChatPostDelaySeconds = 0;
    static size_t g_rpcEmittedThisFrame = 0;
    static constexpr size_t kRpcBudgetPerFrame = 32;
    static std::vector<Game::PlayerId> xpRolesPendingAssignment;
    static size_t xpRoleAssignmentCursor = 0;
    static size_t xpRoleAssignedPlayers = 0;
    static size_t xpRoleAssignedImpostors = 0;
    static bool xpRoleHostAssigned = false;
    static bool xpRoleAssignmentStarted = false;
    static bool xpRoleAssignmentComplete = false;
    static bool xpFarmingCycleAnnouncementPending = false;
    static Game::PlayerId xpFarmingRotationCursor = 0;
    static bool xpHasFarmingRotationCursor = false;
    static XpControllerStage xpStageBeforeParticipantWait = XpControllerStage::PreparingRound;
    static constexpr auto xpRoundPrepareDelay = std::chrono::seconds(1);
    static constexpr auto xpFarmingAnnouncementDelay = std::chrono::seconds(10);
    static constexpr auto xpDrainAnnouncementDelay = std::chrono::seconds(30);
    static constexpr auto xpChatSendInterval = std::chrono::seconds(3);
    static int xpFarmingDurationSeconds = 180;
    static int xpDrainRoundsPerMinute = 7;
    static int xpDrainBetweenRoundRejoinSeconds = 20;
    static int xpFinalRejoinWindowSeconds = 60;
    static int xpDrainRoundsCompleted = 0;
    static int xpDrainRoundsTarget = 0;
    static constexpr auto xpRolloverDuration = std::chrono::seconds(35);
    static constexpr auto transitionGraceDuration = std::chrono::seconds(10);
    static constexpr auto xpHostAuthorityGraceDuration = std::chrono::seconds(10);
    static constexpr auto xpStageTimeout = std::chrono::seconds(60);
    static constexpr auto xpRosterCheckInterval = std::chrono::milliseconds(70);

    static void SetShieldState(Game::PlayerId playerId, bool enabled);
    static void RestoreMatchCycleProtectionSettings();

    static bool IsMatchCycleHost(const PlayerControl* player) {
        return player != nullptr
            && Game::pLocalPlayer != nullptr
            && *Game::pLocalPlayer != nullptr
            && player->fields.PlayerId == (*Game::pLocalPlayer)->fields.PlayerId;
    }

    static int GetMatchCycleTeamCap(size_t nonHostParticipants) {
        if (nonHostParticipants >= 7) return 3;
        if (nonHostParticipants >= 4) return 2;
        if (nonHostParticipants >= 1) return 1;
        return 0;
    }

    bool IsXpControllerRunning() {
        return xpControllerPhase != XpControllerPhase::STOPPED;
    }

    static const char* XpControllerPhaseName(XpControllerPhase phase) {
        switch (phase) {
        case XpControllerPhase::FARMING_ROUND: return "FARMING_ROUND";
        case XpControllerPhase::LOBBY_EVAL: return "LOBBY_EVAL";
        case XpControllerPhase::ROLLOVER_LOOP: return "ROLLOVER_LOOP";
        case XpControllerPhase::STOPPED: return "STOPPED";
        }
        return "UNKNOWN";
    }

    static const char* XpControllerStageName(XpControllerStage stage) {
        switch (stage) {
        case XpControllerStage::PreparingRound: return "PreparingRound";
        case XpControllerStage::AwaitingMatch: return "AwaitingMatch";
        case XpControllerStage::MatchTimer: return "MatchTimer";
        case XpControllerStage::AwaitingLobby: return "AwaitingLobby";
        case XpControllerStage::AwaitingRejoin: return "AwaitingRejoin";
        case XpControllerStage::LobbyDelay: return "LobbyDelay";
        case XpControllerStage::WaitingForParticipants: return "WaitingForParticipants";
        case XpControllerStage::EvaluateLobby: return "EvaluateLobby";
        }
        return "Unknown";
    }

    static void LogXpControllerEvent(const std::string& message) {
        Log.Info("MATCH CYCLE", message);
    }

    static void NotifyXpController(const std::string& title, const std::string& message, ImVec4 color) {
        LogXpControllerEvent(std::format("{}: {}", title, message));
        Toasts::AddToast(title, message, color);
    }

    static std::vector<PlayerControl*> GetConnectedXpPlayers() {
        std::vector<PlayerControl*> players;
        for (auto player : GetAllPlayerControl()) {
            if (player == nullptr) continue;
            auto data = GetPlayerData(player);
            if (data == nullptr || data->fields.Disconnected) continue;
            players.push_back(player);
        }
        std::sort(players.begin(), players.end(), [](PlayerControl* left, PlayerControl* right) {
            return left->fields.PlayerId < right->fields.PlayerId;
        });
        return players;
    }

    static size_t GetConnectedNonHostCount() {
        const auto players = GetConnectedXpPlayers();
        return static_cast<size_t>(std::count_if(players.begin(), players.end(), [](PlayerControl* player) {
            return !IsMatchCycleHost(player);
        }));
    }

    static std::string XpPlayerName(PlayerControl* player) {
        auto data = GetPlayerData(player);
        if (data == nullptr) return "Unknown";
        std::string name = RemoveHtmlTags(convert_from_string(NetworkedPlayerInfo_get_PlayerName(data, nullptr)));
        return name.empty() ? "Unknown" : name;
    }

    static std::string BuildXpControllerDiagnostics() {
        std::string details = std::format(
            "phase={}, stage={}, inLobby={}, inGame={}, connectedNonHost={}, trackedPlayers={}",
            XpControllerPhaseName(xpControllerPhase), XpControllerStageName(xpControllerStage),
            IsInLobby(), IsInGame(), GetConnectedNonHostCount(), xpTrackedPlayers.size());
        const auto connectedPlayers = GetConnectedXpPlayers();
        for (auto player : connectedPlayers) {
            const auto playerId = player->fields.PlayerId;
            const auto data = GetPlayerData(player);
            const auto tracked = xpTrackedPlayers.find(playerId);
            const std::string name = XpPlayerName(player);
            if (tracked == xpTrackedPlayers.end()) {
                details += std::format("; player[{}]='{}' host={} dead={} level={} tracked=false",
                    playerId, name, IsMatchCycleHost(player), data != nullptr && data->fields.IsDead,
                    data != nullptr ? data->fields.PlayerLevel : 0);
                continue;
            }

            const auto& state = tracked->second;
            details += std::format(
                "; player[{}]='{}' host={} dead={} roundImpostor={} farmedThisRound={} objectiveComplete={} impostor={} shielded={} needsRejoin={} connected={}",
                playerId, name, state.is_host, data != nullptr && data->fields.IsDead,
                state.was_impostor_this_round, state.farmed_this_round,
                state.has_completed_objective, state.is_impostor, state.is_shielded,
                state.needs_rejoin, state.connected);
        }
        for (const auto& [playerId, state] : xpTrackedPlayers) {
            const bool currentlyConnected = std::any_of(
                connectedPlayers.begin(), connectedPlayers.end(), [playerId](PlayerControl* player) {
                    return player->fields.PlayerId == playerId;
                });
            if (currentlyConnected) continue;
            details += std::format(
                "; player[{}]='{}' disconnected=true host={} roundImpostor={} farmedThisRound={} objectiveComplete={} impostor={} shielded={} needsRejoin={}",
                playerId, state.name, state.is_host, state.was_impostor_this_round, state.farmed_this_round,
                state.has_completed_objective, state.is_impostor, state.is_shielded,
                state.needs_rejoin);
        }
        return details;
    }

    static bool CanSendXpControllerChat() {
        return Game::pLocalPlayer != nullptr
            && *Game::pLocalPlayer != nullptr
            && Game::HudManager.GetInstance() != nullptr
            && Game::HudManager.GetInstance()->fields.Chat != nullptr;
    }

    static std::string SanitizeXpControllerChatText(const std::string& message) {
        std::string sanitized = message;
        sanitized.erase(std::remove_if(sanitized.begin(), sanitized.end(), [](char character) {
            return character == '[' || character == ']' || character == '{'
                || character == '}' || character == '<' || character == '>';
        }), sanitized.end());
        return sanitized;
    }

    static std::string XpControllerChatName(const std::string& name) {
        std::string sanitized = SanitizeXpControllerChatText(name);
        return sanitized.empty() ? "Player" : sanitized;
    }

    static void QueueXpControllerChat(
        const std::string& message,
        std::chrono::seconds delayAfterSend = std::chrono::seconds::zero()) {
        if (!CanSendXpControllerChat()) {
            Toasts::AddToast("XP Controller", "Lobby chat is unavailable; status message was not sent.", ImVec4(1.f, 0.3f, 0.3f, 1.f));
            return;
        }

        const std::string chatMessage = SanitizeXpControllerChatText(message);
        if (chatMessage.empty()) return;

        if (xpPendingChatMessages.empty()) {
            xpRoundChatBatchStartedAt = std::chrono::steady_clock::now();
        }
        constexpr size_t maxChatLength = 100;
        size_t start = 0;
        size_t queuedChunks = 0;
        while (start < chatMessage.size()) {
            size_t end = (std::min)(start + maxChatLength, chatMessage.size());
            if (end < chatMessage.size()) {
                const size_t lastSpace = chatMessage.rfind(' ', end);
                if (lastSpace != std::string::npos && lastSpace > start) {
                    end = lastSpace;
                }
            }
            xpPendingChatMessages.push_back({
                chatMessage.substr(start, end - start),
                end == chatMessage.size() ? delayAfterSend : std::chrono::seconds::zero(),
            });
            ++queuedChunks;
            start = end;
            while (start < chatMessage.size() && chatMessage[start] == ' ') ++start;
        }
        xpRoundChatMessageCount += queuedChunks;
        if (queuedChunks > 0) xpRoundChatPostDelaySeconds += delayAfterSend.count();
    }

    static void TickXpControllerChat() {
        if (xpPendingChatMessages.empty() || !IsInLobby()) return;

        const auto now = std::chrono::steady_clock::now();
        if (now < xpNextChatSendAt || State.ChatCooldown < 3.f) return;
        if (!CanSendXpControllerChat()) return;

        auto queuedMessage = std::move(xpPendingChatMessages.front());
        xpPendingChatMessages.pop_front();
        State.lobbyRpcQueue.push(new RpcSendChat(*Game::pLocalPlayer, queuedMessage.text));
        State.MessageSent = true;
        LogXpControllerEvent(std::format(
            "Queued chat through Sicko RpcSendChat ({} chars): '{}'.",
            queuedMessage.text.size(), queuedMessage.text));
        xpNextChatSendAt = now + xpChatSendInterval;
        xpPostChatDeadline = now + queuedMessage.delayAfterSend;
        if (xpPendingChatMessages.empty()) {
            xpRoundChatWait += std::chrono::duration_cast<std::chrono::milliseconds>(
                now - xpRoundChatBatchStartedAt);
            xpRoundChatWait += std::chrono::duration_cast<std::chrono::milliseconds>(
                queuedMessage.delayAfterSend);
        }
    }

    static void BroadcastXpControllerPlayerList(
        const std::string& label,
        const std::vector<std::string>& names,
        std::chrono::seconds delayAfterSend = std::chrono::seconds::zero()) {
        constexpr size_t maxChatLength = 100;
        const std::string prefix = label + ":";
        std::vector<std::string> chatNames;
        chatNames.reserve(names.size());
        for (const auto& name : names) chatNames.push_back(XpControllerChatName(name));
        std::string message = prefix;
        bool hasNames = false;
        const size_t firstPendingMessage = xpPendingChatMessages.size();
        const auto flushMessage = [&]() {
            QueueXpControllerChat(message);
        };
        const auto delayAfterLastMessage = [&]() {
            if (delayAfterSend > std::chrono::seconds::zero()
                && xpPendingChatMessages.size() > firstPendingMessage) {
                xpPendingChatMessages.back().delayAfterSend = delayAfterSend;
                xpRoundChatPostDelaySeconds += delayAfterSend.count();
            }
        };

        if (chatNames.empty()) {
            QueueXpControllerChat(message + " none");
            delayAfterLastMessage();
            return;
        }
        for (const auto& name : chatNames) {
            const std::string entry = (hasNames ? ", " : " ") + name;
            if (message.size() + entry.size() > maxChatLength && hasNames) {
                flushMessage();
                message = prefix;
                hasNames = false;
            }
            if (entry.size() > maxChatLength - message.size()) {
                QueueXpControllerChat(prefix + " " + name);
            }
            else {
                message += entry;
                hasNames = true;
            }
        }
        if (hasNames || message != prefix) flushMessage();
        delayAfterLastMessage();
    }

    static void BroadcastXpControllerSummary() {
        std::vector<Game::PlayerId> participantIds;
        participantIds.reserve(xpTrackedPlayers.size());
        for (const auto& [playerId, tracked] : xpTrackedPlayers) {
            if (!tracked.is_host) participantIds.push_back(playerId);
        }
        std::sort(participantIds.begin(), participantIds.end());

        std::vector<std::string> farmedThisRound;
        std::vector<std::string> stillNeedFarming;
        for (const auto playerId : participantIds) {
            const auto& tracked = xpTrackedPlayers.at(playerId);
            if (tracked.has_completed_objective) {
                farmedThisRound.push_back(tracked.name);
            }
            if (!tracked.has_completed_objective) stillNeedFarming.push_back(tracked.name);
        }
        BroadcastXpControllerPlayerList("FARMED", farmedThisRound);
        BroadcastXpControllerPlayerList("NEED", stillNeedFarming, xpFarmingAnnouncementDelay);
    }

    static void SynchronizeXpPlayers() {
        for (auto& [playerId, tracked] : xpTrackedPlayers) {
            tracked.connected = false;
        }

        for (auto player : GetConnectedXpPlayers()) {
            const auto playerId = player->fields.PlayerId;
            auto it = xpTrackedPlayers.try_emplace(playerId).first;
            auto& tracked = it->second;
            tracked.name = XpPlayerName(player);
            tracked.is_host = IsMatchCycleHost(player);

            if (!tracked.is_host && tracked.needs_rejoin && tracked.disconnected_after_rejoin_request) {
                tracked.needs_rejoin = false;
                tracked.disconnected_after_rejoin_request = false;
            }
            tracked.connected = true;
        }

        for (auto& [playerId, tracked] : xpTrackedPlayers) {
            if (!tracked.connected && tracked.needs_rejoin) {
                tracked.disconnected_after_rejoin_request = true;
            }
        }
    }

    static void SnapshotXpRoundRoles() {
        for (auto& [playerId, tracked] : xpTrackedPlayers) {
            tracked.was_impostor_this_round = std::find(
                xpRoundImpostorIds.begin(), xpRoundImpostorIds.end(), playerId)
                != xpRoundImpostorIds.end();
            tracked.farmed_this_round = false;
        }
        for (auto player : GetConnectedXpPlayers()) {
            const auto playerId = player->fields.PlayerId;
            auto& tracked = xpTrackedPlayers[playerId];
            tracked.name = XpPlayerName(player);
            tracked.is_host = IsMatchCycleHost(player);
            tracked.was_impostor_this_round = !tracked.is_host && std::find(
                xpRoundImpostorIds.begin(), xpRoundImpostorIds.end(), playerId)
                != xpRoundImpostorIds.end();
            LogXpControllerEvent(std::format(
                "Round assignment snapshot: player='{}' id={} role={} host={} completedBeforeRound={}.",
                tracked.name, playerId,
                tracked.was_impostor_this_round ? "Impostor" : "Crewmate",
                tracked.is_host, tracked.has_completed_objective));
        }
    }

    static void EvaluateCompletedFarmingRound() {
        for (const auto playerId : xpRoundImpostorIds) {
            auto it = xpTrackedPlayers.find(playerId);
            const bool isLocalHost = Game::pLocalPlayer != nullptr
                && *Game::pLocalPlayer != nullptr
                && playerId == (*Game::pLocalPlayer)->fields.PlayerId;
            if (it == xpTrackedPlayers.end() || it->second.is_host || isLocalHost) continue;

            auto& tracked = it->second;
            tracked.was_impostor_this_round = true;
            tracked.farmed_this_round = true;
            tracked.has_earned_xp = true;
            tracked.has_completed_objective = true;
            LogXpControllerEvent(std::format(
                "Farming completion: player='{}' id={} host=false assignedImpostorThisRound=true currentRole={} farmedThisRound={} farmingComplete={}.",
                tracked.name, playerId,
                tracked.is_impostor ? "Impostor" : "Crewmate",
                tracked.farmed_this_round, tracked.has_completed_objective));
        }
        LogXpControllerEvent(std::format(
            "Completed farming round credited {} assigned Impostor(s) from the immutable round assignment.",
            xpRoundImpostorIds.size()));
        NotifyXpController(
            "Farming round complete",
            std::format("Credited {} assigned Impostor(s).", xpRoundImpostorIds.size()),
            ImVec4(0.f, 1.f, 0.5f, 1.f));
    }

    static void StartNextXpCycle();

    static void RequestRejoinForDrainCompletion() {
        size_t rejoinParticipantCount = 0;
        for (auto& entry : xpTrackedPlayers) {
            auto& tracked = entry.second;
            if (tracked.is_host) continue;
            tracked.needs_rejoin = true;
            tracked.disconnected_after_rejoin_request = !tracked.connected;
            ++rejoinParticipantCount;
        }

        if (rejoinParticipantCount == 0) {
            LogXpControllerEvent("Drain round target reached with no tracked non-host participants to request rejoin.");
            StartNextXpCycle();
            return;
        }

        QueueXpControllerChat("Rejoin now. If you don't, the next loop starts without you.");
        LogXpControllerEvent(std::format(
            "Drain target reached ({}/{} rounds); requesting {} participant(s) to rejoin for up to {}s.",
            xpDrainRoundsCompleted, xpDrainRoundsTarget, rejoinParticipantCount, xpFinalRejoinWindowSeconds));
        NotifyXpController(
            "Rejoin requested",
            std::format("{} participant(s); waiting up to {}s.", rejoinParticipantCount, xpFinalRejoinWindowSeconds),
            ImVec4(1.f, 0.7f, 0.2f, 1.f));
        xpControllerStage = XpControllerStage::AwaitingRejoin;
        xpControllerDeadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(xpFinalRejoinWindowSeconds);
    }

    static bool AllRequestedPlayersRejoined() {
        for (const auto& [playerId, tracked] : xpTrackedPlayers) {
            if (!tracked.is_host && tracked.needs_rejoin) return false;
        }
        return true;
    }

    static bool AllConnectedPlayersEarnedXp() {
        const auto players = GetConnectedXpPlayers();
        const size_t participantCount = GetConnectedNonHostCount();
        return participantCount > 0 && std::all_of(players.begin(), players.end(), [](PlayerControl* player) {
            if (IsMatchCycleHost(player)) return true;
            auto it = xpTrackedPlayers.find(player->fields.PlayerId);
            return it != xpTrackedPlayers.end() && it->second.has_completed_objective;
        });
    }

    static void PauseForNonHostParticipants(std::chrono::steady_clock::time_point now) {
        if (xpControllerStage == XpControllerStage::WaitingForParticipants) return;
        LogXpControllerEvent(std::format(
            "Pausing stage={} because no non-host participants are connected; inLobby={}, trackedPlayers={}.",
            XpControllerStageName(xpControllerStage), IsInLobby(), xpTrackedPlayers.size()));
        xpStageBeforeParticipantWait = xpControllerStage;
        xpControllerStage = XpControllerStage::WaitingForParticipants;
        xpParticipantWaitStarted = now;
    }

    static void ResumeAfterNonHostParticipantReturns(std::chrono::steady_clock::time_point now) {
        const auto pausedFor = std::chrono::duration_cast<std::chrono::seconds>(now - xpParticipantWaitStarted).count();
        LogXpControllerEvent(std::format(
            "Resuming stage={} after non-host participant returned; connectedNonHost={}, pausedSeconds={}.",
            XpControllerStageName(xpStageBeforeParticipantWait), GetConnectedNonHostCount(), pausedFor));
        xpControllerDeadline += now - xpParticipantWaitStarted;
        xpControllerStage = xpStageBeforeParticipantWait;
        xpLastRosterCheck = now;
    }

    static void StopXpController(const std::string& reason, bool showToast = true) {
        if (!IsXpControllerRunning()) return;
        LogXpControllerEvent(std::format(
            "Stopping: reason='{}'. {}",
            reason, BuildXpControllerDiagnostics()));
        xpControllerPhase = XpControllerPhase::STOPPED;
        xpControllerStage = XpControllerStage::PreparingRound;
        xpHostAuthorityLostAt = {};
        xpRoundImpostorIds.clear();
        xpRolesPendingAssignment.clear();
        xpRoleAssignmentCursor = 0;
        xpRoleAssignedPlayers = 0;
        xpRoleAssignedImpostors = 0;
        xpRoleHostAssigned = false;
        xpRoleAssignmentStarted = false;
        xpRoleAssignmentComplete = false;
        xpFarmingCycleAnnouncementPending = false;
        xpDrainRoundsCompleted = 0;
        xpDrainRoundsTarget = 0;
        xpPendingChatMessages.clear();
        xpRoundChatWait = std::chrono::milliseconds::zero();
        xpRoundChatMessageCount = 0;
        xpRoundChatPostDelaySeconds = 0;
        for (auto& [playerId, tracked] : xpTrackedPlayers) {
            if (tracked.is_shielded) SetShieldState(playerId, false);
        }
        RestoreMatchCycleProtectionSettings();
        xpTrackedPlayers.clear();
        if (showToast) {
            Toasts::AddToast("XP Controller", reason, ImVec4(1.f, 0.6f, 0.2f, 1.f));
        }
    }

    static int GetXpDrainRoundsTarget() {
        const int64_t totalDrainRounds =
            static_cast<int64_t>(xpFarmingDurationSeconds) * xpDrainRoundsPerMinute;
        return static_cast<int>((totalDrainRounds + 59) / 60);
    }

    static size_t CountXpControllerChatChunks(const std::string& message) {
        constexpr size_t maxChatLength = 100;
        const std::string chatMessage = SanitizeXpControllerChatText(message);
        size_t chunks = 0;
        size_t start = 0;
        while (start < chatMessage.size()) {
            size_t end = (std::min)(start + maxChatLength, chatMessage.size());
            if (end < chatMessage.size()) {
                const size_t lastSpace = chatMessage.rfind(' ', end);
                if (lastSpace != std::string::npos && lastSpace > start) end = lastSpace;
            }
            ++chunks;
            start = end;
            while (start < chatMessage.size() && chatMessage[start] == ' ') ++start;
        }
        return chunks;
    }

    static int64_t GetXpChatWaitSeconds(size_t messageCount, int64_t postMessageDelaySeconds) {
        const int64_t sendSpacingSeconds = messageCount > 1
            ? static_cast<int64_t>(messageCount - 1) * xpChatSendInterval.count()
            : 0;
        return sendSpacingSeconds + postMessageDelaySeconds;
    }

    static size_t CountXpControllerPlayerListChunks(
        const std::string& label,
        const std::vector<std::string>& names) {
        constexpr size_t maxChatLength = 100;
        const std::string prefix = label + ":";
        std::string message = prefix;
        size_t chunks = 0;
        bool hasNames = false;

        if (names.empty()) return CountXpControllerChatChunks(message + " none");

        for (const auto& rawName : names) {
            const std::string name = XpControllerChatName(rawName);
            const std::string entry = (hasNames ? ", " : " ") + name;
            if (message.size() + entry.size() > maxChatLength && hasNames) {
                chunks += CountXpControllerChatChunks(message);
                message = prefix;
                hasNames = false;
            }
            if (entry.size() > maxChatLength - message.size()) {
                chunks += CountXpControllerChatChunks(prefix + " " + name);
            }
            else {
                message += entry;
                hasNames = true;
            }
        }
        if (hasNames || message != prefix) chunks += CountXpControllerChatChunks(message);
        return chunks;
    }

    static int64_t GetXpFullCycleDurationEstimateSeconds(size_t announcementChunks) {
        std::vector<std::pair<Game::PlayerId, std::string>> participants;
        for (auto player : GetConnectedXpPlayers()) {
            if (IsMatchCycleHost(player)) continue;
            participants.emplace_back(
                player->fields.PlayerId,
                XpControllerChatName(XpPlayerName(player)));
        }
        if (participants.empty()) return 0;

        const size_t teamCap = static_cast<size_t>((std::max)(
            GetMatchCycleTeamCap(participants.size()), 1));
        const size_t drainRounds = static_cast<size_t>((std::max)(GetXpDrainRoundsTarget(), 0));
        std::vector<bool> completed(participants.size(), false);
        for (const auto playerId : xpRoundImpostorIds) {
            const auto it = std::find_if(participants.begin(), participants.end(), [playerId](const auto& participant) {
                return participant.first == playerId;
            });
            if (it != participants.end()) {
                completed[static_cast<size_t>(std::distance(participants.begin(), it))] = true;
            }
        }

        size_t cursor = 0;
        if (xpHasFarmingRotationCursor) {
            const auto nextPlayer = std::lower_bound(
                participants.begin(), participants.end(), xpFarmingRotationCursor,
                [](const auto& participant, Game::PlayerId playerId) {
                    return participant.first < playerId;
                });
            if (nextPlayer != participants.end()) {
                cursor = static_cast<size_t>(std::distance(participants.begin(), nextPlayer));
            }
        }

        int64_t estimate = xpFarmingDurationSeconds + 20 + xpRoundPrepareDelay.count();
        const int64_t initialMessageWait = GetXpChatWaitSeconds(
            xpRoundChatMessageCount + announcementChunks,
            xpRoundChatPostDelaySeconds + xpFarmingAnnouncementDelay.count());
        estimate += initialMessageWait * 2;
        size_t farmedCount = static_cast<size_t>(std::count(completed.begin(), completed.end(), true));
        const auto addRecapEstimate = [&]() {
            std::vector<std::string> farmedNames;
            std::vector<std::string> neededNames;
            for (size_t index = 0; index < participants.size(); ++index) {
                (completed[index] ? farmedNames : neededNames).push_back(participants[index].second);
            }
            const size_t recapChunks =
                CountXpControllerPlayerListChunks("FARMED", farmedNames)
                + CountXpControllerPlayerListChunks("NEED", neededNames);
            estimate += GetXpChatWaitSeconds(
                recapChunks, xpFarmingAnnouncementDelay.count()) * 2;
        };
        if (farmedCount < participants.size()) addRecapEstimate();

        while (farmedCount < participants.size()) {
            std::vector<size_t> selected;
            size_t lastSelected = cursor;
            for (size_t offset = 0; offset < participants.size() && selected.size() < teamCap; ++offset) {
                const size_t index = (cursor + offset) % participants.size();
                if (completed[index]) continue;
                selected.push_back(index);
                lastSelected = index;
            }
            if (selected.empty()) break;
            for (const auto index : selected) {
                completed[index] = true;
                ++farmedCount;
            }
            cursor = (lastSelected + 1) % participants.size();
            estimate += xpFarmingDurationSeconds + 20 + xpRoundPrepareDelay.count();
            if (farmedCount < participants.size()) {
                addRecapEstimate();
            }
        }

        const size_t drainAnnouncementChunks =
            CountXpControllerChatChunks(std::format(
                "Auto-drain starts in 30s: {} rounds, {}s between; each ends after 35s.",
                GetXpDrainRoundsTarget(), xpDrainBetweenRoundRejoinSeconds))
            + CountXpControllerChatChunks(
                "After draining, rejoin. If you don't, the next loop starts without you.");
        estimate += GetXpChatWaitSeconds(
            drainAnnouncementChunks, xpDrainAnnouncementDelay.count()) * 2;
        estimate += static_cast<int64_t>(drainRounds) * xpRolloverDuration.count();
        if (drainRounds > 1) {
            estimate += static_cast<int64_t>(drainRounds - 1) * xpDrainBetweenRoundRejoinSeconds;
        }
        estimate += xpFinalRejoinWindowSeconds;
        return estimate;
    }

    static std::string FormatXpDurationEstimate(int64_t totalSeconds) {
        const int64_t totalMinutes = (totalSeconds + 59) / 60;
        const int64_t hours = totalMinutes / 60;
        const int64_t minutes = totalMinutes % 60;
        return hours > 0
            ? std::format("{}h {}m", hours, minutes)
            : std::format("{}m", minutes);
    }

    static std::string BuildXpFarmingCycleAnnouncement(
        const std::vector<std::string>& firstImpostorNames) {
        std::string names = firstImpostorNames.empty() ? "none" : firstImpostorNames.front();
        for (size_t i = 1; i < firstImpostorNames.size(); ++i) names += ", " + firstImpostorNames[i];
        const auto formatMessage = [&names](int64_t estimate) {
            return std::format(
                "Farming runs {}s. First impostors: {}. Missed your turn? Next big loop in about {}.",
                xpFarmingDurationSeconds, names, FormatXpDurationEstimate(estimate));
        };
        int64_t estimate = GetXpFullCycleDurationEstimateSeconds(
            CountXpControllerChatChunks(formatMessage(0)));
        std::string message;
        for (int attempt = 0; attempt < 3; ++attempt) {
            message = formatMessage(estimate);
            const int64_t updatedEstimate = GetXpFullCycleDurationEstimateSeconds(
                CountXpControllerChatChunks(message));
            if (updatedEstimate == estimate) break;
            estimate = updatedEstimate;
        }
        return message;
    }

    static bool ConfigureMatchCycleTeams(bool unearnedOnly) {
        const auto players = GetConnectedXpPlayers();
        const size_t participantCount = GetConnectedNonHostCount();
        const int teamCap = GetMatchCycleTeamCap(participantCount);
        if (participantCount == 0 || players.empty()) {
            LogXpControllerEvent(std::format(
                "Team setup deferred: connectedPlayers={}, connectedNonHost={}, phase={}, stage={}.",
                players.size(), participantCount, XpControllerPhaseName(xpControllerPhase),
                XpControllerStageName(xpControllerStage)));
            PauseForNonHostParticipants(std::chrono::steady_clock::now());
            return false;
        }

        xpRoundImpostorIds.clear();
        xpRolesPendingAssignment.clear();
        xpRoleAssignmentCursor = 0;
        xpRoleAssignedPlayers = 0;
        xpRoleAssignedImpostors = 0;
        xpRoleHostAssigned = false;
        xpRoleAssignmentStarted = false;
        xpRoleAssignmentComplete = false;
        for (auto& [playerId, tracked] : xpTrackedPlayers) {
            tracked.was_impostor_this_round = false;
            tracked.farmed_this_round = false;
        }

        std::vector<PlayerControl*> nonHostPlayers;
        for (auto player : players) {
            if (!IsMatchCycleHost(player)) nonHostPlayers.push_back(player);
        }

        size_t startIndex = 0;
        if (unearnedOnly && xpHasFarmingRotationCursor) {
            const auto nextPlayer = std::lower_bound(
                nonHostPlayers.begin(), nonHostPlayers.end(), xpFarmingRotationCursor,
                [](PlayerControl* player, Game::PlayerId playerId) {
                    return player->fields.PlayerId < playerId;
                });
            if (nextPlayer != nonHostPlayers.end()) {
                startIndex = static_cast<size_t>(std::distance(nonHostPlayers.begin(), nextPlayer));
            }
        }

        std::vector<PlayerControl*> impostors;
        size_t lastSelectedIndex = startIndex;
        const auto selectImpostors = [&](bool skipAttempted) {
            impostors.clear();
            for (size_t offset = 0; offset < nonHostPlayers.size()
                && impostors.size() < static_cast<size_t>(teamCap); ++offset) {
                const size_t index = (startIndex + offset) % nonHostPlayers.size();
                auto player = nonHostPlayers[index];
                auto it = xpTrackedPlayers.find(player->fields.PlayerId);
                if (it == xpTrackedPlayers.end()
                    || (unearnedOnly && it->second.has_completed_objective)
                    || (unearnedOnly && skipAttempted && it->second.has_farming_attempt)) continue;
                impostors.push_back(player);
                lastSelectedIndex = index;
            }
        };
        if (unearnedOnly) selectImpostors(true);
        else if (xpControllerPhase == XpControllerPhase::ROLLOVER_LOOP
            && !nonHostPlayers.empty()) {
            const auto randomIndex = static_cast<size_t>(
                GenerateRandomNumber(0, static_cast<int>(nonHostPlayers.size() - 1)));
            impostors.push_back(nonHostPlayers[randomIndex]);
        }

        if (unearnedOnly && impostors.empty()) {
            for (auto player : players) {
                if (IsMatchCycleHost(player)) continue;
                auto it = xpTrackedPlayers.find(player->fields.PlayerId);
                if (it != xpTrackedPlayers.end() && !it->second.has_completed_objective) {
                    it->second.has_farming_attempt = false;
                }
            }
            selectImpostors(false);
        }

        if (unearnedOnly && impostors.empty()) {
            xpControllerPhase = XpControllerPhase::LOBBY_EVAL;
            xpControllerStage = XpControllerStage::EvaluateLobby;
            return false;
        }

        for (auto player : impostors) {
            xpTrackedPlayers[player->fields.PlayerId].has_farming_attempt = true;
        }
        xpRoundImpostorIds.clear();
        xpRoundImpostorIds.reserve(impostors.size());
        std::string impostorIds;
        for (auto player : impostors) {
            xpRoundImpostorIds.push_back(player->fields.PlayerId);
            if (!impostorIds.empty()) impostorIds += ",";
            impostorIds += std::format("{}", player->fields.PlayerId);
        }
        if (unearnedOnly && !impostors.empty()) {
            xpFarmingRotationCursor =
                nonHostPlayers[(lastSelectedIndex + 1) % nonHostPlayers.size()]->fields.PlayerId;
            xpHasFarmingRotationCursor = true;
        }
        for (auto& entry : xpTrackedPlayers) {
            entry.second.is_impostor = false;
        }
        for (auto player : impostors) {
            xpTrackedPlayers[player->fields.PlayerId].is_impostor = true;
        }
        LogXpControllerEvent(std::format(
            "Configuring teams: phase={}, connectedPlayers={}, connectedNonHost={}, attackerCap={}, attackersSelected={}, attackerIds=[{}], shieldCap={}.",
            XpControllerPhaseName(xpControllerPhase), players.size(), participantCount, teamCap,
            impostors.size(), impostorIds, teamCap));
        if (unearnedOnly) {
            std::vector<std::string> selectedNames;
            for (auto player : impostors) selectedNames.push_back(XpPlayerName(player));
            std::string nameList = selectedNames.empty() ? "none" : selectedNames.front();
            for (size_t i = 1; i < selectedNames.size(); ++i) {
                nameList += ", " + selectedNames[i];
            }
            NotifyXpController(
                "Farming batch selected",
                std::format("Impostor{}: {} (IDs {}).",
                    selectedNames.size() == 1 ? "" : "s", nameList, impostorIds),
                ImVec4(0.2f, 0.8f, 1.f, 1.f));
        }
        else {
            NotifyXpController(
                "Auto-drain role selected",
                std::format("{} is the random Impostor; protection is off.",
                    impostors.empty() ? "No player" : XpPlayerName(impostors.front())),
                ImVec4(1.f, 0.7f, 0.2f, 1.f));
        }

        std::vector<PlayerControl*> defenders;
        for (auto player : players) {
            const auto playerId = player->fields.PlayerId;
            auto& tracked = xpTrackedPlayers[playerId];
            tracked.is_host = IsMatchCycleHost(player);
            tracked.is_impostor = std::find(impostors.begin(), impostors.end(), player) != impostors.end();
            auto data = GetPlayerData(player);
            if (!tracked.is_impostor && data != nullptr && !data->fields.IsDead) defenders.push_back(player);
        }
        std::stable_sort(defenders.begin(), defenders.end(), [](PlayerControl* left, PlayerControl* right) {
            return !IsMatchCycleHost(left) && IsMatchCycleHost(right);
        });

        if (unearnedOnly) {
            std::vector<Game::PlayerId> desiredShields;
            for (auto player : defenders) {
                if (desiredShields.size() == static_cast<size_t>(teamCap)) break;
                desiredShields.push_back(player->fields.PlayerId);
            }

            for (auto& [playerId, tracked] : xpTrackedPlayers) {
                const bool shouldShield = std::find(desiredShields.begin(), desiredShields.end(), playerId) != desiredShields.end();
                if (tracked.is_shielded && !shouldShield) {
                    SetShieldState(playerId, false);
                    tracked.is_shielded = false;
                }
            }
            for (const auto playerId : desiredShields) {
                auto& tracked = xpTrackedPlayers[playerId];
                if (!tracked.is_shielded) {
                    SetShieldState(playerId, true);
                    tracked.is_shielded = true;
                }
            }
        }
        else {
            for (auto& [playerId, tracked] : xpTrackedPlayers) {
                if (tracked.is_shielded) {
                    SetShieldState(playerId, false);
                    tracked.is_shielded = false;
                }
            }
            RestoreMatchCycleProtectionSettings();
        }

        SnapshotXpRoundRoles();
        return true;
    }

    static void ReconcileMatchCycleShields() {
        if (!IsXpControllerRunning() || !IsHost()
            || xpControllerPhase == XpControllerPhase::ROLLOVER_LOOP) return;

        const auto connectedPlayers = GetConnectedXpPlayers();
        const int teamCap = GetMatchCycleTeamCap(GetConnectedNonHostCount());
        std::vector<PlayerControl*> eligibleDefenders;
        for (auto player : connectedPlayers) {
            auto tracked = xpTrackedPlayers.find(player->fields.PlayerId);
            auto data = GetPlayerData(player);
            if (tracked == xpTrackedPlayers.end() || tracked->second.is_impostor
                || data == nullptr || data->fields.IsDead) continue;
            eligibleDefenders.push_back(player);
        }
        std::stable_sort(eligibleDefenders.begin(), eligibleDefenders.end(), [](PlayerControl* left, PlayerControl* right) {
            return !IsMatchCycleHost(left) && IsMatchCycleHost(right);
        });

        std::vector<Game::PlayerId> desiredShields;
        const auto desiredCount = (std::min)(static_cast<size_t>((std::max)(teamCap, 0)), eligibleDefenders.size());
        for (auto player : eligibleDefenders) {
            if (desiredShields.size() == desiredCount) break;
            desiredShields.push_back(player->fields.PlayerId);
        }

        static std::vector<Game::PlayerId> lastDesiredShields;
        static std::chrono::steady_clock::time_point lastShieldSendAt{};

        const auto now = std::chrono::steady_clock::now();
        const bool sameSet = (desiredShields == lastDesiredShields);
        const bool intervalElapsed =
            (now - lastShieldSendAt) >= std::chrono::milliseconds(protectionIntervalMs);
        if (sameSet && !intervalElapsed) return;

        bool shieldSetChanged = false;
        for (auto& [playerId, tracked] : xpTrackedPlayers) {
            const bool shouldShield = std::find(desiredShields.begin(), desiredShields.end(), playerId) != desiredShields.end();
            if (tracked.is_shielded && !shouldShield) {
                shieldSetChanged = true;
                SetShieldState(playerId, false);
                tracked.is_shielded = false;
            }
        }
        const auto currentProtectionTargets = protectionTargets;
        for (const auto playerId : currentProtectionTargets) {
            if (std::find(desiredShields.begin(), desiredShields.end(), playerId) == desiredShields.end()) {
                SetShieldState(playerId, false);
                auto tracked = xpTrackedPlayers.find(playerId);
                if (tracked != xpTrackedPlayers.end()) tracked->second.is_shielded = false;
            }
        }
        for (const auto playerId : desiredShields) {
            auto& tracked = xpTrackedPlayers[playerId];
            if (!tracked.is_shielded) {
                shieldSetChanged = true;
                SetShieldState(playerId, true);
                tracked.is_shielded = true;
            }
        }
                lastDesiredShields = desiredShields;
        lastShieldSendAt = now;

        if (shieldSetChanged) {
            std::string targets;
            for (const auto playerId : desiredShields) {
                if (!targets.empty()) targets += ", ";
                targets += std::format("{}", playerId);
            }
            LogXpControllerEvent(std::format(
                "Shield roster changed: connectedNonHost={}, shieldCap={}, activeDefenders={}, shieldedIds=[{}]. {}",
                GetConnectedNonHostCount(), teamCap, eligibleDefenders.size(), targets,
                BuildXpControllerDiagnostics()));
        }
    }

    static void BeginXpFarmingRound() {
        if (!IsInLobby() || !IsHost()) return;
        if (!ConfigureMatchCycleTeams(true)) return;
        if (xpFarmingCycleAnnouncementPending) {
            std::vector<std::string> firstImpostorNames;
            for (const auto playerId : xpRoundImpostorIds) {
                const auto tracked = xpTrackedPlayers.find(playerId);
                if (tracked != xpTrackedPlayers.end()) {
                    firstImpostorNames.push_back(XpControllerChatName(tracked->second.name));
                }
            }
            QueueXpControllerChat(
                BuildXpFarmingCycleAnnouncement(firstImpostorNames),
                xpFarmingAnnouncementDelay);
            xpFarmingCycleAnnouncementPending = false;
        }
        xpControllerPhase = XpControllerPhase::FARMING_ROUND;
        xpControllerStage = XpControllerStage::PreparingRound;
        xpControllerDeadline = std::chrono::steady_clock::now() + xpRoundPrepareDelay;
    }

    static void BeginXpRolloverRound() {
        if (!IsInLobby() || !IsHost()) return;
        if (!ConfigureMatchCycleTeams(false)) return;
        xpControllerPhase = XpControllerPhase::ROLLOVER_LOOP;
        xpControllerStage = XpControllerStage::PreparingRound;
        xpControllerDeadline = std::chrono::steady_clock::now();
    }

    static void StartPreparedXpMatch() {
        if (!IsInLobby() || !IsHost() || Game::pAmongUsClient == nullptr || *Game::pAmongUsClient == nullptr) {
            StopXpController("Could not start the next match; host or lobby was lost.");
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!xpPendingChatMessages.empty() || now < xpPostChatDeadline) return;

        if (g_rpcEmittedThisFrame >= kRpcBudgetPerFrame) return;

        LogXpControllerEvent(std::format(
            "Sending match start. {}",
            BuildXpControllerDiagnostics()));
        InnerNetClient_SendStartGame((InnerNetClient*)(*Game::pAmongUsClient), NULL);
        ++g_rpcEmittedThisFrame;
        xpControllerStage = XpControllerStage::AwaitingMatch;
        xpControllerDeadline = std::chrono::steady_clock::now() + xpStageTimeout;
    }

    static void ProcessPendingXpControllerRoles() {
        if (!IsXpControllerRunning() || !IsHost()
            || !xpRoleAssignmentStarted || xpRoleAssignmentComplete) return;

        while (xpRoleAssignmentCursor < xpRolesPendingAssignment.size()
            && g_rpcEmittedThisFrame < kRpcBudgetPerFrame) {
            const auto playerId = xpRolesPendingAssignment[xpRoleAssignmentCursor++];
            auto player = GetPlayerControlById(playerId);
            if (player == nullptr) continue;

            const bool isHost = IsMatchCycleHost(player);
            auto tracked = xpTrackedPlayers.find(playerId);
            const bool isImpostor = !isHost
                && tracked != xpTrackedPlayers.end()
                && tracked->second.was_impostor_this_round;
            PlayerControl_RpcSetRole(
                player,
                isImpostor ? RoleTypes__Enum::Impostor : RoleTypes__Enum::Crewmate,
                false,
                NULL);
            ++g_rpcEmittedThisFrame;
            ++xpRoleAssignedPlayers;
            if (isImpostor) ++xpRoleAssignedImpostors;
            xpRoleHostAssigned = xpRoleHostAssigned || isHost;
            if (tracked != xpTrackedPlayers.end()) {
                tracked->second.is_host = isHost;
                tracked->second.is_impostor = isImpostor;
            }
        }

        if (xpRoleAssignmentCursor == xpRolesPendingAssignment.size()) {
            xpRoleAssignmentComplete = true;
            LogXpControllerEvent(std::format(
                "Applied Match Cycle roles: assignedPlayers={}, impostors={}, hostExplicitlyCrewmate={}. {}",
                xpRoleAssignedPlayers, xpRoleAssignedImpostors,
                xpRoleHostAssigned, BuildXpControllerDiagnostics()));
        }
    }

    bool AssignXpControllerRoles() {
        if (!IsXpControllerRunning() || !IsHost()) return false;

        if (!xpRoleAssignmentStarted && !xpRoleAssignmentComplete) {
            SynchronizeXpPlayers();
            for (auto player : GetConnectedXpPlayers()) {
                xpRolesPendingAssignment.push_back(player->fields.PlayerId);
            }
            if (Game::pLocalPlayer != nullptr && *Game::pLocalPlayer != nullptr) {
                const auto hostId = (*Game::pLocalPlayer)->fields.PlayerId;
                if (std::find(xpRolesPendingAssignment.begin(), xpRolesPendingAssignment.end(), hostId)
                    == xpRolesPendingAssignment.end()) {
                    xpRolesPendingAssignment.push_back(hostId);
                }
            }
            xpRoleAssignmentStarted = true;
        }

        ProcessPendingXpControllerRoles();
        return true;
    }

    static void ForceEndXpMatch() {
        GameManager* gameManager = GameManager__TypeInfo->static_fields->_Instance_k__BackingField;
        if (gameManager == nullptr) {
            StopXpController("Could not end the match because the game manager is unavailable.");
            return;
        }

        // End-game is a state transition RPC; defer by one tick if the frame
        // budget is exhausted rather than racing it against other controller
        // emissions on the same frame.
        if (g_rpcEmittedThisFrame >= kRpcBudgetPerFrame) return;

        LogXpControllerEvent(std::format(
            "Force-ending match after timer. {}",
            BuildXpControllerDiagnostics()));
        GameManager_RpcEndGame(gameManager, GameOverReason__Enum(0), false, NULL);
        ++g_rpcEmittedThisFrame;
        xpControllerStage = XpControllerStage::AwaitingLobby;
        xpControllerDeadline = std::chrono::steady_clock::now() + xpStageTimeout;
        xpTransitionGraceDeadline = std::chrono::steady_clock::now() + transitionGraceDuration;
    }

    static void HandleXpRoundReturnedToLobby() {
        if (xpControllerPhase == XpControllerPhase::ROLLOVER_LOOP) {
            ++xpDrainRoundsCompleted;
            LogXpControllerEvent(std::format(
                "Completed drain round {}/{} ({} configured rounds per farming minute).",
                xpDrainRoundsCompleted, xpDrainRoundsTarget, xpDrainRoundsPerMinute));
            NotifyXpController(
                "Drain round complete",
                std::format("Round {}/{}.", xpDrainRoundsCompleted, xpDrainRoundsTarget),
                ImVec4(1.f, 0.7f, 0.2f, 1.f));
            if (xpDrainRoundsCompleted >= xpDrainRoundsTarget) {
                RequestRejoinForDrainCompletion();
                return;
            }
            LogXpControllerEvent(std::format(
                "Waiting {}s before drain round {}/{}.",
                xpDrainBetweenRoundRejoinSeconds,
                xpDrainRoundsCompleted + 1,
                xpDrainRoundsTarget));
        }
        else {
            LogXpControllerEvent("Lobby detected after farming match; starting 20s lobby delay.");
        }
        xpControllerStage = XpControllerStage::LobbyDelay;
        xpControllerDeadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(xpControllerPhase == XpControllerPhase::ROLLOVER_LOOP
                ? xpDrainBetweenRoundRejoinSeconds : 20);
    }

    static void StartNextXpCycle() {
        for (auto& [playerId, tracked] : xpTrackedPlayers) {
            if (tracked.is_host) continue;
            tracked.has_earned_xp = false;
            tracked.has_completed_objective = false;
            tracked.is_impostor = false;
            tracked.has_farming_attempt = false;
            tracked.needs_rejoin = false;
            tracked.disconnected_after_rejoin_request = false;
        }
        xpRoundImpostorIds.clear();
        xpDrainRoundsCompleted = 0;
        xpDrainRoundsTarget = 0;
        LogXpControllerEvent(std::format(
            "Fixed drain-round sequence completed; starting the next full farming cycle. {}",
            BuildXpControllerDiagnostics()));
        NotifyXpController(
            "Farming cycle restarting",
            std::format("Drain complete. Farming rounds: {}s.", xpFarmingDurationSeconds),
            ImVec4(0.f, 1.f, 0.5f, 1.f));
        xpControllerPhase = XpControllerPhase::FARMING_ROUND;
        xpFarmingCycleAnnouncementPending = true;
        BeginXpFarmingRound();
    }

    static void ProcessXpLobbyDelay() {
        if (GetConnectedNonHostCount() == 0) {
            LogXpControllerEvent(std::format(
                "{}s lobby delay expired with zero non-host players; pausing before XP evaluation. inLobby={}, stage={}.",
                xpControllerPhase == XpControllerPhase::ROLLOVER_LOOP
                    ? xpDrainBetweenRoundRejoinSeconds : 20,
                IsInLobby(), XpControllerStageName(xpControllerStage)));
            PauseForNonHostParticipants(std::chrono::steady_clock::now());
            return;
        }

        SynchronizeXpPlayers();

        if (xpControllerPhase == XpControllerPhase::FARMING_ROUND) {
            const auto priorCompleted = std::count_if(xpTrackedPlayers.begin(), xpTrackedPlayers.end(), [](const auto& entry) {
                return !entry.second.is_host && entry.second.has_completed_objective;
            });
            EvaluateCompletedFarmingRound();
            const auto completed = std::count_if(xpTrackedPlayers.begin(), xpTrackedPlayers.end(), [](const auto& entry) {
                return !entry.second.is_host && entry.second.has_completed_objective;
            });
            LogXpControllerEvent(std::format(
                "Farming round completion: participants={}, completedBefore={}, completedAfter={}; every non-host ID in the immutable Impostor assignment is credited after a started round returns to the lobby.",
                GetConnectedNonHostCount(), priorCompleted, completed));
            xpControllerPhase = XpControllerPhase::LOBBY_EVAL;
            xpControllerStage = XpControllerStage::EvaluateLobby;
            return;
        }

        LogXpControllerEvent(std::format(
            "Drain round target not reached; starting round {}/{}.",
            xpDrainRoundsCompleted + 1, xpDrainRoundsTarget));
        BeginXpRolloverRound();
    }

    int GetXpFarmingDurationSeconds() {
        return xpFarmingDurationSeconds;
    }

    void SetXpFarmingDurationSeconds(int seconds) {
        xpFarmingDurationSeconds = (std::clamp)(seconds, 1, 3600);
    }

    int GetXpDrainRoundsPerMinute() {
        return xpDrainRoundsPerMinute;
    }

    void SetXpDrainRoundsPerMinute(int rounds) {
        xpDrainRoundsPerMinute = (std::clamp)(rounds, 1, 60);
    }

    int GetXpDrainBetweenRoundRejoinSeconds() {
        return xpDrainBetweenRoundRejoinSeconds;
    }

    void SetXpDrainBetweenRoundRejoinSeconds(int seconds) {
        xpDrainBetweenRoundRejoinSeconds = (std::clamp)(seconds, 1, 3600);
    }

    int GetXpFinalRejoinWindowSeconds() {
        return xpFinalRejoinWindowSeconds;
    }

    void SetXpFinalRejoinWindowSeconds(int seconds) {
        xpFinalRejoinWindowSeconds = (std::clamp)(seconds, 1, 3600);
    }

    void StartXpController() {
        if (IsXpControllerRunning()) return;
        if (!IsInLobby() || !IsHost()) {
            Toasts::AddToast("XP Controller", "Start this controller from a host lobby.", ImVec4(1.f, 0.3f, 0.3f, 1.f));
            return;
        }
        if (DrainXp::GetRunningStatus() != 0 || DrainXpV2::GetRunningStatus() != 0) {
            Toasts::AddToast("XP Controller", "Stop the other XP loop before starting this controller.", ImVec4(1.f, 0.3f, 0.3f, 1.f));
            return;
        }
        std::vector<std::string> conflictingSettings;
        if (State.AutoFakeRole) conflictingSettings.push_back("Auto Fake Role");
        if (State.BattleRoyale) conflictingSettings.push_back("Battle Royale");
        if (State.TaskSpeedrun) conflictingSettings.push_back("Task Speedrun");
        if (State.EndGameV2) conflictingSettings.push_back("End Game V2");
        if (State.NoGameEnd) conflictingSettings.push_back("No Game End");
        if (State.GameMode != 0) conflictingSettings.push_back("a non-standard game mode");
        if (!conflictingSettings.empty()) {
            std::string settingsList;
            for (const auto& setting : conflictingSettings) {
                if (!settingsList.empty()) settingsList += ", ";
                settingsList += setting;
            }
            Toasts::AddToast(
                "XP Controller",
                std::format("Disable {} before starting Match Cycle.", settingsList),
                ImVec4(1.f, 0.3f, 0.3f, 1.f));
            LogXpControllerEvent(std::format("Start blocked by conflicting settings: {}.", settingsList));
            return;
        }

        xpTrackedPlayers.clear();
        xpRoundImpostorIds.clear();
        xpPendingChatMessages.clear();
        xpRoundChatWait = std::chrono::milliseconds::zero();
        xpRoundChatMessageCount = 0;
        xpRoundChatPostDelaySeconds = 0;
        xpNextChatSendAt = std::chrono::steady_clock::now();
        xpPostChatDeadline = std::chrono::steady_clock::now();
        xpDrainRoundsCompleted = 0;
        xpDrainRoundsTarget = 0;
        xpHasFarmingRotationCursor = false;
        xpFarmingCycleAnnouncementPending = true;
        State.ForceEndAfterStartTriggered = false;
        for (auto player : GetConnectedXpPlayers()) {
            auto& tracked = xpTrackedPlayers[player->fields.PlayerId];
            tracked.name = XpPlayerName(player);
            tracked.is_host = IsMatchCycleHost(player);
            tracked.connected = true;
        }
        if (xpTrackedPlayers.empty()) {
            Toasts::AddToast("XP Controller", "No connected players are available.", ImVec4(1.f, 0.3f, 0.3f, 1.f));
            return;
        }
        if (GetConnectedNonHostCount() < 2) {
            xpTrackedPlayers.clear();
            Toasts::AddToast("XP Controller", "At least two connected non-host participants are required.", ImVec4(1.f, 0.3f, 0.3f, 1.f));
            return;
        }

        xpControllerPhase = XpControllerPhase::FARMING_ROUND;
        xpLastRosterCheck = std::chrono::steady_clock::now();
        xpTransitionGraceDeadline = xpLastRosterCheck;
        xpHostAuthorityLostAt = {};
        LogXpControllerEvent(std::format("Match Cycle started. {}", BuildXpControllerDiagnostics()));
        BeginXpFarmingRound();
        Toasts::AddToast("XP Controller", "Starting the farming rounds.", ImVec4(0.f, 1.f, 0.5f, 1.f));
    }

    void StopXpController() {
        StopXpController("Stopped.");
    }

    std::string GetXpControllerStatus() {
        if (!IsXpControllerRunning()) return "Stopped";
        if (xpHostAuthorityLostAt != std::chrono::steady_clock::time_point{}) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - xpHostAuthorityLostAt).count();
            const auto graceRemaining = (std::max)(0LL,
                std::chrono::duration_cast<std::chrono::seconds>(xpHostAuthorityGraceDuration).count() - elapsed);
            return std::format("Waiting for host authority - {}s grace", graceRemaining);
        }
        const auto remaining = (std::max)(0LL,
            std::chrono::duration_cast<std::chrono::seconds>(
                xpControllerDeadline - std::chrono::steady_clock::now()).count());
        const char* phaseName = "XP controller";
        switch (xpControllerPhase) {
        case XpControllerPhase::FARMING_ROUND: phaseName = "Farming round"; break;
        case XpControllerPhase::LOBBY_EVAL: phaseName = "Reporting completed farming turns"; break;
        case XpControllerPhase::ROLLOVER_LOOP: phaseName = "Fixed drain sequence"; break;
        case XpControllerPhase::STOPPED: break;
        }
        if (xpControllerPhase == XpControllerPhase::ROLLOVER_LOOP) {
            if (xpControllerStage == XpControllerStage::AwaitingRejoin) {
                return std::format("Drain rounds complete - rejoin window - {}s", remaining);
            }
            return std::format(
                "{} - round {}/{} - {}s",
                phaseName, xpDrainRoundsCompleted + 1, xpDrainRoundsTarget, remaining);
        }
        return std::format("{} - {}s", phaseName, remaining);
    }

    void TickXpController() {
        g_rpcEmittedThisFrame = 0;
        if (!IsXpControllerRunning()) return;
        if (State.PanicMode) {
            StopXpController("Stopped because panic mode was enabled.");
            return;
        }
        ProcessPendingXpControllerRoles();
        const auto now = std::chrono::steady_clock::now();
        if (!IsHost()) {
            if (xpHostAuthorityLostAt == std::chrono::steady_clock::time_point{}) {
                xpHostAuthorityLostAt = now;
                LogXpControllerEvent(std::format(
                    "Host authority temporarily unavailable; pausing cycle for up to {}s. {}",
                    std::chrono::duration_cast<std::chrono::seconds>(xpHostAuthorityGraceDuration).count(),
                    BuildXpControllerDiagnostics()));
            }
            if (now - xpHostAuthorityLostAt >= xpHostAuthorityGraceDuration) {
                StopXpController("Stopped because host authority was not restored within the 10-second grace period.");
            }
            return;
        }
        if (xpHostAuthorityLostAt != std::chrono::steady_clock::time_point{}) {
            const auto authorityPause = now - xpHostAuthorityLostAt;
            xpControllerDeadline += authorityPause;
            LogXpControllerEvent(std::format(
                "Host authority restored; resuming cycle and extending timer by {}ms. {}",
                std::chrono::duration_cast<std::chrono::milliseconds>(authorityPause).count(),
                BuildXpControllerDiagnostics()));
            xpHostAuthorityLostAt = {};
        }
        TickXpControllerChat();
        const bool inLobby = IsInLobby();
        const bool transitioningToLobby = inLobby
            && (xpControllerStage == XpControllerStage::MatchTimer
                || xpControllerStage == XpControllerStage::AwaitingLobby);
        const bool lobbyDelayPending = inLobby
            && xpControllerStage == XpControllerStage::LobbyDelay
            && now < xpControllerDeadline;

        if (xpControllerStage == XpControllerStage::WaitingForParticipants) {
            if (!inLobby) return;
            if (now < xpTransitionGraceDeadline || GetConnectedNonHostCount() == 0) return;
            ResumeAfterNonHostParticipantReturns(now);
        }
        else if (inLobby && !transitioningToLobby && !lobbyDelayPending
            && xpControllerStage != XpControllerStage::AwaitingRejoin
            && now >= xpTransitionGraceDeadline && GetConnectedNonHostCount() == 0) {
            PauseForNonHostParticipants(now);
            return;
        }

        if (!transitioningToLobby && !lobbyDelayPending) {
            SynchronizeXpPlayers();
        }
        if (now - xpLastRosterCheck >= xpRosterCheckInterval) {
            if (!transitioningToLobby && !lobbyDelayPending) {
                xpLastRosterCheck = now;
                ReconcileMatchCycleShields();
            }
        }
        switch (xpControllerStage) {
        case XpControllerStage::PreparingRound:
            if (IsInLobby() && now >= xpControllerDeadline) StartPreparedXpMatch();
            break;
        case XpControllerStage::AwaitingMatch:
            if (now >= xpControllerDeadline) {
                StopXpController("The match did not start before the timeout.");
            }
            break;
        case XpControllerStage::MatchTimer:
            if (IsInLobby()) {
                HandleXpRoundReturnedToLobby();
            }
            else if (now >= xpControllerDeadline) {
                ForceEndXpMatch();
            }
            break;
        case XpControllerStage::AwaitingLobby:
            if (IsInLobby()) {
                HandleXpRoundReturnedToLobby();
            }
            else if (now >= xpControllerDeadline) {
                StopXpController("The lobby did not return before the timeout.");
            }
            break;
        case XpControllerStage::AwaitingRejoin:
            if (AllRequestedPlayersRejoined() || now >= xpControllerDeadline) {
                LogXpControllerEvent(std::format(
                    "Rejoin wait ended: allRequestedRejoined={}, timedOut={}. {}",
                    AllRequestedPlayersRejoined(), now >= xpControllerDeadline,
                    BuildXpControllerDiagnostics()));
                StartNextXpCycle();
            }
            break;
        case XpControllerStage::LobbyDelay:
            if (now >= xpControllerDeadline) {
                ProcessXpLobbyDelay();
            }
            break;
        case XpControllerStage::WaitingForParticipants:
            break;
        case XpControllerStage::EvaluateLobby:
            LogXpControllerEvent(std::format(
                "Evaluating farming results: allNonHostObjectivesComplete={}. {}",
                AllConnectedPlayersEarnedXp(), BuildXpControllerDiagnostics()));
            if (AllConnectedPlayersEarnedXp()) {
                xpDrainRoundsCompleted = 0;
                xpDrainRoundsTarget = GetXpDrainRoundsTarget();
                xpControllerPhase = XpControllerPhase::ROLLOVER_LOOP;
                QueueXpControllerChat(std::format(
                    "Auto-drain starts in 30s: {} rounds, {}s between; each ends after 35s.",
                    xpDrainRoundsTarget, xpDrainBetweenRoundRejoinSeconds),
                    std::chrono::seconds::zero());
                QueueXpControllerChat(
                    "After draining, rejoin. If you don't, the next loop starts without you.",
                    xpDrainAnnouncementDelay);
                LogXpControllerEvent(std::format(
                    "All connected non-host participants completed farming; beginning fixed drain sequence of {} rounds ({} per farming minute, farmingDuration={}s). {}",
                    xpDrainRoundsTarget, xpDrainRoundsPerMinute, xpFarmingDurationSeconds,
                    BuildXpControllerDiagnostics()));
                NotifyXpController(
                    "Auto-drain starting",
                    std::format("{} rounds; protection off.", xpDrainRoundsTarget),
                    ImVec4(1.f, 0.7f, 0.2f, 1.f));
                BeginXpRolloverRound();
            }
            else {
                BroadcastXpControllerSummary();
                BeginXpFarmingRound();
            }
            break;
        }
    }

    void OnXpControllerGameStarted() {
        if (!IsXpControllerRunning() || !IsHost()
            || xpControllerStage != XpControllerStage::AwaitingMatch) return;

        const int matchDurationSeconds = xpControllerPhase == XpControllerPhase::FARMING_ROUND
            ? xpFarmingDurationSeconds : static_cast<int>(xpRolloverDuration.count());
        const auto messageWait = xpRoundChatWait;
        const auto roundDuration = std::chrono::seconds(matchDurationSeconds) + messageWait;
        xpRoundChatWait = std::chrono::milliseconds::zero();
        xpRoundChatMessageCount = 0;
        xpRoundChatPostDelaySeconds = 0;
        xpControllerStage = XpControllerStage::MatchTimer;
        xpControllerDeadline = std::chrono::steady_clock::now()
            + roundDuration;
        LogXpControllerEvent(std::format(
            "Game loading completed; starting {}ms round timer ({}s base + {}ms chat wait). {}",
            roundDuration.count(), matchDurationSeconds, messageWait.count(),
            BuildXpControllerDiagnostics()));
    }

    enum Groups {
        Player,
        Trolling,
        Info,
    };

    static bool openPlayer = true;
    static bool openTrolling = false;
    static bool openInfo = false;

    static bool selectingShiftingToPlayer = false;
    static bool selectingTurningToPlayer = false;
    static std::vector<Game::PlayerId> playersToShift = {};
    static std::string playerToShiftName = "";

    void CloseOtherGroups(Groups group) {
        openPlayer = group == Groups::Player;
        openTrolling = group == Groups::Trolling;
        openInfo = group == Groups::Info;
    }

    void OpenSubGroup(const std::string& name) {
        if (name == "Player") CloseOtherGroups(Groups::Player);
        else if (name == "Trolling") CloseOtherGroups(Groups::Trolling);
        else if (name == "Info" && (IsInMultiplayerGame() || IsInLobby())) CloseOtherGroups(Groups::Info);
    }

    bool ShouldUsePlayerPicker() {
        return selectingShiftingToPlayer || selectingTurningToPlayer;
    }

    std::string GetPlayerPickerDetails() {
        if (selectingShiftingToPlayer) {
            return playerToShiftName.empty() ?
                std::format("Select a player to shift {} players to...", playersToShift.size()) :
                "Select a player to shift " + playerToShiftName + " to...";
        }

        if (selectingTurningToPlayer) {
            return playerToShiftName.empty() ?
                std::format("Select a player to turn {} players into...", playersToShift.size()) :
                "Select a player to turn " + playerToShiftName + " into...";
        }

        return "";
    }

    struct CachedPlayerData {
        PlayerControl* controlPtr = nullptr;
        std::string nameRaw;
        std::string nameClean;
        std::string friendCode;
        std::string puid;
        std::string platform;
        std::string platformName;
        uint64_t psnId = 0;
        uint64_t xboxId = 0;

        std::string finalDisplayName;
        ImVec4 finalColor;
        std::string selectableId;
        std::string colorButtonId;
        int lastUpdateFrame = 0;
        bool isCached = false;
    };

    static std::unordered_map<uint8_t, CachedPlayerData> g_PlayerCache;
    static int g_FrameCounter = 0;

    const int CACHE_UPDATE_FREQ_NORMAL = 60;
    const int CACHE_UPDATE_FREQ_MEETING = 180;

    bool showHiddenInfo = false;

    void ClearCache() {
        g_PlayerCache.clear();
    }

    std::string GetPlatformString(PlayerControl* playerCtrl, app::ClientData* client, uint64_t& outPsn, uint64_t& outXbox, std::string& platformName) {
        if (client == NULL || client->fields.PlatformData == NULL || playerCtrl->fields._.OwnerId != client->fields.Id) {
            return "Unknown";
        }

        outPsn = client->fields.PlatformData->fields.PsnPlatformId;
        outXbox = client->fields.PlatformData->fields.XboxPlatformId;

        // By default, all players have "TESTNAME", except Switch/PlayStation/XBOX (using account names)
        platformName = convert_from_string(client->fields.PlatformData->fields.PlatformName);

        switch (client->fields.PlatformData->fields.Platform) {
        case Platforms__Enum::StandaloneEpicPC:
            return "Epic Games (PC)";
        case Platforms__Enum::StandaloneSteamPC:
            return "Steam (PC)";
        case Platforms__Enum::StandaloneMac:
            return "Mac";
        case Platforms__Enum::StandaloneWin10:
            return "Microsoft Store (PC)";
        case Platforms__Enum::StandaloneItch:
            return "itch.io (PC)";
        case Platforms__Enum::IPhone:
            return "iOS/iPadOS (Mobile)";
        case Platforms__Enum::Android:
            return "Android (Mobile)";
        case Platforms__Enum::Switch:
            return "Nintendo Switch (Console)";
        case Platforms__Enum::Xbox:
            return "Xbox (Console)";
        case Platforms__Enum::Playstation:
            return "Playstation (Console)";
        default:
            return "Unknown";
        }
    }

    void StopProtectionLoop() {
        protectionToggleUiEnabled = false;
        protectionCrewmatesOnly = false;
        protectionTargetIndex = 0;
    }

    void ReapplyProtectionNow() {
        if (!protectionToggleUiEnabled) return;
        lastProtectionTime = std::chrono::steady_clock::now() -
            std::chrono::milliseconds((std::max)(protectionIntervalMs, 1));
    }

    static void ToggleProtection(bool crewmatesOnly) {
        if (protectionToggleUiEnabled) {
            if (protectionCrewmatesOnly == crewmatesOnly) {
                StopProtectionLoop();
            }
            else {
                protectionCrewmatesOnly = crewmatesOnly;
                protectionTargetIndex = 0;
                lastProtectionTime = std::chrono::steady_clock::now() -
                    std::chrono::milliseconds((std::max)(protectionIntervalMs, 1));
            }
            return;
        }

        if (protectionTargets.empty()) {
            for (auto playerId : State.selectedPlayers) protectionTargets.push_back(playerId);
        }
        if (!protectionTargets.empty()) {
            protectionTargetIndex = 0;
            protectionCrewmatesOnly = crewmatesOnly;
            protectionToggleUiEnabled = true;
            lastProtectionTime = std::chrono::steady_clock::now() -
                std::chrono::milliseconds((std::max)(protectionIntervalMs, 1));
        }
    }

    static void SetShieldState(Game::PlayerId playerId, bool enabled) {
        if (enabled) {
            if (!matchCycleProtectionOwnsSettings) {
                matchCyclePreviousProtectionEnabled = protectionToggleUiEnabled;
                matchCyclePreviousCrewmatesOnly = protectionCrewmatesOnly;
                matchCyclePreviousProtectionIntervalMs = protectionIntervalMs;
                matchCyclePreviousProtectionTargets = protectionTargets;
                matchCycleProtectionOwnsSettings = true;
                protectionTargets.clear();
            }

            if (std::find(protectionTargets.begin(), protectionTargets.end(), playerId) == protectionTargets.end()) {
                protectionTargets.push_back(playerId);
            }
            protectionIntervalMs = matchCycleProtectionIntervalMs;
            if (!protectionToggleUiEnabled) {
                ToggleProtection(true);
            }
            else {
                protectionCrewmatesOnly = true;
                ReapplyProtectionNow();
            }
            return;
        }

        if (!matchCycleProtectionOwnsSettings) return;
        protectionTargets.erase(
            std::remove(protectionTargets.begin(), protectionTargets.end(), playerId),
            protectionTargets.end());
        if (protectionTargets.empty()) {
            StopProtectionLoop();
        }
    }

    static void RestoreMatchCycleProtectionSettings() {
        if (!matchCycleProtectionOwnsSettings) return;
        protectionTargets = std::move(matchCyclePreviousProtectionTargets);
        protectionIntervalMs = matchCyclePreviousProtectionIntervalMs;
        protectionCrewmatesOnly = matchCyclePreviousCrewmatesOnly;
        protectionToggleUiEnabled = matchCyclePreviousProtectionEnabled;
        protectionTargetIndex = 0;
        matchCycleProtectionOwnsSettings = false;
        if (protectionToggleUiEnabled) ReapplyProtectionNow();
        else StopProtectionLoop();
    }
    
    void ClearProtectionTargets() {
        StopProtectionLoop();
        protectionTargets.clear();
    }

    bool IsProtectionActiveFor(PlayerControl* pc) {
        if (!protectionToggleUiEnabled || pc == nullptr) return false;
        if (std::find(protectionTargets.begin(), protectionTargets.end(), pc->fields.PlayerId) == protectionTargets.end()) return false;
        if (protectionCrewmatesOnly) {
            auto d = GetPlayerData(pc);
            if (d == nullptr || d->fields.Role == nullptr || PlayerIsImpostor(d)) return false;
        }
        return true;
    }

    void TickProtectionLoop() {
        if (!protectionToggleUiEnabled) return;
        if (protectionTargets.empty()) {
            StopProtectionLoop();
            return;
        }
        if (!(IsInGame() || IsInLobby())) {
            if (matchCycleProtectionOwnsSettings && IsXpControllerRunning()) return;
            StopProtectionLoop();
            return;
        }
        if (matchCycleProtectionOwnsSettings && IsXpControllerRunning()
            && (!IsHost() || !IsInGame())) return;
        if (Game::pLocalPlayer == nullptr || *Game::pLocalPlayer == nullptr) return;

        auto now = std::chrono::steady_clock::now();
        long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastProtectionTime).count();
        const int intervalMs = (std::max)(protectionIntervalMs, 1);
        if (elapsed < intervalMs) return;
        lastProtectionTime = now;

        bool canForce = (IsHost() && IsInGame()) || !State.SafeMode;
        if (!canForce) {
            if (matchCycleProtectionOwnsSettings && IsXpControllerRunning()) {
                return;
            }
            StopProtectionLoop();
            return;
        }

        // Protect all targets on each interval, within the frame budget.
        // If the budget is exhausted mid-list, the remaining targets are picked
        // up on the next interval (protectionTargets is preserved; the loop is
        // not destructive).
        for (auto id : protectionTargets) {
            if (g_rpcEmittedThisFrame >= kRpcBudgetPerFrame) break;
            auto pc = GetPlayerControlById(id);
            if (pc == nullptr) continue;
            auto targetData = GetPlayerData(pc);
            if (targetData == nullptr || targetData->fields.Disconnected) continue;
            if (!IsProtectionActiveFor(pc)) continue;
            auto outfit = GetPlayerOutfit(targetData);
            if (outfit == nullptr) continue;
            PlayerControl_RpcProtectPlayer(*Game::pLocalPlayer, pc, outfit->fields.ColorId, NULL);
            ++g_rpcEmittedThisFrame;
        }
    }

    void StopKillLoop() {
        killToggleUiEnabled = false;
        lastKillTime = std::chrono::steady_clock::now();
    }

    void TickKillLoop() {
        if (!killToggleUiEnabled) return;
        if (!IsInGame()) {
            StopKillLoop();
            return;
        }
        if (Game::pLocalPlayer == nullptr || *Game::pLocalPlayer == nullptr) {
            StopKillLoop();
            return;
        }
        auto localData = GetPlayerData(*Game::pLocalPlayer);
        if (localData == nullptr || localData->fields.IsDead) {
            StopKillLoop();
            return;
        }
        // Only attempt the kill while our own shield is actually up: that is the
        // only situation where "we survive and the shield is broken" is expected.
        // If the shield is down we wait for the protection loop to re-apply it
        // rather than send an unprotected self-kill.
        if ((*Game::pLocalPlayer)->fields.protectedByGuardianId < 0) return;

        auto now = std::chrono::steady_clock::now();
        long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastKillTime).count();
        const int intervalMs = killIntervalMs;
        if (elapsed < intervalMs) return;
        if (g_rpcEmittedThisFrame >= kRpcBudgetPerFrame) return;
        lastKillTime = now;

        // Ordinary kill attempt on ourselves. This is the direct vanilla kill
        // RPC (the call the game's host emits after validation) - NOT the mod's
        // RpcMurderPlayer RPC class, whose Process() force-kills through a
        // shield with a batched message. Whether the game then rejects the
        // attempt, breaks our shield (FailedProtected), or - worst case - lets
        // it through, is game-version dependent: the shield-up guard above keeps
        // an unprotected self-kill from ever being sent, and the loop stops by
        // itself if we die. Verify the actual outcome in a private lobby.
        PlayerControl_RpcMurderPlayer(*Game::pLocalPlayer, *Game::pLocalPlayer, true, NULL);
        ++g_rpcEmittedThisFrame;
    }

    void RenderProtectionControls() {
        auto allPlayers = GetAllPlayerControl();
        protectionTargets.erase(
            std::remove_if(protectionTargets.begin(), protectionTargets.end(), [&](Game::PlayerId playerId) {
                auto player = GetPlayerControlById(playerId);
                auto playerData = player != nullptr ? GetPlayerData(player) : nullptr;
                return playerData == nullptr || playerData->fields.Disconnected;
            }),
            protectionTargets.end());

        std::string targetPreview = std::format("{} selected", protectionTargets.size());
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("Protection targets", targetPreview.c_str())) {
            if (ImGui::SmallButton("Select all")) {
                protectionTargets.clear();
                for (auto player : allPlayers) {
                    if (player == nullptr) continue;
                    auto playerData = GetPlayerData(player);
                    if (playerData == nullptr || playerData->fields.Disconnected) continue;
                    protectionTargets.push_back(playerData->fields.PlayerId);
                }
                protectionTargetIndex = 0;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) {
                protectionTargets.clear();
                protectionTargetIndex = 0;
            }

            for (auto player : allPlayers) {
                if (player == nullptr) continue;
                auto playerData = GetPlayerData(player);
                if (playerData == nullptr || playerData->fields.Disconnected) continue;

                const auto playerId = playerData->fields.PlayerId;
                const bool selected = std::find(protectionTargets.begin(), protectionTargets.end(), playerId) != protectionTargets.end();
                std::string playerName = convert_from_string(NetworkedPlayerInfo_get_PlayerName(playerData, nullptr));
                ImGui::PushID(static_cast<int>(playerId));
                if (ImGui::Selectable(playerName.c_str(), selected, ImGuiSelectableFlags_DontClosePopups)) {
                    if (selected) {
                        protectionTargets.erase(std::remove(protectionTargets.begin(), protectionTargets.end(), playerId), protectionTargets.end());
                    }
                    else {
                        protectionTargets.push_back(playerId);
                    }
                    protectionTargetIndex = 0;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (AnimatedButton("Toggle Protection")) ToggleProtection(false);
        if (AnimatedButton("Toggle Crew Protection")) ToggleProtection(true);
        ImGui::SetNextItemWidth(120 * State.dpiScale);
        ImGui::InputInt("##protectionIntervalMs", &protectionIntervalMs, 1, 10);
        ImGui::SameLine();
        ImGui::Text("ms/target%s", protectionToggleUiEnabled
            ? (protectionCrewmatesOnly ? " (Crew Only On)" : " (On)")
            : " (Off)");
        if (protectionIntervalMs < 1) protectionIntervalMs = 1;
    }

    void RenderKillControls() {
        if (AnimatedButton("Toggle Kill")) {
            if (killToggleUiEnabled) {
                StopKillLoop();
            }
            else if (IsInGame() && Game::pLocalPlayer != nullptr && *Game::pLocalPlayer != nullptr) {
                auto localData = GetPlayerData(*Game::pLocalPlayer);
                if (localData != nullptr && !localData->fields.IsDead && (*Game::pLocalPlayer)->fields.protectedByGuardianId >= 0) {
                    killToggleUiEnabled = true;
                    lastKillTime = std::chrono::steady_clock::now() -
                        std::chrono::milliseconds((std::max)(killIntervalMs, 100));
                }
            }
        }
        ImGui::SetNextItemWidth(120 * State.dpiScale);
        ImGui::InputInt("##killIntervalMs", &killIntervalMs, 1, 10);
        ImGui::SameLine();
        ImGui::Text("ms/attempt%s", killToggleUiEnabled ? " (On)" : " (Off)");
       
    }

    void Render() {
        g_FrameCounter++;

        if ((IsInGame() || IsInLobby())) {
            ColorMapping FAKEROLE_NAMES_COLOR[] = {
                {"Crewmate",		State.CrewmateColor},
                {"Impostor",		State.ImpostorColor},
                {"Scientist",		State.ScientistColor},
                {"Engineer",		State.EngineerColor},
                {"Guardian Angel",	State.GuardianAngelColor},
                {"Shapeshifter",	State.ShapeshifterColor},
                {"Crewmate Ghost",  State.CrewmateGhostColor},
                {"Impostor Ghost",	State.ImpostorGhostColor},
                {"Noisemaker",		State.NoisemakerColor},
                {"Phantom",			State.PhantomColor},
                {"Tracker",			State.TrackerColor},
                {"Detective",		State.DetectiveColor},
                {"Viper",			State.ViperColor},
                {"Judge",           State.JudgeColor},
                {"Influencer",      State.InfluencerColor},
            }; // needs to be updated every render

            ColorMapping GHOSTROLE_NAMES_COLOR[] = {
                {"Guardian Angel",	State.GuardianAngelColor},
                {"Crewmate Ghost",  State.CrewmateGhostColor},
                {"Impostor Ghost",	State.ImpostorGhostColor},
                {"Influencer",	    State.InfluencerColor},

            }; // this too

            ImGui::SameLine(100 * State.dpiScale);
            ImGui::BeginChild("players#list", ImVec2(200, 0) * State.dpiScale, true, ImGuiWindowFlags_NoBackground);

            if (!State.selectedPlayer.has_value() || State.selectedPlayer.validate().is_Disconnected()) {
                State.selectedPlayer = {};
            }

            if (!State.PendingRejoinTargetFC.empty() && State.PendingRejoinReady) {
                for (auto pc : GetAllPlayerControl()) {
                    if (pc == nullptr) continue;
                    auto pd = GetPlayerData(pc);
                    if (pd == nullptr || pd->fields.Disconnected) continue;
                    std::string fc = convert_from_string(pd->fields.FriendCode);
                    std::string name = convert_from_string(NetworkedPlayerInfo_get_PlayerName(pd, nullptr));
                    if (fc == State.PendingRejoinTargetFC || name == State.PendingRejoinTargetFC) {
                        State.selectedPlayer = PlayerSelection(pc);
                        State.selectedPlayers = { pd->fields.PlayerId };
                        State.PendingRejoinTargetFC = "";
                        State.PendingRejoinReady = false;
                        break;
                    }
                }
            }

            if ((selectingShiftingToPlayer || selectingTurningToPlayer) && !((IsHost() && IsInGame()) || !State.SafeMode)) {
                selectingShiftingToPlayer = false;
                selectingTurningToPlayer = false;
                playersToShift = {};
                playerToShiftName = "";
            }

            if (playersToShift.size() != 0) {
                std::vector<Game::PlayerId> newPlayersToShift = {};
                std::string pName = "";

                for (auto pId : playersToShift) {
                    auto pc = GetPlayerControlById(pId);
                    if (pc == NULL) continue;

                    auto outfit = GetPlayerOutfit(GetPlayerData(pc));
                    if (outfit != NULL) {
                        pName = convert_from_string(outfit->fields.PlayerName);
                        newPlayersToShift.push_back(pId);
                    }
                }

                if (newPlayersToShift.size() == 1) playerToShiftName = pName;
                playersToShift = newPlayersToShift;
            }

            auto selectedPlayer = State.selectedPlayer.validate();
            bool shouldEndListBox = ImGui::ListBoxHeader("###players#list", ImVec2(200, 230) * State.dpiScale);
            auto localData = GetPlayerData(*Game::pLocalPlayer);

            State.currentPlayers.clear();
            std::vector<uint8_t> activeIds;

            auto allControllers = GetAllPlayerControl();
            for (auto playerCtrl : allControllers) {
                if (playerCtrl == NULL) continue;
                auto playerData = GetPlayerData(playerCtrl);
                if (playerData == NULL || playerData->fields.Disconnected) continue;

                uint8_t pid = playerData->fields.PlayerId;
                State.currentPlayers.insert(pid);
                activeIds.push_back(pid);

                CachedPlayerData& cache = g_PlayerCache[pid];

                if (cache.controlPtr != playerCtrl) {
                    cache = CachedPlayerData();
                    cache.controlPtr = playerCtrl;
                    cache.isCached = false;
                }

                int updateFrequency = State.InMeeting ? CACHE_UPDATE_FREQ_MEETING : CACHE_UPDATE_FREQ_NORMAL;

                bool needsUpdate = !cache.isCached || ((g_FrameCounter - cache.lastUpdateFrame) > updateFrequency);

                if (needsUpdate) {
                    app::NetworkedPlayerInfo_PlayerOutfit* outfit = GetPlayerOutfit(playerData);

                    if (outfit) {
                        cache.nameRaw = convert_from_string(NetworkedPlayerInfo_get_PlayerName(playerData, nullptr));
                        cache.nameClean = RemoveHtmlTags(cache.nameRaw);
                        cache.friendCode = convert_from_string(playerData->fields.FriendCode);
                        cache.puid = convert_from_string(playerData->fields.Puid);

                        app::ClientData* client = (app::ClientData*)app::InnerNetClient_GetClientFromCharacter((app::InnerNetClient*)(*Game::pAmongUsClient), playerCtrl, NULL);
                        cache.platform = GetPlatformString(playerCtrl, client, cache.psnId, cache.xboxId, cache.platformName);

                        if (!cache.isCached) {
                            cache.selectableId = "##" + ToString(pid);
                            cache.isCached = true;
                        }

                        std::string tempName = cache.nameClean;
                        ImVec4 tempColor = State.LightMode ? AmongUsColorToImVec4(Palette__TypeInfo->static_fields->Black) : AmongUsColorToImVec4(Palette__TypeInfo->static_fields->White);

                        if (IsInMultiplayerGame() || IsInLobby()) {
                            bool isBlacklisted = std::find(State.BlacklistFriendCodes.begin(), State.BlacklistFriendCodes.end(), cache.friendCode) != State.BlacklistFriendCodes.end();
                            bool isWhitelisted = std::find(State.WhitelistFriendCodes.begin(), State.WhitelistFriendCodes.end(), cache.friendCode) != State.WhitelistFriendCodes.end();
                            bool isNameLocked = std::find(State.LockedNames.begin(), State.LockedNames.end(), tempName) != State.LockedNames.end();

                            if (isNameLocked && isBlacklisted) {
                                tempName = "[!] + [-] " + tempName;
                                tempColor = AmongUsColorToImVec4(Palette__TypeInfo->static_fields->ImpostorRed);
                            }
                            else if (isNameLocked && isWhitelisted) {
                                tempName = "[!] + [+] " + tempName;
                                tempColor = AmongUsColorToImVec4(Palette__TypeInfo->static_fields->CrewmateBlue);
                            }
                            else if (isBlacklisted) {
                                tempName = "[-] " + tempName;
                                tempColor = AmongUsColorToImVec4(Palette__TypeInfo->static_fields->ImpostorRed);
                            }
                            else if (isWhitelisted) {
                                tempName = "[+] " + tempName;
                                tempColor = AmongUsColorToImVec4(Palette__TypeInfo->static_fields->CrewmateBlue);
                            }
                            else if (isNameLocked) {
                                tempName = "[!] " + tempName;
                                tempColor = AmongUsColorToImVec4(Palette__TypeInfo->static_fields->Orange);
                            }
                            else if (localData && PlayerIsImpostor(localData) && PlayerIsImpostor(playerData))
                                tempColor = AmongUsColorToImVec4(Palette__TypeInfo->static_fields->ImpostorRoleRed);
                            else if ((playerCtrl == *Game::pLocalPlayer && State.ModDetection) || State.modUsers.count(pid)) {
                                std::string modName = State.modUsers.count(pid) ? RemoveHtmlTags(State.modUsers.at(pid)[0]) : "VLX Menu";
                                if (modName == "KillNetwork")
                                    tempColor = AmongUsColorToImVec4(Palette__TypeInfo->static_fields->ImpostorRed);
                                else if (modName == "BetterAmongUs")
                                    tempColor = AmongUsColorToImVec4(Palette__TypeInfo->static_fields->LogSuccessColor);
                                else if (modName == "AmongUsMenu")
                                    tempColor = AmongUsColorToImVec4(Palette__TypeInfo->static_fields->Orange);
                                else if (modName == "HostGuard")
                                    tempColor = AmongUsColorToImVec4(Palette__TypeInfo->static_fields->LightBlue);
                                else if (modName == "ChocooMenu")
                                    tempColor = AmongUsColorToImVec4(Palette__TypeInfo->static_fields->Purple);
                                else if (modName == "Unknown")
                                    tempColor = AmongUsColorToImVec4(Palette__TypeInfo->static_fields->DisabledGrey);
                                else if (modName == "SickoMenu" || modName == "VLX Menu")
                                    tempColor = State.MenuThemeColor;
                                else if (modName == "SlopMenuCrew")
                                    tempColor = ImVec4(0.744f, 0.855f, 0.1227f, 1.f);
                            }
                        }

                        if (State.RevealRoles) {
                            std::string roleName = GetRoleName(playerData->fields.Role, State.AbbreviatedRoleNames, State.LocalizeRoleNames);
                            tempName = tempName + " (" + roleName + ")";
                            tempColor = AmongUsColorToImVec4(GetRoleColor(playerData->fields.Role, true));
                        }

                        if (playerData->fields.IsDead)
                            tempColor = AmongUsColorToImVec4(Palette__TypeInfo->static_fields->DisabledGrey);

                        cache.finalDisplayName = tempName;
                        cache.finalColor = tempColor;
                        cache.colorButtonId = "##" + tempName + "_ColorButton";
                        cache.lastUpdateFrame = g_FrameCounter;

                        tempColor.w *= State.MenuThemeColor.w;
                    }
                }
            }

            for (auto it = g_PlayerCache.begin(); it != g_PlayerCache.end(); ) {
                if (State.currentPlayers.find(it->first) == State.currentPlayers.end()) {
                    it = g_PlayerCache.erase(it);
                }
                else {
                    ++it;
                }
            }

            for (auto it = State.knownPlayers.begin(); it != State.knownPlayers.end(); ) {
                if (State.currentPlayers.find(*it) == State.currentPlayers.end()) {
                    State.newPlayersAppear.erase(*it);
                    State.finishedPlayers.erase(*it);
                    it = State.knownPlayers.erase(it);
                }
                else {
                    ++it;
                }
            }

            for (auto it = State.newPlayersAppear.begin(); it != State.newPlayersAppear.end(); ) {
                if (State.finishedPlayers.find(it->first) != State.finishedPlayers.end()) {
                    it = State.newPlayersAppear.erase(it);
                }
                else {
                    ++it;
                }
            }

            for (auto playerCtrl : allControllers) {
                if (playerCtrl == NULL) continue;

                auto player = PlayerSelection(playerCtrl);
                if (!player.has_value()) continue;

                auto playerData = GetPlayerData(playerCtrl);
                if (playerData == NULL || playerData->fields.Disconnected) continue;

                app::NetworkedPlayerInfo_PlayerOutfit* outfit = GetPlayerOutfit(playerData);
                if (outfit == NULL) continue;

                uint8_t pid = playerData->fields.PlayerId;

                auto it = g_PlayerCache.find(pid);
                if (it == g_PlayerCache.end()) continue;
                CachedPlayerData& cached = it->second;

                const char* displayPlayerNameC = cached.finalDisplayName.c_str();
                const char* selectableIdC = cached.selectableId.c_str();
                const char* colorButtonIdC = cached.colorButtonId.c_str();

                ImVec4 nameColor = cached.finalColor;

                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0) * State.dpiScale);
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0) * State.dpiScale);

                bool isSelected = std::find(State.selectedPlayers.begin(), State.selectedPlayers.end(), player.get_PlayerId()) != State.selectedPlayers.end();

                if (ImGui::Selectable(selectableIdC, !ShouldUsePlayerPicker() && isSelected)) {
                    if (ShouldUsePlayerPicker()) {
                        std::queue<RPCInterface*>* queue = nullptr;
                        if (IsInGame())
                            queue = &State.rpcQueue;
                        else if (IsInLobby())
                            queue = &State.lobbyRpcQueue;

                        if ((selectingShiftingToPlayer || selectingTurningToPlayer) && !playersToShift.empty()) {
                            for (auto pId : playersToShift) {
                                auto pc = GetPlayerControlById(pId);
                                if (pc == NULL) continue;

                                if (IsHost()) queue->push(new RpcShapeshiftAsHost(pc, player, selectingShiftingToPlayer));
                                else queue->push(new RpcShapeshift(pc, player, selectingShiftingToPlayer));
                            }

                            selectingShiftingToPlayer = false;
                            selectingTurningToPlayer = false;
                        }
                        playersToShift = {};
                        playerToShiftName = "";
                    }
                    else {
                        bool isCtrl = ImGui::IsKeyDown(0x11) || ImGui::IsKeyDown(0xA2) || ImGui::IsKeyDown(0xA3);
                        bool isShifted = ImGui::IsKeyDown(0x10);

                        if (isCtrl) {
                            if (isShifted) {
                                if (State.selectedPlayers.size() == activeIds.size()) {
                                    State.selectedPlayers.clear();
                                    State.selectedPlayer = {};
                                }
                                else {
                                    State.selectedPlayers.clear();
                                    for (auto p : allControllers) {
                                        if (p) {
                                            State.selectedPlayers.push_back(p->fields.PlayerId);
                                            State.selectedPlayer = PlayerSelection(p);
                                        }
                                    }
                                }
                            }
                            else {
                                auto it_sel = std::find(State.selectedPlayers.begin(), State.selectedPlayers.end(), player.get_PlayerId());
                                if (it_sel != State.selectedPlayers.end()) {
                                    State.selectedPlayers.erase(it_sel);
                                    if (State.selectedPlayers.empty()) {
                                        State.selectedPlayer = {};
                                        selectedPlayer = State.selectedPlayer.validate();
                                        if (State.selectedPlayer.has_value()) {
                                            forcedName = cached.nameRaw;
                                            forcedColor = outfit->fields.ColorId;
                                        }
                                    }
                                }
                                else {
                                    State.selectedPlayers.push_back(player.get_PlayerId());
                                    State.selectedPlayer = player.validate();
                                    selectedPlayer = player.validate();
                                    if (State.selectedPlayer.has_value()) {
                                        forcedName = cached.nameRaw;
                                        forcedColor = outfit->fields.ColorId;
                                    }
                                }
                            }
                        }
                        else {
                            State.selectedPlayer = player.validate();
                            selectedPlayer = player.validate();
                            State.selectedPlayers = { player.get_PlayerId() };
                            if (State.selectedPlayer.has_value()) {
                                forcedName = cached.nameRaw;
                                forcedColor = outfit->fields.ColorId;
                            }
                        }
                    }

                    showHiddenInfo = false; // always reset the show state after selection
                }

                ImGui::SameLine();
                auto playerColor = AmongUsColorToImVec4(GetPlayerColor(outfit->fields.ColorId));
                playerColor.w = State.MenuThemeColor.w;
                ImGui::ColorButton(colorButtonIdC, playerColor, ImGuiColorEditFlags_NoBorder | ImGuiColorEditFlags_NoTooltip);
                ImGui::SameLine();
                ImGui::PopStyleVar(2);
                ImGui::Dummy(ImVec2(0, 0) * State.dpiScale);
                ImGui::SameLine();

                if (displayPlayerNameC[0] != '\0') {
                    State.currentPlayers.insert(pid);

                    if (State.knownPlayers.find(pid) == State.knownPlayers.end() && State.finishedPlayers.find(pid) == State.finishedPlayers.end()) {
                        State.knownPlayers.insert(pid);
                        State.newPlayersAppear[pid] = std::chrono::steady_clock::now();
                    }

                    float alpha = 1.0f;
                    auto it_appear = State.newPlayersAppear.find(pid);
                    if (it_appear != State.newPlayersAppear.end()) {
                        auto now = std::chrono::steady_clock::now();
                        float elapsed = std::chrono::duration<float>(now - it_appear->second).count();

                        if (elapsed < State.appearDuration) {
                            float t = elapsed / State.appearDuration;
                            t = sinf(t * 3.14159265f / 2.0f);
                            alpha = t;
                        }
                        else {
                            alpha = 1.0f;
                            State.newPlayersAppear.erase(pid);
                            State.finishedPlayers.insert(pid);
                        }
                    }

                    nameColor.w *= alpha;
                    ImGui::TextColored(nameColor, displayPlayerNameC);
                }
            }
            if (shouldEndListBox)
                ImGui::ListBoxFooter();

            std::vector<PlayerSelection> selectedPlayers = {};
            for (auto id : State.selectedPlayers) {
                auto playerCtrl = GetPlayerControlById(id);
                if (!playerCtrl) continue;
                auto validPlayer = PlayerSelection(playerCtrl).validate();
                if (!validPlayer.has_value() || validPlayer.is_Disconnected()) continue;
                selectedPlayers.push_back(PlayerSelection(playerCtrl));
            }

            /*if () {
                ImGui::NewLine();

                if (ColoredButton(ImVec4(1.f, 0.f, 0.f, 1.f), "Cancel Action")) {
                    selectingShiftingToPlayer = false;
                    selectingTurningToPlayer = false;
                    playersToShift = {};
                    playerToShiftName = "";
                }
            }*/
            if (!ShouldUsePlayerPicker() && selectedPlayer.has_value() && !selectedPlayer.is_Disconnected() && selectedPlayers.size() == 1)
            {
                uint8_t selectedPid = selectedPlayer.get_PlayerData()->fields.PlayerId;

                auto it = g_PlayerCache.find(selectedPid);
                if (it != g_PlayerCache.end())
                {
                    CachedPlayerData& cachedDetails = it->second;

                    if (!selectedPlayer.get_PlayerControl()->fields.notRealPlayer && selectedPlayer.get_PlayerData() != NULL) {
                        bool isUsingMod = (selectedPlayer.is_LocalPlayer() && State.ModDetection) || State.modUsers.count(selectedPid);
                        ImGui::Text("Is using Modified Client: %s", isUsingMod ? "Yes" : "No");
                        if (isUsingMod) ImGui::Text("Mod Name: %s", selectedPlayer.is_LocalPlayer() ? "VLX Menu" : RemoveHtmlTags(State.modUsers.at(selectedPid)[0]).c_str());
                        if (isUsingMod && ((selectedPlayer.is_LocalPlayer() && State.ModDetection) || !State.modUsers.at(selectedPid)[1].empty()))
                            ImGui::Text("Mod Version: %s", selectedPlayer.is_LocalPlayer() ? State.SickoVersion.c_str() : RemoveHtmlTags(State.modUsers.at(selectedPid)[1]).c_str());

                        ImGui::Text("Player ID: %d", selectedPid);

                        /*std::string puid = cachedDetails.puid;
                        std::string puidText = std::format("PUID:\n{}", (!IsStreamerMode()) ? puid : ((puid != "") ? puid.substr(0, 1) + "..." : ""));
                        if (puid != "") {
                            ImGui::Text(const_cast<char*>(puidText.c_str()));
                        }*/

                        uint32_t playerLevel = selectedPlayer.get_PlayerData()->fields.PlayerLevel + 1;
                        ImGui::Text("Level: %d", playerLevel);

                        ImGui::Text("Platform: %s", cachedDetails.platform.c_str());
                        if (!cachedDetails.platformName.empty() && cachedDetails.platformName != "TESTNAME")
                            ImGui::Text("Platform Name: %s", cachedDetails.platformName.c_str());

                        std::string friendCode = cachedDetails.friendCode;
                        bool isWhitelisted = std::find(State.WhitelistFriendCodes.begin(), State.WhitelistFriendCodes.end(), friendCode) != State.WhitelistFriendCodes.end();
                        bool shouldTryHidingInfo = selectedPlayer.is_LocalPlayer() || (isWhitelisted && State.HideWhitelistedPlayerInfo);

                        if (shouldTryHidingInfo) {
                            if (AnimatedButton(showHiddenInfo ? "Hide Sensitive Info" : "Show Sensitive Info")) showHiddenInfo = !showHiddenInfo;
                        }

                        if (!shouldTryHidingInfo || showHiddenInfo) {
                            std::string friendCodeText = std::format("Friend Code: {}", (!IsStreamerMode()) ? friendCode : ((friendCode != "") ? friendCode.substr(0, 1) + "..." : ""));
                            if (friendCode != "") {
                                ImGui::Text(const_cast<char*>(friendCodeText.c_str()));
                            }

                            if (cachedDetails.psnId != 0)
                                ImGui::Text("PSN Platform ID: %llu", cachedDetails.psnId);
                            if (cachedDetails.xboxId != 0)
                                ImGui::Text("Xbox Platform ID: %llu", cachedDetails.xboxId);
                        }
                    }
                    else {
                        ImGui::Text("Is using Modified Client: No");
                        ImGui::Text("Player ID: %d", selectedPid);
                        uint32_t playerLevel = selectedPlayer.get_PlayerData()->fields.PlayerLevel + 1;
                        ImGui::Text("Level: %d", playerLevel);
                    }
                }
            }
            else if (!ShouldUsePlayerPicker()) {
                if (!IsInGame() && !IsInLobby()) ClearCache();
            }

            ImGui::EndChild();
            if (ShouldUsePlayerPicker()) {
                ImGui::SameLine();
                ImGui::BeginChild("playerpicker#actions", ImVec2(300, 0)* State.dpiScale, true, ImGuiWindowFlags_NoBackground);
                
                ImGui::Text(GetPlayerPickerDetails().c_str());
                
                ImGui::NewLine();

                if (ColoredButton(ImVec4(1.f, 0.f, 0.f, 1.f), "Cancel")) {
                    selectingShiftingToPlayer = false;
                    selectingTurningToPlayer = false;
                    playersToShift = {};
                    playerToShiftName = "";
                }

                ImGui::EndChild();
            }
            else {
                ImGui::SameLine();
                ImGui::BeginChild("players#buttons", ImVec2(300, 0) * State.dpiScale, true, ImGuiWindowFlags_NoBackground);
                if (selectedPlayer.has_value()) {
                    if (TabGroup("Player", openPlayer)) {
                        CloseOtherGroups(Groups::Player);
                    }
                    ImGui::SameLine();
                    if (TabGroup("Trolling", openTrolling)) {
                        CloseOtherGroups(Groups::Trolling);
                    }
                    if (IsInMultiplayerGame() || IsInLobby()) ImGui::SameLine();
                    if ((IsInMultiplayerGame() || IsInLobby()) && TabGroup("Info", openInfo)) {
                        CloseOtherGroups(Groups::Info);
                    }
                }
                ImGui::BeginChild("players#actions", ImVec2(300, 0) * State.dpiScale, true, ImGuiWindowFlags_NoBackground);
                if (openPlayer && selectedPlayer.has_value())
                {
                    if (State.DisableMeetings && IsHost()) ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Meetings have been disabled.");
                    GameOptions options;
                    if (IsInGame() && !GetPlayerData(*Game::pLocalPlayer)->fields.IsDead && (!State.DisableMeetings || !IsHost())) { //Player selection doesn't matter
                        if (!State.InMeeting) {
                            if (AnimatedButton("Call Meeting")) {
                                RepairSabotage(*Game::pLocalPlayer);
                                State.rpcQueue.push(new RpcReportBody({}));
                            }
                        }
                        else if (IsHost() || !State.SafeMode) {
                            if (AnimatedButton("Call Meeting")) {
                                RepairSabotage(*Game::pLocalPlayer);
                                State.rpcQueue.push(new RpcForceMeeting(*Game::pLocalPlayer, {}));
                            }
                        }
                    }
                    if ((IsHost() || !State.SafeMode) && State.InMeeting && AnimatedButton("Skip Vote by All")) {
                        State.VoteOffPlayerId = Game::SkippedVote;
                        for (auto player : GetAllPlayerControl()) {
                            /*if (player != selectedPlayer.get_PlayerControl()) {
                                State.rpcQueue.push(new RpcClearVote(player));
                            }*/
                            State.rpcQueue.push(new RpcVotePlayer(player, player, true));
                        }
                    }

                    if (IsInGame() && !State.DisableMeetings && selectedPlayers.size() == 1) {
                        ImGui::NewLine();
                        int playerId = (int)(State.selectedPlayer.get_PlayerId());
                        auto it = std::find(State.validDeadBodyIds.begin(), State.validDeadBodyIds.end(), playerId);
                        // ensure that the dead body actually exists, otherwise we get kicked by the anticheat if we aren't hosting
                        if (!State.InMeeting && it != State.validDeadBodyIds.end()) {
                            if (!GetPlayerData(*Game::pLocalPlayer)->fields.IsDead && AnimatedButton("Report Body")) {
                                State.rpcQueue.push(new RpcReportBody(State.selectedPlayer));
                            }
                        }
                        else if (IsHost() || !State.SafeMode) {
                            if (AnimatedButton("Report Body")) {
                                State.rpcQueue.push(new RpcForceMeeting(*Game::pLocalPlayer, State.selectedPlayer));
                            }
                        }
                    }

                    if (!selectedPlayer.is_Disconnected() && selectedPlayers.size() == 1 && selectedPlayer.has_value())
                    {
                        if (State.playerToFollow.equals(State.selectedPlayer) ||
                            (selectedPlayer.is_LocalPlayer() && State.playerToFollow.has_value())) {
                            if (AnimatedButton("Stop Spectating")) {
                                State.playerToFollow = {};
                            }
                        }
                        else {
                            if (!selectedPlayer.is_LocalPlayer() && AnimatedButton("Spectate")) {
                                State.FreeCam = false;
                                State.playerToFollow = State.selectedPlayer;
                            }
                        }
                    }

                    if (IsInGame() && PlayerIsImpostor(GetPlayerData(*Game::pLocalPlayer))
                        && !GetPlayerData(*Game::pLocalPlayer)->fields.IsDead && ((*Game::pLocalPlayer)->fields.killTimer <= 0.0f)
                        && !State.InMeeting &&
                        !State.selectedPlayer.validate().get_PlayerData()->fields.IsDead)
                    {
                        if (AnimatedButton("Kill"))
                        {
                            if (IsHost() || !State.SafeMode)
                                State.rpcQueue.push(new RpcMurderPlayer(*Game::pLocalPlayer, State.selectedPlayer.validate().get_PlayerControl()));
                            else
                                State.rpcQueue.push(new CmdCheckMurder(State.selectedPlayer));
                        }
                    }
                    else if (IsHost() || !State.SafeMode) {
                        if (IsInGame() && AnimatedButton("Kill"))
                        {
                            for (PlayerSelection p : selectedPlayers) {
                                auto validPlayer = p.validate();
                                if (IsInGame()) {
                                    State.rpcQueue.push(new RpcMurderPlayer(*Game::pLocalPlayer, validPlayer.get_PlayerControl()));
                                }
                                else if (IsInLobby()) {
                                    State.lobbyRpcQueue.push(new RpcMurderPlayer((*Game::pLocalPlayer), validPlayer.get_PlayerControl()));
                                }
                            }
                        }
                    }
                    if (IsInGame() && PlayerIsImpostor(GetPlayerData(*Game::pLocalPlayer))
                        && !GetPlayerData(*Game::pLocalPlayer)->fields.IsDead && ((*Game::pLocalPlayer)->fields.killTimer <= 0.0f)
                        && !State.InMeeting &&
                        !State.selectedPlayer.validate().get_PlayerData()->fields.IsDead)
                    {
                        ImGui::SameLine();
                        if (AnimatedButton("Telekill"))
                        {
                            previousPlayerPosition = GetTrueAdjustedPosition(*Game::pLocalPlayer);
                            for (auto p : selectedPlayers) {
                                auto validPlayer = p.validate();
                                if (IsHost() || !State.SafeMode)
                                    State.rpcQueue.push(new RpcMurderPlayer(*Game::pLocalPlayer, validPlayer.get_PlayerControl()));
                                else
                                    State.rpcQueue.push(new CmdCheckMurder(p));
                            }
                            framesPassed = 40;
                        }
                    }
                    else if (IsInGame() && (IsHost() || !State.SafeMode)) {
                        ImGui::SameLine();
                        if (AnimatedButton("Telekill"))
                        {
                            previousPlayerPosition = GetTrueAdjustedPosition(*Game::pLocalPlayer);
                            for (auto p : selectedPlayers) {
                                auto validPlayer = p.validate();
                                if (IsInGame()) {
                                    State.rpcQueue.push(new RpcMurderPlayer((*Game::pLocalPlayer), validPlayer.get_PlayerControl()));
                                }
                                else if (IsInLobby()) {
                                    State.lobbyRpcQueue.push(new RpcMurderPlayer((*Game::pLocalPlayer), validPlayer.get_PlayerControl()));
                                }
                            }
                            framesPassed = 40;
                        }
                    }

                    if ((IsInMultiplayerGame() || IsInLobby()) && (!selectedPlayer.is_LocalPlayer() || selectedPlayers.size() != 1)) {
                        if (IsHost() && AnimatedButton("Kick")) {
                            State.selectedPlayer = {};
                            State.selectedPlayers.clear();
                            auto future = std::async(std::launch::async, [&]() {
                                for (auto p : selectedPlayers) {
                                    if (p.has_value() && p.validate().get_PlayerControl() != *Game::pLocalPlayer)
                                        app::InnerNetClient_KickPlayer((InnerNetClient*)(*Game::pAmongUsClient), p.validate().get_PlayerControl()->fields._.OwnerId, false, NULL);
                                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                }
                                });
                            future.get();
                        }

                        if (AnimatedButton("Votekick")) {
                            if (IsHost()) {
                                State.selectedPlayer = {};
                                State.selectedPlayers.clear();
                            }
                            for (auto p : selectedPlayers) {
                                if (p.has_value() && p.validate().is_LocalPlayer()) continue;
                                auto future = std::async(std::launch::async, [&]() {
                                    for (auto p : selectedPlayers) {
                                        if (p.has_value() && p.validate().get_PlayerControl() != *Game::pLocalPlayer) {
                                            if (IsInGame()) {
                                                State.rpcQueue.push(new RpcVoteKick(p.validate().get_PlayerControl()));
                                            }
                                            else if (IsInLobby()) {
                                                State.lobbyRpcQueue.push(new RpcVoteKick(p.validate().get_PlayerControl()));
                                            }
                                        }
                                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                    }
                                    });
                                future.get();
                            }
                        }

                        auto hostId = ((InnerNetClient*)(*Game::pAmongUsClient))->fields.HostId;

                        if (!State.SafeMode) {
                            if (AnimatedButton("Attempt to Kick")) {
                                State.selectedPlayer = {};
                                State.selectedPlayers.clear();
                                for (auto p : selectedPlayers) {
                                    if (p.has_value() && p.validate().is_LocalPlayer()) continue;
                                    if (IsInGame()) {
                                        State.rpcQueue.push(new RpcVoteKick(p.validate().get_PlayerControl(), true));
                                    }
                                    else if (IsInLobby()) {
                                        State.lobbyRpcQueue.push(new RpcVoteKick(p.validate().get_PlayerControl(), true));
                                    }
                                }
                            }
                        }
                        else if (IsInGame()) {
                            if (AnimatedButton("Attempt to Ban")) {
                                for (auto p : selectedPlayers) {
                                    if (p.has_value() && p.validate().is_LocalPlayer()) continue;
                                    if (p.has_value() &&
                                        false) continue;
                                    State.rpcQueue.push(new AttemptToBan(p.validate().get_PlayerControl()));
                                }
                            }
                        }

                        if (IsHost() && AnimatedButton("Ban")) {
                            State.selectedPlayer = {};
                            State.selectedPlayers.clear();
                            auto future = std::async(std::launch::async, [&]() {
                                for (auto p : selectedPlayers) {
                                    if (p.has_value() && p.validate().is_LocalPlayer()) continue;
                                    app::InnerNetClient_KickPlayer((InnerNetClient*)(*Game::pAmongUsClient), p.validate().get_PlayerControl()->fields._.OwnerId, true, NULL);
                                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                }
                                });
                            future.get();
                        }

                        std::string friendCode = convert_from_string(selectedPlayer.get_PlayerData()->fields.FriendCode);
                        std::string nickname = RemoveHtmlTags(convert_from_string(GetPlayerOutfit(selectedPlayer.get_PlayerData())->fields.PlayerName));

                        if (selectedPlayers.size() == 1 && friendCode != "") {
                            Game::PlayerId playerId = selectedPlayer.get_PlayerControl()->fields.PlayerId;
                            if (std::find(State.WhitelistFriendCodes.begin(), State.WhitelistFriendCodes.end(), friendCode) == State.WhitelistFriendCodes.end()) {
                                if (std::find(State.BlacklistFriendCodes.begin(), State.BlacklistFriendCodes.end(), friendCode) != State.BlacklistFriendCodes.end()) {
                                    if (AnimatedButton("Remove from Blacklist")) {
                                        State.BlacklistFriendCodes.erase(std::find(State.BlacklistFriendCodes.begin(), State.BlacklistFriendCodes.end(), friendCode));
                                        State.Save();
                                    }
                                }
                                else if (AnimatedButton("Add to Blacklist")) {
                                    State.BlacklistFriendCodes.push_back(friendCode);
                                    State.Save();
                                }
                            }
                            if (std::find(State.BlacklistFriendCodes.begin(), State.BlacklistFriendCodes.end(), friendCode) == State.BlacklistFriendCodes.end()) {
                                if (std::find(State.WhitelistFriendCodes.begin(), State.WhitelistFriendCodes.end(), friendCode) != State.WhitelistFriendCodes.end()) {
                                    if (AnimatedButton("Remove from Whitelist")) {
                                        State.WhitelistFriendCodes.erase(std::find(State.WhitelistFriendCodes.begin(), State.WhitelistFriendCodes.end(), friendCode));
                                        State.Save();
                                    }
                                }
                                else if (AnimatedButton("Add to Whitelist")) {
                                    State.WhitelistFriendCodes.push_back(friendCode);
                                    State.Save();
                                }
                            }
                            ImGui::Dummy(ImVec2(10, 10) * State.dpiScale);
                            if (selectedPlayers.size() == 1 && nickname != "") {
                                std::transform(nickname.begin(), nickname.end(), nickname.begin(), ::tolower);
                                // Convert the name into lowercase
                                Game::PlayerId playerId = selectedPlayer.get_PlayerControl()->fields.PlayerId;
                                if (std::find(State.LockedNames.begin(), State.LockedNames.end(), nickname) != State.LockedNames.end()) {
                                    if (AnimatedButton("Remove Nickname from Name-Checker")) {
                                        State.LockedNames.erase(std::remove(State.LockedNames.begin(), State.LockedNames.end(), nickname), State.LockedNames.end());
                                        State.Save();
                                    }
                                }
                                else {
                                    if (AnimatedButton("Add Nickname to Name-Checker")) {
                                        State.LockedNames.push_back(nickname);
                                        State.Save();
                                    }
                                }
                            }
                        }
                    }

                    if (framesPassed == 0)
                    {
                        if (IsInGame())
                            State.rpcQueue.push(new RpcSnapTo(previousPlayerPosition));
                        else if (IsInLobby())
                            State.lobbyRpcQueue.push(new RpcSnapTo(previousPlayerPosition));
                        framesPassed--;
                    }
                    else framesPassed--;

                    app::RoleBehaviour* playerRole = localData->fields.Role;
                    app::RoleTypes__Enum role = playerRole != nullptr ? playerRole->fields.Role : app::RoleTypes__Enum::Crewmate;

                    if (selectedPlayers.size() == 1) {
                        if ((IsHost() && IsInGame()) || !State.SafeMode)
                        {
                            if (AnimatedButton("Shapeshift To"))
                            {
                                std::queue<RPCInterface*>* queue = nullptr;
                                if (IsInGame())
                                    queue = &State.rpcQueue;
                                else if (IsInLobby())
                                    queue = &State.lobbyRpcQueue;

                                if (IsHost() && IsInGame())
                                    queue->push(new RpcShapeshiftAsHost(*Game::pLocalPlayer, State.selectedPlayer, true));
                                else
                                    queue->push(new RpcShapeshift(*Game::pLocalPlayer, State.selectedPlayer, true));
                            }
                            ImGui::SameLine();
                            if (AnimatedButton("Turn Into"))
                            {
                                std::queue<RPCInterface*>* queue = nullptr;
                                if (IsInGame())
                                    queue = &State.rpcQueue;
                                else if (IsInLobby())
                                    queue = &State.lobbyRpcQueue;

                                if (IsHost() && IsInGame())
                                    queue->push(new RpcShapeshiftAsHost(*Game::pLocalPlayer, State.selectedPlayer, false));
                                else
                                    queue->push(new RpcShapeshift(*Game::pLocalPlayer, State.selectedPlayer, false));
                            }
                        }
                        else if (State.RealRole == RoleTypes__Enum::Shapeshifter && role == RoleTypes__Enum::Shapeshifter) {
                            app::ShapeshifterRole* shapeshifterRole = (app::ShapeshifterRole*)playerRole;
                            if (selectedPlayers.size() == 1 && shapeshifterRole->fields.cooldownSecondsRemaining <= 0) {
                                if (AnimatedButton("Shapeshift To"))
                                {
                                    if (IsInGame())
                                        State.rpcQueue.push(new CmdCheckShapeshift(*Game::pLocalPlayer, State.selectedPlayer, true));
                                    else if (IsInLobby())
                                        State.lobbyRpcQueue.push(new CmdCheckShapeshift(*Game::pLocalPlayer, State.selectedPlayer, true));
                                }
                                ImGui::SameLine();
                                if (AnimatedButton("Turn Into"))
                                {
                                    if (IsInGame())
                                        State.rpcQueue.push(new CmdCheckShapeshift(*Game::pLocalPlayer, State.selectedPlayer, false));
                                    else if (IsInLobby())
                                        State.lobbyRpcQueue.push(new CmdCheckShapeshift(*Game::pLocalPlayer, State.selectedPlayer, false));
                                }
                            }
                        }
                    }

                    if (State.RealRole == RoleTypes__Enum::GuardianAngel && role == RoleTypes__Enum::GuardianAngel) {
                        app::GuardianAngelRole* guardianAngelRole = (app::GuardianAngelRole*)playerRole;
                        if (selectedPlayers.size() == 1 && guardianAngelRole->fields.cooldownSecondsRemaining <= 0 && AnimatedButton("Protect")) {
                            if (IsInGame())
                                State.rpcQueue.push(new CmdCheckProtect(*Game::pLocalPlayer, State.selectedPlayer));
                            else if (IsInLobby())
                                State.lobbyRpcQueue.push(new CmdCheckProtect(*Game::pLocalPlayer, State.selectedPlayer));
                        }
                    }
                    else if ((IsHost() && IsInGame()) || !State.SafeMode) {
                        if (AnimatedButton("Protect")) {
                            for (auto p : selectedPlayers) {
                                app::NetworkedPlayerInfo_PlayerOutfit* outfit = GetPlayerOutfit(p.validate().get_PlayerData());
                                auto colorId = outfit->fields.ColorId;
                                if (IsInGame())
                                    State.rpcQueue.push(new RpcProtectPlayer(*Game::pLocalPlayer, p, colorId));
                                else if (IsInLobby())
                                    State.lobbyRpcQueue.push(new RpcProtectPlayer(*Game::pLocalPlayer, p, colorId));
                            }
                        }
                    }

                    if (IsHost() && selectedPlayers.size() == 1) {
                        auto pid = selectedPlayer.get_PlayerData()->fields.PlayerId;
                        bool isVoteImmune = std::find(State.VoteImmunePlayers.begin(), State.VoteImmunePlayers.end(), pid) != State.VoteImmunePlayers.end();
                        if (AnimatedButton(isVoteImmune ? "Remove Vote Immunity" : "Give Vote Immunity")) {
                            if (isVoteImmune) {
                                State.VoteImmunePlayers.erase(std::remove(State.VoteImmunePlayers.begin(), State.VoteImmunePlayers.end(), pid), State.VoteImmunePlayers.end());
                                State.VoteRedirectTargets.erase(pid);
                            }
                            else
                                State.VoteImmunePlayers.push_back(pid);
                        }
                        if (isVoteImmune) {
                            // build candidate list: Skip (253) first, then other players
                            std::vector<uint8_t> candidates = { 253 };
                            std::vector<std::string> candidateNames = { "Skip" };
                            for (auto pc : GetAllPlayerControl()) {
                                if (pc == nullptr) continue;
                                auto pd = GetPlayerData(pc);
                                if (pd == nullptr || pd->fields.Disconnected) continue;
                                uint8_t otherPid = pd->fields.PlayerId;
                                if (otherPid == pid) continue; 
                                candidates.push_back(otherPid);
                                candidateNames.push_back(RemoveHtmlTags(convert_from_string(NetworkedPlayerInfo_get_PlayerName(pd, NULL))));
                            }

                            uint8_t currentTarget = State.VoteRedirectTargets.count(pid) ? State.VoteRedirectTargets[pid] : 253;
                            int curIndex = 0;
                            for (int i = 0; i < (int)candidates.size(); i++) {
                                if (candidates[i] == currentTarget) { curIndex = i; break; }
                            }
                            // if the stored target left the game, snap back to Skip
                            if (candidates[curIndex] != currentTarget) {
                                curIndex = 0;
                                State.VoteRedirectTargets[pid] = 253;
                            }

                            std::vector<const char*> candidateNamesRaw;
                            for (auto& n : candidateNames) candidateNamesRaw.push_back(n.c_str());
                            ImGui::Text("Redirect Votes To:");
                            if (CustomListBoxInt("Redirect Votes To", &curIndex, candidateNamesRaw))
                                State.VoteRedirectTargets[pid] = candidates[curIndex];
                        }
                    }

                    /*if ((IsInGame() || IsInLobby()) && selectedPlayer.get_PlayerData()->fields.IsDead) {
                        if (AnimatedButton("Revive"))
                        {
                            for (auto p : selectedPlayers) {
                                if (!p.has_value()) continue;
                                if (IsInGame()) {
                                    State.rpcQueue.push(new RpcRevive(p.validate().get_PlayerControl()));
                                }
                                else if (IsInLobby()) {
                                    State.lobbyRpcQueue.push(new RpcRevive(p.validate().get_PlayerControl()));
                                }
                            }
                        }
                    }*/
                    if (selectedPlayers.size() == 1 && !selectedPlayer.is_LocalPlayer() && (IsInMultiplayerGame() || IsInLobby()) && State.AprilFoolsMode) {
                        if (State.ChatCooldown >= 3.5f) {
                            if (AnimatedButton("Mog Player [Sigma]")) {
                                std::vector<std::string> brainrotList = { "Crazy? I was crazy once. They locked me in a room. A rubber room with Fucksons, and Fucksons give me rats.",
                                    "I like my cheese drippy bruh", "Imagine if Ninja got a low taper fade", "I woke up in Ohio, feeling kinda fly", "What trollface are you?",
                                    "Skibidi dop dop dop yes yes", "From the gyatt to the sus to the rizz to the mew", "Yeah I'm edging in Ohio, fanum taxing as I goon",
                                    "You gotta give him that Hawk TUAH and spit on that thang", "Sticking out your gyatt for the rizzler", "I'm Baby Gronk from Ohio",
                                    "19 dollar fortnite card, who wants it?", "Erm, what the sigma?", "I'll take a double triple Grimace Shake on a gyatt",
                                    "I know I'm a SIGMA but that doesnt mean I can't have a GYATT too", "Just put the fries in the bag bro", "Stay on the sigma grindset",
                                    "Sigma Sigma on the wall, who is the skibidiest of them all?", "Duke Dennis did you pray today?", "What kinda bomboclat dawg are ya" };
                                if (IsInGame()) State.rpcQueue.push(new RpcSendChat(*Game::pLocalPlayer, brainrotList[randi(0, (int)brainrotList.size() - 1)], selectedPlayer.get_PlayerControl()));
                                if (IsInLobby()) State.lobbyRpcQueue.push(new RpcSendChat(*Game::pLocalPlayer, brainrotList[randi(0, (int)brainrotList.size() - 1)], selectedPlayer.get_PlayerControl()));
                                State.MessageSent = true;
                            }
                            if (State.DiddyPartyMode && AnimatedButton("Rizz Up Player [Skibidi]")) {
                                std::vector<std::string> rizzLinesList = { "Do you have some Ohio rizz? Because you just turned my brain into pure jelly!",
                                    "If beauty were a Skibidi Toilet, you'd be the one everyone’s trying to get next to!", "Is your name Ohio? Because you’re making my heart do the Skibidi!",
                                    "Is your aura made of coffee? Because you’re brewing up some strong feelings in me!", "I see dat gyatt and I wanna fanum tax some of dat",
                                    "Am I Baby Gronk? Because you can be my Livvy Dunne", "Sup shawty, are you skibidi, because I could use that to my sigma", "Hey shawty, are you skibidi rizz in ohio?",
                                    "Yer a rizzard Harry", "Remind me what a work of skibidi rizz looks like" };
                                if (IsInGame()) State.rpcQueue.push(new RpcSendChat(*Game::pLocalPlayer, rizzLinesList[randi(0, (int)rizzLinesList.size() - 1)], selectedPlayer.get_PlayerControl()));
                                if (IsInLobby()) State.lobbyRpcQueue.push(new RpcSendChat(*Game::pLocalPlayer, rizzLinesList[randi(0, (int)rizzLinesList.size() - 1)], selectedPlayer.get_PlayerControl()));
                                State.MessageSent = true;
                            }
                        }
                    }

                    static int ventId = 0;
                    if (/*(IsHost() || !State.SafeMode) && */IsInGame()) {
                        std::vector<const char*> allVents;
                        switch (State.mapType) {
                        case Settings::MapType::Ship:
                            allVents = SHIPVENTS;
                            break;
                        case Settings::MapType::Hq:
                            allVents = HQVENTS;
                            break;
                        case Settings::MapType::Pb:
                            allVents = PBVENTS;
                            break;
                        case Settings::MapType::Airship:
                            allVents = AIRSHIPVENTS;
                            break;
                        case Settings::MapType::Fungle:
                            allVents = FUNGLEVENTS;
                            break;
                        }
                        ventId = std::clamp(ventId, 0, (int)allVents.size() - 1);

                        CustomListBoxInt("Vent", &ventId, allVents);

                        if (AnimatedButton("Teleport to Vent")) {
                            for (auto p : selectedPlayers) {
                                if (IsHost() || !State.SafeMode)
                                    State.rpcQueue.push(new RpcBootFromVent(p.validate().get_PlayerControl(),
                                        (State.mapType == Settings::MapType::Hq) ? ventId + 1 : ventId)); //MiraHQ vents start from 1 instead of 0
                                else    
                                    State.rpcQueue.push(new RpcBootFromVentNonHost(p.validate().get_PlayerControl(),
                                        (State.mapType == Settings::MapType::Hq) ? ventId + 1 : ventId)); //MiraHQ vents start from 1 instead of 0
                            }
                        }

                        if (AnimatedButton("Teleport to Random Vent")) {
                            for (auto p : selectedPlayers) {
                                bool isHq = State.mapType == Settings::MapType::Hq;
                                int randomVentId = randi((int)isHq, (int)allVents.size() - (int)(!isHq));

                                if (IsHost() || !State.SafeMode)
                                    State.rpcQueue.push(new RpcBootFromVent(p.validate().get_PlayerControl(), randomVentId));
                                else    
                                    State.rpcQueue.push(new RpcBootFromVentNonHost(p.validate().get_PlayerControl(), randomVentId));
                            }
                        }

                        if (selectedPlayers.size() == 1) {
                            for (auto p : selectedPlayers) {
                                if (!p.has_value()) break;
                                auto playerId = p.get_PlayerId();
                                auto it = std::find(State.spamRandomVentTpPlayers.begin(), State.spamRandomVentTpPlayers.end(), playerId);
                                bool isRandomTpSpammed = it != State.spamRandomVentTpPlayers.end();
                                bool isTpSpammed = State.spamVentTpPlayers.find(playerId) != State.spamVentTpPlayers.end();

                                if (!isRandomTpSpammed && (!State.IgnoreVentTpSelf || !p.validate().is_LocalPlayer()) && AnimatedButton("Spam Teleport to Random Vents")) {
                                    State.spamRandomVentTpPlayers.push_back(p.get_PlayerId());
                                    if (isTpSpammed) State.spamVentTpPlayers.erase(playerId);
                                }
                                else if (isRandomTpSpammed && AnimatedButton("Stop Spam Teleport to Random Vents")) {
                                    State.spamRandomVentTpPlayers.erase(it);
                                }

                                if (!isTpSpammed && (!State.IgnoreVentTpSelf || !p.validate().is_LocalPlayer()) && AnimatedButton("Spam Teleport to Vent")) {
                                    State.spamVentTpPlayers[p.get_PlayerId()] = ventId;
                                    if (isRandomTpSpammed) State.spamRandomVentTpPlayers.erase(it);

                                }
                                else if (isTpSpammed && AnimatedButton("Stop Spam Teleport to Vent")) {
                                    State.spamVentTpPlayers.erase(playerId);
                                }
                                break;
                            }
                        }

                        if (State.mapType == Settings::MapType::Fungle && !State.SafeMode) {
                            ImGui::Text("Force Climb Zipline from:");
                            if (AnimatedButton("Bottom to Top")) {
                                for (auto p : selectedPlayers) {
                                    if (!p.has_value()) break;
                                    State.rpcQueue.push(new RpcClimbZipline(p.validate().get_PlayerControl(), false));
                                }
                            }
                            ImGui::SameLine();
                            if (AnimatedButton("Top to Bottom")) {
                                for (auto p : selectedPlayers) {
                                    if (!p.has_value()) break;
                                    State.rpcQueue.push(new RpcClimbZipline(p.validate().get_PlayerControl(), true));
                                }
                            }

                            if (selectedPlayers.size() == 1) {
                                for (auto p : selectedPlayers) {
                                    if (!p.has_value()) break;
                                    auto playerId = p.get_PlayerId();
                                    auto it = std::find(State.spamZiplinePlayers.begin(), State.spamZiplinePlayers.end(), playerId);
                                    bool isZiplineSpammed = it != State.spamZiplinePlayers.end();

                                    if (!isZiplineSpammed && (!State.IgnoreZiplineSelf || !p.validate().is_LocalPlayer()) && AnimatedButton("Spam Climb Zipline")) {
                                        State.spamZiplinePlayers.push_back(p.get_PlayerId());
                                    }
                                    else if (isZiplineSpammed && AnimatedButton("Stop Spam Climbing Zipline")) {
                                        State.spamZiplinePlayers.erase(it);
                                    }
                                    break;
                                }
                            }
                        }
                    }

                    if (IsInGame() && !selectedPlayer.is_Disconnected() && (IsInMultiplayerGame() || selectedPlayer.is_LocalPlayer()))
                    {
                        if ((!State.SafeMode || (selectedPlayer.is_LocalPlayer() && selectedPlayers.size() == 1)) && AnimatedButton("Complete all Tasks")) {
                            if (State.SafeMode) {
                                CompleteAllTasks();
                            }
                            else {
                                for (auto p : selectedPlayers) {
                                    CompleteAllTasks(p.validate().get_PlayerControl());
                                }
                            }
                        }

                        if (selectedPlayers.size() == 1) {
                            auto tasks = GetNormalPlayerTasks(selectedPlayer.get_PlayerControl());

                            if (State.RevealRoles && PlayerIsImpostor(selectedPlayer.get_PlayerData()))
                            {
                                ImGui::TextColored(ImVec4(0.8F, 0.2F, 0.0F, 1.0F), "Fake Tasks:");
                            }
                            else
                            {
                                ImGui::Text("Tasks:");
                            }

                            bool shouldEndListBox = ImGui::ListBoxHeader("###tasks#list"/*, ImVec2(181, 94) * State.dpiScale*/);

                            if (selectedPlayer.get_PlayerControl()->fields.myTasks == nullptr)
                            {
                                ImGui::Text("ERROR: Could not load tasks.");
                            }
                            else
                            {
                                for (auto task : tasks)
                                {
                                    if (task->fields.taskStep == task->fields.MaxStep)
                                        ImGui::TextColored(ImVec4(0.0F, 1.0F, 0.0F, 1.0F), (std::string(TranslateTaskTypes(task->fields._.TaskType))).c_str());
                                    else {
                                        if ((!State.SafeMode || selectedPlayer.is_LocalPlayer())) {
                                            if (AnimatedButton((std::string(TranslateTaskTypes(task->fields._.TaskType))).c_str()))
                                                State.taskRpcQueue.push(new RpcForceCompleteTask(selectedPlayer.get_PlayerControl(), task->fields._._Id_k__BackingField));
                                        }
                                        else ImGui::Text((std::string(TranslateTaskTypes(task->fields._.TaskType))).c_str());
                                    }
                                }
                            }
                            if (shouldEndListBox)
                                ImGui::ListBoxFooter();
                        }
                    }

                    std::string WarnedfriendCode = convert_from_string(selectedPlayer.get_PlayerData()->fields.FriendCode);
                    auto it = State.WarnedFriendCodes.find(WarnedfriendCode);
                    int warnCount = (it != State.WarnedFriendCodes.end()) ? it->second : 0;
                    static char warnReasonBuf[128] = "";

                    ImGui::NewLine();

                    if ((IsInLobby() || IsInMultiplayerGame()) && (!selectedPlayer.is_LocalPlayer() && selectedPlayers.size() == 1)) {

                        double currentTime = ImGui::GetTime();
                        bool cooldownActive = (State.NotifyWarned && (currentTime - State.LastWarnTime < 3.0));

                        ImVec2 buttonSize = ImVec2(0, 0);
                        buttonSize = ImGui::CalcTextSize("Add Warn");
                        buttonSize.x += ImGui::GetStyle().FramePadding.x * 2;
                        buttonSize.y += ImGui::GetStyle().FramePadding.y * 2;

                        if (!cooldownActive) {
                            if (ImGui::Button("Add Warn")) {
                                if (strlen(warnReasonBuf) > 0) {
                                    std::string reasonStr = warnReasonBuf;
                                    State.WarnedFriendCodes[WarnedfriendCode] = warnCount + 1;
                                    State.WarnReasons[WarnedfriendCode].push_back(reasonStr);
                                    warnReasonBuf[0] = '\0';
                                    State.Save();

                                    if (State.NotifyWarned) {
                                        State.LastWarnTime = currentTime;

                                        for (auto& player : GetAllPlayerControl()) {
                                            if (!player) continue;
                                            auto pdata = GetPlayerDataById(player->fields.PlayerId);
                                            if (!pdata) continue;

                                            std::string fc = convert_from_string(pdata->fields.FriendCode);
                                            if (fc == WarnedfriendCode) {
                                                SendPrivateWarnMessage(player, reasonStr.c_str(), warnCount + 1);
                                                break;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        else {
                            ImGui::Dummy(buttonSize);
                        }

                        ImGui::SameLine();
                        ImGui::Text("Total Warns: %d", warnCount);

                        ImGui::InputText("Warn Reason", warnReasonBuf, IM_ARRAYSIZE(warnReasonBuf));
                        ImGui::TextColored(ImVec4(1.f, 0.f, 0.f, 1.f), "Requirement: Enter Warn Reason.");

                        ImGui::NewLine();

                        auto& warnReasons = State.WarnReasons[WarnedfriendCode];
                        if (!warnReasons.empty()) {
                            ImGui::Text("Warn Reasons:");

                            static int selectedReason = 0;
                            selectedReason = std::clamp(selectedReason, 0, (int)warnReasons.size() - 1);

                            std::vector<std::string> numberedReasons;
                            numberedReasons.reserve(warnReasons.size());
                            for (size_t i = 0; i < warnReasons.size(); ++i) {
                                numberedReasons.push_back(std::format("[{}] {}", i + 1, warnReasons[i]));
                            }

                            std::vector<const char*> reasonCStrs;
                            for (const auto& str : numberedReasons) reasonCStrs.push_back(str.c_str());

                            ImGui::PushItemWidth(200);
                            ImGui::ListBox("##WarnReasonList", &selectedReason, reasonCStrs.data(), (int)reasonCStrs.size());
                            ImGui::PopItemWidth();

                            ImGui::SameLine();
                            if (ImGui::Button("Delete")) {
                                if (selectedReason >= 0 && selectedReason < (int)warnReasons.size()) {
                                    warnReasons.erase(warnReasons.begin() + selectedReason);
                                    selectedReason = 0;

                                    if (--State.WarnedFriendCodes[WarnedfriendCode] <= 0) {
                                        State.WarnedFriendCodes.erase(WarnedfriendCode);
                                        State.WarnReasons.erase(WarnedfriendCode);
                                    }

                                    State.Save();
                                }
                            }
                        }
                    }
                }

                if (openTrolling && selectedPlayer.has_value()) {
                    if ((IsHost() && IsInGame()) || !State.SafeMode) {
                        if (AnimatedButton("Send Blank Chat As")) {
                            for (auto p : selectedPlayers) {
                                if (IsInGame()) State.rpcQueue.push(new RpcSendChatNote(p.validate().get_PlayerControl(), 1));
                                if (IsInLobby()) State.lobbyRpcQueue.push(new RpcSendChatNote(p.validate().get_PlayerControl(), 1));
                            }
                        }
                        /*ImGui::SameLine();
                        if (AnimatedButton("Spam Blank Chat As")) {
                            for (auto p : selectedPlayers) {
                                if (IsInGame()) State.rpcQueue.push(new RpcSpamChatNote(p.validate().get_PlayerControl()));
                                if (IsInLobby()) State.lobbyRpcQueue.push(new RpcSpamChatNote(p.validate().get_PlayerControl()));
                            }
                        }*/
                    }

                    if ((IsHost() || !State.SafeMode) && IsInGame() && selectedPlayers.size() == 1) {
                        if (!State.InMeeting) {
                            if (AnimatedButton("Force Meeting By") && !GetPlayerData(selectedPlayer.get_PlayerControl())->fields.IsDead) {
                                if (IsHost() || !State.SafeMode) State.rpcQueue.push(new RpcForceReportBody(selectedPlayer.get_PlayerControl(), {}));
                                else {
                                    State.rpcQueue.push(new RpcReportBody(selectedPlayer));
                                    State.rpcQueue.push(new RpcForceMeeting(selectedPlayer.get_PlayerControl(), {}));
                                }
                            }
                        }
                        else {
                            if (AnimatedButton("Force Meeting By")) {
                                State.rpcQueue.push(new RpcForceMeeting(selectedPlayer.get_PlayerControl(), {}));
                            }
                        }
                    }

                    if ((IsHost() || !State.SafeMode) && selectedPlayer.has_value() && IsInGame() && selectedPlayers.size() == 1) {
                        ImGui::SameLine();
                        if (!State.InMeeting) {
                            if (!selectedPlayer.get_PlayerData()->fields.IsDead && AnimatedButton("Self-Report")) {
                                if (IsHost() || !State.SafeMode) State.rpcQueue.push(new RpcForceReportBody(selectedPlayer.get_PlayerControl(), selectedPlayer));
                                else {
                                    State.rpcQueue.push(new RpcReportBody(selectedPlayer));
                                    State.rpcQueue.push(new RpcForceMeeting(selectedPlayer.get_PlayerControl(), selectedPlayer));
                                }
                            }
                        }
                        else {
                            if (AnimatedButton("Self-Report")) {
                                State.rpcQueue.push(new RpcForceMeeting(selectedPlayer.get_PlayerControl(), State.selectedPlayer));
                            }
                        }
                    }

                    if (selectedPlayers.size() == 1) {
                        app::NetworkedPlayerInfo_PlayerOutfit* outfit = GetPlayerOutfit(selectedPlayer.get_PlayerData());
                        if (outfit != NULL) {
                            auto petId = outfit->fields.PetId;
                            auto skinId = outfit->fields.SkinId;
                            auto hatId = outfit->fields.HatId;
                            auto visorId = outfit->fields.VisorId;
                            auto colorId = outfit->fields.ColorId;
                            auto namePlateId = outfit->fields.NamePlateId;
                            std::queue<RPCInterface*>* queue = nullptr;
                            if (IsInGame())
                                queue = &State.rpcQueue;
                            else if (IsInLobby())
                                queue = &State.lobbyRpcQueue;

                            if (!selectedPlayer.is_LocalPlayer()) {
                                if (!State.activeImpersonation && AnimatedButton(!State.SafeMode ? "Impersonate" : "Copy Outfit")) {
                                    if (queue != nullptr) {
                                        if (IsHost() || !State.SafeMode)
                                            queue->push(new RpcForceColor(*Game::pLocalPlayer, colorId));
                                        else
                                            queue->push(new RpcSetColor(GetRandomColorId()));
                                        queue->push(new RpcSetPet(petId));
                                        queue->push(new RpcSetSkin(skinId));
                                        queue->push(new RpcSetVisor(visorId));
                                        queue->push(new RpcSetHat(hatId));
                                        queue->push(new RpcSetNamePlate(namePlateId));
                                        if (!State.SafeMode) ImpersonateName(selectedPlayer.get_PlayerData());
                                        queue->push(new RpcSetLevel(*Game::pLocalPlayer, selectedPlayer.get_PlayerData()->fields.PlayerLevel));
                                        State.activeImpersonation = true;
                                    }
                                }
                                ImGui::SetNextItemWidth(300 * State.dpiScale);
                                if (ImGui::CollapsingHeader("Cosmetics Stealer")) {
                                    if (!State.SafeMode) {
                                        if (AnimatedButton("Name"))
                                            ImpersonateName(selectedPlayer.get_PlayerData());
                                        ImGui::SameLine();
                                    }
                                    if (IsHost() || !State.SafeMode) {
                                        if (AnimatedButton("Color") && queue != nullptr)
                                            queue->push(new RpcForceColor(*Game::pLocalPlayer, colorId));
                                        ImGui::SameLine();
                                    }
                                    if (AnimatedButton("Hat") && queue != nullptr)
                                        queue->push(new RpcSetHat(hatId));
                                    ImGui::SameLine();
                                    if (AnimatedButton("Skin") && queue != nullptr)
                                        queue->push(new RpcSetSkin(skinId));

                                    if (AnimatedButton("Visor") && queue != nullptr)
                                        queue->push(new RpcSetVisor(visorId));
                                    ImGui::SameLine();
                                    if (AnimatedButton("Pet") && queue != nullptr)
                                        queue->push(new RpcSetPet(petId));
                                    ImGui::SameLine();
                                    if (AnimatedButton("Nameplate") && queue != nullptr)
                                        queue->push(new RpcSetNamePlate(namePlateId));
                                }
                            }

                            ImGui::SetNextItemWidth(300 * State.dpiScale);
                            if (ImGui::CollapsingHeader("Cosmetics Resetter")) {
                                if (!State.SafeMode) {
                                    if (AnimatedButton("Name ", true, ImVec2(0, 0)) && queue != nullptr)
                                        queue->push(new RpcSetName(State.originalName));
                                    ImGui::SameLine();
                                }
                                if (AnimatedButton("Color ", true, ImVec2(0, 0)) && queue != nullptr) {
                                    if (IsHost() || !State.SafeMode) queue->push(new RpcForceColor(*Game::pLocalPlayer, State.originalColor));
                                    else queue->push(new RpcSetColor(State.originalColor));
                                }
                                ImGui::SameLine();
                                if (AnimatedButton("Hat ", true, ImVec2(0, 0)) && queue != nullptr)
                                    queue->push(new RpcSetHat(State.originalHat));
                                ImGui::SameLine();
                                if (AnimatedButton("Skin ", true, ImVec2(0, 0)) && queue != nullptr)
                                    queue->push(new RpcSetSkin(State.originalSkin));

                                if (AnimatedButton("Visor ", true, ImVec2(0, 0)) && queue != nullptr)
                                    queue->push(new RpcSetVisor(State.originalVisor));
                                ImGui::SameLine();
                                if (AnimatedButton("Pet ", true, ImVec2(0, 0)) && queue != nullptr)
                                    queue->push(new RpcSetPet(State.originalNamePlate));
                                ImGui::SameLine();
                                if (AnimatedButton("Nameplate ", true, ImVec2(0, 0)) && queue != nullptr)
                                    queue->push(new RpcSetNamePlate(State.originalNamePlate));
                            }
                        }
                    }

                    if (!State.SafeMode && AnimatedButton("Impersonate Everyone To") && selectedPlayers.size() == 1) {
                        app::NetworkedPlayerInfo_PlayerOutfit* outfit = GetPlayerOutfit(selectedPlayer.get_PlayerData());
                        auto petId = outfit->fields.PetId;
                        auto skinId = outfit->fields.SkinId;
                        auto hatId = outfit->fields.HatId;
                        auto visorId = outfit->fields.VisorId;
                        auto colorId = outfit->fields.ColorId;
                        auto namePlateId = outfit->fields.NamePlateId;
                        std::queue<RPCInterface*>* queue = nullptr;
                        if (IsInGame())
                            queue = &State.rpcQueue;
                        else if (IsInLobby())
                            queue = &State.lobbyRpcQueue;
                        if (queue != nullptr) {
                            for (auto p : GetAllPlayerControl()) {
                                if (p == selectedPlayer.get_PlayerControl()) continue; //preserve the original player
                                std::string name = convert_from_string(NetworkedPlayerInfo_get_PlayerName(selectedPlayer.get_PlayerData(), NULL))
                                    + std::format("<size=0>{}</size>", p->fields.PlayerId);
                                queue->push(new RpcForceColor(p, GetRandomColorId()));
                                queue->push(new RpcForcePet(p, petId));
                                queue->push(new RpcForceSkin(p, skinId));
                                queue->push(new RpcForceVisor(p, visorId));
                                queue->push(new RpcForceHat(p, hatId));
                                queue->push(new RpcForceNamePlate(p, namePlateId));
                                queue->push(new RpcForceName(p, name));
                                queue->push(new RpcSetLevel(p, selectedPlayer.get_PlayerData()->fields.PlayerLevel));
                                State.activeImpersonation = true;
                            }
                        }
                    }

                    if (State.activeImpersonation)
                    {
                        if (AnimatedButton(!State.SafeMode ? "Reset Impersonation" : "Reset Original Outfit"))
                        {
                            ControlAppearance(false);
                        }
                    }

                    if (!State.SafeMode && IsInLobby() && AnimatedButton(selectedPlayers.size() == 1 ? "Allow Player to NoClip" : "Allow Players to NoClip")) {
                        for (auto p : selectedPlayers) {
                            if (p.has_value() && p.validate().is_LocalPlayer()) State.NoClip = true;
                            else State.lobbyRpcQueue.push(new RpcMurderLoop(*Game::pLocalPlayer, p.validate().get_PlayerControl(), 1, true));
                            if (selectedPlayers.size() == 1) {
                                Toasts::AddToast("Allow Player to NoClip", std::format("Allowed {} to NoClip!",
                                    convert_from_string(NetworkedPlayerInfo_get_PlayerName(p.validate().get_PlayerData(), NULL))), ImVec4(0.f, 1.f, 1.f, 1.f));
                            }
                            else {
                                Toasts::AddToast("Allow Player to NoClip", std::format("Allowed {} players to NoClip!", selectedPlayers.size()),
                                    ImVec4(0.f, 1.f, 1.f, 1.f));
                            }
                        }
                    }

                    if (!State.SafeMode) {
                        if (AnimatedButton("Suicide")) {
                            for (auto p : selectedPlayers) {
                                auto validPlayer = p.validate();
                                if (IsInGame()) {
                                    State.rpcQueue.push(new RpcMurderPlayer(validPlayer.get_PlayerControl(), validPlayer.get_PlayerControl(),
                                        validPlayer.get_PlayerControl()->fields.protectedByGuardianId < 0 || State.BypassAngelProt));
                                }
                                else if (IsInLobby()) {
                                    State.lobbyRpcQueue.push(new RpcMurderPlayer(validPlayer.get_PlayerControl(), validPlayer.get_PlayerControl(),
                                        validPlayer.get_PlayerControl()->fields.protectedByGuardianId < 0 || State.BypassAngelProt));
                                }
                            }
                        }
                        ImGui::SameLine();
                        if (AnimatedButton("Exile")) {
                            for (auto p : selectedPlayers) {
                                if (IsInGame()) State.rpcQueue.push(new RpcExiled(p.validate().get_PlayerControl(), true));
                                else State.lobbyRpcQueue.push(new RpcExiled(p.validate().get_PlayerControl(), true));
                            }
                        }
                    }

                    if ((IsHost() || !State.SafeMode) && selectedPlayers.size() == 1) {
                        if (IsInGame()) {
                            if (!State.murderLoop && AnimatedButton("Murder Loop")) {
                                State.murderCount = 200; //controls how many times the player is to be murdered
                                State.murderLoop = true;
                            }
                            if (State.murderLoop && AnimatedButton("Stop Murder Loop")) {
                                State.murderLoop = false;
                                State.murderCount = 0;
                            }
                            ImGui::SameLine();
                            ImGui::Text(std::format("({})", 200 - State.murderCount).c_str());
                        }
                    }

                    if (!State.SafeMode && IsInGame() && selectedPlayers.size() == 1) {
                        ImGui::SameLine();
                        if (!State.suicideLoop && AnimatedButton("Suicide Loop")) {
                            State.suicideCount = 200; //controls how many times the player is to be murdered
                            State.suicideLoop = true;
                        }
                        if (State.suicideLoop && AnimatedButton("Stop Suicide Loop")) {
                            State.suicideCount = 0;
                            State.suicideLoop = false;
                        }
                        ImGui::SameLine();
                        ImGui::Text(std::format("Stop Suicide Loop ({})", 800 - State.suicideCount * 4).c_str());
                    }

                    if (!State.SafeMode && selectedPlayers.size() == 1 && IsInGame()) {
                        if (AnimatedButton("Kill Crewmates By")) {
                            for (auto player : GetAllPlayerControl()) {
                                if (!PlayerIsImpostor(GetPlayerData(player))) {
                                    if (!State.SafeMode) {
                                        if (IsInGame()) {
                                            State.rpcQueue.push(new RpcMurderPlayer(selectedPlayer.get_PlayerControl(), player,
                                                selectedPlayer.get_PlayerControl()->fields.protectedByGuardianId < 0 || State.BypassAngelProt));
                                        }
                                        else if (IsInLobby()) {
                                            State.lobbyRpcQueue.push(new RpcMurderPlayer(selectedPlayer.get_PlayerControl(), player,
                                                selectedPlayer.get_PlayerControl()->fields.protectedByGuardianId < 0 || State.BypassAngelProt));
                                        }
                                    }
                                    else {
                                        if (IsInGame()) {
                                            State.rpcQueue.push(new FakeMurderPlayer(selectedPlayer.get_PlayerControl(), player,
                                                selectedPlayer.get_PlayerControl()->fields.protectedByGuardianId < 0 || State.BypassAngelProt));
                                        }
                                        else if (IsInLobby()) {
                                            State.lobbyRpcQueue.push(new FakeMurderPlayer(selectedPlayer.get_PlayerControl(), player,
                                                selectedPlayer.get_PlayerControl()->fields.protectedByGuardianId < 0 || State.BypassAngelProt));
                                        }
                                    }
                                }
                            }
                        }
                        if (AnimatedButton("Kill Impostors By") && IsInGame()) {
                            for (auto player : GetAllPlayerControl()) {
                                if (!State.SafeMode) {
                                    if (IsInGame()) {
                                        State.rpcQueue.push(new RpcMurderPlayer(selectedPlayer.get_PlayerControl(), player,
                                            selectedPlayer.get_PlayerControl()->fields.protectedByGuardianId < 0 || State.BypassAngelProt));
                                    }
                                    else if (IsInLobby()) {
                                        State.lobbyRpcQueue.push(new RpcMurderPlayer(selectedPlayer.get_PlayerControl(), player,
                                            selectedPlayer.get_PlayerControl()->fields.protectedByGuardianId < 0 || State.BypassAngelProt));
                                    }
                                }
                                else {
                                    if (IsInGame()) {
                                        State.rpcQueue.push(new FakeMurderPlayer(selectedPlayer.get_PlayerControl(), player,
                                            selectedPlayer.get_PlayerControl()->fields.protectedByGuardianId < 0 || State.BypassAngelProt));
                                    }
                                    else if (IsInLobby()) {
                                        State.lobbyRpcQueue.push(new FakeMurderPlayer(selectedPlayer.get_PlayerControl(), player,
                                            selectedPlayer.get_PlayerControl()->fields.protectedByGuardianId < 0 || State.BypassAngelProt));
                                    }
                                }
                            }
                        }
                    }

                    std::queue<RPCInterface*>* queue = nullptr;
                    if (IsInGame())
                        queue = &State.rpcQueue;
                    else if (IsInLobby())
                        queue = &State.lobbyRpcQueue;

                    if ((IsHost() && IsInGame()) || !State.SafeMode)
                    {
                        if (AnimatedButton(selectedPlayers.size() == 1 ? "Shapeshift Player To..." : "Shapeshift Players To...")) {
                            selectingShiftingToPlayer = true;
                            for (PlayerSelection ps : selectedPlayers) {
                                playersToShift.push_back(ps.get_PlayerId());

                                auto outfit = GetPlayerOutfit(ps.validate().get_PlayerData());
                                if (selectedPlayers.size() == 1 && outfit != nullptr)
                                    playerToShiftName = convert_from_string(outfit->fields.PlayerName);
                            }
                        }
                        ImGui::SameLine();
                        if (AnimatedButton(selectedPlayers.size() == 1 ? "Unshift Player" : "Unshift Players")) {
                            for (PlayerSelection ps : selectedPlayers) {
                                auto validPlayer = ps.validate();
                                if (IsHost()) {
                                    queue->push(new RpcShapeshiftAsHost(validPlayer.get_PlayerControl(), validPlayer, true));
                                }
                                else {
                                    queue->push(new RpcShapeshift(validPlayer.get_PlayerControl(), validPlayer, true));
                                }
                            }
                        }

                        if (AnimatedButton(selectedPlayers.size() == 1 ? "Turn Player Into..." : "Turn Players Into...")) {
                            selectingTurningToPlayer = true;
                            for (PlayerSelection ps : selectedPlayers) {
                                if (!ps.has_value()) continue;
                                playersToShift.push_back(ps.get_PlayerId());

                                auto outfit = GetPlayerOutfit(ps.validate().get_PlayerData());
                                if (selectedPlayers.size() == 1 && outfit != nullptr)
                                    playerToShiftName = convert_from_string(outfit->fields.PlayerName);
                            }
                        }
                        ImGui::SameLine();
                        if (AnimatedButton(selectedPlayers.size() == 1 ? "Reset Player" : "Reset Players")) {
                            for (PlayerSelection ps : selectedPlayers) {
                                auto validPlayer = ps.validate();
                                if (IsHost()) {
                                    queue->push(new RpcShapeshiftAsHost(validPlayer.get_PlayerControl(), validPlayer, false));
                                }
                                else {
                                    queue->push(new RpcShapeshift(validPlayer.get_PlayerControl(), validPlayer, false));
                                }
                            }
                        }

                        ImGui::NewLine();

                        if (selectedPlayers.size() == 1 && AnimatedButton("Shift Everyone To"))
                        {
                            for (auto player : GetAllPlayerControl()) {
                                if (IsHost()) {
                                    queue->push(new RpcShapeshiftAsHost(player, State.selectedPlayer, true));
                                }
                                else {
                                    queue->push(new RpcShapeshift(player, State.selectedPlayer, true));
                                }
                            }
                        }
                        if (selectedPlayers.size() == 1) ImGui::SameLine();
                        if (AnimatedButton("Unshift Everyone"))
                        {
                            for (auto player : GetAllPlayerControl()) {
                                if (IsHost()) {
                                    queue->push(new RpcShapeshiftAsHost(player, PlayerSelection(player), true));
                                }
                                else {
                                    queue->push(new RpcShapeshift(player, PlayerSelection(player), true));
                                }
                            }
                        }

                        if (selectedPlayers.size() == 1 && AnimatedButton("Turn Everyone Into"))
                        {
                            for (auto player : GetAllPlayerControl()) {
                                if (IsHost()) {
                                    queue->push(new RpcShapeshiftAsHost(player, State.selectedPlayer, false));
                                }
                                else {
                                    queue->push(new RpcShapeshift(player, State.selectedPlayer, false));
                                }
                            }
                        }
                        if (selectedPlayers.size() == 1) ImGui::SameLine();
                        if (AnimatedButton("Reset Everyone"))
                        {
                            for (auto player : GetAllPlayerControl()) {
                                if (IsHost()) {
                                    queue->push(new RpcShapeshiftAsHost(player, PlayerSelection(player), false));
                                }
                                else {
                                    queue->push(new RpcShapeshift(player, PlayerSelection(player), false));
                                }
                            }
                        }

                        if (!State.SafeMode && selectedPlayers.size() == 1 && selectedPlayer.has_value()) {
                            auto roleType = selectedPlayer.get_PlayerData()->fields.RoleType;
                            if (roleType == RoleTypes__Enum::Phantom) {
                                if (AnimatedButton("Force Vanish"))
                                {
                                    for (auto p : selectedPlayers) {
                                        auto validPlayer = p.validate();
                                        if (IsInGame()) {
                                            State.rpcQueue.push(new RpcVanish(validPlayer.get_PlayerControl()));
                                        }
                                        else if (IsInLobby()) {
                                            State.lobbyRpcQueue.push(new RpcVanish(validPlayer.get_PlayerControl()));
                                        }
                                    }
                                }
                                ImGui::SameLine();
                                if (AnimatedButton("Force Appear"))
                                {
                                    for (auto p : selectedPlayers) {
                                        auto validPlayer = p.validate();
                                        if (IsInGame()) {
                                            State.rpcQueue.push(new RpcVanish(validPlayer.get_PlayerControl(), true));
                                        }
                                        else if (IsInLobby()) {
                                            State.lobbyRpcQueue.push(new RpcVanish(validPlayer.get_PlayerControl(), true));
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if ((IsHost() || !State.SafeMode) && State.InMeeting && selectedPlayers.size() == 1) {
                        if (AnimatedButton("Vote Off")) {
                            State.VoteOffPlayerId = selectedPlayer.get_PlayerControl()->fields.PlayerId;
                            for (auto player : GetAllPlayerControl()) {
                                State.rpcQueue.push(new RpcVotePlayer(player, selectedPlayer.get_PlayerControl()));
                            }
                        }
                        /*ImGui::SameLine();
                        if (AnimatedButton("Force Overrule")) {
                            State.taskRpcQueue.push(new RpcOverrulePlayer(selectedPlayer.get_PlayerControl()));
                            State.taskRpcQueue.push(new RpcEndMeeting());
                        }*/
                    }

                    if (!selectedPlayer.is_LocalPlayer() && selectedPlayers.size() == 1) {
                        if (AnimatedButton("Teleport To")) {
                            if (IsInGame())
                                State.rpcQueue.push(new RpcSnapTo(GetTrueAdjustedPosition(selectedPlayer.get_PlayerControl())));
                            else if (IsInLobby())
                                State.lobbyRpcQueue.push(new RpcSnapTo(GetTrueAdjustedPosition(selectedPlayer.get_PlayerControl())));
                        }
                    }
                    if (!selectedPlayer.is_LocalPlayer() && !State.SafeMode) {
                        ImGui::SameLine();
                        if (AnimatedButton("Teleport To You")) {
                            for (auto p : selectedPlayers) {
                                if (IsInGame())
                                    State.rpcQueue.push(new RpcForceSnapTo(p.validate().get_PlayerControl(), GetTrueAdjustedPosition(*Game::pLocalPlayer)));
                                else if (IsInLobby())
                                    State.lobbyRpcQueue.push(new RpcForceSnapTo(p.validate().get_PlayerControl(), GetTrueAdjustedPosition(*Game::pLocalPlayer)));
                            }
                        }
                    }

                    if ((IsInGame() || IsInLobby()) && selectedPlayer.has_value() && selectedPlayers.size() == 1)
                    {
                        if (State.ActiveAttach && selectedPlayer.has_value() && (State.playerToAttach.equals(State.selectedPlayer) || selectedPlayer.is_LocalPlayer())) {
                            if (AnimatedButton(State.AprilFoolsMode ? "Stop Backshotting" : "Stop Attaching")) {
                                State.playerToAttach = {};
                                State.ActiveAttach = false;
                            }
                        }
                        else {
                            if (!selectedPlayer.is_LocalPlayer() && AnimatedButton(State.AprilFoolsMode ? "Backshot To" : "Attach To")) {
                                State.playerToAttach = State.selectedPlayer;
                                State.ActiveAttach = true;
                            }
                        }
                    }

                    if ((IsHost() || !State.SafeMode) && selectedPlayers.size() == 1 && !selectedPlayer.get_PlayerData()->fields.IsDead) {
                        if (AnimatedButton("Turn into Ghost"))
                        {
                            if (PlayerIsImpostor(selectedPlayer.get_PlayerData())) {
                                if (IsInGame())
                                    State.rpcQueue.push(new RpcSetRole(selectedPlayer.get_PlayerControl(), RoleTypes__Enum::ImpostorGhost));
                                else if (IsInLobby())
                                    State.lobbyRpcQueue.push(new RpcSetRole(selectedPlayer.get_PlayerControl(), RoleTypes__Enum::ImpostorGhost));
                            }
                            else {
                                if (IsInGame())
                                    State.rpcQueue.push(new RpcSetRole(selectedPlayer.get_PlayerControl(), RoleTypes__Enum::CrewmateGhost));
                                else if (IsInLobby())
                                    State.lobbyRpcQueue.push(new RpcSetRole(selectedPlayer.get_PlayerControl(), RoleTypes__Enum::CrewmateGhost));
                            }
                        }
                    }

                    if ((IsHost() || !State.SafeMode) && (IsInGame() || IsInLobby()) && selectedPlayers.size() == 1) {
                        if (!IsInMultiplayerGame() || !selectedPlayer.get_PlayerControl()->fields.roleAssigned)
                        {
                        if (CustomListBoxIntColored("Select Role", &State.FakeRole, FAKEROLES, 100.0f * State.dpiScale, ImVec4(0.f, 0.f, 0.f, 0.f), 0, "", FAKEROLE_NAMES_COLOR, IM_ARRAYSIZE(FAKEROLE_NAMES_COLOR))) {
                                // for some reason, detective is 12 (0x0c) instead of 11, viper is 18 (0x12) instead of 12, and influencer (SpiritGuide) is 21 (0x15) instead of 20
                                if (State.FakeRole >= 14) State.FakeRoleId = State.FakeRole + 7;
                                else if (State.FakeRole >= 12) State.FakeRoleId = State.FakeRole + 6;
                                else if (State.FakeRole == 11) State.FakeRoleId = State.FakeRole + 1;
                                else State.FakeRoleId = State.FakeRole;
                                State.Save();
                            }
                            ImGui::SameLine();
                            if (AnimatedButton("Set Role"))
                            {
                                if (IsInGame())
                                    State.rpcQueue.push(new RpcSetRole(selectedPlayer.get_PlayerControl(), RoleTypes__Enum(State.FakeRoleId)));
                                else if (IsInLobby())
                                    State.lobbyRpcQueue.push(new RpcSetRole(selectedPlayer.get_PlayerControl(), RoleTypes__Enum(State.FakeRoleId)));
                            }
                        }
                        else {
                            static int ghostRole = 0;
                            if (CustomListBoxIntColored("Select Role", &ghostRole, GHOSTROLES, 100.0f * State.dpiScale, ImVec4(0.f, 0.f, 0.f, 0.f), 0, "", GHOSTROLE_NAMES_COLOR, IM_ARRAYSIZE(GHOSTROLE_NAMES_COLOR)))
                                State.Save();
                            ImGui::SameLine();
                            if (AnimatedButton("Set Role"))
                            {
                                auto roleType = RoleTypes__Enum::CrewmateGhost;
                                switch (ghostRole) {
                                case 0:
                                    roleType = RoleTypes__Enum::GuardianAngel;
                                    break;
                                case 1:
                                    roleType = RoleTypes__Enum::CrewmateGhost;
                                    break;
                                case 2:
                                    roleType = RoleTypes__Enum::ImpostorGhost;
                                    break;
                                case 3:
                                    roleType = RoleTypes__Enum::SpiritGuide;
                                    break;
                                }
                                if (IsInGame())
                                    State.rpcQueue.push(new RpcSetRole(selectedPlayer.get_PlayerControl(), roleType));
                                else if (IsInLobby())
                                    State.lobbyRpcQueue.push(new RpcSetRole(selectedPlayer.get_PlayerControl(), roleType));
                            }
                        }
                    }

                    if (GameOptions().GetBool(BoolOptionNames__Enum::VisualTasks)) {
                        if (!State.SafeMode && AnimatedButton("Set Scanner")) {
                            for (auto p : selectedPlayers) {
                                if (IsInGame())
                                    State.rpcQueue.push(new RpcForceScanner(p.validate().get_PlayerControl(), true));
                                else if (IsInLobby())
                                    State.lobbyRpcQueue.push(new RpcForceScanner(p.validate().get_PlayerControl(), true));
                            }
                        }
                        ImGui::SameLine();
                        if (!State.SafeMode && AnimatedButton("Stop Scanner")) {
                            for (auto p : selectedPlayers) {
                                if (IsInGame())
                                    State.rpcQueue.push(new RpcForceScanner(p.validate().get_PlayerControl(), false));
                                else if (IsInLobby())
                                    State.lobbyRpcQueue.push(new RpcForceScanner(p.validate().get_PlayerControl(), false));
                            }
                        }
                    }
                    ImGui::NewLine();
                    if (IsHost() && (IsInGame() || IsInLobby())) {
                    CustomListBoxIntColored(" ", &forcedColor, COLORS, 85.0f * State.dpiScale, ImVec4(0.f, 0.f, 0.f, 0.f), 0, "", COLOR_NAMES_COLOR, IM_ARRAYSIZE(COLOR_NAMES_COLOR));
                        ImGui::SameLine();
                        if (AnimatedButton("Force Color"))
                        {
                            if (IsInGame())
                                State.rpcQueue.push(new RpcForceColor(selectedPlayer.get_PlayerControl(), forcedColor));
                            else if (IsInLobby())
                                State.lobbyRpcQueue.push(new RpcForceColor(selectedPlayer.get_PlayerControl(), forcedColor));
                        }

                        if (AnimatedButton("Randomize Color"))
                        {
                            if (IsInGame())
                                State.rpcQueue.push(new RpcForceColor(selectedPlayer.get_PlayerControl(), GetRandomColorId()));
                            else if (IsInLobby())
                                State.lobbyRpcQueue.push(new RpcForceColor(selectedPlayer.get_PlayerControl(), GetRandomColorId()));
                        }

                        auto pid = selectedPlayer.get_PlayerData()->fields.PlayerId;
                        bool isColorCycling = std::find(State.ColorCycledPlayers.begin(), State.ColorCycledPlayers.end(), pid) != State.ColorCycledPlayers.end();
                        if (AnimatedButton(isColorCycling ? "Stop Cycling Colors" : "Cycle Colors")) {
                            if (isColorCycling) {
                                State.ColorCycledPlayers.erase(std::remove(State.ColorCycledPlayers.begin(), State.ColorCycledPlayers.end(), pid), State.ColorCycledPlayers.end());
                            }
                            else
                                State.ColorCycledPlayers.push_back(pid);
                        }

                        ImGui::Text("Change cycling interval in Self > Randomizers!");
                    }

                    if (IsHost() && (IsInGame() || IsInLobby()) && !selectedPlayer.is_LocalPlayer() && selectedPlayers.size() == 1) {
                        auto pid = selectedPlayer.get_PlayerData()->fields.PlayerId;
                    }

                    if (State.selectedPlayers.size() == 1) {
                        if ((IsInGame() || IsInLobby()) && !selectedPlayer.is_Disconnected() && !selectedPlayer.is_LocalPlayer())
                        {
                            if (State.playerToWhisper.equals(State.selectedPlayer) && State.activeWhisper) {
                                if (AnimatedButton("Stop Whispering To")) {
                                    State.playerToWhisper = {};
                                    State.activeWhisper = false;
                                }
                            }
                            else {
                                if (AnimatedButton("Whisper To")) {
                                    State.playerToWhisper = State.selectedPlayer;
                                    State.activeWhisper = true;
                                }
                            }
                        }
                        //we have to send these rpc messages as ourselves since anticheat only allows you to send rpcs with your own net id
                        /*if (AnimatedButton(!State.SafeMode ? "Force AUM Detection" : "Fake AUM Detection")) {
                            if (IsInGame()) State.rpcQueue.push(new RpcForceDetectAum(selectedPlayer, !State.SafeMode));
                            if (IsInLobby()) State.lobbyRpcQueue.push(new RpcForceDetectAum(selectedPlayer, !State.SafeMode));
                        }
                        ImGui::SameLine();*/
                        if (!State.SafeMode) {
                            static std::string scMessage = "";
                            if (AnimatedButton("Force VLX Chat")) {
                                if (IsInGame()) State.rpcQueue.push(new RpcForceSickoChat(selectedPlayer, scMessage, !State.SafeMode));
                                if (IsInLobby()) State.lobbyRpcQueue.push(new RpcForceSickoChat(selectedPlayer, scMessage, !State.SafeMode));
                            }

                            InputString("SC Message", &scMessage);
                        }


                        if (!State.SafeMode && (IsInGame() || IsInLobby()) && !selectedPlayer.is_Disconnected() && !selectedPlayer.is_LocalPlayer())
                        {
                            if (State.playerToChatAs.equals(State.selectedPlayer) && State.activeChatSpoof) {
                                if (AnimatedButton("Stop Chatting As")) {
                                    State.playerToChatAs = {};
                                    State.activeChatSpoof = false;
                                }
                            }
                            else {
                                if (AnimatedButton("Chat As")) {
                                    State.playerToChatAs = State.selectedPlayer;
                                    State.activeChatSpoof = true;
                                }
                            }
                        }
                    }
                }
                if (openInfo && selectedPlayer.has_value() && selectedPlayers.size() == 1 && !selectedPlayer.get_PlayerControl()->fields.notRealPlayer) {
                    ImGui::Dummy(ImVec2(3, 3) * State.dpiScale);
                    if (AnimatedButton("Steal Data")) {
                        State.StealedPUID = convert_from_string(selectedPlayer.get_PlayerData()->fields.Puid);
                        State.StealedFC = convert_from_string(selectedPlayer.get_PlayerData()->fields.FriendCode);
                        State.Save();
                    }
                    ImGui::Dummy(ImVec2(15, 15) * State.dpiScale);
                    InputString("PUID", &State.StealedPUID);
                    ImGui::Dummy(ImVec2(2, 2) * State.dpiScale);
                    InputString("Friend Code", &State.StealedFC);
                    ImGui::Dummy(ImVec2(10, 10) * State.dpiScale);
                    {
                        if (convert_from_string(selectedPlayer.get_PlayerData()->fields.Puid) != "" && AnimatedButton("Copy PUID"))
                            ClipboardHelper_PutClipboardString(selectedPlayer.get_PlayerData()->fields.Puid, NULL);
                    }
                    ImGui::SameLine();
                    {
                        if (convert_from_string(selectedPlayer.get_PlayerData()->fields.FriendCode) != "" && AnimatedButton("Copy Friend Code"))
                            ClipboardHelper_PutClipboardString(selectedPlayer.get_PlayerData()->fields.FriendCode, NULL);
                    }

                    static int reportReason = 0;
                    if (AnimatedButton("Report Player")) {
                        if (IsInGame()) State.rpcQueue.push(new ReportPlayer(selectedPlayer.get_PlayerControl(), (ReportReasons__Enum)reportReason));
                        if (IsInLobby()) State.lobbyRpcQueue.push(new ReportPlayer(selectedPlayer.get_PlayerControl(), (ReportReasons__Enum)reportReason));
                    }

                    ImGui::Text("Reason");

                    const std::vector<const char*> REPORTREASONS = { "Inappropriate Name", "Inappropriate Chat", "Cheating/Hacking", "Harassment/Misconduct" };

                    CustomListBoxInt("  ", &reportReason, REPORTREASONS);

                    ImGui::Dummy(ImVec2(10, 10) * State.dpiScale);

                    if (State.TempBanEnabled && IsHost() && !selectedPlayer.is_LocalPlayer()) {
                        static int banDays = 0, banHours = 0, banMinutes = 0, banSeconds = 0;

                        ImGui::PushItemWidth(200);
                        ImGui::InputInt("Days", &banDays);     banDays = std::max<int>(0, banDays);
                        ImGui::InputInt("Hours", &banHours);   banHours = std::clamp(banHours, 0, 23);
                        ImGui::InputInt("Minutes", &banMinutes); banMinutes = std::clamp(banMinutes, 0, 59);
                        ImGui::InputInt("Seconds", &banSeconds); banSeconds = std::clamp(banSeconds, 0, 59);
                        ImGui::PopItemWidth();

                        if (ImGui::Button("Confirm TempBan")) {
                            std::string targetFC = convert_from_string(selectedPlayer.get_PlayerData()->fields.FriendCode);
                            std::string selfFC = convert_from_string((*Game::pLocalPlayer)->fields.FriendCode);
                            if (!targetFC.empty() && targetFC != selfFC) {
                                int64_t totalSeconds = 0;
                                totalSeconds += static_cast<int64_t>(banDays) * 86400;
                                totalSeconds += static_cast<int64_t>(banHours) * 3600;
                                totalSeconds += static_cast<int64_t>(banMinutes) * 60;
                                totalSeconds += static_cast<int64_t>(banSeconds);

                                if (totalSeconds > State.MAX_BAN_SECONDS) {
                                    totalSeconds = State.MAX_BAN_SECONDS;
                                }

                                if (totalSeconds > 0) {
                                    auto now = std::chrono::system_clock::now();
                                    auto banEnd = now + std::chrono::seconds(totalSeconds);

                                    State.TempBannedFCs[targetFC] = banEnd;
                                    State.Save();

                                    auto p = selectedPlayer.get_PlayerControl();
                                    // Main & first ban (new temp-banned user):
                                    if (IsInGame()) {
                                        State.rpcQueue.push(new PunishPlayer(p, false));
                                    }
                                    if (IsInLobby()) {
                                        State.lobbyRpcQueue.push(new PunishPlayer(p, false));
                                    }
                                }
                            }
                        }
                    }

                    if (IsHost() && !selectedPlayer.is_LocalPlayer()) {
                        ImGui::Dummy(ImVec2(0, 8) * State.dpiScale);
                        std::string targetFC = convert_from_string(selectedPlayer.get_PlayerData()->fields.FriendCode);
                        if (targetFC.empty()) {
                            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "No friend code available for this player.");
                        }
                        else if (State.Mod_RoleNames.size() <= 1) { 
                            ImGui::TextDisabled("No roles created yet - add some in the Host tab.");
                        }
                        else {
                            static int addRoleIndex = 0; // index into the assignable roles only (excludes "Everyone")
                            int assignableCount = (int)State.Mod_RoleNames.size() - 1;
                            addRoleIndex = std::clamp(addRoleIndex, 0, assignableCount - 1);
                            std::vector<const char*> roleVector(assignableCount, nullptr);
                            for (int i = 0; i < assignableCount; i++) roleVector[i] = State.Mod_RoleNames[i + 1].c_str();

                            ImGui::Text("Add Role:");
                            ImGui::SameLine();
                            CustomListBoxInt("  ", &addRoleIndex, roleVector, 130.0f * State.dpiScale, ImVec4(0, 0, 0, 0), ImGuiComboFlags_None);
                            ImGui::SameLine();
                            if (AnimatedButton("Add##PlayerRole")) {
                                SetFriendCodeInRole(targetFC, addRoleIndex + 1, true);
                            }

                            auto currentRoles = GetFriendCodeRoleIndices(targetFC);
                            if (!currentRoles.empty()) {
                                ImGui::Text("Current Roles:");
                                ImVec4 themeCol = State.RgbMenuTheme ? State.RgbColor : (State.GradientMenuTheme ? State.MenuGradientColor : State.MenuThemeColor);
                                for (int roleIdx : currentRoles) {
                                    if (roleIdx < 0 || roleIdx >= (int)State.Mod_RoleNames.size()) continue;
                                    ImGui::PushStyleColor(ImGuiCol_Button, themeCol);
                                    if (AnimatedButton((State.Mod_RoleNames[roleIdx] + " x##playerrole" + std::to_string(roleIdx)).c_str())) {
                                        SetFriendCodeInRole(targetFC, roleIdx, false);
                                    }
                                    ImGui::PopStyleColor(1);
                                    ImGui::SameLine();
                                }
                                ImGui::NewLine();
                            }
                        }
                    }

                    if ((IsHost() || !State.SafeMode) && (IsInGame() || IsInLobby()) && selectedPlayers.size() == 1) {
                        ImGui::NewLine(); //force a new line

                        if (!State.SafeMode) {
                            if (InputString("Username", &forcedName)) {
                                State.Save();
                            }
                            if (AnimatedButton("Force Name"))
                            {
                                if (IsInGame())
                                    State.rpcQueue.push(new RpcForceName(selectedPlayer.get_PlayerControl(), forcedName));
                                else if (IsInLobby())
                                    State.lobbyRpcQueue.push(new RpcForceName(selectedPlayer.get_PlayerControl(), forcedName));
                            }
                        }

                        if (!State.SafeMode && (IsInGame() || IsInLobby())) {
                            static int level = 0;
                            ImGui::InputInt("Level", &level);
                            if (AnimatedButton("Force Level")) {
                                if (IsInGame())
                                    State.rpcQueue.push(new RpcSetLevel(selectedPlayer.get_PlayerControl(), level));
                                else if (IsInLobby())
                                    State.lobbyRpcQueue.push(new RpcSetLevel(selectedPlayer.get_PlayerControl(), level));
                            }
                        }
                    }
                }
                ImGui::EndChild();
                ImGui::EndChild();
            }
        }

        if (openPlayer || State.selectedPlayers.size() == 0 || IsInLobby()) { //clear murder/suicide loops
            State.murderLoop = false;
            State.suicideLoop = false;
            State.murderCount = 0;
            State.suicideCount = 0;
        }

#pragma region Blinking tab? No more!
        /*static int blinkDelay = 0;
        static bool isBlinking = false;
        if (State.BlinkPlayersTab && !isBlinking) {
            blinkDelay = 5;
            isBlinking = true;
        }
        if (isBlinking && State.BlinkPlayersTab) {
            if (blinkDelay >= 0) blinkDelay--;
            if (blinkDelay == 0) {
                State.BlinkPlayersTab = false;
                isBlinking = false;
            }
        }*/
#pragma endregion
    }
}
