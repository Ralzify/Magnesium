#include "pch.h"
#include "../Public/BuildingSMActor.h"
#include "../../Engine/Public/DataTableFunctionLibrary.h"
#include "../Public/FortGameStateAthena.h"
#include "../Public/FortGameMode.h"
#include "../Public/FortWeapon.h"
#include "../Public/FortKismetLibrary.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../../Engine/Public/AbilitySystemComponent.h"
#include "../../Erbium/Public/Configuration.h"

void ABuildingSMActor::OnDamageServer(ABuildingSMActor* Actor, float Damage,
    FGameplayTagContainer DamageTags, FVector Momentum, __int64 HitInfo, AActor* InstigatedBy,
    AActor* DamageCauser, __int64 EffectContext)
{
    auto GameState = ((AFortGameStateAthena*)UWorld::GetWorld()->GameState);
    auto GameMode = ((AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode);
    auto Controller = (AFortPlayerControllerAthena*)InstigatedBy;

    if (!InstigatedBy || !Actor->IsA<ABuildingSMActor>() || Actor->bPlayerPlaced ||
        Actor->GetHealth() == 1 || (Actor->HasbAllowResourceDrop() && !Actor->bAllowResourceDrop))
        return OnDamageServerOG(Actor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy,
            DamageCauser, EffectContext);
    if (!DamageCauser || !DamageCauser->IsA<AFortWeapon>() ||
        !((AFortWeapon*)DamageCauser)->WeaponData->IsA(UFortWeaponMeleeItemDefinition::StaticClass()))
        return OnDamageServerOG(Actor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy,
            DamageCauser, EffectContext);
    // Only player controllers have a world inventory here; AI controllers and pawns crashed below.
    if (!InstigatedBy->IsA<AFortPlayerControllerAthena>() || !Controller->WorldInventory)
        return OnDamageServerOG(Actor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy,
            DamageCauser, EffectContext);

    auto Resource = UFortKismetLibrary::K2_GetResourceItemDefinition(Actor->ResourceType);
    if (!Resource)
        return OnDamageServerOG(Actor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy,
            DamageCauser, EffectContext);
    auto MaxMat = Resource->GetMaxStackSize();

    auto Playlist = AFortGameMode::GetActivePlaylist(GameState);
    static auto GameData = Playlist ? Playlist->ResourceRates.Get() : nullptr;
    if (!GameData)
        GameData = FindObject<UCurveTable>(GameMode->HasWarmupRequiredPlayerCount() ? L"/Game/Athena/Balance/DataTables/AthenaResourceRates.AthenaResourceRates" : L"/Game/Balance/DataTables/ResourceRates.ResourceRates");

    int ResCount = 0;
    if (Actor->HasBuildingResourceAmountOverride())
    {
        FCurveTableRowHandle& BuildingResourceAmountOverride = Actor->BuildingResourceAmountOverride;

        if (BuildingResourceAmountOverride.RowName.ComparisonIndex > 0)
        {
            float Out = 0.f;
            UDataTableFunctionLibrary::EvaluateCurveTableRow(GameData,
                BuildingResourceAmountOverride.RowName, 0.f, nullptr, &Out, FString());

            float RC = Out / (Actor->GetMaxHealth() / Damage);

            ResCount = (int)round(RC);
        }
    }
    else
    {
        auto ClassData = Actor->GetClassData();
        FCurveTableRowHandle& BuildingResourceAmountOverride = ClassData->BuildingResourceAmountOverride;

        if (BuildingResourceAmountOverride.RowName.ComparisonIndex > 0)
        {
            float Out = 0.f;
            UDataTableFunctionLibrary::EvaluateCurveTableRow(GameData,
                BuildingResourceAmountOverride.RowName, 0.f, nullptr, &Out, FString());

            float RC = Out / (Actor->GetMaxHealth() / Damage);

            ResCount = (int)round(RC);
        }
    }

    if (ResCount > 0)
    {
        auto ItemP = Controller->WorldInventory->Inventory.ItemInstances.Search([&](UFortWorldItem* entry)
            {
                return entry->ItemEntry.ItemDefinition == Resource;
            });
        auto itemEntry = Controller->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
            {
                return entry.ItemDefinition == Resource;
            }, FFortItemEntry::Size());

        if (ItemP)
        {
            auto Item = *ItemP;

            itemEntry->Count += ResCount;
            if (itemEntry->Count > MaxMat)
            {
                AFortInventory::SpawnPickup(Controller->Pawn->K2_GetActorLocation(),
                    Item->ItemEntry.ItemDefinition, itemEntry->Count - MaxMat, 0,
                    EFortPickupSourceTypeFlag::GetTossed(), EFortPickupSpawnSource::GetUnset(),
                    Controller->MyFortPawn);
                itemEntry->Count = MaxMat;
            }

            for (int i = 0; i < itemEntry->StateValues.Num(); i++)
            {
                auto& StateValue = itemEntry->StateValues.Get(i, FFortItemEntryStateValue::Size());

                if (StateValue.StateType != 2)
                    continue;

                StateValue.IntValue = 0;
                break;
            }

            Item->ItemEntry.Count = itemEntry->Count;
            Controller->WorldInventory->UpdateEntry(*itemEntry);
            Item->ItemEntry.bIsDirty = true;
        }
        else
        {
            if (ResCount > MaxMat)
            {
                AFortInventory::SpawnPickup(Controller->Pawn->K2_GetActorLocation(), Resource,
                    ResCount - MaxMat, 0, EFortPickupSourceTypeFlag::GetTossed(),
                    EFortPickupSpawnSource::GetUnset(), Controller->MyFortPawn);
                ResCount = MaxMat;
            }

            Controller->WorldInventory->GiveItem(Resource, ResCount, 0, 0, false);
        }
    }

    if (ResCount > 0)
        Controller->ClientReportDamagedResourceBuilding(Actor,
            ResCount == 0 ? EFortResourceType(EFortResourceType__Enum::GetNone()) : Actor->ResourceType,
            ResCount, Actor->GetHealth() - Damage <= 0, Damage == 100.f);

    Actor->ForceNetUpdate();
    return OnDamageServerOG(Actor, Damage, DamageTags, Momentum, HitInfo, InstigatedBy,
        DamageCauser, EffectContext);
}

uint32 SpawnDecoVft = 0;
uint32 ShouldAllowServerSpawnDecoVft = 0;
struct FSavedTrapClassDefinition
{
    TWeakObjectPtr<UClass> TrapClass;
    TWeakObjectPtr<UFortDecoItemDefinition> ItemDefinition;
};
static std::unordered_map<UClass*, FSavedTrapClassDefinition> SavedTrapDefinitions;
struct FSavedTrapInstanceDefinition
{
    TWeakObjectPtr<ABuildingSMActor> TrapActor;
    TWeakObjectPtr<UClass> TrapClass;
    TWeakObjectPtr<UFortDecoItemDefinition> ItemDefinition;
};
static std::unordered_map<ABuildingSMActor*, FSavedTrapInstanceDefinition>
    SavedTrapInstanceDefinitions;
static SRWLOCK SavedTrapDefinitionsLock = SRWLOCK_INIT;

class FTrySavedTrapDefinitionsLock final
{
public:
    FTrySavedTrapDefinitionsLock() noexcept : bAcquired(TryAcquireSRWLockExclusive(
                &SavedTrapDefinitionsLock) != FALSE)
    {
    }

    ~FTrySavedTrapDefinitionsLock() noexcept
    {
        if (bAcquired)
            ReleaseSRWLockExclusive(&SavedTrapDefinitionsLock);
    }

    FTrySavedTrapDefinitionsLock(const FTrySavedTrapDefinitionsLock&) = delete;
    FTrySavedTrapDefinitionsLock& operator=(const FTrySavedTrapDefinitionsLock&) = delete;

    explicit operator bool() const noexcept
    {
        return bAcquired;
    }

private:
    bool bAcquired = false;
};

static bool IsValidTrapDefinition(UFortDecoItemDefinition* Definition)
{
    if (!Definition || !SDK::MemReadable(Definition, 0x40))
        return false;
    const int32 ObjectIndex = static_cast<int32>(Definition->Index);
    if (ObjectIndex < 0 || ObjectIndex >= TUObjectArray::Num() ||
        TUObjectArray::GetObjectByIndex(ObjectIndex) != Definition)
    {
        return false;
    }
    return Definition->Class && SDK::MemReadable(Definition->Class, 0x40) &&
        Definition->IsA<UFortDecoItemDefinition>();
}

static bool IsAllTeamPlayerJumpPadDefinition(UFortDecoItemDefinition* Definition)
{
    if (!IsValidTrapDefinition(Definition))
        return false;

    static const FName UpwardPad(L"TID_Floor_Player_Jump_Pad_Athena");
    static const FName FreeDirectionalPad(L"TID_Floor_Player_Jump_Pad_Free_Direction_Athena");
    return Definition->Name == UpwardPad || Definition->Name == FreeDirectionalPad;
}

static bool IsLiveJumpPadObject(const UObject* Object)
{
    if (!Object || !SDK::MemReadable(Object, 0x40))
        return false;
    const int32 ObjectIndex = static_cast<int32>(Object->Index);
    return ObjectIndex >= 0 && ObjectIndex < TUObjectArray::Num() &&
        TUObjectArray::GetObjectByIndex(ObjectIndex) == Object &&
        Object->Class && SDK::MemReadable(Object->Class, 0x40);
}

struct FJumpPadTargetingSchema
{
    const UStruct* EffectContainer = nullptr;
    const UStruct* TargetSelectionList = nullptr;
    const UStruct* TargetSelection = nullptr;
    const UStruct* TargetFilter = nullptr;
    int32 EffectContainerSize = 0;
    int32 TargetSelectionListSize = 0;
    int32 TargetSelectionSize = 0;
    int32 TargetFilterSize = 0;
    uint32 ContainerTargetSelectionOffset = 0;
    uint32 SelectionListArrayOffset = 0;
    uint32 SelectionTargetFilterOffset = 0;
    uint32 EnemyFilterOffset = 0;
    uint8 EnemyFilterMask = 0;

    bool IsValid() const
    {
        constexpr int32 kMaximumReflectedStructSize = 0x1000;
        return EffectContainer && TargetSelectionList && TargetSelection && TargetFilter &&
            EffectContainerSize > 0 && EffectContainerSize <= kMaximumReflectedStructSize &&
            TargetSelectionListSize >= sizeof(void*) + sizeof(int32) * 2 &&
            TargetSelectionListSize <= kMaximumReflectedStructSize &&
            TargetSelectionSize > 0 && TargetFilterSize > 0 &&
            TargetSelectionSize <= kMaximumReflectedStructSize &&
            TargetFilterSize <= kMaximumReflectedStructSize &&
            EffectContainerSize >= TargetSelectionListSize &&
            TargetSelectionSize >= TargetFilterSize && ContainerTargetSelectionOffset <=
                static_cast<uint32>(EffectContainerSize - TargetSelectionListSize) &&
            SelectionListArrayOffset <= static_cast<uint32>(TargetSelectionListSize -
                    (sizeof(void*) + sizeof(int32) * 2)) && SelectionTargetFilterOffset <=
                static_cast<uint32>(TargetSelectionSize - TargetFilterSize) &&
            EnemyFilterOffset < static_cast<uint32>(TargetFilterSize) && EnemyFilterMask != 0;
    }
};

static bool ReadStructPropertyLayout(const UStruct* Owner, const char* Name, uint64 PropertyFlags,
    uint32& OutOffset, uint32& OutElementSize, int32& OutArrayDimension, const UField*& OutProperty)
{
    OutOffset = static_cast<uint32>(-1);
    OutElementSize = 0;
    OutArrayDimension = 0;
    OutProperty = nullptr;
    if (!Owner || !Name || Offsets::ElementSize < sizeof(int32))
        return false;
    OutProperty = Owner->GetProperty(Name, PropertyFlags);
    if (!OutProperty || !SDK::MemReadable(OutProperty, static_cast<size_t>((std::max)(
                Offsets::Offset_Internal, Offsets::ElementSize)) + sizeof(uint32)))
    {
        return false;
    }
    OutOffset = SDK::ReadPropertyOffset(GetFromOffset<uint32>(
        OutProperty, Offsets::Offset_Internal));
    OutElementSize = GetFromOffset<uint32>(OutProperty, Offsets::ElementSize);
    OutArrayDimension = GetFromOffset<int32>(OutProperty, Offsets::ElementSize - sizeof(int32));
    return OutOffset != static_cast<uint32>(-1);
}

static FJumpPadTargetingSchema ResolveJumpPadTargetingSchema()
{
    FJumpPadTargetingSchema Result{};
    Result.EffectContainer = SDK::FindStruct("FortGameplayEffectContainer");
    Result.TargetSelectionList = SDK::FindStruct("FortAbilityTargetSelectionList");
    Result.TargetSelection = SDK::FindStruct("FortAbilityTargetSelection");
    Result.TargetFilter = SDK::FindStruct("FortTargetFilter");
    if (!Result.EffectContainer || !Result.TargetSelectionList ||
        !Result.TargetSelection || !Result.TargetFilter)
    {
        return {};
    }

    Result.EffectContainerSize = Result.EffectContainer->GetPropertiesSize();
    Result.TargetSelectionListSize = Result.TargetSelectionList->GetPropertiesSize();
    Result.TargetSelectionSize = Result.TargetSelection->GetPropertiesSize();
    Result.TargetFilterSize = Result.TargetFilter->GetPropertiesSize();
    uint32 ElementSize = 0;
    int32 ArrayDimension = 0;
    const UField* Property = nullptr;
    if (!ReadStructPropertyLayout(Result.EffectContainer, "TargetSelection", 0,
            Result.ContainerTargetSelectionOffset, ElementSize, ArrayDimension, Property) ||
        ElementSize != static_cast<uint32>(Result.TargetSelectionListSize) || ArrayDimension != 1 ||
        !ReadStructPropertyLayout(Result.TargetSelectionList, "List", 0,
            Result.SelectionListArrayOffset, ElementSize, ArrayDimension, Property) ||
        ElementSize != sizeof(void*) + sizeof(int32) * 2 || ArrayDimension != 1 ||
        !ReadStructPropertyLayout(Result.TargetSelection, "TargetFilter", 0,
            Result.SelectionTargetFilterOffset, ElementSize, ArrayDimension, Property) ||
        ElementSize != static_cast<uint32>(Result.TargetFilterSize) || ArrayDimension != 1 ||
        !ReadStructPropertyLayout(Result.TargetFilter, "bExcludePawnEnemies", 0x20000,
            Result.EnemyFilterOffset, ElementSize, ArrayDimension, Property) ||
        ElementSize != sizeof(uint8) || ArrayDimension != 1)
    {
        return {};
    }
    Result.EnemyFilterMask = Property->GetFieldMask();
    return Result.IsValid() ? Result : FJumpPadTargetingSchema{};
}

static const FJumpPadTargetingSchema& GetJumpPadTargetingSchema()
{
    static const FJumpPadTargetingSchema Schema = ResolveJumpPadTargetingSchema();
    return Schema;
}

static void PrewarmPlayerJumpPadReflection()
{
    (void)GetJumpPadTargetingSchema();
    (void)UClass::StaticClass();
    (void)UFortGameplayAbility::StaticClass();
    (void)UAbilitySystemComponent::StaticClass();
    (void)FGameplayAbilitySpec::Size();
}

struct FRawScriptArray
{
    void* Data = nullptr;
    int32 Num = 0;
    int32 Max = 0;
};

static bool IsReadableScriptArray(const FRawScriptArray& Array, int32 ElementSize,
    int32 MaximumElements)
{
    if (ElementSize <= 0 || MaximumElements <= 0 || Array.Num < 0 || Array.Num > MaximumElements ||
        Array.Max < Array.Num || Array.Max > MaximumElements * 4)
    {
        return false;
    }
    if (Array.Num == 0)
        return true;
    return Array.Data && SDK::MemReadable(Array.Data, static_cast<size_t>(Array.Num) *
            static_cast<size_t>(ElementSize));
}

static int32 ClearEnemyExclusionFromTargetSelectionList(uint8* SelectionListMemory,
    const FJumpPadTargetingSchema& Schema)
{
    if (!SelectionListMemory || !Schema.IsValid() || !SDK::MemReadable(SelectionListMemory,
            Schema.TargetSelectionListSize))
    {
        return 0;
    }

    FRawScriptArray Selections{};
    memcpy(&Selections, SelectionListMemory + Schema.SelectionListArrayOffset, sizeof(Selections));
    if (!IsReadableScriptArray(Selections, Schema.TargetSelectionSize, 64))
    {
        return 0;
    }

    int32 Changed = 0;
    for (int32 Index = 0; Index < Selections.Num; Index++)
    {
        auto EnemyFilterByte = reinterpret_cast<uint8*>(Selections.Data) +
            static_cast<size_t>(Index) * Schema.TargetSelectionSize +
            Schema.SelectionTargetFilterOffset + Schema.EnemyFilterOffset;
        if ((*EnemyFilterByte & Schema.EnemyFilterMask) == 0)
            continue;
        *EnemyFilterByte &= static_cast<uint8>(~Schema.EnemyFilterMask);
        ++Changed;
    }
    return Changed;
}

static int32 ClearEnemyExclusionFromEffectContainer(uint8* ContainerMemory,
    const FJumpPadTargetingSchema& Schema)
{
    if (!ContainerMemory || !Schema.IsValid() || !SDK::MemReadable(
            ContainerMemory, Schema.EffectContainerSize))
    {
        return 0;
    }
    return ClearEnemyExclusionFromTargetSelectionList(
        ContainerMemory + Schema.ContainerTargetSelectionOffset, Schema);
}

static bool ReadPropertyLayout(UObject* Owner, const char* Name, uint32& OutOffset,
    uint32& OutElementSize, int32& OutArrayDimension)
{
    OutOffset = static_cast<uint32>(-1);
    OutElementSize = 0;
    OutArrayDimension = 0;
    if (!IsLiveJumpPadObject(Owner) || !Name || Offsets::ElementSize < sizeof(int32))
    {
        return false;
    }

    auto Property = Owner->GetProperty(Name);
    if (!Property || !SDK::MemReadable(Property, static_cast<size_t>((std::max)(
                Offsets::Offset_Internal, Offsets::ElementSize)) + sizeof(uint32)))
    {
        return false;
    }

    OutOffset = SDK::ReadPropertyOffset(GetFromOffset<uint32>(Property, Offsets::Offset_Internal));
    OutElementSize = GetFromOffset<uint32>(Property, Offsets::ElementSize);
    OutArrayDimension = GetFromOffset<int32>(Property, Offsets::ElementSize - sizeof(int32));
    const int32 OwnerSize = Owner->Class->GetPropertiesSize();
    return OutOffset != static_cast<uint32>(-1) &&
        OwnerSize > 0 && OutOffset < static_cast<uint32>(OwnerSize);
}

static int32 ClearEnemyExclusionFromAbility(UFortGameplayAbility* Ability)
{
    if (!IsLiveJumpPadObject(Ability))
        return 0;
    const auto& Schema = GetJumpPadTargetingSchema();
    if (!Schema.IsValid())
        return 0;

    int32 Changed = 0;
    uint32 Offset = 0;
    uint32 ElementSize = 0;
    int32 ArrayDimension = 0;
    if (ReadPropertyLayout(Ability, "EffectContainers", Offset, ElementSize, ArrayDimension) &&
        ElementSize == static_cast<uint32>(Schema.EffectContainerSize) &&
        ArrayDimension > 0 && ArrayDimension <= 64 && static_cast<uint64>(Offset) +
            static_cast<uint64>(ElementSize) * ArrayDimension <=
            static_cast<uint64>(Ability->Class->GetPropertiesSize()))
    {
        for (int32 Index = 0; Index < ArrayDimension; Index++)
        {
            Changed += ClearEnemyExclusionFromEffectContainer(
                reinterpret_cast<uint8*>(Ability) + Offset +
                    static_cast<size_t>(Index) * ElementSize, Schema);
        }
    }

    if (ReadPropertyLayout(Ability, "GameplayEffectContainers", Offset,
            ElementSize, ArrayDimension) && ElementSize == sizeof(FRawScriptArray) &&
        ArrayDimension == 1 && Offset + sizeof(FRawScriptArray) <=
            static_cast<uint32>(Ability->Class->GetPropertiesSize()))
    {
        FRawScriptArray Containers{};
        memcpy(&Containers, reinterpret_cast<uint8*>(Ability) + Offset, sizeof(Containers));
        if (IsReadableScriptArray(Containers, Schema.EffectContainerSize, 64))
        {
            for (int32 Index = 0; Index < Containers.Num; Index++)
            {
                Changed += ClearEnemyExclusionFromEffectContainer(
                    reinterpret_cast<uint8*>(Containers.Data) + static_cast<size_t>(Index) *
                            Schema.EffectContainerSize, Schema);
            }
        }
    }
    return Changed;
}

static bool ReadObjectProperty(UObject* Owner, const char* Name, UObject*& OutObject)
{
    OutObject = nullptr;
    uint32 Offset = 0;
    uint32 ElementSize = 0;
    int32 ArrayDimension = 0;
    if (!ReadPropertyLayout(Owner, Name, Offset, ElementSize, ArrayDimension) ||
        ElementSize != sizeof(UObject*) || ArrayDimension != 1 || Offset + sizeof(UObject*) >
            static_cast<uint32>(Owner->Class->GetPropertiesSize()))
    {
        return false;
    }
    memcpy(&OutObject, reinterpret_cast<uint8*>(Owner) + Offset, sizeof(OutObject));
    return IsLiveJumpPadObject(OutObject);
}

static void AddUniqueJumpPadAbilityClass(std::vector<UClass*>& Classes, UClass* AbilityClass)
{
    if (!IsLiveJumpPadObject(AbilityClass) || !AbilityClass->IsA(UClass::StaticClass()) ||
        std::find(Classes.begin(), Classes.end(), AbilityClass) != Classes.end())
    {
        return;
    }
    Classes.push_back(AbilityClass);
}

static int32 PatchJumpPadAbilitySet(UObject* TrapTemplate, std::vector<UClass*>& AbilityClasses,
    const FName& ExpectedAbilityName)
{
    UObject* AbilitySet = nullptr;
    if (!ReadObjectProperty(TrapTemplate, "AbilitySet", AbilitySet))
    {
        return 0;
    }

    uint32 Offset = 0;
    uint32 ElementSize = 0;
    int32 ArrayDimension = 0;
    if (!ReadPropertyLayout(AbilitySet, "GameplayAbilities", Offset, ElementSize, ArrayDimension) ||
        ElementSize != sizeof(FRawScriptArray) || ArrayDimension != 1 ||
        Offset + sizeof(FRawScriptArray) >
            static_cast<uint32>(AbilitySet->Class->GetPropertiesSize()))
    {
        return 0;
    }

    FRawScriptArray Classes{};
    memcpy(&Classes, reinterpret_cast<uint8*>(AbilitySet) + Offset, sizeof(Classes));
    if (!IsReadableScriptArray(Classes, sizeof(UClass*), 64))
        return 0;

    int32 Changed = 0;
    for (int32 Index = 0; Index < Classes.Num; Index++)
    {
        UClass* AbilityClass = nullptr;
        memcpy(&AbilityClass, reinterpret_cast<uint8*>(Classes.Data) +
                static_cast<size_t>(Index) * sizeof(UClass*), sizeof(AbilityClass));
        if (!IsLiveJumpPadObject(AbilityClass) || AbilityClass->Name != ExpectedAbilityName)
        {
            continue;
        }
        const auto PreviousCount = AbilityClasses.size();
        AddUniqueJumpPadAbilityClass(AbilityClasses, AbilityClass);
        if (AbilityClasses.size() == PreviousCount)
            continue;
        auto AbilityDefault = AbilityClass->GetDefaultObj();
        if (IsLiveJumpPadObject(AbilityDefault) && AbilityDefault->IsA<UFortGameplayAbility>())
        {
            Changed += ClearEnemyExclusionFromAbility(
                static_cast<UFortGameplayAbility*>(AbilityDefault));
        }
    }
    return Changed;
}

static FName GetExpectedJumpPadAbilityName(UFortDecoItemDefinition* Definition)
{
    static const FName UpwardPad(L"TID_Floor_Player_Jump_Pad_Athena");
    static const FName UpwardAbility(L"GA_Trap_FloorJumpPad_C");
    static const FName DirectionalAbility(L"GA_Trap_FloorJumpPadDirectional_C");
    return Definition && Definition->Name == UpwardPad ? UpwardAbility : DirectionalAbility;
}

static bool IsMatchingJumpPadAbility(UFortGameplayAbility* Ability,
    const std::vector<UClass*>& AbilityClasses, const FName& ExpectedName)
{
    if (!IsLiveJumpPadObject(Ability) || !Ability->Class)
        return false;
    return std::find(AbilityClasses.begin(), AbilityClasses.end(),
        Ability->Class) != AbilityClasses.end() || Ability->Class->Name == ExpectedName;
}

static int32 PatchJumpPadAbilitySystem(ABuildingSMActor* TrapActor,
    const std::vector<UClass*>& AbilityClasses, const FName& ExpectedName)
{
    if (!IsLiveJumpPadObject(TrapActor))
        return 0;
    std::vector<UAbilitySystemComponent*> Components;
    for (const char* PropertyName : {
        "AbilitySystemComponent", "ReplicatedAbilitySystemComponent" })
    {
        UObject* ComponentObject = nullptr;
        if (!ReadObjectProperty(TrapActor, PropertyName, ComponentObject) ||
            !ComponentObject->IsA<UAbilitySystemComponent>())
        {
            continue;
        }
        auto Component = static_cast<UAbilitySystemComponent*>(ComponentObject);
        if (std::find(Components.begin(), Components.end(), Component) == Components.end())
        {
            Components.push_back(Component);
        }
    }

    int32 Changed = 0;
    for (auto Component : Components)
    {
        if (!Component->HasActivatableAbilities())
            continue;
        auto& Specs = Component->ActivatableAbilities.Items;
        const int32 SpecSize = FGameplayAbilitySpec::Size();
        if (SpecSize <= 0 || SpecSize > 0x400 || Specs.Num() < 0 || Specs.Num() > 512 ||
            (Specs.Num() > 0 && (!Specs.Data || !SDK::MemReadable(Specs.Data,
                    static_cast<size_t>(Specs.Num()) * SpecSize))))
        {
            continue;
        }

        for (int32 Index = 0; Index < Specs.Num(); Index++)
        {
            auto& Spec = Specs.Get(Index, SpecSize);
            auto PatchAbility = [&](UFortGameplayAbility* Ability)
            {
                if (IsMatchingJumpPadAbility(Ability, AbilityClasses, ExpectedName))
                {
                    Changed += ClearEnemyExclusionFromAbility(Ability);
                }
            };

            if (Spec.HasAbility())
                PatchAbility(Spec.Ability);
            if (Spec.HasReplicatedInstances())
            {
                auto& Instances = Spec.ReplicatedInstances;
                if (Instances.Num() >= 0 && Instances.Num() <= 64 && (Instances.Num() == 0 ||
                        (Instances.Data && SDK::MemReadable(Instances.Data,
                            static_cast<size_t>(Instances.Num()) * sizeof(UFortGameplayAbility*)))))
                {
                    for (auto Instance : Instances)
                        PatchAbility(Instance);
                }
            }
            if (Spec.HasNonReplicatedInstances())
            {
                auto& Instances = Spec.NonReplicatedInstances;
                if (Instances.Num() >= 0 && Instances.Num() <= 64 && (Instances.Num() == 0 ||
                        (Instances.Data && SDK::MemReadable(Instances.Data,
                            static_cast<size_t>(Instances.Num()) * sizeof(UFortGameplayAbility*)))))
                {
                    for (auto Instance : Instances)
                        PatchAbility(Instance);
                }
            }
        }
    }
    return Changed;
}

static void AllowAllTeamsOnPlayerJumpPad(ABuildingSMActor* TrapActor,
    UFortDecoItemDefinition* Definition)
{
    if (!IsAllTeamPlayerJumpPadDefinition(Definition) &&
        TrapActor && IsLiveJumpPadObject(TrapActor))
    {
        const uint8 AttachmentType = TrapActor->HasBuildingAttachmentType()
                ? TrapActor->BuildingAttachmentType : 0;
        Definition = ABuildingSMActor::ResolveTrapDefinitionForAttachment(
                Definition, AttachmentType, TrapActor->Class);
    }
    if (!IsAllTeamPlayerJumpPadDefinition(Definition))
        return;

    std::vector<UClass*> AbilityClasses;
    const FName ExpectedAbilityName = GetExpectedJumpPadAbilityName(Definition);
    auto TrapClass = TrapActor && IsLiveJumpPadObject(TrapActor) ? TrapActor->Class
        : static_cast<UClass*>(const_cast<UObject*>(Definition->BlueprintClass.WeakPtr.Get()));
    if (!IsLiveJumpPadObject(TrapClass) || !TrapClass->IsA(UClass::StaticClass()))
    {
        TrapClass = nullptr;
    }
    auto TrapDefaultObject = IsLiveJumpPadObject(TrapClass) ? TrapClass->GetDefaultObj() : nullptr;
    auto TrapDefault = IsLiveJumpPadObject(TrapDefaultObject) &&
        TrapDefaultObject->IsA<ABuildingSMActor>()
        ? static_cast<ABuildingSMActor*>(TrapDefaultObject) : nullptr;
    if (IsLiveJumpPadObject(TrapDefault))
    {
        PatchJumpPadAbilitySet(TrapDefault, AbilityClasses, ExpectedAbilityName);
    }
    if (AbilityClasses.empty() && TrapActor && TrapActor != TrapDefault)
    {
        PatchJumpPadAbilitySet(TrapActor, AbilityClasses, ExpectedAbilityName);
    }

    const auto& Schema = GetJumpPadTargetingSchema();
    auto ClearTrapFilter = [&](ABuildingSMActor* Target)
    {
        if (!IsLiveJumpPadObject(Target) || !Schema.IsValid())
        {
            return;
        }

        uint32 TriggerFilterOffset = 0;
        uint32 TriggerFilterElementSize = 0;
        int32 TriggerFilterArrayDimension = 0;
        if (!ReadPropertyLayout(Target, "TriggerFilter", TriggerFilterOffset,
                TriggerFilterElementSize, TriggerFilterArrayDimension) ||
            TriggerFilterElementSize != static_cast<uint32>(Schema.TargetFilterSize) ||
            TriggerFilterArrayDimension != 1)
        {
            return;
        }

        const int32 ActorSize = Target->Class->GetPropertiesSize();
        if (ActorSize <= 0 || Schema.TargetFilterSize <= 0 ||
            TriggerFilterOffset >= static_cast<uint32>(ActorSize) || Schema.EnemyFilterOffset >=
                static_cast<uint32>(Schema.TargetFilterSize) ||
            static_cast<uint64>(TriggerFilterOffset) +
                static_cast<uint64>(Schema.TargetFilterSize) > static_cast<uint64>(ActorSize))
        {
            return;
        }

        auto EnemyFilterByte = reinterpret_cast<uint8*>(Target) +
            TriggerFilterOffset + Schema.EnemyFilterOffset;
        if (!SDK::MemReadable(EnemyFilterByte, sizeof(*EnemyFilterByte)) ||
            (*EnemyFilterByte & Schema.EnemyFilterMask) == 0)
        {
            return;
        }

        *EnemyFilterByte &= static_cast<uint8>(~Schema.EnemyFilterMask);
    };
    ClearTrapFilter(TrapDefault);
    if (TrapActor != TrapDefault)
        ClearTrapFilter(TrapActor);

    if (TrapActor)
    {
        PatchJumpPadAbilitySystem(TrapActor, AbilityClasses, ExpectedAbilityName);
    }
}

static UFortDecoItemDefinition* RecoverContextTrapDefinition(
    UFortDecoItemDefinition* ConcreteDefinition, uint8 AttachmentType, UClass* ExpectedTrapClass);

void ABuildingSMActor::RegisterTrapDefinition(UClass* TrapClass,
    UFortDecoItemDefinition* ItemDefinition, ABuildingSMActor* TrapActor)
{
    if (TrapClass && ItemDefinition)
    {
        AllowAllTeamsOnPlayerJumpPad(TrapActor, ItemDefinition);

        FTrySavedTrapDefinitionsLock Lock;
        if (!Lock)
            return;
        SavedTrapDefinitions[TrapClass] = {
            TrapClass, ItemDefinition
        };
        if (TrapActor && SDK::MemReadable(TrapActor, 0x40))
        {
            SavedTrapInstanceDefinitions[TrapActor] = {
                TrapActor, TrapActor->Class, ItemDefinition
            };
        }
    }
}

UFortDecoItemDefinition* ABuildingSMActor::GetTrapDefinition(UClass* TrapClass)
{
    {
        FTrySavedTrapDefinitionsLock Lock;
        if (Lock)
        {
            auto Match = SavedTrapDefinitions.find(TrapClass);
            if (Match != SavedTrapDefinitions.end())
            {
                auto CachedClass = Match->second.TrapClass.Get();
                auto Definition = Match->second.ItemDefinition.Get();
                if (CachedClass == TrapClass && IsValidTrapDefinition(Definition))
                {
                    return Definition;
                }
                SavedTrapDefinitions.erase(Match);
            }
        }
    }

    for (int Index = 0; TrapClass && Index < TUObjectArray::Num(); ++Index)
    {
        auto Object = const_cast<UObject*>(TUObjectArray::GetObjectByIndex(Index));
        auto Definition = Object ? Object->Cast<UFortDecoItemDefinition>() : nullptr;
        if (Definition && Definition->BlueprintClass.WeakPtr.Get() == TrapClass)
        {
            RegisterTrapDefinition(TrapClass, Definition);
            return Definition;
        }
    }

    return nullptr;
}

UFortDecoItemDefinition* ABuildingSMActor::GetTrapDefinition(ABuildingSMActor* TrapActor)
{
    if (!TrapActor || !SDK::MemReadable(TrapActor, 0x40))
        return nullptr;

    if (auto BuildingTrap = TrapActor->Cast<ABuildingTrap>())
    {
        auto TrapData = BuildingTrap->HasTrapData()
            ? static_cast<UObject*>(BuildingTrap->TrapData) : nullptr;
        if (TrapData && TrapData->IsA<UFortDecoItemDefinition>())
        {
            auto Definition = static_cast<UFortDecoItemDefinition*>(TrapData);
            const uint8 AttachmentType = TrapActor->HasBuildingAttachmentType()
                    ? TrapActor->BuildingAttachmentType : 0;
            if (auto ContextDefinition = RecoverContextTrapDefinition(
                    Definition, AttachmentType, TrapActor->Class))
            {
                Definition = ContextDefinition;
            }
            RegisterTrapDefinition(TrapActor->Class, Definition, TrapActor);
            return Definition;
        }
    }

    {
        FTrySavedTrapDefinitionsLock Lock;
        if (Lock)
        {
            auto Match = SavedTrapInstanceDefinitions.find(TrapActor);
            if (Match != SavedTrapInstanceDefinitions.end())
            {
                const auto& Cached = Match->second;
                auto CachedActor = Cached.TrapActor.Get();
                auto CachedClass = Cached.TrapClass.Get();
                auto CachedDefinition = Cached.ItemDefinition.Get();
                if (CachedActor == TrapActor && CachedClass == TrapActor->Class &&
                    IsValidTrapDefinition(CachedDefinition))
                {
                    return CachedDefinition;
                }
                SavedTrapInstanceDefinitions.erase(Match);
            }
        }
    }

    return GetTrapDefinition(TrapActor->Class);
}

UFortDecoItemDefinition* ABuildingSMActor::ResolveTrapDefinitionForAttachment(
    UFortDecoItemDefinition* ItemDefinition, uint8 AttachmentType, UClass* ExpectedTrapClass)
{
    if (!IsValidTrapDefinition(ItemDefinition))
        return nullptr;

    auto ContextDefinition = ItemDefinition->Cast<UFortContextTrapItemDefinition>();
    if (!ContextDefinition)
        return ItemDefinition;

    auto AsTrapDefinition = [](UObject* Object)
    {
        return Object && Object->IsA<UFortDecoItemDefinition>()
            ? static_cast<UFortDecoItemDefinition*>(Object) : nullptr;
    };
    UFortDecoItemDefinition* Candidates[] = {
        AsTrapDefinition(ContextDefinition->HasFloorTrap()
            ? ContextDefinition->FloorTrap : nullptr),
        AsTrapDefinition(ContextDefinition->HasCeilingTrap()
            ? ContextDefinition->CeilingTrap : nullptr),
        AsTrapDefinition(ContextDefinition->HasWallTrap() ? ContextDefinition->WallTrap : nullptr),
        AsTrapDefinition(ContextDefinition->HasStairTrap() ? ContextDefinition->StairTrap : nullptr)
    };

    if (ExpectedTrapClass)
    {
        for (auto Candidate : Candidates)
        {
            if (Candidate && Candidate->BlueprintClass.WeakPtr.Get() == ExpectedTrapClass)
            {
                return Candidate;
            }
        }
    }

    switch (AttachmentType)
    {
    case 0:
    case 6:
        return Candidates[0];
    case 2:
    case 7:
        return Candidates[1];
    case 1:
        return Candidates[2];
    case 8:
        return Candidates[3];
    default:
        return nullptr;
    }
}

static UFortDecoItemDefinition* RecoverContextTrapDefinition(
    UFortDecoItemDefinition* ConcreteDefinition, uint8 AttachmentType, UClass* ExpectedTrapClass)
{
    if (!IsValidTrapDefinition(ConcreteDefinition))
        return nullptr;
    if (ConcreteDefinition->IsA<UFortContextTrapItemDefinition>())
        return ConcreteDefinition;

    UFortDecoItemDefinition* BestMatch = nullptr;
    int BestScore = -1;
    for (int Index = 0; Index < TUObjectArray::Num(); ++Index)
    {
        auto Object = const_cast<UObject*>(TUObjectArray::GetObjectByIndex(Index));
        auto Context = Object ? Object->Cast<UFortContextTrapItemDefinition>() : nullptr;
        if (!Context)
            continue;

        auto Source = reinterpret_cast<UFortDecoItemDefinition*>(Context);
        if (!IsValidTrapDefinition(Source))
            continue;
        auto Child = ABuildingSMActor::ResolveTrapDefinitionForAttachment(
                Source, AttachmentType, ExpectedTrapClass);
        if (!IsValidTrapDefinition(Child))
            continue;

        const bool bExactChild = Child == ConcreteDefinition;
        if (!bExactChild)
        {
            continue;
        }

        int Score = 1000;
        const std::string Name = Source->Name.ToString().c_str();
        if (Name.find("Athena") != std::string::npos)
            Score += 20;
        if (Name.find("Context") != std::string::npos)
            Score += 10;
        if (Score > BestScore)
        {
            BestScore = Score;
            BestMatch = Source;
        }
    }
    return BestMatch;
}

static bool ResolveDecoToolPlayer(AFortDecoTool* DecoTool, AFortPlayerPawnAthena*& Pawn,
    AFortPlayerControllerAthena*& PlayerController, AFortPlayerStateAthena*& PlayerState)
{
    Pawn = nullptr;
    PlayerController = nullptr;
    PlayerState = nullptr;

    if (!DecoTool)
        return false;

    auto Owner = DecoTool->Owner;
    if (Owner)
    {
        Pawn = Owner->Cast<AFortPlayerPawnAthena>();
        PlayerController = Owner->Cast<AFortPlayerControllerAthena>();
    }

    if (!Pawn && DecoTool->Instigator)
        Pawn = DecoTool->Instigator->Cast<AFortPlayerPawnAthena>();

    if (Pawn)
    {
        if (!PlayerController && Pawn->Controller)
            PlayerController = Pawn->Controller->Cast<AFortPlayerControllerAthena>();

        if (Pawn->PlayerState)
            PlayerState = Pawn->PlayerState->Cast<AFortPlayerStateAthena>();
    }

    if (PlayerController && !PlayerState)
        PlayerState = PlayerController->PlayerState;

    return PlayerController && PlayerState;
}

static bool ApplyTrapTeam(ABuildingSMActor* Trap, AFortPlayerStateAthena* PlayerState)
{
    if (!Trap || !PlayerState || !PlayerState->HasTeamIndex())
        return false;

    auto TeamIndex = PlayerState->TeamIndex;

    if (Trap->HasTeam())
        Trap->Team = TeamIndex;

    if (Trap->HasTeamIndex())
        Trap->TeamIndex = TeamIndex;

    if (Trap->HasOwnerPersistentID() && PlayerState->HasWorldPlayerId())
        Trap->OwnerPersistentID = PlayerState->WorldPlayerId;

    if (Trap->HasbPlayerPlaced())
        Trap->bPlayerPlaced = true;

    Trap->SetTeam(TeamIndex);
    Trap->ForceNetUpdate();
    return true;
}

static bool HasAttachedBuildingActor(const TArray<ABuildingSMActor*>& Actors,
    ABuildingSMActor* Actor)
{
    for (auto Candidate : Actors)
    {
        if (Candidate == Actor)
            return true;
    }
    return false;
}

struct FPendingSavedTrapAttachment
{
    TWeakObjectPtr<ABuildingSMActor> Trap;
    TWeakObjectPtr<ABuildingSMActor> Parent;
    uint8 AttachmentType = 0;
    int32 AttachmentSlot = -1;
    uint8 TicksRemaining = 0;
};

static std::vector<FPendingSavedTrapAttachment> PendingSavedTrapAttachments;

static bool EnsureSavedTrapAttachment(ABuildingSMActor* Trap, ABuildingSMActor* AttachedActor,
    uint8 AttachmentType, int32 AttachmentSlot)
{
    if (!Trap || !AttachedActor)
        return false;
    bool bChanged = false;

    if (Trap->HasBuildingAttachmentType() && Trap->BuildingAttachmentType != AttachmentType)
    {
        Trap->BuildingAttachmentType = AttachmentType;
        bChanged = true;
    }
    if (AttachmentSlot >= 0 && AttachmentSlot <= UINT8_MAX && Trap->HasBuildingAttachmentSlot() &&
        Trap->BuildingAttachmentSlot != static_cast<uint8>(AttachmentSlot))
    {
        Trap->BuildingAttachmentSlot = static_cast<uint8>(AttachmentSlot);
        bChanged = true;
    }

    bool bParentTracksTrap = AttachedActor->HasAttachedBuildingActors() && HasAttachedBuildingActor(
            AttachedActor->AttachedBuildingActors, Trap);
    bool bChildRelationObservable = false;
    bool bTrapTracksParent = false;
    if (Trap->HasParentActorToAttachTo())
    {
        bChildRelationObservable = true;
        bTrapTracksParent = Trap->ParentActorToAttachTo == AttachedActor;
    }

    auto BuildingTrap = Trap->Cast<ABuildingTrap>();
    if (BuildingTrap)
    {
        if (BuildingTrap->HasAttachedTo())
        {
            bChildRelationObservable = true;
            bTrapTracksParent = BuildingTrap->AttachedTo == AttachedActor;
        }
        else if (BuildingTrap->GetFunction("GetBuildingAttachedTo"))
        {
            bChildRelationObservable = true;
            bTrapTracksParent = BuildingTrap->GetBuildingAttachedTo() == AttachedActor;
        }
    }
    if ((!bParentTracksTrap || (bChildRelationObservable && !bTrapTracksParent)) &&
        AttachedActor->GetFunction("AttachBuildingActorToMe"))
    {
        AttachedActor->AttachBuildingActorToMe(Trap, true);
        bChanged = true;
    }

    if (Trap->HasParentActorToAttachTo() && Trap->ParentActorToAttachTo != AttachedActor)
    {
        Trap->ParentActorToAttachTo = AttachedActor;
        bChanged = true;
    }

    if (BuildingTrap)
    {
        bool bAttachedToMatches = false;
        bool bAttachedToObservable = false;
        if (BuildingTrap->HasAttachedTo())
        {
            bAttachedToObservable = true;
            bAttachedToMatches = BuildingTrap->AttachedTo == AttachedActor;
        }
        else if (BuildingTrap->GetFunction("GetBuildingAttachedTo"))
        {
            bAttachedToObservable = true;
            bAttachedToMatches = BuildingTrap->GetBuildingAttachedTo() == AttachedActor;
        }

        if ((!bAttachedToObservable || !bAttachedToMatches) &&
            BuildingTrap->GetFunction("SetAttachedTo"))
        {
            BuildingTrap->SetAttachedTo(AttachedActor);
            bChanged = true;
        }
        else if (BuildingTrap->HasAttachedTo() && !bAttachedToMatches)
        {
            BuildingTrap->AttachedTo = AttachedActor;
            bChanged = true;
        }
    }

    if (AttachedActor->HasAttachedBuildingActors() && !HasAttachedBuildingActor(
            AttachedActor->AttachedBuildingActors, Trap))
    {
        AttachedActor->AttachedBuildingActors.Add(Trap);
        bChanged = true;
    }
    if (Trap->HasBuildingActorsAttachedTo() && !HasAttachedBuildingActor(
            Trap->BuildingActorsAttachedTo, AttachedActor))
    {
        Trap->BuildingActorsAttachedTo.Add(AttachedActor);
        bChanged = true;
    }

    if (bChanged)
    {
        Trap->ForceNetUpdate();
        AttachedActor->ForceNetUpdate();
    }

    bParentTracksTrap = AttachedActor->HasAttachedBuildingActors() && HasAttachedBuildingActor(
            AttachedActor->AttachedBuildingActors, Trap);
    bChildRelationObservable = false;
    bTrapTracksParent = false;
    if (BuildingTrap && BuildingTrap->HasAttachedTo())
    {
        bChildRelationObservable = true;
        bTrapTracksParent = BuildingTrap->AttachedTo == AttachedActor;
    }
    else if (BuildingTrap && BuildingTrap->GetFunction("GetBuildingAttachedTo"))
    {
        bChildRelationObservable = true;
        bTrapTracksParent = BuildingTrap->GetBuildingAttachedTo() == AttachedActor;
    }
    else if (Trap->HasParentActorToAttachTo())
    {
        bChildRelationObservable = true;
        bTrapTracksParent = Trap->ParentActorToAttachTo == AttachedActor;
    }

    return bParentTracksTrap && bChildRelationObservable && bTrapTracksParent;
}

static void QueueSavedTrapAttachmentRepair(ABuildingSMActor* Trap, ABuildingSMActor* Parent,
    uint8 AttachmentType, int32 AttachmentSlot)
{
    if (!Trap || !Parent)
        return;
    for (auto& Pending : PendingSavedTrapAttachments)
    {
        if (Pending.Trap.Get() != Trap)
            continue;
        Pending.Parent = TWeakObjectPtr<ABuildingSMActor>(Parent);
        Pending.AttachmentType = AttachmentType;
        Pending.AttachmentSlot = AttachmentSlot;
        Pending.TicksRemaining = 16;
        return;
    }

    PendingSavedTrapAttachments.push_back({
        TWeakObjectPtr<ABuildingSMActor>(Trap), TWeakObjectPtr<ABuildingSMActor>(Parent),
        AttachmentType, AttachmentSlot, 16 });
}

void ABuildingSMActor::TickSavedTrapAttachments()
{
    for (size_t Index = 0;
        Index < PendingSavedTrapAttachments.size();)
    {
        auto& Pending = PendingSavedTrapAttachments[Index];
        auto Trap = Pending.Trap.Get();
        auto Parent = Pending.Parent.Get();
        if (!Trap || !Parent || Trap->bDestroyed || Parent->bDestroyed)
        {
            PendingSavedTrapAttachments.erase(PendingSavedTrapAttachments.begin() + Index);
            continue;
        }

        EnsureSavedTrapAttachment(Trap, Parent, Pending.AttachmentType, Pending.AttachmentSlot);
        if (--Pending.TicksRemaining == 0)
        {
            PendingSavedTrapAttachments.erase(PendingSavedTrapAttachments.begin() + Index);
            continue;
        }
        ++Index;
    }
}

static int GetAttachedBuildingActorCount(ABuildingSMActor* AttachedActor)
{
    if (!AttachedActor || !AttachedActor->HasAttachedBuildingActors())
        return 0;

    return AttachedActor->AttachedBuildingActors.Num();
}

static bool IsChapterFourDirectionalPad(UFortDecoItemDefinition* ItemDefinition,
    double* OutLocalLimit = nullptr)
{
    if (!ItemDefinition || VersionInfo.FortniteVersion < 27.0 ||
        VersionInfo.FortniteVersion >= 28.0)
    {
        return false;
    }

    static const FName UpwardPad(L"TID_Floor_Player_Jump_Pad_Athena");
    static const FName FreeDirectionalPad(L"TID_Floor_Player_Jump_Pad_Free_Direction_Athena");
    if (ItemDefinition->Name == UpwardPad)
    {
        if (OutLocalLimit)
            *OutLocalLimit = 256.0;
        return true;
    }
    if (ItemDefinition->Name == FreeDirectionalPad)
    {
        if (OutLocalLimit)
            *OutLocalLimit = 512.0;
        return true;
    }
    return false;
}

static void SnapChapterFourDirectionalPadToSupport(UFortDecoItemDefinition* ItemDefinition,
    FVector& Location, ABuildingSMActor*& AttachedActor, uint8 AttachmentType)
{
    double LocalLimit = 0.0;
    if (!IsChapterFourDirectionalPad(ItemDefinition, &LocalLimit) ||
        !AttachedActor || !SDK::MemReadable(AttachedActor, 0x40) ||
        AttachedActor->bActorIsBeingDestroyed || AttachedActor->bDestroyed ||
        !AttachedActor->HasBuildingType() || AttachedActor->BuildingType != 1 ||
        (AttachmentType != 0 && AttachmentType != 6))
    {
        return;
    }

    const auto SuppliedLocation = AttachedActor->K2_GetActorLocation();
    const auto SuppliedRotation = AttachedActor->K2_GetActorRotation();
    if (!std::isfinite(static_cast<double>(Location.X)) ||
        !std::isfinite(static_cast<double>(Location.Y)) ||
        !std::isfinite(static_cast<double>(SuppliedLocation.X)) ||
        !std::isfinite(static_cast<double>(SuppliedLocation.Y)) ||
        !std::isfinite(static_cast<double>(SuppliedRotation.Yaw)))
    {
        return;
    }
    const double DeltaX = static_cast<double>(Location.X - SuppliedLocation.X);
    const double DeltaY = static_cast<double>(Location.Y - SuppliedLocation.Y);
    const double YawRadians = static_cast<double>(SuppliedRotation.Yaw) *
        3.14159265358979323846 / 180.0;
    const double CosYaw = std::cos(YawRadians);
    const double SinYaw = std::sin(YawRadians);
    const double LocalX = CosYaw * DeltaX + SinYaw * DeltaY;
    const double LocalY = -SinYaw * DeltaX + CosYaw * DeltaY;
    const double SnappedLocalX = std::clamp(std::round(LocalX / 256.0) * 256.0,
        -LocalLimit, LocalLimit);
    const double SnappedLocalY = std::clamp(std::round(LocalY / 256.0) * 256.0,
        -LocalLimit, LocalLimit);

    Location.X = SuppliedLocation.X + CosYaw * SnappedLocalX - SinYaw * SnappedLocalY;
    Location.Y = SuppliedLocation.Y + SinYaw * SnappedLocalX + CosYaw * SnappedLocalY;
}

static bool ApplyTrapTeamToNewAttachments(ABuildingSMActor* AttachedActor, AFortPlayerStateAthena* PlayerState,
    int PreviousAttachedActorCount, UFortDecoItemDefinition* ItemDefinition = nullptr)
{
    if (!AttachedActor || !AttachedActor->HasAttachedBuildingActors())
        return false;

    auto& AttachedBuildingActors = AttachedActor->AttachedBuildingActors;
    bool bApplied = false;

    for (int i = PreviousAttachedActorCount; i < AttachedBuildingActors.Num(); i++)
    {
        auto AttachedBuildingActor = AttachedBuildingActors.Get(i);
        bApplied = ApplyTrapTeam(AttachedBuildingActor, PlayerState) || bApplied;
        if (AttachedBuildingActor && ItemDefinition)
            ABuildingSMActor::RegisterTrapDefinition(AttachedBuildingActor->Class, ItemDefinition,
                AttachedBuildingActor);
    }

    return bApplied;
}

static UFortDecoItemDefinition* ResolvePlacedDecoDefinition(AFortDecoTool* Tool, uint8 AttachmentType)
{
    if (!Tool)
        return nullptr;
    UObject* Selected = Tool->ItemDefinition;
    if (auto ContextTool = Tool->Cast<AFortDecoTool_ContextTrap>())
    {
        auto ContextDefinition = ContextTool->ContextTrapItemDefinition;
        if (!ContextDefinition || !SDK::MemReadable(ContextDefinition, sizeof(void*)))
            return nullptr;
        switch (AttachmentType)
        {
        case 0: case 6: Selected = ContextDefinition->FloorTrap; break;
        case 2: case 7: Selected = ContextDefinition->CeilingTrap; break;
        case 1: Selected = ContextDefinition->WallTrap; break;
        case 8: Selected = ContextDefinition->StairTrap; break;
        default: return nullptr;
        }
    }
    if (!Selected || !SDK::MemReadable(Selected, 0x40) || !Selected->Class ||
        !SDK::MemReadable(Selected->Class, 0x40) || !Selected->IsA<UFortDecoItemDefinition>())
        return nullptr;
    return static_cast<UFortDecoItemDefinition*>(Selected);
}

struct FLegacyTrapStackSnapshot
{
    FGuid ItemGuid{};
    int32 Count = 0;
};

class FLegacyTrapPlacementPawnScope final
{
public:
    explicit FLegacyTrapPlacementPawnScope(AFortPlayerPawnAthena* InPawn) : Pawn(InPawn)
    {
        if (!Pawn)
            return;
        OriginalLocation = Pawn->K2_GetActorLocation();
        if (Pawn->CharacterMovement)
        {
            OriginalVelocity = Pawn->CharacterMovement->Velocity;
            bHasVelocity = true;
        }
    }

    ~FLegacyTrapPlacementPawnScope()
    {
        if (!Pawn || !bRelocationAttempted)
            return;
        Pawn->K2_SetActorLocation(OriginalLocation, false, nullptr, true);
        if (bHasVelocity && Pawn->CharacterMovement)
            Pawn->CharacterMovement->Velocity = OriginalVelocity;
    }

    FLegacyTrapPlacementPawnScope(const FLegacyTrapPlacementPawnScope&) = delete;
    FLegacyTrapPlacementPawnScope& operator=(const FLegacyTrapPlacementPawnScope&) = delete;

    bool MoveWithinNativeRange(const FVector& TrapLocation)
    {
        if (!Pawn)
            return false;

        auto IsWithinRange = [&](const FVector& Location)
        {
            const auto Delta = Location - TrapLocation;
            return Delta.X * Delta.X + Delta.Y * Delta.Y + Delta.Z * Delta.Z <= 1500.0 * 1500.0;
        };
        if (IsWithinRange(OriginalLocation))
            return true;

        FVector NearbyLocation = TrapLocation;
        NearbyLocation.Z += 512.0;
        bRelocationAttempted = true;
        Pawn->K2_SetActorLocation(NearbyLocation, false, nullptr, true);
        bool bWithinRange = IsWithinRange(Pawn->K2_GetActorLocation());
        if (!bWithinRange)
        {
            bWithinRange = Pawn->K2_TeleportTo(NearbyLocation, Pawn->K2_GetActorRotation()) &&
                IsWithinRange(Pawn->K2_GetActorLocation());
        }
        return bWithinRange;
    }

private:
    AFortPlayerPawnAthena* Pawn = nullptr;
    FVector OriginalLocation{};
    FVector OriginalVelocity{};
    bool bHasVelocity = false;
    bool bRelocationAttempted = false;
};

static bool AreItemGuidsEqual(const FGuid& Left, const FGuid& Right)
{
    return Left.A == Right.A && Left.B == Right.B && Left.C == Right.C && Left.D == Right.D;
}

static UFortWorldItem* FindInventoryItemInstance(AFortInventory* Inventory, const FGuid& ItemGuid)
{
    if (!Inventory)
        return nullptr;

    auto Item = Inventory->Inventory.ItemInstances.Search([&](UFortWorldItem* Candidate)
        {
            return Candidate && AreItemGuidsEqual(Candidate->ItemEntry.ItemGuid, ItemGuid);
        });
    return Item ? *Item : nullptr;
}

static std::vector<FLegacyTrapStackSnapshot> ProtectLegacyTrapStacks(AFortDecoTool* Tool,
        AFortPlayerControllerAthena* PlayerController, uint8 AttachmentType,
        bool bForceProtection = false)
{
    std::vector<FLegacyTrapStackSnapshot> Snapshots;
    if (!Tool || !PlayerController || !PlayerController->WorldInventory || (!bForceProtection &&
            !AFortInventory::ShouldBypassItemConsumption(PlayerController, 1, false)))
    {
        return Snapshots;
    }

    auto Inventory = PlayerController->WorldInventory;
    const auto ToolDefinition = reinterpret_cast<const UFortItemDefinition*>(Tool->ItemDefinition);
    const auto PlacedDefinition = ResolvePlacedDecoDefinition(Tool, AttachmentType);
    auto& Entries = Inventory->Inventory.ReplicatedEntries;
    Snapshots.reserve(Entries.Num());

    for (int32 Index = 0; Index < Entries.Num(); Index++)
    {
        auto& Entry = Entries.Get(Index, FFortItemEntry::Size());
        if (Entry.Count <= 0 || (Entry.ItemDefinition != ToolDefinition &&
                Entry.ItemDefinition != PlacedDefinition))
        {
            continue;
        }

        Snapshots.push_back(
            { Entry.ItemGuid, Entry.Count });

        if (Entry.Count == 1)
        {
            Entry.Count = 2;
            if (auto Item = FindInventoryItemInstance(Inventory, Entry.ItemGuid))
            {
                Item->ItemEntry.Count = 2;
                Item->ItemEntry.bIsDirty = true;
            }
            Inventory->UpdateEntry(Entry);
        }
    }

    return Snapshots;
}

static void RestoreLegacyTrapStacks(AFortInventory* Inventory,
    const std::vector<FLegacyTrapStackSnapshot>& Snapshots)
{
    if (!Inventory)
        return;

    for (const auto& Snapshot : Snapshots)
    {
        auto Entry = Inventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& Candidate)
                {
                    return AreItemGuidsEqual(Candidate.ItemGuid, Snapshot.ItemGuid);
                }, FFortItemEntry::Size());
        if (!Entry)
            continue;

        bool bChanged = Entry->Count != Snapshot.Count;
        int32 RestoredCount = Snapshot.Count;
        Entry->Count = RestoredCount;
        if (auto Item = FindInventoryItemInstance(Inventory, Snapshot.ItemGuid))
        {
            if (Item->ItemEntry.Count != Snapshot.Count)
            {
                Item->ItemEntry.Count = RestoredCount;
                Item->ItemEntry.bIsDirty = true;
                bChanged = true;
            }
        }
        if (bChanged)
            Inventory->UpdateEntry(*Entry);
    }
}

static bool VerifySavedTrapLevel(ABuildingSMActor* Trap, int32 SavedTrapLevel,
    int32 SavedOriginalTrapLevel)
{
    if (SavedTrapLevel < 0 && SavedOriginalTrapLevel < 0)
        return true;

    auto BuildingTrap = Trap ? Trap->Cast<ABuildingTrap>() : nullptr;
    if (!BuildingTrap)
        return false;

    if (SavedTrapLevel >= 0)
    {
        if (BuildingTrap->HasTrapLevel())
        {
            if (BuildingTrap->TrapLevel != SavedTrapLevel)
                return false;
        }
        else if (BuildingTrap->GetFunction("GetTrapLevel"))
        {
            if (BuildingTrap->GetTrapLevel() != SavedTrapLevel)
                return false;
        }
        else
            return false;
    }
    if (SavedOriginalTrapLevel >= 0)
    {
        if (!BuildingTrap->HasOriginalTrapLevel() ||
            BuildingTrap->OriginalTrapLevel != SavedOriginalTrapLevel)
        {
            return false;
        }
    }
    return true;
}

static bool ZeroSavedTrapDurabilityCostOnSet(UObject* AttributeSet)
{
    if (!AttributeSet || !SDK::MemReadable(AttributeSet, 0x40) || !AttributeSet->Class ||
        !SDK::MemReadable(AttributeSet->Class, 0x40) ||
        !FFortGameplayAttributeData::StaticStruct() ||
        !FFortGameplayAttributeData::HasBaseValue() ||
        !FFortGameplayAttributeData::HasCurrentValue())
    {
        return false;
    }

    const uint32 CostOffset = AttributeSet->GetOffset("DurabilityCostPerFire");
    const int32 AttributeSize = FFortGameplayAttributeData::Size();
    if (CostOffset == UINT32_MAX || CostOffset > 0x10000 ||
        AttributeSize <= 0 || AttributeSize > 0x400)
    {
        return false;
    }

    const int32 SetSize = AttributeSet->Class->GetPropertiesSize();
    if (SetSize > 0 && SetSize <= 0x100000 && (static_cast<uint64>(CostOffset) +
            static_cast<uint64>(AttributeSize) > static_cast<uint64>(SetSize)))
    {
        return false;
    }

    auto Cost = reinterpret_cast<FFortGameplayAttributeData*>(
        reinterpret_cast<uint8*>(AttributeSet) + CostOffset);
    if (!SDK::MemReadable(Cost, AttributeSize))
        return false;

    AFortPlayerPawnAthena::WriteDirectAttributeValue(*Cost, 0.0f);
    return true;
}

static void ApplySavedTrapInfiniteDurability(ABuildingSMActor* Trap)
{
    if (!Trap)
        return;

    UObject* AuthoritativeSet = Trap->HasBuildingAttributeSet()
            ? Trap->BuildingAttributeSet : nullptr;
    UObject* ReplicatedSet = Trap->HasReplicatedBuildingAttributeSet()
            ? Trap->ReplicatedBuildingAttributeSet : nullptr;
    bool bChanged = ZeroSavedTrapDurabilityCostOnSet(AuthoritativeSet);
    if (ReplicatedSet && ReplicatedSet != AuthoritativeSet)
    {
        bChanged = ZeroSavedTrapDurabilityCostOnSet(ReplicatedSet) || bChanged;
    }

    if (!bChanged)
        return;

    for (auto AttributeSet : { AuthoritativeSet, ReplicatedSet })
    {
        if (!AttributeSet)
            continue;
        auto OnRepDurability = AttributeSet->GetFunction("OnRep_Durability");
        if (OnRepDurability && OnRepDurability->GetPropertiesSize() == 0)
        {
            AttributeSet->ProcessEvent(OnRepDurability, nullptr);
        }
        if (ReplicatedSet == AuthoritativeSet)
            break;
    }
    Trap->ForceNetUpdate();
}

static ABuildingSMActor* FinalizeSavedTrapInstance(ABuildingSMActor* Trap, UClass* TrapClass,
    ABuildingSMActor* AttachedActor, uint8 AttachmentType, int32 AttachmentSlot,
    int32 SavedTrapLevel, int32 SavedOriginalTrapLevel, UFortDecoItemDefinition* ItemDefinition,
    UFortContextTrapItemDefinition* ContextItemDefinition,
    AFortPlayerControllerAthena* PlayerController, bool bDestroyInvalid)
{
    if (!Trap || !SDK::MemReadable(Trap, 0x40) || !Trap->Class ||
        !SDK::MemReadable(Trap->Class, 0x40))
    {
        return nullptr;
    }
    if (Trap->Class != TrapClass)
    {
        if (bDestroyInvalid)
            Trap->SilentDie(true);
        return nullptr;
    }

    if (ContextItemDefinition && ContextItemDefinition->IsA(UFortTrapItemDefinition::StaticClass()))
    {
        if (auto BuildingTrap = Trap->Cast<ABuildingTrap>();
            BuildingTrap && BuildingTrap->HasTrapData())
        {
            auto ContextTrapDefinition = reinterpret_cast<UFortTrapItemDefinition*>(
                    ContextItemDefinition);
            if (BuildingTrap->TrapData != ContextTrapDefinition)
            {
                BuildingTrap->TrapData = ContextTrapDefinition;
                Trap->ForceNetUpdate();
            }
        }
    }

    const bool bLevelMetadataMatches = VerifySavedTrapLevel(
        Trap, SavedTrapLevel, SavedOriginalTrapLevel);
    const bool bAttachmentRestored = EnsureSavedTrapAttachment(
        Trap, AttachedActor, AttachmentType, AttachmentSlot);
    if (!bAttachmentRestored || (VersionInfo.FortniteVersion < 18 && !bLevelMetadataMatches))
    {
        if (bDestroyInvalid)
            Trap->SilentDie(true);
        return nullptr;
    }
    if (!bLevelMetadataMatches)
    {
        SDK::DbgLog("[TrickshotTrap] accepted modern level normalization "
            "trap=%p savedLevel=%d savedOriginal=%d\n",
            (void*)Trap, SavedTrapLevel, SavedOriginalTrapLevel);
    }
    ApplyTrapTeam(Trap, static_cast<AFortPlayerStateAthena*>(PlayerController->PlayerState));
    ApplySavedTrapInfiniteDurability(Trap);
    QueueSavedTrapAttachmentRepair(Trap, AttachedActor, AttachmentType, AttachmentSlot);
    ABuildingSMActor::RegisterTrapDefinition(TrapClass, ItemDefinition, Trap);
    return Trap;
}

static bool IsExcludedSavedTrapActor(ABuildingSMActor* Candidate,
    const std::vector<TWeakObjectPtr<ABuildingSMActor>>* ExcludedActors,
    const std::vector<TWeakObjectPtr<ABuildingSMActor>>* BaselineActors)
{
    if (!Candidate)
        return false;
    auto Contains = [&](const auto* Actors)
    {
        if (!Actors)
            return false;
        for (const auto& Actor : *Actors)
        {
            if (Actor.Get() == Candidate)
                return true;
        }
        return false;
    };
    return Contains(ExcludedActors) || Contains(BaselineActors);
}

ABuildingSMActor* ABuildingSMActor::SpawnSavedTrap(UClass* TrapClass, const FVector& SavedLocation, const FRotator& SavedRotation,
    ABuildingSMActor* AttachedActor, uint8 AttachmentType, AFortPlayerControllerAthena* PlayerController,
    const wchar_t* ItemDefinitionPath, int32 AttachmentSlot,
    int32 SavedTrapLevel, int32 SavedOriginalTrapLevel,
    UFortDecoItemDefinition* ResolvedItemDefinition, bool bRecoverDeferredPlacement,
    bool bFinalPlacementSweep, const std::vector<TWeakObjectPtr<ABuildingSMActor>>* ExcludedActors,
    const std::vector<TWeakObjectPtr<ABuildingSMActor>>* BaselineActors)
{
    if (!TrapClass || !AttachedActor || !PlayerController || !PlayerController->PlayerState ||
        (VersionInfo.FortniteVersion >= 18 && !SpawnDecoVft))
        return nullptr;

    UObject* SavedItemObject = ResolvedItemDefinition;
    if (!SavedItemObject && ItemDefinitionPath && *ItemDefinitionPath)
    {
        SavedItemObject = (UObject*)SDK::StaticFindObject(
            ItemDefinitionPath, UObject::StaticClass());
    }
    UFortDecoItemDefinition* ItemDefinition = SavedItemObject && SavedItemObject->IsA<UFortDecoItemDefinition>()
        ? (UFortDecoItemDefinition*)SavedItemObject : nullptr;
    SavedItemObject = ItemDefinition;
    if (!ItemDefinition)
        ItemDefinition = GetTrapDefinition(TrapClass);
    if (!ItemDefinition)
        return nullptr;
    auto ConcreteItemDefinition = ResolveTrapDefinitionForAttachment(
            ItemDefinition, AttachmentType, TrapClass);
    if (!ConcreteItemDefinition)
        return nullptr;
    AllowAllTeamsOnPlayerJumpPad(nullptr, ConcreteItemDefinition);
    if (auto ContextDefinition = RecoverContextTrapDefinition(
            ConcreteItemDefinition, AttachmentType, TrapClass))
    {
        ItemDefinition = ContextDefinition;
    }
    auto ResidentDefinitionClass = static_cast<UClass*>(const_cast<UObject*>(
            ConcreteItemDefinition->BlueprintClass.WeakPtr.Get()));
    if (ResidentDefinitionClass && ResidentDefinitionClass != TrapClass)
    {
        return nullptr;
    }

    auto ContextItemDefinition = ItemDefinition->Cast<UFortContextTrapItemDefinition>();

    if (bRecoverDeferredPlacement && AttachedActor->HasAttachedBuildingActors())
    {
        FVector ExpectedDeferredLocation = SavedLocation;
        auto ExpectedDeferredParent = AttachedActor;
        SnapChapterFourDirectionalPadToSupport(ConcreteItemDefinition, ExpectedDeferredLocation,
            ExpectedDeferredParent, AttachmentType);
        std::vector<ABuildingSMActor*> MatchingDeferredChildren;
        for (auto Candidate : AttachedActor->AttachedBuildingActors)
        {
            if (!Candidate || Candidate->Class != TrapClass || IsExcludedSavedTrapActor(
                    Candidate, ExcludedActors, BaselineActors) || Candidate->bDestroyed ||
                (Candidate->HasbActorIsBeingDestroyed() && Candidate->bActorIsBeingDestroyed))
            {
                continue;
            }
            const auto CandidateLocation = Candidate->K2_GetActorLocation();
            const auto Delta = CandidateLocation - ExpectedDeferredLocation;
            constexpr double kDeferredTrapLocationTolerance = 64.0;
            if (Delta.X * Delta.X + Delta.Y * Delta.Y + Delta.Z * Delta.Z >
                kDeferredTrapLocationTolerance * kDeferredTrapLocationTolerance)
            {
                continue;
            }

            MatchingDeferredChildren.push_back(Candidate);
        }
        // SilentDie can remove a trap from the parent's array, so iterate a stable copy.
        ABuildingSMActor* Recovered = nullptr;
        for (auto Candidate : MatchingDeferredChildren)
        {
            if (Recovered)
                continue;
            Recovered = FinalizeSavedTrapInstance(Candidate, TrapClass, AttachedActor,
                    AttachmentType, AttachmentSlot, SavedTrapLevel, SavedOriginalTrapLevel,
                    ItemDefinition, ContextItemDefinition, PlayerController, bFinalPlacementSweep);
        }
        if (Recovered)
        {
            size_t RemovedDuplicates = 0;
            for (auto Candidate : MatchingDeferredChildren)
            {
                if (Candidate == Recovered || !Candidate || Candidate->bDestroyed ||
                    (Candidate->HasbActorIsBeingDestroyed() && Candidate->bActorIsBeingDestroyed))
                {
                    continue;
                }
                Candidate->SilentDie(true);
                ++RemovedDuplicates;
            }
            SDK::DbgLog(
                "[TrickshotTrap] recovered deferred legacy placement trap=%p parent=%p duplicates=%zu\n",
                (void*)Recovered, (void*)AttachedActor, RemovedDuplicates);
            return Recovered;
        }
        if (!MatchingDeferredChildren.empty())
        {
            if (bFinalPlacementSweep)
            {
                SDK::DbgLog(
                    "[TrickshotTrap] removed invalid deferred placement on final sweep parent=%p class=%p\n",
                    (void*)AttachedActor, (void*)TrapClass);
            }
            else
            {
                SDK::DbgLog(
                    "[TrickshotTrap] deferred legacy placement still initializing parent=%p class=%p\n",
                    (void*)AttachedActor, (void*)TrapClass);
            }
            return nullptr;
        }
    }
    if (bFinalPlacementSweep)
        return nullptr;

    auto TrapToolClass = ContextItemDefinition ? FindClass("FortDecoTool_ContextTrap") : nullptr;
    const bool bUsingContextTool = TrapToolClass != nullptr;
    if (!TrapToolClass)
        TrapToolClass = FindClass("FortTrapTool");
    if (!TrapToolClass)
        return nullptr;

    auto Tool = UWorld::SpawnActor<AFortDecoTool>(TrapToolClass, FVector{}, FRotator{}, PlayerController);
    if (!Tool)
        return nullptr;

    SavedItemObject = bUsingContextTool ? static_cast<UObject*>(ItemDefinition)
        : static_cast<UObject*>(ConcreteItemDefinition);
    int32 PlacementTrapLevel = SavedOriginalTrapLevel >= 0 ? SavedOriginalTrapLevel
            : SavedTrapLevel >= 0 ? SavedTrapLevel : 0;
    Tool->ItemDefinition = SavedItemObject;
    Tool->Owner = PlayerController->MyFortPawn ? (AActor*)PlayerController->MyFortPawn : (AActor*)PlayerController;
    Tool->Instigator = PlayerController->MyFortPawn;
    auto WeaponTool = Tool->Cast<AFortWeapon>();
    if (WeaponTool && WeaponTool->HasWeaponData())
    {
        WeaponTool->WeaponData = reinterpret_cast<UFortWeaponItemDefinition*>(SavedItemObject);
    }
    else if (VersionInfo.FortniteVersion < 18)
    {
        Tool->K2_DestroyActor();
        return nullptr;
    }
    if (WeaponTool && WeaponTool->HasWeaponLevel())
        WeaponTool->WeaponLevel = PlacementTrapLevel;
    if (bUsingContextTool)
    {
        auto ContextTool = Tool->Cast<AFortDecoTool_ContextTrap>();
        if (!ContextTool)
        {
            Tool->K2_DestroyActor();
            return nullptr;
        }
        if (ContextTool->HasContextTrapItemDefinition())
            ContextTool->ContextTrapItemDefinition = ContextItemDefinition;
        if (ContextTool->GetFunction("SetContextTrapItemDefinition"))
            ContextTool->SetContextTrapItemDefinition(ContextItemDefinition);
    }

    FVector Location = SavedLocation;
    FRotator Rotation = SavedRotation;
    SnapChapterFourDirectionalPadToSupport(ConcreteItemDefinition, Location, AttachedActor,
        AttachmentType);
    ABuildingSMActor* Trap = nullptr;
    if (VersionInfo.FortniteVersion < 18)
    {
        // Legacy SpawnDeco vtable layouts are not stable, so let ProcessEvent marshal the real signature.
        if (SavedItemObject && SavedItemObject->IsA<UFortItemDefinition>() &&
            PlayerController->WorldInventory)
        {
            auto Inventory = PlayerController->WorldInventory;
            auto ProtectedStacks = ProtectLegacyTrapStacks(
                Tool, PlayerController, AttachmentType, true);
            auto PlacementItem = Inventory->GiveItem((UFortItemDefinition*)SavedItemObject, 1);
            FGuid PlacementItemGuid{};
            int32 OriginalPlacementItemCount = 0;
            const bool bHasPlacementItem = PlacementItem != nullptr;
            if (bHasPlacementItem)
            {
                PlacementItemGuid = PlacementItem->ItemEntry.ItemGuid;
                PlacementItem->ItemEntry.Level = PlacementTrapLevel;
                PlacementItem->ItemEntry.bIsDirty = true;
                if (auto PlacementEntry = Inventory->Inventory.ReplicatedEntries.Search(
                        [&](FFortItemEntry& Candidate)
                        {
                            return AreItemGuidsEqual(Candidate.ItemGuid, PlacementItemGuid);
                        }, FFortItemEntry::Size()))
                {
                    PlacementEntry->Level = PlacementTrapLevel;
                }
                OriginalPlacementItemCount = (std::max)(0, PlacementItem->ItemEntry.Count - 1);
                if (WeaponTool && WeaponTool->HasItemEntryGuid())
                {
                    WeaponTool->ItemEntryGuid = PlacementItemGuid;
                }
            }
            const int PreviousCount = GetAttachedBuildingActorCount(AttachedActor);
            {
                FLegacyTrapPlacementPawnScope PawnPlacement(PlayerController->MyFortPawn);
                if (PawnPlacement.MoveWithinNativeRange(Location))
                {
                    Tool->ServerSpawnDeco(Location, Rotation, AttachedActor, AttachmentType);
                }
            }
            if (bHasPlacementItem)
            {
                auto Entry = Inventory->Inventory.ReplicatedEntries.Search(
                    [&](FFortItemEntry& Candidate)
                    {
                        return AreItemGuidsEqual(Candidate.ItemGuid, PlacementItemGuid);
                    }, FFortItemEntry::Size());
                if (OriginalPlacementItemCount == 0)
                {
                    if (Entry || FindInventoryItemInstance(Inventory, PlacementItemGuid))
                    {
                        Inventory->Remove(PlacementItemGuid);
                    }
                }
                else if (Entry)
                {
                    Entry->Count = OriginalPlacementItemCount;
                    if (auto Item = FindInventoryItemInstance(Inventory, PlacementItemGuid))
                    {
                        Item->ItemEntry.Count = OriginalPlacementItemCount;
                        Item->ItemEntry.bIsDirty = true;
                    }
                    Inventory->UpdateEntry(*Entry);
                }
            }
            RestoreLegacyTrapStacks(Inventory, ProtectedStacks);
            if (AttachedActor->HasAttachedBuildingActors() && AttachedActor->AttachedBuildingActors.Num() > PreviousCount)
                Trap = AttachedActor->AttachedBuildingActors.Get(PreviousCount);
        }
    }
    else if (VersionInfo.FortniteVersion >= 27)
    {
        auto SpawnDeco = (ABuildingSMActor * (*)(AFortDecoTool*, TSubclassOf<ABuildingSMActor>&,
            FVector&, FRotator&, ABuildingSMActor*, uint8_t, int))Tool->Vft[SpawnDecoVft];
        TSubclassOf<ABuildingSMActor> TrapSubclass;
        TrapSubclass.ClassPtr = TrapClass;
        if (SpawnDeco)
            Trap = SpawnDeco(Tool, TrapSubclass, Location, Rotation, AttachedActor, AttachmentType, 0);
    }
    else
    {
        auto SpawnDeco = (ABuildingSMActor * (*)(AFortDecoTool*, UClass*, FVector&, FRotator&,
            ABuildingSMActor*, uint8_t, int))Tool->Vft[SpawnDecoVft];
        if (SpawnDeco)
            Trap = SpawnDeco(Tool, TrapClass, Location, Rotation, AttachedActor, AttachmentType, 0);
    }

    if (IsExcludedSavedTrapActor(Trap, ExcludedActors, BaselineActors))
    {
        Tool->K2_DestroyActor();
        return nullptr;
    }
    Trap = FinalizeSavedTrapInstance(Trap, TrapClass, AttachedActor, AttachmentType,
        AttachmentSlot, SavedTrapLevel, SavedOriginalTrapLevel, ItemDefinition,
        ContextItemDefinition, PlayerController, true);
    Tool->K2_DestroyActor();
    return Trap;
}

void AFortDecoTool::ServerSpawnDeco_(UObject* Context, FFrame& Stack)
{
    FVector Location;
    FRotator Rotation;
    ABuildingSMActor* AttachedActor;
    uint8_t InBuildingAttachmentType;
    Stack.StepCompiledIn(&Location);
    Stack.StepCompiledIn(&Rotation);
    Stack.StepCompiledIn(&AttachedActor);
    Stack.StepCompiledIn(&InBuildingAttachmentType);
    Stack.IncrementCode();
    auto DecoTool = (AFortDecoTool*)Context;

    AFortPlayerPawnAthena* Pawn = nullptr;
    AFortPlayerControllerAthena* PlayerController = nullptr;
    AFortPlayerStateAthena* PlayerState = nullptr;
    if (!ResolveDecoToolPlayer(DecoTool, Pawn, PlayerController, PlayerState))
        return;
    AllowAllTeamsOnPlayerJumpPad(nullptr, ResolvePlacedDecoDefinition(
            DecoTool, InBuildingAttachmentType));

    if (VersionInfo.FortniteVersion >= 18) // idk when they stripped it, guessing s18
    {
        auto ItemDefinition = (UFortDecoItemDefinition*)DecoTool->ItemDefinition;

        if (auto ContextTrapTool = DecoTool->Cast<AFortDecoTool_ContextTrap>()) {
            switch ((int)InBuildingAttachmentType) {
            case 0:
            case 6:
                ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->FloorTrap;
                break;
            case 7:
            case 2:
                ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->CeilingTrap;
                break;
            case 1:
                ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->WallTrap;
                break;
            case 8:
                ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->StairTrap;
                break;
            }
        }

        if (!ItemDefinition)
            return;
        SnapChapterFourDirectionalPadToSupport(ItemDefinition, Location, AttachedActor,
            InBuildingAttachmentType);

        auto ShouldAllowServerSpawnDeco = (bool (*)(AFortDecoTool*, FVector&, FRotator&,
            ABuildingSMActor*, uint8_t)) DecoTool->Vft[ShouldAllowServerSpawnDecoVft];

        if (ShouldAllowServerSpawnDecoVft && !ShouldAllowServerSpawnDeco(DecoTool, Location,
            Rotation, AttachedActor, InBuildingAttachmentType))
            return;

        auto PreviousAttachedActorCount = GetAttachedBuildingActorCount(AttachedActor);
        ABuildingSMActor* NewTrap = nullptr;
        if (VersionInfo.FortniteVersion >= 27)
        {
            auto SpawnDeco = (ABuildingSMActor * (*)(AFortDecoTool*, TSubclassOf<ABuildingSMActor>&,
                FVector&, FRotator&, ABuildingSMActor*, uint8_t, int)) DecoTool->Vft[SpawnDecoVft];

            TSubclassOf<ABuildingSMActor> SubclassOf;
            SubclassOf.ClassPtr = ItemDefinition->BlueprintClass.Get();
            NewTrap = SpawnDecoVft && SpawnDeco ? SpawnDeco(DecoTool, SubclassOf, Location,
                Rotation, AttachedActor, InBuildingAttachmentType, 0) : nullptr;
        }
        else
        {
            auto SpawnDeco = (ABuildingSMActor * (*)(AFortDecoTool*, UClass*, FVector&, FRotator&,
                ABuildingSMActor*, uint8_t, int)) DecoTool->Vft[SpawnDecoVft];

            NewTrap = SpawnDecoVft && SpawnDeco ? SpawnDeco(DecoTool,
                ItemDefinition->BlueprintClass.Get(), Location, Rotation, AttachedActor,
                InBuildingAttachmentType, 0) : nullptr;
        }

        ApplyTrapTeam(NewTrap, PlayerState);
        if (NewTrap)
            QueueSavedTrapAttachmentRepair(NewTrap, AttachedActor, InBuildingAttachmentType, -1);
        ABuildingSMActor::RegisterTrapDefinition(
            NewTrap ? NewTrap->Class : ItemDefinition->BlueprintClass.Get(),
            ItemDefinition, NewTrap);
        ApplyTrapTeamToNewAttachments(AttachedActor, PlayerState, PreviousAttachedActorCount,
            ItemDefinition);

        if (FConfiguration::bInfiniteAmmo || (PlayerController->HasbInfiniteAmmo() &&
                PlayerController->bInfiniteAmmo))
            return;

        auto Resource = UFortKismetLibrary::GetDefaultObj()->K2_GetResourceItemDefinition(AttachedActor->ResourceType);
        auto item = PlayerController->WorldInventory->Inventory.ItemInstances.Search([&](UFortWorldItem* Item) {
            return Item->ItemEntry.ItemDefinition == DecoTool->ItemDefinition;
            });
        auto itemEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry) {
            return entry.ItemDefinition == DecoTool->ItemDefinition;
            }, FFortItemEntry::Size());
        if (!itemEntry)
            return;

        itemEntry->Count--;
        if (itemEntry->Count <= 0)
            PlayerController->WorldInventory->Remove(itemEntry->ItemGuid);
        else
        {
            (*item)->ItemEntry.Count = itemEntry->Count;
            PlayerController->WorldInventory->UpdateEntry(*itemEntry);
            (*item)->ItemEntry.bIsDirty = true;
        }
    }

    if (VersionInfo.FortniteVersion < 18)
    {
        auto PreviousAttachedActorCount = GetAttachedBuildingActorCount(AttachedActor);
        auto ProtectedStacks = ProtectLegacyTrapStacks(DecoTool, PlayerController,
                InBuildingAttachmentType);
        callOG(DecoTool, Stack.GetCurrentNativeFunction(), ServerSpawnDeco, Location, Rotation,
            AttachedActor, InBuildingAttachmentType);
        RestoreLegacyTrapStacks(PlayerController->WorldInventory, ProtectedStacks);
        ApplyTrapTeamToNewAttachments(AttachedActor, PlayerState, PreviousAttachedActorCount,
            ResolvePlacedDecoDefinition(DecoTool, InBuildingAttachmentType));
    }
}

void AFortDecoTool_ContextTrap::ServerSpawnDeco_Implementation(UObject* Context, FFrame& Stack)
{
    auto& Location = *(FVector*)Stack.Locals;
    auto& Rotation = *(FRotator*)(__int64(Stack.Locals) + FVector::Size());
    auto& AttachedActor = *(ABuildingSMActor**)(__int64(Stack.Locals) + FVector::Size() + FRotator::Size());
    auto& InBuildingAttachmentType = *(uint8_t*)(__int64(Stack.Locals) + FVector::Size() + FRotator::Size() + 8);
    auto DecoTool = (AFortDecoTool_ContextTrap*)Context;

    AFortPlayerPawnAthena* Pawn = nullptr;
    AFortPlayerControllerAthena* PlayerController = nullptr;
    AFortPlayerStateAthena* PlayerState = nullptr;
    if (!ResolveDecoToolPlayer(DecoTool, Pawn, PlayerController, PlayerState))
        return;
    AllowAllTeamsOnPlayerJumpPad(nullptr, ResolvePlacedDecoDefinition(
            DecoTool, InBuildingAttachmentType));

    if (VersionInfo.FortniteVersion >= 18) // idk when they stripped it, guessing s18
    {
        auto ItemDefinition = (UFortDecoItemDefinition*)DecoTool->ItemDefinition;

        if (auto ContextTrapTool = DecoTool->Cast<AFortDecoTool_ContextTrap>()) {
            switch ((int)InBuildingAttachmentType) {
            case 0:
            case 6:
                ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->FloorTrap;
                break;
            case 7:
            case 2:
                ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->CeilingTrap;
                break;
            case 1:
                ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->WallTrap;
                break;
            case 8:
                ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->StairTrap;
                break;
            }
        }

        if (!ItemDefinition)
            return;
        SnapChapterFourDirectionalPadToSupport(ItemDefinition, Location, AttachedActor,
            InBuildingAttachmentType);

        auto ShouldAllowServerSpawnDeco = (bool (*)(AFortDecoTool*, FVector&, FRotator&,
            ABuildingSMActor*, uint8_t)) DecoTool->Vft[ShouldAllowServerSpawnDecoVft];

        if (ShouldAllowServerSpawnDecoVft && !ShouldAllowServerSpawnDeco(DecoTool, Location,
            Rotation, AttachedActor, InBuildingAttachmentType))
            return;

        auto PreviousAttachedActorCount = GetAttachedBuildingActorCount(AttachedActor);
        ABuildingSMActor* NewTrap = nullptr;
        if (VersionInfo.FortniteVersion >= 27)
        {
            auto SpawnDeco = (ABuildingSMActor * (*)(AFortDecoTool*, TSubclassOf<ABuildingSMActor>&,
                FVector&, FRotator&, ABuildingSMActor*, uint8_t, int)) DecoTool->Vft[SpawnDecoVft];

            TSubclassOf<ABuildingSMActor> SubclassOf;
            SubclassOf.ClassPtr = ItemDefinition->BlueprintClass.Get();
            NewTrap = SpawnDecoVft && SpawnDeco ? SpawnDeco(DecoTool, SubclassOf, Location,
                Rotation, AttachedActor, InBuildingAttachmentType, 0) : nullptr;
        }
        else
        {
            auto SpawnDeco = (ABuildingSMActor * (*)(AFortDecoTool*, UClass*, FVector&, FRotator&,
                ABuildingSMActor*, uint8_t, int)) DecoTool->Vft[SpawnDecoVft];

            NewTrap = SpawnDecoVft && SpawnDeco ? SpawnDeco(DecoTool,
                ItemDefinition->BlueprintClass.Get(), Location, Rotation, AttachedActor,
                InBuildingAttachmentType, 0) : nullptr;
        }

        ApplyTrapTeam(NewTrap, PlayerState);
        if (NewTrap)
            QueueSavedTrapAttachmentRepair(NewTrap, AttachedActor, InBuildingAttachmentType, -1);
        ABuildingSMActor::RegisterTrapDefinition(
            NewTrap ? NewTrap->Class : ItemDefinition->BlueprintClass.Get(),
            ItemDefinition, NewTrap);
        ApplyTrapTeamToNewAttachments(AttachedActor, PlayerState, PreviousAttachedActorCount,
            ItemDefinition);

        if (FConfiguration::bInfiniteAmmo || (PlayerController->HasbInfiniteAmmo() &&
                PlayerController->bInfiniteAmmo))
            return;

        auto Resource = UFortKismetLibrary::GetDefaultObj()->K2_GetResourceItemDefinition(AttachedActor->ResourceType);
        auto item = PlayerController->WorldInventory->Inventory.ItemInstances.Search([&](UFortWorldItem* Item) {
            return Item->ItemEntry.ItemDefinition == DecoTool->ItemDefinition;
            });
        auto itemEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry) {
            return entry.ItemDefinition == DecoTool->ItemDefinition;
            }, FFortItemEntry::Size());
        if (!itemEntry)
            return;

        itemEntry->Count--;
        if (itemEntry->Count <= 0)
            PlayerController->WorldInventory->Remove(itemEntry->ItemGuid);
        else
        {
            (*item)->ItemEntry.Count = itemEntry->Count;
            PlayerController->WorldInventory->UpdateEntry(*itemEntry);
            (*item)->ItemEntry.bIsDirty = true;
        }
    }

    if (VersionInfo.FortniteVersion < 18)
    {
        auto PreviousAttachedActorCount = GetAttachedBuildingActorCount(AttachedActor);
        auto ProtectedStacks = ProtectLegacyTrapStacks(DecoTool, PlayerController,
                InBuildingAttachmentType);
        ServerSpawnDeco_ImplementationOG(Context, Stack);
        RestoreLegacyTrapStacks(PlayerController->WorldInventory, ProtectedStacks);
        ApplyTrapTeamToNewAttachments(AttachedActor, PlayerState, PreviousAttachedActorCount,
            ResolvePlacedDecoDefinition(DecoTool, InBuildingAttachmentType));
    }
}

uint8 GetBuildingTypeFromBuildingAttachmentType(uint8 BuildingAttachmentType)
{
    if (uint8(BuildingAttachmentType) <= 7)
    {
        LONG Val = 0xC5;
        if (BitTest(&Val, uint8(BuildingAttachmentType)))
            return 1;
    }
    if (BuildingAttachmentType == 1)
        return 0;
    return 12;
}

extern uint64_t PayBuildableClassPlacementCost_;
extern uint64_t CanAffordToPlaceBuildableClass_;
extern uint64_t CantBuild_;
void AFortDecoTool::ServerCreateBuildingAndSpawnDeco(UObject* Context, FFrame& Stack)
{
    FVector BuildingLocation{};
    FRotator BuildingRotation{};
    FVector Location{};
    FRotator Rotation{};
    uint8_t InBuildingAttachmentType = 0;
    bool bSpawnDecoOnExtraPiece = false;
    FVector BuildingExtraPieceLocation{};
    auto NativeFunction = Stack.GetCurrentNativeFunction();
    Stack.StepCompiledIn(&BuildingLocation);
    Stack.StepCompiledIn(&BuildingRotation);
    Stack.StepCompiledIn(&Location);
    Stack.StepCompiledIn(&Rotation);
    Stack.StepCompiledIn(&InBuildingAttachmentType);
    if (NativeFunction && NativeFunction->GetProperty("bSpawnDecoOnExtraPiece"))
        Stack.StepCompiledIn(&bSpawnDecoOnExtraPiece);
    if (NativeFunction && NativeFunction->GetProperty("BuildingExtraPieceLocation"))
        Stack.StepCompiledIn(&BuildingExtraPieceLocation);
    Stack.IncrementCode();

    auto Tool = (AFortDecoTool*)Context;
    if (!Tool)
        return;

    auto Pawn = (AFortPlayerPawnAthena*)Tool->Owner;
    if (!Pawn)
        return;

    auto PlayerController = (AFortPlayerControllerAthena*)Pawn->Controller;
    if (!PlayerController)
        return;

    auto PlayerState = (AFortPlayerStateAthena*)Pawn->PlayerState;
    if (!PlayerState)
        return;

    auto ItemDefinition = (UFortDecoItemDefinition*)Tool->ItemDefinition;

    if (auto ContextTrapTool = Tool->Cast<AFortDecoTool_ContextTrap>()) {
        if (!ContextTrapTool->ContextTrapItemDefinition || !SDK::MemReadable(ContextTrapTool->ContextTrapItemDefinition, sizeof(void*)))
            return;
        switch ((int)InBuildingAttachmentType) {
        case 0:
        case 6:
            ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->FloorTrap;
            break;
        case 7:
        case 2:
            ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->CeilingTrap;
            break;
        case 1:
            ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->WallTrap;
            break;
        case 8:
            ItemDefinition = (UFortDecoItemDefinition*)ContextTrapTool->ContextTrapItemDefinition->StairTrap;
            break;
        }
    }
    if (!ItemDefinition || !SDK::MemReadable(ItemDefinition, 0x40) ||
        !ItemDefinition->Class || !SDK::MemReadable(ItemDefinition->Class, 0x40) ||
        !ItemDefinition->IsA<UFortDecoItemDefinition>())
        return;

    TArray<const UObject*> AutoCreateAttachmentBuildingShapes;
    for (auto& AutoCreateAttachmentBuildingShape : ItemDefinition->AutoCreateAttachmentBuildingShapes)
    {
        AutoCreateAttachmentBuildingShapes.Add(AutoCreateAttachmentBuildingShape);
    }

    auto World = UWorld::GetWorld();
    auto GameState = World ? (AFortGameStateAthena*)World->GameState : nullptr;
    if (!GameState)
    {
        AutoCreateAttachmentBuildingShapes.Free();
        return;
    }
    auto bIgnoreCanAffordCheck = UFortKismetLibrary::DoesItemDefinitionHaveGameplayTag(ItemDefinition, FGameplayTag(FName(L"Trap.ExtraPiece.Cost.Ignore")));
    TSubclassOf<AActor> BuildingClass{};
    EFortResourceType ResourceType = PlayerController->CurrentResourceType;

    if (ItemDefinition->HasAutoCreateAttachmentBuildingResourceType() && ItemDefinition->AutoCreateAttachmentBuildingResourceType != EFortResourceType(EFortResourceType__Enum::GetNone()))
        ResourceType = ItemDefinition->AutoCreateAttachmentBuildingResourceType;
    auto BuildingType = GetBuildingTypeFromBuildingAttachmentType(InBuildingAttachmentType);

    for (auto Shape : AutoCreateAttachmentBuildingShapes)
    {
        for (auto& Class : GameState->AllPlayerBuildableClasses)
        {
            if (!Class || !Class->GetDefaultObj())
                continue;
            auto Default = (ABuildingSMActor*)Class->GetDefaultObj();

            UObject* EditModePatternData = nullptr;

            if (Default->HasEditModePatternData())
                EditModePatternData = Default->EditModePatternData;
            else
            {
                auto ClassData = Default->GetClassData();
                if (ClassData)
                    EditModePatternData = ClassData->EditModePatternData;
            }

            if (Default->ResourceType == ResourceType && Default->BuildingType == BuildingType &&
                EditModePatternData == Shape)
            {
                BuildingClass = Class;
                goto _out;
            }
        }
    }

    return;
_out:

    FBuildingClassData BuildingClassData;
    BuildingClassData.BuildingClass = BuildingClass;
    BuildingClassData.PreviousBuildingLevel = -1;

    static auto UpgradeLevelOffset = FBuildingClassData::StaticStruct()->GetOffset("UpgradeLevel");
    if (VersionInfo.EngineVersion >= 5.3)
        *(uint8*)(__int64(&BuildingClassData) + UpgradeLevelOffset) = 0;
    else
        *(uint32*)(__int64(&BuildingClassData) + UpgradeLevelOffset) = 0;

    UFortWorldItem* Item = nullptr;
    const bool bInfiniteMaterials = FConfiguration::bInfiniteMats.load(std::memory_order_acquire);
    if (!bInfiniteMaterials)
    {
        auto CanAffordToPlaceBuildableClass = (bool(*)(AFortPlayerControllerAthena*, FBuildingClassData)) CanAffordToPlaceBuildableClass_;

        if (CanAffordToPlaceBuildableClass)
        {
            const bool bOverrideBuildFree = PlayerController->HasbBuildFree() &&
                PlayerController->bBuildFree;
            if (bOverrideBuildFree)
            {
                bool bBuildFree = false;
                PlayerController->bBuildFree = bBuildFree;
            }
            const bool bCanAfford = CanAffordToPlaceBuildableClass(
                    PlayerController, BuildingClassData);
            if (bOverrideBuildFree)
            {
                bool bBuildFree = true;
                PlayerController->bBuildFree = bBuildFree;
            }
            if (!bCanAfford)
                return;
        }
        else
        {
            auto Resource = UFortKismetLibrary::K2_GetResourceItemDefinition(((ABuildingSMActor*)BuildingClass->GetDefaultObj())->ResourceType);

            auto ItemP = PlayerController->WorldInventory->Inventory.ItemInstances.Search([&](UFortWorldItem* entry)
                { return entry->ItemEntry.ItemDefinition == Resource; });

            if (!ItemP)
                return;

            Item = *ItemP;

            if (Item->ItemEntry.Count < 10)
                return;
        }
    }

    struct _Pad_0xC
    {
        uint8_t Padding[0xC];
    };
    struct _Pad_0x18
    {
        uint8_t Padding[0x18];
    };

    TArray<ABuildingSMActor*> RemoveBuildings;
    if (VersionInfo.FortniteVersion >= 27)
    {
        char _Unk_OutVar1;
        auto CantBuild = (__int64 (*)(UWorld*, TSubclassOf<AActor>&, _Pad_0x18, _Pad_0x18, bool,
            TArray<ABuildingSMActor*> *, char*))CantBuild_;

        if (CantBuild(UWorld::GetWorld(), BuildingClass, *(_Pad_0x18*)&BuildingLocation,
            *(_Pad_0x18*)&BuildingRotation, false, &RemoveBuildings, &_Unk_OutVar1))
            return;
    }
    else
    {
        char _Unk_OutVar1;
        auto CantBuild = (__int64 (*)(UWorld*, const UClass*, _Pad_0xC, _Pad_0xC, bool,
            TArray<ABuildingSMActor*> *, char*))CantBuild_;
        auto CantBuildNew = (__int64 (*)(UWorld*, const UClass*, _Pad_0x18, _Pad_0x18, bool,
            TArray<ABuildingSMActor*> *, char*))CantBuild_;

        if (VersionInfo.FortniteVersion >= 20.00 ? CantBuildNew(UWorld::GetWorld(), BuildingClass,
            *(_Pad_0x18*)&BuildingLocation, *(_Pad_0x18*)&BuildingRotation, false, &RemoveBuildings,
            &_Unk_OutVar1) : CantBuild(UWorld::GetWorld(), BuildingClass, *(_Pad_0xC*)&BuildingLocation, *(_Pad_0xC*)&BuildingRotation, false, &RemoveBuildings, &_Unk_OutVar1))
            return;
    }

    for (auto& RemoveBuilding : RemoveBuildings)
        RemoveBuilding->K2_DestroyActor();
    RemoveBuildings.Free();

    static auto K2_SpawnBuildingActor = ABuildingSMActor::GetDefaultObj()->GetFunction("K2_SpawnBuildingActor");

    ABuildingSMActor* Building = nullptr;
    Building = UWorld::SpawnActorUnfinished<ABuildingSMActor>(BuildingClass, BuildingLocation,
        BuildingRotation, PlayerController);
    if (!Building)
        return;
    Building->InitializeKismetSpawnedBuildingActor(Building, PlayerController, true, nullptr, false);
    UWorld::FinishSpawnActor(Building, BuildingLocation, BuildingRotation);

    Building->bPlayerPlaced = true;

    if (((AFortPlayerStateAthena*)PlayerController->PlayerState)->HasTeamIndex())
        Building->Team = ((AFortPlayerStateAthena*)PlayerController->PlayerState)->TeamIndex;

    if (Building->HasTeamIndex())
        Building->TeamIndex = Building->Team;

    if (Building->HasOwnerPersistentID() && PlayerState->HasWorldPlayerId())
        Building->OwnerPersistentID = PlayerState->WorldPlayerId;

    if (!bInfiniteMaterials)
    {
        auto PayBuildableClassPlacementCost = (int(*)(AFortPlayerControllerAthena*, FBuildingClassData)) PayBuildableClassPlacementCost_;
        if (PayBuildableClassPlacementCost)
        {
            const bool bOverrideBuildFree = PlayerController->HasbBuildFree() &&
                PlayerController->bBuildFree;
            if (bOverrideBuildFree)
            {
                bool bBuildFree = false;
                PlayerController->bBuildFree = bBuildFree;
            }
            PayBuildableClassPlacementCost(PlayerController, BuildingClassData);
            if (bOverrideBuildFree)
            {
                bool bBuildFree = true;
                PlayerController->bBuildFree = bBuildFree;
            }
        }
        else if (Item)
        {
            Item->ItemEntry.Count -= 10;
            PlayerController->WorldInventory->Update(&Item->ItemEntry);
        }
    }

    Tool->ServerSpawnDeco(Location, Rotation, Building, InBuildingAttachmentType);
}

void ABuildingSMActor::PostLoadHook()
{
    SDK::DbgLog("  [BSM] 0 start (CDO=%p)\n", (void*)GetDefaultObj());
    PrewarmPlayerJumpPadReflection();
    bool _hasOverride = GetDefaultObj() && GetDefaultObj()->HasBuildingResourceAmountOverride();
    SDK::DbgLog("  [BSM] 0a HasBuildingResourceAmountOverride=%d\n", (int)_hasOverride);
    if (!_hasOverride)
    {
        SDK::DbgLog("  [BSM] 0b pre-FindPattern#1\n");
        GetSparseClassData_ = Memcury::Scanner::FindPattern("48 83 EC ? 48 8B 81 ? ? ? ? 45 33 C0 4C 8B C9").Get();
        SDK::DbgLog("  [BSM] 0c FP#1=%p\n", (void*)GetSparseClassData_);

        if (!GetSparseClassData_)
            GetSparseClassData_ = Memcury::Scanner::FindPattern("48 83 EC ? 48 8B 81 ? ? ? ? 48 85 C0 74 ? 48 83 C4 ? C3").Get();
        SDK::DbgLog("  [BSM] 0d FP#2 done GSCD=%p\n", (void*)GetSparseClassData_);

        if (!GetSparseClassData_)
            GetSparseClassData_ = Memcury::Scanner::FindPattern("48 83 EC ? 48 8B 81 ? ? ? ? 45 33 C0 48 85 C0 75").Get();
        SDK::DbgLog("  [BSM] 0e FP#3 done GSCD=%p\n", (void*)GetSparseClassData_);
    }
    SDK::DbgLog("  [BSM] 1 sparse-class-data done\n");
    if (VersionInfo.FortniteVersion >= 18)
    {
        SpawnDecoVft = FindSpawnDecoVft();
        SDK::DbgLog("  [BSM] 2 SpawnDecoVft=%p\n", (void*)SpawnDecoVft);
        ShouldAllowServerSpawnDecoVft = FindShouldAllowServerSpawnDecoVft();
        SDK::DbgLog("  [BSM] 3 ShouldAllowServerSpawnDecoVft=%p\n", (void*)ShouldAllowServerSpawnDecoVft);
    }
    else
    {
        SpawnDecoVft = 0;
        ShouldAllowServerSpawnDecoVft = 0;
        SDK::DbgLog("  [BSM] 2 legacy deco path; native vtable lookup skipped\n");
    }

    auto OnDamageServerAddr = FindFunctionCall(L"OnDamageServer",
        VersionInfo.EngineVersion == 4.16 ? std::vector<uint8_t>{ 0x4C, 0x89,
        0x4C } : VersionInfo.EngineVersion == 4.19 || VersionInfo.EngineVersion >= 4.27 ? std::vector<uint8_t>{ 0x48,
        0x8B, 0xC4 } : std::vector<uint8_t>{ 0x40, 0x55 });
    SDK::DbgLog("  [BSM] 4 OnDamageServerAddr=%p\n", (void*)OnDamageServerAddr);

    Utils::Hook(OnDamageServerAddr, OnDamageServer, OnDamageServerOG);
    SDK::DbgLog("  [BSM] 5 OnDamageServer hooked\n");

    auto ServerSpawnDecoFunc = AFortDecoTool::GetDefaultObj()->GetFunction("ServerSpawnDeco");
    if (ServerSpawnDecoFunc)
        Utils::ExecHook(ServerSpawnDecoFunc, AFortDecoTool::ServerSpawnDeco_,
            AFortDecoTool::ServerSpawnDeco_OG);

    auto CreateBuildingAndSpawnDecoFunc =
        AFortDecoTool::GetDefaultObj()->GetFunction("ServerCreateBuildingAndSpawnDeco");
    auto GameStateClass = AFortGameStateAthena::StaticClass();
    const bool bSupportsCustomCreateBuildingAndSpawnDeco = CreateBuildingAndSpawnDecoFunc &&
        CreateBuildingAndSpawnDecoFunc->GetProperty("BuildingLocation") &&
        GameStateClass && GameStateClass->GetProperty("AllPlayerBuildableClasses");
    if (bSupportsCustomCreateBuildingAndSpawnDeco)
    {
        Utils::ExecHook(CreateBuildingAndSpawnDecoFunc,
            AFortDecoTool::ServerCreateBuildingAndSpawnDeco,
            AFortDecoTool::ServerCreateBuildingAndSpawnDecoOG);
    }
    else
    {
        // 7.40's RPC carries only Location, Rotation and attachment type, and its game state has no AllPlayerBuildableClasses.
        SDK::DbgLog("  [BSM] legacy ServerCreateBuildingAndSpawnDeco left native\n");
    }
    if (AFortDecoTool_ContextTrap::StaticClass())
    {
        auto Func = AFortDecoTool_ContextTrap::GetDefaultObj()->GetFunction("ServerSpawnDeco");

        if (!Func)
            Func = AFortDecoTool_ContextTrap::GetDefaultObj()->GetFunction("ServerSpawnDeco_Implementation");

        if (Func && Func != ServerSpawnDecoFunc)
            Utils::ExecHook(Func, AFortDecoTool_ContextTrap::ServerSpawnDeco_Implementation,
                AFortDecoTool_ContextTrap::ServerSpawnDeco_ImplementationOG);

        auto Func2 = AFortDecoTool_ContextTrap::GetDefaultObj()->GetFunction("ServerCreateBuildingAndSpawnDeco_Implementation");

        if (!Func2)
            Func2 = AFortDecoTool_ContextTrap::GetDefaultObj()->GetFunction("ServerCreateBuildingAndSpawnDeco");

        if (bSupportsCustomCreateBuildingAndSpawnDeco && Func2 &&
            Func2 != CreateBuildingAndSpawnDecoFunc)
        {
            Utils::ExecHook(Func2, AFortDecoTool::ServerCreateBuildingAndSpawnDeco,
                AFortDecoTool::ServerCreateBuildingAndSpawnDecoOG);
        }
    }
    SDK::DbgLog("  [BSM] 6 PostLoadHook complete\n");
}
