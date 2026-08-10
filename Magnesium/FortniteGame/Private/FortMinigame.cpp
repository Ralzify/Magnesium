#include "pch.h"
#include "../Public/FortMinigame.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortPlayerStateAthena.h"
#include "../Public/FortGameStateAthena.h"
#include "../Public/FortVolume.h"
#include <thread>
#include <unordered_map>

void AFortMinigame::SetState(AFortMinigame* Minigame, uint8 NewState)
{
    TArray<AFortPlayerStateAthena *> Players;
    Minigame->GetParticipatingPlayers(Players);

    printf("[CreativeRuntime] (SetState): %d\n", NewState);

    if (NewState == EFortMinigameState::GetTransitioning())
    {
        for (int i = 0; i < Players.Num(); i++)
        {
            AFortPlayerStateAthena* PlayerState = Players[i];
            if (PlayerState)
            {
                AFortPlayerControllerAthena* Controller = PlayerState->GetOwner()->Cast<AFortPlayerControllerAthena>();
                if (Controller && Controller->MyFortPawn)
                {
                    if (Minigame->NumTeams == 0)
                        Controller->ServerSetTeam(i + 3);
                    else
                        Controller->ServerSetTeam((i % Minigame->NumTeams) + 3);

                    Minigame->OnPlayerPawnPossessedDuringTransition(Controller->MyFortPawn);
                }
            }
        }

        Minigame->AdvanceState();
        Minigame->HandleMinigameStarted();
    }
    else if (NewState == EFortMinigameState::GetWaitingForCameras())
    {
        for (int i = 0; i < Players.Num(); i++)
        {
            if (!Players[i])
                continue;

            auto Player = Players[i]->Cast<AFortPlayerStateAthena>();
            if (!Player || !Player->GetOwner())
                continue;

            auto Controller = Player->GetOwner()->Cast<AFortPlayerControllerAthena>();
            if (!Controller)
                continue;
            auto Pawn = Controller->MyFortPawn;
            if (!Pawn)
                continue;

            Minigame->OnClientFinishTeleportingForMinigame(Pawn);
        }

        // this can crash btw!
        std::thread([Minigame, NewState]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            if (Minigame && !IsBadReadPtr(Minigame) && SetStateOG)
                SetStateOG(Minigame, NewState);
        }).detach();
    }
    else if (NewState == EFortMinigameState::GetPostGameReset())
    {
        SetStateOG(Minigame, NewState);
        for (int i = 0; i < Players.Num(); i++)
        {
            auto Player = Players[i]->Cast<AFortPlayerStateAthena>();
            auto Controller = Player->GetOwner()->Cast<AFortPlayerControllerAthena>();
            if (!Controller)
                continue;
            auto Pawn = Controller->MyFortPawn;
            if (!Pawn)
                continue;

            Minigame->OnPlayerPawnPossessedDuringTransition(Pawn);
        }

        Minigame->CurrentState = (uint8_t)EFortMinigameState::GetPreGame();
    }

    Players.Free();

    if (NewState != EFortMinigameState::GetWaitingForCameras() && NewState != EFortMinigameState::GetPostGameReset())
        return SetStateOG(Minigame, NewState);
}

namespace
{
    // One entry per live AFortMinigame; rebuilt as minigames come and go.
    struct FMinigameWatch
    {
        uint8 LastState = 0xFF;
        ULONGLONG StateEnteredMs = 0;
        ULONGLONG LastNudgeMs = 0;
        uint8 AddPlayerAttempts = 0;
        bool bSeen = false;
        bool bPatchedSpawnSetting = false;
    };

    std::unordered_map<AFortMinigame*, FMinigameWatch> GMinigameWatches;

    const char* MinigameStateName(uint8 State)
    {
        switch (State)
        {
        case 0: return "PreGame";
        case 1: return "Setup";
        case 2: return "Transitioning";
        case 3: return "WaitingForCameras";
        case 4: return "Warmup";
        case 5: return "InProgress";
        case 6: return "PostGameTimeDilation";
        case 7: return "PostRoundEnd";
        case 8: return "PostGameEnd";
        case 9: return "PostGameAbandon";
        case 10: return "PostGameReset";
        default: return "Unknown";
        }
    }

    // The volume's owning players, used when the native path never registered
    // anyone with the minigame it is about to start.
    void AddVolumePlayersToMinigame(AFortMinigame* Minigame)
    {
        if (!Minigame->HasVolume() || !Minigame->Volume)
            return;

        TArray<AFortPlayerControllerAthena*> Controllers;
        Utils::GetAll<AFortPlayerControllerAthena>(Controllers);

        for (int i = 0; i < Controllers.Num(); i++)
        {
            auto Controller = Controllers[i];
            if (!Controller || !Controller->PlayerState)
                continue;
            if (!Controller->HasCreativePlotLinkedVolume() ||
                (AActor*)Controller->CreativePlotLinkedVolume != Minigame->Volume)
                continue;

            SDK::DbgLog(
                "[CreativeMinigame] adding %s to minigame %p (volume %p)\n",
                Controller->Name.ToString().c_str(),
                Minigame,
                Minigame->Volume);

            Minigame->AddMinigamePlayer(Controller->PlayerState, false);
        }

        Controllers.Free();
    }

    // Replays the two client acknowledgements the native start sequence waits
    // on, then hands the state machine forward.
    void NudgeMinigame(AFortMinigame* Minigame, uint8 State, int64 Transitioning, int64 WaitingForCameras)
    {
        TArray<AFortPlayerStateAthena*> Players;
        Minigame->GetParticipatingPlayers(Players);

        for (int i = 0; i < Players.Num(); i++)
        {
            auto PlayerState = Players[i];
            if (!PlayerState || !PlayerState->GetOwner())
                continue;

            auto Controller = PlayerState->GetOwner()->Cast<AFortPlayerControllerAthena>();
            if (!Controller)
                continue;

            auto Pawn = Controller->MyFortPawn;
            if (!Pawn)
                continue;

            if ((int64)State == Transitioning)
                Minigame->OnPlayerPawnPossessedDuringTransition(Pawn);
            else if ((int64)State == WaitingForCameras)
                Minigame->OnClientFinishTeleportingForMinigame(Pawn);
        }

        SDK::DbgLog(
            "[CreativeMinigame] %p stalled in %s with %d player(s); advancing\n",
            Minigame,
            MinigameStateName(State),
            Players.Num());

        Players.Free();

        Minigame->AdvanceState();
    }

    // PostGameReset does its own island/player cleanup but never schedules the
    // hop back to PreGame, so the island stays in the HUD-less post-game screen
    // forever. Hand the pawns back and close the loop ourselves.
    void ResetMinigameToPreGame(AFortMinigame* Minigame, int64 PreGame)
    {
        TArray<AFortPlayerStateAthena*> Players;
        Minigame->GetParticipatingPlayers(Players);

        for (int i = 0; i < Players.Num(); i++)
        {
            auto PlayerState = Players[i];
            if (!PlayerState || !PlayerState->GetOwner())
                continue;

            auto Controller = PlayerState->GetOwner()->Cast<AFortPlayerControllerAthena>();
            if (!Controller)
                continue;

            auto Pawn = Controller->MyFortPawn;
            if (!Pawn)
                continue;

            Minigame->OnPlayerPawnPossessedDuringTransition(Pawn);
        }

        SDK::DbgLog(
            "[CreativeMinigame] %p stalled in PostGameReset with %d player(s); "
            "returning to PreGame\n",
            Minigame,
            Players.Num());

        Players.Free();

        Minigame->CurrentState = (uint8)PreGame;
        Minigame->OnRep_CurrentState();
    }
}

void AFortMinigame::TickCreativeMinigames()
{
    // The 18.x builds drive the same sequence from the native SetState hook.
    if (SetStateOG || !GetDefaultObj())
        return;

    auto World = UWorld::GetWorld();
    if (!World)
        return;

    // Creative only — CreativePortalManager is null on Battle Royale playlists,
    // which keeps the actor sweep below off the hot path there.
    auto GameState = (AFortGameStateAthena*)World->GameState;
    if (!GameState || !GameState->HasCreativePortalManager() ||
        !GameState->CreativePortalManager)
        return;

    static ULONGLONG NextScanMs = 0;
    const ULONGLONG NowMs = GetTickCount64();
    if (NowMs < NextScanMs)
        return;
    NextScanMs = NowMs + 250;

    const int64 PreGame = EFortMinigameState::GetPreGame();
    const int64 Setup = EFortMinigameState::GetSetup();
    const int64 Transitioning = EFortMinigameState::GetTransitioning();
    const int64 WaitingForCameras = EFortMinigameState::GetWaitingForCameras();
    const int64 Warmup = EFortMinigameState::GetWarmup();
    const int64 PostGameTimeDilation = EFortMinigameState::GetPostGameTimeDilation();
    const int64 PostRoundEnd = EFortMinigameState::GetPostRoundEnd();
    const int64 PostGameEnd = EFortMinigameState::GetPostGameEnd();
    const int64 PostGameAbandon = EFortMinigameState::GetPostGameAbandon();
    const int64 PostGameReset = EFortMinigameState::GetPostGameReset();
    if (Transitioning < 0 || WaitingForCameras < 0)
        return;

    const int64 SpawnPads = EFortMinigamePlayerSpawnLocationSetting::GetSpawnPads();
    const int64 CurrentLocation = EFortMinigamePlayerSpawnLocationSetting::GetCurrentLocation();

    TArray<AFortMinigame*> Minigames;
    Utils::GetAll<AFortMinigame>(Minigames);

    for (auto& Entry : GMinigameWatches)
        Entry.second.bSeen = false;

    for (int i = 0; i < Minigames.Num(); i++)
    {
        auto Minigame = Minigames[i];
        if (!Minigame || !Minigame->HasCurrentState())
            continue;

        auto& Watch = GMinigameWatches[Minigame];
        Watch.bSeen = true;

        const uint8 State = Minigame->CurrentState;
        const int32 StartCount =
            Minigame->HasPlayerStartComponents() ? Minigame->PlayerStartComponents.Num() : -1;

        if (State != Watch.LastState)
        {
            SDK::DbgLog(
                "[CreativeMinigame] %p %s -> %s volume=%p spawnPads=%d spawnMode=%d teams=%d\n",
                Minigame,
                MinigameStateName(Watch.LastState),
                MinigameStateName(State),
                Minigame->HasVolume() ? Minigame->Volume : nullptr,
                StartCount,
                Minigame->HasSpawnLocationSetting() ? (int)Minigame->SpawnLocationSetting : -1,
                Minigame->HasNumTeams() ? Minigame->NumTeams : -1);

            Watch.LastState = State;
            Watch.StateEnteredMs = NowMs;
            Watch.LastNudgeMs = 0;
            Watch.AddPlayerAttempts = 0;
        }

        // An island with no spawn pads can never resolve a player start, and the
        // native start sequence then blocks forever waiting for a teleport that
        // is never issued. Keep the setting in sync with what the island has.
        if ((int64)State == PreGame && SpawnPads >= 0 && CurrentLocation >= 0 &&
            StartCount >= 0 && Minigame->HasSpawnLocationSetting())
        {
            const int64 Setting = (int64)Minigame->SpawnLocationSetting;

            if (StartCount == 0 && Setting == SpawnPads)
            {
                Minigame->SpawnLocationSetting = (uint8)CurrentLocation;
                Watch.bPatchedSpawnSetting = true;
                SDK::DbgLog(
                    "[CreativeMinigame] %p has no spawn pads; "
                    "spawn location setting -> CurrentLocation\n",
                    Minigame);
            }
            else if (StartCount > 0 && Watch.bPatchedSpawnSetting &&
                     Setting == CurrentLocation)
            {
                Minigame->SpawnLocationSetting = (uint8)SpawnPads;
                Watch.bPatchedSpawnSetting = false;
                SDK::DbgLog(
                    "[CreativeMinigame] %p now has %d spawn pad(s); "
                    "spawn location setting -> SpawnPads\n",
                    Minigame,
                    StartCount);
            }

            continue;
        }

        const ULONGLONG StalledMs = NowMs - Watch.StateEnteredMs;
        const bool bStartSequence =
            (Setup >= 0 && (int64)State == Setup) ||
            (int64)State == Transitioning ||
            (int64)State == WaitingForCameras;
        const bool bWarmup = Warmup >= 0 && (int64)State == Warmup;
        const bool bPostGameReset =
            PreGame >= 0 && PostGameReset >= 0 && (int64)State == PostGameReset;
        const bool bPostGame =
            (PostGameTimeDilation >= 0 && (int64)State == PostGameTimeDilation) ||
            (PostRoundEnd >= 0 && (int64)State == PostRoundEnd) ||
            (PostGameEnd >= 0 && (int64)State == PostGameEnd) ||
            (PostGameAbandon >= 0 && (int64)State == PostGameAbandon);

        if (!bStartSequence && !bWarmup && !bPostGameReset && !bPostGame)
            continue;

        // Warmup owns a real countdown and the post-game states own the winner
        // and scoreboard displays, so only treat those as stalled well past the
        // configured durations.
        ULONGLONG StallLimitMs = 3000;
        if (bWarmup)
        {
            const float Duration =
                Minigame->HasWarmupDuration() ? Minigame->WarmupDuration : 0.f;
            StallLimitMs =
                8000ULL + (Duration > 0.f ? (ULONGLONG)(Duration * 1000.f) : 0ULL);
        }
        else if (bPostGame)
        {
            const float Delay =
                Minigame->HasPostGameResetDelay() ? Minigame->PostGameResetDelay : 0.f;
            StallLimitMs =
                10000ULL + (Delay > 0.f ? (ULONGLONG)(Delay * 1000.f) : 0ULL);
        }

        if (StalledMs < StallLimitMs || NowMs - Watch.LastNudgeMs < 2000)
            continue;

        Watch.LastNudgeMs = NowMs;

        if (bPostGameReset)
        {
            ResetMinigameToPreGame(Minigame, PreGame);
            continue;
        }

        if (bStartSequence)
        {
            TArray<AFortPlayerStateAthena*> Players;
            Minigame->GetParticipatingPlayers(Players);
            const int32 PlayerCount = Players.Num();
            Players.Free();

            // Nobody registered with the minigame: try to enrol the plot's
            // owners, but give up after a few passes rather than looping here.
            if (PlayerCount == 0 && Watch.AddPlayerAttempts < 3)
            {
                ++Watch.AddPlayerAttempts;
                AddVolumePlayersToMinigame(Minigame);
                continue;
            }
        }

        NudgeMinigame(Minigame, State, Transitioning, WaitingForCameras);
    }

    Minigames.Free();

    for (auto It = GMinigameWatches.begin(); It != GMinigameWatches.end();)
    {
        if (It->second.bSeen)
            ++It;
        else
            It = GMinigameWatches.erase(It);
    }
}

void AFortMinigame::Hook()
{
    if (!GetDefaultObj())
        return;

    if (VersionInfo.FortniteVersion < 18.00 || VersionInfo.FortniteVersion >= 19.00)
        return;

    auto SetStateAddr = FindSetState();
    if (SetStateAddr)
        Utils::Hook(SetStateAddr, SetState, SetStateOG);
}
