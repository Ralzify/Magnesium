#include "pch.h"
#include "../Public/NetDriver.h"
#include "../../Erbium/Public/AutoHosting.h"
#include "../../Erbium/Public/Calendar.h"
#include "../../Erbium/Public/Configuration.h"
#include "../../Erbium/Public/Finders.h"
#include "../../Erbium/Public/GUI.h"
#include "../../FortniteGame/Public/BattleRoyaleGamePhaseLogic.h"
#include "../../FortniteGame/Public/CustomSafeZoneRuntime.h"
#include "../../FortniteGame/Public/FortGameMode.h"
#include "../../FortniteGame/Public/FortAthenaMutator.h"
#include "../../FortniteGame/Public/FortInventory.h"
#include "../../FortniteGame/Public/FortMinigame.h"
#include "../../FortniteGame/Public/FortPhysicsPawn.h"
#include "../../FortniteGame/Public/FortPlayerPawnAthena.h"
#include "../../FortniteGame/Public/FortVehicleMods.h"
#include "../../FortniteGame/Public/FortWeapon.h"
#include "../Public/AbilitySystemComponent.h"
#include "../../Erbium/Support/Public/VersionFeatureAdapter.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>

uint32_t NetworkObjectListOffset = 0;
uint32_t ReplicationFrameOffset = 0;
uint32_t ClientWorldPackageNameOffset = 0;
uint32_t DestroyedStartupOrDormantActorsOffset = 0;
uint32_t DestroyedStartupOrDormantActorGUIDsOffset = 0;
uint32_t ClientVisibleLevelNamesOffset = 0;

std::unordered_map<UNetConnection*, TArray<FNetViewer*>> ViewerMap;

namespace
{
    struct FReplicationObjectSnapshot
    {
        const UObject* Object = nullptr;
        int32 ObjectIndex = -1;
        int32 ObjectSerialNumber = 0;
    };

    bool CaptureLiveReplicationObject(const UObject* Object,
        FReplicationObjectSnapshot& OutSnapshot)
    {
        OutSnapshot = {};
        if (!Object)
            return false;

        const int32 ObjectIndex = Object->Index;
        if (ObjectIndex < 0 || ObjectIndex >= TUObjectArray::Num())
            return false;

        auto Item = TUObjectArray::GetItemByIndex(ObjectIndex);
        constexpr int32 InvalidObjectFlags = 0x20;
        if (!Item || Item->GetObject() != Object || (Item->GetFlags() & InvalidObjectFlags) ||
            !Object->Class)
        {
            return false;
        }

        OutSnapshot.Object = Object;
        OutSnapshot.ObjectIndex = ObjectIndex;
        OutSnapshot.ObjectSerialNumber = Item->SerialRef();
        return true;
    }

    bool MatchesLiveReplicationObject(const FReplicationObjectSnapshot& Snapshot)
    {
        if (!Snapshot.Object || Snapshot.ObjectIndex < 0 ||
            Snapshot.ObjectIndex >= TUObjectArray::Num())
        {
            return false;
        }

        auto Item = TUObjectArray::GetItemByIndex(Snapshot.ObjectIndex);
        constexpr int32 InvalidObjectFlags = 0x20;
        if (!Item || Item->GetObject() != Snapshot.Object ||
            (Item->GetFlags() & InvalidObjectFlags) || (Snapshot.ObjectSerialNumber != 0 &&
             Item->SerialRef() != Snapshot.ObjectSerialNumber))
        {
            return false;
        }

        return true;
    }

    bool CaptureNetworkActorInWorld(const FNetworkObjectInfo* NetworkActorInfo, const UWorld* World,
        AActor*& OutActor, FReplicationObjectSnapshot& OutSnapshot)
    {
        OutActor = nullptr;
        OutSnapshot = {};
        if (!NetworkActorInfo || !World)
        {
            return false;
        }

        const int32 ObjectIndex = NetworkActorInfo->WeakActor.ObjectIndex;
        const int32 ObjectSerialNumber = NetworkActorInfo->WeakActor.ObjectSerialNumber;
        if (ObjectIndex < 0 || ObjectIndex >= TUObjectArray::Num())
            return false;

        auto Item = TUObjectArray::GetItemByIndex(ObjectIndex);
        constexpr int32 InvalidObjectFlags = 0x20;
        if (!Item || (ObjectSerialNumber != 0 && Item->SerialRef() != ObjectSerialNumber) ||
            (Item->GetFlags() & InvalidObjectFlags) || Item->GetObject() != NetworkActorInfo->Actor)
        {
            return false;
        }

        auto Actor = const_cast<AActor*>(static_cast<const AActor*>(Item->GetObject()));
        if (!Actor || !Actor->Class)
            return false;

        OutSnapshot.Object = Actor;
        OutSnapshot.ObjectIndex = ObjectIndex;
        OutSnapshot.ObjectSerialNumber = Item->SerialRef();
        OutActor = Actor;

        const UObject* Outer = Actor->Outer;
        if (Outer == World)
            return true;
        FReplicationObjectSnapshot OuterSnapshot;
        if (!CaptureLiveReplicationObject(Outer, OuterSnapshot) ||
            Outer->Class != ULevel::StaticClass())
        {
            return false;
        }

        auto Level = static_cast<const ULevel*>(Outer);
        return Level->HasOwningWorld() && Level->OwningWorld == World;
    }
}
const ULevel* GetLevel(const AActor* Actor)
{
    auto Outer = Actor->Outer;

    while (Outer)
    {
        if (Outer->Class == ULevel::StaticClass())
            return (ULevel*)Outer;
        else
            Outer = Outer->Outer;
    }

    return nullptr;
}

void BuildViewerMap(UNetDriver* Driver)
{
    for (auto& Conn : Driver->ClientConnections)
    {
        auto Owner = Conn->OwningActor;
        if (Owner)
        {
            auto OutViewTarget = Owner;
            if (auto Controller = Conn->PlayerController)
                if (auto ViewTarget = Controller->GetViewTarget())
                        OutViewTarget = ViewTarget;

            Conn->ViewTarget = OutViewTarget;

            TArray<FNetViewer*> Viewers;
            Viewers.Reserve(1 + Conn->Children.Num());
            Viewers.Add(FNetViewer::Create(Conn));

            for (auto& Child : Conn->Children)
            {
                if (auto Controller = Child->PlayerController)
                {
                    Child->ViewTarget = Controller->GetViewTarget();
                    Viewers.Add(FNetViewer::Create(Child));
                }
                else
                    Child->ViewTarget = nullptr;
            }
            ViewerMap[Conn] = Viewers;
        }
        else
        {
            Conn->ViewTarget = nullptr;
            for (auto& Child : Conn->Children)
                Child->ViewTarget = nullptr;
        }
    }
}

static FNetworkObjectList& GetNetworkObjectList(UNetDriver* Driver)
{
    return *(*(class TSharedPtr<FNetworkObjectList>*)(__int64(Driver) + NetworkObjectListOffset));
}

UNetConnection* IsActorOwnedByAndRelevantToConnection(const AActor* Actor,
    TArray<FNetViewer*>& ConnViewers, bool& bOutHasNullViewTarget)
{
    auto IsNetRelevantForIdx = FindIsNetRelevantForVft();
    if (IsNetRelevantForIdx == 0)
        return ConnViewers[0]->Connection;

    bool (*&IsRelevancyOwnerFor)(const AActor*, const AActor*, const AActor*,
        const AActor*) = decltype(IsRelevancyOwnerFor)(Actor->Vft[IsNetRelevantForIdx + 2]);
    AActor* (*&GetNetOwner)(const AActor*) = decltype(GetNetOwner)(Actor->Vft[IsNetRelevantForIdx + (VersionInfo.EngineVersion >= 4.19 ? 6 : 5)]);

    const AActor* ActorOwner = GetNetOwner(Actor);

    bOutHasNullViewTarget = false;

    for (auto& Viewer : ConnViewers)
    {
        auto Conn = Viewer->Connection;

        if (Conn->ViewTarget == nullptr)
        {
            bOutHasNullViewTarget = true;
        }

        if (ActorOwner == Conn->PlayerController ||
            (Conn->PlayerController && ActorOwner == Conn->PlayerController->Pawn) ||
            (Conn->ViewTarget && IsRelevancyOwnerFor(Conn->ViewTarget, Actor, ActorOwner, Conn->OwningActor)))
        {
            return Conn;
        }
    }

    return nullptr;
}

bool IsActorRelevantToConnection(const AActor* Actor, const TArray<FNetViewer*>& ConnectionViewers)
{
    auto IsNetRelevantForIdx = FindIsNetRelevantForVft();
    if (IsNetRelevantForIdx == 0)
        return true;

    bool (*&IsNetRelevantFor)(const AActor*, const AActor*, const AActor*,
        const FVector&) = decltype(IsNetRelevantFor)(Actor->Vft[IsNetRelevantForIdx]);

    for (auto& Viewer : ConnectionViewers)
    {
        if (!Viewer)
            continue;

        if (IsNetRelevantFor(Actor, Viewer->InViewer, Viewer->ViewTarget, Viewer->ViewLocation))
        {
            return true;
        }
    }

    return false;
}

bool IsLevelInitializedForActor(const UNetDriver* NetDriver, const AActor* InActor,
    UNetConnection* InConnection)
{
    static bool (*ClientHasInitializedLevelFor)(const UNetConnection*, const UObject*) = decltype(ClientHasInitializedLevelFor)(FindClientHasInitializedLevelFor());

    const bool bCorrectWorld = NetDriver->WorldPackage != nullptr &&
        (!ClientWorldPackageNameOffset || *(FName*)(__int64(InConnection) + ClientWorldPackageNameOffset) == NetDriver->WorldPackage->Name) &&
        (!ClientHasInitializedLevelFor || ClientHasInitializedLevelFor(InConnection, InActor));
    const bool bIsConnectionPC = (InActor == InConnection->PlayerController);
    return bCorrectWorld || bIsConnectionPC;
}

struct FPrioActor
{
    AActor* Actor;
    FReplicationObjectSnapshot ActorSnapshot;
    UActorChannel* Channel;
    FReplicationObjectSnapshot ChannelSnapshot;
    float Priority;
    bool bIsRelevant;
    bool bLevelInitializedForActor;

    bool operator<(FPrioActor& _Rhs)
    {
        return Priority < _Rhs.Priority;
    }
};

struct FValidatedChannelSnapshot
{
    UActorChannel* Channel = nullptr;
    FReplicationObjectSnapshot ObjectSnapshot;
};

std::unordered_map<UNetConnection*, UEAllocatedVector<FPrioActor>> PriorityLists;

void (*GetActorLocation)(AActor*, FFrame&, FVector*);
void ServerReplicateActors(UNetDriver* Driver, float DeltaSeconds)
{
    if (!ReplicationFrameOffset)
        return;

    FReplicationObjectSnapshot DriverSnapshot;
    FReplicationObjectSnapshot WorldSnapshot;
    if (!CaptureLiveReplicationObject(Driver, DriverSnapshot) || !Driver->HasWorld() ||
        !CaptureLiveReplicationObject(Driver->World, WorldSnapshot))
    {
        return;
    }
    UWorld* const DriverWorld = Driver->World;

    (*(int*)(__int64(Driver) + ReplicationFrameOffset))++;

    BuildViewerMap(Driver);
    if (ViewerMap.size() == 0)
        return;

    static FName ActorName = FName(L"Actor");

    auto& NetworkObjectList = GetNetworkObjectList(Driver);
    auto& ActiveNetworkObjects = NetworkObjectList.ActiveNetworkObjects;
    auto IsNetReady = (int32(*)(UNetConnection*, bool))FindIsNetReady();
    static auto CloseActorChannel = (void (*)(UActorChannel*, uint8_t))FindCloseActorChannel();
    std::unordered_map<UNetConnection*, std::unordered_map<AActor*, FValidatedChannelSnapshot>>
        OpenActorChannels;

    for (auto& ViewerPair : ViewerMap)
    {
        auto& Conn = ViewerPair.first;
        auto& Viewers = ViewerPair.second;

        auto& List = PriorityLists[Conn];
        List.clear();
        List.reserve(ActiveNetworkObjects.Num());
        auto& ChannelMap = OpenActorChannels[Conn];
        if (Conn->OpenChannels.Num() > 0)
            ChannelMap.reserve(Conn->OpenChannels.Num());
        for (auto Chan : Conn->OpenChannels)
        {
            FReplicationObjectSnapshot ChannelSnapshot;
            if (!CaptureLiveReplicationObject(Chan, ChannelSnapshot) ||
                Chan->Class != UActorChannel::StaticClass() || !Chan->Actor)
            {
                continue;
            }
            ChannelMap.emplace(Chan->Actor, FValidatedChannelSnapshot{
                    Chan, ChannelSnapshot });
        }
    }

    auto TimeSeconds = UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());

    auto Scale = Driver->NetServerMaxTickRate / FConfiguration::MaxTickRate;
    FFrame FakeStack;
    for (auto& ActorInfo : ActiveNetworkObjects)
    {
        auto NetworkActorInfo = ActorInfo.Get();
        if (!NetworkActorInfo)
        {
            continue;
        }

        AActor* Actor = nullptr;
        FReplicationObjectSnapshot ActorSnapshot;

        if (!CaptureNetworkActorInWorld(NetworkActorInfo, DriverWorld, Actor, ActorSnapshot) ||
 Actor->NetDriverName != Driver->NetDriverName)
        {
            continue;
        }

        if (Actor->bActorIsBeingDestroyed || Actor->RemoteRole == 0
            || ((Actor->HasbNetStartup() ? Actor->bNetStartup : false) && Actor->NetDormancy == 4))
        {
            NetworkActorInfo->NextUpdateTime = 43857458734643857485478534.f; // never gonna update lol
            continue;
        }

        if (!Actor->bReplicates)
            continue;

        bool bAnyRelevant = false;
        for (auto& ViewerPair : ViewerMap)
        {
            auto Conn = ViewerPair.first;
            auto& Viewers = ViewerPair.second;
            UActorChannel* Channel = nullptr;
            FReplicationObjectSnapshot ChannelSnapshot;
            auto ConnectionChannels = OpenActorChannels.find(Conn);
            if (ConnectionChannels != OpenActorChannels.end())
            {
                auto ExistingChannel = ConnectionChannels->second.find(Actor);
                if (ExistingChannel != ConnectionChannels->second.end())
                {
                    Channel = ExistingChannel->second.Channel;
                    ChannelSnapshot = ExistingChannel->second.ObjectSnapshot;
                }
            }

            bool bLevelInitializedForActor = IsLevelInitializedForActor(Driver, Actor, Conn);
            if (!Channel && (!bLevelInitializedForActor || !IsActorRelevantToConnection(Actor, Viewers)))
            {
                continue;
            }

            auto PriorityConn = Conn;
            bool bDoCullCheck = true;

            if (Actor->bAlwaysRelevant || Actor->bTearOff)
                bDoCullCheck = false;

            if (Actor->bOnlyRelevantToOwner)
            {
                bDoCullCheck = false;
                bool bHasNullViewTarget = false;

                if (!(PriorityConn = IsActorOwnedByAndRelevantToConnection(Actor, Viewers,
                    bHasNullViewTarget)))
                {
                    if (!bHasNullViewTarget && Channel != NULL &&
                        Driver->GetTime() - Channel->GetRelevantTime() >= Driver->RelevantTimeout)
                        CloseActorChannel(Channel, 3);

                    continue;
                }
            }
            else if (VersionInfo.FortniteVersion >= 20) // its broken on legacy builds. idk why
            {
                if (VersionInfo.FortniteVersion == 1.72 || VersionInfo.FortniteVersion == 0.00)
                {
                    auto& DormantConnections = *(TSet<TWeakObjectPtr<UNetConnection>>*)(__int64(NetworkActorInfo) + 0x28);

                    if (DormantConnections.Contains(Conn))
                        continue;
                }
                else if (NetworkActorInfo->DormantConnections.Contains(Conn))
                    continue;

                static auto FlushDormancy = FindFlushDormancy();
                if (VersionInfo.FortniteVersion != 1.72 && VersionInfo.FortniteVersion != 0.00 &&
                    (VersionInfo.FortniteVersion >= 20 || FlushDormancy))
                    if (Actor->GetNetDormancy() > 1 && Channel && !Channel->IsPendingDormancy() &&
                        !Channel->IsDormant())
                        ((int32(*)(UActorChannel*))FindStartBecomingDormant())(Channel);
            }

            bool bIsRelevant = bLevelInitializedForActor && !Actor->bTearOff &&
                (!Channel || Driver->GetTime() - Channel->GetRelevantTime() > 1.0)
                && IsActorRelevantToConnection(Actor, Viewers);
            bool bIsRecentlyRelevant = bIsRelevant || (Channel && Driver->GetTime() - Channel->GetRelevantTime() < Driver->RelevantTimeout);

            if (bIsRecentlyRelevant && (!Channel || Channel->Actor))
            {
                bAnyRelevant = true;
                auto Priority = 0.f;

                bool bIsAController = false;
                for (auto& Viewer : Viewers)
                {
                    if (Actor == Viewer->InViewer)
                        bIsAController = true;
                }
                if (Actor->RootComponent && bIsAController)
                {
                    FVector Loc;

                    GetActorLocation(Actor, FakeStack, &Loc);

                    float SmallestDisSquared = (std::numeric_limits<float>::max)();
                    int32 ViewersThatSkipActor = 0;

                    for (auto& Viewer : Viewers)
                    {
                        auto DistanceSquared = (Loc - Viewer->ViewLocation).SizeSquared();
                        SmallestDisSquared = (float)min(SmallestDisSquared, DistanceSquared);

                        if (bDoCullCheck && DistanceSquared > Actor->NetCullDistanceSquared)
                            ViewersThatSkipActor++;
                    }

                    if (bDoCullCheck && ViewersThatSkipActor == Viewers.Num())
                        continue;

                    const float MaxDistanceScaling = 60000.f * 60000.f;

                    const float DistanceFactor = std::clamp(
                        (SmallestDisSquared) / MaxDistanceScaling, 0.f, 1.f);

                    Priority += DistanceFactor;
                }

                if (Actor->NetDormancy > 1)
                    Priority -= 1.5f;

                for (auto& Viewer : Viewers)
                    if (Actor == Viewer->ViewTarget || Actor == Viewer->InViewer)
                        Priority = -(std::numeric_limits<float>::max)();

                auto& PriorityList = PriorityLists[Conn];
                PriorityList.push_back({
                    Actor, ActorSnapshot, Channel, ChannelSnapshot, Priority, bIsRelevant,
                    bLevelInitializedForActor });
            }

            if (Channel && !bIsRecentlyRelevant && (Actor->bTearOff || !bLevelInitializedForActor ||
                    !(Actor->HasbNetStartup() ? Actor->bNetStartup : false)))
            {
                CloseActorChannel(Channel, Actor->bTearOff ? 4 : 3);
                continue;
            }
        }

        if (bAnyRelevant)
            ((void (*)(AActor*, UNetDriver*))FindCallPreReplication())(Actor, Driver);
    }

    for (auto& PriorityListPair : PriorityLists)
    {
        auto Conn = PriorityListPair.first;
        auto& Viewers = ViewerMap[Conn];
        auto& PriorityActors = PriorityListPair.second;
        int i = 0;
        if (!Conn->ViewTarget)
            goto _out;

        std::sort(PriorityActors.begin(), PriorityActors.end());

        if (IsNetReady && VersionInfo.FortniteVersion < 22 && !IsNetReady(Conn, false))
            goto _out;

        if (DestroyedStartupOrDormantActorGUIDsOffset)
        {
            auto& DestroyedStartupOrDormantActors = *(TMap<uint32, FActorDestructionInfo*>*)(__int64(Driver) + DestroyedStartupOrDormantActorsOffset);
            auto& DestroyedStartupOrDormantActors_UE53 = *(TMap<uint64, FActorDestructionInfo*>*)(__int64(Driver) + DestroyedStartupOrDormantActorsOffset);
            auto& DestroyedStartupOrDormantActorGUIDs = *(TSet<uint32>*)(__int64(Conn) + DestroyedStartupOrDormantActorGUIDsOffset);
            auto& DestroyedStartupOrDormantActorGUIDs_UE53 = *(TSet<uint64>*)(__int64(Conn) + DestroyedStartupOrDormantActorGUIDsOffset);
            auto& ClientVisibleLevelNames = *(TSet<int32>*)(__int64(Conn) + ClientVisibleLevelNamesOffset);
            static auto SetChannelActorForDestroy = (void (*)(UActorChannel*, FActorDestructionInfo*))FindSetChannelActorForDestroy();
            static auto SendDestructionInfo = (void (*)(UNetDriver*, UNetConnection*,
                FActorDestructionInfo*))FindSendDestructionInfo();

            if (VersionInfo.EngineVersion >= 5.3)
            {
                for (auto& NetGUID : DestroyedStartupOrDormantActorGUIDs_UE53)
                {
                    auto DestructionInfoPtr = DestroyedStartupOrDormantActors_UE53.Search(
                        [&](uint64& GUID, FActorDestructionInfo*& InfoUPtr)
                        {
                            return GUID == NetGUID ;
                        });

                    if (DestructionInfoPtr)
                    {
                        auto DestructionInfo = *DestructionInfoPtr;

                        if (SetChannelActorForDestroy)
                        {
                            auto Channel = ((UActorChannel * (*)(UNetConnection*, FName*, uint8_t, int)) FindCreateChannel())(
                                Conn, &ActorName, 2, -1);

                            if (Channel)
                                SetChannelActorForDestroy(Channel, DestructionInfo);
                        }
                        else if (SendDestructionInfo)
                            SendDestructionInfo(Driver, Conn, DestructionInfo);
                    }
                }
                DestroyedStartupOrDormantActorGUIDs_UE53.Reset();
            }
            else
            {
                for (auto& NetGUID : DestroyedStartupOrDormantActorGUIDs)
                {
                    auto DestructionInfoPtr = DestroyedStartupOrDormantActors.Search(
                        [&](uint32& GUID, FActorDestructionInfo*& InfoUPtr)
                        {
                            return GUID == NetGUID ;
                        });

                    if (DestructionInfoPtr)
                    {
                        auto DestructionInfo = *DestructionInfoPtr;

                        if (SetChannelActorForDestroy)
                        {
                            auto Channel = ((UActorChannel * (*)(UNetConnection*, FName*, uint8_t, int)) FindCreateChannel())(
                                Conn, &ActorName, 2, -1);

                            if (Channel)
                                SetChannelActorForDestroy(Channel, DestructionInfo);
                        }
                        else if (SendDestructionInfo)
                            SendDestructionInfo(Driver, Conn, DestructionInfo);
                    }
                }
                DestroyedStartupOrDormantActorGUIDs.Reset();
            }
        }

        static auto SendClientAdjustment = FindSendClientAdjustment();
        if (SendClientAdjustment)
            for (auto& Viewer : Viewers)
            {
                if (Viewer->Connection->PlayerController)
                    ((void (*)(AFortPlayerControllerAthena*))SendClientAdjustment)(Viewer->Connection->PlayerController);
            }

        for (auto& PriorityActor : PriorityActors)
        {
            auto Actor = PriorityActor.Actor;
            if (!MatchesLiveReplicationObject(PriorityActor.ActorSnapshot))
                continue;

            UActorChannel* Channel = PriorityActor.Channel;
            FReplicationObjectSnapshot ChannelSnapshot = PriorityActor.ChannelSnapshot;
            bool bChannelValidated = false;
            if (Channel && (!MatchesLiveReplicationObject(ChannelSnapshot) ||
                    Channel->Class != UActorChannel::StaticClass() || Channel->Actor != Actor))
            {
                continue;
            }
            bChannelValidated = Channel != nullptr;

            if (!Channel || Channel->Actor)
            {
                if (!Channel)
                {
                    if (VersionInfo.FortniteVersion >= 20)
                        Channel = ((UActorChannel * (*)(UNetConnection*, FName*, uint8_t, int)) FindCreateChannel())(
                            Conn, &ActorName, 2, -1);
                    else
                        Channel = ((UActorChannel * (*)(UNetConnection*, int, bool, int32_t)) FindCreateChannel())(
                            Conn, 2, true, -1);

                    bChannelValidated = CaptureLiveReplicationObject(Channel, ChannelSnapshot) &&
                        Channel->Class == UActorChannel::StaticClass();
                    if (bChannelValidated)
                        ((void (*)(UActorChannel*, AActor*, uint8_t))FindSetChannelActor())(Channel,
                            Actor, 0);

                    bChannelValidated = bChannelValidated && MatchesLiveReplicationObject(
                            ChannelSnapshot) && MatchesLiveReplicationObject(
                            PriorityActor.ActorSnapshot);
                }

                if (Channel && bChannelValidated && Channel->Actor == Actor)
                {
                    if (PriorityActor.bIsRelevant)
                        Channel->GetRelevantTime() = Driver->GetTime() + 0.5 * ((float)rand() / 32767.f);

                    if (VersionInfo.FortniteVersion >= 22 || (IsNetReady && IsNetReady(Conn, false))) // actually uchannel::isnetready
                        ((int32(*)(UActorChannel*))FindReplicateActor())(Channel);
                    else
                        Actor->ForceNetUpdate();

                    if (IsNetReady && VersionInfo.FortniteVersion < 22 && !IsNetReady(Conn, false))
                    {
                        break;
                    }
                }

                if (Channel && Actor->bTearOff &&
                    (!PriorityActor.bLevelInitializedForActor || !(Actor->HasbNetStartup() ? Actor->bNetStartup : false)))
                    CloseActorChannel(Channel, 4);
            }
            i++;
        }

    _out: PriorityActors.clear();
    }
    PriorityLists.clear();

    for (auto& ViewerPair : ViewerMap)
    {
        for (auto& Viewer : ViewerPair.second)
            free(Viewer);

        ViewerPair.second.Free();
    }

    ViewerMap.clear();
}
static UClass* GetLowerSeasonStormEffectClass()
{
    static UClass* StormEffectClass = nullptr;
    if (!StormEffectClass)
        StormEffectClass = const_cast<UClass*>(FindObject<UClass>(L"/Game/Athena/SafeZone/GE_OutsideSafeZoneDamage.GE_OutsideSafeZoneDamage_C"));
    return StormEffectClass;
}

static std::unordered_set<AFortPlayerControllerAthena*> GRespawnManagedStormPlayers;
static std::unordered_set<AFortPlayerControllerAthena*> GRespawnStormStateLoggedPlayers;

static std::vector<FActiveGameplayEffectHandle> FindLowerSeasonStormEffectHandles(
    UAbilitySystemComponent* AbilitySystemComponent, UClass* StormEffectClass)
{
    std::vector<FActiveGameplayEffectHandle> Handles;
    if (!AbilitySystemComponent || !StormEffectClass ||
        !SDK::MemReadable(AbilitySystemComponent, sizeof(UObject)) ||
        !AbilitySystemComponent->HasActiveGameplayEffects() ||
        !FActiveGameplayEffectsContainer::StaticStruct() ||
        !FActiveGameplayEffect::StaticStruct() || !FGameplayEffectSpec::StaticStruct() ||
        !FActiveGameplayEffectsContainer::HasGameplayEffects_Internal() ||
        !FActiveGameplayEffect::HasSpec() || !FGameplayEffectSpec::HasDef())
        return Handles;

    const int32 ActiveGameplayEffectsSize = FActiveGameplayEffectsContainer::Size();
    const int32 EffectsOffset = FActiveGameplayEffectsContainer::GameplayEffects_Internal__Offset;
    if (ActiveGameplayEffectsSize < static_cast<int32>(sizeof(TArray<FActiveGameplayEffect>)) ||
        EffectsOffset < 0 || EffectsOffset > ActiveGameplayEffectsSize -
            static_cast<int32>(sizeof(TArray<FActiveGameplayEffect>)))
    {
        return Handles;
    }

    auto& Effects = AbilitySystemComponent->ActiveGameplayEffects.GameplayEffects_Internal;
    if (!SDK::MemReadable(&Effects, sizeof(Effects)))
        return Handles;

    const int EffectCount = Effects.Num();
    const int EffectCapacity = Effects.Max();
    const int32 ActiveEffectSize = FActiveGameplayEffect::Size();
    const int32 GameplayEffectSpecSize = FGameplayEffectSpec::Size();
    const int32 GameplayEffectSpecOffset = FActiveGameplayEffect::Spec__Offset;
    const int32 GameplayEffectDefinitionOffset = FGameplayEffectSpec::Def__Offset;
    constexpr int MaxSafeStormEffectCount = 4096;
    if (EffectCount < 0 || EffectCount > MaxSafeStormEffectCount || EffectCapacity < EffectCount ||
        EffectCapacity > 100000 || ActiveEffectSize < 0x14 || ActiveEffectSize > 0x1000 ||
        GameplayEffectSpecSize < static_cast<int32>(sizeof(void*)) ||
        GameplayEffectSpecSize > ActiveEffectSize || GameplayEffectSpecOffset < 0 ||
        GameplayEffectSpecOffset > ActiveEffectSize - GameplayEffectSpecSize ||
        GameplayEffectDefinitionOffset < 0 || GameplayEffectDefinitionOffset >
            GameplayEffectSpecSize - static_cast<int32>(sizeof(void*)))
    {
        return Handles;
    }

    if (EffectCount == 0)
        return Handles;

    const size_t EffectsByteCount = static_cast<size_t>(EffectCount) *
        static_cast<size_t>(ActiveEffectSize);
    if (!Effects.GetData() || !SDK::MemReadable(Effects.GetData(), EffectsByteCount))
    {
        return Handles;
    }

    Handles.reserve(EffectCount);
    for (int Index = 0; Index < EffectCount; Index++)
    {
        auto& Effect = Effects.Get(Index, ActiveEffectSize);
        auto EffectDefinition = Effect.Spec.Def;
        if (EffectDefinition && SDK::MemReadable(EffectDefinition, sizeof(UObject)) &&
            EffectDefinition->IsA(StormEffectClass))
        {
            Handles.push_back(*(FActiveGameplayEffectHandle*)(__int64(&Effect) + 0xc));
        }
    }
    return Handles;
}

static int NormalizeLowerSeasonStormEffectLevels(AFortGameMode* GameMode,
    UClass* StormEffectClass, int DesiredLevel, int& UpdatedCount)
{
    UpdatedCount = 0;
    if (!GameMode || !StormEffectClass || DesiredLevel < 1)
        return 0;

    const bool bCanReadLevel = FGameplayEffectSpec::HasLevel();
    int FoundCount = 0;
    for (auto& UncastedPlayer : GameMode->AlivePlayers)
    {
        auto Player = (AFortPlayerControllerAthena*)UncastedPlayer;
        if (!Player || !Player->PlayerState || !Player->PlayerState->AbilitySystemComponent)
            continue;
        if (AFortPlayerControllerAthena::IsCheatSpawnedBotController(Player))
        {
            continue;
        }

        auto AbilitySystemComponent = Player->PlayerState->AbilitySystemComponent;
        auto& Effects = AbilitySystemComponent->ActiveGameplayEffects.GameplayEffects_Internal;
        for (int Index = 0; Index < Effects.Num(); Index++)
        {
            auto& Effect = Effects.Get(Index, FActiveGameplayEffect::Size());
            if (!Effect.Spec.Def || !Effect.Spec.Def->IsA(StormEffectClass))
                continue;

            FoundCount++;
            const float PreviousLevel = bCanReadLevel ? Effect.Spec.Level : -1.f;
            if (bCanReadLevel && std::isfinite((double)PreviousLevel) &&
                std::abs(PreviousLevel - (float)DesiredLevel) < 0.01f)
            {
                continue;
            }

            auto Handle = *(FActiveGameplayEffectHandle*)(__int64(&Effect) + 0xc);
            if (Handle.Handle <= 0)
                continue;

            AbilitySystemComponent->SetActiveGameplayEffectLevel(Handle, DesiredLevel);
            UpdatedCount++;
            SDK::DbgLog("[SafeZone] normalized native storm effect controller=%p handle=%d level=%.2f->%d\n",
                (void*)Player, Handle.Handle, PreviousLevel, DesiredLevel);
        }
    }

    return FoundCount;
}

static bool RemoveLowerSeasonStormEffectHandle(UAbilitySystemComponent* AbilitySystemComponent,
    const FActiveGameplayEffectHandle& Handle)
{
    if (!AbilitySystemComponent || Handle.Handle <= 0)
        return false;

    auto RemoveActiveEffectFn = AbilitySystemComponent->GetFunction("RemoveActiveGameplayEffect");
    return RemoveActiveEffectFn && AbilitySystemComponent->Call<bool>(RemoveActiveEffectFn, Handle, -1);
}

static void RemoveLowerSeasonStormEffects(UAbilitySystemComponent* AbilitySystemComponent,
    UClass* StormEffectClass)
{
    if (!AbilitySystemComponent || !StormEffectClass)
        return;

    auto Handles = FindLowerSeasonStormEffectHandles(AbilitySystemComponent, StormEffectClass);
    bool bRemovedEveryHandle = !Handles.empty();
    for (auto& Handle : Handles)
        if (!RemoveLowerSeasonStormEffectHandle(AbilitySystemComponent, Handle))
            bRemovedEveryHandle = false;

    if (bRemovedEveryHandle || Handles.empty())
        return;

    static UFunction* RemoveBySourceFn = nullptr;
    if (!RemoveBySourceFn)
        RemoveBySourceFn = const_cast<UFunction*>(FindObject<UFunction>(L"/Script/GameplayAbilities.AbilitySystemComponent.RemoveActiveGameplayEffectBySourceEffect"));
    if (!RemoveBySourceFn)
        return;

    struct
    {
        UClass* GameplayEffect;
        UObject* InstigatorAbilitySystemComponent;
        int StacksToRemove;
    } Params{ StormEffectClass, AbilitySystemComponent, -1 };

    AbilitySystemComponent->ProcessEvent(RemoveBySourceFn, &Params);
}

static bool SetReflectedSafeZoneBool(UObject* Object, const char* PropertyName, bool Value)
{
    if (!Object)
        return false;

    auto Property = Object->GetProperty(PropertyName, 0x20000);
    if (!Property)
        return false;

    auto Offset = GetFromOffset<uint32>(Property, Offsets::Offset_Internal);
    auto Mask = Property->GetFieldMask();
    auto& Byte = GetFromOffset<uint8_t>(Object, Offset);
    const bool Previous = Mask ? (Byte & Mask) != 0 : Byte != 0;
    if (Previous == Value)
        return false;

    if (Mask)
        Value ? Byte |= Mask : Byte &= ~Mask;
    else
        Byte = Value ? 1 : 0;

    return true;
}

static bool TryGetReflectedSafeZoneBool(UObject* Object, const char* PropertyName, bool& OutValue)
{
    OutValue = false;
    if (!Object)
        return false;

    auto Property = Object->GetProperty(PropertyName, 0x20000);
    if (!Property)
        return false;

    auto Offset = GetFromOffset<uint32>(Property, Offsets::Offset_Internal);
    if (Offset == (uint32)-1)
        return false;

    const auto Mask = Property->GetFieldMask();
    const auto Byte = GetFromOffset<uint8_t>(Object, Offset);
    OutValue = Mask ? (Byte & Mask) != 0 : Byte != 0;
    return true;
}

static bool SetReflectedSafeZoneClass(UObject* Object, const char* PropertyName,
    UClass* NewValue, UClass*& PreviousValue)
{
    PreviousValue = nullptr;
    if (!Object)
        return false;

    auto Property = Object->GetProperty(PropertyName, 0x400);
    if (!Property)
        return false;

    auto Offset = GetFromOffset<uint32>(Property, Offsets::Offset_Internal);
    auto ElementSize = GetFromOffset<uint32>(Property, Offsets::ElementSize);
    if (Offset == (uint32)-1 || ElementSize != sizeof(UClass*))
        return false;

    auto& Value = GetFromOffset<UClass*>(Object, Offset);
    PreviousValue = Value;
    Value = NewValue;
    return true;
}

bool SuppressOutsideSafeZoneEffectForController(
    AFortPlayerControllerAthena* Player, bool bPrimeSafeZoneLatch)
{
    if (!Player || !Player->PlayerState)
        return false;

    auto AbilitySystemComponent = Player->PlayerState->AbilitySystemComponent;
    auto StormEffectClass = GetLowerSeasonStormEffectClass();
    if (!AbilitySystemComponent || !StormEffectClass)
        return false;

    auto Handles = FindLowerSeasonStormEffectHandles(AbilitySystemComponent, StormEffectClass);
    const bool bHadOutsideSafeZoneEffect = !Handles.empty();
    if (bHadOutsideSafeZoneEffect)
    {
        RemoveLowerSeasonStormEffects(AbilitySystemComponent, StormEffectClass);
    }

    auto Pawn = Player->MyFortPawn;
    if (!Pawn && Player->Pawn && Player->Pawn->IsA(AFortPlayerPawnAthena::StaticClass()))
    {
        Pawn = (AFortPlayerPawnAthena*)Player->Pawn;
    }
    if (!Pawn)
        return bHadOutsideSafeZoneEffect;

    bool bOutsideSafeZone = false;
    bool ReflectedValue = false;
    if (TryGetReflectedSafeZoneBool(Pawn, "bIsOutsideSafeZone", ReflectedValue))
    {
        bOutsideSafeZone |= ReflectedValue;
    }
    if (Pawn->HasbIsInAnyStorm())
        bOutsideSafeZone |= Pawn->bIsInAnyStorm;
    if (Pawn->HasbIsInsideSafeZone())
        bOutsideSafeZone |= !Pawn->bIsInsideSafeZone;

    if (bPrimeSafeZoneLatch || bOutsideSafeZone || bHadOutsideSafeZoneEffect)
    {
        UClass* PreviousAppliedEffect = nullptr;
        if (SetReflectedSafeZoneClass(Pawn, "SafeZoneAppliedGE", StormEffectClass,
                PreviousAppliedEffect) && PreviousAppliedEffect != StormEffectClass)
        {
            Pawn->ForceNetUpdate();
        }
    }

    return bHadOutsideSafeZoneEffect;
}

void ResetLowerSeasonStormStateForRespawn(AFortPlayerControllerAthena* Player,
    AFortPlayerPawnAthena* OldPawn, AFortPlayerPawnAthena* NewPawn)
{
    if (VersionInfo.FortniteVersion >= 7.00 || !Player || !Player->PlayerState)
        return;

    auto AbilitySystemComponent = Player->PlayerState->AbilitySystemComponent;
    auto StormEffectClass = GetLowerSeasonStormEffectClass();
    if (VersionInfo.FortniteVersion >= 4.00)
    {
        GRespawnManagedStormPlayers.insert(Player);
        GRespawnStormStateLoggedPlayers.erase(Player);
    }
    const int HandlesBefore = (int)FindLowerSeasonStormEffectHandles(
        AbilitySystemComponent, StormEffectClass).size();
    RemoveLowerSeasonStormEffects(AbilitySystemComponent, StormEffectClass);
    const int HandlesAfter = (int)FindLowerSeasonStormEffectHandles(
        AbilitySystemComponent, StormEffectClass).size();

    bool bOldOutsideReset = false;
    bool bNewOutsideReset = false;
    UClass* OldAppliedEffect = nullptr;
    UClass* NewAppliedEffect = nullptr;

    if (OldPawn)
    {
        bOldOutsideReset = SetReflectedSafeZoneBool(OldPawn, "bIsOutsideSafeZone", false);
        SetReflectedSafeZoneBool(OldPawn, "bIsOutsideSafeZoneCached", false);
        SetReflectedSafeZoneClass(OldPawn, "SafeZoneAppliedGE", nullptr, OldAppliedEffect);
    }

    if (NewPawn && NewPawn != OldPawn)
    {
        bNewOutsideReset = SetReflectedSafeZoneBool(NewPawn, "bIsOutsideSafeZone", false);
        SetReflectedSafeZoneBool(NewPawn, "bIsOutsideSafeZoneCached", false);
        SetReflectedSafeZoneClass(NewPawn, "SafeZoneAppliedGE", nullptr, NewAppliedEffect);
        NewPawn->ForceNetUpdate();
    }

    SDK::DbgLog(
        "[SafeZone] respawn reset controller=%p oldPawn=%p newPawn=%p handles=%d->%d oldOutside=%d newOutside=%d oldGE=%p newGE=%p\n",
        (void*)Player, (void*)OldPawn, (void*)NewPawn, HandlesBefore, HandlesAfter,
        (int)bOldOutsideReset, (int)bNewOutsideReset,
        (void*)OldAppliedEffect, (void*)NewAppliedEffect);
}

static void SynchronizeRespawnManagedStormEffects(UNetDriver* Driver,
    AFortGameMode* GameMode, UClass* StormEffectClass, int DamageEffectLevel)
{
    if (!Driver || !GameMode || !StormEffectClass || VersionInfo.FortniteVersion < 4.00 ||
        GRespawnManagedStormPlayers.empty())
    {
        return;
    }

    auto Indicator = GameMode->HasSafeZoneIndicator() ? GameMode->SafeZoneIndicator : nullptr;
    if (!Indicator)
        return;

    // 4.5-6.21 lack IsInCurrentSafeZone, and GetSafeZoneRadius can return the next circle's preview.
    auto GetSafeZoneCenterFn = Indicator->GetFunction("GetSafeZoneCenter");
    if (!GetSafeZoneCenterFn || !Indicator->HasRadius())
        return;

    const auto SafeZoneCenter = Indicator->Call<FVector>(GetSafeZoneCenterFn);
    const float SafeZoneRadius = Indicator->Radius;
    if (!std::isfinite(SafeZoneCenter.X) || !std::isfinite(SafeZoneCenter.Y) ||
        !std::isfinite(SafeZoneRadius) || SafeZoneRadius <= 0.0f)
    {
        return;
    }

    const double SafeZoneRadiusSquared = (double)SafeZoneRadius * (double)SafeZoneRadius;

    // Below 5.00 the native inside-check is wrong after a respawn, so resync every player.
    const bool bSynchronizeAllPlayers = VersionInfo.FortniteVersion < 5.00;
    std::unordered_set<AFortPlayerControllerAthena*> PlayersToSynchronize;
    for (auto Connection : Driver->ClientConnections)
    {
        if (!Connection)
            continue;

        auto Player = Connection->PlayerController;
        if (!Player && Connection->OwningActor &&
            Connection->OwningActor->IsA(AFortPlayerControllerAthena::StaticClass()))
        {
            Player = (AFortPlayerControllerAthena*)Connection->OwningActor;
        }

        if (Player && (bSynchronizeAllPlayers || GRespawnManagedStormPlayers.contains(Player)))
            PlayersToSynchronize.insert(Player);
    }

    for (auto& UncastedPlayer : GameMode->AlivePlayers)
    {
        auto Player = (AFortPlayerControllerAthena*)UncastedPlayer;
        if (Player && (bSynchronizeAllPlayers || GRespawnManagedStormPlayers.contains(Player)))
            PlayersToSynchronize.insert(Player);
    }

    for (auto Player : PlayersToSynchronize)
    {
        if (AFortPlayerControllerAthena::IsCheatSpawnedBotController(Player))
        {
            continue;
        }

        if (!Player->MyFortPawn || !Player->PlayerState ||
            !Player->PlayerState->AbilitySystemComponent)
        {
            continue;
        }

        auto Pawn = Player->MyFortPawn;
        auto AbilitySystemComponent = Player->PlayerState->AbilitySystemComponent;
        const auto PawnLocation = Pawn->K2_GetActorLocation();
        const double DeltaX = (double)PawnLocation.X - (double)SafeZoneCenter.X;
        const double DeltaY = (double)PawnLocation.Y - (double)SafeZoneCenter.Y;
        const bool bInsideSafeZone = DeltaX * DeltaX + DeltaY * DeltaY <= SafeZoneRadiusSquared;
        auto Handles = FindLowerSeasonStormEffectHandles(AbilitySystemComponent, StormEffectClass);
        if (GRespawnStormStateLoggedPlayers.insert(Player).second)
        {
            SDK::DbgLog(
                "[SafeZone] respawn sync observed controller=%p pawn=(%.1f,%.1f) wall=(%.1f,%.1f) radius=%.1f last=%.1f next=%.1f inside=%d handles=%d\n",
                (void*)Player, PawnLocation.X, PawnLocation.Y,
                SafeZoneCenter.X, SafeZoneCenter.Y, SafeZoneRadius,
                Indicator->HasLastRadius() ? Indicator->LastRadius : -1.f,
                Indicator->HasNextRadius() ? Indicator->NextRadius : -1.f,
                (int)bInsideSafeZone, (int)Handles.size());
        }

        if (bInsideSafeZone)
        {
            if (!Handles.empty())
            {
                RemoveLowerSeasonStormEffects(AbilitySystemComponent, StormEffectClass);
                SDK::DbgLog("[SafeZone] respawn sync removed storm effect controller=%p handles=%d\n",
                    (void*)Player, (int)Handles.size());
            }

            SetReflectedSafeZoneBool(Pawn, "bIsOutsideSafeZone", false);
            SetReflectedSafeZoneBool(Pawn, "bIsOutsideSafeZoneCached", false);
            const bool bInStormChanged = SetReflectedSafeZoneBool(Pawn, "bIsInAnyStorm", false);
            const bool bInsideChanged = SetReflectedSafeZoneBool(Pawn, "bIsInsideSafeZone", true);
            if (bInStormChanged)
                if (auto OnRepFn = Pawn->GetFunction("OnRep_IsInAnyStorm"))
                    Pawn->ProcessEvent(OnRepFn, nullptr);
            if (bInsideChanged)
                if (auto OnRepFn = Pawn->GetFunction("OnRep_IsInsideSafeZone"))
                    Pawn->ProcessEvent(OnRepFn, nullptr);
            UClass* PreviousAppliedEffect = nullptr;
            SetReflectedSafeZoneClass(Pawn, "SafeZoneAppliedGE", nullptr, PreviousAppliedEffect);
            continue;
        }

        if (Handles.empty())
        {
            auto Context = AbilitySystemComponent->MakeEffectContext();
            Context.Instigator = Player;
            Context.Causer = Pawn;
            Context.AddSourceObject(Pawn);
            auto Handle = AbilitySystemComponent->BP_ApplyGameplayEffectToSelf(
                StormEffectClass, (float)DamageEffectLevel, Context);

            if (Handle.Handle > 0)
                SDK::DbgLog("[SafeZone] respawn sync applied storm effect controller=%p handle=%d level=%d\n",
                    (void*)Player, Handle.Handle, DamageEffectLevel);
        }

        SetReflectedSafeZoneBool(Pawn, "bIsOutsideSafeZone", true);
        SetReflectedSafeZoneBool(Pawn, "bIsInAnyStorm", true);
        SetReflectedSafeZoneBool(Pawn, "bIsInsideSafeZone", false);
        UClass* PreviousAppliedEffect = nullptr;
        SetReflectedSafeZoneClass(Pawn, "SafeZoneAppliedGE", StormEffectClass, PreviousAppliedEffect);
        Pawn->ForceNetUpdate();
    }
}

static void SyncErbiumSafeZonePause(UNetDriver* Driver)
{
    auto World = UWorld::GetWorld();
    if (!World || Driver != World->NetDriver)
        return;

    UFortGameStateComponent_BattleRoyaleGamePhaseLogic::TickSafeZonePause();
}

static void TickModernGamePhaseLogicOnce(UNetDriver* Driver)
{
    if (VersionInfo.FortniteVersion < 25.20 || !Driver)
        return;

    auto World = UWorld::GetWorld();
    if (!World || Driver != World->NetDriver)
        return;

    struct FModernPhaseTickGuard
    {
        UWorld* World = nullptr;
        UNetDriver* Driver = nullptr;
        AFortGameStateAthena* GameState = nullptr;
        double DriverTime = -1.0;
        bool bHasDriverTime = false;
    };
    static FModernPhaseTickGuard Guard;
    auto GameState = (AFortGameStateAthena*)World->GameState;
    const double DriverTime = Driver->GetTime();
    if (Guard.World != World || Guard.Driver != Driver || Guard.GameState != GameState ||
        !std::isfinite(DriverTime) || (Guard.bHasDriverTime && DriverTime < Guard.DriverTime))
    {
        Guard = {};
        Guard.World = World;
        Guard.Driver = Driver;
        Guard.GameState = GameState;
    }

    if (Guard.bHasDriverTime && std::isfinite(DriverTime) && DriverTime == Guard.DriverTime)
    {
        return;
    }
    Guard.DriverTime = DriverTime;
    Guard.bHasDriverTime = std::isfinite(DriverTime);

    auto GamePhaseLogic = CustomSafeZoneRuntime::ResolveLiveComponentPhaseLogic(World);
    if (GamePhaseLogic)
        GamePhaseLogic->Tick();
}

static void DriveLegacySafeZoneInsideChecks(UNetDriver* Driver)
{
    // Pre-7.00 can reach SafeZones without arming its repeating inside-check.
    if (VersionInfo.FortniteVersion >= 7.00)
        return;

    auto World = UWorld::GetWorld();
    if (!World || Driver != World->NetDriver)
        return;

    auto GameMode = (AFortGameMode*)World->AuthorityGameMode;
    auto GameState = (AFortGameStateAthena*)World->GameState;
    if (!GameMode || !GameState || !GameMode->HasSafeZoneIndicator() || !GameMode->SafeZoneIndicator ||
        !GameState->HasGamePhase() || GameState->GamePhase != 4)
    {
        return;
    }

    if (GameMode->HasbSafeZoneActive() && !GameMode->bSafeZoneActive)
        GameMode->bSafeZoneActive = true;

    static UWorld* LastWorld = nullptr;
    static float NextCheckTime = 0.f;
    static bool bLoggedForWorld = false;
    static bool bLoggedFourFiveRespawnReplacement = false;
    static int LastEffectHandleCount = -1;
    static int LastEffectLevel = -1;
    if (LastWorld != World)
    {
        LastWorld = World;
        GRespawnManagedStormPlayers.clear();
        GRespawnStormStateLoggedPlayers.clear();
        NextCheckTime = 0.f;
        bLoggedForWorld = false;
        bLoggedFourFiveRespawnReplacement = false;
        LastEffectHandleCount = -1;
        LastEffectLevel = -1;
    }

    const float TimeSeconds = (float)UGameplayStatics::GetTimeSeconds(World);
    if (TimeSeconds < NextCheckTime)
        return;
    NextCheckTime = TimeSeconds + 1.f;

    if (GameMode->HasGE_OutsideSafeZone() && !GameMode->GE_OutsideSafeZone)
        GameMode->GE_OutsideSafeZone = GetLowerSeasonStormEffectClass();

    auto SafeZoneInsideChecksFn = GameMode->GetFunction("SafeZoneInsideChecks");
    const auto Playlist = AFortGameMode::GetActivePlaylist(GameState);
    const float StormEffectDelay = Playlist && Playlist->HasStormEffectDelay()
        ? Playlist->StormEffectDelay : -1.f;

    if (!bLoggedForWorld)
    {
        bLoggedForWorld = true;
        SDK::DbgLog(
            "[SafeZone] legacy native damage watchdog armed version=%.2f fn=%p effect=%p active=%d paused=%d phase=%d locations=%d delay=%.2f\n",
            VersionInfo.FortniteVersion, (void*)SafeZoneInsideChecksFn,
            (void*)(GameMode->HasGE_OutsideSafeZone() ? GameMode->GE_OutsideSafeZone : nullptr),
            (int)(GameMode->HasbSafeZoneActive() ? GameMode->bSafeZoneActive : true),
            (int)(GameMode->HasbSafeZonePaused() ? GameMode->bSafeZonePaused : false),
            (int)GameState->GamePhase,
            GameMode->HasSafeZoneLocations() ? GameMode->SafeZoneLocations.Num() : -1,
            StormEffectDelay);
    }

    // 4.x fires an instant storm GE at respawned pawns even when the indicator says safe.
    const bool bReplaceFourFiveNativeCheck =
        VersionInfo.FortniteVersion >= 4.00 && VersionInfo.FortniteVersion < 5.00 &&
        !GRespawnManagedStormPlayers.empty();
    if (bReplaceFourFiveNativeCheck && !bLoggedFourFiveRespawnReplacement)
    {
        bLoggedFourFiveRespawnReplacement = true;
        SDK::DbgLog("[SafeZone] 4.x post-respawn damage check switched to indicator ownership\n");
    }

    if (SafeZoneInsideChecksFn && !bReplaceFourFiveNativeCheck)
        GameMode->ProcessEvent(SafeZoneInsideChecksFn, nullptr);

    if (SafeZoneInsideChecksFn || bReplaceFourFiveNativeCheck)
    {
        const int GameModePhase = GameMode->HasSafeZonePhase() ? GameMode->SafeZonePhase : 0;
        const int IndicatorPhase = GameMode->SafeZoneIndicator->HasCurrentPhase()
            ? GameMode->SafeZoneIndicator->CurrentPhase : 0;
        const int HighestStormPhase = GameModePhase > IndicatorPhase ? GameModePhase : IndicatorPhase;
        const int DamageEffectLevel = HighestStormPhase > 1 ? HighestStormPhase : 1;
        SynchronizeRespawnManagedStormEffects(
            Driver, GameMode, GetLowerSeasonStormEffectClass(), DamageEffectLevel);
        int UpdatedEffectCount = 0;
        const int EffectHandleCount = NormalizeLowerSeasonStormEffectLevels(
            GameMode, GetLowerSeasonStormEffectClass(), DamageEffectLevel, UpdatedEffectCount);

        if (UpdatedEffectCount > 0 || EffectHandleCount != LastEffectHandleCount ||
            DamageEffectLevel != LastEffectLevel)
        {
            SDK::DbgLog("[SafeZone] native storm effects gameModePhase=%d indicatorPhase=%d level=%d handles=%d updated=%d\n",
                GameModePhase, IndicatorPhase, DamageEffectLevel, EffectHandleCount, UpdatedEffectCount);
            LastEffectHandleCount = EffectHandleCount;
            LastEffectLevel = DamageEffectLevel;
        }
    }
}

namespace
{
    struct FAuthoritativeMatchLifecycleState
    {
        UWorld* World = nullptr;
        AFortGameMode* GameMode = nullptr;
        AFortGameStateAthena* GameState = nullptr;
        UFortGameStateComponent_BattleRoyaleGamePhaseLogic* PhaseLogic = nullptr;
        ULONGLONG NextPhaseLogicResolveMs = 0;
        bool bObservedLiveMatch = false;
        bool bObservedLegacyLivePhase = false;
        bool bObservedNativeTerminal = false;
        bool bEndLatched = false;
        bool bRestartPreflightBlocking = false;
        ULONGLONG NextRestartPreflightMs = 0;
    };

    FAuthoritativeMatchLifecycleState
        GAuthoritativeMatchLifecycleState{};

    bool IsLiveLifecycleObject(const UObject* Object)
    {
        if (!Object)
        {
            return false;
        }

        const int32 ObjectIndex = Object->Index;
        if (ObjectIndex < 0 || ObjectIndex >= TUObjectArray::Num())
        {
            return false;
        }

        auto Item = TUObjectArray::GetItemByIndex(ObjectIndex);
        constexpr int32 InvalidObjectFlags = 0x20;
        return Item && Item->GetObject() == Object && !(Item->GetFlags() & InvalidObjectFlags) &&
            Object->Class;
    }

    bool IsOwnedByLifecycleGameState(const UObject* Object, const AFortGameStateAthena* GameState)
    {
        const UObject* Current = Object;
        for (int32 Depth = 0;
            Current && Depth < 16;
            Depth++)
        {
            if (Current == GameState)
                return true;
            if (!IsLiveLifecycleObject(Current))
                return false;
            Current = Current->Outer;
        }
        return false;
    }

    UFortGameStateComponent_BattleRoyaleGamePhaseLogic* ResolveCurrentLifecyclePhaseLogic(
            FAuthoritativeMatchLifecycleState& State, UWorld* World,
            AFortGameStateAthena* GameState)
    {
        if (VersionInfo.FortniteVersion < 25.20)
            return nullptr;

        if (IsLiveLifecycleObject(State.PhaseLogic) && IsOwnedByLifecycleGameState(
                State.PhaseLogic, GameState))
        {
            return State.PhaseLogic;
        }
        State.PhaseLogic = nullptr;

        const ULONGLONG Now = GetTickCount64();
        if (Now < State.NextPhaseLogicResolveMs)
            return nullptr;

        auto PhaseLogicClass = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::StaticClass();
        if (!IsLiveLifecycleObject(PhaseLogicClass))
        {
            State.NextPhaseLogicResolveMs = Now + 1000;
            return nullptr;
        }

        UFortGameStateComponent_BattleRoyaleGamePhaseLogic* Candidate = nullptr;

        auto DefaultObject = (const UFortGameStateComponent_BattleRoyaleGamePhaseLogic*)
                PhaseLogicClass->GetDefaultObj();
        if (DefaultObject && DefaultObject->GetFunction("Get"))
        {
            Candidate = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(World);
        }

        if (IsLiveLifecycleObject(Candidate) && IsOwnedByLifecycleGameState(Candidate, GameState))
        {
            State.PhaseLogic = Candidate;
            return Candidate;
        }

        // Some builds ship a stale reflected Get helper; search this GameState's components instead.
        if (IsLiveLifecycleObject(GameState))
        {
            Candidate = (UFortGameStateComponent_BattleRoyaleGamePhaseLogic*)
                    GameState->GetComponentByClass(PhaseLogicClass);
            if (IsLiveLifecycleObject(Candidate) && !Candidate->IsDefaultObject() &&
                IsOwnedByLifecycleGameState(Candidate, GameState))
            {
                State.PhaseLogic = Candidate;
                return Candidate;
            }
        }

        State.NextPhaseLogicResolveMs = Now + 250;
        return nullptr;
    }

    bool TryReadLifecycleByte(const UObject* Owner, const char* PropertyName, uint8& Value)
    {
        if (!IsLiveLifecycleObject(Owner))
            return false;

        const uint32 Offset = Owner->GetOffset(PropertyName);
        if (Offset == static_cast<uint32>(-1) || Offset >= 0x10000)
        {
            return false;
        }

        const auto Address = (const uint8*)Owner + Offset;
        if (!SDK::MemReadable(Address, sizeof(uint8)))
        {
            return false;
        }

        Value = *Address;
        return true;
    }

    void TickAuthoritativeMatchLifecycle(UNetDriver* Driver)
    {
        auto World = UWorld::GetWorld();
        if (!Driver || !World || Driver != World->NetDriver)
        {
            return;
        }

        AutoHosting::TickPostMatchShutdown();

        auto GameMode = World->AuthorityGameMode ? World->AuthorityGameMode->Cast<AFortGameMode>()
                : nullptr;
        auto GameState = World->GameState ? World->GameState->Cast<AFortGameStateAthena>()
                : nullptr;
        if (!IsLiveLifecycleObject(GameMode) || !IsLiveLifecycleObject(GameState))
        {
            return;
        }

        auto& State = GAuthoritativeMatchLifecycleState;
        const bool bGenerationChanged = State.World != World || State.GameMode != GameMode ||
            State.GameState != GameState;
        bool bResetLifecycleForGeneration = false;
        if (bGenerationChanged)
        {
            const bool bHadGeneration = State.World || State.GameMode || State.GameState;
            const bool bExplicitEndPending = GUI::gsStatus.load(std::memory_order_acquire) == Ended;
            const bool bPreviousGenerationActive = State.bObservedLiveMatch || State.bEndLatched ||
                bExplicitEndPending;
            const bool bPreserveAutoHostEnd = (State.bEndLatched || bExplicitEndPending) &&
                FConfiguration::bAutoHost.load(std::memory_order_acquire);

            State = {};
            State.World = World;
            State.GameMode = GameMode;
            State.GameState = GameState;

            if (bHadGeneration && bPreviousGenerationActive && !bPreserveAutoHostEnd)
            {
                GUI::ResetServerLifecycle();
                bResetLifecycleForGeneration = true;
                SDK::DbgLog(
                    "[MatchLifecycle] New world/match generation detected; lifecycle reset\n");
            }
        }

        static const FName WaitingPostMatchState(L"WaitingPostMatch");
        static const FName WaitingToStartState(L"WaitingToStart");
        static const FName InProgressState(L"InProgress");

        bool bWaitingPostMatch = false;
        bool bWaitingToStart = false;
        bool bMatchStateInProgress = false;
        if (GameMode->HasMatchState())
        {
            const FName MatchState = GameMode->MatchState;
            bWaitingPostMatch = MatchState == WaitingPostMatchState;
            bWaitingToStart = MatchState == WaitingToStartState;
            bMatchStateInProgress = MatchState == InProgressState;
        }

        bool bPhaseLive = false;
        bool bPhasePregame = false;
        bool bPhaseTerminal = false;
        bool bStepTerminal = false;
        bool bUsingModernPhase = false;

        if (VersionInfo.FortniteVersion >= 25.20)
        {
            auto PhaseLogic = ResolveCurrentLifecyclePhaseLogic(State, World, GameState);
            if (PhaseLogic)
            {
                uint8 Phase = 0;
                uint8 PhaseStep = 0;
                const bool bHasPhase = TryReadLifecycleByte(PhaseLogic, "GamePhase", Phase);
                const bool bHasPhaseStep = TryReadLifecycleByte(PhaseLogic, "GamePhaseStep",
                        PhaseStep);
                bUsingModernPhase = bHasPhase || bHasPhaseStep;
                if (bHasPhase)
                {
                    bPhaseLive = Phase == (uint8)EAthenaGamePhase::Aircraft || Phase ==
                            (uint8)EAthenaGamePhase::SafeZones;
                    bPhasePregame = Phase == (uint8)EAthenaGamePhase::Setup || Phase ==
                            (uint8)EAthenaGamePhase::Warmup;
                    bPhaseTerminal = Phase == (uint8)EAthenaGamePhase::EndGame;
                }
                if (bHasPhaseStep)
                {
                    bStepTerminal = PhaseStep == (uint8)EAthenaGamePhaseStep::EndGame;
                }
            }
        }

        if (!bUsingModernPhase)
        {
            if (GameState->HasGamePhase())
            {
                const uint8 Phase = GameState->GamePhase;
                bPhaseLive = Phase == (uint8)EAthenaGamePhase::Aircraft || Phase ==
                        (uint8)EAthenaGamePhase::SafeZones;
                bPhasePregame = Phase == (uint8)EAthenaGamePhase::Setup || Phase ==
                        (uint8)EAthenaGamePhase::Warmup;
                bPhaseTerminal = Phase == (uint8)EAthenaGamePhase::EndGame;
                if (bPhaseLive)
                    State.bObservedLegacyLivePhase = true;
            }
            if (GameState->HasGamePhaseStep())
            {
                bStepTerminal = GameState->GamePhaseStep == (uint8)EAthenaGamePhaseStep::EndGame;
            }

            if (VersionInfo.FortniteVersion >= 25.20 && !State.bObservedLegacyLivePhase)
            {
                bPhaseTerminal = false;
                bStepTerminal = false;
            }
        }

        if (bResetLifecycleForGeneration)
        {
            if (bMatchStateInProgress || bPhaseLive)
            {
                GUI::gsStatus.store(StartedMatch, std::memory_order_release);
            }
        }

        const bool bNativeTerminal = bWaitingPostMatch || bPhaseTerminal || bStepTerminal;

        const bool bAuthoritativeNewGeneration = bWaitingToStart || bPhasePregame ||
            (State.bObservedNativeTerminal && !bNativeTerminal && bPhaseLive);
        if (State.bEndLatched && bAuthoritativeNewGeneration && !FConfiguration::bAutoHost.load(
                std::memory_order_acquire))
        {
            State.bEndLatched = false;
            State.bObservedLiveMatch = false;
            State.bObservedLegacyLivePhase = !bUsingModernPhase && bPhaseLive;
            State.bObservedNativeTerminal = false;
            AFortPlayerControllerAthena::ResetRespawnCameraForMatchRestart();
            GUI::ResetServerLifecycle();
            UFortGameStateComponent_BattleRoyaleGamePhaseLogic::ResetSafeZonePauseForMatch(World);
            CustomSafeZoneRuntime::ResetForMatch(World);
            State.bRestartPreflightBlocking = true;
            State.NextRestartPreflightMs = 0;
            SDK::DbgLog("[MatchLifecycle] Same-world match restart detected; lifecycle reset\n");
        }

        if (State.bRestartPreflightBlocking)
        {
            const ULONGLONG Now = GetTickCount64();
            if (CustomSafeZoneRuntime::IsMatchRestartPreflightPending(World) &&
                Now >= State.NextRestartPreflightMs)
            {
                auto MapInfo = GameState->HasMapInfo() ? GameState->MapInfo : nullptr;
                const int32 PhaseCapacity = MapInfo ? AFortGameMode::
                        ResolveMovingSafeZonePreflightCapacity(GameMode, MapInfo) : 0;
                const auto Preflight = CustomSafeZoneRuntime::PreflightForServerStart(
                        World, GameMode, MapInfo, PhaseCapacity, true, true);
                State.NextRestartPreflightMs = Now + 100ULL;
                SDK::DbgLog("[MatchLifecycle] held restart moving-zone "
                    "preflight=%d capacity=%d\n", (int)Preflight, PhaseCapacity);
            }

            if (CustomSafeZoneRuntime::IsMatchRestartPreflightPending(World))
            {
                GUI::gsStatus.store(NotReady, std::memory_order_release);
            }
            else
            {
                State.bRestartPreflightBlocking = false;
                if (bWaitingToStart)
                    GUI::MarkServerJoinable();
                else if (bPhaseLive || bMatchStateInProgress)
                {
                    GUI::gsStatus.store(StartedMatch, std::memory_order_release);
                }
                SDK::DbgLog(
                    "[MatchLifecycle] held restart preflight accepted; phase flow resumed\n");
            }
        }

        const EGSStatus CurrentStatus = GUI::gsStatus.load(std::memory_order_acquire);
        if (CurrentStatus == StartedMatch || CurrentStatus == Ended || bMatchStateInProgress ||
            bPhaseLive)
        {
            State.bObservedLiveMatch = true;
        }

        if (bNativeTerminal)
            State.bObservedNativeTerminal = true;

        const bool bExplicitEnd = CurrentStatus == Ended;
        const bool bTerminal = bExplicitEnd || (State.bObservedLiveMatch && bNativeTerminal);
        if (!State.bEndLatched && bTerminal)
        {
            State.bEndLatched = true;
            GUI::gsStatus.store(Ended, std::memory_order_release);

            AutoHosting::OnAuthoritativeMatchEnded();

            const char* Source = bExplicitEnd ? "explicit winner path" : (bWaitingPostMatch
                        ? "WaitingPostMatch" : (bUsingModernPhase ? "modern phase"
                            : "legacy phase"));
            SDK::DbgLog("[MatchLifecycle] Match ended via %s; autoHost=%d\n", Source,
                FConfiguration::bAutoHost.load(std::memory_order_acquire) ? 1 : 0);
        }
    }
}

void UNetDriver::TickFlush(UNetDriver* Driver, float DeltaSeconds)
{
    auto ActiveWorld = UWorld::GetWorld();
    if (!ActiveWorld || Driver != ActiveWorld->NetDriver)
    {
        TickFlushOG(Driver, DeltaSeconds);
        return;
    }

    GUI::SafeZoneMapGameTick();
    GUI::PlayerNamesGameTick();
    if (auto World = UWorld::GetWorld();
        World && Driver == World->NetDriver)
    {
        AFortInventory::TickRegeneratingItems();
        AFortWeaponRanged::TickProjectileRelays();
        AFortGameMode::TickPendingVehicleSpawns();
        AFortGameMode::TickSupplyDropSuppression();
        FortVehicleMods::TickPendingConstruction();
        FortVehicleBump::Tick();
        AFortOctopusVehicle::TickTowhookAttachments();
        AFortPlayerControllerAthena::TickVehicleLoadoutReconcile();
        AFortMinigame::TickCreativeMinigames();
        Calendar::TickSnow(); // drain the Calendar tab's snow request
    }
    TickAuthoritativeMatchLifecycle(Driver);
    FFortAthenaHeistCompatibility::Tick(Driver, DeltaSeconds);
    FFortAthenaNativeLTMCompatibility::Tick(Driver, DeltaSeconds);
    FFortAthenaScoreRoyaleCompatibility::Tick(Driver, DeltaSeconds);
    if (!CustomSafeZoneRuntime::IsMatchRestartPreflightPending(Driver ? Driver->World : nullptr))
    {
        SyncErbiumSafeZonePause(Driver);
        AFortGameMode::TickLateGameSafeZonePhaseFallback(Driver);
        SyncErbiumSafeZonePause(Driver);
        DriveLegacySafeZoneInsideChecks(Driver);

        if (auto World = UWorld::GetWorld();
            World && Driver == World->NetDriver)
        {
            AFortGameMode::TickGameplayConfigurationPolicy(DeltaSeconds);
        }

        TickModernGamePhaseLogicOnce(Driver);
    }
    AFortPlayerControllerAthena::TickNukeRockets(DeltaSeconds);

    VersionFeatureAdapter::TickServerFrame(Driver);

    if (Driver->ClientConnections.Num() > 0)
    {
        const ULONGLONG ReplicationStartMs = GetTickCount64();
        ServerReplicateActors(Driver, DeltaSeconds);
        const ULONGLONG ReplicationElapsedMs = GetTickCount64() - ReplicationStartMs;
        static uint32 SlowManualReplicationLogs = 0;
        if (ReplicationElapsedMs >= 8ULL && GUI::gsStatus == Ended && SlowManualReplicationLogs < 8)
        {
            ++SlowManualReplicationLogs;
            SDK::DbgLog("[ReplicationTiming] manual ServerReplicateActors "
                "elapsed=%llu ms clients=%d version=%.2f\n", ReplicationElapsedMs,
                Driver->ClientConnections.Num(), VersionInfo.FortniteVersion);
        }
    }

    if (GUI::gsStatus == Joinable && VersionInfo.FortniteVersion >= 11.00)
    {
        auto Time = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
        auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;
        auto CurrentPlaylist = AFortGameMode::GetActivePlaylist(GameState);
        const bool bSkipAircraft = CurrentPlaylist && CurrentPlaylist->HasbSkipAircraft() &&
            CurrentPlaylist->bSkipAircraft;
        auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
        if (!bSkipAircraft && GameState->HasWarmupCountdownEndTime() &&
            GameMode->MatchState == FName(L"InProgress") &&
            GameState->WarmupCountdownEndTime <= Time)
        {
            UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(),
                FString(L"startaircraft"), nullptr);
        }
    }
    else if (GUI::gsStatus == Ended && (FConfiguration::bAutoRestart || (FConfiguration::WebhookURL && *FConfiguration::WebhookURL)))
    {
        auto WorldNetDriver = UWorld::GetWorld()->NetDriver;
        auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
        if (Driver == WorldNetDriver && Driver->ClientConnections.Num() == 0)
        {
            static bool stopped = false;

            if (!stopped)
            {
                stopped = true;

                if constexpr (FConfiguration::WebhookURL && *FConfiguration::WebhookURL)
                {
                    auto curl = curl_easy_init();

                    curl_easy_setopt(curl, CURLOPT_URL, FConfiguration::WebhookURL);
                    curl_slist* headers = curl_slist_append(NULL, "Content-Type: application/json");
                    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

                    char version[6];

                    sprintf_s(version,
                        VersionInfo.FortniteVersion >= 5.00 || VersionInfo.FortniteVersion < 1.2 ? "%.2f" : "%.1f",
                        VersionInfo.FortniteVersion);

                    auto Playlist = AFortGameMode::GetActivePlaylist(GameMode->GameState);
                    auto payload = UEAllocatedString("{\"embeds\": [{\"title\": \"Match has ended!\", \"fields\": [{\"name\":\"Version\",\"value\":\"") + version + "\"}, {\"name\":\"Playlist\",\"value\":\"" + (Playlist ? Playlist->PlaylistName.ToString() : "Playlist_DefaultSolo") + "\"}], \"color\": " + "\"7237230\", \"footer\": {\"text\":\"Erbium\", \"icon_url\":\"https://cdn.discordapp.com/attachments/1341168629378584698/1436803905119064105/L0WnFa.png.png?ex=6910ef69&is=690f9de9&hm=01a0888b46647959b38ee58df322048ab49e2a5a678e52d4502d9c5e3978d805&\"}, \"timestamp\":\"" + iso8601() + "\"}] }";

                    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());

                    curl_easy_perform(curl);

                    curl_easy_cleanup(curl);
                }

                if (FConfiguration::bAutoRestart && !FConfiguration::bAutoHost)
                    TerminateProcess(GetCurrentProcess(), 0);
            }
        }
    }

    const ULONGLONG NativeTickFlushStartMs = GetTickCount64();
    TickFlushOG(Driver, DeltaSeconds);
    const ULONGLONG NativeTickFlushElapsedMs = GetTickCount64() - NativeTickFlushStartMs;
    static uint32 SlowNativeTickFlushLogs = 0;
    if (NativeTickFlushElapsedMs >= 8ULL && GUI::gsStatus == Ended && SlowNativeTickFlushLogs < 8)
    {
        ++SlowNativeTickFlushLogs;
        SDK::DbgLog("[ReplicationTiming] native TickFlush "
            "elapsed=%llu ms version=%.2f\n", NativeTickFlushElapsedMs,
            VersionInfo.FortniteVersion);
    }
    auto World = UWorld::GetWorld();
    if (World && Driver == World->NetDriver && Driver->ClientConnections.Num() > 0)
    {
        AFortPlayerControllerAthena::TickPendingVictoryCrownNotifications();
    }
}

uint64_t ServerReplicateActors_;
void UNetDriver::TickFlush__RepGraph(UNetDriver* Driver, float DeltaSeconds)
{
    auto ActiveWorld = UWorld::GetWorld();
    if (!ActiveWorld || Driver != ActiveWorld->NetDriver)
    {
        TickFlushOG(Driver, DeltaSeconds);
        return;
    }

    GUI::SafeZoneMapGameTick();
    GUI::PlayerNamesGameTick();
    if (auto World = UWorld::GetWorld();
        World && Driver == World->NetDriver)
    {
        AFortInventory::TickRegeneratingItems();
        AFortWeaponRanged::TickProjectileRelays();
        AFortGameMode::TickPendingVehicleSpawns();
        AFortGameMode::TickSupplyDropSuppression();
        FortVehicleMods::TickPendingConstruction();
        FortVehicleBump::Tick();
        AFortOctopusVehicle::TickTowhookAttachments();
        AFortPlayerControllerAthena::TickVehicleLoadoutReconcile();
        AFortMinigame::TickCreativeMinigames();
        Calendar::TickSnow(); // drain the Calendar tab's snow request
    }
    TickAuthoritativeMatchLifecycle(Driver);
    FFortAthenaHeistCompatibility::Tick(Driver, DeltaSeconds);
    FFortAthenaNativeLTMCompatibility::Tick(Driver, DeltaSeconds);
    FFortAthenaScoreRoyaleCompatibility::Tick(Driver, DeltaSeconds);
    if (!CustomSafeZoneRuntime::IsMatchRestartPreflightPending(Driver ? Driver->World : nullptr))
    {
        SyncErbiumSafeZonePause(Driver);
        AFortGameMode::TickLateGameSafeZonePhaseFallback(Driver);
        SyncErbiumSafeZonePause(Driver);
        DriveLegacySafeZoneInsideChecks(Driver);
        if (auto World = UWorld::GetWorld();
            World && Driver == World->NetDriver)
        {
            AFortGameMode::TickGameplayConfigurationPolicy(DeltaSeconds);
        }
        TickModernGamePhaseLogicOnce(Driver);
    }

    VersionFeatureAdapter::TickServerFrame(Driver);

    if (Driver->ReplicationDriver)
        AFortPlayerControllerAthena::TickNukeRockets(DeltaSeconds);

    if (Driver->ReplicationDriver)
    {
        // this is our main netdriver
        if (Driver->ClientConnections.Num() > 0)
        {
            const ULONGLONG ReplicationStartMs = GetTickCount64();
            ((void (*)(UObject*, float)) ServerReplicateActors_)(Driver->ReplicationDriver, DeltaSeconds);
            const ULONGLONG ReplicationElapsedMs = GetTickCount64() - ReplicationStartMs;
            static uint32 SlowRepGraphReplicationLogs = 0;
            if (ReplicationElapsedMs >= 8ULL && GUI::gsStatus == Ended &&
                SlowRepGraphReplicationLogs < 8)
            {
                ++SlowRepGraphReplicationLogs;
                SDK::DbgLog("[ReplicationTiming] RepGraph ServerReplicateActors "
                    "elapsed=%llu ms clients=%d version=%.2f\n", ReplicationElapsedMs,
                    Driver->ClientConnections.Num(), VersionInfo.FortniteVersion);
            }
        }

        if (GUI::gsStatus == Joinable && VersionInfo.FortniteVersion >= 11.00)
        {
            auto Time = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
            auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;
            auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
            auto CurrentPlaylist = AFortGameMode::GetActivePlaylist(GameState);
            const bool bSkipAircraft = CurrentPlaylist && CurrentPlaylist->HasbSkipAircraft() &&
                CurrentPlaylist->bSkipAircraft;
            if (!bSkipAircraft && GameState->HasWarmupCountdownEndTime() &&
                GameMode->MatchState == FName(L"InProgress") &&
                GameState->WarmupCountdownEndTime <= Time)
            {
                UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(),
                    FString(L"startaircraft"), nullptr);
            }
        }
        else if (GUI::gsStatus == Ended && (FConfiguration::bAutoRestart || (FConfiguration::WebhookURL && *FConfiguration::WebhookURL)))
        {
            auto WorldNetDriver = UWorld::GetWorld()->NetDriver;
            auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
            if (Driver == WorldNetDriver && Driver->ClientConnections.Num() == 0)
            {
                static bool stopped = false;

                if (!stopped)
                {
                    stopped = true;

                    if constexpr (FConfiguration::WebhookURL && *FConfiguration::WebhookURL)
                    {
                        auto curl = curl_easy_init();

                        curl_easy_setopt(curl, CURLOPT_URL, FConfiguration::WebhookURL);
                        curl_slist* headers = curl_slist_append(NULL, "Content-Type: application/json");
                        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

                        char version[6];

                        sprintf_s(version,
                            VersionInfo.FortniteVersion >= 5.00 || VersionInfo.FortniteVersion < 1.2 ? "%.2f" : "%.1f",
                            VersionInfo.FortniteVersion);

                        auto Playlist = AFortGameMode::GetActivePlaylist(GameMode->GameState);
                        auto payload = UEAllocatedString("{\"embeds\": [{\"title\": \"Match has ended!\", \"fields\": [{\"name\":\"Version\",\"value\":\"") + version + "\"}, {\"name\":\"Playlist\",\"value\":\"" + (Playlist ? Playlist->PlaylistName.ToString() : "Playlist_DefaultSolo") + "\"}], \"color\": " + "\"7237230\", \"footer\": {\"text\":\"Erbium\", \"icon_url\":\"https://cdn.discordapp.com/attachments/1341168629378584698/1436803905119064105/L0WnFa.png.png?ex=6910ef69&is=690f9de9&hm=01a0888b46647959b38ee58df322048ab49e2a5a678e52d4502d9c5e3978d805&\"}, \"timestamp\":\"" + iso8601() + "\"}] }";

                        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());

                        curl_easy_perform(curl);

                        curl_easy_cleanup(curl);
                    }

                    if (FConfiguration::bAutoRestart && !FConfiguration::bAutoHost)
                        TerminateProcess(GetCurrentProcess(), 0);
                }
            }
        }
    }

    const ULONGLONG NativeTickFlushStartMs = GetTickCount64();
    TickFlushOG(Driver, DeltaSeconds);
    const ULONGLONG NativeTickFlushElapsedMs = GetTickCount64() - NativeTickFlushStartMs;
    static uint32 SlowRepGraphNativeTickFlushLogs = 0;
    if (NativeTickFlushElapsedMs >= 8ULL && GUI::gsStatus == Ended &&
        SlowRepGraphNativeTickFlushLogs < 8)
    {
        ++SlowRepGraphNativeTickFlushLogs;
        SDK::DbgLog("[ReplicationTiming] RepGraph native TickFlush "
            "elapsed=%llu ms version=%.2f\n", NativeTickFlushElapsedMs,
            VersionInfo.FortniteVersion);
    }
    auto World = UWorld::GetWorld();
    if (World && Driver == World->NetDriver && Driver->ClientConnections.Num() > 0)
    {
        AFortPlayerControllerAthena::TickPendingVictoryCrownNotifications();
    }
}

enum class EReplicationSystemSendPass : unsigned
{
    Invalid, PostTickDispatch, TickFlush,
};

struct FSendUpdateParams
{
    EReplicationSystemSendPass SendPass = EReplicationSystemSendPass::TickFlush;
    float DeltaSeconds = 0.f;
};

void SendClientMoveAdjustments(UNetDriver* Driver)
{
    static auto SendClientAdjustment = (void(*)(AFortPlayerControllerAthena*)) FindSendClientAdjustment();
    if (SendClientAdjustment)
    {
        for (UNetConnection* Connection : Driver->ClientConnections)
        {
            if (Connection == nullptr || Connection->ViewTarget == nullptr)
                continue;

            if (AFortPlayerControllerAthena* PC = Connection->PlayerController)
                SendClientAdjustment(PC);

            for (UNetConnection* ChildConnection : Connection->Children)
            {
                if (ChildConnection == nullptr)
                    continue;

                if (AFortPlayerControllerAthena* PC = ChildConnection->PlayerController)
                    SendClientAdjustment(PC);
            }
        }
    }
}

void UNetDriver::TickFlush__Iris(UNetDriver* Driver, float DeltaSeconds)
{
    const float NativeDeltaSeconds = DeltaSeconds;
    auto ActiveWorld = UWorld::GetWorld();
    if (!ActiveWorld || Driver != ActiveWorld->NetDriver)
    {
        TickFlushOG(Driver, NativeDeltaSeconds);
        return;
    }

    GUI::SafeZoneMapGameTick();
    GUI::PlayerNamesGameTick();
    if (auto World = UWorld::GetWorld();
        World && Driver == World->NetDriver)
    {
        AFortInventory::TickRegeneratingItems();
        AFortWeaponRanged::TickProjectileRelays();
        AFortGameMode::TickPendingVehicleSpawns();
        AFortGameMode::TickSupplyDropSuppression();
        FortVehicleMods::TickPendingConstruction();
        FortVehicleBump::Tick();
        AFortOctopusVehicle::TickTowhookAttachments();
        AFortPlayerControllerAthena::TickVehicleLoadoutReconcile();
        AFortMinigame::TickCreativeMinigames();
        Calendar::TickSnow(); // drain the Calendar tab's snow request
    }
    TickAuthoritativeMatchLifecycle(Driver);
    FFortAthenaHeistCompatibility::Tick(Driver, DeltaSeconds);
    FFortAthenaNativeLTMCompatibility::Tick(Driver, DeltaSeconds);
    FFortAthenaScoreRoyaleCompatibility::Tick(Driver, DeltaSeconds);
    if (!CustomSafeZoneRuntime::IsMatchRestartPreflightPending(Driver ? Driver->World : nullptr))
    {
        SyncErbiumSafeZonePause(Driver);
        AFortGameMode::TickLateGameSafeZonePhaseFallback(Driver);
        SyncErbiumSafeZonePause(Driver);
        DriveLegacySafeZoneInsideChecks(Driver);

        if (auto World = UWorld::GetWorld();
            World && Driver == World->NetDriver)
        {
            AFortGameMode::TickGameplayConfigurationPolicy(DeltaSeconds);
        }

        TickModernGamePhaseLogicOnce(Driver);
    }

    AFortPlayerControllerAthena::TickNukeRockets(DeltaSeconds);

    VersionFeatureAdapter::TickServerFrame(Driver);

    if (Driver->ClientConnections.Num() > 0)
    {
        auto ReplicationSystem = *(UObject**)(__int64(&Driver->ReplicationDriver) + 8);

        if (ReplicationSystem)
        {
            static void(*UpdateIrisReplicationViews)(UNetDriver*) = decltype(UpdateIrisReplicationViews)(FindUpdateIrisReplicationViews());
            static void(*PreSendUpdate)(UObject*, FSendUpdateParams&) = decltype(PreSendUpdate)(FindPreSendUpdate());

            UpdateIrisReplicationViews(Driver);
            SendClientMoveAdjustments(Driver);
            FSendUpdateParams Params;
            Params.DeltaSeconds = DeltaSeconds;
            const ULONGLONG PreSendStartMs = GetTickCount64();
            PreSendUpdate(ReplicationSystem, Params);
            const ULONGLONG PreSendElapsedMs = GetTickCount64() - PreSendStartMs;
            static uint32 SlowIrisPreSendLogs = 0;
            if (PreSendElapsedMs >= 8ULL && GUI::gsStatus == Ended && SlowIrisPreSendLogs < 8)
            {
                ++SlowIrisPreSendLogs;
                SDK::DbgLog("[ReplicationTiming] Iris PreSendUpdate "
                    "elapsed=%llu ms clients=%d version=%.2f\n", PreSendElapsedMs,
                    Driver->ClientConnections.Num(), VersionInfo.FortniteVersion);
            }
        }
    }

    if (GUI::gsStatus == Joinable && VersionInfo.FortniteVersion < 25.20)
    {
        auto Time = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
        auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;
        auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
        auto CurrentPlaylist = AFortGameMode::GetActivePlaylist(GameState);
        const bool bSkipAircraft = CurrentPlaylist && CurrentPlaylist->HasbSkipAircraft() &&
            CurrentPlaylist->bSkipAircraft;
        if (!bSkipAircraft && GameState->HasWarmupCountdownEndTime() &&
            GameMode->MatchState == FName(L"InProgress") &&
            GameState->WarmupCountdownEndTime <= Time)
        {
            UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(),
                FString(L"startaircraft"), nullptr);
        }
    }
    else if (GUI::gsStatus == Ended && (FConfiguration::bAutoRestart || (FConfiguration::WebhookURL && *FConfiguration::WebhookURL)))
    {
        auto WorldNetDriver = UWorld::GetWorld()->NetDriver;
        auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
        if (Driver == WorldNetDriver && Driver->ClientConnections.Num() == 0)
        {
            static bool stopped = false;

            if (!stopped)
            {
                stopped = true;

                if constexpr (FConfiguration::WebhookURL && *FConfiguration::WebhookURL)
                {
                    auto curl = curl_easy_init();

                    curl_easy_setopt(curl, CURLOPT_URL, FConfiguration::WebhookURL);
                    curl_slist* headers = curl_slist_append(NULL, "Content-Type: application/json");
                    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

                    char version[6];

                    sprintf_s(version,
                        VersionInfo.FortniteVersion >= 5.00 || VersionInfo.FortniteVersion < 1.2 ? "%.2f" : "%.1f",
                        VersionInfo.FortniteVersion);

                    auto Playlist = AFortGameMode::GetActivePlaylist(GameMode->GameState);
                    auto payload = UEAllocatedString("{\"embeds\": [{\"title\": \"Match has ended!\", \"fields\": [{\"name\":\"Version\",\"value\":\"") + version + "\"}, {\"name\":\"Playlist\",\"value\":\"" + (Playlist ? Playlist->PlaylistName.ToString() : "Playlist_DefaultSolo") + "\"}], \"color\": " + "\"7237230\", \"footer\": {\"text\":\"Erbium\", \"icon_url\":\"https://cdn.discordapp.com/attachments/1341168629378584698/1436803905119064105/L0WnFa.png.png?ex=6910ef69&is=690f9de9&hm=01a0888b46647959b38ee58df322048ab49e2a5a678e52d4502d9c5e3978d805&\"}, \"timestamp\":\"" + iso8601() + "\"}] }";

                    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());

                    curl_easy_perform(curl);

                    curl_easy_cleanup(curl);
                }

                if (FConfiguration::bAutoRestart && !FConfiguration::bAutoHost)
                    TerminateProcess(GetCurrentProcess(), 0);
            }
        }
    }

    const ULONGLONG NativeTickFlushStartMs = GetTickCount64();
    TickFlushOG(Driver, NativeDeltaSeconds);
    const ULONGLONG NativeTickFlushElapsedMs = GetTickCount64() - NativeTickFlushStartMs;
    static uint32 SlowIrisNativeTickFlushLogs = 0;
    if (NativeTickFlushElapsedMs >= 8ULL && GUI::gsStatus == Ended &&
        SlowIrisNativeTickFlushLogs < 8)
    {
        ++SlowIrisNativeTickFlushLogs;
        SDK::DbgLog("[ReplicationTiming] Iris native TickFlush "
            "elapsed=%llu ms version=%.2f\n", NativeTickFlushElapsedMs,
            VersionInfo.FortniteVersion);
    }
}

void (*SetNetDormancyOG)(AActor* Actor, int NewDormancy);
void SetNetDormancy(AActor* Actor, int NewDormancy)
{
    auto Driver = (UNetDriver*)UWorld::GetWorld()->NetDriver;

    SetNetDormancyOG(Actor, NewDormancy);

    if (Driver)
        if (NewDormancy <= 1)
            for (auto& Conn : Driver->ClientConnections)
                ((void(*)(UNetConnection*, AActor*)) FindFlushDormancy())(Conn, Actor);
}

void (*FlushNetDormancyOG)(AActor* Actor);
void FlushNetDormancy(AActor* Actor)
{
    auto Driver = (UNetDriver*)UWorld::GetWorld()->NetDriver;

    FlushNetDormancyOG(Actor);

    if (Driver)
        if (Actor->NetDormancy > 1)
            for (auto& Conn : Driver->ClientConnections)
                ((void(*)(UNetConnection*, AActor*)) FindFlushDormancy())(Conn, Actor);
}

void UNetDriver::PostLoadHook()
{
    if (VersionInfo.EngineVersion == 4.16)
    {
        NetworkObjectListOffset = 0x3f8;
        ReplicationFrameOffset = 0x288;
    }
    else if (VersionInfo.EngineVersion == 4.19)
    {
        NetworkObjectListOffset = 0x490;
        ReplicationFrameOffset = 0x2c8;
    }
    else if (VersionInfo.FortniteVersion >= 2.5 && VersionInfo.FortniteVersion <= 3.1)
    {
        NetworkObjectListOffset = VersionInfo.FortniteVersion == 3.1 ? 0x4F8 : 0x4F0;
        ReplicationFrameOffset = 0x328;
    }
    else if (VersionInfo.FortniteVersion <= 3.3)
    {
        NetworkObjectListOffset = VersionInfo.FortniteVersion == 3.3 ? 0x508 : 0x500;
        ReplicationFrameOffset = 0x330;
    }
    else if (VersionInfo.FortniteVersion >= 20.40 && VersionInfo.FortniteVersion < 22)
    {
        NetworkObjectListOffset = 0x6b8;
        ReplicationFrameOffset = 0x3d8;
    }
    else if (std::floor(VersionInfo.FortniteVersion) == 20)
    {
        NetworkObjectListOffset = 0x6c8;
        ReplicationFrameOffset = 0x3e0;
    }
    else if (std::floor(VersionInfo.FortniteVersion) == 22)
    {
        NetworkObjectListOffset = 0x708;
        ReplicationFrameOffset = 0x428;
    }
    else if (VersionInfo.FortniteVersion >= 23 && VersionInfo.FortniteVersion < 24.30)
    {
        ReplicationFrameOffset = VersionInfo.FortniteVersion == 24.20 ? 0x438 : 0x440;
        NetworkObjectListOffset = VersionInfo.FortniteVersion < 24 ? 0x720 : 0x730;
    }
    else if (VersionInfo.FortniteVersion >= 24.30 && VersionInfo.FortniteVersion < 28)
    {
        NetworkObjectListOffset = VersionInfo.FortniteVersion < 25.11 ? 0x738 : 0x750;
        ReplicationFrameOffset = VersionInfo.FortniteVersion < 25.11 ? 0x440 : 0x458;
    }
    else if (VersionInfo.FortniteVersion >= 28)
    {
        NetworkObjectListOffset = 0x760;
        ReplicationFrameOffset = 0x468;
    }

    if (VersionInfo.FortniteVersion <= 1.72 && VersionInfo.FortniteVersion != 1.1 &&
        VersionInfo.FortniteVersion != 1.11)
        ClientWorldPackageNameOffset = 0x336A8;
    else if (VersionInfo.FortniteVersion == 1.8 || VersionInfo.FortniteVersion == 1.81 ||
        VersionInfo.FortniteVersion == 1.82 || VersionInfo.FortniteVersion == 1.9)
        ClientWorldPackageNameOffset = 0x33788;
    else if (VersionInfo.FortniteVersion == 1.10)
        ClientWorldPackageNameOffset = 0x337A8;
    else if (VersionInfo.FortniteVersion == 1.11)
        ClientWorldPackageNameOffset = 0x337B8;
    else if (VersionInfo.FortniteVersion >= 2.2 && VersionInfo.FortniteVersion <= 2.4)
        ClientWorldPackageNameOffset = 0xA17A8;
    else if (VersionInfo.FortniteVersion == 2.42 || VersionInfo.FortniteVersion == 2.5)
        ClientWorldPackageNameOffset = 0x17F8;
    else if (VersionInfo.FortniteVersion == 3.1)
        ClientWorldPackageNameOffset = 0x1818;
    else if (VersionInfo.FortniteVersion == 3.2)
        ClientWorldPackageNameOffset = 0x1820;
    else if (VersionInfo.FortniteVersion == 3.3)
        ClientWorldPackageNameOffset = 0x1828;
    else if (VersionInfo.FortniteVersion < 24 && VersionInfo.FortniteVersion > 23.20)
        ClientWorldPackageNameOffset = 0x17D0;
    else if (VersionInfo.FortniteVersion >= 23 && VersionInfo.FortniteVersion <= 23.20)
        ClientWorldPackageNameOffset = 0x1780;
    else if (std::floor(VersionInfo.FortniteVersion) == 22)
        ClientWorldPackageNameOffset = 0x1730;
    else if (VersionInfo.FortniteVersion >= 28)
        ClientWorldPackageNameOffset = 0x1828;
    else if (VersionInfo.FortniteVersion >= 25.30)
        ClientWorldPackageNameOffset = 0x1820;
    else if (VersionInfo.EngineVersion == 5.2)
        ClientWorldPackageNameOffset = 0x1818;
    else if (VersionInfo.FortniteVersion >= 24)
        ClientWorldPackageNameOffset = 0x1820;
    else if (VersionInfo.FortniteVersion >= 20.20)
        ClientWorldPackageNameOffset = 0x16b8;
    else if (VersionInfo.FortniteVersion >= 20)
        ClientWorldPackageNameOffset = 0x1698;

    if (VersionInfo.FortniteVersion >= 25.10)
    {
        DestroyedStartupOrDormantActorsOffset = VersionInfo.FortniteVersion >= 28 ? 0x328 : 0x318;
        DestroyedStartupOrDormantActorGUIDsOffset = VersionInfo.FortniteVersion >= 28 ? 0x14b8 : (VersionInfo.FortniteVersion < 25.30 ? 0x14a8 : 0x14b0);
        ClientVisibleLevelNamesOffset = DestroyedStartupOrDormantActorGUIDsOffset + (VersionInfo.FortniteVersion < 24 ? 0x190 : 0x1e0);
    }
    else if (VersionInfo.FortniteVersion >= 23)
    {
        DestroyedStartupOrDormantActorsOffset = std::floor(VersionInfo.FortniteVersion) == 24 && VersionInfo.FortniteVersion < 24.30 ? 0x2f8 : 0x300;
        DestroyedStartupOrDormantActorGUIDsOffset = VersionInfo.EngineVersion == 5.2 ? 0x14a8 : 0x14b0;
        ClientVisibleLevelNamesOffset = DestroyedStartupOrDormantActorGUIDsOffset + (VersionInfo.FortniteVersion < 24 ? 0x190 : 0x1e0);
    }
    else if (VersionInfo.FortniteVersion >= 20.40)
    {
        DestroyedStartupOrDormantActorsOffset = 0x2e8;
        DestroyedStartupOrDormantActorGUIDsOffset = VersionInfo.EngineVersion >= 5.1 ? 0x14b0 : 0x1488;
        ClientVisibleLevelNamesOffset = DestroyedStartupOrDormantActorGUIDsOffset + (VersionInfo.EngineVersion >= 5.1 ? 0xf0 : 0xa0);
    }
    else if (VersionInfo.FortniteVersion >= 20)
    {
        DestroyedStartupOrDormantActorsOffset = 0x2f0;
        DestroyedStartupOrDormantActorGUIDsOffset = VersionInfo.FortniteVersion >= 20.20 ? 0x1488 : 0x1468;
        ClientVisibleLevelNamesOffset = DestroyedStartupOrDormantActorGUIDsOffset + 0xa0;
    }

    if (!FindServerReplicateActors())
    {
        if (VersionInfo.EngineVersion >= 5.3 && FConfiguration::bEnableIris)
        {
            SDK::DbgLog("[ReplicationPolicy] version=%.2f model=Iris "
                "explicitSend=%d nativeSend=1\n", VersionInfo.FortniteVersion, 1);
            FindSendClientAdjustment();
            FindUpdateIrisReplicationViews();
            FindPreSendUpdate();
            Utils::Hook(FindTickFlush(), TickFlush__Iris, TickFlushOG);
            return;
        }
        FindCreateChannel();
        FindSetChannelActor();
        FindReplicateActor();
        FindSendClientAdjustment();
        FindIsNetRelevantForVft();
        FindCallPreReplication();
        FindCloseActorChannel();
        FindStartBecomingDormant();
        FindClientHasInitializedLevelFor();
        FindSetChannelActorForDestroy();
        FindSendDestructionInfo();
        FindIsNetReady();

        if (VersionInfo.FortniteVersion < 3.4)
            FindFlushDormancy();
        else
        {
            FindGetNamePool();
        }

        GetActorLocation = (void(*)(AActor*, FFrame&,
            FVector*))AActor::GetDefaultObj()->GetFunction("K2_GetActorLocation")->GetNativeFunc();

        SDK::DbgLog("[ReplicationPolicy] version=%.2f model=Generic "
            "explicitSend=%d nativeSend=1\n", VersionInfo.FortniteVersion, 1);
        Utils::Hook(FindTickFlush(), TickFlush, TickFlushOG);
    }
    else
    {
        ServerReplicateActors_ = FindServerReplicateActors();

        SDK::DbgLog("[ReplicationPolicy] version=%.2f model=RepGraph "
            "explicitSend=1 nativeSend=1\n", VersionInfo.FortniteVersion);
        Utils::Hook(FindTickFlush(), TickFlush__RepGraph, TickFlushOG);
    }

    if (VersionInfo.FortniteVersion < 3.4 && FindFlushDormancy())
    {
        Utils::Hook(__int64(AActor::GetDefaultObj()->GetFunction("FlushNetDormancy")->GetImpl()),
            FlushNetDormancy, FlushNetDormancyOG);
        Utils::Hook(__int64(AActor::GetDefaultObj()->GetFunction("SetNetDormancy")->GetImpl()),
            SetNetDormancy, SetNetDormancyOG);
    }
}
