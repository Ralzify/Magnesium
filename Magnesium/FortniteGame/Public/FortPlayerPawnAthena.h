#pragma once
#include "../../pch.h"
#include "GameplayTagContainer.h"

enum class ETryExitVehicleBehavior : uint8
{
    DoNotForce = 0,
    ForceOnBlocking = 1,
    ForceAlways = 2
};

class UCharacterMovementComponent : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UCharacterMovementComponent);

    DEFINE_PROP(Velocity, FVector);

    DEFINE_BITFIELD_PROP(bCheatFlying, bool);

    DEFINE_FUNC(SetMovementMode, void);
};

class UFortMovementComp_CharacterAthena : public UCharacterMovementComponent
{
public:
    UCLASS_COMMON_MEMBERS(UFortMovementComp_CharacterAthena);

    DEFINE_PROP(JumpPenaltyResetTime, float);
};

struct FZiplinePawnState
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FZiplinePawnState);

    DEFINE_STRUCT_PROP(bJumped, bool);

    uint8_t Padding[0x100];
};

class AFortAscenderZipline : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortAscenderZipline);

    DEFINE_NEWOBJ_PROP(PawnUsingHandle, AActor);
    DEFINE_PROP(PreviousPawnUsingHandle, TWeakObjectPtr<AActor>);

    DEFINE_FUNC(OnRep_PawnUsingHandle, void);
    DEFINE_FUNC(OnZipliningStarted, void);
    DEFINE_FUNC(OnZipliningStopped, void);
};

struct FFortGameplayAttributeData
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FFortGameplayAttributeData);

    DEFINE_STRUCT_PROP(CurrentValue, float);
    DEFINE_STRUCT_PROP(BaseValue, float);
    DEFINE_STRUCT_PROP(Minimum, float);
    DEFINE_STRUCT_PROP(Maximum, float);
    DEFINE_STRUCT_PROP(UnclampedBaseValue, float);
    DEFINE_STRUCT_PROP(UnclampedCurrentValue, float);
};

class UFortHealthSet : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UFortHealthSet);

    DEFINE_PROP(Health, FFortGameplayAttributeData);
    DEFINE_PROP(MaxHealth, FFortGameplayAttributeData);

    // Naming trap: on the builds that need the fallback path in
    // AFortPlayerPawnAthena, "Shield" is the MAX shield attribute and
    // "CurrentShield" is the current one - not the other way around.
    // Inverting these gives you a full shield bar that dies instantly.
    DEFINE_PROP(Shield, FFortGameplayAttributeData);
    DEFINE_PROP(CurrentShield, FFortGameplayAttributeData);
    // Newer builds (Ch1 S5+) use "Shield" for current and "MaxShield" for max,
    // instead of the early "Shield"(max)/"CurrentShield"(current) pair.
    DEFINE_PROP(MaxShield, FFortGameplayAttributeData);

    DEFINE_FUNC(OnRep_Health, void);
    DEFINE_FUNC(OnRep_MaxHealth, void);
    DEFINE_FUNC(OnRep_Shield, void);
    DEFINE_FUNC(OnRep_CurrentShield, void);
    DEFINE_FUNC(OnRep_MaxShield, void);
};

struct FDamagerInfo
{
public:
    AActor* DamageCauser;
    int32 DamageAmount;
    FGameplayTagContainer SourceTags;
};

class AFortPlayerPawnAthena : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortPlayerPawnAthena);

    DEFINE_PROP(CurrentWeapon, AActor*);
    DEFINE_PROP(PreviousWeapon, AActor*);
    DEFINE_PROP(Controller, AActor*);
    DEFINE_PROP(IncomingPickups, TArray<AActor*>);
    DEFINE_PROP(CharacterMovement, UCharacterMovementComponent*);
    DEFINE_PROP(ZiplineState, FZiplinePawnState);
    DEFINE_BITFIELD_PROP(bMovingEmote);
    DEFINE_PROP(EmoteWalkSpeed, float);
    DEFINE_BITFIELD_PROP(bMovingEmoteForwardOnly);
    DEFINE_BITFIELD_PROP(bMovingEmoteFollowingOnly);
    DEFINE_PROP(LastFallDistance, float);
    DEFINE_PROP(GameplayTags, FGameplayTagContainer);
    DEFINE_BITFIELD_PROP(bIsInAnyStorm);
    DEFINE_BITFIELD_PROP(bIsInsideSafeZone);
    DEFINE_PROP(AIControllerClass, TSubclassOf<AActor>);
    DEFINE_PROP(PlayerState, AActor*);
    DEFINE_PROP(BaseEyeHeight, float);
    // Native guided-missile control stores the character camera here so it can
    // be restored when possession returns from FortRemoteControlledPawnAthena.
    DEFINE_PROP(StoredControlRotation, FRotator);
    DEFINE_PROP(OnHeldObjectPickedUp, TMulticastInlineDelegate<void(AActor*)>);
    DEFINE_PROP(OnHeldObjectDropped, TMulticastInlineDelegate<void(AActor*)>);
    DEFINE_PROP(OnEnteredAircraft, TMulticastInlineDelegate<void()>);
    DEFINE_PROP(PickupSpeedMultiplier, float);
    DEFINE_PROP(HeldObject, TWeakObjectPtr<AActor>);
    DEFINE_PROP(RepActiveMovementModeExtension, void*);
    DEFINE_BITFIELD_PROP(bIsPlayingEmote);
    DEFINE_PROP(HealthSet, UFortHealthSet*);
    DEFINE_PROP(CurrentWeaponList, TArray<AActor*>);
    DEFINE_PROP(bShouldDropItemsOnDeath, bool);
    DEFINE_PROP(MoveSoundStimulusBroadcastInterval, uint16_t);
    DEFINE_PROP(Damagers, TArray<FDamagerInfo>);
    DEFINE_PROP(LastReplicatedEmoteExecuted, UObject*);
    DEFINE_PROP(Mesh, UActorComponent*);
    DEFINE_BITFIELD_PROP(bIsDBNO);
    DEFINE_BITFIELD_PROP(bWasDBNOOnDeath);
    DEFINE_BITFIELD_PROP(bIsHiddenForDeath);
    DEFINE_BITFIELD_PROP(bIsSkydiving);
    DEFINE_BITFIELD_PROP(bIsSkydivingFromBus);
    DEFINE_PROP(RegisteredMovementModeExtentionLogic, TMap<uint32, UObject*>);
    DEFINE_PROP(VehicleInputComponent, UObject*);

    // Used by the PlayerAI system (native player-bot driving).
    DEFINE_PROP(CurrentMovementStyle, uint8);
    DEFINE_PROP(RemoteViewPitch, uint8); // replicated aim pitch clients render
    DEFINE_FUNC(AddMovementInput, void);
    DEFINE_FUNC(Jump, void);
    DEFINE_FUNC(StopJumping, void);
    DEFINE_FUNC(Crouch, void);
    DEFINE_FUNC(UnCrouch, void);
    DEFINE_FUNC(PawnStartFire, void);
    DEFINE_FUNC(PawnStopFire, void);

    DEFINE_FUNC(BeginSkydiving, void);

    // ---------------------------------------------------------------------
    // Health / shield
    //
    // Early builds (roughly S1-S4) ship without the native SetShield and
    // SetMaxShield UFunctions. UObject::Call returns silently when the
    // UFunction is missing, so on those builds every shield write - god,
    // regen, lategame, bot shield, respawn, the cheat commands - did
    // nothing at all, with no crash and no log.
    //
    // So each setter probes for its native UFunction and writes the
    // attribute set directly when it isn't there. This is a capability
    // probe rather than a version number on purpose: builds nobody has
    // tested still degrade correctly, and the fallback can never engage on
    // a build where the native setter exists.
    //
    // The fallback needs all three of these or it looks broken:
    //   1. BaseValue AND CurrentValue - the ASC aggregator recomputes from
    //      whichever one you left stale and stomps the write.
    //   2. The server-side OnRep - runs the game's own attribute-changed
    //      broadcast and clamping that a raw memory write skips. Without
    //      it the server value is right but the client HUD never updates.
    //      CRUCIAL: call it with NO argument. It is a GAS repnotify that
    //      applies delta (NewBase - OldValue.Base) to the aggregated current
    //      value. A zeroed OldValue (what a no-arg call gives) makes the delta
    //      the full new value; passing the just-written attribute makes the
    //      delta zero, so current shield never leaves 0. This is the one line
    //      that separates "works like Core" from "reports success, does
    //      nothing" - the exact shield bug on Ch1 S5.
    //   3. ForceNetUpdate on the PAWN, not the attribute set - the set
    //      replicates as a subobject of the pawn's actor channel, so the
    //      pawn is what has to be dirtied.
    // ---------------------------------------------------------------------
    // A missing native setter AND a missing property means the write is
    // dropped, which is exactly the silent failure this whole path exists to
    // fix - so say so once instead of failing quietly a second way.
    static void WarnMissingAttributeOnce(bool& bWarned, const char* AttributeName)
    {
        if (bWarned)
            return;

        bWarned = true;
        SDK::DbgLog("  [HealthSet] no native setter and no '%s' property on this build - writes will not apply\n", AttributeName);
    }

    void SetHealth(float NewValue) const
    {
        static auto Fn = GetFunction("SetHealth");

        if (Fn)
        {
            Call<void>(Fn, NewValue);
            return;
        }

        auto Set = HealthSet;

        if (!Set || !Set->HasHealth())
        {
            static bool bWarned = false;
            WarnMissingAttributeOnce(bWarned, "Health");
            return;
        }

        auto& Attribute = Set->Health;
        Attribute.BaseValue = NewValue;
        Attribute.CurrentValue = NewValue;

        Set->OnRep_Health();
        // Re-apply after the OnRep recompute (see SetShield for why).
        Attribute.CurrentValue = NewValue;
        Attribute.BaseValue = NewValue;

        ForceNetUpdate();
    }

    void SetMaxHealth(float NewValue) const
    {
        static auto Fn = GetFunction("SetMaxHealth");

        if (Fn)
        {
            Call<void>(Fn, NewValue);
            return;
        }

        auto Set = HealthSet;

        if (!Set || !Set->HasMaxHealth())
        {
            static bool bWarned = false;
            WarnMissingAttributeOnce(bWarned, "MaxHealth");
            return;
        }

        auto& Attribute = Set->MaxHealth;
        Attribute.BaseValue = NewValue;
        Attribute.CurrentValue = NewValue;
        Attribute.Maximum = NewValue;

        Set->OnRep_MaxHealth();
        // Re-apply after the OnRep recompute (see SetShield for why).
        Attribute.BaseValue = NewValue;
        Attribute.CurrentValue = NewValue;
        Attribute.Maximum = NewValue;

        ForceNetUpdate();
    }

    void SetShield(float NewValue) const
    {
        // Shield is the one attribute a capability probe gets WRONG. Early builds
        // (<= 5.0) DO ship a SetShield UFunction, but it does not actually apply
        // current shield - so "the function exists, call it" left the bar at 0
        // even though it reported success. Core gates this purely by version for
        // this exact reason. So: native only above 5.0 (and only if present);
        // 5.0 and below always take the direct attribute write. SetMaxShield is a
        // separate, working function on those builds, which is why max shield
        // applied while current shield did not.
        static auto Fn = GetFunction("SetShield");

        if (Fn && VersionInfo.FortniteVersion > 5.0)
        {
            Call<void>(Fn, NewValue);
            return;
        }

        auto Set = HealthSet;

        if (!Set || !Set->HasCurrentShield())
        {
            static bool bWarned = false;
            WarnMissingAttributeOnce(bWarned, "CurrentShield");
            return;
        }

        // When the GE path is used it adds a flat +1, so positive values are
        // pre-subtracted by 1. An exact clear must skip that GE entirely;
        // applying it after a zero write is what leaves 1 shield on the HUD.
        const bool bUsesShieldGE = ShieldAbsorbUsesGE();
        const bool bClearingShield = NewValue <= 0.f;
        float Target = bClearingShield ? 0.f :
            (bUsesShieldGE ? (NewValue - 1.f) : NewValue);
        if (Target < 0.f)
            Target = 0.f;

        auto& Attribute = Set->CurrentShield;
        Attribute.BaseValue = Target;
        Attribute.CurrentValue = Target;

        // OnRep is what makes damage read the shield on the earliest builds
        // (1.7.2) - exactly Core's path. On S4+ the same OnRep recomputes current
        // from a GAS aggregator seeded at 0 and wipes it, so we re-write after.
        Set->OnRep_CurrentShield();
        Attribute.CurrentValue = Target;
        Attribute.BaseValue = Target;

        // The raw write only ABSORBS on the earliest builds. By S4 the shield
        // lives in the ability-system aggregator that native damage reads, and a
        // struct write never reaches it. ActivateShieldAbsorb applies the game's
        // own shield GE (which does reach the aggregator) - it syncs the
        // aggregator from the value we wrote and adds its flat +1, landing on
        // NewValue. No-op on 1.7.2. Defined in the .cpp.
        if (bUsesShieldGE && !bClearingShield)
            ActivateShieldAbsorb();

        ForceNetUpdate();
    }

    // Applies the shield GE so the written value absorbs on S4+ (see .cpp).
    void ActivateShieldAbsorb() const;
    // Whether SetShield will route through the GE (which adds a flat +1).
    bool ShieldAbsorbUsesGE() const;

    void SetMaxShield(float NewValue) const
    {
        // Same story as SetShield but the native SetMaxShield came back a little
        // earlier, so Core's threshold is 3.0 rather than 5.0.
        static auto Fn = GetFunction("SetMaxShield");

        if (Fn && VersionInfo.FortniteVersion > 3.0)
        {
            Call<void>(Fn, NewValue);
            return;
        }

        auto Set = HealthSet;

        if (!Set || !Set->HasShield())
        {
            static bool bWarned = false;
            WarnMissingAttributeOnce(bWarned, "Shield");
            return;
        }

        // "Shield" is the max-shield attribute on these builds.
        auto& Attribute = Set->Shield;
        Attribute.BaseValue = NewValue;
        Attribute.CurrentValue = NewValue;
        Attribute.Maximum = NewValue;

        Set->OnRep_Shield();
        // Re-apply after the OnRep recompute (see SetShield for why).
        Attribute.BaseValue = NewValue;
        Attribute.CurrentValue = NewValue;
        Attribute.Maximum = NewValue;

        ForceNetUpdate();
    }

    float GetHealth() const
    {
        static auto Fn = GetFunction("GetHealth");

        if (Fn)
            return Call<float>(Fn);

        auto Set = HealthSet;
        return (Set && Set->HasHealth()) ? Set->Health.CurrentValue : 0.f;
    }

    float GetMaxHealth() const
    {
        static auto Fn = GetFunction("GetMaxHealth");

        if (Fn)
            return Call<float>(Fn);

        auto Set = HealthSet;
        return (Set && Set->HasMaxHealth()) ? Set->MaxHealth.CurrentValue : 0.f;
    }

    float GetShield() const
    {
        static auto Fn = GetFunction("GetShield");

        if (Fn)
            return Call<float>(Fn);

        auto Set = HealthSet;
        return (Set && Set->HasCurrentShield()) ? Set->CurrentShield.CurrentValue : 0.f;
    }

    float GetMaxShield() const
    {
        static auto Fn = GetFunction("GetMaxShield");

        if (Fn)
            return Call<float>(Fn);

        auto Set = HealthSet;
        return (Set && Set->HasShield()) ? Set->Shield.CurrentValue : 0.f;
    }
    DEFINE_FUNC(EquipWeaponDefinition, AActor*);
    DEFINE_FUNC(LaunchCharacterJump, void);
    DEFINE_FUNC(OnCapsuleBeginOverlap, void);
    DEFINE_FUNC(ServerHandlePickup, void);
    DEFINE_FUNC(IsDBNO, bool);
    // These are reflected BoolProperty fields on legacy Athena builds (not
    // callable functions). Treating them as UFunctions leaves a respawned pawn
    // authoritatively in its death state.
    DEFINE_BITFIELD_PROP(bIsDying);
    DEFINE_BITFIELD_PROP(bPlayedDying);
    DEFINE_FUNC(PickUpActor, void);
    DEFINE_FUNC(OnRep_IsInAnyStorm, void);
    DEFINE_FUNC(OnRep_IsInsideSafeZone, void);
    DEFINE_FUNC(OnRep_PlayerState, void);
    DEFINE_FUNC(OnRep_IsDBNO, void);
    DEFINE_FUNC(ServerSetAttachment, void);
    DEFINE_FUNC(GetActiveZipline, AFortAscenderZipline*);
    DEFINE_FUNC(ServerOnExitVehicle, void);
    DEFINE_FUNC(SetInVortex, void);
    DEFINE_FUNC(ClientInternalEquipWeapon, void);
    DEFINE_FUNC(ServerInternalEquipWeapon, void);
    DEFINE_FUNC(SetGravityMultiplier, void);
    DEFINE_FUNC(SetActorEnableCollision, void);
    DEFINE_FUNC(OnRep_LastReplicatedEmoteExecuted, void);
    DEFINE_FUNC(EmoteStopped, void);
    DEFINE_FUNC(ServerChoosePart, void);
    DEFINE_FUNC(ServerThrowCarriedPlayer, void);
    DEFINE_FUNC(LocalThrowCarriedPlayer, void);
    DEFINE_FUNC(ServerInterrogateDBNOPlayer, void);
    DEFINE_FUNC(GetVehicleActor, AActor*);

    DefUHookOg(ServerHandlePickup_);
    DefUHookOg(ServerHandlePickupInfo);
    DefHookOg(bool, FinishedTargetSpline, void*);
    DefUHookOg(ServerSendZiplineState);
    DefUHookOg(OnCapsuleBeginOverlap_);
    static void MovingEmoteStopped(UObject*, FFrame&);
    static void ServerNotifyPawnHit(UObject* Context, FFrame& Stack);
    DefUHookOg(Athena_MedConsumable_Triggered);
    DefUHookOg(ServerOnExitVehicle_);
    DefUHookOg(EmoteStopped_);
    static void ServerHandlePickupWithRequestedSwap(UObject*, FFrame&);
    DefHookOg(void, EndSkydiving, AFortPlayerPawnAthena*);
    DefUHookOg(ServerReviveFromDBNO_);
    DefUHookOg(ServerThrowCarriedPlayer_);
    DefUHookOg(ServerInterrogateDBNOPlayer_);

    InitPostLoadHooks;
};
