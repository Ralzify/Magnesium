#include "pch.h"
#include "../Public/FortAthenaMutator.h"
#include "../Public/BattleRoyaleGamePhaseLogic.h"
#include "../Public/FortSafeZoneIndicator.h"
#include "../Public/LevelStreamingDynamic.h"
#include "../../Engine/Public/NetDriver.h"
#include "../../Erbium/Public/Configuration.h"

#include <cmath>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>

namespace
{
    constexpr double HeistMinimumVersion = 5.40;
    constexpr double HeistEndVersionExclusive = 6.00;
    constexpr double ExitCraftDiscoveryInterval = 0.20;

    struct FHeistCompatibilityState
    {
        UWorld* World = nullptr;
        const UFortPlaylistAthena* Playlist = nullptr;
        bool bPlaylistPrepared = false;
        bool bAdditionalLevelsComplete = false;
        bool bLoggedMissingPlaylistLoadFunctions = false;
        bool bLoggedMissingAdditionalLevelData = false;
        bool bPublishedCompatibilityPhaseStep = false;
        uint8 PublishedCompatibilityPhaseStep =
            static_cast<uint8>(EAthenaGamePhaseStep::None);
        bool bObservedNativePhaseStep = false;
        uint8 ObservedNativePhaseStep =
            static_cast<uint8>(EAthenaGamePhaseStep::None);
        double NextPlaylistPreparationAttemptTime = 0.0;
        double NextAdditionalLevelAttemptTime = 0.0;
        double NextExitCraftDiscoveryTime = 0.0;
        std::unordered_map<AFortAthenaExitCraftSpawner*, double>
            ScheduledExitCraftSpawners;
        std::unordered_map<AFortAthenaMutator_Heist*, uint8>
            LastNotifiedPhaseSteps;
    };

    FHeistCompatibilityState GHeistCompatibilityState;

    void ResetHeistCompatibilityState(
        UWorld* World,
        const UFortPlaylistAthena* Playlist = nullptr)
    {
        GHeistCompatibilityState = {};
        GHeistCompatibilityState.World = World;
        GHeistCompatibilityState.Playlist = Playlist;
    }

    bool EqualsInsensitive(
        const wchar_t* Left,
        const wchar_t* Right)
    {
        if (!Left || !Right)
            return false;

        std::wstring LowerLeft = Left;
        std::wstring LowerRight = Right;
        std::transform(
            LowerLeft.begin(), LowerLeft.end(),
            LowerLeft.begin(),
            [](wchar_t Character)
            {
                return static_cast<wchar_t>(std::towlower(Character));
            });
        std::transform(
            LowerRight.begin(), LowerRight.end(),
            LowerRight.begin(),
            [](wchar_t Character)
            {
                return static_cast<wchar_t>(std::towlower(Character));
            });
        return LowerLeft == LowerRight;
    }

    bool IsHeistPlaylistIdentifier(const wchar_t* Identifier)
    {
        static constexpr const wchar_t* Identifiers[] = {
            L"/Game/Athena/Playlists/Bling/"
                L"Playlist_Bling_Solo.Playlist_Bling_Solo",
            L"Playlist_Bling_Solo",
            L"/Game/Athena/Playlists/Bling/"
                L"Playlist_Bling_Duos.Playlist_Bling_Duos",
            L"Playlist_Bling_Duos",
            L"/Game/Athena/Playlists/Bling/"
                L"Playlist_Bling_Squads.Playlist_Bling_Squads",
            L"Playlist_Bling_Squads",
            L"The Getaway",
            L"The Getaway Solos",
            L"The Getaway Duos",
            L"The Getaway Squads",
            L"Getaway"
        };

        for (const auto Candidate : Identifiers)
        {
            if (EqualsInsensitive(Identifier, Candidate))
                return true;
        }
        return false;
    }

    bool IsSaneArray(int32 Num, int32 Max, int32 Limit = 128)
    {
        return Num >= 0 && Max >= Num && Num <= Limit;
    }

    bool IsValidatedNativeFunction(uintptr_t Address)
    {
        if (!Address)
            return false;

        auto TextSection =
            Memcury::PE::Section::GetSection(".text");
        const uintptr_t TextStart =
            TextSection.GetSectionStart().Get();
        const uintptr_t TextEnd =
            TextSection.GetSectionEnd().Get();
        if (!TextStart || TextEnd <= TextStart ||
            Address < TextStart || Address >= TextEnd)
        {
            return false;
        }

        const uintptr_t ModuleBase = Memcury::PE::GetModuleBase();
        const auto NtHeaders = Memcury::PE::GetNTHeaders();
        if (!ModuleBase || !NtHeaders)
            return false;

        constexpr uintptr_t RequiredReadableBytes = 16;
        const uintptr_t ModuleSize =
            NtHeaders->OptionalHeader.SizeOfImage;
        if (ModuleSize < RequiredReadableBytes ||
            Address < ModuleBase ||
            Address - ModuleBase >
                ModuleSize - RequiredReadableBytes)
        {
            return false;
        }

        return SDK::MemReadable(
            reinterpret_cast<void*>(Address),
            RequiredReadableBytes);
    }

    uintptr_t FindNativeLoadCurrentPlaylistData()
    {
        static bool bInitialized = false;
        static uintptr_t Address = 0;
        if (bInitialized)
            return Address;
        bInitialized = true;

        if (!FFortAthenaHeistCompatibility::IsSupportedBuild())
            return 0;

        auto StringRef = Memcury::Scanner::FindStringRef(
            L"PLAYLIST: Playlist Object is loading its assets in "
            L"AFortGameStateAthena::LoadCurrentPlaylistData(), "
            L"PlaylistName is %s (Server Side)",
            false);
        if (!StringRef.IsValid())
        {
            StringRef = Memcury::Scanner::FindStringRef(
                L"PLAYLIST: Playlist Object is loading its assets in "
                L"AFortGameStateAthena::LoadCurrentPlaylistData(), "
                L"PlaylistName is %s (Client Side)",
                false);
        }

        if (StringRef.IsValid())
        {
            const uintptr_t ReferenceAddress = StringRef.Get();
            const uintptr_t Candidate =
                StringRef.FindFunctionBoundary(false).Get();
            if (IsValidatedNativeFunction(Candidate) &&
                Candidate <= ReferenceAddress &&
                ReferenceAddress - Candidate <= 2048)
            {
                Address = Candidate;
            }
        }

        SDK::DbgLog(
            "[Heist] native LoadCurrentPlaylistData resolver=%p\n",
            reinterpret_cast<void*>(Address));
        return Address;
    }

    uintptr_t FindNativeInitializePlaylistDataPreDataLoad()
    {
        static bool bInitialized = false;
        static uintptr_t Address = 0;
        if (bInitialized)
            return Address;
        bInitialized = true;

        if (!FFortAthenaHeistCompatibility::IsSupportedBuild())
            return 0;

        const uintptr_t Candidate =
            Memcury::Scanner::FindPattern(
                "40 53 48 83 EC ? 48 8B D9 48 8B 89 ? ? ? ? "
                "48 85 C9 74 ? 80 BB",
                false)
                .Get();
        if (IsValidatedNativeFunction(Candidate))
            Address = Candidate;

        SDK::DbgLog(
            "[Heist] native InitializePlaylistDataPreDataLoad "
            "resolver=%p\n",
            reinterpret_cast<void*>(Address));
        return Address;
    }

    std::vector<AFortAthenaMutator_Heist*> FindHeistMutators(
        AFortGameStateAthena* GameState)
    {
        std::vector<AFortAthenaMutator_Heist*> Result;
        const UClass* HeistClass = AFortAthenaMutator_Heist::StaticClass();
        if (!GameState || !HeistClass)
            return Result;

        auto AddUnique =
            [&](AActor* Candidate)
            {
                if (!Candidate || !Candidate->IsA(HeistClass))
                    return;

                auto Heist = static_cast<AFortAthenaMutator_Heist*>(Candidate);
                if (std::find(Result.begin(), Result.end(), Heist) ==
                    Result.end())
                {
                    Result.push_back(Heist);
                }
            };

        if (GameState->HasGameplayMutators())
        {
            auto& Mutators = GameState->GameplayMutators;
            if (IsSaneArray(Mutators.Num(), Mutators.Max(), 64))
            {
                for (int32 Index = 0; Index < Mutators.Num(); ++Index)
                    AddUnique(Mutators[Index]);
            }
        }

        if (Result.empty())
        {
            TArray<AActor*> HeistActors;
            Utils::GetAll(HeistClass, HeistActors);
            if (IsSaneArray(HeistActors.Num(), HeistActors.Max(), 64))
            {
                for (auto HeistActor : HeistActors)
                    AddUnique(HeistActor);
            }
            HeistActors.Free();
        }

        return Result;
    }

    bool HasStreamedPlaylistLevel(
        AFortGameStateAthena* GameState,
        const FName& LevelName)
    {
        if (!GameState || !GameState->HasAdditionalPlaylistLevelsStreamed())
            return false;

        auto& StreamedLevels = GameState->AdditionalPlaylistLevelsStreamed;
        if (!IsSaneArray(
                StreamedLevels.Num(), StreamedLevels.Max(), 256))
        {
            return false;
        }

        const UStruct* AdditionalLevelStruct =
            FAdditionalLevelStreamed::StaticStruct();
        if (AdditionalLevelStruct)
        {
            const int32 StructSize =
                AdditionalLevelStruct->GetPropertiesSize();
            if (StructSize <= 0 || StructSize > 0x100)
                return false;

            for (int32 Index = 0; Index < StreamedLevels.Num(); ++Index)
            {
                auto& Existing =
                    StreamedLevels.Get(Index, StructSize);
                if (FAdditionalLevelStreamed::HasLevelName() &&
                    Existing.LevelName == LevelName)
                {
                    return true;
                }
            }
            return false;
        }

        auto& LegacyLevels =
            reinterpret_cast<TArray<FName>&>(StreamedLevels);
        for (int32 Index = 0; Index < LegacyLevels.Num(); ++Index)
        {
            if (LegacyLevels[Index] == LevelName)
                return true;
        }
        return false;
    }

    bool AddStreamedPlaylistLevel(
        AFortGameStateAthena* GameState,
        const FName& LevelName,
        bool bServerOnly)
    {
        if (!GameState ||
            !GameState->HasAdditionalPlaylistLevelsStreamed() ||
            HasStreamedPlaylistLevel(GameState, LevelName))
        {
            return false;
        }

        auto& StreamedLevels = GameState->AdditionalPlaylistLevelsStreamed;
        const UStruct* AdditionalLevelStruct =
            FAdditionalLevelStreamed::StaticStruct();
        if (!AdditionalLevelStruct)
        {
            reinterpret_cast<TArray<FName>&>(StreamedLevels).Add(LevelName);
            return true;
        }

        const int32 StructSize =
            AdditionalLevelStruct->GetPropertiesSize();
        if (StructSize <= 0 || StructSize > 0x100 ||
            !FAdditionalLevelStreamed::HasLevelName() ||
            !FAdditionalLevelStreamed::HasbIsServerOnly())
        {
            return false;
        }

        void* Memory = FMemory::Malloc(StructSize);
        if (!Memory)
            return false;
        memset(Memory, 0, StructSize);

        auto Level = static_cast<FAdditionalLevelStreamed*>(Memory);
        FName MutableLevelName = LevelName;
        Level->LevelName = MutableLevelName;
        Level->bIsServerOnly = bServerOnly;
        StreamedLevels.Add(*Level, StructSize);
        FMemory::Free(Memory);
        return true;
    }

    bool HasNoParameters(UFunction* Function)
    {
        if (!Function)
            return false;

        const auto Parameters = Function->GetParamsNamed();
        return Parameters.Size == 0 &&
            Parameters.NameOffsetMap.empty();
    }

    bool CallReflectedNoParams(UObject* Object, const char* FunctionName)
    {
        if (!Object || !FunctionName)
            return false;

        UFunction* Function = Object->GetFunction(FunctionName);
        if (!HasNoParameters(Function))
            return false;

        Object->Call<void>(Function);
        return true;
    }

    bool InvokeGamePhaseStep(
        UObject* Target,
        UFunction* Function,
        uint8 NewStep)
    {
        if (!Target || !Function)
            return false;

        const auto Parameters = Function->GetParamsNamed();
        if (Parameters.Size <= 0 || Parameters.Size > 0x40 ||
            Parameters.NameOffsetMap.size() != 1)
        {
            return false;
        }

        constexpr uint64 CPF_Parm = 0x0000000000000080;
        constexpr uint64 CPF_OutParm = 0x0000000000000100;
        constexpr uint64 CPF_ReturnParm = 0x0000000000000400;
        const auto& Parameter = Parameters.NameOffsetMap[0];
        // Delegate-compatible functions can choose their own parameter name.
        // The exact one-byte input layout is the ABI contract that matters.
        if (!(Parameter.PropertyFlags & CPF_Parm) ||
            (Parameter.PropertyFlags &
                (CPF_OutParm | CPF_ReturnParm)) ||
            Parameter.ElementSize != sizeof(uint8) ||
            Parameter.Offset > Parameters.Size ||
            sizeof(uint8) >
                static_cast<uint32>(Parameters.Size) -
                    Parameter.Offset)
        {
            return false;
        }

        void* Memory = FMemory::Malloc(Parameters.Size);
        if (!Memory)
            return false;
        memset(Memory, 0, Parameters.Size);
        memcpy(
            static_cast<uint8*>(Memory) + Parameter.Offset,
            &NewStep,
            sizeof(NewStep));
        Target->ProcessEvent(Function, Memory);
        FMemory::Free(Memory);
        return true;
    }

    bool ResolveExitCraftCallbackLayout(
        UFunction* Function,
        uint32& ParametersSize,
        uint32& ExitCraftOffset,
        uint32& ExitCraftSpawnerOffset)
    {
        ParametersSize = 0;
        ExitCraftOffset = uint32(-1);
        ExitCraftSpawnerOffset = uint32(-1);
        if (!Function)
            return false;

        const auto Parameters = Function->GetParamsNamed();
        if (Parameters.Size <= 0 || Parameters.Size > 0x80 ||
            Parameters.NameOffsetMap.size() != 2)
        {
            return false;
        }

        constexpr uint64 CPF_Parm = 0x0000000000000080;
        constexpr uint64 CPF_OutParm = 0x0000000000000100;
        constexpr uint64 CPF_ReturnParm = 0x0000000000000400;
        for (const auto& Parameter : Parameters.NameOffsetMap)
        {
            if (!(Parameter.PropertyFlags & CPF_Parm) ||
                (Parameter.PropertyFlags &
                    (CPF_OutParm | CPF_ReturnParm)) ||
                Parameter.ElementSize != sizeof(void*) ||
                Parameter.Offset > Parameters.Size ||
                sizeof(void*) >
                    static_cast<uint32>(Parameters.Size) -
                        Parameter.Offset)
            {
                return false;
            }

            uint32* Destination = nullptr;
            if (Parameter.Name == "ExitCraft")
                Destination = &ExitCraftOffset;
            else if (Parameter.Name == "ExitCraftSpawner")
                Destination = &ExitCraftSpawnerOffset;
            else
                return false;

            if (*Destination != uint32(-1))
                return false;
            *Destination = Parameter.Offset;
        }

        if (ExitCraftOffset == uint32(-1) ||
            ExitCraftSpawnerOffset == uint32(-1) ||
            ExitCraftOffset == ExitCraftSpawnerOffset)
        {
            return false;
        }

        ParametersSize = static_cast<uint32>(Parameters.Size);
        return true;
    }

    bool CanInvokeExitCraftSpawned(UFunction* Function)
    {
        uint32 ParametersSize = 0;
        uint32 ExitCraftOffset = 0;
        uint32 ExitCraftSpawnerOffset = 0;
        return ResolveExitCraftCallbackLayout(
            Function,
            ParametersSize,
            ExitCraftOffset,
            ExitCraftSpawnerOffset);
    }

    bool InvokeExitCraftSpawned(
        UObject* Target,
        UFunction* Function,
        AFortAthenaExitCraft* ExitCraft,
        AFortAthenaExitCraftSpawner* ExitCraftSpawner)
    {
        if (!Target)
            return false;

        uint32 ParametersSize = 0;
        uint32 ExitCraftOffset = 0;
        uint32 ExitCraftSpawnerOffset = 0;
        if (!ResolveExitCraftCallbackLayout(
                Function,
                ParametersSize,
                ExitCraftOffset,
                ExitCraftSpawnerOffset))
        {
            return false;
        }

        void* Memory = FMemory::Malloc(ParametersSize);
        if (!Memory)
            return false;
        memset(Memory, 0, ParametersSize);
        memcpy(
            static_cast<uint8*>(Memory) + ExitCraftOffset,
            &ExitCraft,
            sizeof(ExitCraft));
        memcpy(
            static_cast<uint8*>(Memory) + ExitCraftSpawnerOffset,
            &ExitCraftSpawner,
            sizeof(ExitCraftSpawner));
        Target->ProcessEvent(Function, Memory);
        FMemory::Free(Memory);
        return true;
    }

    bool BroadcastGamePhaseStepChanged(
        AFortGameStateAthena* GameState,
        uint8 NewStep,
        std::unordered_set<UObject*>& InvokedTargets)
    {
        if (!GameState || !GameState->HasGamePhaseStepChanged())
            return false;

        auto& InvocationList =
            GameState->GamePhaseStepChanged.InvocationList;
        if (!IsSaneArray(
                InvocationList.Num(), InvocationList.Max(), 64) ||
            InvocationList.Num() == 0)
        {
            return false;
        }

        struct FPhaseDelegateSnapshot
        {
            FWeakObjectPtr Object;
            FName FunctionName;
        };
        std::vector<FPhaseDelegateSnapshot> Delegates;
        Delegates.reserve(InvocationList.Num());
        for (int32 Index = 0; Index < InvocationList.Num(); ++Index)
        {
            auto& Delegate =
                InvocationList.Get(Index, FScriptDelegate::Size());
            Delegates.push_back(
                { Delegate.Object, Delegate.FunctionName });
        }

        bool bInvoked = false;
        for (const auto& Delegate : Delegates)
        {
            auto Target =
                const_cast<UObject*>(Delegate.Object.Get());
            if (!Target ||
                !SDK::MemReadable(Target, sizeof(UObject)))
            {
                continue;
            }

            UFunction* Function =
                Target->GetFunction(Delegate.FunctionName);
            if (!InvokeGamePhaseStep(Target, Function, NewStep))
                continue;

            InvokedTargets.insert(Target);
            bInvoked = true;
        }

        return bInvoked;
    }

    uint8 DeriveGamePhaseStep(
        AFortGameStateAthena* GameState,
        float Now,
        float& TimeRemaining)
    {
        TimeRemaining = 0.0f;
        if (!GameState || !GameState->HasGamePhase())
            return static_cast<uint8>(EAthenaGamePhaseStep::None);

        switch (static_cast<EAthenaGamePhase>(GameState->GamePhase))
        {
        case EAthenaGamePhase::Setup:
            return static_cast<uint8>(EAthenaGamePhaseStep::Setup);

        case EAthenaGamePhase::Warmup:
            if (GameState->HasWarmupCountdownStartTime() &&
                GameState->WarmupCountdownStartTime < 0.0f)
            {
                return static_cast<uint8>(EAthenaGamePhaseStep::Setup);
            }
            if (GameState->HasWarmupCountdownEndTime() &&
                GameState->WarmupCountdownEndTime > Now)
            {
                TimeRemaining =
                    (std::max)(
                        GameState->WarmupCountdownEndTime - Now,
                        0.0f);
                return static_cast<uint8>(
                    TimeRemaining <= 10.0f
                        ? EAthenaGamePhaseStep::GetReady
                        : EAthenaGamePhaseStep::Warmup);
            }
            return static_cast<uint8>(EAthenaGamePhaseStep::GetReady);

        case EAthenaGamePhase::Aircraft:
        {
            uint8 Step =
                static_cast<uint8>(EAthenaGamePhaseStep::BusFlying);
            if (!GameState->HasAircrafts())
                return Step;

            auto& Aircrafts = GameState->Aircrafts;
            if (!IsSaneArray(Aircrafts.Num(), Aircrafts.Max(), 16))
                return Step;

            for (auto Aircraft : Aircrafts)
            {
                if (!Aircraft || !Aircraft->HasDropStartTime())
                    continue;
                if (Aircraft->DropStartTime > Now)
                {
                    Step =
                        static_cast<uint8>(
                            EAthenaGamePhaseStep::BusLocked);
                    TimeRemaining = (std::max)(
                        TimeRemaining,
                        Aircraft->DropStartTime - Now + 1.0f);
                }
            }
            return Step;
        }

        case EAthenaGamePhase::SafeZones:
        {
            if (GameState->HasbIsInFinalCountdown() &&
                GameState->bIsInFinalCountdown)
            {
                return static_cast<uint8>(
                    EAthenaGamePhaseStep::FinalCountdown);
            }
            if (GameState->HasbIsInCountdown() &&
                GameState->bIsInCountdown)
            {
                return static_cast<uint8>(
                    EAthenaGamePhaseStep::Countdown);
            }

            auto Indicator =
                GameState->HasSafeZoneIndicator()
                    ? GameState->SafeZoneIndicator
                    : nullptr;
            const UClass* IndicatorClass =
                AFortSafeZoneIndicator::StaticClass();
            if (!Indicator || !IndicatorClass ||
                !Indicator->IsA(IndicatorClass))
            {
                if (GameState->HasSafeZonesStartTime() &&
                    GameState->SafeZonesStartTime > Now)
                {
                    TimeRemaining =
                        GameState->SafeZonesStartTime - Now;
                }
                return static_cast<uint8>(
                    EAthenaGamePhaseStep::StormForming);
            }

            auto SafeZoneIndicator =
                static_cast<AFortSafeZoneIndicator*>(Indicator);
            if (!SafeZoneIndicator->HasSafeZoneStartShrinkTime() ||
                !SafeZoneIndicator->HasSafeZoneFinishShrinkTime())
            {
                return static_cast<uint8>(
                    EAthenaGamePhaseStep::StormForming);
            }

            if (SafeZoneIndicator->SafeZoneStartShrinkTime > Now)
            {
                TimeRemaining =
                    SafeZoneIndicator->SafeZoneStartShrinkTime - Now;
                return static_cast<uint8>(
                    EAthenaGamePhaseStep::StormHolding);
            }
            if (SafeZoneIndicator->SafeZoneFinishShrinkTime > Now)
            {
                TimeRemaining =
                    SafeZoneIndicator->SafeZoneFinishShrinkTime - Now;
                return static_cast<uint8>(
                    EAthenaGamePhaseStep::StormShrinking);
            }
            return static_cast<uint8>(
                EAthenaGamePhaseStep::StormHolding);
        }

        case EAthenaGamePhase::EndGame:
            return static_cast<uint8>(EAthenaGamePhaseStep::EndGame);

        default:
            return static_cast<uint8>(EAthenaGamePhaseStep::None);
        }
    }

    void UpdateAndNotifyHeistGamePhaseStep(
        AFortGameStateAthena* GameState,
        float Now)
    {
        if (!GameState || !GameState->HasGamePhaseStep())
            return;

        float TimeRemaining = 0.0f;
        const uint8 NewStep =
            DeriveGamePhaseStep(GameState, Now, TimeRemaining);
        if (NewStep ==
            static_cast<uint8>(EAthenaGamePhaseStep::None))
        {
            return;
        }

        if (GameState->HasGamePhaseStepTimeRemaining())
            GameState->GamePhaseStepTimeRemaining = TimeRemaining;

        const bool bNeedsCompatibilityWrite =
            GameState->GamePhaseStep != NewStep;
        const bool bChangedByCompatibility =
            bNeedsCompatibilityWrite &&
            (!GHeistCompatibilityState
                 .bPublishedCompatibilityPhaseStep ||
             GHeistCompatibilityState
                 .PublishedCompatibilityPhaseStep != NewStep);
        if (bNeedsCompatibilityWrite)
        {
            const uint8 PreviousStep = GameState->GamePhaseStep;
            GameState->GetGamePhaseStep() = NewStep;
            if (bChangedByCompatibility)
            {
                GHeistCompatibilityState
                    .bPublishedCompatibilityPhaseStep = true;
                GHeistCompatibilityState
                    .PublishedCompatibilityPhaseStep = NewStep;
                GHeistCompatibilityState.bObservedNativePhaseStep =
                    false;
                GameState->ForceNetUpdate();
                SDK::DbgLog(
                    "[Heist] GamePhaseStep %u -> %u "
                    "(remaining=%.2f)\n",
                    static_cast<unsigned>(PreviousStep),
                    static_cast<unsigned>(NewStep),
                    TimeRemaining);
            }
        }
        else if (
            !GHeistCompatibilityState
                 .bPublishedCompatibilityPhaseStep ||
            GHeistCompatibilityState.PublishedCompatibilityPhaseStep !=
                NewStep)
        {
            // Native phase publication reached this step before the
            // compatibility tick. Trust its delegate path and do not send a
            // second callback to mutators which already existed. Remember
            // those mutators so one which appears later can still receive the
            // current step on a subsequent tick.
            GHeistCompatibilityState.bPublishedCompatibilityPhaseStep =
                false;
            GHeistCompatibilityState.PublishedCompatibilityPhaseStep =
                NewStep;
            if (!GHeistCompatibilityState.bObservedNativePhaseStep ||
                GHeistCompatibilityState.ObservedNativePhaseStep !=
                    NewStep)
            {
                GHeistCompatibilityState.bObservedNativePhaseStep = true;
                GHeistCompatibilityState.ObservedNativePhaseStep =
                    NewStep;

                auto NativeHeists = FindHeistMutators(GameState);
                std::unordered_set<AFortAthenaMutator_Heist*>
                    LiveNativeHeists;
                for (auto Heist : NativeHeists)
                {
                    if (!Heist)
                        continue;
                    LiveNativeHeists.insert(Heist);
                    GHeistCompatibilityState
                        .LastNotifiedPhaseSteps[Heist] = NewStep;
                }

                for (auto Iterator =
                         GHeistCompatibilityState
                             .LastNotifiedPhaseSteps.begin();
                     Iterator !=
                         GHeistCompatibilityState
                             .LastNotifiedPhaseSteps.end();)
                {
                    if (!LiveNativeHeists.contains(Iterator->first))
                    {
                        Iterator =
                            GHeistCompatibilityState
                                .LastNotifiedPhaseSteps.erase(Iterator);
                    }
                    else
                    {
                        ++Iterator;
                    }
                }
                return;
            }
        }

        std::unordered_set<UObject*> BroadcastTargets;
        if (bChangedByCompatibility)
        {
            BroadcastGamePhaseStepChanged(
                GameState, NewStep, BroadcastTargets);
        }

        auto Heists = FindHeistMutators(GameState);
        std::unordered_set<AFortAthenaMutator_Heist*> LiveHeists;
        for (auto Heist : Heists)
        {
            if (!Heist)
                continue;
            LiveHeists.insert(Heist);

            auto Existing =
                GHeistCompatibilityState.LastNotifiedPhaseSteps.find(
                    Heist);
            if (Existing !=
                    GHeistCompatibilityState.LastNotifiedPhaseSteps.end() &&
                Existing->second == NewStep)
            {
                continue;
            }

            UFunction* OnGamePhaseStepChanged =
                Heist->GetFunction("OnGamePhaseStepChanged");
            if (BroadcastTargets.contains(Heist))
            {
                GHeistCompatibilityState.LastNotifiedPhaseSteps[Heist] =
                    NewStep;
                continue;
            }

            if (!InvokeGamePhaseStep(
                    Heist, OnGamePhaseStepChanged, NewStep))
                continue;

            GHeistCompatibilityState.LastNotifiedPhaseSteps[Heist] =
                NewStep;
            SDK::DbgLog(
                "[Heist] notified %s of GamePhaseStep=%u\n",
                Heist->Name.ToString().c_str(),
                static_cast<unsigned>(NewStep));
        }

        for (auto Iterator =
                 GHeistCompatibilityState.LastNotifiedPhaseSteps.begin();
             Iterator !=
                 GHeistCompatibilityState.LastNotifiedPhaseSteps.end();)
        {
            if (!LiveHeists.contains(Iterator->first))
                Iterator =
                    GHeistCompatibilityState.LastNotifiedPhaseSteps.erase(
                        Iterator);
            else
                ++Iterator;
        }
    }

    bool IsSpawnerAlreadyResolved(
        AFortAthenaExitCraftSpawner* Spawner,
        const std::vector<AFortAthenaMutator_Heist*>& Heists)
    {
        const UStruct* EntryStruct =
            FHeistExitCraftData::StaticStruct();
        if (!Spawner || !EntryStruct ||
            !FHeistExitCraftData::HasExitCraftSpawner() ||
            !FHeistExitCraftData::HasSpawnedExitCraft())
        {
            return false;
        }

        const int32 EntrySize = EntryStruct->GetPropertiesSize();
        if (EntrySize <= 0 || EntrySize > 0x100)
            return false;

        for (auto Heist : Heists)
        {
            if (!Heist || !Heist->HasSpawnedExitCraftList())
                continue;

            auto& Entries = Heist->SpawnedExitCraftList;
            if (!IsSaneArray(Entries.Num(), Entries.Max(), 16))
                continue;
            for (int32 Index = 0; Index < Entries.Num(); ++Index)
            {
                auto& Entry = Entries.Get(Index, EntrySize);
                if (Entry.ExitCraftSpawner == Spawner &&
                    Entry.SpawnedExitCraft)
                {
                    return true;
                }
            }
        }
        return false;
    }

    bool SpawnExitCraft(
        AFortAthenaExitCraftSpawner* Spawner,
        AFortGameStateAthena* GameState)
    {
        if (!Spawner || !GameState ||
            !Spawner->HasExitCraftInfo())
        {
            return false;
        }

        auto Heists = FindHeistMutators(GameState);
        if (Heists.empty())
            return false;
        if (IsSpawnerAlreadyResolved(Spawner, Heists))
        {
            Spawner->K2_DestroyActor();
            return true;
        }

        bool bHasSpawnCallback = false;
        for (auto Heist : Heists)
        {
            if (Heist &&
                CanInvokeExitCraftSpawned(
                    Heist->GetFunction("OnExitCraftSpawned")))
            {
                bHasSpawnCallback = true;
                break;
            }
        }
        if (!bHasSpawnCallback)
            return false;

        UFortAthenaExitCraftInfo* Info = Spawner->ExitCraftInfo;
        if (!Info || !Info->HasExitCaftClass())
            return false;

        const UClass* CraftClass = Info->ExitCaftClass.Get();
        const UClass* ReflectedCraftClass =
            AFortAthenaExitCraft::StaticClass();
        if (!CraftClass || !ReflectedCraftClass)
            return false;
        const UObject* CraftDefaultObject =
            CraftClass->GetDefaultObj();
        if (!CraftDefaultObject ||
            !CraftDefaultObject->IsA(ReflectedCraftClass))
        {
            SDK::DbgLog(
                "[Heist] rejected non-exit-craft class %s\n",
                CraftClass->Name.ToString().c_str());
            return false;
        }

        FVector SpawnLocation = Spawner->K2_GetActorLocation();
        const FRotator SpawnRotation = Spawner->K2_GetActorRotation();

        const UStruct* ExitCraftInfoStruct =
            FExitCraftInfo::StaticStruct();
        if (Info->HasExitCraftInfo() && ExitCraftInfoStruct &&
            FExitCraftInfo::HasExitCraftZOffset())
        {
            SpawnLocation.Z +=
                Info->ExitCraftInfo.ExitCraftZOffset.Evaluate(0.0f);
        }

        UWorld* World = UWorld::GetWorld();
        if (!World)
            return false;

        auto Craft = World->SpawnActorUnfinished<AFortAthenaExitCraft>(
            CraftClass, SpawnLocation, SpawnRotation);
        if (!Craft)
            return false;

        if (Craft->HasExitCraftInfo())
            Craft->ExitCraftInfo = Info;

        Craft = World->FinishSpawnActor<AFortAthenaExitCraft>(
            Craft, SpawnLocation, SpawnRotation);
        if (!Craft)
            return false;

        bool bNotified = false;
        const UStruct* EntryStruct =
            FHeistExitCraftData::StaticStruct();
        const int32 EntrySize =
            EntryStruct ? EntryStruct->GetPropertiesSize() : 0;
        const bool bCanUpdateEntry =
            EntrySize > 0 && EntrySize <= 0x100 &&
            FHeistExitCraftData::HasExitCraftSpawner() &&
            FHeistExitCraftData::HasSpawnedExitCraft();

        for (auto Heist : Heists)
        {
            if (!Heist)
                continue;

            if (bCanUpdateEntry &&
                Heist->HasSpawnedExitCraftList())
            {
                auto& Entries = Heist->SpawnedExitCraftList;
                if (IsSaneArray(Entries.Num(), Entries.Max(), 16))
                {
                    for (int32 Index = 0;
                         Index < Entries.Num(); ++Index)
                    {
                        auto& Entry =
                            Entries.Get(Index, EntrySize);
                        if (Entry.ExitCraftSpawner == Spawner &&
                            !Entry.SpawnedExitCraft)
                        {
                            Entry.SpawnedExitCraft = Craft;
                            break;
                        }
                    }
                }
            }

            UFunction* OnExitCraftSpawned =
                Heist->GetFunction("OnExitCraftSpawned");
            if (!InvokeExitCraftSpawned(
                    Heist,
                    OnExitCraftSpawned,
                    Craft,
                    Spawner))
            {
                continue;
            }
            Heist->ForceNetUpdate();
            bNotified = true;
        }

        if (!bNotified)
        {
            Craft->K2_DestroyActor();
            return false;
        }

        SDK::DbgLog(
            "[Heist] spawned exit craft %s from %s\n",
            Craft->Name.ToString().c_str(),
            Spawner->Name.ToString().c_str());
        Spawner->K2_DestroyActor();
        return true;
    }

    void TickExitCraftSpawners(
        AFortGameStateAthena* GameState,
        double Now)
    {
        if (!GameState ||
            Now < GHeistCompatibilityState.NextExitCraftDiscoveryTime)
        {
            return;
        }
        GHeistCompatibilityState.NextExitCraftDiscoveryTime =
            Now + ExitCraftDiscoveryInterval;

        const UClass* SpawnerClass =
            AFortAthenaExitCraftSpawner::StaticClass();
        if (!SpawnerClass)
            return;

        TArray<AActor*> SpawnerActors;
        Utils::GetAll(SpawnerClass, SpawnerActors);
        if (!IsSaneArray(
                SpawnerActors.Num(), SpawnerActors.Max(), 16))
        {
            SpawnerActors.Free();
            return;
        }

        std::unordered_set<AFortAthenaExitCraftSpawner*> LiveSpawners;
        for (auto Actor : SpawnerActors)
        {
            if (!Actor || !Actor->IsA(SpawnerClass))
                continue;

            auto Spawner =
                static_cast<AFortAthenaExitCraftSpawner*>(Actor);
            LiveSpawners.insert(Spawner);

            auto Scheduled =
                GHeistCompatibilityState.ScheduledExitCraftSpawners.find(
                    Spawner);
            if (Scheduled ==
                GHeistCompatibilityState.ScheduledExitCraftSpawners.end())
            {
                if (!Spawner->HasExitCraftInfo() ||
                    !Spawner->ExitCraftInfo ||
                    !Spawner->ExitCraftInfo->HasExitCraftInfo() ||
                    !FExitCraftInfo::StaticStruct() ||
                    !FExitCraftInfo::HasExitCraftSpawnDelay())
                {
                    continue;
                }

                float Delay =
                    Spawner->ExitCraftInfo->ExitCraftInfo
                        .ExitCraftSpawnDelay.Evaluate(0.0f);
                if (!std::isfinite(Delay) ||
                    Delay < 0.0f || Delay > 3600.0f)
                {
                    SDK::DbgLog(
                        "[Heist] rejected exit-craft delay %.2f on %s\n",
                        Delay,
                        Spawner->Name.ToString().c_str());
                    continue;
                }

                // StartExitCraftSpawnTimer normally performs this cleanup
                // before binding its stripped callback. The polling fallback
                // replaces that timer path, so preserve the cleanup when the
                // reflected no-parameter event is available.
                CallReflectedNoParams(
                    Spawner, "DestroyBlockingActors");

                Scheduled =
                    GHeistCompatibilityState
                        .ScheduledExitCraftSpawners
                        .emplace(Spawner, Now + Delay).first;
                SDK::DbgLog(
                    "[Heist] scheduled %s in %.2f seconds\n",
                    Spawner->Name.ToString().c_str(), Delay);
            }

            if (Now >= Scheduled->second &&
                SpawnExitCraft(Spawner, GameState))
            {
                GHeistCompatibilityState
                    .ScheduledExitCraftSpawners.erase(Spawner);
            }
        }

        for (auto Iterator =
                 GHeistCompatibilityState
                     .ScheduledExitCraftSpawners.begin();
             Iterator !=
                 GHeistCompatibilityState
                     .ScheduledExitCraftSpawners.end();)
        {
            if (!LiveSpawners.contains(Iterator->first))
                Iterator =
                    GHeistCompatibilityState
                        .ScheduledExitCraftSpawners.erase(Iterator);
            else
                ++Iterator;
        }

        SpawnerActors.Free();
    }
}

bool FFortAthenaHeistCompatibility::IsSupportedBuild()
{
    return VersionInfo.FortniteVersion >= HeistMinimumVersion &&
        VersionInfo.FortniteVersion < HeistEndVersionExclusive;
}

bool FFortAthenaHeistCompatibility::IsHeistPlaylist(
    const UFortPlaylistAthena* Playlist)
{
    if (!IsSupportedBuild() || !Playlist ||
        !AFortAthenaMutator_Heist::StaticClass())
    {
        return false;
    }

    if (FConfiguration::Playlist)
    {
        const std::wstring ConfiguredPath =
            FConfiguration::Playlist;
        if (IsHeistPlaylistIdentifier(ConfiguredPath.c_str()))
        {
            // SetupPlaylist falls back to DefaultSolo when the configured
            // asset cannot be resolved. Never let the configured path alone
            // turn that unrelated fallback object into a Heist playlist.
            static std::wstring CachedConfiguredPath;
            static const UFortPlaylistAthena*
                CachedConfiguredPlaylist =
                nullptr;
            static bool bConfiguredLookupAttempted = false;
            if (!bConfiguredLookupAttempted ||
                CachedConfiguredPath != ConfiguredPath)
            {
                bConfiguredLookupAttempted = true;
                CachedConfiguredPath = ConfiguredPath;
                CachedConfiguredPlaylist =
                    FindObject<UFortPlaylistAthena>(
                        FConfiguration::Playlist);
            }
            auto ConfiguredPlaylist = CachedConfiguredPlaylist;
            if (ConfiguredPlaylist == Playlist)
                return true;
        }
    }

    const auto ObjectName = Playlist->Name.ToWString();
    if (IsHeistPlaylistIdentifier(ObjectName.c_str()))
    {
        return true;
    }

    if (Playlist->HasPlaylistName())
    {
        const auto PlaylistName =
            Playlist->PlaylistName.ToWString();
        if (IsHeistPlaylistIdentifier(PlaylistName.c_str()))
        {
            return true;
        }
    }

    if (!Playlist->HasModifierList())
        return false;

    auto& ModifierList =
        const_cast<UFortPlaylistAthena*>(Playlist)->ModifierList;
    if (!IsSaneArray(
            ModifierList.Num(), ModifierList.Max(), 32))
    {
        return false;
    }

    const UClass* HeistClass =
        AFortAthenaMutator_Heist::StaticClass();
    for (int32 ModifierIndex = 0;
         ModifierIndex < ModifierList.Num(); ++ModifierIndex)
    {
        auto Modifier =
            ModifierList.Get(
                ModifierIndex, FSoftObjectPtr::Size()).Get();
        if (!Modifier || !Modifier->HasMutators())
            continue;

        auto& Mutators =
            const_cast<UFortGameplayModifierItemDefinition*>(Modifier)
                ->Mutators;
        if (!IsSaneArray(Mutators.Num(), Mutators.Max(), 32))
            continue;

        for (int32 MutatorIndex = 0;
             MutatorIndex < Mutators.Num(); ++MutatorIndex)
        {
            UClass* MutatorClass =
                Mutators.Get(
                    MutatorIndex, FSoftObjectPtr::Size()).Get();
            if (!MutatorClass)
                continue;
            const UObject* DefaultObject =
                MutatorClass->GetDefaultObj();
            if (MutatorClass == HeistClass ||
                (DefaultObject && DefaultObject->IsA(HeistClass)))
            {
                return true;
            }
        }
    }

    return false;
}

void FFortAthenaHeistCompatibility::PreparePlaylist(
    AFortGameStateAthena* GameState,
    const UFortPlaylistAthena* Playlist)
{
    UWorld* World = UWorld::GetWorld();
    if (!IsSupportedBuild() || !World || !GameState ||
        !IsHeistPlaylist(Playlist))
    {
        return;
    }

    if (GHeistCompatibilityState.World != World ||
        GHeistCompatibilityState.Playlist != Playlist)
    {
        ResetHeistCompatibilityState(World, Playlist);
    }
    if (GHeistCompatibilityState.bPlaylistPrepared)
        return;

    const bool bLoaded =
        GameState->HasbPlaylistDataIsLoaded() &&
        GameState->bPlaylistDataIsLoaded;
    const bool bLoading =
        GameState->HasbPlaylistDataIsActivelyLoading() &&
        GameState->bPlaylistDataIsActivelyLoading;
    if (bLoaded)
    {
        GHeistCompatibilityState.bPlaylistPrepared = true;
        SDK::DbgLog(
            "[Heist] playlist data already loaded; "
            "skipping duplicate load\n");
        return;
    }
    if (bLoading)
        return;

    UFunction* Initialize =
        GameState->GetFunction(
            "InitializePlaylistDataPreDataLoad");
    UFunction* Load =
        GameState->GetFunction("LoadCurrentPlaylistData");
    if (HasNoParameters(Initialize) &&
        HasNoParameters(Load))
    {
        GameState->Call<void>(Initialize);
        GameState->Call<void>(Load);
        GHeistCompatibilityState.bPlaylistPrepared = true;
        SDK::DbgLog(
            "[Heist] invoked reflected playlist initialization "
            "pipeline\n");
        return;
    }

    const uintptr_t NativeInitialize =
        FindNativeInitializePlaylistDataPreDataLoad();
    const uintptr_t NativeLoad =
        FindNativeLoadCurrentPlaylistData();
    if (!NativeInitialize || !NativeLoad)
    {
        if (!GHeistCompatibilityState
                 .bLoggedMissingPlaylistLoadFunctions)
        {
            GHeistCompatibilityState
                .bLoggedMissingPlaylistLoadFunctions = true;
            SDK::DbgLog(
                "[Heist] reflected playlist loaders unavailable "
                "(init=%p load=%p); validated native fallback "
                "incomplete (init=%p load=%p)\n",
                static_cast<void*>(Initialize),
                static_cast<void*>(Load),
                reinterpret_cast<void*>(NativeInitialize),
                reinterpret_cast<void*>(NativeLoad));
        }
        return;
    }

    using PlaylistDataFunction = void(*)(AFortGameStateAthena*);
    reinterpret_cast<PlaylistDataFunction>(NativeInitialize)(GameState);
    reinterpret_cast<PlaylistDataFunction>(NativeLoad)(GameState);
    GHeistCompatibilityState.bPlaylistPrepared = true;
    SDK::DbgLog(
        "[Heist] invoked validated native playlist initialization "
        "pipeline\n");
}

bool FFortAthenaHeistCompatibility::LoadAdditionalPlaylistLevels(
    AFortGameStateAthena* GameState,
    const UFortPlaylistAthena* Playlist)
{
    UWorld* World = UWorld::GetWorld();
    if (!IsSupportedBuild() || !World || !GameState ||
        !IsHeistPlaylist(Playlist))
    {
        return false;
    }

    if (GHeistCompatibilityState.World != World ||
        GHeistCompatibilityState.Playlist != Playlist)
    {
        ResetHeistCompatibilityState(World, Playlist);
    }
    if (GHeistCompatibilityState.bAdditionalLevelsComplete)
        return true;

    const UClass* StreamingClass =
        ULevelStreamingDynamic::StaticClass();
    if (!StreamingClass)
    {
        SDK::DbgLog(
            "[Heist] LevelStreamingDynamic class unavailable\n");
        return false;
    }

    int32 RequestedLevels = 0;
    int32 LoadedLevels = 0;
    int32 DeclaredLevels = 0;
    int32 ValidLevelEntries = 0;
    bool bInvalidLevelArray = false;
    auto LoadLevels =
        [&](TArray<TSoftObjectPtr<UWorld>>& Levels,
            bool bServerOnly)
        {
            if (!IsSaneArray(Levels.Num(), Levels.Max(), 64))
            {
                bInvalidLevelArray = true;
                return;
            }

            DeclaredLevels += Levels.Num();
            for (int32 Index = 0; Index < Levels.Num(); ++Index)
            {
                auto& Level =
                    Levels.Get(Index, FSoftObjectPtr::Size());
                const FName LevelName =
                    Level.ObjectID.AssetPathName;
                if (!LevelName.IsValid())
                {
                    bInvalidLevelArray = true;
                    continue;
                }
                ++ValidLevelEntries;
                if (HasStreamedPlaylistLevel(GameState, LevelName))
                    continue;

                ++RequestedLevels;
                bool bSuccess = false;
                ULevelStreamingDynamic::
                    LoadLevelInstanceBySoftObjectPtr(
                        World, Level, FVector(), FRotator(),
                        &bSuccess, FString(), nullptr);
                if (!bSuccess)
                {
                    SDK::DbgLog(
                        "[Heist] failed to request playlist level %s\n",
                        LevelName.ToString().c_str());
                    continue;
                }

                const bool bAdded =
                    AddStreamedPlaylistLevel(
                    GameState, LevelName, bServerOnly);
                if (!bAdded &&
                    !HasStreamedPlaylistLevel(
                        GameState, LevelName))
                {
                    SDK::DbgLog(
                        "[Heist] playlist level loaded but could not "
                        "be recorded: %s\n",
                        LevelName.ToString().c_str());
                    continue;
                }

                ++LoadedLevels;
                if (bAdded)
                {
                    CallReflectedNoParams(
                        GameState,
                        "OnFinishedStreamingAdditionalPlaylistLevel");
                }
            }
        };

    if (Playlist->HasAdditionalLevels())
        LoadLevels(Playlist->AdditionalLevels, false);
    if (Playlist->HasAdditionalLevelsServerOnly())
        LoadLevels(Playlist->AdditionalLevelsServerOnly, true);

    if (DeclaredLevels == 0 || ValidLevelEntries == 0)
    {
        if (!GHeistCompatibilityState
                 .bLoggedMissingAdditionalLevelData)
        {
            GHeistCompatibilityState
                .bLoggedMissingAdditionalLevelData = true;
            SDK::DbgLog(
                "[Heist] additional playlist level data is not "
                "available yet (declared=%d valid=%d)\n",
                DeclaredLevels, ValidLevelEntries);
        }
        return false;
    }

    if (LoadedLevels > 0)
        GameState->OnRep_AdditionalPlaylistLevelsStreamed();

    SDK::DbgLog(
        "[Heist] additional playlist levels requested=%d accepted=%d "
        "invalidArray=%d\n",
        RequestedLevels, LoadedLevels,
        bInvalidLevelArray ? 1 : 0);

    GHeistCompatibilityState.bAdditionalLevelsComplete =
        !bInvalidLevelArray &&
        ValidLevelEntries > 0 &&
        LoadedLevels == RequestedLevels;
    return GHeistCompatibilityState.bAdditionalLevelsComplete;
}

void FFortAthenaHeistCompatibility::Tick(
    UNetDriver* Driver,
    float DeltaSeconds)
{
    (void)DeltaSeconds;
    if (!IsSupportedBuild() || !Driver)
        return;

    UWorld* World = UWorld::GetWorld();
    if (!World || Driver != World->NetDriver ||
        !World->GameState)
    {
        return;
    }

    auto GameState =
        World->GameState->Cast<AFortGameStateAthena>();
    if (!GameState)
        return;

    UFortPlaylistAthena* Playlist = nullptr;
    if (GameState->HasCurrentPlaylistInfo() &&
        FPlaylistPropertyArray::HasBasePlaylist())
    {
        Playlist = const_cast<UFortPlaylistAthena*>(
            GameState->CurrentPlaylistInfo.BasePlaylist);
    }
    if (!Playlist && GameState->HasCurrentPlaylistData())
    {
        Playlist = const_cast<UFortPlaylistAthena*>(
            GameState->CurrentPlaylistData);
    }
    if (!IsHeistPlaylist(Playlist))
        return;

    if (GHeistCompatibilityState.World != World ||
        GHeistCompatibilityState.Playlist != Playlist)
    {
        ResetHeistCompatibilityState(World, Playlist);
    }

    const double Now =
        UGameplayStatics::GetTimeSeconds(World);
    if (!GHeistCompatibilityState.bPlaylistPrepared &&
        Now >= GHeistCompatibilityState
                   .NextPlaylistPreparationAttemptTime)
    {
        GHeistCompatibilityState.NextPlaylistPreparationAttemptTime =
            Now + 1.0;
        PreparePlaylist(GameState, Playlist);
    }
    if (!GHeistCompatibilityState.bAdditionalLevelsComplete &&
        Now >= GHeistCompatibilityState
                   .NextAdditionalLevelAttemptTime)
    {
        GHeistCompatibilityState.NextAdditionalLevelAttemptTime =
            Now + 1.0;
        LoadAdditionalPlaylistLevels(GameState, Playlist);
    }

    UpdateAndNotifyHeistGamePhaseStep(
        GameState, static_cast<float>(Now));
    TickExitCraftSpawners(GameState, Now);
}

void AFortAthenaMutator_GiveItemsAtGamePhaseStep::OnGamePhaseStepChanged(UObject* Context, FFrame& Stack)
{
    TScriptInterface<IInterface> SafeZoneInterface;
    uint8_t GamePhaseStep;
    
    Stack.StepCompiledIn(&SafeZoneInterface);
    Stack.StepCompiledIn(&GamePhaseStep);
    Stack.IncrementCode();
    auto Mutator = (AFortAthenaMutator_GiveItemsAtGamePhaseStep*)Context;

    if (GamePhaseStep == Mutator->PhaseToGiveItems)
        for (auto& UncastedPC : Mutator->CachedGameMode->AlivePlayers)
        {
            auto PlayerController = (AFortPlayerControllerAthena*)UncastedPC;

            for (auto& Item : Mutator->ItemsToGive)
                PlayerController->WorldInventory->GiveItem(Item.ItemToDrop, (int)Item.NumberToGive.Evaluate());
        }
}

void AFortAthenaMutator_GiveItemsAtGamePhase::OnGamePhaseChanged(UObject* Context, FFrame& Stack)
{
    uint8_t GamePhase;

    Stack.StepCompiledIn(&GamePhase);
    Stack.IncrementCode();
    auto Mutator = (AFortAthenaMutator_GiveItemsAtGamePhase*)Context;

    if (GamePhase == Mutator->PhaseToGiveItems)
        for (auto& UncastedPC : Mutator->CachedGameMode->AlivePlayers)
        {
            auto PlayerController = (AFortPlayerControllerAthena*)UncastedPC;

            for (auto& Item : Mutator->ItemsToGive)
                PlayerController->WorldInventory->GiveItem(Item.ItemToDrop, (int)Item.NumberToGive.Evaluate());
        }
}

void AFortAthenaMutator_GiveItemsAtGamePhaseStep::PostLoadHook()
{
    if (!GetDefaultObj())
        return;

    Utils::ExecHook(GetDefaultObj()->GetFunction("OnGamePhaseStepChanged"), OnGamePhaseStepChanged);
}

void AFortAthenaMutator_GiveItemsAtGamePhase::PostLoadHook()
{
    if (!GetDefaultObj())
        return;

    Utils::ExecHook(GetDefaultObj()->GetFunction("OnGamePhaseChanged"), OnGamePhaseChanged);
}
