#include "pch.h"
#include "../Public/FortWeapon.h"
#include "../Public/FortGameStateAthena.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortPlayerPawnAthena.h"
#include "../Public/FortPlayerStateAthena.h"
#include "../Public/BattleRoyaleGamePhaseLogic.h"
#include "../Public/FortGameMode.h"
#include "../../Erbium/Public/Configuration.h"
#include "../../Erbium/Public/GUI.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>

namespace
{
    constexpr uint32 kMaxNotifyParamsSize = 0x1000;
    constexpr uint32 kMaxHitResultSize = 0x200;
    constexpr uint32 kMaxEffectCallParamsSize = 0x400;
    constexpr uint32 kMaxFrameSnapshotSize = 0x100;
    constexpr uint32 kExpectedEffectContextSize = 0x18;
    constexpr uint32 kExpectedEffectContainerSize = 0xB8;
    constexpr uint32 kExpectedGetUnmodifiedDamageParamsSize =
        0x20;
    constexpr uint32 kExpectedDamageCueSharedSize = 0xA8;
    constexpr uint32 kExpectedDamageCueNonSharedSize = 0x10;
    constexpr int32 kExpectedEffectContainerCount = 5;
    constexpr int32 kMaxEffectsPerContainer = 8;
    constexpr int32 kMaxDamageEffects =
        kExpectedEffectContainerCount *
        kMaxEffectsPerContainer;
    constexpr double kMaxRecordedOriginErrorCm = 300.0;
    constexpr double kMaxLaunchOriginDriftCm = 250.0;
    constexpr double kImpactBoundsToleranceCm = 300.0;
    constexpr double kPlayerImpactHorizontalEnvelopeCm =
        150.0;
    constexpr double kPlayerImpactVerticalEnvelopeCm =
        225.0;
    constexpr double kMaxActorBoundsExtentCm = 100000.0;
    constexpr double kMaxReportedTravelCm = 500000.0;
    constexpr double kMinLaunchCorroborationDot = 0.95;
    constexpr double kMinProjectileAzimuthDot = 0.995;
    constexpr double kMinServerAimDot = 0.80;
    constexpr double kMinAdjustedServerAimDot = 0.50;
    // A projectile starts at the muzzle while the adjusted shot direction is
    // derived from the player's view. At contact range that camera/muzzle
    // baseline can be larger than the entire reported flight, so a fixed
    // angular cone is geometrically invalid. These corridor limits preserve
    // the old long-range angles while providing a bounded near-field offset.
    constexpr double kCompatibilityParallaxAllowanceCm = 100.0;
    // Vehicle-mod weapons are represented by an actor near the turret pivot,
    // while the client hit RPC starts at the moving muzzle. FN30's machine-gun
    // mod consistently places those two authoritative points more than 100 cm
    // apart. Keep the larger corridor exclusive to a server-validated mounted
    // seat and bounded by the existing launch-origin drift limit.
    constexpr double kMountedWeaponParallaxAllowanceCm =
        kMaxLaunchOriginDriftCm;
    constexpr double kMaxMountedTraceStartHostDistanceCm =
        1000.0;
    constexpr double kCompatibilityRearwardSlackCm = 50.0;
    constexpr double kCompatibilityContactRangeCm = 200.0;
    constexpr double kCompatibilityContactRearwardSlackCm = 125.0;
    constexpr double kAuthoritativeTraceBacktrackCm = 60.0;
    constexpr double kAuthoritativeTraceForwardCm = 30.0;
    constexpr double kMaxAuthoritativeImpactErrorCm = 50.0;
    constexpr double kMaxAuthoritativePhysicsPointErrorCm =
        35.0;
    constexpr double kProjectileAzimuthSlope =
        0.100376805806222; // tan(acos(0.995))
    constexpr double kProjectileUpwardSlope =
        0.087488663525924; // tan(5 degrees)
    constexpr double kProjectileDownwardSlope =
        1.732050807568877; // tan(60 degrees)
    constexpr double kMaxUpwardPitchDeviationRadians =
        5.0 * 3.14159265358979323846 / 180.0;
    constexpr double kMaxDownwardPitchDeviationRadians =
        60.0 * 3.14159265358979323846 / 180.0;
    constexpr double kMaxConservativeProjectileSpeedCmPerSecond =
        300000.0;
    constexpr double kMaxPlausibleShooterSpeedCmPerSecond =
        3000.0;
    constexpr double kMaxCurrentAimFallbackAgeSeconds =
        0.125;
    constexpr double kMaxCachedLaunchGeometryAgeSeconds =
        0.25;
    constexpr double kProjectileFlightTimeGraceSeconds = 0.25;
    constexpr double kAutomaticTimestampGraceSeconds = 0.10;
    constexpr float kMaxProjectileAgeSeconds = 15.f;
    constexpr float kMaxFutureProjectileTimestampSeconds = 2.f;
    constexpr float kMaxAutomaticStreamSeconds = 60.f;
    constexpr float kMaxReportedTimestampAgeSeconds =
        kMaxProjectileAgeSeconds +
        kMaxFutureProjectileTimestampSeconds;
    constexpr float kReplayTombstoneSeconds =
        kMaxReportedTimestampAgeSeconds +
        kMaxFutureProjectileTimestampSeconds;
    constexpr float kShotReplayRetentionSeconds =
        kMaxAutomaticStreamSeconds +
        kReplayTombstoneSeconds;
    constexpr float kProjectileTimestampToleranceSeconds =
        0.01f;
    constexpr float kHitRateWindowSeconds = 1.f;
    constexpr float kCompatibilityFutureFireTimeToleranceSeconds =
        0.05f;
    constexpr float kProjectileVisualRelayFreshnessSeconds =
        0.50f;
    constexpr float kProjectileVisualRelayPendingSeconds =
        3.0f;
    constexpr float kProjectileGeometryWaitSeconds =
        0.05f;
    constexpr float kNativeFireTokenBindWindowSeconds =
        0.025f;
    constexpr float kDamageFeedbackEpsilon = 0.01f;
    constexpr float kMaxDamageFeedbackMagnitude = 1000000.f;

    struct FFieldView
    {
        int32 Offset = -1;
        uint32 Size = 0;

        bool IsValid(uint32 ContainerSize) const
        {
            return Offset >= 0 &&
                Size > 0 &&
                static_cast<uint64>(Offset) + Size <= ContainerSize;
        }
    };

    struct FNotifyPawnHitSchema
    {
        UFunction* Function = nullptr;
        uint32 ParamsSize = 0;
        FFieldView Hit;
        FFieldView ProjectileOrigin;
        FFieldView ProjectileTimestamp;

        bool IsValid() const
        {
            return Function &&
                ParamsSize > 0 &&
                ParamsSize <= kMaxNotifyParamsSize &&
                Hit.IsValid(ParamsSize) &&
                ProjectileOrigin.IsValid(ParamsSize);
        }
    };

    struct FHitResultSchema
    {
        const UStruct* Struct = nullptr;
        uint32 Size = 0;
        FFieldView Component;
        FFieldView Actor;
        FFieldView HitObjectHandle;
        FFieldView PhysicalMaterial;
        FFieldView BoneName;
        FFieldView MyBoneName;
        FFieldView FaceIndex;
        FFieldView Time;
        FFieldView Distance;
        FFieldView ImpactPoint;
        FFieldView Location;
        FFieldView ImpactNormal;
        FFieldView Normal;
        FFieldView TraceStart;
        FFieldView TraceEnd;
        FFieldView PenetrationDepth;
        FFieldView MyItem;
        FFieldView Item;
        FFieldView ElementIndex;
        FFieldView BlockingHit;

        const UStruct* ActorInstanceHandleStruct = nullptr;
        uint32 ActorInstanceHandleSize = 0;
        FFieldView HandleReferenceObject;

        bool IsValid() const
        {
            return Struct &&
                Size > 0 &&
                Size <= kMaxHitResultSize &&
                (Component.IsValid(Size) ||
                    Actor.IsValid(Size) ||
                    (HitObjectHandle.IsValid(Size) &&
                        ActorInstanceHandleStruct &&
                        ActorInstanceHandleSize ==
                            HitObjectHandle.Size &&
                        HandleReferenceObject.IsValid(
                            ActorInstanceHandleSize))) &&
                (ImpactPoint.IsValid(Size) || Location.IsValid(Size));
        }
    };

    struct FProjectileRequestSchema
    {
        UFunction* Function = nullptr;
        uint32 ParamsSize = 0;
        FFieldView Request;

        const UStruct* RequestStruct = nullptr;
        uint32 RequestSize = 0;
        FFieldView StartPosition;
        FFieldView StartDirection;
        FFieldView Timestamp;

        bool IsValid() const
        {
            return Function &&
                ParamsSize > 0 &&
                ParamsSize <= 0x100 &&
                RequestStruct &&
                RequestSize > 0 &&
                RequestSize <= 0x100 &&
                Request.IsValid(ParamsSize) &&
                Request.Size == RequestSize &&
                StartPosition.IsValid(RequestSize) &&
                StartPosition.Size ==
                    static_cast<uint32>(FVector::Size()) &&
                StartDirection.IsValid(RequestSize) &&
                StartDirection.Size ==
                    static_cast<uint32>(FVector::Size()) &&
                Timestamp.IsValid(RequestSize) &&
                Timestamp.Size == sizeof(float);
        }
    };

    struct FLightweightProjectileVisualSchema
    {
        const UClass* RangedItemDefinitionClass = nullptr;
        UFunction* HasLightweightProjectile = nullptr;
        uint32 HasLightweightProjectileSize = 0;
        FFieldView HasLightweightProjectileReturn;
        UFunction* EndActiveAbility = nullptr;
        uint32 EndActiveAbilitySize = 0;
        FFieldView EndFiringTimestamp;

        bool CanIdentifyWeapon() const
        {
            return RangedItemDefinitionClass &&
                HasLightweightProjectile &&
                HasLightweightProjectileSize > 0 &&
                HasLightweightProjectileSize <= 0x40 &&
                HasLightweightProjectileReturn.IsValid(
                    HasLightweightProjectileSize) &&
                HasLightweightProjectileReturn.Size ==
                    sizeof(bool);
        }

        bool CanEndStream() const
        {
            return EndActiveAbility &&
                EndActiveAbilitySize > 0 &&
                EndActiveAbilitySize <= 0x20 &&
                EndFiringTimestamp.IsValid(
                    EndActiveAbilitySize) &&
                EndFiringTimestamp.Size ==
                    sizeof(float);
        }
    };

    struct FServerProjectileStateSchema
    {
        UFunction* SetStateFunction = nullptr;
        uint32 SetStateParamsSize = 0;
        FFieldView DamageStart;
        FFieldView DamageDirection;
        UFunction* EndAbilityFunction = nullptr;
        UFunction* StopRequestFunction = nullptr;

        bool CanDecodeSetState() const
        {
            return SetStateFunction &&
                SetStateParamsSize ==
                    static_cast<uint32>(
                        FVector::Size() * 2) &&
                DamageStart.IsValid(SetStateParamsSize) &&
                DamageStart.Size ==
                    static_cast<uint32>(FVector::Size()) &&
                DamageStart.Offset == 0 &&
                DamageDirection.IsValid(SetStateParamsSize) &&
                DamageDirection.Size ==
                    static_cast<uint32>(FVector::Size()) &&
                DamageDirection.Offset ==
                    static_cast<uint32>(FVector::Size());
        }
    };

    struct FEffectApiSchema
    {
        UObject* BlueprintLibrary = nullptr;
        UObject* FortBlueprintLibrary = nullptr;

        UFunction* GetAbilitySystem = nullptr;
        uint32 GetAbilitySystemSize = 0;
        FFieldView GetAbilitySystemActor;
        FFieldView GetAbilitySystemReturn;

        UFunction* MakeFortContext = nullptr;
        uint32 MakeFortContextSize = 0;
        FFieldView ContextSourceObject;
        FFieldView ContextEffectCauser;
        FFieldView ContextDamageSource;
        FFieldView ContextLevel;
        FFieldView ContextReturn;

        UFunction* AddHitResult = nullptr;
        uint32 AddHitResultSize = 0;
        FFieldView AddHitContext;
        FFieldView AddHitValue;
        FFieldView AddHitReset;
        uint32 HitResultSize = 0;

        UFunction* SetContextOrigin = nullptr;
        uint32 SetContextOriginSize = 0;
        FFieldView SetOriginContext;
        FFieldView SetOriginValue;

        UFunction* SetContextCritical = nullptr;
        uint32 SetContextCriticalSize = 0;
        FFieldView SetCriticalContext;
        FFieldView SetCriticalValue;

        UFunction* GetContextInstigator = nullptr;
        uint32 GetContextInstigatorSize = 0;
        FFieldView GetInstigatorContext;
        FFieldView GetInstigatorReturn;

        UFunction* GetContextSource = nullptr;
        uint32 GetContextSourceSize = 0;
        FFieldView GetSourceContext;
        FFieldView GetSourceReturn;

        UFunction* ApplyFortEffect = nullptr;
        uint32 ApplyFortEffectSize = 0;
        FFieldView ApplyEffectClass;
        FFieldView ApplyTarget;
        FFieldView ApplySourceObject;
        FFieldView ApplyEffectCauser;
        FFieldView ApplyDamageSource;
        FFieldView ApplyLevel;
        FFieldView ApplyContext;
        FFieldView ApplyReturn;

        const UStruct* EffectContainerStruct = nullptr;
        uint32 EffectContainerSize = 0;
        FFieldView ContainerApplicationTag;
        FFieldView ContainerTargetEffects;

        bool CanSetCritical() const
        {
            const uint32 ContextSize = ContextReturn.Size;
            return FortBlueprintLibrary &&
                SetContextCritical &&
                SetContextCriticalSize > 0 &&
                SetContextCriticalSize <= 0x80 &&
                SetCriticalContext.IsValid(
                    SetContextCriticalSize) &&
                SetCriticalContext.Size == ContextSize &&
                SetCriticalValue.IsValid(
                    SetContextCriticalSize) &&
                SetCriticalValue.Size == sizeof(bool);
        }

        bool IsValid() const
        {
            const uint32 ContextSize = ContextReturn.Size;
            return BlueprintLibrary &&
                GetAbilitySystem &&
                GetAbilitySystemActor.IsValid(GetAbilitySystemSize) &&
                GetAbilitySystemActor.Size == sizeof(AActor*) &&
                GetAbilitySystemReturn.IsValid(GetAbilitySystemSize) &&
                GetAbilitySystemReturn.Size ==
                    sizeof(UAbilitySystemComponent*) &&
                MakeFortContext &&
                MakeFortContextSize > 0 &&
                MakeFortContextSize <= 0x100 &&
                ContextSourceObject.IsValid(MakeFortContextSize) &&
                ContextSourceObject.Size == sizeof(UObject*) &&
                ContextEffectCauser.IsValid(MakeFortContextSize) &&
                ContextEffectCauser.Size == sizeof(AActor*) &&
                ContextDamageSource.IsValid(MakeFortContextSize) &&
                ContextDamageSource.Size == sizeof(AActor*) &&
                ContextLevel.IsValid(MakeFortContextSize) &&
                ContextLevel.Size == sizeof(int32) &&
                ContextReturn.IsValid(MakeFortContextSize) &&
                ContextSize == kExpectedEffectContextSize &&
                (ContextReturn.Offset % alignof(void*)) == 0 &&
                AddHitResult &&
                AddHitResultSize > 0 &&
                AddHitResultSize <= kMaxEffectCallParamsSize &&
                AddHitContext.IsValid(AddHitResultSize) &&
                AddHitContext.Size == ContextSize &&
                AddHitValue.IsValid(AddHitResultSize) &&
                AddHitValue.Size == HitResultSize &&
                AddHitReset.IsValid(AddHitResultSize) &&
                AddHitReset.Size == sizeof(bool) &&
                SetContextOrigin &&
                SetContextOriginSize > 0 &&
                SetContextOriginSize <= 0x100 &&
                SetOriginContext.IsValid(SetContextOriginSize) &&
                SetOriginContext.Size == ContextSize &&
                SetOriginValue.IsValid(SetContextOriginSize) &&
                SetOriginValue.Size ==
                    static_cast<uint32>(FVector::Size()) &&
                GetContextInstigator &&
                GetInstigatorContext.IsValid(GetContextInstigatorSize) &&
                GetInstigatorContext.Size == ContextSize &&
                GetInstigatorReturn.IsValid(GetContextInstigatorSize) &&
                GetInstigatorReturn.Size == sizeof(AActor*) &&
                GetContextSource &&
                GetSourceContext.IsValid(GetContextSourceSize) &&
                GetSourceContext.Size == ContextSize &&
                GetSourceReturn.IsValid(GetContextSourceSize) &&
                GetSourceReturn.Size == sizeof(UObject*) &&
                ApplyFortEffect &&
                ApplyFortEffectSize > 0 &&
                ApplyFortEffectSize <= 0x100 &&
                ApplyEffectClass.IsValid(ApplyFortEffectSize) &&
                ApplyEffectClass.Size == sizeof(UClass*) &&
                ApplyTarget.IsValid(ApplyFortEffectSize) &&
                ApplyTarget.Size == sizeof(UAbilitySystemComponent*) &&
                ApplySourceObject.IsValid(ApplyFortEffectSize) &&
                ApplySourceObject.Size == sizeof(UObject*) &&
                ApplyEffectCauser.IsValid(ApplyFortEffectSize) &&
                ApplyEffectCauser.Size == sizeof(AActor*) &&
                ApplyDamageSource.IsValid(ApplyFortEffectSize) &&
                ApplyDamageSource.Size == sizeof(AActor*) &&
                ApplyLevel.IsValid(ApplyFortEffectSize) &&
                ApplyLevel.Size == sizeof(float) &&
                ApplyContext.IsValid(ApplyFortEffectSize) &&
                ApplyContext.Size == ContextSize &&
                ApplyReturn.IsValid(ApplyFortEffectSize) &&
                ApplyReturn.Size == sizeof(FActiveGameplayEffectHandle) &&
                EffectContainerStruct &&
                EffectContainerSize ==
                    kExpectedEffectContainerSize &&
                ContainerTargetEffects.IsValid(EffectContainerSize) &&
                ContainerTargetEffects.Size == sizeof(TArray<UClass*>);
        }
    };

    struct FDamageFeedbackSchema
    {
        UObject* FortKismetLibrary = nullptr;
        UFunction* GetUnmodifiedDamage = nullptr;
        uint32 GetUnmodifiedDamageSize = 0;
        FFieldView GetUnmodifiedDamageContext;
        FFieldView GetUnmodifiedDamageReturn;

        UFunction* BatchedCueFunction = nullptr;
        uint32 BatchedCueParamsSize = 0;
        FFieldView BatchedCueShared;
        FFieldView BatchedCueNonShared;

        UFunction* DisplayHitNotifyFunction = nullptr;
        uint32 DisplayHitNotifyParamsSize = 0;
        FFieldView DisplayDamageDealt;
        FFieldView DisplayCriticalHit;
        FFieldView DisplayHitActor;

        uint32 PawnSize = 0;
        FFieldView AccumulatedShared;
        FFieldView AccumulatedNonShared;

        const UStruct* SharedStruct = nullptr;
        uint32 SharedSize = 0;
        FFieldView SharedMagnitude;
        FFieldView SharedCritical;
        FFieldView SharedValid;

        const UStruct* NonSharedStruct = nullptr;
        uint32 NonSharedSize = 0;
        FFieldView NonSharedHitActor;

        bool IsValid() const
        {
            return FortKismetLibrary &&
                GetUnmodifiedDamage &&
                GetUnmodifiedDamageSize ==
                    kExpectedGetUnmodifiedDamageParamsSize &&
                GetUnmodifiedDamageContext.IsValid(
                    GetUnmodifiedDamageSize) &&
                GetUnmodifiedDamageContext.Offset == 0 &&
                GetUnmodifiedDamageContext.Size ==
                    kExpectedEffectContextSize &&
                GetUnmodifiedDamageReturn.IsValid(
                    GetUnmodifiedDamageSize) &&
                GetUnmodifiedDamageReturn.Offset ==
                    static_cast<int32>(
                        kExpectedEffectContextSize) &&
                GetUnmodifiedDamageReturn.Size ==
                    sizeof(float) &&
                PawnSize > 0 &&
                PawnSize <= 0x100000 &&
                AccumulatedShared.IsValid(PawnSize) &&
                AccumulatedShared.Size ==
                    kExpectedDamageCueSharedSize &&
                AccumulatedNonShared.IsValid(PawnSize) &&
                AccumulatedNonShared.Size ==
                    kExpectedDamageCueNonSharedSize &&
                SharedStruct &&
                SharedSize ==
                    kExpectedDamageCueSharedSize &&
                SharedMagnitude.IsValid(SharedSize) &&
                SharedMagnitude.Offset == 0x60 &&
                SharedMagnitude.Size == sizeof(float) &&
                SharedCritical.IsValid(SharedSize) &&
                SharedCritical.Offset == 0x66 &&
                SharedCritical.Size == sizeof(uint8) &&
                SharedValid.IsValid(SharedSize) &&
                SharedValid.Offset == 0xA6 &&
                SharedValid.Size == sizeof(uint8) &&
                NonSharedStruct &&
                NonSharedSize ==
                    kExpectedDamageCueNonSharedSize &&
                NonSharedHitActor.IsValid(NonSharedSize) &&
                NonSharedHitActor.Offset == 0 &&
                NonSharedHitActor.Size == sizeof(AActor*);
        }

        bool CanRewriteBatchedCue() const
        {
            return IsValid() &&
                BatchedCueFunction &&
                BatchedCueFunction->ExecFunction &&
                BatchedCueParamsSize ==
                    kExpectedDamageCueSharedSize +
                        kExpectedDamageCueNonSharedSize &&
                BatchedCueShared.IsValid(
                    BatchedCueParamsSize) &&
                BatchedCueShared.Offset == 0 &&
                BatchedCueShared.Size ==
                    kExpectedDamageCueSharedSize &&
                BatchedCueNonShared.IsValid(
                    BatchedCueParamsSize) &&
                BatchedCueNonShared.Offset ==
                    static_cast<int32>(
                        kExpectedDamageCueSharedSize) &&
                BatchedCueNonShared.Size ==
                    kExpectedDamageCueNonSharedSize;
        }

        bool CanRewriteDisplayHitNotify() const
        {
            return IsValid() &&
                DisplayHitNotifyFunction &&
                DisplayHitNotifyParamsSize > 0 &&
                DisplayHitNotifyParamsSize <= 0x80 &&
                DisplayDamageDealt.IsValid(
                    DisplayHitNotifyParamsSize) &&
                DisplayDamageDealt.Size == sizeof(float) &&
                DisplayCriticalHit.IsValid(
                    DisplayHitNotifyParamsSize) &&
                DisplayCriticalHit.Size == sizeof(bool) &&
                DisplayHitActor.IsValid(
                    DisplayHitNotifyParamsSize) &&
                DisplayHitActor.Size == sizeof(AActor*);
        }
    };

    struct FDamageBatchSnapshot
    {
        AActor* HitActor = nullptr;
        float Magnitude = 0.f;
        bool Valid = false;
        bool Critical = false;
    };

    struct FLineOfSightSchema
    {
        UFunction* Function = nullptr;
        uint32 ParamsSize = 0;
        FFieldView Other;
        FFieldView ViewPoint;
        FFieldView AlternateChecks;
        FFieldView ReturnValue;

        bool IsValid() const
        {
            return Function &&
                ParamsSize > 0 &&
                ParamsSize <= 0x80 &&
                Other.IsValid(ParamsSize) &&
                Other.Size == sizeof(AActor*) &&
                ViewPoint.IsValid(ParamsSize) &&
                ViewPoint.Size ==
                    static_cast<uint32>(FVector::Size()) &&
                AlternateChecks.IsValid(ParamsSize) &&
                AlternateChecks.Size == sizeof(bool) &&
                ReturnValue.IsValid(ParamsSize) &&
                ReturnValue.Size == sizeof(bool);
        }
    };

    struct FWorldLineTraceSchema
    {
        UObject* Library = nullptr;
        UFunction* Function = nullptr;
        uint32 ParamsSize = 0;
        FFieldView WorldContextObject;
        FFieldView Start;
        FFieldView End;
        FFieldView TraceChannel;
        FFieldView TraceComplex;
        FFieldView ActorsToIgnore;
        FFieldView OutHit;
        FFieldView IgnoreSelf;
        FFieldView ReturnValue;

        bool IsValid() const
        {
            return Library &&
                Function &&
                Function->ExecFunction &&
                ParamsSize > 0 &&
                ParamsSize <= kMaxEffectCallParamsSize &&
                WorldContextObject.IsValid(ParamsSize) &&
                WorldContextObject.Size == sizeof(UObject*) &&
                Start.IsValid(ParamsSize) &&
                Start.Size ==
                    static_cast<uint32>(FVector::Size()) &&
                End.IsValid(ParamsSize) &&
                End.Size ==
                    static_cast<uint32>(FVector::Size()) &&
                TraceChannel.IsValid(ParamsSize) &&
                TraceChannel.Size == sizeof(uint8) &&
                TraceComplex.IsValid(ParamsSize) &&
                TraceComplex.Size == sizeof(bool) &&
                ActorsToIgnore.IsValid(ParamsSize) &&
                ActorsToIgnore.Size ==
                    sizeof(TArray<AActor*>) &&
                OutHit.IsValid(ParamsSize) &&
                OutHit.Size > 0 &&
                OutHit.Size <= kMaxHitResultSize &&
                IgnoreSelf.IsValid(ParamsSize) &&
                IgnoreSelf.Size == sizeof(bool) &&
                ReturnValue.IsValid(ParamsSize) &&
                ReturnValue.Size == sizeof(bool);
        }
    };

    struct FActorBoundsSchema
    {
        UFunction* Function = nullptr;
        uint32 ParamsSize = 0;
        FFieldView OnlyCollidingComponents;
        FFieldView Origin;
        FFieldView BoxExtent;
        FFieldView IncludeFromChildActors;

        bool IsValid() const
        {
            return Function &&
                ParamsSize > 0 &&
                ParamsSize <= 0x80 &&
                OnlyCollidingComponents.IsValid(ParamsSize) &&
                OnlyCollidingComponents.Size == sizeof(bool) &&
                Origin.IsValid(ParamsSize) &&
                Origin.Size ==
                    static_cast<uint32>(FVector::Size()) &&
                BoxExtent.IsValid(ParamsSize) &&
                BoxExtent.Size ==
                    static_cast<uint32>(FVector::Size()) &&
                IncludeFromChildActors.IsValid(ParamsSize) &&
                IncludeFromChildActors.Size == sizeof(bool);
        }
    };

    struct FComponentLineTraceSchema
    {
        const UClass* ComponentClass = nullptr;
        UFunction* Function = nullptr;
        uint32 ParamsSize = 0;
        FFieldView TraceStart;
        FFieldView TraceEnd;
        FFieldView TraceComplex;
        FFieldView BoneName;
        FFieldView OutHit;
        uint32 HitResultSize = 0;
        FFieldView ReturnValue;

        bool IsValid() const
        {
            return ComponentClass &&
                Function &&
                ParamsSize > 0 &&
                ParamsSize <= kMaxEffectCallParamsSize &&
                TraceStart.IsValid(ParamsSize) &&
                TraceStart.Size ==
                    static_cast<uint32>(FVector::Size()) &&
                TraceEnd.IsValid(ParamsSize) &&
                TraceEnd.Size ==
                    static_cast<uint32>(FVector::Size()) &&
                TraceComplex.IsValid(ParamsSize) &&
                TraceComplex.Size == sizeof(bool) &&
                BoneName.IsValid(ParamsSize) &&
                BoneName.Size > 0 &&
                BoneName.Size <= 8 &&
                OutHit.IsValid(ParamsSize) &&
                OutHit.Size == HitResultSize &&
                ReturnValue.IsValid(ParamsSize) &&
                ReturnValue.Size == sizeof(bool);
        }
    };

    struct FClosestPhysicsPointSchema
    {
        const UClass* ComponentClass = nullptr;
        UFunction* Function = nullptr;
        uint32 ParamsSize = 0;
        FFieldView WorldPosition;
        FFieldView ClosestWorldPosition;
        FFieldView Normal;
        FFieldView BoneName;
        FFieldView Distance;
        FFieldView ReturnValue;

        bool IsValid() const
        {
            return ComponentClass &&
                Function &&
                ParamsSize > 0 &&
                ParamsSize <= 0x100 &&
                WorldPosition.IsValid(ParamsSize) &&
                WorldPosition.Size ==
                    static_cast<uint32>(FVector::Size()) &&
                ClosestWorldPosition.IsValid(ParamsSize) &&
                ClosestWorldPosition.Size ==
                    static_cast<uint32>(FVector::Size()) &&
                Normal.IsValid(ParamsSize) &&
                Normal.Size ==
                    static_cast<uint32>(FVector::Size()) &&
                BoneName.IsValid(ParamsSize) &&
                BoneName.Size > 0 &&
                BoneName.Size <= 8 &&
                Distance.IsValid(ParamsSize) &&
                Distance.Size == sizeof(float) &&
                ReturnValue.IsValid(ParamsSize) &&
                ReturnValue.Size == sizeof(bool);
        }
    };

    struct FDamageZoneSchema
    {
        UFunction* Function = nullptr;
        uint32 ParamsSize = 0;
        FFieldView HitResult;
        FFieldView ReturnValue;

        bool IsValid() const
        {
            return Function &&
                ParamsSize > 0 &&
                ParamsSize <= kMaxEffectCallParamsSize &&
                HitResult.IsValid(ParamsSize) &&
                HitResult.Size > 0 &&
                HitResult.Size <= kMaxHitResultSize &&
                ReturnValue.IsValid(ParamsSize) &&
                ReturnValue.Size == sizeof(uint8);
        }
    };

    struct FResolvedHit
    {
        AActor* Target = nullptr;
        UActorComponent* TargetComponent = nullptr;
        FVector ImpactPoint{};
    };

    struct FDamageEffectSet
    {
        std::array<UClass*, kMaxDamageEffects> Classes{};
        int32 Count = 0;
        int32 ContainerIndex = -1;
    };

    struct FAbilityEffectCache
    {
        TWeakObjectPtr<UFortGameplayAbility> Ability{};
        bool EnvironmentTarget = false;
        bool Resolved = false;
        std::array<TWeakObjectPtr<UClass>, kMaxDamageEffects>
            Effects{};
        int32 Count = 0;
        int32 ContainerIndex = -1;
    };

    struct FWeaponHitBudget
    {
        TWeakObjectPtr<AFortWeaponRanged> Weapon{};
        AFortWeaponRanged* WeaponIdentity = nullptr;
        float WindowStart = -1.f;
        int32 Count = 0;
    };

    struct FCanonicalShotBudget
    {
        uint64 StreamGeneration = 0;
        int64 ShotIndex = -1;
        float LastSeen = -1.f;
        int32 Count = 0;
    };

    struct FProjectileReportBudget
    {
        uint64 ShotKey = 0;
        uint64 Fingerprint = 0;
        float LastSeen = -1.f;
    };

    struct FProjectileLedgerEntry
    {
        TWeakObjectPtr<AFortWeaponRanged> Weapon{};
        TWeakObjectPtr<AFortPlayerPawnAthena> Shooter{};
        AFortWeaponRanged* WeaponIdentity = nullptr;
        AFortPlayerPawnAthena* ShooterIdentity = nullptr;
        FVector Start{};
        FVector Direction{};
        float ProjectileTimestamp = 0.f;
        float RecordedAt = -1.f;
        float StoppedAt = -1.f;
        double ShotInterval = 0.1;
        uint64 Generation = 0;
        int64 ReservedShotIndex = -1;
        int32 PelletsPerShot = 1;
        int32 RemainingSingleShotHits = 1;
        float ServerFireToken = -1.f;
        bool TimestampFromClient = true;
        bool ReservedInitializedTimestamp = false;
        float ReservedPreviousTimestamp = 0.f;
        bool CompatibilityFallback = false;
        bool AuthoritativeShotSnapshot = false;
        bool Automatic = false;
        bool Active = false;
        bool Reserved = false;
    };

    struct FProjectileIngressCapability
    {
        UClass* WeaponClassIdentity = nullptr;
        bool NativeIngressObserved = false;
    };

    struct FProjectileCompatibilityTokenState
    {
        TWeakObjectPtr<AFortWeaponRanged> Weapon{};
        TWeakObjectPtr<AFortPlayerPawnAthena> Shooter{};
        AFortWeaponRanged* WeaponIdentity = nullptr;
        AFortPlayerPawnAthena* ShooterIdentity = nullptr;
        float NewestFireToken = -1.f;
    };

    struct FProjectileVisualRelayState
    {
        TWeakObjectPtr<AFortWeaponRanged> Weapon{};
        AFortWeaponRanged* WeaponIdentity = nullptr;
        float LastCapturedFireToken = -1.f;
        float LastVisualFireToken = -1.f;
        float LastNativeMulticastFireToken = -1.f;
        float LastNativeMulticastAt = -1.f;
        float ActiveVisualFireToken = -1.f;
        float PendingBaselineFireToken = -1.f;
        float PendingStartedAt = -1.f;
        FVector LatestDamageStart{};
        FVector LatestAdjustedAimDirection{};
        float LatestDamageStateAt = -1.f;
        bool HasLatestDamageState = false;
        bool LightweightProjectile = false;
        bool PendingActivation = false;
        bool CaptureActive = false;
        bool CaptureActiveBeforePending = false;
        bool Active = false;
    };

    struct FReflectedBoolField
    {
        int32 Offset = -1;
        uint32 ContainerSize = 0;
        uint8 Mask = 0;

        bool IsValid() const
        {
            return Offset >= 0 &&
                ContainerSize > 0 &&
                static_cast<uint64>(Offset) + 1 <=
                    ContainerSize &&
                Mask != 0;
        }
    };

    struct FTargetingReplicationSchema
    {
        FReflectedBoolField PawnIsTargeting;
        UFunction* GetIsTargetingFunction = nullptr;
        uint32 GetIsTargetingParamsSize = 0;
        FFieldView GetIsTargetingReturnValue;
        UFunction* SetTargetingFunction = nullptr;
        uint32 SetTargetingParamsSize = 0;
        FFieldView SetTargetingValue;
        UFunction* FastSharedReplicationFunction = nullptr;
        uint32 FastSharedReplicationParamsSize = 0;
        FFieldView FastSharedMovement;
        FReflectedBoolField
            FastSharedMovementIsTargeting;

        bool CanPoll() const
        {
            return PawnIsTargeting.IsValid() &&
                GetIsTargetingFunction &&
                GetIsTargetingFunction->ExecFunction &&
                GetIsTargetingParamsSize > 0 &&
                GetIsTargetingParamsSize <=
                    kMaxFrameSnapshotSize &&
                GetIsTargetingReturnValue.IsValid(
                    GetIsTargetingParamsSize) &&
                GetIsTargetingReturnValue.Size ==
                    sizeof(bool);
        }

        bool CanPatchFastSharedMovement() const
        {
            return CanPoll() &&
                FastSharedReplicationFunction &&
                FastSharedReplicationFunction->ExecFunction &&
                FastSharedReplicationParamsSize > 0 &&
                FastSharedReplicationParamsSize <=
                    kMaxNotifyParamsSize &&
                FastSharedMovement.IsValid(
                    FastSharedReplicationParamsSize) &&
                FastSharedMovement.Size ==
                    FastSharedMovementIsTargeting
                        .ContainerSize &&
                FastSharedMovementIsTargeting.IsValid();
        }

        bool CanHookSetTargeting() const
        {
            return CanPoll() &&
                SetTargetingFunction &&
                SetTargetingFunction->ExecFunction &&
                SetTargetingParamsSize > 0 &&
                SetTargetingParamsSize <=
                    kMaxFrameSnapshotSize &&
                SetTargetingValue.IsValid(
                    SetTargetingParamsSize) &&
                SetTargetingValue.Size == sizeof(bool);
        }
    };

    struct FCompatibilityTargetingAssertion
    {
        AFortPlayerPawnAthena* Pawn = nullptr;
        int32 ObjectIndex = -1;
    };

    enum class ETargetingMirrorSource : uint8
    {
        Poll,
        DecodedTransition
    };

    struct FHitBudgetReservation
    {
        FWeaponHitBudget* WeaponBudget = nullptr;
        FCanonicalShotBudget* ShotBudget = nullptr;
        FProjectileReportBudget* ReportBudget = nullptr;
        bool Active = false;
    };

    struct FDamageRequest
    {
        AFortWeaponRanged* Weapon = nullptr;
        AFortPlayerPawnAthena* ShooterPawn = nullptr;
        AFortPlayerControllerAthena* ShooterController = nullptr;
        UAbilitySystemComponent* ShooterAbilitySystem = nullptr;
        UAbilitySystemComponent* TargetAbilitySystem = nullptr;
        UFortGameplayAbility* FireAbility = nullptr;
        AActor* Target = nullptr;
        AFortPlayerPawnAthena* TargetPlayerPawn = nullptr;
        AFortAthenaVehicle* TargetVehicle = nullptr;
        UActorComponent* TargetHitComponent = nullptr;
        FVector ProjectileOrigin{};
        FVector LaunchDirection{};
        FVector ImpactPoint{};
        float ProjectileTimestamp = 0.f;
        bool HasProjectileTimestamp = false;
        bool IsCriticalHit = false;
        int32 AbilityLevel = 1;
        FDamageEffectSet Effects{};
        FProjectileLedgerEntry* LaunchReservation = nullptr;
        FHitBudgetReservation HitBudgetReservation{};
    };

    struct FActiveDamageFeedback
    {
        FActiveDamageFeedback* Previous = nullptr;
        AFortPlayerPawnAthena* ShooterPawn = nullptr;
        AFortPlayerPawnAthena* TargetPawn = nullptr;
        const uint8* Context = nullptr;
        FDamageBatchSnapshot Before{};
        bool ReadBefore = false;
        bool CueHandled = false;
        bool CueRewritten = false;
        bool ExpectedCritical = false;
        bool DisplayNotifyHandled = false;
        bool DisplayNotifyRewritten = false;
    };

    struct FScopedActiveDamageFeedback
    {
        FActiveDamageFeedback* Active = nullptr;

        explicit FScopedActiveDamageFeedback(
            FActiveDamageFeedback* InActive);
        ~FScopedActiveDamageFeedback();
    };

    FNotifyPawnHitSchema GNotifyPawnHitSchema{};
    FHitResultSchema GHitResultSchema{};
    FProjectileRequestSchema GProjectileRequestSchema{};
    FLightweightProjectileVisualSchema
        GLightweightProjectileVisualSchema{};
    FServerProjectileStateSchema
        GServerProjectileStateSchema{};
    FEffectApiSchema GEffectApiSchema{};
    FDamageFeedbackSchema GDamageFeedbackSchema{};
    FLineOfSightSchema GLineOfSightSchema{};
    FWorldLineTraceSchema GWorldLineTraceSchema{};
    FActorBoundsSchema GActorBoundsSchema{};
    FComponentLineTraceSchema GComponentLineTraceSchema{};
    FClosestPhysicsPointSchema GClosestPhysicsPointSchema{};
    FDamageZoneSchema GDamageZoneSchema{};
    FTargetingReplicationSchema
        GTargetingReplicationSchema{};

    alignas(16) std::array<uint8, 0x100> GMakeContextParams{};
    std::array<FAbilityEffectCache, 64> GEffectCache{};
    size_t GEffectCacheCursor = 0;
    std::array<FWeaponHitBudget, 1024> GLaunchBudgets{};
    size_t GLaunchBudgetCursor = 0;
    std::array<FWeaponHitBudget, 1024> GIngressBudgets{};
    size_t GIngressBudgetCursor = 0;
    std::array<FCanonicalShotBudget, 262144>
        GCanonicalShotBudgets{};
    // Exact report replay tombstones are separate from the aggregate pellet
    // budget. This keeps legitimate multi-pellet shots possible without
    // allowing one accepted pellet RPC to be replayed up to the pellet cap.
    std::array<FProjectileReportBudget, 1048576>
        GProjectileReportBudgets{};
    std::array<FProjectileLedgerEntry, 16384>
        GProjectileLedger{};
    std::array<FProjectileIngressCapability, 1024>
        GProjectileIngressCapabilities{};
    size_t GProjectileIngressCapabilityCursor = 0;
    std::array<FProjectileCompatibilityTokenState, 1024>
        GProjectileCompatibilityTokenStates{};
    size_t GProjectileCompatibilityTokenStateCursor = 0;
    std::array<FProjectileVisualRelayState, 1024>
        GProjectileVisualRelayStates{};
    size_t GProjectileVisualRelayStateCursor = 0;
    size_t GProjectileLedgerCursor = 0;
    uint64 GProjectileGeneration = 1;
    UWorld* GProjectileWorldIdentity = nullptr;
    float GLastProjectileServerTime = -1.f;
    thread_local bool GInsideNotifyPawnHit = false;
    thread_local bool GInsideProjectileVisualRelay = false;
    thread_local bool GInsideBatchedDamageCue = false;
    thread_local FActiveDamageFeedback*
        GActiveDamageFeedback = nullptr;
    UFunction* GProjectileActorNotifyFunction = nullptr;
    void (*GProjectileActorNotifyOriginal)(
        UObject*,
        FFrame&) = nullptr;
    void (*GSetTargetingOriginal)(
        UObject*,
        FFrame&) = nullptr;
    void (*GFastSharedReplicationOriginal)(
        UObject*,
        FFrame&) = nullptr;
    bool GTargetingReplicationResolved = false;
    bool GTargetingSetHookInstalled = false;
    bool GTargetingFastSharedHookInstalled = false;
    bool GTargetingReplicationReadyLogged = false;
    uint32 GTargetingReplicationRetryTicks = 0;
    uint32 GTargetingReplicationFailureLogs = 0;
    uint32 GTargetingProcessEventFailureLogs = 0;
    double GLastTargetingPollAt = -1.0;
    std::array<FCompatibilityTargetingAssertion, 512>
        GCompatibilityTargetingAssertions{};
    size_t GCompatibilityTargetingAssertionCursor = 0;
    using FPawnProcessEvent =
        void (*)(
            const UObject*,
            UFunction*,
            void*);
    FPawnProcessEvent GPawnProcessEventOriginal = nullptr;
    void PawnProcessEventDamageFeedback(
        const UObject* Context,
        UFunction* Function,
        void* Params);

    FScopedActiveDamageFeedback::
        FScopedActiveDamageFeedback(
            FActiveDamageFeedback* InActive)
        : Active(InActive)
    {
        if (!Active)
            return;

        Active->Previous = GActiveDamageFeedback;
        GActiveDamageFeedback = Active;
    }

    FScopedActiveDamageFeedback::
        ~FScopedActiveDamageFeedback()
    {
        if (Active &&
            GActiveDamageFeedback == Active)
        {
            GActiveDamageFeedback = Active->Previous;
        }
    }

    bool IsUsableObject(const UObject* Object)
    {
        if (!Object || !SDK::MemReadable(Object, 0x40))
            return false;

        const int32 ObjectIndex = Object->Index;
        if (ObjectIndex < 0 || ObjectIndex >= TUObjectArray::Num())
            return false;

        const auto Item =
            TUObjectArray::GetItemByIndex(ObjectIndex);
        const int32 SkipFlags =
            Offsets::bEncryptedObjects ? 0x10200000 : 0x20;
        return Item &&
            Item->GetObject() == Object &&
            !(Item->GetFlags() & SkipFlags) &&
            Object->Class &&
            SDK::MemReadable(Object->Class, 0x40);
    }

    bool IsUsableActor(const AActor* Actor)
    {
        return IsUsableObject(Actor) &&
            (!Actor->HasbActorIsBeingDestroyed() ||
                !Actor->bActorIsBeingDestroyed);
    }

    void LogProjectileDiagnostic(
        const char* Result,
        const char* Stage,
        AFortWeaponRanged* Weapon,
        AActor* Target = nullptr)
    {
        static int32 TraceCount = 0;
        if (TraceCount++ >= 128)
            return;

        SDK::DbgLog(
            "  [ProjectileDamage] %s stage=%s weapon=%s target=%s\n",
            Result ? Result : "trace",
            Stage ? Stage : "unknown",
            IsUsableActor(Weapon)
                ? Weapon->Name.ToString().c_str()
                : "invalid",
            IsUsableActor(Target)
                ? Target->Name.ToString().c_str()
                : "none");
    }

    template <typename TObject>
    TObject* ResolveWeakObject(
        const TWeakObjectPtr<TObject>& WeakObject)
    {
        if (WeakObject.ObjectIndex < 0 ||
            WeakObject.ObjectIndex >= TUObjectArray::Num() ||
            WeakObject.ObjectSerialNumber == 0)
        {
            return nullptr;
        }

        const auto Item =
            TUObjectArray::GetItemByIndex(
                WeakObject.ObjectIndex);
        if (!Item ||
            Item->SerialRef() !=
                WeakObject.ObjectSerialNumber)
        {
            return nullptr;
        }

        auto Object =
            const_cast<UObject*>(Item->GetObject());
        return IsUsableObject(Object)
            ? Object->Cast<TObject>()
            : nullptr;
    }

    bool IsFiniteVector(const FVector& Value)
    {
        return std::isfinite(Value.X) &&
            std::isfinite(Value.Y) &&
            std::isfinite(Value.Z) &&
            std::abs(Value.X) < 1.0e9 &&
            std::abs(Value.Y) < 1.0e9 &&
            std::abs(Value.Z) < 1.0e9;
    }

    uint64 MixHash64(uint64 Value)
    {
        Value ^= Value >> 30;
        Value *= 0xBF58476D1CE4E5B9ull;
        Value ^= Value >> 27;
        Value *= 0x94D049BB133111EBull;
        Value ^= Value >> 31;
        return Value;
    }

    uint64 HashVector(
        uint64 Seed,
        const FVector& Value)
    {
        // Normalize signed zero so equivalent geometry has one identity. The
        // validated FVector is otherwise hashed exactly; discarded client
        // metadata such as bone/material names cannot create a new report.
        const double Components[] = {
            Value.X == 0.0 ? 0.0 : Value.X,
            Value.Y == 0.0 ? 0.0 : Value.Y,
            Value.Z == 0.0 ? 0.0 : Value.Z
        };
        for (const double Component : Components)
        {
            uint64 Bits = 0;
            static_assert(
                sizeof(Bits) == sizeof(Component),
                "unexpected FVector scalar size");
            std::memcpy(
                &Bits,
                &Component,
                sizeof(Bits));
            Seed = MixHash64(
                Seed ^ Bits ^
                0x9E3779B97F4A7C15ull);
        }
        return Seed;
    }

    uint64 BuildProjectileReportFingerprint(
        AFortWeaponRanged* Weapon,
        AActor* Target,
        const FVector& ProjectileOrigin,
        const FVector& ImpactPoint,
        float ProjectileTimestamp)
    {
        uint64 Hash = MixHash64(
            static_cast<uint64>(
                reinterpret_cast<uintptr_t>(Weapon)) ^
            0xD6E8FEB86659FD93ull);
        Hash = MixHash64(
            Hash ^
            static_cast<uint64>(
                reinterpret_cast<uintptr_t>(Target)));
        Hash = HashVector(Hash, ProjectileOrigin);
        Hash = HashVector(Hash, ImpactPoint);
        uint32 TimestampBits = 0;
        std::memcpy(
            &TimestampBits,
            &ProjectileTimestamp,
            sizeof(TimestampBits));
        Hash = MixHash64(
            Hash ^
            static_cast<uint64>(TimestampBits));
        return Hash | 1ull;
    }

    FFieldView GetStructField(
        const UStruct* Struct,
        const char* Name)
    {
        if (!Struct)
            return {};

        const auto Property = Struct->GetProperty(Name);
        if (!Property)
            return {};

        return {
            static_cast<int32>(DecryptPropOffset(
                GetFromOffset<uint32>(
                    Property,
                    Offsets::Offset_Internal))),
            GetFromOffset<uint32>(
                Property,
                Offsets::ElementSize)
        };
    }

    FFieldView GetNamedParameter(
        const UFunction::ParamsNamed& Params,
        const char* Name)
    {
        for (const auto& Parameter : Params.NameOffsetMap)
        {
            if (Parameter.Name == Name)
            {
                return {
                    static_cast<int32>(Parameter.Offset),
                    Parameter.ElementSize
                };
            }
        }

        return {};
    }

    template <typename T>
    bool ReadValue(
        const void* Container,
        uint32 ContainerSize,
        const FFieldView& Field,
        T& OutValue,
        uint32 ExpectedSize = sizeof(T))
    {
        if (!Container ||
            !Field.IsValid(ContainerSize) ||
            Field.Size != ExpectedSize ||
            !SDK::MemReadable(
                static_cast<const uint8*>(Container) +
                    Field.Offset,
                Field.Size))
        {
            return false;
        }

        std::memset(&OutValue, 0, sizeof(T));
        std::memcpy(
            &OutValue,
            static_cast<const uint8*>(Container) +
                Field.Offset,
            Field.Size);
        return true;
    }

    template <typename T>
    bool ReadReflectedObjectValue(
        UObject* Object,
        const char* PropertyName,
        T& OutValue,
        uint32 ExpectedSize = sizeof(T))
    {
        if (!IsUsableObject(Object) ||
            !PropertyName ||
            ExpectedSize == 0 ||
            ExpectedSize > sizeof(T))
        {
            return false;
        }

        const auto Property =
            Object->GetProperty(PropertyName);
        const size_t PropertyMetadataSize =
            static_cast<size_t>((std::max)(
                Offsets::ElementSize,
                Offsets::Offset_Internal)) +
            sizeof(uint32);
        if (!Property ||
            !SDK::MemReadable(
                Property,
                PropertyMetadataSize) ||
            GetFromOffset<uint32>(
                Property,
                Offsets::ElementSize) != ExpectedSize)
        {
            return false;
        }

        const int32 Offset =
            static_cast<int32>(DecryptPropOffset(
                GetFromOffset<uint32>(
                    Property,
                    Offsets::Offset_Internal)));
        if (Offset < 0 ||
            Offset > 0x100000 ||
            !SDK::MemReadable(
                reinterpret_cast<const uint8*>(Object) +
                    Offset,
                ExpectedSize))
        {
            return false;
        }

        std::memset(&OutValue, 0, sizeof(T));
        std::memcpy(
            &OutValue,
            reinterpret_cast<const uint8*>(Object) +
                Offset,
            ExpectedSize);
        return true;
    }

    bool WriteBytes(
        void* Container,
        uint32 ContainerSize,
        const FFieldView& Field,
        const void* Value,
        uint32 ValueSize)
    {
        if (!Container ||
            !Value ||
            !Field.IsValid(ContainerSize) ||
            Field.Size != ValueSize)
        {
            return false;
        }

        std::memcpy(
            static_cast<uint8*>(Container) + Field.Offset,
            Value,
            ValueSize);
        return true;
    }

    template <typename T>
    bool WriteValue(
        void* Container,
        uint32 ContainerSize,
        const FFieldView& Field,
        const T& Value,
        uint32 ExpectedSize = sizeof(T))
    {
        return Field.Size == ExpectedSize &&
            WriteBytes(
                Container,
                ContainerSize,
                Field,
                &Value,
            ExpectedSize);
    }

    bool DecodeFrameReference(
        FFrame& Frame,
        void* OutValue,
        uint32 ValueSize)
    {
        if (!OutValue || ValueSize == 0)
        {
            return false;
        }

        if (Frame.Code)
        {
            if (!Offsets::Step)
                return false;
        }
        else
        {
            if (!Offsets::StepExplicitProperty ||
                Offsets::FFrame_PropertyChainForCompiledIn >
                    kMaxFrameSnapshotSize -
                        sizeof(void*))
            {
                return false;
            }

            const auto ChainAddress =
                reinterpret_cast<const uint8*>(&Frame) +
                Offsets::FFrame_PropertyChainForCompiledIn;
            const UField* PropertyChain = nullptr;
            if (!SDK::MemReadable(
                    ChainAddress,
                    sizeof(PropertyChain)))
            {
                return false;
            }
            std::memcpy(
                &PropertyChain,
                ChainAddress,
                sizeof(PropertyChain));
            if (!PropertyChain ||
                !SDK::MemReadable(
                    reinterpret_cast<const uint8*>(
                        PropertyChain) +
                        Offsets::FFrame_Next,
                    sizeof(void*)))
            {
                return false;
            }
        }

        void* Source =
            Frame.StepCompiledInRefInternal(OutValue);
        if (!Source ||
            !SDK::MemReadable(Source, ValueSize))
        {
            return false;
        }

        if (Source != OutValue)
            std::memcpy(OutValue, Source, ValueSize);
        return true;
    }

    bool SnapshotFrameForDecode(
        const FFrame& Source,
        std::array<uint8, kMaxFrameSnapshotSize>&
            Storage,
        FFrame*& OutFrame)
    {
        OutFrame = nullptr;
        if (Offsets::FFrame_PropertyChainForCompiledIn >
                kMaxFrameSnapshotSize - sizeof(void*) ||
            Offsets::FFrame_CurrentNativeFunction >
                kMaxFrameSnapshotSize - sizeof(void*))
        {
            return false;
        }

        uint32 RequiredSize =
            static_cast<uint32>(sizeof(FFrame));
        RequiredSize =
            (std::max)(
                RequiredSize,
                Offsets::FFrame_PropertyChainForCompiledIn +
                    static_cast<uint32>(sizeof(void*)));
        RequiredSize =
            (std::max)(
                RequiredSize,
                Offsets::FFrame_CurrentNativeFunction +
                    static_cast<uint32>(sizeof(void*)));
        if (RequiredSize > Storage.size() ||
            !SDK::MemReadable(&Source, RequiredSize))
        {
            return false;
        }

        Storage.fill(0);
        std::memcpy(
            Storage.data(),
            &Source,
            RequiredSize);
        OutFrame =
            reinterpret_cast<FFrame*>(Storage.data());
        return true;
    }

    FNotifyPawnHitSchema ResolveNotifySchema(
        UFunction* Function)
    {
        FNotifyPawnHitSchema Result{};
        if (!Function)
            return Result;

        const auto Params = Function->GetParamsNamed();
        Result.Function = Function;
        Result.ParamsSize = Params.Size;
        Result.Hit = GetNamedParameter(Params, "Hit");
        if (!Result.Hit.IsValid(Result.ParamsSize))
            Result.Hit = GetNamedParameter(Params, "HitResult");
        Result.ProjectileOrigin =
            GetNamedParameter(
                Params,
                "ProjectileOriginPosition");
        Result.ProjectileTimestamp =
            GetNamedParameter(
                Params,
                "ProjectileStartTimestamp");
        return Result;
    }

    FProjectileRequestSchema ResolveProjectileRequestSchema(
        UFunction* Function)
    {
        FProjectileRequestSchema Result{};
        if (!Function)
            return Result;

        const auto Params = Function->GetParamsNamed();
        Result.Function = Function;
        Result.ParamsSize = Params.Size;
        Result.Request =
            GetNamedParameter(
                Params,
                "ProjectileRequestNet");
        Result.RequestStruct =
            FindStruct(
                "FortLightweightProjectileRequest_Net");
        if (!Result.RequestStruct)
            return Result;

        Result.RequestSize =
            static_cast<uint32>(
                Result.RequestStruct->GetPropertiesSize());
        Result.StartPosition =
            GetStructField(
                Result.RequestStruct,
                "StartPosition");
        Result.StartDirection =
            GetStructField(
                Result.RequestStruct,
                "StartDirection");
        Result.Timestamp =
            GetStructField(
                Result.RequestStruct,
                "Timestamp");
        return Result;
    }

    FLightweightProjectileVisualSchema
        ResolveLightweightProjectileVisualSchema(
            AFortWeaponRanged* DefaultWeapon)
    {
        FLightweightProjectileVisualSchema Result{};
        if (!DefaultWeapon)
            return Result;

        Result.RangedItemDefinitionClass =
            UFortWeaponRangedItemDefinition::StaticClass();
        auto DefaultRangedItemDefinition =
            Result.RangedItemDefinitionClass
                ? Result.RangedItemDefinitionClass
                    ->GetDefaultObj()
                : nullptr;
        Result.HasLightweightProjectile =
            DefaultRangedItemDefinition
                ? DefaultRangedItemDefinition->GetFunction(
                    "HasLightweightProjectile")
                : nullptr;
        if (Result.HasLightweightProjectile)
        {
            const auto Params =
                Result.HasLightweightProjectile
                    ->GetParamsNamed();
            Result.HasLightweightProjectileSize =
                Params.Size;
            Result.HasLightweightProjectileReturn =
                GetNamedParameter(
                    Params,
                    "ReturnValue");
        }

        Result.EndActiveAbility =
            DefaultWeapon->GetFunction(
                "MulticastLWProjectile_EndActiveAbility");
        if (Result.EndActiveAbility)
        {
            const auto Params =
                Result.EndActiveAbility
                    ->GetParamsNamed();
            Result.EndActiveAbilitySize = Params.Size;
            Result.EndFiringTimestamp =
                GetNamedParameter(
                    Params,
                    "FiringTimestamp");
        }
        return Result;
    }

    FServerProjectileStateSchema
        ResolveServerProjectileStateSchema(
            AFortWeaponRanged* DefaultWeapon)
    {
        FServerProjectileStateSchema Result{};
        if (!DefaultWeapon)
            return Result;

        Result.SetStateFunction =
            DefaultWeapon->GetFunction(
                "ServerLWProjectile_SetDamageStartAndDirection");
        Result.EndAbilityFunction =
            DefaultWeapon->GetFunction(
                "ServerLWProjectile_EndActiveAbility");
        Result.StopRequestFunction =
            DefaultWeapon->GetFunction(
                "ServerStopProjectileRequest");
        if (!Result.SetStateFunction)
            return Result;

        const auto Params =
            Result.SetStateFunction->GetParamsNamed();
        Result.SetStateParamsSize = Params.Size;
        Result.DamageStart =
            GetNamedParameter(Params, "InDamageStart");
        Result.DamageDirection =
            GetNamedParameter(
                Params,
                "InDamageDirection");
        return Result;
    }

    FHitResultSchema ResolveHitResultSchema()
    {
        FHitResultSchema Result{};
        Result.Struct = FindStruct("HitResult");
        if (!Result.Struct)
            return Result;

        Result.Size =
            static_cast<uint32>(
                Result.Struct->GetPropertiesSize());
        Result.Component =
            GetStructField(Result.Struct, "Component");
        Result.Actor =
            GetStructField(Result.Struct, "Actor");
        Result.HitObjectHandle =
            GetStructField(
                Result.Struct,
                "HitObjectHandle");
        Result.PhysicalMaterial =
            GetStructField(
                Result.Struct,
                "PhysMaterial");
        Result.BoneName =
            GetStructField(Result.Struct, "BoneName");
        Result.MyBoneName =
            GetStructField(Result.Struct, "MyBoneName");
        Result.FaceIndex =
            GetStructField(Result.Struct, "FaceIndex");
        Result.Time =
            GetStructField(Result.Struct, "Time");
        Result.Distance =
            GetStructField(Result.Struct, "Distance");
        Result.ImpactPoint =
            GetStructField(Result.Struct, "ImpactPoint");
        Result.Location =
            GetStructField(Result.Struct, "Location");
        Result.ImpactNormal =
            GetStructField(Result.Struct, "ImpactNormal");
        Result.Normal =
            GetStructField(Result.Struct, "Normal");
        Result.TraceStart =
            GetStructField(Result.Struct, "TraceStart");
        Result.TraceEnd =
            GetStructField(Result.Struct, "TraceEnd");
        Result.PenetrationDepth =
            GetStructField(
                Result.Struct,
                "PenetrationDepth");
        Result.MyItem =
            GetStructField(Result.Struct, "MyItem");
        Result.Item =
            GetStructField(Result.Struct, "Item");
        Result.ElementIndex =
            GetStructField(Result.Struct, "ElementIndex");
        Result.BlockingHit =
            GetStructField(Result.Struct, "bBlockingHit");
        Result.ActorInstanceHandleStruct =
            FindStruct("ActorInstanceHandle");
        if (Result.ActorInstanceHandleStruct)
        {
            Result.ActorInstanceHandleSize =
                static_cast<uint32>(
                    Result.ActorInstanceHandleStruct
                        ->GetPropertiesSize());
            Result.HandleReferenceObject =
                GetStructField(
                    Result.ActorInstanceHandleStruct,
                    "ReferenceObject");
            if (!Result.HandleReferenceObject.IsValid(
                    Result.ActorInstanceHandleSize))
            {
                Result.HandleReferenceObject =
                    GetStructField(
                        Result.ActorInstanceHandleStruct,
                        "Actor");
            }
        }
        return Result;
    }

    FDamageZoneSchema ResolveDamageZoneSchema()
    {
        FDamageZoneSchema Result{};
        auto PawnDefault =
            AFortPlayerPawnAthena::GetDefaultObj();
        Result.Function = PawnDefault
            ? PawnDefault->GetFunction("GetDamageZone")
            : nullptr;
        if (!Result.Function)
            return {};

        const auto Params =
            Result.Function->GetParamsNamed();
        Result.ParamsSize = Params.Size;
        Result.HitResult =
            GetNamedParameter(Params, "InHitResult");
        Result.ReturnValue =
            GetNamedParameter(Params, "ReturnValue");
        return Result;
    }

    FComponentLineTraceSchema
        ResolveComponentLineTraceSchema()
    {
        FComponentLineTraceSchema Result{};
        auto PrimitiveComponentClass =
            FindClass("PrimitiveComponent");
        Result.ComponentClass =
            PrimitiveComponentClass;
        auto PrimitiveComponentDefault =
            PrimitiveComponentClass
                ? PrimitiveComponentClass->GetDefaultObj()
                : nullptr;
        Result.Function = PrimitiveComponentDefault
            ? PrimitiveComponentDefault->GetFunction(
                "K2_LineTraceComponent")
            : nullptr;
        if (!Result.Function)
            return {};

        const auto Params =
            Result.Function->GetParamsNamed();
        Result.ParamsSize = Params.Size;
        Result.TraceStart =
            GetNamedParameter(Params, "TraceStart");
        Result.TraceEnd =
            GetNamedParameter(Params, "TraceEnd");
        Result.TraceComplex =
            GetNamedParameter(Params, "bTraceComplex");
        Result.BoneName =
            GetNamedParameter(Params, "BoneName");
        Result.OutHit =
            GetNamedParameter(Params, "OutHit");
        Result.HitResultSize = GHitResultSchema.Size;
        Result.ReturnValue =
            GetNamedParameter(Params, "ReturnValue");
        return Result;
    }

    FClosestPhysicsPointSchema
        ResolveClosestPhysicsPointSchema()
    {
        FClosestPhysicsPointSchema Result{};
        auto SkeletalMeshComponentClass =
            FindClass("SkeletalMeshComponent");
        Result.ComponentClass =
            SkeletalMeshComponentClass;
        auto SkeletalMeshComponentDefault =
            SkeletalMeshComponentClass
                ? SkeletalMeshComponentClass->GetDefaultObj()
                : nullptr;
        Result.Function = SkeletalMeshComponentDefault
            ? SkeletalMeshComponentDefault->GetFunction(
                "K2_GetClosestPointOnPhysicsAsset")
            : nullptr;
        if (!Result.Function)
            return {};

        const auto Params =
            Result.Function->GetParamsNamed();
        Result.ParamsSize = Params.Size;
        Result.WorldPosition =
            GetNamedParameter(Params, "WorldPosition");
        Result.ClosestWorldPosition =
            GetNamedParameter(
                Params,
                "ClosestWorldPosition");
        Result.Normal =
            GetNamedParameter(Params, "Normal");
        Result.BoneName =
            GetNamedParameter(Params, "BoneName");
        Result.Distance =
            GetNamedParameter(Params, "Distance");
        Result.ReturnValue =
            GetNamedParameter(Params, "ReturnValue");
        return Result;
    }

    bool ResolveCriticalDamageZone(
        AFortPlayerPawnAthena* Target,
        const void* HitMemory,
        uint32 HitSize,
        bool& OutCritical,
        uint8& OutZone)
    {
        OutCritical = false;
        OutZone = 0xFF;
        if (!IsUsableActor(Target) ||
            !GDamageZoneSchema.IsValid() ||
            HitSize != GDamageZoneSchema.HitResult.Size ||
            !SDK::MemReadable(HitMemory, HitSize))
        {
            return false;
        }

        alignas(16) std::array<
            uint8,
            kMaxEffectCallParamsSize> Params{};
        if (!WriteBytes(
                Params.data(),
                GDamageZoneSchema.ParamsSize,
                GDamageZoneSchema.HitResult,
                HitMemory,
                HitSize))
        {
            return false;
        }

        Target->ProcessEvent(
            GDamageZoneSchema.Function,
            Params.data());
        if (!ReadValue(
                Params.data(),
                GDamageZoneSchema.ParamsSize,
                GDamageZoneSchema.ReturnValue,
                OutZone) ||
            OutZone > 4)
        {
            OutZone = 0xFF;
            return false;
        }

        // EFortDamageZone::Critical is value 2 on the Chapter 5 builds
        // covered by this hook.
        OutCritical = OutZone == 2;
        return true;
    }

    bool ResolveHit(
        const void* HitMemory,
        uint32 HitSize,
        FResolvedHit& OutHit)
    {
        if (!GHitResultSchema.IsValid() ||
            HitSize != GHitResultSchema.Size ||
            !SDK::MemReadable(HitMemory, HitSize))
        {
            return false;
        }

        AActor* ComponentOwner = nullptr;
        UActorComponent* ResolvedComponent = nullptr;
        if (GHitResultSchema.Component.IsValid(HitSize) &&
            GHitResultSchema.Component.Size ==
                sizeof(TWeakObjectPtr<UActorComponent>))
        {
            TWeakObjectPtr<UActorComponent> WeakComponent{};
            if (ReadValue(
                    HitMemory,
                    HitSize,
                    GHitResultSchema.Component,
                    WeakComponent))
            {
                auto Component =
                    ResolveWeakObject(WeakComponent);
                if (IsUsableObject(Component) &&
                    Component->IsA(
                        UActorComponent::StaticClass()))
                {
                    ResolvedComponent = Component;
                    auto OwnerObject = Component->GetOwner();
                    auto Owner = IsUsableObject(OwnerObject)
                        ? OwnerObject->Cast<AActor>()
                        : nullptr;
                    if (IsUsableActor(Owner))
                        ComponentOwner = Owner;
                }
            }
        }

        AActor* ReportedActor = nullptr;
        if (GHitResultSchema.Actor.IsValid(HitSize) &&
            GHitResultSchema.Actor.Size ==
                sizeof(TWeakObjectPtr<AActor>))
        {
            TWeakObjectPtr<AActor> WeakActor{};
            if (ReadValue(
                    HitMemory,
                    HitSize,
                    GHitResultSchema.Actor,
                    WeakActor))
            {
                auto Actor = ResolveWeakObject(WeakActor);
                if (IsUsableActor(Actor))
                    ReportedActor = Actor;
            }
        }

        AActor* HandleActor = nullptr;
        if (GHitResultSchema.HitObjectHandle.IsValid(
                HitSize) &&
            GHitResultSchema.ActorInstanceHandleStruct &&
            GHitResultSchema.ActorInstanceHandleSize ==
                GHitResultSchema.HitObjectHandle.Size &&
            GHitResultSchema.HandleReferenceObject.IsValid(
                GHitResultSchema.ActorInstanceHandleSize) &&
            GHitResultSchema.HandleReferenceObject.Size ==
                sizeof(TWeakObjectPtr<UObject>))
        {
            const auto HandleMemory =
                static_cast<const uint8*>(HitMemory) +
                GHitResultSchema.HitObjectHandle.Offset;
            TWeakObjectPtr<UObject> WeakReference{};
            if (ReadValue(
                    HandleMemory,
                    GHitResultSchema.ActorInstanceHandleSize,
                    GHitResultSchema.HandleReferenceObject,
                    WeakReference))
            {
                const bool HasReference =
                    WeakReference.ObjectIndex >= 0 ||
                    WeakReference.ObjectSerialNumber != 0;
                if (HasReference)
                {
                    auto ReferenceObject =
                        ResolveWeakObject(WeakReference);
                    HandleActor =
                        ReferenceObject
                            ? ReferenceObject->Cast<AActor>()
                            : nullptr;
                    if (!HandleActor && ReferenceObject)
                    {
                        auto ReferenceComponent =
                            ReferenceObject
                                ->Cast<UActorComponent>();
                        auto ReferenceOwner =
                            ReferenceComponent
                                ? ReferenceComponent->GetOwner()
                                : nullptr;
                        HandleActor =
                            IsUsableObject(ReferenceOwner)
                            ? ReferenceOwner->Cast<AActor>()
                            : nullptr;
                    }
                    if (!IsUsableActor(HandleActor))
                        HandleActor = nullptr;
                }
            }
        }

        // Chapter 5 building and actor-instance hits can legitimately expose
        // a shared/proxy component owner. Prefer the actor-instance handle,
        // which matches FHitResult::GetActor(), then the legacy actor field;
        // use the component owner only as a fallback.
        AActor* ResolvedTarget =
            HandleActor
                ? HandleActor
                : (ReportedActor
                    ? ReportedActor
                    : ComponentOwner);
        if (!ResolvedTarget)
        {
            return false;
        }
        const bool HasIdentityMismatch =
            (ComponentOwner &&
                ComponentOwner != ResolvedTarget) ||
            (ReportedActor &&
                ReportedActor != ResolvedTarget) ||
            (HandleActor &&
                HandleActor != ResolvedTarget);
        if (HasIdentityMismatch &&
            ResolvedTarget->Cast<
                AFortPlayerPawnAthena>())
        {
            LogProjectileDiagnostic(
                "reject",
                "player-hit-identity",
                nullptr,
                ResolvedTarget);
            return false;
        }
        if (HasIdentityMismatch)
        {
            LogProjectileDiagnostic(
                "trace",
                "hit-proxy-mismatch",
                nullptr,
                ResolvedTarget);
        }

        OutHit.Target = ResolvedTarget;
        // A component can only be used for an authoritative follow-up trace
        // when it is a live primitive owned by the exact resolved target.
        // Proxy/shared components remain valid for actor resolution, but must
        // never supply vehicle part or pawn damage-zone metadata.
        OutHit.TargetComponent =
            ResolvedComponent &&
                ComponentOwner == ResolvedTarget
            ? ResolvedComponent
            : nullptr;

        if (!ReadValue(
                HitMemory,
                HitSize,
                GHitResultSchema.ImpactPoint,
                OutHit.ImpactPoint,
                FVector::Size()) &&
            !ReadValue(
                HitMemory,
                HitSize,
                GHitResultSchema.Location,
                OutHit.ImpactPoint,
                FVector::Size()))
        {
            return false;
        }

        return IsFiniteVector(OutHit.ImpactPoint);
    }

    bool TraceAuthoritativeOwnedComponentHit(
        AActor* Target,
        UActorComponent* TargetComponent,
        const FVector& LaunchOrigin,
        const FVector& LaunchDirection,
        const FVector& ImpactPoint,
        void* OutHitMemory,
        uint32 HitSize)
    {
        if (!IsUsableActor(Target) ||
            !IsUsableObject(TargetComponent) ||
            !GComponentLineTraceSchema.IsValid() ||
            !TargetComponent->IsA(
                GComponentLineTraceSchema.ComponentClass) ||
            TargetComponent->GetOwner() != Target ||
            HitSize != GHitResultSchema.Size ||
            !OutHitMemory ||
            !IsFiniteVector(LaunchOrigin) ||
            !IsFiniteVector(LaunchDirection) ||
            !IsFiniteVector(ImpactPoint))
        {
            return false;
        }

        // The launch direction is only the initial ballistic tangent. At the
        // target, bullet drop and close camera/muzzle parallax make the
        // validated launch-to-impact chord a better local trace direction.
        FVector TraceDirection =
            (ImpactPoint - LaunchOrigin)
                .GetSafeNormal();
        if (TraceDirection.IsZero())
            TraceDirection =
                LaunchDirection.GetSafeNormal();
        if (TraceDirection.IsZero())
            return false;

        // Trace only the exact server-owned target component, across a short
        // segment around the validated impact. This produces authoritative
        // component, bone, shape/item and physical-material metadata without
        // choosing a world channel.
        const FVector TraceStart =
            ImpactPoint -
                TraceDirection *
                    kAuthoritativeTraceBacktrackCm;
        const FVector TraceEnd =
            ImpactPoint +
                TraceDirection *
                    kAuthoritativeTraceForwardCm;
        const bool TraceComplex = false;
        alignas(16) std::array<
            uint8,
            kMaxEffectCallParamsSize> Params{};
        if (!WriteBytes(
                Params.data(),
                GComponentLineTraceSchema.ParamsSize,
                GComponentLineTraceSchema.TraceStart,
                &TraceStart,
                FVector::Size()) ||
            !WriteBytes(
                Params.data(),
                GComponentLineTraceSchema.ParamsSize,
                GComponentLineTraceSchema.TraceEnd,
                &TraceEnd,
                FVector::Size()) ||
            !WriteValue(
                Params.data(),
                GComponentLineTraceSchema.ParamsSize,
                GComponentLineTraceSchema.TraceComplex,
                TraceComplex))
        {
            return false;
        }

        TargetComponent->ProcessEvent(
            GComponentLineTraceSchema.Function,
            Params.data());

        bool HitTargetComponent = false;
        std::array<uint8, kMaxHitResultSize>
            ServerHit{};
        if (!ReadValue(
                Params.data(),
                GComponentLineTraceSchema.ParamsSize,
                GComponentLineTraceSchema.ReturnValue,
                HitTargetComponent) ||
            !HitTargetComponent ||
            !ReadValue(
                Params.data(),
                GComponentLineTraceSchema.ParamsSize,
                GComponentLineTraceSchema.OutHit,
                ServerHit,
                HitSize))
        {
            return false;
        }

        if (GHitResultSchema.BoneName.IsValid(HitSize) &&
            GHitResultSchema.BoneName.Size ==
                GComponentLineTraceSchema.BoneName.Size)
        {
            std::array<uint8, 8> TraceBoneName{};
            if (ReadValue(
                    Params.data(),
                    GComponentLineTraceSchema.ParamsSize,
                    GComponentLineTraceSchema.BoneName,
                    TraceBoneName,
                    GComponentLineTraceSchema.BoneName.Size))
            {
                const bool HasTraceBone =
                    std::any_of(
                        TraceBoneName.begin(),
                        TraceBoneName.begin() +
                            GComponentLineTraceSchema
                                .BoneName.Size,
                        [](uint8 Value)
                        {
                            return Value != 0;
                        });
                if (HasTraceBone)
                {
                    WriteBytes(
                        ServerHit.data(),
                        HitSize,
                        GHitResultSchema.BoneName,
                        TraceBoneName.data(),
                        GHitResultSchema.BoneName.Size);
                }
            }
        }

        FResolvedHit ResolvedServerHit{};
        if (!ResolveHit(
                ServerHit.data(),
                HitSize,
                ResolvedServerHit) ||
            ResolvedServerHit.Target != Target ||
            FVector::Dist(
                ResolvedServerHit.ImpactPoint,
                ImpactPoint) >
                kMaxAuthoritativeImpactErrorCm)
        {
            return false;
        }

        std::memcpy(
            OutHitMemory,
            ServerHit.data(),
            HitSize);
        return true;
    }

    bool TraceAuthoritativePlayerHit(
        AFortPlayerPawnAthena* Target,
        const FVector& LaunchOrigin,
        const FVector& LaunchDirection,
        const FVector& ImpactPoint,
        void* OutHitMemory,
        uint32 HitSize)
    {
        if (!IsUsableActor(Target) ||
            !Target->HasMesh() ||
            !IsUsableObject(Target->Mesh))
        {
            return false;
        }

        return TraceAuthoritativeOwnedComponentHit(
            Target,
            Target->Mesh,
            LaunchOrigin,
            LaunchDirection,
            ImpactPoint,
            OutHitMemory,
            HitSize);
    }

    void ClearUntrustedHitClassification(
        void* HitMemory,
        uint32 HitSize)
    {
        if (!HitMemory ||
            HitSize != GHitResultSchema.Size)
        {
            return;
        }

        const FFieldView ZeroFields[] = {
            GHitResultSchema.PhysicalMaterial,
            GHitResultSchema.BoneName,
            GHitResultSchema.MyBoneName,
            GHitResultSchema.ElementIndex
        };
        for (const auto& Field : ZeroFields)
        {
            if (Field.IsValid(HitSize))
            {
                std::memset(
                    static_cast<uint8*>(HitMemory) +
                        Field.Offset,
                    0,
                    Field.Size);
            }
        }

        const int32 InvalidIndex = -1;
        const FFieldView IndexFields[] = {
            GHitResultSchema.FaceIndex,
            GHitResultSchema.MyItem,
            GHitResultSchema.Item
        };
        for (const auto& Field : IndexFields)
        {
            if (!Field.IsValid(HitSize))
                continue;

            if (Field.Size == sizeof(InvalidIndex))
            {
                std::memcpy(
                    static_cast<uint8*>(HitMemory) +
                        Field.Offset,
                    &InvalidIndex,
                    sizeof(InvalidIndex));
            }
            else
            {
                std::memset(
                    static_cast<uint8*>(HitMemory) +
                        Field.Offset,
                    0,
                    Field.Size);
            }
        }
    }

    bool InitializeCanonicalPlayerHitIdentity(
        void* HitMemory,
        uint32 HitSize,
        AFortPlayerPawnAthena* Target)
    {
        if (!HitMemory ||
            HitSize != GHitResultSchema.Size ||
            !IsUsableActor(Target))
        {
            return false;
        }

        std::memset(HitMemory, 0, HitSize);
        bool WroteIdentity = false;
        if (GHitResultSchema.Actor.IsValid(HitSize) &&
            GHitResultSchema.Actor.Size ==
                sizeof(TWeakObjectPtr<AActor>))
        {
            const TWeakObjectPtr<AActor> WeakTarget(Target);
            WroteIdentity =
                WriteValue(
                    HitMemory,
                    HitSize,
                    GHitResultSchema.Actor,
                    WeakTarget) ||
                WroteIdentity;
        }

        if (GHitResultSchema.Component.IsValid(HitSize) &&
            GHitResultSchema.Component.Size ==
                sizeof(TWeakObjectPtr<UActorComponent>) &&
            Target->HasMesh() &&
            IsUsableObject(Target->Mesh))
        {
            const TWeakObjectPtr<UActorComponent>
                WeakMesh(Target->Mesh);
            WroteIdentity =
                WriteValue(
                    HitMemory,
                    HitSize,
                    GHitResultSchema.Component,
                    WeakMesh) ||
                WroteIdentity;
        }

        if (GHitResultSchema.HitObjectHandle.IsValid(
                HitSize) &&
            GHitResultSchema.ActorInstanceHandleStruct &&
            GHitResultSchema.ActorInstanceHandleSize ==
                GHitResultSchema.HitObjectHandle.Size &&
            GHitResultSchema.HandleReferenceObject.IsValid(
                GHitResultSchema.ActorInstanceHandleSize) &&
            GHitResultSchema.HandleReferenceObject.Size ==
                sizeof(TWeakObjectPtr<UObject>))
        {
            auto HandleMemory =
                static_cast<uint8*>(HitMemory) +
                    GHitResultSchema.HitObjectHandle.Offset;
            const TWeakObjectPtr<UObject> WeakTarget(Target);
            WroteIdentity =
                WriteValue(
                    HandleMemory,
                    GHitResultSchema.ActorInstanceHandleSize,
                    GHitResultSchema.HandleReferenceObject,
                    WeakTarget) ||
                WroteIdentity;
        }
        return WroteIdentity;
    }

    bool BuildAuthoritativeClosestPhysicsHit(
        AFortPlayerPawnAthena* Target,
        const FVector& ImpactPoint,
        void* OutHitMemory,
        uint32 HitSize)
    {
        if (!IsUsableActor(Target) ||
            !Target->HasMesh() ||
            !IsUsableObject(Target->Mesh) ||
            !GClosestPhysicsPointSchema.IsValid() ||
            !Target->Mesh->IsA(
                GClosestPhysicsPointSchema.ComponentClass) ||
            !GHitResultSchema.BoneName.IsValid(HitSize) ||
            GHitResultSchema.BoneName.Size !=
                GClosestPhysicsPointSchema.BoneName.Size ||
            HitSize != GHitResultSchema.Size ||
            !OutHitMemory ||
            !IsFiniteVector(ImpactPoint))
        {
            return false;
        }

        alignas(16) std::array<uint8, 0x100>
            Params{};
        if (!WriteBytes(
                Params.data(),
                GClosestPhysicsPointSchema.ParamsSize,
                GClosestPhysicsPointSchema.WorldPosition,
                &ImpactPoint,
                FVector::Size()))
        {
            return false;
        }

        Target->Mesh->ProcessEvent(
            GClosestPhysicsPointSchema.Function,
            Params.data());

        bool FoundPoint = false;
        float Distance = 0.f;
        FVector ClosestWorldPosition{};
        std::array<uint8, 8> BoneName{};
        if (!ReadValue(
                Params.data(),
                GClosestPhysicsPointSchema.ParamsSize,
                GClosestPhysicsPointSchema.ReturnValue,
                FoundPoint) ||
            !FoundPoint ||
            !ReadValue(
                Params.data(),
                GClosestPhysicsPointSchema.ParamsSize,
                GClosestPhysicsPointSchema.Distance,
                Distance) ||
            !ReadValue(
                Params.data(),
                GClosestPhysicsPointSchema.ParamsSize,
                GClosestPhysicsPointSchema
                    .ClosestWorldPosition,
                ClosestWorldPosition,
                FVector::Size()) ||
            !ReadValue(
                Params.data(),
                GClosestPhysicsPointSchema.ParamsSize,
                GClosestPhysicsPointSchema.BoneName,
                BoneName,
                GClosestPhysicsPointSchema.BoneName.Size) ||
            !std::isfinite(Distance) ||
            Distance < 0.f ||
            Distance >
                kMaxAuthoritativePhysicsPointErrorCm ||
            !IsFiniteVector(ClosestWorldPosition) ||
            FVector::Dist(
                ImpactPoint,
                ClosestWorldPosition) >
                kMaxAuthoritativePhysicsPointErrorCm)
        {
            return false;
        }

        if (!InitializeCanonicalPlayerHitIdentity(
                OutHitMemory,
                HitSize,
                Target) ||
            !WriteBytes(
                OutHitMemory,
                HitSize,
                GHitResultSchema.BoneName,
                BoneName.data(),
                GHitResultSchema.BoneName.Size))
        {
            return false;
        }
        return true;
    }

    void SanitizeUntrustedHitMetadata(
        void* HitMemory,
        uint32 HitSize,
        const FVector& ProjectileOrigin,
        const FVector& ImpactPoint)
    {
        if (!HitMemory ||
            HitSize != GHitResultSchema.Size ||
            !IsFiniteVector(ProjectileOrigin) ||
            !IsFiniteVector(ImpactPoint))
        {
            return;
        }

        const FVector SafeNormal =
            (ProjectileOrigin - ImpactPoint)
                .GetSafeNormal();
        const double RawDistance =
            FVector::Dist(
                ProjectileOrigin,
                ImpactPoint);
        const float Distance =
            std::isfinite(RawDistance)
                ? static_cast<float>(
                    (std::min)(
                        RawDistance,
                        static_cast<double>(
                            (std::numeric_limits<float>::max)())))
                : 0.f;
        const float Time = 1.f;
        const float ZeroFloat = 0.f;
        const uint8 BlockingHit = 1;

        // Preserve classification metadata (bone, physical material, face,
        // item and element). Fortnite's weapon damage calculations consume
        // those fields for damage zones and building/material interactions.
        // Only geometry derived from the validated launch and impact is
        // canonicalized here.
        const struct
        {
            FFieldView Field;
            const void* Value;
            uint32 Size;
        } CanonicalFields[] = {
            { GHitResultSchema.Location,
                &ImpactPoint,
                static_cast<uint32>(FVector::Size()) },
            { GHitResultSchema.ImpactPoint,
                &ImpactPoint,
                static_cast<uint32>(FVector::Size()) },
            { GHitResultSchema.TraceStart,
                &ProjectileOrigin,
                static_cast<uint32>(FVector::Size()) },
            { GHitResultSchema.TraceEnd,
                &ImpactPoint,
                static_cast<uint32>(FVector::Size()) },
            { GHitResultSchema.Normal,
                &SafeNormal,
                static_cast<uint32>(FVector::Size()) },
            { GHitResultSchema.ImpactNormal,
                &SafeNormal,
                static_cast<uint32>(FVector::Size()) },
            { GHitResultSchema.Time,
                &Time,
                sizeof(Time) },
            { GHitResultSchema.Distance,
                &Distance,
                sizeof(Distance) },
            { GHitResultSchema.PenetrationDepth,
                &ZeroFloat,
                sizeof(ZeroFloat) },
            { GHitResultSchema.BlockingHit,
                &BlockingHit,
                sizeof(BlockingHit) }
        };
        for (const auto& Field : CanonicalFields)
        {
            if (Field.Field.IsValid(HitSize) &&
                Field.Field.Size == Field.Size)
            {
                std::memcpy(
                    static_cast<uint8*>(HitMemory) +
                        Field.Field.Offset,
                    Field.Value,
                    Field.Size);
            }
        }
    }

    AFortPlayerPawnAthena* AsPlayerPawn(AActor* Actor)
    {
        return IsUsableActor(Actor)
            ? Actor->Cast<AFortPlayerPawnAthena>()
            : nullptr;
    }

    AFortPlayerControllerAthena* AsPlayerController(
        AActor* Actor)
    {
        return IsUsableActor(Actor)
            ? Actor->Cast<AFortPlayerControllerAthena>()
            : nullptr;
    }

    AFortPlayerStateAthena* GetAthenaPlayerState(
        AFortPlayerControllerAthena* Controller)
    {
        if (!IsUsableActor(Controller) ||
            !Controller->HasPlayerState() ||
            !IsUsableActor(Controller->PlayerState))
        {
            return nullptr;
        }

        return Controller->PlayerState
            ->Cast<AFortPlayerStateAthena>();
    }

    AFortPlayerStateAthena* GetAthenaPlayerState(
        AFortPlayerPawnAthena* Pawn)
    {
        if (!IsUsableActor(Pawn))
            return nullptr;

        if (Pawn->HasPlayerState() &&
            IsUsableActor(Pawn->PlayerState))
        {
            auto PlayerState =
                Pawn->PlayerState
                    ->Cast<AFortPlayerStateAthena>();
            if (PlayerState)
                return PlayerState;
        }

        return Pawn->HasController()
            ? GetAthenaPlayerState(
                AsPlayerController(Pawn->Controller))
            : nullptr;
    }

    bool ResolveShooter(
        AFortWeaponRanged* Weapon,
        AFortPlayerPawnAthena*& OutPawn,
        AFortPlayerControllerAthena*& OutController)
    {
        OutPawn = nullptr;
        OutController = nullptr;
        if (!IsUsableActor(Weapon))
            return false;

        AActor* Owner =
            Weapon->HasOwner() ? Weapon->Owner : nullptr;
        AActor* Instigator =
            Weapon->HasInstigator()
                ? Weapon->Instigator
                : nullptr;

        OutPawn = AsPlayerPawn(Instigator);
        if (!OutPawn)
            OutPawn = AsPlayerPawn(Owner);

        OutController = AsPlayerController(Owner);
        if (!OutController &&
            OutPawn &&
            OutPawn->HasController())
        {
            OutController =
                AsPlayerController(OutPawn->Controller);
        }

        if (!OutPawn && OutController)
        {
            if (OutController->HasMyFortPawn())
                OutPawn = OutController->MyFortPawn;
            if (!OutPawn && OutController->HasPawn())
                OutPawn = OutController->Pawn;
        }

        if (!IsUsableActor(OutPawn) ||
            !IsUsableActor(OutController))
        {
            return false;
        }

        if (OutController->HasMyFortPawn() &&
            OutController->MyFortPawn &&
            OutController->MyFortPawn != OutPawn)
        {
            return false;
        }

        return true;
    }

    bool WeaponBelongsToPawn(
        AFortPlayerPawnAthena* Pawn,
        AFortWeaponRanged* Weapon)
    {
        if (!IsUsableActor(Pawn) ||
            !IsUsableActor(Weapon))
        {
            return false;
        }

        if (Pawn->HasCurrentWeapon() &&
            Pawn->CurrentWeapon == Weapon)
        {
            return true;
        }

        if (!Pawn->HasCurrentWeaponList())
            return false;

        const auto& Weapons = Pawn->CurrentWeaponList;
        if (Weapons.Num() < 0 ||
            Weapons.Num() > 64 ||
            (Weapons.Num() > 0 &&
                (!Weapons.Data ||
                    !SDK::MemReadable(
                        Weapons.Data,
                        static_cast<size_t>(Weapons.Num()) *
                            sizeof(AActor*)))))
        {
            return false;
        }

        for (int32 Index = 0; Index < Weapons.Num(); Index++)
        {
            if (Weapons.Get(Index) == Weapon)
                return true;
        }

        return false;
    }

    bool WeaponIsCurrentForPawn(
        AFortPlayerPawnAthena* Pawn,
        AFortWeaponRanged* Weapon)
    {
        if (!IsUsableActor(Pawn) ||
            !IsUsableActor(Weapon))
        {
            return false;
        }

        return Pawn->HasCurrentWeapon()
            ? Pawn->CurrentWeapon == Weapon
            : WeaponBelongsToPawn(Pawn, Weapon);
    }

    bool ResolveReflectedTargetingBool(
        const UStruct* ContainerStruct,
        const char* PropertyName,
        FReflectedBoolField& OutField)
    {
        OutField = FReflectedBoolField{};
        if (!IsUsableObject(ContainerStruct) ||
            !PropertyName ||
            Offsets::FieldMask == 0)
        {
            return false;
        }

        const auto Property =
            ContainerStruct->GetProperty(
                PropertyName,
                0x20000);
        const size_t RequiredMetadataSize =
            (std::max)(
                (std::max)(
                    static_cast<size_t>(
                        Offsets::Offset_Internal) +
                        sizeof(uint32),
                    static_cast<size_t>(
                        Offsets::ElementSize) +
                        sizeof(uint32)),
                static_cast<size_t>(
                    Offsets::FieldMask) +
                    sizeof(uint8));
        if (!Property ||
            !SDK::MemReadable(
                Property,
                RequiredMetadataSize))
        {
            return false;
        }

        const uint32 ElementSize =
            GetFromOffset<uint32>(
                Property,
                Offsets::ElementSize);
        const int32 Offset =
            static_cast<int32>(DecryptPropOffset(
                GetFromOffset<uint32>(
                    Property,
                    Offsets::Offset_Internal)));
        const uint8 Mask = Property->GetFieldMask();
        const bool IsValidBoolMask =
            Mask == 0xFF ||
            (Mask != 0 &&
                (Mask & static_cast<uint8>(Mask - 1)) == 0);
        const int32 ContainerSize =
            ContainerStruct->GetPropertiesSize();
        if (ElementSize != sizeof(bool) ||
            Offset < 0 ||
            ContainerSize <= 0 ||
            ContainerSize > 0x100000 ||
            static_cast<uint64>(Offset) + 1 >
                static_cast<uint32>(ContainerSize) ||
            !IsValidBoolMask)
        {
            return false;
        }

        OutField.Offset = Offset;
        OutField.ContainerSize =
            static_cast<uint32>(ContainerSize);
        OutField.Mask = Mask;
        return true;
    }

    UFunction* ResolveBoolGetter(
        UObject* DefaultObject,
        uint32& OutParamsSize,
        FFieldView& OutReturnValue)
    {
        OutParamsSize = 0;
        OutReturnValue = {};
        if (!IsUsableObject(DefaultObject))
            return nullptr;

        constexpr const char* CandidateNames[] = {
            "GetIsTargeting",
            "IsTargeting"
        };
        for (const char* CandidateName :
            CandidateNames)
        {
            auto Function =
                DefaultObject->GetFunction(
                    CandidateName);
            if (!Function ||
                !Function->ExecFunction)
            {
                continue;
            }

            const auto Params =
                Function->GetParamsNamed();
            const auto ReturnValue =
                GetNamedParameter(
                    Params,
                    "ReturnValue");
            if (Params.Size > 0 &&
                Params.Size <=
                    kMaxFrameSnapshotSize &&
                Params.NameOffsetMap.size() == 1 &&
                ReturnValue.IsValid(Params.Size) &&
                ReturnValue.Size == sizeof(bool))
            {
                OutParamsSize = Params.Size;
                OutReturnValue = ReturnValue;
                return Function;
            }
        }

        return nullptr;
    }

    FTargetingReplicationSchema
        ResolveTargetingReplicationSchema()
    {
        FTargetingReplicationSchema Result{};
        auto WeaponClass = AFortWeapon::StaticClass();
        auto PawnClass =
            AFortPlayerPawnAthena::StaticClass();
        auto DefaultWeapon =
            WeaponClass
                ? WeaponClass->GetDefaultObj()
                : nullptr;
        auto DefaultPawn =
            PawnClass
                ? PawnClass->GetDefaultObj()
                : nullptr;
        auto SharedMovementStruct =
            FindObject<UStruct>(
                L"/Script/FortniteGame.SharedRepMovement");
        if (!DefaultWeapon ||
            !DefaultPawn ||
            !ResolveReflectedTargetingBool(
                PawnClass,
                "bIsTargeting",
                Result.PawnIsTargeting))
        {
            return Result;
        }

        Result.GetIsTargetingFunction =
            ResolveBoolGetter(
                DefaultWeapon,
                Result.GetIsTargetingParamsSize,
                Result.GetIsTargetingReturnValue);

        Result.FastSharedReplicationFunction =
            DefaultPawn->GetFunction(
                "FastSharedReplication");
        if (SharedMovementStruct &&
            Result.FastSharedReplicationFunction &&
            Result.FastSharedReplicationFunction
                ->ExecFunction &&
            ResolveReflectedTargetingBool(
                SharedMovementStruct,
                "bIsTargeting",
                Result.FastSharedMovementIsTargeting))
        {
            const auto FastSharedParams =
                Result.FastSharedReplicationFunction
                    ->GetParamsNamed();
            const auto SharedMovement =
                GetNamedParameter(
                    FastSharedParams,
                    "SharedRepMovement");
            if (FastSharedParams.Size > 0 &&
                FastSharedParams.Size <=
                    kMaxNotifyParamsSize &&
                FastSharedParams
                    .NameOffsetMap.size() == 1 &&
                SharedMovement.IsValid(
                    FastSharedParams.Size) &&
                SharedMovement.Size ==
                    Result
                        .FastSharedMovementIsTargeting
                        .ContainerSize)
            {
                Result.FastSharedReplicationParamsSize =
                    FastSharedParams.Size;
                Result.FastSharedMovement =
                    SharedMovement;
            }
        }

        auto SetTargeting =
            DefaultWeapon->GetFunction(
                "SetTargeting");
        if (SetTargeting &&
            SetTargeting->ExecFunction)
        {
            const auto Params =
                SetTargeting->GetParamsNamed();
            if (Params.Size > 0 &&
                Params.Size <=
                    kMaxFrameSnapshotSize &&
                Params.NameOffsetMap.size() == 1)
            {
                const auto& Parameter =
                    Params.NameOffsetMap[0];
                FFieldView Value{
                    static_cast<int32>(
                        Parameter.Offset),
                    Parameter.ElementSize
                };
                if (Value.IsValid(Params.Size) &&
                    Value.Size == sizeof(bool))
                {
                    Result.SetTargetingFunction =
                        SetTargeting;
                    Result.SetTargetingParamsSize =
                        Params.Size;
                    Result.SetTargetingValue = Value;
                }
            }
        }

        return Result;
    }

    bool ReadWeaponIsTargeting(
        AFortWeaponRanged* Weapon,
        bool& OutIsTargeting)
    {
        OutIsTargeting = false;
        if (!IsUsableActor(Weapon) ||
            !GTargetingReplicationSchema.CanPoll())
        {
            return false;
        }

        alignas(16) std::array<
            uint8,
            kMaxFrameSnapshotSize> Params{};
        Weapon->ProcessEvent(
            GTargetingReplicationSchema
                .GetIsTargetingFunction,
            Params.data());
        return ReadValue(
            Params.data(),
            GTargetingReplicationSchema
                .GetIsTargetingParamsSize,
            GTargetingReplicationSchema
                .GetIsTargetingReturnValue,
            OutIsTargeting);
    }

    bool ReadReflectedBoolField(
        const void* Container,
        const FReflectedBoolField& Field,
        bool& OutValue)
    {
        OutValue = false;
        if (!Container || !Field.IsValid())
            return false;

        const auto Address =
            static_cast<const uint8*>(Container) +
            Field.Offset;
        if (!SDK::MemReadable(Address, 1))
            return false;

        OutValue = (*Address & Field.Mask) != 0;
        return true;
    }

    bool WriteReflectedBoolField(
        void* Container,
        const FReflectedBoolField& Field,
        bool Value)
    {
        if (!Container || !Field.IsValid())
            return false;

        auto Address =
            static_cast<uint8*>(Container) +
            Field.Offset;
        if (!SDK::MemReadable(Address, 1))
            return false;

        if (Value)
            *Address |= Field.Mask;
        else
            *Address &= static_cast<uint8>(~Field.Mask);
        return true;
    }

    bool OwnsCompatibilityTargetingAssertion(
        AFortPlayerPawnAthena* Pawn)
    {
        if (!IsUsableActor(Pawn))
            return false;

        const int32 ObjectIndex =
            static_cast<int32>(Pawn->Index);
        for (auto& Assertion :
            GCompatibilityTargetingAssertions)
        {
            if (Assertion.Pawn != Pawn)
                continue;
            if (Assertion.ObjectIndex == ObjectIndex)
                return true;

            // A UObject slot can be recycled after a pawn is destroyed.
            // Never let ownership from the old object carry into the new one.
            Assertion = {};
            return false;
        }
        return false;
    }

    void SetCompatibilityTargetingAssertionOwned(
        AFortPlayerPawnAthena* Pawn,
        bool Owned)
    {
        if (!Pawn)
            return;

        const int32 ObjectIndex =
            IsUsableActor(Pawn)
                ? static_cast<int32>(Pawn->Index)
                : -1;
        for (auto& Assertion :
            GCompatibilityTargetingAssertions)
        {
            if (Assertion.Pawn != Pawn)
                continue;

            if (!Owned)
            {
                Assertion = {};
                return;
            }
            if (Assertion.ObjectIndex == ObjectIndex)
                return;

            Assertion = {};
            break;
        }

        if (!Owned || ObjectIndex < 0)
            return;

        FCompatibilityTargetingAssertion* Slot = nullptr;
        for (auto& Assertion :
            GCompatibilityTargetingAssertions)
        {
            if (!Assertion.Pawn)
            {
                Slot = &Assertion;
                break;
            }
        }
        if (!Slot)
        {
            Slot = &GCompatibilityTargetingAssertions[
                GCompatibilityTargetingAssertionCursor++ %
                GCompatibilityTargetingAssertions.size()];
        }

        Slot->Pawn = Pawn;
        Slot->ObjectIndex = ObjectIndex;
    }

    bool SetPawnIsTargeting(
        AFortPlayerPawnAthena* Pawn,
        bool IsTargeting,
        ETargetingMirrorSource Source)
    {
        const auto& Field =
            GTargetingReplicationSchema
                .PawnIsTargeting;
        if (!IsUsableActor(Pawn) ||
            !Pawn->HasAuthority() ||
            !Field.IsValid())
        {
            return false;
        }

        bool Previous = false;
        if (!ReadReflectedBoolField(
                Pawn,
                Field,
                Previous))
        {
            return false;
        }

        const bool OwnsAssertion =
            OwnsCompatibilityTargetingAssertion(Pawn);
        if (!IsTargeting &&
            Source == ETargetingMirrorSource::Poll &&
            !OwnsAssertion)
        {
            // bIsTargeting is also written by native pawn state. A sampled
            // weapon false is not sufficient authority to erase a native
            // assertion that this compatibility path did not create.
            return false;
        }

        if (Previous == IsTargeting)
        {
            if (!IsTargeting)
            {
                SetCompatibilityTargetingAssertionOwned(
                    Pawn,
                    false);
            }
            return false;
        }

        if (!WriteReflectedBoolField(
                Pawn,
                Field,
                IsTargeting))
        {
            return false;
        }

        SetCompatibilityTargetingAssertionOwned(
            Pawn,
            IsTargeting);
        Pawn->FlushNetDormancy();
        Pawn->ForceNetUpdate();
        return true;
    }

    void MirrorWeaponTargeting(
        AFortWeaponRanged* Weapon,
        bool IsTargeting)
    {
        if (!IsUsableActor(Weapon) ||
            !Weapon->HasAuthority())
        {
            return;
        }

        AFortPlayerPawnAthena* Pawn = nullptr;
        AFortPlayerControllerAthena* Controller =
            nullptr;
        if (ResolveShooter(
                Weapon,
                Pawn,
                Controller) &&
            WeaponIsCurrentForPawn(
                Pawn,
                Weapon))
        {
            SetPawnIsTargeting(
                Pawn,
                IsTargeting,
                ETargetingMirrorSource::
                    DecodedTransition);
        }
    }

    void SetTargetingHook(
        UObject* Context,
        FFrame& Stack)
    {
        auto Weapon =
            IsUsableObject(Context)
                ? Context->Cast<
                    AFortWeaponRanged>()
                : nullptr;
        bool IsTargeting = false;
        bool Decoded = false;
        if (Weapon &&
            GTargetingReplicationSchema
                .CanHookSetTargeting())
        {
            alignas(16) std::array<
                uint8,
                kMaxFrameSnapshotSize> DecodeStorage{};
            FFrame* DecodeStack = nullptr;
            Decoded =
                SnapshotFrameForDecode(
                    Stack,
                    DecodeStorage,
                    DecodeStack) &&
                DecodeFrameReference(
                    *DecodeStack,
                    &IsTargeting,
                    sizeof(IsTargeting));
        }

        if (GSetTargetingOriginal)
            GSetTargetingOriginal(Context, Stack);

        if (Decoded)
        {
            MirrorWeaponTargeting(
                Weapon,
                IsTargeting);
        }
    }

    bool ReadPawnAuthoritativeTargeting(
        AFortPlayerPawnAthena* Pawn,
        bool& OutIsTargeting)
    {
        OutIsTargeting = false;
        if (!IsUsableActor(Pawn) ||
            !Pawn->HasAuthority())
        {
            return false;
        }

        bool PawnIsTargeting = false;
        const bool ReadPawnState =
            ReadReflectedBoolField(
                Pawn,
                GTargetingReplicationSchema
                    .PawnIsTargeting,
                PawnIsTargeting);
        if (ReadPawnState &&
            PawnIsTargeting &&
            !OwnsCompatibilityTargetingAssertion(Pawn))
        {
            // Native pawn targeting is authoritative. In particular, do not
            // replace this true with a weapon getter that is one frame stale.
            OutIsTargeting = true;
            return true;
        }

        if (!Pawn->HasCurrentWeapon())
        {
            OutIsTargeting = false;
            return true;
        }

        auto CurrentWeapon =
            IsUsableActor(Pawn->CurrentWeapon)
                ? Pawn->CurrentWeapon->Cast<
                    AFortWeaponRanged>()
                : nullptr;
        if (!CurrentWeapon)
        {
            OutIsTargeting = false;
            return true;
        }
        if (ReadWeaponIsTargeting(
                CurrentWeapon,
                OutIsTargeting))
        {
            return true;
        }

        OutIsTargeting = PawnIsTargeting;
        return ReadPawnState;
    }

    bool PatchFastSharedTargeting(
        AFortPlayerPawnAthena* Pawn,
        void* SharedMovement)
    {
        bool IncomingIsTargeting = false;
        if (!ReadReflectedBoolField(
                SharedMovement,
                GTargetingReplicationSchema
                    .FastSharedMovementIsTargeting,
                IncomingIsTargeting))
        {
            return false;
        }

        // The movement payload is built by native pawn code and is the newest
        // source at this call site. Compatibility may assert a missing true,
        // but must never erase an incoming true with a stale weapon false.
        if (IncomingIsTargeting)
            return true;

        bool IsTargeting = false;
        if (!ReadPawnAuthoritativeTargeting(
                Pawn,
                IsTargeting) ||
            !IsTargeting)
        {
            return false;
        }

        return WriteReflectedBoolField(
            SharedMovement,
            GTargetingReplicationSchema
                .FastSharedMovementIsTargeting,
            true);
    }

    void FastSharedReplicationHook(
        UObject* Context,
        FFrame& Stack)
    {
        auto Pawn =
            IsUsableObject(Context)
                ? Context->Cast<
                    AFortPlayerPawnAthena>()
                : nullptr;
        if (Pawn &&
            GTargetingReplicationSchema
                .CanPatchFastSharedMovement() &&
            !Stack.Code &&
            Stack.Locals &&
            SDK::MemReadable(
                Stack.Locals,
                GTargetingReplicationSchema
                    .FastSharedReplicationParamsSize))
        {
            auto SharedMovement =
                Stack.Locals +
                GTargetingReplicationSchema
                    .FastSharedMovement.Offset;
            PatchFastSharedTargeting(
                Pawn,
                SharedMovement);
        }

        if (GFastSharedReplicationOriginal)
        {
            GFastSharedReplicationOriginal(
                Context,
                Stack);
        }
    }

    bool EnsureTargetingReplication()
    {
        if (VersionInfo.FortniteVersion < 24.00)
            return false;

        // The pawn ProcessEvent interception is the part that patches the
        // multicast parameter buffer before Unreal routes it to observers.
        // An ExecFunction hook alone runs after that routing point, so a
        // resolved schema is not sufficient to declare remote glint ready.
        if (GTargetingReplicationResolved &&
            GPawnProcessEventOriginal)
        {
            return GTargetingReplicationSchema
                .CanPatchFastSharedMovement();
        }

        if (GTargetingReplicationRetryTicks > 0)
        {
            --GTargetingReplicationRetryTicks;
            return false;
        }

        if (!GTargetingReplicationResolved)
        {
            const auto ResolvedSchema =
                ResolveTargetingReplicationSchema();
            if (!ResolvedSchema.CanPatchFastSharedMovement())
            {
                GTargetingReplicationRetryTicks = 60;
                if (GTargetingReplicationFailureLogs++ < 3)
                {
                    SDK::DbgLog(
                        "  [SniperGlint] waiting for the Chapter 5 targeting/fast-shared schema on %.2f\n",
                        VersionInfo.FortniteVersion);
                }
                return false;
            }

            GTargetingReplicationSchema =
                ResolvedSchema;
            GTargetingReplicationResolved = true;

            if (GTargetingReplicationSchema
                    .CanHookSetTargeting())
            {
                Utils::ExecHook(
                    GTargetingReplicationSchema
                        .SetTargetingFunction,
                    SetTargetingHook,
                    GSetTargetingOriginal);
                GTargetingSetHookInstalled =
                    GSetTargetingOriginal != nullptr;
            }
            if (GTargetingReplicationSchema
                    .CanPatchFastSharedMovement())
            {
                // Keep the Exec hook as a local/native fallback, but do not
                // use it as the readiness criterion for remote observers.
                Utils::ExecHook(
                    GTargetingReplicationSchema
                        .FastSharedReplicationFunction,
                    FastSharedReplicationHook,
                    GFastSharedReplicationOriginal);
                GTargetingFastSharedHookInstalled =
                    GFastSharedReplicationOriginal != nullptr;
            }
        }

        if (!GPawnProcessEventOriginal &&
            Offsets::ProcessEventVft > 0 &&
            Offsets::ProcessEventVft < 0x1000)
        {
            Utils::Hook<AFortPlayerPawnAthena>(
                static_cast<uint32>(
                    Offsets::ProcessEventVft),
                PawnProcessEventDamageFeedback,
                GPawnProcessEventOriginal);
        }

        if (!GPawnProcessEventOriginal)
        {
            GTargetingReplicationRetryTicks = 30;
            if (GTargetingProcessEventFailureLogs++ < 4)
            {
                SDK::DbgLog(
                    "  [SniperGlint] waiting for pre-serialization pawn ProcessEvent interception (vft=%lld) on %.2f\n",
                    static_cast<long long>(
                        Offsets::ProcessEventVft),
                    VersionInfo.FortniteVersion);
            }
            return false;
        }

        if (!GTargetingReplicationReadyLogged)
        {
            GTargetingReplicationReadyLogged = true;
            SDK::DbgLog(
                "  [SniperGlint] targeting replication enabled on %.2f (transition-hook=%s fast-shared=rpc)\n",
                VersionInfo.FortniteVersion,
                GTargetingSetHookInstalled
                    ? "yes"
                    : "poll");
        }
        return true;
    }

    AFortPlayerPawnAthena* ResolveControllerPawn(
        AFortPlayerControllerAthena* Controller)
    {
        if (!IsUsableActor(Controller))
            return nullptr;

        AFortPlayerPawnAthena* Pawn = nullptr;
        if (Controller->HasMyFortPawn())
            Pawn = Controller->MyFortPawn;
        if (!Pawn && Controller->HasPawn())
            Pawn = AsPlayerPawn(Controller->Pawn);
        return IsUsableActor(Pawn)
            ? Pawn
            : nullptr;
    }

    void SyncControllerTargeting(
        AFortPlayerControllerAthena* Controller)
    {
        auto Pawn =
            ResolveControllerPawn(Controller);
        if (!Pawn ||
            !Pawn->HasAuthority())
        {
            return;
        }
        bool IsTargeting = false;
        if (!ReadPawnAuthoritativeTargeting(
                Pawn,
                IsTargeting))
        {
            return;
        }

        // Fast shared movement serializes this transient pawn bit. It is not
        // a normal replicated property, so ForceNetUpdate alone was never
        // enough to make remote optic glints react.
        SetPawnIsTargeting(
            Pawn,
            IsTargeting,
            ETargetingMirrorSource::Poll);
    }

    void SyncTargetingControllerArray(
        const TArray<AActor*>& Controllers)
    {
        if (Controllers.Num() < 0 ||
            Controllers.Num() > 512 ||
            (Controllers.Num() > 0 &&
                (!Controllers.Data ||
                    !SDK::MemReadable(
                        Controllers.Data,
                        static_cast<size_t>(
                            Controllers.Num()) *
                            sizeof(AActor*)))))
        {
            return;
        }

        for (int32 Index = 0;
            Index < Controllers.Num();
            Index++)
        {
            auto ControllerActor =
                Controllers.Get(Index);
            auto Controller =
                IsUsableActor(ControllerActor)
                    ? ControllerActor->Cast<
                        AFortPlayerControllerAthena>()
                    : nullptr;
            if (Controller)
                SyncControllerTargeting(Controller);
        }
    }

    void TickTargetingReplication()
    {
        if (!EnsureTargetingReplication())
            return;

        // SetTargeting is the authoritative transition and is already hooked
        // on FN30. Polling every player and bot at 20 Hz only duplicates that
        // work; retain polling solely as the fallback for builds where the
        // transition function cannot be hooked.
        if (GTargetingSetHookInstalled)
            return;

        auto World = UWorld::GetWorld();
        auto GameMode =
            World &&
                IsUsableActor(
                    World->AuthorityGameMode)
                ? World->AuthorityGameMode
                    ->Cast<AFortGameMode>()
                : nullptr;
        if (!GameMode)
            return;

        const double Now =
            UGameplayStatics::GetTimeSeconds(World);
        constexpr double PollIntervalSeconds = 0.05;
        if (GLastTargetingPollAt >= 0.0 &&
            Now >= GLastTargetingPollAt &&
            Now - GLastTargetingPollAt <
                PollIntervalSeconds)
        {
            return;
        }
        GLastTargetingPollAt = Now;

        if (GameMode->HasAlivePlayers())
        {
            SyncTargetingControllerArray(
                GameMode->AlivePlayers);
        }
        if (GameMode->HasAliveBots())
        {
            SyncTargetingControllerArray(
                GameMode->AliveBots);
        }
    }

    bool IsAutomaticWeapon(
        AFortWeaponRanged* Weapon)
    {
        if (!IsUsableActor(Weapon) ||
            !Weapon->HasWeaponData() ||
            !IsUsableObject(Weapon->WeaponData))
        {
            return false;
        }

        const auto Property =
            Weapon->WeaponData->GetProperty("TriggerType");
        if (!Property ||
            !SDK::MemReadable(
                Property,
                static_cast<size_t>((std::max)(
                    Offsets::ElementSize,
                    Offsets::Offset_Internal)) +
                    sizeof(uint32)) ||
            GetFromOffset<uint32>(
                Property,
                Offsets::ElementSize) != sizeof(uint8))
        {
            return false;
        }

        const int32 Offset =
            static_cast<int32>(DecryptPropOffset(
                GetFromOffset<uint32>(
                    Property,
                    Offsets::Offset_Internal)));
        if (Offset < 0 ||
            !SDK::MemReadable(
                reinterpret_cast<const uint8*>(
                    Weapon->WeaponData) +
                    Offset,
                sizeof(uint8)))
        {
            return false;
        }

        uint8 TriggerType = 0;
        std::memcpy(
            &TriggerType,
            reinterpret_cast<const uint8*>(
                Weapon->WeaponData) +
                Offset,
            sizeof(TriggerType));
        return TriggerType == 1;
    }

    double GetWeaponShotInterval(
        AFortWeaponRanged* Weapon)
    {
        float FiringRate = 10.f;
        if (Weapon &&
            Weapon->HasWeaponData() &&
            IsUsableObject(Weapon->WeaponData))
        {
            auto Stats =
                AFortInventory::GetStats(
                    Weapon->WeaponData);
            if (Stats &&
                FFortRangedWeaponStats::HasFiringRate())
            {
                const float ReportedRate =
                    Stats->FiringRate;
                if (std::isfinite(ReportedRate) &&
                    ReportedRate > 0.f &&
                    ReportedRate <= 100.f)
                {
                    FiringRate = ReportedRate < 1.f
                        ? 1.f / ReportedRate
                        : ReportedRate;
                }
            }
        }

        return 1.0 /
            std::clamp(
                static_cast<double>(FiringRate),
                1.0,
                100.0);
    }

    int32 GetBulletsPerCartridge(
        AFortWeaponRanged* Weapon)
    {
        if (!Weapon ||
            !Weapon->HasWeaponData() ||
            !IsUsableObject(Weapon->WeaponData))
        {
            return 1;
        }

        auto Stats =
            AFortInventory::GetStats(
                Weapon->WeaponData);
        if (!Stats ||
            !FFortRangedWeaponStats::
                HasBulletsPerCartridge())
        {
            return 1;
        }

        return std::clamp(
            Stats->BulletsPerCartridge,
            1,
            64);
    }

    bool ReadReflectedByte(
        UObject* Object,
        const char* PropertyName,
        uint8& OutValue)
    {
        if (!IsUsableObject(Object) || !PropertyName)
            return false;

        const auto Property =
            Object->GetProperty(PropertyName);
        if (!Property ||
            !SDK::MemReadable(
                Property,
                static_cast<size_t>((std::max)(
                    Offsets::ElementSize,
                    Offsets::Offset_Internal)) +
                    sizeof(uint32)) ||
            GetFromOffset<uint32>(
                Property,
                Offsets::ElementSize) != sizeof(uint8))
        {
            return false;
        }

        const int32 Offset =
            static_cast<int32>(DecryptPropOffset(
                GetFromOffset<uint32>(
                    Property,
                    Offsets::Offset_Internal)));
        if (Offset < 0 ||
            !SDK::MemReadable(
                reinterpret_cast<const uint8*>(Object) +
                    Offset,
                sizeof(OutValue)))
        {
            return false;
        }

        std::memcpy(
            &OutValue,
            reinterpret_cast<const uint8*>(Object) +
                Offset,
            sizeof(OutValue));
        return true;
    }

    bool MatchAllowsDamage(
        AFortPlayerControllerAthena* ShooterController)
    {
        auto World = UWorld::GetWorld();
        if (!IsUsableObject(World) ||
            !World->HasGameState() ||
            !IsUsableActor(World->GameState) ||
            !IsUsableActor(ShooterController))
        {
            return false;
        }

        const bool MatchStarted =
            GUI::gsStatus == StartedMatch;
        const bool ShooterInAircraft =
            ShooterController->IsInAircraft();
        if (!MatchStarted || ShooterInAircraft)
            return false;

        auto GameState =
            World->GameState->Cast<AFortGameStateAthena>();
        if (!IsUsableActor(GameState))
        {
            return false;
        }

        uint8 LegacyPhase =
            static_cast<uint8>(EAthenaGamePhase::Count);
        uint8 LegacyStep =
            static_cast<uint8>(EAthenaGamePhaseStep::Count);
        const bool HasLegacyPhase =
            ReadReflectedByte(
                GameState,
                "GamePhase",
                LegacyPhase);
        ReadReflectedByte(
            GameState,
            "GamePhaseStep",
            LegacyStep);

        uint8 Phase = LegacyPhase;
        uint8 Step = LegacyStep;
        const char* Source = "legacy";
        auto PhaseLogic =
            VersionInfo.FortniteVersion >= 25.20
                ? UFortGameStateComponent_BattleRoyaleGamePhaseLogic::
                    Get(GameState)
                : nullptr;
        uint8 ComponentPhase = 0;
        uint8 ComponentStep = 0;
        if (IsUsableObject(PhaseLogic) &&
            ReadReflectedByte(
                PhaseLogic,
                "GamePhase",
                ComponentPhase))
        {
            Phase = ComponentPhase;
            Source = "component";
            if (ReadReflectedByte(
                    PhaseLogic,
                    "GamePhaseStep",
                    ComponentStep))
            {
                Step = ComponentStep;
            }
        }
        else if (!HasLegacyPhase)
        {
            static int32 MissingPhaseTraceCount = 0;
            if (MissingPhaseTraceCount++ < 8)
            {
                SDK::DbgLog(
                    "  [ProjectileDamage] phase-state source=unavailable status=%d late-game=%d in-aircraft=%d allowed=0\n",
                    static_cast<int32>(GUI::gsStatus),
                    FConfiguration::bLateGame.load(),
                    ShooterInAircraft);
            }
            return false;
        }

        const auto TypedPhase =
            static_cast<EAthenaGamePhase>(Phase);
        const bool Allowed =
            (FConfiguration::bLateGame &&
                TypedPhase != EAthenaGamePhase::EndGame &&
                TypedPhase != EAthenaGamePhase::Count) ||
            TypedPhase == EAthenaGamePhase::None ||
            TypedPhase == EAthenaGamePhase::Aircraft ||
            TypedPhase == EAthenaGamePhase::SafeZones;

        static int32 PhaseTraceCount = 0;
        if (PhaseTraceCount++ < 32)
        {
            SDK::DbgLog(
                "  [ProjectileDamage] phase-state source=%s phase=%u step=%u legacy-phase=%u legacy-step=%u status=%d late-game=%d in-aircraft=%d allowed=%d\n",
                Source,
                static_cast<uint32>(Phase),
                static_cast<uint32>(Step),
                static_cast<uint32>(LegacyPhase),
                static_cast<uint32>(LegacyStep),
                static_cast<int32>(GUI::gsStatus),
                FConfiguration::bLateGame.load(),
                ShooterInAircraft,
                Allowed);
        }
        return Allowed;
    }

    bool AreEnemies(
        AFortPlayerControllerAthena* Shooter,
        AFortPlayerPawnAthena* TargetPawn)
    {
        if (!IsUsableActor(Shooter) ||
            !IsUsableActor(TargetPawn))
        {
            return false;
        }

        auto ShooterState =
            GetAthenaPlayerState(Shooter);
        auto TargetState =
            GetAthenaPlayerState(TargetPawn);
        if (!ShooterState ||
            !TargetState ||
            !ShooterState->HasTeamIndex() ||
            !TargetState->HasTeamIndex())
        {
            return false;
        }

        const uint8 ShooterTeam = ShooterState->TeamIndex;
        const uint8 TargetTeam = TargetState->TeamIndex;
        return ShooterTeam != 0 &&
            ShooterTeam != 0xff &&
            TargetTeam != 0 &&
            TargetTeam != 0xff &&
            ShooterTeam != TargetTeam;
    }

    UAbilitySystemComponent* GetPlayerAbilitySystem(
        AFortPlayerControllerAthena* Controller)
    {
        auto PlayerState =
            GetAthenaPlayerState(Controller);
        if (!PlayerState ||
            !PlayerState->HasAbilitySystemComponent())
        {
            return nullptr;
        }

        auto AbilitySystem =
            PlayerState->AbilitySystemComponent;
        return IsUsableObject(AbilitySystem) &&
            AbilitySystem->IsA(
                UAbilitySystemComponent::StaticClass())
            ? AbilitySystem
            : nullptr;
    }

    UAbilitySystemComponent* GetPlayerAbilitySystem(
        AFortPlayerPawnAthena* Pawn)
    {
        auto PlayerState =
            GetAthenaPlayerState(Pawn);
        if (!PlayerState ||
            !PlayerState->HasAbilitySystemComponent())
        {
            return nullptr;
        }

        auto AbilitySystem =
            PlayerState->AbilitySystemComponent;
        return IsUsableObject(AbilitySystem) &&
            AbilitySystem->IsA(
                UAbilitySystemComponent::StaticClass())
            ? AbilitySystem
            : nullptr;
    }

    UAbilitySystemComponent* GetActorAbilitySystem(
        AActor* Actor)
    {
        if (!GEffectApiSchema.IsValid() ||
            !IsUsableActor(Actor))
        {
            return nullptr;
        }

        alignas(16) std::array<uint8, 0x80> Params{};
        if (GEffectApiSchema.GetAbilitySystemSize >
            Params.size())
        {
            return nullptr;
        }

        if (!WriteValue(
                Params.data(),
                GEffectApiSchema.GetAbilitySystemSize,
                GEffectApiSchema.GetAbilitySystemActor,
                Actor))
        {
            return nullptr;
        }

        GEffectApiSchema.BlueprintLibrary->ProcessEvent(
            GEffectApiSchema.GetAbilitySystem,
            Params.data());

        UAbilitySystemComponent* Result = nullptr;
        if (ReadValue(
                Params.data(),
                GEffectApiSchema.GetAbilitySystemSize,
                GEffectApiSchema.GetAbilitySystemReturn,
                Result) &&
            IsUsableObject(Result) &&
            Result->IsA(
                UAbilitySystemComponent::StaticClass()))
        {
            return Result;
        }

        // Some C5 building actors keep a lazily-created ASC in a reflected
        // field but do not advertise it through AbilitySystemInterface until
        // their building attribute set has been requested.
        const char* AbilitySystemFields[] = {
            "AbilitySystemComponent",
            "ReplicatedAbilitySystemComponent"
        };
        for (const char* FieldName : AbilitySystemFields)
        {
            auto Property = Actor->GetProperty(FieldName);
            if (!Property ||
                GetFromOffset<uint32>(
                    Property,
                    Offsets::ElementSize) !=
                    sizeof(UAbilitySystemComponent*))
            {
                continue;
            }

            const int32 Offset =
                static_cast<int32>(DecryptPropOffset(
                    GetFromOffset<uint32>(
                        Property,
                        Offsets::Offset_Internal)));
            UAbilitySystemComponent* Direct = nullptr;
            if (Offset < 0 ||
                !SDK::MemReadable(
                    reinterpret_cast<const uint8*>(
                        Actor) +
                        Offset,
                    sizeof(Direct)))
            {
                continue;
            }
            std::memcpy(
                &Direct,
                reinterpret_cast<const uint8*>(Actor) +
                    Offset,
                sizeof(Direct));
            if (IsUsableObject(Direct) &&
                Direct->IsA(
                    UAbilitySystemComponent::StaticClass()))
            {
                return Direct;
            }
        }

        return nullptr;
    }

    bool ResolveFireAbility(
        AFortWeaponRanged* Weapon,
        UAbilitySystemComponent* AbilitySystem,
        bool UseImpactAbility,
        UFortGameplayAbility*& OutAbility,
        int32& OutLevel)
    {
        OutAbility = nullptr;
        OutLevel = 1;
        if (!IsUsableActor(Weapon) ||
            !IsUsableObject(AbilitySystem) ||
            !AbilitySystem->HasActivatableAbilities())
        {
            return false;
        }

        int32 WantedHandle = -1;
        if (UseImpactAbility)
        {
            if (!Weapon->HasImpactAbilitySpecHandle())
                return false;
            WantedHandle =
                Weapon->ImpactAbilitySpecHandle.Handle;
        }
        else
        {
            if (!Weapon->HasPrimaryAbilitySpecHandle())
                return false;
            WantedHandle =
                Weapon->PrimaryAbilitySpecHandle.Handle;
        }
        if (WantedHandle <= 0)
            return false;

        auto& Specs =
            AbilitySystem->ActivatableAbilities.Items;
        const int32 SpecSize =
            FGameplayAbilitySpec::Size();
        if (SpecSize <= 0 ||
            SpecSize > 0x400 ||
            Specs.Num() < 0 ||
            Specs.Num() > 2048 ||
            (Specs.Num() > 0 &&
                (!Specs.Data ||
                    !SDK::MemReadable(
                        Specs.Data,
                        static_cast<size_t>(Specs.Num()) *
                            SpecSize))))
        {
            return false;
        }

        for (int32 Index = 0; Index < Specs.Num(); Index++)
        {
            auto& Spec = Specs.Get(Index, SpecSize);
            if (!Spec.HasHandle() ||
                Spec.Handle.Handle != WantedHandle ||
                !Spec.HasAbility() ||
                !IsUsableObject(Spec.Ability) ||
                !Spec.Ability->IsA(
                    UFortGameplayAbility::StaticClass()))
            {
                continue;
            }

            OutAbility = Spec.Ability;
            if (Spec.HasLevel() &&
                Spec.Level > 0 &&
                Spec.Level <= 1000)
            {
                OutLevel = Spec.Level;
            }
            return true;
        }

        return false;
    }

    enum class EEffectArrayReadResult
    {
        Empty,
        Populated,
        Invalid
    };

    EEffectArrayReadResult ReadEffectClassArray(
        const uint8* ContainerMemory,
        std::array<UClass*, kMaxEffectsPerContainer>&
            OutClasses,
        int32& OutCount)
    {
        OutCount = 0;
        TArray<UClass*> Classes{};
        if (!ReadValue(
                ContainerMemory,
                GEffectApiSchema.EffectContainerSize,
                GEffectApiSchema.ContainerTargetEffects,
                Classes))
        {
            return EEffectArrayReadResult::Invalid;
        }

        if (Classes.Num() == 0)
            return EEffectArrayReadResult::Empty;

        if (Classes.Num() < 0 ||
            Classes.Num() > kMaxEffectsPerContainer ||
            !Classes.Data ||
            !SDK::MemReadable(
                Classes.Data,
                static_cast<size_t>(Classes.Num()) *
                    sizeof(UClass*)))
        {
            return EEffectArrayReadResult::Invalid;
        }

        for (int32 Index = 0; Index < Classes.Num(); Index++)
        {
            auto EffectClass = Classes.Get(Index);
            if (!IsUsableObject(EffectClass) ||
                !EffectClass->IsA(UClass::StaticClass()))
            {
                return EEffectArrayReadResult::Invalid;
            }

            auto EffectDefault = EffectClass->GetDefaultObj();
            if (!IsUsableObject(EffectDefault) ||
                !EffectDefault->IsA(
                    UGameplayEffect::StaticClass()))
            {
                return EEffectArrayReadResult::Invalid;
            }

            OutClasses[OutCount++] = EffectClass;
        }
        return EEffectArrayReadResult::Populated;
    }

    bool ResolveDamageEffectsUncached(
        UFortGameplayAbility* Ability,
        bool EnvironmentTarget,
        FDamageEffectSet& OutEffects)
    {
        if (!IsUsableObject(Ability) ||
            !GEffectApiSchema.IsValid())
        {
            return false;
        }

        const auto EffectContainersProperty =
            Ability->GetProperty("EffectContainers");
        if (!EffectContainersProperty ||
            Offsets::ElementSize < sizeof(int32) ||
            !SDK::MemReadable(
                EffectContainersProperty,
                Offsets::Offset_Internal +
                    sizeof(uint32)))
        {
            return false;
        }

        const int32 ContainersOffset =
            static_cast<int32>(DecryptPropOffset(
                GetFromOffset<uint32>(
                    EffectContainersProperty,
                    Offsets::Offset_Internal)));
        const uint32 ContainerSize =
            GetFromOffset<uint32>(
                EffectContainersProperty,
                Offsets::ElementSize);
        const int32 ArrayDimension =
            GetFromOffset<int32>(
                EffectContainersProperty,
                Offsets::ElementSize - sizeof(int32));
        if (ContainersOffset < 0 ||
            ContainerSize !=
                GEffectApiSchema.EffectContainerSize ||
            ArrayDimension !=
                kExpectedEffectContainerCount ||
            !SDK::MemReadable(
                reinterpret_cast<const uint8*>(Ability) +
                    ContainersOffset,
                static_cast<size_t>(ContainerSize) *
                    ArrayDimension))
        {
            return false;
        }

        FDamageEffectSet Aggregated{};
        for (int32 ContainerIndex = 0;
            ContainerIndex < ArrayDimension;
            ContainerIndex++)
        {
            const auto ContainerMemory =
                reinterpret_cast<const uint8*>(Ability) +
                ContainersOffset +
                static_cast<size_t>(ContainerIndex) *
                    ContainerSize;

            std::array<
                UClass*,
                kMaxEffectsPerContainer> CandidateClasses{};
            int32 CandidateCount = 0;
            const auto ReadResult = ReadEffectClassArray(
                ContainerMemory,
                CandidateClasses,
                CandidateCount);
            if (ReadResult == EEffectArrayReadResult::Invalid)
                return false;
            if (ReadResult == EEffectArrayReadResult::Empty)
            {
                continue;
            }

            for (int32 EffectIndex = 0;
                EffectIndex < CandidateCount;
                EffectIndex++)
            {
                auto EffectClass =
                    CandidateClasses[EffectIndex];
                bool Duplicate = false;
                for (int32 ExistingIndex = 0;
                    ExistingIndex < Aggregated.Count;
                    ExistingIndex++)
                {
                    if (Aggregated.Classes[ExistingIndex] ==
                        EffectClass)
                    {
                        Duplicate = true;
                        break;
                    }
                }
                if (Duplicate)
                    continue;

                if (Aggregated.Count >=
                    static_cast<int32>(
                        Aggregated.Classes.size()))
                {
                    return false;
                }
                Aggregated.Classes[Aggregated.Count++] =
                    EffectClass;
                SDK::DbgLog(
                    "  [ProjectileDamage] candidate ability=%s container=%d effect=%s target=%s\n",
                    Ability->Name.ToString().c_str(),
                    ContainerIndex,
                    EffectClass->Name.ToString().c_str(),
                    EnvironmentTarget
                        ? "environment"
                        : "pawn");
            }
        }

        if (Aggregated.Count <= 0)
        {
            return false;
        }

        Aggregated.ContainerIndex = -1;
        OutEffects = Aggregated;
        SDK::DbgLog(
            "  [ProjectileDamage] selected ability=%s effects=%d target=%s\n",
            Ability->Name.ToString().c_str(),
            Aggregated.Count,
            EnvironmentTarget ? "environment" : "pawn");
        return true;
    }

    bool ResolveDamageEffects(
        UFortGameplayAbility* Ability,
        bool EnvironmentTarget,
        FDamageEffectSet& OutEffects)
    {
        for (auto& Entry : GEffectCache)
        {
            if (!Entry.Resolved ||
                Entry.EnvironmentTarget != EnvironmentTarget ||
                ResolveWeakObject(Entry.Ability) != Ability)
            {
                continue;
            }

            OutEffects.Count = Entry.Count;
            OutEffects.ContainerIndex =
                Entry.ContainerIndex;
            for (int32 Index = 0;
                Index < Entry.Count;
                Index++)
            {
                auto Effect =
                    ResolveWeakObject(
                        Entry.Effects[Index]);
                if (!IsUsableObject(Effect))
                {
                    Entry.Resolved = false;
                    return ResolveDamageEffects(
                        Ability,
                        EnvironmentTarget,
                        OutEffects);
                }
                OutEffects.Classes[Index] = Effect;
            }
            return Entry.Count > 0;
        }

        FDamageEffectSet Resolved{};
        const bool Success =
            ResolveDamageEffectsUncached(
                Ability,
                EnvironmentTarget,
                Resolved);

        auto& CacheEntry =
            GEffectCache[
                GEffectCacheCursor %
                GEffectCache.size()];
        GEffectCacheCursor++;
        CacheEntry =
            FAbilityEffectCache{};
        CacheEntry.Ability =
            TWeakObjectPtr<UFortGameplayAbility>(Ability);
        CacheEntry.EnvironmentTarget =
            EnvironmentTarget;
        CacheEntry.Resolved = true;
        CacheEntry.Count =
            Success ? Resolved.Count : 0;
        CacheEntry.ContainerIndex =
            Success ? Resolved.ContainerIndex : -1;
        for (int32 Index = 0;
            Index < CacheEntry.Count;
            Index++)
        {
            CacheEntry.Effects[Index] =
                TWeakObjectPtr<UClass>(
                    Resolved.Classes[Index]);
        }

        if (Success)
            OutEffects = Resolved;
        return Success;
    }

    float GetServerTimeSeconds()
    {
        auto World = UWorld::GetWorld();
        if (!IsUsableObject(World))
            return -1.f;

        const double Time =
            UGameplayStatics::GetTimeSeconds(World);
        return std::isfinite(Time) &&
            Time >= 0.0 &&
            Time <=
                static_cast<double>(
                    (std::numeric_limits<float>::max)())
            ? static_cast<float>(Time)
            : -1.f;
    }

    void ResetProjectileTrackingState()
    {
        for (auto& Entry : GLaunchBudgets)
            Entry = FWeaponHitBudget{};
        for (auto& Entry : GIngressBudgets)
            Entry = FWeaponHitBudget{};
        for (auto& Entry : GCanonicalShotBudgets)
            Entry = FCanonicalShotBudget{};
        for (auto& Entry : GProjectileReportBudgets)
            Entry = FProjectileReportBudget{};
        for (auto& Entry : GProjectileLedger)
            Entry = FProjectileLedgerEntry{};
        for (auto& Entry : GProjectileIngressCapabilities)
            Entry = FProjectileIngressCapability{};
        for (auto& Entry :
            GProjectileCompatibilityTokenStates)
        {
            Entry = FProjectileCompatibilityTokenState{};
        }
        for (auto& Entry : GProjectileVisualRelayStates)
            Entry = FProjectileVisualRelayState{};
        GLaunchBudgetCursor = 0;
        GIngressBudgetCursor = 0;
        GProjectileLedgerCursor = 0;
        GProjectileIngressCapabilityCursor = 0;
        GProjectileCompatibilityTokenStateCursor = 0;
        GProjectileVisualRelayStateCursor = 0;
        // Keep the generation monotonic. A reset invalidates all retained
        // keys, but reusing generation values would make diagnostics and any
        // stale local observations unnecessarily ambiguous.
    }

    bool SynchronizeProjectileTimeEpoch(float Now)
    {
        auto World = UWorld::GetWorld();
        if (!IsUsableObject(World) ||
            !std::isfinite(Now) ||
            Now < 0.f)
        {
            return false;
        }

        const bool WorldChanged =
            GProjectileWorldIdentity &&
            GProjectileWorldIdentity != World;
        const bool TimeRolledBack =
            GLastProjectileServerTime >= 0.f &&
            Now < GLastProjectileServerTime;
        if (WorldChanged || TimeRolledBack)
        {
            // Launches, aggregate pellet counters, and exact-report
            // tombstones form one security epoch. Never expire only one side
            // of that relationship when a match/world clock resets.
            ResetProjectileTrackingState();
        }

        GProjectileWorldIdentity = World;
        GLastProjectileServerTime = Now;
        return true;
    }

    void MarkNativeProjectileIngress(
        AFortWeaponRanged* Weapon)
    {
        if (!IsUsableActor(Weapon) ||
            !IsUsableObject(Weapon->Class))
        {
            return;
        }

        for (auto& Entry :
            GProjectileIngressCapabilities)
        {
            if (Entry.WeaponClassIdentity ==
                Weapon->Class)
            {
                Entry.NativeIngressObserved = true;
                return;
            }
        }

        auto& Entry =
            GProjectileIngressCapabilities[
                GProjectileIngressCapabilityCursor %
                GProjectileIngressCapabilities.size()];
        GProjectileIngressCapabilityCursor++;
        Entry = FProjectileIngressCapability{};
        Entry.WeaponClassIdentity = Weapon->Class;
        Entry.NativeIngressObserved = true;
    }

    bool HasObservedNativeProjectileIngress(
        AFortWeaponRanged* Weapon)
    {
        if (!IsUsableActor(Weapon) ||
            !IsUsableObject(Weapon->Class))
        {
            return true;
        }

        for (const auto& Entry :
            GProjectileIngressCapabilities)
        {
            if (Entry.WeaponClassIdentity ==
                    Weapon->Class &&
                Entry.NativeIngressObserved)
            {
                return true;
            }
        }
        return false;
    }

    FProjectileCompatibilityTokenState*
        GetProjectileCompatibilityTokenState(
            AFortWeaponRanged* Weapon,
            AFortPlayerPawnAthena* Shooter,
            bool Create)
    {
        if (!IsUsableActor(Weapon) ||
            !IsUsableActor(Shooter))
        {
            return nullptr;
        }

        FProjectileCompatibilityTokenState* Reusable =
            nullptr;
        for (auto& Entry :
            GProjectileCompatibilityTokenStates)
        {
            if (Entry.WeaponIdentity == Weapon &&
                Entry.ShooterIdentity == Shooter &&
                ResolveWeakObject(Entry.Weapon) ==
                    Weapon &&
                ResolveWeakObject(Entry.Shooter) ==
                    Shooter)
            {
                return &Entry;
            }

            if (!Reusable &&
                (!Entry.WeaponIdentity ||
                    ResolveWeakObject(Entry.Weapon) !=
                        Entry.WeaponIdentity ||
                    ResolveWeakObject(Entry.Shooter) !=
                        Entry.ShooterIdentity))
            {
                Reusable = &Entry;
            }
        }

        if (!Create)
            return nullptr;

        if (!Reusable)
        {
            Reusable =
                &GProjectileCompatibilityTokenStates[
                    GProjectileCompatibilityTokenStateCursor %
                    GProjectileCompatibilityTokenStates
                        .size()];
            GProjectileCompatibilityTokenStateCursor++;
        }

        *Reusable = FProjectileCompatibilityTokenState{};
        Reusable->Weapon =
            TWeakObjectPtr<AFortWeaponRanged>(Weapon);
        Reusable->Shooter =
            TWeakObjectPtr<AFortPlayerPawnAthena>(
                Shooter);
        Reusable->WeaponIdentity = Weapon;
        Reusable->ShooterIdentity = Shooter;
        return Reusable;
    }

    FProjectileVisualRelayState*
        GetProjectileVisualRelayState(
            AFortWeaponRanged* Weapon,
            bool Create)
    {
        if (!IsUsableActor(Weapon))
            return nullptr;

        FProjectileVisualRelayState* Reusable = nullptr;
        for (auto& Entry : GProjectileVisualRelayStates)
        {
            if (Entry.WeaponIdentity == Weapon &&
                ResolveWeakObject(Entry.Weapon) == Weapon)
            {
                return &Entry;
            }

            if (!Reusable &&
                (!Entry.WeaponIdentity ||
                    ResolveWeakObject(Entry.Weapon) !=
                        Entry.WeaponIdentity))
            {
                Reusable = &Entry;
            }
        }

        if (!Create)
            return nullptr;

        if (!Reusable)
        {
            Reusable =
                &GProjectileVisualRelayStates[
                    GProjectileVisualRelayStateCursor %
                    GProjectileVisualRelayStates.size()];
            GProjectileVisualRelayStateCursor++;
        }

        *Reusable = FProjectileVisualRelayState{};
        Reusable->Weapon =
            TWeakObjectPtr<AFortWeaponRanged>(Weapon);
        Reusable->WeaponIdentity = Weapon;
        return Reusable;
    }

    bool GetServerAimDirection(
        AFortPlayerControllerAthena* Controller,
        FVector& OutDirection)
    {
        if (!IsUsableActor(Controller))
            return false;

        const FRotator AimRotation =
            Controller->GetControlRotation();
        if (!std::isfinite(AimRotation.Pitch) ||
            !std::isfinite(AimRotation.Yaw))
        {
            return false;
        }

        const double PitchRadians =
            AimRotation.Pitch *
            3.14159265358979323846 / 180.0;
        const double YawRadians =
            AimRotation.Yaw *
            3.14159265358979323846 / 180.0;
        const double CosPitch = std::cos(PitchRadians);
        OutDirection = FVector{
            CosPitch * std::cos(YawRadians),
            CosPitch * std::sin(YawRadians),
            std::sin(PitchRadians)
        };
        return IsFiniteVector(OutDirection) &&
            !OutDirection.IsZero();
    }

    bool ValidateLaunchAgainstServerAim(
        AFortPlayerControllerAthena* Controller,
        const FVector& LaunchDirection,
        double MinimumAimDot = kMinServerAimDot)
    {
        FVector AimDirection{};
        if (!GetServerAimDirection(
                Controller,
                AimDirection))
        {
            return false;
        }

        const auto NormalizedLaunch =
            LaunchDirection.GetSafeNormal();
        return !NormalizedLaunch.IsZero() &&
            std::isfinite(MinimumAimDot) &&
            MinimumAimDot >= -1.0 &&
            MinimumAimDot <= 1.0 &&
            NormalizedLaunch.Dot(AimDirection) >=
                MinimumAimDot;
    }

    bool ValidateProjectileTravelDirectionWithAzimuth(
        const FVector& LaunchDirection,
        const FVector& TravelDirection,
        double MinimumAzimuthDot,
        double& OutCorrelation)
    {
        const auto NormalizedLaunch =
            LaunchDirection.GetSafeNormal();
        const auto NormalizedTravel =
            TravelDirection.GetSafeNormal();
        if (NormalizedLaunch.IsZero() ||
            NormalizedTravel.IsZero() ||
            !std::isfinite(MinimumAzimuthDot) ||
            MinimumAzimuthDot < -1.0 ||
            MinimumAzimuthDot > 1.0)
        {
            return false;
        }

        const double FullDot =
            NormalizedLaunch.Dot(NormalizedTravel);
        if (!std::isfinite(FullDot))
            return false;

        const double LaunchHorizontalSquared =
            NormalizedLaunch.X * NormalizedLaunch.X +
            NormalizedLaunch.Y * NormalizedLaunch.Y;
        const double TravelHorizontalSquared =
            NormalizedTravel.X * NormalizedTravel.X +
            NormalizedTravel.Y * NormalizedTravel.Y;
        if (LaunchHorizontalSquared > 0.0001 &&
            TravelHorizontalSquared > 0.0001)
        {
            const double HorizontalDot =
                (NormalizedLaunch.X * NormalizedTravel.X +
                    NormalizedLaunch.Y * NormalizedTravel.Y) /
                std::sqrt(
                    LaunchHorizontalSquared *
                    TravelHorizontalSquared);
            if (!std::isfinite(HorizontalDot) ||
                HorizontalDot < MinimumAzimuthDot)
            {
                return false;
            }

            const double LaunchPitch =
                std::atan2(
                    NormalizedLaunch.Z,
                    std::sqrt(
                        LaunchHorizontalSquared));
            const double TravelPitch =
                std::atan2(
                    NormalizedTravel.Z,
                    std::sqrt(
                        TravelHorizontalSquared));
            const double PitchDeviation =
                TravelPitch - LaunchPitch;
            if (!std::isfinite(PitchDeviation) ||
                PitchDeviation >
                    kMaxUpwardPitchDeviationRadians ||
                PitchDeviation <
                    -kMaxDownwardPitchDeviationRadians)
            {
                return false;
            }

            OutCorrelation =
                (std::min)(
                    HorizontalDot,
                    std::cos(std::abs(PitchDeviation)));
            return true;
        }

        if (FullDot < kMinLaunchCorroborationDot)
            return false;

        OutCorrelation = FullDot;
        return true;
    }

    bool ValidateProjectileTravelDirection(
        const FVector& LaunchDirection,
        const FVector& TravelDirection,
        double& OutCorrelation)
    {
        return ValidateProjectileTravelDirectionWithAzimuth(
            LaunchDirection,
            TravelDirection,
            kMinProjectileAzimuthDot,
            OutCorrelation);
    }

    bool ValidateCompatibilityTravelCorridor(
        const FVector& ReferenceDirection,
        const FVector& TravelDelta,
        double& OutCorrelation,
        double RearwardSlackCm =
            kCompatibilityRearwardSlackCm,
        double ParallaxAllowanceCm =
            kCompatibilityParallaxAllowanceCm)
    {
        OutCorrelation = 0.0;
        if (!IsFiniteVector(ReferenceDirection) ||
            !IsFiniteVector(TravelDelta))
        {
            return false;
        }

        const FVector NormalizedReference =
            ReferenceDirection.GetSafeNormal();
        if (NormalizedReference.IsZero())
            return false;

        const double TravelDistance =
            TravelDelta.Magnitude();
        if (!std::isfinite(TravelDistance))
            return false;

        const double Forward =
            TravelDelta.Dot(NormalizedReference);
        if (!std::isfinite(Forward) ||
            !std::isfinite(RearwardSlackCm) ||
            RearwardSlackCm < 0.0 ||
            !std::isfinite(ParallaxAllowanceCm) ||
            ParallaxAllowanceCm < 0.0 ||
            ParallaxAllowanceCm >
                kMaxLaunchOriginDriftCm ||
            Forward < -RearwardSlackCm)
        {
            return false;
        }

        const double ForwardPath =
            (std::max)(0.0, Forward);
        const double SideLimit =
            std::hypot(
                ParallaxAllowanceCm,
                kProjectileAzimuthSlope * ForwardPath);
        const double UpwardLimit =
            std::hypot(
                ParallaxAllowanceCm,
                kProjectileUpwardSlope * ForwardPath);
        const double DownwardLimit =
            std::hypot(
                ParallaxAllowanceCm,
                kProjectileDownwardSlope * ForwardPath);

        const double HorizontalSquared =
            NormalizedReference.X *
                NormalizedReference.X +
            NormalizedReference.Y *
                NormalizedReference.Y;
        if (HorizontalSquared > 0.0001)
        {
            const double Horizontal =
                std::sqrt(HorizontalSquared);
            const double SideError =
                std::abs(
                    TravelDelta.X *
                        (-NormalizedReference.Y /
                            Horizontal) +
                    TravelDelta.Y *
                        (NormalizedReference.X /
                            Horizontal));
            const double PitchError =
                TravelDelta.X *
                    (-NormalizedReference.Z *
                        NormalizedReference.X /
                        Horizontal) +
                TravelDelta.Y *
                    (-NormalizedReference.Z *
                        NormalizedReference.Y /
                        Horizontal) +
                TravelDelta.Z * Horizontal;
            if (!std::isfinite(SideError) ||
                !std::isfinite(PitchError) ||
                SideError > SideLimit ||
                PitchError > UpwardLimit ||
                PitchError < -DownwardLimit)
            {
                return false;
            }
        }
        else
        {
            // Exact vertical aim has no stable azimuth/pitch basis. Bound its
            // total perpendicular error by the same lateral corridor.
            const double PerpendicularSquared =
                (std::max)(
                    0.0,
                    TravelDistance * TravelDistance -
                        Forward * Forward);
            const double PerpendicularError =
                std::sqrt(PerpendicularSquared);
            if (!std::isfinite(PerpendicularError) ||
                PerpendicularError > SideLimit)
            {
                return false;
            }
        }

        if (TravelDistance <=
            (std::numeric_limits<double>::epsilon)())
        {
            // Direction is undefined when a muzzle already overlaps the hit
            // surface. The bounded corridor and later target/LOS checks still
            // fully constrain this contact-range report.
            OutCorrelation = 1.0;
        }
        else
        {
            OutCorrelation =
                (std::max)(
                    -1.0,
                    (std::min)(
                        1.0,
                        Forward / TravelDistance));
        }
        return true;
    }

    bool IsImpactWithinActorBounds(
        AActor* Target,
        const FVector& ImpactPoint,
        bool OnlyCollidingComponents,
        bool IncludeFromChildActors = false)
    {
        if (!GActorBoundsSchema.IsValid() ||
            !IsUsableActor(Target) ||
            !IsFiniteVector(ImpactPoint))
        {
            return false;
        }

        alignas(16) std::array<uint8, 0x80> Params{};
        if (!WriteValue(
                Params.data(),
                GActorBoundsSchema.ParamsSize,
                GActorBoundsSchema.OnlyCollidingComponents,
                OnlyCollidingComponents) ||
            !WriteValue(
                Params.data(),
                GActorBoundsSchema.ParamsSize,
                GActorBoundsSchema.IncludeFromChildActors,
                IncludeFromChildActors))
        {
            return false;
        }

        Target->ProcessEvent(
            GActorBoundsSchema.Function,
            Params.data());

        FVector Origin{};
        FVector BoxExtent{};
        if (!ReadValue(
                Params.data(),
                GActorBoundsSchema.ParamsSize,
                GActorBoundsSchema.Origin,
                Origin,
                FVector::Size()) ||
            !ReadValue(
                Params.data(),
                GActorBoundsSchema.ParamsSize,
                GActorBoundsSchema.BoxExtent,
                BoxExtent,
                FVector::Size()) ||
            !IsFiniteVector(Origin) ||
            !IsFiniteVector(BoxExtent) ||
            BoxExtent.X < 0.0 ||
            BoxExtent.Y < 0.0 ||
            BoxExtent.Z < 0.0 ||
            BoxExtent.X > kMaxActorBoundsExtentCm ||
            BoxExtent.Y > kMaxActorBoundsExtentCm ||
            BoxExtent.Z > kMaxActorBoundsExtentCm)
        {
            return false;
        }

        const bool DegenerateBounds =
            BoxExtent.X <=
                (std::numeric_limits<double>::epsilon)() &&
            BoxExtent.Y <=
                (std::numeric_limits<double>::epsilon)() &&
            BoxExtent.Z <=
                (std::numeric_limits<double>::epsilon)();
        if (OnlyCollidingComponents &&
            DegenerateBounds)
        {
            // Some Chapter 5 build pieces expose their collision through a
            // child/proxy component and return an all-zero colliding-only box.
            // Retry the same server-owned actor bounds without that filter.
            return IsImpactWithinActorBounds(
                Target,
                ImpactPoint,
                false,
                true);
        }

        const bool WithinBounds =
            std::abs(ImpactPoint.X - Origin.X) <=
                BoxExtent.X + kImpactBoundsToleranceCm &&
            std::abs(ImpactPoint.Y - Origin.Y) <=
                BoxExtent.Y + kImpactBoundsToleranceCm &&
            std::abs(ImpactPoint.Z - Origin.Z) <=
                BoxExtent.Z + kImpactBoundsToleranceCm;
        if (!WithinBounds &&
            OnlyCollidingComponents)
        {
            // Build pieces commonly keep the authoritative geometry on a
            // non-colliding child/proxy component. Retry the broader
            // server-owned bounds whenever the narrow box misses, not only
            // when it is exactly zero.
            return IsImpactWithinActorBounds(
                Target,
                ImpactPoint,
                false,
                true);
        }
        if (!WithinBounds)
        {
            static int32 BoundsTraceCount = 0;
            if (BoundsTraceCount++ < 8)
            {
                SDK::DbgLog(
                    "  [ProjectileDamage] bounds-detail target=%s colliding-only=%d impact=(%.1f,%.1f,%.1f) origin=(%.1f,%.1f,%.1f) extent=(%.1f,%.1f,%.1f)\n",
                    Target->Name.ToString().c_str(),
                    OnlyCollidingComponents,
                    ImpactPoint.X,
                    ImpactPoint.Y,
                    ImpactPoint.Z,
                    Origin.X,
                    Origin.Y,
                    Origin.Z,
                    BoxExtent.X,
                    BoxExtent.Y,
                    BoxExtent.Z);
            }
        }
        return WithinBounds;
    }

    bool IsImpactWithinPlayerEnvelope(
        AFortPlayerPawnAthena* Target,
        const FVector& ImpactPoint)
    {
        if (!IsUsableActor(Target) ||
            !IsFiniteVector(ImpactPoint))
        {
            return false;
        }

        const FVector PlayerLocation =
            Target->K2_GetActorLocation();
        if (!IsFiniteVector(PlayerLocation))
            return false;

        const bool WithinEnvelope =
            std::abs(
                ImpactPoint.X -
                PlayerLocation.X) <=
                    kPlayerImpactHorizontalEnvelopeCm &&
            std::abs(
                ImpactPoint.Y -
                PlayerLocation.Y) <=
                    kPlayerImpactHorizontalEnvelopeCm &&
            std::abs(
                ImpactPoint.Z -
                PlayerLocation.Z) <=
                    kPlayerImpactVerticalEnvelopeCm;
        if (!WithinEnvelope)
        {
            static int32 PlayerBoundsTraceCount = 0;
            if (PlayerBoundsTraceCount++ < 8)
            {
                SDK::DbgLog(
                    "  [ProjectileDamage] player-envelope target=%s impact=(%.1f,%.1f,%.1f) location=(%.1f,%.1f,%.1f)\n",
                    Target->Name.ToString().c_str(),
                    ImpactPoint.X,
                    ImpactPoint.Y,
                    ImpactPoint.Z,
                    PlayerLocation.X,
                    PlayerLocation.Y,
                    PlayerLocation.Z);
            }
        }
        return WithinEnvelope;
    }

    bool IsPawnAuthoritativelySeatedInVehicle(
        AActor* HostVehicle,
        AFortPlayerPawnAthena* ShooterPawn,
        int32& OutSeatIndex)
    {
        OutSeatIndex = -1;
        if (!IsUsableActor(HostVehicle) ||
            !IsUsableActor(ShooterPawn))
        {
            return false;
        }

        auto AthenaVehicle =
            HostVehicle->Cast<AFortAthenaVehicle>();
        if (!IsUsableActor(AthenaVehicle))
            return false;

        // FindSeatIndex is a native function on FortAthenaVehicle, not on
        // FortVehicleSeatComponent. Looking it up on the component always
        // returned null on FN30, which made every mounted weapon look
        // unseated: the turret host then blocked the server LOS trace and the
        // larger, vehicle-only muzzle corridor was never selected. Keep this
        // on the live server-owned vehicle rather than decoding PlayerSlots,
        // whose storage differs across Chapter 5 vehicle subclasses.
        auto FindSeatIndexFunction =
            AthenaVehicle->GetFunction("FindSeatIndex");
        if (!IsUsableObject(FindSeatIndexFunction) ||
            FindSeatIndexFunction->GetParamsNamed().Size != 0x10)
        {
            return false;
        }

        const int32 SeatIndex =
            AthenaVehicle->Call<int32>(
                FindSeatIndexFunction,
                ShooterPawn);
        if (SeatIndex < 0 || SeatIndex > 15)
            return false;

        OutSeatIndex = SeatIndex;
        return true;
    }

    AActor* ResolveNativeOccupiedVehicleForTrace(
        AFortPlayerPawnAthena* ShooterPawn)
    {
        if (!IsUsableActor(ShooterPawn))
            return nullptr;

        // The server-owned pawn/vehicle association remains valid while a
        // passenger uses a vehicle-mod weapon. Resolve it dynamically because
        // legacy pawn subclasses expose one of several function names.
        UFunction* GetVehicleFunction =
            ShooterPawn->GetFunction("GetVehicleActor");
        if (!GetVehicleFunction)
        {
            GetVehicleFunction =
                ShooterPawn->GetFunction("GetVehicle");
        }
        if (!GetVehicleFunction)
        {
            GetVehicleFunction =
                ShooterPawn->GetFunction("BP_GetVehicle");
        }
        if (!GetVehicleFunction)
            return nullptr;

        auto HostVehicle =
            ShooterPawn->Call<AActor*>(GetVehicleFunction);
        int32 SeatIndex = -1;
        return IsPawnAuthoritativelySeatedInVehicle(
                HostVehicle,
                ShooterPawn,
                SeatIndex)
            ? HostVehicle
            : nullptr;
    }

    AActor* ResolveMountedHostVehicleForTrace(
        AFortWeaponRanged* ReportingWeapon,
        AFortPlayerPawnAthena* ShooterPawn)
    {
        // MountedWeaponInfoRepped is declared by
        // FortWeaponRangedForVehicle, not AFortWeaponRanged. The generated
        // property accessor caches one offset globally, so calling Has* on a
        // normal rifle after a vehicle weapon was seen would incorrectly reuse
        // the vehicle subclass offset and read beyond that rifle's object.
        static const UClass* VehicleWeaponClass = nullptr;
        if (!VehicleWeaponClass)
        {
            VehicleWeaponClass =
                FindClass("FortWeaponRangedForVehicle");
        }
        if (!IsUsableActor(ReportingWeapon) ||
            !IsUsableActor(ShooterPawn) ||
            !IsUsableObject(VehicleWeaponClass) ||
            !ReportingWeapon->IsA(VehicleWeaponClass) ||
            !WeaponIsCurrentForPawn(
                ShooterPawn,
                ReportingWeapon))
        {
            return nullptr;
        }

        AActor* HostVehicle = nullptr;
        if (ReportingWeapon->HasMountedWeaponInfoRepped())
        {
            const auto& MountedInfo =
                ReportingWeapon->MountedWeaponInfoRepped;
            if (FMountedWeaponInfoRepped::HasHostVehicleCached())
            {
                auto HostObject =
                    MountedInfo.HostVehicleCached.ObjectPointer;
                // HostVehicleCached is transient replicated/interface state.
                // During equip/unequip it can briefly contain a non-null
                // sentinel or stale value. Cast dereferences UObject::Class,
                // so validate membership in the live object array first.
                if (IsUsableObject(HostObject))
                    HostVehicle = HostObject->Cast<AActor>();
            }
            else if (
                FMountedWeaponInfoRepped::
                    HasHostVehicleCachedActor())
            {
                HostVehicle =
                    MountedInfo.HostVehicleCachedActor;
            }
        }

        int32 AuthoritativeSeatIndex = -1;
        if (IsPawnAuthoritativelySeatedInVehicle(
                HostVehicle,
                ShooterPawn,
                AuthoritativeSeatIndex))
        {
            // The native occupied-seat result is authoritative. FN30's mod-seat
            // equip RPC can update the replicated cached index one frame after
            // firing begins, so do not reject an exact native occupancy match
            // merely because that transient index still names the prior mode.
            return HostVehicle;
        }

        // MountedWeaponInfoRepped is transient and its host/seat cache can lag
        // the first rounds of a sustained FN30 mod-seat burst. Fall back only
        // to the server-owned pawn association, and still require this exact
        // vehicle-weapon subclass, current weapon identity, vehicle interface,
        // and authoritative occupied seat. No client-provided actor is trusted.
        return ResolveNativeOccupiedVehicleForTrace(
            ShooterPawn);
    }

    bool ReadLiveWeaponTraceStart(
        AFortWeaponRanged* ReportingWeapon,
        const char* FunctionName,
        uint32 ExpectedParamsSize,
        FVector& OutStart)
    {
        OutStart = FVector{};
        if (!IsUsableActor(ReportingWeapon) ||
            !FunctionName)
        {
            return false;
        }

        auto Function =
            ReportingWeapon->GetFunction(FunctionName);
        if (!IsUsableObject(Function))
            return false;

        const auto Params = Function->GetParamsNamed();
        const auto PatternIndex =
            GetNamedParameter(Params, "PatternIndex");
        const auto ReturnValue =
            GetNamedParameter(Params, "ReturnValue");
        if (Params.Size != ExpectedParamsSize ||
            !PatternIndex.IsValid(Params.Size) ||
            PatternIndex.Size != sizeof(int32) ||
            !ReturnValue.IsValid(Params.Size) ||
            ReturnValue.Size != FVector::Size())
        {
            return false;
        }

        alignas(16) std::array<uint8, 0x80> Buffer{};
        constexpr int32 FirstMuzzlePattern = 0;
        if (!WriteValue(
                Buffer.data(),
                Params.Size,
                PatternIndex,
                FirstMuzzlePattern))
        {
            return false;
        }

        ReportingWeapon->ProcessEvent(
            Function,
            Buffer.data());
        return ReadValue(
                Buffer.data(),
                Params.Size,
                ReturnValue,
                OutStart,
                FVector::Size()) &&
            IsFiniteVector(OutStart);
    }

    bool HasExactImpactLineOfSight(
        AFortPlayerControllerAthena* Controller,
        AFortWeaponRanged* ReportingWeapon,
        AActor* Target,
        const FVector& ViewPoint,
        const FVector& ImpactPoint)
    {
        if (!GWorldLineTraceSchema.IsValid() ||
            !IsUsableActor(Controller) ||
            !IsUsableActor(Target) ||
            !IsFiniteVector(ViewPoint) ||
            !IsFiniteVector(ImpactPoint))
        {
            return false;
        }

        FVector TraceDirection =
            (ImpactPoint - ViewPoint)
                .GetSafeNormal();
        if (TraceDirection.IsZero())
            return true;
        const FVector TraceEnd =
            ImpactPoint + TraceDirection * 5.0;

        std::array<AActor*, 5> IgnoredData{};
        int32 IgnoredCount = 0;
        IgnoredData[IgnoredCount++] = Controller;
        auto ShooterPawn =
            ResolveControllerPawn(Controller);
        if (ShooterPawn)
        {
            IgnoredData[IgnoredCount++] =
                ShooterPawn;
            if (IsUsableActor(ReportingWeapon))
            {
                IgnoredData[IgnoredCount++] =
                    ReportingWeapon;
            }
            if (ShooterPawn->HasCurrentWeapon() &&
                IsUsableActor(
                    ShooterPawn->CurrentWeapon) &&
                ShooterPawn->CurrentWeapon !=
                    ReportingWeapon &&
                IgnoredCount <
                    static_cast<int32>(
                        IgnoredData.size()))
            {
                IgnoredData[IgnoredCount++] =
                    ShooterPawn->CurrentWeapon;
            }
        }

        // A mounted weapon's authoritative origin is at the turret muzzle,
        // but that point can still lie inside the host vehicle's collision.
        // Ignore only the positively validated MountedWeaponInfoRepped host;
        // ordinary projectile weapons retain the exact same LOS validation.
        auto MountedHostVehicle =
            ResolveMountedHostVehicleForTrace(
                ReportingWeapon,
                ShooterPawn);
        if (MountedHostVehicle &&
            MountedHostVehicle != Target &&
            IgnoredCount <
                static_cast<int32>(IgnoredData.size()))
        {
            IgnoredData[IgnoredCount++] =
                MountedHostVehicle;
        }

        TArray<AActor*> IgnoredActors{};
        IgnoredActors.Data = IgnoredData.data();
        IgnoredActors.NumElements = IgnoredCount;
        IgnoredActors.MaxElements = IgnoredCount;

        alignas(16) std::array<
            uint8,
            kMaxEffectCallParamsSize> Params{};
        UObject* WorldContext = Controller;
        constexpr uint8 VisibilityTraceType = 0;
        const bool TraceComplex = false;
        const bool IgnoreSelf = true;
        if (!WriteValue(
                Params.data(),
                GWorldLineTraceSchema.ParamsSize,
                GWorldLineTraceSchema
                    .WorldContextObject,
                WorldContext) ||
            !WriteBytes(
                Params.data(),
                GWorldLineTraceSchema.ParamsSize,
                GWorldLineTraceSchema.Start,
                &ViewPoint,
                FVector::Size()) ||
            !WriteBytes(
                Params.data(),
                GWorldLineTraceSchema.ParamsSize,
                GWorldLineTraceSchema.End,
                &TraceEnd,
                FVector::Size()) ||
            !WriteValue(
                Params.data(),
                GWorldLineTraceSchema.ParamsSize,
                GWorldLineTraceSchema.TraceChannel,
                VisibilityTraceType) ||
            !WriteValue(
                Params.data(),
                GWorldLineTraceSchema.ParamsSize,
                GWorldLineTraceSchema.TraceComplex,
                TraceComplex) ||
            !WriteValue(
                Params.data(),
                GWorldLineTraceSchema.ParamsSize,
                GWorldLineTraceSchema.ActorsToIgnore,
                IgnoredActors) ||
            !WriteValue(
                Params.data(),
                GWorldLineTraceSchema.ParamsSize,
                GWorldLineTraceSchema.IgnoreSelf,
                IgnoreSelf))
        {
            return false;
        }

        GWorldLineTraceSchema.Library->ProcessEvent(
            GWorldLineTraceSchema.Function,
            Params.data());

        bool HitAnything = false;
        if (!ReadValue(
                Params.data(),
                GWorldLineTraceSchema.ParamsSize,
                GWorldLineTraceSchema.ReturnValue,
                HitAnything))
        {
            return false;
        }
        if (!HitAnything)
            return true;

        std::array<uint8, kMaxHitResultSize>
            ServerHit{};
        FResolvedHit ResolvedHit{};
        if (!ReadValue(
                Params.data(),
                GWorldLineTraceSchema.ParamsSize,
                GWorldLineTraceSchema.OutHit,
                ServerHit,
                GHitResultSchema.Size) ||
            !ResolveHit(
                ServerHit.data(),
                GHitResultSchema.Size,
                ResolvedHit))
        {
            return false;
        }

        if (ResolvedHit.Target == Target)
            return true;

        // Some pawn meshes do not block Visibility. In that case the trace
        // can report geometry immediately behind the validated impact. It is
        // still clear only when the first blocking point is no earlier than
        // that impact (within a tiny numeric tolerance); ownership alone is
        // never sufficient because a target-owned attachment can obstruct it.
        const double ClaimedDistance =
            FVector::Dist(ViewPoint, ImpactPoint);
        const double BlockingDistance =
            FVector::Dist(
                ViewPoint,
                ResolvedHit.ImpactPoint);
        return std::isfinite(ClaimedDistance) &&
            std::isfinite(BlockingDistance) &&
            BlockingDistance + 2.0 >=
                ClaimedDistance;
    }

    bool HasServerLineOfSight(
        AFortPlayerControllerAthena* Controller,
        AFortWeaponRanged* ReportingWeapon,
        AActor* Target,
        const FVector& ViewPoint,
        const FVector& LaunchDirection,
        const FVector& CorroboratedProjectileOrigin,
        const FVector& ImpactPoint)
    {
        if (!GLineOfSightSchema.IsValid() ||
            !IsUsableActor(Controller) ||
            !IsUsableActor(Target) ||
            !IsFiniteVector(ViewPoint) ||
            !IsFiniteVector(LaunchDirection) ||
            !IsFiniteVector(CorroboratedProjectileOrigin) ||
            !IsFiniteVector(ImpactPoint))
        {
            return false;
        }

        if (Target->Cast<AFortPlayerPawnAthena>() &&
            GWorldLineTraceSchema.IsValid())
        {
            // Actor-centric LineOfSightTo aims at the pawn center and can be
            // blocked by a ramp even when the validated head impact is
            // exposed. Trace the exact server-validated impact instead.
            if (HasExactImpactLineOfSight(
                    Controller,
                    ReportingWeapon,
                    Target,
                    ViewPoint,
                    ImpactPoint))
            {
                return true;
            }

            auto ShooterPawn =
                ResolveControllerPawn(Controller);
            auto MountedHost =
                ResolveMountedHostVehicleForTrace(
                    ReportingWeapon,
                    ShooterPawn);
            if (!MountedHost)
                return false;

            // The canonical projectile ledger deliberately records a
            // server-owned weapon/pivot origin. FN30's vehicle-mod turret
            // rotates its physical muzzle around that pivot, though, and the
            // pivot-to-impact ray can cross the roof or nearby cover even when
            // the actual server muzzle ray is clear. Retry only after exact
            // occupied-seat/current-weapon validation, and only from live
            // server-computed weapon locations bounded to every corroborating
            // server/client point. The target-specific visibility trace and
            // all cover checks remain unchanged.
            const FVector WeaponLocation =
                ReportingWeapon->K2_GetActorLocation();
            const FVector HostLocation =
                MountedHost->K2_GetActorLocation();
            if (!IsFiniteVector(WeaponLocation) ||
                !IsFiniteVector(HostLocation))
            {
                return false;
            }

            struct FLiveTraceStartFunction
            {
                const char* Name;
                uint32 ParamsSize;
            };
            constexpr std::array<
                FLiveTraceStartFunction,
                2> LiveStartFunctions{{
                {"GetMuzzleLocation", 0x20},
                {"GetDamageStartLocation", 0x38}
            }};
            for (const auto& CandidateFunction :
                LiveStartFunctions)
            {
                FVector LiveStart{};
                if (!ReadLiveWeaponTraceStart(
                        ReportingWeapon,
                        CandidateFunction.Name,
                        CandidateFunction.ParamsSize,
                        LiveStart))
                {
                    continue;
                }

                const double WeaponDistance =
                    FVector::Dist(
                        WeaponLocation,
                        LiveStart);
                const double HostDistance =
                    FVector::Dist(
                        HostLocation,
                        LiveStart);
                const double LedgerDistance =
                    FVector::Dist(
                        ViewPoint,
                        LiveStart);
                const double ReportedDistance =
                    FVector::Dist(
                        CorroboratedProjectileOrigin,
                        LiveStart);
                if (!std::isfinite(WeaponDistance) ||
                    !std::isfinite(HostDistance) ||
                    !std::isfinite(LedgerDistance) ||
                    !std::isfinite(ReportedDistance) ||
                    WeaponDistance >
                        kMaxRecordedOriginErrorCm ||
                    HostDistance >
                        kMaxMountedTraceStartHostDistanceCm ||
                    LedgerDistance >
                        kMaxRecordedOriginErrorCm ||
                    ReportedDistance >
                        kMaxRecordedOriginErrorCm)
                {
                    continue;
                }

                const FVector LiveTravel =
                    ImpactPoint - LiveStart;
                double DirectionCorrelation = 0.0;
                if (!ValidateCompatibilityTravelCorridor(
                        LaunchDirection,
                        LiveTravel,
                        DirectionCorrelation,
                        kCompatibilityRearwardSlackCm,
                        kMountedWeaponParallaxAllowanceCm))
                {
                    continue;
                }

                if (HasExactImpactLineOfSight(
                        Controller,
                        ReportingWeapon,
                        Target,
                        LiveStart,
                        ImpactPoint))
                {
                    static int32 MountedRetryTraceCount = 0;
                    if (MountedRetryTraceCount++ < 8)
                    {
                        SDK::DbgLog(
                            "  [ProjectileDamage] mounted-los-retry source=%s weapon=%s pivot-offset=%.1f reported-offset=%.1f correlation=%.3f\n",
                            CandidateFunction.Name,
                            ReportingWeapon->Name
                                .ToString()
                                .c_str(),
                            WeaponDistance,
                            ReportedDistance,
                            DirectionCorrelation);
                    }
                    return true;
                }
            }
            return false;
        }

        alignas(16) std::array<uint8, 0x80> Params{};
        AActor* Other = Target;
        const bool AlternateChecks = true;
        if (!WriteValue(
                Params.data(),
                GLineOfSightSchema.ParamsSize,
                GLineOfSightSchema.Other,
                Other) ||
            !WriteBytes(
                Params.data(),
                GLineOfSightSchema.ParamsSize,
                GLineOfSightSchema.ViewPoint,
                &ViewPoint,
                FVector::Size()) ||
            !WriteValue(
                Params.data(),
                GLineOfSightSchema.ParamsSize,
                GLineOfSightSchema.AlternateChecks,
                AlternateChecks))
        {
            return false;
        }

        Controller->ProcessEvent(
            GLineOfSightSchema.Function,
            Params.data());

        bool Result = false;
        return ReadValue(
                Params.data(),
                GLineOfSightSchema.ParamsSize,
                GLineOfSightSchema.ReturnValue,
                Result) &&
            Result;
    }

    bool ConsumeLaunchBudget(
        AFortWeaponRanged* Weapon,
        float Now,
        FProjectileLedgerEntry* LaunchReservation,
        uint64 ReportFingerprint,
        FHitBudgetReservation& OutReservation);

    bool IsProjectileEntryExpired(
        const FProjectileLedgerEntry& Entry,
        float Now)
    {
        if (!Entry.Active ||
            !std::isfinite(Entry.RecordedAt) ||
            !std::isfinite(Now) ||
            Now < Entry.RecordedAt)
        {
            return true;
        }

        if (!Entry.Automatic)
        {
            return Now - Entry.RecordedAt >
                kReplayTombstoneSeconds;
        }

        if (Now - Entry.RecordedAt >
            kMaxAutomaticStreamSeconds +
                kReplayTombstoneSeconds)
        {
            return true;
        }

        if (Entry.StoppedAt >= 0.f)
        {
            return !std::isfinite(Entry.StoppedAt) ||
                Now < Entry.StoppedAt ||
                Now - Entry.StoppedAt >
                    kReplayTombstoneSeconds;
        }

        return false;
    }

    bool RecordProjectileLaunch(
        AFortWeaponRanged* Weapon,
        const void* RequestMemory,
        uint32 RequestSize,
        bool TimestampFromClient,
        bool ForceSingleShot = false)
    {
        if (!IsUsableActor(Weapon))
        {
            return false;
        }

        if (!GProjectileRequestSchema.IsValid() ||
            RequestSize !=
                GProjectileRequestSchema.RequestSize ||
            !SDK::MemReadable(
                RequestMemory,
                RequestSize))
        {
            return false;
        }

        FVector Start{};
        FVector Direction{};
        float ProjectileTimestamp = 0.f;
        if (!ReadValue(
                RequestMemory,
                RequestSize,
                GProjectileRequestSchema.StartPosition,
                Start,
                FVector::Size()) ||
            !ReadValue(
                RequestMemory,
                RequestSize,
                GProjectileRequestSchema.StartDirection,
                Direction,
                FVector::Size()) ||
            !ReadValue(
                RequestMemory,
                RequestSize,
                GProjectileRequestSchema.Timestamp,
                ProjectileTimestamp) ||
            !IsFiniteVector(Start) ||
            !IsFiniteVector(Direction) ||
            !std::isfinite(ProjectileTimestamp))
        {
            return false;
        }

        AFortPlayerPawnAthena* ShooterPawn = nullptr;
        AFortPlayerControllerAthena* ShooterController =
            nullptr;
        const float Now = GetServerTimeSeconds();
        const double DirectionMagnitude =
            Direction.Magnitude();
        if (!SynchronizeProjectileTimeEpoch(Now))
            return false;

        if (!ResolveShooter(
                Weapon,
                ShooterPawn,
                ShooterController) ||
            !WeaponIsCurrentForPawn(ShooterPawn, Weapon) ||
            DirectionMagnitude < 0.5 ||
            DirectionMagnitude > 1.5 ||
            !ValidateLaunchAgainstServerAim(
                ShooterController,
                Direction))
        {
            return false;
        }

        const FVector ShooterLocation =
            ShooterPawn->K2_GetActorLocation();
        const FVector WeaponLocation =
            Weapon->K2_GetActorLocation();
        if (!IsFiniteVector(ShooterLocation) ||
            !IsFiniteVector(WeaponLocation) ||
            (std::min)(
                FVector::Dist(
                    ShooterLocation,
                    Start),
                FVector::Dist(
                    WeaponLocation,
                    Start)) >
                kMaxLaunchOriginDriftCm)
        {
            return false;
        }

        const bool Automatic =
            !ForceSingleShot &&
            IsAutomaticWeapon(Weapon);
        const double LegalShotInterval =
            GetWeaponShotInterval(Weapon);
        const double ShotInterval =
            Automatic ? LegalShotInterval : 0.0;
        const double PendingServerDedupeWindow =
            std::clamp(
                LegalShotInterval * 0.90,
                0.01,
                0.95);
        const int32 PelletsPerShot =
            GetBulletsPerCartridge(Weapon);

        FVector NormalizedDirection =
            Direction.GetSafeNormal();

        for (auto& Existing : GProjectileLedger)
        {
            if (ForceSingleShot)
                break;

            if (!Existing.Active ||
                Existing.WeaponIdentity != Weapon ||
                Existing.ShooterIdentity != ShooterPawn ||
                !std::isfinite(Existing.RecordedAt) ||
                Now < Existing.RecordedAt ||
                Now - Existing.RecordedAt > 0.50f ||
                FVector::Dist(Existing.Start, Start) > 1.0 ||
                Existing.Direction.Dot(
                    NormalizedDirection) < 0.9999)
            {
                continue;
            }

            if (!Existing.TimestampFromClient &&
                TimestampFromClient)
            {
                Existing.ProjectileTimestamp =
                    ProjectileTimestamp;
                Existing.TimestampFromClient = true;
                return true;
            }
            if (!Existing.TimestampFromClient &&
                !TimestampFromClient &&
                (Automatic ||
                    static_cast<double>(
                        Now - Existing.RecordedAt) <
                        PendingServerDedupeWindow))
            {
                return true;
            }
            if (Existing.TimestampFromClient &&
                TimestampFromClient &&
                std::abs(
                    Existing.ProjectileTimestamp -
                    ProjectileTimestamp) <= 0.0005f)
            {
                return true;
            }
        }

        // Ignore a duplicate delivery of the same server multicast instead
        // of minting a second canonical generation for one projectile.
        if (Automatic)
        {
            for (const auto& Existing : GProjectileLedger)
            {
                if (!Existing.Active ||
                    Existing.WeaponIdentity != Weapon ||
                    Existing.ShooterIdentity != ShooterPawn ||
                    !std::isfinite(Existing.RecordedAt) ||
                    Now < Existing.RecordedAt ||
                    Now - Existing.RecordedAt > 0.25f ||
                    std::abs(
                        Existing.ProjectileTimestamp -
                        ProjectileTimestamp) > 0.0005f ||
                    FVector::Dist(Existing.Start, Start) > 1.0 ||
                    Existing.Direction.Dot(
                        NormalizedDirection) < 0.9999)
                {
                    continue;
                }
                return true;
            }
        }

        // Exact per-token snapshots are independent one-shot records. The
        // legacy stream cleanup is only needed when a looping automatic
        // request is being created.
        if (!ForceSingleShot)
        {
            // A ranged weapon has one active looping request. Preserve older
            // generations for their in-flight projectiles, but do not let a
            // missed stop authorize two concurrent cadence streams.
            for (auto& Existing : GProjectileLedger)
            {
                if (Existing.Active &&
                    IsProjectileEntryExpired(Existing, Now))
                {
                    Existing.Active = false;
                    Existing.Reserved = false;
                    continue;
                }

                if (Existing.Active &&
                    Existing.Automatic &&
                    Existing.StoppedAt < 0.f &&
                    Existing.WeaponIdentity == Weapon &&
                    Existing.ShooterIdentity == ShooterPawn &&
                    ResolveWeakObject(Existing.Weapon) ==
                        Weapon &&
                    ResolveWeakObject(Existing.Shooter) ==
                        ShooterPawn)
                {
                    Existing.StoppedAt = Now;
                }
            }
        }

        FProjectileLedgerEntry* Entry = nullptr;
        for (size_t Offset = 0;
            Offset < GProjectileLedger.size();
            Offset++)
        {
            const size_t Index =
                (GProjectileLedgerCursor + Offset) %
                GProjectileLedger.size();
            auto& Candidate =
                GProjectileLedger[Index];
            const bool Reusable =
                !Candidate.Active ||
                IsProjectileEntryExpired(
                    Candidate,
                    Now) ||
                (Candidate.WeaponIdentity == Weapon &&
                    ResolveWeakObject(Candidate.Weapon) !=
                        Weapon);
            if (!Reusable)
                continue;

            Entry = &Candidate;
            GProjectileLedgerCursor =
                (Index + 1) %
                GProjectileLedger.size();
            break;
        }

        if (!Entry)
        {
            static bool WarnedLedgerFull = false;
            if (!WarnedLedgerFull)
            {
                WarnedLedgerFull = true;
                SDK::DbgLog(
                    "  [ProjectileDamage] projectile ledger is full; preserving live entries and rejecting new launch records\n");
            }
            return false;
        }

        *Entry = FProjectileLedgerEntry{};
        Entry->Weapon =
            TWeakObjectPtr<AFortWeaponRanged>(Weapon);
        Entry->Shooter =
            TWeakObjectPtr<AFortPlayerPawnAthena>(
                ShooterPawn);
        Entry->WeaponIdentity = Weapon;
        Entry->ShooterIdentity = ShooterPawn;
        Entry->Start = Start;
        Entry->Direction = Direction.GetSafeNormal();
        Entry->ProjectileTimestamp =
            ProjectileTimestamp;
        Entry->TimestampFromClient =
            TimestampFromClient;
        Entry->ServerFireToken =
            TimestampFromClient
                ? -1.f
                : ProjectileTimestamp;
        Entry->RecordedAt = Now;
        Entry->StoppedAt = -1.f;
        Entry->Generation = GProjectileGeneration++;
        if (GProjectileGeneration == 0)
            GProjectileGeneration = 1;
        Entry->ReservedShotIndex = -1;
        Entry->AuthoritativeShotSnapshot =
            ForceSingleShot;
        Entry->Automatic = Automatic;
        Entry->ShotInterval = ShotInterval;
        Entry->PelletsPerShot = PelletsPerShot;
        Entry->RemainingSingleShotHits =
            Entry->PelletsPerShot;
        Entry->Active = true;
        return true;
    }

    bool RecordServerProjectileState(
        AFortWeaponRanged* Weapon,
        const FVector& DamageStart,
        const FVector& DamageDirection)
    {
        if (!IsUsableActor(Weapon) ||
            !Weapon->HasAuthority() ||
            !IsFiniteVector(DamageStart) ||
            !IsFiniteVector(DamageDirection))
        {
            return false;
        }

        const float ReceivedAt = GetServerTimeSeconds();
        if (!std::isfinite(ReceivedAt) ||
            ReceivedAt < 0.f ||
            !SynchronizeProjectileTimeEpoch(
                ReceivedAt))
        {
            return false;
        }

        AFortPlayerPawnAthena* ShooterPawn = nullptr;
        AFortPlayerControllerAthena* ShooterController =
            nullptr;
        const double DirectionMagnitude =
            DamageDirection.Magnitude();
        if (!ResolveShooter(
                Weapon,
                ShooterPawn,
                ShooterController) ||
            !WeaponIsCurrentForPawn(
                ShooterPawn,
                Weapon) ||
            DirectionMagnitude < 0.5 ||
            DirectionMagnitude > 1.5 ||
            !ValidateLaunchAgainstServerAim(
                ShooterController,
                DamageDirection,
                kMinAdjustedServerAimDot))
        {
            return false;
        }

        const FVector ShooterLocation =
            ShooterPawn->K2_GetActorLocation();
        const FVector WeaponLocation =
            Weapon->K2_GetActorLocation();
        if (!IsFiniteVector(ShooterLocation) ||
            !IsFiniteVector(WeaponLocation) ||
            (std::min)(
                FVector::Dist(
                    ShooterLocation,
                    DamageStart),
                FVector::Dist(
                    WeaponLocation,
                    DamageStart)) >
                kMaxLaunchOriginDriftCm)
        {
            return false;
        }

        auto RelayState =
            GetProjectileVisualRelayState(
                Weapon,
                true);
        if (!RelayState)
            return false;

        RelayState->LatestDamageStart =
            FVector(
                DamageStart.X,
                DamageStart.Y,
                DamageStart.Z);
        RelayState->LatestAdjustedAimDirection =
            DamageDirection.GetSafeNormal();
        RelayState->LatestDamageStateAt =
            ReceivedAt;
        RelayState->HasLatestDamageState =
            !RelayState
                ->LatestAdjustedAimDirection
                .IsZero();
        RelayState->LightweightProjectile = true;
        RelayState->CaptureActive = true;
        return RelayState->HasLatestDamageState;
    }

    bool IsUsableCompatibilityFireToken(
        float FireToken,
        float Now)
    {
        return std::isfinite(FireToken) &&
            FireToken > 0.f &&
            FireToken <=
                Now +
                    kCompatibilityFutureFireTimeToleranceSeconds &&
            static_cast<double>(Now) -
                    static_cast<double>(FireToken) <=
                static_cast<double>(
                    kMaxProjectileAgeSeconds) +
                    kProjectileFlightTimeGraceSeconds;
    }

    bool ResolveCompatibilityFireToken(
        AFortWeaponRanged* Weapon,
        float Now,
        float& OutFireToken,
        const char*& OutSource)
    {
        OutFireToken = -1.f;
        OutSource = "none";
        float Verified = -1.f;
        float Last = -1.f;
        const bool ReadVerified =
            ReadReflectedObjectValue(
                Weapon,
                "LastFireTimeVerified",
                Verified);
        const bool ReadLast =
            ReadReflectedObjectValue(
                Weapon,
                "LastFireTime",
                Last);
        const bool VerifiedUsable =
            ReadVerified &&
            IsUsableCompatibilityFireToken(
                Verified,
                Now);
        const bool LastUsable =
            ReadLast &&
            IsUsableCompatibilityFireToken(
                Last,
                Now);

        // Both fields live on the authoritative weapon. Prefer the newer
        // usable value: LastFireTimeVerified can trail LastFireTime by one
        // replication update during sustained automatic fire.
        if (VerifiedUsable &&
            (!LastUsable || Verified >= Last))
        {
            OutFireToken = Verified;
            OutSource = "verified";
        }
        else if (LastUsable)
        {
            OutFireToken = Last;
            OutSource = "last";
        }

        static int32 TraceCount = 0;
        if (TraceCount++ < 8)
        {
            SDK::DbgLog(
                "  [ProjectileDamage] compatibility-fire-state weapon=%s now=%.6f verified-read=%d verified=%.6f last-read=%d last=%.6f selected=%s token=%.6f\n",
                IsUsableActor(Weapon)
                    ? Weapon->Name.ToString().c_str()
                    : "invalid",
                Now,
                ReadVerified,
                Verified,
                ReadLast,
                Last,
                OutSource,
                OutFireToken);
        }
        return OutFireToken > 0.f;
    }

    bool IsLightweightProjectileWeapon(
        AFortWeaponRanged* Weapon)
    {
        if (!IsUsableActor(Weapon) ||
            !GLightweightProjectileVisualSchema
                .CanIdentifyWeapon() ||
            !Weapon->HasWeaponData() ||
            !IsUsableObject(Weapon->WeaponData) ||
            !Weapon->WeaponData->IsA(
                GLightweightProjectileVisualSchema
                    .RangedItemDefinitionClass))
        {
            return false;
        }

        alignas(16) std::array<uint8, 0x40> Params{};
        Weapon->WeaponData->ProcessEvent(
            GLightweightProjectileVisualSchema
                .HasLightweightProjectile,
            Params.data());

        bool Result = false;
        return ReadValue(
                Params.data(),
                GLightweightProjectileVisualSchema
                    .HasLightweightProjectileSize,
                GLightweightProjectileVisualSchema
                    .HasLightweightProjectileReturn,
                Result) &&
            Result;
    }

    bool WriteServerOwnedProjectileRequest(
        void* RequestMemory,
        uint32 RequestSize,
        const FVector& Start,
        const FVector& Direction,
        float FireToken)
    {
        if (!GProjectileRequestSchema.IsValid() ||
            !RequestMemory ||
            RequestSize !=
                GProjectileRequestSchema.RequestSize ||
            !IsFiniteVector(Start) ||
            !IsFiniteVector(Direction) ||
            !std::isfinite(FireToken))
        {
            return false;
        }

        const FVector NormalizedDirection =
            Direction.GetSafeNormal();
        return !NormalizedDirection.IsZero() &&
            WriteBytes(
                RequestMemory,
                RequestSize,
                GProjectileRequestSchema.StartPosition,
                &Start,
                FVector::Size()) &&
            WriteBytes(
                RequestMemory,
                RequestSize,
                GProjectileRequestSchema.StartDirection,
                &NormalizedDirection,
                FVector::Size()) &&
            WriteValue(
                RequestMemory,
                RequestSize,
                GProjectileRequestSchema.Timestamp,
                FireToken);
    }

    bool BroadcastProjectileVisualRequest(
        AFortWeaponRanged* Weapon,
        const FVector& Start,
        const FVector& Direction,
        float FireToken,
        bool ForceSingleShot = false)
    {
        if (!IsUsableActor(Weapon) ||
            !Weapon->HasAuthority() ||
            !GProjectileRequestSchema.IsValid() ||
            !IsFiniteVector(Start) ||
            !IsFiniteVector(Direction) ||
            !std::isfinite(FireToken))
        {
            return false;
        }

        alignas(16) std::array<uint8, 0x100> Params{};
        if (GProjectileRequestSchema.ParamsSize >
                Params.size() ||
            !GProjectileRequestSchema.Request.IsValid(
                GProjectileRequestSchema.ParamsSize))
        {
            return false;
        }

        auto RequestMemory =
            Params.data() +
            GProjectileRequestSchema.Request.Offset;
        if (!WriteServerOwnedProjectileRequest(
                RequestMemory,
                GProjectileRequestSchema.RequestSize,
                Start,
                Direction,
                FireToken))
        {
            return false;
        }

        // Record the exact server-owned launch before the multicast. The
        // hooked local Exec path normally sees the same request too, but
        // network routing order differs between minor engine builds.
        if (!RecordProjectileLaunch(
                Weapon,
                RequestMemory,
                GProjectileRequestSchema.RequestSize,
                false,
                ForceSingleShot))
        {
            return false;
        }

        const bool WasInsideRelay =
            GInsideProjectileVisualRelay;
        GInsideProjectileVisualRelay = true;
        Weapon->ProcessEvent(
            GProjectileRequestSchema.Function,
            Params.data());
        GInsideProjectileVisualRelay = WasInsideRelay;
        return true;
    }

    void PrepareServerAbilityActivation(
        UObject* AbilitySourceObject)
    {
        if (VersionInfo.FortniteVersion < 28.00 ||
            VersionInfo.FortniteVersion >= 32.00 ||
            !IsUsableObject(AbilitySourceObject))
        {
            return;
        }

        auto Weapon =
            AbilitySourceObject->Cast<AFortWeaponRanged>();
        if (!IsUsableActor(Weapon) ||
            !Weapon->HasAuthority() ||
            !GLightweightProjectileVisualSchema
                .CanIdentifyWeapon() ||
            !IsLightweightProjectileWeapon(Weapon))
        {
            return;
        }

        const float Now = GetServerTimeSeconds();
        if (!SynchronizeProjectileTimeEpoch(Now))
            return;

        auto RelayState =
            GetProjectileVisualRelayState(Weapon, true);
        if (!RelayState)
            return;
        if (RelayState->CaptureActive &&
            IsAutomaticWeapon(Weapon))
        {
            return;
        }
        RelayState->CaptureActiveBeforePending =
            RelayState->CaptureActive;
        RelayState->LightweightProjectile = true;
        RelayState->CaptureActive = true;
        RelayState->HasLatestDamageState = false;
        RelayState->LatestDamageStateAt = -1.f;

        float Verified = -1.f;
        float Last = -1.f;
        const bool ReadVerified =
            ReadReflectedObjectValue(
                Weapon,
                "LastFireTimeVerified",
                Verified);
        const bool ReadLast =
            ReadReflectedObjectValue(
                Weapon,
                "LastFireTime",
                Last);
        RelayState->PendingBaselineFireToken = -1.f;
        if (ReadVerified &&
            std::isfinite(Verified) &&
            Verified >= 0.f)
        {
            RelayState->PendingBaselineFireToken =
                Verified;
        }
        if (ReadLast &&
            std::isfinite(Last) &&
            Last >
                RelayState->PendingBaselineFireToken)
        {
            RelayState->PendingBaselineFireToken =
                Last;
        }
        RelayState->PendingActivation = true;
        RelayState->PendingStartedAt = Now;
    }

    bool ResolveServerOwnedProjectileLaunch(
        AFortWeaponRanged* Weapon,
        AFortPlayerPawnAthena* ShooterPawn,
        AFortPlayerControllerAthena* ShooterController,
        FVector& OutStart,
        FVector& OutDirection,
        bool& OutUsedCachedStart,
        bool& OutUsedCachedDirection)
    {
        OutUsedCachedStart = false;
        OutUsedCachedDirection = false;
        if (!IsUsableActor(Weapon) ||
            !IsUsableActor(ShooterPawn) ||
            !IsUsableActor(ShooterController))
        {
            return false;
        }

        const FVector PawnLocation =
            ShooterPawn->K2_GetActorLocation();
        const FVector WeaponLocation =
            Weapon->K2_GetActorLocation();
        FVector AimDirection{};
        if (!IsFiniteVector(PawnLocation) ||
            !IsFiniteVector(WeaponLocation) ||
            !GetServerAimDirection(
                ShooterController,
                AimDirection))
        {
            return false;
        }

        const float Now = GetServerTimeSeconds();
        auto RelayState =
            GetProjectileVisualRelayState(
                Weapon,
                false);
        const bool HasFreshDamageState =
            RelayState &&
            RelayState->HasLatestDamageState &&
            std::isfinite(Now) &&
            std::isfinite(
                RelayState->LatestDamageStateAt) &&
            (!RelayState->PendingActivation ||
                !std::isfinite(
                    RelayState->PendingStartedAt) ||
                RelayState->LatestDamageStateAt +
                        0.001f >=
                    RelayState->PendingStartedAt) &&
            Now >= RelayState->LatestDamageStateAt &&
            static_cast<double>(
                Now -
                RelayState->LatestDamageStateAt) <=
                kMaxCachedLaunchGeometryAgeSeconds;

        FVector Start{};
        bool ReadStart = HasFreshDamageState;
        if (HasFreshDamageState)
        {
            Start = RelayState->LatestDamageStart;
            ReadStart = IsFiniteVector(Start);
        }
        OutUsedCachedStart =
            ReadStart &&
            IsFiniteVector(Start) &&
            (std::min)(
                FVector::Dist(PawnLocation, Start),
                FVector::Dist(WeaponLocation, Start)) <=
                kMaxLaunchOriginDriftCm;
        if (!OutUsedCachedStart)
        {
            Start = FVector(
                WeaponLocation.X,
                WeaponLocation.Y,
                WeaponLocation.Z);
        }

        FVector Direction{};
        bool ReadDirection = HasFreshDamageState;
        if (HasFreshDamageState)
        {
            Direction =
                RelayState->LatestAdjustedAimDirection;
            ReadDirection =
                IsFiniteVector(Direction);
        }
        const double DirectionMagnitude =
            ReadDirection
                ? Direction.Magnitude()
                : 0.0;
        const double CachedAimDot =
            ReadDirection
                ? Direction.GetSafeNormal().Dot(
                    AimDirection.GetSafeNormal())
                : -1.0;
        OutUsedCachedDirection =
            ReadDirection &&
            IsFiniteVector(Direction) &&
            DirectionMagnitude >= 0.5 &&
            DirectionMagnitude <= 1.5 &&
            std::isfinite(CachedAimDot) &&
            CachedAimDot >=
                kMinAdjustedServerAimDot;
        if (!OutUsedCachedDirection)
        {
            Direction = FVector(
                AimDirection.X,
                AimDirection.Y,
                AimDirection.Z);
        }

        OutStart = FVector(
            Start.X,
            Start.Y,
            Start.Z);
        OutDirection = FVector(
            Direction.X,
            Direction.Y,
            Direction.Z);
        return true;
    }

    void RelayServerAbilityActivation(
        UObject* AbilitySourceObject)
    {
        if (VersionInfo.FortniteVersion < 28.00 ||
            VersionInfo.FortniteVersion >= 32.00 ||
            GInsideProjectileVisualRelay ||
            !IsUsableObject(AbilitySourceObject))
        {
            return;
        }

        auto Weapon =
            AbilitySourceObject->Cast<AFortWeaponRanged>();
        if (!IsUsableActor(Weapon) ||
            !Weapon->HasAuthority() ||
            !GProjectileRequestSchema.IsValid() ||
            !GLightweightProjectileVisualSchema
                .CanIdentifyWeapon() ||
            !IsLightweightProjectileWeapon(Weapon))
        {
            return;
        }

        const float Now = GetServerTimeSeconds();
        if (!SynchronizeProjectileTimeEpoch(Now))
            return;

        auto RelayState =
            GetProjectileVisualRelayState(
                Weapon,
                false);
        if (!RelayState ||
            !RelayState->PendingActivation)
        {
            return;
        }
        if (!std::isfinite(
                RelayState->PendingStartedAt) ||
            Now < RelayState->PendingStartedAt ||
            Now - RelayState->PendingStartedAt >
                kProjectileVisualRelayPendingSeconds)
        {
            RelayState->PendingActivation = false;
            return;
        }
        const float BaselineFireToken =
            RelayState->PendingBaselineFireToken;

        AFortPlayerPawnAthena* ShooterPawn = nullptr;
        AFortPlayerControllerAthena* ShooterController =
            nullptr;
        if (!ResolveShooter(
                Weapon,
                ShooterPawn,
                ShooterController) ||
            !WeaponIsCurrentForPawn(
                ShooterPawn,
                Weapon))
        {
            return;
        }

        float FireToken = -1.f;
        const char* FireTokenSource = "none";
        if (!ResolveCompatibilityFireToken(
                Weapon,
                Now,
                FireToken,
                FireTokenSource))
        {
            return;
        }

        const double FireAge =
            static_cast<double>(Now) -
            static_cast<double>(FireToken);
        if (!std::isfinite(FireAge) ||
            FireAge <
                -static_cast<double>(
                    kCompatibilityFutureFireTimeToleranceSeconds) ||
            FireAge >
                static_cast<double>(
                    kProjectileVisualRelayFreshnessSeconds) ||
            FireToken <=
                BaselineFireToken +
                    kProjectileTimestampToleranceSeconds ||
            FireToken <=
                RelayState->LastCapturedFireToken +
                    kProjectileTimestampToleranceSeconds)
        {
            return;
        }

        FVector Start{};
        FVector Direction{};
        bool UsedCachedStart = false;
        bool UsedCachedDirection = false;
        if (!ResolveServerOwnedProjectileLaunch(
                Weapon,
                ShooterPawn,
                ShooterController,
                Start,
                Direction,
                UsedCachedStart,
                UsedCachedDirection))
        {
            return;
        }

        const bool Broadcast =
            BroadcastProjectileVisualRequest(
                Weapon,
                Start,
                Direction,
                FireToken,
                true);
        if (Broadcast)
        {
            RelayState->LastCapturedFireToken =
                FireToken;
            RelayState->LastVisualFireToken =
                FireToken;
            RelayState->ActiveVisualFireToken =
                FireToken;
            RelayState->Active = true;
            RelayState->PendingActivation = false;
        }

        static int32 TraceCount = 0;
        if (TraceCount++ < 4)
        {
            SDK::DbgLog(
                "  [ProjectileDamage] visual-relay weapon=%s fire-source=%s token=%.6f age=%.3f start=%s direction=%s sent=%d\n",
                Weapon->Name.ToString().c_str(),
                FireTokenSource,
                FireToken,
                FireAge,
                UsedCachedStart ? "cached" : "weapon",
                UsedCachedDirection
                    ? "cached"
                    : "control",
                Broadcast);
        }
    }

    void CaptureAuthoritativeProjectileLaunch(
        AFortWeaponRanged* Weapon)
    {
        if (!IsUsableActor(Weapon) ||
            !Weapon->HasAuthority() ||
            !GProjectileRequestSchema.IsValid())
        {
            return;
        }

        const float Now = GetServerTimeSeconds();
        if (!SynchronizeProjectileTimeEpoch(Now))
            return;

        auto RelayState =
            GetProjectileVisualRelayState(
                Weapon,
                false);
        if (!RelayState ||
            !RelayState->LightweightProjectile)
        {
            if (!GLightweightProjectileVisualSchema
                    .CanIdentifyWeapon() ||
                !IsLightweightProjectileWeapon(Weapon))
            {
                return;
            }
            RelayState =
                GetProjectileVisualRelayState(
                    Weapon,
                    true);
            if (RelayState)
                RelayState->LightweightProjectile = true;
        }
        if (!RelayState)
            return;

        if (RelayState->PendingActivation &&
            (!std::isfinite(
                    RelayState->PendingStartedAt) ||
                Now < RelayState->PendingStartedAt ||
                Now - RelayState->PendingStartedAt >
                    kProjectileVisualRelayPendingSeconds))
        {
            RelayState->PendingActivation = false;
            RelayState->CaptureActive =
                RelayState
                    ->CaptureActiveBeforePending;
            RelayState->CaptureActiveBeforePending =
                false;
            RelayState->PendingStartedAt = -1.f;
            RelayState->PendingBaselineFireToken = -1.f;
            RelayState->HasLatestDamageState = false;
            RelayState->LatestDamageStateAt = -1.f;
            if (!RelayState->CaptureActive)
                return;
        }

        float FireToken = -1.f;
        const char* FireTokenSource = "none";
        if (!ResolveCompatibilityFireToken(
                Weapon,
                Now,
                FireToken,
                FireTokenSource) ||
            (RelayState->PendingActivation &&
                FireToken <=
                    RelayState
                        ->PendingBaselineFireToken +
                        kProjectileTimestampToleranceSeconds) ||
            FireToken <=
                RelayState->LastCapturedFireToken +
                    kProjectileTimestampToleranceSeconds)
        {
            return;
        }

        const double FireAge =
            static_cast<double>(Now) -
            static_cast<double>(FireToken);
        if (!std::isfinite(FireAge) ||
            FireAge <
                -static_cast<double>(
                    kCompatibilityFutureFireTimeToleranceSeconds) ||
            FireAge >
                static_cast<double>(
                    kProjectileVisualRelayFreshnessSeconds))
        {
            return;
        }

        const bool HasActivationGeometry =
            RelayState->HasLatestDamageState &&
            std::isfinite(
                RelayState->LatestDamageStateAt) &&
            (!RelayState->PendingActivation ||
                !std::isfinite(
                    RelayState->PendingStartedAt) ||
                RelayState->LatestDamageStateAt +
                        0.001f >=
                    RelayState->PendingStartedAt);
        if (RelayState->PendingActivation &&
            !HasActivationGeometry &&
            FireAge >= 0.0 &&
            FireAge <
                static_cast<double>(
                    kProjectileGeometryWaitSeconds))
        {
            // LastFireTime can advance a frame before the authoritative
            // adjusted muzzle ray arrives. Briefly defer instead of
            // multicasting the previous shot's direction.
            return;
        }

        AFortPlayerPawnAthena* ShooterPawn = nullptr;
        AFortPlayerControllerAthena* ShooterController =
            nullptr;
        if (!ResolveShooter(
                Weapon,
                ShooterPawn,
                ShooterController) ||
            !WeaponIsCurrentForPawn(
                ShooterPawn,
                Weapon))
        {
            return;
        }

        FVector Start{};
        FVector Direction{};
        bool UsedCachedStart = false;
        bool UsedCachedDirection = false;
        if (!ResolveServerOwnedProjectileLaunch(
                Weapon,
                ShooterPawn,
                ShooterController,
                Start,
                Direction,
                UsedCachedStart,
                UsedCachedDirection))
        {
            return;
        }

        // LastFireTime advances for each physical Chapter 5 round, including
        // the individual rounds inside a burst. Always mint one bounded local
        // snapshot per fresh server token. If the native multicast path is
        // absent, send that same per-shot request so observers see the round.
        bool Relayed = false;
        bool Recorded = false;
        const bool NativeTokenObserved =
            std::isfinite(
                RelayState
                    ->LastNativeMulticastFireToken) &&
            std::isfinite(
                RelayState->LastNativeMulticastAt) &&
            Now >= RelayState->LastNativeMulticastAt &&
            Now - RelayState->LastNativeMulticastAt <=
                kProjectileVisualRelayFreshnessSeconds &&
            std::abs(
                RelayState
                    ->LastNativeMulticastFireToken -
                FireToken) <=
                kProjectileTimestampToleranceSeconds;
        if (!NativeTokenObserved)
        {
            Relayed =
                BroadcastProjectileVisualRequest(
                    Weapon,
                    Start,
                    Direction,
                    FireToken,
                    true);
            Recorded = Relayed;
        }
        else
        {
            alignas(16) std::array<uint8, 0x100>
                Request{};
            Recorded =
                WriteServerOwnedProjectileRequest(
                    Request.data(),
                    GProjectileRequestSchema.RequestSize,
                    Start,
                    Direction,
                    FireToken) &&
                RecordProjectileLaunch(
                    Weapon,
                    Request.data(),
                    GProjectileRequestSchema.RequestSize,
                    false,
                    true);
        }

        if (Recorded)
        {
            RelayState->LastCapturedFireToken =
                FireToken;
            RelayState->PendingActivation = false;
            RelayState->CaptureActive = true;
            RelayState->CaptureActiveBeforePending =
                false;
            if (Relayed)
            {
                RelayState->LastVisualFireToken =
                    FireToken;
                RelayState->ActiveVisualFireToken =
                    FireToken;
                RelayState->Active = true;
            }
        }

        static int32 TraceCount = 0;
        if (TraceCount++ < 8)
        {
            SDK::DbgLog(
                "  [ProjectileDamage] shot-capture weapon=%s fire-source=%s token=%.6f age=%.3f start=%s direction=%s recorded=%d relayed=%d\n",
                Weapon->Name.ToString().c_str(),
                FireTokenSource,
                FireToken,
                FireAge,
                UsedCachedStart ? "cached" : "weapon",
                UsedCachedDirection
                    ? "cached"
                    : "control",
                Recorded,
                Relayed);
        }
    }

    void TickServerProjectileRelays()
    {
        if (VersionInfo.FortniteVersion < 28.00 ||
            VersionInfo.FortniteVersion >= 32.00 ||
            GInsideProjectileVisualRelay ||
            !GProjectileRequestSchema.IsValid() ||
            !GLightweightProjectileVisualSchema
                .CanIdentifyWeapon())
        {
            return;
        }

        const float Now = GetServerTimeSeconds();
        if (!SynchronizeProjectileTimeEpoch(Now))
            return;

        for (auto& Entry : GProjectileVisualRelayStates)
        {
            if (!Entry.WeaponIdentity)
                continue;

            auto Weapon = ResolveWeakObject(Entry.Weapon);
            if (Weapon != Entry.WeaponIdentity ||
                !IsUsableActor(Weapon) ||
                !Weapon->HasAuthority())
            {
                Entry = FProjectileVisualRelayState{};
                continue;
            }

            if (Entry.WeaponIdentity == Weapon &&
                (Entry.CaptureActive ||
                    Entry.PendingActivation))
            {
                CaptureAuthoritativeProjectileLaunch(
                    Weapon);
            }
        }
    }

    bool ValidateReportedCompatibilityLaunch(
        const FVector& ReportedProjectileOrigin,
        const FVector& ImpactPoint,
        bool ReadServerStart,
        const FVector& ServerStart,
        bool ReadServerDirection,
        const FVector& ServerDirection,
        const FVector& CurrentPawnLocation,
        const FVector& CurrentWeaponLocation,
        const FVector& CurrentAimDirection,
        double FireAge,
        FVector& OutStart,
        FVector& OutDirection,
        bool& OutUsedServerDirection,
        double ParallaxAllowanceCm =
            kCompatibilityParallaxAllowanceCm)
    {
        OutUsedServerDirection = false;
        if (!IsFiniteVector(ReportedProjectileOrigin) ||
            !IsFiniteVector(ImpactPoint) ||
            !IsFiniteVector(CurrentPawnLocation) ||
            !IsFiniteVector(CurrentWeaponLocation) ||
            !IsFiniteVector(CurrentAimDirection) ||
            !std::isfinite(FireAge) ||
            FireAge < 0.0)
        {
            return false;
        }

        // The RPC origin is useful only as corroboration. It must never become
        // the authorizing ray: at contact range a client could otherwise place
        // it on the claimed impact and manufacture a zero-length "valid" hit.
        if (!ReadServerStart ||
            !IsFiniteVector(ServerStart))
        {
            return false;
        }

        const double MaxCurrentOriginError =
            kMaxLaunchOriginDriftCm +
            kMaxPlausibleShooterSpeedCmPerSecond *
                (FireAge +
                    kAutomaticTimestampGraceSeconds);
        const double ServerCurrentOriginError =
            (std::min)(
                FVector::Dist(
                    CurrentPawnLocation,
                    ServerStart),
                FVector::Dist(
                    CurrentWeaponLocation,
                    ServerStart));
        const double ReportedOriginError =
            FVector::Dist(
                ServerStart,
                ReportedProjectileOrigin);
        const double ReportedCurrentOriginError =
            (std::min)(
                FVector::Dist(
                    CurrentPawnLocation,
                    ReportedProjectileOrigin),
                FVector::Dist(
                    CurrentWeaponLocation,
                    ReportedProjectileOrigin));
        if (!std::isfinite(ServerCurrentOriginError) ||
            !std::isfinite(ReportedOriginError) ||
            !std::isfinite(ReportedCurrentOriginError) ||
            ServerCurrentOriginError > MaxCurrentOriginError ||
            ReportedOriginError >
                kMaxRecordedOriginErrorCm ||
            ReportedCurrentOriginError >
                MaxCurrentOriginError +
                    kMaxRecordedOriginErrorCm)
        {
            return false;
        }

        const FVector CandidateStart(
            ServerStart.X,
            ServerStart.Y,
            ServerStart.Z);
        const FVector CanonicalTravelDelta =
            ImpactPoint - CandidateStart;
        const FVector ReportedTravelDelta =
            ImpactPoint - ReportedProjectileOrigin;
        const FVector NormalizedAim =
            CurrentAimDirection.GetSafeNormal();
        const double CanonicalTravelDistance =
            CanonicalTravelDelta.Magnitude();
        const double TravelDistance =
            (std::max)(
                CanonicalTravelDistance,
                ReportedTravelDelta.Magnitude());
        if (!IsFiniteVector(CanonicalTravelDelta) ||
            !IsFiniteVector(ReportedTravelDelta) ||
            !std::isfinite(CanonicalTravelDistance) ||
            !std::isfinite(TravelDistance) ||
            TravelDistance > kMaxReportedTravelCm ||
            TravelDistance >
                kMaxConservativeProjectileSpeedCmPerSecond *
                    (FireAge +
                        kProjectileFlightTimeGraceSeconds) ||
            NormalizedAim.IsZero())
        {
            return false;
        }

        FVector NormalizedServerDirection{};
        bool HasUsableServerDirection = false;
        if (ReadServerDirection &&
            IsFiniteVector(ServerDirection))
        {
            const double ServerDirectionMagnitude =
                ServerDirection.Magnitude();
            if (std::isfinite(
                    ServerDirectionMagnitude) &&
                ServerDirectionMagnitude >= 0.5 &&
                ServerDirectionMagnitude <= 1.5)
            {
                NormalizedServerDirection =
                    ServerDirection.GetSafeNormal();
                if (!NormalizedServerDirection.IsZero())
                {
                    HasUsableServerDirection = true;
                }
            }
        }

        double CurrentAimCorrelation = 0.0;
        bool CurrentAimValid =
            FireAge <=
                kMaxCurrentAimFallbackAgeSeconds &&
            ValidateCompatibilityTravelCorridor(
                NormalizedAim,
                CanonicalTravelDelta,
                CurrentAimCorrelation,
                kCompatibilityRearwardSlackCm,
                ParallaxAllowanceCm);
        if (!CurrentAimValid &&
            FireAge <=
                kMaxCurrentAimFallbackAgeSeconds &&
            CanonicalTravelDistance <=
                kCompatibilityContactRangeCm)
        {
            // The server muzzle may already overlap a nearby collider. Permit
            // a bounded, fresh rearward interval from that server-owned start.
            CurrentAimValid =
                ValidateCompatibilityTravelCorridor(
                    NormalizedAim,
                    CanonicalTravelDelta,
                    CurrentAimCorrelation,
                    kCompatibilityContactRearwardSlackCm,
                    ParallaxAllowanceCm);
        }

        bool CachedDirectionValid = false;
        double CachedDirectionCorrelation = 0.0;
        if (HasUsableServerDirection)
        {
            if (FireAge <=
                kMaxCurrentAimFallbackAgeSeconds)
            {
                // A fresh current control aim is the authoritative direction.
                // A cached weapon direction may only stand in when it agrees.
                CachedDirectionValid =
                    NormalizedServerDirection.Dot(
                        NormalizedAim) >=
                        kMinProjectileAzimuthDot &&
                    ValidateCompatibilityTravelCorridor(
                        NormalizedServerDirection,
                        CanonicalTravelDelta,
                        CachedDirectionCorrelation,
                        kCompatibilityRearwardSlackCm,
                        ParallaxAllowanceCm);
                if (!CachedDirectionValid &&
                    CanonicalTravelDistance <=
                        kCompatibilityContactRangeCm &&
                    NormalizedServerDirection.Dot(
                        NormalizedAim) >=
                        kMinProjectileAzimuthDot)
                {
                    CachedDirectionValid =
                        ValidateCompatibilityTravelCorridor(
                            NormalizedServerDirection,
                            CanonicalTravelDelta,
                            CachedDirectionCorrelation,
                            kCompatibilityContactRearwardSlackCm,
                            ParallaxAllowanceCm);
                }
            }
            else
            {
                // Once the shooter can have moved their aim, retain only the
                // original strict launch/travel comparison for the cached shot.
                const FVector StrictTravelDirection =
                    CanonicalTravelDelta.GetSafeNormal();
                CachedDirectionValid =
                    !StrictTravelDirection.IsZero() &&
                    ValidateProjectileTravelDirection(
                        NormalizedServerDirection,
                        StrictTravelDirection,
                        CachedDirectionCorrelation);
            }
        }

        if (!CurrentAimValid &&
            !CachedDirectionValid)
        {
            return false;
        }

        OutStart = FVector(
            CandidateStart.X,
            CandidateStart.Y,
            CandidateStart.Z);
        OutUsedServerDirection = !CurrentAimValid;
        const FVector& ValidatedReference =
            CurrentAimValid
                ? NormalizedAim
                : NormalizedServerDirection;
        OutDirection = FVector(
            ValidatedReference.X,
            ValidatedReference.Y,
            ValidatedReference.Z);
        return true;
    }

    bool TryReserveCompatibilityLaunch(
        AFortWeaponRanged* Weapon,
        AFortPlayerPawnAthena* ShooterPawn,
        const FVector& ReportedProjectileOrigin,
        const FVector& ImpactPoint,
        float ProjectileTimestamp,
        float Now,
        const FVector& CurrentPawnLocation,
        const FVector& CurrentWeaponLocation,
        const FVector& CurrentAimDirection,
        FVector& OutLaunchOrigin,
        FVector& OutLaunchDirection,
        FProjectileLedgerEntry*& OutReservation)
    {
        if (!IsUsableActor(Weapon) ||
            !IsUsableActor(ShooterPawn) ||
            HasObservedNativeProjectileIngress(Weapon) ||
            !WeaponIsCurrentForPawn(
                ShooterPawn,
                Weapon))
        {
            return false;
        }

        float FireToken = -1.f;
        const char* FireTokenSource = "none";
        if (!ResolveCompatibilityFireToken(
                Weapon,
                Now,
                FireToken,
                FireTokenSource))
        {
            return false;
        }

        const double FireAge =
            (std::max)(
                0.0,
                static_cast<double>(Now) -
                    static_cast<double>(FireToken));
        FVector Start{};
        FVector Direction{};
        bool UsedServerDirection = false;
        const char* LaunchStateSource = "none";

        FVector CurrentStart{};
        FVector CurrentDirection{};
        const bool ReadCurrentStart =
            ReadReflectedObjectValue(
                Weapon,
                "CurrentDamageStartLocation",
                CurrentStart,
                FVector::Size());
        const bool ReadCurrentDirection =
            ReadReflectedObjectValue(
                Weapon,
                "CurrentAdjustedAimDirection",
                CurrentDirection,
                FVector::Size());
        bool CompatibilityLaunchValid =
            ValidateReportedCompatibilityLaunch(
                ReportedProjectileOrigin,
                ImpactPoint,
                ReadCurrentStart,
                CurrentStart,
                ReadCurrentDirection,
                CurrentDirection,
                CurrentPawnLocation,
                CurrentWeaponLocation,
                CurrentAimDirection,
                FireAge,
                Start,
                Direction,
                UsedServerDirection);
        bool UsedMountedWeaponOrigin = false;
        if (!CompatibilityLaunchValid &&
            ResolveMountedHostVehicleForTrace(
                Weapon,
                ShooterPawn))
        {
            // During sustained vehicle-turret fire, FN30 can advance
            // LastFireTime before CurrentDamageStartLocation moves to the new
            // muzzle transform. Retry only for a positively validated mounted
            // seat, using the authoritative weapon actor location and current
            // server aim. The reported origin remains bounded corroboration;
            // it never becomes the authorizing ray.
            CompatibilityLaunchValid =
                ValidateReportedCompatibilityLaunch(
                    ReportedProjectileOrigin,
                    ImpactPoint,
                    true,
                    CurrentWeaponLocation,
                    false,
                    FVector{},
                    CurrentPawnLocation,
                    CurrentWeaponLocation,
                    CurrentAimDirection,
                    FireAge,
                    Start,
                    Direction,
                    UsedServerDirection,
                    kMountedWeaponParallaxAllowanceCm);
            UsedMountedWeaponOrigin =
                CompatibilityLaunchValid;
        }
        if (CompatibilityLaunchValid)
        {
            LaunchStateSource =
                UsedMountedWeaponOrigin
                    ? "mounted-weapon-origin-current-aim"
                    : UsedServerDirection
                    ? "server-origin-cached-direction"
                    : "server-origin-current-aim";
        }
        else
        {
            const FVector TravelDirection =
                (ImpactPoint -
                    ReportedProjectileOrigin)
                    .GetSafeNormal();
            const FVector NormalizedAim =
                CurrentAimDirection.GetSafeNormal();
            const FVector NormalizedCurrentDirection =
                CurrentDirection.GetSafeNormal();
            static int32 RejectTraceCount = 0;
            if (RejectTraceCount++ < 16)
            {
                SDK::DbgLog(
                    "  [ProjectileDamage] compatibility-launch-reject weapon=%s token=%.6f age=%.3f current-read=%d/%d current-origin=%.1f current-dir-mag=%.3f reported-current=%.1f aim-dot=%.3f current-dir-dot=%.3f travel=%.1f\n",
                    Weapon->Name.ToString().c_str(),
                    FireToken,
                    FireAge,
                    ReadCurrentStart,
                    ReadCurrentDirection,
                    FVector::Dist(
                        CurrentStart,
                        ReportedProjectileOrigin),
                    CurrentDirection.Magnitude(),
                    (std::min)(
                        FVector::Dist(
                            CurrentPawnLocation,
                            ReportedProjectileOrigin),
                        FVector::Dist(
                            CurrentWeaponLocation,
                            ReportedProjectileOrigin)),
                    NormalizedAim.Dot(
                        TravelDirection),
                    NormalizedCurrentDirection.Dot(
                        TravelDirection),
                    FVector::Dist(
                        ReportedProjectileOrigin,
                        ImpactPoint));
            }
            return false;
        }

        // A canonical launch for this exact server fire token always wins.
        // Older live/tombstoned rounds must not block a later physical burst
        // round whose LastFireTime has advanced.
        for (auto& Existing : GProjectileLedger)
        {
            if (!Existing.Active)
                continue;
            if (IsProjectileEntryExpired(
                    Existing,
                    Now))
            {
                Existing.Active = false;
                Existing.Reserved = false;
                continue;
            }
            if (Existing.WeaponIdentity != Weapon ||
                Existing.ShooterIdentity != ShooterPawn ||
                ResolveWeakObject(Existing.Weapon) !=
                    Weapon ||
                ResolveWeakObject(Existing.Shooter) !=
                    ShooterPawn)
            {
                continue;
            }

            const bool SameServerFireToken =
                Existing.ServerFireToken >= 0.f &&
                std::abs(
                    Existing.ServerFireToken -
                    FireToken) <=
                    kProjectileTimestampToleranceSeconds;
            if (SameServerFireToken)
            {
                return false;
            }
        }

        auto TokenState =
            GetProjectileCompatibilityTokenState(
                Weapon,
                ShooterPawn,
                false);
        if (TokenState &&
            TokenState->NewestFireToken >= 0.f &&
            FireToken <= TokenState->NewestFireToken)
        {
            return false;
        }

        FProjectileLedgerEntry* Entry = nullptr;
        for (size_t Offset = 0;
            Offset < GProjectileLedger.size();
            Offset++)
        {
            const size_t Index =
                (GProjectileLedgerCursor + Offset) %
                GProjectileLedger.size();
            auto& Candidate =
                GProjectileLedger[Index];
            const bool Reusable =
                !Candidate.Active ||
                IsProjectileEntryExpired(
                    Candidate,
                    Now) ||
                (Candidate.WeaponIdentity == Weapon &&
                    ResolveWeakObject(Candidate.Weapon) !=
                        Weapon);
            if (!Reusable)
                continue;

            Entry = &Candidate;
            GProjectileLedgerCursor =
                (Index + 1) %
                GProjectileLedger.size();
            break;
        }
        if (!Entry)
            return false;

        if (!TokenState)
        {
            TokenState =
                GetProjectileCompatibilityTokenState(
                    Weapon,
                    ShooterPawn,
                    true);
        }
        if (!TokenState)
            return false;

        *Entry = FProjectileLedgerEntry{};
        Entry->Weapon =
            TWeakObjectPtr<AFortWeaponRanged>(Weapon);
        Entry->Shooter =
            TWeakObjectPtr<AFortPlayerPawnAthena>(
                ShooterPawn);
        Entry->WeaponIdentity = Weapon;
        Entry->ShooterIdentity = ShooterPawn;
        Entry->Start = Start;
        Entry->Direction = Direction;
        Entry->ProjectileTimestamp =
            ProjectileTimestamp;
        Entry->TimestampFromClient = true;
        Entry->RecordedAt =
            (std::min)(FireToken, Now);
        Entry->StoppedAt = -1.f;
        Entry->ServerFireToken = FireToken;
        Entry->CompatibilityFallback = true;
        Entry->Generation = GProjectileGeneration++;
        if (GProjectileGeneration == 0)
            GProjectileGeneration = 1;
        Entry->ReservedShotIndex = 0;
        // LastFireTime advances per authoritative shot. Even for automatic
        // weapons, one fallback token authorizes one bounded shot rather than
        // creating a long client-timestamp-derived firing stream.
        Entry->Automatic = false;
        Entry->ShotInterval = 0.0;
        Entry->PelletsPerShot =
            GetBulletsPerCartridge(Weapon);
        Entry->RemainingSingleShotHits =
            Entry->PelletsPerShot;
        Entry->Active = true;
        Entry->Reserved = true;
        TokenState->NewestFireToken = FireToken;

        // A positively validated mounted fallback is also a safe baseline for
        // the existing authoritative visual relay. Seed the accepted token so
        // the relay records only later LastFireTime advances and cannot
        // duplicate this bounded compatibility shot.
        if (UsedMountedWeaponOrigin &&
            GLightweightProjectileVisualSchema.CanIdentifyWeapon() &&
            IsLightweightProjectileWeapon(Weapon))
        {
            if (const auto RelayState =
                    GetProjectileVisualRelayState(Weapon, true))
            {
                RelayState->LightweightProjectile = true;
                RelayState->LastCapturedFireToken =
                    (std::max)(
                        RelayState->LastCapturedFireToken,
                        FireToken);
                RelayState->PendingActivation = false;
                RelayState->PendingStartedAt = -1.f;
                RelayState->PendingBaselineFireToken = -1.f;
                RelayState->CaptureActiveBeforePending = false;
                RelayState->CaptureActive = true;
            }
        }

        OutLaunchOrigin = Start;
        OutLaunchDirection = Direction;
        OutReservation = Entry;

        static int32 TraceCount = 0;
        if (TraceCount++ < 8)
        {
            SDK::DbgLog(
                "  [ProjectileDamage] launch-ingress source=server-state-fallback weapon=%s fire-source=%s launch-source=%s token=%.6f age=%.3f generation=%llu\n",
                Weapon->Name.ToString().c_str(),
                FireTokenSource,
                LaunchStateSource,
                FireToken,
                FireAge,
                static_cast<unsigned long long>(
                    Entry->Generation));
        }
        return true;
    }

    bool ReserveProjectileLaunch(
        AFortWeaponRanged* Weapon,
        AFortPlayerPawnAthena* ShooterPawn,
        AFortPlayerControllerAthena* ShooterController,
        const FVector& ReportedProjectileOrigin,
        const FVector& ImpactPoint,
        float ProjectileTimestamp,
        float Now,
        FVector& OutLaunchOrigin,
        FVector& OutLaunchDirection,
        FProjectileLedgerEntry*& OutReservation)
    {
        OutReservation = nullptr;
        if (!IsUsableActor(Weapon) ||
            !IsUsableActor(ShooterPawn) ||
            !IsUsableActor(ShooterController) ||
            !IsFiniteVector(ReportedProjectileOrigin) ||
            !IsFiniteVector(ImpactPoint) ||
            !std::isfinite(ProjectileTimestamp) ||
            !std::isfinite(Now) ||
            Now < 0.f)
        {
            return false;
        }

        const FVector CurrentPawnLocation =
            ShooterPawn->K2_GetActorLocation();
        const FVector CurrentWeaponLocation =
            Weapon->K2_GetActorLocation();
        FVector CurrentAimDirection{};
        if (!IsFiniteVector(CurrentPawnLocation) ||
            !IsFiniteVector(CurrentWeaponLocation) ||
            !GetServerAimDirection(
                ShooterController,
                CurrentAimDirection))
        {
            return false;
        }

        FProjectileLedgerEntry* Best = nullptr;
        FVector BestOrigin{};
        FVector BestDirection{};
        int64 BestShotIndex = -1;
        bool BestNeedsTimestampBinding = false;
        const bool IsValidatedMountedWeapon =
            ResolveMountedHostVehicleForTrace(
                Weapon,
                ShooterPawn) != nullptr;
        double BestScore =
            (std::numeric_limits<double>::max)();
        for (auto& Entry : GProjectileLedger)
        {
            if (!Entry.Active || Entry.Reserved)
                continue;

            if (IsProjectileEntryExpired(Entry, Now))
            {
                Entry.Active = false;
                Entry.Reserved = false;
                continue;
            }

            if (Entry.WeaponIdentity != Weapon ||
                Entry.ShooterIdentity != ShooterPawn)
            {
                continue;
            }

            if (ResolveWeakObject(Entry.Weapon) != Weapon ||
                ResolveWeakObject(Entry.Shooter) !=
                    ShooterPawn)
            {
                continue;
            }

            if (!Entry.Automatic &&
                Entry.RemainingSingleShotHits <= 0)
            {
                continue;
            }

            const bool NeedsTimestampBinding =
                !Entry.TimestampFromClient;
            const double ReportedTimestampDelta =
                NeedsTimestampBinding
                    ? 0.0
                    : static_cast<double>(
                        ProjectileTimestamp -
                        Entry.ProjectileTimestamp);
            double LaunchDeltaSeconds = 0.0;
            double TimestampError = 0.0;
            int64 CandidateShotIndex = 0;
            if (Entry.Automatic)
            {
                const double AutomaticShotInterval =
                    Entry.ShotInterval;
                if (!std::isfinite(
                        AutomaticShotInterval) ||
                    AutomaticShotInterval <= 0.0 ||
                    AutomaticShotInterval > 1.0)
                {
                    continue;
                }

                if (!std::isfinite(ReportedTimestampDelta) ||
                    ReportedTimestampDelta <
                        -kProjectileTimestampToleranceSeconds)
                {
                    continue;
                }

                const double ShotIndex =
                    std::round(
                        (std::max)(
                            0.0,
                            ReportedTimestampDelta) /
                        AutomaticShotInterval);
                if (!std::isfinite(ShotIndex) ||
                    ShotIndex < 0.0 ||
                    ShotIndex >
                        static_cast<double>(
                            (std::numeric_limits<int64>::max)()))
                {
                    continue;
                }
                CandidateShotIndex =
                    static_cast<int64>(ShotIndex);
                LaunchDeltaSeconds =
                    ShotIndex * AutomaticShotInterval;
                if (LaunchDeltaSeconds >
                    static_cast<double>(
                        kMaxAutomaticStreamSeconds))
                {
                    continue;
                }
                TimestampError =
                    std::abs(
                        ReportedTimestampDelta -
                        LaunchDeltaSeconds);
                const double TimestampTolerance =
                    (std::max)(
                        static_cast<double>(
                            kProjectileTimestampToleranceSeconds),
                        AutomaticShotInterval * 0.35);
                if (TimestampError > TimestampTolerance)
                    continue;
            }
            else
            {
                TimestampError =
                    std::abs(ReportedTimestampDelta);
                if (TimestampError >
                    kProjectileTimestampToleranceSeconds)
                {
                    continue;
                }
            }

            const double StreamElapsed =
                static_cast<double>(
                    Now - Entry.RecordedAt);
            const double FutureSlotGrace =
                Entry.Automatic
                    ? (std::min)(
                        static_cast<double>(
                            kProjectileTimestampToleranceSeconds),
                        Entry.ShotInterval * 0.10)
                    : 0.0;
            if (LaunchDeltaSeconds >
                StreamElapsed +
                    FutureSlotGrace)
            {
                continue;
            }

            if (Entry.StoppedAt >= 0.f &&
                LaunchDeltaSeconds >
                    static_cast<double>(
                        Entry.StoppedAt -
                        Entry.RecordedAt) +
                        FutureSlotGrace)
            {
                continue;
            }

            const double FlightElapsed =
                StreamElapsed - LaunchDeltaSeconds;
            if (FlightElapsed <
                    -FutureSlotGrace ||
                FlightElapsed >
                    static_cast<double>(
                        kMaxProjectileAgeSeconds) +
                        kProjectileFlightTimeGraceSeconds)
            {
                continue;
            }

            FVector CandidateOrigin = Entry.Start;
            FVector CandidateDirection =
                Entry.Direction;
            bool UsesServerOwnedLaunchOrigin = true;
            double OriginError =
                FVector::Dist(
                    Entry.Start,
                    ReportedProjectileOrigin);
            if (Entry.Automatic &&
                LaunchDeltaSeconds >
                    kProjectileTimestampToleranceSeconds)
            {
                const double CurrentOriginError =
                    (std::min)(
                        FVector::Dist(
                            CurrentPawnLocation,
                            ReportedProjectileOrigin),
                        FVector::Dist(
                            CurrentWeaponLocation,
                            ReportedProjectileOrigin));
                const double MaxCurrentOriginError =
                    kMaxLaunchOriginDriftCm +
                    kMaxPlausibleShooterSpeedCmPerSecond *
                        ((std::max)(0.0, FlightElapsed) +
                            kAutomaticTimestampGraceSeconds);
                const double MaxInitialOriginError =
                    kMaxLaunchOriginDriftCm +
                    kMaxPlausibleShooterSpeedCmPerSecond *
                        (LaunchDeltaSeconds +
                            kAutomaticTimestampGraceSeconds);
                if (CurrentOriginError >
                        MaxCurrentOriginError ||
                    OriginError >
                        MaxInitialOriginError)
                {
                    continue;
                }

                // Never turn the client-reported origin into the authorizing
                // ray for a later automatic slot. Use the current
                // server-owned weapon origin and let the bounded parallax
                // corridor absorb the normal camera/muzzle baseline.
                CandidateOrigin = FVector(
                    CurrentWeaponLocation.X,
                    CurrentWeaponLocation.Y,
                    CurrentWeaponLocation.Z);
                CandidateDirection =
                    CurrentAimDirection;
                OriginError =
                    FVector::Dist(
                        CandidateOrigin,
                        ReportedProjectileOrigin);
            }
            else if (OriginError >
                kMaxRecordedOriginErrorCm)
            {
                continue;
            }

            double DirectionCorrelation = 0.0;
            const FVector TravelDelta =
                ImpactPoint - CandidateOrigin;
            const double TravelDistance =
                TravelDelta.Magnitude();
            const FVector TravelDirection =
                TravelDelta.GetSafeNormal();
            bool DirectionValid =
                !TravelDirection.IsZero() &&
                ValidateProjectileTravelDirection(
                    CandidateDirection,
                    TravelDirection,
                    DirectionCorrelation);
            if (!DirectionValid &&
                UsesServerOwnedLaunchOrigin &&
                std::isfinite(TravelDistance))
            {
                // Camera aim and the physical muzzle are separated by a
                // meaningful baseline. Use the bounded parallax corridor for
                // every recorded one-shot origin so upper-body/ADS shots are
                // not rejected merely because a fixed angular cone points
                // through the lower torso at short range.
                DirectionValid =
                    ValidateCompatibilityTravelCorridor(
                        CandidateDirection,
                        TravelDelta,
                        DirectionCorrelation);
                if (!DirectionValid &&
                    IsValidatedMountedWeapon)
                {
                    // A vehicle weapon actor is located at its turret pivot,
                    // not at the muzzle used by the projectile RPC. Once the
                    // authoritative host seat proves this is the shooter's
                    // mounted weapon, allow that bounded pivot/muzzle baseline
                    // without relaxing any ordinary weapon.
                    DirectionValid =
                        ValidateCompatibilityTravelCorridor(
                            CandidateDirection,
                            TravelDelta,
                            DirectionCorrelation,
                            kCompatibilityRearwardSlackCm,
                            kMountedWeaponParallaxAllowanceCm);
                }
                if (!DirectionValid &&
                    TravelDistance <=
                        kCompatibilityContactRangeCm)
                {
                    // The muzzle can overlap or sit just beyond a nearby
                    // target surface; retain a larger bounded rearward slack
                    // only for that contact-range geometry.
                    DirectionValid =
                        ValidateCompatibilityTravelCorridor(
                            CandidateDirection,
                            TravelDelta,
                            DirectionCorrelation,
                            kCompatibilityContactRearwardSlackCm);
                    if (!DirectionValid &&
                        IsValidatedMountedWeapon)
                    {
                        DirectionValid =
                            ValidateCompatibilityTravelCorridor(
                                CandidateDirection,
                                TravelDelta,
                                DirectionCorrelation,
                                kCompatibilityContactRearwardSlackCm,
                                kMountedWeaponParallaxAllowanceCm);
                    }
                }
            }
            if (!DirectionValid)
            {
                continue;
            }

            if (!std::isfinite(TravelDistance) ||
                TravelDistance >
                    kMaxReportedTravelCm ||
                TravelDistance >
                    kMaxConservativeProjectileSpeedCmPerSecond *
                    ((std::max)(0.0, FlightElapsed) +
                        kProjectileFlightTimeGraceSeconds))
            {
                continue;
            }

            const double Score =
                OriginError +
                (1.0 - DirectionCorrelation) * 1000.0 +
                TimestampError * 10000.0 +
                (std::max)(0.0, FlightElapsed) * 0.1 -
                (Entry.AuthoritativeShotSnapshot
                    ? 0.01
                    : 0.0);
            if (!Best || Score < BestScore)
            {
                Best = &Entry;
                BestOrigin = CandidateOrigin;
                BestDirection = CandidateDirection;
                BestShotIndex = CandidateShotIndex;
                BestNeedsTimestampBinding =
                    NeedsTimestampBinding;
                BestScore = Score;
            }
        }

        if (!Best)
        {
            if (TryReserveCompatibilityLaunch(
                    Weapon,
                    ShooterPawn,
                    ReportedProjectileOrigin,
                    ImpactPoint,
                    ProjectileTimestamp,
                    Now,
                    CurrentPawnLocation,
                    CurrentWeaponLocation,
                    CurrentAimDirection,
                    OutLaunchOrigin,
                    OutLaunchDirection,
                    OutReservation))
            {
                return true;
            }

            static bool WarnedMissingLaunch = false;
            if (!WarnedMissingLaunch)
            {
                WarnedMissingLaunch = true;
                SDK::DbgLog(
                    "  [ProjectileDamage] hit had no matching server-observed projectile launch; report rejected\n");
            }
            return false;
        }

        Best->Reserved = true;
        Best->ReservedShotIndex = BestShotIndex;
        if (BestNeedsTimestampBinding)
        {
            Best->ReservedInitializedTimestamp = true;
            Best->ReservedPreviousTimestamp =
                Best->ProjectileTimestamp;
            Best->ProjectileTimestamp =
                ProjectileTimestamp;
            Best->TimestampFromClient = true;
        }
        OutLaunchOrigin = BestOrigin;
        OutLaunchDirection = BestDirection;
        OutReservation = Best;
        return true;
    }

    void ReleaseProjectileReservation(
        FProjectileLedgerEntry* Reservation,
        bool Commit)
    {
        if (!Reservation || !Reservation->Reserved)
            return;

        if (!Commit &&
            Reservation->ReservedInitializedTimestamp)
        {
            Reservation->ProjectileTimestamp =
                Reservation->ReservedPreviousTimestamp;
            Reservation->TimestampFromClient = false;
        }
        if (Commit && !Reservation->Automatic)
        {
            Reservation->RemainingSingleShotHits--;
            // Keep an exhausted launch as a replay tombstone until its normal
            // flight-age expiry. The canonical shot budget will reject any
            // report beyond the snapshotted pellet count.
            if (Reservation->RemainingSingleShotHits < 0)
                Reservation->RemainingSingleShotHits = 0;
        }
        Reservation->ReservedInitializedTimestamp = false;
        Reservation->ReservedPreviousTimestamp = 0.f;
        Reservation->ReservedShotIndex = -1;
        Reservation->Reserved = false;
    }

    void StopProjectileStreams(
        AFortWeaponRanged* Weapon)
    {
        const float Now = GetServerTimeSeconds();
        if (!IsUsableActor(Weapon) ||
            !SynchronizeProjectileTimeEpoch(Now))
        {
            return;
        }

        for (auto& Entry : GProjectileLedger)
        {
            if (Entry.Active &&
                Entry.Automatic &&
                Entry.StoppedAt < 0.f &&
                Entry.WeaponIdentity == Weapon &&
                ResolveWeakObject(Entry.Weapon) == Weapon)
            {
                Entry.StoppedAt = Now;
            }
        }
    }

    void EndProjectileVisualRelay(
        AFortWeaponRanged* Weapon)
    {
        auto RelayState =
            GetProjectileVisualRelayState(Weapon, false);
        if (!RelayState)
            return;

        const bool HadActiveVisual =
            RelayState->Active;
        const float ActiveVisualFireToken =
            RelayState->ActiveVisualFireToken;
        RelayState->Active = false;
        RelayState->ActiveVisualFireToken = -1.f;
        RelayState->CaptureActive = false;
        RelayState->CaptureActiveBeforePending = false;
        RelayState->PendingActivation = false;
        RelayState->PendingStartedAt = -1.f;
        RelayState->PendingBaselineFireToken = -1.f;
        RelayState->HasLatestDamageState = false;
        RelayState->LatestDamageStateAt = -1.f;
        if (!HadActiveVisual)
            return;

        if (!IsUsableActor(Weapon) ||
            !Weapon->HasAuthority() ||
            !GLightweightProjectileVisualSchema
                .CanEndStream() ||
            !std::isfinite(
                ActiveVisualFireToken) ||
            ActiveVisualFireToken <= 0.f)
        {
            return;
        }

        alignas(16) std::array<uint8, 0x20> Params{};
        if (!WriteValue(
                Params.data(),
                GLightweightProjectileVisualSchema
                    .EndActiveAbilitySize,
                GLightweightProjectileVisualSchema
                    .EndFiringTimestamp,
                ActiveVisualFireToken))
        {
            return;
        }
        Weapon->ProcessEvent(
            GLightweightProjectileVisualSchema
                .EndActiveAbility,
            Params.data());
    }

    void GetWeaponHitLimits(
        AFortWeaponRanged* Weapon,
        int32& OutRateLimit,
        int32& OutTimestampLimit)
    {
        int32 BulletsPerCartridge =
            GetBulletsPerCartridge(Weapon);
        float FiringRate = 4.f;
        if (Weapon &&
            Weapon->HasWeaponData() &&
            IsUsableObject(Weapon->WeaponData))
        {
            auto Stats =
                AFortInventory::GetStats(Weapon->WeaponData);
            if (Stats &&
                FFortRangedWeaponStats::HasFiringRate())
            {
                const float ReportedRate =
                    Stats->FiringRate;
                if (std::isfinite(ReportedRate) &&
                    ReportedRate > 0.f &&
                    ReportedRate <= 100.f)
                {
                    FiringRate = ReportedRate < 1.f
                        ? 1.f / ReportedRate
                        : ReportedRate;
                }
            }
        }

        const float ExpectedHitsPerSecond =
            std::clamp(
                FiringRate *
                    static_cast<float>(
                        BulletsPerCartridge),
                1.f,
                256.f);
        OutRateLimit = std::clamp(
            static_cast<int32>(
                std::ceil(
                    ExpectedHitsPerSecond * 4.f)) + 8,
            16,
            512);
        OutTimestampLimit = std::clamp(
            BulletsPerCartridge,
            1,
            64);
    }

    bool ConsumeLaunchBudget(
        AFortWeaponRanged* Weapon,
        float Now,
        FProjectileLedgerEntry* LaunchReservation,
        uint64 ReportFingerprint,
        FHitBudgetReservation& OutReservation)
    {
        OutReservation = {};
        if (!IsUsableActor(Weapon) ||
            !std::isfinite(Now) ||
            Now < 0.f ||
            !LaunchReservation ||
            !LaunchReservation->Reserved ||
            LaunchReservation->Generation == 0 ||
            LaunchReservation->ReservedShotIndex < 0 ||
            ReportFingerprint == 0)
        {
            return false;
        }

        int32 RateLimit = 0;
        int32 TimestampLimit = 0;
        GetWeaponHitLimits(
            Weapon,
            RateLimit,
            TimestampLimit);
        TimestampLimit = std::clamp(
            LaunchReservation->PelletsPerShot,
            1,
            64);

        FWeaponHitBudget* WeaponBudget = nullptr;
        for (auto& Entry : GLaunchBudgets)
        {
            if (Entry.WeaponIdentity == Weapon &&
                ResolveWeakObject(Entry.Weapon) == Weapon)
            {
                WeaponBudget = &Entry;
                break;
            }
        }

        if (!WeaponBudget)
        {
            WeaponBudget =
                &GLaunchBudgets[
                    GLaunchBudgetCursor %
                    GLaunchBudgets.size()];
            GLaunchBudgetCursor++;
            *WeaponBudget = FWeaponHitBudget{};
            WeaponBudget->Weapon =
                TWeakObjectPtr<AFortWeaponRanged>(Weapon);
            WeaponBudget->WeaponIdentity = Weapon;
        }

        if (WeaponBudget->WindowStart < 0.f ||
            Now < WeaponBudget->WindowStart ||
            Now - WeaponBudget->WindowStart >=
                kHitRateWindowSeconds)
        {
            WeaponBudget->WindowStart = Now;
            WeaponBudget->Count = 0;
        }

        if (WeaponBudget->Count >= RateLimit)
            return false;

        constexpr size_t ShotBudgetWays = 8;
        constexpr size_t ShotBudgetBucketCount =
            GCanonicalShotBudgets.size() /
            ShotBudgetWays;
        constexpr size_t ShotBudgetBucketMask =
            ShotBudgetBucketCount - 1;
        static_assert(
            GCanonicalShotBudgets.size() %
                    ShotBudgetWays == 0 &&
                (ShotBudgetBucketCount &
                    ShotBudgetBucketMask) == 0,
            "canonical shot budget needs power-of-two buckets");

        const uint64 ShotKey =
            MixHash64(
                LaunchReservation->Generation ^
                MixHash64(
                    static_cast<uint64>(
                        LaunchReservation->
                            ReservedShotIndex) +
                    0x9E3779B97F4A7C15ull)) |
            1ull;
        uint64 Hash =
            LaunchReservation->Generation ^
            (static_cast<uint64>(
                LaunchReservation->ReservedShotIndex) +
                0x9E3779B97F4A7C15ull);
        Hash = MixHash64(Hash);

        FCanonicalShotBudget* ShotBudget = nullptr;
        FCanonicalShotBudget* Reusable = nullptr;
        const size_t BucketStart =
            (static_cast<size_t>(Hash) &
                ShotBudgetBucketMask) *
            ShotBudgetWays;
        for (size_t Way = 0;
            Way < ShotBudgetWays;
            Way++)
        {
            auto& Entry =
                GCanonicalShotBudgets[
                    BucketStart + Way];
            const bool SameShot =
                Entry.StreamGeneration ==
                    LaunchReservation->Generation &&
                Entry.ShotIndex ==
                    LaunchReservation->ReservedShotIndex;
            const bool Expired =
                Entry.StreamGeneration == 0 ||
                !std::isfinite(Entry.LastSeen) ||
                Entry.LastSeen < 0.f ||
                Now < Entry.LastSeen ||
                Now - Entry.LastSeen >
                    kShotReplayRetentionSeconds;
            if (SameShot)
            {
                ShotBudget = &Entry;
                break;
            }

            if (Expired && !Reusable)
                Reusable = &Entry;
        }

        if (!ShotBudget)
            ShotBudget = Reusable;
        if (!ShotBudget)
            return false;

        if (ShotBudget->StreamGeneration !=
                LaunchReservation->Generation ||
            ShotBudget->ShotIndex !=
                LaunchReservation->ReservedShotIndex)
        {
            *ShotBudget = FCanonicalShotBudget{};
            ShotBudget->StreamGeneration =
                LaunchReservation->Generation;
            ShotBudget->ShotIndex =
                LaunchReservation->ReservedShotIndex;
        }

        if (ShotBudget->Count >= TimestampLimit)
            return false;

        constexpr size_t ReportBudgetWays = 8;
        constexpr size_t ReportBudgetBucketCount =
            GProjectileReportBudgets.size() /
            ReportBudgetWays;
        constexpr size_t ReportBudgetBucketMask =
            ReportBudgetBucketCount - 1;
        static_assert(
            GProjectileReportBudgets.size() %
                    ReportBudgetWays == 0 &&
                (ReportBudgetBucketCount &
                    ReportBudgetBucketMask) == 0,
            "report replay budget needs power-of-two buckets");

        const uint64 ReportHash =
            MixHash64(ReportFingerprint);
        const size_t ReportBucketStart =
            (static_cast<size_t>(ReportHash) &
                ReportBudgetBucketMask) *
            ReportBudgetWays;
        FProjectileReportBudget* ReportBudget = nullptr;
        FProjectileReportBudget* ReusableReport = nullptr;
        for (size_t Way = 0;
            Way < ReportBudgetWays;
            Way++)
        {
            auto& Entry =
                GProjectileReportBudgets[
                    ReportBucketStart + Way];
            const bool SameReport =
                Entry.Fingerprint == ReportFingerprint;
            const bool Expired =
                Entry.ShotKey == 0 ||
                !std::isfinite(Entry.LastSeen) ||
                Entry.LastSeen < 0.f ||
                Now < Entry.LastSeen ||
                (Now >= Entry.LastSeen &&
                    Now - Entry.LastSeen >
                        kShotReplayRetentionSeconds);
            if (SameReport && !Expired)
            {
                // A pellet count is only an aggregate upper bound. It must
                // never let an identical accepted report consume another
                // pellet slot.
                return false;
            }

            if (Expired && !ReusableReport)
                ReusableReport = &Entry;
        }

        ReportBudget = ReusableReport;
        if (!ReportBudget)
            return false;

        *ReportBudget = FProjectileReportBudget{};
        ReportBudget->ShotKey = ShotKey;
        ReportBudget->Fingerprint = ReportFingerprint;
        ReportBudget->LastSeen = Now;

        ++WeaponBudget->Count;
        ++ShotBudget->Count;
        ShotBudget->LastSeen = Now;
        OutReservation.WeaponBudget = WeaponBudget;
        OutReservation.ShotBudget = ShotBudget;
        OutReservation.ReportBudget = ReportBudget;
        OutReservation.Active = true;
        return true;
    }

    void ReleaseHitBudgetReservation(
        FHitBudgetReservation& Reservation,
        bool Commit)
    {
        if (!Reservation.Active)
            return;

        if (!Commit)
        {
            if (Reservation.ReportBudget)
            {
                *Reservation.ReportBudget =
                    FProjectileReportBudget{};
            }
            if (Reservation.WeaponBudget &&
                Reservation.WeaponBudget->Count > 0)
            {
                --Reservation.WeaponBudget->Count;
            }
            if (Reservation.ShotBudget &&
                Reservation.ShotBudget->Count > 0)
            {
                --Reservation.ShotBudget->Count;
                if (Reservation.ShotBudget->Count == 0)
                    *Reservation.ShotBudget =
                        FCanonicalShotBudget{};
            }
        }

        Reservation = {};
    }

    bool ConsumeHitIngressBudget(
        AFortWeaponRanged* Weapon,
        float Now)
    {
        constexpr int32 kMaxReportsPerWeaponPerSecond = 128;
        if (!IsUsableActor(Weapon) ||
            !std::isfinite(Now) ||
            Now < 0.f)
        {
            return false;
        }

        FWeaponHitBudget* Budget = nullptr;
        for (auto& Entry : GIngressBudgets)
        {
            if (Entry.WeaponIdentity == Weapon &&
                ResolveWeakObject(Entry.Weapon) == Weapon)
            {
                Budget = &Entry;
                break;
            }
        }

        if (!Budget)
        {
            Budget =
                &GIngressBudgets[
                    GIngressBudgetCursor %
                    GIngressBudgets.size()];
            GIngressBudgetCursor++;
            *Budget = FWeaponHitBudget{};
            Budget->Weapon =
                TWeakObjectPtr<AFortWeaponRanged>(
                    Weapon);
            Budget->WeaponIdentity = Weapon;
        }

        if (Budget->WindowStart < 0.f ||
            Now < Budget->WindowStart ||
            Now - Budget->WindowStart >=
                kHitRateWindowSeconds)
        {
            Budget->WindowStart = Now;
            Budget->Count = 0;
        }

        if (Budget->Count >=
            kMaxReportsPerWeaponPerSecond)
        {
            return false;
        }

        Budget->Count++;
        return true;
    }

    bool BuildDamageRequest(
        AFortWeaponRanged* Weapon,
        const void* HitMemory,
        uint32 HitSize,
        const FVector& ProjectileOrigin,
        float ProjectileTimestamp,
        bool HasProjectileTimestamp,
        FDamageRequest& OutRequest,
        bool& OutSuppressOriginal)
    {
        // Once a native-frame Chapter 5 hit reaches this replacement, invalid
        // and unsupported reports fail closed. Passing a rejected report to
        // the old handler would create a second, replayable damage path on a
        // sub-build where Epic's native implementation happens to work.
        OutSuppressOriginal = true;
        const float Now = GetServerTimeSeconds();
        if (!SynchronizeProjectileTimeEpoch(Now))
        {
            LogProjectileDiagnostic(
                "reject",
                "time-epoch",
                Weapon);
            return false;
        }

        if (!ConsumeHitIngressBudget(Weapon, Now))
        {
            LogProjectileDiagnostic(
                "reject",
                "ingress-budget",
                Weapon);
            return false;
        }

        FResolvedHit Hit{};
        if (!ResolveHit(HitMemory, HitSize, Hit))
        {
            LogProjectileDiagnostic(
                "reject",
                "hit-resolve",
                Weapon);
            return false;
        }
        if (!IsFiniteVector(ProjectileOrigin))
        {
            LogProjectileDiagnostic(
                "reject",
                "projectile-origin",
                Weapon,
                Hit.Target);
            return false;
        }
        AFortPlayerPawnAthena* ShooterPawn = nullptr;
        AFortPlayerControllerAthena* ShooterController =
            nullptr;
        if (!ResolveShooter(
                Weapon,
                ShooterPawn,
                ShooterController))
        {
            LogProjectileDiagnostic(
                "reject",
                "shooter",
                Weapon,
                Hit.Target);
            return false;
        }
        if (!WeaponBelongsToPawn(ShooterPawn, Weapon))
        {
            LogProjectileDiagnostic(
                "reject",
                "weapon-ownership",
                Weapon,
                Hit.Target);
            return false;
        }
        if (Hit.Target == ShooterPawn)
        {
            LogProjectileDiagnostic(
                "reject",
                "self-hit",
                Weapon,
                Hit.Target);
            return false;
        }
        if (!MatchAllowsDamage(ShooterController))
        {
            LogProjectileDiagnostic(
                "reject",
                "match-phase",
                Weapon,
                Hit.Target);
            return false;
        }

        if (!Hit.Target->HasbCanBeDamaged() ||
            !Hit.Target->bCanBeDamaged)
        {
            LogProjectileDiagnostic(
                "reject",
                "target-damageable",
                Weapon,
                Hit.Target);
            return false;
        }
        auto TargetPlayerPawn =
            AsPlayerPawn(Hit.Target);
        auto TargetVehicle =
            Hit.Target->Cast<AFortAthenaVehicle>();
        const bool ImpactWithinTarget =
            TargetPlayerPawn
                ? IsImpactWithinPlayerEnvelope(
                    TargetPlayerPawn,
                    Hit.ImpactPoint)
                : IsImpactWithinActorBounds(
                    Hit.Target,
                    Hit.ImpactPoint,
                    true);
        if (!ImpactWithinTarget)
        {
            LogProjectileDiagnostic(
                "reject",
                "target-bounds",
                Weapon,
                Hit.Target);
            return false;
        }

        if (TargetPlayerPawn)
        {
            if ((TargetPlayerPawn->HasbIsDying() &&
                    TargetPlayerPawn->bIsDying))
            {
                LogProjectileDiagnostic(
                    "reject",
                    "target-dying",
                    Weapon,
                    Hit.Target);
                return false;
            }
            if (!AreEnemies(
                    ShooterController,
                    TargetPlayerPawn))
            {
                LogProjectileDiagnostic(
                    "reject",
                    "target-team",
                    Weapon,
                    Hit.Target);
                return false;
            }

        }

        auto ShooterAbilitySystem =
            GetPlayerAbilitySystem(ShooterController);
        if (!ShooterAbilitySystem)
        {
            LogProjectileDiagnostic(
                "reject",
                "shooter-asc",
                Weapon,
                Hit.Target);
            return false;
        }
        UFortGameplayAbility* FireAbility = nullptr;
        int32 AbilityLevel = 1;
        FDamageEffectSet Effects{};
        bool ResolvedDamageAbility = false;
        if (ShooterAbilitySystem &&
            ResolveFireAbility(
                Weapon,
                ShooterAbilitySystem,
                false,
                FireAbility,
                AbilityLevel) &&
            ResolveDamageEffects(
                FireAbility,
                TargetPlayerPawn == nullptr,
                Effects))
        {
            ResolvedDamageAbility = true;
        }

        if (!ResolvedDamageAbility &&
            ShooterAbilitySystem &&
            ResolveFireAbility(
                Weapon,
                ShooterAbilitySystem,
                true,
                FireAbility,
                AbilityLevel) &&
            ResolveDamageEffects(
                FireAbility,
                TargetPlayerPawn == nullptr,
                Effects))
        {
            ResolvedDamageAbility = true;
        }

        if (!ResolvedDamageAbility)
        {
            static bool WarnedMissingDamageContainer = false;
            if (!WarnedMissingDamageContainer)
            {
                WarnedMissingDamageContainer = true;
                SDK::DbgLog(
                    "  [ProjectileDamage] no target gameplay effects were found on the primary or impact ability\n");
            }
            LogProjectileDiagnostic(
                "reject",
                "ability-effects",
                Weapon,
                Hit.Target);
            return false;
        }

        auto TargetAbilitySystem =
            GetActorAbilitySystem(Hit.Target);
        if (!TargetAbilitySystem && TargetPlayerPawn)
        {
            TargetAbilitySystem =
                GetPlayerAbilitySystem(TargetPlayerPawn);
        }
        if (!TargetAbilitySystem && !TargetPlayerPawn)
        {
            auto CreateBuildingAttributes =
                Hit.Target->GetFunction(
                    "GetBuildingAttributeSet");
            if (CreateBuildingAttributes)
            {
                const auto Params =
                    CreateBuildingAttributes
                        ->GetParamsNamed();
                if (Params.Size <= 0x20)
                {
                    alignas(16)
                        std::array<uint8, 0x20>
                            CreateParams{};
                    Hit.Target->ProcessEvent(
                        CreateBuildingAttributes,
                        CreateParams.data());
                    TargetAbilitySystem =
                        GetActorAbilitySystem(
                            Hit.Target);
                }
            }
        }
        if (!TargetAbilitySystem)
        {
            LogProjectileDiagnostic(
                "reject",
                "target-asc",
                Weapon,
                Hit.Target);
            return false;
        }

        FVector LaunchOrigin{};
        FVector LaunchDirection{};
        FProjectileLedgerEntry* LaunchReservation =
            nullptr;
        FHitBudgetReservation HitBudgetReservation{};
        if (!ReserveProjectileLaunch(
                Weapon,
                ShooterPawn,
                ShooterController,
                ProjectileOrigin,
                Hit.ImpactPoint,
                ProjectileTimestamp,
                Now,
                LaunchOrigin,
                LaunchDirection,
                LaunchReservation))
        {
            LogProjectileDiagnostic(
                "reject",
                "projectile-ledger",
                Weapon,
                Hit.Target);
            return false;
        }

        // From this point onward the report has consumed the right to use a
        // validated launch record. A failed replay/rate/geometry check must
        // not escape through the native fallback on a fixed sub-build.
        OutSuppressOriginal = true;
        const uint64 ReportFingerprint =
            BuildProjectileReportFingerprint(
                Weapon,
                Hit.Target,
                ProjectileOrigin,
                Hit.ImpactPoint,
                ProjectileTimestamp);
        auto RejectReserved =
            [&](const char* Stage)
        {
            ReleaseHitBudgetReservation(
                HitBudgetReservation,
                false);
            ReleaseProjectileReservation(
                LaunchReservation,
                false);
            LogProjectileDiagnostic(
                "reject",
                Stage,
                Weapon,
                Hit.Target);
        };
        if (LaunchDirection.IsZero())
        {
            RejectReserved("launch-direction");
            return false;
        }
        if (FVector::Dist(
                LaunchOrigin,
                Hit.ImpactPoint) >
            kMaxReportedTravelCm)
        {
            RejectReserved("travel-distance");
            return false;
        }
        if (!HasServerLineOfSight(
                ShooterController,
                Weapon,
                Hit.Target,
                LaunchOrigin,
                LaunchDirection,
                ProjectileOrigin,
                Hit.ImpactPoint))
        {
            RejectReserved("line-of-sight");
            return false;
        }
        if (!ConsumeLaunchBudget(
                Weapon,
                Now,
                LaunchReservation,
                ReportFingerprint,
                HitBudgetReservation))
        {
            RejectReserved("shot-budget");
            return false;
        }

        OutRequest.Weapon = Weapon;
        OutRequest.ShooterPawn = ShooterPawn;
        OutRequest.ShooterController =
            ShooterController;
        OutRequest.ShooterAbilitySystem =
            ShooterAbilitySystem;
        OutRequest.TargetAbilitySystem =
            TargetAbilitySystem;
        OutRequest.FireAbility = FireAbility;
        OutRequest.Target = Hit.Target;
        OutRequest.TargetPlayerPawn =
            TargetPlayerPawn;
        OutRequest.TargetVehicle =
            TargetVehicle;
        OutRequest.TargetHitComponent =
            Hit.TargetComponent;
        std::memcpy(
            &OutRequest.ProjectileOrigin,
            &LaunchOrigin,
            FVector::Size());
        std::memcpy(
            &OutRequest.LaunchDirection,
            &LaunchDirection,
            FVector::Size());
        std::memcpy(
            &OutRequest.ImpactPoint,
            &Hit.ImpactPoint,
            FVector::Size());
        OutRequest.ProjectileTimestamp =
            ProjectileTimestamp;
        OutRequest.HasProjectileTimestamp =
            HasProjectileTimestamp;
        OutRequest.AbilityLevel = AbilityLevel;
        OutRequest.Effects = Effects;
        OutRequest.LaunchReservation =
            LaunchReservation;
        OutRequest.HitBudgetReservation =
            HitBudgetReservation;
        return true;
    }

    const uint8* MakeFortEffectContext(
        const FDamageRequest& Request)
    {
        if (!GEffectApiSchema.IsValid() ||
            !IsUsableObject(
                Request.ShooterAbilitySystem) ||
            !IsUsableActor(Request.Weapon))
        {
            return nullptr;
        }

        UObject* SourceObject = Request.Weapon;
        AActor* EffectCauser = Request.Weapon;
        AActor* DamageSource = Request.Weapon;
        const int32 Level = Request.AbilityLevel;
        if (!WriteValue(
                GMakeContextParams.data(),
                GEffectApiSchema.MakeFortContextSize,
                GEffectApiSchema.ContextSourceObject,
                SourceObject) ||
            !WriteValue(
                GMakeContextParams.data(),
                GEffectApiSchema.MakeFortContextSize,
                GEffectApiSchema.ContextEffectCauser,
                EffectCauser) ||
            !WriteValue(
                GMakeContextParams.data(),
                GEffectApiSchema.MakeFortContextSize,
                GEffectApiSchema.ContextDamageSource,
                DamageSource) ||
            !WriteValue(
                GMakeContextParams.data(),
                GEffectApiSchema.MakeFortContextSize,
                GEffectApiSchema.ContextLevel,
                Level))
        {
            return nullptr;
        }

        Request.ShooterAbilitySystem->ProcessEvent(
            GEffectApiSchema.MakeFortContext,
            GMakeContextParams.data());
        return GMakeContextParams.data() +
            GEffectApiSchema.ContextReturn.Offset;
    }

    bool ReadContextActor(
        UFunction* Function,
        uint32 ParamsSize,
        const FFieldView& ContextField,
        const FFieldView& ReturnField,
        const uint8* Context,
        AActor*& OutActor)
    {
        alignas(16) std::array<uint8, 0x80> Params{};
        if (!Function ||
            ParamsSize > Params.size() ||
            !WriteBytes(
                Params.data(),
                ParamsSize,
                ContextField,
                Context,
                GEffectApiSchema.ContextReturn.Size))
        {
            return false;
        }

        GEffectApiSchema.BlueprintLibrary->ProcessEvent(
            Function,
            Params.data());
        return ReadValue(
            Params.data(),
            ParamsSize,
            ReturnField,
            OutActor);
    }

    bool ReadContextSource(
        const uint8* Context,
        UObject*& OutSource)
    {
        alignas(16) std::array<uint8, 0x80> Params{};
        if (GEffectApiSchema.GetContextSourceSize >
                Params.size() ||
            !WriteBytes(
                Params.data(),
                GEffectApiSchema.GetContextSourceSize,
                GEffectApiSchema.GetSourceContext,
                Context,
                GEffectApiSchema.ContextReturn.Size))
        {
            return false;
        }

        GEffectApiSchema.BlueprintLibrary->ProcessEvent(
            GEffectApiSchema.GetContextSource,
            Params.data());
        return ReadValue(
            Params.data(),
            GEffectApiSchema.GetContextSourceSize,
            GEffectApiSchema.GetSourceReturn,
            OutSource);
    }

    bool IsKnownNonDamageProjectileEffect(
        UClass* EffectClass)
    {
        if (!IsUsableObject(EffectClass) ||
            !EffectClass->IsA(UClass::StaticClass()))
        {
            return true;
        }

        const auto EffectName =
            EffectClass->Name.ToString();
        return EffectName.find("Impulse") !=
                std::string::npos ||
            EffectName.find("Knockback") !=
                std::string::npos;
    }

    bool ReadUnmodifiedDamage(
        const uint8* Context,
        float& OutDamage)
    {
        OutDamage = 0.f;
        alignas(16) std::array<uint8, 0x40> Params{};
        if (!GDamageFeedbackSchema.IsValid() ||
            !Context ||
            GDamageFeedbackSchema.GetUnmodifiedDamageSize >
                Params.size() ||
            !WriteBytes(
                Params.data(),
                GDamageFeedbackSchema
                    .GetUnmodifiedDamageSize,
                GDamageFeedbackSchema
                    .GetUnmodifiedDamageContext,
                Context,
                kExpectedEffectContextSize))
        {
            return false;
        }

        GDamageFeedbackSchema.FortKismetLibrary
            ->ProcessEvent(
                GDamageFeedbackSchema
                    .GetUnmodifiedDamage,
                Params.data());
        if (!ReadValue(
                Params.data(),
                GDamageFeedbackSchema
                    .GetUnmodifiedDamageSize,
                GDamageFeedbackSchema
                    .GetUnmodifiedDamageReturn,
                OutDamage) ||
            !std::isfinite(OutDamage) ||
            OutDamage < 0.f ||
            OutDamage > kMaxDamageFeedbackMagnitude)
        {
            OutDamage = 0.f;
            return false;
        }
        return true;
    }

    void PawnProcessEventDamageFeedback(
        const UObject* Context,
        UFunction* Function,
        void* Params)
    {
        auto CallOriginal = [&]()
        {
            if (GPawnProcessEventOriginal)
            {
                GPawnProcessEventOriginal(
                    Context,
                    Function,
                    Params);
            }
        };

        // ProcessEvent sees the multicast parameter buffer before Unreal
        // serializes it for remote clients. Patch the reflected
        // FSharedRepMovement bit here; an ExecFunction hook alone runs after
        // RPC routing and therefore cannot repair optic glints.
        if (Function ==
                GTargetingReplicationSchema
                    .FastSharedReplicationFunction &&
            GTargetingReplicationSchema
                .CanPatchFastSharedMovement() &&
            Params &&
            SDK::MemReadable(
                Params,
                GTargetingReplicationSchema
                    .FastSharedReplicationParamsSize))
        {
            auto Pawn =
                IsUsableObject(Context)
                    ? Context->Cast<
                        AFortPlayerPawnAthena>()
                    : nullptr;
            if (Pawn)
            {
                auto SharedMovement =
                    static_cast<uint8*>(Params) +
                    GTargetingReplicationSchema
                        .FastSharedMovement.Offset;
                PatchFastSharedTargeting(
                    Pawn,
                    SharedMovement);
            }
        }

        auto Active = GActiveDamageFeedback;
        if (Active &&
            Context == Active->ShooterPawn &&
            Function ==
                GDamageFeedbackSchema
                    .DisplayHitNotifyFunction &&
            GDamageFeedbackSchema
                .CanRewriteDisplayHitNotify() &&
            Params &&
            SDK::MemReadable(
                Params,
                GDamageFeedbackSchema
                    .DisplayHitNotifyParamsSize))
        {
            alignas(16) std::array<uint8, 0x80>
                ParamsCopy{};
            std::memcpy(
                ParamsCopy.data(),
                Params,
                GDamageFeedbackSchema
                    .DisplayHitNotifyParamsSize);

            AActor* HitActor = nullptr;
            float NativeDamage = 0.f;
            bool NativeCritical = false;
            if (ReadValue(
                    ParamsCopy.data(),
                    GDamageFeedbackSchema
                        .DisplayHitNotifyParamsSize,
                    GDamageFeedbackSchema
                        .DisplayHitActor,
                    HitActor) &&
                HitActor == Active->TargetPawn &&
                ReadValue(
                    ParamsCopy.data(),
                    GDamageFeedbackSchema
                        .DisplayHitNotifyParamsSize,
                    GDamageFeedbackSchema
                        .DisplayDamageDealt,
                    NativeDamage) &&
                ReadValue(
                    ParamsCopy.data(),
                    GDamageFeedbackSchema
                        .DisplayHitNotifyParamsSize,
                    GDamageFeedbackSchema
                        .DisplayCriticalHit,
                    NativeCritical) &&
                std::isfinite(NativeDamage) &&
                NativeDamage >= 0.f &&
                NativeDamage <=
                    kMaxDamageFeedbackMagnitude)
            {
                float UnmodifiedDamage = 0.f;
                const bool ReadDamage =
                    ReadUnmodifiedDamage(
                        Active->Context,
                        UnmodifiedDamage) &&
                    UnmodifiedDamage >
                        kDamageFeedbackEpsilon;
                bool Rewritten = false;
                if (ReadDamage &&
                    UnmodifiedDamage >
                        NativeDamage +
                            kDamageFeedbackEpsilon)
                {
                    Rewritten =
                        WriteValue(
                            ParamsCopy.data(),
                            GDamageFeedbackSchema
                                .DisplayHitNotifyParamsSize,
                            GDamageFeedbackSchema
                                .DisplayDamageDealt,
                            UnmodifiedDamage);
                }

                if (Active->ExpectedCritical &&
                    !NativeCritical)
                {
                    const bool Critical = true;
                    Rewritten =
                        WriteValue(
                            ParamsCopy.data(),
                            GDamageFeedbackSchema
                                .DisplayHitNotifyParamsSize,
                            GDamageFeedbackSchema
                                .DisplayCriticalHit,
                            Critical) ||
                        Rewritten;
                }

                Active->DisplayNotifyHandled = true;
                Active->DisplayNotifyRewritten =
                    Rewritten;
                if (ReadDamage)
                {
                    // The original event now feeds the full value into
                    // Fortnite's own accumulator/flush path. Do not add that
                    // value again in the legacy post-effect fallback.
                    Active->CueHandled = true;
                }

                if (Rewritten &&
                    GPawnProcessEventOriginal)
                {
                    GPawnProcessEventOriginal(
                        Context,
                        Function,
                        ParamsCopy.data());
                    return;
                }
            }

            CallOriginal();
            return;
        }

        if (GInsideBatchedDamageCue ||
            !Active ||
            Active->CueHandled ||
            Context != Active->ShooterPawn ||
            Function !=
                GDamageFeedbackSchema
                    .BatchedCueFunction ||
            !GDamageFeedbackSchema
                .CanRewriteBatchedCue() ||
            !Params ||
            !SDK::MemReadable(
                Params,
                GDamageFeedbackSchema
                    .BatchedCueParamsSize))
        {
            CallOriginal();
            return;
        }

        const bool WasInside =
            GInsideBatchedDamageCue;
        GInsideBatchedDamageCue = true;

        auto SharedMemory =
            static_cast<uint8*>(Params) +
            GDamageFeedbackSchema
                .BatchedCueShared.Offset;
        auto NonSharedMemory =
            static_cast<uint8*>(Params) +
            GDamageFeedbackSchema
                .BatchedCueNonShared.Offset;
        AActor* HitActor = nullptr;
        float NativeMagnitude = 0.f;
        uint8 Valid = 0;
        float UnmodifiedDamage = 0.f;
        bool Rewritten = false;
        float CorrectedMagnitude = NativeMagnitude;
        if (ReadValue(
                NonSharedMemory,
                GDamageFeedbackSchema.NonSharedSize,
                GDamageFeedbackSchema
                    .NonSharedHitActor,
                HitActor) &&
            HitActor == Active->TargetPawn &&
            ReadValue(
                SharedMemory,
                GDamageFeedbackSchema.SharedSize,
                GDamageFeedbackSchema
                    .SharedMagnitude,
                NativeMagnitude) &&
            ReadValue(
                SharedMemory,
                GDamageFeedbackSchema.SharedSize,
                GDamageFeedbackSchema.SharedValid,
                Valid) &&
            Valid != 0 &&
            std::isfinite(NativeMagnitude) &&
            NativeMagnitude >= 0.f &&
            NativeMagnitude <=
                kMaxDamageFeedbackMagnitude &&
            ReadUnmodifiedDamage(
                Active->Context,
                UnmodifiedDamage) &&
            UnmodifiedDamage >
                kDamageFeedbackEpsilon)
        {
            float BaseMagnitude = 0.f;
            if (Active->ReadBefore &&
                Active->Before.Valid &&
                Active->Before.HitActor ==
                    Active->TargetPawn &&
                NativeMagnitude +
                        kDamageFeedbackEpsilon >=
                    Active->Before.Magnitude)
            {
                BaseMagnitude =
                    Active->Before.Magnitude;
            }

            const float NativeContribution =
                NativeMagnitude - BaseMagnitude;
            CorrectedMagnitude =
                BaseMagnitude +
                UnmodifiedDamage;
            const bool AlreadyFull =
                std::isfinite(NativeContribution) &&
                NativeContribution >
                    kDamageFeedbackEpsilon &&
                UnmodifiedDamage <=
                    NativeContribution +
                        kDamageFeedbackEpsilon;
            if (std::isfinite(NativeContribution) &&
                NativeContribution >
                    kDamageFeedbackEpsilon &&
                UnmodifiedDamage >
                    NativeContribution +
                        kDamageFeedbackEpsilon &&
                std::isfinite(CorrectedMagnitude) &&
                CorrectedMagnitude <=
                    kMaxDamageFeedbackMagnitude)
            {
                Rewritten =
                    WriteValue(
                        SharedMemory,
                        GDamageFeedbackSchema
                            .SharedSize,
                        GDamageFeedbackSchema
                            .SharedMagnitude,
                        CorrectedMagnitude);
                Active->CueRewritten = Rewritten;
            }
            Active->CueHandled =
                AlreadyFull || Rewritten;

            static int32 TraceCount = 0;
            if (TraceCount++ < 4)
            {
                SDK::DbgLog(
                    "  [ProjectileDamage] display-cue target=%p native=%.2f raw=%.2f corrected=%.2f rewritten=%d\n",
                    static_cast<void*>(
                        Active->TargetPawn),
                    NativeMagnitude,
                    UnmodifiedDamage,
                    CorrectedMagnitude,
                    Rewritten);
            }
        }

        CallOriginal();
        GInsideBatchedDamageCue = WasInside;
    }

    bool ReadDamageBatchSnapshot(
        AFortPlayerPawnAthena* ShooterPawn,
        FDamageBatchSnapshot& OutSnapshot)
    {
        OutSnapshot = {};
        if (!GDamageFeedbackSchema.IsValid() ||
            !IsUsableActor(ShooterPawn) ||
            !SDK::MemReadable(
                ShooterPawn,
                GDamageFeedbackSchema.PawnSize))
        {
            return false;
        }

        const auto PawnMemory =
            reinterpret_cast<const uint8*>(ShooterPawn);
        const auto SharedMemory =
            PawnMemory +
            GDamageFeedbackSchema.AccumulatedShared.Offset;
        const auto NonSharedMemory =
            PawnMemory +
            GDamageFeedbackSchema.AccumulatedNonShared.Offset;
        uint8 Valid = 0;
        uint8 Critical = 0;
        if (!ReadValue(
                SharedMemory,
                GDamageFeedbackSchema.SharedSize,
                GDamageFeedbackSchema.SharedMagnitude,
                OutSnapshot.Magnitude) ||
            !ReadValue(
                SharedMemory,
                GDamageFeedbackSchema.SharedSize,
                GDamageFeedbackSchema.SharedCritical,
                Critical) ||
            !ReadValue(
                SharedMemory,
                GDamageFeedbackSchema.SharedSize,
                GDamageFeedbackSchema.SharedValid,
                Valid) ||
            !ReadValue(
                NonSharedMemory,
                GDamageFeedbackSchema.NonSharedSize,
                GDamageFeedbackSchema.NonSharedHitActor,
                OutSnapshot.HitActor) ||
            !std::isfinite(OutSnapshot.Magnitude) ||
            OutSnapshot.Magnitude < 0.f ||
            OutSnapshot.Magnitude >
                kMaxDamageFeedbackMagnitude)
        {
            OutSnapshot = {};
            return false;
        }

        OutSnapshot.Valid = Valid != 0;
        OutSnapshot.Critical = Critical != 0;
        return true;
    }

    bool WriteDamageBatchMagnitude(
        AFortPlayerPawnAthena* ShooterPawn,
        float Magnitude)
    {
        if (!GDamageFeedbackSchema.IsValid() ||
            !IsUsableActor(ShooterPawn) ||
            !std::isfinite(Magnitude) ||
            Magnitude < 0.f ||
            Magnitude > kMaxDamageFeedbackMagnitude ||
            !SDK::MemReadable(
                ShooterPawn,
                GDamageFeedbackSchema.PawnSize))
        {
            return false;
        }

        auto SharedMemory =
            reinterpret_cast<uint8*>(ShooterPawn) +
            GDamageFeedbackSchema.AccumulatedShared.Offset;
        auto MagnitudeMemory =
            SharedMemory +
            GDamageFeedbackSchema.SharedMagnitude.Offset;
        MEMORY_BASIC_INFORMATION MemoryInfo{};
        if (VirtualQuery(
                MagnitudeMemory,
                &MemoryInfo,
                sizeof(MemoryInfo)) != sizeof(MemoryInfo) ||
            MemoryInfo.State != MEM_COMMIT ||
            (MemoryInfo.Protect &
                (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        {
            return false;
        }

        const DWORD BaseProtection =
            MemoryInfo.Protect & 0xFF;
        const bool Writable =
            BaseProtection == PAGE_READWRITE ||
            BaseProtection == PAGE_WRITECOPY ||
            BaseProtection == PAGE_EXECUTE_READWRITE ||
            BaseProtection == PAGE_EXECUTE_WRITECOPY;
        const auto RegionEnd =
            reinterpret_cast<uintptr_t>(
                MemoryInfo.BaseAddress) +
            MemoryInfo.RegionSize;
        const auto WriteEnd =
            reinterpret_cast<uintptr_t>(
                MagnitudeMemory) +
            sizeof(float);
        if (!Writable ||
            WriteEnd < reinterpret_cast<uintptr_t>(
                MagnitudeMemory) ||
            WriteEnd > RegionEnd)
        {
            return false;
        }

        return WriteValue(
            SharedMemory,
            GDamageFeedbackSchema.SharedSize,
            GDamageFeedbackSchema.SharedMagnitude,
            Magnitude);
    }

    void CorrectDamageBatchMagnitude(
        const FDamageRequest& Request,
        bool ReadBefore,
        const FDamageBatchSnapshot& Before,
        float UnmodifiedDamage)
    {
        if (!Request.TargetPlayerPawn ||
            !GDamageFeedbackSchema.IsValid() ||
            !std::isfinite(UnmodifiedDamage) ||
            UnmodifiedDamage <= kDamageFeedbackEpsilon ||
            UnmodifiedDamage >
                kMaxDamageFeedbackMagnitude)
        {
            return;
        }

        FDamageBatchSnapshot After{};
        const bool ReadAfter =
            ReadDamageBatchSnapshot(
                Request.ShooterPawn,
                After);
        if (!ReadAfter ||
            !After.Valid ||
            After.HitActor != Request.TargetPlayerPawn)
        {
            static int32 MissingBatchTraceCount = 0;
            if (MissingBatchTraceCount++ < 4)
            {
                SDK::DbgLog(
                    "  [ProjectileDamage] display-batch unavailable target=%p raw=%.2f critical=%d read=%d valid=%d hit=%p magnitude=%.2f batch-critical=%d\n",
                    static_cast<void*>(
                        Request.TargetPlayerPawn),
                    UnmodifiedDamage,
                    Request.IsCriticalHit,
                    ReadAfter,
                    After.Valid,
                    static_cast<void*>(
                        After.HitActor),
                    After.Magnitude,
                    After.Critical);
            }
            return;
        }

        float BaseMagnitude = 0.f;
        if (ReadBefore &&
            Before.Valid &&
            Before.HitActor == Request.TargetPlayerPawn &&
            After.Magnitude + kDamageFeedbackEpsilon >=
                Before.Magnitude)
        {
            BaseMagnitude = Before.Magnitude;
        }

        const float NativeContribution =
            After.Magnitude - BaseMagnitude;
        if (!std::isfinite(NativeContribution) ||
            NativeContribution <= kDamageFeedbackEpsilon ||
            UnmodifiedDamage <=
                NativeContribution +
                    kDamageFeedbackEpsilon)
        {
            return;
        }

        const float CorrectedMagnitude =
            BaseMagnitude + UnmodifiedDamage;
        if (!std::isfinite(CorrectedMagnitude) ||
            CorrectedMagnitude >
                kMaxDamageFeedbackMagnitude)
        {
            return;
        }

        // Re-read immediately before the four-byte write. This preserves a
        // cue that was flushed or replaced reentrantly while the damage
        // execution was returning.
        FDamageBatchSnapshot Current{};
        if (!ReadDamageBatchSnapshot(
                Request.ShooterPawn,
                Current) ||
            !Current.Valid ||
            Current.HitActor != Request.TargetPlayerPawn ||
            std::fabs(
                Current.Magnitude -
                After.Magnitude) >
                    kDamageFeedbackEpsilon ||
            !WriteDamageBatchMagnitude(
                Request.ShooterPawn,
                CorrectedMagnitude))
        {
            return;
        }

        static int32 CorrectedBatchTraceCount = 0;
        if (CorrectedBatchTraceCount++ < 4)
        {
            SDK::DbgLog(
                "  [ProjectileDamage] display-batch target=%p before=%.2f native=%.2f raw=%.2f corrected=%.2f critical=%d\n",
                static_cast<void*>(
                    Request.TargetPlayerPawn),
                BaseMagnitude,
                NativeContribution,
                UnmodifiedDamage,
                CorrectedMagnitude,
                Request.IsCriticalHit);
        }
    }

    bool EnrichEffectContext(
        const FDamageRequest& Request,
        const void* HitMemory,
        uint32 HitSize,
        const uint8* Context,
        bool IsCriticalEffect)
    {
        alignas(16) std::array<
            uint8,
            kMaxEffectCallParamsSize> AddHitParams{};
        if (GEffectApiSchema.AddHitResultSize >
                AddHitParams.size())
        {
            return false;
        }

        const bool Reset = true;
        if (!WriteBytes(
                AddHitParams.data(),
                GEffectApiSchema.AddHitResultSize,
                GEffectApiSchema.AddHitContext,
                Context,
                GEffectApiSchema.ContextReturn.Size) ||
            !WriteBytes(
                AddHitParams.data(),
                GEffectApiSchema.AddHitResultSize,
                GEffectApiSchema.AddHitValue,
                HitMemory,
                HitSize) ||
            !WriteValue(
                AddHitParams.data(),
                GEffectApiSchema.AddHitResultSize,
                GEffectApiSchema.AddHitReset,
                Reset))
        {
            return false;
        }

        GEffectApiSchema.BlueprintLibrary->ProcessEvent(
            GEffectApiSchema.AddHitResult,
            AddHitParams.data());

        if (GEffectApiSchema.CanSetCritical())
        {
            alignas(16) std::array<uint8, 0x80>
                SetCriticalParams{};
            if (GEffectApiSchema.SetContextCriticalSize >
                    SetCriticalParams.size() ||
                !WriteBytes(
                    SetCriticalParams.data(),
                    GEffectApiSchema.SetContextCriticalSize,
                    GEffectApiSchema.SetCriticalContext,
                    Context,
                    GEffectApiSchema.ContextReturn.Size) ||
                !WriteValue(
                    SetCriticalParams.data(),
                    GEffectApiSchema.SetContextCriticalSize,
                    GEffectApiSchema.SetCriticalValue,
                    IsCriticalEffect))
            {
                return false;
            }

            GEffectApiSchema.FortBlueprintLibrary->ProcessEvent(
                GEffectApiSchema.SetContextCritical,
                SetCriticalParams.data());
        }

        alignas(16) std::array<uint8, 0x100>
            SetOriginParams{};
        if (GEffectApiSchema.SetContextOriginSize >
                SetOriginParams.size() ||
            !WriteBytes(
                SetOriginParams.data(),
                GEffectApiSchema.SetContextOriginSize,
                GEffectApiSchema.SetOriginContext,
                Context,
                GEffectApiSchema.ContextReturn.Size) ||
            !WriteBytes(
                SetOriginParams.data(),
                GEffectApiSchema.SetContextOriginSize,
                GEffectApiSchema.SetOriginValue,
                &Request.ProjectileOrigin,
                FVector::Size()))
        {
            return false;
        }

        GEffectApiSchema.BlueprintLibrary->ProcessEvent(
            GEffectApiSchema.SetContextOrigin,
            SetOriginParams.data());

        AActor* ContextInstigator = nullptr;
        UObject* ContextSource = nullptr;
        auto ShooterState =
            GetAthenaPlayerState(Request.ShooterController);
        const bool ReadInstigator =
            ReadContextActor(
                GEffectApiSchema.GetContextInstigator,
                GEffectApiSchema.GetContextInstigatorSize,
                GEffectApiSchema.GetInstigatorContext,
                GEffectApiSchema.GetInstigatorReturn,
                Context,
                ContextInstigator);
        const bool ExpectedInstigator =
            ContextInstigator == Request.ShooterPawn ||
                ContextInstigator ==
                    Request.ShooterController ||
                ContextInstigator == ShooterState;
        const bool ReadSource =
            ReadContextSource(Context, ContextSource);
        const bool ExpectedSource =
            ContextSource == Request.Weapon ||
            (Request.Weapon->HasWeaponData() &&
                ContextSource ==
                    Request.Weapon->WeaponData);
        if (!ReadInstigator ||
            !ExpectedInstigator ||
            !ReadSource ||
            !ExpectedSource)
        {
            // The context was constructed entirely from server-owned values.
            // Getter readback varies between Fortnite minor versions, so keep
            // it as a bounded compatibility diagnostic rather than rejecting
            // an otherwise valid effect context.
            LogProjectileDiagnostic(
                "trace",
                "context-readback",
                Request.Weapon,
                Request.Target);
        }
        return true;
    }

    bool ApplyFortGameplayEffects(
        const FDamageRequest& Request,
        const void* HitMemory,
        uint32 HitSize)
    {
        if (!IsUsableActor(Request.Weapon) ||
            !IsUsableActor(Request.ShooterPawn) ||
            !IsUsableActor(Request.ShooterController) ||
            !IsUsableActor(Request.Target) ||
            !IsUsableObject(Request.ShooterAbilitySystem) ||
            !IsUsableObject(Request.TargetAbilitySystem) ||
            !IsUsableObject(Request.FireAbility))
        {
            LogProjectileDiagnostic(
                "reject",
                "gas-request",
                Request.Weapon,
                Request.Target);
            return false;
        }

        bool Invoked = false;
        bool AnyAccepted = false;
        for (int32 Index = 0;
            Index < Request.Effects.Count;
            Index++)
        {
            auto EffectClass =
                Request.Effects.Classes[Index];
            if (!IsUsableObject(EffectClass) ||
                !EffectClass->IsA(UClass::StaticClass()) ||
                !IsUsableActor(Request.Target) ||
                !IsUsableObject(
                    Request.TargetAbilitySystem))
            {
                LogProjectileDiagnostic(
                    "reject",
                    "effect-class",
                    Request.Weapon,
                    Request.Target);
                break;
            }

            const bool NonDamageEffect =
                IsKnownNonDamageProjectileEffect(
                    EffectClass);
            const bool CriticalEffect =
                Request.IsCriticalHit &&
                !NonDamageEffect;

            // A Fort damage execution can mutate its extended context.
            // Build and enrich a fresh context for each gameplay effect,
            // matching the weapon ability's normal effect-spec path.
            const uint8* Context =
                MakeFortEffectContext(Request);
            if (!Context)
            {
                LogProjectileDiagnostic(
                    "reject",
                    "context-create",
                    Request.Weapon,
                    Request.Target);
                break;
            }
            if (!EnrichEffectContext(
                    Request,
                    HitMemory,
                    HitSize,
                    Context,
                    CriticalEffect))
            {
                LogProjectileDiagnostic(
                    "reject",
                    "context-enrich",
                    Request.Weapon,
                    Request.Target);
                break;
            }

            alignas(16) std::array<uint8, 0x100>
                Params{};
            UAbilitySystemComponent* Target =
                Request.TargetAbilitySystem;
            UObject* SourceObject = Request.Weapon;
            AActor* EffectCauser = Request.Weapon;
            AActor* DamageSource = Request.Weapon;
            const float Level =
                static_cast<float>(
                    Request.AbilityLevel);

            if (!WriteValue(
                    Params.data(),
                    GEffectApiSchema.ApplyFortEffectSize,
                    GEffectApiSchema.ApplyEffectClass,
                    EffectClass) ||
                !WriteValue(
                    Params.data(),
                    GEffectApiSchema.ApplyFortEffectSize,
                    GEffectApiSchema.ApplyTarget,
                    Target) ||
                !WriteValue(
                    Params.data(),
                    GEffectApiSchema.ApplyFortEffectSize,
                    GEffectApiSchema.ApplySourceObject,
                    SourceObject) ||
                !WriteValue(
                    Params.data(),
                    GEffectApiSchema.ApplyFortEffectSize,
                    GEffectApiSchema.ApplyEffectCauser,
                    EffectCauser) ||
                !WriteValue(
                    Params.data(),
                    GEffectApiSchema.ApplyFortEffectSize,
                    GEffectApiSchema.ApplyDamageSource,
                    DamageSource) ||
                !WriteValue(
                    Params.data(),
                    GEffectApiSchema.ApplyFortEffectSize,
                    GEffectApiSchema.ApplyLevel,
                    Level) ||
                !WriteBytes(
                    Params.data(),
                    GEffectApiSchema.ApplyFortEffectSize,
                    GEffectApiSchema.ApplyContext,
                    Context,
                    GEffectApiSchema.ContextReturn.Size))
            {
                LogProjectileDiagnostic(
                    "reject",
                    "effect-pack",
                    Request.Weapon,
                    Request.Target);
                break;
            }

            FActiveDamageFeedback ActiveFeedback{};
            FActiveDamageFeedback* FeedbackScope = nullptr;
            if (Request.TargetPlayerPawn &&
                !NonDamageEffect &&
                GDamageFeedbackSchema.IsValid())
            {
                ActiveFeedback.ShooterPawn =
                    Request.ShooterPawn;
                ActiveFeedback.TargetPawn =
                    Request.TargetPlayerPawn;
                ActiveFeedback.Context =
                    Params.data() +
                    GEffectApiSchema.ApplyContext.Offset;
                ActiveFeedback.ExpectedCritical =
                    CriticalEffect;
                ActiveFeedback.ReadBefore =
                    ReadDamageBatchSnapshot(
                        Request.ShooterPawn,
                        ActiveFeedback.Before);
                FeedbackScope = &ActiveFeedback;
            }

            {
                FScopedActiveDamageFeedback Scope(
                    FeedbackScope);
                Request.ShooterAbilitySystem->ProcessEvent(
                    GEffectApiSchema.ApplyFortEffect,
                    Params.data());
            }
            Invoked = true;

            FActiveGameplayEffectHandle Result{};
            const bool ReadResult =
                ReadValue(
                    Params.data(),
                    GEffectApiSchema.ApplyFortEffectSize,
                    GEffectApiSchema.ApplyReturn,
                    Result);
            const bool Accepted =
                ReadResult &&
                Result.bPassedFiltersAndWasExecuted;
            static int32 EffectTraceCount = 0;
            if (EffectTraceCount++ < 8)
            {
                SDK::DbgLog(
                    "  [ProjectileDamage] effect-result effect=%s read=%d handle=%d accepted=%d\n",
                    EffectClass->Name.ToString().c_str(),
                    ReadResult,
                    ReadResult ? Result.Handle : -1,
                    Accepted);
            }
            if (Accepted)
            {
                AnyAccepted = true;
                if (Request.TargetPlayerPawn)
                {
                    float UnmodifiedDamage = 0.f;
                    const auto AppliedContext =
                        Params.data() +
                        GEffectApiSchema
                            .ApplyContext.Offset;
                    const bool ReadDamage =
                        ReadUnmodifiedDamage(
                            AppliedContext,
                            UnmodifiedDamage);
                    static int32 DamageSourceTraceCount = 0;
                    if (DamageSourceTraceCount++ < 4)
                    {
                        SDK::DbgLog(
                            "  [ProjectileDamage] display-source effect=%s read=%d unmodified=%.2f critical-context=%d\n",
                            EffectClass->Name
                                .ToString().c_str(),
                            ReadDamage,
                            UnmodifiedDamage,
                            CriticalEffect);
                    }
                    if (ReadDamage &&
                        !NonDamageEffect &&
                        UnmodifiedDamage >
                            kDamageFeedbackEpsilon &&
                        !ActiveFeedback.CueHandled)
                    {
                        // Deferred, non-fatal cues remain in the pawn's
                        // accumulator. Correct them before the next effect can
                        // flush and clear that native batch.
                        CorrectDamageBatchMagnitude(
                            Request,
                            ActiveFeedback.ReadBefore,
                            ActiveFeedback.Before,
                            UnmodifiedDamage);
                    }
                }
            }
        }

        if (Invoked && !AnyAccepted)
        {
            static bool WarnedRejectedEffect = false;
            if (!WarnedRejectedEffect)
            {
                WarnedRejectedEffect = true;
                SDK::DbgLog(
                    "  [ProjectileDamage] Fortnite GAS rejected a projectile effect; no attribute fallback was applied\n");
            }
        }
        return AnyAccepted;
    }

    FEffectApiSchema ResolveEffectApiSchema()
    {
        FEffectApiSchema Result{};
        auto BlueprintLibraryClass =
            FindClass("AbilitySystemBlueprintLibrary");
        auto FortBlueprintLibraryClass =
            FindClass(
                "FortAbilitySystemBlueprintLibrary");
        auto FortAbilitySystemClass =
            FindClass("FortAbilitySystemComponent");
        Result.BlueprintLibrary =
            BlueprintLibraryClass
                ? BlueprintLibraryClass->GetDefaultObj()
                : nullptr;
        Result.FortBlueprintLibrary =
            FortBlueprintLibraryClass
                ? FortBlueprintLibraryClass
                    ->GetDefaultObj()
                : nullptr;
        auto FortAbilitySystemDefault =
            FortAbilitySystemClass
                ? FortAbilitySystemClass->GetDefaultObj()
                : nullptr;
        if (!Result.BlueprintLibrary ||
            !FortAbilitySystemDefault)
        {
            return {};
        }

        Result.GetAbilitySystem =
            Result.BlueprintLibrary->GetFunction(
                "GetAbilitySystemComponent");
        Result.AddHitResult =
            Result.BlueprintLibrary->GetFunction(
                "EffectContextAddHitResult");
        Result.SetContextOrigin =
            Result.BlueprintLibrary->GetFunction(
                "EffectContextSetOrigin");
        Result.SetContextCritical =
            Result.FortBlueprintLibrary
                ? Result.FortBlueprintLibrary->GetFunction(
                    "EffectContextSetCritical")
                : nullptr;
        Result.GetContextInstigator =
            Result.BlueprintLibrary->GetFunction(
                "EffectContextGetInstigatorActor");
        Result.GetContextSource =
            Result.BlueprintLibrary->GetFunction(
                "EffectContextGetSourceObject");
        Result.MakeFortContext =
            FortAbilitySystemDefault->GetFunction(
                "MakeFortEffectContext");
        Result.ApplyFortEffect =
            FortAbilitySystemDefault->GetFunction(
                "BP_FortApplyGameplayEffectToTarget");
        if (!Result.GetAbilitySystem ||
            !Result.AddHitResult ||
            !Result.SetContextOrigin ||
            !Result.GetContextInstigator ||
            !Result.GetContextSource ||
            !Result.MakeFortContext ||
            !Result.ApplyFortEffect)
        {
            return {};
        }

        auto Params =
            Result.GetAbilitySystem->GetParamsNamed();
        Result.GetAbilitySystemSize = Params.Size;
        Result.GetAbilitySystemActor =
            GetNamedParameter(Params, "Actor");
        Result.GetAbilitySystemReturn =
            GetNamedParameter(Params, "ReturnValue");

        Params = Result.MakeFortContext->GetParamsNamed();
        Result.MakeFortContextSize = Params.Size;
        Result.ContextSourceObject =
            GetNamedParameter(Params, "SourceObject");
        Result.ContextEffectCauser =
            GetNamedParameter(Params, "EffectCauser");
        Result.ContextDamageSource =
            GetNamedParameter(Params, "DamageSource");
        Result.ContextLevel =
            GetNamedParameter(
                Params,
                "GameplayEffectLevel");
        Result.ContextReturn =
            GetNamedParameter(Params, "ReturnValue");

        Params = Result.AddHitResult->GetParamsNamed();
        Result.AddHitResultSize = Params.Size;
        Result.AddHitContext =
            GetNamedParameter(Params, "EffectContext");
        Result.AddHitValue =
            GetNamedParameter(Params, "HitResult");
        Result.AddHitReset =
            GetNamedParameter(Params, "bReset");
        Result.HitResultSize = GHitResultSchema.Size;

        Params =
            Result.SetContextOrigin->GetParamsNamed();
        Result.SetContextOriginSize = Params.Size;
        Result.SetOriginContext =
            GetNamedParameter(Params, "EffectContext");
        Result.SetOriginValue =
            GetNamedParameter(Params, "Origin");

        if (Result.SetContextCritical)
        {
            Params =
                Result.SetContextCritical->GetParamsNamed();
            Result.SetContextCriticalSize = Params.Size;
            Result.SetCriticalContext =
                GetNamedParameter(Params, "EffectContext");
            Result.SetCriticalValue =
                GetNamedParameter(Params, "bInCritical");
        }

        Params =
            Result.GetContextInstigator->GetParamsNamed();
        Result.GetContextInstigatorSize = Params.Size;
        Result.GetInstigatorContext =
            GetNamedParameter(Params, "EffectContext");
        Result.GetInstigatorReturn =
            GetNamedParameter(Params, "ReturnValue");

        Params =
            Result.GetContextSource->GetParamsNamed();
        Result.GetContextSourceSize = Params.Size;
        Result.GetSourceContext =
            GetNamedParameter(Params, "EffectContext");
        Result.GetSourceReturn =
            GetNamedParameter(Params, "ReturnValue");

        Params =
            Result.ApplyFortEffect->GetParamsNamed();
        Result.ApplyFortEffectSize = Params.Size;
        Result.ApplyEffectClass =
            GetNamedParameter(
                Params,
                "GameplayEffectClass");
        Result.ApplyTarget =
            GetNamedParameter(Params, "TargetOfEffect");
        Result.ApplySourceObject =
            GetNamedParameter(
                Params,
                "OptionalSourceObject");
        Result.ApplyEffectCauser =
            GetNamedParameter(
                Params,
                "OptionalEffectCauser");
        Result.ApplyDamageSource =
            GetNamedParameter(
                Params,
                "OptionalDamageSource");
        Result.ApplyLevel =
            GetNamedParameter(Params, "Level");
        Result.ApplyContext =
            GetNamedParameter(Params, "EffectContext");
        Result.ApplyReturn =
            GetNamedParameter(Params, "ReturnValue");

        Result.EffectContainerStruct =
            FindStruct("FortGameplayEffectContainer");
        if (Result.EffectContainerStruct)
        {
            Result.EffectContainerSize =
                static_cast<uint32>(
                    Result.EffectContainerStruct
                        ->GetPropertiesSize());
            Result.ContainerApplicationTag =
                GetStructField(
                    Result.EffectContainerStruct,
                    "ApplicationTag");
            Result.ContainerTargetEffects =
                GetStructField(
                    Result.EffectContainerStruct,
                    "TargetGameplayEffectClasses");
        }
        return Result;
    }

    FDamageFeedbackSchema ResolveDamageFeedbackSchema()
    {
        FDamageFeedbackSchema Result{};
        auto FortKismetLibraryClass =
            FindClass("FortKismetLibrary");
        auto PawnClass =
            AFortPlayerPawnAthena::StaticClass();
        auto DefaultPawn =
            PawnClass
                ? PawnClass->GetDefaultObj()
                : nullptr;
        Result.SharedStruct =
            FindStruct(
                "AthenaBatchedDamageGameplayCues_Shared");
        Result.NonSharedStruct =
            FindStruct(
                "AthenaBatchedDamageGameplayCues_NonShared");
        Result.FortKismetLibrary =
            FortKismetLibraryClass
                ? FortKismetLibraryClass->GetDefaultObj()
                : nullptr;
        Result.GetUnmodifiedDamage =
            Result.FortKismetLibrary
                ? Result.FortKismetLibrary->GetFunction(
                    "GetUnmodifiedDamage")
                : nullptr;
        Result.BatchedCueFunction =
            DefaultPawn
                ? DefaultPawn->GetFunction(
                    "NetMulticast_Athena_BatchedDamageCues")
                : nullptr;
        Result.DisplayHitNotifyFunction =
            DefaultPawn
                ? DefaultPawn->GetFunction(
                    "OnDisplayHitNotify")
                : nullptr;
        if (!Result.FortKismetLibrary ||
            !Result.GetUnmodifiedDamage ||
            !PawnClass ||
            !Result.SharedStruct ||
            !Result.NonSharedStruct)
        {
            return {};
        }

        auto Params =
            Result.GetUnmodifiedDamage->GetParamsNamed();
        Result.GetUnmodifiedDamageSize = Params.Size;
        Result.GetUnmodifiedDamageContext =
            GetNamedParameter(Params, "Context");
        if (!Result.GetUnmodifiedDamageContext.IsValid(
                Result.GetUnmodifiedDamageSize))
        {
            Result.GetUnmodifiedDamageContext =
                GetNamedParameter(
                    Params,
                    "EffectContext");
        }
        Result.GetUnmodifiedDamageReturn =
            GetNamedParameter(Params, "ReturnValue");

        if (Result.BatchedCueFunction)
        {
            Params =
                Result.BatchedCueFunction
                    ->GetParamsNamed();
            Result.BatchedCueParamsSize = Params.Size;
            Result.BatchedCueShared =
                GetNamedParameter(Params, "SharedData");
            Result.BatchedCueNonShared =
                GetNamedParameter(
                    Params,
                    "NonSharedData");
        }

        if (Result.DisplayHitNotifyFunction)
        {
            Params =
                Result.DisplayHitNotifyFunction
                    ->GetParamsNamed();
            Result.DisplayHitNotifyParamsSize =
                Params.Size;
            Result.DisplayDamageDealt =
                GetNamedParameter(
                    Params,
                    "DamageDealt");
            Result.DisplayCriticalHit =
                GetNamedParameter(
                    Params,
                    "bCriticalHit");
            Result.DisplayHitActor =
                GetNamedParameter(
                    Params,
                    "HitActor");
        }

        const int32 PawnSize =
            PawnClass->GetPropertiesSize();
        if (PawnSize > 0)
        {
            Result.PawnSize =
                static_cast<uint32>(PawnSize);
        }
        Result.AccumulatedShared =
            GetStructField(
                PawnClass,
                "AccumulatedBatchData_Shared");
        Result.AccumulatedNonShared =
            GetStructField(
                PawnClass,
                "AccumulatedBatchData_NonShared");

        const int32 SharedSize =
            Result.SharedStruct->GetPropertiesSize();
        if (SharedSize > 0)
        {
            Result.SharedSize =
                static_cast<uint32>(SharedSize);
        }
        Result.SharedMagnitude =
            GetStructField(
                Result.SharedStruct,
                "Magnitude");
        Result.SharedCritical =
            GetStructField(
                Result.SharedStruct,
                "bIsCritical");
        Result.SharedValid =
            GetStructField(
                Result.SharedStruct,
                "bIsValid");

        const int32 NonSharedSize =
            Result.NonSharedStruct->GetPropertiesSize();
        if (NonSharedSize > 0)
        {
            Result.NonSharedSize =
                static_cast<uint32>(NonSharedSize);
        }
        Result.NonSharedHitActor =
            GetStructField(
                Result.NonSharedStruct,
                "HitActor");
        return Result;
    }

    FLineOfSightSchema ResolveLineOfSightSchema()
    {
        FLineOfSightSchema Result{};
        auto ControllerClass =
            AFortPlayerControllerAthena::StaticClass();
        auto ControllerDefault = ControllerClass
            ? ControllerClass->GetDefaultObj()
            : nullptr;
        Result.Function = ControllerDefault
            ? ControllerDefault->GetFunction(
                "LineOfSightTo")
            : nullptr;
        if (!Result.Function)
            return {};

        const auto Params =
            Result.Function->GetParamsNamed();
        Result.ParamsSize = Params.Size;
        Result.Other =
            GetNamedParameter(Params, "Other");
        Result.ViewPoint =
            GetNamedParameter(Params, "ViewPoint");
        Result.AlternateChecks =
            GetNamedParameter(
                Params,
                "bAlternateChecks");
        Result.ReturnValue =
            GetNamedParameter(Params, "ReturnValue");
        return Result;
    }

    FWorldLineTraceSchema
        ResolveWorldLineTraceSchema()
    {
        FWorldLineTraceSchema Result{};
        Result.Library =
            const_cast<UKismetSystemLibrary*>(
                UKismetSystemLibrary::GetDefaultObj());
        Result.Function =
            Result.Library
                ? Result.Library->GetFunction(
                    "LineTraceSingle")
                : nullptr;
        if (!Result.Function)
            return {};

        const auto Params =
            Result.Function->GetParamsNamed();
        Result.ParamsSize = Params.Size;
        Result.WorldContextObject =
            GetNamedParameter(
                Params,
                "WorldContextObject");
        Result.Start =
            GetNamedParameter(Params, "Start");
        Result.End =
            GetNamedParameter(Params, "End");
        Result.TraceChannel =
            GetNamedParameter(
                Params,
                "TraceChannel");
        Result.TraceComplex =
            GetNamedParameter(
                Params,
                "bTraceComplex");
        Result.ActorsToIgnore =
            GetNamedParameter(
                Params,
                "ActorsToIgnore");
        Result.OutHit =
            GetNamedParameter(Params, "OutHit");
        Result.IgnoreSelf =
            GetNamedParameter(
                Params,
                "bIgnoreSelf");
        Result.ReturnValue =
            GetNamedParameter(
                Params,
                "ReturnValue");
        return Result;
    }

    FActorBoundsSchema ResolveActorBoundsSchema()
    {
        FActorBoundsSchema Result{};
        auto ActorClass = AActor::StaticClass();
        auto ActorDefault = ActorClass
            ? ActorClass->GetDefaultObj()
            : nullptr;
        Result.Function = ActorDefault
            ? ActorDefault->GetFunction("GetActorBounds")
            : nullptr;
        if (!Result.Function)
            return {};

        const auto Params =
            Result.Function->GetParamsNamed();
        Result.ParamsSize = Params.Size;
        Result.OnlyCollidingComponents =
            GetNamedParameter(
                Params,
                "bOnlyCollidingComponents");
        Result.Origin =
            GetNamedParameter(Params, "Origin");
        Result.BoxExtent =
            GetNamedParameter(Params, "BoxExtent");
        Result.IncludeFromChildActors =
            GetNamedParameter(
                Params,
                "bIncludeFromChildActors");
        return Result;
    }

    struct FScopedNotifyGuard
    {
        FScopedNotifyGuard()
        {
            GInsideNotifyPawnHit = true;
        }

        ~FScopedNotifyGuard()
        {
            GInsideNotifyPawnHit = false;
        }
    };

    void TraceProjectileActorNotifyPawnHit(
        UObject* Context,
        FFrame& Stack)
    {
        auto Projectile =
            IsUsableObject(Context)
                ? Context->Cast<AActor>()
                : nullptr;
        AActor* Owner = nullptr;
        AActor* Instigator = nullptr;
        UObject* AssociatedItem = nullptr;
        FVector FireStart{};
        const bool HasFireStart =
            ReadReflectedObjectValue(
                Context,
                "FireStartLoc",
                FireStart,
                FVector::Size());
        if (IsUsableActor(Projectile))
        {
            if (Projectile->HasOwner() &&
                IsUsableActor(Projectile->Owner))
            {
                Owner = Projectile->Owner;
            }
            if (Projectile->HasInstigator() &&
                IsUsableActor(Projectile->Instigator))
            {
                Instigator = Projectile->Instigator;
            }
            ReadReflectedObjectValue(
                Projectile,
                "AssociatedItemDef",
                AssociatedItem);
            if (!IsUsableObject(AssociatedItem))
                AssociatedItem = nullptr;
        }

        static int32 TraceCount = 0;
        if (TraceCount++ < 4)
        {
            SDK::DbgLog(
                "  [ProjectileDamage] projectile-actor-hit-ingress projectile=%s class=%s owner=%s owner-class=%s instigator=%s item=%s authority=%d fire-start=%s(%.1f,%.1f,%.1f) code=%p locals=%p\n",
                Projectile
                    ? Projectile->Name.ToString().c_str()
                    : "invalid",
                Projectile && IsUsableObject(Projectile->Class)
                    ? Projectile->Class->Name.ToString().c_str()
                    : "invalid",
                Owner
                    ? Owner->Name.ToString().c_str()
                    : "none",
                Owner && IsUsableObject(Owner->Class)
                    ? Owner->Class->Name.ToString().c_str()
                    : "none",
                Instigator
                    ? Instigator->Name.ToString().c_str()
                    : "none",
                AssociatedItem
                    ? AssociatedItem->Name.ToString().c_str()
                    : "none",
                Projectile && Projectile->HasAuthority(),
                HasFireStart ? "" : "unavailable:",
                FireStart.X,
                FireStart.Y,
                FireStart.Z,
                Stack.Code,
                Stack.Locals);
        }

        // This hook is diagnostic-only until the live build proves which
        // projectile class/owner chain is used. Preserve the native behavior
        // and leave the RPC frame completely untouched.
        if (GProjectileActorNotifyOriginal)
            GProjectileActorNotifyOriginal(Context, Stack);
    }
}

void AFortWeaponRanged::NotifyServerAbilityActivated(
    UObject* AbilitySourceObject)
{
    // The adjusted muzzle-to-crosshair state is commonly written by the
    // projectile setter immediately after GAS activation. Keep the relay
    // armed here and let that setter, TickFlush, or contact-hit ingress send
    // it with current per-shot geometry.
    (void)AbilitySourceObject;
}

void AFortWeaponRanged::
    NotifyServerAbilityActivationFailed(
        UObject* AbilitySourceObject)
{
    if (VersionInfo.FortniteVersion < 28.00 ||
        VersionInfo.FortniteVersion >= 32.00 ||
        !IsUsableObject(AbilitySourceObject))
    {
        return;
    }

    auto Weapon =
        AbilitySourceObject->Cast<
            AFortWeaponRanged>();
    auto RelayState =
        IsUsableActor(Weapon)
            ? GetProjectileVisualRelayState(
                Weapon,
                false)
            : nullptr;
    if (!RelayState ||
        !RelayState->PendingActivation)
    {
        return;
    }

    RelayState->PendingActivation = false;
    RelayState->CaptureActive =
        RelayState->CaptureActiveBeforePending;
    RelayState->CaptureActiveBeforePending = false;
    RelayState->PendingStartedAt = -1.f;
    RelayState->PendingBaselineFireToken = -1.f;
    RelayState->HasLatestDamageState = false;
    RelayState->LatestDamageStateAt = -1.f;
}

void AFortWeaponRanged::
    NotifyServerAbilityActivationStarted(
        UObject* AbilitySourceObject)
{
    PrepareServerAbilityActivation(
        AbilitySourceObject);
}

void AFortWeaponRanged::TickProjectileRelays()
{
    TickTargetingReplication();
    TickServerProjectileRelays();
}

void AFortWeaponRanged::
    ServerLWProjectile_SetDamageStartAndDirection_(
        UObject* Context,
        FFrame& Stack)
{
    auto Weapon =
        IsUsableObject(Context)
            ? Context->Cast<AFortWeaponRanged>()
            : nullptr;
    bool Decoded = false;
    bool Recorded = false;
    FVector DamageStart{};
    FVector DamageDirection{};
    if (Weapon &&
        GServerProjectileStateSchema
            .CanDecodeSetState())
    {
        alignas(16) std::array<
            uint8,
            kMaxFrameSnapshotSize> DecodeStorage{};
        FFrame* DecodeStack = nullptr;
        Decoded =
            SnapshotFrameForDecode(
                Stack,
                DecodeStorage,
                DecodeStack) &&
            DecodeFrameReference(
                *DecodeStack,
                &DamageStart,
                FVector::Size()) &&
            DecodeFrameReference(
                *DecodeStack,
                &DamageDirection,
                FVector::Size());
        if (Decoded)
        {
            Recorded = RecordServerProjectileState(
                Weapon,
                DamageStart,
                DamageDirection);
            if (Recorded)
            {
                if (!GInsideProjectileVisualRelay)
                    MarkNativeProjectileIngress(Weapon);
            }
        }
    }

    static int32 TraceCount = 0;
    if (TraceCount++ < 4)
    {
        SDK::DbgLog(
            "  [ProjectileDamage] launch-ingress source=server-setter weapon=%s code=%p locals=%p decoded=%d recorded=%d\n",
            Weapon
                ? Weapon->Name.ToString().c_str()
                : "invalid",
            Stack.Code,
            Stack.Locals,
            Decoded,
            Recorded);
    }

    if (ServerLWProjectile_SetDamageStartAndDirection_OG)
    {
        ServerLWProjectile_SetDamageStartAndDirection_OG(
            Context,
            Stack);
    }

    if (Decoded && Recorded && Weapon)
    {
        CaptureAuthoritativeProjectileLaunch(
            Weapon);
    }
}

void AFortWeaponRanged::
    ServerLWProjectile_EndActiveAbility_(
        UObject* Context,
        FFrame& Stack)
{
    auto Weapon =
        IsUsableObject(Context)
            ? Context->Cast<AFortWeaponRanged>()
            : nullptr;
    if (Weapon)
    {
        EndProjectileVisualRelay(Weapon);
        StopProjectileStreams(Weapon);
    }

    if (ServerLWProjectile_EndActiveAbility_OG)
    {
        ServerLWProjectile_EndActiveAbility_OG(
            Context,
            Stack);
    }
}

void AFortWeaponRanged::ServerStopProjectileRequest_(
    UObject* Context,
    FFrame& Stack)
{
    auto Weapon =
        IsUsableObject(Context)
            ? Context->Cast<AFortWeaponRanged>()
            : nullptr;
    if (Weapon)
    {
        EndProjectileVisualRelay(Weapon);
        StopProjectileStreams(Weapon);
    }

    if (ServerStopProjectileRequest_OG)
        ServerStopProjectileRequest_OG(Context, Stack);
}

void AFortWeaponRanged::
    MulticastProjectileRequestUnreliable_(
        UObject* Context,
        FFrame& Stack)
{
    auto Weapon =
        IsUsableObject(Context)
            ? Context->Cast<AFortWeaponRanged>()
            : nullptr;
    bool Decoded = false;
    bool Recorded = false;
    const bool NativeIngress =
        !GInsideProjectileVisualRelay;
    if (GProjectileRequestSchema.IsValid() && Weapon)
    {
        alignas(16) std::array<uint8, 0x100>
            RequestCopy{};
        alignas(16) std::array<
            uint8,
            kMaxFrameSnapshotSize> DecodeStorage{};
        FFrame* DecodeStack = nullptr;
        Decoded =
            SnapshotFrameForDecode(
                Stack,
                DecodeStorage,
                DecodeStack) &&
            GProjectileRequestSchema.Request.Size <=
                RequestCopy.size() &&
            DecodeFrameReference(
                *DecodeStack,
                RequestCopy.data(),
                GProjectileRequestSchema.Request.Size);
        if (Decoded)
        {
            Recorded =
                !NativeIngress ||
                RecordProjectileLaunch(
                    Weapon,
                    RequestCopy.data(),
                    GProjectileRequestSchema.Request.Size,
                    true);
            if (Recorded)
            {
                if (NativeIngress)
                {
                    FVector Start{};
                    FVector Direction{};
                    float FireToken = -1.f;
                    const float ReceivedAt =
                        GetServerTimeSeconds();
                    auto RelayState =
                        GetProjectileVisualRelayState(
                            Weapon,
                            true);
                    if (RelayState &&
                        ReadValue(
                            RequestCopy.data(),
                            GProjectileRequestSchema
                                .RequestSize,
                            GProjectileRequestSchema
                                .StartPosition,
                            Start,
                            FVector::Size()) &&
                        ReadValue(
                            RequestCopy.data(),
                            GProjectileRequestSchema
                                .RequestSize,
                            GProjectileRequestSchema
                                .StartDirection,
                            Direction,
                            FVector::Size()) &&
                        ReadValue(
                            RequestCopy.data(),
                            GProjectileRequestSchema
                                .RequestSize,
                            GProjectileRequestSchema
                                .Timestamp,
                            FireToken) &&
                        IsFiniteVector(Start) &&
                        IsFiniteVector(Direction) &&
                        std::isfinite(FireToken) &&
                        std::isfinite(ReceivedAt))
                    {
                        float ServerFireToken = -1.f;
                        const char* FireTokenSource =
                            "none";
                        const bool HasCurrentServerToken =
                            ResolveCompatibilityFireToken(
                                Weapon,
                                ReceivedAt,
                                ServerFireToken,
                                FireTokenSource) &&
                            std::abs(
                                ReceivedAt -
                                ServerFireToken) <=
                                kNativeFireTokenBindWindowSeconds;
                        RelayState
                            ->LastNativeMulticastFireToken =
                                HasCurrentServerToken
                                    ? ServerFireToken
                                    : FireToken;
                        RelayState
                            ->LastNativeMulticastAt =
                                ReceivedAt;
                        RelayState->LatestDamageStart =
                            FVector(
                                Start.X,
                                Start.Y,
                                Start.Z);
                        RelayState
                            ->LatestAdjustedAimDirection =
                                Direction.GetSafeNormal();
                        RelayState->LatestDamageStateAt =
                            ReceivedAt;
                        RelayState->HasLatestDamageState =
                            !RelayState
                                ->LatestAdjustedAimDirection
                                .IsZero();
                        RelayState
                            ->LightweightProjectile =
                                true;
                        RelayState->CaptureActive = true;
                    }
                    MarkNativeProjectileIngress(Weapon);
                }
            }
        }
    }

    static int32 TraceCount = 0;
    if (TraceCount++ < 4)
    {
        SDK::DbgLog(
            "  [ProjectileDamage] launch-ingress source=multicast weapon=%s code=%p locals=%p decoded=%d recorded=%d\n",
            Weapon
                ? Weapon->Name.ToString().c_str()
                : "invalid",
            Stack.Code,
            Stack.Locals,
            Decoded,
            Recorded);
    }

    if (MulticastProjectileRequestUnreliable_OG)
    {
        MulticastProjectileRequestUnreliable_OG(
            Context,
            Stack);
    }

    if (NativeIngress && Recorded && Weapon)
    {
        CaptureAuthoritativeProjectileLaunch(
            Weapon);
    }
}

void AFortWeaponRanged::
    MulticastStopProjectileRequestUnreliable_(
        UObject* Context,
        FFrame& Stack)
{
    auto Weapon =
        IsUsableObject(Context)
            ? Context->Cast<AFortWeaponRanged>()
            : nullptr;
    if (Weapon)
    {
        auto RelayState =
            GetProjectileVisualRelayState(
                Weapon,
                false);
        if (RelayState)
        {
            RelayState->Active = false;
            RelayState->ActiveVisualFireToken = -1.f;
            RelayState->CaptureActive = false;
            RelayState->CaptureActiveBeforePending =
                false;
            RelayState->PendingActivation = false;
            RelayState->PendingStartedAt = -1.f;
            RelayState->PendingBaselineFireToken =
                -1.f;
            RelayState->HasLatestDamageState = false;
            RelayState->LatestDamageStateAt = -1.f;
        }
        StopProjectileStreams(Weapon);
    }

    if (MulticastStopProjectileRequestUnreliable_OG)
    {
        MulticastStopProjectileRequestUnreliable_OG(
            Context,
            Stack);
    }
}

void AFortWeaponRanged::ServerNotifyPawnHit_(
    UObject* Context,
    FFrame& Stack)
{
    auto CallOriginal = [&]()
    {
        if (ServerNotifyPawnHit_OG)
            ServerNotifyPawnHit_OG(Context, Stack);
    };

    if (GInsideNotifyPawnHit ||
        !GNotifyPawnHitSchema.IsValid() ||
        !GHitResultSchema.IsValid() ||
        !GEffectApiSchema.IsValid() ||
        !IsUsableObject(Context))
    {
        CallOriginal();
        return;
    }

    auto Weapon = Context->Cast<AFortWeaponRanged>();
    if (!Weapon)
    {
        CallOriginal();
        return;
    }

    alignas(16) std::array<uint8, kMaxHitResultSize>
        HitCopy{};
    if (GNotifyPawnHitSchema.Hit.Size >
        HitCopy.size())
    {
        // This is a malformed native-frame report in the version-gated
        // replacement path. Do not expose it to a replayable native fallback.
        return;
    }
    FVector ProjectileOrigin{};
    float ProjectileTimestamp = 0.f;
    const bool HasTimestampField =
        GNotifyPawnHitSchema.ProjectileTimestamp.IsValid(
            GNotifyPawnHitSchema.ParamsSize);
    alignas(16) std::array<
        uint8,
        kMaxFrameSnapshotSize> DecodeStorage{};
    FFrame* DecodeStack = nullptr;
    const bool Decoded =
        SnapshotFrameForDecode(
            Stack,
            DecodeStorage,
            DecodeStack) &&
        DecodeFrameReference(
            *DecodeStack,
            HitCopy.data(),
            GNotifyPawnHitSchema.Hit.Size) &&
        DecodeFrameReference(
            *DecodeStack,
            &ProjectileOrigin,
            FVector::Size());
    if (Decoded && HasTimestampField)
    {
        ProjectileTimestamp =
            (std::numeric_limits<float>::quiet_NaN)();
        DecodeStack->StepCompiledIn(
            &ProjectileTimestamp);
    }

    static int32 IngressTraceCount = 0;
    if (IngressTraceCount++ < 8)
    {
        SDK::DbgLog(
            "  [ProjectileDamage] hit-ingress weapon=%s code=%p locals=%p decoded=%d origin=(%.1f,%.1f,%.1f) timestamp=%.6f\n",
            Weapon->Name.ToString().c_str(),
            Stack.Code,
            Stack.Locals,
            Decoded,
            ProjectileOrigin.X,
            ProjectileOrigin.Y,
            ProjectileOrigin.Z,
            ProjectileTimestamp);
    }
    if (!Decoded ||
        (HasTimestampField &&
            !std::isfinite(ProjectileTimestamp)))
    {
        LogProjectileDiagnostic(
            "reject",
            "frame-decode",
            Weapon);
        return;
    }

    // A contact-range projectile can report its hit before the next network
    // TickFlush. Retry the already-armed, server-owned launch relay here so the
    // canonical ledger entry exists before reservation. This never trusts hit
    // geometry: the relay still requires an advanced LastFireTime, authority,
    // the current weapon, and the server's own launch origin/direction.
    CaptureAuthoritativeProjectileLaunch(Weapon);

    FScopedNotifyGuard Guard{};
    FDamageRequest Request{};
    bool SuppressOriginal = false;
    if (!BuildDamageRequest(
            Weapon,
            HitCopy.data(),
            GNotifyPawnHitSchema.Hit.Size,
            ProjectileOrigin,
            ProjectileTimestamp,
            HasTimestampField,
            Request,
            SuppressOriginal))
    {
        if (!SuppressOriginal)
            CallOriginal();
        return;
    }

    // Classification metadata must come from a target-owned server component.
    // Players trace their mesh; vehicles trace the exact component named by
    // the validated hit. A failed trace receives a clean body/hull context
    // instead of forwarding client-supplied bone or part metadata.
    int32 AuthoritativeHitSource = 0;
    bool AuthoritativeVehicleHit = false;
    if (Request.TargetPlayerPawn)
    {
        if (TraceAuthoritativePlayerHit(
                Request.TargetPlayerPawn,
                Request.ProjectileOrigin,
                Request.LaunchDirection,
                Request.ImpactPoint,
                HitCopy.data(),
                GNotifyPawnHitSchema.Hit.Size))
        {
            AuthoritativeHitSource = 1;
        }
        else if (BuildAuthoritativeClosestPhysicsHit(
                Request.TargetPlayerPawn,
                Request.ImpactPoint,
                HitCopy.data(),
                GNotifyPawnHitSchema.Hit.Size))
        {
            AuthoritativeHitSource = 2;
        }
        else
        {
            InitializeCanonicalPlayerHitIdentity(
                HitCopy.data(),
                GNotifyPawnHitSchema.Hit.Size,
                Request.TargetPlayerPawn);
        }
    }
    else if (Request.TargetVehicle)
    {
        AuthoritativeVehicleHit =
            TraceAuthoritativeOwnedComponentHit(
                Request.TargetVehicle,
                Request.TargetHitComponent,
                Request.ProjectileOrigin,
                Request.LaunchDirection,
                Request.ImpactPoint,
                HitCopy.data(),
                GNotifyPawnHitSchema.Hit.Size);
        if (!AuthoritativeVehicleHit)
        {
            // The actor hit is still valid and may take ordinary hull damage,
            // but a client-supplied bone/shape must never select a tire or
            // another replicated damageable part.
            ClearUntrustedHitClassification(
                HitCopy.data(),
                GNotifyPawnHitSchema.Hit.Size);
        }

        static int32 VehicleHitTraceCount = 0;
        if (VehicleHitTraceCount++ < 16)
        {
            SDK::DbgLog(
                "  [ProjectileDamage] vehicle-part-hit weapon=%s component=%s authoritative=%d\n",
                Weapon->Name.ToString().c_str(),
                IsUsableObject(Request.TargetHitComponent)
                    ? Request.TargetHitComponent->Name
                        .ToString().c_str()
                    : "none",
                AuthoritativeVehicleHit ? 1 : 0);
        }
    }

    // Rebuild effect-facing geometry from the validated server launch and
    // impact while retaining server-derived material/bone metadata.
    SanitizeUntrustedHitMetadata(
        HitCopy.data(),
        GNotifyPawnHitSchema.Hit.Size,
        Request.ProjectileOrigin,
        Request.ImpactPoint);

    bool CriticalHit = false;
    uint8 DamageZone = 0xFF;
    bool ClassifiedCritical = false;
    if (Request.TargetPlayerPawn &&
        AuthoritativeHitSource != 0)
    {
        ClassifiedCritical =
            ResolveCriticalDamageZone(
                Request.TargetPlayerPawn,
                HitCopy.data(),
                GNotifyPawnHitSchema.Hit.Size,
                CriticalHit,
                DamageZone);
    }
    Request.IsCriticalHit =
        ClassifiedCritical && CriticalHit;

    static int32 DamageZoneTraceCount = 0;
    if (Request.TargetPlayerPawn &&
        DamageZoneTraceCount++ < 8)
    {
        SDK::DbgLog(
            "  [ProjectileDamage] damage-zone weapon=%s authoritative-source=%d classified=%d zone=%u critical=%d\n",
            Weapon->Name.ToString().c_str(),
            AuthoritativeHitSource,
            ClassifiedCritical,
            DamageZone,
            Request.IsCriticalHit);
    }

    if (!ApplyFortGameplayEffects(
            Request,
            HitCopy.data(),
            GNotifyPawnHitSchema.Hit.Size))
    {
        // The native handler is the compatibility fallback. Commit the
        // validated shot before forwarding so a fixed sub-build cannot apply
        // native damage repeatedly from replays of the same report.
        ReleaseHitBudgetReservation(
            Request.HitBudgetReservation,
            true);
        ReleaseProjectileReservation(
            Request.LaunchReservation,
            true);
        if (!Stack.Code &&
            Stack.Locals &&
            SDK::MemReadable(
                Stack.Locals,
                GNotifyPawnHitSchema.ParamsSize))
        {
            std::memcpy(
                Stack.Locals +
                    GNotifyPawnHitSchema.Hit.Offset,
                HitCopy.data(),
                GNotifyPawnHitSchema.Hit.Size);
            WriteBytes(
                Stack.Locals,
                GNotifyPawnHitSchema.ParamsSize,
                GNotifyPawnHitSchema.ProjectileOrigin,
                &Request.ProjectileOrigin,
                FVector::Size());
        }
        CallOriginal();
        return;
    }

    ReleaseHitBudgetReservation(
        Request.HitBudgetReservation,
        true);
    ReleaseProjectileReservation(
        Request.LaunchReservation,
        true);

    static int32 SuccessTraceCount = 0;
    if (SuccessTraceCount++ < 8)
    {
        SDK::DbgLog(
            "  [ProjectileDamage] applied weapon=%s target=%s effects=%d\n",
            Weapon->Name.ToString().c_str(),
            Request.Target->Name.ToString().c_str(),
            Request.Effects.Count);
    }

    // This is a replacement path for the affected Chapter 5 builds. Calling
    // the old native handler as well would make a fixed sub-build process the
    // same hit twice. Invalid or unsupported native-frame reports fail closed;
    // only a validated GAS execution failure uses the committed fallback.
}

void AFortWeaponRanged::PostLoadHook()
{
    if (VersionInfo.FortniteVersion < 28.00 ||
        VersionInfo.FortniteVersion >= 32.00 ||
        ServerLWProjectile_SetDamageStartAndDirection_OG ||
        ServerLWProjectile_EndActiveAbility_OG ||
        ServerStopProjectileRequest_OG ||
        ServerNotifyPawnHit_OG ||
        MulticastProjectileRequestUnreliable_OG ||
        MulticastStopProjectileRequestUnreliable_OG)
    {
        return;
    }

    auto WeaponClass = StaticClass();
    auto DefaultWeapon = WeaponClass
        ? static_cast<AFortWeaponRanged*>(
            WeaponClass->GetDefaultObj())
        : nullptr;
    GServerProjectileStateSchema =
        ResolveServerProjectileStateSchema(
            DefaultWeapon);
    auto NotifyFunction = DefaultWeapon
        ? DefaultWeapon->GetFunction(
            "ServerNotifyPawnHit")
        : nullptr;
    auto ProjectileRequestFunction = DefaultWeapon
        ? DefaultWeapon->GetFunction(
            "MulticastProjectileRequestUnreliable")
        : nullptr;
    auto StopProjectileRequestFunction = DefaultWeapon
        ? DefaultWeapon->GetFunction(
            "MulticastStopProjectileRequestUnreliable")
        : nullptr;
    auto SetProjectileStateFunction =
        GServerProjectileStateSchema.SetStateFunction;
    if (!NotifyFunction ||
        !NotifyFunction->ExecFunction ||
        !ProjectileRequestFunction ||
        !ProjectileRequestFunction->ExecFunction ||
        !SetProjectileStateFunction ||
        !SetProjectileStateFunction->ExecFunction)
    {
        SDK::DbgLog(
            "  [ProjectileDamage] skipped: Chapter 5 projectile RPCs have no native implementation on %.2f\n",
            VersionInfo.FortniteVersion);
        return;
    }

    GHitResultSchema = ResolveHitResultSchema();
    GComponentLineTraceSchema =
        ResolveComponentLineTraceSchema();
    GClosestPhysicsPointSchema =
        ResolveClosestPhysicsPointSchema();
    GDamageZoneSchema = ResolveDamageZoneSchema();
    GNotifyPawnHitSchema =
        ResolveNotifySchema(NotifyFunction);
    GProjectileRequestSchema =
        ResolveProjectileRequestSchema(
            ProjectileRequestFunction);
    GLightweightProjectileVisualSchema =
        ResolveLightweightProjectileVisualSchema(
            DefaultWeapon);
    if (!GHitResultSchema.IsValid() ||
        !GNotifyPawnHitSchema.IsValid() ||
        !GProjectileRequestSchema.IsValid() ||
        !GServerProjectileStateSchema
            .CanDecodeSetState() ||
        GNotifyPawnHitSchema.Hit.Size !=
            GHitResultSchema.Size ||
        GNotifyPawnHitSchema.ProjectileOrigin.Size !=
            static_cast<uint32>(FVector::Size()) ||
        GNotifyPawnHitSchema.ProjectileTimestamp.Size !=
            sizeof(float))
    {
        SDK::DbgLog(
            "  [ProjectileDamage] skipped: projectile request/hit schemas are incompatible on %.2f\n",
            VersionInfo.FortniteVersion);
        return;
    }
    if (!GLightweightProjectileVisualSchema
            .CanIdentifyWeapon())
    {
        SDK::DbgLog(
            "  [ProjectileDamage] warning: lightweight projectile identification is unavailable; observer projectile relay is disabled\n");
        GLightweightProjectileVisualSchema = {};
    }
    if (!GDamageZoneSchema.IsValid() ||
        GDamageZoneSchema.HitResult.Size !=
            GHitResultSchema.Size)
    {
        GDamageZoneSchema = {};
    }

    GEffectApiSchema = ResolveEffectApiSchema();
    if (!GEffectApiSchema.IsValid())
    {
        SDK::DbgLog(
            "  [ProjectileDamage] skipped: Fortnite GAS projectile APIs are unavailable or incompatible on %.2f\n",
            VersionInfo.FortniteVersion);
        return;
    }
    if (!GDamageZoneSchema.IsValid() ||
        (!GComponentLineTraceSchema.IsValid() &&
            !GClosestPhysicsPointSchema.IsValid()) ||
        !GEffectApiSchema.CanSetCritical())
    {
        SDK::DbgLog(
            "  [ProjectileDamage] warning: authoritative damage-zone classification unavailable; critical projectile hits are disabled\n");
        GDamageZoneSchema = {};
    }

    GDamageFeedbackSchema =
        ResolveDamageFeedbackSchema();
    if (!GDamageFeedbackSchema.IsValid())
    {
        SDK::DbgLog(
            "  [ProjectileDamage] warning: native Chapter 5 damage-feedback schema is unavailable; full pre-clamp damage-number correction is disabled\n");
        GDamageFeedbackSchema = {};
    }
    else if ((GDamageFeedbackSchema
                .CanRewriteDisplayHitNotify() ||
            GDamageFeedbackSchema
                .CanRewriteBatchedCue()) &&
        Offsets::ProcessEventVft > 0 &&
        Offsets::ProcessEventVft < 0x1000)
    {
        if (!GPawnProcessEventOriginal)
        {
            Utils::Hook<AFortPlayerPawnAthena>(
                static_cast<uint32>(
                    Offsets::ProcessEventVft),
                PawnProcessEventDamageFeedback,
                GPawnProcessEventOriginal);
        }
    }
    else
    {
        SDK::DbgLog(
            "  [ProjectileDamage] warning: batched damage-cue interception is unavailable; fatal full-damage number correction is disabled\n");
    }

    GLineOfSightSchema = ResolveLineOfSightSchema();
    if (!GLineOfSightSchema.IsValid())
    {
        SDK::DbgLog(
            "  [ProjectileDamage] skipped: server line-of-sight validation is unavailable on %.2f\n",
            VersionInfo.FortniteVersion);
        return;
    }
    GWorldLineTraceSchema =
        ResolveWorldLineTraceSchema();
    if (!GWorldLineTraceSchema.IsValid() ||
        GWorldLineTraceSchema.OutHit.Size !=
            GHitResultSchema.Size)
    {
        SDK::DbgLog(
            "  [ProjectileDamage] warning: exact impact line trace is unavailable; using actor line-of-sight fallback\n");
        GWorldLineTraceSchema = {};
    }

    GActorBoundsSchema = ResolveActorBoundsSchema();
    if (!GActorBoundsSchema.IsValid())
    {
        SDK::DbgLog(
            "  [ProjectileDamage] skipped: server actor-bounds validation is unavailable on %.2f\n",
            VersionInfo.FortniteVersion);
        return;
    }

    auto ProjectileActorClass =
        FindClass("FortProjectileAthena");
    auto DefaultProjectile =
        ProjectileActorClass
            ? ProjectileActorClass->GetDefaultObj()
            : nullptr;
    GProjectileActorNotifyFunction =
        DefaultProjectile
            ? DefaultProjectile->GetFunction(
                "ServerNotifyPawnHit")
            : nullptr;
    if (GProjectileActorNotifyFunction &&
        GProjectileActorNotifyFunction->ExecFunction)
    {
        Utils::ExecHook(
            GProjectileActorNotifyFunction,
            TraceProjectileActorNotifyPawnHit,
            GProjectileActorNotifyOriginal);
    }

    Utils::ExecHook(
        SetProjectileStateFunction,
        ServerLWProjectile_SetDamageStartAndDirection_,
        ServerLWProjectile_SetDamageStartAndDirection_OG);
    Utils::ExecHook(
        ProjectileRequestFunction,
        MulticastProjectileRequestUnreliable_,
        MulticastProjectileRequestUnreliable_OG);
    if (GServerProjectileStateSchema.EndAbilityFunction &&
        GServerProjectileStateSchema.EndAbilityFunction
            ->ExecFunction)
    {
        Utils::ExecHook(
            GServerProjectileStateSchema.EndAbilityFunction,
            ServerLWProjectile_EndActiveAbility_,
            ServerLWProjectile_EndActiveAbility_OG);
    }
    if (GServerProjectileStateSchema.StopRequestFunction &&
        GServerProjectileStateSchema.StopRequestFunction
            ->ExecFunction)
    {
        Utils::ExecHook(
            GServerProjectileStateSchema.StopRequestFunction,
            ServerStopProjectileRequest_,
            ServerStopProjectileRequest_OG);
    }
    if (StopProjectileRequestFunction &&
        StopProjectileRequestFunction->ExecFunction)
    {
        Utils::ExecHook(
            StopProjectileRequestFunction,
            MulticastStopProjectileRequestUnreliable_,
            MulticastStopProjectileRequestUnreliable_OG);
    }
    Utils::ExecHook(
        NotifyFunction,
        ServerNotifyPawnHit_,
        ServerNotifyPawnHit_OG);

    SDK::DbgLog(
        "  [ProjectileDamage] hooked Chapter 5 projectile ledger + native Fort GAS (hit-params=0x%X hit=0x%X origin=0x%X timestamp=%s set-state=0x%X visual-relay=%s visual-end=%s damage-cue=%s end=%s server-stop=%s multicast-stop=%s projectile-actor=%s)\n",
        GNotifyPawnHitSchema.ParamsSize,
        GNotifyPawnHitSchema.Hit.Size,
        GNotifyPawnHitSchema.ProjectileOrigin.Size,
        GNotifyPawnHitSchema.ProjectileTimestamp.IsValid(
            GNotifyPawnHitSchema.ParamsSize)
            ? "yes"
            : "no",
        GServerProjectileStateSchema.SetStateParamsSize,
        GLightweightProjectileVisualSchema
                .CanIdentifyWeapon()
            ? "yes"
            : "no",
        GLightweightProjectileVisualSchema
                .CanEndStream()
            ? "yes"
            : "no",
        GPawnProcessEventOriginal
            ? "yes"
            : "no",
        ServerLWProjectile_EndActiveAbility_OG
            ? "yes"
            : "no",
        ServerStopProjectileRequest_OG
            ? "yes"
            : "no",
        MulticastStopProjectileRequestUnreliable_OG
            ? "yes"
            : "no",
        GProjectileActorNotifyOriginal
            ? "trace"
            : "unavailable");
}
