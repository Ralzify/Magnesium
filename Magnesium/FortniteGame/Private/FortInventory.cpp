#include "pch.h"
#include "../Public/FortInventory.h"
#include "../Public/FortPlayerPawnAthena.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortGameStateAthena.h"
#include "../Public/FortAthenaMutator.h"
#include "../Public/FortKismetLibrary.h"
#include "../../Erbium/Public/Configuration.h"
#include "../../Erbium/Support/Public/VersionFeatureAdapter.h"
#include "../Public/FortWeapon.h"
#include "../Public/FortWeaponMods.h"
#include <ShlObj.h>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <vector>

uint32_t OnItemInstanceAddedVft;
uint64_t ApplyGadgetDataAddress;
uint64_t ClearAbility_;

namespace
{
    constexpr int32 GhostCharacterPartSlotCount = 6;

    struct FGhostCharacterPartRestore
    {
        AFortPlayerPawnAthena* Pawn = nullptr;
        AFortPlayerStateAthena* PlayerState = nullptr;
        UObject* Parts[GhostCharacterPartSlotCount]{};
        bool bRestore[GhostCharacterPartSlotCount]{};
    };

    std::unordered_map<AFortPlayerControllerAthena*, FGhostCharacterPartRestore>
        GhostCharacterPartRestores;

    struct FPendingGhostModeCleanup
    {
        TWeakObjectPtr<AFortPlayerControllerAthena> Owner;
        TWeakObjectPtr<UFortItemDefinition> ItemDefinition;
        FGhostCharacterPartRestore CharacterParts{};
        bool bRestoreCharacterParts = false;
        double EarliestFinalizeTime = 0.0;
        double RetryDeadline = 0.0;
        double ForceFinalizeDeadline = 0.0;
        int32 NativeEndAttempts = 0;
        int32 FinalizationPass = 0;
        bool bLoggedWaitingForPawn = false;
        bool bHarvestingToolRefocusAttempted = false;
    };

    std::vector<FPendingGhostModeCleanup> PendingGhostModeCleanups;

    struct FTrackedGhostModeSession
    {
        TWeakObjectPtr<AFortPlayerControllerAthena> Owner;
        TWeakObjectPtr<UFortItemDefinition> ItemDefinition;
        FGuid ItemGuid{};
        double StartedAt = 0.0;
        double ExpireAt = 0.0;
        double EarliestExitCleanupTime = 0.0;
        float InitialPawnExitStartTime = 0.0f;
        bool bHasInitialPawnExitStartTime = false;
        bool bObservedControllerGhostMode = false;
        bool bBackingRemovalObserved = false;
        bool bExitRequested = false;
    };

    std::vector<FTrackedGhostModeSession> TrackedGhostModeSessions;

    bool ResolveFixedCharacterPartArray(UObject* Owner, const char* PropertyName,
        UObject**& OutParts)
    {
        OutParts = nullptr;
        if (!Owner || !Owner->Class || !PropertyName || Offsets::ElementSize < sizeof(int32))
        {
            return false;
        }

        auto Property = Owner->GetProperty(PropertyName);
        if (!Property)
            return false;

        const size_t RequiredPropertyBytes = static_cast<size_t>(max(Offsets::Offset_Internal,
                Offsets::ElementSize)) + sizeof(uint32);
        if (!SDK::MemReadable(Property, RequiredPropertyBytes))
        {
            return false;
        }

        const int32 PropertyOffset = static_cast<int32>(SDK::ReadPropertyOffset(
                GetFromOffset<uint32>(Property, Offsets::Offset_Internal)));
        const uint32 ElementSize = GetFromOffset<uint32>(Property, Offsets::ElementSize);
        const int32 ArrayDimension = GetFromOffset<int32>(Property,
            Offsets::ElementSize - sizeof(int32));
        const int32 OwnerSize = Owner->Class->GetPropertiesSize();
        if (PropertyOffset < 0 || ElementSize != sizeof(UObject*) ||
            ArrayDimension < GhostCharacterPartSlotCount || ArrayDimension > 16 ||
            OwnerSize <= PropertyOffset || static_cast<size_t>(ElementSize) * ArrayDimension >
                static_cast<size_t>(OwnerSize - PropertyOffset))
        {
            return false;
        }

        auto Parts = reinterpret_cast<UObject**>(reinterpret_cast<uint8*>(Owner) + PropertyOffset);
        if (!SDK::MemReadable(Parts, static_cast<size_t>(ElementSize) * ArrayDimension))
        {
            return false;
        }

        OutParts = Parts;
        return true;
    }

    bool ResolvePlayerStateCharacterPartArray(AFortPlayerStateAthena* PlayerState,
        UObject**& OutParts, const wchar_t*& OutDirtyProperty)
    {
        OutParts = nullptr;
        OutDirtyProperty = nullptr;
        if (!PlayerState || !PlayerState->Class)
            return false;

        // 6.21 uses the flat fixed array; the adjacent struct layout stays supported too.
        if (ResolveFixedCharacterPartArray(PlayerState, "CharacterParts", OutParts))
        {
            OutDirtyProperty = L"CharacterParts";
            return true;
        }
        if (ResolveFixedCharacterPartArray(PlayerState, "LocalCharacterParts", OutParts))
        {
            OutDirtyProperty = L"LocalCharacterParts";
            return true;
        }

        auto CharacterPartsProperty = PlayerState->GetProperty("CharacterParts", 0x100000);
        auto CharacterPartsStruct = FindObject<UStruct>(
            L"/Script/FortniteGame.CustomCharacterParts");
        if (!CharacterPartsProperty || !CharacterPartsStruct ||
            Offsets::ElementSize < sizeof(int32))
        {
            return false;
        }

        auto PartsProperty = CharacterPartsStruct->GetProperty("Parts");
        if (!PartsProperty)
            return false;

        const int32 OuterOffset = static_cast<int32>(SDK::ReadPropertyOffset(GetFromOffset<uint32>(
                    CharacterPartsProperty, Offsets::Offset_Internal)));
        const uint32 OuterSize = GetFromOffset<uint32>(CharacterPartsProperty,
            Offsets::ElementSize);
        const int32 PartsOffset = static_cast<int32>(SDK::ReadPropertyOffset(GetFromOffset<uint32>(
                    PartsProperty, Offsets::Offset_Internal)));
        const uint32 PartElementSize = GetFromOffset<uint32>(PartsProperty, Offsets::ElementSize);
        const int32 PartArrayDimension = GetFromOffset<int32>(PartsProperty,
            Offsets::ElementSize - sizeof(int32));
        const int32 PlayerStateSize = PlayerState->Class->GetPropertiesSize();
        if (OuterOffset < 0 || PartsOffset < 0 || PartElementSize != sizeof(UObject*) ||
            PartArrayDimension < GhostCharacterPartSlotCount || PartArrayDimension > 16 ||
            OuterSize < static_cast<uint32>(PartsOffset) || static_cast<size_t>(PartElementSize) *
                    PartArrayDimension > static_cast<size_t>(OuterSize - PartsOffset) ||
            PlayerStateSize <= OuterOffset || OuterSize > static_cast<uint32>(
                PlayerStateSize - OuterOffset))
        {
            return false;
        }

        auto Parts = reinterpret_cast<UObject**>(reinterpret_cast<uint8*>(PlayerState) +
            OuterOffset + PartsOffset);
        if (!SDK::MemReadable(Parts, static_cast<size_t>(PartElementSize) * PartArrayDimension))
        {
            return false;
        }

        OutParts = Parts;
        OutDirtyProperty = L"CharacterParts";
        return true;
    }

    bool CaptureCurrentGhostCharacterParts(AFortPlayerControllerAthena* PlayerController,
        FGhostCharacterPartRestore& OutRestore)
    {
        OutRestore = {};
        if (!PlayerController)
            return false;

        auto Pawn = PlayerController->MyFortPawn ? PlayerController->MyFortPawn
            : PlayerController->Pawn ? PlayerController->Pawn->Cast<AFortPlayerPawnAthena>()
                : nullptr;
        auto PlayerState = PlayerController->PlayerState ? PlayerController->PlayerState
                ->Cast<AFortPlayerStateAthena>() : nullptr;
        if (!Pawn || !PlayerState)
            return false;

        UObject** PawnParts = nullptr;
        UObject** PlayerStateParts = nullptr;
        const wchar_t* IgnoredDirtyProperty = nullptr;
        ResolveFixedCharacterPartArray(Pawn, "CharacterParts", PawnParts);
        ResolvePlayerStateCharacterPartArray(PlayerState, PlayerStateParts, IgnoredDirtyProperty);

        int32 CapturedCount = 0;
        for (int32 PartType = 0;
            PartType < GhostCharacterPartSlotCount;
            PartType++)
        {
            UObject* Part = PawnParts ? PawnParts[PartType] : nullptr;
            if (!Part && PlayerStateParts)
                Part = PlayerStateParts[PartType];
            if (!Part && PartType == 3)
            {
                Part = const_cast<UObject*>(FindObject<UObject>(
                        L"/Game/Characters/CharacterParts/Backpacks/"
                        L"NoBackpack.NoBackpack"));
            }
            if (!Part)
                continue;

            OutRestore.Parts[PartType] = Part;
            OutRestore.bRestore[PartType] = true;
            ++CapturedCount;
        }

        if (CapturedCount == 0)
            return false;
        OutRestore.Pawn = Pawn;
        OutRestore.PlayerState = PlayerState;
        return true;
    }

    bool CaptureGhostCharacterPartRestore(AFortPlayerControllerAthena* PlayerController,
        const UFortItemDefinition* ItemDefinition, FGhostCharacterPartRestore& OutRestore)
    {
        OutRestore = {};
        if (!PlayerController || !UFortKismetLibrary::IsGhostModeItemDefinition(ItemDefinition))
        {
            return false;
        }

        auto Gadget = ItemDefinition->Cast<UFortGadgetItemDefinition>();
        auto Pawn = PlayerController->MyFortPawn ? PlayerController->MyFortPawn
            : PlayerController->Pawn ? PlayerController->Pawn->Cast<AFortPlayerPawnAthena>()
                : nullptr;
        auto PlayerState = PlayerController->PlayerState ? PlayerController->PlayerState
                ->Cast<AFortPlayerStateAthena>() : nullptr;

        auto PreGrantRestore = GhostCharacterPartRestores.find(PlayerController);
        if (PreGrantRestore != GhostCharacterPartRestores.end())
        {
            OutRestore = PreGrantRestore->second;
            GhostCharacterPartRestores.erase(PreGrantRestore);
            OutRestore.Pawn = Pawn;
            OutRestore.PlayerState = PlayerState;
            if (OutRestore.Pawn && OutRestore.PlayerState)
                return true;
            OutRestore = {};
        }

        if (!Gadget || !Gadget->HasCharacterParts() || !Pawn || !Pawn->Class || !PlayerState ||
            Gadget->CharacterParts.Num() <= 0 || Gadget->CharacterParts.Num() >
                GhostCharacterPartSlotCount || Gadget->CharacterParts.Max() <
                Gadget->CharacterParts.Num() || !SDK::MemReadable(Gadget->CharacterParts.Data,
                static_cast<size_t>(Gadget->CharacterParts.Num()) * sizeof(UObject*)) ||
            Offsets::ElementSize < sizeof(int32))
        {
            return false;
        }

        auto PreviousPartsProperty = Pawn->GetProperty("PreviousCharacterParts");
        const size_t RequiredPropertyBytes = static_cast<size_t>(max(Offsets::Offset_Internal,
                Offsets::ElementSize)) + sizeof(uint32);
        if (!PreviousPartsProperty || !SDK::MemReadable(PreviousPartsProperty,
                RequiredPropertyBytes))
        {
            return false;
        }

        const int32 PreviousPartsOffset = static_cast<int32>(SDK::ReadPropertyOffset(
                GetFromOffset<uint32>(PreviousPartsProperty, Offsets::Offset_Internal)));
        const uint32 PartElementSize = GetFromOffset<uint32>(PreviousPartsProperty,
                Offsets::ElementSize);
        const int32 PartArrayDimension = GetFromOffset<int32>(PreviousPartsProperty,
                Offsets::ElementSize - sizeof(int32));
        const int32 PawnPropertiesSize = Pawn->Class->GetPropertiesSize();
        if (PreviousPartsOffset < 0 || PartElementSize != sizeof(UObject*) ||
            PartArrayDimension < GhostCharacterPartSlotCount || PartArrayDimension > 16 ||
            PawnPropertiesSize <= PreviousPartsOffset || static_cast<size_t>(PartElementSize) *
                    PartArrayDimension > static_cast<size_t>(PawnPropertiesSize -
                    PreviousPartsOffset))
        {
            return false;
        }

        auto PreviousParts = reinterpret_cast<UObject**>(reinterpret_cast<uint8*>(Pawn) +
            PreviousPartsOffset);
        if (!SDK::MemReadable(PreviousParts, static_cast<size_t>(PartElementSize) *
                    PartArrayDimension))
        {
            return false;
        }

        int32 CapturedCount = 0;
        for (int32 Index = 0;
            Index < Gadget->CharacterParts.Num(); Index++)
        {
            auto GadgetPart = Gadget->CharacterParts[Index];
            if (!GadgetPart || !GadgetPart->Class)
                continue;

            const int32 PartTypeOffset = GadgetPart->GetOffset("CharacterPartType");
            if (PartTypeOffset < 0 || PartTypeOffset >= GadgetPart->Class->GetPropertiesSize() ||
                !SDK::MemReadable(reinterpret_cast<uint8*>(GadgetPart) + PartTypeOffset,
                    sizeof(uint8)))
            {
                continue;
            }

            const uint8 PartType = GetFromOffset<uint8>(GadgetPart, PartTypeOffset);
            if (PartType >= GhostCharacterPartSlotCount || OutRestore.bRestore[PartType])
            {
                continue;
            }

            UObject* PreviousPart = PreviousParts[PartType];
            if (!PreviousPart && PartType == 3)
            {
                PreviousPart = const_cast<UObject*>(FindObject<UObject>(
                        L"/Game/Characters/CharacterParts/Backpacks/"
                        L"NoBackpack.NoBackpack"));
            }
            if (!PreviousPart)
                continue;

            OutRestore.Parts[PartType] = PreviousPart;
            OutRestore.bRestore[PartType] = true;
            ++CapturedCount;
        }

        if (CapturedCount == 0)
            return false;
        OutRestore.Pawn = Pawn;
        OutRestore.PlayerState = PlayerState;
        return true;
    }

    void ApplyGhostCharacterPartRestore(const FGhostCharacterPartRestore& Restore)
    {
        if (!Restore.Pawn || !Restore.PlayerState)
            return;

        int32 RestoredCount = 0;
        for (uint8 PartType = 0;
            PartType < GhostCharacterPartSlotCount; PartType++)
        {
            if (!Restore.bRestore[PartType] || !Restore.Parts[PartType])
            {
                continue;
            }
            Restore.Pawn->ServerChoosePart(PartType, Restore.Parts[PartType]);
            ++RestoredCount;
        }

        // ServerChoosePart is an RPC entry point, not a reliable local state writer on the 6.21 dedicated server.
        int32 PawnArrayWrites = 0;
        UObject** PawnParts = nullptr;
        if (ResolveFixedCharacterPartArray(Restore.Pawn, "CharacterParts", PawnParts))
        {
            for (int32 PartType = 0;
                PartType < GhostCharacterPartSlotCount;
                PartType++)
            {
                if (!Restore.bRestore[PartType] || !Restore.Parts[PartType])
                {
                    continue;
                }
                PawnParts[PartType] = Restore.Parts[PartType];
                ++PawnArrayWrites;
            }
        }

        int32 PlayerStateArrayWrites = 0;
        UObject** PlayerStateParts = nullptr;
        const wchar_t* DirtyProperty = nullptr;
        if (ResolvePlayerStateCharacterPartArray(Restore.PlayerState, PlayerStateParts,
                DirtyProperty))
        {
            for (int32 PartType = 0;
                PartType < GhostCharacterPartSlotCount;
                PartType++)
            {
                if (!Restore.bRestore[PartType] || !Restore.Parts[PartType])
                {
                    continue;
                }
                PlayerStateParts[PartType] = Restore.Parts[PartType];
                ++PlayerStateArrayWrites;
            }
            if (DirtyProperty)
            {
                VersionFeatureAdapter::MarkReplicatedPropertyDirty(Restore.PlayerState,
                        DirtyProperty);
            }
        }

        if (auto OnRepCharacterParts = Restore.PlayerState->GetFunction("OnRep_CharacterParts"))
        {
            Restore.PlayerState->ProcessEvent(OnRepCharacterParts, nullptr);
        }
        if (auto PartsReinitialized = Restore.Pawn->GetFunction("OnCharacterPartsReinitialized"))
        {
            Restore.Pawn->ProcessEvent(PartsReinitialized, nullptr);
        }
        if (auto KismetDefault = UFortKismetLibrary::GetDefaultObj())
        {
            if (KismetDefault->GetFunction("UpdatePlayerCustomCharacterPartsVisualization"))
            {
                UFortKismetLibrary::UpdatePlayerCustomCharacterPartsVisualization(
                        Restore.PlayerState);
            }
        }
        Restore.PlayerState->ForceNetUpdate();
        Restore.Pawn->ForceNetUpdate();
        SDK::DbgLog("[GhostMode] restored pre-gadget character parts "
            "pawn=%p playerState=%p selected=%d "
            "pawnWrites=%d playerStateWrites=%d\n", static_cast<void*>(Restore.Pawn),
            static_cast<void*>(Restore.PlayerState), RestoredCount, PawnArrayWrites,
            PlayerStateArrayWrites);
    }

    void RemoveGhostGadgetAbilitySetFallback(AFortPlayerControllerAthena* PlayerController,
        const UFortItemDefinition* ItemDefinition)
    {
        if (!PlayerController || !UFortKismetLibrary::IsGhostModeItemDefinition(ItemDefinition) ||
            !PlayerController->PlayerState)
        {
            return;
        }

        auto Gadget = ItemDefinition->Cast<UFortGadgetItemDefinition>();
        auto PlayerState = PlayerController->PlayerState->Cast<AFortPlayerStateAthena>();
        auto AbilitySystemComponent = PlayerState ? PlayerState->AbilitySystemComponent : nullptr;
        const UFortAbilitySet* AbilitySet = Gadget && Gadget->HasAbilitySet()
                ? Gadget->AbilitySet.Get() : nullptr;
        if (!AbilitySystemComponent || !AbilitySet)
            return;

        std::vector<FGameplayAbilitySpecHandle> AbilitiesToRemove;
        auto& AbilitySpecs = AbilitySystemComponent->ActivatableAbilities.Items;
        for (int32 SpecIndex = 0;
            SpecIndex < AbilitySpecs.Num(); SpecIndex++)
        {
            auto& Spec = AbilitySpecs.Get(SpecIndex, FGameplayAbilitySpec::Size());
            if (!Spec.Ability)
                continue;

            bool bFromGhostSet = false;
            for (int32 AbilityIndex = 0;
                AbilityIndex < AbilitySet->GameplayAbilities.Num();
                AbilityIndex++)
            {
                auto AbilityClass = AbilitySet->GameplayAbilities[AbilityIndex].Get();
                if (AbilityClass && Spec.Ability->Class == AbilityClass)
                {
                    bFromGhostSet = true;
                    break;
                }
            }
            if (bFromGhostSet)
            {
                AbilitiesToRemove.push_back(Spec.Handle);
            }
        }

        std::vector<FActiveGameplayEffectHandle> EffectHandlesToRemove;
        std::vector<UClass*> EffectClassesToRemove;
        int32 IndirectSpookyEffects = 0;
        {
            auto& ActiveEffects = AbilitySystemComponent
                ->ActiveGameplayEffects.GameplayEffects_Internal;
            for (int32 EffectIndex = 0;
                EffectIndex < ActiveEffects.Num(); EffectIndex++)
            {
                auto& ActiveEffect = ActiveEffects.Get(EffectIndex, FActiveGameplayEffect::Size());
                if (!ActiveEffect.Spec.Def)
                    continue;

                bool bFromGhostSet = false;
                if (AbilitySet->HasGrantedGameplayEffects())
                {
                    for (int32 GrantedIndex = 0;
                        GrantedIndex <AbilitySet->GrantedGameplayEffects.Num();
                        GrantedIndex++)
                    {
                        auto& GrantedEffect = AbilitySet->GrantedGameplayEffects.Get(GrantedIndex,
                                FGameplayEffectApplicationInfoHard::Size());
                        auto EffectClass = GrantedEffect.GameplayEffect.Get();
                        if (EffectClass && ActiveEffect.Spec.Def->Class == EffectClass)
                        {
                            bFromGhostSet = true;
                            break;
                        }
                    }
                }

                bool bIndirectSpookyEffect = false;
                if (ActiveEffect.Spec.Def->Class)
                {
                    const auto EffectClassName = ActiveEffect.Spec.Def->Class->Name.ToWString();
                    static const wchar_t* const
                        GhostEffectClassNames[] =
                    {
                        L"GE_CBGA_SpookyMist_C", L"GE_SpookyMist_Equipped_C",
                        L"GE_SpookyMist_FallDamageImmune_C", L"GE_SpookyMist_Speed_C",
                        L"GE_SpookyMist_GC_LoopingOnPlayer_C", L"GE_SpookyMist_GC_Trail_C",
                        L"GE_SpookyMist_GC_Wobble_C"
                    };
                    for (const auto KnownClassName : GhostEffectClassNames)
                    {
                        if (EffectClassName == KnownClassName)
                        {
                            bIndirectSpookyEffect = true;
                            break;
                        }
                    }
                }

                if (!bFromGhostSet && !bIndirectSpookyEffect)
                    continue;

                auto Handle = *reinterpret_cast<FActiveGameplayEffectHandle*>(
                        reinterpret_cast<uint8*>(&ActiveEffect) + 0xC);
                if (Handle.Handle <= 0)
                    continue;

                EffectHandlesToRemove.push_back(Handle);
                EffectClassesToRemove.push_back(ActiveEffect.Spec.Def->Class);
                if (bIndirectSpookyEffect && !bFromGhostSet) ++IndirectSpookyEffects;
            }
        }

        int32 RemovedEffects = 0;
        int32 SourceEffectFallbacks = 0;
        auto RemoveActiveEffect = AbilitySystemComponent->GetFunction("RemoveActiveGameplayEffect");
        auto RemoveBySourceEffect = AbilitySystemComponent->GetFunction(
                "RemoveActiveGameplayEffectBySourceEffect");
        std::unordered_set<UClass*> SourceFallbackClasses;
        for (size_t EffectIndex = 0;
            EffectIndex < EffectHandlesToRemove.size();
            ++EffectIndex)
        {
            bool bRemoved = false;
            if (RemoveActiveEffect)
            {
                bRemoved = AbilitySystemComponent->Call<bool>(RemoveActiveEffect,
                    EffectHandlesToRemove[EffectIndex], -1);
            }
            if (bRemoved)
            {
                ++RemovedEffects;
                continue;
            }

            if (EffectIndex < EffectClassesToRemove.size() && EffectClassesToRemove[EffectIndex])
            {
                SourceFallbackClasses.insert(EffectClassesToRemove[EffectIndex]);
            }
        }
        if (RemoveBySourceEffect)
        {
            for (auto EffectClass : SourceFallbackClasses)
            {
                AbilitySystemComponent->Call<void>(RemoveBySourceEffect, EffectClass,
                    static_cast<UAbilitySystemComponent*>(nullptr), -1);
                ++SourceEffectFallbacks;
            }
        }

        int32 RemovedAbilities = 0;
        if (ClearAbility_)
        {
            auto ClearAbility = (void(*)(UAbilitySystemComponent*,
                    FGameplayAbilitySpecHandle&))ClearAbility_;
            for (auto& AbilityHandle : AbilitiesToRemove)
            {
                ClearAbility(AbilitySystemComponent, AbilityHandle);
                ++RemovedAbilities;
            }
        }

        if (RemovedEffects > 0 || SourceEffectFallbacks > 0)
        {
            PlayerState->ForceNetUpdate();
            PlayerController->ForceNetUpdate();
        }

        if (!AbilitiesToRemove.empty() || !EffectHandlesToRemove.empty())
        {
            SDK::DbgLog("[GhostMode] removed residual gadget ability set "
                "controller=%p abilities=%d/%zu effects=%d/%zu "
                "sourceFallbacks=%d indirectSpooky=%d\n", static_cast<void*>(PlayerController),
                RemovedAbilities, AbilitiesToRemove.size(),
                RemovedEffects, EffectHandlesToRemove.size(), SourceEffectFallbacks,
                IndirectSpookyEffects);
        }
    }

    bool ClearPawnGhostModeFlag(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn || !Pawn->Class)
            return false;

        auto Property = Pawn->GetProperty("GhostMode", 0x20000);
        if (!Property)
            return false;

        const int32 Offset = static_cast<int32>(SDK::ReadPropertyOffset(GetFromOffset<uint32>(
                Property, Offsets::Offset_Internal)));
        const uint8 FieldMask = Property->GetFieldMask();
        if (Offset < 0 || FieldMask == 0 || Offset >= Pawn->Class->GetPropertiesSize() ||
            !SDK::MemReadable(reinterpret_cast<uint8*>(Pawn) + Offset, sizeof(uint8)))
        {
            return false;
        }

        auto& Value = GetFromOffset<uint8>(Pawn, Offset);
        const bool bWasSet = (Value & FieldMask) != 0;
        Value &= static_cast<uint8>(~FieldMask);
        return bWasSet;
    }

    bool ForceClearControllerGhostModeState(AFortPlayerControllerAthena* PlayerController)
    {
        if (!PlayerController || !PlayerController->Class)
            return false;

        auto RepDataStruct = FindObject<UStruct>(L"/Script/FortniteGame.GhostModeRepData");
        auto RepDataProperty = PlayerController->GetProperty("GhostModeRepData");
        auto InGhostModeProperty = RepDataStruct ? RepDataStruct->GetProperty(
                "bInGhostMode", 0x20000) : nullptr;
        if (!RepDataStruct || !RepDataProperty || !InGhostModeProperty)
        {
            return false;
        }

        const int32 RepDataOffset = static_cast<int32>(
            SDK::ReadPropertyOffset(GetFromOffset<uint32>(
                RepDataProperty, Offsets::Offset_Internal)));
        const int32 InGhostModeOffset = static_cast<int32>(
            SDK::ReadPropertyOffset(GetFromOffset<uint32>(InGhostModeProperty,
                Offsets::Offset_Internal)));
        const int32 RepDataSize = RepDataStruct->GetPropertiesSize();
        const uint8 FieldMask = InGhostModeProperty->GetFieldMask();
        if (RepDataOffset < 0 || InGhostModeOffset < 0 ||
            RepDataSize < static_cast<int32>(sizeof(uint8)) ||
            RepDataSize > 0x40 || FieldMask == 0 || InGhostModeOffset >= RepDataSize ||
            RepDataOffset > PlayerController->Class->GetPropertiesSize() - RepDataSize)
        {
            return false;
        }

        auto RepData = reinterpret_cast<uint8*>(PlayerController) + RepDataOffset;
        if (!SDK::MemReadable(RepData, static_cast<size_t>(RepDataSize)))
        {
            return false;
        }

        auto& InGhostMode = *reinterpret_cast<uint8*>(RepData + InGhostModeOffset);
        const bool bWasSet = (InGhostMode & FieldMask) != 0;
        InGhostMode &= static_cast<uint8>(~FieldMask);

        auto ItemDefinitionProperty = RepDataStruct->GetProperty("GhostModeItemDef");
        if (ItemDefinitionProperty)
        {
            const int32 ItemDefinitionOffset = static_cast<int32>(SDK::ReadPropertyOffset(
                    GetFromOffset<uint32>(ItemDefinitionProperty, Offsets::Offset_Internal)));
            const uint32 ItemDefinitionSize = GetFromOffset<uint32>(ItemDefinitionProperty,
                    Offsets::ElementSize);
            if (ItemDefinitionOffset >= 0 && ItemDefinitionSize == sizeof(UObject*) &&
                ItemDefinitionOffset <= RepDataSize - static_cast<int32>(sizeof(UObject*)))
            {
                *reinterpret_cast<UObject**>(RepData + ItemDefinitionOffset) = nullptr;
            }
        }

        if (auto OnRepGhostMode = PlayerController->GetFunction("OnRep_GhostModeRepData"))
        {
            PlayerController->ProcessEvent(OnRepGhostMode, nullptr);
        }
        PlayerController->ForceNetUpdate();
        return bWasSet;
    }

    bool RefocusHarvestingToolAfterGhostMode(AFortPlayerControllerAthena* PlayerController)
    {
        if (!PlayerController || !PlayerController->WorldInventory)
        {
            return false;
        }

        auto Entry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search(
                [](FFortItemEntry& Candidate)
                {
                    return Candidate.ItemDefinition && Candidate.ItemDefinition->ItemType ==
                            EFortItemType::GetWeaponHarvest();
                }, FFortItemEntry::Size());
        if (!Entry)
            return false;

        FGuid Guid = Entry->ItemGuid;
        auto Pawn = PlayerController->MyFortPawn ? PlayerController->MyFortPawn
            : PlayerController->Pawn ? PlayerController->Pawn->Cast<AFortPlayerPawnAthena>()
                : nullptr;
        auto CurrentWeapon = Pawn && Pawn->HasCurrentWeapon() && Pawn->CurrentWeapon
                ? Pawn->CurrentWeapon->Cast<AFortWeapon>() : nullptr;
        if (CurrentWeapon && CurrentWeapon->HasItemEntryGuid())
        {
            if (CurrentWeapon->ItemEntryGuid == Guid)
                return true;

            auto CurrentEntry = PlayerController->WorldInventory
                ->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& Candidate)
                    {
                        return Candidate.ItemGuid == CurrentWeapon->ItemEntryGuid;
                    }, FFortItemEntry::Size());
            if (CurrentEntry && CurrentEntry->ItemDefinition &&
                !UFortKismetLibrary::IsGhostModeItemDefinition(CurrentEntry->ItemDefinition))
            {
                SDK::DbgLog("[GhostMode] preserved selected weapon after exit "
                    "controller=%p definition=%s\n", static_cast<void*>(PlayerController),
                    CurrentEntry->ItemDefinition->Name.ToString().c_str());
                return false;
            }
        }

        PlayerController->ClientEquipItem(Guid, true);
        PlayerController->ServerExecuteInventoryItem(Guid);
        SDK::DbgLog("[GhostMode] retried harvesting-tool focus "
            "controller=%p definition=%s\n", static_cast<void*>(PlayerController),
            Entry->ItemDefinition->Name.ToString().c_str());
        return true;
    }

    AFortPlayerControllerAthena* ResolveGhostModeController(
        UAbilitySystemComponent* AbilitySystemComponent)
    {
        if (!AbilitySystemComponent)
            return nullptr;

        AActor* OwnerActor = AbilitySystemComponent->HasOwnerActor()
                ? AbilitySystemComponent->OwnerActor : nullptr;
        if (OwnerActor)
        {
            if (auto Controller = OwnerActor->Cast<AFortPlayerControllerAthena>())
            {
                return Controller;
            }
            if (OwnerActor->Owner)
            {
                if (auto Controller = OwnerActor->Owner->Cast<AFortPlayerControllerAthena>())
                {
                    return Controller;
                }
            }
        }

        auto Avatar = AbilitySystemComponent->HasAvatarActor() ? AbilitySystemComponent->AvatarActor
                : nullptr;
        auto Pawn = Avatar ? Avatar->Cast<AFortPlayerPawnAthena>() : nullptr;
        return Pawn && Pawn->Controller ? Pawn->Controller->Cast<AFortPlayerControllerAthena>()
            : nullptr;
    }

    double ResolveGhostModeLifetimeSeconds(const UFortItemDefinition* ItemDefinition)
    {
        constexpr double FallbackLifetimeSeconds = 45.0;
        auto Gadget = ItemDefinition ? ItemDefinition->Cast<UFortGadgetItemDefinition>() : nullptr;
        auto AbilitySet = Gadget && Gadget->HasAbilitySet() ? Gadget->AbilitySet.Get() : nullptr;
        if (!AbilitySet || !AbilitySet->HasGameplayAbilities())
            return FallbackLifetimeSeconds;

        for (int32 Index = 0;
            Index < AbilitySet->GameplayAbilities.Num(); ++Index)
        {
            auto AbilityClass = AbilitySet->GameplayAbilities[Index].Get();
            if (!AbilityClass || AbilityClass->Name.ToWString() != L"GA_SpookyMist_PassiveSetup_C")
            {
                continue;
            }

            auto AbilityDefault = AbilityClass->GetDefaultObj();
            auto DurationProperty = AbilityDefault ? AbilityDefault->GetProperty("AbilityDuration")
                : nullptr;
            if (!AbilityDefault || !AbilityDefault->Class || !DurationProperty)
                break;

            const int32 Offset = static_cast<int32>(SDK::ReadPropertyOffset(GetFromOffset<uint32>(
                    DurationProperty, Offsets::Offset_Internal)));
            const uint32 ElementSize = GetFromOffset<uint32>(
                DurationProperty, Offsets::ElementSize);
            if (Offset < 0 || ElementSize < sizeof(FScalableFloat) ||
                Offset > AbilityDefault->Class->GetPropertiesSize() -
                    static_cast<int32>(sizeof(FScalableFloat)) || !SDK::MemReadable(
                    reinterpret_cast<uint8*>(AbilityDefault) + Offset, sizeof(FScalableFloat)))
            {
                break;
            }

            auto Duration = GetFromOffset<FScalableFloat>(AbilityDefault, Offset);
            const float Evaluated = Duration.Evaluate(1.0f);
            if (std::isfinite(Evaluated) && Evaluated >= 5.0f && Evaluated <= 120.0f)
            {
                return Evaluated;
            }
            break;
        }
        return FallbackLifetimeSeconds;
    }

    auto FindTrackedGhostModeSession(AFortPlayerControllerAthena* PlayerController)
    {
        return std::find_if(TrackedGhostModeSessions.begin(), TrackedGhostModeSessions.end(),
            [&](const FTrackedGhostModeSession& Session)
            {
                return Session.Owner.Get() == PlayerController;
            });
    }

    bool RequestTrackedGhostModeExit(AFortPlayerControllerAthena* PlayerController,
        const char* Source, double DelaySeconds = 0.05)
    {
        if (!PlayerController)
            return false;

        auto Session = FindTrackedGhostModeSession(PlayerController);
        if (Session == TrackedGhostModeSessions.end())
            return false;

        auto World = UWorld::GetWorld();
        const double Now = World ? UGameplayStatics::GetTimeSeconds(World) : 0.0;
        const double RequestedTime = Now + max(DelaySeconds, 0.0);
        if (!Session->bExitRequested || RequestedTime < Session->EarliestExitCleanupTime)
        {
            Session->bExitRequested = true;
            Session->EarliestExitCleanupTime = RequestedTime;
            SDK::DbgLog("[GhostMode] observed authoritative exit signal "
                "controller=%p source=%s cleanupAt=%.2f\n", static_cast<void*>(PlayerController),
                Source ? Source : "unknown", RequestedTime);
        }
        return true;
    }

    void MarkTrackedGhostModeBackingRemoved(AFortPlayerControllerAthena* PlayerController,
        const FGuid& ItemGuid)
    {
        if (!PlayerController)
            return;

        auto Session = FindTrackedGhostModeSession(PlayerController);
        if (Session != TrackedGhostModeSessions.end() && Session->ItemGuid.A == ItemGuid.A &&
            Session->ItemGuid.B == ItemGuid.B && Session->ItemGuid.C == ItemGuid.C &&
            Session->ItemGuid.D == ItemGuid.D)
        {
            Session->bBackingRemovalObserved = true;
        }
    }

    bool ReadGhostExitFloat(AFortPlayerPawnAthena* Pawn, const char* PropertyName, float& OutValue)
    {
        OutValue = 0.0f;
        if (!Pawn || !Pawn->Class || !PropertyName)
            return false;

        const int32 Offset = Pawn->GetOffset(PropertyName);
        if (Offset < 0 || Offset > Pawn->Class->GetPropertiesSize() -
                static_cast<int32>(sizeof(float)) || !SDK::MemReadable(
                reinterpret_cast<uint8*>(Pawn) + Offset, sizeof(float)))
        {
            return false;
        }

        const float Value = GetFromOffset<float>(Pawn, Offset);
        if (!std::isfinite(Value))
            return false;
        OutValue = Value;
        return true;
    }

    bool IsControllerStillInGhostMode(AFortPlayerControllerAthena* PlayerController)
    {
        if (!PlayerController)
            return false;
        auto IsInGhostMode = PlayerController->GetFunction("IsInGhostMode");
        return IsInGhostMode && PlayerController->Call<bool>(IsInGhostMode);
    }

    void QueueGhostModeTerminalCleanup(AFortPlayerControllerAthena* PlayerController,
        const UFortItemDefinition* ItemDefinition, const FGhostCharacterPartRestore& CharacterParts,
        bool bRestoreCharacterParts)
    {
        if (!PlayerController || !UFortKismetLibrary::IsGhostModeItemDefinition(ItemDefinition))
        {
            return;
        }

        auto World = UWorld::GetWorld();
        const double Now = World ? UGameplayStatics::GetTimeSeconds(World) : 0.0;
        double EarliestFinalizeTime = Now + 0.75;
        auto Pawn = PlayerController->MyFortPawn ? PlayerController->MyFortPawn
            : PlayerController->Pawn ? PlayerController->Pawn->Cast<AFortPlayerPawnAthena>()
                : nullptr;
        float ExitStartTime = 0.0f;
        float ExitDuration = 0.0f;
        if (ReadGhostExitFloat(Pawn, "GhostModeExitDuration", ExitDuration) &&
            ExitDuration > 0.0f && ExitDuration <= 5.0f)
        {
            if (ReadGhostExitFloat(Pawn, "GhostModeExitStartTime", ExitStartTime) &&
                ExitStartTime > 0.01f && std::abs(static_cast<double>(ExitStartTime) - Now) <= 10.0)
            {
                EarliestFinalizeTime = max(Now + 0.05, static_cast<double>(ExitStartTime) +
                        ExitDuration + 0.10);
            }
            else
            {
                EarliestFinalizeTime = Now + ExitDuration + 0.10;
            }
        }

        auto Existing = std::find_if(PendingGhostModeCleanups.begin(),
            PendingGhostModeCleanups.end(), [&](const FPendingGhostModeCleanup& Pending)
            {
                return Pending.Owner.Get() == PlayerController;
            });
        FPendingGhostModeCleanup Pending{};
        Pending.Owner = TWeakObjectPtr<AFortPlayerControllerAthena>(PlayerController);
        Pending.ItemDefinition = TWeakObjectPtr<UFortItemDefinition>(
                const_cast<UFortItemDefinition*>(ItemDefinition));
        Pending.CharacterParts = CharacterParts;
        Pending.bRestoreCharacterParts = bRestoreCharacterParts;
        Pending.EarliestFinalizeTime = EarliestFinalizeTime;
        Pending.RetryDeadline = EarliestFinalizeTime + 0.50;
        Pending.ForceFinalizeDeadline = EarliestFinalizeTime + 2.0;
        if (Existing != PendingGhostModeCleanups.end())
        {
            Existing->EarliestFinalizeTime = max(Existing->EarliestFinalizeTime,
                Pending.EarliestFinalizeTime);
            Existing->RetryDeadline = max(Existing->RetryDeadline, Pending.RetryDeadline);
            Existing->ForceFinalizeDeadline = max(Existing->ForceFinalizeDeadline,
                Pending.ForceFinalizeDeadline);
            if (!Existing->ItemDefinition.Get())
                Existing->ItemDefinition = Pending.ItemDefinition;
            if (!Existing->bRestoreCharacterParts && Pending.bRestoreCharacterParts)
            {
                Existing->CharacterParts = Pending.CharacterParts;
                Existing->bRestoreCharacterParts = true;
            }
        }
        else
            PendingGhostModeCleanups.push_back(Pending);

        SDK::DbgLog("[GhostMode] queued post-transition cleanup "
            "controller=%p earliest=%.2f duration=%.2f "
            "restoreParts=%d\n", static_cast<void*>(PlayerController), EarliestFinalizeTime,
            ExitDuration, bRestoreCharacterParts ? 1 : 0);
    }

    void TickPendingGhostModeCleanups(double Now)
    {
        for (size_t Index = 0;
            Index < PendingGhostModeCleanups.size();)
        {
            auto& Pending = PendingGhostModeCleanups[Index];
            auto PlayerController = Pending.Owner.Get();
            if (!PlayerController)
            {
                PendingGhostModeCleanups.erase(PendingGhostModeCleanups.begin() + Index);
                continue;
            }

            if (Now < Pending.EarliestFinalizeTime)
            {
                ++Index;
                continue;
            }

            bool bControllerStateForceAttempted = false;
            if (IsControllerStillInGhostMode(PlayerController))
            {
                if (Now < Pending.RetryDeadline)
                {
                    ++Index;
                    continue;
                }

                if (auto EndGhostMode = PlayerController->GetFunction("EndGhostMode"))
                {
                    PlayerController->Call<void>(EndGhostMode);
                    PlayerController->ForceNetUpdate();
                }
                ++Pending.NativeEndAttempts;

                if (IsControllerStillInGhostMode(PlayerController) &&
                    Now < Pending.ForceFinalizeDeadline && Pending.NativeEndAttempts < 3)
                {
                    Pending.RetryDeadline = Now + 0.50;
                    ++Index;
                    continue;
                }

                if (IsControllerStillInGhostMode(PlayerController))
                {
                    const bool bCleared = ForceClearControllerGhostModeState(PlayerController);
                    bControllerStateForceAttempted = true;
                    SDK::DbgLog("[GhostMode] forced stale controller exit "
                        "controller=%p attempts=%d cleared=%d\n",
                        static_cast<void*>(PlayerController), Pending.NativeEndAttempts,
                        bCleared ? 1 : 0);
                }
            }

            if (Pending.FinalizationPass == 0 && !bControllerStateForceAttempted)
            {
                const bool bCleared = ForceClearControllerGhostModeState(PlayerController);
                SDK::DbgLog("[GhostMode] normalized controller exit state "
                    "controller=%p cleared=%d\n", static_cast<void*>(PlayerController),
                    bCleared ? 1 : 0);
            }

            auto ExitPawn = PlayerController->MyFortPawn ? PlayerController->MyFortPawn
                : PlayerController->Pawn ? PlayerController->Pawn->Cast<AFortPlayerPawnAthena>()
                    : nullptr;
            auto ExitPlayerState = PlayerController->PlayerState ? PlayerController->PlayerState
                        ->Cast<AFortPlayerStateAthena>() : nullptr;
            const bool bPawnIsDying = ExitPawn && ExitPawn->HasbIsDying() && ExitPawn->bIsDying;
            if (Pending.bRestoreCharacterParts && (!ExitPawn || !ExitPlayerState || bPawnIsDying))
            {
                if (!Pending.bLoggedWaitingForPawn)
                {
                    SDK::DbgLog("[GhostMode] waiting for replacement pawn before "
                        "visual restore controller=%p pawn=%p state=%p "
                        "dying=%d\n", static_cast<void*>(PlayerController),
                        static_cast<void*>(ExitPawn), static_cast<void*>(ExitPlayerState),
                        bPawnIsDying ? 1 : 0);
                    Pending.bLoggedWaitingForPawn = true;
                }
                ++Index;
                continue;
            }

            if (ExitPawn)
            {
                if (Pending.FinalizationPass == 0)
                {
                    if (auto EndGhostModeExit = ExitPawn->GetFunction("EndGhostModeExit"))
                    {
                        ExitPawn->ProcessEvent(EndGhostModeExit, nullptr);
                    }
                }

                // PlayerPawn_Athena_Generic keeps a separate Blueprint bit for CP_Body_SpookyMist, and it can share a byte with other flags.
                if (ClearPawnGhostModeFlag(ExitPawn))
                {
                    SDK::DbgLog("[GhostMode] cleared pawn presentation flag "
                        "controller=%p pawn=%p\n", static_cast<void*>(PlayerController),
                        static_cast<void*>(ExitPawn));
                }
                ExitPawn->ForceNetUpdate();
            }

            auto ItemDefinition = Pending.ItemDefinition.Get();
            if (ItemDefinition)
            {
                RemoveGhostGadgetAbilitySetFallback(PlayerController, ItemDefinition);
            }

            if (Pending.bRestoreCharacterParts)
            {
                auto& Restore = Pending.CharacterParts;
                Restore.Pawn = ExitPawn;
                Restore.PlayerState = ExitPlayerState;
                ApplyGhostCharacterPartRestore(Restore);
            }

            if (!Pending.bHarvestingToolRefocusAttempted)
            {
                Pending.bHarvestingToolRefocusAttempted = true;
                RefocusHarvestingToolAfterGhostMode(PlayerController);
            }

            SDK::DbgLog("[GhostMode] finalized post-transition cleanup "
                "controller=%p pass=%d\n", static_cast<void*>(PlayerController),
                Pending.FinalizationPass);

            static constexpr double ReassertionDelays[] =
            {
                0.25, 0.75, 1.50
            };
            if (Pending.FinalizationPass <static_cast<int32>(std::size(ReassertionDelays)))
            {
                const double Delay = ReassertionDelays[Pending.FinalizationPass];
                ++Pending.FinalizationPass;
                Pending.EarliestFinalizeTime = Now + Delay;
                Pending.RetryDeadline = Pending.EarliestFinalizeTime;
                Pending.ForceFinalizeDeadline = max(Pending.ForceFinalizeDeadline,
                    Pending.EarliestFinalizeTime + 1.0);
                ++Index;
                continue;
            }
            PendingGhostModeCleanups.erase(PendingGhostModeCleanups.begin() + Index);
        }
    }

    struct FRegeneratingInventoryItem
    {
        TWeakObjectPtr<AFortPlayerControllerAthena> Owner;
        TWeakObjectPtr<UFortAmmoItemDefinition> AmmoDefinition;
        FGuid ItemGuid{};
        int32 MaxCount = 0;
        double CooldownSeconds = 0.0;
        double NextRefillTime = 0.0;
    };

    struct FRechargingWeaponAmmo
    {
        TWeakObjectPtr<AFortPlayerControllerAthena> Owner;
        TWeakObjectPtr<UFortWeaponItemDefinition> WeaponDefinition;
        FGuid ItemGuid{};
        int32 MaxLoadedAmmo = 0;
        int32 RechargeAmount = 0;
        int32 LastObservedLoadedAmmo = 0;
        double RechargeIntervalSeconds = 0.0;
        double NextRefillTime = 0.0;
        int32 EquipSnapshotLoadedAmmo = 0;
        double EquipSnapshotNextRefillTime = 0.0;
        bool bEquipInProgress = false;
    };

    std::vector<FRegeneratingInventoryItem> RegeneratingInventoryItems;
    std::vector<FRechargingWeaponAmmo> RechargingWeaponAmmo;
    std::unordered_map<AFortPlayerControllerAthena*, std::vector<FGuid>>
        NativeDeathInventoryRetention;
    std::unordered_map<AFortPlayerControllerAthena*, FGuid> PendingCarmineFocus;

    constexpr size_t MaxTrackedRechargingWeapons = 512;
    constexpr double NativeRechargeGraceSeconds = 0.10;

    bool IsExact1040CarmineGadget(const UFortItemDefinition* Definition)
    {
        return VersionInfo.FortniteVersion == 10.40 && Definition &&
            (Definition->Name.ToWString() == L"AGID_CarminePack" || Definition->Name.ToWString() ==
                 L"AGID_AshtonPack");
    }

    bool IsExact1040BaseAshtonGadget(const UFortItemDefinition* Definition)
    {
        return VersionInfo.FortniteVersion == 10.40 && Definition && Definition->Name.ToWString() ==
                L"AGID_AshtonPack";
    }

    bool IsExact1040AshtonMiloGadget(const UFortItemDefinition* Definition)
    {
        return VersionInfo.FortniteVersion == 10.40 && Definition && Definition->Name.ToWString() ==
                L"AGID_AshtonPack_Milo";
    }

    bool FocusOrQueueExact1040Carmine(AFortPlayerControllerAthena* PlayerController,
        const FGuid& ItemGuid)
    {
        if (!PlayerController)
            return false;

        if (!PlayerController->MyFortPawn)
        {
            PendingCarmineFocus[PlayerController] = ItemGuid;
            SDK::DbgLog("[Ashton1040] queued Carmine focus until "
                "pawn possession controller=%p\n", static_cast<void*>(PlayerController));
            return true;
        }

        PlayerController->ServerExecuteInventoryItem(ItemGuid);
        PlayerController->ClientEquipItem(ItemGuid, true);
        return true;
    }

    UFortWeaponItemDefinition* ResolveExact1040CarmineBacking()
    {
        if (VersionInfo.FortniteVersion != 10.40)
            return nullptr;

        auto CarmineGadget = const_cast<UFortGadgetItemDefinition*>(
                FindObject<UFortGadgetItemDefinition>(L"/Game/Athena/Items/Gameplay/BackPacks/"
                    L"CarminePack/AGID_CarminePack."
                    L"AGID_CarminePack"));
        auto CarmineBacking = const_cast<UFortWeaponItemDefinition*>(
                FindObject<UFortWeaponItemDefinition>(L"/Game/Athena/Items/Gameplay/BackPacks/"
                    L"CarminePack/D_CarminePack."
                    L"D_CarminePack"));
        if (!CarmineGadget || !CarmineBacking)
            return nullptr;

        CarmineGadget->AddToRoot();
        CarmineBacking->AddToRoot();
        return CarmineBacking;
    }

    const UFortItemDefinition* ResolveExact1040AshtonGadgetAlias(
            const UFortItemDefinition* Definition)
    {
        if (!IsExact1040BaseAshtonGadget(Definition))
            return Definition;

        auto CarmineGadget = const_cast<UFortGadgetItemDefinition*>(
                FindObject<UFortGadgetItemDefinition>(L"/Game/Athena/Items/Gameplay/BackPacks/"
                    L"CarminePack/AGID_CarminePack."
                    L"AGID_CarminePack"));
        if (!CarmineGadget)
            return Definition;

        CarmineGadget->AddToRoot();
        static bool bLoggedAlias = false;
        if (!bLoggedAlias)
        {
            bLoggedAlias = true;
            SDK::DbgLog("[Ashton1040] aliased incomplete "
                "AGID_AshtonPack to authored AGID_CarminePack "
                "before inventory replication\n");
        }
        return CarmineGadget;
    }

    void PreloadExact1040CarmineDependencies()
    {
        ResolveExact1040CarmineBacking();

        auto CarmineAbilitySet = FindObject<UFortAbilitySet>(
            L"/Game/Athena/Items/Gameplay/BackPacks/"
            L"CarminePack/AS_CarminePack.AS_CarminePack");
        if (CarmineAbilitySet)
        {
            const_cast<UFortAbilitySet*>(CarmineAbilitySet)->AddToRoot();
        }
        auto AshtonAbilitySet = FindObject<UFortAbilitySet>(
            L"/Game/Athena/Items/Gameplay/BackPacks/"
            L"Ashton/AS_AshtonPack.AS_AshtonPack");
        if (AshtonAbilitySet)
        {
            const_cast<UFortAbilitySet*>(AshtonAbilitySet)->AddToRoot();
        }

        static constexpr const wchar_t* ClassPaths[] = {
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_CarminePack_PassiveSetup."
                L"GA_CarminePack_PassiveSetup_C", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_CarminePack_DashOrSmash."
                L"GA_CarminePack_DashOrSmash_C", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_CarminePack_Jump_NotMoving."
                L"GA_CarminePack_Jump_NotMoving_C", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_CarminePack_Punch."
                L"GA_CarminePack_Punch_C", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_CarminePack_LifeSteal."
                L"GA_CarminePack_LifeSteal_C", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_AshtonPack_Carmine_GemPickup_Passive."
                L"GA_AshtonPack_Carmine_GemPickup_Passive_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_AshtonPack_GemPickupFX."
                L"GA_AshtonPack_GemPickupFX_C", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/DA_CarminePack."
                L"DA_CarminePack_C", L"/Game/Athena/VaultedGameplayCueNotifies/"
                L"Carmine/GCN_Carmine_Beam."
                L"GCN_Carmine_Beam_C", L"/Game/Athena/VaultedGameplayCueNotifies/"
                L"Carmine/GCL_Carmine_Beam_Loop."
                L"GCL_Carmine_Beam_Loop_C", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_GC_Beam_Loop."
                L"GE_Carmine_GC_Beam_Loop_C", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_Beam_Damage."
                L"GE_Carmine_Beam_Damage_C", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_Beam_Damage_P."
                L"GE_Carmine_Beam_Damage_P_C", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Ashton_Carmine_LockInPlace."
                L"GE_Ashton_Carmine_LockInPlace_C", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_AbilityBlocker."
                L"GE_Carmine_AbilityBlocker_C", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_DamageImmune."
                L"GE_Carmine_DamageImmune_C", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_GC_Aura."
                L"GE_Carmine_GC_Aura_C", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_GC_Skydive."
                L"GE_Carmine_GC_Skydive_C", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Ashton_Carmine_GemPickUpAnim."
                L"GE_Ashton_Carmine_GemPickUpAnim_C", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Ashton_Carmine_FinalGemPickUpAnim."
                L"GE_Ashton_Carmine_FinalGemPickUpAnim_C", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_AshtonPack_Carmine_StonePickUpAnim."
                L"GA_AshtonPack_Carmine_StonePickUpAnim_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_AshtonPack_Carmine_FinalGem."
                L"GA_AshtonPack_Carmine_FinalGem_C", L"/Game/Athena/VaultedGameplayCueNotifies/"
                L"Carmine/GCN_Carmine_Transform."
                L"GCN_Carmine_Transform_C", L"/Game/Athena/VaultedGameplayCueNotifies/"
                L"Carmine/GCL_Carmine_Skydive."
                L"GCL_Carmine_Skydive_C", L"/Game/Blueprints/Camera/Athena/"
                L"Athena_PlayerCameraModeCarmineSpawn."
                L"Athena_PlayerCameraModeCarmineSpawn_C", L"/Game/Blueprints/Camera/Athena/"
                L"Athena_PlayerCameraModeCarmine_Beam."
                L"Athena_PlayerCameraModeCarmine_Beam_C",
            L"/Game/Characters/Player/Male/Male_Avg_Base/"
                L"Gauntlet_Player_AnimBlueprint."
                L"Gauntlet_Player_AnimBlueprint_C"
        };
        for (const auto Path : ClassPaths)
        {
            auto Class = FindObject<UClass>(Path);
            if (Class)
                Class->AddToRoot();
        }

        static constexpr const wchar_t* VisualPaths[] = {
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/FX/P_Jim_LaserBlast."
                L"P_Jim_LaserBlast", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/FX/P_Jim_LaserBlast_Muzzle."
                L"P_Jim_LaserBlast_Muzzle", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/FX/P_Jim_LaserBlast_Dust."
                L"P_Jim_LaserBlast_Dust", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/FX/P_Jim_LaserBlast_Impact."
                L"P_Jim_LaserBlast_Impact", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/FX/P_Jim_LaserBlast_Impact_Player."
                L"P_Jim_LaserBlast_Impact_Player",
            L"/Game/Animation/Game/MainPlayer/Skydive/Freefall/"
                L"Custom/Jim/Transitions/Spawn_Montage."
                L"Spawn_Montage", L"/Game/Animation/Game/MainPlayer/Combat/Gadgets/"
                L"ExtraLarge/Jim/Jim_FistBeam_Montage."
                L"Jim_FistBeam_Montage", L"/Game/Animation/Game/MainPlayer/Combat/Gadgets/"
                L"ExtraLarge/Jim/Jim_FistBeam_Outro_M."
                L"Jim_FistBeam_Outro_M", L"/Game/Animation/Game/MainPlayer/Combat/Gadgets/"
                L"ExtraLarge/Jim/Jim_PowerUp_Montage."
                L"Jim_PowerUp_Montage", L"/Game/Animation/Game/MainPlayer/Combat/Gadgets/"
                L"ExtraLarge/Jim/Jim_Victory_Montage."
                L"Jim_Victory_Montage"
        };
        for (const auto Path : VisualPaths)
        {
            auto Asset = FindObject<UObject>(Path);
            if (Asset)
                const_cast<UObject*>(Asset)->AddToRoot();
        }
    }

    void PreloadExact1040AshtonMiloDependencies()
    {
        auto AbilitySet = FindObject<UFortAbilitySet>(L"/Game/Athena/Items/Gameplay/BackPacks/"
            L"Ashton/Milo/AS_AshtonPack_Milo."
            L"AS_AshtonPack_Milo");
        if (AbilitySet)
        {
            const_cast<UFortAbilitySet*>(AbilitySet)->AddToRoot();
        }

        auto BoostAbilitySet = FindObject<UFortAbilitySet>(L"/Game/Athena/Items/Gameplay/BackPacks/"
            L"BoostJumpPack/AS_BoostJumpPack."
            L"AS_BoostJumpPack");
        if (BoostAbilitySet)
        {
            const_cast<UFortAbilitySet*>(BoostAbilitySet)->AddToRoot();
        }

        static constexpr const wchar_t* ClassPaths[] = {
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"Ashton/GA_AshtonPack_EMPTYABILITY."
                L"GA_AshtonPack_EMPTYABILITY_C", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"Ashton/Milo/GA_AshtonPack_EquipWeapon_Milo."
                L"GA_AshtonPack_EquipWeapon_Milo_C", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"Ashton/Milo/GA_AshtonPack_PassiveSetup_Milo."
                L"GA_AshtonPack_PassiveSetup_Milo_C", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"Ashton/Milo/GA_AshtonPack_Milo_BlockAbilities."
                L"GA_AshtonPack_Milo_BlockAbilities_C", L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"Ashton/Milo/GAT_AshtonPack_Milo_GemPickupHeal."
                L"GAT_AshtonPack_Milo_GemPickupHeal_C",
            L"/Game/Weapons/FORT_Rifles/Blueprints/Assault/"
                L"B_Assault_AshtonPack_Milo."
                L"B_Assault_AshtonPack_Milo_C", L"/Game/Weapons/FORT_Rifles/Blueprints/"
                L"B_Rifle_AshtonPack_Milo_Launcher."
                L"B_Rifle_AshtonPack_Milo_Launcher_C", L"/Game/Athena/Items/Weapons/Abilities/"
                L"GA_Ranged_Ashton_Milo_Explosive_Athena."
                L"GA_Ranged_Ashton_Milo_Explosive_Athena_C", L"/Game/Abilities/Weapons/Ranged/"
                L"GA_Ranged_GenericDamage."
                L"GA_Ranged_GenericDamage_C", L"/Game/Weapons/FORT_RocketLaunchers/Blueprints/"
                L"B_Prj_AshtonPack_Milo_Launcher."
                L"B_Prj_AshtonPack_Milo_Launcher_C"
        };
        for (const auto Path : ClassPaths)
        {
            auto Class = FindObject<UClass>(Path);
            if (Class)
                Class->AddToRoot();
        }
    }

    bool AreGuidsEqual(const FGuid& Left, const FGuid& Right)
    {
        return Left.A == Right.A && Left.B == Right.B && Left.C == Right.C && Left.D == Right.D;
    }

    bool IsSameRegenItem(const FRegeneratingInventoryItem& State,
        const AFortPlayerControllerAthena* Owner, const FGuid& ItemGuid)
    {
        return State.Owner.Get() == Owner && AreGuidsEqual(State.ItemGuid, ItemGuid);
    }

    void RemoveRegenItemAt(size_t Index)
    {
        if (Index + 1 != RegeneratingInventoryItems.size())
        {
            RegeneratingInventoryItems[Index] = RegeneratingInventoryItems.back();
        }
        RegeneratingInventoryItems.pop_back();
    }

    void RemoveRechargingWeaponAt(size_t Index)
    {
        if (Index + 1 != RechargingWeaponAmmo.size())
        {
            RechargingWeaponAmmo[Index] = RechargingWeaponAmmo.back();
        }
        RechargingWeaponAmmo.pop_back();
    }

    bool IsNitroFistsDefinition(const UFortWeaponItemDefinition* WeaponDefinition)
    {
        if (!WeaponDefinition || VersionInfo.FortniteVersion < 30.0 ||
            VersionInfo.FortniteVersion >= 31.0)
        {
            return false;
        }

        const auto DefinitionName = WeaponDefinition->Name.ToString();
        return DefinitionName.rfind("WID_Moonflax_NitroGauntlet", 0) == 0;
    }

    bool NotifyWeaponRechargeStarted(AFortPlayerControllerAthena* Owner, const FGuid& ItemGuid,
        double ServerStartTime)
    {
        if (!Owner || !std::isfinite(ServerStartTime))
            return false;

        auto RechargeComponentClass = FindClass("FortControllerComponent_RechargeWeapons");
        auto RechargeComponent = RechargeComponentClass ? Owner->GetComponentByClass(
                    RechargeComponentClass) : nullptr;
        UObject* NotificationTarget = RechargeComponent;
        auto ClientStartedFunction = NotificationTarget ? NotificationTarget->GetFunction(
                    "ClientItemStartedRecharging") : nullptr;
        if (!ClientStartedFunction)
        {
            NotificationTarget = static_cast<UObject*>(Owner);
            ClientStartedFunction = Owner->GetFunction("ClientItemStartedRecharging");
        }
        if (!ClientStartedFunction)
            return false;

        const auto Params = ClientStartedFunction->GetParamsNamed();
        const size_t AllocationSize = static_cast<size_t>(Params.Size);
        if (AllocationSize < sizeof(FGuid) + sizeof(float) || AllocationSize > 0x4000)
        {
            return false;
        }

        uint32 GuidOffset = uint32(-1);
        uint32 StartTimeOffset = uint32(-1);
        for (const auto& Param : Params.NameOffsetMap)
        {
            if (Param.Name == "ItemGuid")
            {
                if (GuidOffset != uint32(-1))
                    return false;
                GuidOffset = Param.Offset;
            }
            else if (Param.Name == "InServerStartTime" || Param.Name == "ServerStartTime")
            {
                if (StartTimeOffset != uint32(-1))
                    return false;
                StartTimeOffset = Param.Offset;
            }
            else
            {
                return false;
            }
        }
        if (GuidOffset == uint32(-1) || StartTimeOffset == uint32(-1) ||
            GuidOffset + sizeof(ItemGuid) > AllocationSize ||
            StartTimeOffset + sizeof(float) > AllocationSize)
        {
            return false;
        }

        auto Memory = FMemory::Malloc(AllocationSize);
        if (!Memory)
            return false;
        memset(Memory, 0, AllocationSize);
        const float StartTime = static_cast<float>(ServerStartTime);
        memcpy(static_cast<uint8*>(Memory) + GuidOffset, &ItemGuid, sizeof(ItemGuid));
        memcpy(static_cast<uint8*>(Memory) + StartTimeOffset, &StartTime, sizeof(StartTime));
        NotificationTarget->ProcessEvent(ClientStartedFunction, Memory);
        FMemory::Free(Memory);
        return true;
    }

    AFortWeapon* ResolveEquippedWeaponForItem(AFortPlayerControllerAthena* Owner,
        UFortWeaponItemDefinition* WeaponDefinition, const FGuid& ItemGuid)
    {
        if (!Owner || !WeaponDefinition || !Owner->HasMyFortPawn() || !Owner->MyFortPawn ||
            !Owner->MyFortPawn->HasCurrentWeapon())
        {
            return nullptr;
        }

        auto WeaponActor = Owner->MyFortPawn->CurrentWeapon;
        auto FortWeaponClass = AFortWeapon::StaticClass();
        if (!WeaponActor || !FortWeaponClass || !WeaponActor->IsA(FortWeaponClass))
        {
            return nullptr;
        }

        auto Weapon = static_cast<AFortWeapon*>(WeaponActor);
        if (!Weapon->HasItemEntryGuid() || !AreGuidsEqual(Weapon->ItemEntryGuid, ItemGuid) ||
            (Weapon->HasWeaponData() && Weapon->WeaponData != WeaponDefinition) ||
            !Weapon->HasAmmoCount())
        {
            return nullptr;
        }

        return Weapon;
    }

    bool IsLiveRechargeObject(const UObject* Object)
    {
        if (!Object || !SDK::MemReadable(Object, sizeof(UObject)))
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
            Object->Class && SDK::MemReadable(Object->Class, sizeof(UClass));
    }

    bool SyncWeaponAmmo(AFortWeapon* Weapon, int32 NewLoadedAmmo)
    {
        if (!Weapon || !Weapon->HasAmmoCount())
            return false;

        const int32 OldAmmoCount = Weapon->AmmoCount;
        if (OldAmmoCount != NewLoadedAmmo)
        {
            Weapon->AmmoCount = NewLoadedAmmo;
            if (auto OnRepAmmoCount = Weapon->GetFunction("OnRep_AmmoCount"))
            {
                Weapon->Call<void>(OnRepAmmoCount, OldAmmoCount);
            }
        }

        VersionFeatureAdapter::MarkReplicatedPropertyDirty(Weapon, L"AmmoCount");
        Weapon->ForceNetUpdate();
        return true;
    }

    int32 SyncKnownWeaponAmmo(AFortPlayerControllerAthena* Owner,
        UFortWeaponItemDefinition* WeaponDefinition, const FGuid& ItemGuid, int32 NewLoadedAmmo)
    {
        if (!Owner || !WeaponDefinition || !Owner->HasMyFortPawn() ||
            !IsLiveRechargeObject(Owner->MyFortPawn))
        {
            return 0;
        }

        auto Pawn = Owner->MyFortPawn;
        std::unordered_set<AFortWeapon*> Seen;
        int32 SyncedCount = 0;
        auto SyncCandidate = [&](AActor* Candidate)
        {
            if (!IsLiveRechargeObject(Candidate))
                return;

            auto Weapon = Candidate->Cast<AFortWeapon>();
            if (!Weapon || !Seen.insert(Weapon).second || !Weapon->HasItemEntryGuid() ||
                !AreGuidsEqual(Weapon->ItemEntryGuid, ItemGuid) || (Weapon->HasWeaponData() &&
                    Weapon->WeaponData != WeaponDefinition) || !Weapon->HasAmmoCount())
            {
                return;
            }

            if (SyncWeaponAmmo(Weapon, NewLoadedAmmo)) ++SyncedCount;
        };

        if (Pawn->HasCurrentWeapon())
            SyncCandidate(Pawn->CurrentWeapon);
        if (Pawn->HasPreviousWeapon())
            SyncCandidate(Pawn->PreviousWeapon);

        if (Pawn->HasCurrentWeaponList())
        {
            const auto& Weapons = Pawn->CurrentWeaponList;
            const int32 WeaponCount = Weapons.Num();
            if (WeaponCount >= 0 && WeaponCount <= 64 && Weapons.Max() >= WeaponCount &&
                Weapons.Max() <= 128 && (WeaponCount == 0 || (Weapons.Data && SDK::MemReadable(
                            Weapons.Data, static_cast<size_t>(WeaponCount) * sizeof(AActor*)))))
            {
                for (int32 Index = 0;
                    Index < WeaponCount;
                    ++Index)
                {
                    SyncCandidate(Weapons.Get(Index));
                }
            }
        }

        return SyncedCount;
    }

    bool TryEvaluateWeaponRechargeGetter(UFortWeaponItemDefinition* WeaponDefinition,
        const char* FunctionName, int32 ItemLevel, float& OutValue)
    {
        OutValue = 0.0f;
        if (!WeaponDefinition || !FunctionName)
            return false;

        auto Function = WeaponDefinition->GetFunction(FunctionName);
        if (!Function)
            return false;

        const auto Params = Function->GetParamsNamed();
        const size_t AllocationSize = static_cast<size_t>(Params.Size);
        if (AllocationSize < sizeof(float) || AllocationSize > 0x4000)
        {
            return false;
        }

        uint32 LevelOffset = uint32(-1);
        uint32 ReturnOffset = uint32(-1);
        for (const auto& Param : Params.NameOffsetMap)
        {
            if (Param.Name == "InLevel" || Param.Name == "Level")
            {
                if (LevelOffset != uint32(-1))
                    return false;
                LevelOffset = Param.Offset;
            }
            else if (Param.Name == "ReturnValue")
            {
                if (ReturnOffset != uint32(-1))
                    return false;
                ReturnOffset = Param.Offset;
            }
            else
            {
                return false;
            }
        }

        if (LevelOffset == uint32(-1) || ReturnOffset == uint32(-1) ||
            LevelOffset + sizeof(ItemLevel) > AllocationSize ||
            ReturnOffset + sizeof(OutValue) > AllocationSize)
        {
            return false;
        }

        auto Memory = FMemory::Malloc(AllocationSize);
        if (!Memory)
            return false;
        memset(Memory, 0, AllocationSize);
        memcpy(static_cast<uint8*>(Memory) + LevelOffset, &ItemLevel, sizeof(ItemLevel));
        WeaponDefinition->ProcessEvent(Function, Memory);
        memcpy(&OutValue, static_cast<uint8*>(Memory) + ReturnOffset, sizeof(OutValue));
        FMemory::Free(Memory);
        return std::isfinite(OutValue);
    }

    bool ResolveWeaponRechargeSettings(UFortWeaponItemDefinition* WeaponDefinition, int32 ItemLevel,
        int32& MaxLoadedAmmo, int32& RechargeAmount, double& RechargeIntervalSeconds)
    {
        if (!WeaponDefinition)
            return false;

        const bool bNitroFists = IsNitroFistsDefinition(WeaponDefinition);
        const auto DefinitionName = WeaponDefinition->Name.ToString();

        if (!bNitroFists && (!WeaponDefinition->HasbRechargeAmmoToClip() ||
                !WeaponDefinition->bRechargeAmmoToClip))
        {
            return false;
        }

        auto Stats = AFortInventory::GetStats(WeaponDefinition);
        MaxLoadedAmmo = Stats ? Stats->ClipSize : 0;

        float RechargeQuantityValue = 0.0f;
        const bool bHasRechargeQuantityProperty = WeaponDefinition->HasWeaponRechargeAmmoQuantity();
        if (bHasRechargeQuantityProperty)
        {
            auto Quantity = WeaponDefinition->WeaponRechargeAmmoQuantity;
            if (std::isfinite(Quantity.Value) && Quantity.Value > 0.0f)
            {
                RechargeQuantityValue = Quantity.Evaluate(static_cast<float>(max(ItemLevel, 1)));
            }
        }
        else
        {
            TryEvaluateWeaponRechargeGetter(WeaponDefinition, "GetWeaponRechargeAmmoQuantity",
                max(ItemLevel, 1), RechargeQuantityValue);
        }
        RechargeAmount = std::isfinite(RechargeQuantityValue) && RechargeQuantityValue > 0.0f &&
                RechargeQuantityValue <= 10000.0f ? static_cast<int32>(
                  std::round(RechargeQuantityValue)) : 0;

        float RechargeRateValue = 0.0f;
        const bool bHasRechargeRateProperty = WeaponDefinition->HasWeaponRechargeAmmoRate();
        if (bHasRechargeRateProperty)
        {
            auto Rate = WeaponDefinition->WeaponRechargeAmmoRate;
            if (std::isfinite(Rate.Value) && Rate.Value > 0.0f)
            {
                RechargeRateValue = Rate.Evaluate(static_cast<float>(max(ItemLevel, 1)));
            }
        }
        else
        {
            TryEvaluateWeaponRechargeGetter(WeaponDefinition, "GetWeaponRechargeAmmoRate",
                max(ItemLevel, 1), RechargeRateValue);
        }
        RechargeIntervalSeconds = static_cast<double>(RechargeRateValue);

        // Nitro Fists use a gauntlet stat row on FN30, so ClipSize is not reliable there.
        if (bNitroFists)
        {
            MaxLoadedAmmo = DefinitionName.find("_Mythic") != std::string::npos ? 5 : 4;
            if (RechargeAmount <= 0 || RechargeAmount > MaxLoadedAmmo)
            {
                RechargeAmount = 1;
            }
            if (!std::isfinite(RechargeIntervalSeconds) || RechargeIntervalSeconds <= 0.0 ||
                RechargeIntervalSeconds > 300.0)
            {
                RechargeIntervalSeconds = 8.0;
            }
        }

        return MaxLoadedAmmo > 0 && MaxLoadedAmmo <= 10000 && RechargeAmount > 0 &&
            RechargeAmount <= MaxLoadedAmmo && std::isfinite(RechargeIntervalSeconds) &&
            RechargeIntervalSeconds > 0.0 && RechargeIntervalSeconds <= 3600.0;
    }

    void ObserveRechargingWeaponAmmo(AFortPlayerControllerAthena* Owner,
        UFortWeaponItemDefinition* WeaponDefinition, const FGuid& ItemGuid, int32 ItemLevel,
        int32 PreviousLoadedAmmo, int32 NewLoadedAmmo)
    {
        if (!Owner || !Owner->WorldInventory || !WeaponDefinition)
        {
            return;
        }

        auto World = UWorld::GetWorld();
        if (!World)
            return;

        const double NowSeconds = UGameplayStatics::GetTimeSeconds(World);
        for (size_t Index = 0;
            Index < RechargingWeaponAmmo.size();
            ++Index)
        {
            auto& State = RechargingWeaponAmmo[Index];
            if (State.Owner.Get() != Owner || !AreGuidsEqual(State.ItemGuid, ItemGuid))
            {
                continue;
            }

            State.WeaponDefinition = TWeakObjectPtr<UFortWeaponItemDefinition>(WeaponDefinition);
            if (State.bEquipInProgress)
                return;

            State.LastObservedLoadedAmmo = std::clamp(NewLoadedAmmo, 0, State.MaxLoadedAmmo);
            bool bStartedRechargeCycle = false;

            if (NewLoadedAmmo >= State.MaxLoadedAmmo)
            {
                State.NextRefillTime = 0.0;
            }
            else if (NewLoadedAmmo > PreviousLoadedAmmo)
            {
                State.NextRefillTime = NowSeconds + State.RechargeIntervalSeconds +
                    NativeRechargeGraceSeconds;
                bStartedRechargeCycle = true;
            }
            else if (NewLoadedAmmo < PreviousLoadedAmmo && State.NextRefillTime <= 0.0)
            {
                State.NextRefillTime = NowSeconds + State.RechargeIntervalSeconds +
                    NativeRechargeGraceSeconds;
                bStartedRechargeCycle = true;
            }
            if (bStartedRechargeCycle)
            {
                NotifyWeaponRechargeStarted(Owner, ItemGuid, NowSeconds);
            }
            return;
        }

        int32 MaxLoadedAmmo = 0;
        int32 RechargeAmount = 0;
        double RechargeIntervalSeconds = 0.0;
        if (!ResolveWeaponRechargeSettings(WeaponDefinition, ItemLevel, MaxLoadedAmmo,
                RechargeAmount, RechargeIntervalSeconds))
        {
            return;
        }

        if (RechargingWeaponAmmo.size() >= MaxTrackedRechargingWeapons)
        {
            RechargingWeaponAmmo.erase(std::remove_if(RechargingWeaponAmmo.begin(),
                    RechargingWeaponAmmo.end(), [](const FRechargingWeaponAmmo& State)
                    {
                        return !State.Owner.Get() || !State.WeaponDefinition.Get();
                    }), RechargingWeaponAmmo.end());
        }

        if (RechargingWeaponAmmo.size() >= MaxTrackedRechargingWeapons)
        {
            return;
        }

        FRechargingWeaponAmmo State{};
        State.Owner = TWeakObjectPtr<AFortPlayerControllerAthena>(Owner);
        State.WeaponDefinition = TWeakObjectPtr<UFortWeaponItemDefinition>(WeaponDefinition);
        State.ItemGuid = ItemGuid;
        State.MaxLoadedAmmo = MaxLoadedAmmo;
        State.RechargeAmount = RechargeAmount;
        State.LastObservedLoadedAmmo = std::clamp(NewLoadedAmmo, 0, MaxLoadedAmmo);
        State.RechargeIntervalSeconds = RechargeIntervalSeconds;
        if (NewLoadedAmmo < MaxLoadedAmmo)
        {
            State.NextRefillTime = NowSeconds + RechargeIntervalSeconds +
                NativeRechargeGraceSeconds;
        }
        RechargingWeaponAmmo.push_back(State);

        const bool bClientTimerStarted = NewLoadedAmmo < MaxLoadedAmmo &&
            NotifyWeaponRechargeStarted(Owner, ItemGuid, NowSeconds);

        auto RechargeComponentClass = FindClass("FortControllerComponent_RechargeWeapons");
        auto RechargeComponent = RechargeComponentClass ? Owner->GetComponentByClass(
                    RechargeComponentClass) : nullptr;
        SDK::DbgLog("[WeaponRecharge] registered definition=%s ammo=%d/%d "
            "amount=%d interval=%.2f nativeComponent=%s\n",
            WeaponDefinition->Name.ToString().c_str(), NewLoadedAmmo, MaxLoadedAmmo, RechargeAmount,
            RechargeIntervalSeconds, RechargeComponent ? "present" : "missing");
        if (NewLoadedAmmo < MaxLoadedAmmo)
        {
            SDK::DbgLog("[WeaponRecharge] client-timer definition=%s "
                "started=%d serverStart=%.3f\n", WeaponDefinition->Name.ToString().c_str(),
                bClientTimerStarted ? 1 : 0, NowSeconds);
        }
    }

    bool IsTrackedRechargingWeaponAmmo(const AFortPlayerControllerAthena* Owner,
        const FGuid& ItemGuid)
    {
        return std::any_of(RechargingWeaponAmmo.begin(), RechargingWeaponAmmo.end(),
            [&](const FRechargingWeaponAmmo& State)
            {
                return State.Owner.Get() == Owner && AreGuidsEqual(State.ItemGuid, ItemGuid);
            });
    }

    void BroadcastWorldItemAmmoChanged(UFortWorldItem* Item)
    {
        if (!Item)
            return;

        static auto BroadcastFunction = Item->GetFunction("BroadcastOnItemChanged");
        if (BroadcastFunction)
        {
            Item->Call<void>(BroadcastFunction, false, true, false, false);
        }
    }

    void ScheduleRegeneratingInventoryItem(AFortPlayerControllerAthena* Owner,
        UFortAmmoItemDefinition* AmmoDefinition, const FGuid& ItemGuid, int32 MaxCount,
        double CooldownSeconds)
    {
        if (!Owner || !Owner->WorldInventory || !AmmoDefinition ||
            MaxCount <= 0 || !std::isfinite(CooldownSeconds) ||
            CooldownSeconds <= 0.0 || CooldownSeconds > 3600.0)
        {
            return;
        }

        auto ReplicatedEntry = Owner->WorldInventory->Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Candidate)
                {
                    return AreGuidsEqual(Candidate.ItemGuid, ItemGuid) &&
                        Candidate.ItemDefinition == AmmoDefinition;
                }, FFortItemEntry::Size());
        if (!ReplicatedEntry || ReplicatedEntry->Count >= MaxCount)
            return;

        for (auto& State : RegeneratingInventoryItems)
        {
            if (!IsSameRegenItem(State, Owner, ItemGuid))
                continue;

            State.AmmoDefinition = TWeakObjectPtr<UFortAmmoItemDefinition>(AmmoDefinition);
            State.MaxCount = max(State.MaxCount, MaxCount);
            State.CooldownSeconds = CooldownSeconds;
            return;
        }

        auto World = UWorld::GetWorld();
        if (!World)
            return;

        FRegeneratingInventoryItem State{};
        State.Owner = TWeakObjectPtr<AFortPlayerControllerAthena>(Owner);
        State.AmmoDefinition = TWeakObjectPtr<UFortAmmoItemDefinition>(AmmoDefinition);
        State.ItemGuid = ItemGuid;
        State.MaxCount = MaxCount;
        State.CooldownSeconds = CooldownSeconds;
        State.NextRefillTime = UGameplayStatics::GetTimeSeconds(World) + CooldownSeconds;
        RegeneratingInventoryItems.push_back(State);

        SDK::DbgLog("[ItemRegen] scheduled definition=%s count=%d max=%d cooldown=%.2f\n",
            AmmoDefinition->Name.ToString().c_str(), ReplicatedEntry->Count, MaxCount,
            CooldownSeconds);
    }

    bool HasTrackedGhostBackingItem(const FTrackedGhostModeSession& Session,
        const UFortItemDefinition*& OutDefinition)
    {
        OutDefinition = Session.ItemDefinition.Get();
        auto PlayerController = Session.Owner.Get();
        if (!PlayerController || !PlayerController->WorldInventory)
            return false;

        auto Entry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Candidate)
                {
                    return AreGuidsEqual(Candidate.ItemGuid, Session.ItemGuid);
                }, FFortItemEntry::Size());
        if (!Entry || !UFortKismetLibrary::IsGhostModeItemDefinition(Entry->ItemDefinition))
        {
            return false;
        }

        OutDefinition = Entry->ItemDefinition;
        return true;
    }

    void BeginTrackedGhostModeCleanup(FTrackedGhostModeSession& Session, const char* Source)
    {
        auto PlayerController = Session.Owner.Get();
        if (!PlayerController)
            return;

        const UFortItemDefinition* ItemDefinition = nullptr;
        const bool bHadBackingItem = HasTrackedGhostBackingItem(Session, ItemDefinition);
        if (!ItemDefinition)
            ItemDefinition = Session.ItemDefinition.Get();
        if (!ItemDefinition)
            return;

        SDK::DbgLog("[GhostMode] beginning tracked terminal cleanup "
            "controller=%p source=%s backing=%d\n", static_cast<void*>(PlayerController),
            Source ? Source : "unknown", bHadBackingItem ? 1 : 0);

        if (bHadBackingItem)
        {
            UFortKismetLibrary::CleanupGhostMode(PlayerController, true);
            return;
        }

        FGhostCharacterPartRestore Restore{};
        const bool bRestoreCharacterParts = CaptureGhostCharacterPartRestore(PlayerController,
                ItemDefinition, Restore);
        UFortKismetLibrary::CleanupGhostMode(PlayerController, false);
        UFortKismetLibrary::NotifyGhostModeItemRemoved(PlayerController, ItemDefinition);
        QueueGhostModeTerminalCleanup(PlayerController, ItemDefinition, Restore,
            bRestoreCharacterParts);
    }

    void TickTrackedGhostModeSessions(double Now)
    {
        for (size_t Index = 0;
            Index < TrackedGhostModeSessions.size();)
        {
            auto& Session = TrackedGhostModeSessions[Index];
            auto PlayerController = Session.Owner.Get();
            if (!PlayerController || !PlayerController->WorldInventory)
            {
                TrackedGhostModeSessions.erase(TrackedGhostModeSessions.begin() + Index);
                continue;
            }

            const UFortItemDefinition* ItemDefinition = nullptr;
            const bool bBackingItemPresent = HasTrackedGhostBackingItem(Session, ItemDefinition);
            if (!bBackingItemPresent)
            {
                if (!Session.bBackingRemovalObserved)
                {
                    BeginTrackedGhostModeCleanup(Session, "backing-item-falling-edge");
                }
                TrackedGhostModeSessions.erase(TrackedGhostModeSessions.begin() + Index);
                continue;
            }

            if (!Session.bExitRequested)
            {
                const bool bControllerInGhostMode = IsControllerStillInGhostMode(PlayerController);
                if (bControllerInGhostMode)
                {
                    Session.bObservedControllerGhostMode = true;
                }
                else if (Session.bObservedControllerGhostMode)
                {
                    RequestTrackedGhostModeExit(PlayerController, "controller-ghost-falling-edge",
                        0.05);
                }

                auto Pawn = PlayerController->MyFortPawn ? PlayerController->MyFortPawn
                    : PlayerController->Pawn ? PlayerController->Pawn->Cast<AFortPlayerPawnAthena>()
                        : nullptr;
                float ExitStartTime = 0.0f;
                if (ReadGhostExitFloat(Pawn, "GhostModeExitStartTime", ExitStartTime))
                {
                    const bool bChangedFromBaseline = Session.bHasInitialPawnExitStartTime
                            ? std::abs(ExitStartTime - Session.InitialPawnExitStartTime) > 0.01f
                            : ExitStartTime > 0.0f;
                    if (bChangedFromBaseline && ExitStartTime >= 0.0f && std::abs(
                            static_cast<double>(ExitStartTime) - Now) <= 10.0)
                    {
                        RequestTrackedGhostModeExit(PlayerController, "pawn-exit-transition", 0.05);
                    }
                }
            }

            if (!Session.bExitRequested && Now >= Session.ExpireAt)
            {
                // The authored Shadow Stone duration comes from GA_SpookyMist_PassiveSetup - 45 seconds on 6.21.
                Session.bExitRequested = true;
                Session.EarliestExitCleanupTime = Now;
                SDK::DbgLog("[GhostMode] authored lifetime watchdog expired "
                    "controller=%p elapsed=%.2f\n", static_cast<void*>(PlayerController),
                    Now - Session.StartedAt);
            }

            if (!Session.bExitRequested || Now < Session.EarliestExitCleanupTime)
            {
                ++Index;
                continue;
            }

            BeginTrackedGhostModeCleanup(Session, "latched-exit");
            TrackedGhostModeSessions.erase(TrackedGhostModeSessions.begin() + Index);
        }
    }
}

void CaptureGhostModeCharacterPartsBeforeGrant(AFortPlayerControllerAthena* PlayerController)
{
    if (VersionInfo.FortniteVersion < 5.30 || VersionInfo.FortniteVersion > 8.00 ||
        !PlayerController)
    {
        return;
    }

    if (GhostCharacterPartRestores.contains(PlayerController))
    {
        return;
    }

    FGhostCharacterPartRestore Restore{};
    if (!CaptureCurrentGhostCharacterParts(PlayerController, Restore))
    {
        SDK::DbgLog("[GhostMode] pre-grant character-part snapshot "
            "unavailable controller=%p\n", static_cast<void*>(PlayerController));
        return;
    }

    GhostCharacterPartRestores.emplace(PlayerController, Restore);
    int32 CapturedCount = 0;
    for (const bool bCaptured : Restore.bRestore)
        CapturedCount += bCaptured ? 1 : 0;
    SDK::DbgLog("[GhostMode] captured pre-grant character parts "
        "controller=%p pawn=%p count=%d\n", static_cast<void*>(PlayerController),
        static_cast<void*>(Restore.Pawn), CapturedCount);
}

void AFortInventory::TrackGhostModeActivation(AFortPlayerControllerAthena* PlayerController,
    const UFortItemDefinition* ItemDefinition, const FGuid& ItemGuid)
{
    if (VersionInfo.FortniteVersion < 5.30 || VersionInfo.FortniteVersion > 8.00 ||
        !PlayerController || !UFortKismetLibrary::IsGhostModeItemDefinition(ItemDefinition))
    {
        return;
    }

    auto World = UWorld::GetWorld();
    if (!World)
        return;

    const double Now = UGameplayStatics::GetTimeSeconds(World);
    const double LifetimeSeconds = ResolveGhostModeLifetimeSeconds(ItemDefinition);
    FTrackedGhostModeSession Session{};
    Session.Owner = TWeakObjectPtr<AFortPlayerControllerAthena>(PlayerController);
    Session.ItemDefinition = TWeakObjectPtr<UFortItemDefinition>(
            const_cast<UFortItemDefinition*>(ItemDefinition));
    Session.ItemGuid = ItemGuid;
    Session.StartedAt = Now;
    Session.ExpireAt = Now + LifetimeSeconds + 1.0;

    auto Pawn = PlayerController->MyFortPawn ? PlayerController->MyFortPawn : PlayerController->Pawn
            ? PlayerController->Pawn->Cast<AFortPlayerPawnAthena>() : nullptr;
    Session.bHasInitialPawnExitStartTime = ReadGhostExitFloat(Pawn, "GhostModeExitStartTime",
            Session.InitialPawnExitStartTime);

    auto Existing = FindTrackedGhostModeSession(PlayerController);
    if (Existing != TrackedGhostModeSessions.end())
        *Existing = Session;
    else
        TrackedGhostModeSessions.push_back(Session);

    SDK::DbgLog("[GhostMode] armed lifecycle tracker controller=%p "
        "item=%08X-%08X-%08X-%08X lifetime=%.2f\n", static_cast<void*>(PlayerController),
        static_cast<uint32>(ItemGuid.A), static_cast<uint32>(ItemGuid.B),
        static_cast<uint32>(ItemGuid.C), static_cast<uint32>(ItemGuid.D), LifetimeSeconds);
}

void AFortInventory::NotifyGhostModeExitAbilityActivated(
    UAbilitySystemComponent* AbilitySystemComponent, const UFortGameplayAbility* Ability)
{
    if (VersionInfo.FortniteVersion < 5.30 || VersionInfo.FortniteVersion > 8.00 ||
        !Ability || !Ability->Class)
    {
        return;
    }

    const auto AbilityClassName = Ability->Class->Name.ToWString();
    if (AbilityClassName != L"GA_Exit_SpookyMist_C" && AbilityClassName !=
            L"GA_SpookyMist_ForcedExit_C")
    {
        return;
    }

    auto PlayerController = ResolveGhostModeController(AbilitySystemComponent);
    RequestTrackedGhostModeExit(PlayerController, AbilityClassName == L"GA_Exit_SpookyMist_C"
            ? "GA_Exit_SpookyMist" : "GA_SpookyMist_ForcedExit", 0.05);
}

void AFortInventory::NotifyGhostModeHarvestingToolRequested(
    AFortPlayerControllerAthena* PlayerController, const UFortItemDefinition* ItemDefinition)
{
    if (VersionInfo.FortniteVersion < 5.30 || VersionInfo.FortniteVersion > 8.00 ||
        !PlayerController || !ItemDefinition || ItemDefinition->ItemType !=
            EFortItemType::GetWeaponHarvest())
    {
        return;
    }

    RequestTrackedGhostModeExit(PlayerController, "harvesting-tool-request", 0.05);
}

bool UFortWorldItemDefinition::ServerExecute(UFortItem* Item, AFortPlayerControllerAthena* Instigator) const
{
    if (!this || !Item || !Instigator)
        return false;

    const int32 ServerExecuteVft = FindWorldItemDefinitionServerExecuteVft();
    if (ServerExecuteVft < 0 || ServerExecuteVft >= 1024 || !Vft[ServerExecuteVft])
        return false;

    return ((bool(*)(const UFortWorldItemDefinition*, UFortItem*,
        AFortPlayerControllerAthena*))Vft[ServerExecuteVft])(this, Item, Instigator);
}

UFortWorldItem* AFortInventory::GiveItem(const UFortItemDefinition* Def, int Count, int LoadedAmmo,
    int Level, bool ShowPickupNoti, bool updateInventory, int PhantomReserveAmmo,
    TArray<FFortItemEntryStateValue> StateValues, bool bNotifyItemInstanceAdded,
    TArray<float> GenericAttributeValues, bool* OutForceFocusHandled,
    bool* OutGadgetInitializationDispatched)
{
    if (OutForceFocusHandled)
        *OutForceFocusHandled = false;
    if (OutGadgetInitializationDispatched)
        *OutGadgetInitializationDispatched = false;
    if (!this || !Def || !Count)
        return nullptr;

    // The cooked 10.40 Ashton gadget is partial (head part only, no D_AshtonPack); the playlist's Carmine gadget is complete.
    Def = ResolveExact1040AshtonGadgetAlias(Def);

    auto PlayerController = Owner ? Owner->Cast<AFortPlayerControllerAthena>() : nullptr;
    auto Gadget = Def->Cast<UFortGadgetItemDefinition>();
    const bool bCarmineGadget = IsExact1040CarmineGadget(Def);
    const bool bAshtonMiloGadget = IsExact1040AshtonMiloGadget(Def);
    if (bCarmineGadget)
        PreloadExact1040CarmineDependencies();
    if (bAshtonMiloGadget)
        PreloadExact1040AshtonMiloDependencies();

    // Exclusive Chapter 1 gadgets want the backpack emptied before their native ServerExecute. The Infinity Gauntlet sets this flag.
    if (VersionInfo.FortniteVersion >= 4.0 && VersionInfo.FortniteVersion <= 4.5 &&
        PlayerController && PlayerController->MyFortPawn && Gadget &&
        Gadget->HasbDropAllOnEquip() && Gadget->bDropAllOnEquip)
    {
        const FVector DropLocation = PlayerController->MyFortPawn->K2_GetActorLocation();
        for (int32 Index = Inventory.ReplicatedEntries.Num() - 1; Index >= 0; Index--)
        {
            auto& ExistingEntry = Inventory.ReplicatedEntries.Get(Index, FFortItemEntry::Size());
            auto ExistingDefinition = ExistingEntry.ItemDefinition;
            if (!ExistingDefinition)
                continue;

            const bool CanDrop = ExistingDefinition->HasbCanBeDropped()
                ? ExistingDefinition->bCanBeDropped : (ExistingDefinition->GetPickupComponent()
                    ? ExistingDefinition->GetPickupComponent()->bCanBeDroppedFromInventory : false);
            if (!CanDrop)
                continue;

            const FGuid ExistingGuid = ExistingEntry.ItemGuid;
            AFortInventory::SpawnPickup(DropLocation, ExistingEntry, EFortPickupSourceTypeFlag::GetPlayer(),
                EFortPickupSpawnSource::GetUnset(), PlayerController->MyFortPawn);
            Remove(ExistingGuid);
        }
    }

    UFortWorldItem* Item = (UFortWorldItem*)Def->CreateTemporaryItemInstanceBP(Count, Level);
    if (!Item)
    {
        SDK::DbgLog("[Inventory] CreateTemporaryItemInstanceBP failed: Def=%p Count=%d Level=%d FN=%.2f\n",
            (void*)Def, Count, Level, VersionInfo.FortniteVersion);
        return nullptr;
    }

    Item->SetOwningControllerForTemporaryItem(Owner);
    if (Item->HasOwnerInventory())
        Item->OwnerInventory = this;
    else if (Item->HasOwnerInventoryWeak())
        Item->OwnerInventoryWeak = this;
    Item->ItemEntry.ParentInventory = this;
    Item->ItemEntry.LoadedAmmo = LoadedAmmo;
    if (Item->ItemEntry.HasPhantomReserveAmmo())
        Item->ItemEntry.PhantomReserveAmmo = PhantomReserveAmmo;
    if (auto WeaponDef = Def->Cast<UFortWeaponItemDefinition>())
        FFortWeaponMods::CopyDefinitionSlotsToEntry(WeaponDef, Item->ItemEntry);
    const bool bHasWeaponModSlots = FFortWeaponMods::HasEntrySlots(Item->ItemEntry);
    if (Item->ItemEntry.HasStateValues() && StateValues.Num() > 0)
    {
        auto NewData = FMemory::Malloc(FFortItemEntryStateValue::Size() * StateValues.Num());
        memcpy(NewData, StateValues.Data, FFortItemEntryStateValue::Size() * StateValues.Num());
        Item->ItemEntry.StateValues.NumElements = StateValues.Num();
        Item->ItemEntry.StateValues.MaxElements = StateValues.Num();
        Item->ItemEntry.StateValues.Data = (FFortItemEntryStateValue*)NewData;
    }
    if (Item->ItemEntry.HasGenericAttributeValues() && GenericAttributeValues.Num() > 0)
    {
        auto NewData = FMemory::Malloc(sizeof(float) * GenericAttributeValues.Num());
        memcpy(NewData, GenericAttributeValues.Data, sizeof(float) * GenericAttributeValues.Num());
        Item->ItemEntry.GenericAttributeValues.NumElements = GenericAttributeValues.Num();
        Item->ItemEntry.GenericAttributeValues.MaxElements = GenericAttributeValues.Num();
        Item->ItemEntry.GenericAttributeValues.Data = static_cast<float*>(NewData);
    }

    if (Item->ItemEntry.ItemGuid.A == 0 && Item->ItemEntry.ItemGuid.B == 0 &&
        Item->ItemEntry.ItemGuid.C == 0 && Item->ItemEntry.ItemGuid.D == 0)
    {
        CoCreateGuid((GUID*)&Item->ItemEntry.ItemGuid);

        if (FFortItemEntry::HasTrackerGuid() && Item->ItemEntry.TrackerGuid.A == 0 &&
            Item->ItemEntry.TrackerGuid.B == 0 && Item->ItemEntry.TrackerGuid.C == 0 &&
            Item->ItemEntry.TrackerGuid.D == 0)
            CoCreateGuid((GUID*)&Item->ItemEntry.TrackerGuid);
    }

    auto& AddedReplicatedEntry = this->Inventory.ReplicatedEntries.Add(
            Item->ItemEntry, FFortItemEntry::Size());
    auto* ReplicatedEntry = &AddedReplicatedEntry;
    ReplicatedEntry->bIsReplicatedCopy = true;
    if (bHasWeaponModSlots)
    {
        FFortWeaponMods::CopyEntrySlots(Item->ItemEntry, *ReplicatedEntry);
    }
    this->Inventory.ItemInstances.Add(Item);

    if (VersionInfo.FortniteVersion <= 3.60)
    {
        auto PlayerController = (AFortPlayerControllerAthena*)Owner;
        auto PlayerState = PlayerController ? (AFortPlayerStateAthena*)PlayerController->PlayerState : nullptr;
        bool bIsBotInventory = PlayerState && PlayerState->HasbIsABot() && PlayerState->bIsABot;

        if (!bIsBotInventory && PlayerController && PlayerController->QuickBars &&
            (IsPrimaryQuickbar(Def) || Def->ItemType == EFortItemType::GetBuildingPiece() ||
                Def->ItemType == EFortItemType::GetTrap() ||
                Def->ItemType == EFortItemType::GetWeaponHarvest()))
        {
            PlayerController->QuickBars->ServerAddItemInternal(Item->ItemEntry.ItemGuid,
                !(IsPrimaryQuickbar(Def) || Def->ItemType == EFortItemType::GetWeaponHarvest()),
                -3);
        }
    }

    if (updateInventory)
    {
        bRequiresLocalUpdate = true;
        bRequiresSaving = true;

        HandleInventoryLocalUpdate();

        // Native inventory work can move the fast-array allocation, so never hold the Add() reference across an update.
        ReplicatedEntry = Inventory.ReplicatedEntries.Search([&](FFortItemEntry& Candidate)
            {
                return Candidate.ItemGuid == Item->ItemEntry.ItemGuid;
            }, FFortItemEntry::Size());
        if (ReplicatedEntry)
        {
            ReplicatedEntry->bIsDirty = false;
            Inventory.MarkItemDirty(*ReplicatedEntry);
        }
        ForceNetUpdate();
        Item->ItemEntry.bIsDirty = true;
    }

    if (bHasWeaponModSlots && ReplicatedEntry)
    {
        FFortWeaponMods::CopyEntrySlots(*ReplicatedEntry, Item->ItemEntry);
    }

    bool bCarmineInitialized = false;
    if (bCarmineGadget)
    {
        bCarmineInitialized = bNotifyItemInstanceAdded ? InitializeGadgetItemWithFallback(
                      Item, true) : false;
    }
    bool bAshtonMiloInitialized = false;
    if (bAshtonMiloGadget)
    {
        bAshtonMiloInitialized = bNotifyItemInstanceAdded ? InitializeGadgetItemWithFallback(
                      Item, true) : false;
    }
    bool bItemAddedFallbackDispatched = false;
    bool bCarmineFallbackDispatched = false;
    const IInterface* InventoryOwnerInterface = Owner ? Owner->GetInterface(
                  IFortInventoryOwnerInterface::StaticClass()) : nullptr;
    if ((!bCarmineGadget || !bCarmineInitialized) && (!bAshtonMiloGadget ||
         !bAshtonMiloInitialized) && bNotifyItemInstanceAdded && OnItemInstanceAddedVft &&
        OnItemInstanceAddedVft < 1024 && Item->Vft && Item->Vft[OnItemInstanceAddedVft] &&
        InventoryOwnerInterface)
    {
        ((void(*)(const UFortWorldItem*, const IInterface*))
            Item->Vft[OnItemInstanceAddedVft])(Item, InventoryOwnerInterface);
        bItemAddedFallbackDispatched = true;
        bCarmineFallbackDispatched = bCarmineGadget;
    }

    bool bCarmineFocusHandled = false;
    if (bCarmineGadget && (bCarmineInitialized || bCarmineFallbackDispatched) && PlayerController &&
        Def->HasbForceFocusWhenAdded() && Def->bForceFocusWhenAdded)
    {
        bCarmineFocusHandled = IsExact1040BaseAshtonGadget(Def)
                ? EnsureExact1040AshtonBackingAndFocus() : FocusOrQueueExact1040Carmine(
                      PlayerController, Item->ItemEntry.ItemGuid);
    }
    if (VersionInfo.FortniteVersion >= 4.0 && VersionInfo.FortniteVersion <= 4.5 &&
        PlayerController && Gadget && Def->HasbForceFocusWhenAdded() && Def->bForceFocusWhenAdded)
    {
        PlayerController->ServerExecuteInventoryItem(Item->ItemEntry.ItemGuid);
        PlayerController->ClientEquipItem(Item->ItemEntry.ItemGuid, true);
    }
    if (OutForceFocusHandled && bCarmineGadget)
        *OutForceFocusHandled = bCarmineFocusHandled;
    if (OutGadgetInitializationDispatched)
    {
        *OutGadgetInitializationDispatched = bCarmineGadget ? bCarmineInitialized ||
                      bItemAddedFallbackDispatched : bAshtonMiloGadget ? bAshtonMiloInitialized ||
                          bItemAddedFallbackDispatched : bItemAddedFallbackDispatched;
    }

    auto RechargingWeaponDefinition = Item->ItemEntry.ItemDefinition
            ? Item->ItemEntry.ItemDefinition->Cast<UFortWeaponItemDefinition>() : nullptr;
    if (PlayerController && RechargingWeaponDefinition)
    {
        ObserveRechargingWeaponAmmo(PlayerController, RechargingWeaponDefinition,
            Item->ItemEntry.ItemGuid, Item->ItemEntry.Level, Item->ItemEntry.LoadedAmmo,
            Item->ItemEntry.LoadedAmmo);
    }

    return Item;
}

bool AFortInventory::InitializeGadgetItem(UFortWorldItem* Item, bool updateInventory)
{
    if (!this || !Item || !Item->ItemEntry.ItemDefinition)
        return false;

    auto Gadget = Item->ItemEntry.ItemDefinition->Cast<UFortGadgetItemDefinition>();
    const IInterface* InventoryOwnerInterface = Owner ? Owner->GetInterface(
                  IFortInventoryOwnerInterface::StaticClass()) : nullptr;
    if (!Gadget || !InventoryOwnerInterface)
        return false;

    const bool bCarmineGadget = IsExact1040CarmineGadget(Gadget);
    const bool bAshtonMiloGadget = IsExact1040AshtonMiloGadget(Gadget);
    if (bCarmineGadget)
        PreloadExact1040CarmineDependencies();
    if (bAshtonMiloGadget)
        PreloadExact1040AshtonMiloDependencies();

    const UFortAbilitySet* LoadedAbilitySet = nullptr;
    if (Gadget->HasAbilitySet())
        LoadedAbilitySet = Gadget->AbilitySet.Get();

    const bool bBigTeamGlider = VersionInfo.FortniteVersion == 10.40 && Gadget->Name.ToWString() ==
            L"Athena_Glider_Item_BigTeamMode";
    if (!LoadedAbilitySet && bBigTeamGlider)
    {
        LoadedAbilitySet = FindObject<UFortAbilitySet>(L"/Game/Athena/Items/Gameplay/BackPacks/"
            L"GliderItem/AS_Athena_Glider_Item."
            L"AS_Athena_Glider_Item");
    }
    if (!LoadedAbilitySet && IsExact1040BaseAshtonGadget(Gadget))
    {
        LoadedAbilitySet = FindObject<UFortAbilitySet>(L"/Game/Athena/Items/Gameplay/BackPacks/"
            L"Ashton/AS_AshtonPack.AS_AshtonPack");
    }
    if (!LoadedAbilitySet && bCarmineGadget && !IsExact1040BaseAshtonGadget(Gadget))
    {
        LoadedAbilitySet = FindObject<UFortAbilitySet>(L"/Game/Athena/Items/Gameplay/BackPacks/"
            L"CarminePack/AS_CarminePack.AS_CarminePack");
    }
    if (!LoadedAbilitySet && bAshtonMiloGadget)
    {
        LoadedAbilitySet = FindObject<UFortAbilitySet>(L"/Game/Athena/Items/Gameplay/BackPacks/"
            L"Ashton/Milo/AS_AshtonPack_Milo."
            L"AS_AshtonPack_Milo");
    }
    if (LoadedAbilitySet)
        LoadedAbilitySet->AddToRoot();

    UClass* LoadedAttributeSet = Gadget->HasAttributeSet() ? Gadget->AttributeSet.Get() : nullptr;
    UClass* LoadedGameplayAbility = Gadget->HasGameplayAbility() ? Gadget->GameplayAbility.Get()
            : nullptr;
    auto LoadedWeaponDefinition = Gadget->GetWeaponItemDefinition();

    bool bApplied = false;
    if (ApplyGadgetDataAddress)
    {
        bApplied = ((bool (*)(UFortGadgetItemDefinition*, const IInterface*, UFortItem*,
                uint8_t))ApplyGadgetDataAddress)(Gadget, InventoryOwnerInterface,
                    reinterpret_cast<UFortItem*>(Item), 1);
    }

    if (updateInventory && bApplied)
        Update(&Item->ItemEntry);

    SDK::DbgLog("[Gadget] native initialization definition=%s item=%p "
        "abilitySet=%p attributeSet=%p gameplayAbility=%p "
        "weapon=%p applied=%d\n", Gadget->Name.ToString().c_str(), static_cast<void*>(Item),
        static_cast<const void*>(LoadedAbilitySet), static_cast<void*>(LoadedAttributeSet),
        static_cast<void*>(LoadedGameplayAbility), static_cast<void*>(LoadedWeaponDefinition),
        bApplied ? 1 : 0);
    return bApplied;
}

bool AFortInventory::InitializeGadgetItemWithFallback(UFortWorldItem* Item, bool updateInventory)
{
    if (!this || !Item || !Item->ItemEntry.ItemDefinition || !Item->ItemEntry.ItemDefinition
             ->Cast<UFortGadgetItemDefinition>())
    {
        return false;
    }

    const bool bForceStockMiloNotification = IsExact1040AshtonMiloGadget(
            Item->ItemEntry.ItemDefinition);
    const bool bForceStockNotification = bForceStockMiloNotification;

    if (!bForceStockNotification && InitializeGadgetItem(Item, updateInventory))
        return true;

    const IInterface* InventoryOwnerInterface = Owner ? Owner->GetInterface(
                  IFortInventoryOwnerInterface::StaticClass()) : nullptr;
    if (!InventoryOwnerInterface || !OnItemInstanceAddedVft || OnItemInstanceAddedVft >= 1024 ||
        !Item->Vft || !Item->Vft[OnItemInstanceAddedVft])
    {
        return false;
    }

    ((void(*)(const UFortWorldItem*, const IInterface*))
        Item->Vft[OnItemInstanceAddedVft])(Item, InventoryOwnerInterface);
    SDK::DbgLog("[Gadget] dispatched item-added %s "
        "definition=%s item=%p\n", bForceStockNotification ? "forced-stock path" : "fallback",
        Item->ItemEntry.ItemDefinition->Name.ToString().c_str(), static_cast<void*>(Item));
    return true;
}

bool AFortInventory::EnsureExact1040AshtonBackingAndFocus(FGuid* OutBackingGuid)
{
    if (OutBackingGuid)
        *OutBackingGuid = {};
    auto PlayerController = Owner ? Owner->Cast<AFortPlayerControllerAthena>() : nullptr;
    auto BackingDefinition = ResolveExact1040CarmineBacking();
    if (!PlayerController || !BackingDefinition)
    {
        return false;
    }

    auto Existing = Inventory.ReplicatedEntries.Search([&](FFortItemEntry& Entry)
            {
                return Entry.ItemDefinition == BackingDefinition;
            }, FFortItemEntry::Size());
    FGuid BackingGuid{};
    if (Existing)
    {
        BackingGuid = Existing->ItemGuid;
    }
    else
    {
        auto BackingItem = GiveItem(BackingDefinition, 1, 0, 0, false, true, 0,
            {}, true);
        if (!BackingItem)
            return false;
        BackingGuid = BackingItem->ItemEntry.ItemGuid;
        SDK::DbgLog("[Ashton1040] granted explicit "
            "D_CarminePack backing for authored gauntlet "
            "controller=%p\n", static_cast<void*>(PlayerController));
    }

    if (OutBackingGuid)
        *OutBackingGuid = BackingGuid;
    return FocusOrQueueExact1040Carmine(PlayerController, BackingGuid);
}

int32 AFortInventory::GiveItemToSingleStack(const UFortItemDefinition* Definition, int32 Count,
    bool ShowPickupNoti)
{
    if (!this || !Definition || Count <= 0)
        return 0;

    int32 MaxStackSize = Definition->GetMaxStackSize();
    if (MaxStackSize <= 0)
    {
        MaxStackSize = (std::numeric_limits<int32>::max)();
    }

    auto ExistingEntry = Inventory.ReplicatedEntries.Search([&](FFortItemEntry& Entry)
        {
            return Entry.ItemDefinition == Definition;
        }, FFortItemEntry::Size());

    if (!ExistingEntry)
    {
        const int32 GrantedCount = (std::min)(Count, MaxStackSize);
        return GiveItem(Definition, GrantedCount, 0, 0, ShowPickupNoti) ? GrantedCount : 0;
    }

    const int32 ExistingCount = (std::max)(ExistingEntry->Count, 0);
    if (ExistingCount >= MaxStackSize)
        return 0;

    const int64 DesiredCount = static_cast<int64>(ExistingCount) + static_cast<int64>(Count);
    int32 NewCount = static_cast<int32>((std::min)(DesiredCount, static_cast<int64>(MaxStackSize)));
    const int32 GrantedCount = NewCount - ExistingCount;
    if (GrantedCount <= 0)
        return 0;

    FGuid ExistingGuid = ExistingEntry->ItemGuid;
    ExistingEntry->Count = NewCount;

    auto ExistingItem = Inventory.ItemInstances.Search([&](UFortWorldItem* Item)
        {
            return Item && AreGuidsEqual(Item->ItemEntry.ItemGuid, ExistingGuid);
        });
    if (ExistingItem && *ExistingItem)
    {
        (*ExistingItem)->ItemEntry.Count = NewCount;
        (*ExistingItem)->ItemEntry.bIsDirty = true;
    }

    UpdateEntry(*ExistingEntry);
    return GrantedCount;
}

UFortWorldItem* AFortInventory::GiveItem(FFortItemEntry& entry, int Count, bool ShowPickupNoti,
    bool updateInventory, bool* OutForceFocusHandled)
{
    if (OutForceFocusHandled)
        *OutForceFocusHandled = false;
    if (Count == -1)
        Count = entry.Count;

    if (!FFortWeaponMods::HasEntrySlots(entry))
    {
        return GiveItem(entry.ItemDefinition, Count, entry.LoadedAmmo, entry.Level, ShowPickupNoti,
            updateInventory, entry.HasPhantomReserveAmmo() ? entry.PhantomReserveAmmo : 0,
            entry.HasStateValues() ? entry.StateValues : TArray<FFortItemEntryStateValue>{}, true,
            entry.HasGenericAttributeValues() ? entry.GenericAttributeValues : TArray<float>{},
            OutForceFocusHandled);
    }

    bool bDefinitionFocusHandled = false;
    auto Item = GiveItem(entry.ItemDefinition, Count, entry.LoadedAmmo, entry.Level, ShowPickupNoti,
        false, entry.HasPhantomReserveAmmo() ? entry.PhantomReserveAmmo : 0, entry.HasStateValues()
            ? entry.StateValues : TArray<FFortItemEntryStateValue>{}, false,
        entry.HasGenericAttributeValues() ? entry.GenericAttributeValues : TArray<float>{},
        &bDefinitionFocusHandled);
    if (!Item)
        return nullptr;

    auto ReplicatedEntry = Inventory.ReplicatedEntries.Search([&](FFortItemEntry& Candidate)
        {
            return Candidate.ItemGuid == Item->ItemEntry.ItemGuid;
        }, FFortItemEntry::Size());

    FFortWeaponMods::CopyEntrySlots(entry, Item->ItemEntry);
    if (ReplicatedEntry)
        FFortWeaponMods::CopyEntrySlots(entry, *ReplicatedEntry);

    if (updateInventory)
    {
        bRequiresLocalUpdate = true;
        bRequiresSaving = true;
        HandleInventoryLocalUpdate();

        ReplicatedEntry = Inventory.ReplicatedEntries.Search([&](FFortItemEntry& Candidate)
            {
                return Candidate.ItemGuid == Item->ItemEntry.ItemGuid;
            }, FFortItemEntry::Size());
        if (ReplicatedEntry)
        {
            ReplicatedEntry->bIsDirty = false;
            Inventory.MarkItemDirty(*ReplicatedEntry);
            FFortWeaponMods::CopyEntrySlots(*ReplicatedEntry, Item->ItemEntry);
        }

        ForceNetUpdate();
        Item->ItemEntry.bIsDirty = true;
    }

    const IInterface* InventoryOwnerInterface = Owner ? Owner->GetInterface(
                  IFortInventoryOwnerInterface::StaticClass()) : nullptr;
    if (OnItemInstanceAddedVft && OnItemInstanceAddedVft < 1024 && Item->Vft &&
        Item->Vft[OnItemInstanceAddedVft] && InventoryOwnerInterface && !bDefinitionFocusHandled)
    {
        ((void(*)(const UFortWorldItem*, const IInterface*))
            Item->Vft[OnItemInstanceAddedVft])(Item, InventoryOwnerInterface);
        if (IsExact1040CarmineGadget(Item->ItemEntry.ItemDefinition))
        {
            auto PlayerController = Owner->Cast<AFortPlayerControllerAthena>();
            auto Definition = Item->ItemEntry.ItemDefinition;
            if (PlayerController && Definition->HasbForceFocusWhenAdded() && Definition
                    ->bForceFocusWhenAdded)
            {
                bDefinitionFocusHandled = IsExact1040BaseAshtonGadget(Definition)
                        ? EnsureExact1040AshtonBackingAndFocus() : FocusOrQueueExact1040Carmine(
                              PlayerController, Item->ItemEntry.ItemGuid);
            }
        }
    }
    if (OutForceFocusHandled)
        *OutForceFocusHandled = bDefinitionFocusHandled;

    return Item;
}

void AFortInventory::HandlePendingCarmineFocus(AFortPlayerControllerAthena* PlayerController)
{
    auto Pending = PendingCarmineFocus.find(PlayerController);
    if (Pending == PendingCarmineFocus.end() || !PlayerController ||
        !PlayerController->MyFortPawn || !PlayerController->WorldInventory)
    {
        return;
    }

    const FGuid ItemGuid = Pending->second;
    auto Entry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Candidate)
                {
                    return AreGuidsEqual(Candidate.ItemGuid, ItemGuid);
                }, FFortItemEntry::Size());
    PendingCarmineFocus.erase(Pending);
    const bool bValidPendingDefinition = Entry && (IsExact1040CarmineGadget(
             Entry->ItemDefinition) || (Entry->ItemDefinition &&
          Entry->ItemDefinition->Name.ToWString() == L"D_CarminePack"));
    if (!bValidPendingDefinition)
    {
        return;
    }

    PlayerController->ServerExecuteInventoryItem(ItemGuid);
    PlayerController->ClientEquipItem(ItemGuid, true);
    SDK::DbgLog("[Ashton1040] completed queued Carmine focus "
        "controller=%p\n", static_cast<void*>(PlayerController));
}

void AFortInventory::SetRequiresUpdate()
{
    Inventory.MarkArrayDirty();
    bRequiresLocalUpdate = true;
    bRequiresSaving = true;
    HandleInventoryLocalUpdate();

    ForceNetUpdate();
}

void AFortInventory::Update(FFortItemEntry* Entry)
{
    if (!Entry)
        return SetRequiresUpdate();

    if (Entry->bIsReplicatedCopy)
    {
        FGuid UpdatedGuid = Entry->ItemGuid;
        Entry->bIsDirty = false;
        Inventory.MarkItemDirty(*Entry);
        SetRequiresUpdate();

        auto UpdatedReplicatedEntry = Inventory.ReplicatedEntries.Search(
            [&](FFortItemEntry& Candidate)
            {
                return Candidate.ItemGuid == UpdatedGuid;
            }, FFortItemEntry::Size());
        auto ItemInstance = Inventory.ItemInstances.Search([&](UFortWorldItem* Candidate)
            {
                return Candidate && Candidate->ItemEntry.ItemGuid == UpdatedGuid;
            });
        if (UpdatedReplicatedEntry && ItemInstance && *ItemInstance)
        {
            FFortWeaponMods::CopyEntrySlots(*UpdatedReplicatedEntry, (*ItemInstance)->ItemEntry);
        }
        goto _out;
    }

    if (Entry->ItemGuid.A == 0 && Entry->ItemGuid.B == 0 && Entry->ItemGuid.C == 0 && Entry->ItemGuid.D == 0)
        goto _out;

    for (int i = 0; i < Inventory.ReplicatedEntries.Num(); i++)
    {
        auto& repEntry = Inventory.ReplicatedEntries.Get(i, FFortItemEntry::Size());

        if (repEntry.ItemGuid == Entry->ItemGuid)
        {
            FGuid UpdatedGuid = Entry->ItemGuid;
            TArray<void*> PreviousWeaponModSlots{};
            const bool bPreserveWeaponModAllocation = FFortWeaponMods::IsSupported() &&
                FFortItemEntry::HasWeaponModSlots();
            if (bPreserveWeaponModAllocation)
            {
                PreviousWeaponModSlots = repEntry.WeaponModSlots;
            }

            repEntry = *Entry;
            if (bPreserveWeaponModAllocation)
            {
                repEntry.WeaponModSlots = PreviousWeaponModSlots;
            }
            FFortWeaponMods::CopyEntrySlots(*Entry, repEntry);
            repEntry.bIsDirty = false;
            Inventory.MarkItemDirty(repEntry);
            SetRequiresUpdate();

            auto UpdatedReplicatedEntry = Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Candidate)
                {
                    return Candidate.ItemGuid == UpdatedGuid;
                }, FFortItemEntry::Size());
            if (UpdatedReplicatedEntry)
            {
                FFortWeaponMods::CopyEntrySlots(*UpdatedReplicatedEntry, *Entry);
            }
            break;
        }
    }
_out: return;
}

void AFortInventory::RemoveWeaponAbilities(AActor* Weapon__Uncasted)
{
    auto Weapon = (AFortWeapon*)Weapon__Uncasted;
    auto ClearAbility = (void(*)(UAbilitySystemComponent*, FGameplayAbilitySpecHandle&)) ClearAbility_;
    auto PlayerController = (AFortPlayerControllerAthena*)((AFortPlayerPawnAthena*)Weapon->Instigator)->Controller;
    if (!PlayerController)
        return;
    auto AbilitySystemComponent = PlayerController->PlayerState->AbilitySystemComponent;

    if (Weapon->PrimaryAbilitySpecHandle.Handle != -1)
    {
        ClearAbility(AbilitySystemComponent, Weapon->PrimaryAbilitySpecHandle);
        Weapon->PrimaryAbilitySpecHandle.Handle = -1;
    }
    if (Weapon->SecondaryAbilitySpecHandle.Handle != -1)
    {
        ClearAbility(AbilitySystemComponent, Weapon->SecondaryAbilitySpecHandle);
        Weapon->SecondaryAbilitySpecHandle.Handle = -1;
    }
    if (Weapon->ReloadAbilitySpecHandle.Handle != -1)
    {
        ClearAbility(AbilitySystemComponent, Weapon->ReloadAbilitySpecHandle);
        Weapon->ReloadAbilitySpecHandle.Handle = -1;
    }
    if (Weapon->ImpactAbilitySpecHandle.Handle != -1)
    {
        ClearAbility(AbilitySystemComponent, Weapon->ImpactAbilitySpecHandle);
        Weapon->ImpactAbilitySpecHandle.Handle = -1;
    }
    if (Weapon->EquippedAbilityHandles.Num())
    {
        for (int i = 0; i < Weapon->EquippedAbilityHandles.Num(); i++)
        {
            auto& Handle = Weapon->EquippedAbilityHandles.Get(i, FGameplayAbilitySpecHandle::Size());

            if (IsBadReadPtr(&Handle)) // what
                continue;

            if (Handle.Handle != -1)
            {
                ClearAbility(AbilitySystemComponent, Handle);
            }
        }
        Weapon->EquippedAbilityHandles.ResetNum();
    }

    if (Weapon->EquippedAbilitySetHandles.Num())
    {
        for (int i = 0; i < Weapon->EquippedAbilitySetHandles.Num(); i++)
        {
            auto& Handle = Weapon->EquippedAbilitySetHandles.Get(i, FFortAbilitySetHandle::Size());

            UFortKismetLibrary::UnequipFortAbilitySet(Handle);
        }

        PlayerController->AppliedInGameModifierAbilitySetHandles.Reset();
        Weapon->EquippedAbilitySetHandles.ResetNum();
    }
}

void AFortInventory::Remove(FGuid Guid)
{
    auto PendingOwner = Owner ? Owner->Cast<AFortPlayerControllerAthena>() : nullptr;
    auto Pending = PendingCarmineFocus.find(PendingOwner);
    if (Pending != PendingCarmineFocus.end() && AreGuidsEqual(Pending->second, Guid))
    {
        PendingCarmineFocus.erase(Pending);
    }

    auto ItemEntryIdx = Inventory.ReplicatedEntries.SearchIndex([&](FFortItemEntry& entry)
        { return entry.ItemGuid == Guid; }, FFortItemEntry::Size());
    if (ItemEntryIdx == -1)
        return;

    auto& RemovedEntry = Inventory.ReplicatedEntries.Get(ItemEntryIdx, FFortItemEntry::Size());
    auto EntryDef = RemovedEntry.ItemDefinition;
    FGhostCharacterPartRestore GhostCharacterPartRestore{};
    const bool bRestoreGhostCharacterParts = CaptureGhostCharacterPartRestore(
            PendingOwner, EntryDef, GhostCharacterPartRestore);

    auto ItemInstanceIdx = Inventory.ItemInstances.SearchIndex([&](UFortWorldItem* entry)
        { return entry && entry->ItemEntry.ItemGuid == Guid; });
    auto ItemInstanceResult = Inventory.ItemInstances.Search([&](UFortWorldItem* entry)
        { return entry && entry->ItemEntry.ItemGuid == Guid; });

    auto Instance = ItemInstanceResult ? *ItemInstanceResult : nullptr;

    // TArray::Remove shifts raw elements and does not destruct nested arrays, so free the attachment buffers here.
    if (FFortWeaponMods::IsSupported() && FFortItemEntry::HasWeaponModSlots())
    {
        if (Instance && RemovedEntry.WeaponModSlots.Data && RemovedEntry.WeaponModSlots.Data ==
                Instance->ItemEntry.WeaponModSlots.Data)
        {
            Instance->ItemEntry.WeaponModSlots.Data = nullptr;
            Instance->ItemEntry.WeaponModSlots.NumElements = 0;
            Instance->ItemEntry.WeaponModSlots.MaxElements = 0;
        }

        FFortWeaponMods::FreeEntrySlots(RemovedEntry);
        if (Instance)
            FFortWeaponMods::FreeEntrySlots(Instance->ItemEntry);
    }

    if (ItemEntryIdx != -1)
        Inventory.ReplicatedEntries.Remove(ItemEntryIdx, FFortItemEntry::Size());
    if (ItemInstanceIdx != -1)
        Inventory.ItemInstances.Remove(ItemInstanceIdx);

    auto PlayerController = (AFortPlayerControllerAthena*)Owner;
    // 7.40 removed FortPlayerController::QuickBars, and reading it there resolves to offset -1, i.e. PlayerController - 1.
    AFortQuickBars* QuickBars = nullptr;
    if (VersionInfo.FortniteVersion < 7.40 && PlayerController && PlayerController->HasQuickBars())
    {
        QuickBars = PlayerController->QuickBars;
    }

    if (QuickBars && EntryDef)
    {
        auto& QuickBar = IsPrimaryQuickbar(EntryDef) ? QuickBars->PrimaryQuickBar : QuickBars->SecondaryQuickBar;
        int i = 0;
        for (i = 0; i < QuickBar.Slots.Num(); i++)
        {
            auto& Slot = QuickBar.Slots.Get(i, FQuickBarSlot::Size());

            for (auto& Item : Slot.Items)
                if (Item == Guid)
                    goto _Out;
        }
        goto _Skip;
    _Out: QuickBars->EmptySlot(!IsPrimaryQuickbar(EntryDef), i);
        QuickBars->ServerRemoveItemInternal(Guid, false, true);
        for (int i = 0; i < PlayerController->WorldInventory->Inventory.ReplicatedEntries.Num(); i++)
        {
            auto& Entry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Get(i, FFortItemEntry::Size());

            if (Entry.ItemDefinition && Entry.ItemDefinition->ItemType == EFortItemType::GetWeaponHarvest())
            {
                PlayerController->ServerExecuteInventoryItem(Entry.ItemGuid);
                QuickBars->ServerActivateSlotInternal(0, 0, 0.f, true);
            }
        }
    }

_Skip:

    bRequiresLocalUpdate = true;
    bRequiresSaving = true;

    HandleInventoryLocalUpdate();

    Inventory.MarkArrayDirty();
    ForceNetUpdate();

    if (OnItemInstanceAddedVft && Instance && Instance->ItemEntry.ItemDefinition)
    {
        ((bool(*)(const UFortWorldItem*, const IInterface*, uint32_t)) Instance->Vft[OnItemInstanceAddedVft + 1])(
            Instance, Owner->GetInterface(IFortInventoryOwnerInterface::StaticClass()),
            Instance->ItemEntry.Count);
    }
    UFortKismetLibrary::NotifyGhostModeItemRemoved(PendingOwner, EntryDef);
    QueueGhostModeTerminalCleanup(PendingOwner, EntryDef, GhostCharacterPartRestore,
        bRestoreGhostCharacterParts);
    if (UFortKismetLibrary::IsGhostModeItemDefinition(EntryDef))
    {
        MarkTrackedGhostModeBackingRemoved(PendingOwner, Guid);
    }
}

int32 AFortInventory::RemoveItem(FGuid Guid, int32 Count, bool bKeepFinalStackEmpty)
{
    auto ItemEntry = Inventory.ReplicatedEntries.Search([&](FFortItemEntry& Entry)
            {
                return Entry.ItemGuid == Guid;
            }, FFortItemEntry::Size());
    if (!ItemEntry || Count == 0)
        return 0;

    const bool bRemoveAll = Count < 0;
    const int32 ExistingCount = max(ItemEntry->Count, 0);
    const int32 RemovedCount = bRemoveAll ? ExistingCount : min(ExistingCount, Count);

    if (bRemoveAll || (!bKeepFinalStackEmpty && (ExistingCount <= 0 || Count >= ExistingCount)))
    {
        Remove(Guid);
        return RemovedCount;
    }

    int32 NewCount = bKeepFinalStackEmpty && Count >= ExistingCount ? 0
            : ExistingCount - RemovedCount;
    ItemEntry->Count = NewCount;

    auto ItemInstance = Inventory.ItemInstances.Search([&](UFortWorldItem* Item)
            {
                return Item && Item->ItemEntry.ItemGuid == Guid;
            });
    if (ItemInstance && *ItemInstance)
    {
        (*ItemInstance)->ItemEntry.Count = NewCount;
        (*ItemInstance)->ItemEntry.bIsDirty = true;
    }

    UpdateEntry(*ItemEntry);
    return RemovedCount;
}

bool AFortInventory::ShouldBypassItemConsumption(AFortPlayerControllerAthena* PlayerController,
    int32 Count, bool bForceRemoval)
{
    if (!PlayerController || Count <= 0 || bForceRemoval)
        return false;

    return
        FConfiguration::bInfiniteAmmo.load(std::memory_order_acquire) ||
        (PlayerController->HasbInfiniteAmmo() && PlayerController->bInfiniteAmmo);
}

FFortRangedWeaponStats* AFortInventory::GetStats(const UFortWeaponItemDefinition* Def)
{
    if (!Def || !Def->WeaponStatHandle.DataTable)
        return nullptr;

    auto Val = Def->WeaponStatHandle.DataTable->RowMap.Search([Def](FName& Key, uint8_t* Value)
        {
            return Def->WeaponStatHandle.RowName == Key && Value;
        });

    return Val ? *(FFortRangedWeaponStats**)Val : nullptr;
}

FFortRangedWeaponStats* AFortInventory::CloneStats(const UFortWeaponItemDefinition* Def)
{
    auto BaseStats = GetStats(Def);
    if (!BaseStats)
        return nullptr;

    auto NewStats = (FFortRangedWeaponStats*)FMemory::Malloc(sizeof(FFortRangedWeaponStats));
    FMemory::Memcpy(NewStats, BaseStats, sizeof(FFortRangedWeaponStats));

    return NewStats;
}

int32 AFortInventory::ReloadAllWeaponAmmo(AFortPlayerControllerAthena* PlayerController)
{
    if (!PlayerController || !PlayerController->WorldInventory)
        return 0;

    auto Inventory = PlayerController->WorldInventory;
    struct FReloadTarget
    {
        UFortWeaponItemDefinition* WeaponDefinition = nullptr;
        FGuid ItemGuid{};
        int32 ItemLevel = 0;
        int32 MaxLoadedAmmo = 0;
    };

    std::vector<FReloadTarget> Targets;
    const int32 EntryCount = Inventory->Inventory.ReplicatedEntries.Num();
    if (EntryCount <= 0)
        return 0;
    Targets.reserve(static_cast<size_t>(EntryCount));

    for (int32 Index = 0; Index < EntryCount; ++Index)
    {
        auto& Entry = Inventory->Inventory.ReplicatedEntries.Get(Index, FFortItemEntry::Size());
        if (!Entry.ItemDefinition)
            continue;

        auto WeaponDefinition = Entry.ItemDefinition->Cast<UFortWeaponItemDefinition>();
        if (auto Gadget = Entry.ItemDefinition->Cast<UFortGadgetItemDefinition>())
        {
            WeaponDefinition = Gadget->GetWeaponItemDefinition();
        }
        if (!WeaponDefinition)
            continue;

        int32 MaxLoadedAmmo = 0;
        int32 RechargeAmount = 0;
        double RechargeIntervalSeconds = 0.0;
        if (!ResolveWeaponRechargeSettings(WeaponDefinition, Entry.Level, MaxLoadedAmmo,
                RechargeAmount, RechargeIntervalSeconds))
        {
            auto Stats = GetStats(WeaponDefinition);
            MaxLoadedAmmo = Stats ? Stats->ClipSize : 0;
        }
        if (MaxLoadedAmmo <= 0)
            continue;

        Targets.push_back({
            WeaponDefinition, Entry.ItemGuid, Entry.Level, MaxLoadedAmmo
        });
    }

    int32 ReloadedWeaponCount = 0;
    for (auto& Target : Targets)
    {
        auto ReplicatedEntry = Inventory->Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Candidate)
                {
                    return AreGuidsEqual(Candidate.ItemGuid, Target.ItemGuid);
                }, FFortItemEntry::Size());
        if (!ReplicatedEntry)
            continue;

        auto ItemInstance = Inventory->Inventory.ItemInstances.Search([&](UFortWorldItem* Candidate)
            {
                return Candidate && AreGuidsEqual(Candidate->ItemEntry.ItemGuid, Target.ItemGuid);
            });
        const int32 PreviousLoadedAmmo = ReplicatedEntry->LoadedAmmo;
        bool bInventoryChanged = false;
        if (ReplicatedEntry->LoadedAmmo != Target.MaxLoadedAmmo)
        {
            ReplicatedEntry->LoadedAmmo = Target.MaxLoadedAmmo;
            bInventoryChanged = true;
        }
        if (ItemInstance && *ItemInstance && (*ItemInstance)->ItemEntry.LoadedAmmo !=
                Target.MaxLoadedAmmo)
        {
            (*ItemInstance)->ItemEntry.LoadedAmmo = Target.MaxLoadedAmmo;
            (*ItemInstance)->ItemEntry.bIsDirty = true;
            bInventoryChanged = true;
        }

        if (bInventoryChanged)
            Inventory->UpdateEntry(*ReplicatedEntry);

        ItemInstance = Inventory->Inventory.ItemInstances.Search([&](UFortWorldItem* Candidate)
            {
                return Candidate && AreGuidsEqual(Candidate->ItemEntry.ItemGuid, Target.ItemGuid);
            });
        if (ItemInstance && *ItemInstance)
            BroadcastWorldItemAmmoChanged(*ItemInstance);

        SyncKnownWeaponAmmo(PlayerController, Target.WeaponDefinition, Target.ItemGuid,
            Target.MaxLoadedAmmo);
        if (IsTrackedRechargingWeaponAmmo(PlayerController, Target.ItemGuid))
        {
            ObserveRechargingWeaponAmmo(PlayerController, Target.WeaponDefinition, Target.ItemGuid,
                Target.ItemLevel, PreviousLoadedAmmo, Target.MaxLoadedAmmo);
        }

        ++ReloadedWeaponCount;
    }

    return ReloadedWeaponCount;
}

FFortItemEntry* AFortInventory::MakeItemEntry(const UFortItemDefinition* ItemDefinition,
    int32 Count, int32 Level)
{
    auto ItemEntry = (FFortItemEntry*)malloc(FFortItemEntry::Size());
    memset((PBYTE)ItemEntry, 0, FFortItemEntry::Size());

    ItemEntry->MostRecentArrayReplicationKey = -1;
    ItemEntry->ReplicationID = -1;
    ItemEntry->ReplicationKey = -1;

    auto ItemDef = ItemDefinition;
    if (auto MultiWorldItemDef = ItemDef->Cast<UFortWorldMultiItemDefinition>())
    {
        ItemDef = MultiWorldItemDef->ItemInfos.Get(rand() % MultiWorldItemDef->ItemInfos.Num(), FFortWorldMultiItemInfo::Size()).ItemDefinition;
    }
    ItemEntry->ItemDefinition = ItemDef;
    ItemEntry->Count = Count;
    ItemEntry->Durability = 1.f;
    ItemEntry->GameplayAbilitySpecHandle = FGameplayAbilitySpecHandle(-1);
    ItemEntry->ParentInventory.ObjectIndex = -1;
    ItemEntry->Level = Level;
    if (auto Weapon = ItemDef->IsA<UFortGadgetItemDefinition>() ? (UFortWeaponItemDefinition*)((UFortGadgetItemDefinition*)ItemDef)->GetWeaponItemDefinition() : ItemDef->Cast<UFortWeaponItemDefinition>())
    {
        auto Stats = GetStats(Weapon);
        if (Stats)
        {
            ItemEntry->LoadedAmmo = Stats->ClipSize;

            if (Weapon->HasbUsesPhantomReserveAmmo() && Weapon->bUsesPhantomReserveAmmo)
                ItemEntry->PhantomReserveAmmo = (Stats->InitialClips - 1) * Stats->ClipSize;
        }
    }
    if (auto WeaponDef = ItemDef->Cast<UFortWeaponItemDefinition>())
    {
        if (FFortWeaponMods::IsSupported() && WeaponDef->HasWeaponModSlots() &&
            FFortItemEntry::HasWeaponModSlots())
        {
            ItemEntry->WeaponModSlots = WeaponDef->WeaponModSlots;
        }
    }
    if (ItemEntry->HasPickupVariantIndex())
        ItemEntry->PickupVariantIndex = -1;
    if (ItemEntry->HasItemVariantDataMappingIndex())
        ItemEntry->ItemVariantDataMappingIndex = -1;
    if (ItemEntry->HasOrderIndex())
        ItemEntry->OrderIndex = -1;

    return ItemEntry;
}

uint64_t SetPickupItems;
AFortPickupAthena* AFortInventory::SpawnPickup(FVector Loc, FFortItemEntry& Entry,
    long long SourceTypeFlag, long long SpawnSource, AFortPlayerPawnAthena* Pawn, int OverrideCount,
    bool Toss, bool RandomRotation, bool bCombine, const UClass* OverrideClass, FVector FinalLoc)
{
    if (!&Entry)
        return nullptr;
    AFortPickupAthena* NewPickup = UWorld::SpawnActor<AFortPickupAthena>(
        OverrideClass ? OverrideClass : AFortPickupAthena::StaticClass(), Loc,
        {});
    if (!NewPickup)
        return nullptr;

    if (NewPickup->HasbRandomRotation())
        NewPickup->bRandomRotation = RandomRotation;
    if (Entry.Level != -1)
        NewPickup->PrimaryPickupItemEntry.Level = Entry.Level;
    NewPickup->PrimaryPickupItemEntry.ItemDefinition = Entry.ItemDefinition;
    NewPickup->PrimaryPickupItemEntry.LoadedAmmo = Entry.LoadedAmmo;
    NewPickup->PrimaryPickupItemEntry.Count = OverrideCount != -1 ? OverrideCount : Entry.Count;
    static auto HasPhantomReserveAmmo = Entry.HasPhantomReserveAmmo();
    if (HasPhantomReserveAmmo)
        NewPickup->PrimaryPickupItemEntry.PhantomReserveAmmo = Entry.PhantomReserveAmmo;

    bool bAllowRandomMods = false;
    if (FFortWeaponMods::IsSupported())
    {
        bAllowRandomMods = SourceTypeFlag != EFortPickupSourceTypeFlag::GetPlayer() &&
            SourceTypeFlag != EFortPickupSourceTypeFlag::GetTossed() &&
            SpawnSource != EFortPickupSpawnSource::GetPlayerElimination() &&
            SpawnSource != EFortPickupSpawnSource::GetTossedByPlayer();
    }
    FFortWeaponMods::InitializePickup(NewPickup, Entry, bAllowRandomMods);

    if (SetPickupItems)
    {
        TArray<FFortItemEntry> a{};
        if (VersionInfo.FortniteVersion >= 16)
            ((void(*)(AFortPickupAthena*, FFortItemEntry*, TArray<FFortItemEntry>*, uint8_t, bool, uint8_t)) SetPickupItems)(
                NewPickup, &NewPickup->PrimaryPickupItemEntry, &a,
                (uint8_t)EFortPickupSourceTypeFlag::GetContainer(), false,
                (uint8_t)EFortPickupSpawnSource::GetChest());
        else
            ((void(*)(AFortPickupAthena*, FFortItemEntry*, TArray<FFortItemEntry>*, bool)) SetPickupItems)(
                NewPickup, &NewPickup->PrimaryPickupItemEntry, &a, false);
    }
    else
        NewPickup->OnRep_PrimaryPickupItemEntry();
    NewPickup->PawnWhoDroppedPickup = Pawn;

    auto FinalLocation = Loc;

    if (FinalLoc.X || FinalLoc.Y || FinalLoc.Z)
        FinalLocation = FinalLoc;
    NewPickup->TossPickup(FinalLocation, Pawn, -1, Toss, true, (uint8)SourceTypeFlag, (uint8)SpawnSource);

    if (SpawnSource != -1)
        NewPickup->bTossedFromContainer = SpawnSource == EFortPickupSpawnSource::GetChest() || SpawnSource == EFortPickupSpawnSource::GetAmmoBox();
    if (NewPickup->bTossedFromContainer)
        NewPickup->OnRep_TossedFromContainer();

    if (!SetPickupItems && OverrideClass)
    {
        const UClass* GameModePickupClass = FindClass("FortGameModePickup");
        const UObject* PickupDefault = OverrideClass->GetDefaultObj();
        if (GameModePickupClass && PickupDefault && PickupDefault->IsA(GameModePickupClass))
        {
            UWorld* World = UWorld::GetWorld();
            auto GameState = World && World->GameState ? World->GameState
                          ->Cast<AFortGameStateAthena>() : nullptr;
            if (GameState && GameState->HasOnPickupSpawnedAndReady())
            {
                GameState->OnPickupSpawnedAndReady.Process(NewPickup,
                        const_cast<UFortItemDefinition*>(Entry.ItemDefinition));
            }
        }
    }

    return NewPickup;
}

AFortPickupAthena* AFortInventory::SpawnPickup(FVector Loc,
    const UFortItemDefinition* ItemDefinition, int Count, int LoadedAmmo, long long SourceTypeFlag,
    long long SpawnSource, AFortPlayerPawnAthena* Pawn, bool Toss, bool bRandomRotation,
    const UClass* OverrideClass)
{
    auto ItemEntry = MakeItemEntry(ItemDefinition, Count, -1);
    if (LoadedAmmo != -1)
        ItemEntry->LoadedAmmo = LoadedAmmo;

    auto Pickup = SpawnPickup(Loc, *ItemEntry, SourceTypeFlag, SpawnSource, Pawn, -1, Toss, true,
        bRandomRotation, OverrideClass);
    FFortWeaponMods::FreeEntrySlots(*ItemEntry);
    free(ItemEntry);
    return Pickup;
}

AFortPickupAthena* AFortInventory::SpawnPickup(ABuildingContainer* Container, FFortItemEntry& Entry,
    AFortPlayerPawnAthena* Pawn, int OverrideCount)
{
    if (!&Entry)
        return nullptr;

    auto ContainerLoc = Container->K2_GetActorLocation();
    auto SpawnLocation = Container->HasLootSpawnLocation_Athena() ? Container->LootSpawnLocation_Athena : Container->LootSpawnLocation;
    if (VersionInfo.FortniteVersion >= 24)
    {
        auto& ProperSpawnLoc = *(FVector3f*)(__int64(Container) + Container->LootSpawnLocation_Athena__Offset);

        SpawnLocation.X = ProperSpawnLoc.X;
        SpawnLocation.Y = ProperSpawnLoc.Y;
        SpawnLocation.Z = ProperSpawnLoc.Z;
    }
    auto Loc = ContainerLoc + (Container->GetActorForwardVector() * SpawnLocation.X) + (Container->GetActorRightVector() * SpawnLocation.Y) + (Container->GetActorUpVector() * SpawnLocation.Z);
    AFortPickupAthena* NewPickup = UWorld::SpawnActor<AFortPickupAthena>(Loc, {});

    if (!NewPickup)
        return nullptr;

    if (NewPickup->HasbRandomRotation())
        NewPickup->bRandomRotation = true;
    NewPickup->PrimaryPickupItemEntry.Level = Entry.Level;
    NewPickup->PrimaryPickupItemEntry.ItemDefinition = Entry.ItemDefinition;
    NewPickup->PrimaryPickupItemEntry.LoadedAmmo = Entry.LoadedAmmo;
    NewPickup->PrimaryPickupItemEntry.Count = OverrideCount != -1 ? OverrideCount : Entry.Count;
    static auto HasPhantomReserveAmmo = Entry.HasPhantomReserveAmmo();
    if (HasPhantomReserveAmmo)
        NewPickup->PrimaryPickupItemEntry.PhantomReserveAmmo = Entry.PhantomReserveAmmo;

    FFortWeaponMods::InitializePickup(NewPickup, Entry, true);

    if (SetPickupItems)
    {
        TArray<FFortItemEntry> a{};
        if (VersionInfo.FortniteVersion >= 16)
            ((void(*)(AFortPickupAthena*, FFortItemEntry*, TArray<FFortItemEntry>*, uint8_t, bool, uint8_t)) SetPickupItems)(
                NewPickup, &NewPickup->PrimaryPickupItemEntry, &a,
                (uint8_t)EFortPickupSourceTypeFlag::GetContainer(), false,
                (uint8_t)EFortPickupSpawnSource::GetChest());
        else
            ((void(*)(AFortPickupAthena*, FFortItemEntry*, TArray<FFortItemEntry>*, bool)) SetPickupItems)(
                NewPickup, &NewPickup->PrimaryPickupItemEntry, &a, false);
    }
    else
        NewPickup->OnRep_PrimaryPickupItemEntry();

    NewPickup->PawnWhoDroppedPickup = Pawn;

    static auto tpfcPtr = UFortKismetLibrary::GetDefaultObj()->GetFunction("TossPickupFromContainer");
    if (tpfcPtr)
    {
        if (!UFortKismetLibrary::TossPickupFromContainer__Ptr)
            UFortKismetLibrary::TossPickupFromContainer__Ptr = tpfcPtr;

        UFortKismetLibrary::TossPickupFromContainer(UWorld::GetWorld(), Container, NewPickup, 10,
            (int32)std::clamp((float)rand() * 0.0003357036f, 0.f, 10.f),
            Container->LootTossConeHalfAngle_Athena, Container->LootTossDirection_Athena,
            Container->LootTossSpeed_Athena, Container->bForceHidePickupMinimapIndicator);
    }
    else
    {
        auto FinalLoc = Loc + (Container->GetActorForwardVector() * Container->LootFinalLocation.X) + (Container->GetActorRightVector() * Container->LootFinalLocation.Y) + (Container->GetActorUpVector() * Container->LootFinalLocation.Z);

        NewPickup->TossPickup(Loc, Pawn, -1, true, true, EFortPickupSourceTypeFlag::GetContainer(),
            EFortPickupSpawnSource::GetChest());
    }

    NewPickup->bTossedFromContainer = true;
    NewPickup->OnRep_TossedFromContainer();

    return NewPickup;
}

bool AFortInventory::IsPrimaryQuickbar(const UFortItemDefinition* ItemDefinition)
{
    return
        ItemDefinition->ItemType == EFortItemType::GetWeaponHarvest() ||
        ItemDefinition->ItemType == EFortItemType::GetWorldResource() ||
        ItemDefinition->ItemType == EFortItemType::GetAmmo() ||
        ItemDefinition->ItemType == EFortItemType::GetTrap() ||
        ItemDefinition->ItemType == EFortItemType::GetBuildingPiece() ||
        ItemDefinition->ItemType == EFortItemType::GetEditTool() ||
        ItemDefinition->ItemType == EFortItemType::GetIngredient() ||
        (ItemDefinition->HasbForceIntoOverflow() && ItemDefinition->bForceIntoOverflow)
        ? false : true;
}

void AFortInventory::TickRegeneratingItems()
{
    if (RegeneratingInventoryItems.empty() && RechargingWeaponAmmo.empty() &&
        PendingGhostModeCleanups.empty() && TrackedGhostModeSessions.empty())
        return;

    auto World = UWorld::GetWorld();
    if (!World)
        return;

    const double NowSeconds = UGameplayStatics::GetTimeSeconds(World);

    TickTrackedGhostModeSessions(NowSeconds);
    TickPendingGhostModeCleanups(NowSeconds);

    for (size_t Index = 0;
        Index < RegeneratingInventoryItems.size();)
    {
        auto& State = RegeneratingInventoryItems[Index];
        auto Owner = State.Owner.Get();
        auto AmmoDefinition = State.AmmoDefinition.Get();

        if (!Owner || !Owner->WorldInventory || !AmmoDefinition || State.MaxCount <= 0 ||
            !std::isfinite(State.CooldownSeconds) || State.CooldownSeconds <= 0.0)
        {
            RemoveRegenItemAt(Index);
            continue;
        }

        auto Inventory = Owner->WorldInventory;
        auto ReplicatedEntry = Inventory->Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Candidate)
                {
                    return AreGuidsEqual(Candidate.ItemGuid, State.ItemGuid) &&
                        Candidate.ItemDefinition == AmmoDefinition;
                }, FFortItemEntry::Size());
        auto ItemInstance = Inventory->Inventory.ItemInstances.Search([&](UFortWorldItem* Candidate)
                {
                    return Candidate && AreGuidsEqual(Candidate->ItemEntry.ItemGuid,
                            State.ItemGuid) && Candidate->ItemEntry.ItemDefinition ==
                            AmmoDefinition;
                });

        if (!ReplicatedEntry || !ItemInstance || !*ItemInstance)
        {
            RemoveRegenItemAt(Index);
            continue;
        }

        if (ReplicatedEntry->Count >= State.MaxCount)
        {
            RemoveRegenItemAt(Index);
            continue;
        }

        if (NowSeconds < State.NextRefillTime)
        {
            ++Index;
            continue;
        }

        int32 NewCount = min(max(ReplicatedEntry->Count, 0) + 1, State.MaxCount);
        ReplicatedEntry->Count = NewCount;
        (*ItemInstance)->ItemEntry.Count = NewCount;
        (*ItemInstance)->ItemEntry.bIsDirty = true;

        const bool bReachedMaximum = NewCount >= State.MaxCount;
        const int32 MaximumCount = State.MaxCount;
        const auto DefinitionName = AmmoDefinition->Name.ToString();
        if (bReachedMaximum)
        {
            RemoveRegenItemAt(Index);
        }
        else
        {
            State.NextRefillTime = NowSeconds + State.CooldownSeconds;
            ++Index;
        }

        Inventory->UpdateEntry(*ReplicatedEntry);
        SDK::DbgLog("[ItemRegen] refilled definition=%s count=%d max=%d\n", DefinitionName.c_str(),
            NewCount, MaximumCount);
    }

    for (size_t Index = 0;
        Index < RechargingWeaponAmmo.size();)
    {
        auto& State = RechargingWeaponAmmo[Index];
        auto Owner = State.Owner.Get();
        auto WeaponDefinition = State.WeaponDefinition.Get();
        if (!Owner || !Owner->WorldInventory || !WeaponDefinition || State.MaxLoadedAmmo <= 0 ||
            State.RechargeAmount <= 0 || !std::isfinite(State.RechargeIntervalSeconds) ||
            State.RechargeIntervalSeconds <= 0.0)
        {
            RemoveRechargingWeaponAt(Index);
            continue;
        }

        auto Inventory = Owner->WorldInventory;
        auto ReplicatedEntry = Inventory->Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Candidate)
                {
                    return AreGuidsEqual(Candidate.ItemGuid, State.ItemGuid) &&
                        Candidate.ItemDefinition == WeaponDefinition;
                }, FFortItemEntry::Size());
        auto ItemInstance = Inventory->Inventory.ItemInstances.Search([&](UFortWorldItem* Candidate)
                {
                    return Candidate && AreGuidsEqual(Candidate->ItemEntry.ItemGuid,
                            State.ItemGuid) && Candidate->ItemEntry.ItemDefinition ==
                            WeaponDefinition;
                });
        if (!ReplicatedEntry || !ItemInstance || !*ItemInstance)
        {
            RemoveRechargingWeaponAt(Index);
            continue;
        }

        int32 CurrentLoadedAmmo = std::clamp(ReplicatedEntry->LoadedAmmo, 0, State.MaxLoadedAmmo);

        if (CurrentLoadedAmmo < State.LastObservedLoadedAmmo && !ResolveEquippedWeaponForItem(Owner,
                WeaponDefinition, State.ItemGuid))
        {
            CurrentLoadedAmmo = State.LastObservedLoadedAmmo;
            ReplicatedEntry->LoadedAmmo = CurrentLoadedAmmo;
            (*ItemInstance)->ItemEntry.LoadedAmmo = CurrentLoadedAmmo;
            (*ItemInstance)->ItemEntry.bIsDirty = true;
            Inventory->UpdateEntry(*ReplicatedEntry);
            BroadcastWorldItemAmmoChanged(*ItemInstance);
            SyncKnownWeaponAmmo(Owner, WeaponDefinition, State.ItemGuid, CurrentLoadedAmmo);
        }

        if (CurrentLoadedAmmo >= State.MaxLoadedAmmo)
        {
            if (State.LastObservedLoadedAmmo != State.MaxLoadedAmmo)
            {
                SyncKnownWeaponAmmo(Owner, WeaponDefinition, State.ItemGuid, State.MaxLoadedAmmo);
            }
            State.LastObservedLoadedAmmo = State.MaxLoadedAmmo;
            State.NextRefillTime = 0.0;
            ++Index;
            continue;
        }

        if (CurrentLoadedAmmo > State.LastObservedLoadedAmmo)
        {
            SyncKnownWeaponAmmo(Owner, WeaponDefinition, State.ItemGuid, CurrentLoadedAmmo);
            State.LastObservedLoadedAmmo = CurrentLoadedAmmo;
            State.NextRefillTime = NowSeconds + State.RechargeIntervalSeconds +
                NativeRechargeGraceSeconds;
            NotifyWeaponRechargeStarted(Owner, State.ItemGuid, NowSeconds);
            ++Index;
            continue;
        }
        bool bStartedRechargeCycle = false;
        if (CurrentLoadedAmmo <State.LastObservedLoadedAmmo)
        {
            State.LastObservedLoadedAmmo = CurrentLoadedAmmo;
            if (State.NextRefillTime <= 0.0)
            {
                State.NextRefillTime = NowSeconds + State.RechargeIntervalSeconds +
                    NativeRechargeGraceSeconds;
                bStartedRechargeCycle = true;
            }
        }
        else if (State.NextRefillTime <= 0.0)
        {
            State.NextRefillTime = NowSeconds + State.RechargeIntervalSeconds +
                NativeRechargeGraceSeconds;
            bStartedRechargeCycle = true;
        }
        if (bStartedRechargeCycle)
        {
            NotifyWeaponRechargeStarted(Owner, State.ItemGuid, NowSeconds);
        }

        if (NowSeconds < State.NextRefillTime)
        {
            ++Index;
            continue;
        }

        int32 NewLoadedAmmo = min(CurrentLoadedAmmo + State.RechargeAmount, State.MaxLoadedAmmo);
        ReplicatedEntry->LoadedAmmo = NewLoadedAmmo;
        auto WorldItem = *ItemInstance;
        WorldItem->ItemEntry.LoadedAmmo = NewLoadedAmmo;
        WorldItem->ItemEntry.bIsDirty = true;
        State.LastObservedLoadedAmmo = NewLoadedAmmo;

        const int32 MaximumLoadedAmmo = State.MaxLoadedAmmo;
        const auto DefinitionName = WeaponDefinition->Name.ToString();
        if (NewLoadedAmmo >= State.MaxLoadedAmmo)
        {
            State.NextRefillTime = 0.0;
            ++Index;
        }
        else
        {
            State.NextRefillTime = NowSeconds + State.RechargeIntervalSeconds +
                NativeRechargeGraceSeconds;
            NotifyWeaponRechargeStarted(Owner, State.ItemGuid, NowSeconds);
            ++Index;
        }

        Inventory->UpdateEntry(*ReplicatedEntry);
        BroadcastWorldItemAmmoChanged(WorldItem);
        const int32 SyncedWeaponActors = SyncKnownWeaponAmmo(Owner, WeaponDefinition,
                State.ItemGuid, NewLoadedAmmo);
        SDK::DbgLog("[WeaponRecharge] fallback-refilled "
            "definition=%s ammo=%d/%d actor-sync-count=%d\n", DefinitionName.c_str(), NewLoadedAmmo,
            MaximumLoadedAmmo, SyncedWeaponActors);
    }
}

bool AFortInventory::BeginTrackedRechargeEquip(AFortPlayerControllerAthena* PlayerController,
    const FGuid& ItemGuid)
{
    if (!PlayerController || !PlayerController->WorldInventory ||
        !FFortItemEntry::HasLoadedAmmo() || !FFortItemEntry::HasItemGuid() ||
        !FFortItemEntry::HasItemDefinition())
    {
        return false;
    }

    for (auto& State : RechargingWeaponAmmo)
    {
        if (State.Owner.Get() != PlayerController || !AreGuidsEqual(State.ItemGuid, ItemGuid))
        {
            continue;
        }

        auto WeaponDefinition = State.WeaponDefinition.Get();
        if (!WeaponDefinition)
            return false;

        auto Inventory = PlayerController->WorldInventory;
        auto ReplicatedEntry = Inventory->Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Candidate)
                {
                    return AreGuidsEqual(Candidate.ItemGuid, ItemGuid) &&
                        Candidate.ItemDefinition == WeaponDefinition;
                }, FFortItemEntry::Size());
        if (!ReplicatedEntry)
            return false;

        int32 SnapshotLoadedAmmo = std::clamp(ReplicatedEntry->LoadedAmmo, 0, State.MaxLoadedAmmo);
        if (!ResolveEquippedWeaponForItem(PlayerController, WeaponDefinition, ItemGuid))
        {
            SnapshotLoadedAmmo = max(SnapshotLoadedAmmo, State.LastObservedLoadedAmmo);
        }

        State.EquipSnapshotLoadedAmmo = std::clamp(SnapshotLoadedAmmo, 0, State.MaxLoadedAmmo);
        State.EquipSnapshotNextRefillTime = State.NextRefillTime;
        State.bEquipInProgress = true;
        return true;
    }

    return false;
}

void AFortInventory::FinishTrackedRechargeEquip(AFortPlayerControllerAthena* PlayerController,
    const FGuid& ItemGuid, AFortWeapon* EquippedWeapon)
{
    if (!PlayerController)
        return;

    for (auto& State : RechargingWeaponAmmo)
    {
        if (State.Owner.Get() != PlayerController || !AreGuidsEqual(State.ItemGuid, ItemGuid) ||
            !State.bEquipInProgress)
        {
            continue;
        }

        const int32 SnapshotLoadedAmmo = std::clamp(State.EquipSnapshotLoadedAmmo, 0,
            State.MaxLoadedAmmo);
        const double SnapshotNextRefillTime = State.EquipSnapshotNextRefillTime;
        State.bEquipInProgress = false;
        State.LastObservedLoadedAmmo = SnapshotLoadedAmmo;
        State.NextRefillTime = SnapshotLoadedAmmo >= State.MaxLoadedAmmo ? 0.0
                : SnapshotNextRefillTime;

        auto WeaponDefinition = State.WeaponDefinition.Get();
        auto Inventory = PlayerController->WorldInventory;
        if (!WeaponDefinition || !Inventory)
            return;

        auto ReplicatedEntry = Inventory->Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Candidate)
                {
                    return AreGuidsEqual(Candidate.ItemGuid, ItemGuid) &&
                        Candidate.ItemDefinition == WeaponDefinition;
                }, FFortItemEntry::Size());
        auto ItemInstance = Inventory->Inventory.ItemInstances.Search([&](UFortWorldItem* Candidate)
                {
                    return Candidate && AreGuidsEqual(Candidate->ItemEntry.ItemGuid, ItemGuid) &&
                        Candidate->ItemEntry.ItemDefinition == WeaponDefinition;
                });

        bool bInventoryChanged = false;
        if (ReplicatedEntry && ReplicatedEntry->LoadedAmmo != SnapshotLoadedAmmo)
        {
            ReplicatedEntry->GetLoadedAmmo() = SnapshotLoadedAmmo;
            bInventoryChanged = true;
        }
        if (ItemInstance && *ItemInstance && (*ItemInstance)->ItemEntry.LoadedAmmo !=
                SnapshotLoadedAmmo)
        {
            (*ItemInstance)->ItemEntry.GetLoadedAmmo() = SnapshotLoadedAmmo;
            (*ItemInstance)->ItemEntry.bIsDirty = true;
            bInventoryChanged = true;
        }
        if (bInventoryChanged && ReplicatedEntry)
            Inventory->UpdateEntry(*ReplicatedEntry);
        if (bInventoryChanged && ItemInstance && *ItemInstance)
            BroadcastWorldItemAmmoChanged(*ItemInstance);

        bool bReturnedWeaponSynced = false;
        if (EquippedWeapon && IsLiveRechargeObject(EquippedWeapon) &&
            EquippedWeapon->HasItemEntryGuid() && AreGuidsEqual(EquippedWeapon->ItemEntryGuid,
                ItemGuid) && (!EquippedWeapon->HasWeaponData() || EquippedWeapon->WeaponData ==
                    WeaponDefinition))
        {
            bReturnedWeaponSynced = SyncWeaponAmmo(EquippedWeapon, SnapshotLoadedAmmo);
        }

        const int32 SyncedKnownActors = bReturnedWeaponSynced ? 1 : SyncKnownWeaponAmmo(
                    PlayerController, WeaponDefinition, ItemGuid, SnapshotLoadedAmmo);

        if (SnapshotLoadedAmmo < State.MaxLoadedAmmo)
        {
            auto World = UWorld::GetWorld();
            const double NowSeconds = World ? UGameplayStatics::GetTimeSeconds(World) : 0.0;
            if (State.NextRefillTime <= 0.0 && World)
            {
                State.NextRefillTime = NowSeconds + State.RechargeIntervalSeconds +
                    NativeRechargeGraceSeconds;
            }

            if (State.NextRefillTime > 0.0)
            {
                const double OriginalStartTime = max(0.0, State.NextRefillTime -
                        State.RechargeIntervalSeconds - NativeRechargeGraceSeconds);
                NotifyWeaponRechargeStarted(PlayerController, ItemGuid, OriginalStartTime);
            }
        }

        SDK::DbgLog("[WeaponRecharge] equip-reconciled "
            "definition=%s ammo=%d/%d inventory-restored=%d "
            "returned-sync=%d actor-sync-count=%d\n", WeaponDefinition->Name.ToString().c_str(),
            SnapshotLoadedAmmo, State.MaxLoadedAmmo, bInventoryChanged ? 1 : 0,
            bReturnedWeaponSynced ? 1 : 0, SyncedKnownActors);
        return;
    }
}

void AFortInventory::BeginNativeDeathInventoryRetention(
    AFortPlayerControllerAthena* PlayerController, const std::vector<FGuid>& ItemGuids)
{
    if (!PlayerController || ItemGuids.empty())
        return;

    NativeDeathInventoryRetention[PlayerController] = ItemGuids;
}

void AFortInventory::EndNativeDeathInventoryRetention(AFortPlayerControllerAthena* PlayerController)
{
    if (PlayerController)
        NativeDeathInventoryRetention.erase(PlayerController);
}

void AFortInventory::UpdateEntry(FFortItemEntry& Entry)
{
    if (!this)
        return; // wtf 3.5

    Update(&Entry);
}

using RemoveInventoryItemWithQuickBarFn = bool(*)(IInterface*, FGuid&, int32, bool, bool);

using RemoveInventoryItemSingleFlagFn = bool(*)(IInterface*, FGuid&, int32, bool);

enum class ERemoveInventoryItemAbi
{
    None, QuickBarFlagOnly, WithoutQuickBarFlag, WithQuickBarFlag
};

RemoveInventoryItemWithQuickBarFn
    RemoveInventoryItemWithQuickBarOG = nullptr;
RemoveInventoryItemSingleFlagFn
    RemoveInventoryItemQuickBarOnlyOG = nullptr;
RemoveInventoryItemSingleFlagFn
    RemoveInventoryItemWithoutQuickBarOG = nullptr;
ERemoveInventoryItemAbi RemoveInventoryItemAbi = ERemoveInventoryItemAbi::None;

bool CallRemoveInventoryItemOriginal(IInterface* Interface, FGuid& ItemGuid, int32 Count,
    bool bForceRemoveFromQuickBars, bool bForceRemoval)
{
    if (RemoveInventoryItemAbi == ERemoveInventoryItemAbi::WithQuickBarFlag &&
        RemoveInventoryItemWithQuickBarOG)
    {
        return RemoveInventoryItemWithQuickBarOG(Interface, ItemGuid, Count,
            bForceRemoveFromQuickBars, bForceRemoval);
    }

    if (RemoveInventoryItemAbi == ERemoveInventoryItemAbi::QuickBarFlagOnly &&
        RemoveInventoryItemQuickBarOnlyOG)
    {
        return RemoveInventoryItemQuickBarOnlyOG(Interface, ItemGuid, Count,
            bForceRemoveFromQuickBars);
    }

    if (RemoveInventoryItemAbi == ERemoveInventoryItemAbi::WithoutQuickBarFlag &&
        RemoveInventoryItemWithoutQuickBarOG)
    {
        return RemoveInventoryItemWithoutQuickBarOG(Interface, ItemGuid, Count, bForceRemoval);
    }

    return false;
}

bool RemoveInventoryItemInternal(IInterface* Interface, FGuid& ItemGuid, int32 Count,
    bool bForceRemoveFromQuickBars, bool bForceRemoval)
{
    if (!Interface)
    {
        return CallRemoveInventoryItemOriginal(Interface, ItemGuid, Count,
            bForceRemoveFromQuickBars, bForceRemoval);
    }

    auto PlayerControllerClass = FindClass("FortPlayerController");
    auto PlayerControllerSuper = PlayerControllerClass ? PlayerControllerClass->GetSuper()
            : nullptr;
    if (!PlayerControllerSuper)
    {
        return CallRemoveInventoryItemOriginal(Interface, ItemGuid, Count,
            bForceRemoveFromQuickBars, bForceRemoval);
    }

    const uint64 InterfaceOffset = static_cast<uint64>(PlayerControllerSuper->GetPropertiesSize()) +
        (VersionInfo.EngineVersion >= 4.27 ? 16ull : 8ull);
    const uint64 InterfaceAddress = reinterpret_cast<uint64>(Interface);
    if (InterfaceAddress < InterfaceOffset)
    {
        return CallRemoveInventoryItemOriginal(Interface, ItemGuid, Count,
            bForceRemoveFromQuickBars, bForceRemoval);
    }

    auto PlayerController = reinterpret_cast<AFortPlayerControllerAthena*>(
            InterfaceAddress - InterfaceOffset);
    if (!SDK::MemReadable(PlayerController, sizeof(void*)))
    {
        return CallRemoveInventoryItemOriginal(Interface, ItemGuid, Count,
            bForceRemoveFromQuickBars, bForceRemoval);
    }

    if (auto Retention = NativeDeathInventoryRetention.find(PlayerController);
        Retention != NativeDeathInventoryRetention.end())
    {
        const bool bRetainItem = std::any_of(Retention->second.begin(), Retention->second.end(),
                [&](const FGuid& RetainedGuid)
                {
                    return
                        RetainedGuid.A == ItemGuid.A && RetainedGuid.B == ItemGuid.B &&
                        RetainedGuid.C == ItemGuid.C && RetainedGuid.D == ItemGuid.D;
                });
        if (bRetainItem)
            return true;
    }

    auto WorldInventory = PlayerController->WorldInventory;
    if (!WorldInventory || !SDK::MemReadable(WorldInventory, sizeof(void*)) || Count == 0)
    {
        return CallRemoveInventoryItemOriginal(Interface, ItemGuid, Count,
            bForceRemoveFromQuickBars, bForceRemoval);
    }

    auto ItemInstance = WorldInventory->Inventory.ItemInstances.Search([&](UFortWorldItem* Item)
            {
                return Item && Item->ItemEntry.ItemGuid == ItemGuid;
            });
    auto ItemEntry = WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& Entry)
            {
                return Entry.ItemGuid == ItemGuid;
            }, FFortItemEntry::Size());
    if (!ItemEntry)
    {
        return CallRemoveInventoryItemOriginal(Interface, ItemGuid, Count,
            bForceRemoveFromQuickBars, bForceRemoval);
    }
    auto ItemDefinition = ItemEntry->ItemDefinition;

    if (!UFortKismetLibrary::IsGhostModeItemDefinition(ItemDefinition) &&
        AFortInventory::ShouldBypassItemConsumption(PlayerController, Count, bForceRemoval))
    {
        return true;
    }

    if (!ItemInstance || !*ItemInstance)
    {
        if (UFortKismetLibrary::IsGhostModeItemDefinition(ItemDefinition))
        {
            const int32 ExistingCount = max(ItemEntry->Count, 0);
            const bool bTerminalRemoval = Count < 0 || bForceRemoval || ExistingCount <= 0 ||
                Count >= ExistingCount;
            if (bTerminalRemoval)
            {
                WorldInventory->Remove(ItemGuid);
                SDK::DbgLog("[GhostMode] removed instance-less backing "
                    "entry through inventory lifecycle "
                    "controller=%p count=%d\n", static_cast<void*>(PlayerController),
                    ExistingCount);
            }
            else
            {
                ItemEntry->Count = max(ExistingCount - Count, 0);
                WorldInventory->UpdateEntry(*ItemEntry);
            }
            return true;
        }

        return CallRemoveInventoryItemOriginal(Interface, ItemGuid, Count,
            bForceRemoveFromQuickBars, bForceRemoval);
    }

    auto Item = *ItemInstance;
    const bool bForceRemoveItem = bForceRemoveFromQuickBars || bForceRemoval;

    auto AmmoDefinition = ItemDefinition ? ItemDefinition->Cast<UFortAmmoItemDefinition>()
            : nullptr;

    const float ItemLevel = Item->ItemEntry.Level > 0 ? static_cast<float>(Item->ItemEntry.Level)
            : 1.0f;
    double RegenCooldownSeconds = 0.0;
    int32 RegenMaximumCount = 0;
    if (Count > 0 && !bForceRemoveItem && AmmoDefinition && AmmoDefinition->HasRegenCooldown())
    {
        RegenCooldownSeconds = AmmoDefinition->EvaluateRegenCooldown(ItemLevel);

        RegenMaximumCount = max(AmmoDefinition->GetMaxStackSize(), ItemEntry->Count);
        if (RegenMaximumCount <= 0 || RegenMaximumCount > 10000 ||
            !std::isfinite(RegenCooldownSeconds) || RegenCooldownSeconds <= 0.0 ||
            RegenCooldownSeconds > 3600.0)
        {
            RegenCooldownSeconds = 0.0;
            RegenMaximumCount = 0;
        }
    }

    bool bKeepFinalStackEmpty = false;
    if (Count > 0 && !bForceRemoveItem && ItemDefinition &&
        ItemDefinition->HasbPersistInInventoryWhenFinalStackEmpty() &&
        ItemDefinition->bPersistInInventoryWhenFinalStackEmpty)
    {
        auto OtherStack = WorldInventory->Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Entry)
                {
                    return Entry.ItemDefinition == ItemDefinition && Entry.ItemGuid != ItemGuid &&
                        Entry.Count > 0;
                }, FFortItemEntry::Size());
        bKeepFinalStackEmpty = !OtherStack;
    }

    const int32 RemovedCount = WorldInventory->RemoveItem(ItemGuid, Count, bKeepFinalStackEmpty);

    if (RemovedCount > 0 && RegenMaximumCount > 0)
    {
        ScheduleRegeneratingInventoryItem(PlayerController, AmmoDefinition, ItemGuid,
            RegenMaximumCount, RegenCooldownSeconds);
    }

    return true;
}

bool RemoveInventoryItemWithQuickBar(IInterface* Interface, FGuid& ItemGuid, int32 Count,
    bool bForceRemoveFromQuickBars, bool bForceRemoval)
{
    return RemoveInventoryItemInternal(Interface, ItemGuid, Count, bForceRemoveFromQuickBars,
        bForceRemoval);
}

bool RemoveInventoryItemWithoutQuickBar(IInterface* Interface, FGuid& ItemGuid, int32 Count,
    bool bForceRemoval)
{
    return RemoveInventoryItemInternal(Interface, ItemGuid, Count, false, bForceRemoval);
}

bool RemoveInventoryItemQuickBarOnly(IInterface* Interface, FGuid& ItemGuid, int32 Count,
    bool bForceRemoveFromQuickBars)
{
    return RemoveInventoryItemInternal(Interface, ItemGuid, Count, bForceRemoveFromQuickBars,
        false);
}

ERemoveInventoryItemAbi ResolveRemoveInventoryItemAbi()
{
    const auto ControllerClass = AFortPlayerControllerAthena::StaticClass();
    const auto ControllerDefault = ControllerClass ? ControllerClass->GetDefaultObj() : nullptr;
    const auto ServerRemoveInventoryItem = ControllerDefault ? ControllerDefault->GetFunction(
                "ServerRemoveInventoryItem") : nullptr;

    if (ServerRemoveInventoryItem)
    {
        constexpr uint64 CPF_Parm = 0x0000000000000080;
        constexpr uint64 CPF_ReturnParm = 0x0000000000000400;
        const auto Parameters = ServerRemoveInventoryItem->GetParamsNamed();
        const bool bHasMetadata = !Parameters.NameOffsetMap.empty();
        bool bHasItemGuid = false;
        bool bHasCount = false;
        bool bHasForceRemoval = false;
        bool bHasQuickBarFlag = false;
        bool bHasUnknownParameter = false;

        for (const auto& Parameter : Parameters.NameOffsetMap)
        {
            if (!(Parameter.PropertyFlags & CPF_Parm) || (Parameter.PropertyFlags & CPF_ReturnParm))
            {
                continue;
            }

            if (Parameter.Name == "ItemGuid" || Parameter.Name == "ItemGUID")
            {
                bHasItemGuid = true;
            }
            else if (Parameter.Name == "Count")
            {
                bHasCount = true;
            }
            else if (Parameter.Name == "bForceRemoveFromQuickBars" || Parameter.Name ==
                    "bForceRemoveFromQuickbars")
            {
                bHasQuickBarFlag = true;
            }
            else if (Parameter.Name == "bForceRemoval")
            {
                bHasForceRemoval = true;
            }
            else if (Parameter.Name != "bForcePersistWhenEmpty")
            {
                bHasUnknownParameter = true;
            }
        }

        if (bHasItemGuid && bHasCount && bHasForceRemoval && bHasQuickBarFlag &&
            !bHasUnknownParameter)
        {
            return ERemoveInventoryItemAbi::WithQuickBarFlag;
        }

        if (bHasItemGuid && bHasCount && bHasQuickBarFlag && !bHasForceRemoval &&
            !bHasUnknownParameter)
        {
            return ERemoveInventoryItemAbi::QuickBarFlagOnly;
        }

        if (bHasItemGuid && bHasCount && bHasForceRemoval && !bHasQuickBarFlag &&
            !bHasUnknownParameter)
        {
            return ERemoveInventoryItemAbi::WithoutQuickBarFlag;
        }

        if (bHasMetadata)
            return ERemoveInventoryItemAbi::None;
    }

    const double FortniteVersion = VersionInfo.FortniteVersion;
    if (FortniteVersion == 1.72)
    {
        return ERemoveInventoryItemAbi::QuickBarFlagOnly;
    }

    const bool bKnownFiveArgumentBuild = (FortniteVersion >= 1.91 && FortniteVersion <= 6.00) ||
        FortniteVersion == 1.10 || FortniteVersion == 1.11 || FortniteVersion == 10.40 ||
        FortniteVersion == 13.40;
    if (bKnownFiveArgumentBuild)
    {
        return ERemoveInventoryItemAbi::WithQuickBarFlag;
    }

    return ERemoveInventoryItemAbi::None;
}

void SetLoadedAmmo(UFortWorldItem* Item, int LoadedAmmo)
{
    if (!Item)
        return;

    const int32 PreviousItemLoadedAmmo = Item->ItemEntry.LoadedAmmo;
    Item->ItemEntry.LoadedAmmo = LoadedAmmo;
    Item->ItemEntry.bIsDirty = true;

    auto PlayerController = (AFortPlayerControllerAthena*)Item->GetOwningController();
    if (!PlayerController || !PlayerController->WorldInventory)
        return;

    auto repEnt = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& item)
        { return item.ItemGuid == Item->ItemEntry.ItemGuid; }, FFortItemEntry::Size());
    if (!repEnt)
        return;

    const int32 PreviousLoadedAmmo = repEnt == &Item->ItemEntry ? PreviousItemLoadedAmmo
            : repEnt->LoadedAmmo;
    auto WeaponDefinition = Item->ItemEntry.ItemDefinition ? Item->ItemEntry.ItemDefinition->Cast<
                UFortWeaponItemDefinition>() : nullptr;

    repEnt->LoadedAmmo = LoadedAmmo;
    PlayerController->WorldInventory->UpdateEntry(*repEnt);
    BroadcastWorldItemAmmoChanged(Item);

    if (WeaponDefinition && IsTrackedRechargingWeaponAmmo(PlayerController,
            Item->ItemEntry.ItemGuid))
    {
        ObserveRechargingWeaponAmmo(PlayerController, WeaponDefinition, Item->ItemEntry.ItemGuid,
            Item->ItemEntry.Level, PreviousLoadedAmmo, LoadedAmmo);
    }
}

void SetPhantomReserveAmmo(UFortWorldItem* Item, unsigned int PhantomReserveAmmo)
{
    if (!Item)
        return;

    auto PlayerController = (AFortPlayerControllerAthena*)Item->GetOwningController();
    if (!PlayerController || !PlayerController->WorldInventory)
        return;

    auto repEnt = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& item)
        { return item.ItemGuid == Item->ItemEntry.ItemGuid; }, FFortItemEntry::Size());
    if (!repEnt)
        return;

    const int32 PreviousPhantomReserveAmmo = repEnt->PhantomReserveAmmo;
    auto WeaponDefinition = Item->ItemEntry.ItemDefinition ? Item->ItemEntry.ItemDefinition->Cast<
                UFortWeaponItemDefinition>() : nullptr;
    const bool bPreservePhantomReserve = AFortInventory::ShouldBypassItemConsumption(
            PlayerController, 1, false) && ResolveEquippedWeaponForItem(PlayerController,
            WeaponDefinition, Item->ItemEntry.ItemGuid) && PreviousPhantomReserveAmmo > 0 &&
        PhantomReserveAmmo <= static_cast<unsigned int>((std::numeric_limits<int32>::max)()) &&
        static_cast<int32>(PhantomReserveAmmo) <PreviousPhantomReserveAmmo;
    int32 AppliedPhantomReserveAmmo = bPreservePhantomReserve ? PreviousPhantomReserveAmmo
            : static_cast<int32>(PhantomReserveAmmo);

    repEnt->PhantomReserveAmmo = AppliedPhantomReserveAmmo;
    Item->ItemEntry.PhantomReserveAmmo = AppliedPhantomReserveAmmo;
    PlayerController->WorldInventory->UpdateEntry(*repEnt);
    Item->ItemEntry.bIsDirty = true;
}

void SpawnPickup_(UObject* Object, FFrame& Stack, AFortPickupAthena** Ret)
{
    UFortItemDefinition* ItemDefinition;
    int32 NumberToSpawn;
    AFortPlayerPawnAthena* TriggeringPawn;
    FVector Position;
    FVector Direction;
    Stack.StepCompiledIn(&ItemDefinition);
    Stack.StepCompiledIn(&NumberToSpawn);
    Stack.StepCompiledIn(&TriggeringPawn);
    Stack.StepCompiledIn(&Position);
    Stack.StepCompiledIn(&Direction);
    Stack.IncrementCode();

    *Ret = AFortInventory::SpawnPickup(Position, ItemDefinition, NumberToSpawn, -1,
        EFortPickupSourceTypeFlag::GetOther(), EFortPickupSpawnSource::GetSupplyDrop());
}

void SpawnGameModePickup_(UObject* Object, FFrame& Stack, AFortPickupAthena** Ret)
{
    UFortWorldItemDefinition* ItemDefinition = nullptr;
    TSubclassOf<AFortPickupAthena> PickupClass{};
    int32 NumberToSpawn = 0;
    AFortPlayerPawnAthena* TriggeringPawn = nullptr;
    FVector Position{};
    FVector Direction{};
    Stack.StepCompiledIn(&ItemDefinition);
    Stack.StepCompiledIn(&PickupClass);
    Stack.StepCompiledIn(&NumberToSpawn);
    Stack.StepCompiledIn(&TriggeringPawn);
    Stack.StepCompiledIn(&Position);
    Stack.StepCompiledIn(&Direction);
    Stack.IncrementCode();

    *Ret = nullptr;
    if (!ItemDefinition || NumberToSpawn <= 0)
        return;

    const UClass* GameModePickupClass = FindClass("FortGameModePickup");
    const UClass* OverrideClass = PickupClass.Get();
    if (!OverrideClass)
        OverrideClass = GameModePickupClass;
    const UObject* PickupDefault = OverrideClass ? OverrideClass->GetDefaultObj() : nullptr;
    if (!GameModePickupClass || !PickupDefault || !PickupDefault->IsA(GameModePickupClass))
    {
        SDK::DbgLog("[SupplyDrop] rejected invalid game-mode pickup class=%p item=%p\n",
            (void*)OverrideClass, (void*)ItemDefinition);
        return;
    }

    const bool bCurrentAshtonStone = FFortAthenaNativeLTMCompatibility::IsCurrentAshtonStone(
                TriggeringPawn, ItemDefinition);
    if (bCurrentAshtonStone)
    {
        if (FFortAthenaNativeLTMCompatibility::IsAshtonStoneCaptured(ItemDefinition))
        {
            SDK::DbgLog("[Ashton1040] suppressed deferred pickup "
                "spawn for captured stone item=%s\n", ItemDefinition->Name.ToString().c_str());
            return;
        }

        UWorld* World = UWorld::GetWorld();
        auto ExistingActors = World ? UGameplayStatics::GetAllActorsOfClass(
                      World, GameModePickupClass) : TArray<AActor*>{};
        if (ExistingActors.Num() >= 0 && ExistingActors.Num() <= 256 && ExistingActors.Max() >=
                ExistingActors.Num() && ExistingActors.Max() <= 512)
        {
            for (auto Actor : ExistingActors)
            {
                auto ExistingPickup = Actor && Actor->IsA(GameModePickupClass) ? Actor->Cast<
                              AFortPickupAthena>() : nullptr;
                if (!ExistingPickup || !ExistingPickup->HasAuthority() || (ExistingPickup
                         ->HasbActorIsBeingDestroyed() && ExistingPickup->bActorIsBeingDestroyed) ||
                    (ExistingPickup->HasbPickedUp() && ExistingPickup->bPickedUp) || ExistingPickup
                            ->PrimaryPickupItemEntry.ItemDefinition != ItemDefinition)
                {
                    continue;
                }

                ExistingPickup->SetLifeSpan(0.0f);
                ExistingPickup->ForceNetUpdate();
                *Ret = ExistingPickup;
                SDK::DbgLog("[Ashton1040] reused existing stone "
                    "pickup for deferred carrier callback "
                    "item=%s pickup=%p class=%s\n", ItemDefinition->Name.ToString().c_str(),
                    static_cast<void*>(ExistingPickup), ExistingPickup->Class->Name
                        .ToString().c_str());
                break;
            }
        }
        ExistingActors.Free();
        if (*Ret)
            return;
    }

    *Ret = AFortInventory::SpawnPickup(Position, ItemDefinition, NumberToSpawn, -1,
        EFortPickupSourceTypeFlag::GetOther(), EFortPickupSpawnSource::GetSupplyDrop(),
        TriggeringPawn, true, false, OverrideClass);

    if (*Ret)
    {
        (*Ret)->SetLifeSpan(0.0f);
        (*Ret)->ForceNetUpdate();
    }

    SDK::DbgLog(
        "[SupplyDrop] game-mode pickup item=%p class=%p result=%p direction=(%.1f,%.1f,%.1f)\n",
        (void*)ItemDefinition, (void*)OverrideClass, (void*)*Ret, Direction.X, Direction.Y,
        Direction.Z);
}

void RemoveInventoryStateValue()
{
    printf("Sup\n");
}

void SetInventoryStateValue()
{
    printf("Sup2\n");
}

void AFortInventory::PostLoadHook()
{
    SDK::DbgLog("  [FI] 0 pre-FindSetPickupItems\n");
    SetPickupItems = FindSetPickupItems();
    SDK::DbgLog("  [FI] 0a pre-FindApplyGadgetData\n");
    ApplyGadgetDataAddress = FindApplyGadgetData();
    SDK::DbgLog("  [FI] 0b pre-FindOnItemInstanceAddedVft\n");
    OnItemInstanceAddedVft = FindOnItemInstanceAddedVft();
    SDK::DbgLog("  [FI] 0c pre-FindClearAbility\n");
    ClearAbility_ = FindClearAbility();
    SDK::DbgLog("  [FI] 1 finds done\n");

    const auto RemoveInventoryItemAddress = FindRemoveInventoryItem();
    RemoveInventoryItemAbi = ResolveRemoveInventoryItemAbi();

    bool bRemoveInventoryItemHooked = false;
    if (RemoveInventoryItemAddress && RemoveInventoryItemAbi ==
            ERemoveInventoryItemAbi::WithQuickBarFlag)
    {
        Utils::Hook(RemoveInventoryItemAddress, RemoveInventoryItemWithQuickBar,
            RemoveInventoryItemWithQuickBarOG);
        bRemoveInventoryItemHooked = RemoveInventoryItemWithQuickBarOG != nullptr;
    }
    else if (RemoveInventoryItemAddress && RemoveInventoryItemAbi ==
            ERemoveInventoryItemAbi::QuickBarFlagOnly)
    {
        Utils::Hook(RemoveInventoryItemAddress, RemoveInventoryItemQuickBarOnly,
            RemoveInventoryItemQuickBarOnlyOG);
        bRemoveInventoryItemHooked = RemoveInventoryItemQuickBarOnlyOG != nullptr;
    }
    else if (RemoveInventoryItemAddress && RemoveInventoryItemAbi ==
            ERemoveInventoryItemAbi::WithoutQuickBarFlag)
    {
        Utils::Hook(RemoveInventoryItemAddress, RemoveInventoryItemWithoutQuickBar,
            RemoveInventoryItemWithoutQuickBarOG);
        bRemoveInventoryItemHooked = RemoveInventoryItemWithoutQuickBarOG != nullptr;
    }

    const char* RemoveInventoryItemAbiName = "unresolved";
    switch (RemoveInventoryItemAbi)
    {
    case ERemoveInventoryItemAbi::QuickBarFlagOnly:
        RemoveInventoryItemAbiName = "quickbar-only";
        break;
    case ERemoveInventoryItemAbi::WithoutQuickBarFlag:
        RemoveInventoryItemAbiName = "force-removal-only";
        break;
    case ERemoveInventoryItemAbi::WithQuickBarFlag:
        RemoveInventoryItemAbiName = "quickbar-and-force-removal";
        break;
    default:
        break;
    }
    SDK::DbgLog("  [FI] 2 RemoveInventoryItem %s "
        "(abi=%s target=%p)\n", bRemoveInventoryItemHooked ? "hooked" : "unavailable",
        RemoveInventoryItemAbiName, reinterpret_cast<void*>(RemoveInventoryItemAddress));

    auto SetOwningInventory = Memcury::Scanner::FindPattern("48 85 D2 74 ? 80 BA ? ? ? ? ? 75 ? 48 89 91").Get();
    if (!SetOwningInventory)
        SetOwningInventory = Memcury::Scanner::FindPattern("48 83 EC ? 48 85 D2 74 ? 80 BA ? ? ? ? ? 75 ? 48 81 C1").Get();
    if (!SetOwningInventory)
        SetOwningInventory = Memcury::Scanner::FindPattern("48 85 D2 74 ? 48 89 5C 24 ? 57 48 83 EC ? 48 8B F9 48 8B DA 48 8B CA E8 ? ? ? ? 83 F8 ? 75").Get();

    if (SetOwningInventory)
    {
        auto WorldItemVft = UFortWorldItem::GetDefaultObj()->Vft;
        int SetOwningInventoryIdx = 0;

        for (int i = 0; i < 0x200; i++)
        {
            if (WorldItemVft[i] == (void*)SetOwningInventory)
            {
                SetOwningInventoryIdx = i;
                break;
            }
        }

        if (SetOwningInventoryIdx)
        {
            auto HasPhantomReserveAmmo = FFortItemEntry::HasPhantomReserveAmmo();
            const uint32 LoadedAmmoSetterIndex = uint32(SetOwningInventoryIdx -
                    (HasPhantomReserveAmmo ? (VersionInfo.EngineVersion < 4.27 ? 2 : 3) : 1));

            Utils::Hook<UFortWorldItem>(LoadedAmmoSetterIndex, SetLoadedAmmo);
            if (HasPhantomReserveAmmo)
                Utils::Hook<UFortWorldItem>(uint32(SetOwningInventoryIdx - (VersionInfo.EngineVersion < 4.27 ? 1 : 2)), SetPhantomReserveAmmo);

            SDK::DbgLog("[WeaponRecharge] loaded-ammo setter hook "
                "installed index=%u\n", LoadedAmmoSetterIndex);
        }
        else
        {
            SDK::DbgLog("[WeaponRecharge] loaded-ammo setter hook "
                "unavailable: owner setter vft index missing\n");
        }
    }
    else
    {
        SDK::DbgLog("[WeaponRecharge] loaded-ammo setter hook "
            "unavailable: owner setter signature missing\n");
    }

    SDK::DbgLog("  [FI] 3 SetOwningInventory block done\n");
    if (auto sd = DefaultObjImpl("FortAthenaSupplyDrop"))
    {
        if (auto SpawnPickupFunction = sd->GetFunction("SpawnPickup"))
        {
            Utils::ExecHook(SpawnPickupFunction, SpawnPickup_);
        }
        if (auto SpawnGameModePickupFunction = sd->GetFunction("SpawnGameModePickup"))
        {
            Utils::ExecHook(SpawnGameModePickupFunction, SpawnGameModePickup_);
        }
    }
    SDK::DbgLog("  [FI] 4 PostLoadHook complete\n");
}
