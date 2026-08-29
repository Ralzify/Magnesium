#pragma once
#include "../../pch.h"
#include "GameplayTagContainer.h"
#include "../../Engine/Public/CurveTable.h"

class AFortPlayerControllerAthena;
class UFortItemDefinition;

enum class ETryExitVehicleBehavior : uint8
{
    DoNotForce = 0, ForceOnBlocking = 1, ForceAlways = 2
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

    DEFINE_PROP(Shield, FFortGameplayAttributeData);
    DEFINE_PROP(CurrentShield, FFortGameplayAttributeData);
    // Ch1 S5+ uses "Shield"/"MaxShield"; earlier builds use "Shield" for max and "CurrentShield" for current.
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
    DEFINE_PROP(LastDamagedTime, float);
    DEFINE_PROP(LastReplicatedEmoteExecuted, UObject*);
    DEFINE_PROP(Mesh, UActorComponent*);
    DEFINE_BITFIELD_PROP(bIsDBNO);
    DEFINE_BITFIELD_PROP(bWasDBNOOnDeath);
    DEFINE_BITFIELD_PROP(bIsHiddenForDeath);
    DEFINE_BITFIELD_PROP(bIsSkydiving);
    DEFINE_BITFIELD_PROP(bIsSkydivingFromBus);
    DEFINE_PROP(GliderRedeployAllowedRow, FScalableFloat);
    DEFINE_PROP(GliderRedeployLateralVelocityMultiplierRow, FScalableFloat);
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

    // Roughly S1-S4 ship without native SetShield/SetMaxShield, and UObject::Call returns silently when one is missing.
    static void WarnMissingAttributeOnce(bool& bWarned, const char* AttributeName)
    {
        if (bWarned)
            return;

        bWarned = true;
        SDK::DbgLog("  [HealthSet] no native setter and no '%s' property on this build - writes will not apply\n", AttributeName);
    }

    static void WriteDirectAttributeValue(FFortGameplayAttributeData& Attribute, float NewValue)
    {
        Attribute.BaseValue = NewValue;
        Attribute.CurrentValue = NewValue;

        if (FFortGameplayAttributeData::HasUnclampedBaseValue())
            Attribute.UnclampedBaseValue = NewValue;
        if (FFortGameplayAttributeData::HasUnclampedCurrentValue())
            Attribute.UnclampedCurrentValue = NewValue;
    }

    static std::vector<uint8_t> SnapshotDirectAttribute(const FFortGameplayAttributeData& Attribute)
    {
        const uint32 AttributeSize = FFortGameplayAttributeData::Size();
        if (AttributeSize == 0 || AttributeSize > 0x400)
            return {};

        std::vector<uint8_t> Snapshot(AttributeSize);
        memcpy(Snapshot.data(), &Attribute, AttributeSize);
        return Snapshot;
    }

    // Never use UObject::Call's one-argument fast path here: FFortGameplayAttributeData has a runtime-reflected size.
    static bool NotifyDirectAttributeRep(UFortHealthSet* Set, const char* FunctionName,
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
                SDK::DbgLog("  [HealthSet] refused parameterless dispatch for %s "
                    "with nonzero buffer size=%u\n", FunctionName, Parameters.Size);
                return false;
            }

            // FN 1.7.2 and 2.50 expose genuine zero-parameter repnotifies, and text dumps omit ParmsSize.
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
            const auto& Parameter = Parameters.NameOffsetMap[Index];
            if (!(Parameter.PropertyFlags & CPF_Parm) || (Parameter.PropertyFlags & CPF_ReturnParm))
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
        if (InputParameterCount != 1 || OldValueParameterIndex < 0 || PreviousValue.empty() ||
            BufferSize == 0 || BufferSize > 0x1000 ||
            Parameters.NameOffsetMap[OldValueParameterIndex].Offset > BufferSize ||
            PreviousValue.size() > BufferSize - Parameters.NameOffsetMap[
                    OldValueParameterIndex].Offset)
        {
            static UFunction* WarnedFunction = nullptr;
            if (WarnedFunction != Function)
            {
                WarnedFunction = Function;
                SDK::DbgLog("  [HealthSet] could not marshal real OldValue for %s; "
                    "notification skipped\n", FunctionName);
            }
            return false;
        }

        void* Memory = FMemory::Malloc(BufferSize);
        if (!Memory)
            return false;
        memset(Memory, 0, BufferSize);
        memcpy((PBYTE)Memory + Parameters.NameOffsetMap[OldValueParameterIndex].Offset,
            PreviousValue.data(), PreviousValue.size());
        Set->ProcessEvent(Function, Memory);
        FMemory::Free(Memory);
        return true;
    }

    bool ApplyLegacyShieldAggregatorOverride(float NewMaxShield, float NewShield) const;

    bool SetLegacyRespawnAttributesExact(float NewMaxHealth, float NewHealth, float NewMaxShield,
        float NewShield) const
    {
        auto Set = HasHealthSet() ? HealthSet : nullptr;
        if (!Set)
            return false;

        auto NativeSetMaxHealth = GetFunction("SetMaxHealth");
        auto NativeSetHealth = GetFunction("SetHealth");
        if (!NativeSetMaxHealth || !NativeSetHealth)
        {
            SDK::DbgLog("  [HealthSet] legacy respawn refused without native "
                "health setters pawn=%p maxFn=%p healthFn=%p FN=%.2f\n",
                (void*)this, (void*)NativeSetMaxHealth, (void*)NativeSetHealth,
                VersionInfo.FortniteVersion);
            return false;
        }

        Call<void>(NativeSetMaxHealth, NewMaxHealth);
        Call<void>(NativeSetHealth, NewHealth);

        if (VersionInfo.FortniteVersion <= 2.50)
        {
            const bool bApplied = ApplyLegacyShieldAggregatorOverride(NewMaxShield, NewShield);
            ForceNetUpdate();
            return bApplied;
        }

        const float PreviousLiveMaxShield = GetMaxShield();
        const float PreviousLiveShield = GetShield();

        auto ApplyShieldExact = [Set](FFortGameplayAttributeData& Attribute, const char* OnRepName,
            float NewValue, float PreviousLiveValue, bool bSetMaximum)
            {
                const float PreviousCurrentValue = PreviousLiveValue;
                const auto PreviousValue = SnapshotDirectAttribute(Attribute);
                WriteDirectAttributeValue(Attribute, NewValue);
                if (bSetMaximum && FFortGameplayAttributeData::HasMaximum())
                {
                    Attribute.Maximum = NewValue;
                }

                const bool bNotificationSafe = FPlatformMath::IsFinite(PreviousCurrentValue) &&
                    FPlatformMath::IsFinite(NewValue) && NewValue >= 0.f && NewValue <= 10000.f &&
                    NotifyDirectAttributeRep(Set, OnRepName, PreviousValue);
                return bNotificationSafe;
            };

        bool bShieldSynchronized = true;
        bool bCurrentShieldAppliedBeforeCapacity = false;
        if (Set->HasShield() && Set->HasCurrentShield() &&
            FPlatformMath::IsFinite(PreviousLiveShield) && PreviousLiveShield > NewMaxShield &&
            NewMaxShield >= 0.f)
        {
            const float PreCapacityShield = (std::min)(NewShield, NewMaxShield);
            const bool bPreCapacityClampSynchronized = ApplyShieldExact(
                Set->CurrentShield, "OnRep_CurrentShield",
                PreCapacityShield, PreviousLiveShield, false);
            bShieldSynchronized &= bPreCapacityClampSynchronized;
            if (!bPreCapacityClampSynchronized)
            {
                ForceNetUpdate();
                return false;
            }
            bCurrentShieldAppliedBeforeCapacity = true;
        }
        if (Set->HasShield())
        {
            bShieldSynchronized &= ApplyShieldExact(Set->Shield, "OnRep_Shield",
                NewMaxShield, PreviousLiveMaxShield, true);
        }
        if (Set->HasCurrentShield() && !bCurrentShieldAppliedBeforeCapacity)
        {
            bShieldSynchronized &= ApplyShieldExact(Set->CurrentShield, "OnRep_CurrentShield",
                NewShield, PreviousLiveShield, false);
        }
        ForceNetUpdate();
        return bShieldSynchronized;
    }

    void SetHealth(float NewValue) const
    {
        auto Set = HasHealthSet() ? HealthSet : nullptr;
        if (HasMinimumHealthGodMode(this) && Set && Set->HasHealth() &&
            FFortGameplayAttributeData::StaticStruct() && FFortGameplayAttributeData::HasMinimum())
        {
            const float Minimum = Set->Health.Minimum;
            if (FPlatformMath::IsFinite(NewValue) && FPlatformMath::IsFinite(Minimum) &&
                Minimum > 0.f && NewValue < Minimum)
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

        NotifyDirectAttributeRep(Set, "OnRep_Health", PreviousValue);
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

        NotifyDirectAttributeRep(Set, "OnRep_MaxHealth", PreviousValue);
        WriteDirectAttributeValue(Attribute, NewValue);
        Attribute.Maximum = NewValue;

        ForceNetUpdate();
    }

    void SetShield(float NewValue) const
    {
        if (!FPlatformMath::IsFinite(NewValue) || NewValue < 0.f)
            NewValue = 0.f;

        // Builds <= 5.0 do ship SetShield, but it never applies current shield - so gate by version, not by capability.
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

        const bool bUsesShieldGE = ShieldAbsorbUsesGE();
        const bool bClearingShield = NewValue <= 0.f;
        float Target = bClearingShield ? 0.f : (bUsesShieldGE ? (NewValue - 1.f) : NewValue);
        if (Target < 0.f)
            Target = 0.f;

        auto& Attribute = Set->CurrentShield;
        const auto PreviousValue = SnapshotDirectAttribute(Attribute);
        WriteDirectAttributeValue(Attribute, Target);

        // On S4+ this OnRep recomputes current from a GAS aggregator seeded at 0, so re-write afterwards.
        NotifyDirectAttributeRep(Set, "OnRep_CurrentShield", PreviousValue);
        WriteDirectAttributeValue(Attribute, Target);

        // By S4 the shield lives in the ability-system aggregator native damage reads, which a struct write never reaches.
        if (bUsesShieldGE && !bClearingShield)
            ActivateShieldAbsorb();

        ForceNetUpdate();
    }

    // Applies the shield GE so the written value absorbs on S4+ (see .cpp).
    void ActivateShieldAbsorb() const;
    bool ShieldAbsorbUsesGE() const;

    void SetMaxShield(float NewValue) const
    {
        // Same as SetShield, except native SetMaxShield came back earlier, so the threshold is 3.0.
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

        auto& Attribute = Set->Shield;
        const auto PreviousValue = SnapshotDirectAttribute(Attribute);
        WriteDirectAttributeValue(Attribute, NewValue);
        Attribute.Maximum = NewValue;

        NotifyDirectAttributeRep(Set, "OnRep_Shield", PreviousValue);
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

    // A reactive item names what it watches in ObservedPlayerStats - "StatManager.AthenaKills" for the Candy Axe - and reacts client-side.
    bool SetClientObservedStat(FName StatName, int32 StatValue) const;
    int32 GetClientObservedStat(FName StatName, int32 DefaultValue = -1) const;

    DEFINE_FUNC(EquipWeaponDefinition, AActor*);
    DEFINE_FUNC(LaunchCharacterJump, void);
    DEFINE_FUNC(OnCapsuleBeginOverlap, void);
    DEFINE_FUNC(ServerHandlePickup, void);
    DEFINE_FUNC(IsDBNO, bool);
    // These are reflected BoolProperty fields on legacy Athena builds, not callable functions.
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

    static bool ReviveFromDBNOCompat(AFortPlayerPawnAthena* Pawn, AController* EventInstigator,
        bool bNativeTransitionAlreadyAttempted = false);

    static bool EnsurePlayerMapIcon(AFortPlayerControllerAthena* Controller,
        AFortPlayerPawnAthena* Pawn, const UObject* PreferredCharacterDefinition = nullptr);

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

    static void TickHealthStateRepair(UNetDriver* Driver);

    static bool PlayCommandGrantPickupAnimation(AFortPlayerPawnAthena* Pawn,
        const UFortItemDefinition* ItemDefinition, int32 Count, int32 LoadedAmmo, int32 Level);

    static bool SetMinimumHealthGodMode(AFortPlayerControllerAthena* Controller, bool bEnabled);
    static bool HasMinimumHealthGodMode(const AFortPlayerControllerAthena* Controller);
    static bool HasMinimumHealthGodMode(const AFortPlayerPawnAthena* Pawn);
    static bool SetFullHealthGodMode(AFortPlayerControllerAthena* Controller,
        AFortPlayerPawnAthena* Pawn, bool bEnabled);
    static bool HasFullHealthGodMode(const AFortPlayerControllerAthena* Controller);
    static bool HasFullHealthGodMode(const AFortPlayerPawnAthena* Pawn);
    static bool DisableGodModes(AFortPlayerControllerAthena* Controller,
        AFortPlayerPawnAthena* Pawn);

    InitPostLoadHooks;
};
