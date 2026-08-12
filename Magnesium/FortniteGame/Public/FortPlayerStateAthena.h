#pragma once
#include "../../pch.h"
#include "../../Engine/Public/AbilitySystemComponent.h"
#include "GameplayTagContainer.h"

struct FUniqueNetIdRepl
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FUniqueNetIdRepl);
    uint8_t Pad[0x50];

    DEFINE_STRUCT_PROP(ReplicationBytes, TArray<uint8>);
};

struct FDeathInfo
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FDeathInfo);

    DEFINE_STRUCT_PROP(Killer, AActor*);
    DEFINE_STRUCT_PROP(bDBNO, bool);
    DEFINE_STRUCT_PROP(DeathLocation, FVector);
    DEFINE_STRUCT_PROP(DeathTags, FGameplayTagContainer);
    DEFINE_STRUCT_PROP(DeathCause, uint8);
    DEFINE_STRUCT_PROP(DeathClassSlot, uint8);
    DEFINE_STRUCT_PROP(Downer, AActor*);
    DEFINE_STRUCT_PROP(FinisherOrDowner, AActor*);
    DEFINE_STRUCT_PROP(Distance, float);
    DEFINE_STRUCT_PROP(bInitialized, bool);
    DEFINE_STRUCT_PROP(FinisherOrDownerTags, FGameplayTagContainer);
    DEFINE_STRUCT_PROP(VictimTags, FGameplayTagContainer);
};

struct FFortRespawnData
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FFortRespawnData);

    DEFINE_STRUCT_PROP(RespawnLocation, FVector);
    DEFINE_STRUCT_PROP(RespawnRotation, FRotator);
    DEFINE_STRUCT_PROP(bClientIsReady, bool);
    DEFINE_STRUCT_PROP(bServerIsReady, bool);
    DEFINE_STRUCT_PROP(bRespawnDataAvailable, bool);
};

class AFortPlayerStateAthena;

struct FPlayerTeam
{
    USCRIPTSTRUCT_COMMON_MEMBERS(FPlayerTeam);
    TArray<AFortPlayerStateAthena*> TeamMembers;
};

class AFortPlayerStateAthena : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortPlayerStateAthena);

    DEFINE_PROP(AbilitySystemComponent, UAbilitySystemComponent*);
    DEFINE_PROP(SquadId, uint8);
    DEFINE_PROP(TeamIndex, uint8);
    DEFINE_PROP(UniqueId, FUniqueNetIdRepl);
    DEFINE_PROP(UniqueID, FUniqueNetIdRepl);
    DEFINE_PROP(PawnDeathLocation, FVector);
    DEFINE_PROP(DeathInfo, FDeathInfo);
    DEFINE_PROP(Kills, int32);
    DEFINE_PROP(KillScore, int32);
    DEFINE_PROP(TeamKillScore, int32);
    DEFINE_PROP(NumChestsOpened, int32);
    DEFINE_PROP(NumAmmoCansOpened, int32);
    DEFINE_PROP(NumSupplyDropsOpened, int32);
    DEFINE_PROP(NumLlamasOpened, int32);
    DEFINE_PROP(NumForagedItemsConsumed, int32);
    DEFINE_PROP(NumMinutesAlive, int32);
    DEFINE_PROP(NumBronzeCoinsCollected, int32);
    DEFINE_PROP(NumSilverCoinsCollected, int32);
    DEFINE_PROP(NumGoldCoinsCollected, int32);
    DEFINE_PROP(TotalPlayerScore, int32);
    DEFINE_PROP(TeamScorePlacement, int32);
    DEFINE_PROP(TeamScore, int32);
    DEFINE_PROP(Place, int32);
    DEFINE_PROP(RespawnData, FFortRespawnData);
    DEFINE_PROP(SeasonLevelUIDisplay, int32);
    DEFINE_PROP(CharacterParts, const UObject**);
    DEFINE_PROP(HeroType, const UObject*);
    DEFINE_BITFIELD_PROP(bIsABot);
    DEFINE_BITFIELD_PROP(bIsSpectator);
    DEFINE_BITFIELD_PROP(bOnlySpectator);
    DEFINE_BITFIELD_PROP(bIsInactive);
    DEFINE_BITFIELD_PROP(bHasWonAGame);
    DEFINE_BITFIELD_PROP(bInGhostMode);
    DEFINE_PROP(WorldPlayerId, int16);
    DEFINE_PROP(SpectatingTarget, UObject*);
    DEFINE_PROP(TeamMemberState, uint8);
    DEFINE_PROP(ReplicatedTeamMemberState, uint8);
    DEFINE_PROP(PlayerTeam, FPlayerTeam*);

    DEFINE_FUNC(GetPlayerName, FString);
    DEFINE_FUNC(OnRep_SquadId, void);
    DEFINE_FUNC(OnRep_DeathInfo, void);
    DEFINE_FUNC(OnRep_bIsInactive, void);
    DEFINE_FUNC(OnRep_SpectatingTarget, void);
    DEFINE_STATIC_FUNC(ToDeathCause, uint8);
    DEFINE_FUNC(OnRep_Kills, void);
    DEFINE_FUNC(OnRep_TeamKillScore, void);
    DEFINE_FUNC(OnRep_NumChestsOpened, void);
    DEFINE_FUNC(OnRep_NumAmmoCansOpened, void);
    DEFINE_FUNC(OnRep_NumSupplyDropsOpened, void);
    DEFINE_FUNC(OnRep_NumLlamasOpened, void);
    DEFINE_FUNC(OnRep_NumForagedItemsConsumed, void);
    DEFINE_FUNC(OnRep_NumMinutesAlive, void);
    DEFINE_FUNC(OnRep_NumBronzeCoinsCollected, void);
    DEFINE_FUNC(OnRep_NumSilverCoinsCollected, void);
    DEFINE_FUNC(OnRep_NumGoldCoinsCollected, void);
    DEFINE_FUNC(OnRep_TotalPlayerScore, void);
    DEFINE_FUNC(OnRep_TeamScorePlacement, void);
    DEFINE_FUNC(OnRep_TeamScore, void);
    DEFINE_FUNC(ClientReportKill, void);
    DEFINE_FUNC(ClientReportTeamKill, void);
    DEFINE_FUNC(OnRep_Place, void);
    DEFINE_FUNC(OnRep_SeasonLevelUIDisplay, void);
    DEFINE_FUNC(OnRep_PlayerName, void);
    DEFINE_FUNC(OnRep_ReplicatedTeamMemberState, void);
    DEFINE_FUNC(ClientReportTournamentStatUpdate, void);
    DEFINE_FUNC(GetPingInMilliseconds, float);

    void ApplyKillScore(int32 NewScore);
    int32 GetEffectiveKillScore() const;

    FString& GetSavedNetworkAddress()
    {
        static auto SavedNetworkAddressOffset = GetOffset("SavedNetworkAddress");
        return *reinterpret_cast<FString*>(reinterpret_cast<uint8_t*>(this) + SavedNetworkAddressOffset);
    }
};
