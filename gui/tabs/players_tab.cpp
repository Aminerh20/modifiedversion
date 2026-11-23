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
#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <deque>
#include <string>
#include <utility>
#include "_rpc.h"
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
    static constexpr int matchCycleFourImpostorIntervalMs = 90;
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
        FarmingConfirmations,
        DrainFarmingPrompt,
        SoloLobbyKeepAlive,
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
        bool disconnected_during_farming_round = false;
        bool was_impostor_this_round = false;
        bool farmed_this_round = false;
        bool demoted_to_ghost_this_round = false;
        std::string puid;
        std::string friend_code;
        std::string name;
    };

    struct XpControllerChatMessage {
        std::string text;
        std::chrono::seconds delayAfterSend{0};
        bool startsDrainFarmingPromptWindow = false;
    };

    struct XpLevelBaseline {
        uint32_t originalLevel = 0;
        uint32_t lastLevel = 0;
        uint32_t highestObservedLevel = 0;
        bool patchDetected = false;
        bool rejoinRequested = false;
        std::string name;
    };

    enum class XpProgressMode {
        LevelTracking = 0,
        LevelWithChatFallback = 1,
        LegacyChat = 2,
    };

    struct XpControllerSnapshot {
        XpControllerPhase phase = XpControllerPhase::STOPPED;
        XpControllerStage stage = XpControllerStage::PreparingRound;
        bool valid = false;
        bool deadlineIsMax = false;
        int64_t deadlineRemainingMs = 0;
        int64_t transitionRemainingMs = 0;
        int progressMode = static_cast<int>(XpProgressMode::LevelTracking);
        std::chrono::steady_clock::time_point roundStartedAt;
        bool roundShieldFailureWaived = false;
        int drainRoundsCompleted = 0;
        int drainRoundsTarget = 0;
        bool drainStopRequested = false;
        size_t drainStopEligibleCount = 0;
        size_t drainStopRequiredVotes = 0;
        std::unordered_set<Game::PlayerId> drainStopVoterIds;
        bool drainFarmingPromptResponse = false;
        bool drainFarmingPromptResponseWindowStarted = false;
        std::vector<Game::PlayerId> drainFarmingBonusPlayerIds;
        bool drainReturnAfterFarmingBreak = false;
        bool drainBonusBatchPendingStart = false;
        bool drainFarmingPromptGracePending = false;
        bool farmingConfirmationPending = false;
        Game::PlayerId farmingRotationCursor = 0;
        bool hasFarmingRotationCursor = false;
        std::vector<Game::PlayerId> roundImpostorIds;
        std::vector<std::string> roundImpostorNames;
        std::unordered_set<Game::PlayerId> completedPlayerIds;
        std::unordered_set<Game::PlayerId> retryPlayerIds;
        std::unordered_map<Game::PlayerId, bool> confirmationResponses;
        std::unordered_map<Game::PlayerId, uint32_t> roundLevelBaselines;
        bool roundShieldFailure = false;
        bool noActiveFarmingImpostors = false;
        bool roundFailureDetectedBeforeFiftyPercent = false;
        std::unordered_set<Game::PlayerId> ghostedBeforeFiftyPercentIds;
        bool allowFourImpostors = true;
        std::unordered_set<Game::PlayerId> drainFarmingMeUsedPlayerIds;
        bool legacyFarmingRoundFailed = false;
        std::unordered_map<Game::PlayerId, int> legacyFarmingFailureStreak;
        std::unordered_set<Game::PlayerId> deferredFarmingRetryIds;
        std::unordered_set<Game::PlayerId> deferredFarmingRetryUsedIds;
        bool deferredFarmingRetryActive = false;
        std::unordered_map<Game::PlayerId, int> protectedKillCounts;
        std::unordered_set<Game::PlayerId> ghostedImpostorIds;
        bool farmingSafetyWarningsPending = false;
        bool soloKeepAliveRound = false;
        bool soloKeepAliveNeedsRoundSetup = false;
        XpControllerPhase soloResumePhase = XpControllerPhase::STOPPED;
        XpControllerStage soloResumeStage = XpControllerStage::PreparingRound;
        bool soloResumeDeadlineIsMax = false;
        int64_t soloResumeDeadlineRemainingMs = 0;
    };

    static XpControllerPhase xpControllerPhase = XpControllerPhase::STOPPED;
    static XpControllerStage xpControllerStage = XpControllerStage::PreparingRound;
    static std::unordered_map<Game::PlayerId, XpTrackedPlayer> xpTrackedPlayers;
    static std::vector<Game::PlayerId> xpRoundImpostorIds;
    static std::vector<std::string> xpRoundImpostorNames;
    static std::unordered_map<Game::PlayerId, bool> xpFarmingConfirmationResponses;
    static std::unordered_set<Game::PlayerId> xpFarmingCompletedPlayerIds;
    static std::unordered_set<Game::PlayerId> xpFarmingRetryPlayerIds;
    static std::unordered_map<Game::PlayerId, int> xpLegacyFarmingFailureStreak;
    static std::unordered_set<Game::PlayerId> xpDeferredFarmingRetryIds;
    static std::unordered_set<Game::PlayerId> xpDeferredFarmingRetryUsedIds;
    static bool xpDeferredFarmingRetryActive = false;
    static std::unordered_map<Game::PlayerId, int> xpRoundProtectedKillCounts;
    static std::unordered_set<Game::PlayerId> xpRoundGhostedImpostorIds;
    static bool xpFarmingSafetyWarningsPending = false;
    static size_t xpPendingGhostRoleQueueTicks = 0;
    static std::chrono::steady_clock::time_point xpControllerDeadline;
    static std::chrono::steady_clock::time_point xpLastRosterCheck;
    static std::chrono::steady_clock::time_point xpTransitionGraceDeadline;
    static std::chrono::steady_clock::time_point xpHostAuthorityLostAt;
    static std::chrono::steady_clock::time_point xpPostChatDeadline;
    static std::chrono::steady_clock::time_point xpNextChatSendAt;
    static std::chrono::steady_clock::time_point xpRoundChatBatchStartedAt;
    static std::chrono::milliseconds xpRoundChatWait{ 0 };
    static std::deque<XpControllerChatMessage> xpPendingChatMessages;
    static std::deque<std::string> xpPendingInGameChatMessages;
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
    static bool xpControllerPreviousNoGameEnd = false;
    static bool xpControllerOwnsNoGameEnd = false;
    static bool xpFarmingConfirmationMode = true;
    static bool xpDrainUntilStopMode = true;
    static XpProgressMode xpProgressMode = XpProgressMode::LevelTracking;
    static std::unordered_map<Game::PlayerId, uint32_t> xpRoundLevelBaselines;
    static bool xpRoundShieldFailure = false;
    static bool xpRoundShieldFailureWaived = false;
    static bool xpNoActiveFarmingImpostors = false;
    static bool xpRoundFailureDetectedBeforeFiftyPercent = false;
    static std::unordered_set<Game::PlayerId> xpRoundGhostedBeforeFiftyPercentIds;
    static bool xpAllowFourImpostors = true;
    static std::chrono::steady_clock::time_point xpRoundStartedAt;
    static std::unordered_set<Game::PlayerId> xpDrainFarmingMeUsedPlayerIds;
    static bool xpLegacyFarmingRoundFailed = false;
    static bool xpDrainFarmingPromptMode = true;
    static bool xpDrainFarmingPromptResponse = false;
    static bool xpDrainFarmingPromptResponseWindowStarted = false;
    static std::vector<Game::PlayerId> xpDrainFarmingBonusPlayerIds;
    static bool xpDrainReturnAfterFarmingBreak = false;
    static bool xpDrainBonusBatchPendingStart = false;
    static bool xpDrainFarmingPromptGracePending = false;
    static bool xpSoloKeepAliveRound = false;
    static bool xpSoloKeepAliveNeedsRoundSetup = false;
    static XpControllerPhase xpSoloResumePhase = XpControllerPhase::STOPPED;
    static XpControllerStage xpSoloResumeStage = XpControllerStage::PreparingRound;
    static bool xpSoloResumeDeadlineIsMax = false;
    static std::chrono::steady_clock::time_point xpSoloResumeDeadline;
    static std::chrono::steady_clock::time_point xpSoloKeepAliveStartedAt;
    static bool xpFarmingConfirmationPending = false;
    static bool xpDrainStopRequested = false;
    static size_t xpDrainStopEligibleCount = 0;
    static size_t xpDrainStopRequiredVotes = 0;
    static std::unordered_set<Game::PlayerId> xpDrainStopVoterIds;
    static bool xpDrainRejoinGracePending = false;
    static bool xpDrainRejoinNoticePendingStart = false;
    static bool xpDrainMeetingAnnouncementPending = false;
    static bool xpControllerPaused = false;
    static bool xpPauseDeadlineIsMax = false;
    static int64_t xpPauseDeadlineRemainingMs = 0;
    static XpControllerSnapshot xpControllerSnapshot;
    static bool xpLevelRecorderRunning = false;
    static std::unordered_map<Game::PlayerId, XpLevelBaseline> xpLevelBaselines;
    static std::chrono::steady_clock::time_point xpLevelRejoinDeadline;
    static Game::PlayerId xpFarmingRotationCursor = 0;
    static bool xpHasFarmingRotationCursor = false;
    static constexpr auto xpRoundPrepareDelay = std::chrono::seconds(1);
    static constexpr auto xpFarmingLobbyGraceDuration = std::chrono::seconds(20);
    static constexpr auto xpDrainFarmingPromptResponseDuration = std::chrono::seconds(15);
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
    static constexpr auto xpSoloKeepAliveInterval = std::chrono::minutes(5);

    static void BeginXpFarmingRound();
    static void BeginXpRolloverRound();
    static void SetShieldState(Game::PlayerId playerId, bool enabled);
    static void RestoreMatchCycleProtectionSettings();
    static void RecordConnectedXpLevels();
    static void CheckXpLevelPatchAfterRound();

    static bool IsMatchCycleHost(const PlayerControl* player) {
        return player != nullptr
            && Game::pLocalPlayer != nullptr
            && *Game::pLocalPlayer != nullptr
            && player->fields.PlayerId == (*Game::pLocalPlayer)->fields.PlayerId;
    }

    static int GetMatchCycleTeamCap(size_t nonHostParticipants) {
        if (nonHostParticipants >= 8 && xpAllowFourImpostors) return 4;
        if (nonHostParticipants >= 6) return 3;
        if (nonHostParticipants >= 4) return 2;
        if (nonHostParticipants >= 1) return 1;
        return 0;
    }

    static std::chrono::milliseconds GetMatchCycleUpdateInterval() {
        return std::chrono::milliseconds(
            xpRoundImpostorIds.size() >= 4
                ? matchCycleFourImpostorIntervalMs
                : matchCycleProtectionIntervalMs);
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
        case XpControllerStage::FarmingConfirmations: return "FarmingConfirmations";
        case XpControllerStage::DrainFarmingPrompt: return "DrainFarmingPrompt";
        case XpControllerStage::SoloLobbyKeepAlive: return "SoloLobbyKeepAlive";
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

    static bool QueueXpControllerChat(
        const std::string& message,
        std::chrono::seconds delayAfterSend = std::chrono::seconds::zero(),
        bool startsDrainFarmingPromptWindow = false) {
        if (!CanSendXpControllerChat()) {
            Toasts::AddToast("XP Controller", "Lobby chat is unavailable; status message was not sent.", ImVec4(1.f, 0.3f, 0.3f, 1.f));
            return false;
        }

        const std::string chatMessage = SanitizeXpControllerChatText(message);
        if (chatMessage.empty()) return false;

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
                end == chatMessage.size() && startsDrainFarmingPromptWindow,
            });
            ++queuedChunks;
            start = end;
            while (start < chatMessage.size() && chatMessage[start] == ' ') ++start;
        }
        xpRoundChatMessageCount += queuedChunks;
        if (queuedChunks > 0) xpRoundChatPostDelaySeconds += delayAfterSend.count();
        return queuedChunks > 0;
    }

    static void TickXpControllerChat() {
        if (!xpDrainMeetingAnnouncementPending
            && !xpPendingInGameChatMessages.empty()
            && IsInGame()
            && State.ChatCooldown >= 3.f
            && CanSendXpControllerChat()) {
            std::string message = std::move(xpPendingInGameChatMessages.front());
            xpPendingInGameChatMessages.pop_front();
            State.rpcQueue.push(new RpcSendChat(*Game::pLocalPlayer, message));
            State.MessageSent = true;
            LogXpControllerEvent(std::format("Queued in-game acknowledgment: '{}'.", message));
            return;
        }
        if (xpDrainMeetingAnnouncementPending && IsInGame() && State.InMeeting
            && State.ChatCooldown >= 3.f && CanSendXpControllerChat()) {
            const std::string meetingText =
                xpProgressMode == XpProgressMode::LegacyChat
                ? std::format("Draining round {}.", xpDrainRoundsCompleted + 1)
                : std::format("Drain round {}. Meeting stays open while levels are tracked.",
                    xpDrainRoundsCompleted + 1);
            State.rpcQueue.push(new RpcSendChat(*Game::pLocalPlayer, meetingText));
            State.MessageSent = true;
            xpDrainMeetingAnnouncementPending = false;
            if (xpProgressMode == XpProgressMode::LegacyChat) {
                xpPendingInGameChatMessages.push_back(std::format(
                    "Type STOP to stop auto-drain. Majority needed: {}/{}.",
                    xpDrainStopRequiredVotes, xpDrainStopEligibleCount));
            }
            LogXpControllerEvent(std::format(
                "Queued drain round {} meeting announcement.",
                xpDrainRoundsCompleted + 1));
            return;
        }
        if (xpPendingChatMessages.empty() || !IsInLobby()) return;

        const auto now = std::chrono::steady_clock::now();
        if (now < xpNextChatSendAt || State.ChatCooldown < 3.f) return;
        if (!CanSendXpControllerChat()) return;

        auto queuedMessage = std::move(xpPendingChatMessages.front());
        xpPendingChatMessages.pop_front();
        State.lobbyRpcQueue.push(new RpcSendChat(
            *Game::pLocalPlayer, queuedMessage.text, nullptr,
            queuedMessage.startsDrainFarmingPromptWindow));
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
            const bool completed = xpFarmingCompletedPlayerIds.contains(playerId)
                || tracked.has_completed_objective;
            if (completed) {
                farmedThisRound.push_back(tracked.name);
            }
            if (!completed) stillNeedFarming.push_back(tracked.name);
        }
        BroadcastXpControllerPlayerList("FARMED", farmedThisRound);
        BroadcastXpControllerPlayerList("NEED", stillNeedFarming, xpFarmingAnnouncementDelay);
    }

    template <typename Map>
    static void MigrateXpPlayerMapKey(
        Map& values,
        Game::PlayerId oldPlayerId,
        Game::PlayerId newPlayerId) {
        const auto oldEntry = values.find(oldPlayerId);
        if (oldEntry == values.end()) return;
        auto value = std::move(oldEntry->second);
        values.erase(oldEntry);
        values.insert_or_assign(newPlayerId, std::move(value));
    }

    template <typename Set>
    static void MigrateXpPlayerSetKey(
        Set& values,
        Game::PlayerId oldPlayerId,
        Game::PlayerId newPlayerId) {
        if (values.erase(oldPlayerId) != 0) values.insert(newPlayerId);
    }

    static void MigrateXpPlayerVectorId(
        std::vector<Game::PlayerId>& values,
        Game::PlayerId oldPlayerId,
        Game::PlayerId newPlayerId) {
        std::replace(values.begin(), values.end(), oldPlayerId, newPlayerId);
        std::unordered_set<Game::PlayerId> seen;
        values.erase(
            std::remove_if(values.begin(), values.end(), [&seen](Game::PlayerId playerId) {
                return !seen.insert(playerId).second;
            }),
            values.end());
    }

    static void MigrateXpPlayerId(
        Game::PlayerId oldPlayerId,
        Game::PlayerId newPlayerId) {
        MigrateXpPlayerMapKey(xpTrackedPlayers, oldPlayerId, newPlayerId);
        MigrateXpPlayerMapKey(xpFarmingConfirmationResponses, oldPlayerId, newPlayerId);
        MigrateXpPlayerMapKey(xpLegacyFarmingFailureStreak, oldPlayerId, newPlayerId);
        MigrateXpPlayerMapKey(xpRoundProtectedKillCounts, oldPlayerId, newPlayerId);
        MigrateXpPlayerMapKey(xpRoundLevelBaselines, oldPlayerId, newPlayerId);
        MigrateXpPlayerMapKey(xpLevelBaselines, oldPlayerId, newPlayerId);

        MigrateXpPlayerSetKey(xpFarmingCompletedPlayerIds, oldPlayerId, newPlayerId);
        MigrateXpPlayerSetKey(xpFarmingRetryPlayerIds, oldPlayerId, newPlayerId);
        MigrateXpPlayerSetKey(xpDeferredFarmingRetryIds, oldPlayerId, newPlayerId);
        MigrateXpPlayerSetKey(xpDeferredFarmingRetryUsedIds, oldPlayerId, newPlayerId);
        MigrateXpPlayerSetKey(xpRoundGhostedImpostorIds, oldPlayerId, newPlayerId);
        MigrateXpPlayerSetKey(xpRoundGhostedBeforeFiftyPercentIds, oldPlayerId, newPlayerId);
        MigrateXpPlayerSetKey(xpDrainStopVoterIds, oldPlayerId, newPlayerId);
        MigrateXpPlayerSetKey(xpDrainFarmingMeUsedPlayerIds, oldPlayerId, newPlayerId);

        MigrateXpPlayerVectorId(xpRoundImpostorIds, oldPlayerId, newPlayerId);
        MigrateXpPlayerVectorId(xpRolesPendingAssignment, oldPlayerId, newPlayerId);
        MigrateXpPlayerVectorId(xpDrainFarmingBonusPlayerIds, oldPlayerId, newPlayerId);
        if (xpHasFarmingRotationCursor && xpFarmingRotationCursor == oldPlayerId) {
            xpFarmingRotationCursor = newPlayerId;
        }
        LogXpControllerEvent(std::format(
            "Reconnected participant identity: migrated state from PlayerId={} to PlayerId={}.",
            oldPlayerId, newPlayerId));
    }

    static void SynchronizeXpPlayers() {
        for (auto& [playerId, tracked] : xpTrackedPlayers) {
            tracked.connected = false;
        }

        for (auto player : GetConnectedXpPlayers()) {
            const auto playerId = player->fields.PlayerId;
            auto data = GetPlayerData(player);
            const std::string puid = data != nullptr && data->fields.Puid != nullptr
                ? convert_from_string(data->fields.Puid)
                : "";
            const std::string friendCode = data != nullptr && data->fields.FriendCode != nullptr
                ? convert_from_string(data->fields.FriendCode)
                : "";

            if (!xpTrackedPlayers.contains(playerId)) {
                const auto priorIdentity = std::find_if(
                    xpTrackedPlayers.begin(), xpTrackedPlayers.end(),
                    [&puid, &friendCode](const auto& entry) {
                        const auto& tracked = entry.second;
                        if (tracked.connected || tracked.is_host) return false;
                        if (!puid.empty() && !tracked.puid.empty()) {
                            return tracked.puid == puid;
                        }
                        return !friendCode.empty()
                            && !tracked.friend_code.empty()
                            && tracked.friend_code == friendCode;
                    });
                if (priorIdentity != xpTrackedPlayers.end()) {
                    MigrateXpPlayerId(priorIdentity->first, playerId);
                }
            }

            auto it = xpTrackedPlayers.try_emplace(playerId).first;
            auto& tracked = it->second;
            tracked.name = XpPlayerName(player);
            tracked.is_host = IsMatchCycleHost(player);
            if (!puid.empty()) tracked.puid = puid;
            if (!friendCode.empty()) tracked.friend_code = friendCode;

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
        size_t creditedCount = 0;
        for (const auto playerId : xpRoundImpostorIds) {
            auto it = xpTrackedPlayers.find(playerId);
            const bool isLocalHost = Game::pLocalPlayer != nullptr
                && *Game::pLocalPlayer != nullptr
                && playerId == (*Game::pLocalPlayer)->fields.PlayerId;
            if (it == xpTrackedPlayers.end() || it->second.is_host || isLocalHost) continue;

            auto& tracked = it->second;
            tracked.was_impostor_this_round = true;
            if (!tracked.connected
                || tracked.disconnected_during_farming_round
                || xpRoundGhostedBeforeFiftyPercentIds.contains(playerId)) {
                tracked.farmed_this_round = false;
                tracked.has_earned_xp = false;
                tracked.has_completed_objective = false;
                xpFarmingCompletedPlayerIds.erase(playerId);
                xpFarmingRetryPlayerIds.insert(playerId);
                LogXpControllerEvent(std::format(
                    "Farming retry: player='{}' id={} disconnected during the round or was ghosted before 50%.",
                    tracked.name, playerId));
                continue;
            }
            tracked.farmed_this_round = true;
            tracked.has_earned_xp = true;
            tracked.has_completed_objective = true;
            xpFarmingCompletedPlayerIds.insert(playerId);
            xpFarmingRetryPlayerIds.erase(playerId);
            ++creditedCount;
            LogXpControllerEvent(std::format(
                "Farming completion: player='{}' id={} host=false assignedImpostorThisRound=true currentRole={} farmedThisRound={} farmingComplete={}.",
                tracked.name, playerId,
                tracked.is_impostor ? "Impostor" : "Crewmate",
                tracked.farmed_this_round, tracked.has_completed_objective));
        }
        LogXpControllerEvent(std::format(
            "Completed farming round credited {} of {} assigned Impostor(s) from the immutable round assignment.",
            creditedCount, xpRoundImpostorIds.size()));
        NotifyXpController(
            "Farming round complete",
            std::format("Credited {} of {} assigned Impostor(s).",
                creditedCount, xpRoundImpostorIds.size()),
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
            LogXpControllerEvent("Drain sequence ended with no tracked non-host participants to request rejoin.");
            StartNextXpCycle();
            return;
        }

        QueueXpControllerChat(std::format(
            "DRAINING MODE ENDED. Rejoin within {} seconds before the next farming cycle.",
            xpFinalRejoinWindowSeconds));
        LogXpControllerEvent(std::format(
            "Drain sequence stopped after {} round(s); requesting {} participant(s) to rejoin during the fixed {}s window.",
            xpDrainRoundsCompleted, rejoinParticipantCount, xpFinalRejoinWindowSeconds));
        NotifyXpController(
            "Rejoin requested",
            std::format("{} participant(s); waiting {}s.", rejoinParticipantCount, xpFinalRejoinWindowSeconds),
            ImVec4(1.f, 0.7f, 0.2f, 1.f));
        xpControllerStage = XpControllerStage::AwaitingRejoin;
        xpDrainRejoinNoticePendingStart = true;
        xpControllerDeadline = (std::chrono::steady_clock::time_point::max)();
    }

    static bool AllRequestedPlayersRejoined() {
        for (const auto& [playerId, tracked] : xpTrackedPlayers) {
            if (!tracked.is_host && tracked.needs_rejoin) return false;
        }
        return true;
    }

    static bool AllTrackedPlayersEarnedXp() {
        const auto players = GetConnectedXpPlayers();
        const size_t participantCount = GetConnectedNonHostCount();
        if (participantCount == 0) return false;
        const bool connectedParticipantsComplete = std::all_of(
            players.begin(), players.end(), [](PlayerControl* player) {
                if (IsMatchCycleHost(player)) return true;
                const auto it = xpTrackedPlayers.find(player->fields.PlayerId);
                return it != xpTrackedPlayers.end()
                    && (xpFarmingCompletedPlayerIds.contains(player->fields.PlayerId)
                        || it->second.has_completed_objective);
            });
        if (!connectedParticipantsComplete) return false;

        return std::all_of(
            xpTrackedPlayers.begin(), xpTrackedPlayers.end(), [](const auto& entry) {
                const auto& tracked = entry.second;
                return tracked.is_host
                    || xpFarmingCompletedPlayerIds.contains(entry.first)
                    || tracked.has_completed_objective;
            });
    }

    static void EnterSoloLobbyKeepAlive(
        std::chrono::steady_clock::time_point now,
        bool needsRoundSetup = false) {
        if (xpControllerStage == XpControllerStage::SoloLobbyKeepAlive) return;
        xpSoloResumePhase = xpControllerPhase;
        xpSoloResumeStage = xpControllerStage;
        xpSoloResumeDeadline = xpControllerDeadline;
        xpSoloResumeDeadlineIsMax =
            xpControllerDeadline == (std::chrono::steady_clock::time_point::max)();
        xpSoloKeepAliveNeedsRoundSetup = needsRoundSetup;
        xpSoloKeepAliveRound = false;
        xpSoloKeepAliveStartedAt = now;
        xpControllerStage = XpControllerStage::SoloLobbyKeepAlive;
        xpControllerDeadline = now + xpSoloKeepAliveInterval;
        LogXpControllerEvent(std::format(
            "No non-host participants are connected; entering solo lobby keep-alive and scheduling a brief start/end in {}s. {}",
            xpSoloKeepAliveInterval.count(), BuildXpControllerDiagnostics()));
    }

    static void ScheduleNextSoloKeepAlive(std::chrono::steady_clock::time_point now) {
        xpSoloKeepAliveRound = false;
        xpSoloKeepAliveStartedAt = now;
        xpControllerStage = XpControllerStage::SoloLobbyKeepAlive;
        xpControllerDeadline = now + xpSoloKeepAliveInterval;
        LogXpControllerEvent(std::format(
            "Solo keep-alive round ended; next lobby start is scheduled in {}s.",
            xpSoloKeepAliveInterval.count()));
    }

    static void ResumeFromSoloLobbyKeepAlive(std::chrono::steady_clock::time_point now) {
        const auto soloDuration = now - xpSoloKeepAliveStartedAt;
        const auto resumedPhase = xpSoloResumePhase;
        const bool needsRoundSetup = xpSoloKeepAliveNeedsRoundSetup;
        xpControllerPhase = resumedPhase;
        xpControllerStage = xpSoloResumeStage;
        xpControllerDeadline = xpSoloResumeDeadlineIsMax
            ? (std::chrono::steady_clock::time_point::max)()
            : xpSoloResumeDeadline + soloDuration;
        xpSoloKeepAliveNeedsRoundSetup = false;
        xpSoloKeepAliveStartedAt = {};
        xpSoloKeepAliveRound = false;
        xpLastRosterCheck = now;
        LogXpControllerEvent(std::format(
            "Non-host participant joined during solo keep-alive; resuming phase={} stage={} after pausing timers for {}ms.",
            XpControllerPhaseName(resumedPhase),
            XpControllerStageName(xpControllerStage),
            std::chrono::duration_cast<std::chrono::milliseconds>(soloDuration).count()));

        if (!needsRoundSetup) return;
        if (xpControllerPhase == XpControllerPhase::FARMING_ROUND) {
            BeginXpFarmingRound();
        }
        else if (xpControllerPhase == XpControllerPhase::ROLLOVER_LOOP) {
            BeginXpRolloverRound();
        }
    }

    static void StartSoloLobbyKeepAliveMatch() {
        if (!IsInLobby() || !IsHost()
            || Game::pAmongUsClient == nullptr || *Game::pAmongUsClient == nullptr) {
            ScheduleNextSoloKeepAlive(std::chrono::steady_clock::now());
            LogXpControllerEvent(
                "Solo lobby keep-alive could not start because lobby/host state was unavailable; retrying in five minutes.");
            return;
        }
        if (g_rpcEmittedThisFrame >= kRpcBudgetPerFrame) return;
        LogXpControllerEvent(
            "Starting a host-only keep-alive match; it will be ended as soon as the game loads.");
        InnerNetClient_SendStartGame((InnerNetClient*)(*Game::pAmongUsClient), NULL);
        ++g_rpcEmittedThisFrame;
        xpSoloKeepAliveRound = true;
        xpControllerStage = XpControllerStage::AwaitingMatch;
        xpControllerDeadline = std::chrono::steady_clock::now() + xpStageTimeout;
    }

    static void StopXpController(const std::string& reason, bool showToast = true) {
        if (!IsXpControllerRunning()) return;
        LogXpControllerEvent(std::format(
            "Stopping: reason='{}'. {}",
            reason, BuildXpControllerDiagnostics()));
        xpControllerPhase = XpControllerPhase::STOPPED;
        xpControllerStage = XpControllerStage::PreparingRound;
        xpControllerPaused = false;
        xpPauseDeadlineRemainingMs = 0;
        xpControllerSnapshot.valid = false;
        xpHostAuthorityLostAt = {};
        xpRoundImpostorIds.clear();
        xpRoundImpostorNames.clear();
        xpFarmingConfirmationResponses.clear();
        xpFarmingCompletedPlayerIds.clear();
        xpFarmingRetryPlayerIds.clear();
        xpDrainStopRequested = false;
        xpDrainRejoinGracePending = false;
        xpDrainRejoinNoticePendingStart = false;
        xpDrainMeetingAnnouncementPending = false;
        xpFarmingConfirmationPending = false;
        xpDrainFarmingPromptResponse = false;
        xpDrainFarmingPromptResponseWindowStarted = false;
        xpDrainFarmingBonusPlayerIds.clear();
        xpDrainReturnAfterFarmingBreak = false;
        xpDrainBonusBatchPendingStart = false;
        xpDrainFarmingPromptGracePending = false;
        xpSoloKeepAliveRound = false;
        xpSoloKeepAliveNeedsRoundSetup = false;
        xpSoloKeepAliveStartedAt = {};
        xpPendingGhostRoleQueueTicks = 0;
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
        xpPendingInGameChatMessages.clear();
        xpRoundChatWait = std::chrono::milliseconds::zero();
        xpRoundChatMessageCount = 0;
        xpRoundChatPostDelaySeconds = 0;
        for (auto& [playerId, tracked] : xpTrackedPlayers) {
            if (tracked.is_shielded) SetShieldState(playerId, false);
        }
        RestoreMatchCycleProtectionSettings();
        if (xpControllerOwnsNoGameEnd) {
            State.NoGameEnd = xpControllerPreviousNoGameEnd;
            xpControllerOwnsNoGameEnd = false;
        }
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

        const std::string drainStartMessage = xpDrainUntilStopMode
            ? "Draining starts in 30s. Type STOP only when you are done."
            : std::format(
                "Auto-drain starts in 30s: {} rounds, {}s between; each ends after 35s.",
                GetXpDrainRoundsTarget(), xpDrainBetweenRoundRejoinSeconds);
        estimate += GetXpChatWaitSeconds(
            CountXpControllerChatChunks(drainStartMessage), 0) * 2;
        estimate += static_cast<int64_t>(drainRounds) * xpRolloverDuration.count();
        if (drainRounds > 1) {
            estimate += static_cast<int64_t>(drainRounds - 1) * xpDrainBetweenRoundRejoinSeconds;
            if (xpDrainFarmingPromptMode) {
                estimate += static_cast<int64_t>(drainRounds - 1)
                    * (xpFarmingLobbyGraceDuration.count()
                        + xpDrainFarmingPromptResponseDuration.count());
            }
        }
        estimate += transitionGraceDuration.count();
        estimate += GetXpChatWaitSeconds(
            CountXpControllerChatChunks(std::format(
                "DRAINING MODE ENDED. Rejoin within {} seconds before the next farming cycle.",
                xpFinalRejoinWindowSeconds)),
            0) * 2;
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
        if (xpFarmingConfirmationMode || xpDrainUntilStopMode) {
            return std::format(
                "Farming runs {}s. First impostors: {}. Cycle duration varies with confirmations and drain completion.",
                xpFarmingDurationSeconds, names);
        }
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
            if (participantCount == 0) {
                EnterSoloLobbyKeepAlive(std::chrono::steady_clock::now(), true);
            }
            else {
                StopXpController("No connected host player is available for team setup.");
            }
            return false;
        }

        xpRoundImpostorIds.clear();
        xpRoundImpostorNames.clear();
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
            tracked.demoted_to_ghost_this_round = false;
            tracked.disconnected_during_farming_round = false;
        }
        xpRoundProtectedKillCounts.clear();
        xpRoundGhostedImpostorIds.clear();
        xpRoundGhostedBeforeFiftyPercentIds.clear();
        xpFarmingSafetyWarningsPending = false;
        xpPendingGhostRoleQueueTicks = 0;

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
                    || (unearnedOnly && (xpFarmingCompletedPlayerIds.contains(player->fields.PlayerId)
                        || it->second.has_completed_objective))
                    || (unearnedOnly && skipAttempted && it->second.has_farming_attempt)) continue;
                impostors.push_back(player);
                lastSelectedIndex = index;
            }
        };
        if (unearnedOnly && xpDrainReturnAfterFarmingBreak
            && !xpDrainFarmingBonusPlayerIds.empty()) {
            std::vector<Game::PlayerId> stillConnected;
            for (const auto playerId : xpDrainFarmingBonusPlayerIds) {
                const auto requestedPlayer = std::find_if(
                    nonHostPlayers.begin(), nonHostPlayers.end(), [playerId](PlayerControl* player) {
                        return player->fields.PlayerId == playerId;
                    });
                if (requestedPlayer == nonHostPlayers.end()) {
                    LogXpControllerEvent(std::format(
                        "Skipping bonus ME requester playerId={} because they disconnected before assignment.",
                        playerId));
                    continue;
                }
                if (impostors.size() >= static_cast<size_t>((std::max)(teamCap, 0))) {
                    stillConnected.push_back(playerId);
                    continue;
                }
                impostors.push_back(*requestedPlayer);
                lastSelectedIndex = static_cast<size_t>(
                    std::distance(nonHostPlayers.begin(), requestedPlayer));
            }
            xpDrainFarmingBonusPlayerIds = std::move(stillConnected);
        }
        else if (unearnedOnly) {
            const auto addEligible = [&](bool retryPriority) {
                for (size_t offset = 0;
                    offset < nonHostPlayers.size()
                    && impostors.size() < static_cast<size_t>(teamCap);
                    ++offset) {
                    const size_t index = (startIndex + offset) % nonHostPlayers.size();
                    auto player = nonHostPlayers[index];
                    const auto playerId = player->fields.PlayerId;
                    auto it = xpTrackedPlayers.find(playerId);
                    if (it == xpTrackedPlayers.end()
                        || xpFarmingCompletedPlayerIds.contains(playerId)
                        || it->second.has_completed_objective
                        || (!retryPriority && it->second.has_farming_attempt)
                        || (retryPriority != xpFarmingRetryPlayerIds.contains(playerId))) continue;
                    impostors.push_back(player);
                    lastSelectedIndex = index;
                }
            };

            if (xpFarmingConfirmationMode) {
                addEligible(true);
                addEligible(false);
            }
            else {
                selectImpostors(true);
            }
            for (size_t offset = 0;
                offset < nonHostPlayers.size()
                && impostors.size() < static_cast<size_t>(teamCap);
                ++offset) {
                const size_t index = (startIndex + offset) % nonHostPlayers.size();
                auto player = nonHostPlayers[index];
                const auto playerId = player->fields.PlayerId;
                const auto tracked = xpTrackedPlayers.find(playerId);
                const bool alreadySelected = std::any_of(
                    impostors.begin(), impostors.end(), [playerId](PlayerControl* selected) {
                        return selected->fields.PlayerId == playerId;
                    });
                if (tracked == xpTrackedPlayers.end()
                    || alreadySelected
                    || xpFarmingCompletedPlayerIds.contains(playerId)
                    || tracked->second.has_completed_objective
                    || xpDeferredFarmingRetryIds.contains(playerId)) continue;
                impostors.push_back(player);
                lastSelectedIndex = index;
            }
        }
        else if (xpControllerPhase == XpControllerPhase::ROLLOVER_LOOP
            && !nonHostPlayers.empty()) {
            const auto randomIndex = static_cast<size_t>(
                GenerateRandomNumber(0, static_cast<int>(nonHostPlayers.size() - 1)));
            impostors.push_back(nonHostPlayers[randomIndex]);
        }

        if (unearnedOnly && impostors.empty() && xpDrainReturnAfterFarmingBreak) {
            xpDrainReturnAfterFarmingBreak = false;
            xpDrainBonusBatchPendingStart = false;
            xpDrainFarmingBonusPlayerIds.clear();
            xpControllerPhase = XpControllerPhase::ROLLOVER_LOOP;
            xpControllerStage = XpControllerStage::LobbyDelay;
            xpControllerDeadline = std::chrono::steady_clock::now()
                + std::chrono::seconds(xpDrainBetweenRoundRejoinSeconds);
            LogXpControllerEvent(std::format(
                "No accepted ME requesters remained connected for assignment; resuming drain after {}s.",
                xpDrainBetweenRoundRejoinSeconds));
            return false;
        }

        if (unearnedOnly
            && impostors.empty()
            && xpProgressMode == XpProgressMode::LegacyChat
            && !xpDeferredFarmingRetryIds.empty()
            && !xpDeferredFarmingRetryActive) {
            for (auto player : nonHostPlayers) {
                if (impostors.size() >= static_cast<size_t>((std::max)(teamCap, 0))) break;
                const auto playerId = player->fields.PlayerId;
                if (!xpDeferredFarmingRetryIds.contains(playerId)) continue;
                if (xpDeferredFarmingRetryUsedIds.contains(playerId)) continue;
                if (xpFarmingCompletedPlayerIds.contains(playerId)) continue;
                impostors.push_back(player);
                xpDeferredFarmingRetryUsedIds.insert(playerId);
            }
            if (!impostors.empty()) {
                xpDeferredFarmingRetryActive = true;
                std::string names;
                for (auto player : impostors) {
                    if (!names.empty()) names += ", ";
                    names += XpControllerChatName(XpPlayerName(player));
                }
                QueueXpControllerChat(std::format(
                    "Final batch retry: {}. Separate targets; do not farm one crewmate together.",
                    names.empty() ? "assigned players" : names));
                LogXpControllerEvent(std::format(
                    "Starting deferred one-chance farming retry for [{}].",
                    names));
            }
        }

        if (unearnedOnly && impostors.empty()) {
            if (!xpFarmingConfirmationMode) {
                for (auto player : players) {
                    if (IsMatchCycleHost(player)) continue;
                    auto it = xpTrackedPlayers.find(player->fields.PlayerId);
                    if (it != xpTrackedPlayers.end()
                        && !xpFarmingCompletedPlayerIds.contains(player->fields.PlayerId)
                        && !it->second.has_completed_objective) {
                        it->second.has_farming_attempt = false;
                    }
                }
                selectImpostors(false);
            }
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
        xpRoundImpostorNames.clear();
        xpRoundImpostorNames.reserve(impostors.size());
        std::string impostorIds;
        for (auto player : impostors) {
            xpRoundImpostorIds.push_back(player->fields.PlayerId);
            xpRoundImpostorNames.push_back(XpControllerChatName(XpPlayerName(player)));
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
                "Drain round role selected",
                std::format("Drain round (not farming): {} is the random Impostor; protection is off.",
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
            const size_t actualShieldCap = (std::min)(
                (std::min)(
                    static_cast<size_t>((std::max)(teamCap, 0)),
                    impostors.size()),
                defenders.size());
            for (auto player : defenders) {
                if (desiredShields.size() == actualShieldCap) break;
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

    static uint32_t GetXpObservedLevel(Game::PlayerId playerId) {
        auto player = GetPlayerControlById(playerId);
        if (player == nullptr) return 0;
        auto data = GetPlayerData(player);
        if (data == nullptr) return 0;
        return data->fields.PlayerLevel + 1;
    }

    static bool HasXpRoundPassedFiftyPercent() {
        if (xpRoundStartedAt == std::chrono::steady_clock::time_point{}) return false;
        const int roundSeconds = xpControllerPhase == XpControllerPhase::FARMING_ROUND
            ? xpFarmingDurationSeconds
            : static_cast<int>(xpRolloverDuration.count());
        const auto elapsed = std::chrono::steady_clock::now() - xpRoundStartedAt;
        return elapsed >= std::chrono::milliseconds(
            static_cast<int64_t>(roundSeconds) * 500);
    }

    static bool HasXpFarmingRoundFailure() {
        return xpNoActiveFarmingImpostors;
    }

    static void CaptureXpRoundLevelBaselines(bool farmingRound) {
        xpRoundLevelBaselines.clear();
        std::vector<Game::PlayerId> captureIds;
        if (farmingRound) {
            captureIds = xpRoundImpostorIds;
        }
        else {
            for (const auto& [playerId, tracked] : xpTrackedPlayers) {
                if (!tracked.is_host) captureIds.push_back(playerId);
            }
        }

        for (const auto playerId : captureIds) {
            const uint32_t level = GetXpObservedLevel(playerId);
            if (level != 0) xpRoundLevelBaselines[playerId] = level;
        }
    }

    static bool EvaluateXpLevelFarmingRound() {
        size_t completedCount = 0;
        std::string resultSummary = "FARMING LEVEL RESULT: ";
        for (const auto playerId : xpRoundImpostorIds) {
            auto tracked = xpTrackedPlayers.find(playerId);
            if (tracked == xpTrackedPlayers.end() || tracked->second.is_host) continue;

            const uint32_t currentLevel = GetXpObservedLevel(playerId);
            const auto baseline = xpRoundLevelBaselines.find(playerId);
            const bool leveledUp = tracked->second.connected
                && !tracked->second.disconnected_during_farming_round
                && baseline != xpRoundLevelBaselines.end()
                && baseline->second != 0
                && currentLevel >= baseline->second + 1
                && !xpRoundGhostedBeforeFiftyPercentIds.contains(playerId);

            tracked->second.has_earned_xp = leveledUp;
            tracked->second.has_completed_objective = leveledUp;
            tracked->second.farmed_this_round = leveledUp;
            if (leveledUp) {
                xpFarmingCompletedPlayerIds.insert(playerId);
                xpFarmingRetryPlayerIds.erase(playerId);
                ++completedCount;
            }
            else {
                xpFarmingCompletedPlayerIds.erase(playerId);
                xpFarmingRetryPlayerIds.insert(playerId);
            }

            if (resultSummary != "FARMING LEVEL RESULT: ") resultSummary += ", ";
            resultSummary += std::format("{} {}",
                XpControllerChatName(tracked->second.name),
                leveledUp ? "leveled up" : "retry");
            LogXpControllerEvent(std::format(
                "Farming level result: playerId={}, name='{}', baseline={}, current={}, leveledUp={}.",
                playerId, tracked->second.name,
                baseline == xpRoundLevelBaselines.end() ? 0 : baseline->second,
                currentLevel, leveledUp));
        }
        QueueXpControllerChat(resultSummary + ".");
        return completedCount == xpRoundImpostorIds.size();
    }

    static bool EvaluateXpLevelDrainRound() {
        size_t activeCount = 0;
        size_t leveledCount = 0;
        for (const auto& [playerId, tracked] : xpTrackedPlayers) {
            if (tracked.is_host) continue;
            const auto baseline = xpRoundLevelBaselines.find(playerId);
            if (baseline == xpRoundLevelBaselines.end()) continue;
            const uint32_t currentLevel = GetXpObservedLevel(playerId);
            ++activeCount;
            if (currentLevel >= baseline->second + 1) ++leveledCount;
        }
        LogXpControllerEvent(std::format(
            "Drain level result: round={}, active={}, leveledUp={}.",
            xpDrainRoundsCompleted + 1, activeCount, leveledCount));
        return activeCount > 0 && leveledCount == activeCount;
    }

    static bool DemoteFastestFarmingImpostorToGhost() {
        Game::PlayerId offenderId = 0;
        int highestKillCount = 0;
        for (const auto playerId : xpRoundImpostorIds) {
            const auto tracked = xpTrackedPlayers.find(playerId);
            const auto kills = xpRoundProtectedKillCounts.find(playerId);
            if (tracked == xpTrackedPlayers.end()
                || !tracked->second.is_impostor
                || tracked->second.demoted_to_ghost_this_round
                || kills == xpRoundProtectedKillCounts.end()
                || kills->second <= highestKillCount) continue;
            offenderId = playerId;
            highestKillCount = kills->second;
        }

        if (highestKillCount == 0) return false;
        auto offender = GetPlayerControlById(offenderId);
        auto tracked = xpTrackedPlayers.find(offenderId);
        if (offender == nullptr || tracked == xpTrackedPlayers.end()) return false;

        if (xpPendingGhostRoleQueueTicks == 0) {
            xpPendingGhostRoleQueueTicks = State.rpcQueue.size();
        }
        State.rpcQueue.push(new RpcSetRole(offender, RoleTypes__Enum::ImpostorGhost));
        ++xpPendingGhostRoleQueueTicks;
        tracked->second.is_impostor = false;
        tracked->second.demoted_to_ghost_this_round = true;
        xpRoundGhostedImpostorIds.insert(offenderId);
        xpFarmingSafetyWarningsPending = true;
        if (!HasXpRoundPassedFiftyPercent()) {
            xpRoundGhostedBeforeFiftyPercentIds.insert(offenderId);
        }
        LogXpControllerEvent(std::format(
            "Demoted farming Impostor to ghost after {} protected-target kill(s) and no replacement targets: playerId={}, name='{}'.",
            highestKillCount, offenderId, tracked->second.name));
        return true;
    }

    static size_t CountActiveXpFarmingImpostors() {
        return static_cast<size_t>(std::count_if(
            xpRoundImpostorIds.begin(), xpRoundImpostorIds.end(),
            [](Game::PlayerId playerId) {
                const auto tracked = xpTrackedPlayers.find(playerId);
                if (tracked == xpTrackedPlayers.end()
                    || !tracked->second.is_impostor
                    || tracked->second.demoted_to_ghost_this_round
                    || !tracked->second.connected) return false;
                auto player = GetPlayerControlById(playerId);
                if (player == nullptr) return false;
                auto data = GetPlayerData(player);
                return data != nullptr && !data->fields.IsDead;
            }));
    }

    void OnXpControllerProtectedCrewmateKilled(
        Game::PlayerId killerId,
        Game::PlayerId victimId) {
        if (!IsXpControllerRunning()
            || !IsHost()
            || xpControllerPhase != XpControllerPhase::FARMING_ROUND
            || xpControllerStage != XpControllerStage::MatchTimer) return;

        const auto killer = xpTrackedPlayers.find(killerId);
        const auto victim = xpTrackedPlayers.find(victimId);
        if (killer == xpTrackedPlayers.end()
            || victim == xpTrackedPlayers.end()
            || killer->second.is_host
            || !killer->second.is_impostor
            || killer->second.demoted_to_ghost_this_round
            || victim->second.is_host
            || victim->second.is_impostor
            || !victim->second.is_shielded) return;

        const int killCount = ++xpRoundProtectedKillCounts[killerId];
        LogXpControllerEvent(std::format(
            "Farming shield target killed: killerId={}, victimId={}, killerProtectedKillCount={}.",
            killerId, victimId, killCount));
    }

    void OnXpControllerPlayerLeft(Game::PlayerId playerId) {
        if (!IsXpControllerRunning()) return;

        auto tracked = xpTrackedPlayers.find(playerId);
        if (tracked == xpTrackedPlayers.end()) return;
        tracked->second.connected = false;
        if (xpControllerPhase != XpControllerPhase::FARMING_ROUND
            || std::find(
                xpRoundImpostorIds.begin(), xpRoundImpostorIds.end(), playerId)
                == xpRoundImpostorIds.end()) return;

        tracked->second.disconnected_during_farming_round = true;
        tracked->second.has_earned_xp = false;
        tracked->second.has_completed_objective = false;
        xpFarmingCompletedPlayerIds.erase(playerId);
        xpFarmingRetryPlayerIds.insert(playerId);
        LogXpControllerEvent(std::format(
            "Assigned farming participant disconnected; completion revoked pending a later successful batch: playerId={}, name='{}'.",
            playerId, tracked->second.name));
    }

    void OnXpControllerChatSent() {
        if (xpControllerStage != XpControllerStage::DrainFarmingPrompt
            || xpDrainFarmingPromptResponseWindowStarted) return;

        xpDrainFarmingPromptResponseWindowStarted = true;
        xpControllerDeadline = std::chrono::steady_clock::now()
            + xpDrainFarmingPromptResponseDuration;
        LogXpControllerEvent(std::format(
            "ME response window started after the prompt was sent ({} seconds).",
            xpDrainFarmingPromptResponseDuration.count()));
    }

    static void ReconcileMatchCycleShields() {
        if (!IsXpControllerRunning() || !IsHost()
            || xpControllerPhase == XpControllerPhase::ROLLOVER_LOOP) return;
        if (matchCycleProtectionOwnsSettings) {
            protectionIntervalMs =
                static_cast<int>(GetMatchCycleUpdateInterval().count());
        }

        const auto connectedPlayers = GetConnectedXpPlayers();
        const int teamCap = GetMatchCycleTeamCap(GetConnectedNonHostCount());
        std::vector<PlayerControl*> eligibleDefenders;
        for (auto player : connectedPlayers) {
            auto tracked = xpTrackedPlayers.find(player->fields.PlayerId);
            auto data = GetPlayerData(player);
            if (tracked == xpTrackedPlayers.end() || tracked->second.is_impostor
                || tracked->second.demoted_to_ghost_this_round
                || data == nullptr || data->fields.IsDead) continue;
            eligibleDefenders.push_back(player);
        }
        std::stable_sort(eligibleDefenders.begin(), eligibleDefenders.end(), [](PlayerControl* left, PlayerControl* right) {
            return !IsMatchCycleHost(left) && IsMatchCycleHost(right);
        });

        std::vector<Game::PlayerId> desiredShields;
        size_t activeImpostorCount = CountActiveXpFarmingImpostors();
        const auto getRequiredShieldCount = [&]() {
            return (std::min)(
                static_cast<size_t>((std::max)(teamCap, 0)),
                CountActiveXpFarmingImpostors());
        };
        size_t requiredShieldCount = getRequiredShieldCount();
        size_t desiredCount = (std::min)(requiredShieldCount, eligibleDefenders.size());
        while (xpControllerPhase == XpControllerPhase::FARMING_ROUND
            && xpControllerStage == XpControllerStage::MatchTimer
            && requiredShieldCount > 0
            && desiredCount < requiredShieldCount
            && DemoteFastestFarmingImpostorToGhost()) {
            activeImpostorCount = CountActiveXpFarmingImpostors();
            requiredShieldCount = getRequiredShieldCount();
            desiredCount = (std::min)(requiredShieldCount, eligibleDefenders.size());
        }
        if (xpControllerPhase == XpControllerPhase::FARMING_ROUND
            && xpControllerStage == XpControllerStage::MatchTimer
            && !xpRoundImpostorIds.empty()
            && activeImpostorCount == 0) {
            if (!HasXpRoundPassedFiftyPercent()) {
                xpRoundFailureDetectedBeforeFiftyPercent = true;
            }
            xpNoActiveFarmingImpostors = true;
            if (!xpRoundShieldFailure) {
                LogXpControllerEvent(
                    "No active farming Impostors remain after ghost conversion; ending the round early.");
            }
            xpRoundShieldFailure = true;
        }
        else if (xpControllerPhase == XpControllerPhase::FARMING_ROUND
            && xpControllerStage == XpControllerStage::MatchTimer
            && requiredShieldCount > 0
            && eligibleDefenders.empty()) {
            if (!HasXpRoundPassedFiftyPercent()) {
                xpRoundFailureDetectedBeforeFiftyPercent = true;
            }
            if (!xpRoundShieldFailure) {
                LogXpControllerEvent(std::format(
                    "No living Crewmate targets remain to protect {} active farming Impostor(s).",
                    activeImpostorCount));
            }
            xpRoundShieldFailure = true;
        }
        else if (xpRoundShieldFailure && !xpRoundShieldFailureWaived) {
            LogXpControllerEvent(std::format(
                "Farming shield target restored: requiredTargets={}, availableTargets={}; canceling pending failure.",
                requiredShieldCount, desiredCount));
            xpRoundShieldFailure = false;
        }
        for (auto player : eligibleDefenders) {
            if (desiredShields.size() == desiredCount) break;
            desiredShields.push_back(player->fields.PlayerId);
        }

        static std::vector<Game::PlayerId> lastDesiredShields;
        static std::chrono::steady_clock::time_point lastShieldSendAt{};

        const auto now = std::chrono::steady_clock::now();
        const bool sameSet = (desiredShields == lastDesiredShields);
        const auto updateInterval = GetMatchCycleUpdateInterval();
        const bool intervalElapsed = (now - lastShieldSendAt) >= updateInterval;
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
        xpControllerPhase = XpControllerPhase::FARMING_ROUND;
        if (GetConnectedNonHostCount() == 0) {
            EnterSoloLobbyKeepAlive(std::chrono::steady_clock::now(), true);
            return;
        }
        if (!ConfigureMatchCycleTeams(true)) return;
        xpRoundShieldFailure = false;
        xpRoundShieldFailureWaived = false;
        xpRoundFailureDetectedBeforeFiftyPercent = false;
        xpNoActiveFarmingImpostors = false;
        xpLegacyFarmingRoundFailed = false;
        xpRoundStartedAt = {};
        if (xpProgressMode != XpProgressMode::LegacyChat) {
            CaptureXpRoundLevelBaselines(true);
        }
        xpFarmingConfirmationResponses.clear();
        xpFarmingConfirmationPending = false;
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
        else if (xpFarmingConfirmationMode) {
            std::string nextBatchNames;
            for (const auto& name : xpRoundImpostorNames) {
                if (!nextBatchNames.empty()) nextBatchNames += ", ";
                nextBatchNames += name;
            }
            QueueXpControllerChat(std::format(
                "Next farming batch ({}): {}.",
                xpRoundImpostorNames.size(),
                nextBatchNames.empty() ? "none" : nextBatchNames));
        }
        xpControllerPhase = XpControllerPhase::FARMING_ROUND;
        xpControllerStage = XpControllerStage::PreparingRound;
        xpControllerDeadline = std::chrono::steady_clock::now() + xpRoundPrepareDelay;
    }

    static void BeginXpRolloverRound() {
        if (!IsInLobby() || !IsHost()) return;
        xpControllerPhase = XpControllerPhase::ROLLOVER_LOOP;
        if (GetConnectedNonHostCount() == 0) {
            EnterSoloLobbyKeepAlive(std::chrono::steady_clock::now(), true);
            return;
        }
        if (!ConfigureMatchCycleTeams(false)) return;
        xpRoundShieldFailure = false;
        xpRoundShieldFailureWaived = false;
        xpNoActiveFarmingImpostors = false;
        xpRoundStartedAt = {};
        if (xpProgressMode != XpProgressMode::LegacyChat) {
            CaptureXpRoundLevelBaselines(false);
        }
        xpControllerPhase = XpControllerPhase::ROLLOVER_LOOP;
        xpDrainMeetingAnnouncementPending = false;
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
                && std::find(xpRoundImpostorIds.begin(), xpRoundImpostorIds.end(), playerId)
                    != xpRoundImpostorIds.end();
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
        struct ScopedNoGameEndOverride {
            bool previous;
            ScopedNoGameEndOverride() : previous(State.NoGameEnd) { State.NoGameEnd = false; }
            ~ScopedNoGameEndOverride() { State.NoGameEnd = previous; }
        };
        [[maybe_unused]] ScopedNoGameEndOverride allowScheduledEnd;
        GameManager_RpcEndGame(gameManager, GameOverReason__Enum(0), false, NULL);
        ++g_rpcEmittedThisFrame;
        xpControllerStage = XpControllerStage::AwaitingLobby;
        xpControllerDeadline = std::chrono::steady_clock::now() + xpStageTimeout;
        xpTransitionGraceDeadline = std::chrono::steady_clock::now() + transitionGraceDuration;
        xpDrainMeetingAnnouncementPending = false;
    }

    static void BeginXpFarmingConfirmationWindow() {
        xpFarmingConfirmationPending = true;
        std::string assignedNames;
        std::string assignedDetails;
        for (size_t assignedIndex = 0; assignedIndex < xpRoundImpostorIds.size(); ++assignedIndex) {
            if (!assignedNames.empty()) assignedNames += ", ";
            const std::string name = assignedIndex < xpRoundImpostorNames.size()
                ? xpRoundImpostorNames[assignedIndex]
                : std::format("Player {}", xpRoundImpostorIds[assignedIndex]);
            assignedNames += name;
            if (!assignedDetails.empty()) assignedDetails += ", ";
            assignedDetails += std::format("{}='{}'", xpRoundImpostorIds[assignedIndex], name);
        }
        const bool batchPromptQueued = QueueXpControllerChat(std::format(
            "Farming batch ({}): {}.",
            xpRoundImpostorIds.size(),
            assignedNames.empty() ? "none" : assignedNames));
        const bool instructionPromptQueued = QueueXpControllerChat(
            "Type DONE or RETRY in 30s. DONE/no reply=DONE. RETRY=another turn.");
        if (!batchPromptQueued || !instructionPromptQueued) {
            StopXpController("Could not queue the farming confirmation prompt.");
            return;
        }

        xpControllerStage = XpControllerStage::FarmingConfirmations;
        xpControllerDeadline = (std::chrono::steady_clock::time_point::max)();
        LogXpControllerEvent(std::format(
            "Queued farming confirmation prompt for assigned players [{}]; the response window starts after both messages are sent.",
            assignedDetails));
    }

    static void ContinueXpDrainBonusBatchesOrResume() {
        if (!xpDrainFarmingBonusPlayerIds.empty()) {
            xpControllerPhase = XpControllerPhase::FARMING_ROUND;
            xpControllerStage = XpControllerStage::LobbyDelay;
            xpDrainBonusBatchPendingStart = true;
            xpControllerDeadline = std::chrono::steady_clock::now()
                + xpFarmingLobbyGraceDuration;
            LogXpControllerEvent(std::format(
                "{} ME bonus requester(s) remain; starting their next batch after {}s.",
                xpDrainFarmingBonusPlayerIds.size(),
                xpFarmingLobbyGraceDuration.count()));
            return;
        }

        xpDrainReturnAfterFarmingBreak = false;
        xpDrainBonusBatchPendingStart = false;
        xpControllerPhase = XpControllerPhase::ROLLOVER_LOOP;
        xpControllerStage = XpControllerStage::LobbyDelay;
        xpControllerDeadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(xpDrainBetweenRoundRejoinSeconds);
        LogXpControllerEvent(std::format(
            "All accepted ME bonus batches completed; returning to drain after {}s.",
            xpDrainBetweenRoundRejoinSeconds));
    }

    static void ProcessXpFarmingConfirmations() {
        xpFarmingConfirmationPending = false;
        size_t completedCount = 0;
        std::string resultSummary = "FARMING RESULT: ";
        for (const auto playerId : xpRoundImpostorIds) {
            const auto response = xpFarmingConfirmationResponses.find(playerId);
            auto tracked = xpTrackedPlayers.find(playerId);
            if (tracked == xpTrackedPlayers.end() || tracked->second.is_host) continue;
            const bool participantAvailable = tracked->second.connected
                && !tracked->second.disconnected_during_farming_round;
            const bool completed = participantAvailable
                && !xpRoundGhostedBeforeFiftyPercentIds.contains(playerId)
                && (response == xpFarmingConfirmationResponses.end()
                    ? true
                    : response->second);
            const size_t assignedIndex = static_cast<size_t>(std::distance(
                xpRoundImpostorIds.begin(),
                std::find(xpRoundImpostorIds.begin(), xpRoundImpostorIds.end(), playerId)));
            const std::string name = assignedIndex < xpRoundImpostorNames.size()
                ? xpRoundImpostorNames[assignedIndex]
                : std::format("Player {}", playerId);
            if (resultSummary != "FARMING RESULT: ") resultSummary += ", ";
            const std::string result = xpRoundGhostedBeforeFiftyPercentIds.contains(playerId)
                ? "RETRY (ghosted before 50%)"
                : (!participantAvailable
                    ? "RETRY (disconnected)"
                : (response == xpFarmingConfirmationResponses.end()
                    ? "DONE (no reply)"
                    : (completed ? "DONE" : "RETRY")));
            resultSummary += std::format("{} {}", name, result);
            tracked->second.has_earned_xp = completed;
            tracked->second.has_completed_objective = completed;
            if (completed) xpFarmingCompletedPlayerIds.insert(playerId);
            else xpFarmingCompletedPlayerIds.erase(playerId);
            if (completed) xpFarmingRetryPlayerIds.erase(playerId);
            else xpFarmingRetryPlayerIds.insert(playerId);
            if (completed) ++completedCount;
        }
        QueueXpControllerChat(resultSummary + ".");
        LogXpControllerEvent(std::format(
            "Farming confirmations closed: {} of {} assigned players completed; connected no-reply assignments are credited and disconnected no-reply assignments remain eligible.",
            completedCount, xpRoundImpostorIds.size()));
        xpFarmingConfirmationResponses.clear();
        xpControllerPhase = XpControllerPhase::LOBBY_EVAL;
        xpControllerStage = XpControllerStage::EvaluateLobby;
        if (xpDrainReturnAfterFarmingBreak) {
            ContinueXpDrainBonusBatchesOrResume();
        }
    }

    static void BeginXpDrainFarmingPrompt() {
        xpDrainFarmingPromptResponse = false;
        xpDrainFarmingPromptResponseWindowStarted = false;
        xpDrainFarmingBonusPlayerIds.clear();
        xpDrainBonusBatchPendingStart = false;
        xpControllerStage = XpControllerStage::DrainFarmingPrompt;
        xpControllerDeadline = (std::chrono::steady_clock::time_point::max)();
        if (!QueueXpControllerChat(
            "Bonus round? Non-host players type ME once in 15s. Already-farmed players are eligible.",
            std::chrono::seconds::zero(),
            true)) {
            StopXpController("Could not queue the drain farming prompt.");
            return;
        }
        LogXpControllerEvent(std::format(
            "Queued 15-second ME prompt after drain round {}; timer will start when the prompt is sent.",
            xpDrainRoundsCompleted));
    }

    static void ProcessXpDrainFarmingPrompt() {
        xpDrainFarmingPromptResponseWindowStarted = false;
        xpDrainFarmingBonusPlayerIds.erase(
            std::remove_if(
                xpDrainFarmingBonusPlayerIds.begin(),
                xpDrainFarmingBonusPlayerIds.end(),
                [](Game::PlayerId playerId) {
                    const auto requester = xpTrackedPlayers.find(playerId);
                    return requester == xpTrackedPlayers.end()
                        || requester->second.is_host
                        || !requester->second.connected;
                }),
            xpDrainFarmingBonusPlayerIds.end());
        if (xpDrainFarmingBonusPlayerIds.empty()) {
            if (xpDrainFarmingPromptResponse) {
                QueueXpControllerChat("All ME requesters disconnected. Continuing drain rounds.");
                LogXpControllerEvent(
                    "ME farming requests expired because all non-host requesters disconnected.");
            }
            else {
                QueueXpControllerChat("No ME received. Continuing drain rounds.");
            }
            xpDrainFarmingPromptResponse = false;
            xpDrainFarmingPromptResponseWindowStarted = false;
            xpControllerPhase = XpControllerPhase::ROLLOVER_LOOP;
            xpControllerStage = XpControllerStage::LobbyDelay;
            xpControllerDeadline = std::chrono::steady_clock::now()
                + std::chrono::seconds(xpDrainBetweenRoundRejoinSeconds);
            LogXpControllerEvent(std::format(
                "Drain farming prompt ended without an active requester; continuing after {}s.",
                xpDrainBetweenRoundRejoinSeconds));
            return;
        }

        xpDrainReturnAfterFarmingBreak = true;
        xpDrainBonusBatchPendingStart = false;
        xpControllerPhase = XpControllerPhase::FARMING_ROUND;
        xpFarmingCycleAnnouncementPending = false;
        xpControllerStage = XpControllerStage::PreparingRound;
        xpControllerDeadline = std::chrono::steady_clock::now()
            + xpRoundPrepareDelay;
        QueueXpControllerChat(std::format(
            "{} ME request(s) accepted. Starting bonus farming batches.",
            xpDrainFarmingBonusPlayerIds.size()));
        LogXpControllerEvent(std::format(
            "{} ME request(s) accepted; starting bonus farming batches before returning to drain.",
            xpDrainFarmingBonusPlayerIds.size()));
        BeginXpFarmingRound();
    }

    static void HandleXpRoundReturnedToLobby() {
        CheckXpLevelPatchAfterRound();
        if (xpControllerPhase == XpControllerPhase::ROLLOVER_LOOP) {
            ++xpDrainRoundsCompleted;
            if (xpProgressMode != XpProgressMode::LegacyChat) {
                if (xpRoundShieldFailure && !xpRoundShieldFailureWaived) {
                    xpRoundShieldFailure = false;
                    xpDrainRejoinGracePending = true;
                    xpControllerStage = XpControllerStage::LobbyDelay;
                    xpControllerDeadline = std::chrono::steady_clock::now()
                        + transitionGraceDuration;
                    QueueXpControllerChat(
                        "Drain round failed: no replacement shield target. Retrying after rejoin.");
                    LogXpControllerEvent(
                        "Level-mode drain round failed because no replacement shield target remained.");
                    return;
                }

                const bool allLeveledUp = EvaluateXpLevelDrainRound();
                if (!allLeveledUp) {
                    xpDrainRejoinGracePending = true;
                    xpControllerStage = XpControllerStage::LobbyDelay;
                    xpControllerDeadline = std::chrono::steady_clock::now()
                        + transitionGraceDuration;
                    LogXpControllerEvent(
                        "Level-mode drain levels stopped increasing; entering rejoin grace.");
                    return;
                }

                xpControllerStage = XpControllerStage::LobbyDelay;
                xpControllerDeadline = std::chrono::steady_clock::now()
                    + std::chrono::seconds(xpDrainBetweenRoundRejoinSeconds);
                QueueXpControllerChat(std::format(
                    "Drain round {} complete. Levels increased.",
                    xpDrainRoundsCompleted));
                LogXpControllerEvent(std::format(
                    "Level-mode drain round {} successful; waiting {}s and continuing.",
                    xpDrainRoundsCompleted, xpDrainBetweenRoundRejoinSeconds));
                return;
            }
            LogXpControllerEvent(std::format(
                "Completed drain round {}/{} ({} configured rounds per farming minute).",
                xpDrainRoundsCompleted, xpDrainRoundsTarget, xpDrainRoundsPerMinute));
            NotifyXpController(
                "Drain round complete",
                xpDrainUntilStopMode
                    ? std::format("Round {} completed.", xpDrainRoundsCompleted)
                    : std::format("Round {}/{}.", xpDrainRoundsCompleted, xpDrainRoundsTarget),
                ImVec4(1.f, 0.7f, 0.2f, 1.f));
            if (xpDrainStopRequested
                || (!xpDrainUntilStopMode && xpDrainRoundsCompleted >= xpDrainRoundsTarget)) {
                xpDrainStopRequested = false;
                xpDrainRejoinGracePending = true;
                xpControllerStage = XpControllerStage::LobbyDelay;
                xpControllerDeadline = std::chrono::steady_clock::now()
                    + transitionGraceDuration;
                LogXpControllerEvent(std::format(
                    "Drain ended; waiting {}s for the lobby transition to settle before announcing the {}s rejoin window.",
                    transitionGraceDuration.count(), xpFinalRejoinWindowSeconds));
                return;
            }
            if (xpDrainFarmingPromptMode) {
                xpDrainFarmingPromptGracePending = true;
                xpControllerStage = XpControllerStage::LobbyDelay;
                xpControllerDeadline = std::chrono::steady_clock::now()
                    + xpFarmingLobbyGraceDuration;
                LogXpControllerEvent(std::format(
                    "Drain round {} completed; waiting {}s lobby grace before the ME prompt.",
                    xpDrainRoundsCompleted,
                    xpFarmingLobbyGraceDuration.count()));
                return;
            }
            LogXpControllerEvent(std::format(
                "Waiting {}s before drain round {}/{}.",
                xpDrainBetweenRoundRejoinSeconds,
                xpDrainRoundsCompleted + 1,
                xpDrainRoundsTarget));
        }
        else {
            if (!xpRoundFailureDetectedBeforeFiftyPercent
                && HasXpFarmingRoundFailure()
                && HasXpRoundPassedFiftyPercent()) {
                xpRoundShieldFailureWaived = true;
            }
            if (xpProgressMode != XpProgressMode::LegacyChat
                && HasXpFarmingRoundFailure()
                && !xpRoundShieldFailureWaived) {
                for (const auto playerId : xpRoundImpostorIds) {
                    xpFarmingCompletedPlayerIds.erase(playerId);
                    xpFarmingRetryPlayerIds.insert(playerId);
                    auto tracked = xpTrackedPlayers.find(playerId);
                    if (tracked == xpTrackedPlayers.end()) continue;
                    tracked->second.has_earned_xp = false;
                    tracked->second.has_completed_objective = false;
                    tracked->second.farmed_this_round = false;
                }
                xpControllerStage = XpControllerStage::LobbyDelay;
                xpControllerDeadline = std::chrono::steady_clock::now()
                    + xpFarmingLobbyGraceDuration;
                QueueXpControllerChat(
                    "Farming round failed before 50%; assigned Impostors remain incomplete and will be retried.");
                LogXpControllerEvent(std::format(
                    "Farming round failed before 50%; cleared completion for all {} assigned Impostor(s), including ghosted players. {}",
                    xpRoundImpostorIds.size(), BuildXpControllerDiagnostics()));
                return;
            }
            if (xpProgressMode != XpProgressMode::LegacyChat) {
                bool levelEvidenceAvailable = true;
                for (const auto playerId : xpRoundImpostorIds) {
                    const auto baseline = xpRoundLevelBaselines.find(playerId);
                    if (baseline == xpRoundLevelBaselines.end()
                        || GetXpObservedLevel(playerId) == 0) {
                        levelEvidenceAvailable = false;
                        break;
                    }
                }
                if (xpProgressMode == XpProgressMode::LevelWithChatFallback
                    && !levelEvidenceAvailable) {
                    xpFarmingConfirmationPending = true;
                    xpControllerStage = XpControllerStage::LobbyDelay;
                    xpControllerDeadline = std::chrono::steady_clock::now()
                        + xpFarmingLobbyGraceDuration;
                    LogXpControllerEvent(
                        "Farming level evidence unavailable; activating legacy chat confirmation fallback.");
                    return;
                }

                EvaluateXpLevelFarmingRound();
                xpControllerStage = XpControllerStage::LobbyDelay;
                xpControllerDeadline = std::chrono::steady_clock::now()
                    + xpFarmingLobbyGraceDuration;
                LogXpControllerEvent(
                    "Farming round evaluated from level baselines; no DONE/RETRY prompt in level mode.");
                return;
            }
            if (xpProgressMode == XpProgressMode::LegacyChat) {
                xpLegacyFarmingRoundFailed =
                    xpLegacyFarmingRoundFailed
                    || (xpNoActiveFarmingImpostors
                        && !xpRoundShieldFailureWaived);
                if (xpLegacyFarmingRoundFailed) {
                    for (const auto playerId : xpRoundImpostorIds) {
                        xpFarmingCompletedPlayerIds.erase(playerId);
                        xpFarmingRetryPlayerIds.insert(playerId);
                        auto tracked = xpTrackedPlayers.find(playerId);
                        if (tracked != xpTrackedPlayers.end()) {
                            tracked->second.has_earned_xp = false;
                            tracked->second.has_completed_objective = false;
                        }
                        if (xpDeferredFarmingRetryActive
                            && xpDeferredFarmingRetryIds.contains(playerId)) {
                            xpDeferredFarmingRetryIds.erase(playerId);
                            xpDeferredFarmingRetryUsedIds.erase(playerId);
                            xpLegacyFarmingFailureStreak[playerId] = 0;
                            xpFarmingRetryPlayerIds.erase(playerId);
                        }
                        else {
                            const int streak =
                                xpLegacyFarmingFailureStreak[playerId] + 1;
                            xpLegacyFarmingFailureStreak[playerId] = streak;
                            if (streak >= 2) {
                                xpDeferredFarmingRetryIds.insert(playerId);
                                xpFarmingRetryPlayerIds.erase(playerId);
                                xpLegacyFarmingFailureStreak[playerId] = 0;
                                LogXpControllerEvent(std::format(
                                    "PlayerId={} reached two consecutive farming failures; deferred to one final retry.",
                                    playerId));
                            }
                        }
                    }
                    if (xpDeferredFarmingRetryActive
                        && xpDeferredFarmingRetryIds.empty()) {
                        xpDeferredFarmingRetryActive = false;
                        LogXpControllerEvent(
                            "Deferred retry failed; keeping the participants eligible for normal farming batches.");
                    }
                    xpRoundShieldFailure = false;
                    xpNoActiveFarmingImpostors = false;
                    LogXpControllerEvent(
                        "Legacy farming round failed before 50%; keeping the batch eligible for retry.");
                }
                else {
                    xpRoundShieldFailureWaived = false;
                    EvaluateCompletedFarmingRound();
                    if (xpDeferredFarmingRetryActive) {
                        for (const auto playerId : xpRoundImpostorIds) {
                            xpDeferredFarmingRetryIds.erase(playerId);
                            xpDeferredFarmingRetryUsedIds.erase(playerId);
                            xpLegacyFarmingFailureStreak[playerId] = 0;
                            xpFarmingRetryPlayerIds.erase(playerId);
                        }
                        xpDeferredFarmingRetryActive =
                            !xpDeferredFarmingRetryIds.empty();
                        LogXpControllerEvent(
                            "Deferred final farming retry succeeded; batch marked complete.");
                    }
                }
                xpControllerStage = XpControllerStage::LobbyDelay;
                xpControllerDeadline = std::chrono::steady_clock::now()
                    + xpFarmingLobbyGraceDuration;
                return;
            }
                if (xpFarmingConfirmationMode) {
                xpFarmingConfirmationPending = true;
                LogXpControllerEvent(
                    "Farming match returned to lobby; allowing the 20-second lobby grace period before requesting confirmations.");
                xpControllerStage = XpControllerStage::LobbyDelay;
                xpControllerDeadline = std::chrono::steady_clock::now()
                    + xpFarmingLobbyGraceDuration;
                return;
            }
            LogXpControllerEvent("Lobby detected after farming match; starting 20s lobby delay.");
        }
        xpControllerStage = XpControllerStage::LobbyDelay;
        xpControllerDeadline = std::chrono::steady_clock::now()
            + std::chrono::seconds(xpControllerPhase == XpControllerPhase::ROLLOVER_LOOP
                ? xpDrainBetweenRoundRejoinSeconds
                : xpFarmingLobbyGraceDuration.count());
    }

    static void StartNextXpCycle() {
        xpDrainFarmingMeUsedPlayerIds.clear();
        xpLegacyFarmingFailureStreak.clear();
        xpDeferredFarmingRetryIds.clear();
        xpDeferredFarmingRetryUsedIds.clear();
        xpDeferredFarmingRetryActive = false;
        for (auto& [playerId, tracked] : xpTrackedPlayers) {
            if (tracked.is_host) continue;
            tracked.has_earned_xp = false;
            tracked.has_completed_objective = false;
            tracked.is_impostor = false;
            tracked.has_farming_attempt = false;
            tracked.needs_rejoin = false;
            tracked.disconnected_after_rejoin_request = false;
        }
        xpFarmingCompletedPlayerIds.clear();
        xpRoundImpostorIds.clear();
        xpRoundImpostorNames.clear();
        xpFarmingRetryPlayerIds.clear();
        xpDrainRoundsCompleted = 0;
        xpDrainRoundsTarget = 0;
        LogXpControllerEvent(std::format(
            "Drain sequence completed; starting the next full farming cycle. {}",
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
        if (xpFarmingSafetyWarningsPending) {
            QueueXpControllerChat(
                "Impostors: stay separate; never have two Impostors farm one Crewmate.");
            xpFarmingSafetyWarningsPending = false;
            LogXpControllerEvent(std::format(
                "Queued post-grace safety warnings for {} ghosted farming Impostor(s).",
                xpRoundGhostedImpostorIds.size()));
        }
        if (xpDrainRejoinGracePending) {
            xpDrainRejoinGracePending = false;
            RequestRejoinForDrainCompletion();
            return;
        }
        if (xpDrainFarmingPromptGracePending) {
            xpDrainFarmingPromptGracePending = false;
            BeginXpDrainFarmingPrompt();
            return;
        }
        if (GetConnectedNonHostCount() == 0) {
            LogXpControllerEvent(std::format(
                "{}s lobby delay expired with zero non-host players; pausing before XP evaluation. inLobby={}, stage={}.",
                xpControllerPhase == XpControllerPhase::ROLLOVER_LOOP
                    ? xpDrainBetweenRoundRejoinSeconds : 20,
                IsInLobby(), XpControllerStageName(xpControllerStage)));
            EnterSoloLobbyKeepAlive(std::chrono::steady_clock::now());
            return;
        }

        SynchronizeXpPlayers();

        if (xpDrainBonusBatchPendingStart
            && xpDrainReturnAfterFarmingBreak
            && !xpDrainFarmingBonusPlayerIds.empty()) {
            xpDrainBonusBatchPendingStart = false;
            BeginXpFarmingRound();
            return;
        }

        if (xpControllerPhase == XpControllerPhase::FARMING_ROUND) {
            if (xpProgressMode != XpProgressMode::LegacyChat
                && !xpFarmingConfirmationPending) {
                xpControllerPhase = XpControllerPhase::LOBBY_EVAL;
                xpControllerStage = XpControllerStage::EvaluateLobby;
                LogXpControllerEvent(
                    "Level-mode farming lobby delay complete; evaluating completed players.");
                return;
            }
            if (xpProgressMode == XpProgressMode::LegacyChat
                && xpLegacyFarmingRoundFailed) {
                xpLegacyFarmingRoundFailed = false;
                xpControllerPhase = XpControllerPhase::LOBBY_EVAL;
                xpControllerStage = XpControllerStage::EvaluateLobby;
                LogXpControllerEvent(
                    "Failed legacy farming round excluded the batch from completion.");
                return;
            }
            if (xpFarmingConfirmationMode) {
                BeginXpFarmingConfirmationWindow();
                return;
            }
            const auto priorCompleted = std::count_if(xpTrackedPlayers.begin(), xpTrackedPlayers.end(), [](const auto& entry) {
                return !entry.second.is_host && entry.second.has_completed_objective;
            });
            EvaluateCompletedFarmingRound();
            const auto completed = std::count_if(xpTrackedPlayers.begin(), xpTrackedPlayers.end(), [](const auto& entry) {
                return !entry.second.is_host && entry.second.has_completed_objective;
            });
            LogXpControllerEvent(std::format(
                "Farming round completion: participants={}, completedBefore={}, completedAfter={}; disconnected or pre-50% ghosted assignments remain incomplete.",
                GetConnectedNonHostCount(), priorCompleted, completed));
            if (xpDrainReturnAfterFarmingBreak) {
                ContinueXpDrainBonusBatchesOrResume();
                return;
            }
            xpControllerPhase = XpControllerPhase::LOBBY_EVAL;
            xpControllerStage = XpControllerStage::EvaluateLobby;
            return;
        }

        if (xpControllerPhase == XpControllerPhase::LOBBY_EVAL) {
            xpControllerStage = XpControllerStage::EvaluateLobby;
            LogXpControllerEvent(
                "Farming evaluation lobby delay completed; checking participant completion again.");
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

    int GetXpProgressMode() {
        return static_cast<int>(xpProgressMode);
    }

    void SetXpProgressMode(int mode) {
        xpProgressMode = static_cast<XpProgressMode>((std::clamp)(mode, 0, 2));
        xpDrainUntilStopMode = xpProgressMode == XpProgressMode::LegacyChat;
        xpFarmingConfirmationMode =
            xpProgressMode == XpProgressMode::LevelWithChatFallback;
        LogXpControllerEvent(std::format(
            "XP progress mode changed to {}.",
            xpProgressMode == XpProgressMode::LevelTracking ? "LevelTracking"
            : xpProgressMode == XpProgressMode::LevelWithChatFallback ? "LevelWithChatFallback"
            : "LegacyChat"));
    }

    void SetXpFinalRejoinWindowSeconds(int seconds) {
        xpFinalRejoinWindowSeconds = (std::clamp)(seconds, 1, 3600);
    }

    bool GetXpFarmingConfirmationMode() {
        return xpFarmingConfirmationMode;
    }

    void SetXpFarmingConfirmationMode(bool enabled) {
        xpFarmingConfirmationMode = enabled;
    }

    bool GetXpAllowFourImpostors() {
        return xpAllowFourImpostors;
    }

    void SetXpAllowFourImpostors(bool enabled) {
        xpAllowFourImpostors = enabled;
    }

    bool GetXpDrainUntilStopMode() {
        return xpDrainUntilStopMode;
    }

    void SetXpDrainUntilStopMode(bool enabled) {
        xpDrainUntilStopMode = enabled;
    }

    bool GetXpDrainFarmingPromptMode() {
        return xpDrainFarmingPromptMode;
    }

    void SetXpDrainFarmingPromptMode(bool enabled) {
        xpDrainFarmingPromptMode = enabled;
    }

    void PauseXpController() {
        if (!IsXpControllerRunning() || xpControllerPaused) return;
        xpControllerPaused = true;
        xpPauseDeadlineIsMax =
            xpControllerDeadline == (std::chrono::steady_clock::time_point::max)();
        xpPauseDeadlineRemainingMs = xpPauseDeadlineIsMax
            ? 0
            : (std::max)(int64_t{0},
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    xpControllerDeadline - std::chrono::steady_clock::now()).count());
        LogXpControllerEvent(std::format(
            "Match Cycle paused with {}ms remaining.",
            xpPauseDeadlineRemainingMs));
    }

    void ResumeXpController() {
        if (!IsXpControllerRunning() || !xpControllerPaused) return;
        const auto now = std::chrono::steady_clock::now();
        xpControllerPaused = false;
        xpControllerDeadline = xpPauseDeadlineIsMax
            ? (std::chrono::steady_clock::time_point::max)()
            : now + std::chrono::milliseconds(xpPauseDeadlineRemainingMs);
        LogXpControllerEvent(std::format(
            "Match Cycle resumed with {}ms remaining.",
            xpPauseDeadlineRemainingMs));
    }

    bool IsXpControllerPaused() {
        return xpControllerPaused;
    }

    void SaveXpControllerSnapshot() {
        xpControllerSnapshot.valid = IsXpControllerRunning();
        xpControllerSnapshot.phase = xpControllerPhase;
        xpControllerSnapshot.stage = xpControllerStage;
        xpControllerSnapshot.progressMode = static_cast<int>(xpProgressMode);
        xpControllerSnapshot.deadlineIsMax =
            xpControllerDeadline == (std::chrono::steady_clock::time_point::max)();
        xpControllerSnapshot.deadlineRemainingMs = xpControllerSnapshot.deadlineIsMax
            ? 0
            : (std::max)(int64_t{0},
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    xpControllerDeadline - std::chrono::steady_clock::now()).count());
        xpControllerSnapshot.transitionRemainingMs =
            xpTransitionGraceDeadline == std::chrono::steady_clock::time_point{}
            ? 0
            : (std::max)(int64_t{0},
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    xpTransitionGraceDeadline - std::chrono::steady_clock::now()).count());
        xpControllerSnapshot.drainRoundsCompleted = xpDrainRoundsCompleted;
        xpControllerSnapshot.drainRoundsTarget = xpDrainRoundsTarget;
        xpControllerSnapshot.drainStopRequested = xpDrainStopRequested;
        xpControllerSnapshot.drainStopEligibleCount = xpDrainStopEligibleCount;
        xpControllerSnapshot.drainStopRequiredVotes = xpDrainStopRequiredVotes;
        xpControllerSnapshot.drainStopVoterIds = xpDrainStopVoterIds;
        xpControllerSnapshot.drainFarmingPromptResponse = xpDrainFarmingPromptResponse;
        xpControllerSnapshot.drainFarmingPromptResponseWindowStarted =
            xpDrainFarmingPromptResponseWindowStarted;
        xpControllerSnapshot.drainFarmingBonusPlayerIds = xpDrainFarmingBonusPlayerIds;
        xpControllerSnapshot.drainReturnAfterFarmingBreak = xpDrainReturnAfterFarmingBreak;
        xpControllerSnapshot.drainBonusBatchPendingStart = xpDrainBonusBatchPendingStart;
        xpControllerSnapshot.drainFarmingPromptGracePending = xpDrainFarmingPromptGracePending;
        xpControllerSnapshot.farmingConfirmationPending = xpFarmingConfirmationPending;
        xpControllerSnapshot.farmingRotationCursor = xpFarmingRotationCursor;
        xpControllerSnapshot.hasFarmingRotationCursor = xpHasFarmingRotationCursor;
        xpControllerSnapshot.roundImpostorIds = xpRoundImpostorIds;
        xpControllerSnapshot.roundImpostorNames = xpRoundImpostorNames;
        xpControllerSnapshot.completedPlayerIds = xpFarmingCompletedPlayerIds;
        xpControllerSnapshot.retryPlayerIds = xpFarmingRetryPlayerIds;
        xpControllerSnapshot.confirmationResponses = xpFarmingConfirmationResponses;
        xpControllerSnapshot.roundLevelBaselines = xpRoundLevelBaselines;
        xpControllerSnapshot.roundShieldFailure = xpRoundShieldFailure;
        xpControllerSnapshot.noActiveFarmingImpostors = xpNoActiveFarmingImpostors;
        xpControllerSnapshot.roundFailureDetectedBeforeFiftyPercent =
            xpRoundFailureDetectedBeforeFiftyPercent;
        xpControllerSnapshot.ghostedBeforeFiftyPercentIds =
            xpRoundGhostedBeforeFiftyPercentIds;
        xpControllerSnapshot.allowFourImpostors = xpAllowFourImpostors;
        xpControllerSnapshot.roundShieldFailureWaived = xpRoundShieldFailureWaived;
        xpControllerSnapshot.roundStartedAt = xpRoundStartedAt;
        xpControllerSnapshot.drainFarmingMeUsedPlayerIds = xpDrainFarmingMeUsedPlayerIds;
        xpControllerSnapshot.legacyFarmingRoundFailed = xpLegacyFarmingRoundFailed;
        xpControllerSnapshot.legacyFarmingFailureStreak = xpLegacyFarmingFailureStreak;
        xpControllerSnapshot.deferredFarmingRetryIds = xpDeferredFarmingRetryIds;
        xpControllerSnapshot.deferredFarmingRetryUsedIds = xpDeferredFarmingRetryUsedIds;
        xpControllerSnapshot.deferredFarmingRetryActive = xpDeferredFarmingRetryActive;
        xpControllerSnapshot.protectedKillCounts = xpRoundProtectedKillCounts;
        xpControllerSnapshot.ghostedImpostorIds = xpRoundGhostedImpostorIds;
        xpControllerSnapshot.farmingSafetyWarningsPending =
            xpFarmingSafetyWarningsPending;
        xpControllerSnapshot.soloKeepAliveRound = xpSoloKeepAliveRound;
        xpControllerSnapshot.soloKeepAliveNeedsRoundSetup = xpSoloKeepAliveNeedsRoundSetup;
        xpControllerSnapshot.soloResumePhase = xpSoloResumePhase;
        xpControllerSnapshot.soloResumeStage = xpSoloResumeStage;
        xpControllerSnapshot.soloResumeDeadlineIsMax = xpSoloResumeDeadlineIsMax;
        xpControllerSnapshot.soloResumeDeadlineRemainingMs =
            xpSoloResumeDeadlineIsMax
            ? 0
            : (std::max)(int64_t{0},
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    xpSoloResumeDeadline - std::chrono::steady_clock::now()).count());
        NotifyXpController(
            "Match Cycle snapshot saved",
            "Pause and restore this exact controller state.",
            ImVec4(0.2f, 0.8f, 1.f, 1.f));
    }

    bool LoadXpControllerSnapshot() {
        if (!xpControllerSnapshot.valid) return false;
        xpControllerPhase = xpControllerSnapshot.phase;
        xpControllerStage = xpControllerSnapshot.stage;
        SetXpProgressMode(xpControllerSnapshot.progressMode);
        xpDrainRoundsCompleted = xpControllerSnapshot.drainRoundsCompleted;
        xpDrainRoundsTarget = xpControllerSnapshot.drainRoundsTarget;
        xpDrainStopRequested = xpControllerSnapshot.drainStopRequested;
        xpDrainStopEligibleCount = xpControllerSnapshot.drainStopEligibleCount;
        xpDrainStopRequiredVotes = xpControllerSnapshot.drainStopRequiredVotes;
        xpDrainStopVoterIds = xpControllerSnapshot.drainStopVoterIds;
        xpDrainFarmingPromptResponse = xpControllerSnapshot.drainFarmingPromptResponse;
        xpDrainFarmingPromptResponseWindowStarted =
            xpControllerSnapshot.drainFarmingPromptResponseWindowStarted;
        xpDrainFarmingBonusPlayerIds = xpControllerSnapshot.drainFarmingBonusPlayerIds;
        xpDrainReturnAfterFarmingBreak = xpControllerSnapshot.drainReturnAfterFarmingBreak;
        xpDrainBonusBatchPendingStart = xpControllerSnapshot.drainBonusBatchPendingStart;
        xpDrainFarmingPromptGracePending = xpControllerSnapshot.drainFarmingPromptGracePending;
        xpFarmingConfirmationPending = xpControllerSnapshot.farmingConfirmationPending;
        xpFarmingRotationCursor = xpControllerSnapshot.farmingRotationCursor;
        xpHasFarmingRotationCursor = xpControllerSnapshot.hasFarmingRotationCursor;
        xpRoundImpostorIds = xpControllerSnapshot.roundImpostorIds;
        xpRoundImpostorNames = xpControllerSnapshot.roundImpostorNames;
        xpFarmingCompletedPlayerIds = xpControllerSnapshot.completedPlayerIds;
        xpFarmingRetryPlayerIds = xpControllerSnapshot.retryPlayerIds;
        xpFarmingConfirmationResponses = xpControllerSnapshot.confirmationResponses;
        xpRoundLevelBaselines = xpControllerSnapshot.roundLevelBaselines;
        xpRoundShieldFailure = xpControllerSnapshot.roundShieldFailure;
        xpNoActiveFarmingImpostors =
            xpControllerSnapshot.noActiveFarmingImpostors;
        xpRoundFailureDetectedBeforeFiftyPercent =
            xpControllerSnapshot.roundFailureDetectedBeforeFiftyPercent;
        xpRoundGhostedBeforeFiftyPercentIds =
            xpControllerSnapshot.ghostedBeforeFiftyPercentIds;
        xpAllowFourImpostors = xpControllerSnapshot.allowFourImpostors;
        xpRoundShieldFailureWaived = xpControllerSnapshot.roundShieldFailureWaived;
        xpRoundStartedAt = xpControllerSnapshot.roundStartedAt;
        xpDrainFarmingMeUsedPlayerIds =
            xpControllerSnapshot.drainFarmingMeUsedPlayerIds;
        xpLegacyFarmingRoundFailed =
            xpControllerSnapshot.legacyFarmingRoundFailed;
        xpLegacyFarmingFailureStreak =
            xpControllerSnapshot.legacyFarmingFailureStreak;
        xpDeferredFarmingRetryIds =
            xpControllerSnapshot.deferredFarmingRetryIds;
        xpDeferredFarmingRetryUsedIds =
            xpControllerSnapshot.deferredFarmingRetryUsedIds;
        xpDeferredFarmingRetryActive =
            xpControllerSnapshot.deferredFarmingRetryActive;
        xpRoundProtectedKillCounts = xpControllerSnapshot.protectedKillCounts;
        xpRoundGhostedImpostorIds = xpControllerSnapshot.ghostedImpostorIds;
        xpFarmingSafetyWarningsPending =
            xpControllerSnapshot.farmingSafetyWarningsPending;
        xpSoloKeepAliveRound = xpControllerSnapshot.soloKeepAliveRound;
        xpSoloKeepAliveNeedsRoundSetup =
            xpControllerSnapshot.soloKeepAliveNeedsRoundSetup;
        xpSoloResumePhase = xpControllerSnapshot.soloResumePhase;
        xpSoloResumeStage = xpControllerSnapshot.soloResumeStage;
        xpSoloResumeDeadlineIsMax = xpControllerSnapshot.soloResumeDeadlineIsMax;
        for (const auto playerId : xpRoundGhostedImpostorIds) {
            const auto tracked = xpTrackedPlayers.find(playerId);
            if (tracked == xpTrackedPlayers.end()) continue;
            tracked->second.is_impostor = false;
            tracked->second.demoted_to_ghost_this_round = true;
        }
        const auto now = std::chrono::steady_clock::now();
        xpControllerDeadline = xpControllerSnapshot.deadlineIsMax
            ? (std::chrono::steady_clock::time_point::max)()
            : now + std::chrono::milliseconds(xpControllerSnapshot.deadlineRemainingMs);
        xpSoloResumeDeadline = xpSoloResumeDeadlineIsMax
            ? (std::chrono::steady_clock::time_point::max)()
            : now + std::chrono::milliseconds(
                xpControllerSnapshot.soloResumeDeadlineRemainingMs);
        xpSoloKeepAliveStartedAt =
            xpControllerStage == XpControllerStage::SoloLobbyKeepAlive
            ? now : std::chrono::steady_clock::time_point{};
        xpTransitionGraceDeadline =
            now + std::chrono::milliseconds(xpControllerSnapshot.transitionRemainingMs);
        xpControllerPaused = false;
        NotifyXpController(
            "Match Cycle snapshot loaded",
            "The saved phase, round, and player state were restored.",
            ImVec4(0.2f, 0.8f, 1.f, 1.f));
        return true;
    }

    void StartXpLevelRecorder() {
        xpLevelRecorderRunning = true;
        xpLevelRejoinDeadline = {};
        for (auto player : GetConnectedXpPlayers()) {
            const auto playerId = player->fields.PlayerId;
            auto data = GetPlayerData(player);
            if (data == nullptr) continue;
            auto& baseline = xpLevelBaselines.try_emplace(playerId).first->second;
            baseline.originalLevel = data->fields.PlayerLevel + 1;
            baseline.lastLevel = baseline.originalLevel;
            baseline.highestObservedLevel = baseline.originalLevel;
            baseline.patchDetected = false;
            baseline.rejoinRequested = false;
            baseline.name = XpPlayerName(player);
        }
        NotifyXpController(
            "Level recorder started",
            "Baselines will be checked after each completed Match Cycle round.",
            ImVec4(0.2f, 0.8f, 1.f, 1.f));
    }

    void ClearXpLevelRecorder() {
        xpLevelRecorderRunning = false;
        xpLevelBaselines.clear();
        xpLevelRejoinDeadline = {};
        NotifyXpController(
            "Level recorder cleared",
            "All saved level baselines were removed.",
            ImVec4(0.2f, 0.8f, 1.f, 1.f));
    }

    bool IsXpLevelRecorderRunning() {
        return xpLevelRecorderRunning;
    }

    static void RecordConnectedXpLevels() {
        if (!xpLevelRecorderRunning) return;
        for (auto player : GetConnectedXpPlayers()) {
            if (IsMatchCycleHost(player)) continue;
            const auto playerId = player->fields.PlayerId;
            auto data = GetPlayerData(player);
            if (data == nullptr) continue;
            const uint32_t level = data->fields.PlayerLevel + 1;
            auto it = xpLevelBaselines.find(playerId);
            if (it == xpLevelBaselines.end()) {
                xpLevelBaselines[playerId] = {
                    level,
                    level,
                    level,
                    false,
                    false,
                    XpPlayerName(player),
                };
            }
            else {
                it->second.lastLevel = level;
                it->second.highestObservedLevel =
                    (std::max)(it->second.highestObservedLevel, level);
                it->second.name = XpPlayerName(player);
            }
        }
    }

    static void CheckXpLevelPatchAfterRound() {
        if (!xpLevelRecorderRunning) return;
        RecordConnectedXpLevels();

        std::vector<std::string> patchedNames;
        for (auto& [playerId, baseline] : xpLevelBaselines) {
            if (baseline.rejoinRequested || baseline.patchDetected) continue;
            auto player = GetPlayerControlById(playerId);
            if (player == nullptr) continue;
            auto data = GetPlayerData(player);
            if (data == nullptr) continue;
            const uint32_t currentLevel = data->fields.PlayerLevel + 1;
            baseline.lastLevel = currentLevel;
            if (baseline.originalLevel != 0
                && currentLevel >= baseline.originalLevel
                && currentLevel < baseline.highestObservedLevel) {
                baseline.patchDetected = true;
                baseline.rejoinRequested = true;
                patchedNames.push_back(baseline.name);
                LogXpControllerEvent(std::format(
                    "Level patch rollback detected after round: playerId={}, name='{}', original={}, highestObserved={}, current={}.",
                    playerId, baseline.name, baseline.originalLevel,
                    baseline.highestObservedLevel, currentLevel));
            }
        }

        if (!patchedNames.empty()) {
            std::string names = patchedNames.front();
            for (size_t i = 1; i < patchedNames.size(); ++i) names += ", " + patchedNames[i];
            QueueXpControllerChat(std::format(
                "LEVEL PATCH DETECTED: {}. Rejoin within 30 seconds.",
                names));
            xpLevelRejoinDeadline = std::chrono::steady_clock::now()
                + std::chrono::seconds(30);
        }
    }

    std::string GetXpLevelRecorderStatus() {
        if (!xpLevelRecorderRunning) return "Level recorder: OFF";
        size_t patched = 0;
        size_t awaiting = 0;
        for (const auto& [playerId, baseline] : xpLevelBaselines) {
            if (baseline.rejoinRequested || baseline.patchDetected) {
                ++patched;
            }
            else if (baseline.originalLevel != 0) {
                ++awaiting;
            }
        }
        return std::format(
            "Level recorder: ON | {} tracked | {} patch detected | {} awaiting",
            xpLevelBaselines.size(), patched, awaiting);
    }

    void OnXpControllerChatMessage(Game::PlayerId playerId, const std::string& message) {
        if (!IsXpControllerRunning() || !IsHost()) return;

        const auto first = message.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return;
        const auto last = message.find_last_not_of(" \t\r\n");
        std::string command = message.substr(first, last - first + 1);
        std::transform(command.begin(), command.end(), command.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });

        if (command == "done" || command == "retry" || command == "me"
            || command == "stop" || command == "continue") {
            LogXpControllerEvent(std::format(
                "Chat command observed: playerId={}, command={}, phase={}, stage={}, inLobby={}, inMeeting={}.",
                playerId, command, XpControllerPhaseName(xpControllerPhase),
                XpControllerStageName(xpControllerStage), IsInLobby(), State.InMeeting));
        }

        if (xpControllerPhase == XpControllerPhase::FARMING_ROUND
            && IsInLobby()
            && (command == "done" || command == "retry")) {
            const auto tracked = xpTrackedPlayers.find(playerId);
            const std::string playerName = tracked == xpTrackedPlayers.end()
                ? std::format("Player {}", playerId)
                : XpControllerChatName(tracked->second.name);
            const bool assigned = std::find(
                xpRoundImpostorIds.begin(), xpRoundImpostorIds.end(), playerId)
                != xpRoundImpostorIds.end();
            if (!assigned) {
                LogXpControllerEvent(std::format(
                    "Farming confirmation ignored: playerId={} is not in the assigned round roster; response={}.",
                    playerId, command));
                QueueXpControllerChat(std::format(
                    "{}: {} ignored; not assigned this batch.",
                    playerName, command == "done" ? "DONE" : "RETRY"));
                return;
            }

            const bool confirmationStage =
                xpControllerStage == XpControllerStage::LobbyDelay
                || xpControllerStage == XpControllerStage::FarmingConfirmations;
            if (!xpFarmingConfirmationPending || !confirmationStage) {
                LogXpControllerEvent(std::format(
                    "Farming confirmation not active: playerId={}, response={}, stage={}, assigned={}.",
                    playerId, command, XpControllerStageName(xpControllerStage), assigned));
                QueueXpControllerChat(std::format(
                    "{}: wait for the farming confirmation prompt.",
                    playerName));
                return;
            }

            if (xpControllerStage == XpControllerStage::FarmingConfirmations
                && xpControllerDeadline != (std::chrono::steady_clock::time_point::max)()
                && std::chrono::steady_clock::now() >= xpControllerDeadline) {
                LogXpControllerEvent(std::format(
                    "Late farming confirmation ignored: playerId={}, response={}.",
                    playerId, command));
                QueueXpControllerChat(std::format(
                    "{}: response window closed.",
                    playerName));
                return;
            }

            const auto existingResponse = xpFarmingConfirmationResponses.find(playerId);
            if (existingResponse != xpFarmingConfirmationResponses.end()) {
                LogXpControllerEvent(std::format(
                    "Duplicate farming confirmation ignored: playerId={}, firstResponse={}, duplicateResponse={}.",
                    playerId, existingResponse->second ? "DONE" : "RETRY", command));
                QueueXpControllerChat(std::format(
                    "{}: already recorded {}; ignored.",
                    playerName, existingResponse->second ? "DONE" : "RETRY"));
                return;
            }

            const bool completed = command == "done";
            xpFarmingConfirmationResponses[playerId] = completed;
            if (completed) xpFarmingCompletedPlayerIds.insert(playerId);
            else xpFarmingCompletedPlayerIds.erase(playerId);
            if (tracked != xpTrackedPlayers.end()) {
                tracked->second.has_earned_xp = completed;
                tracked->second.has_completed_objective = completed;
            }
            if (completed) xpFarmingRetryPlayerIds.erase(playerId);
            else xpFarmingRetryPlayerIds.insert(playerId);
            LogXpControllerEvent(std::format(
                "Farming confirmation received: playerId={}, response={}.",
                playerId, completed ? "DONE" : "RETRY"));
            QueueXpControllerChat(std::format(
                "{}: {} received. {}",
                playerName,
                completed ? "DONE" : "RETRY",
                completed ? "Marked complete." : "Another farming turn will be assigned."));
            return;
        }

        if (xpControllerStage == XpControllerStage::DrainFarmingPrompt
            && xpControllerPhase == XpControllerPhase::ROLLOVER_LOOP
            && IsInLobby()
            && command == "me") {
            if (!xpDrainFarmingPromptResponseWindowStarted) {
                QueueXpControllerChat("ME offer is not open yet.");
                return;
            }
            if (std::chrono::steady_clock::now() >= xpControllerDeadline) {
                QueueXpControllerChat("ME window closed. Continuing drain.");
                return;
            }
            const auto tracked = xpTrackedPlayers.find(playerId);
            if (tracked == xpTrackedPlayers.end()
                || tracked->second.is_host
                || !tracked->second.connected) return;
            if (xpDrainFarmingMeUsedPlayerIds.contains(playerId)) {
                QueueXpControllerChat(
                    "ME rejected: you already used your one bonus farming round.");
                LogXpControllerEvent(std::format(
                    "ME rejected for playerId={} because their one-time bonus round was already used.",
                    playerId));
                return;
            }
            xpDrainFarmingPromptResponse = true;
            xpDrainFarmingBonusPlayerIds.push_back(playerId);
            xpDrainFarmingMeUsedPlayerIds.insert(playerId);
            const std::string playerName = XpControllerChatName(tracked->second.name);
            QueueXpControllerChat(std::format(
                "{}: ME accepted. {} player(s) queued for bonus farming.",
                playerName,
                xpDrainFarmingBonusPlayerIds.size()));
            LogXpControllerEvent(std::format(
                "ME received from playerId={} for a one-time bonus farming round; accepted requesters={}.",
                playerId, xpDrainFarmingBonusPlayerIds.size()));
            return;
        }

        if (xpDrainUntilStopMode && xpControllerStage == XpControllerStage::MatchTimer
            && xpControllerPhase == XpControllerPhase::ROLLOVER_LOOP
            && State.InMeeting) {
            if (command == "stop") {
                const auto tracked = xpTrackedPlayers.find(playerId);
                const auto connectedPlayers = GetConnectedXpPlayers();
                const bool eligibleVoter = tracked != xpTrackedPlayers.end()
                    && !tracked->second.is_host
                    && std::any_of(
                        connectedPlayers.begin(), connectedPlayers.end(),
                        [playerId](PlayerControl* player) {
                            return player->fields.PlayerId == playerId;
                        });
                if (!eligibleVoter) {
                    LogXpControllerEvent(std::format(
                        "Drain STOP vote ignored for ineligible playerId={}.",
                        playerId));
                    return;
                }

                const bool inserted = xpDrainStopVoterIds.insert(playerId).second;
                const size_t voteCount = xpDrainStopVoterIds.size();
                if (!inserted) {
                    xpPendingInGameChatMessages.push_back(std::format(
                        "STOP already counted: {}/{}.",
                        voteCount, xpDrainStopRequiredVotes));
                    return;
                }

                LogXpControllerEvent(std::format(
                    "Drain STOP vote recorded: playerId={}, votes={}/{}, round={}.",
                    playerId, voteCount, xpDrainStopRequiredVotes,
                    xpDrainRoundsCompleted + 1));
                if (!xpDrainStopRequested
                    && voteCount >= xpDrainStopRequiredVotes) {
                    xpDrainStopRequested = true;
                    xpPendingInGameChatMessages.push_back(std::format(
                        "STOP {}/{}. Majority reached; ending after this round.",
                        voteCount, xpDrainStopRequiredVotes));
                    NotifyXpController(
                        "Drain stop approved",
                        std::format("{}/{} votes. Ending after this round.",
                            voteCount, xpDrainStopRequiredVotes),
                        ImVec4(1.f, 0.7f, 0.2f, 1.f));
                }
                else {
                    xpPendingInGameChatMessages.push_back(std::format(
                        "STOP {}/{}.",
                        voteCount, xpDrainStopRequiredVotes));
                }
            }
        }
    }

    bool ShouldHoldXpDrainMeeting() {
        return IsXpControllerRunning()
            && IsHost()
            && xpDrainUntilStopMode
            && xpControllerPhase == XpControllerPhase::ROLLOVER_LOOP
            && xpControllerStage == XpControllerStage::MatchTimer
            && State.InMeeting;
    }

    static void ReconcileXpRoundRolesAtStart() {
        if (xpControllerPhase != XpControllerPhase::FARMING_ROUND) return;

        SynchronizeXpPlayers();
        std::vector<Game::PlayerId> actualImpostorIds;
        size_t correctedRoleCount = 0;
        for (auto player : GetConnectedXpPlayers()) {
            const auto playerId = player->fields.PlayerId;
            const bool expectedImpostor = std::find(
                xpRoundImpostorIds.begin(), xpRoundImpostorIds.end(), playerId)
                != xpRoundImpostorIds.end();
            auto data = GetPlayerData(player);
            if (data == nullptr) continue;

            const bool actualImpostor = PlayerIsImpostor(data);
            if (actualImpostor) actualImpostorIds.push_back(playerId);
            if (actualImpostor != expectedImpostor) {
                LogXpControllerEvent(std::format(
                    "Correcting round role mismatch: playerId={}, expected={}, actual={}.",
                    playerId,
                    expectedImpostor ? "Impostor" : "Crewmate",
                    actualImpostor ? "Impostor" : "Crewmate"));
                PlayerControl_RpcSetRole(
                    player,
                    expectedImpostor ? RoleTypes__Enum::Impostor : RoleTypes__Enum::Crewmate,
                    false,
                    NULL);
                ++correctedRoleCount;
            }

            auto tracked = xpTrackedPlayers.find(playerId);
            if (tracked != xpTrackedPlayers.end()) {
                tracked->second.is_impostor = expectedImpostor;
            }
        }

        LogXpControllerEvent(std::format(
            "Verified round role roster: expectedImpostorIds={}, observedImpostorIds={}, correctedRoles={}.",
            xpRoundImpostorIds.size(), actualImpostorIds.size(), correctedRoleCount));
    }

    void StartXpController() {
        if (IsXpControllerRunning()) return;
        if (!IsInLobby() || !IsHost()) {
            Toasts::AddToast("XP Controller", "Start this controller from a host lobby.", ImVec4(1.f, 0.3f, 0.3f, 1.f));
            return;
        }
        if (xpDrainUntilStopMode && State.DisableMeetings) {
            Toasts::AddToast(
                "XP Controller",
                "Turn off Disable Meetings to use drain-until-STOP mode.",
                ImVec4(1.f, 0.3f, 0.3f, 1.f));
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
        xpFarmingCompletedPlayerIds.clear();
        xpFarmingRetryPlayerIds.clear();
        xpDrainFarmingMeUsedPlayerIds.clear();
        xpRoundImpostorIds.clear();
        xpLegacyFarmingFailureStreak.clear();
        xpDeferredFarmingRetryIds.clear();
        xpDeferredFarmingRetryUsedIds.clear();
        xpDeferredFarmingRetryActive = false;
        xpRoundImpostorNames.clear();
        xpFarmingConfirmationResponses.clear();
        xpPendingChatMessages.clear();
        xpRoundChatWait = std::chrono::milliseconds::zero();
        xpRoundChatMessageCount = 0;
        xpRoundChatPostDelaySeconds = 0;
        xpNextChatSendAt = std::chrono::steady_clock::now();
        xpPostChatDeadline = std::chrono::steady_clock::now();
        xpDrainRoundsCompleted = 0;
        xpDrainRoundsTarget = 0;
        xpDrainStopRequested = false;
        xpDrainStopEligibleCount = 0;
        xpDrainStopRequiredVotes = 0;
        xpDrainStopVoterIds.clear();
        xpDrainRejoinGracePending = false;
        xpDrainRejoinNoticePendingStart = false;
        xpDrainMeetingAnnouncementPending = false;
        xpPendingInGameChatMessages.clear();
        xpRoundLevelBaselines.clear();
        xpRoundShieldFailure = false;
        xpRoundShieldFailureWaived = false;
        xpRoundFailureDetectedBeforeFiftyPercent = false;
        xpRoundStartedAt = {};
        xpFarmingConfirmationPending = false;
        xpDrainFarmingPromptResponse = false;
        xpDrainFarmingPromptResponseWindowStarted = false;
        xpDrainFarmingBonusPlayerIds.clear();
        xpDrainReturnAfterFarmingBreak = false;
        xpDrainBonusBatchPendingStart = false;
        xpDrainFarmingPromptGracePending = false;
        xpSoloKeepAliveRound = false;
        xpSoloKeepAliveNeedsRoundSetup = false;
        xpSoloResumePhase = XpControllerPhase::STOPPED;
        xpSoloResumeStage = XpControllerStage::PreparingRound;
        xpSoloResumeDeadlineIsMax = false;
        xpSoloResumeDeadline = {};
        xpSoloKeepAliveStartedAt = {};
        xpControllerPaused = false;
        xpPauseDeadlineRemainingMs = 0;
        xpControllerSnapshot.valid = false;
        xpHasFarmingRotationCursor = false;
        xpFarmingCycleAnnouncementPending = true;
        State.ForceEndAfterStartTriggered = false;
        for (auto player : GetConnectedXpPlayers()) {
            auto data = GetPlayerData(player);
            auto& tracked = xpTrackedPlayers[player->fields.PlayerId];
            tracked.name = XpPlayerName(player);
            tracked.is_host = IsMatchCycleHost(player);
            tracked.connected = true;
            if (data != nullptr) {
                if (data->fields.Puid != nullptr) {
                    tracked.puid = convert_from_string(data->fields.Puid);
                }
                if (data->fields.FriendCode != nullptr) {
                    tracked.friend_code = convert_from_string(data->fields.FriendCode);
                }
            }
        }
        if (xpTrackedPlayers.empty()) {
            Toasts::AddToast("XP Controller", "No connected players are available.", ImVec4(1.f, 0.3f, 0.3f, 1.f));
            return;
        }
        xpControllerPreviousNoGameEnd = State.NoGameEnd;
        xpControllerOwnsNoGameEnd = true;
        State.NoGameEnd = true;
        xpControllerPhase = XpControllerPhase::FARMING_ROUND;
        xpControllerStage = XpControllerStage::PreparingRound;
        xpControllerDeadline = std::chrono::steady_clock::now();
        xpLastRosterCheck = std::chrono::steady_clock::now();
        xpTransitionGraceDeadline = xpLastRosterCheck;
        xpHostAuthorityLostAt = {};
        LogXpControllerEvent(std::format("Match Cycle started. {}", BuildXpControllerDiagnostics()));
        BeginXpFarmingRound();
        Toasts::AddToast(
            "XP Controller",
            GetConnectedNonHostCount() == 0
                ? "Waiting for players; solo lobby keep-alive is enabled."
                : "Starting the farming rounds.",
            ImVec4(0.f, 1.f, 0.5f, 1.f));
    }

    void StopXpController() {
        StopXpController("Stopped.");
    }

    std::string GetXpControllerStatus() {
        if (!IsXpControllerRunning()) return "Stopped";
        if (xpControllerPaused) return "MATCH CYCLE PAUSED";
        if (xpControllerStage == XpControllerStage::AwaitingRejoin
            && xpDrainRejoinNoticePendingStart) {
            return "Sending drain-ended rejoin notice";
        }
        if (xpControllerStage == XpControllerStage::FarmingConfirmations
            && xpControllerDeadline == (std::chrono::steady_clock::time_point::max)()) {
            return "Sending farming confirmation prompt";
        }
        if (xpControllerStage == XpControllerStage::DrainFarmingPrompt) {
            if (!xpDrainFarmingPromptResponseWindowStarted) {
                return "Sending farming offer";
            }
            const auto responseSeconds = (std::max)(0LL,
                std::chrono::duration_cast<std::chrono::seconds>(
                    xpControllerDeadline - std::chrono::steady_clock::now()).count());
            return std::format("Waiting for ME - {}s", responseSeconds);
        }
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
        case XpControllerPhase::ROLLOVER_LOOP:
            phaseName = xpProgressMode == XpProgressMode::LegacyChat
                ? (xpDrainUntilStopMode ? "Drain sequence" : "Fixed drain sequence")
                : "Level drain sequence";
            break;
        case XpControllerPhase::STOPPED: break;
        }
        size_t trackedCount = 0;
        size_t disconnectedCount = 0;
        size_t needingRejoinCount = 0;
        for (const auto& [playerId, tracked] : xpTrackedPlayers) {
            if (tracked.is_host) continue;
            ++trackedCount;
            if (!tracked.connected) ++disconnectedCount;
            if (tracked.needs_rejoin) ++needingRejoinCount;
        }
        const std::string rosterStatus = std::format(
            " | {} tracked / {} disconnected / {} needing rejoin",
            trackedCount, disconnectedCount, needingRejoinCount);
        if (xpControllerStage == XpControllerStage::SoloLobbyKeepAlive) {
            return std::format(
                "Waiting for participants - solo lobby keep-alive in {}s{}",
                remaining, rosterStatus);
        }
        if (xpControllerPhase == XpControllerPhase::ROLLOVER_LOOP) {
            if (xpControllerStage == XpControllerStage::AwaitingRejoin) {
                return std::format("Drain rounds complete - rejoin window - {}s{}", remaining, rosterStatus);
            }
            if (xpProgressMode != XpProgressMode::LegacyChat) {
                return std::format(
                    "{} - round {} (level-based) - {}s{}",
                    phaseName, xpDrainRoundsCompleted + 1, remaining, rosterStatus);
            }
            if (xpDrainUntilStopMode) {
                return std::format(
                    "{} - round {} (until STOP) - {}s{}",
                    phaseName, xpDrainRoundsCompleted + 1, remaining, rosterStatus);
            }
            return std::format(
                "{} - round {}/{} - {}s{}",
                phaseName, xpDrainRoundsCompleted + 1, xpDrainRoundsTarget, remaining, rosterStatus);
        }
        return std::format("{} - {}s{}", phaseName, remaining, rosterStatus);
    }

    void TickXpController() {
        g_rpcEmittedThisFrame = 0;
        RecordConnectedXpLevels();
        if (xpLevelRejoinDeadline != std::chrono::steady_clock::time_point{}
            && std::chrono::steady_clock::now() >= xpLevelRejoinDeadline) {
            xpLevelRejoinDeadline = {};
            size_t waitingCount = 0;
            for (auto& [playerId, baseline] : xpLevelBaselines) {
                if (!baseline.rejoinRequested) continue;
                baseline.rejoinRequested = false;
                ++waitingCount;
            }
            if (waitingCount > 0 && IsInLobby()) {
                QueueXpControllerChat(std::format(
                    "LEVEL PATCH rejoin window ended. {} player(s) not confirmed.",
                    waitingCount));
            }
            LogXpControllerEvent(std::format(
                "Level patch rejoin window ended for {} player(s).",
                waitingCount));
        }
        if (!IsXpControllerRunning()) return;
        if (State.PanicMode) {
            StopXpController("Stopped because panic mode was enabled.");
            return;
        }
        if (xpControllerPaused) return;
        State.NoGameEnd = true;
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

        if (xpControllerStage == XpControllerStage::SoloLobbyKeepAlive) {
            if (!inLobby) return;
            if (GetConnectedNonHostCount() > 0) {
                ResumeFromSoloLobbyKeepAlive(now);
            }
            else if (now >= xpControllerDeadline) {
                StartSoloLobbyKeepAliveMatch();
                return;
            }
        }
        else if (inLobby && !transitioningToLobby
            && GetConnectedNonHostCount() == 0) {
            EnterSoloLobbyKeepAlive(now);
            return;
        }
        if (!transitioningToLobby && !lobbyDelayPending) {
            SynchronizeXpPlayers();
        }
        if (now - xpLastRosterCheck >= GetMatchCycleUpdateInterval()) {
            if (!transitioningToLobby && !lobbyDelayPending) {
                xpLastRosterCheck = now;
                ReconcileMatchCycleShields();
            }
        }
        if (xpPendingGhostRoleQueueTicks > 0) {
            --xpPendingGhostRoleQueueTicks;
            return;
        }
        if (xpControllerPhase == XpControllerPhase::FARMING_ROUND
            && xpControllerStage == XpControllerStage::MatchTimer
            && !xpRoundImpostorIds.empty()
            && CountActiveXpFarmingImpostors() == 0) {
            if (!xpNoActiveFarmingImpostors) {
                LogXpControllerEvent(
                    "No active farming Impostors remain; latching an early round end.");
            }
            xpNoActiveFarmingImpostors = true;
            xpRoundShieldFailure = true;
        }
        switch (xpControllerStage) {
        case XpControllerStage::PreparingRound:
            if (IsInLobby() && now >= xpControllerDeadline) StartPreparedXpMatch();
            break;
        case XpControllerStage::AwaitingMatch:
            if (xpSoloKeepAliveRound && IsInGame()) {
                xpControllerStage = XpControllerStage::MatchTimer;
                xpRoundStartedAt = now;
                xpControllerDeadline = now;
                LogXpControllerEvent(
                    "Host-only keep-alive game loaded; ending it immediately.");
                ForceEndXpMatch();
            }
            else if (now >= xpControllerDeadline && xpSoloKeepAliveRound) {
                LogXpControllerEvent(
                    "Host-only keep-alive game did not start before timeout; retrying in five minutes.");
                ScheduleNextSoloKeepAlive(now);
            }
            else if (now >= xpControllerDeadline) {
                StopXpController("The match did not start before the timeout.");
            }
            break;
        case XpControllerStage::MatchTimer:
            if (xpSoloKeepAliveRound && IsInLobby()) {
                ScheduleNextSoloKeepAlive(now);
            }
            else if (xpSoloKeepAliveRound) {
                ForceEndXpMatch();
            }
            else if (IsInLobby()) {
                HandleXpRoundReturnedToLobby();
            }
            else if (xpNoActiveFarmingImpostors || xpRoundShieldFailure) {
                if (HasXpRoundPassedFiftyPercent()
                    && !xpRoundFailureDetectedBeforeFiftyPercent) {
                    xpRoundShieldFailureWaived = true;
                    if (!xpNoActiveFarmingImpostors) {
                        xpRoundShieldFailure = false;
                    }
                    LogXpControllerEvent(std::format(
                        "{} waived because the round passed 50% ({}/{}s); ending normally.",
                        xpNoActiveFarmingImpostors
                            ? "No active farming Impostors"
                            : "No living Crewmate targets remain to protect",
                        std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - xpRoundStartedAt).count(),
                        xpControllerPhase == XpControllerPhase::FARMING_ROUND
                            ? xpFarmingDurationSeconds : static_cast<int>(xpRolloverDuration.count())));
                    ForceEndXpMatch();
                    break;
                }
                if (xpProgressMode == XpProgressMode::LegacyChat
                    && xpNoActiveFarmingImpostors) {
                    xpLegacyFarmingRoundFailed = true;
                }
                LogXpControllerEvent(
                    xpNoActiveFarmingImpostors
                        ? "No active farming Impostors remain; force-ending round early."
                        : "No living Crewmate targets remain to protect; force-ending round early.");
                ForceEndXpMatch();
            }
            else if (now >= xpControllerDeadline) {
                ForceEndXpMatch();
            }
            break;
        case XpControllerStage::AwaitingLobby:
            if (IsInLobby()) {
                if (xpSoloKeepAliveRound) {
                    ScheduleNextSoloKeepAlive(now);
                }
                else {
                    HandleXpRoundReturnedToLobby();
                }
            }
            else if (now >= xpControllerDeadline) {
                StopXpController("The lobby did not return before the timeout.");
            }
            break;
        case XpControllerStage::AwaitingRejoin:
            if (xpDrainRejoinNoticePendingStart
                && xpPendingChatMessages.empty()
                && now >= xpPostChatDeadline) {
                xpDrainRejoinNoticePendingStart = false;
                xpControllerDeadline = now + std::chrono::seconds(xpFinalRejoinWindowSeconds);
                LogXpControllerEvent(std::format(
                    "Starting the configured {}s rejoin window after the drain-ended notice was sent.",
                    xpFinalRejoinWindowSeconds));
            }
            else if (!xpDrainRejoinNoticePendingStart && now >= xpControllerDeadline) {
                LogXpControllerEvent(std::format(
                    "Fixed rejoin window ended: allRequestedRejoined={}, elapsed={}s. {}",
                    AllRequestedPlayersRejoined(), xpFinalRejoinWindowSeconds,
                    BuildXpControllerDiagnostics()));
                StartNextXpCycle();
            }
            break;
        case XpControllerStage::LobbyDelay:
            if (now >= xpControllerDeadline) {
                ProcessXpLobbyDelay();
            }
            break;
        case XpControllerStage::FarmingConfirmations:
            if (xpControllerDeadline == (std::chrono::steady_clock::time_point::max)()
                && xpPendingChatMessages.empty() && now >= xpPostChatDeadline) {
                xpRoundChatWait = std::chrono::milliseconds::zero();
                xpRoundChatMessageCount = 0;
                xpRoundChatPostDelaySeconds = 0;
                xpControllerDeadline = now + std::chrono::seconds(30);
                LogXpControllerEvent("Farming confirmation response window started (30 seconds).");
            }
            else if (now >= xpControllerDeadline) {
                ProcessXpFarmingConfirmations();
            }
            break;
        case XpControllerStage::DrainFarmingPrompt:
            if (xpDrainFarmingPromptResponseWindowStarted
                && now >= xpControllerDeadline) {
                ProcessXpDrainFarmingPrompt();
            }
            break;
        case XpControllerStage::SoloLobbyKeepAlive:
            break;
        case XpControllerStage::EvaluateLobby: {
            if (xpDrainReturnAfterFarmingBreak
                && !xpDrainFarmingBonusPlayerIds.empty()) {
                LogXpControllerEvent(std::format(
                    "Starting the next accepted ME bonus batch; {} requester(s) remain.",
                    xpDrainFarmingBonusPlayerIds.size()));
                BeginXpFarmingRound();
                break;
            }
            const bool allParticipantsComplete = AllTrackedPlayersEarnedXp();
            LogXpControllerEvent(std::format(
                "Evaluating farming results: allTrackedNonHostObjectivesComplete={}. {}",
                allParticipantsComplete, BuildXpControllerDiagnostics()));
            if (allParticipantsComplete) {
                xpDrainRoundsCompleted = 0;
                xpDrainRoundsTarget = GetXpDrainRoundsTarget();
                xpControllerPhase = XpControllerPhase::ROLLOVER_LOOP;
                if (xpProgressMode != XpProgressMode::LegacyChat) {
                    QueueXpControllerChat(
                        "Level draining starts. Rounds continue until level gains stop.",
                        std::chrono::seconds::zero());
                    LogXpControllerEvent(
                        "Level-based draining started with no fixed round limit.");
                }
                else if (xpDrainUntilStopMode) {
                    QueueXpControllerChat(
                        "Draining starts in 30s. Type STOP only when you are done.",
                        std::chrono::seconds::zero());
                }
                else {
                    QueueXpControllerChat(std::format(
                        "Auto-drain starts in 30s: {} rounds, {}s between; each ends after 35s.",
                        xpDrainRoundsTarget, xpDrainBetweenRoundRejoinSeconds),
                        std::chrono::seconds::zero());
                }
                if (xpProgressMode == XpProgressMode::LegacyChat) {
                    LogXpControllerEvent(std::format(
                        "All tracked non-host participants completed farming; beginning {} drain mode (target={} rounds, {} per farming minute, farmingDuration={}s). {}",
                        xpDrainUntilStopMode ? "STOP-controlled" : "fixed-count",
                        xpDrainRoundsTarget, xpDrainRoundsPerMinute, xpFarmingDurationSeconds,
                        BuildXpControllerDiagnostics()));
                    NotifyXpController(
                        "Auto-drain starting",
                        xpDrainUntilStopMode
                            ? "Until STOP is requested; protection off."
                            : std::format("{} rounds; protection off.", xpDrainRoundsTarget),
                        ImVec4(1.f, 0.7f, 0.2f, 1.f));
                }
                else {
                    LogXpControllerEvent(std::format(
                        "All connected non-host participants completed level-based farming; beginning continuous level drain. {}",
                        BuildXpControllerDiagnostics()));
                    NotifyXpController(
                        "Level drain starting",
                        "Protection off; continuous until level gains stop.",
                        ImVec4(1.f, 0.7f, 0.2f, 1.f));
                }
                BeginXpRolloverRound();
            }
            else {
                BroadcastXpControllerSummary();
                const auto connectedPlayers = GetConnectedXpPlayers();
                const bool connectedParticipantNeedsFarming = std::any_of(
                    connectedPlayers.begin(), connectedPlayers.end(), [](PlayerControl* player) {
                        if (IsMatchCycleHost(player)) return false;
                        const auto tracked = xpTrackedPlayers.find(player->fields.PlayerId);
                        return tracked == xpTrackedPlayers.end()
                            || (!xpFarmingCompletedPlayerIds.contains(player->fields.PlayerId)
                                && !tracked->second.has_completed_objective);
                    });
                if (connectedParticipantNeedsFarming) {
                    BeginXpFarmingRound();
                }
                else {
                    xpControllerStage = XpControllerStage::LobbyDelay;
                    xpControllerDeadline = now + xpFarmingLobbyGraceDuration;
                    LogXpControllerEvent(
                        "All connected participants are complete; waiting for disconnected unfinished participants to rejoin before rollover.");
                }
            }
            break;
        }
        }
    }

    void OnXpControllerGameStarted() {
        if (!IsXpControllerRunning() || !IsHost()
            || xpControllerStage != XpControllerStage::AwaitingMatch) return;

        if (xpSoloKeepAliveRound) {
            xpControllerStage = XpControllerStage::MatchTimer;
            xpRoundStartedAt = std::chrono::steady_clock::now();
            xpControllerDeadline = xpRoundStartedAt;
            LogXpControllerEvent(
                "Host-only keep-alive game loaded; the controller will end it on the next update.");
            return;
        }

        ReconcileXpRoundRolesAtStart();
        const int matchDurationSeconds = xpControllerPhase == XpControllerPhase::FARMING_ROUND
            ? xpFarmingDurationSeconds : static_cast<int>(xpRolloverDuration.count());
        const auto messageWait = xpControllerPhase == XpControllerPhase::ROLLOVER_LOOP
            ? std::chrono::milliseconds::zero() : xpRoundChatWait;
        const auto roundDuration = std::chrono::seconds(matchDurationSeconds) + messageWait;
        xpRoundChatWait = std::chrono::milliseconds::zero();
        xpRoundChatMessageCount = 0;
        xpRoundChatPostDelaySeconds = 0;
        xpControllerStage = XpControllerStage::MatchTimer;
        xpRoundStartedAt = std::chrono::steady_clock::now();
        xpControllerDeadline = std::chrono::steady_clock::now()
            + roundDuration;
        if (xpControllerPhase == XpControllerPhase::ROLLOVER_LOOP
            && (xpProgressMode != XpProgressMode::LegacyChat || xpDrainUntilStopMode)) {
            xpDrainStopEligibleCount = GetConnectedNonHostCount();
            xpDrainStopRequiredVotes = xpProgressMode == XpProgressMode::LegacyChat
                && xpDrainUntilStopMode
                ? (xpDrainStopEligibleCount + 1) / 2
                : 0;
            xpDrainStopVoterIds.clear();
            xpDrainStopRequested = false;
            xpDrainMeetingAnnouncementPending = true;
            if (xpProgressMode == XpProgressMode::LegacyChat) {
                State.rpcQueue.push(new RpcReportBody({}));
            }
        }
        LogXpControllerEvent(std::format(
            "Game loading completed; starting {}ms round timer ({}s base + {}ms chat wait). {}",
            roundDuration.count(), matchDurationSeconds, messageWait.count(),
            BuildXpControllerDiagnostics()));
    }

    enum Groups {
        Player,
        Social,
        Trolling,
        Info,
    };

    static bool openPlayer = true;
    static bool openSocial = false;
    static bool openTrolling = false;
    static bool openInfo = false;

    static bool selectingShiftingToPlayer = false;
    static bool selectingTurningToPlayer = false;
    static std::vector<Game::PlayerId> playersToShift = {};
    static std::string playerToShiftName = "";

    void CloseOtherGroups(Groups group) {
        openPlayer = group == Groups::Player;
        openSocial = group == Groups::Social;
        openTrolling = group == Groups::Trolling;
        openInfo = group == Groups::Info;
    }

    void OpenSubGroup(const std::string& name) {
        if (name == "Player") CloseOtherGroups(Groups::Player);
        else if (name == "Social") CloseOtherGroups(Groups::Social);
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
            protectionIntervalMs = static_cast<int>(GetMatchCycleUpdateInterval().count());
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
                    if (TabGroup("Social", openSocial)) {
                        CloseOtherGroups(Groups::Social);
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
                if (openSocial && selectedPlayer.has_value() && selectedPlayers.size() == 1 &&
                    !selectedPlayer.is_Disconnected() && !selectedPlayer.get_PlayerControl()->fields.notRealPlayer) {
                    if (State.activeImpersonation && AnimatedButton(!State.SafeMode ? "Reset Impersonation" : "Reset Original Outfit")) {
                        ControlAppearance(false);
                    }

                    if (!selectedPlayer.is_LocalPlayer()) {
                        auto outfit = GetPlayerOutfit(selectedPlayer.get_PlayerData());
                        if (outfit != nullptr) {
                            std::queue<RPCInterface*>* queue = IsInGame() ? &State.rpcQueue :
                                IsInLobby() ? &State.lobbyRpcQueue : nullptr;

                            if (!State.activeImpersonation && AnimatedButton(!State.SafeMode ? "Copy Avatar" : "Copy Outfit") && queue != nullptr) {
                                if (IsHost() || !State.SafeMode)
                                    queue->push(new RpcForceColor(*Game::pLocalPlayer, outfit->fields.ColorId));
                                else
                                    queue->push(new RpcSetColor(GetRandomColorId()));
                                queue->push(new RpcSetPet(outfit->fields.PetId));
                                queue->push(new RpcSetSkin(outfit->fields.SkinId));
                                queue->push(new RpcSetVisor(outfit->fields.VisorId));
                                queue->push(new RpcSetHat(outfit->fields.HatId));
                                queue->push(new RpcSetNamePlate(outfit->fields.NamePlateId));
                                if (!State.SafeMode) ImpersonateName(selectedPlayer.get_PlayerData());
                                queue->push(new RpcSetLevel(*Game::pLocalPlayer, selectedPlayer.get_PlayerData()->fields.PlayerLevel));
                                State.activeImpersonation = true;
                            }
                        }

                        auto localPlayer = *Game::pLocalPlayer;
                        if (localPlayer != nullptr && localPlayer->fields.cosmetics != nullptr &&
                            localPlayer->fields.cosmetics->fields.currentPet != nullptr) {
                            std::queue<RPCInterface*>* queue = IsInGame() ? &State.rpcQueue :
                                IsInLobby() ? &State.lobbyRpcQueue : nullptr;
                            if (queue != nullptr && AnimatedButton("Pet Player")) {
                                queue->push(new RpcPetPlayer(selectedPlayer.get_PlayerControl()));
                            }
                        }
                        else {
                            ImGui::TextDisabled("Equip a pet to use Pet Player.");
                        }

                        if ((IsInGame() || IsInLobby()) && State.playerToWhisper.equals(State.selectedPlayer) && State.activeWhisper) {
                            if (AnimatedButton("Stop Whispering To")) {
                                State.playerToWhisper = {};
                                State.activeWhisper = false;
                            }
                        }
                        else if ((IsInGame() || IsInLobby()) && AnimatedButton("Whisper To")) {
                            State.playerToWhisper = State.selectedPlayer;
                            State.activeWhisper = true;
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
