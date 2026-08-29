#include "pch.h"
#include "../Public/FortMinigame.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortPlayerStateAthena.h"
#include "../Public/FortGameStateAthena.h"
#include "../Public/FortVolume.h"
#include "../../Erbium/Support/Public/FaultGuard.h"
#include "../../Erbium/Support/Public/VersionFeatureAdapter.h"
#include <unordered_map>
#include <vector>

namespace
{
    struct FPendingMinigameSetState
    {
        TWeakObjectPtr<AFortMinigame> Minigame;
        uint8 State = 0;
        uint8 ExpectedCurrentState = 0;
        ULONGLONG DueMs = 0;
    };

    std::unordered_map<AFortMinigame*, FPendingMinigameSetState> GPendingMinigameSetStates;
    constexpr size_t MaxPendingMinigameSetStates = 64;

    void QueueMinigameSetState(AFortMinigame* Minigame, uint8 State)
    {
        if (!Minigame || !Minigame->HasCurrentState())
            return;

        const ULONGLONG DueMs = GetTickCount64() + 1000;
        const uint8 ExpectedCurrentState = Minigame->CurrentState;
        auto Existing = GPendingMinigameSetStates.find(Minigame);
        if (Existing != GPendingMinigameSetStates.end())
        {
            Existing->second.Minigame = TWeakObjectPtr<AFortMinigame>(Minigame);
            Existing->second.State = State;
            Existing->second.ExpectedCurrentState = ExpectedCurrentState;
            Existing->second.DueMs = (std::min)(Existing->second.DueMs, DueMs);
            return;
        }

        if (GPendingMinigameSetStates.size() >= MaxPendingMinigameSetStates)
        {
            SDK::DbgLog("[CreativeMinigame] delayed SetState queue full; "
                "dropping minigame=%p state=%d\n", Minigame, (int)State);
            return;
        }

        GPendingMinigameSetStates.emplace(Minigame, FPendingMinigameSetState{
                TWeakObjectPtr<AFortMinigame>(Minigame), State, ExpectedCurrentState, DueMs });
    }

    void TickPendingMinigameSetStates()
    {
        if (GPendingMinigameSetStates.empty())
            return;

        const ULONGLONG NowMs = GetTickCount64();
        std::vector<FPendingMinigameSetState> Due;
        for (auto It = GPendingMinigameSetStates.begin();
             It != GPendingMinigameSetStates.end();)
        {
            if (NowMs < It->second.DueMs)
            {
                ++It;
                continue;
            }

            Due.push_back(It->second);
            It = GPendingMinigameSetStates.erase(It);
        }

        for (const auto& Pending : Due)
        {
            auto Minigame = Pending.Minigame.Get();
            if (!AFortMinigame::SetStateOG || !VersionFeatureAdapter::IsLiveActor(Minigame) ||
                !Minigame->HasCurrentState() || Minigame->CurrentState !=
                    Pending.ExpectedCurrentState)
            {
                continue;
            }

            AFortMinigame::SetStateOG(Minigame, Pending.State);
        }
    }

    AFortPlayerControllerAthena* GetLiveParticipantController(AFortPlayerStateAthena* PlayerState)
    {
        if (!VersionFeatureAdapter::IsLiveActor(PlayerState))
            return nullptr;

        auto Owner = PlayerState->GetOwner();
        if (!VersionFeatureAdapter::IsLiveObject(Owner))
            return nullptr;

        auto Controller = Owner->Cast<AFortPlayerControllerAthena>();
        return VersionFeatureAdapter::IsLiveActor(Controller) ? Controller : nullptr;
    }
}

void AFortMinigame::SetState(AFortMinigame* Minigame, uint8 NewState)
{
    if (!Minigame || !SetStateOG)
        return;

    TArray<AFortPlayerStateAthena *> Players;
    Minigame->GetParticipatingPlayers(Players);

    printf("[CreativeRuntime] (SetState): %d\n", NewState);

    if (NewState == EFortMinigameState::GetTransitioning())
    {
        for (int i = 0; i < Players.Num(); i++)
        {
            AFortPlayerStateAthena* PlayerState = Players[i];
            auto Controller = GetLiveParticipantController(PlayerState);
            if (Controller && VersionFeatureAdapter::IsLiveActor(Controller->MyFortPawn))
            {
                if (Minigame->NumTeams == 0)
                    Controller->ServerSetTeam(i + 3);
                else
                    Controller->ServerSetTeam((i % Minigame->NumTeams) + 3);

                Minigame->OnPlayerPawnPossessedDuringTransition(Controller->MyFortPawn);
            }
        }

        Minigame->AdvanceState();
        Minigame->HandleMinigameStarted();
    }
    else if (NewState == EFortMinigameState::GetWaitingForCameras())
    {
        for (int i = 0; i < Players.Num(); i++)
        {
            auto Controller = GetLiveParticipantController(Players[i]);
            if (!Controller)
                continue;
            auto Pawn = Controller->MyFortPawn;
            if (!VersionFeatureAdapter::IsLiveActor(Pawn))
                continue;

            Minigame->OnClientFinishTeleportingForMinigame(Pawn);
        }

        QueueMinigameSetState(Minigame, NewState);
    }
    else if (NewState == EFortMinigameState::GetPostGameReset())
    {
        SetStateOG(Minigame, NewState);
        for (int i = 0; i < Players.Num(); i++)
        {
            auto Controller = GetLiveParticipantController(Players[i]);
            if (!Controller)
                continue;
            auto Pawn = Controller->MyFortPawn;
            if (!VersionFeatureAdapter::IsLiveActor(Pawn))
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

    bool ProcessMinigameEventGuarded(const UObject* Object, UFunction* Function, void* Parameters)
    {
        ++GGuardedNativeCallDepth;
        bool bSucceeded = false;

        __try
        {
            Object->ProcessEvent(Function, Parameters);
            bSucceeded = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        --GGuardedNativeCallDepth;
        return bSucceeded;
    }

    // Legacy UFunction child lists expose parameters in build-dependent order, so pack by exact reflected name.
    bool TryAddMinigamePlayer(AFortMinigame* Minigame, AFortPlayerStateAthena* PlayerState,
        bool bForceSpawn)
    {
        if (!VersionFeatureAdapter::IsLiveActor(Minigame) ||
            !VersionFeatureAdapter::IsLiveActor(PlayerState))
            return false;

        UFunction* Function = Minigame->GetFunction("AddMinigamePlayer");
        if (!Function)
            return false;

        const auto Parameters = Function->GetParamsNamed();
        constexpr uint32 MaxParameterBytes = 0x40;
        if (Parameters.Size == 0 || Parameters.Size > MaxParameterBytes ||
            Parameters.NameOffsetMap.size() != 2)
        {
            return false;
        }

        constexpr uint64 CPF_Parm = 0x0000000000000080;
        constexpr uint64 CPF_OutParm = 0x0000000000000100;
        constexpr uint64 CPF_ReturnParm = 0x0000000000000400;
        uint32 PlayerStateOffset = UINT32_MAX;
        uint32 ForceSpawnOffset = UINT32_MAX;

        for (const auto& Parameter : Parameters.NameOffsetMap)
        {
            if (!(Parameter.PropertyFlags & CPF_Parm) || (Parameter.PropertyFlags &
                    (CPF_OutParm | CPF_ReturnParm)))
            {
                return false;
            }

            uint32 ExpectedSize = 0;
            uint32* DestinationOffset = nullptr;
            if (Parameter.Name == "PlayerState")
            {
                ExpectedSize = sizeof(PlayerState);
                DestinationOffset = &PlayerStateOffset;
            }
            else if (Parameter.Name == "bForceSpawn")
            {
                ExpectedSize = sizeof(bool);
                DestinationOffset = &ForceSpawnOffset;
            }
            else
            {
                return false;
            }

            if (*DestinationOffset != UINT32_MAX || Parameter.ElementSize != ExpectedSize ||
                Parameter.Offset > Parameters.Size ||
                ExpectedSize > Parameters.Size - Parameter.Offset)
            {
                return false;
            }

            *DestinationOffset = Parameter.Offset;
        }

        if (PlayerStateOffset == UINT32_MAX || ForceSpawnOffset == UINT32_MAX ||
            (PlayerStateOffset < ForceSpawnOffset + sizeof(bool) && ForceSpawnOffset <
                    PlayerStateOffset + sizeof(PlayerState)))
        {
            return false;
        }

        alignas(void*) uint8 ParameterMemory[MaxParameterBytes]{};
        memcpy(ParameterMemory + PlayerStateOffset, &PlayerState, sizeof(PlayerState));
        memcpy(ParameterMemory + ForceSpawnOffset, &bForceSpawn, sizeof(bForceSpawn));

        return ProcessMinigameEventGuarded(Minigame, Function, ParameterMemory);
    }

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

    void AddVolumePlayersToMinigame(AFortMinigame* Minigame)
    {
        if (!Minigame->HasVolume() || !Minigame->Volume)
            return;

        TArray<AFortPlayerControllerAthena*> Controllers;
        Utils::GetAll<AFortPlayerControllerAthena>(Controllers);

        for (int i = 0; i < Controllers.Num(); i++)
        {
            auto Controller = Controllers[i];
            if (!VersionFeatureAdapter::IsLiveActor(Controller) ||
                !VersionFeatureAdapter::IsLiveActor(Controller->PlayerState))
                continue;
            if (!Controller->HasCreativePlotLinkedVolume() ||
                (AActor*)Controller->CreativePlotLinkedVolume != Minigame->Volume)
                continue;

            if (TryAddMinigamePlayer(Minigame, Controller->PlayerState, false))
            {
                SDK::DbgLog("[CreativeMinigame] added %s to minigame %p "
                    "(volume %p)\n", Controller->Name.ToString().c_str(), Minigame,
                    Minigame->Volume);
            }
            else
            {
                SDK::DbgLog("[CreativeMinigame] skipped adding %s to minigame %p; "
                    "AddMinigamePlayer schema unavailable or call faulted\n",
                    Controller->Name.ToString().c_str(), Minigame);
            }
        }

        Controllers.Free();
    }

    void NudgeMinigame(AFortMinigame* Minigame, uint8 State, int64 Transitioning, int64 WaitingForCameras)
    {
        TArray<AFortPlayerStateAthena*> Players;
        Minigame->GetParticipatingPlayers(Players);

        for (int i = 0; i < Players.Num(); i++)
        {
            auto Controller = GetLiveParticipantController(Players[i]);
            if (!Controller)
                continue;

            auto Pawn = Controller->MyFortPawn;
            if (!VersionFeatureAdapter::IsLiveActor(Pawn))
                continue;

            if ((int64)State == Transitioning)
                Minigame->OnPlayerPawnPossessedDuringTransition(Pawn);
            else if ((int64)State == WaitingForCameras)
                Minigame->OnClientFinishTeleportingForMinigame(Pawn);
        }

        SDK::DbgLog("[CreativeMinigame] %p stalled in %s with %d player(s); advancing\n", Minigame,
            MinigameStateName(State), Players.Num());

        Players.Free();

        Minigame->AdvanceState();
    }

    void ResetMinigameToPreGame(AFortMinigame* Minigame, int64 PreGame)
    {
        TArray<AFortPlayerStateAthena*> Players;
        Minigame->GetParticipatingPlayers(Players);

        for (int i = 0; i < Players.Num(); i++)
        {
            auto Controller = GetLiveParticipantController(Players[i]);
            if (!Controller)
                continue;

            auto Pawn = Controller->MyFortPawn;
            if (!VersionFeatureAdapter::IsLiveActor(Pawn))
                continue;

            Minigame->OnPlayerPawnPossessedDuringTransition(Pawn);
        }

        SDK::DbgLog("[CreativeMinigame] %p stalled in PostGameReset with %d player(s); "
            "returning to PreGame\n", Minigame, Players.Num());

        Players.Free();

        Minigame->CurrentState = (uint8)PreGame;
        Minigame->OnRep_CurrentState();
    }
}

void AFortMinigame::TickCreativeMinigames()
{
    TickPendingMinigameSetStates();

    if (VersionInfo.FortniteVersion >= 18.00 || !GetDefaultObj())
        return;

    auto World = UWorld::GetWorld();
    if (!World)
        return;

    auto GameState = (AFortGameStateAthena*)World->GameState;
    if (!GameState || !GameState->HasCreativePortalManager() || !GameState->CreativePortalManager)
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
                Minigame, MinigameStateName(Watch.LastState), MinigameStateName(State),
                Minigame->HasVolume() ? Minigame->Volume : nullptr, StartCount,
                Minigame->HasSpawnLocationSetting() ? (int)Minigame->SpawnLocationSetting : -1,
                Minigame->HasNumTeams() ? Minigame->NumTeams : -1);

            Watch.LastState = State;
            Watch.StateEnteredMs = NowMs;
            Watch.LastNudgeMs = 0;
            Watch.AddPlayerAttempts = 0;
        }

        if ((int64)State == PreGame && SpawnPads >= 0 && CurrentLocation >= 0 &&
            StartCount >= 0 && Minigame->HasSpawnLocationSetting())
        {
            const int64 Setting = (int64)Minigame->SpawnLocationSetting;

            if (StartCount == 0 && Setting == SpawnPads)
            {
                Minigame->SpawnLocationSetting = (uint8)CurrentLocation;
                Watch.bPatchedSpawnSetting = true;
                SDK::DbgLog("[CreativeMinigame] %p has no spawn pads; "
                    "spawn location setting -> CurrentLocation\n", Minigame);
            }
            else if (StartCount > 0 && Watch.bPatchedSpawnSetting && Setting == CurrentLocation)
            {
                Minigame->SpawnLocationSetting = (uint8)SpawnPads;
                Watch.bPatchedSpawnSetting = false;
                SDK::DbgLog("[CreativeMinigame] %p now has %d spawn pad(s); "
                    "spawn location setting -> SpawnPads\n", Minigame, StartCount);
            }

            continue;
        }

        const ULONGLONG StalledMs = NowMs - Watch.StateEnteredMs;
        const bool bStartSequence = (Setup >= 0 && (int64)State == Setup) ||
            (int64)State == Transitioning || (int64)State == WaitingForCameras;
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

        ULONGLONG StallLimitMs = 3000;
        if (bWarmup)
        {
            const float Duration = Minigame->HasWarmupDuration() ? Minigame->WarmupDuration : 0.f;
            StallLimitMs = 8000ULL + (Duration > 0.f ? (ULONGLONG)(Duration * 1000.f) : 0ULL);
        }
        else if (bPostGame)
        {
            const float Delay =
                Minigame->HasPostGameResetDelay() ? Minigame->PostGameResetDelay : 0.f;
            StallLimitMs = 10000ULL + (Delay > 0.f ? (ULONGLONG)(Delay * 1000.f) : 0ULL);
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
        if (It->second.bSeen) ++It;
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
