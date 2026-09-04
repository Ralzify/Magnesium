#include "pch.h"
#include "../Public/FortPhysicsPawn.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortWeapon.h"
#include "../Public/BattleRoyaleGamePhaseLogic.h"
#include "../Public/FortVehicleMods.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <iterator>
#include <string>
#include <vector>

struct FReplicatedPhysicsPawnState
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FReplicatedPhysicsPawnState);

    DEFINE_STRUCT_PROP(Rotation, FQuat);
    DEFINE_STRUCT_PROP(Translation, FVector);
    DEFINE_STRUCT_PROP(LinearVelocity, FVector);
    DEFINE_STRUCT_PROP(AngularVelocity, FVector);
};

struct FReplicatedAthenaVehiclePhysicsState
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FReplicatedAthenaVehiclePhysicsState);

    DEFINE_STRUCT_PROP(Rotation, FQuat);
    DEFINE_STRUCT_PROP(Translation, FVector);
    DEFINE_STRUCT_PROP(LinearVelocity, FVector);
    DEFINE_STRUCT_PROP(AngularVelocity, FVector);
};

namespace
{
    bool IsUsablePhysicsObject(const UObject* Object)
    {
        if (!Object || IsBadReadPtr((void*)Object) ||
            Object->Index < 0 || Object->Index >= TUObjectArray::Num())
        {
            return false;
        }

        auto* Item = TUObjectArray::GetItemByIndex(Object->Index);
        constexpr int32 InvalidObjectFlags = 0x20;
        return Item && Item->GetObject() == Object && !(Item->GetFlags() & InvalidObjectFlags) &&
            Object->Class && !IsBadReadPtr(Object->Class);
    }

    constexpr double BumpKmhToCentimetersPerSecond = 27.7777778;
    constexpr double BumpDegreesToRadians = 0.017453292519943295;

    constexpr double BumpDefaultMinDirection = -0.35;
    constexpr double BumpDefaultAngleAdjustment = 0.0;

    const UClass* MatchOctopusVehicle()
    {
        return AFortOctopusVehicle::StaticClass();
    }

    struct FBumpLaunchProfile
    {
        const UClass* (*MatchClass)();
        const char* ClassNameFragment;
        double MinFortniteVersion;
        double ForwardScale;
        double VerticalScale;
        double MaxSpeed;
    };

    constexpr FBumpLaunchProfile BumpLaunchProfiles[] =
    {
        { MatchOctopusVehicle, "octopus", 27.00, 2.56, 3.83, 9200.0 },
        { nullptr, "baller", 27.00, 2.56, 3.83, 9200.0 },

        { nullptr, "saucer", 0.0, 0.45, 4.20, 9000.0 },
        { nullptr, "ufo", 0.0, 0.45, 4.20, 9000.0 },

        { nullptr, nullptr, 0.0, 0.25, 0.65, 3200.0 },
    };

    const FBumpLaunchProfile& ResolveLaunchProfile(AActor* Vehicle)
    {
        std::string ClassName;

        if (Vehicle->Class)
        {
            ClassName = Vehicle->Class->Name.ToString().c_str();

            for (auto& Character : ClassName)
                Character = (char)std::tolower((unsigned char)Character);
        }

        for (const auto& Profile : BumpLaunchProfiles)
        {
            if (Profile.MinFortniteVersion > 0.0 &&
                VersionInfo.FortniteVersion < Profile.MinFortniteVersion)
            {
                continue;
            }

            if (Profile.MatchClass)
            {
                if (const UClass* Match = Profile.MatchClass())
                {
                    if (Vehicle->IsA(Match))
                        return Profile;
                }
            }

            if (Profile.ClassNameFragment)
            {
                if (!ClassName.empty() &&
                    ClassName.find(Profile.ClassNameFragment) != std::string::npos)
                {
                    return Profile;
                }

                continue;
            }

            if (!Profile.MatchClass)
                return Profile;
        }

        return BumpLaunchProfiles[std::size(BumpLaunchProfiles) - 1];
    }

    constexpr double BumpDefaultMinSpeedToDamage = 700.0;
    constexpr double BumpDefaultMaxSpeedToDamage = 2800.0;
    constexpr double BumpDefaultMinSpeedDamage = 15.0;
    constexpr double BumpDefaultMaxSpeedDamage = 90.0;

    constexpr double BumpHullFront = 320.0;
    constexpr double BumpHullRear = 130.0;
    constexpr double BumpHullSide = 165.0;
    constexpr double BumpHullVertical = 170.0;

    constexpr ULONGLONG BumpCooldownMs = 750;

    constexpr ULONGLONG BumpPawnCacheLifetimeMs = 500;

    constexpr double BumpMinDirectionTestDistance = 60.0;

    constexpr ULONGLONG BumpVehicleScanLifetimeMs = 2000;

    constexpr ULONGLONG BumpDriverMoveGraceMs = 400;

    constexpr ULONGLONG BumpMinSampleIntervalMs = 60;

    constexpr ULONGLONG BumpMaxSampleIntervalMs = 500;

    constexpr double BumpMaxMeasuredSpeed = 12000.0;

    FVector CopyVector(const FVector& Value)
    {
        return FVector(Value.X, Value.Y, Value.Z);
    }

    struct FBumpPawnCooldown
    {
        TWeakObjectPtr<AFortPlayerPawnAthena> Pawn;
        ULONGLONG NextLaunchTimeMs = 0;
    };

    struct FBumpVehicleClass
    {
        const UClass* Class = nullptr;
        bool IsVehicle = false;
        const FBumpLaunchProfile* Profile = nullptr;
    };

    struct FBumpVehicleState
    {
        TWeakObjectPtr<AActor> Vehicle;
        FVector LastLocation{};
        ULONGLONG LastSampleTimeMs = 0;
        ULONGLONG LastDriverMoveTimeMs = 0;
        bool HasSample = false;
    };

    struct FBumpTuning
    {
        double ForwardScale = 0.0;
        double VerticalScale = 0.0;
        double MaxSpeed = 0.0;
        double MinDirection = BumpDefaultMinDirection;
        double AngleAdjustmentDegrees = BumpDefaultAngleAdjustment;

        double MinSpeedToDamage = BumpDefaultMinSpeedToDamage;
        double MaxSpeedToDamage = BumpDefaultMaxSpeedToDamage;
        double MinSpeedDamage = BumpDefaultMinSpeedDamage;
        double MaxSpeedDamage = BumpDefaultMaxSpeedDamage;
    };

    std::vector<TWeakObjectPtr<AFortPlayerPawnAthena>> GBumpPawnCache;
    std::vector<FBumpPawnCooldown> GBumpCooldowns;
    std::vector<FBumpVehicleClass> GBumpVehicleClasses;
    std::vector<FBumpVehicleState> GBumpVehicleStates;
    ULONGLONG GBumpPawnCacheExpiryMs = 0;
    ULONGLONG GBumpVehicleScanExpiryMs = 0;
    uint32 GBumpLogCount = 0;
    bool GBumpScanInProgress = false;

    FBumpVehicleClass ClassifyVehicle(AActor* Actor)
    {
        auto ActorClass = Actor->Class;

        if (!ActorClass)
            return {};

        for (const auto& Entry : GBumpVehicleClasses)
        {
            if (Entry.Class == ActorClass)
                return Entry;
        }

        bool IsVehicle = false;

        static const UClass* AthenaVehicleClass = AFortAthenaVehicle::StaticClass();
        static const UClass* OctopusVehicleClass = AFortOctopusVehicle::StaticClass();
        static const UClass* SeatComponentClass = UFortVehicleSeatComponent::StaticClass();

        if (AthenaVehicleClass && Actor->IsA(AthenaVehicleClass))
            IsVehicle = true;
        else if (OctopusVehicleClass && Actor->IsA(OctopusVehicleClass))
            IsVehicle = true;
        else if (SeatComponentClass && Actor->GetComponentByClass(SeatComponentClass))
            IsVehicle = true;

        GBumpVehicleClasses.push_back(
            { ActorClass, IsVehicle, &ResolveLaunchProfile(Actor) });
        return GBumpVehicleClasses.back();
    }

    FBumpTuning ResolveBumpTuning(AFortAthenaVehicle* Vehicle, const FBumpLaunchProfile& Profile)
    {
        FBumpTuning Tuning{};

        Tuning.ForwardScale = Profile.ForwardScale;
        Tuning.VerticalScale = Profile.VerticalScale;
        Tuning.MaxSpeed = Profile.MaxSpeed;

        if (!Vehicle)
            return Tuning;

        if (Vehicle->HasPawnLaunchForwardVelocityScale())
        {
            const double Value = Vehicle->PawnLaunchForwardVelocityScale;

            if (std::isfinite(Value) && Value > 0.0)
                Tuning.ForwardScale = Value;
        }

        if (Vehicle->HasPawnLaunchVerticalVelocityScale())
        {
            const double Value = Vehicle->PawnLaunchVerticalVelocityScale;

            if (std::isfinite(Value) && Value >= 0.0)
                Tuning.VerticalScale = Value;
        }

        if (Vehicle->HasPawnLaunchMaxSpeed())
        {
            const double Value = Vehicle->PawnLaunchMaxSpeed;

            if (std::isfinite(Value) && Value > 0.0)
                Tuning.MaxSpeed = Value;
        }

        if (Vehicle->HasPawnLaunchMinDirection())
        {
            const double Value = Vehicle->PawnLaunchMinDirection;

            if (std::isfinite(Value) && Value >= -1.0 && Value <= 1.0)
                Tuning.MinDirection = Value;
        }

        if (Vehicle->HasPawnLaunchAngleAdjustment())
        {
            const double Value = Vehicle->PawnLaunchAngleAdjustment;

            if (std::isfinite(Value))
                Tuning.AngleAdjustmentDegrees = std::clamp(Value, -89.0, 89.0);
        }

        if (Vehicle->HasVehicleMinHorSpeedToDamage() && Vehicle->HasVehicleMaxHorSpeedToDamage() &&
            Vehicle->HasVehicleMinHorSpeedDamage() && Vehicle->HasVehicleMaxHorSpeedDamage())
        {
            const double MinSpeed = Vehicle->VehicleMinHorSpeedToDamage;
            const double MaxSpeed = Vehicle->VehicleMaxHorSpeedToDamage;
            const double MinDamage = Vehicle->VehicleMinHorSpeedDamage;
            const double MaxDamage = Vehicle->VehicleMaxHorSpeedDamage;

            if (std::isfinite(MinSpeed) && std::isfinite(MaxSpeed) &&
                std::isfinite(MinDamage) && std::isfinite(MaxDamage) &&
                MinSpeed >= 0.0 && MaxSpeed > MinSpeed &&
                MinDamage >= 0.0 && MaxDamage >= MinDamage && MaxDamage > 0.0)
            {
                Tuning.MinSpeedToDamage = MinSpeed;
                Tuning.MaxSpeedToDamage = MaxSpeed;
                Tuning.MinSpeedDamage = MinDamage;
                Tuning.MaxSpeedDamage = MaxDamage;
            }
        }

        return Tuning;
    }

    double ResolveBumpDamage(double Speed, const FBumpTuning& Tuning)
    {
        if (Speed <= Tuning.MinSpeedToDamage)
            return Tuning.MinSpeedDamage;

        if (Speed >= Tuning.MaxSpeedToDamage)
            return Tuning.MaxSpeedDamage;

        const double Range = Tuning.MaxSpeedToDamage - Tuning.MinSpeedToDamage;

        if (Range <= 0.0)
            return Tuning.MaxSpeedDamage;

        const double Alpha = (Speed - Tuning.MinSpeedToDamage) / Range;
        return Tuning.MinSpeedDamage + (Tuning.MaxSpeedDamage - Tuning.MinSpeedDamage) * Alpha;
    }

    FVector BuildBumpLaunchVelocity(const FVector& TravelDirection, double Speed,
        const FBumpTuning& Tuning)
    {
        double Horizontal = Speed * Tuning.ForwardScale;
        double Vertical = Speed * Tuning.VerticalScale;

        if (Tuning.AngleAdjustmentDegrees != 0.0)
        {
            const double Magnitude = std::sqrt((Horizontal * Horizontal) + (Vertical * Vertical));
            const double Angle = std::atan2(Vertical, Horizontal) +
                (Tuning.AngleAdjustmentDegrees * BumpDegreesToRadians);

            Horizontal = Magnitude * std::cos(Angle);
            Vertical = Magnitude * std::sin(Angle);
        }

        const double Magnitude = std::sqrt((Horizontal * Horizontal) + (Vertical * Vertical));

        if (Tuning.MaxSpeed > 0.0 && Magnitude > Tuning.MaxSpeed)
        {
            const double Scale = Tuning.MaxSpeed / Magnitude;

            Horizontal *= Scale;
            Vertical *= Scale;
        }

        double Multiplier = FConfiguration::VehicleBumpForceMultiplier;

        if (!std::isfinite(Multiplier) || Multiplier < 0.f)
            Multiplier = 1.f;

        return FVector(TravelDirection.X * Horizontal * Multiplier,
            TravelDirection.Y * Horizontal * Multiplier, Vertical * Multiplier);
    }

    bool IsBumpOnCooldown(AFortPlayerPawnAthena* Pawn, ULONGLONG CurrentTimeMs)
    {
        for (auto Entry = GBumpCooldowns.begin(); Entry != GBumpCooldowns.end();)
        {
            auto* Tracked = Entry->Pawn.Get();

            if (!Tracked || CurrentTimeMs >= Entry->NextLaunchTimeMs)
            {
                if (Tracked == Pawn)
                {
                    Entry->NextLaunchTimeMs = CurrentTimeMs + BumpCooldownMs;
                    return false;
                }

                Entry = GBumpCooldowns.erase(Entry);
                continue;
            }

            if (Tracked == Pawn)
                return true;

            ++Entry;
        }

        GBumpCooldowns.push_back({ Pawn, CurrentTimeMs + BumpCooldownMs });
        return false;
    }

    const std::vector<TWeakObjectPtr<AFortPlayerPawnAthena>>& GetBumpPawnCache(
        ULONGLONG CurrentTimeMs)
    {
        if (CurrentTimeMs < GBumpPawnCacheExpiryMs)
            return GBumpPawnCache;

        GBumpPawnCacheExpiryMs = CurrentTimeMs + BumpPawnCacheLifetimeMs;
        GBumpPawnCache.clear();

        if (!AFortPlayerPawnAthena::StaticClass())
            return GBumpPawnCache;

        TArray<AFortPlayerPawnAthena*> Pawns;
        Utils::GetAll<AFortPlayerPawnAthena>(Pawns);

        for (auto Pawn : Pawns)
        {
            if (Pawn)
                GBumpPawnCache.push_back(Pawn);
        }

        Pawns.Free();
        return GBumpPawnCache;
    }

    AActor* GetRiddenVehicle(AFortPlayerPawnAthena* Pawn)
    {
        if (!IsUsablePhysicsObject(Pawn))
            return nullptr;

        UFunction* GetVehicleFunction = Pawn->GetFunction("GetVehicleActor");

        if (!GetVehicleFunction)
            GetVehicleFunction = Pawn->GetFunction("GetVehicle");

        if (!GetVehicleFunction)
            GetVehicleFunction = Pawn->GetFunction("BP_GetVehicle");

        auto* Vehicle = GetVehicleFunction ? Pawn->Call<AActor*>(GetVehicleFunction) : nullptr;
        return IsUsablePhysicsObject(Vehicle) ? Vehicle : nullptr;
    }

    bool IsRidingAVehicle(AFortPlayerPawnAthena* Pawn, AActor* Vehicle)
    {
        if (GetRiddenVehicle(Pawn))
            return true;

        UFunction* FindSeatIndexFunction = Vehicle->GetFunction("FindSeatIndex");

        if (FindSeatIndexFunction && FindSeatIndexFunction->GetParamsNamed().Size == 0x10)
        {
            const int32 SeatIndex = Vehicle->Call<int32>(FindSeatIndexFunction, Pawn);

            if (SeatIndex >= 0)
                return true;
        }

        return false;
    }

    bool CanBumpPawn(AFortPlayerPawnAthena* Pawn, AActor* Vehicle)
    {
        if (IsRidingAVehicle(Pawn, Vehicle))
            return false;

        if (Pawn->IsDBNO())
            return false;

        if (Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving)
            return false;

        if (Pawn->GetHealth() <= 0.f)
            return false;

        return true;
    }

    struct FBumpDriver
    {
        AFortPlayerPawnAthena* Pawn = nullptr;
        bool Resolved = false;
    };

    FBumpDriver ResolveVehicleDriver(AActor* Vehicle)
    {
        FBumpDriver Result{};

        if (auto* GetDriverFunction = Vehicle->GetFunction("GetDriver"))
        {
            Result.Resolved = true;

            if (auto* Driver = Vehicle->Call<AActor*>(GetDriverFunction))
            {
                Result.Pawn = Driver->Cast<AFortPlayerPawnAthena>();

                if (Result.Pawn)
                    return Result;

                Result.Resolved = false;
            }
        }

        if (auto* GetPawnAtSeatFunction = Vehicle->GetFunction("GetPawnAtSeat"))
        {
            Result.Resolved = true;

            if (auto* Driver = Vehicle->Call<AActor*>(GetPawnAtSeatFunction, (int32)0))
                Result.Pawn = Driver->Cast<AFortPlayerPawnAthena>();
        }

        return Result;
    }

    bool TryGetTeam(AFortPlayerPawnAthena* Pawn, uint8& OutTeam)
    {
        if (!Pawn)
            return false;

        AActor* State = Pawn->HasPlayerState() ? Pawn->PlayerState : nullptr;

        if (!State && Pawn->Controller)
        {
            if (auto* Controller = Pawn->Controller->Cast<AFortPlayerControllerAthena>())
                State = (AActor*)Controller->PlayerState;
        }

        auto* StateAthena = State ? State->Cast<AFortPlayerStateAthena>() : nullptr;

        if (!StateAthena || !StateAthena->HasTeamIndex())
            return false;

        OutTeam = StateAthena->TeamIndex;
        return true;
    }

    bool IsEveryPlayerTheirOwnTeam()
    {
        auto* World = UWorld::GetWorld();
        auto* GameState = World ? (AFortGameStateAthena*)World->GameState : nullptr;

        const auto Playlist = AFortGameMode::GetActivePlaylist(GameState);
        if (!Playlist || !Playlist->HasMaxSquadSize())
        {
            return false;
        }

        return Playlist->MaxSquadSize <= 1;
    }

    bool ShouldDamageBumpedPawn(AFortPlayerPawnAthena* Victim, const FBumpDriver& Driver)
    {
        if (!Driver.Resolved)
            return IsEveryPlayerTheirOwnTeam();

        if (!Driver.Pawn)
            return true;

        if (Driver.Pawn == Victim)
            return false;

        uint8 DriverTeam = 0;
        uint8 VictimTeam = 0;

        if (TryGetTeam(Driver.Pawn, DriverTeam) && TryGetTeam(Victim, VictimTeam))
            return DriverTeam != VictimTeam;

        return IsEveryPlayerTheirOwnTeam();
    }

    void ApplyBumpDamage(AFortPlayerPawnAthena* Pawn, AActor* Vehicle,
        AFortPlayerPawnAthena* Driver, double Damage)
    {
        if (!IsUsablePhysicsObject(Pawn) || !std::isfinite(Damage) || Damage <= 0.0)
            return;

        const auto Health = Pawn->GetHealth();
        const auto Shield = Pawn->GetShield();
        if (!std::isfinite(Health) || Health <= 0.f || !std::isfinite(Shield) || Shield < 0.f)
        {
            SDK::DbgLog(
                "[VehicleBump] damage skipped: invalid health state pawn=%p health=%.3f shield=%.3f\n",
                (void*)Pawn, Health, Shield);
            return;
        }

        if (Pawn->HasbCanBeDamaged() && !Pawn->bCanBeDamaged)
            return;

        if (AFortPlayerPawnAthena::HasFullHealthGodMode(Pawn))
            return;

        auto* DriverController = Driver && Driver->Controller
            ? Driver->Controller->Cast<AFortPlayerControllerAthena>() : nullptr;

        auto Remaining = (float)Damage;

        if (Shield > 0.f)
        {
            const auto ShieldDamage = Shield < Remaining ? Shield : Remaining;
            Pawn->SetShield(Shield - ShieldDamage);
            Remaining -= ShieldDamage;
        }

        if (Remaining > 0.f)
        {
            if (Health <= Remaining)
            {
                if (AFortPlayerPawnAthena::HasMinimumHealthGodMode(Pawn))
                {
                    Pawn->SetHealth(1.f);
                }
                else if (auto* ForceKillFunction = Pawn->GetFunction("ForceKill"))
                {
                    FGameplayTag DeathTag{};
                    Pawn->Call<void>(ForceKillFunction, DeathTag, DriverController, Vehicle);
                }
                else
                {
                    SDK::DbgLog(
                        "[VehicleBump] fatal damage skipped: ForceKill unavailable pawn=%p vehicle=%p\n",
                        (void*)Pawn, (void*)Vehicle);
                }
            }
            else
                Pawn->SetHealth(Health - Remaining);
        }
    }

    void LaunchPawnWithVelocity(AFortPlayerPawnAthena* Pawn, const FVector& LaunchVelocity)
    {
        if (auto* LaunchJump = Pawn->GetFunction("LaunchCharacterJump"))
        {
            Pawn->Call<void>(LaunchJump, LaunchVelocity, true, true, true, true);
            return;
        }

        if (auto* Launch = Pawn->GetFunction("LaunchCharacter"))
            Pawn->Call<void>(Launch, LaunchVelocity, true, true);
    }

    void EvaluateBump(AActor* Vehicle, const FVector& Location, const FVector& Velocity,
        ULONGLONG CurrentTimeMs)
    {
        if (GBumpScanInProgress)
            return;

        const double Speed = Velocity.Magnitude();

        double MinimumSpeedKmh = FConfiguration::VehicleBumpMinSpeedKmh;

        if (!std::isfinite(MinimumSpeedKmh) || MinimumSpeedKmh < 0.f)
            MinimumSpeedKmh = 0.f;

        if (!std::isfinite(Speed) || Speed < MinimumSpeedKmh * BumpKmhToCentimetersPerSecond)
        {
            return;
        }

        const FVector TravelDirection = FVector(Velocity.X, Velocity.Y, 0.0).GetSafeNormal();

        if (TravelDirection.IsZero())
            return;

        const FBumpVehicleClass VehicleClass = ClassifyVehicle(Vehicle);

        if (!VehicleClass.IsVehicle || !VehicleClass.Profile)
            return;

        const auto& Pawns = GetBumpPawnCache(CurrentTimeMs);

        if (Pawns.empty())
            return;

        const FBumpTuning Tuning = ResolveBumpTuning(
            Vehicle->Cast<AFortAthenaVehicle>(), *VehicleClass.Profile);

        FBumpDriver Driver{};
        bool DriverLookedUp = false;

        GBumpScanInProgress = true;

        for (const auto& WeakPawn : Pawns)
        {
            auto* Pawn = WeakPawn.Get();

            if (!Pawn)
                continue;

            const FVector ToPawn = Pawn->K2_GetActorLocation() - Location;

            if (std::fabs(ToPawn.Z) > BumpHullVertical)
                continue;

            const double Along = (ToPawn.X * TravelDirection.X) + (ToPawn.Y * TravelDirection.Y);

            if (Along > BumpHullFront || Along < -BumpHullRear)
                continue;

            const double Side = (ToPawn.X * TravelDirection.Y) - (ToPawn.Y * TravelDirection.X);

            if (std::fabs(Side) > BumpHullSide)
                continue;

            const double HorizontalDistance =
                std::sqrt((ToPawn.X * ToPawn.X) + (ToPawn.Y * ToPawn.Y));

            if (HorizontalDistance > BumpMinDirectionTestDistance &&
                (Along / HorizontalDistance) < Tuning.MinDirection)
            {
                continue;
            }

            if (!CanBumpPawn(Pawn, Vehicle))
                continue;

            if (IsBumpOnCooldown(Pawn, CurrentTimeMs))
                continue;

            const FVector LaunchVelocity = BuildBumpLaunchVelocity(TravelDirection, Speed, Tuning);

            LaunchPawnWithVelocity(Pawn, LaunchVelocity);

            double Damage = 0.0;

            if (FConfiguration::bVehicleBumpDamage)
            {
                if (!DriverLookedUp)
                {
                    DriverLookedUp = true;
                    Driver = ResolveVehicleDriver(Vehicle);
                }

                if (ShouldDamageBumpedPawn(Pawn, Driver))
                {
                    Damage = ResolveBumpDamage(Speed, Tuning);
                    ApplyBumpDamage(Pawn, Vehicle, Driver.Pawn, Damage);
                }
            }

            Pawn->ForceNetUpdate();

            if (GBumpLogCount++ < 8)
            {
                SDK::DbgLog("[VehicleBump] %s hit %s at %.0f km/h -> launch %.0f %.0f %.0f "
                    "damage %.0f (fwd=%.2f up=%.2f cap=%.0f angle=%.1f driver=%s)\n",
                    Vehicle->Name.ToString().c_str(), Pawn->Name.ToString().c_str(),
                    Speed / BumpKmhToCentimetersPerSecond, (double)LaunchVelocity.X,
                    (double)LaunchVelocity.Y, (double)LaunchVelocity.Z, Damage, Tuning.ForwardScale,
                    Tuning.VerticalScale, Tuning.MaxSpeed, Tuning.AngleAdjustmentDegrees,
                    Driver.Pawn ? Driver.Pawn->Name.ToString().c_str()
                        : (Driver.Resolved ? "none" : "unknown"));
            }
        }

        GBumpScanInProgress = false;
    }

    FBumpVehicleState& GetVehicleState(AActor* Vehicle)
    {
        for (auto& State : GBumpVehicleStates)
        {
            if (State.Vehicle.Get() == Vehicle)
                return State;
        }

        GBumpVehicleStates.push_back({});
        GBumpVehicleStates.back().Vehicle = Vehicle;
        return GBumpVehicleStates.back();
    }
}

void FortVehicleBump::OnVehicleMoved(AActor* Vehicle, const FVector& Location,
    const FVector& LinearVelocity)
{
    if (VersionInfo.FortniteVersion < 4.30 || !FConfiguration::bVehicleBumpLaunch || !Vehicle)
        return;

    const ULONGLONG CurrentTimeMs = GetTickCount64();

    auto& State = GetVehicleState(Vehicle);
    State.LastDriverMoveTimeMs = CurrentTimeMs;
    State.LastLocation = CopyVector(Location);
    State.LastSampleTimeMs = CurrentTimeMs;
    State.HasSample = true;

    EvaluateBump(Vehicle, Location, LinearVelocity, CurrentTimeMs);
}

void FortVehicleBump::Tick()
{
    if (VersionInfo.FortniteVersion < 4.30 || !FConfiguration::bVehicleBumpLaunch)
        return;

    auto* World = UWorld::GetWorld();

    if (!World)
        return;

    const ULONGLONG CurrentTimeMs = GetTickCount64();

    if (CurrentTimeMs >= GBumpVehicleScanExpiryMs)
    {
        GBumpVehicleScanExpiryMs = CurrentTimeMs + BumpVehicleScanLifetimeMs;

        const UClass* VehicleClass = AFortAthenaVehicle::StaticClass();

        if (!VehicleClass)
            VehicleClass = AFortPhysicsPawn::StaticClass();

        if (VehicleClass)
        {
            TArray<AActor*> Vehicles;
            Utils::GetAll<AActor>(VehicleClass, Vehicles);

            for (auto* Vehicle : Vehicles)
            {
                if (Vehicle)
                    GetVehicleState(Vehicle);
            }

            Vehicles.Free();
        }
    }

    for (size_t Index = 0; Index < GBumpVehicleStates.size();)
    {
        auto* Vehicle = GBumpVehicleStates[Index].Vehicle.Get();

        if (!Vehicle)
        {
            GBumpVehicleStates.erase(GBumpVehicleStates.begin() + Index);
            continue;
        }

        if (CurrentTimeMs - GBumpVehicleStates[Index].LastDriverMoveTimeMs <BumpDriverMoveGraceMs)
        {
            ++Index;
            continue;
        }

        const ULONGLONG ElapsedMs = CurrentTimeMs - GBumpVehicleStates[Index].LastSampleTimeMs;

        if (GBumpVehicleStates[Index].HasSample && ElapsedMs < BumpMinSampleIntervalMs)
        {
            ++Index;
            continue;
        }

        const FVector Location = Vehicle->K2_GetActorLocation();

        if (!GBumpVehicleStates[Index].HasSample || ElapsedMs > BumpMaxSampleIntervalMs)
        {
            GBumpVehicleStates[Index].LastLocation = CopyVector(Location);
            GBumpVehicleStates[Index].LastSampleTimeMs = CurrentTimeMs;
            GBumpVehicleStates[Index].HasSample = true;
            ++Index;
            continue;
        }

        const double ElapsedSeconds = (double)ElapsedMs * 0.001;
        const FVector Delta = Location - GBumpVehicleStates[Index].LastLocation;
        const FVector Velocity(Delta.X / ElapsedSeconds, Delta.Y / ElapsedSeconds,
            Delta.Z / ElapsedSeconds);

        GBumpVehicleStates[Index].LastLocation = CopyVector(Location);
        GBumpVehicleStates[Index].LastSampleTimeMs = CurrentTimeMs;

        if (Velocity.Magnitude() <= BumpMaxMeasuredSpeed)
            EvaluateBump(Vehicle, Location, Velocity, CurrentTimeMs);

        ++Index;
    }
}

void AFortPhysicsPawn::ServerMove(UObject* Context, FFrame& Stack)
{
    FQuat Rotation;
    FVector Translation;
    FVector LinearVelocity;
    FVector AngularVelocity;

    static auto StateStruct = FReplicatedPhysicsPawnState::StaticStruct();
    if (StateStruct)
    {
        auto State = (FReplicatedPhysicsPawnState*)malloc(FReplicatedPhysicsPawnState::Size());
        Stack.StepCompiledIn(State);
        Stack.IncrementCode();

        Rotation = State->Rotation;
        Translation = State->Translation;
        LinearVelocity = State->LinearVelocity;
        AngularVelocity = State->AngularVelocity;

        free(State);
    }
    else
    {
        auto State = (FReplicatedAthenaVehiclePhysicsState*)malloc(FReplicatedAthenaVehiclePhysicsState::Size());
        Stack.StepCompiledIn(State);
        Stack.IncrementCode();

        Rotation = State->Rotation;
        Translation = State->Translation;
        LinearVelocity = State->LinearVelocity;
        AngularVelocity = State->AngularVelocity;

        free(State);
    }
    auto Pawn = (AFortPhysicsPawn*)Context;

    if (VersionInfo.EngineVersion < 4.26)
    {
        Rotation.X -= 2.5f;
        Rotation.Y /= 0.3f;
        Rotation.Z -= -2.0f;
        Rotation.W /= -1.2f;
    }
    else
    {
        Rotation.X -= 0.3f;
        Rotation.Y /= -0.75f;
        Rotation.Z += 0.15f;
        Rotation.W /= 1.1f;
    }

    UPrimitiveComponent* RootComponent = Pawn->RootComponent->Cast<UPrimitiveComponent>();

    if (RootComponent)
    {
        RootComponent->K2_SetWorldTransform(FTransform(Translation, Rotation), false, nullptr, true);
        RootComponent->SetPhysicsLinearVelocity(LinearVelocity, 0, FName(0));
        RootComponent->SetPhysicsAngularVelocityInRadians(AngularVelocity, 0, FName(0));
    }

    FortVehicleBump::OnVehicleMoved(Pawn, Translation, LinearVelocity);
}

void AFortSpaghettiVehicle::ServerUpdateTowhook(UObject* Context, FFrame& Stack)
{
    FVector InNetTowhookAimDir;

    Stack.StepCompiledIn(&InNetTowhookAimDir);
    Stack.IncrementCode();
    auto Vehicle = (AFortSpaghettiVehicle*)Context;

    Vehicle->NetTowhookAimDir = InNetTowhookAimDir;
    Vehicle->OnRep_NetTowhookAimDir();
}

struct FHitResult
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FHitResult);

    DEFINE_STRUCT_PROP(Location, FVector);
    DEFINE_STRUCT_PROP(ImpactPoint, FVector);
    DEFINE_STRUCT_PROP(ImpactNormal, FVector);
    DEFINE_STRUCT_PROP(Normal, FVector);
    DEFINE_STRUCT_PROP(Component, TWeakObjectPtr<UActorComponent>);
};

struct FAttachedInfo
{
public:
    USCRIPTSTRUCT_COMMON_MEMBERS(FAttachedInfo);

    DEFINE_STRUCT_PROP(Hit, FHitResult);
    DEFINE_STRUCT_NEWOBJ_PROP(AttachedToActor, AActor);
};

class AFortOctopusTowhookAttachableProjectile : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortOctopusTowhookAttachableProjectile);

    DEFINE_PROP(AttachedInfo, FAttachedInfo);
    DEFINE_PROP(OwningVehicle, AFortOctopusVehicle*);

    DEFINE_FUNC(Kill, void);
};

static bool IsFiniteTowhookVector(const FVector& Value)
{
    return std::isfinite((double)Value.X) && std::isfinite((double)Value.Y) &&
        std::isfinite((double)Value.Z);
}

struct FTowhookAttachment
{
    TWeakObjectPtr<AFortOctopusVehicle> Vehicle;
    TWeakObjectPtr<AActor> Projectile;
    TWeakObjectPtr<UActorComponent> Component;
};

static std::vector<FTowhookAttachment> GTowhookAttachments;

static bool IsTowhookAttached(AFortOctopusVehicle* Vehicle)
{
    return FNetTowhookAttachState::HasComponent() && Vehicle->HasReplicatedAttachState() &&
        Vehicle->ReplicatedAttachState.Component != nullptr;
}

static void WriteTowhookAttachState(AFortOctopusVehicle* Vehicle, UActorComponent* Component,
    FVector LocalLocation, FVector LocalNormal)
{
    auto Write = [&](FNetTowhookAttachState& State)
        {
            if (FNetTowhookAttachState::HasComponent())
                State.Component = Component;
            if (FNetTowhookAttachState::HasLocalLocation())
                State.LocalLocation = LocalLocation;
            if (FNetTowhookAttachState::HasLocalNormal())
                State.LocalNormal = LocalNormal;
        };

    if (Vehicle->HasReplicatedAttachState())
        Write(Vehicle->ReplicatedAttachState);

    Vehicle->OnRep_ReplicatedAttachState();

    if (Vehicle->HasLocalAttachState())
        Write(Vehicle->LocalAttachState);

    Vehicle->ForceNetUpdate();
}

static void ClearTowhookAttachState(AFortOctopusVehicle* Vehicle)
{
    const bool bWasAttached = IsTowhookAttached(Vehicle);

    WriteTowhookAttachState(Vehicle, nullptr, FVector(), FVector());

    auto* BreakTowhookFunction = Vehicle->GetFunction("BreakTowhook");
    if (bWasAttached && BreakTowhookFunction && BreakTowhookFunction->GetParamsNamed().Size == 0)
        Vehicle->ProcessEvent(BreakTowhookFunction, nullptr);
}

static void ReleaseTowhookAttachment(AFortOctopusVehicle* Vehicle, const AActor* Projectile,
    bool bReleaseMatching)
{
    for (auto Attachment = GTowhookAttachments.begin();
        Attachment != GTowhookAttachments.end(); )
    {
        auto* AttachedVehicle = Attachment->Vehicle.Get();
        if (!IsUsablePhysicsObject(AttachedVehicle))
        {
            Attachment = GTowhookAttachments.erase(Attachment);
            continue;
        }

        if (AttachedVehicle != Vehicle ||
            (Attachment->Projectile.Get() == Projectile) != bReleaseMatching)
        {
            ++Attachment;
            continue;
        }

        Attachment = GTowhookAttachments.erase(Attachment);
        ClearTowhookAttachState(AttachedVehicle);
    }
}

static bool ApplyTowhookHit(AFortOctopusVehicle* Vehicle,
    AFortOctopusTowhookAttachableProjectile* Projectile)
{
    auto* Comp = Projectile->AttachedInfo.Hit.Component.Get();
    if (!Comp)
        return false;

    auto ComponentToWorld = Comp->K2_GetComponentToWorld();
    auto HitLocation = CopyVector(Projectile->AttachedInfo.Hit.Location);
    auto HitNormal = CopyVector(Projectile->AttachedInfo.Hit.Normal);
    auto LocalLocation = UKismetMathLibrary::InverseTransformLocation(ComponentToWorld, HitLocation);
    auto LocalNormal = UKismetMathLibrary::InverseTransformDirection(ComponentToWorld, HitNormal);

    if (!IsFiniteTowhookVector(LocalLocation) || !IsFiniteTowhookVector(LocalNormal))
        return false;

    ReleaseTowhookAttachment(Vehicle, (AActor*)Projectile, false);
    WriteTowhookAttachState(Vehicle, Comp, LocalLocation, LocalNormal);

    FTowhookAttachment Attachment{};
    Attachment.Vehicle = TWeakObjectPtr<AFortOctopusVehicle>(Vehicle);
    Attachment.Projectile = TWeakObjectPtr<AActor>((AActor*)Projectile);
    Attachment.Component = TWeakObjectPtr<UActorComponent>(Comp);
    GTowhookAttachments.push_back(Attachment);

    auto Owner = Comp->GetOwner();
    printf("[Ballers] Attached %s (%s) at %f %f %f\n", Comp->Name.ToString().c_str(),
        Owner ? Owner->Name.ToString().c_str() : "<none>", HitLocation.X, HitLocation.Y,
        HitLocation.Z);
    return true;
}

void AFortOctopusVehicle::TickTowhookAttachments()
{
    for (auto Attachment = GTowhookAttachments.begin();
        Attachment != GTowhookAttachments.end(); )
    {
        auto* Vehicle = Attachment->Vehicle.Get();
        if (!IsUsablePhysicsObject(Vehicle))
        {
            Attachment = GTowhookAttachments.erase(Attachment);
            continue;
        }

        if (!IsTowhookAttached(Vehicle))
        {
            Attachment = GTowhookAttachments.erase(Attachment);
            continue;
        }

        auto* Projectile = (AFortOctopusTowhookAttachableProjectile*)Attachment->Projectile.Get();
        if (IsUsablePhysicsObject(Projectile) &&
            Projectile->AttachedInfo.Hit.Component.Get() == Attachment->Component.Get())
        {
            ++Attachment;
            continue;
        }

        Attachment = GTowhookAttachments.erase(Attachment);
        ClearTowhookAttachState(Vehicle);
        printf("[Ballers] Released towhook on %s\n", Vehicle->Name.ToString().c_str());
    }
}

void AFortOctopusVehicle::ServerUpdateTowhook(UObject* Context, FFrame& Stack)
{
    FVector InNetTowhookAimDir;

    Stack.StepCompiledIn(&InNetTowhookAimDir);
    Stack.IncrementCode();
    auto Vehicle = (AFortOctopusVehicle*)Context;

    printf("[Ballers] ServerUpdateTowhook aim=%f %f %f attached=%d projectile=%d\n",
        InNetTowhookAimDir.X, InNetTowhookAimDir.Y, InNetTowhookAimDir.Z,
        IsTowhookAttached(Vehicle),
        Vehicle->HasTowHookProjectile() && Vehicle->TowHookProjectile != nullptr);

    Vehicle->NetTowhookAimDir = InNetTowhookAimDir;
    Vehicle->OnRep_NetTowhookAimDir();
}

static bool GNativeTowhookPublishes = false;

uint64_t CanGrappleToComponent_ = 0;
void (*OnRep_ReplicatedAttachedInfoOG)(AFortOctopusTowhookAttachableProjectile* _this);
void OnRep_ReplicatedAttachedInfo(AFortOctopusTowhookAttachableProjectile* _this)
{
    auto OwningVehicle = _this->OwningVehicle;
    const bool bAttachedBeforeOG = OwningVehicle && IsTowhookAttached(OwningVehicle);

    OnRep_ReplicatedAttachedInfoOG(_this);

    if (!OwningVehicle)
    {
        printf("[Ballers] OnRep_ReplicatedAttachedInfo: no owning vehicle\n");
        return;
    }

    if (!bAttachedBeforeOG && IsTowhookAttached(OwningVehicle) && !GNativeTowhookPublishes)
    {
        GNativeTowhookPublishes = true;
        printf("[Ballers] Native towhook publishing detected; leaving the anchor to the game\n");
    }

    if (GNativeTowhookPublishes)
        return;

    if (_this->AttachedInfo.HasAttachedToActor() && !_this->AttachedInfo.AttachedToActor)
    {
        ReleaseTowhookAttachment(OwningVehicle, (AActor*)_this, true);
        return;
    }

    auto Comp = _this->AttachedInfo.Hit.Component.Get();
    if (!Comp)
    {
        ReleaseTowhookAttachment(OwningVehicle, (AActor*)_this, true);
        return;
    }

    for (const auto& Attachment : GTowhookAttachments)
    {
        if (Attachment.Vehicle.Get() == OwningVehicle &&
            Attachment.Projectile.Get() == (AActor*)_this)
        {
            return;
        }
    }

    auto CanGrappleToComponent = (bool(*)(AFortOctopusVehicle*, UActorComponent*))CanGrappleToComponent_;

    if (!CanGrappleToComponent_ || !CanGrappleToComponent(OwningVehicle, Comp))
    {
        printf("[Ballers] Rejected %s (resolver=%p)\n", Comp->Name.ToString().c_str(),
            (void*)CanGrappleToComponent_);
        ReleaseTowhookAttachment(OwningVehicle, (AActor*)_this, true);
        return;
    }

    if (!ApplyTowhookHit(OwningVehicle, _this))
        printf("[Ballers] OnRep_ReplicatedAttachedInfo: apply failed\n");
}

namespace
{
    AFortPlayerPawnAthena* GetCannonPawnAtSeat(AActor* Vehicle, int32 SeatIndex)
    {
        if (!IsUsablePhysicsObject(Vehicle) || SeatIndex < 0)
            return nullptr;

        if (auto* GetPawnAtSeatFunction = Vehicle->GetFunction("GetPawnAtSeat"))
        {
            auto* Occupant = Vehicle->Call<AActor*>(GetPawnAtSeatFunction, SeatIndex);
            return IsUsablePhysicsObject(Occupant)
                ? Occupant->Cast<AFortPlayerPawnAthena>() : nullptr;
        }

        auto* SeatComponent = (UFortVehicleSeatComponent*)Vehicle->GetComponentByClass(
            UFortVehicleSeatComponent::StaticClass());

        if (!IsUsablePhysicsObject(SeatComponent) || !SeatComponent->HasPlayerSlots() ||
            SeatIndex >= SeatComponent->PlayerSlots.Num())
        {
            return nullptr;
        }

        auto* Occupant = SeatComponent->PlayerSlots
            .Get(SeatIndex, FAthenaCarPlayerSlot::Size()).Player;
        return IsUsablePhysicsObject(Occupant) ? Occupant : nullptr;
    }

    bool IsCannonVehicle(AActor* Vehicle)
    {
        return IsUsablePhysicsObject(Vehicle) && Vehicle->GetFunction("OnPreLaunchPawn") &&
            Vehicle->GetFunction("OnLaunchPawn");
    }

    AFortPlayerPawnAthena* GetWeaponHolder(AActor* Weapon)
    {
        AActor* Current = Weapon;

        for (int32 Step = 0;
            IsUsablePhysicsObject(Current) && Step < 4; ++Step)
        {
            if (auto* Pawn = Current->Cast<AFortPlayerPawnAthena>())
                return Pawn;

            if (auto* Controller = Current->Cast<AFortPlayerControllerAthena>())
            {
                auto* Pawn = (Controller->HasMyFortPawn() && Controller->MyFortPawn)
                    ? Controller->MyFortPawn : Controller->Pawn;
                return IsUsablePhysicsObject(Pawn) ? Pawn->Cast<AFortPlayerPawnAthena>() : nullptr;
            }

            auto* Owner = Current->HasOwner() ? Current->Owner : nullptr;
            Current = IsUsablePhysicsObject(Owner) ? Owner : nullptr;
        }

        return nullptr;
    }

    constexpr double CannonLaunchForwardSpeed = 6000.0;
    constexpr double CannonLaunchLateralSpeed = 5000.0;
    constexpr double CannonLaunchVerticalSpeed = 7500.0;

    double SanitizeCannonMultiplier(float Value)
    {
        return (std::isfinite(Value) && Value >= 0.f) ? (double)Value : 1.0;
    }

    bool NormalizeCannonLaunchDirection(FVector& Direction)
    {
        const double X = Direction.X;
        const double Y = Direction.Y;
        const double Z = Direction.Z;
        const double SizeSquared = X * X + Y * Y + Z * Z;
        if (!std::isfinite(X) || !std::isfinite(Y) ||
            !std::isfinite(Z) || !std::isfinite(SizeSquared) || SizeSquared <= 1.e-8)
        {
            return false;
        }

        const double Scale = 1.0 / std::sqrt(SizeSquared);
        Direction = FVector(X * Scale, Y * Scale, Z * Scale);
        return true;
    }

    bool HasCannonLaunchDirectionParameter(UFunction* Function)
    {
        if (!Function)
            return false;

        constexpr uint64 CPF_Parm = 0x80;
        constexpr uint64 CPF_OutParm = 0x100;
        constexpr uint64 CPF_ReturnParm = 0x400;
        const auto Parameters = Function->GetParamsNamed();
        int32 InputCount = 0;
        bool bFoundDirection = false;
        for (const auto& Parameter : Parameters.NameOffsetMap)
        {
            if (!(Parameter.PropertyFlags & CPF_Parm) || (Parameter.PropertyFlags & CPF_ReturnParm))
            {
                continue;
            }

            ++InputCount;
            bFoundDirection = !(Parameter.PropertyFlags & CPF_OutParm) && Parameter.ElementSize ==
                    static_cast<uint32>(FVector::Size()) && Parameter.Offset <= Parameters.Size &&
                Parameter.ElementSize <= Parameters.Size - Parameter.Offset;
        }

        return InputCount == 1 && bFoundDirection;
    }

    bool IsCannonFireFunctionOwner(UFunction* Function)
    {
        if (!IsUsablePhysicsObject(Function) || !IsUsablePhysicsObject(Function->Outer))
        {
            return false;
        }

        auto OwnerName = Function->Outer->Name.ToString();
        std::transform(OwnerName.begin(), OwnerName.end(), OwnerName.begin(),
            [](unsigned char Character)
            {
                return static_cast<char>(std::tolower(Character));
            });
        return OwnerName.find("cannon") != std::string::npos;
    }

    void ForcePawnOutOfCannon(AFortPlayerPawnAthena* Pawn)
    {
        auto* ExitFunction = IsUsablePhysicsObject(Pawn)
            ? Pawn->GetFunction("ServerOnExitVehicle") : nullptr;

        if (!ExitFunction)
            return;

        alignas(16) std::array<uint8, 0x100> ExitParams{};

        if (ExitFunction->GetParamsNamed().Size > ExitParams.size())
            return;

        Pawn->ProcessEvent(ExitFunction, ExitParams.data());
    }
}

void AFortWeaponRangedMountedCannon::ServerFireActorInCannon(UObject* Context, FFrame& Stack)
{
    FVector LaunchDir{};

    auto* FireFunction = Stack.GetCurrentNativeFunction();
    if (FireFunction && !HasCannonLaunchDirectionParameter(FireFunction))
    {
        Stack.IncrementCode();
        return;
    }

    Stack.StepCompiledIn(&LaunchDir);

    Stack.IncrementCode();

    if (!NormalizeCannonLaunchDirection(LaunchDir))
        return;

    auto* Weapon = (AActor*)Context;

    if (!IsUsablePhysicsObject(Weapon))
        return;

    auto* Operator = GetWeaponHolder(Weapon);
    auto* Cannon = GetRiddenVehicle(Operator);

    if (!IsCannonVehicle(Cannon))
        return;

    auto* PreLaunchFunction = Cannon->GetFunction("OnPreLaunchPawn");
    auto* LaunchFunction = Cannon->GetFunction("OnLaunchPawn");
    if (!PreLaunchFunction || !LaunchFunction)
        return;

    AFortPlayerPawnAthena* TargetPawn = GetCannonPawnAtSeat(Cannon, 1);
    if (TargetPawn == Operator)
        TargetPawn = nullptr;

    if (!TargetPawn)
        TargetPawn = Operator;

    if (!TargetPawn)
        return;

    Cannon->Call<void>(PreLaunchFunction, TargetPawn, LaunchDir);
    ForcePawnOutOfCannon(TargetPawn);

    if (FConfiguration::bCannonLaunchAnimations)
    {
        Cannon->Call<void>(LaunchFunction, TargetPawn, LaunchDir);
    }
    else
    {
        LaunchPawnWithVelocity(TargetPawn, FVector(LaunchDir.X * CannonLaunchForwardSpeed *
                    SanitizeCannonMultiplier(FConfiguration::CannonLaunchXMultiplier),
                LaunchDir.Y * CannonLaunchLateralSpeed *
                    SanitizeCannonMultiplier(FConfiguration::CannonLaunchYMultiplier),
                LaunchDir.Z * CannonLaunchVerticalSpeed *
                    SanitizeCannonMultiplier(FConfiguration::CannonLaunchZMultiplier)));
    }

    if (auto* LaunchedFunction = Cannon->GetFunction("MultiCastPushCannonLaunchedPlayer"))
    {
        alignas(16) std::array<uint8, 0x80> LaunchedParams{};

        if (LaunchedFunction->GetParamsNamed().Size <= LaunchedParams.size())
            Cannon->ProcessEvent(LaunchedFunction, LaunchedParams.data());
    }

    TargetPawn->ForceNetUpdate();
}

void AFortPhysicsPawn::Hook()
{
    FortVehicleMods::InstallHooks();

    auto DefaultPhysPawn = GetDefaultObj();
    if (DefaultPhysPawn)
    {
        auto ServerMoveFn = DefaultPhysPawn->GetFunction("ServerMove");

        if (ServerMoveFn)
        {
            Utils::ExecHook(ServerMoveFn, ServerMove);
            Utils::ExecHook(DefaultPhysPawn->GetFunction("ServerMoveReliable"), ServerMove);
        }
        else
        {
            auto ServerUpdatePhysicsParamsFn = DefaultPhysPawn->GetFunction("ServerUpdatePhysicsParams");

            if (ServerUpdatePhysicsParamsFn)
                Utils::ExecHook(ServerUpdatePhysicsParamsFn, ServerMove);
        }
    }
    else
    {
        auto DefaultVehicle = DefaultObjImpl("FortAthenaVehicle");

        if (DefaultVehicle)
            Utils::ExecHook(DefaultVehicle->GetFunction("ServerUpdatePhysicsParams"), ServerMove);
    }

    auto DefaultOctopusVehicle = AFortOctopusVehicle::GetDefaultObj();

    if (DefaultOctopusVehicle)
    {
        Utils::ExecHook(DefaultOctopusVehicle->GetFunction("ServerUpdateTowhook"), AFortOctopusVehicle::ServerUpdateTowhook);
    }

    auto DefaultSpaghettiVehicle = AFortSpaghettiVehicle::GetDefaultObj();

    if (DefaultSpaghettiVehicle)
        Utils::ExecHook(DefaultSpaghettiVehicle->GetFunction("ServerUpdateTowhook"), AFortSpaghettiVehicle::ServerUpdateTowhook);

    if (AFortOctopusTowhookAttachableProjectile::StaticClass())
    {
        auto OnRep_ReplicatedAttachedInfoIdx = AFortOctopusTowhookAttachableProjectile::GetDefaultObj()->GetFunction("OnRep_ReplicatedAttachedInfo")->GetVTableIndex();

        auto OnRep_ReplicatedAttachedInfo__Impl = AFortOctopusTowhookAttachableProjectile::GetDefaultObj()->Vft[OnRep_ReplicatedAttachedInfoIdx];
        auto CanGrappleToComponent = Memcury::Scanner(OnRep_ReplicatedAttachedInfo__Impl).ScanFor({ 0xFF, 0x90 }).Get();

        for (int i = 0; i < 2000; i++)
        {
            auto Ptr = (uint8_t*)(CanGrappleToComponent - i);

            if (*Ptr == 0x48 && *(Ptr + 1) == 0x8B)
            {
                if (*(Ptr + 3) == 0xE8)
                {
                    CanGrappleToComponent_ = Memcury::Scanner(Ptr).RelativeOffset(4).Get();
                    break;
                }
                else if (*(Ptr + 3) == 0xFF && (*(Ptr + 4) & 0xF0) == 0x90)
                {
                    CanGrappleToComponent_ = uint64_t(AFortOctopusTowhookAttachableProjectile::GetDefaultObj()->Vft[*(uint32_t*)(Ptr + 5) / 8]);
                    break;
                }
            }
        }

        Utils::Hook<AFortOctopusTowhookAttachableProjectile>(OnRep_ReplicatedAttachedInfoIdx,
            OnRep_ReplicatedAttachedInfo, OnRep_ReplicatedAttachedInfoOG);

        printf("[Ballers] Hook installed idx=%d CanGrappleToComponent=%p attach-info=%d "
            "attached-to-actor=%d local-state=%d\n", (int)OnRep_ReplicatedAttachedInfoIdx,
            (void*)CanGrappleToComponent_,
            AFortOctopusTowhookAttachableProjectile::GetDefaultObj()->HasAttachedInfo(),
            AFortOctopusTowhookAttachableProjectile::GetDefaultObj()
                ->AttachedInfo.HasAttachedToActor(),
            AFortOctopusVehicle::GetDefaultObj()->HasLocalAttachState());
    }

    auto MountedCannonWeapon = AFortWeaponRangedMountedCannon::GetDefaultObj();
    auto FireActorInCannonFn = MountedCannonWeapon
        ? MountedCannonWeapon->GetFunction("ServerFireActorInCannon") : nullptr;

    if (!FireActorInCannonFn)
    {
        auto* Candidate = (UFunction*)TUObjectArray::FindObject(
            "ServerFireActorInCannon", 0, FindClass("Function"));
        if (IsCannonFireFunctionOwner(Candidate))
            FireActorInCannonFn = Candidate;
    }

    if (FireActorInCannonFn && !HasCannonLaunchDirectionParameter(FireActorInCannonFn))
    {
        SDK::DbgLog("[Cannon] rejected incompatible ServerFireActorInCannon "
            "owner=%p FN=%.2f\n", (void*)FireActorInCannonFn->Outer, VersionInfo.FortniteVersion);
        FireActorInCannonFn = nullptr;
    }

    if (FireActorInCannonFn)
    {
        Utils::ExecHook(FireActorInCannonFn,
            AFortWeaponRangedMountedCannon::ServerFireActorInCannon);
    }

    SDK::DbgLog("[Cannon] ServerFireActorInCannon %s (class=%p)\n",
        FireActorInCannonFn ? "hooked" : "not present on this build", (void*)MountedCannonWeapon);
}