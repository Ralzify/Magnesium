#pragma once
#include "../../pch.h"
#include "../../Engine/Public/DataTable.h"
#include "../../Engine/Public/DataTableFunctionLibrary.h"
#include "../../Engine/Public/CurveTable.h"
#include "GameplayTagContainer.h"
#include "../../Engine/Public/AbilitySystemComponent.h"

struct FUIExtension final
{
public:
    uint8                                         Slot;                                              // 0x0000(0x0001)(Edit, ZeroConstructor, DisableEditOnInstance, IsPlainOldData, NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic)
    uint8                                         Pad_1[0x7];                                        // 0x0001(0x0007)(Fixing Size After Last Property [ Dumper-7 ])
    TSoftClassPtr<class UClass>                   WidgetClass;                                       // 0x0008(0x0028)(Edit, DisableEditOnInstance, UObjectWrapper, HasGetValueTypeHash, NativeAccessSpecifierPublic)
};

struct EPlaylistUIExtensionSlot
{
public:
    UENUM_COMMON_MEMBERS(EPlaylistUIExtensionSlot);

    DEFINE_ENUM_PROP(Primary);
};

struct EUIExtensionSlot
{
public:
    UENUM_COMMON_MEMBERS(EUIExtensionSlot);

    DEFINE_ENUM_PROP(Primary);
};

struct FFortDeliveryInfoRequirementsFilter
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FFortDeliveryInfoRequirementsFilter);

    DEFINE_STRUCT_BITFIELD_PROP(bApplyToPlayerPawns);
};

struct FFortAbilitySetDeliveryInfo
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FFortAbilitySetDeliveryInfo);

    DEFINE_STRUCT_PROP(DeliveryRequirements, FFortDeliveryInfoRequirementsFilter);
    DEFINE_STRUCT_PROP(AbilitySets, TArray<TSoftObjectPtr<UFortAbilitySet>>);
};

// Authored Score Royale rules.  These are reflection-backed because Epic
// changed their native layout between the legacy and plugin-mounted versions
// of the mode; callers must index ScoreDataList with FAthenaScoreData::Size().
struct FAthenaScoreData
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FAthenaScoreData);

    DEFINE_STRUCT_PROP(ScoringEvent, uint8);
    DEFINE_STRUCT_PROP(ScoreNameText, FText);
    DEFINE_STRUCT_PROP(EventInclusionTags, FGameplayTagContainer);
    DEFINE_STRUCT_PROP(NumOccurrencesForScore, int32);
    DEFINE_STRUCT_PROP(NumOccurrencesPermitted, int32);
    DEFINE_STRUCT_PROP(ScoreAwarded, FScalableFloat);
};

struct FWinConditionScoreData
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FWinConditionScoreData);

    DEFINE_STRUCT_PROP(GoalScore, FScalableFloat);
    DEFINE_STRUCT_PROP(BigScoreThreshold, FScalableFloat);
    DEFINE_STRUCT_PROP(ScoreDataList, TArray<FAthenaScoreData>);
};

class UFortGameplayModifierItemDefinition : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UFortGameplayModifierItemDefinition);

    DEFINE_PROP(PersistentAbilitySets, TArray<FFortAbilitySetDeliveryInfo>);
    DEFINE_PROP(Mutators, TArray<TSoftClassPtr<UClass>>);
};

class UFortPlaylistAthena : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UFortPlaylistAthena);

    DEFINE_PROP(PlaylistName, FName);
    DEFINE_PROP(PlaylistId, int32);
    DEFINE_PROP(MaxPlayers, int32);
    DEFINE_PROP(AdditionalLevels, TArray<TSoftObjectPtr<UWorld>>);
    DEFINE_PROP(AdditionalLevelsServerOnly, TArray<TSoftObjectPtr<UWorld>>);
    DEFINE_PROP(GarbageCollectionFrequency, float);
    DEFINE_PROP(MaxSquadSize, int32);
    DEFINE_PROP(LootTierData, TSoftObjectPtr<UDataTable>);
    DEFINE_PROP(LootPackages, TSoftObjectPtr<UDataTable>);
    DEFINE_PROP(AirCraftBehavior, uint8);
    DEFINE_PROP(SafeZoneStartUp, uint8);
    DEFINE_PROP(StormEffectDelay, float);
    DEFINE_PROP(LastSafeZoneIndex, int32);
    DEFINE_PROP(bDisplayRespawnWidget, bool);
    DEFINE_PROP(bRespawnInAir, bool);
    DEFINE_PROP(RespawnHeight, FScalableFloat);
    DEFINE_PROP(RespawnTime, FScalableFloat);
    DEFINE_PROP(RespawnType, uint8);
    DEFINE_PROP(bAllowJoinInProgress, bool);
    DEFINE_PROP(GameData, TSoftObjectPtr<UCurveTable>);
    DEFINE_PROP(ResourceRates, TSoftObjectPtr<UCurveTable>);
    DEFINE_PROP(UIExtensions, TArray<FUIExtension>);
    DEFINE_PROP(GameplayTagContainer, FGameplayTagContainer);
    DEFINE_PROP(bSkipWarmup, bool);
    DEFINE_PROP(bSkipAircraft, bool);
    DEFINE_PROP(bUseDefaultSupplyDrops, bool);
    DEFINE_PROP(bForceRespawnLocationInsideOfVolume, bool);
    DEFINE_PROP(bIsLargeTeamGame, bool);
    DEFINE_PROP(ModifierList, TArray<TSoftObjectPtr<UFortGameplayModifierItemDefinition>>);
    DEFINE_PROP(PreloadPersistentLevel, TSoftObjectPtr<UWorld>);
    DEFINE_PROP(UIDisplayName, FText);
    DEFINE_PROP(JoinInProgressMatchType, FText);
    DEFINE_PROP(ScoringData, FWinConditionScoreData);
    DEFINE_PROP(bDisplayScoreInHUD, bool);
};
