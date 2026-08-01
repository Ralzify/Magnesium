#include "pch.h"
#include "../Public/FortInventory.h"
#include "../Public/FortPlayerPawnAthena.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortGameStateAthena.h"
#include "../Public/FortAthenaMutator.h"
#include "../Public/FortKismetLibrary.h"
#include "../../Erbium/Public/Configuration.h"
#include "../../Erbium/PlayerAI/Public/VersionFeatureAdapter.h"
#include "../Public/FortWeapon.h"
#include "../Public/FortWeaponMods.h"
#include <ShlObj.h>
#include <cmath>
#include <limits>
#include <vector>

uint32_t OnItemInstanceAddedVft;
uint64_t ApplyGadgetDataAddress;

namespace
{
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
    };

    std::vector<FRegeneratingInventoryItem> RegeneratingInventoryItems;
    std::vector<FRechargingWeaponAmmo> RechargingWeaponAmmo;
    std::unordered_map<
        AFortPlayerControllerAthena*,
        std::vector<FGuid>>
        NativeDeathInventoryRetention;
    std::unordered_map<
        AFortPlayerControllerAthena*,
        FGuid>
        PendingCarmineFocus;

    constexpr size_t MaxTrackedRechargingWeapons = 128;
    constexpr double NativeRechargeGraceSeconds = 0.10;

    bool IsExact1040CarmineGadget(
        const UFortItemDefinition* Definition)
    {
        return VersionInfo.FortniteVersion == 10.40 &&
            Definition &&
            (Definition->Name.ToWString() ==
                 L"AGID_CarminePack" ||
             Definition->Name.ToWString() ==
                 L"AGID_AshtonPack");
    }

    bool IsExact1040BaseAshtonGadget(
        const UFortItemDefinition* Definition)
    {
        return VersionInfo.FortniteVersion == 10.40 &&
            Definition &&
            Definition->Name.ToWString() ==
                L"AGID_AshtonPack";
    }

    bool IsExact1040AshtonMiloGadget(
        const UFortItemDefinition* Definition)
    {
        return VersionInfo.FortniteVersion == 10.40 &&
            Definition &&
            Definition->Name.ToWString() ==
                L"AGID_AshtonPack_Milo";
    }

    bool FocusOrQueueExact1040Carmine(
        AFortPlayerControllerAthena* PlayerController,
        const FGuid& ItemGuid)
    {
        if (!PlayerController)
            return false;

        if (!PlayerController->MyFortPawn)
        {
            PendingCarmineFocus[PlayerController] =
                ItemGuid;
            SDK::DbgLog(
                "[Ashton1040] queued Carmine focus until "
                "pawn possession controller=%p\n",
                static_cast<void*>(PlayerController));
            return true;
        }

        PlayerController->ServerExecuteInventoryItem(
            ItemGuid);
        PlayerController->ClientEquipItem(
            ItemGuid, true);
        return true;
    }

    UFortWeaponItemDefinition*
        ResolveExact1040CarmineBacking()
    {
        if (VersionInfo.FortniteVersion != 10.40)
            return nullptr;

        auto CarmineGadget =
            const_cast<UFortGadgetItemDefinition*>(
                FindObject<UFortGadgetItemDefinition>(
                    L"/Game/Athena/Items/Gameplay/BackPacks/"
                    L"CarminePack/AGID_CarminePack."
                    L"AGID_CarminePack"));
        auto CarmineBacking =
            const_cast<UFortWeaponItemDefinition*>(
                FindObject<UFortWeaponItemDefinition>(
                    L"/Game/Athena/Items/Gameplay/BackPacks/"
                    L"CarminePack/D_CarminePack."
                    L"D_CarminePack"));
        if (!CarmineGadget ||
            !CarmineBacking)
            return nullptr;

        CarmineGadget->AddToRoot();
        CarmineBacking->AddToRoot();
        return CarmineBacking;
    }

    const UFortItemDefinition*
        ResolveExact1040AshtonGadgetAlias(
            const UFortItemDefinition* Definition)
    {
        if (!IsExact1040BaseAshtonGadget(Definition))
            return Definition;

        auto CarmineGadget =
            const_cast<UFortGadgetItemDefinition*>(
                FindObject<UFortGadgetItemDefinition>(
                    L"/Game/Athena/Items/Gameplay/BackPacks/"
                    L"CarminePack/AGID_CarminePack."
                    L"AGID_CarminePack"));
        if (!CarmineGadget)
            return Definition;

        CarmineGadget->AddToRoot();
        static bool bLoggedAlias = false;
        if (!bLoggedAlias)
        {
            bLoggedAlias = true;
            SDK::DbgLog(
                "[Ashton1040] aliased incomplete "
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
            const_cast<UFortAbilitySet*>(CarmineAbilitySet)
                ->AddToRoot();
        }
        auto AshtonAbilitySet = FindObject<UFortAbilitySet>(
            L"/Game/Athena/Items/Gameplay/BackPacks/"
            L"Ashton/AS_AshtonPack.AS_AshtonPack");
        if (AshtonAbilitySet)
        {
            const_cast<UFortAbilitySet*>(AshtonAbilitySet)
                ->AddToRoot();
        }

        static constexpr const wchar_t* ClassPaths[] = {
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_CarminePack_PassiveSetup."
                L"GA_CarminePack_PassiveSetup_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_CarminePack_DashOrSmash."
                L"GA_CarminePack_DashOrSmash_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_CarminePack_Jump_NotMoving."
                L"GA_CarminePack_Jump_NotMoving_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_CarminePack_Punch."
                L"GA_CarminePack_Punch_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_CarminePack_LifeSteal."
                L"GA_CarminePack_LifeSteal_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_AshtonPack_Carmine_GemPickup_Passive."
                L"GA_AshtonPack_Carmine_GemPickup_Passive_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_AshtonPack_GemPickupFX."
                L"GA_AshtonPack_GemPickupFX_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/DA_CarminePack."
                L"DA_CarminePack_C",
            L"/Game/Athena/VaultedGameplayCueNotifies/"
                L"Carmine/GCN_Carmine_Beam."
                L"GCN_Carmine_Beam_C",
            L"/Game/Athena/VaultedGameplayCueNotifies/"
                L"Carmine/GCL_Carmine_Beam_Loop."
                L"GCL_Carmine_Beam_Loop_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_GC_Beam_Loop."
                L"GE_Carmine_GC_Beam_Loop_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_Beam_Damage."
                L"GE_Carmine_Beam_Damage_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_Beam_Damage_P."
                L"GE_Carmine_Beam_Damage_P_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Ashton_Carmine_LockInPlace."
                L"GE_Ashton_Carmine_LockInPlace_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_AbilityBlocker."
                L"GE_Carmine_AbilityBlocker_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_DamageImmune."
                L"GE_Carmine_DamageImmune_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_GC_Aura."
                L"GE_Carmine_GC_Aura_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Carmine_GC_Skydive."
                L"GE_Carmine_GC_Skydive_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Ashton_Carmine_GemPickUpAnim."
                L"GE_Ashton_Carmine_GemPickUpAnim_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GE_Ashton_Carmine_FinalGemPickUpAnim."
                L"GE_Ashton_Carmine_FinalGemPickUpAnim_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_AshtonPack_Carmine_StonePickUpAnim."
                L"GA_AshtonPack_Carmine_StonePickUpAnim_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/GA_AshtonPack_Carmine_FinalGem."
                L"GA_AshtonPack_Carmine_FinalGem_C",
            L"/Game/Athena/VaultedGameplayCueNotifies/"
                L"Carmine/GCN_Carmine_Transform."
                L"GCN_Carmine_Transform_C",
            L"/Game/Athena/VaultedGameplayCueNotifies/"
                L"Carmine/GCL_Carmine_Skydive."
                L"GCL_Carmine_Skydive_C",
            L"/Game/Blueprints/Camera/Athena/"
                L"Athena_PlayerCameraModeCarmineSpawn."
                L"Athena_PlayerCameraModeCarmineSpawn_C",
            L"/Game/Blueprints/Camera/Athena/"
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
                L"P_Jim_LaserBlast",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/FX/P_Jim_LaserBlast_Muzzle."
                L"P_Jim_LaserBlast_Muzzle",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/FX/P_Jim_LaserBlast_Dust."
                L"P_Jim_LaserBlast_Dust",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/FX/P_Jim_LaserBlast_Impact."
                L"P_Jim_LaserBlast_Impact",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"CarminePack/FX/P_Jim_LaserBlast_Impact_Player."
                L"P_Jim_LaserBlast_Impact_Player",
            L"/Game/Animation/Game/MainPlayer/Skydive/Freefall/"
                L"Custom/Jim/Transitions/Spawn_Montage."
                L"Spawn_Montage",
            L"/Game/Animation/Game/MainPlayer/Combat/Gadgets/"
                L"ExtraLarge/Jim/Jim_FistBeam_Montage."
                L"Jim_FistBeam_Montage",
            L"/Game/Animation/Game/MainPlayer/Combat/Gadgets/"
                L"ExtraLarge/Jim/Jim_FistBeam_Outro_M."
                L"Jim_FistBeam_Outro_M",
            L"/Game/Animation/Game/MainPlayer/Combat/Gadgets/"
                L"ExtraLarge/Jim/Jim_PowerUp_Montage."
                L"Jim_PowerUp_Montage",
            L"/Game/Animation/Game/MainPlayer/Combat/Gadgets/"
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
        auto AbilitySet = FindObject<UFortAbilitySet>(
            L"/Game/Athena/Items/Gameplay/BackPacks/"
            L"Ashton/Milo/AS_AshtonPack_Milo."
            L"AS_AshtonPack_Milo");
        if (AbilitySet)
        {
            const_cast<UFortAbilitySet*>(AbilitySet)
                ->AddToRoot();
        }

        auto BoostAbilitySet = FindObject<UFortAbilitySet>(
            L"/Game/Athena/Items/Gameplay/BackPacks/"
            L"BoostJumpPack/AS_BoostJumpPack."
            L"AS_BoostJumpPack");
        if (BoostAbilitySet)
        {
            const_cast<UFortAbilitySet*>(BoostAbilitySet)
                ->AddToRoot();
        }

        static constexpr const wchar_t* ClassPaths[] = {
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"Ashton/GA_AshtonPack_EMPTYABILITY."
                L"GA_AshtonPack_EMPTYABILITY_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"Ashton/Milo/GA_AshtonPack_EquipWeapon_Milo."
                L"GA_AshtonPack_EquipWeapon_Milo_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"Ashton/Milo/GA_AshtonPack_PassiveSetup_Milo."
                L"GA_AshtonPack_PassiveSetup_Milo_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"Ashton/Milo/GA_AshtonPack_Milo_BlockAbilities."
                L"GA_AshtonPack_Milo_BlockAbilities_C",
            L"/Game/Athena/Items/Gameplay/BackPacks/"
                L"Ashton/Milo/GAT_AshtonPack_Milo_GemPickupHeal."
                L"GAT_AshtonPack_Milo_GemPickupHeal_C",
            L"/Game/Weapons/FORT_Rifles/Blueprints/Assault/"
                L"B_Assault_AshtonPack_Milo."
                L"B_Assault_AshtonPack_Milo_C",
            L"/Game/Weapons/FORT_Rifles/Blueprints/"
                L"B_Rifle_AshtonPack_Milo_Launcher."
                L"B_Rifle_AshtonPack_Milo_Launcher_C",
            L"/Game/Athena/Items/Weapons/Abilities/"
                L"GA_Ranged_Ashton_Milo_Explosive_Athena."
                L"GA_Ranged_Ashton_Milo_Explosive_Athena_C",
            L"/Game/Abilities/Weapons/Ranged/"
                L"GA_Ranged_GenericDamage."
                L"GA_Ranged_GenericDamage_C",
            L"/Game/Weapons/FORT_RocketLaunchers/Blueprints/"
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
        return Left.A == Right.A &&
            Left.B == Right.B &&
            Left.C == Right.C &&
            Left.D == Right.D;
    }

    bool IsSameRegenItem(
        const FRegeneratingInventoryItem& State,
        const AFortPlayerControllerAthena* Owner,
        const FGuid& ItemGuid)
    {
        return State.Owner.Get() == Owner &&
            AreGuidsEqual(State.ItemGuid, ItemGuid);
    }

    void RemoveRegenItemAt(size_t Index)
    {
        if (Index + 1 != RegeneratingInventoryItems.size())
        {
            RegeneratingInventoryItems[Index] =
                RegeneratingInventoryItems.back();
        }
        RegeneratingInventoryItems.pop_back();
    }

    void RemoveRechargingWeaponAt(size_t Index)
    {
        if (Index + 1 != RechargingWeaponAmmo.size())
        {
            RechargingWeaponAmmo[Index] =
                RechargingWeaponAmmo.back();
        }
        RechargingWeaponAmmo.pop_back();
    }

    bool IsNitroFistsDefinition(
        const UFortWeaponItemDefinition* WeaponDefinition)
    {
        if (!WeaponDefinition ||
            VersionInfo.FortniteVersion < 30.0 ||
            VersionInfo.FortniteVersion >= 31.0)
        {
            return false;
        }

        const auto DefinitionName =
            WeaponDefinition->Name.ToString();
        return DefinitionName.rfind(
            "WID_Moonflax_NitroGauntlet", 0) == 0;
    }

    bool NotifyNitroFistsRechargeStarted(
        AFortPlayerControllerAthena* Owner,
        const FGuid& ItemGuid,
        double ServerStartTime)
    {
        if (!Owner || !std::isfinite(ServerStartTime))
            return false;

        auto RechargeComponentClass =
            FindClass("FortControllerComponent_RechargeWeapons");
        auto RechargeComponent =
            RechargeComponentClass
                ? Owner->GetComponentByClass(
                    RechargeComponentClass)
                : nullptr;
        auto ClientStartedFunction =
            RechargeComponent
                ? RechargeComponent->GetFunction(
                    "ClientItemStartedRecharging")
                : nullptr;
        if (!ClientStartedFunction)
            return false;

        // This native client RPC seeds the same server-time countdown used
        // by WBP_NitroGautletReticle's GetRemainingCooldownTimer path. The
        // client component also keeps a pending-GUID map, so this remains
        // valid while the equipped weapon is being constructed.
        RechargeComponent->Call<void>(
            ClientStartedFunction,
            ItemGuid,
            static_cast<float>(ServerStartTime));
        return true;
    }

    AFortWeapon* ResolveEquippedWeaponForItem(
        AFortPlayerControllerAthena* Owner,
        UFortWeaponItemDefinition* WeaponDefinition,
        const FGuid& ItemGuid)
    {
        if (!Owner || !WeaponDefinition ||
            !Owner->HasMyFortPawn() || !Owner->MyFortPawn ||
            !Owner->MyFortPawn->HasCurrentWeapon())
        {
            return nullptr;
        }

        auto WeaponActor = Owner->MyFortPawn->CurrentWeapon;
        auto FortWeaponClass = AFortWeapon::StaticClass();
        if (!WeaponActor || !FortWeaponClass ||
            !WeaponActor->IsA(FortWeaponClass))
        {
            return nullptr;
        }

        auto Weapon = static_cast<AFortWeapon*>(WeaponActor);
        if (!Weapon->HasItemEntryGuid() ||
            !AreGuidsEqual(Weapon->ItemEntryGuid, ItemGuid) ||
            (Weapon->HasWeaponData() &&
                Weapon->WeaponData != WeaponDefinition) ||
            !Weapon->HasAmmoCount())
        {
            return nullptr;
        }

        return Weapon;
    }

    bool SyncWeaponAmmo(
        AFortWeapon* Weapon,
        int32 NewLoadedAmmo)
    {
        if (!Weapon || !Weapon->HasAmmoCount())
            return false;

        const int32 OldAmmoCount = Weapon->AmmoCount;
        if (OldAmmoCount != NewLoadedAmmo)
        {
            // The equipped actor owns the authoritative/live magazine.
            // Keep it in step with the inventory representation and drive
            // the normal local rep-notify path for server-side listeners.
            Weapon->AmmoCount = NewLoadedAmmo;
            if (auto OnRepAmmoCount =
                Weapon->GetFunction("OnRep_AmmoCount"))
            {
                Weapon->Call<void>(
                    OnRepAmmoCount, OldAmmoCount);
            }
        }

        // A locally predicted shot can lower the owning client's ammo while
        // the server value remains unchanged under Infinite Ammo. Poll-model
        // replication normally sees no delta in that case, and push-model
        // builds require an explicit dirty mark. The item-change broadcast
        // plus this mark sends the correction now instead of waiting for the
        // weapon to be reconstructed on the next equip.
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(
            Weapon, L"AmmoCount");
        Weapon->ForceNetUpdate();
        return true;
    }

    bool SyncEquippedWeaponAmmo(
        AFortPlayerControllerAthena* Owner,
        UFortWeaponItemDefinition* WeaponDefinition,
        const FGuid& ItemGuid,
        int32 NewLoadedAmmo)
    {
        return SyncWeaponAmmo(
            ResolveEquippedWeaponForItem(
                Owner, WeaponDefinition, ItemGuid),
            NewLoadedAmmo);
    }

    bool ResolveNitroFistsRechargeSettings(
        UFortWeaponItemDefinition* WeaponDefinition,
        int32 ItemLevel,
        int32& MaxLoadedAmmo,
        int32& RechargeAmount,
        double& RechargeIntervalSeconds)
    {
        if (!IsNitroFistsDefinition(WeaponDefinition))
            return false;

        const auto DefinitionName =
            WeaponDefinition->Name.ToString();
        // Do not reinterpret a gauntlet stat row as ranged-weapon stats.
        // FN30 defines four charges on the regular fists and five on the
        // mythic variant.
        MaxLoadedAmmo =
            DefinitionName.find("_Mythic") !=
                std::string::npos
            ? 5
            : 4;

        float RechargeQuantityValue = 0.0f;
        if (WeaponDefinition->HasWeaponRechargeAmmoQuantity())
        {
            RechargeQuantityValue =
                WeaponDefinition->WeaponRechargeAmmoQuantity.Evaluate(
                    static_cast<float>(max(ItemLevel, 1)));
        }
        RechargeAmount =
            static_cast<int32>(std::round(RechargeQuantityValue));

        float RechargeRateValue = 0.0f;
        if (WeaponDefinition->HasWeaponRechargeAmmoRate())
        {
            RechargeRateValue =
                WeaponDefinition->WeaponRechargeAmmoRate.Evaluate(
                    static_cast<float>(max(ItemLevel, 1)));
        }
        RechargeIntervalSeconds =
            static_cast<double>(RechargeRateValue);

        // These are the authoritative FN30 curve-table defaults:
        // Moonflax.NitroGauntlets.RechargeQuantity = 1 and both
        // RechargeRate rows = 8. The fallback is limited to the exact
        // Nitro Fists definitions in case a server-only asset load cannot
        // evaluate the scalable-float curve.
        if (RechargeAmount <= 0 || RechargeAmount > MaxLoadedAmmo)
            RechargeAmount = 1;
        if (!std::isfinite(RechargeIntervalSeconds) ||
            RechargeIntervalSeconds <= 0.0 ||
            RechargeIntervalSeconds > 300.0)
        {
            RechargeIntervalSeconds = 8.0;
        }

        return MaxLoadedAmmo > 0 &&
            RechargeAmount > 0;
    }

    void ObserveRechargingWeaponAmmo(
        AFortPlayerControllerAthena* Owner,
        UFortWeaponItemDefinition* WeaponDefinition,
        const FGuid& ItemGuid,
        int32 ItemLevel,
        int32 PreviousLoadedAmmo,
        int32 NewLoadedAmmo)
    {
        if (!Owner || !Owner->WorldInventory ||
            !WeaponDefinition)
        {
            return;
        }

        int32 MaxLoadedAmmo = 0;
        int32 RechargeAmount = 0;
        double RechargeIntervalSeconds = 0.0;
        if (!ResolveNitroFistsRechargeSettings(
                WeaponDefinition,
                ItemLevel,
                MaxLoadedAmmo,
                RechargeAmount,
                RechargeIntervalSeconds))
        {
            return;
        }

        auto World = UWorld::GetWorld();
        if (!World)
            return;

        const double NowSeconds =
            UGameplayStatics::GetTimeSeconds(World);
        for (size_t Index = 0;
            Index < RechargingWeaponAmmo.size();
            ++Index)
        {
            auto& State = RechargingWeaponAmmo[Index];
            if (State.Owner.Get() != Owner ||
                !AreGuidsEqual(State.ItemGuid, ItemGuid))
            {
                continue;
            }

            State.WeaponDefinition =
                TWeakObjectPtr<UFortWeaponItemDefinition>(
                    WeaponDefinition);
            State.MaxLoadedAmmo = MaxLoadedAmmo;
            State.RechargeAmount = RechargeAmount;
            State.RechargeIntervalSeconds =
                RechargeIntervalSeconds;
            State.LastObservedLoadedAmmo =
                std::clamp(
                    NewLoadedAmmo, 0, MaxLoadedAmmo);
            bool bStartedRechargeCycle = false;

            // A native recharge is an increase. Give its controller
            // component a complete interval before the watchdog can add
            // another charge. Further consumption does not reset an
            // already-running interval.
            if (NewLoadedAmmo >= MaxLoadedAmmo)
            {
                State.NextRefillTime = 0.0;
            }
            else if (NewLoadedAmmo > PreviousLoadedAmmo)
            {
                State.NextRefillTime =
                    NowSeconds +
                    RechargeIntervalSeconds +
                    NativeRechargeGraceSeconds;
                bStartedRechargeCycle = true;
            }
            else if (NewLoadedAmmo < PreviousLoadedAmmo &&
                State.NextRefillTime <= 0.0)
            {
                State.NextRefillTime =
                    NowSeconds +
                    RechargeIntervalSeconds +
                    NativeRechargeGraceSeconds;
                bStartedRechargeCycle = true;
            }
            if (bStartedRechargeCycle)
            {
                NotifyNitroFistsRechargeStarted(
                    Owner, ItemGuid, NowSeconds);
            }
            return;
        }

        if (RechargingWeaponAmmo.size() >=
                MaxTrackedRechargingWeapons)
        {
            return;
        }

        FRechargingWeaponAmmo State{};
        State.Owner =
            TWeakObjectPtr<AFortPlayerControllerAthena>(Owner);
        State.WeaponDefinition =
            TWeakObjectPtr<UFortWeaponItemDefinition>(
                WeaponDefinition);
        State.ItemGuid = ItemGuid;
        State.MaxLoadedAmmo = MaxLoadedAmmo;
        State.RechargeAmount = RechargeAmount;
        State.LastObservedLoadedAmmo =
            std::clamp(
                NewLoadedAmmo, 0, MaxLoadedAmmo);
        State.RechargeIntervalSeconds =
            RechargeIntervalSeconds;
        if (NewLoadedAmmo < MaxLoadedAmmo)
        {
            State.NextRefillTime =
                NowSeconds +
                RechargeIntervalSeconds +
                NativeRechargeGraceSeconds;
        }
        RechargingWeaponAmmo.push_back(State);

        const bool bClientTimerStarted =
            NewLoadedAmmo < MaxLoadedAmmo &&
            NotifyNitroFistsRechargeStarted(
                Owner, ItemGuid, NowSeconds);

        auto RechargeComponentClass =
            FindClass("FortControllerComponent_RechargeWeapons");
        auto RechargeComponent =
            RechargeComponentClass
                ? Owner->GetComponentByClass(
                    RechargeComponentClass)
                : nullptr;
        SDK::DbgLog(
            "[WeaponRecharge] registered definition=%s ammo=%d/%d "
            "amount=%d interval=%.2f nativeComponent=%s\n",
            WeaponDefinition->Name.ToString().c_str(),
            NewLoadedAmmo,
            MaxLoadedAmmo,
            RechargeAmount,
            RechargeIntervalSeconds,
            RechargeComponent ? "present" : "missing");
        if (NewLoadedAmmo < MaxLoadedAmmo)
        {
            SDK::DbgLog(
                "[WeaponRecharge] client-timer definition=%s "
                "started=%d serverStart=%.3f\n",
                WeaponDefinition->Name.ToString().c_str(),
                bClientTimerStarted ? 1 : 0,
                NowSeconds);
        }
    }

    void BroadcastWorldItemAmmoChanged(UFortWorldItem* Item)
    {
        if (!Item)
            return;

        static auto BroadcastFunction =
            Item->GetFunction("BroadcastOnItemChanged");
        if (BroadcastFunction)
        {
            Item->Call<void>(
                BroadcastFunction,
                false,
                true,
                false,
                false);
        }
    }

    void ScheduleRegeneratingInventoryItem(
        AFortPlayerControllerAthena* Owner,
        UFortAmmoItemDefinition* AmmoDefinition,
        const FGuid& ItemGuid,
        int32 MaxCount,
        double CooldownSeconds)
    {
        if (!Owner || !Owner->WorldInventory || !AmmoDefinition ||
            MaxCount <= 0 || !std::isfinite(CooldownSeconds) ||
            CooldownSeconds <= 0.0 || CooldownSeconds > 3600.0)
        {
            return;
        }

        auto ReplicatedEntry =
            Owner->WorldInventory->Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Candidate)
                {
                    return AreGuidsEqual(Candidate.ItemGuid, ItemGuid) &&
                        Candidate.ItemDefinition == AmmoDefinition;
                },
                FFortItemEntry::Size());
        if (!ReplicatedEntry || ReplicatedEntry->Count >= MaxCount)
            return;

        for (auto& State : RegeneratingInventoryItems)
        {
            if (!IsSameRegenItem(State, Owner, ItemGuid))
                continue;

            State.AmmoDefinition =
                TWeakObjectPtr<UFortAmmoItemDefinition>(AmmoDefinition);
            State.MaxCount = max(State.MaxCount, MaxCount);
            State.CooldownSeconds = CooldownSeconds;
            return;
        }

        auto World = UWorld::GetWorld();
        if (!World)
            return;

        FRegeneratingInventoryItem State{};
        State.Owner =
            TWeakObjectPtr<AFortPlayerControllerAthena>(Owner);
        State.AmmoDefinition =
            TWeakObjectPtr<UFortAmmoItemDefinition>(AmmoDefinition);
        State.ItemGuid = ItemGuid;
        State.MaxCount = MaxCount;
        State.CooldownSeconds = CooldownSeconds;
        State.NextRefillTime =
            UGameplayStatics::GetTimeSeconds(World) + CooldownSeconds;
        RegeneratingInventoryItems.push_back(State);

        SDK::DbgLog(
            "[ItemRegen] scheduled definition=%s count=%d max=%d cooldown=%.2f\n",
            AmmoDefinition->Name.ToString().c_str(),
            ReplicatedEntry->Count,
            MaxCount,
            CooldownSeconds);
    }
}

bool UFortWorldItemDefinition::ServerExecute(UFortItem* Item, AFortPlayerControllerAthena* Instigator) const
{
    if (!this || !Item || !Instigator)
        return false;

    const int32 ServerExecuteVft = FindWorldItemDefinitionServerExecuteVft();
    if (ServerExecuteVft < 0 || ServerExecuteVft >= 1024 || !Vft[ServerExecuteVft])
        return false;

    return ((bool(*)(const UFortWorldItemDefinition*, UFortItem*, AFortPlayerControllerAthena*))Vft[ServerExecuteVft])(
        this, Item, Instigator);
}

UFortWorldItem* AFortInventory::GiveItem(const UFortItemDefinition* Def, int Count, int LoadedAmmo, int Level, bool ShowPickupNoti, bool updateInventory, int PhantomReserveAmmo, TArray<FFortItemEntryStateValue> StateValues, bool bNotifyItemInstanceAdded, TArray<float> GenericAttributeValues, bool* OutForceFocusHandled, bool* OutGadgetInitializationDispatched)
{
    if (OutForceFocusHandled)
        *OutForceFocusHandled = false;
    if (OutGadgetInitializationDispatched)
        *OutGadgetInitializationDispatched = false;
    if (!this || !Def || !Count)
        return nullptr;

    // The cooked 10.40 Ashton gadget is a legacy partial definition: it has
    // only the head part, the ordinary player AnimBP, an incomplete ability
    // set and a missing D_AshtonPack package. The playlist-authored Carmine
    // gadget is the complete Endgame Thanos carrier. Substitute before the
    // definition ever reaches a replicated inventory entry so clients receive
    // the full body, Gauntlet AnimBP, stone abilities and beam cue lifecycle.
    Def = ResolveExact1040AshtonGadgetAlias(Def);

    auto PlayerController = Owner ? Owner->Cast<AFortPlayerControllerAthena>() : nullptr;
    auto Gadget = Def->Cast<UFortGadgetItemDefinition>();
    const bool bCarmineGadget =
        IsExact1040CarmineGadget(Def);
    const bool bAshtonMiloGadget =
        IsExact1040AshtonMiloGadget(Def);
    if (bCarmineGadget)
        PreloadExact1040CarmineDependencies();
    if (bAshtonMiloGadget)
        PreloadExact1040AshtonMiloDependencies();

    // Exclusive Chapter 1 gadgets expect the backpack to be emptied before
    // their native ServerExecute runs. The Infinity Gauntlet sets this flag.
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
                ? ExistingDefinition->bCanBeDropped
                : (ExistingDefinition->GetPickupComponent()
                    ? ExistingDefinition->GetPickupComponent()->bCanBeDroppedFromInventory
                    : false);
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
        FFortWeaponMods::CopyDefinitionSlotsToEntry(
            WeaponDef, Item->ItemEntry);
    const bool bHasWeaponModSlots =
        FFortWeaponMods::HasEntrySlots(Item->ItemEntry);
    if (Item->ItemEntry.HasStateValues() && StateValues.Num() > 0)
    {
        auto NewData = FMemory::Malloc(FFortItemEntryStateValue::Size() * StateValues.Num());
        memcpy(NewData, StateValues.Data, FFortItemEntryStateValue::Size() * StateValues.Num());
        Item->ItemEntry.StateValues.NumElements = StateValues.Num();
        Item->ItemEntry.StateValues.MaxElements = StateValues.Num();
        Item->ItemEntry.StateValues.Data = (FFortItemEntryStateValue*)NewData;
    }
    if (Item->ItemEntry.HasGenericAttributeValues() &&
        GenericAttributeValues.Num() > 0)
    {
        auto NewData = FMemory::Malloc(
            sizeof(float) * GenericAttributeValues.Num());
        memcpy(
            NewData,
            GenericAttributeValues.Data,
            sizeof(float) * GenericAttributeValues.Num());
        Item->ItemEntry.GenericAttributeValues.NumElements =
            GenericAttributeValues.Num();
        Item->ItemEntry.GenericAttributeValues.MaxElements =
            GenericAttributeValues.Num();
        Item->ItemEntry.GenericAttributeValues.Data =
            static_cast<float*>(NewData);
    }

    if (Item->ItemEntry.ItemGuid.A == 0 && Item->ItemEntry.ItemGuid.B == 0 && Item->ItemEntry.ItemGuid.C == 0 && Item->ItemEntry.ItemGuid.D == 0)
    {
        CoCreateGuid((GUID*)&Item->ItemEntry.ItemGuid);

        if (FFortItemEntry::HasTrackerGuid() && Item->ItemEntry.TrackerGuid.A == 0 && Item->ItemEntry.TrackerGuid.B == 0 && Item->ItemEntry.TrackerGuid.C == 0 && Item->ItemEntry.TrackerGuid.D == 0)
            CoCreateGuid((GUID*)&Item->ItemEntry.TrackerGuid);
    }


    auto& AddedReplicatedEntry =
        this->Inventory.ReplicatedEntries.Add(
            Item->ItemEntry, FFortItemEntry::Size());
    auto* ReplicatedEntry = &AddedReplicatedEntry;
    ReplicatedEntry->bIsReplicatedCopy = true;
    if (bHasWeaponModSlots)
    {
        FFortWeaponMods::CopyEntrySlots(
            Item->ItemEntry, *ReplicatedEntry);
    }
    this->Inventory.ItemInstances.Add(Item);

    /*if (Item->ItemEntry.ItemDefinition->bForceFocusWhenAdded)
    {
        ((AFortPlayerControllerAthena*)Owner)->ServerExecuteInventoryItem(Item->ItemEntry.ItemGuid);
        ((AFortPlayerControllerAthena*)Owner)->ClientEquipItem(Item->ItemEntry.ItemGuid, true);
    }*/

    if (VersionInfo.FortniteVersion <= 3.60)
    {
        auto PlayerController = (AFortPlayerControllerAthena*)Owner;
        auto PlayerState = PlayerController ? (AFortPlayerStateAthena*)PlayerController->PlayerState : nullptr;
        bool bIsBotInventory = PlayerState && PlayerState->HasbIsABot() && PlayerState->bIsABot;

        if (!bIsBotInventory && PlayerController && PlayerController->QuickBars && (IsPrimaryQuickbar(Def) || Def->ItemType == EFortItemType::GetBuildingPiece() || Def->ItemType == EFortItemType::GetTrap() || Def->ItemType == EFortItemType::GetWeaponHarvest()))
        {
            PlayerController->QuickBars->ServerAddItemInternal(Item->ItemEntry.ItemGuid, !(IsPrimaryQuickbar(Def) || Def->ItemType == EFortItemType::GetWeaponHarvest()), -3);
        }
    }

    if (updateInventory)
    {
        //Update(&Item->ItemEntry);

        bRequiresLocalUpdate = true;
        bRequiresSaving = true;

        HandleInventoryLocalUpdate(); // calls UpdateItemInstances, the func we actually want

        // Native inventory work can move the fast-array allocation. Never
        // retain the Add() reference across HandleInventoryLocalUpdate.
        ReplicatedEntry = Inventory.ReplicatedEntries.Search(
            [&](FFortItemEntry& Candidate)
            {
                return Candidate.ItemGuid ==
                    Item->ItemEntry.ItemGuid;
            },
            FFortItemEntry::Size());
        if (ReplicatedEntry)
        {
            ReplicatedEntry->bIsDirty = false;
            Inventory.MarkItemDirty(*ReplicatedEntry);
        }
        ForceNetUpdate();
        Item->ItemEntry.bIsDirty = true;
    }

    // HandleInventoryLocalUpdate may copy the replicated array header back to
    // the item instance. Detach it so bench changes cannot mutate both entries.
    if (bHasWeaponModSlots && ReplicatedEntry)
    {
        FFortWeaponMods::CopyEntrySlots(
            *ReplicatedEntry, Item->ItemEntry);
    }

    bool bCarmineInitialized = false;
    if (bCarmineGadget)
    {
        // Install Carmine's server ability set immediately. The replicated
        // item still follows the owning client's ordinary item-added path,
        // where WaitAnimBPOverrideReady starts Spawn_Montage and the authored
        // lift/skydive timers once the full Gauntlet AnimBP is actually ready.
        bCarmineInitialized =
            bNotifyItemInstanceAdded
                ? InitializeGadgetItemWithFallback(
                      Item, true)
                : false;
    }
    bool bAshtonMiloInitialized = false;
    if (bAshtonMiloGadget)
    {
        // The Chitauri backpack owns the three pieces of visible equipment.
        // Prefer the validated gadget path after preloading its soft ability
        // set, then fall through to the ordinary virtual notification if that
        // private path is unavailable for the executable in use.
        bAshtonMiloInitialized =
            bNotifyItemInstanceAdded
                ? InitializeGadgetItemWithFallback(
                      Item, true)
                : false;
    }
    bool bItemAddedFallbackDispatched = false;
    bool bCarmineFallbackDispatched = false;
    const IInterface* InventoryOwnerInterface =
        Owner
            ? Owner->GetInterface(
                  IFortInventoryOwnerInterface::
                      StaticClass())
            : nullptr;
    if ((!bCarmineGadget || !bCarmineInitialized) &&
        (!bAshtonMiloGadget ||
         !bAshtonMiloInitialized) &&
        bNotifyItemInstanceAdded &&
        OnItemInstanceAddedVft &&
        OnItemInstanceAddedVft < 1024 &&
        Item->Vft &&
        Item->Vft[OnItemInstanceAddedVft] &&
        InventoryOwnerInterface)
    {
        ((void(*)(const UFortWorldItem*, const IInterface*))
            Item->Vft[OnItemInstanceAddedVft])(
                Item, InventoryOwnerInterface);
        bItemAddedFallbackDispatched = true;
        bCarmineFallbackDispatched =
            bCarmineGadget;
    }

    // The S4 gauntlet is force-focused when collected. Route that focus through
    // ServerExecuteInventoryItem so the native gadget execution path above is
    // used instead of merely equipping its backing weapon definition.
    bool bCarmineFocusHandled = false;
    if (bCarmineGadget &&
        (bCarmineInitialized ||
         bCarmineFallbackDispatched) &&
        PlayerController &&
        Def->HasbForceFocusWhenAdded() &&
        Def->bForceFocusWhenAdded)
    {
        bCarmineFocusHandled =
            IsExact1040BaseAshtonGadget(Def)
                ? EnsureExact1040AshtonBackingAndFocus()
                : FocusOrQueueExact1040Carmine(
                      PlayerController,
                      Item->ItemEntry.ItemGuid);
    }
    if (VersionInfo.FortniteVersion >= 4.0 &&
        VersionInfo.FortniteVersion <= 4.5 &&
        PlayerController && Gadget &&
        Def->HasbForceFocusWhenAdded() &&
        Def->bForceFocusWhenAdded)
    {
        PlayerController->ServerExecuteInventoryItem(
            Item->ItemEntry.ItemGuid);
        PlayerController->ClientEquipItem(
            Item->ItemEntry.ItemGuid, true);
    }
    if (OutForceFocusHandled && bCarmineGadget)
        *OutForceFocusHandled =
            bCarmineFocusHandled;
    if (OutGadgetInitializationDispatched)
    {
        *OutGadgetInitializationDispatched =
            bCarmineGadget
                ? bCarmineInitialized ||
                      bItemAddedFallbackDispatched
                : bAshtonMiloGadget
                    ? bAshtonMiloInitialized ||
                          bItemAddedFallbackDispatched
                    : bItemAddedFallbackDispatched;
    }

    auto RechargingWeaponDefinition =
        Item->ItemEntry.ItemDefinition
            ? Item->ItemEntry.ItemDefinition->Cast<
                UFortWeaponItemDefinition>()
            : nullptr;
    if (PlayerController &&
        IsNitroFistsDefinition(RechargingWeaponDefinition))
    {
        // Register at grant time as well as at the loaded-ammo setter.
        // This keeps the repair event-bound and still detects FN30 paths
        // that mutate the replicated entry without calling that setter.
        ObserveRechargingWeaponAmmo(
            PlayerController,
            RechargingWeaponDefinition,
            Item->ItemEntry.ItemGuid,
            Item->ItemEntry.Level,
            Item->ItemEntry.LoadedAmmo,
            Item->ItemEntry.LoadedAmmo);
    }

    return Item;
}

bool AFortInventory::InitializeGadgetItem(
    UFortWorldItem* Item,
    bool updateInventory)
{
    if (!this || !Item || !Item->ItemEntry.ItemDefinition)
        return false;

    auto Gadget =
        Item->ItemEntry.ItemDefinition
            ->Cast<UFortGadgetItemDefinition>();
    const IInterface* InventoryOwnerInterface =
        Owner
            ? Owner->GetInterface(
                  IFortInventoryOwnerInterface::StaticClass())
            : nullptr;
    if (!Gadget || !InventoryOwnerInterface)
        return false;

    const bool bCarmineGadget =
        IsExact1040CarmineGadget(Gadget);
    const bool bAshtonMiloGadget =
        IsExact1040AshtonMiloGadget(Gadget);
    if (bCarmineGadget)
        PreloadExact1040CarmineDependencies();
    if (bAshtonMiloGadget)
        PreloadExact1040AshtonMiloDependencies();

    // ApplyGadgetData uses Get() on these authored soft references. The
    // stripped 10.40 server never queues their normal preload, leaving both
    // the ability-set handle and the tracked attribute set uninitialized.
    // Resolve them once on the game thread before invoking the native path.
    const UFortAbilitySet* LoadedAbilitySet = nullptr;
    if (Gadget->HasAbilitySet())
        LoadedAbilitySet = Gadget->AbilitySet.Get();

    const bool bBigTeamGlider =
        VersionInfo.FortniteVersion == 10.40 &&
        Gadget->Name.ToWString() ==
            L"Athena_Glider_Item_BigTeamMode";
    if (!LoadedAbilitySet && bBigTeamGlider)
    {
        LoadedAbilitySet = FindObject<UFortAbilitySet>(
            L"/Game/Athena/Items/Gameplay/BackPacks/"
            L"GliderItem/AS_Athena_Glider_Item."
            L"AS_Athena_Glider_Item");
    }
    if (!LoadedAbilitySet &&
        IsExact1040BaseAshtonGadget(Gadget))
    {
        LoadedAbilitySet = FindObject<UFortAbilitySet>(
            L"/Game/Athena/Items/Gameplay/BackPacks/"
            L"Ashton/AS_AshtonPack.AS_AshtonPack");
    }
    if (!LoadedAbilitySet &&
        bCarmineGadget &&
        !IsExact1040BaseAshtonGadget(Gadget))
    {
        LoadedAbilitySet = FindObject<UFortAbilitySet>(
            L"/Game/Athena/Items/Gameplay/BackPacks/"
            L"CarminePack/AS_CarminePack.AS_CarminePack");
    }
    if (!LoadedAbilitySet && bAshtonMiloGadget)
    {
        LoadedAbilitySet = FindObject<UFortAbilitySet>(
            L"/Game/Athena/Items/Gameplay/BackPacks/"
            L"Ashton/Milo/AS_AshtonPack_Milo."
            L"AS_AshtonPack_Milo");
    }
    if (LoadedAbilitySet)
        LoadedAbilitySet->AddToRoot();

    UClass* LoadedAttributeSet =
        Gadget->HasAttributeSet()
            ? Gadget->AttributeSet.Get()
            : nullptr;
    UClass* LoadedGameplayAbility =
        Gadget->HasGameplayAbility()
            ? Gadget->GameplayAbility.Get()
            : nullptr;
    auto LoadedWeaponDefinition =
        Gadget->GetWeaponItemDefinition();

    bool bApplied = false;
    if (ApplyGadgetDataAddress)
    {
        bApplied =
            ((bool (*)(
                UFortGadgetItemDefinition*,
                const IInterface*,
                UFortItem*,
                uint8_t))ApplyGadgetDataAddress)(
                    Gadget,
                    InventoryOwnerInterface,
                    reinterpret_cast<UFortItem*>(Item),
                    1);
    }

    if (updateInventory && bApplied)
        Update(&Item->ItemEntry);

    SDK::DbgLog(
        "[Gadget] native initialization definition=%s item=%p "
        "abilitySet=%p attributeSet=%p gameplayAbility=%p "
        "weapon=%p applied=%d\n",
        Gadget->Name.ToString().c_str(),
        static_cast<void*>(Item),
        static_cast<const void*>(LoadedAbilitySet),
        static_cast<void*>(LoadedAttributeSet),
        static_cast<void*>(LoadedGameplayAbility),
        static_cast<void*>(LoadedWeaponDefinition),
        bApplied ? 1 : 0);
    return bApplied;
}

bool AFortInventory::InitializeGadgetItemWithFallback(
    UFortWorldItem* Item,
    bool updateInventory)
{
    if (!this || !Item ||
        !Item->ItemEntry.ItemDefinition ||
        !Item->ItemEntry.ItemDefinition
             ->Cast<UFortGadgetItemDefinition>())
    {
        return false;
    }

    const bool bForceStockMiloNotification =
        IsExact1040AshtonMiloGadget(
            Item->ItemEntry.ItemDefinition);
    const bool bForceStockNotification =
        bForceStockMiloNotification;

    // Milo's cooked passive creates its rifle, launcher and jetpack from its
    // server item-added event, so it needs that stock notification. Carmine
    // instead uses ApplyGadgetData here for immediate server abilities; its
    // replicated owning-client item-added event independently owns the
    // AnimBP-ready transformation.
    if (!bForceStockNotification &&
        InitializeGadgetItem(Item, updateInventory))
        return true;

    const IInterface* InventoryOwnerInterface =
        Owner
            ? Owner->GetInterface(
                  IFortInventoryOwnerInterface::StaticClass())
            : nullptr;
    if (!InventoryOwnerInterface ||
        !OnItemInstanceAddedVft ||
        OnItemInstanceAddedVft >= 1024 ||
        !Item->Vft ||
        !Item->Vft[OnItemInstanceAddedVft])
    {
        return false;
    }

    ((void(*)(const UFortWorldItem*, const IInterface*))
        Item->Vft[OnItemInstanceAddedVft])(
            Item, InventoryOwnerInterface);
    SDK::DbgLog(
        "[Gadget] dispatched item-added %s "
        "definition=%s item=%p\n",
        bForceStockNotification
            ? "forced-stock path"
            : "fallback",
        Item->ItemEntry.ItemDefinition->Name
            .ToString().c_str(),
        static_cast<void*>(Item));
    return true;
}

bool AFortInventory::
    EnsureExact1040AshtonBackingAndFocus(
        FGuid* OutBackingGuid)
{
    if (OutBackingGuid)
        *OutBackingGuid = {};
    auto PlayerController =
        Owner
            ? Owner->Cast<
                  AFortPlayerControllerAthena>()
            : nullptr;
    auto BackingDefinition =
        ResolveExact1040CarmineBacking();
    if (!PlayerController ||
        !BackingDefinition)
    {
        return false;
    }

    auto Existing =
        Inventory.ReplicatedEntries.Search(
            [&](FFortItemEntry& Entry)
            {
                return Entry.ItemDefinition ==
                    BackingDefinition;
            },
            FFortItemEntry::Size());
    FGuid BackingGuid{};
    if (Existing)
    {
        BackingGuid = Existing->ItemGuid;
    }
    else
    {
        auto BackingItem = GiveItem(
            BackingDefinition,
            1,
            0,
            0,
            false,
            true,
            0,
            {},
            true);
        if (!BackingItem)
            return false;
        BackingGuid =
            BackingItem->ItemEntry.ItemGuid;
        SDK::DbgLog(
            "[Ashton1040] granted explicit "
            "D_CarminePack backing for authored gauntlet "
            "controller=%p\n",
            static_cast<void*>(PlayerController));
    }

    if (OutBackingGuid)
        *OutBackingGuid = BackingGuid;
    return FocusOrQueueExact1040Carmine(
        PlayerController, BackingGuid);
}

int32 AFortInventory::GiveItemToSingleStack(
    const UFortItemDefinition* Definition,
    int32 Count,
    bool ShowPickupNoti)
{
    if (!this || !Definition || Count <= 0)
        return 0;

    int32 MaxStackSize = Definition->GetMaxStackSize();
    if (MaxStackSize <= 0)
    {
        // Resource definitions expose a finite cap on supported builds. If a
        // fork omits that reflection, still merge safely instead of reverting
        // to GiveItem's one-new-GUID-per-grant behavior.
        MaxStackSize = (std::numeric_limits<int32>::max)();
    }

    auto ExistingEntry = Inventory.ReplicatedEntries.Search(
        [&](FFortItemEntry& Entry)
        {
            return Entry.ItemDefinition == Definition;
        },
        FFortItemEntry::Size());

    if (!ExistingEntry)
    {
        const int32 GrantedCount =
            (std::min)(Count, MaxStackSize);
        return GiveItem(
            Definition,
            GrantedCount,
            0,
            0,
            ShowPickupNoti)
            ? GrantedCount
            : 0;
    }

    const int32 ExistingCount =
        (std::max)(ExistingEntry->Count, 0);
    if (ExistingCount >= MaxStackSize)
        return 0;

    const int64 DesiredCount =
        static_cast<int64>(ExistingCount) +
        static_cast<int64>(Count);
    int32 NewCount = static_cast<int32>(
        (std::min)(
            DesiredCount,
            static_cast<int64>(MaxStackSize)));
    const int32 GrantedCount = NewCount - ExistingCount;
    if (GrantedCount <= 0)
        return 0;

    FGuid ExistingGuid = ExistingEntry->ItemGuid;
    ExistingEntry->Count = NewCount;

    auto ExistingItem = Inventory.ItemInstances.Search(
        [&](UFortWorldItem* Item)
        {
            return Item &&
                AreGuidsEqual(
                    Item->ItemEntry.ItemGuid,
                    ExistingGuid);
        });
    if (ExistingItem && *ExistingItem)
    {
        (*ExistingItem)->ItemEntry.Count = NewCount;
        (*ExistingItem)->ItemEntry.bIsDirty = true;
    }

    UpdateEntry(*ExistingEntry);
    return GrantedCount;
}

UFortWorldItem* AFortInventory::GiveItem(FFortItemEntry& entry, int Count, bool ShowPickupNoti, bool updateInventory, bool* OutForceFocusHandled)
{
    if (OutForceFocusHandled)
        *OutForceFocusHandled = false;
    if (Count == -1)
        Count = entry.Count;

    // Preserve the original inventory/callback/force-focus ordering outside
    // Chapter 5. Deferred notification is needed only while copying the
    // attachment slots that native item listeners consume.
    if (!FFortWeaponMods::HasEntrySlots(entry))
    {
        return GiveItem(
            entry.ItemDefinition,
            Count,
            entry.LoadedAmmo,
            entry.Level,
            ShowPickupNoti,
            updateInventory,
            entry.HasPhantomReserveAmmo()
                ? entry.PhantomReserveAmmo
                : 0,
            entry.HasStateValues()
                ? entry.StateValues
                : TArray<FFortItemEntryStateValue>{},
            true,
            entry.HasGenericAttributeValues()
                ? entry.GenericAttributeValues
                : TArray<float>{},
            OutForceFocusHandled);
    }

    bool bDefinitionFocusHandled = false;
    auto Item = GiveItem(
        entry.ItemDefinition,
        Count,
        entry.LoadedAmmo,
        entry.Level,
        ShowPickupNoti,
        false,
        entry.HasPhantomReserveAmmo() ? entry.PhantomReserveAmmo : 0,
        entry.HasStateValues()
            ? entry.StateValues
            : TArray<FFortItemEntryStateValue>{},
        false,
        entry.HasGenericAttributeValues()
            ? entry.GenericAttributeValues
            : TArray<float>{},
        &bDefinitionFocusHandled);
    if (!Item)
        return nullptr;

    auto ReplicatedEntry = Inventory.ReplicatedEntries.Search(
        [&](FFortItemEntry& Candidate)
        {
            return Candidate.ItemGuid == Item->ItemEntry.ItemGuid;
        },
        FFortItemEntry::Size());

    FFortWeaponMods::CopyEntrySlots(entry, Item->ItemEntry);
    if (ReplicatedEntry)
        FFortWeaponMods::CopyEntrySlots(entry, *ReplicatedEntry);

    if (updateInventory)
    {
        bRequiresLocalUpdate = true;
        bRequiresSaving = true;
        HandleInventoryLocalUpdate();

        ReplicatedEntry = Inventory.ReplicatedEntries.Search(
            [&](FFortItemEntry& Candidate)
            {
                return Candidate.ItemGuid == Item->ItemEntry.ItemGuid;
            },
            FFortItemEntry::Size());
        if (ReplicatedEntry)
        {
            ReplicatedEntry->bIsDirty = false;
            Inventory.MarkItemDirty(*ReplicatedEntry);
            FFortWeaponMods::CopyEntrySlots(
                *ReplicatedEntry, Item->ItemEntry);
        }

        ForceNetUpdate();
        Item->ItemEntry.bIsDirty = true;
    }

    // Pickup/custom entries must expose their source attachment set before
    // inventory listeners initialize the item's abilities and equipped state.
    // The definition overload deliberately deferred this one notification.
    const IInterface* InventoryOwnerInterface =
        Owner
            ? Owner->GetInterface(
                  IFortInventoryOwnerInterface::
                      StaticClass())
            : nullptr;
    if (OnItemInstanceAddedVft &&
        OnItemInstanceAddedVft < 1024 &&
        Item->Vft &&
        Item->Vft[OnItemInstanceAddedVft] &&
        InventoryOwnerInterface &&
        !bDefinitionFocusHandled)
    {
        ((void(*)(const UFortWorldItem*, const IInterface*))
            Item->Vft[OnItemInstanceAddedVft])(
                Item, InventoryOwnerInterface);
        if (IsExact1040CarmineGadget(
                Item->ItemEntry.ItemDefinition))
        {
            auto PlayerController =
                Owner->Cast<
                    AFortPlayerControllerAthena>();
            auto Definition =
                Item->ItemEntry.ItemDefinition;
            if (PlayerController &&
                Definition
                    ->HasbForceFocusWhenAdded() &&
                Definition
                    ->bForceFocusWhenAdded)
            {
                bDefinitionFocusHandled =
                    IsExact1040BaseAshtonGadget(
                        Definition)
                        ? EnsureExact1040AshtonBackingAndFocus()
                        : FocusOrQueueExact1040Carmine(
                              PlayerController,
                              Item->ItemEntry.ItemGuid);
            }
        }
    }
    if (OutForceFocusHandled)
        *OutForceFocusHandled =
            bDefinitionFocusHandled;

    return Item;
}

void AFortInventory::HandlePendingCarmineFocus(
    AFortPlayerControllerAthena* PlayerController)
{
    auto Pending =
        PendingCarmineFocus.find(PlayerController);
    if (Pending == PendingCarmineFocus.end() ||
        !PlayerController ||
        !PlayerController->MyFortPawn ||
        !PlayerController->WorldInventory)
    {
        return;
    }

    const FGuid ItemGuid = Pending->second;
    auto Entry =
        PlayerController->WorldInventory
            ->Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Candidate)
                {
                    return AreGuidsEqual(
                        Candidate.ItemGuid,
                        ItemGuid);
                },
                FFortItemEntry::Size());
    PendingCarmineFocus.erase(Pending);
    const bool bValidPendingDefinition =
        Entry &&
        (IsExact1040CarmineGadget(
             Entry->ItemDefinition) ||
         (Entry->ItemDefinition &&
          Entry->ItemDefinition->Name.ToWString() ==
              L"D_CarminePack"));
    if (!bValidPendingDefinition)
    {
        return;
    }

    PlayerController->ServerExecuteInventoryItem(
        ItemGuid);
    PlayerController->ClientEquipItem(
        ItemGuid, true);
    SDK::DbgLog(
        "[Ashton1040] completed queued Carmine focus "
        "controller=%p\n",
        static_cast<void*>(PlayerController));
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
            },
            FFortItemEntry::Size());
        auto ItemInstance = Inventory.ItemInstances.Search(
            [&](UFortWorldItem* Candidate)
            {
                return Candidate &&
                    Candidate->ItemEntry.ItemGuid == UpdatedGuid;
            });
        if (UpdatedReplicatedEntry && ItemInstance && *ItemInstance)
        {
            FFortWeaponMods::CopyEntrySlots(
                *UpdatedReplicatedEntry,
                (*ItemInstance)->ItemEntry);
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
            const bool bPreserveWeaponModAllocation =
                FFortWeaponMods::IsSupported() &&
                FFortItemEntry::HasWeaponModSlots();
            if (bPreserveWeaponModAllocation)
            {
                PreviousWeaponModSlots =
                    repEntry.WeaponModSlots;
            }

            // Preserve the destination's owned nested buffer across the
            // outer entry assignment. CopyEntrySlots can then replace and
            // release it safely instead of losing the allocation header.
            repEntry = *Entry;
            if (bPreserveWeaponModAllocation)
            {
                repEntry.WeaponModSlots =
                    PreviousWeaponModSlots;
            }
            FFortWeaponMods::CopyEntrySlots(*Entry, repEntry);
            repEntry.bIsDirty = false;
            Inventory.MarkItemDirty(repEntry);
            SetRequiresUpdate();

            auto UpdatedReplicatedEntry = Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Candidate)
                {
                    return Candidate.ItemGuid == UpdatedGuid;
                },
                FFortItemEntry::Size());
            if (UpdatedReplicatedEntry)
            {
                FFortWeaponMods::CopyEntrySlots(
                    *UpdatedReplicatedEntry, *Entry);
            }
            break;
        }
    }
_out:
    return;
    /*bRequiresLocalUpdate = true;
    HandleInventoryLocalUpdate();

    return Entry ? Inventory.MarkItemDirty(*Entry) : Inventory.MarkArrayDirty();*/
}

uint64_t ClearAbility_;
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

    /*if (Weapon->WeaponData->EquippedAbilitySet)
    {
        bool bRemoved = false;
        for (auto& [Key, Value] : *(TMap<FGuid, FFortAbilitySetHandle>*)& PlayerController->AppliedInGameModifierAbilitySetHandles)
            if (Key == Weapon->ItemEntryGuid)
            {
                UFortKismetLibrary::UnequipFortAbilitySet(Value);
                bRemoved = true;
                break;
            }

        if (bRemoved)
            PlayerController->ClientRemoveItemAbilitySet(Weapon->WeaponData->EquippedAbilitySet, Weapon->ItemEntryGuid);
    }*/
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
    auto PendingOwner =
        Owner
            ? Owner->Cast<
                  AFortPlayerControllerAthena>()
            : nullptr;
    auto Pending =
        PendingCarmineFocus.find(PendingOwner);
    if (Pending != PendingCarmineFocus.end() &&
        AreGuidsEqual(Pending->second, Guid))
    {
        PendingCarmineFocus.erase(Pending);
    }

    auto ItemEntryIdx = Inventory.ReplicatedEntries.SearchIndex([&](FFortItemEntry& entry)
        { return entry.ItemGuid == Guid; }, FFortItemEntry::Size());
    if (ItemEntryIdx == -1)
        return;

    auto& RemovedEntry = Inventory.ReplicatedEntries.Get(
        ItemEntryIdx, FFortItemEntry::Size());
    auto EntryDef = RemovedEntry.ItemDefinition;

    auto ItemInstanceIdx = Inventory.ItemInstances.SearchIndex([&](UFortWorldItem* entry)
        { return entry && entry->ItemEntry.ItemGuid == Guid; });
    auto ItemInstanceResult = Inventory.ItemInstances.Search([&](UFortWorldItem* entry)
        { return entry && entry->ItemEntry.ItemGuid == Guid; });

    // Save the object before mutating either replicated array. Search returns
    // storage owned by the array, which is invalid after Remove.
    auto Instance = ItemInstanceResult ? *ItemInstanceResult : nullptr;

    // TArray::Remove performs a raw element shift and does not destruct nested
    // arrays. Release the attachment buffers explicitly. If a native inventory
    // update ever left the two entries sharing a header, clear one owner first
    // so the allocation is released exactly once.
    if (FFortWeaponMods::IsSupported() &&
        FFortItemEntry::HasWeaponModSlots())
    {
        if (Instance &&
            RemovedEntry.WeaponModSlots.Data &&
            RemovedEntry.WeaponModSlots.Data ==
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
    // 7.40 removed FortPlayerController::QuickBars and moved to the client-side
    // quickbar model. Reading DEFINE_PROP(QuickBars) when it is absent resolves
    // to offset -1 and produces a bogus pointer (PlayerController - 1). Keep the
    // legacy actor bookkeeping only on builds that expose it; newer builds are
    // updated by the replicated inventory path below.
    AFortQuickBars* QuickBars = nullptr;
    if (VersionInfo.FortniteVersion < 7.40 && PlayerController &&
        PlayerController->HasQuickBars())
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
    _Out:
        QuickBars->EmptySlot(!IsPrimaryQuickbar(EntryDef), i);
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

    HandleInventoryLocalUpdate(); // calls UpdateItemInstances, the func we actually want

    Inventory.MarkArrayDirty();
    ForceNetUpdate();

    if (OnItemInstanceAddedVft && Instance && Instance->ItemEntry.ItemDefinition)
    {
        ((bool(*)(const UFortWorldItem*, const IInterface*, uint32_t)) Instance->Vft[OnItemInstanceAddedVft + 1])(Instance, Owner->GetInterface(IFortInventoryOwnerInterface::StaticClass()), Instance->ItemEntry.Count);
        //((bool(*)(const UFortItemDefinition*, const IInterface*, UFortWorldItem*)) Instance->ItemEntry.ItemDefinition->Vft[OnItemInstanceAddedVft + 1])(Instance->ItemEntry.ItemDefinition, Owner->GetInterface(IFortInventoryOwnerInterface::StaticClass()), Instance);
    }
    //HandleInventoryLocalUpdate();
    //Update(nullptr);
}

int32 AFortInventory::RemoveItem(
    FGuid Guid,
    int32 Count,
    bool bKeepFinalStackEmpty)
{
    auto ItemEntry =
        Inventory.ReplicatedEntries.Search(
            [&](FFortItemEntry& Entry)
            {
                return Entry.ItemGuid == Guid;
            },
            FFortItemEntry::Size());
    if (!ItemEntry || Count == 0)
        return 0;

    const bool bRemoveAll = Count < 0;
    const int32 ExistingCount = max(ItemEntry->Count, 0);
    const int32 RemovedCount =
        bRemoveAll ? ExistingCount : min(ExistingCount, Count);

    // Remove zero/negative terminal stacks too. They are invalid unless the
    // definition explicitly asked the caller to preserve the final empty
    // stack.
    if (bRemoveAll ||
        (!bKeepFinalStackEmpty &&
            (ExistingCount <= 0 || Count >= ExistingCount)))
    {
        Remove(Guid);
        return RemovedCount;
    }

    int32 NewCount =
        bKeepFinalStackEmpty && Count >= ExistingCount
            ? 0
            : ExistingCount - RemovedCount;
    ItemEntry->Count = NewCount;

    auto ItemInstance =
        Inventory.ItemInstances.Search(
            [&](UFortWorldItem* Item)
            {
                return Item &&
                    Item->ItemEntry.ItemGuid == Guid;
            });
    if (ItemInstance && *ItemInstance)
    {
        (*ItemInstance)->ItemEntry.Count = NewCount;
        (*ItemInstance)->ItemEntry.bIsDirty = true;
    }

    UpdateEntry(*ItemEntry);
    return RemovedCount;
}

bool AFortInventory::ShouldBypassItemConsumption(
    AFortPlayerControllerAthena* PlayerController,
    int32 Count,
    bool bForceRemoval)
{
    if (!PlayerController || Count <= 0 || bForceRemoval)
        return false;

    return
        FConfiguration::bInfiniteAmmo.load(
            std::memory_order_acquire) ||
        (PlayerController->HasbInfiniteAmmo() &&
            PlayerController->bInfiniteAmmo);
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

FFortItemEntry* AFortInventory::MakeItemEntry(const UFortItemDefinition* ItemDefinition, int32 Count, int32 Level)
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
        if (FFortWeaponMods::IsSupported() &&
            WeaponDef->HasWeaponModSlots() &&
            FFortItemEntry::HasWeaponModSlots())
        {
            // MakeItemEntry returns a short-lived, caller-owned entry. Keep a
            // read-only view here; every durable destination (pickup or
            // inventory entry) deep-copies it before this temporary is freed.
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
AFortPickupAthena* AFortInventory::SpawnPickup(FVector Loc, FFortItemEntry& Entry, long long SourceTypeFlag, long long SpawnSource, AFortPlayerPawnAthena* Pawn, int OverrideCount, bool Toss, bool RandomRotation, bool bCombine, const UClass* OverrideClass, FVector FinalLoc)
{
    if (!&Entry)
        return nullptr;
    AFortPickupAthena* NewPickup = UWorld::SpawnActor<AFortPickupAthena>(OverrideClass ? OverrideClass : AFortPickupAthena::StaticClass(), Loc, {});
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
        bAllowRandomMods =
            SourceTypeFlag != EFortPickupSourceTypeFlag::GetPlayer() &&
            SourceTypeFlag != EFortPickupSourceTypeFlag::GetTossed() &&
            SpawnSource != EFortPickupSpawnSource::GetPlayerElimination() &&
            SpawnSource != EFortPickupSpawnSource::GetTossedByPlayer();
    }
    FFortWeaponMods::InitializePickup(
        NewPickup, Entry, bAllowRandomMods);

    if (SetPickupItems)
    {
        TArray<FFortItemEntry> a{};
        if (VersionInfo.FortniteVersion >= 16)
            ((void(*)(AFortPickupAthena*, FFortItemEntry*, TArray<FFortItemEntry>*, uint8_t, bool, uint8_t)) SetPickupItems)(NewPickup, &NewPickup->PrimaryPickupItemEntry, &a, (uint8_t)EFortPickupSourceTypeFlag::GetContainer(), false, (uint8_t)EFortPickupSpawnSource::GetChest());
        else
            ((void(*)(AFortPickupAthena*, FFortItemEntry*, TArray<FFortItemEntry>*, bool)) SetPickupItems)(NewPickup, &NewPickup->PrimaryPickupItemEntry, &a, false);
    }
    else
        NewPickup->OnRep_PrimaryPickupItemEntry();
    //NewPickup->OnRep_PrimaryPickupItemEntry();
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
        const UClass* GameModePickupClass =
            FindClass("FortGameModePickup");
        const UObject* PickupDefault =
            OverrideClass->GetDefaultObj();
        if (GameModePickupClass &&
            PickupDefault &&
            PickupDefault->IsA(GameModePickupClass))
        {
            UWorld* World = UWorld::GetWorld();
            auto GameState =
                World && World->GameState
                    ? World->GameState
                          ->Cast<AFortGameStateAthena>()
                    : nullptr;
            if (GameState &&
                GameState
                    ->HasOnPickupSpawnedAndReady())
            {
                GameState->OnPickupSpawnedAndReady
                    .Process(
                        NewPickup,
                        const_cast<UFortItemDefinition*>(
                            Entry.ItemDefinition));
            }
        }
    }

    return NewPickup;
}

AFortPickupAthena* AFortInventory::SpawnPickup(FVector Loc, const UFortItemDefinition* ItemDefinition, int Count, int LoadedAmmo, long long SourceTypeFlag, long long SpawnSource, AFortPlayerPawnAthena* Pawn, bool Toss, bool bRandomRotation, const UClass* OverrideClass)
{
    auto ItemEntry = MakeItemEntry(ItemDefinition, Count, -1);
    if (LoadedAmmo != -1) // -1 keeps the clip/phantom ammo MakeItemEntry derived from the weapon's stats
        ItemEntry->LoadedAmmo = LoadedAmmo;

    auto Pickup = SpawnPickup(Loc, *ItemEntry, SourceTypeFlag, SpawnSource, Pawn, -1, Toss, true, bRandomRotation, OverrideClass);
    FFortWeaponMods::FreeEntrySlots(*ItemEntry);
    free(ItemEntry);
    return Pickup;
}


AFortPickupAthena* AFortInventory::SpawnPickup(ABuildingContainer* Container, FFortItemEntry& Entry, AFortPlayerPawnAthena* Pawn, int OverrideCount)
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

    FFortWeaponMods::InitializePickup(
        NewPickup, Entry, true);

    if (SetPickupItems)
    {
        TArray<FFortItemEntry> a{};
        if (VersionInfo.FortniteVersion >= 16)
            ((void(*)(AFortPickupAthena*, FFortItemEntry*, TArray<FFortItemEntry>*, uint8_t, bool, uint8_t)) SetPickupItems)(NewPickup, &NewPickup->PrimaryPickupItemEntry, &a, (uint8_t)EFortPickupSourceTypeFlag::GetContainer(), false, (uint8_t)EFortPickupSpawnSource::GetChest());
        else
            ((void(*)(AFortPickupAthena*, FFortItemEntry*, TArray<FFortItemEntry>*, bool)) SetPickupItems)(NewPickup, &NewPickup->PrimaryPickupItemEntry, &a, false);
    }
    else
        NewPickup->OnRep_PrimaryPickupItemEntry();
    //NewPickup->OnRep_PrimaryPickupItemEntry();

    NewPickup->PawnWhoDroppedPickup = Pawn;


    //auto bFloorLoot = Container->IsA<ATiered_Athena_FloorLoot_01_C>() || Container->IsA<ATiered_Athena_FloorLoot_Warmup_C>();
    //UFortKismetLibrary::TossPickupFromContainer(UWorld::GetWorld(), Container, NewPickup, 1, 0, Container->LootTossConeHalfAngle_Athena, Container->LootTossDirection_Athena, Container->LootTossSpeed_Athena, false);
    static auto tpfcPtr = UFortKismetLibrary::GetDefaultObj()->GetFunction("TossPickupFromContainer");
    if (tpfcPtr)
    {
        if (!UFortKismetLibrary::TossPickupFromContainer__Ptr)
            UFortKismetLibrary::TossPickupFromContainer__Ptr = tpfcPtr;

        UFortKismetLibrary::TossPickupFromContainer(UWorld::GetWorld(), Container, NewPickup, 10, (int32)std::clamp((float)rand() * 0.0003357036f, 0.f, 10.f), Container->LootTossConeHalfAngle_Athena, Container->LootTossDirection_Athena, Container->LootTossSpeed_Athena, Container->bForceHidePickupMinimapIndicator);
    }
    else
    {
        auto FinalLoc = Loc + (Container->GetActorForwardVector() * Container->LootFinalLocation.X) + (Container->GetActorRightVector() * Container->LootFinalLocation.Y) + (Container->GetActorUpVector() * Container->LootFinalLocation.Z);

        NewPickup->TossPickup(Loc, Pawn, -1, true, true, EFortPickupSourceTypeFlag::GetContainer(), EFortPickupSpawnSource::GetChest());
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
    if (RegeneratingInventoryItems.empty() &&
        RechargingWeaponAmmo.empty())
        return;

    auto World = UWorld::GetWorld();
    if (!World)
        return;

    const double NowSeconds =
        UGameplayStatics::GetTimeSeconds(World);

    for (size_t Index = 0;
        Index < RegeneratingInventoryItems.size();)
    {
        auto& State = RegeneratingInventoryItems[Index];
        auto Owner = State.Owner.Get();
        auto AmmoDefinition = State.AmmoDefinition.Get();

        if (!Owner || !Owner->WorldInventory || !AmmoDefinition ||
            State.MaxCount <= 0 ||
            !std::isfinite(State.CooldownSeconds) ||
            State.CooldownSeconds <= 0.0)
        {
            RemoveRegenItemAt(Index);
            continue;
        }

        auto Inventory = Owner->WorldInventory;
        auto ReplicatedEntry =
            Inventory->Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Candidate)
                {
                    return AreGuidsEqual(
                            Candidate.ItemGuid,
                            State.ItemGuid) &&
                        Candidate.ItemDefinition == AmmoDefinition;
                },
                FFortItemEntry::Size());
        auto ItemInstance =
            Inventory->Inventory.ItemInstances.Search(
                [&](UFortWorldItem* Candidate)
                {
                    return Candidate &&
                        AreGuidsEqual(
                            Candidate->ItemEntry.ItemGuid,
                            State.ItemGuid) &&
                        Candidate->ItemEntry.ItemDefinition ==
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

        int32 NewCount = min(
            max(ReplicatedEntry->Count, 0) + 1,
            State.MaxCount);
        ReplicatedEntry->Count = NewCount;
        (*ItemInstance)->ItemEntry.Count = NewCount;
        (*ItemInstance)->ItemEntry.bIsDirty = true;

        const bool bReachedMaximum =
            NewCount >= State.MaxCount;
        const int32 MaximumCount = State.MaxCount;
        const auto DefinitionName =
            AmmoDefinition->Name.ToString();
        if (bReachedMaximum)
        {
            RemoveRegenItemAt(Index);
        }
        else
        {
            // One charge is restored per complete cooldown. Starting the
            // next interval from the current server time prevents a hitch
            // from granting several queued charges in a single frame.
            State.NextRefillTime =
                NowSeconds + State.CooldownSeconds;
            ++Index;
        }

        Inventory->UpdateEntry(*ReplicatedEntry);
        SDK::DbgLog(
            "[ItemRegen] refilled definition=%s count=%d max=%d\n",
            DefinitionName.c_str(),
            NewCount,
            MaximumCount);
    }

    for (size_t Index = 0;
        Index < RechargingWeaponAmmo.size();)
    {
        auto& State = RechargingWeaponAmmo[Index];
        auto Owner = State.Owner.Get();
        auto WeaponDefinition =
            State.WeaponDefinition.Get();
        if (!Owner || !Owner->WorldInventory ||
            !WeaponDefinition ||
            State.MaxLoadedAmmo <= 0 ||
            State.RechargeAmount <= 0 ||
            !std::isfinite(State.RechargeIntervalSeconds) ||
            State.RechargeIntervalSeconds <= 0.0)
        {
            RemoveRechargingWeaponAt(Index);
            continue;
        }

        auto Inventory = Owner->WorldInventory;
        auto ReplicatedEntry =
            Inventory->Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Candidate)
                {
                    return AreGuidsEqual(
                            Candidate.ItemGuid,
                            State.ItemGuid) &&
                        Candidate.ItemDefinition ==
                            WeaponDefinition;
                },
                FFortItemEntry::Size());
        auto ItemInstance =
            Inventory->Inventory.ItemInstances.Search(
                [&](UFortWorldItem* Candidate)
                {
                    return Candidate &&
                        AreGuidsEqual(
                            Candidate->ItemEntry.ItemGuid,
                            State.ItemGuid) &&
                        Candidate->ItemEntry.ItemDefinition ==
                            WeaponDefinition;
                });
        if (!ReplicatedEntry || !ItemInstance ||
            !*ItemInstance)
        {
            RemoveRechargingWeaponAt(Index);
            continue;
        }

        int32 CurrentLoadedAmmo =
            std::clamp(
                ReplicatedEntry->LoadedAmmo,
                0,
                State.MaxLoadedAmmo);
        if (CurrentLoadedAmmo >= State.MaxLoadedAmmo)
        {
            if (State.LastObservedLoadedAmmo !=
                State.MaxLoadedAmmo)
            {
                SyncEquippedWeaponAmmo(
                    Owner,
                    WeaponDefinition,
                    State.ItemGuid,
                    State.MaxLoadedAmmo);
            }
            State.LastObservedLoadedAmmo =
                State.MaxLoadedAmmo;
            State.NextRefillTime = 0.0;
            ++Index;
            continue;
        }

        // The native FN30 recharge component normally wins this race.
        // If it restored a charge, observe the increase and defer this
        // watchdog for another full interval instead of double-granting.
        if (CurrentLoadedAmmo >
            State.LastObservedLoadedAmmo)
        {
            SyncEquippedWeaponAmmo(
                Owner,
                WeaponDefinition,
                State.ItemGuid,
                CurrentLoadedAmmo);
            State.LastObservedLoadedAmmo =
                CurrentLoadedAmmo;
            State.NextRefillTime =
                NowSeconds +
                State.RechargeIntervalSeconds +
                NativeRechargeGraceSeconds;
            NotifyNitroFistsRechargeStarted(
                Owner, State.ItemGuid, NowSeconds);
            ++Index;
            continue;
        }
        bool bStartedRechargeCycle = false;
        if (CurrentLoadedAmmo <
            State.LastObservedLoadedAmmo)
        {
            State.LastObservedLoadedAmmo =
                CurrentLoadedAmmo;
            if (State.NextRefillTime <= 0.0)
            {
                State.NextRefillTime =
                    NowSeconds +
                    State.RechargeIntervalSeconds +
                    NativeRechargeGraceSeconds;
                bStartedRechargeCycle = true;
            }
        }
        else if (State.NextRefillTime <= 0.0)
        {
            State.NextRefillTime =
                NowSeconds +
                State.RechargeIntervalSeconds +
                NativeRechargeGraceSeconds;
            bStartedRechargeCycle = true;
        }
        if (bStartedRechargeCycle)
        {
            NotifyNitroFistsRechargeStarted(
                Owner, State.ItemGuid, NowSeconds);
        }

        if (NowSeconds < State.NextRefillTime)
        {
            ++Index;
            continue;
        }

        int32 NewLoadedAmmo =
            min(
                CurrentLoadedAmmo +
                    State.RechargeAmount,
                State.MaxLoadedAmmo);
        ReplicatedEntry->LoadedAmmo = NewLoadedAmmo;
        auto WorldItem = *ItemInstance;
        WorldItem->ItemEntry.LoadedAmmo =
            NewLoadedAmmo;
        WorldItem->ItemEntry.bIsDirty = true;
        State.LastObservedLoadedAmmo =
            NewLoadedAmmo;

        const int32 MaximumLoadedAmmo =
            State.MaxLoadedAmmo;
        const auto DefinitionName =
            WeaponDefinition->Name.ToString();
        if (NewLoadedAmmo >= State.MaxLoadedAmmo)
        {
            State.NextRefillTime = 0.0;
            ++Index;
        }
        else
        {
            State.NextRefillTime =
                NowSeconds +
                State.RechargeIntervalSeconds +
                NativeRechargeGraceSeconds;
            NotifyNitroFistsRechargeStarted(
                Owner, State.ItemGuid, NowSeconds);
            ++Index;
        }

        Inventory->UpdateEntry(*ReplicatedEntry);
        BroadcastWorldItemAmmoChanged(WorldItem);
        const bool bEquippedWeaponSynced =
            SyncEquippedWeaponAmmo(
                Owner,
                WeaponDefinition,
                State.ItemGuid,
                NewLoadedAmmo);
        SDK::DbgLog(
            "[WeaponRecharge] fallback-refilled "
            "definition=%s ammo=%d/%d equipped-sync=%d\n",
            DefinitionName.c_str(),
            NewLoadedAmmo,
            MaximumLoadedAmmo,
            bEquippedWeaponSynced ? 1 : 0);
    }
}

void AFortInventory::BeginNativeDeathInventoryRetention(
    AFortPlayerControllerAthena* PlayerController,
    const std::vector<FGuid>& ItemGuids)
{
    if (!PlayerController || ItemGuids.empty())
        return;

    NativeDeathInventoryRetention[PlayerController] =
        ItemGuids;
}

void AFortInventory::EndNativeDeathInventoryRetention(
    AFortPlayerControllerAthena* PlayerController)
{
    if (PlayerController)
        NativeDeathInventoryRetention.erase(
            PlayerController);
}


void AFortInventory::UpdateEntry(FFortItemEntry& Entry)
{
    if (!this)
        return; // wtf 3.5


    /*auto ent = Inventory.ReplicatedEntries.Search([&](FFortItemEntry& item)
        { return item.ItemGuid == Entry.ItemGuid; }, FFortItemEntry::Size());
    if (ent)
        *ent = Entry;*/

        /*auto ent2 = Inventory.ItemInstances.Search([&](UFortWorldItem* item)
            { return item->ItemEntry.ItemGuid == Entry.ItemGuid; });
        if (ent2)
            (*ent2)->ItemEntry = Entry;*/
            //memcpy((PBYTE)&(*ent)->ItemEntry, (const PBYTE)&Entry, FFortItemEntry::Size());

    Update(&Entry);
}

using RemoveInventoryItemWithQuickBarFn = bool(*)(
    IInterface*,
    FGuid&,
    int32,
    bool,
    bool);

using RemoveInventoryItemSingleFlagFn = bool(*)(
    IInterface*,
    FGuid&,
    int32,
    bool);

enum class ERemoveInventoryItemAbi
{
    None,
    QuickBarFlagOnly,
    WithoutQuickBarFlag,
    WithQuickBarFlag
};

RemoveInventoryItemWithQuickBarFn
    RemoveInventoryItemWithQuickBarOG = nullptr;
RemoveInventoryItemSingleFlagFn
    RemoveInventoryItemQuickBarOnlyOG = nullptr;
RemoveInventoryItemSingleFlagFn
    RemoveInventoryItemWithoutQuickBarOG = nullptr;
ERemoveInventoryItemAbi RemoveInventoryItemAbi =
    ERemoveInventoryItemAbi::None;

bool CallRemoveInventoryItemOriginal(
    IInterface* Interface,
    FGuid& ItemGuid,
    int32 Count,
    bool bForceRemoveFromQuickBars,
    bool bForceRemoval)
{
    if (RemoveInventoryItemAbi ==
            ERemoveInventoryItemAbi::WithQuickBarFlag &&
        RemoveInventoryItemWithQuickBarOG)
    {
        return RemoveInventoryItemWithQuickBarOG(
            Interface,
            ItemGuid,
            Count,
            bForceRemoveFromQuickBars,
            bForceRemoval);
    }

    if (RemoveInventoryItemAbi ==
            ERemoveInventoryItemAbi::QuickBarFlagOnly &&
        RemoveInventoryItemQuickBarOnlyOG)
    {
        return RemoveInventoryItemQuickBarOnlyOG(
            Interface,
            ItemGuid,
            Count,
            bForceRemoveFromQuickBars);
    }

    if (RemoveInventoryItemAbi ==
            ERemoveInventoryItemAbi::WithoutQuickBarFlag &&
        RemoveInventoryItemWithoutQuickBarOG)
    {
        return RemoveInventoryItemWithoutQuickBarOG(
            Interface,
            ItemGuid,
            Count,
            bForceRemoval);
    }

    return false;
}

bool RemoveInventoryItemInternal(
    IInterface* Interface,
    FGuid& ItemGuid,
    int32 Count,
    bool bForceRemoveFromQuickBars,
    bool bForceRemoval)
{
    if (!Interface)
    {
        return CallRemoveInventoryItemOriginal(
            Interface,
            ItemGuid,
            Count,
            bForceRemoveFromQuickBars,
            bForceRemoval);
    }

    auto PlayerControllerClass = FindClass("FortPlayerController");
    auto PlayerControllerSuper =
        PlayerControllerClass
            ? PlayerControllerClass->GetSuper()
            : nullptr;
    if (!PlayerControllerSuper)
    {
        return CallRemoveInventoryItemOriginal(
            Interface,
            ItemGuid,
            Count,
            bForceRemoveFromQuickBars,
            bForceRemoval);
    }

    const uint64 InterfaceOffset =
        static_cast<uint64>(
            PlayerControllerSuper->GetPropertiesSize()) +
        (VersionInfo.EngineVersion >= 4.27 ? 16ull : 8ull);
    const uint64 InterfaceAddress =
        reinterpret_cast<uint64>(Interface);
    if (InterfaceAddress < InterfaceOffset)
    {
        return CallRemoveInventoryItemOriginal(
            Interface,
            ItemGuid,
            Count,
            bForceRemoveFromQuickBars,
            bForceRemoval);
    }

    auto PlayerController =
        reinterpret_cast<AFortPlayerControllerAthena*>(
            InterfaceAddress - InterfaceOffset);
    if (!SDK::MemReadable(PlayerController, sizeof(void*)))
    {
        return CallRemoveInventoryItemOriginal(
            Interface,
            ItemGuid,
            Count,
            bForceRemoveFromQuickBars,
            bForceRemoval);
    }

    if (auto Retention =
            NativeDeathInventoryRetention.find(
                PlayerController);
        Retention !=
            NativeDeathInventoryRetention.end())
    {
        const bool bRetainItem =
            std::any_of(
                Retention->second.begin(),
                Retention->second.end(),
                [&](const FGuid& RetainedGuid)
                {
                    return
                        RetainedGuid.A == ItemGuid.A &&
                        RetainedGuid.B == ItemGuid.B &&
                        RetainedGuid.C == ItemGuid.C &&
                        RetainedGuid.D == ItemGuid.D;
                });
        if (bRetainItem)
            return true;
    }

    auto WorldInventory = PlayerController->WorldInventory;
    if (!WorldInventory ||
        !SDK::MemReadable(WorldInventory, sizeof(void*)) ||
        Count == 0)
    {
        return CallRemoveInventoryItemOriginal(
            Interface,
            ItemGuid,
            Count,
            bForceRemoveFromQuickBars,
            bForceRemoval);
    }

    auto ItemInstance =
        WorldInventory->Inventory.ItemInstances.Search(
            [&](UFortWorldItem* Item)
            {
                return Item &&
                    Item->ItemEntry.ItemGuid == ItemGuid;
            });
    auto ItemEntry =
        WorldInventory->Inventory.ReplicatedEntries.Search(
            [&](FFortItemEntry& Entry)
            {
                return Entry.ItemGuid == ItemGuid;
            },
            FFortItemEntry::Size());
    if (!ItemEntry)
    {
        return CallRemoveInventoryItemOriginal(
            Interface,
            ItemGuid,
            Count,
            bForceRemoveFromQuickBars,
            bForceRemoval);
    }

    // Item instances are not materialized consistently for every inventory
    // stack on every supported build. A valid replicated row is sufficient
    // to identify and suppress ordinary consumption. The quickbar flag is
    // commonly set when the final trap or throwable is used, so only the
    // distinct force-removal flag may override Infinite Ammo here.
    if (AFortInventory::ShouldBypassItemConsumption(
            PlayerController, Count, bForceRemoval))
    {
        return true;
    }

    if (!ItemInstance || !*ItemInstance)
    {
        return CallRemoveInventoryItemOriginal(
            Interface,
            ItemGuid,
            Count,
            bForceRemoveFromQuickBars,
            bForceRemoval);
    }

    auto Item = *ItemInstance;
    auto ItemDefinition = ItemEntry->ItemDefinition;
    const bool bForceRemoveItem =
        bForceRemoveFromQuickBars || bForceRemoval;

    auto AmmoDefinition =
        ItemDefinition
            ? ItemDefinition->Cast<UFortAmmoItemDefinition>()
            : nullptr;

    const float ItemLevel =
        Item->ItemEntry.Level > 0
            ? static_cast<float>(Item->ItemEntry.Level)
            : 1.0f;
    double RegenCooldownSeconds = 0.0;
    int32 RegenMaximumCount = 0;
    if (Count > 0 && !bForceRemoveItem && AmmoDefinition &&
        AmmoDefinition->HasRegenCooldown())
    {
        RegenCooldownSeconds =
            AmmoDefinition->EvaluateRegenCooldown(ItemLevel);

        // GetMaxStackSize can depend on an attribute set for some item
        // types. The pre-consumption count is an authoritative lower
        // bound and preserves the full charge capacity if that lookup is
        // unavailable in a server-only session.
        RegenMaximumCount = max(
            AmmoDefinition->GetMaxStackSize(),
            ItemEntry->Count);
        if (RegenMaximumCount <= 0 ||
            RegenMaximumCount > 10000 ||
            !std::isfinite(RegenCooldownSeconds) ||
            RegenCooldownSeconds <= 0.0 ||
            RegenCooldownSeconds > 3600.0)
        {
            RegenCooldownSeconds = 0.0;
            RegenMaximumCount = 0;
        }
    }

    bool bKeepFinalStackEmpty = false;
    if (Count > 0 &&
        !bForceRemoveItem &&
        ItemDefinition &&
        ItemDefinition->HasbPersistInInventoryWhenFinalStackEmpty() &&
        ItemDefinition->bPersistInInventoryWhenFinalStackEmpty)
    {
        auto OtherStack =
            WorldInventory->Inventory.ReplicatedEntries.Search(
                [&](FFortItemEntry& Entry)
                {
                    return Entry.ItemDefinition == ItemDefinition &&
                        Entry.ItemGuid != ItemGuid &&
                        Entry.Count > 0;
                },
                FFortItemEntry::Size());
        bKeepFinalStackEmpty = !OtherStack;
    }

    const int32 RemovedCount =
        WorldInventory->RemoveItem(
            ItemGuid,
            Count,
            bKeepFinalStackEmpty);

    if (RemovedCount > 0 && RegenMaximumCount > 0)
    {
        ScheduleRegeneratingInventoryItem(
            PlayerController,
            AmmoDefinition,
            ItemGuid,
            RegenMaximumCount,
            RegenCooldownSeconds);
    }

    // Reaching a persistent empty stack or removing an already-empty stale
    // entry can legitimately remove zero units while still succeeding.
    return true;
}

bool RemoveInventoryItemWithQuickBar(
    IInterface* Interface,
    FGuid& ItemGuid,
    int32 Count,
    bool bForceRemoveFromQuickBars,
    bool bForceRemoval)
{
    return RemoveInventoryItemInternal(
        Interface,
        ItemGuid,
        Count,
        bForceRemoveFromQuickBars,
        bForceRemoval);
}

bool RemoveInventoryItemWithoutQuickBar(
    IInterface* Interface,
    FGuid& ItemGuid,
    int32 Count,
    bool bForceRemoval)
{
    return RemoveInventoryItemInternal(
        Interface,
        ItemGuid,
        Count,
        false,
        bForceRemoval);
}

bool RemoveInventoryItemQuickBarOnly(
    IInterface* Interface,
    FGuid& ItemGuid,
    int32 Count,
    bool bForceRemoveFromQuickBars)
{
    return RemoveInventoryItemInternal(
        Interface,
        ItemGuid,
        Count,
        bForceRemoveFromQuickBars,
        false);
}

ERemoveInventoryItemAbi ResolveRemoveInventoryItemAbi()
{
    // The reflected RPC is a separate function, but its named flag changes
    // track the three interface layouts seen in the supported binaries.
    // Treat that correlation as a selector, never as permission to guess an
    // unfamiliar native ABI.
    const auto ControllerClass =
        AFortPlayerControllerAthena::StaticClass();
    const auto ControllerDefault =
        ControllerClass
            ? ControllerClass->GetDefaultObj()
            : nullptr;
    const auto ServerRemoveInventoryItem =
        ControllerDefault
            ? ControllerDefault->GetFunction(
                "ServerRemoveInventoryItem")
            : nullptr;

    if (ServerRemoveInventoryItem)
    {
        constexpr uint64 CPF_Parm =
            0x0000000000000080;
        constexpr uint64 CPF_ReturnParm =
            0x0000000000000400;
        const auto Parameters =
            ServerRemoveInventoryItem->GetParamsNamed();
        const bool bHasMetadata =
            !Parameters.NameOffsetMap.empty();
        bool bHasItemGuid = false;
        bool bHasCount = false;
        bool bHasForceRemoval = false;
        bool bHasQuickBarFlag = false;
        bool bHasUnknownParameter = false;

        for (const auto& Parameter :
            Parameters.NameOffsetMap)
        {
            if (!(Parameter.PropertyFlags & CPF_Parm) ||
                (Parameter.PropertyFlags & CPF_ReturnParm))
            {
                continue;
            }

            if (Parameter.Name == "ItemGuid" ||
                Parameter.Name == "ItemGUID")
            {
                bHasItemGuid = true;
            }
            else if (Parameter.Name == "Count")
            {
                bHasCount = true;
            }
            else if (
                Parameter.Name ==
                    "bForceRemoveFromQuickBars" ||
                Parameter.Name ==
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

        if (bHasItemGuid && bHasCount &&
            bHasForceRemoval && bHasQuickBarFlag &&
            !bHasUnknownParameter)
        {
            return ERemoveInventoryItemAbi::
                WithQuickBarFlag;
        }

        if (bHasItemGuid && bHasCount &&
            bHasQuickBarFlag && !bHasForceRemoval &&
            !bHasUnknownParameter)
        {
            return ERemoveInventoryItemAbi::
                QuickBarFlagOnly;
        }

        if (bHasItemGuid && bHasCount &&
            bHasForceRemoval && !bHasQuickBarFlag &&
            !bHasUnknownParameter)
        {
            return ERemoveInventoryItemAbi::
                WithoutQuickBarFlag;
        }

        // A partial or unfamiliar reflected layout is evidence that this
        // build should not receive a guessed native detour.
        if (bHasMetadata)
            return ERemoveInventoryItemAbi::None;
    }

    // These fallbacks are only for an empty reflected layout and only where
    // the native interface form is independently evidenced. Do not turn an
    // unknown or zero version into a guessed detour.
    const double FortniteVersion =
        VersionInfo.FortniteVersion;
    if (FortniteVersion == 1.72)
    {
        return ERemoveInventoryItemAbi::
            QuickBarFlagOnly;
    }

    const bool bKnownFiveArgumentBuild =
        (FortniteVersion >= 1.91 &&
            FortniteVersion <= 6.00) ||
        FortniteVersion == 1.10 ||
        FortniteVersion == 1.11 ||
        FortniteVersion == 10.40 ||
        FortniteVersion == 13.40;
    if (bKnownFiveArgumentBuild)
    {
        return ERemoveInventoryItemAbi::
            WithQuickBarFlag;
    }

    return ERemoveInventoryItemAbi::None;
}

void SetLoadedAmmo(UFortWorldItem* Item, int LoadedAmmo)
{
    if (!Item)
        return;

    // The item instance is the first durable copy of the magazine. Update it
    // even when a legacy build cannot resolve the owning controller or its
    // replicated row yet; otherwise a later equip can resurrect stale ammo.
    const int32 PreviousItemLoadedAmmo =
        Item->ItemEntry.LoadedAmmo;
    Item->ItemEntry.LoadedAmmo = LoadedAmmo;
    Item->ItemEntry.bIsDirty = true;

    auto PlayerController = (AFortPlayerControllerAthena*)Item->GetOwningController();
    if (!PlayerController || !PlayerController->WorldInventory)
        return;

    //PlayerController->WorldInventory->UpdateEntry(Item->ItemEntry);
    auto repEnt = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& item)
        { return item.ItemGuid == Item->ItemEntry.ItemGuid; }, FFortItemEntry::Size());
    if (!repEnt)
        return;

    const int32 PreviousLoadedAmmo =
        repEnt == &Item->ItemEntry
            ? PreviousItemLoadedAmmo
            : repEnt->LoadedAmmo;
    auto WeaponDefinition =
        Item->ItemEntry.ItemDefinition
            ? Item->ItemEntry.ItemDefinition->Cast<
                UFortWeaponItemDefinition>()
            : nullptr;

    // Persist the magazine value reported by the native weapon path.
    // Re-equipping hydrates AFortWeapon::AmmoCount from this item entry, so
    // retaining the previous/full value here makes every weapon swap look
    // like a free reload. Infinite Ammo is enforced against reserve-ammo and
    // consumable-stack removal instead: magazines behave normally and can be
    // reloaded forever without traps, throwables, or reserve ammo decreasing.
    repEnt->LoadedAmmo = LoadedAmmo;
    PlayerController->WorldInventory->UpdateEntry(*repEnt);
    BroadcastWorldItemAmmoChanged(Item);

    if (IsNitroFistsDefinition(WeaponDefinition))
    {
        // The common item-change broadcast above preserves the signal used
        // by FN30's FortControllerComponent_RechargeWeapons.
        ObserveRechargingWeaponAmmo(
            PlayerController,
            WeaponDefinition,
            Item->ItemEntry.ItemGuid,
            Item->ItemEntry.Level,
            PreviousLoadedAmmo,
            LoadedAmmo);
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

    const int32 PreviousPhantomReserveAmmo =
        repEnt->PhantomReserveAmmo;
    auto WeaponDefinition =
        Item->ItemEntry.ItemDefinition
            ? Item->ItemEntry.ItemDefinition->Cast<
                UFortWeaponItemDefinition>()
            : nullptr;
    const bool bPreservePhantomReserve =
        AFortInventory::ShouldBypassItemConsumption(
            PlayerController, 1, false) &&
        ResolveEquippedWeaponForItem(
            PlayerController,
            WeaponDefinition,
            Item->ItemEntry.ItemGuid) &&
        PreviousPhantomReserveAmmo > 0 &&
        PhantomReserveAmmo <=
            static_cast<unsigned int>(
                (std::numeric_limits<int32>::max)()) &&
        static_cast<int32>(PhantomReserveAmmo) <
            PreviousPhantomReserveAmmo;
    int32 AppliedPhantomReserveAmmo =
        bPreservePhantomReserve
            ? PreviousPhantomReserveAmmo
            : static_cast<int32>(PhantomReserveAmmo);

    repEnt->PhantomReserveAmmo =
        AppliedPhantomReserveAmmo;
    Item->ItemEntry.PhantomReserveAmmo =
        AppliedPhantomReserveAmmo;
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

    *Ret = AFortInventory::SpawnPickup(Position, ItemDefinition, NumberToSpawn, -1, EFortPickupSourceTypeFlag::GetOther(), EFortPickupSpawnSource::GetSupplyDrop());
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

    const UClass* GameModePickupClass =
        FindClass("FortGameModePickup");
    const UClass* OverrideClass = PickupClass.Get();
    if (!OverrideClass)
        OverrideClass = GameModePickupClass;
    const UObject* PickupDefault =
        OverrideClass ? OverrideClass->GetDefaultObj() : nullptr;
    if (!GameModePickupClass ||
        !PickupDefault ||
        !PickupDefault->IsA(GameModePickupClass))
    {
        SDK::DbgLog(
            "[SupplyDrop] rejected invalid game-mode pickup class=%p item=%p\n",
            (void*)OverrideClass,
            (void*)ItemDefinition);
        return;
    }

    const bool bCurrentAshtonStone =
        FFortAthenaNativeLTMCompatibility::
            IsCurrentAshtonStone(
                TriggeringPawn, ItemDefinition);
    if (bCurrentAshtonStone)
    {
        if (FFortAthenaNativeLTMCompatibility::
                IsAshtonStoneCaptured(
                    ItemDefinition))
        {
            SDK::DbgLog(
                "[Ashton1040] suppressed deferred pickup "
                "spawn for captured stone item=%s\n",
                ItemDefinition->Name.ToString().c_str());
            return;
        }

        // The rock carrier can disappear before its already-queued SpawnLoot
        // callback runs. In that gap the compatibility watchdog may restore
        // the missing gem; reuse it here instead of materializing a second
        // copy of the same stone. Scan the common game-mode base so a
        // color-specific subclass and the compatibility base are both seen.
        UWorld* World = UWorld::GetWorld();
        auto ExistingActors =
            World
                ? UGameplayStatics::GetAllActorsOfClass(
                      World, GameModePickupClass)
                : TArray<AActor*>{};
        if (ExistingActors.Num() >= 0 &&
            ExistingActors.Num() <= 256 &&
            ExistingActors.Max() >=
                ExistingActors.Num() &&
            ExistingActors.Max() <= 512)
        {
            for (auto Actor : ExistingActors)
            {
                auto ExistingPickup =
                    Actor &&
                            Actor->IsA(
                                GameModePickupClass)
                        ? Actor->Cast<
                              AFortPickupAthena>()
                        : nullptr;
                if (!ExistingPickup ||
                    !ExistingPickup->HasAuthority() ||
                    (ExistingPickup
                         ->HasbActorIsBeingDestroyed() &&
                     ExistingPickup
                         ->bActorIsBeingDestroyed) ||
                    (ExistingPickup->HasbPickedUp() &&
                     ExistingPickup->bPickedUp) ||
                    ExistingPickup
                            ->PrimaryPickupItemEntry
                            .ItemDefinition !=
                        ItemDefinition)
                {
                    continue;
                }

                ExistingPickup->SetLifeSpan(0.0f);
                ExistingPickup->ForceNetUpdate();
                *Ret = ExistingPickup;
                SDK::DbgLog(
                    "[Ashton1040] reused existing stone "
                    "pickup for deferred carrier callback "
                    "item=%s pickup=%p class=%s\n",
                    ItemDefinition->Name
                        .ToString().c_str(),
                    static_cast<void*>(
                        ExistingPickup),
                    ExistingPickup->Class->Name
                        .ToString().c_str());
                break;
            }
        }
        ExistingActors.Free();
        if (*Ret)
            return;
    }

    // 10.40's Ashton rock supply drops use this native UFunction instead of
    // SpawnPickup so the six stone definitions retain their authored custom
    // pickup classes. The stripped server has no usable body for it; spawning
    // the generic pickup here would also bypass Ashton's lifecycle delegates.
    *Ret = AFortInventory::SpawnPickup(
        Position,
        ItemDefinition,
        NumberToSpawn,
        -1,
        EFortPickupSourceTypeFlag::GetOther(),
        EFortPickupSpawnSource::GetSupplyDrop(),
        TriggeringPawn,
        true,
        false,
        OverrideClass);

    if (*Ret)
    {
        // FortGameModePickup objectives are persistent world objectives. The
        // generic TossPickup path can inherit a finite pickup lifespan, which
        // made every uncollected Infinity Stone expire and caused the Ashton
        // watchdog to spawn it again. A zero lifespan clears that timer.
        (*Ret)->SetLifeSpan(0.0f);
        (*Ret)->ForceNetUpdate();
    }

    SDK::DbgLog(
        "[SupplyDrop] game-mode pickup item=%p class=%p result=%p direction=(%.1f,%.1f,%.1f)\n",
        (void*)ItemDefinition,
        (void*)OverrideClass,
        (void*)*Ret,
        Direction.X,
        Direction.Y,
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

    const auto RemoveInventoryItemAddress =
        FindRemoveInventoryItem();
    RemoveInventoryItemAbi =
        ResolveRemoveInventoryItemAbi();

    bool bRemoveInventoryItemHooked = false;
    if (RemoveInventoryItemAddress &&
        RemoveInventoryItemAbi ==
            ERemoveInventoryItemAbi::WithQuickBarFlag)
    {
        Utils::Hook(
            RemoveInventoryItemAddress,
            RemoveInventoryItemWithQuickBar,
            RemoveInventoryItemWithQuickBarOG);
        bRemoveInventoryItemHooked =
            RemoveInventoryItemWithQuickBarOG != nullptr;
    }
    else if (RemoveInventoryItemAddress &&
        RemoveInventoryItemAbi ==
            ERemoveInventoryItemAbi::QuickBarFlagOnly)
    {
        Utils::Hook(
            RemoveInventoryItemAddress,
            RemoveInventoryItemQuickBarOnly,
            RemoveInventoryItemQuickBarOnlyOG);
        bRemoveInventoryItemHooked =
            RemoveInventoryItemQuickBarOnlyOG != nullptr;
    }
    else if (RemoveInventoryItemAddress &&
        RemoveInventoryItemAbi ==
            ERemoveInventoryItemAbi::WithoutQuickBarFlag)
    {
        Utils::Hook(
            RemoveInventoryItemAddress,
            RemoveInventoryItemWithoutQuickBar,
            RemoveInventoryItemWithoutQuickBarOG);
        bRemoveInventoryItemHooked =
            RemoveInventoryItemWithoutQuickBarOG != nullptr;
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
    SDK::DbgLog(
        "  [FI] 2 RemoveInventoryItem %s "
        "(abi=%s target=%p)\n",
        bRemoveInventoryItemHooked
            ? "hooked"
            : "unavailable",
        RemoveInventoryItemAbiName,
        reinterpret_cast<void*>(
            RemoveInventoryItemAddress));
    // need to see if these are used
    //Utils::Hook(FindRemoveInventoryStateValue(), RemoveInventoryStateValue);
    //Utils::Hook(FindSetInventoryStateValue(), SetInventoryStateValue);

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
            const uint32 LoadedAmmoSetterIndex =
                uint32(
                    SetOwningInventoryIdx -
                    (HasPhantomReserveAmmo
                        ? (VersionInfo.EngineVersion < 4.27
                            ? 2
                            : 3)
                        : 1));

            Utils::Hook<UFortWorldItem>(
                LoadedAmmoSetterIndex,
                SetLoadedAmmo);
            if (HasPhantomReserveAmmo)
                Utils::Hook<UFortWorldItem>(uint32(SetOwningInventoryIdx - (VersionInfo.EngineVersion < 4.27 ? 1 : 2)), SetPhantomReserveAmmo);

            SDK::DbgLog(
                "[WeaponRecharge] loaded-ammo setter hook "
                "installed index=%u\n",
                LoadedAmmoSetterIndex);
        }
        else
        {
            SDK::DbgLog(
                "[WeaponRecharge] loaded-ammo setter hook "
                "unavailable: owner setter vft index missing\n");
        }
    }
    else
    {
        SDK::DbgLog(
            "[WeaponRecharge] loaded-ammo setter hook "
            "unavailable: owner setter signature missing\n");
    }

    SDK::DbgLog("  [FI] 3 SetOwningInventory block done\n");
    if (auto sd = DefaultObjImpl("FortAthenaSupplyDrop"))
    {
        if (auto SpawnPickupFunction =
                sd->GetFunction("SpawnPickup"))
        {
            Utils::ExecHook(
                SpawnPickupFunction,
                SpawnPickup_);
        }
        if (auto SpawnGameModePickupFunction =
                sd->GetFunction("SpawnGameModePickup"))
        {
            Utils::ExecHook(
                SpawnGameModePickupFunction,
                SpawnGameModePickup_);
        }
    }
    SDK::DbgLog("  [FI] 4 PostLoadHook complete\n");
}
