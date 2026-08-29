#pragma once
#include "../../pch.h"
#include "GameplayTagContainer.h"
#include "../../Engine/Public/CurveTable.h"

class AFortPlayerControllerAthena;
class UFortItemDefinition;

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
    DEFINE_FUNC(IsMovingOnGround, bool);
    DEFINE_FUNC(IsFalling, bool);
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

struct FFortClientObservedStat : public SDK::FFastArraySerializerItem
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FFortClientObservedStat);

    DEFINE_STRUCT_PROP(StatName, FName);
    DEFINE_STRUCT_PROP(StatValue, int32);
};

struct FFortClientObservedStatArray : public SDK::FFastArraySerializer
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FFortClientObservedStatArray);

    DEFINE_STRUCT_PROP(ObservedStats, TArray<FFortClientObservedStat>);
    DEFINE_STRUCT_PROP(MyStatManager, UObject*);
};

class UNetDriver;
class AController;

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
    DEFINE_PROP(ClientObservedStats, FFortClientObservedStatArray);
    // Native damage timestamp inherited from FortPawn. The health-state
    // watchdog uses it to distinguish a lethal hit from scripted zero-health
    // possession/form transitions.
    DEFINE_PROP(LastDamagedTime, float);
    DEFINE_PROP(LastReplicatedEmoteExecuted, UObject*);
    DEFINE_PROP(Mesh, UActorComponent*);
    DEFINE_BITFIELD_PROP(bIsDBNO);
    DEFINE_BITFIELD_PROP(bWasDBNOOnDeath);
    DEFINE_BITFIELD_PROP(bIsHiddenForDeath);
    DEFINE_BITFIELD_PROP(bIsSkydiving);
    DEFINE_BITFIELD_PROP(bIsSkydivingFromBus);
    DEFINE_PROP(GliderRedeployAllowedRow, FScalableFloat);
    DEFINE_PROP(
        GliderRedeployLateralVelocityMultiplierRow, FScalableFloat);
    DEFINE_PROP(GliderRedeployHeighLimitRow, FScalableFloat);
    DEFINE_PROP(RegisteredMovementModeExtentionLogic, TMap<uint32, UObject*>);
    DEFINE_PROP(VehicleInputComponent, UObject*);

    // Server-driven player-bot movement.
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
    //      GAS repnotifies apply (NewBase - OldValue.Base) to the live
    //      aggregator. The old value must therefore be captured BEFORE the
    //      direct write. Supplying a zero old value works for the first spawn,
    //      but adds the requested amount again when a persistent PlayerState
    //      ASC is rebound during respawn (for example 32 + 100 = 132 health).
    //      Supplying the already-written value produces a zero delta. The real
    //      pre-write snapshot handles both cases exactly.
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

    // Direct setters must keep the raw/unclamped GAS values coherent too.
    // Otherwise a later aggregator recompute can restore the stale value.
    static void WriteDirectAttributeValue(FFortGameplayAttributeData& Attribute, float NewValue)
    {
        Attribute.BaseValue = NewValue;
        Attribute.CurrentValue = NewValue;

        if (FFortGameplayAttributeData::HasUnclampedBaseValue())
            Attribute.UnclampedBaseValue = NewValue;
        if (FFortGameplayAttributeData::HasUnclampedCurrentValue())
            Attribute.UnclampedCurrentValue = NewValue;
    }

    static std::vector<uint8_t> SnapshotDirectAttribute(
        const FFortGameplayAttributeData& Attribute)
    {
        const uint32 AttributeSize =
            FFortGameplayAttributeData::Size();
        if (AttributeSize == 0 || AttributeSize > 0x400)
            return {};

        std::vector<uint8_t> Snapshot(AttributeSize);
        memcpy(Snapshot.data(), &Attribute, AttributeSize);
        return Snapshot;
    }

    // Marshal the reflected OldValue parameter when this build exposes it.
    // Some very early repnotifies have no parameter at all; those retain their
    // native no-argument path. Never use UObject::Call's one-argument fast path
    // here because FFortGameplayAttributeData has a runtime-reflected size.
    static bool NotifyDirectAttributeRep(
        UFortHealthSet* Set,
        const char* FunctionName,
        const std::vector<uint8_t>& PreviousValue)
    {
        if (!Set || !FunctionName)
            return false;

        auto Function = Set->GetFunction(FunctionName);
        if (!Function)
            return false;

        const auto Parameters = Function->GetParams();
        if (Parameters.NameOffsetMap.empty())
        {
            if (Parameters.Size != 0)
            {
                SDK::DbgLog(
                    "  [HealthSet] refused parameterless dispatch for %s "
                    "with nonzero buffer size=%u\n",
                    FunctionName, Parameters.Size);
                return false;
            }

            // FN 1.7.2/2.50 expose genuine zero-parameter repnotifies. Their
            // known-good Core path writes the nonnegative absolute attribute
            // first and invokes the no-argument bridge. Runtime reflection
            // still has to validate ParmsSize because text dumps omit it.
            Set->ProcessEvent(Function, nullptr);
            return true;
        }

        constexpr uint64 CPF_Parm = 0x0000000000000080;
        constexpr uint64 CPF_OutParm = 0x0000000000000100;
        constexpr uint64 CPF_ReturnParm = 0x0000000000000400;
        int32 OldValueParameterIndex = -1;
        int32 InputParameterCount = 0;
        for (int32 Index = 0;
            Index < Parameters.NameOffsetMap.size(); ++Index)
        {
            const auto& Parameter =
                Parameters.NameOffsetMap[Index];
            if (!(Parameter.PropertyFlags & CPF_Parm) ||
                (Parameter.PropertyFlags & CPF_ReturnParm))
            {
                continue;
            }

            ++InputParameterCount;
            if ((Parameter.PropertyFlags & CPF_OutParm) ||
                Parameter.ElementSize != PreviousValue.size())
                continue;

            if (OldValueParameterIndex >= 0)
            {
                OldValueParameterIndex = -1;
                break;
            }
            OldValueParameterIndex = Index;
        }

        const uint32 BufferSize = Parameters.Size;
        if (InputParameterCount != 1 ||
            OldValueParameterIndex < 0 || PreviousValue.empty() ||
            BufferSize == 0 || BufferSize > 0x1000 ||
            Parameters.NameOffsetMap[OldValueParameterIndex].Offset >
                BufferSize ||
            PreviousValue.size() >
                BufferSize - Parameters.NameOffsetMap[
                    OldValueParameterIndex].Offset)
        {
            // A nonzero unknown parameter layout cannot safely be invoked with
            // nullptr. Log and fail closed instead of asking ProcessEvent to
            // dereference a missing parameter buffer.
            static UFunction* WarnedFunction = nullptr;
            if (WarnedFunction != Function)
            {
                WarnedFunction = Function;
                SDK::DbgLog(
                    "  [HealthSet] could not marshal real OldValue for %s; "
                    "notification skipped\n",
                    FunctionName);
            }
            return false;
        }

        void* Memory = FMemory::Malloc(BufferSize);
        if (!Memory)
            return false;
        memset(Memory, 0, BufferSize);
        memcpy(
            (PBYTE)Memory + Parameters.NameOffsetMap[
                OldValueParameterIndex].Offset,
            PreviousValue.data(), PreviousValue.size());
        Set->ProcessEvent(Function, Memory);
        FMemory::Free(Memory);
        return true;
    }

    // Uses the early GAS container's native Override operation so the live
    // damage aggregator and the replicated HealthSet agree after respawn.
    // Implemented in the .cpp and capability-gated to the Season 1/2 family.
    bool ApplyLegacyShieldAggregatorOverride(
        float NewMaxShield,
        float NewShield) const;

    // Pre-Season-3 RestartPlayer reuses a PlayerState-owned ASC. Use the
    // pawn's native health setters plus the ASC container's native attribute
    // override for shield; raw HealthSet values alone can look full while the
    // damage aggregator still contains zero.
    bool SetLegacyRespawnAttributesExact(
        float NewMaxHealth,
        float NewHealth,
        float NewMaxShield,
        float NewShield) const
    {
        auto Set = HasHealthSet() ? HealthSet : nullptr;
        if (!Set)
            return false;

        auto NativeSetMaxHealth = GetFunction("SetMaxHealth");
        auto NativeSetHealth = GetFunction("SetHealth");
        if (!NativeSetMaxHealth || !NativeSetHealth)
        {
            SDK::DbgLog(
                "  [HealthSet] legacy respawn refused without native "
                "health setters pawn=%p maxFn=%p healthFn=%p FN=%.2f\n",
                (void*)this, (void*)NativeSetMaxHealth,
                (void*)NativeSetHealth,
                VersionInfo.FortniteVersion);
            return false;
        }

        Call<void>(NativeSetMaxHealth, NewMaxHealth);
        Call<void>(NativeSetHealth, NewHealth);

        // The early PlayerState-owned ASC requires its native aggregate
        // mutation. Applying raw values/OnRep first would run callbacks twice
        // and was the path that could produce a full-looking but inert bar.
        if (VersionInfo.FortniteVersion <= 2.50)
        {
            const bool bApplied =
                ApplyLegacyShieldAggregatorOverride(
                    NewMaxShield, NewShield);
            ForceNetUpdate();
            return bApplied;
        }

        const float PreviousLiveMaxShield = GetMaxShield();
        const float PreviousLiveShield = GetShield();

        auto ApplyShieldExact = [Set](
            FFortGameplayAttributeData& Attribute,
            const char* OnRepName,
            float NewValue,
            float PreviousLiveValue,
            bool bSetMaximum)
            {
                const float PreviousCurrentValue = PreviousLiveValue;
                const auto PreviousValue =
                    SnapshotDirectAttribute(Attribute);
                WriteDirectAttributeValue(Attribute, NewValue);
                if (bSetMaximum &&
                    FFortGameplayAttributeData::HasMaximum())
                {
                    Attribute.Maximum = NewValue;
                }

                const bool bNotificationSafe =
                    FPlatformMath::IsFinite(PreviousCurrentValue) &&
                    FPlatformMath::IsFinite(NewValue) &&
                    NewValue >= 0.f && NewValue <= 10000.f &&
                    NotifyDirectAttributeRep(
                        Set, OnRepName, PreviousValue);
                // Do not repaint the raw struct after OnRep. On the earliest
                // GAS builds that callback synchronizes (and may recompute)
                // the authoritative aggregator used by damage. Rewriting the
                // display-facing values here can make verification report 100
                // shield while the aggregator correctly remained at zero.
                return bNotificationSafe;
            };

        bool bShieldSynchronized = true;
        // Legacy naming: Shield is capacity and CurrentShield is current. If
        // capacity is moving downward, first clamp current shield beneath that
        // new ceiling. This follows Core's absolute fallback and never exposes
        // a transient negative capacity to native listeners.
        bool bCurrentShieldAppliedBeforeCapacity = false;
        if (Set->HasShield() && Set->HasCurrentShield() &&
            FPlatformMath::IsFinite(PreviousLiveShield) &&
            PreviousLiveShield > NewMaxShield &&
            NewMaxShield >= 0.f)
        {
            const float PreCapacityShield =
                (std::min)(NewShield, NewMaxShield);
            const bool bPreCapacityClampSynchronized = ApplyShieldExact(
                Set->CurrentShield, "OnRep_CurrentShield",
                PreCapacityShield, PreviousLiveShield, false);
            bShieldSynchronized &= bPreCapacityClampSynchronized;
            if (!bPreCapacityClampSynchronized)
            {
                // The capacity listener may clamp/re-enter through the current
                // value. If its prerequisite notification could not be safely
                // marshalled, leave capacity unchanged and retry later rather
                // than exposing an invalid current-over-maximum transition.
                ForceNetUpdate();
                return false;
            }
            bCurrentShieldAppliedBeforeCapacity = true;
        }
        if (Set->HasShield())
        {
            bShieldSynchronized &= ApplyShieldExact(
                Set->Shield, "OnRep_Shield",
                NewMaxShield, PreviousLiveMaxShield, true);
        }
        if (Set->HasCurrentShield() &&
            !bCurrentShieldAppliedBeforeCapacity)
        {
            bShieldSynchronized &= ApplyShieldExact(
                Set->CurrentShield, "OnRep_CurrentShield",
                NewShield, PreviousLiveShield, false);
        }
        ForceNetUpdate();
        return bShieldSynchronized;
    }

    void SetHealth(float NewValue) const
    {
        // Clamp custom direct writers only while our explicit "god min"
        // policy is active. Some modern attribute layouts default Minimum to
        // 1 even while their native clamp-state bookkeeping is disabled.
        auto Set = HasHealthSet() ? HealthSet : nullptr;
        if (HasMinimumHealthGodMode(this) &&
            Set && Set->HasHealth() &&
            FFortGameplayAttributeData::StaticStruct() &&
            FFortGameplayAttributeData::HasMinimum())
        {
            const float Minimum = Set->Health.Minimum;
            if (FPlatformMath::IsFinite(NewValue) &&
                FPlatformMath::IsFinite(Minimum) &&
                Minimum > 0.f &&
                NewValue < Minimum)
            {
                NewValue = Minimum;
            }
        }

        static auto Fn = GetFunction("SetHealth");

        if (Fn)
        {
            Call<void>(Fn, NewValue);
            return;
        }

        if (!Set || !Set->HasHealth())
        {
            static bool bWarned = false;
            WarnMissingAttributeOnce(bWarned, "Health");
            return;
        }

        auto& Attribute = Set->Health;
        const auto PreviousValue = SnapshotDirectAttribute(Attribute);
        WriteDirectAttributeValue(Attribute, NewValue);

        NotifyDirectAttributeRep(
            Set, "OnRep_Health", PreviousValue);
        // Re-apply after the OnRep recompute (see SetShield for why).
        WriteDirectAttributeValue(Attribute, NewValue);

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
        const auto PreviousValue = SnapshotDirectAttribute(Attribute);
        WriteDirectAttributeValue(Attribute, NewValue);
        Attribute.Maximum = NewValue;

        NotifyDirectAttributeRep(
            Set, "OnRep_MaxHealth", PreviousValue);
        // Re-apply after the OnRep recompute (see SetShield for why).
        WriteDirectAttributeValue(Attribute, NewValue);
        Attribute.Maximum = NewValue;

        ForceNetUpdate();
    }

    void SetShield(float NewValue) const
    {
        // Shield is never a signed resource. Keep every server-authored write
        // inside the lower bound before either the native setter or the legacy
        // attribute fallback sees it. In particular, cosmetic gameplay effects
        // may be compensated by subtracting their grant from a zero-shield pawn.
        if (!FPlatformMath::IsFinite(NewValue) || NewValue < 0.f)
            NewValue = 0.f;

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
        const auto PreviousValue = SnapshotDirectAttribute(Attribute);
        WriteDirectAttributeValue(Attribute, Target);

        // OnRep is what makes damage read the shield on the earliest builds
        // (1.7.2) - exactly Core's path. On S4+ the same OnRep recomputes current
        // from a GAS aggregator seeded at 0 and wipes it, so we re-write after.
        NotifyDirectAttributeRep(
            Set, "OnRep_CurrentShield", PreviousValue);
        WriteDirectAttributeValue(Attribute, Target);

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
        const auto PreviousValue = SnapshotDirectAttribute(Attribute);
        WriteDirectAttributeValue(Attribute, NewValue);
        Attribute.Maximum = NewValue;

        NotifyDirectAttributeRep(
            Set, "OnRep_Shield", PreviousValue);
        // Re-apply after the OnRep recompute (see SetShield for why).
        WriteDirectAttributeValue(Attribute, NewValue);
        Attribute.Maximum = NewValue;

        ForceNetUpdate();
    }

    float GetHealth() const
    {
        static auto Fn = GetFunction("GetHealth");

        if (Fn)
            return Call<float>(Fn);

        auto Set = HasHealthSet() ? HealthSet : nullptr;
        return (Set && Set->HasHealth()) ? Set->Health.CurrentValue : 0.f;
    }

    float GetMaxHealth() const
    {
        static auto Fn = GetFunction("GetMaxHealth");

        if (Fn)
            return Call<float>(Fn);

        auto Set = HasHealthSet() ? HealthSet : nullptr;
        return (Set && Set->HasMaxHealth()) ? Set->MaxHealth.CurrentValue : 0.f;
    }

    float GetShield() const
    {
        static auto Fn = GetFunction("GetShield");

        if (Fn)
            return Call<float>(Fn);

        auto Set = HasHealthSet() ? HealthSet : nullptr;
        return (Set && Set->HasCurrentShield()) ? Set->CurrentShield.CurrentValue : 0.f;
    }

    float GetMaxShield() const
    {
        static auto Fn = GetFunction("GetMaxShield");

        if (Fn)
            return Call<float>(Fn);

        auto Set = HasHealthSet() ? HealthSet : nullptr;
        return (Set && Set->HasShield()) ? Set->Shield.CurrentValue : 0.f;
    }

    // ---------------------------------------------------------------------
    // Reactive cosmetics
    //
    // A reactive item names what it watches in its cosmetic definition's
    // ObservedPlayerStats - "StatManager.AthenaKills" for the Candy Axe -
    // and does the reacting entirely on the client. The only thing the
    // server owes it is this pawn's replicated mirror of those values:
    // ClientObservedStats, a fast array of {StatName, StatValue}.
    //
    // ServerModifyStat does not get there on its own. It updates the
    // controller's StatManager, which only forwards into this array for
    // stats the pawn registered during the native cosmetic setup a custom
    // server never runs, so the value lands somewhere the client never
    // reads. Writing the fast array is what actually lights the lights.
    //
    // Returns false when the build has no ClientObservedStats (every access
    // below is reflected, so the whole path is skipped rather than guessed).
    bool SetClientObservedStat(FName StatName, int32 StatValue) const;
    // Current replicated value, or DefaultValue when the stat is absent.
    int32 GetClientObservedStat(
        FName StatName, int32 DefaultValue = -1) const;

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
    DEFINE_FUNC(ServerOnExitVehicle, AActor*);
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

    // Performs the native same-pawn DBNO transition. Reviving is deliberately
    // separate from the death/respawn pipeline: starting a respawn here leaves
    // the revived pawn behind and creates a duplicate at the configured spawn.
    static bool ReviveFromDBNOCompat(
        AFortPlayerPawnAthena* Pawn,
        AController* EventInstigator,
        // Set only after a normal interaction already invoked the native
        // transition and a later game-thread verification still found DBNO.
        // This prevents the fallback from replaying the same native entry.
        bool bNativeTransitionAlreadyAttempted = false);

    // Adds and configures Fortnite's native replicated map component for a
    // possessed player pawn. The implementation probes reflected capabilities
    // so the same call works from the earliest Athena builds through UE5.
    static bool EnsurePlayerMapIcon(
        AFortPlayerControllerAthena* Controller,
        AFortPlayerPawnAthena* Pawn,
        const UObject* PreferredCharacterDefinition = nullptr);

    // Polls portrait loads queued by EnsurePlayerMapIcon and replaces the
    // immediate generic marker once the selected skin's texture is resident.
    // Must run on the game thread.
    static void TickPendingPlayerMapIcons();

    DefUHookOg(ServerHandlePickup_);
    DefUHookOg(ServerHandlePickupInfo);
    DefHookOg(bool, FinishedTargetSpline, void*);
    DefUHookOg(ServerSendZiplineState);
    DefUHookOg(OnCapsuleBeginOverlap_);
    static void MovingEmoteStopped(UObject*, FFrame&);
    static void ServerNotifyPawnHit(UObject* Context, FFrame& Stack);
    DefUHookOg(Athena_MedConsumable_Triggered);
    DefUHookOgRet(AActor*, ServerOnExitVehicle_);
    DefUHookOg(EmoteStopped_);
    DefUHookOg(ServerHandlePickupWithRequestedSwap);
    DefHookOg(void, EndSkydiving, AFortPlayerPawnAthena*);
    DefUHookOg(ServerReviveFromDBNO_);
    DefUHookOg(ServerThrowCarriedPlayer_);
    DefUHookOg(ServerInterrogateDBNOPlayer_);

    // Repairs native damage outcomes that leave a possessed pawn with an
    // invalid shield or at zero health without entering DBNO/death.
    static void TickHealthStateRepair(UNetDriver* Driver);

    static bool PlayCommandGrantPickupAnimation(
        AFortPlayerPawnAthena* Pawn,
        const UFortItemDefinition* ItemDefinition,
        int32 Count,
        int32 LoadedAmmo,
        int32 Level);

    // "god min" remains controller-scoped so it survives pawn replacement.
    // The active pawn still receives a real Health.Minimum of 1, allowing
    // shields and health to take damage normally without entering death.
    static bool SetMinimumHealthGodMode(
        AFortPlayerControllerAthena* Controller,
        bool bEnabled);
    static bool HasMinimumHealthGodMode(
        const AFortPlayerControllerAthena* Controller);
    static bool HasMinimumHealthGodMode(
        const AFortPlayerPawnAthena* Pawn);
    // Full God is ownership tracked as well: disabling it restores only the
    // exact flag/floor that Magnesium changed and leaves authored immunity
    // alone. Unlike minimum God, it remains attached to this pawn generation.
    static bool SetFullHealthGodMode(
        AFortPlayerControllerAthena* Controller,
        AFortPlayerPawnAthena* Pawn,
        bool bEnabled);
    static bool HasFullHealthGodMode(
        const AFortPlayerControllerAthena* Controller);
    // Observational overload used only when callers need to identify any
    // full-immunity shape, including policies not owned by Magnesium.
    static bool HasFullHealthGodMode(
        const AFortPlayerPawnAthena* Pawn);
    // Removes every God-mode mechanism Magnesium can apply. This is kept as
    // one operation so a minimum-health grant and a full-immunity grant can
    // never leave each other hidden behind a misleading "disabled" state.
    static bool DisableGodModes(
        AFortPlayerControllerAthena* Controller,
        AFortPlayerPawnAthena* Pawn);

    InitPostLoadHooks;
};
