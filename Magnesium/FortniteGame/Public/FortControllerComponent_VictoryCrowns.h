#pragma once
#include "../../pch.h"
#include "../Public/FortInventory.h"
#include "../../Engine/Public/DataTable.h"

class UFortControllerComponent_VictoryCrowns : public UFortControllerComponent
{
public:
    UCLASS_COMMON_MEMBERS(UFortControllerComponent_VictoryCrowns);

    DEFINE_PROP(OnHasWonCrownInMatch, TMulticastInlineDelegate<void()>);
    DEFINE_PROP(OnHasWonRoyalRoyale, TMulticastInlineDelegate<void()>);
    DEFINE_PROP(CrownInventoryItemClass, TSoftObjectPtr<UFortWorldItemDefinition>);
    DEFINE_PROP(VictoryCrownPlaylistData, UDataTable*);
    DEFINE_PROP(SourceTagsForRoyalRoyale, FGameplayTagContainer*);
	DEFINE_BITFIELD_PROP(bWonCrownInMatch);
	DEFINE_BITFIELD_PROP(bWonRoyalRoyale);

    DEFINE_FUNC(OnRep_WonCrownInMatch, void);
    DEFINE_FUNC(OnRep_WonRoyalRoyale, void);
    DEFINE_FUNC(GetCrownInPlayerInventory, UFortWorldItem*);
    DEFINE_FUNC(DebugForceSetRoyalRoyaleAchievedCount, void); // int32
    DEFINE_FUNC(AuthorityHasHeldCrownItem, bool); // UFortItem* CrownItem
    DEFINE_FUNC(HasWonCrownInMatch, bool);
    DEFINE_FUNC(HasWonRoyalRoyale, bool);
};

class UFortPlayerStateComponent_VictoryCrowns : public UActorComponent
{
public:
    UCLASS_COMMON_MEMBERS(UFortPlayerStateComponent_VictoryCrowns);

    DEFINE_BITFIELD_PROP(bHasWonRoyalRoyale);

    DEFINE_FUNC(GetWonRoyalRoyale, bool);
};
