#include "pch.h"
#include "../Public/FortWeapon.h"
#include "../Public/FortGameStateAthena.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortPlayerPawnAthena.h"
#include "../Public/FortPlayerStateAthena.h"
#include "../Public/BattleRoyaleGamePhaseLogic.h"
#include "../../Erbium/Public/Configuration.h"
#include "../../Erbium/Public/GUI.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
    constexpr uint32 kMaxNotifyParamsSize = 0x1000;
    constexpr uint32 kMaxHitResultSize = 0x200;
    constexpr uint32 kMaxEffectCallParamsSize = 0x400;
    constexpr uint32 kMaxFrameSnapshotSize = 0x100;
    constexpr uint32 kExpectedEffectContextSize = 0x18;
    constexpr uint32 kExpectedEffectContainerSize = 0xB8;
    constexpr int32 kExpectedEffectContainerCount = 5;
    constexpr int32 kMaxEffectsPerContainer = 8;
    constexpr int32 kMaxDamageEffects =
        kExpectedEffectContainerCount *
        kMaxEffectsPerContainer;
    constexpr double kMaxRecordedOriginErrorCm = 300.0;
    constexpr double kMaxLaunchOriginDriftCm = 250.0;
    constexpr double kImpactBoundsToleranceCm = 300.0;
    constexpr double kMaxActorBoundsExtentCm = 100000.0;
    constexpr double kMaxReportedTravelCm = 500000.0;
    constexpr double kMinLaunchCorroborationDot = 0.95;
    constexpr double kMinProjectileAzimuthDot = 0.995;
    constexpr double kMinServerAimDot = 0.80;
    // A projectile starts at the muzzle while the adjusted shot direction is
    // derived from the player's view. At contact range that camera/muzzle
    // baseline can be larger than the entire reported flight, so a fixed
    // angular cone is geometrically invalid. These corridor limits preserve
    // the old long-range angles while providing a bounded near-field offset.
    constexpr double kCompatibilityParallaxAllowanceCm = 100.0;
    constexpr double kCompatibilityRearwardSlackCm = 50.0;
    constexpr double kCompatibilityContactRangeCm = 200.0;
    constexpr double kCompatibilityContactRearwardSlackCm = 125.0;
    constexpr double kAuthoritativeTraceBacktrackCm = 150.0;
    constexpr double kAuthoritativeTraceForwardCm = 75.0;
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

    FNotifyPawnHitSchema GNotifyPawnHitSchema{};
    FHitResultSchema GHitResultSchema{};
    FProjectileRequestSchema GProjectileRequestSchema{};
    FServerProjectileStateSchema
        GServerProjectileStateSchema{};
    FEffectApiSchema GEffectApiSchema{};
    FLineOfSightSchema GLineOfSightSchema{};
    FActorBoundsSchema GActorBoundsSchema{};
    FComponentLineTraceSchema GComponentLineTraceSchema{};
    FClosestPhysicsPointSchema GClosestPhysicsPointSchema{};
    FDamageZoneSchema GDamageZoneSchema{};

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
    size_t GProjectileLedgerCursor = 0;
    uint64 GProjectileGeneration = 1;
    UWorld* GProjectileWorldIdentity = nullptr;
    float GLastProjectileServerTime = -1.f;
    thread_local bool GInsideNotifyPawnHit = false;
    UFunction* GProjectileActorNotifyFunction = nullptr;
    void (*GProjectileActorNotifyOriginal)(
        UObject*,
        FFrame&) = nullptr;

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
            !IsUsableObject(Target->Mesh) ||
            !GComponentLineTraceSchema.IsValid() ||
            !Target->Mesh->IsA(
                GComponentLineTraceSchema.ComponentClass) ||
            HitSize != GHitResultSchema.Size ||
            !OutHitMemory ||
            !IsFiniteVector(LaunchOrigin) ||
            !IsFiniteVector(LaunchDirection) ||
            !IsFiniteVector(ImpactPoint))
        {
            return false;
        }

        FVector TraceDirection =
            LaunchDirection.GetSafeNormal();
        if (TraceDirection.IsZero())
        {
            TraceDirection =
                (ImpactPoint - LaunchOrigin)
                    .GetSafeNormal();
        }
        if (TraceDirection.IsZero())
            return false;

        // Trace only the server-owned target mesh, across a short segment
        // around the validated impact. This produces authoritative component,
        // bone and physical-material metadata without choosing a world channel.
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

        Target->Mesh->ProcessEvent(
            GComponentLineTraceSchema.Function,
            Params.data());

        bool HitTargetMesh = false;
        std::array<uint8, kMaxHitResultSize>
            ServerHit{};
        if (!ReadValue(
                Params.data(),
                GComponentLineTraceSchema.ParamsSize,
                GComponentLineTraceSchema.ReturnValue,
                HitTargetMesh) ||
            !HitTargetMesh ||
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
                    FConfiguration::bLateGame,
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
                FConfiguration::bLateGame,
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
            SDK::DbgLog(
                "  [ProjectileDamage] matched %s ability=%s handle=%d source=%p level=%d\n",
                UseImpactAbility ? "impact" : "primary",
                OutAbility->Name.ToString().c_str(),
                WantedHandle,
                Spec.HasSourceObject()
                    ? static_cast<void*>(Spec.SourceObject)
                    : nullptr,
                OutLevel);
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
        GLaunchBudgetCursor = 0;
        GIngressBudgetCursor = 0;
        GProjectileLedgerCursor = 0;
        GProjectileIngressCapabilityCursor = 0;
        GProjectileCompatibilityTokenStateCursor = 0;
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
        const FVector& LaunchDirection)
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
            NormalizedLaunch.Dot(AimDirection) >=
                kMinServerAimDot;
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
            kCompatibilityRearwardSlackCm)
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
            Forward < -RearwardSlackCm)
        {
            return false;
        }

        const double ForwardPath =
            (std::max)(0.0, Forward);
        const double SideLimit =
            std::hypot(
                kCompatibilityParallaxAllowanceCm,
                kProjectileAzimuthSlope * ForwardPath);
        const double UpwardLimit =
            std::hypot(
                kCompatibilityParallaxAllowanceCm,
                kProjectileUpwardSlope * ForwardPath);
        const double DownwardLimit =
            std::hypot(
                kCompatibilityParallaxAllowanceCm,
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
        const FVector& ImpactPoint)
    {
        if (!GActorBoundsSchema.IsValid() ||
            !IsUsableActor(Target) ||
            !IsFiniteVector(ImpactPoint))
        {
            return false;
        }

        alignas(16) std::array<uint8, 0x80> Params{};
        const bool OnlyCollidingComponents = true;
        const bool IncludeFromChildActors = false;
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

        return std::abs(ImpactPoint.X - Origin.X) <=
                BoxExtent.X + kImpactBoundsToleranceCm &&
            std::abs(ImpactPoint.Y - Origin.Y) <=
                BoxExtent.Y + kImpactBoundsToleranceCm &&
            std::abs(ImpactPoint.Z - Origin.Z) <=
                BoxExtent.Z + kImpactBoundsToleranceCm;
    }

    bool HasServerLineOfSight(
        AFortPlayerControllerAthena* Controller,
        AActor* Target,
        const FVector& ViewPoint)
    {
        if (!GLineOfSightSchema.IsValid() ||
            !IsUsableActor(Controller) ||
            !IsUsableActor(Target) ||
            !IsFiniteVector(ViewPoint))
        {
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
        bool TimestampFromClient)
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

        const bool Automatic = IsAutomaticWeapon(Weapon);
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
        Entry->RecordedAt = Now;
        Entry->StoppedAt = -1.f;
        Entry->Generation = GProjectileGeneration++;
        if (GProjectileGeneration == 0)
            GProjectileGeneration = 1;
        Entry->ReservedShotIndex = -1;
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
        if (!GProjectileRequestSchema.IsValid() ||
            !IsFiniteVector(DamageStart) ||
            !IsFiniteVector(DamageDirection))
        {
            return false;
        }

        const float ReceivedAt = GetServerTimeSeconds();
        alignas(16) std::array<uint8, 0x100>
            SyntheticRequest{};
        if (!std::isfinite(ReceivedAt) ||
            ReceivedAt < 0.f ||
            GProjectileRequestSchema.RequestSize >
                SyntheticRequest.size() ||
            !WriteBytes(
                SyntheticRequest.data(),
                GProjectileRequestSchema.RequestSize,
                GProjectileRequestSchema.StartPosition,
                &DamageStart,
                FVector::Size()) ||
            !WriteBytes(
                SyntheticRequest.data(),
                GProjectileRequestSchema.RequestSize,
                GProjectileRequestSchema.StartDirection,
                &DamageDirection,
                FVector::Size()) ||
            !WriteValue(
                SyntheticRequest.data(),
                GProjectileRequestSchema.RequestSize,
                GProjectileRequestSchema.Timestamp,
                ReceivedAt))
        {
            return false;
        }

        return RecordProjectileLaunch(
            Weapon,
            SyntheticRequest.data(),
            GProjectileRequestSchema.RequestSize,
            false);
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
        if (TraceCount++ < 64)
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
        bool& OutUsedServerDirection)
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
                CurrentAimCorrelation);
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
                    kCompatibilityContactRearwardSlackCm);
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
                        CachedDirectionCorrelation);
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
                            kCompatibilityContactRearwardSlackCm);
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
        if (ValidateReportedCompatibilityLaunch(
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
                UsedServerDirection))
        {
            LaunchStateSource =
                UsedServerDirection
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
            if (RejectTraceCount++ < 96)
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

        // A normal launch for this weapon always wins. Also preserve an
        // exhausted compatibility entry as a tombstone: the same server fire
        // token must never mint another generation.
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

            if (!Existing.CompatibilityFallback ||
                Existing.ServerFireToken == FireToken)
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

        OutLaunchOrigin = Start;
        OutLaunchDirection = Direction;
        OutReservation = Entry;

        static int32 TraceCount = 0;
        if (TraceCount++ < 64)
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

                CandidateOrigin = FVector(
                    ReportedProjectileOrigin.X,
                    ReportedProjectileOrigin.Y,
                    ReportedProjectileOrigin.Z);
                CandidateDirection =
                    CurrentAimDirection;
                OriginError = CurrentOriginError;
            }
            else if (OriginError >
                kMaxRecordedOriginErrorCm)
            {
                continue;
            }

            double DirectionCorrelation = 0.0;
            const FVector TravelDelta =
                ImpactPoint - CandidateOrigin;
            const FVector TravelDirection =
                TravelDelta.GetSafeNormal();
            if (TravelDirection.IsZero() ||
                !ValidateProjectileTravelDirection(
                    CandidateDirection,
                    TravelDirection,
                    DirectionCorrelation))
            {
                continue;
            }

            const double TravelDistance =
                TravelDelta.Magnitude();
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
                TimestampError * 10000.0;
            if (Score < BestScore)
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
        if (!IsImpactWithinActorBounds(
                Hit.Target,
                Hit.ImpactPoint))
        {
            LogProjectileDiagnostic(
                "reject",
                "target-bounds",
                Weapon,
                Hit.Target);
            return false;
        }

        auto TargetPlayerPawn =
            AsPlayerPawn(Hit.Target);
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
                Hit.Target,
                LaunchOrigin))
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

    bool EnrichEffectContext(
        const FDamageRequest& Request,
        const void* HitMemory,
        uint32 HitSize,
        const uint8* Context)
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
                    Request.IsCriticalHit))
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
                return AnyAccepted;
            }
            if (!EnrichEffectContext(
                    Request,
                    HitMemory,
                    HitSize,
                    Context))
            {
                LogProjectileDiagnostic(
                    "reject",
                    "context-enrich",
                    Request.Weapon,
                    Request.Target);
                return AnyAccepted;
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
                return AnyAccepted;
            }

            Request.ShooterAbilitySystem->ProcessEvent(
                GEffectApiSchema.ApplyFortEffect,
                Params.data());
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
            if (EffectTraceCount++ < 128)
            {
                SDK::DbgLog(
                    "  [ProjectileDamage] effect-result effect=%s read=%d handle=%d accepted=%d\n",
                    EffectClass->Name.ToString().c_str(),
                    ReadResult,
                    ReadResult ? Result.Handle : -1,
                    Accepted);
            }
            if (Accepted)
                AnyAccepted = true;
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
        if (TraceCount++ < 32)
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
                MarkNativeProjectileIngress(Weapon);
        }
    }

    static int32 TraceCount = 0;
    if (TraceCount++ < 12)
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
        StopProjectileStreams(Weapon);

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
        StopProjectileStreams(Weapon);

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
            Recorded = RecordProjectileLaunch(
                Weapon,
                RequestCopy.data(),
                GProjectileRequestSchema.Request.Size,
                true);
            if (Recorded)
                MarkNativeProjectileIngress(Weapon);
        }
    }

    static int32 TraceCount = 0;
    if (TraceCount++ < 12)
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
        StopProjectileStreams(Weapon);

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
    if (IngressTraceCount++ < 24)
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

    // Player classification metadata must come from the target's server-owned
    // mesh. If that trace is unavailable, build a clean body-hit context
    // instead of forwarding a client-supplied bone or component.
    int32 AuthoritativeHitSource = 0;
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
        DamageZoneTraceCount++ < 64)
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
    if (SuccessTraceCount++ < 24)
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

    GLineOfSightSchema = ResolveLineOfSightSchema();
    if (!GLineOfSightSchema.IsValid())
    {
        SDK::DbgLog(
            "  [ProjectileDamage] skipped: server line-of-sight validation is unavailable on %.2f\n",
            VersionInfo.FortniteVersion);
        return;
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
        "  [ProjectileDamage] hooked Chapter 5 projectile ledger + native Fort GAS (hit-params=0x%X hit=0x%X origin=0x%X timestamp=%s set-state=0x%X end=%s server-stop=%s multicast-stop=%s projectile-actor=%s)\n",
        GNotifyPawnHitSchema.ParamsSize,
        GNotifyPawnHitSchema.Hit.Size,
        GNotifyPawnHitSchema.ProjectileOrigin.Size,
        GNotifyPawnHitSchema.ProjectileTimestamp.IsValid(
            GNotifyPawnHitSchema.ParamsSize)
            ? "yes"
            : "no",
        GServerProjectileStateSchema.SetStateParamsSize,
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
