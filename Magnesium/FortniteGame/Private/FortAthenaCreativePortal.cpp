#include "pch.h"
#include "../Public/FortAthenaCreativePortal.h"
#include "../Public/FortGameStateAthena.h"
#include "../../Erbium/Public/Configuration.h"
#include "../../Erbium/Support/Public/VersionFeatureAdapter.h"

namespace
{
    constexpr uint8 CreativeEditPermission = 1;

    bool IsLiveObject(const UObject* Object)
    {
        return Object && SDK::MemReadable(Object, sizeof(UObject)) &&
            Object->Class && SDK::MemReadable(Object->Class, sizeof(UObject));
    }

    int32 GetUniqueIdSize()
    {
        auto Struct = FUniqueNetIdRepl::StaticStruct();
        if (!Struct)
            return 0;

        const int32 ReflectedSize = Struct->GetPropertiesSize();
        if (ReflectedSize > 0 && ReflectedSize <= static_cast<int32>(sizeof(FUniqueNetIdRepl)))
        {
            return ReflectedSize;
        }

        const uint32 BytesOffset = Struct->GetOffset("ReplicationBytes");
        if (BytesOffset == static_cast<uint32>(-1))
            return 0;

        const uint32 InferredSize = (BytesOffset + sizeof(TArray<uint8>) + 7u) & ~7u;
        return InferredSize <= sizeof(FUniqueNetIdRepl) ? static_cast<int32>(InferredSize) : 0;
    }

    FUniqueNetIdRepl* GetPlayerUniqueId(AFortPlayerStateAthena* PlayerState)
    {
        if (!IsLiveObject(PlayerState))
            return nullptr;

        if (PlayerState->HasUniqueID())
            return &PlayerState->UniqueID;
        if (PlayerState->HasUniqueId())
            return &PlayerState->UniqueId;
        return nullptr;
    }

    bool UniqueIdsEqual(const FUniqueNetIdRepl& Left, const FUniqueNetIdRepl& Right);
    bool HasUniqueIdValue(const FUniqueNetIdRepl& Id);

    bool IsZeroUniqueId(const FUniqueNetIdRepl& Id)
    {
        const int32 Size = GetUniqueIdSize();
        if (Size <= 0 || !SDK::MemReadable(&Id, Size))
            return true;

        const auto Bytes = reinterpret_cast<const uint8*>(&Id);
        for (int32 Index = 0; Index < Size; ++Index)
        {
            if (Bytes[Index] != 0)
                return false;
        }
        return true;
    }

    bool CallUniqueIdPredicate(const char* FunctionName, const char* LeftParameterName,
        const FUniqueNetIdRepl& Left, const char* RightParameterName, const FUniqueNetIdRepl* Right,
        bool& Result)
    {
        auto Library = const_cast<UObject*>(DefaultObjImpl("FortKismetLibrary"));
        auto Function = Library ? Library->GetFunction(FunctionName) : nullptr;
        const int32 IdSize = GetUniqueIdSize();
        if (!Function || IdSize <= 0)
            return false;

        const uint32 LeftOffset = Function->GetOffset(LeftParameterName);
        const uint32 ReturnOffset = Function->GetOffset("ReturnValue");
        const uint32 RightOffset = Right ? Function->GetOffset(RightParameterName) : 0;
        if (LeftOffset == static_cast<uint32>(-1) || ReturnOffset == static_cast<uint32>(-1) ||
            (Right && RightOffset == static_cast<uint32>(-1)) || LeftOffset + IdSize > 0x1000 ||
            ReturnOffset >= 0x1000 || (Right && RightOffset + IdSize > 0x1000))
        {
            return false;
        }

        const int32 ReflectedSize = Function->GetPropertiesSize();
        const size_t BufferSize = ReflectedSize > 0 && ReflectedSize <= 0x1000
            ? static_cast<size_t>(ReflectedSize) : 0x1000;
        if (LeftOffset + IdSize > BufferSize || ReturnOffset >= BufferSize ||
            (Right && RightOffset + IdSize > BufferSize))
        {
            return false;
        }

        std::vector<uint8> Params(BufferSize, 0);
        memcpy(Params.data() + LeftOffset, &Left, IdSize);
        if (Right)
            memcpy(Params.data() + RightOffset, Right, IdSize);
        Library->ProcessEvent(Function, Params.data());
        Result = Params[ReturnOffset] != 0;
        return true;
    }

    bool CopyUniqueId(FUniqueNetIdRepl& Destination, const FUniqueNetIdRepl& Source,
        AFortPlayerControllerAthena* PlayerController)
    {
        if (!HasUniqueIdValue(Source))
            return false;
        if (HasUniqueIdValue(Destination))
            return UniqueIdsEqual(Destination, Source);

        // FUniqueNetIdRepl's account handle is opaque and cannot be rebuilt from reflected bytes.
        auto CloneFunction = IsLiveObject(PlayerController)
            ? PlayerController->GetFunction("GetGameAccountId") : nullptr;
        const int32 IdSize = GetUniqueIdSize();
        if (CloneFunction && IdSize > 0)
        {
            const uint32 ReturnOffset = CloneFunction->GetOffset("ReturnValue");
            if (ReturnOffset != static_cast<uint32>(-1) && ReturnOffset + IdSize <= 0x1000)
            {
                const int32 ReflectedSize = CloneFunction->GetPropertiesSize();
                const size_t BufferSize = ReflectedSize > 0 && ReflectedSize <= 0x1000
                    ? static_cast<size_t>(ReflectedSize) : 0x1000;
                if (ReturnOffset + IdSize <= BufferSize)
                {
                    std::vector<uint8> Params(BufferSize, 0);
                    // Generated thunks assign into an already-constructed result, so seed the wrapper's vtable first.
                    memcpy(Params.data() + ReturnOffset, &Source, sizeof(void*));
                    PlayerController->ProcessEvent(CloneFunction, Params.data());
                    auto OwnedClone = reinterpret_cast<FUniqueNetIdRepl*>(
                            Params.data() + ReturnOffset);
                    if (HasUniqueIdValue(*OwnedClone) && UniqueIdsEqual(*OwnedClone, Source))
                    {
                        memcpy(&Destination, OwnedClone, IdSize);
                        memset(OwnedClone, 0, IdSize);
                        return true;
                    }
                }
            }
        }

        return false;
    }

    bool UniqueIdsEqual(const FUniqueNetIdRepl& Left, const FUniqueNetIdRepl& Right)
    {
        bool NativeResult = false;
        if (CallUniqueIdPredicate("EqualEqual_UniqueNetIdReplUniqueNetIdRepl",
                "A", Left, "B", &Right, NativeResult))
        {
            return NativeResult;
        }

        if (FUniqueNetIdRepl::StaticStruct() && FUniqueNetIdRepl::HasReplicationBytes())
        {
            const auto& LeftBytes = Left.ReplicationBytes;
            const auto& RightBytes = Right.ReplicationBytes;
            if (LeftBytes.Num() != RightBytes.Num() ||
                LeftBytes.Num() <= 0 || LeftBytes.Num() > 0x100)
            {
                return false;
            }

            const size_t Bytes = static_cast<size_t>(LeftBytes.Num());
            return SDK::MemReadable(LeftBytes.Data, Bytes) &&
                SDK::MemReadable(RightBytes.Data, Bytes) &&
                memcmp(LeftBytes.Data, RightBytes.Data, Bytes) == 0;
        }

        const int32 Size = GetUniqueIdSize();
        return Size > 0 && SDK::MemReadable(&Left, Size) && SDK::MemReadable(&Right, Size) &&
            memcmp(&Left, &Right, Size) == 0;
    }

    bool HasUniqueIdValue(const FUniqueNetIdRepl& Id)
    {
        if (IsZeroUniqueId(Id))
            return false;

        bool NativeResult = false;
        if (CallUniqueIdPredicate("IsValid_UniqueNetIdRepl",
                "InUniqueNetIdRepl", Id, nullptr, nullptr, NativeResult))
        {
            return NativeResult;
        }

        if (FUniqueNetIdRepl::StaticStruct() && FUniqueNetIdRepl::HasReplicationBytes())
        {
            const auto& Bytes = Id.ReplicationBytes;
            return Bytes.Num() > 0 && Bytes.Num() <= 0x100 && SDK::MemReadable(Bytes.Data,
                    static_cast<size_t>(Bytes.Num()));
        }

        const int32 Size = GetUniqueIdSize();
        if (Size <= 0 || !SDK::MemReadable(&Id, Size))
            return false;

        const auto Bytes = reinterpret_cast<const uint8*>(&Id);
        for (int32 Index = 0; Index < Size; ++Index)
        {
            if (Bytes[Index] != 0)
                return true;
        }
        return false;
    }

    void NotifyNoArgs(UObject* Object, const char* FunctionName)
    {
        if (!IsLiveObject(Object))
            return;

        if (auto Function = Object->GetFunction(FunctionName))
            VersionFeatureAdapter::SafeCallNoArgs(Object, Function);
    }

    void MarkDirty(const UObject* Object, const wchar_t* PropertyName)
    {
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(Object, PropertyName);
    }

    bool AddReadyPlayer(AFortAthenaCreativePortal* Portal, const FUniqueNetIdRepl& PlayerId,
        AFortPlayerControllerAthena* PlayerController)
    {
        if (!Portal || !Portal->HasPlayersReady())
            return false;

        const int32 ElementSize = GetUniqueIdSize();
        if (ElementSize <= 0)
            return false;

        auto& ReadyPlayers = Portal->PlayersReady;
        if (ReadyPlayers.Num() < 0 || ReadyPlayers.Num() > 0x100 || (ReadyPlayers.Num() > 0 &&
             !SDK::MemReadable(ReadyPlayers.Data,
                 static_cast<size_t>(ReadyPlayers.Num()) * ElementSize)))
        {
            return false;
        }

        for (int32 Index = 0; Index < ReadyPlayers.Num(); ++Index)
        {
            if (UniqueIdsEqual(ReadyPlayers.Get(Index, ElementSize), PlayerId))
                return true;
        }

        FUniqueNetIdRepl OwnedPlayerId{};
        if (!CopyUniqueId(OwnedPlayerId, PlayerId, PlayerController))
            return false;
        ReadyPlayers.Add(OwnedPlayerId, ElementSize);
        MarkDirty(Portal, L"PlayersReady");
        NotifyNoArgs(Portal, "OnRep_PlayersReady");
        return true;
    }

    const UFortCreativeRealEstatePlotItemDefinition* ResolveCreativePlot()
    {
        auto Plot = FindObject<UFortCreativeRealEstatePlotItemDefinition>(
            FConfiguration::CreativePlot);
        if (!Plot)
        {
            Plot = FindObject<UFortCreativeRealEstatePlotItemDefinition>(
                L"/Game/Playgrounds/Items/Plots/Temperate_Medium.Temperate_Medium");
        }
        if (!Plot)
        {
            Plot = FindObject<UFortCreativeRealEstatePlotItemDefinition>(
                L"/CR_Legacy/Playgrounds/Items/Plots/Temperate_Medium.Temperate_Medium");
        }
        return Plot;
    }

    void SetPermissionData(UObject* PermissionOwner, FUniqueNetIdRepl* AccountId,
        FFortCreativePlotPermissionData* PlotPermissions)
    {
        if (!IsLiveObject(PermissionOwner) || !AccountId)
            return;

        if (PlotPermissions && FFortCreativePlotPermissionData::StaticStruct() &&
            FFortCreativePlotPermissionData::HasPermission())
        {
            PlotPermissions->GetPermission() = CreativeEditPermission;
            MarkDirty(PermissionOwner, L"PlotPermissions");
        }

        MarkDirty(PermissionOwner, L"AccountIdOfOwner");
        NotifyNoArgs(PermissionOwner, "OnRep_AccountIdOfOwner");
        NotifyNoArgs(PermissionOwner, "OnRep_PlotPermissionsChanged");
    }

    bool DoesLevelSaveAllowEditing(UFortLevelSaveComponent* LevelSave,
        const FUniqueNetIdRepl& PlayerId)
    {
        if (!IsLiveObject(LevelSave))
            return false;
        auto Check = LevelSave->GetFunction("DoesPlayerHavePermissionToEdit");
        return !Check || LevelSave->Call<bool>(Check, PlayerId);
    }

    UPlayspaceComponent_CreativeToolsPermission*
        FindPlayspacePermissionComponent(AFortVolume* Volume)
    {
        if (!IsLiveObject(Volume))
            return nullptr;

        auto PermissionClass = UPlayspaceComponent_CreativeToolsPermission::StaticClass();
        if (!PermissionClass)
            return nullptr;

        if (auto Component = Volume->GetComponentByClass(PermissionClass))
        {
            return (UPlayspaceComponent_CreativeToolsPermission*)Component;
        }

        if (Volume->HasPlayspace() && IsLiveObject(Volume->Playspace))
        {
            return (UPlayspaceComponent_CreativeToolsPermission*)
                Volume->Playspace->GetComponentByClass(PermissionClass);
        }

        return nullptr;
    }

    bool ApplyCreativeVolumePermission(AFortVolume* Volume, AFortPlayerStateAthena* PlayerState,
        AFortPlayerControllerAthena* PlayerController,
        const UFortCreativeRealEstatePlotItemDefinition* Plot)
    {
        if (!IsLiveObject(Volume) || !IsLiveObject(PlayerState))
            return false;

        auto PlayerId = GetPlayerUniqueId(PlayerState);
        if (!PlayerId)
            return false;

        bool bAppliedPermission = false;

        auto LevelSaveClass = UFortLevelSaveComponent::StaticClass();
        auto LevelSave = LevelSaveClass ? (UFortLevelSaveComponent*)
                Volume->GetComponentByClass(LevelSaveClass) : nullptr;
        if (IsLiveObject(LevelSave))
        {
            if (LevelSave->HasAccountIdOfOwner())
            {
                bAppliedPermission |= CopyUniqueId(LevelSave->AccountIdOfOwner, *PlayerId,
                    PlayerController);
            }
            if (LevelSave->HasbIsLoaded())
                LevelSave->bIsLoaded = true;
            if (LevelSave->HasbLoadPlaysetFromPlot())
                LevelSave->bLoadPlaysetFromPlot = true;
            if (LevelSave->HasbAutoLoadFromRestrictedPlotDefinition())
                LevelSave->bAutoLoadFromRestrictedPlotDefinition = true;
            if (Plot && LevelSave->HasRestrictedPlotDefinition())
                LevelSave->RestrictedPlotDefinition = Plot;

            FFortCreativePlotPermissionData* PermissionData = nullptr;
            if (LevelSave->HasPlotPermissions())
            {
                PermissionData = &LevelSave->PlotPermissions;
                bAppliedPermission = true;
            }
            SetPermissionData(LevelSave, PlayerId, PermissionData);

            if (!DoesLevelSaveAllowEditing(LevelSave, *PlayerId))
            {
                SDK::DbgLog(
                    "[Creative] warning: level-save permission predicate still denied the portal owner\n");
            }

            MarkDirty(LevelSave, L"bIsLoaded");
            MarkDirty(LevelSave, L"bLoadPlaysetFromPlot");
            MarkDirty(LevelSave, L"bAutoLoadFromRestrictedPlotDefinition");
            MarkDirty(LevelSave, L"RestrictedPlotDefinition");
        }

        auto PlayspacePermission = FindPlayspacePermissionComponent(Volume);
        if (IsLiveObject(PlayspacePermission))
        {
            if (PlayspacePermission->HasAccountIdOfOwner())
            {
                bAppliedPermission |= CopyUniqueId(PlayspacePermission->AccountIdOfOwner, *PlayerId,
                    PlayerController);
            }

            FFortCreativePlotPermissionData* PermissionData = nullptr;
            if (PlayspacePermission->HasPlotPermissions())
            {
                PermissionData = &PlayspacePermission->PlotPermissions;
                bAppliedPermission = true;
            }
            SetPermissionData(PlayspacePermission, PlayerId, PermissionData);
        }

        if (Volume->HasbNeverAllowSaving())
        {
            Volume->bNeverAllowSaving = false;
            MarkDirty(Volume, L"bNeverAllowSaving");
        }
        if (Volume->HasVolumeState())
        {
            Volume->VolumeState = 3;
            MarkDirty(Volume, L"VolumeState");
            NotifyNoArgs(Volume, "OnRep_VolumeState");
        }

        Volume->ForceNetUpdate();
        return bAppliedPermission;
    }

    void EnablePlayerCreativeEditing(AFortPlayerStateAthena* PlayerState)
    {
        if (!IsLiveObject(PlayerState))
            return;

        auto CanEditFunction = PlayerState->GetFunction("CanEditCreativeIsland");
        if (CanEditFunction && PlayerState->Call<bool>(CanEditFunction))
            return;

        auto Setter = PlayerState->GetFunction("Server_SetCanEditCreativeIsland");
        if (!Setter)
            return;

        const uint32 CanEditOffset = Setter->GetOffset("bCanEdit");
        if (CanEditOffset == static_cast<uint32>(-1) || CanEditOffset >= 0x1000)
        {
            return;
        }

        const int32 ReflectedSize = Setter->GetPropertiesSize();
        const size_t BufferSize = ReflectedSize > 0 && ReflectedSize <= 0x1000
            ? static_cast<size_t>(ReflectedSize) : 0x1000;
        if (CanEditOffset >= BufferSize)
            return;

        std::vector<uint8> Params(BufferSize, 0);
        Params[CanEditOffset] = 1;
        PlayerState->ProcessEvent(Setter, Params.data());
        MarkDirty(PlayerState, L"bCanEditCreativeIsland");
    }

    void LinkPortalAndVolume(AFortAthenaCreativePortal* Portal,
        AFortPlayerControllerAthena* PlayerController, AFortPlayerStateAthena* PlayerState)
    {
        if (!IsLiveObject(Portal) || !IsLiveObject(PlayerController) ||
            !IsLiveObject(PlayerState) || !Portal->HasLinkedVolume() ||
            !IsLiveObject(Portal->LinkedVolume))
        {
            return;
        }

        auto PlayerId = GetPlayerUniqueId(PlayerState);
        if (PlayerId && Portal->HasOwningPlayer())
        {
            CopyUniqueId(Portal->OwningPlayer, *PlayerId, PlayerController);
            MarkDirty(Portal, L"OwningPlayer");
            NotifyNoArgs(Portal, "OnRep_OwningPlayer");
            AddReadyPlayer(Portal, *PlayerId, PlayerController);
        }

        if (Portal->HasbIsPublishedPortal())
        {
            Portal->bIsPublishedPortal = false;
            MarkDirty(Portal, L"bIsPublishedPortal");
            NotifyNoArgs(Portal, "OnRep_PublishedPortal");
        }
        if (Portal->HasbPortalOpen())
        {
            Portal->bPortalOpen = true;
            MarkDirty(Portal, L"bPortalOpen");
            NotifyNoArgs(Portal, "OnRep_PortalOpen");
        }
        if (Portal->HasbUserInitiatedLoad())
            Portal->bUserInitiatedLoad = true;
        if (Portal->HasbInErrorState())
            Portal->bInErrorState = false;

        auto Volume = Portal->LinkedVolume;
        if (Volume->HasLinkedPortals() && !Volume->LinkedPortals.Contains((AActor*)Portal))
        {
            Volume->LinkedPortals.Add((AActor*)Portal);
        }

        if (PlayerController->HasOwnedPortal())
            PlayerController->OwnedPortal = Portal;
        if (PlayerController->HasCreativePlotLinkedVolume())
        {
            PlayerController->CreativePlotLinkedVolume = Volume;
            MarkDirty(PlayerController, L"CreativePlotLinkedVolume");
            NotifyNoArgs(PlayerController, "OnRep_CreativePlotLinkedVolume");
        }

        MarkDirty(Portal, L"bUserInitiatedLoad");
        MarkDirty(Portal, L"bInErrorState");
        Portal->ForceNetUpdate();
        Volume->ForceNetUpdate();
    }

    AFortAthenaCreativePortal* FindPortalForPlayer(AFortCreativePortalManager* Manager,
        AFortPlayerControllerAthena* PlayerController, AFortPlayerStateAthena* PlayerState)
    {
        if (!IsLiveObject(Manager) || !IsLiveObject(PlayerController) || !IsLiveObject(PlayerState))
        {
            return nullptr;
        }

        if (PlayerController->HasOwnedPortal() && IsLiveObject(PlayerController->OwnedPortal) &&
            PlayerController->OwnedPortal->IsA(AFortAthenaCreativePortal::StaticClass()))
        {
            auto Existing = (AFortAthenaCreativePortal*)
                PlayerController->OwnedPortal;
            if (Existing->HasLinkedVolume() && IsLiveObject(Existing->LinkedVolume))
            {
                return Existing;
            }
        }

        auto PlayerId = GetPlayerUniqueId(PlayerState);
        if (!PlayerId)
            return nullptr;

        if (Manager->HasAllPortals())
        {
            for (auto Actor : Manager->AllPortals)
            {
                if (!IsLiveObject(Actor) || !Actor->IsA(AFortAthenaCreativePortal::StaticClass()))
                {
                    continue;
                }

                auto Portal = (AFortAthenaCreativePortal*)Actor;
                if (Portal->HasOwningPlayer() && UniqueIdsEqual(Portal->OwningPlayer, *PlayerId))
                {
                    return Portal;
                }
            }
        }

        AFortAthenaCreativePortal* Portal = nullptr;
        if (Manager->HasAvailablePortals() && Manager->AvailablePortals.Num() > 0)
        {
            for (auto Actor : Manager->AvailablePortals)
            {
                if (!IsLiveObject(Actor) || !Actor->IsA(AFortAthenaCreativePortal::StaticClass()))
                {
                    continue;
                }

                auto Candidate = (AFortAthenaCreativePortal*)Actor;
                if (Candidate->HasOwningPlayer() && HasUniqueIdValue(Candidate->OwningPlayer) &&
                    !UniqueIdsEqual(Candidate->OwningPlayer, *PlayerId))
                {
                    continue;
                }

                Portal = Candidate;
                break;
            }
        }

        if (!Portal && Manager->HasAllPortals())
        {
            for (auto Actor : Manager->AllPortals)
            {
                if (!IsLiveObject(Actor) || !Actor->IsA(AFortAthenaCreativePortal::StaticClass()))
                {
                    continue;
                }

                auto Candidate = (AFortAthenaCreativePortal*)Actor;
                if (Candidate->HasOwningPlayer() && HasUniqueIdValue(Candidate->OwningPlayer))
                {
                    continue;
                }

                Portal = Candidate;
                break;
            }
        }

        if (!Portal)
            return nullptr;

        if (auto MarkUsed = Manager->GetFunction("MarkPortalUsed"))
        {
            Manager->Call<void>(MarkUsed, Portal);
            return Portal;
        }

        if (Manager->HasAvailablePortals())
        {
            for (int32 Index = 0;
                 Index < Manager->AvailablePortals.Num(); ++Index)
            {
                if (Manager->AvailablePortals[Index] == Portal)
                {
                    Manager->AvailablePortals.Remove(Index);
                    break;
                }
            }
        }
        if (Manager->HasUsedPortals() && !Manager->UsedPortals.Contains((AActor*)Portal))
        {
            Manager->UsedPortals.Add((AActor*)Portal);
        }

        return Portal;
    }

    void ConfigurePlayset(AFortAthenaCreativePortal* Portal,
        const UFortCreativeRealEstatePlotItemDefinition* Plot)
    {
        if (!IsLiveObject(Portal) || !Portal->HasLinkedVolume() ||
            !IsLiveObject(Portal->LinkedVolume) || !Plot || !Plot->HasBasePlayset())
        {
            return;
        }

        auto IslandPlayset = Plot->BasePlayset.Get();
        if (!IslandPlayset)
            return;

        auto StreamClass = UPlaysetLevelStreamComponent::StaticClass();
        auto Stream = StreamClass ? (UPlaysetLevelStreamComponent*)
                Portal->LinkedVolume->GetComponentByClass(StreamClass) : nullptr;
        if (!IsLiveObject(Stream))
            return;

        if (auto SetPlayset = Stream->GetFunction("SetPlayset"))
            Stream->Call<void>(SetPlayset, IslandPlayset);
        else if (Stream->HasCurrentPlayset())
        {
            Stream->CurrentPlayset = IslandPlayset;
            NotifyNoArgs(Stream, "OnRep_CurrentPlayset");
        }

        if (Stream->HasbAutoLoadLevel())
            Stream->bAutoLoadLevel = true;
        if (Stream->HasbAutoActivate())
            Stream->bAutoActivate = true;

        auto LoadPlayset = (void (*)(UPlaysetLevelStreamComponent*))FindLoadPlayset();
        if (LoadPlayset)
            LoadPlayset(Stream);
    }

    void EnsureSettingsMachine(AFortAthenaCreativePortal* Portal)
    {
        if (!IsLiveObject(Portal) || !Portal->HasLinkedVolume() ||
            !IsLiveObject(Portal->LinkedVolume))
        {
            return;
        }

        auto Volume = Portal->LinkedVolume;
        UFortMinigameVolumeComponent* MinigameVolume = nullptr;
        if (auto MinigameClass = UFortMinigameVolumeComponent::StaticClass())
        {
            MinigameVolume = (UFortMinigameVolumeComponent*)
                Volume->GetComponentByClass(MinigameClass);
        }

        AMinigameSettingsMachine_C* SettingsMachine = nullptr;
        if (IsLiveObject(MinigameVolume) && MinigameVolume->HasCurrentMinigameSettingsMachine() &&
            IsLiveObject((UObject*)
                MinigameVolume->CurrentMinigameSettingsMachine))
        {
            SettingsMachine = (AMinigameSettingsMachine_C*)
                MinigameVolume->CurrentMinigameSettingsMachine;
        }

        if (!SettingsMachine)
        {
            auto SettingsMachineClass = FindObject<UClass>(
                L"/Game/Athena/Items/Gameplay/MinigameSettingsControl/MinigameSettingsMachine.MinigameSettingsMachine_C");
            if (SettingsMachineClass)
            {
                SettingsMachine = UWorld::SpawnActor<AMinigameSettingsMachine_C>(
                        SettingsMachineClass, Volume->K2_GetActorLocation(), {}, Volume);
            }
        }

        if (!IsLiveObject(SettingsMachine))
            return;

        if (SettingsMachine->HasSettingsVolume())
        {
            SettingsMachine->SettingsVolume = Volume;
            NotifyNoArgs(SettingsMachine, "OnRep_SettingsVolume");
        }
        if (SettingsMachine->HasCreativeLinkComponent())
        {
            auto LinkClass = UFortCreativeVolumeLinkComponent::StaticClass();
            SettingsMachine->CreativeLinkComponent = LinkClass ? (UFortCreativeVolumeLinkComponent*)
                    Volume->GetComponentByClass(LinkClass) : nullptr;
        }
        if (IsLiveObject(MinigameVolume) && MinigameVolume->HasCurrentMinigameSettingsMachine())
        {
            MinigameVolume->CurrentMinigameSettingsMachine = SettingsMachine;
        }
    }

    void EnableCreativeControllerState(AFortPlayerControllerAthena* PlayerController)
    {
        if (!IsLiveObject(PlayerController))
            return;

        if (PlayerController->HasbIsCreativeQuickbarEnabled())
        {
            const bool OldValue = PlayerController->bIsCreativeQuickbarEnabled;
            PlayerController->bIsCreativeQuickbarEnabled = true;
            PlayerController->OnRep_IsCreativeQuickbarEnabled(OldValue);
            MarkDirty(PlayerController, L"bIsCreativeQuickbarEnabled");
        }
        if (PlayerController->HasbIsCreativeQuickmenuEnabled())
        {
            PlayerController->bIsCreativeQuickmenuEnabled = true;
            MarkDirty(PlayerController, L"bIsCreativeQuickmenuEnabled");
            NotifyNoArgs(PlayerController, "OnRep_IsCreativeQuickmenuEnabled");
        }
        if (PlayerController->HasbIsCreativeModeEnabled())
        {
            PlayerController->bIsCreativeModeEnabled = true;
            PlayerController->OnRep_IsCreativeModeEnabled();
            MarkDirty(PlayerController, L"bIsCreativeModeEnabled");
        }
    }

    void GiveCreativePhone(AFortPlayerControllerAthena* PlayerController)
    {
        if (!IsLiveObject(PlayerController) || !IsLiveObject(PlayerController->WorldInventory))
        {
            return;
        }

        auto CreativePhone = FindObject<UFortWeaponItemDefinition>(
            L"/Game/Athena/Items/Weapons/Prototype/WID_CreativeTool.WID_CreativeTool");
        if (!CreativePhone)
            CreativePhone = FindObject<UFortWeaponItemDefinition>(L"WID_CreativeTool");
        if (!CreativePhone)
            return;

        auto ItemEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Entry)
                {
                    return Entry.ItemDefinition == CreativePhone;
                }, FFortItemEntry::Size());
        if (ItemEntry)
            return;

        int32 LoadedAmmo = 0;
        if (auto Stats = AFortInventory::GetStats(CreativePhone))
            LoadedAmmo = Stats->ClipSize;

        PlayerController->WorldInventory->GiveItem(CreativePhone, 1, LoadedAmmo);
        if (auto PhoneCreated = PlayerController->GetFunction("ClientCreativePhoneCreated"))
        {
            PlayerController->Call<void>(PhoneCreated);
        }
    }
}

void AFortMinigameSettingsBuilding::BeginPlay(AFortMinigameSettingsBuilding* Settings)
{
    return BeginPlayOG(Settings);
}

bool AFortAthenaCreativePortal::PrepareLinkedVolumeForEditing(
    AFortPlayerControllerAthena* PlayerController)
{
    if (!IsLiveObject(PlayerController) || !IsLiveObject(PlayerController->PlayerState))
    {
        return false;
    }

    auto PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;
    AFortAthenaCreativePortal* Portal = nullptr;
    if (PlayerController->HasOwnedPortal() && IsLiveObject(PlayerController->OwnedPortal) &&
        PlayerController->OwnedPortal->IsA(StaticClass()))
    {
        Portal = (AFortAthenaCreativePortal*)
            PlayerController->OwnedPortal;
    }

    AFortVolume* Volume = nullptr;
    if (Portal && Portal->HasLinkedVolume() && IsLiveObject(Portal->LinkedVolume))
    {
        Volume = Portal->LinkedVolume;
        LinkPortalAndVolume(Portal, PlayerController, PlayerState);
    }
    else if (PlayerController->HasCreativePlotLinkedVolume() &&
        IsLiveObject(PlayerController->CreativePlotLinkedVolume))
    {
        Volume = PlayerController->CreativePlotLinkedVolume;
    }

    if (!Volume)
        return false;

    const bool bAppliedPermission = ApplyCreativeVolumePermission(
        Volume, PlayerState, PlayerController, ResolveCreativePlot());
    EnablePlayerCreativeEditing(PlayerState);
    return bAppliedPermission;
}

AFortAthenaCreativePortal* AFortAthenaCreativePortal::Create(
    AFortPlayerControllerAthena* PlayerController)
{
    auto World = UWorld::GetWorld();
    if (!World || !IsLiveObject(PlayerController) || !IsLiveObject(PlayerController->PlayerState) ||
        !IsLiveObject(World->GameState))
    {
        return nullptr;
    }

    auto GameState = (AFortGameStateAthena*)World->GameState;
    auto PlayerState = (AFortPlayerStateAthena*)PlayerController->PlayerState;
    if (!GameState->HasCreativePortalManager() || !IsLiveObject(GameState->CreativePortalManager))
    {
        return nullptr;
    }

    auto Portal = FindPortalForPlayer(GameState->CreativePortalManager, PlayerController,
        PlayerState);
    if (!Portal || !Portal->HasLinkedVolume() || !IsLiveObject(Portal->LinkedVolume))
    {
        return nullptr;
    }

    printf("[Creative] Assigned portal %s to %s\n", Portal->Name.ToString().c_str(),
        PlayerController->Name.ToString().c_str());

    LinkPortalAndVolume(Portal, PlayerController, PlayerState);
    auto Plot = ResolveCreativePlot();
    ApplyCreativeVolumePermission(Portal->LinkedVolume, PlayerState, PlayerController, Plot);
    EnablePlayerCreativeEditing(PlayerState);
    ConfigurePlayset(Portal, Plot);
    EnsureSettingsMachine(Portal);

    printf("[Creative] Setup portal and edit permission\n");
    return Portal;
}

void AFortAthenaCreativePortal::TeleportPlayerToLinkedVolume(UObject* Context, FFrame& Stack)
{
    AFortPlayerPawnAthena* PlayerPawn = nullptr;
    bool bUseSpawnTags = false;
    Stack.StepCompiledIn(&PlayerPawn);
    Stack.StepCompiledIn(&bUseSpawnTags);
    Stack.IncrementCode();

    auto Portal = (AFortAthenaCreativePortal*)Context;
    if (!IsLiveObject(Portal) || !IsLiveObject(PlayerPawn) || !Portal->HasLinkedVolume() ||
        !IsLiveObject(Portal->LinkedVolume) || !IsLiveObject(PlayerPawn->Controller))
    {
        return;
    }

    auto PlayerController = (AFortPlayerControllerAthena*)
        PlayerPawn->Controller;
    LinkPortalAndVolume(Portal, PlayerController,
        (AFortPlayerStateAthena*)PlayerController->PlayerState);
    PrepareLinkedVolumeForEditing(PlayerController);
    GiveCreativePhone(PlayerController);
    EnableCreativeControllerState(PlayerController);

    const FVector BeforeLocation = PlayerPawn->K2_GetActorLocation();
    bool bNativeMovedPawn = false;
    auto NativeFunction = Stack.GetCurrentNativeFunction();
    if (NativeFunction && TeleportPlayerToLinkedVolumeOG && TeleportPlayerToLinkedVolumeOG !=
            TeleportPlayerToLinkedVolume)
    {
        NativeFunction->ExecFunction = (void*)TeleportPlayerToLinkedVolumeOG;
        Portal->Call<void>(NativeFunction, PlayerPawn, bUseSpawnTags);
        NativeFunction->ExecFunction = (void*)TeleportPlayerToLinkedVolume;

        if (IsLiveObject(PlayerPawn))
        {
            const FVector AfterLocation = PlayerPawn->K2_GetActorLocation();
            bNativeMovedPawn = (AfterLocation - BeforeLocation).SizeSquared() > 10000.0;
        }
    }

    if (!bNativeMovedPawn && IsLiveObject(PlayerPawn))
    {
        auto Location = Portal->LinkedVolume->K2_GetActorLocation();
        Location.Z = 10000;
        PlayerPawn->K2_TeleportTo(Location, FRotator());
        PlayerPawn->BeginSkydiving(false);
    }

    PrepareLinkedVolumeForEditing(PlayerController);
}

void AFortAthenaCreativePortal::Hook()
{
    auto PortalClass = StaticClass();
    auto DefaultPortal = PortalClass ? (AFortAthenaCreativePortal*)PortalClass->GetDefaultObj()
        : nullptr;
    if (!DefaultPortal)
        return;

    auto TeleportFunction = DefaultPortal->GetFunction("TeleportPlayerToLinkedVolume");
    if (TeleportFunction)
    {
        Utils::ExecHook(TeleportFunction, TeleportPlayerToLinkedVolume,
            TeleportPlayerToLinkedVolumeOG);
    }
}

void AFortMinigameSettingsBuilding::Hook()
{
    if (!GetDefaultObj())
        return;
}
