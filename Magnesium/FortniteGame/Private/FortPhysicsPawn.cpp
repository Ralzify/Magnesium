#include "pch.h"
#include "../Public/FortPhysicsPawn.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortWeapon.h"
#include "../Public/BattleRoyaleGamePhaseLogic.h"
#include "../Public/FortVehicleMods.h"

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

// ---------------------------------------------------------------------------
// Vehicle -> player knockback (getting bumped by a car)
//
// Retail Fortnite simulates a vehicle wherever it is authoritative, so driving
// into someone produces a genuine physics contact and the native code launches
// the pawn out of it - harder the faster the vehicle was travelling.
//
// Magnesium never simulates a vehicle. The driving client owns the physics and
// ServerMove below writes the transform and velocity it reports directly onto
// the actor. That is a teleport, and teleports generate no contacts, so nothing
// server side ever notices the pawn standing in the road. This is why the bump
// is missing outright rather than weak or unreliable, and why no amount of
// collision tuning brings it back.
//
// So the contact test is done here instead, inside that same RPC - the one
// moment the server knows both where a driven vehicle is and how fast it is
// going. The shape of the launch is not invented: Fortnite ships its own tuning
// as AFortAthenaVehicle::PawnLaunch*, and it is read per vehicle on every build
// that has it, so a Whiplash throws you differently from an ATK exactly as it
// should.
// ---------------------------------------------------------------------------
namespace
{
    constexpr double BumpKmhToCentimetersPerSecond = 27.7777778;
    constexpr double BumpDegreesToRadians = 0.017453292519943295;

    // ---- Fallback launch profiles ----------------------------------------
    //
    // Used only for whichever PawnLaunch* the running build does not expose;
    // any native value always wins over the number here.
    //
    // The scales are back-solved from how far players actually get sent in
    // retail, because a launch is a ballistic arc and its apex pins the
    // vertical component exactly: apex = v_z^2 / 2g, so v_z = sqrt(2*g*apex)
    // at Fortnite's g of 980 uu/s^2.
    //
    //   Car, ~20 m up and ~30 m out  -> v_z ~= 1980, v_xy ~= 750
    //   UFO, ~200 m up and ~100 m out -> v_z ~= 6260, v_xy ~= 780
    //
    // Two things fall out of that. Vertical dominates - a car bump is roughly
    // 2.5x more up than out, which is why a model weighted toward forward
    // velocity looks like a shove rather than a launch. And the horizontal
    // component is near identical between the two vehicles: what makes the UFO
    // famous is purely how much higher it throws you, so that is where the
    // per-vehicle profile differs.
    //
    // What the arithmetic does NOT survive is the trip to the client. Measured
    // in game, these scales land about five times short, because a server-side
    // LaunchCharacter arrives as a position correction rather than a real
    // impulse and most of the arc is spent before the client ever sees it.
    // FConfiguration::VehicleBumpForceMultiplier carries that factor. So treat
    // the numbers below as the *shape* of each vehicle's launch - up versus out,
    // car versus UFO - and the multiplier as its magnitude; the distances quoted
    // here are the pre-loss velocities, not what a player actually travels.
    constexpr double BumpDefaultMinDirection = -0.35;
    constexpr double BumpDefaultAngleAdjustment = 0.0;

    const UClass* MatchOctopusVehicle()
    {
        return AFortOctopusVehicle::StaticClass();
    }

    struct FBumpLaunchProfile
    {
        // Preferred match: an exact class test, for vehicles the SDK declares.
        // Null where it does not.
        const UClass* (*MatchClass)();
        // Fallback match: case-insensitive substring of the vehicle class name.
        // Both null is the catch-all, which must be last.
        const char* ClassNameFragment;
        // 0 applies to every build. Anything else is a behaviour a specific
        // season introduced and earlier builds must not inherit.
        double MinFortniteVersion;
        double ForwardScale;
        double VerticalScale;
        double MaxSpeed;
    };

    // Ordered: first match wins.
    constexpr FBumpLaunchProfile BumpLaunchProfiles[] =
    {
        // The Baller, from 27.00 on, where it throws players roughly 300 m up
        // and 800 m out - far enough that it is a distinct mechanic rather than
        // a strong bump. Back-solved the same way as everything else:
        //
        //   apex 300 m -> v_z ~= 7670,  and a 15.6 s flight over 800 m
        //   -> v_xy ~= 5110
        //
        // Unlike the car and the UFO this one throws hard in both axes, so the
        // horizontal scale is large too. Solved against a Baller speed near
        // 2000 uu/s; the cap is set to exactly that launch, so the reference
        // clip is the ceiling and a slower Baller sends you proportionally less
        // far rather than a faster one sending you into orbit.
        //
        // Matched by class first because Magnesium already declares it -
        // "Octopus" is Fortnite's internal name for the Baller. The name
        // fragments only matter if a build fails to resolve that class.
        { MatchOctopusVehicle, "octopus", 27.00, 2.56, 3.83, 9200.0 },
        { nullptr,             "baller",  27.00, 2.56, 3.83, 9200.0 },

        // The UFO. Matched by name because the only raw dump available here is
        // 13.40, which predates it - "Saucer" is Fortnite's internal name for it
        // throughout its assets. Scales are solved against a saucer top speed of
        // roughly 1500 uu/s, so if the launch comes out over- or under-powered
        // this is the pair to move.
        { nullptr, "saucer", 0.0, 0.45, 4.20, 9000.0 },
        { nullptr, "ufo",    0.0, 0.45, 4.20, 9000.0 },

        // Cars, and everything else that does not call itself out. At a
        // Whiplash's ~3300 uu/s this lands on v_xy 825 / v_z 2145 before the
        // delivery-loss multiplier.
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
                // Tested against the actor, not its class object: this asks
                // whether the vehicle is one of these, which is what subclass
                // matching means here.
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

    // ---- Fallback damage curve -------------------------------------------
    //
    // Same arrangement: native VehicleMin/MaxHorSpeed(To)Damage wins where the
    // build has it. Damage ramps linearly between the two speeds.
    constexpr double BumpDefaultMinSpeedToDamage = 700.0;
    constexpr double BumpDefaultMaxSpeedToDamage = 2800.0;
    constexpr double BumpDefaultMinSpeedDamage = 15.0;
    constexpr double BumpDefaultMaxSpeedDamage = 90.0;

    // Half-extents of the hull we test against, in the frame of the vehicle's
    // travel direction rather than its facing - a car sliding sideways hits you
    // with the side it is actually travelling towards. A Fortnite car is roughly
    // 550x250 and a pawn capsule adds ~42; the front is stretched further so a
    // pawn cannot pass entirely through the hull between two move RPCs at speed.
    constexpr double BumpHullFront = 320.0;
    constexpr double BumpHullRear = 130.0;
    constexpr double BumpHullSide = 165.0;
    constexpr double BumpHullVertical = 170.0;

    // One contact must not re-launch the same pawn on every RPC that follows it.
    constexpr ULONGLONG BumpCooldownMs = 750;

    // Who is in the match changes far more slowly than ServerMove fires, so the
    // pawn set is enumerated on a timer and only their positions are re-read.
    constexpr ULONGLONG BumpPawnCacheLifetimeMs = 500;

    // Below this the direction from vehicle to pawn is numerically meaningless,
    // and the pawn is inside the hull anyway.
    constexpr double BumpMinDirectionTestDistance = 60.0;

    // ---- Driverless tracking ---------------------------------------------
    //
    // A vehicle nobody is driving still rolls, gets shot down a hill, or keeps
    // the momentum its last driver left it with, and it is supposed to bump
    // whoever it hits. No client sends move RPCs for it, so its motion has to
    // be measured from the server's own view of it.
    constexpr ULONGLONG BumpVehicleScanLifetimeMs = 2000;

    // How long after a move RPC a vehicle is still considered driver-owned.
    // Comfortably longer than the gap between RPCs at any sane send rate.
    constexpr ULONGLONG BumpDriverMoveGraceMs = 400;

    // Two samples closer together than this make the derived velocity mostly
    // quantization noise. It also caps what parked vehicles cost, since reading
    // a location is a reflected call and most of a map's vehicles never move.
    constexpr ULONGLONG BumpMinSampleIntervalMs = 60;

    // Past this the pair is too far apart to divide - a server hitch would turn
    // an ordinary displacement into a fake high speed - so the sample is thrown
    // away and re-seeded rather than used.
    constexpr ULONGLONG BumpMaxSampleIntervalMs = 500;

    // Comfortably above the fastest thing anyone drives or flies (~430 km/h),
    // so exceeding it means the vehicle was moved rather than moving.
    constexpr double BumpMaxMeasuredSpeed = 12000.0;

    // SDK::FVector only declares assignment from a non-const lvalue and an
    // rvalue, so a const reference cannot be assigned directly. Rebuilding it as
    // a prvalue picks the move overload.
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

    // A vehicle currently being watched by the tick path, so driverless motion
    // can be measured. Also records when a driver last moved it, because the
    // RPC path already knows the real velocity and the derived one would only
    // be a worse estimate of the same thing.
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
    // The scan calls into script while holding a reference to the pawn cache.
    // Nothing on that path should re-enter it, but re-entering would rebuild
    // the cache mid-iteration, so it is refused outright.
    bool GBumpScanInProgress = false;

    // Only actual vehicles bump. ServerMove is shared with every other physics
    // pawn, and a loose one rolling past a player must not launch them.
    //
    // The seat component is the fallback rather than the primary test because it
    // costs a reflected call: a build whose Baller or Driftboard does not descend
    // from FortAthenaVehicle still resolves correctly, and the answer is cached
    // per class so it is paid once per vehicle type per session. The launch
    // profile is resolved on the same pass, since it is also a per-class answer
    // and costs a string search.
    // Returned by value: the cache is a vector that reallocates as new vehicle
    // classes appear, so a reference into it would not stay valid.
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
        // The Baller is named separately because Magnesium declares it under
        // FortPhysicsPawn rather than FortAthenaVehicle, and it has a launch
        // profile of its own - leaving it to the seat-component fallback would
        // make that profile depend on a lookup that is only a fallback.
        else if (OctopusVehicleClass && Actor->IsA(OctopusVehicleClass))
            IsVehicle = true;
        else if (SeatComponentClass && Actor->GetComponentByClass(SeatComponentClass))
            IsVehicle = true;

        GBumpVehicleClasses.push_back(
            { ActorClass, IsVehicle, &ResolveLaunchProfile(Actor) });
        return GBumpVehicleClasses.back();
    }

    FBumpTuning ResolveBumpTuning(
        AFortAthenaVehicle* Vehicle,
        const FBumpLaunchProfile& Profile)
    {
        FBumpTuning Tuning{};

        Tuning.ForwardScale = Profile.ForwardScale;
        Tuning.VerticalScale = Profile.VerticalScale;
        Tuning.MaxSpeed = Profile.MaxSpeed;

        if (!Vehicle)
            return Tuning;

        // A build can expose some of these and not others, so each is probed
        // independently. Zero is rejected for the scales that gate the launch
        // and for the cap, because a zero there means "never launch anyone" -
        // which is what an unset property looks like, not a deliberate setting.
        if (Vehicle->HasPawnLaunchForwardVelocityScale())
        {
            const double Value = Vehicle->PawnLaunchForwardVelocityScale;

            if (std::isfinite(Value) && Value > 0.0)
                Tuning.ForwardScale = Value;
        }

        // Vertical is the exception: a vehicle that shoves without popping you
        // into the air is a real configuration, so zero is honoured here.
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

        // The damage curve is taken as a set rather than field by field: a
        // half-native, half-default curve can invert (min speed above max, or
        // min damage above max) and produce nonsense at the ramp's edges.
        if (Vehicle->HasVehicleMinHorSpeedToDamage() &&
            Vehicle->HasVehicleMaxHorSpeedToDamage() &&
            Vehicle->HasVehicleMinHorSpeedDamage() &&
            Vehicle->HasVehicleMaxHorSpeedDamage())
        {
            const double MinSpeed = Vehicle->VehicleMinHorSpeedToDamage;
            const double MaxSpeed = Vehicle->VehicleMaxHorSpeedToDamage;
            const double MinDamage = Vehicle->VehicleMinHorSpeedDamage;
            const double MaxDamage = Vehicle->VehicleMaxHorSpeedDamage;

            if (std::isfinite(MinSpeed) && std::isfinite(MaxSpeed) &&
                std::isfinite(MinDamage) && std::isfinite(MaxDamage) &&
                MinSpeed >= 0.0 && MaxSpeed > MinSpeed &&
                MinDamage >= 0.0 && MaxDamage >= MinDamage &&
                MaxDamage > 0.0)
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
        return Tuning.MinSpeedDamage +
            (Tuning.MaxSpeedDamage - Tuning.MinSpeedDamage) * Alpha;
    }

    // Splits the impact speed into a horizontal push along the vehicle's travel
    // direction and a vertical pop, tilts the result up by the vehicle's angle
    // adjustment, then caps it.
    FVector BuildBumpLaunchVelocity(
        const FVector& TravelDirection,
        double Speed,
        const FBumpTuning& Tuning)
    {
        double Horizontal = Speed * Tuning.ForwardScale;
        double Vertical = Speed * Tuning.VerticalScale;

        if (Tuning.AngleAdjustmentDegrees != 0.0)
        {
            const double Magnitude =
                std::sqrt((Horizontal * Horizontal) + (Vertical * Vertical));
            const double Angle = std::atan2(Vertical, Horizontal) +
                (Tuning.AngleAdjustmentDegrees * BumpDegreesToRadians);

            Horizontal = Magnitude * std::cos(Angle);
            Vertical = Magnitude * std::sin(Angle);
        }

        // The game's own ceiling caps the game's own numbers, so it is applied
        // before the multiplier. The multiplier is not part of that tuning - it
        // is the correction for what the launch loses reaching the client - and
        // clamping after it would cap the corrected value against an uncorrected
        // ceiling, throwing away most of the launch again.
        const double Magnitude =
            std::sqrt((Horizontal * Horizontal) + (Vertical * Vertical));

        if (Tuning.MaxSpeed > 0.0 && Magnitude > Tuning.MaxSpeed)
        {
            const double Scale = Tuning.MaxSpeed / Magnitude;

            Horizontal *= Scale;
            Vertical *= Scale;
        }

        double Multiplier = FConfiguration::VehicleBumpForceMultiplier;

        if (!std::isfinite(Multiplier) || Multiplier < 0.f)
            Multiplier = 1.f;

        return FVector(
            TravelDirection.X * Horizontal * Multiplier,
            TravelDirection.Y * Horizontal * Multiplier,
            Vertical * Multiplier);
    }

    bool IsBumpOnCooldown(AFortPlayerPawnAthena* Pawn, ULONGLONG CurrentTimeMs)
    {
        for (auto Entry = GBumpCooldowns.begin(); Entry != GBumpCooldowns.end();)
        {
            auto* Tracked = Entry->Pawn.Get();

            // Prune here rather than on a separate timer: a pawn that died or a
            // cooldown that lapsed is dead weight in a list walked per contact.
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

    // The server-owned pawn -> vehicle association, which stays valid for as
    // long as the pawn occupies a seat. Legacy pawn subclasses expose it under
    // one of several names.
    AActor* GetRiddenVehicle(AFortPlayerPawnAthena* Pawn)
    {
        if (!Pawn)
            return nullptr;

        UFunction* GetVehicleFunction = Pawn->GetFunction("GetVehicleActor");

        if (!GetVehicleFunction)
            GetVehicleFunction = Pawn->GetFunction("GetVehicle");

        if (!GetVehicleFunction)
            GetVehicleFunction = Pawn->GetFunction("BP_GetVehicle");

        return GetVehicleFunction ? Pawn->Call<AActor*>(GetVehicleFunction) : nullptr;
    }

    // Riders move with the vehicle instead of being run over by it, and the
    // driver's own pawn is attached to the vehicle - so it sits inside the hull
    // permanently and would otherwise be launched out of its own car on every
    // move RPC. Getting this wrong is the one failure that breaks driving
    // outright, so it is asked two independent ways.
    bool IsRidingAVehicle(AFortPlayerPawnAthena* Pawn, AActor* Vehicle)
    {
        if (GetRiddenVehicle(Pawn))
            return true;

        // Ask the vehicle instead when the pawn side is unavailable. This is the
        // vehicle's own native seat lookup rather than a walk of PlayerSlots,
        // whose storage differs across Chapter 5 vehicle subclasses - the same
        // reason FortWeaponRanged resolves seats this way. The parameter-size
        // check rejects a same-named function with a different signature.
        UFunction* FindSeatIndexFunction = Vehicle->GetFunction("FindSeatIndex");

        if (FindSeatIndexFunction &&
            FindSeatIndexFunction->GetParamsNamed().Size == 0x10)
        {
            const int32 SeatIndex = Vehicle->Call<int32>(FindSeatIndexFunction, Pawn);

            if (SeatIndex >= 0)
                return true;
        }

        return false;
    }

    // Everything here costs a reflected call, so it runs only for a pawn that
    // already passed the hull test - normally none of them.
    bool CanBumpPawn(AFortPlayerPawnAthena* Pawn, AActor* Vehicle)
    {
        if (IsRidingAVehicle(Pawn, Vehicle))
            return false;

        // A downed player is already on the floor; launching the crawl across
        // the map looks like a bug rather than a hit.
        if (Pawn->IsDBNO())
            return false;

        if (Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving)
            return false;

        if (Pawn->GetHealth() <= 0.f)
            return false;

        return true;
    }

    // ---- Run-over damage --------------------------------------------------

    struct FBumpDriver
    {
        AFortPlayerPawnAthena* Pawn = nullptr;
        // False means the build exposed no way to ask. That is different from
        // "there is no driver", and the damage rules treat it differently:
        // an empty vehicle damages whoever it rolls into, but a vehicle whose
        // occupancy is unknown must not be assumed empty.
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

                // GetDriver answered with something that is not a player pawn -
                // a controller on some builds, an AI pawn on others. Fall
                // through to the seat lookup rather than reporting an empty seat.
                if (Result.Pawn)
                    return Result;

                Result.Resolved = false;
            }
        }

        // Seat 0 is the driver's seat on every Fortnite vehicle.
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

        // The pawn's own PlayerState is set for a live pawn; the controller is
        // the fallback for the window around respawn where it briefly is not.
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

    // True when this playlist has no teams to be friendly with, so every hit is
    // an enemy hit by definition.
    bool IsEveryPlayerTheirOwnTeam()
    {
        auto* World = UWorld::GetWorld();
        auto* GameState = World
            ? (AFortGameStateAthena*)World->GameState
            : nullptr;

        if (!GameState || !GameState->HasCurrentPlaylistInfo() ||
            !GameState->CurrentPlaylistInfo.BasePlaylist ||
            !GameState->CurrentPlaylistInfo.BasePlaylist->HasMaxSquadSize())
        {
            return false;
        }

        return GameState->CurrentPlaylistInfo.BasePlaylist->MaxSquadSize <= 1;
    }

    // Only two situations deal damage: nobody is driving, or the driver is an
    // enemy. Running your own squadmate over shoves them and nothing else.
    //
    // Every uncertain case falls back to the solo test, because the two mistakes
    // are not equally bad: a missed hit is a curiosity, killing your own
    // squadmate loses the game.
    bool ShouldDamageBumpedPawn(
        AFortPlayerPawnAthena* Victim,
        const FBumpDriver& Driver)
    {
        // The build offered no way to ask who is driving, so an empty seat
        // cannot be told apart from an occupied one.
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

    void ApplyBumpDamage(
        AFortPlayerPawnAthena* Pawn,
        AActor* Vehicle,
        AFortPlayerPawnAthena* Driver,
        double Damage)
    {
        if (Damage <= 0.0)
            return;

        if (Pawn->HasbCanBeDamaged() && !Pawn->bCanBeDamaged)
            return;

        if (AFortPlayerPawnAthena::HasFullHealthGodMode(Pawn))
            return;

        auto* DriverController = Driver && Driver->Controller
            ? Driver->Controller->Cast<AFortPlayerControllerAthena>()
            : nullptr;

        auto Remaining = (float)Damage;
        const auto Shield = Pawn->GetShield();

        if (Shield > 0.f)
        {
            const auto ShieldDamage = Shield < Remaining ? Shield : Remaining;
            Pawn->SetShield(Shield - ShieldDamage);
            Remaining -= ShieldDamage;
        }

        if (Remaining > 0.f)
        {
            const auto Health = Pawn->GetHealth();

            if (Health <= Remaining)
            {
                if (AFortPlayerPawnAthena::HasMinimumHealthGodMode(Pawn))
                {
                    Pawn->SetHealth(1.f);
                }
                else if (auto* ForceKillFunction = Pawn->GetFunction("ForceKill"))
                {
                    // The driver's controller is the killer so the elimination
                    // is credited and the feed reads correctly; a driverless
                    // vehicle passes null and the game attributes it to the
                    // world, which is what it is.
                    FGameplayTag DeathTag{};
                    Pawn->Call<void>(
                        ForceKillFunction, DeathTag, DriverController, Vehicle);
                }
                else
                {
                    Pawn->SetHealth(0.f);
                }
            }
            else
                Pawn->SetHealth(Health - Remaining);
        }
    }

    // Shared with the cannon launch below. The velocity overrides the pawn's own
    // on both axes rather than adding to it: for a bump, adding lets a player
    // sprinting into the grille cancel most of the hit, which reads as the bump
    // randomly not working; for a cannon, it keeps a moving cannon from eating
    // part of the shot.
    void LaunchPawnWithVelocity(AFortPlayerPawnAthena* Pawn, const FVector& LaunchVelocity)
    {
        if (auto* LaunchJump = Pawn->GetFunction("LaunchCharacterJump"))
        {
            Pawn->Call<void>(LaunchJump, LaunchVelocity, true, true, true, true);
            return;
        }

        // Pre-Athena naming. Same effect, without the jump-penalty handling.
        if (auto* Launch = Pawn->GetFunction("LaunchCharacter"))
            Pawn->Call<void>(Launch, LaunchVelocity, true, true);
    }

    // Shared by both entry points. Location and Velocity are whatever the caller
    // could establish: reported by the driving client, or measured from the
    // vehicle's own motion when nobody is driving it.
    void EvaluateBump(
        AActor* Vehicle,
        const FVector& Location,
        const FVector& Velocity,
        ULONGLONG CurrentTimeMs)
    {
        if (GBumpScanInProgress)
            return;

        const double Speed = Velocity.Magnitude();

        // Speed gates before anything else touches a pawn, so a parked or
        // crawling vehicle costs nothing beyond this comparison.
        double MinimumSpeedKmh = FConfiguration::VehicleBumpMinSpeedKmh;

        if (!std::isfinite(MinimumSpeedKmh) || MinimumSpeedKmh < 0.f)
            MinimumSpeedKmh = 0.f;

        if (!std::isfinite(Speed) ||
            Speed < MinimumSpeedKmh * BumpKmhToCentimetersPerSecond)
        {
            return;
        }

        // Direction of travel, flattened. A vehicle falling straight down should
        // not launch anyone sideways on the strength of its descent.
        const FVector TravelDirection =
            FVector(Velocity.X, Velocity.Y, 0.0).GetSafeNormal();

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

        // Looked up once per contact pass rather than per pawn: who is driving
        // decides damage for everyone this vehicle touches this frame.
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

            const double Along =
                (ToPawn.X * TravelDirection.X) + (ToPawn.Y * TravelDirection.Y);

            if (Along > BumpHullFront || Along < -BumpHullRear)
                continue;

            // Right vector for an up of (0,0,1) is (Forward.Y, -Forward.X, 0).
            const double Side =
                (ToPawn.X * TravelDirection.Y) - (ToPawn.Y * TravelDirection.X);

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

            const FVector LaunchVelocity =
                BuildBumpLaunchVelocity(TravelDirection, Speed, Tuning);

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
                SDK::DbgLog(
                    "[VehicleBump] %s hit %s at %.0f km/h -> launch %.0f %.0f %.0f "
                    "damage %.0f (fwd=%.2f up=%.2f cap=%.0f angle=%.1f driver=%s)\n",
                    Vehicle->Name.ToString().c_str(),
                    Pawn->Name.ToString().c_str(),
                    Speed / BumpKmhToCentimetersPerSecond,
                    (double)LaunchVelocity.X,
                    (double)LaunchVelocity.Y,
                    (double)LaunchVelocity.Z,
                    Damage,
                    Tuning.ForwardScale,
                    Tuning.VerticalScale,
                    Tuning.MaxSpeed,
                    Tuning.AngleAdjustmentDegrees,
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

void FortVehicleBump::OnVehicleMoved(
    AActor* Vehicle,
    const FVector& Location,
    const FVector& LinearVelocity)
{
    if (!FConfiguration::bVehicleBumpLaunch || !Vehicle)
        return;

    const ULONGLONG CurrentTimeMs = GetTickCount64();

    // Claim this vehicle for the RPC path. The tick path measures velocity from
    // position deltas, and a client-authored transform write is a teleport as
    // far as that measurement is concerned - it would read as a huge bogus
    // speed. Seeding the sample here means the tick path resumes cleanly from
    // the real position the moment the driver stops sending moves.
    auto& State = GetVehicleState(Vehicle);
    State.LastDriverMoveTimeMs = CurrentTimeMs;
    State.LastLocation = CopyVector(Location);
    State.LastSampleTimeMs = CurrentTimeMs;
    State.HasSample = true;

    EvaluateBump(Vehicle, Location, LinearVelocity, CurrentTimeMs);
}

void FortVehicleBump::Tick()
{
    if (!FConfiguration::bVehicleBumpLaunch)
        return;

    auto* World = UWorld::GetWorld();

    if (!World)
        return;

    const ULONGLONG CurrentTimeMs = GetTickCount64();

    if (CurrentTimeMs >= GBumpVehicleScanExpiryMs)
    {
        GBumpVehicleScanExpiryMs = CurrentTimeMs + BumpVehicleScanLifetimeMs;

        // FortAthenaVehicle covers cars, boats and the rest; FortPhysicsPawn is
        // the older base for builds that predate it. ClassifyVehicle still has
        // the final say on whether each result actually bumps.
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

    // Indexed rather than iterated: EvaluateBump calls into script, and anything
    // on that path that reached OnVehicleMoved would append to this vector and
    // invalidate an iterator. Indices survive the reallocation.
    for (size_t Index = 0; Index < GBumpVehicleStates.size();)
    {
        auto* Vehicle = GBumpVehicleStates[Index].Vehicle.Get();

        if (!Vehicle)
        {
            GBumpVehicleStates.erase(GBumpVehicleStates.begin() + Index);
            continue;
        }

        // A driven vehicle is handled by the RPC path, which has the client's
        // actual reported velocity instead of a two-sample estimate of it. The
        // grace period covers ordinary gaps between move RPCs so a driver with
        // a slow send rate does not get handled by both paths alternately.
        if (CurrentTimeMs - GBumpVehicleStates[Index].LastDriverMoveTimeMs <
            BumpDriverMoveGraceMs)
        {
            ++Index;
            continue;
        }

        // Reading the location costs a reflected call, so the sample interval
        // gates it. Most vehicles in a match are parked and this is all they
        // ever cost.
        const ULONGLONG ElapsedMs =
            CurrentTimeMs - GBumpVehicleStates[Index].LastSampleTimeMs;

        if (GBumpVehicleStates[Index].HasSample &&
            ElapsedMs < BumpMinSampleIntervalMs)
        {
            ++Index;
            continue;
        }

        const FVector Location = Vehicle->K2_GetActorLocation();

        // No previous sample, or one too old to divide by: a long gap (a server
        // hitch, or a vehicle that just came back from the RPC path) would turn
        // an ordinary displacement into a huge fake speed. Re-seed instead.
        if (!GBumpVehicleStates[Index].HasSample ||
            ElapsedMs > BumpMaxSampleIntervalMs)
        {
            GBumpVehicleStates[Index].LastLocation = CopyVector(Location);
            GBumpVehicleStates[Index].LastSampleTimeMs = CurrentTimeMs;
            GBumpVehicleStates[Index].HasSample = true;
            ++Index;
            continue;
        }

        const double ElapsedSeconds = (double)ElapsedMs * 0.001;
        const FVector Delta = Location - GBumpVehicleStates[Index].LastLocation;
        const FVector Velocity(
            Delta.X / ElapsedSeconds,
            Delta.Y / ElapsedSeconds,
            Delta.Z / ElapsedSeconds);

        GBumpVehicleStates[Index].LastLocation = CopyVector(Location);
        GBumpVehicleStates[Index].LastSampleTimeMs = CurrentTimeMs;

        // Faster than any vehicle travels, so this is a teleport rather than
        // motion - a respawn, or a level stream popping it into place.
        // Measuring speed from it would fling bystanders across the map.
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

    // The transform above is a teleport, so it produces no contacts and native
    // never sees the vehicle hit anyone. Run the bump test off the state the
    // driver just reported instead - Translation/LinearVelocity rather than the
    // actor, since only the root component was written and the rotation fed to
    // it has already been through the quantization fixups above.
    FortVehicleBump::OnVehicleMoved(Pawn, Translation, LinearVelocity);
}

void AFortOctopusVehicle::ServerUpdateTowhook(UObject* Context, FFrame& Stack)
{
    FVector InNetTowhookAimDir;

    Stack.StepCompiledIn(&InNetTowhookAimDir);
    Stack.IncrementCode();
    auto Vehicle = (AFortOctopusVehicle*)Context;

    printf("ServerUpdateTowhook\n");
    Vehicle->NetTowhookAimDir = InNetTowhookAimDir;
    Vehicle->OnRep_NetTowhookAimDir();
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
};

class AFortOctopusTowhookAttachableProjectile : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortOctopusTowhookAttachableProjectile);

    DEFINE_PROP(AttachedInfo, FAttachedInfo);
    DEFINE_PROP(OwningVehicle, AFortOctopusVehicle*);

    DEFINE_FUNC(Kill, void);
};

uint64_t CanGrappleToComponent_ = 0;
void (*OnRep_ReplicatedAttachedInfoOG)(AFortOctopusTowhookAttachableProjectile* _this);
void OnRep_ReplicatedAttachedInfo(AFortOctopusTowhookAttachableProjectile* _this)
{
    OnRep_ReplicatedAttachedInfoOG(_this);

    auto OwningVehicle = _this->OwningVehicle;
    if (!OwningVehicle)
        return;

    auto Comp = _this->AttachedInfo.Hit.Component.Get();
    /*if (!Comp)
    {
        _this->Kill();
        return;
    }*/

    auto CanGrappleToComponent = (bool(*)(AFortOctopusVehicle*, UActorComponent*))CanGrappleToComponent_;

    if (!CanGrappleToComponent(OwningVehicle, Comp))
    {
        // game automatically kills for us in OG
        return;
    }

    // the old projectile is supposed to die too, i dont know why it doesn't. i'll have to look at this more
    OwningVehicle->ReplicatedAttachState.Component = Comp;
    OwningVehicle->ReplicatedAttachState.LocalLocation = UKismetMathLibrary::InverseTransformLocation(Comp->K2_GetComponentToWorld(), _this->AttachedInfo.Hit.Location);
    OwningVehicle->ReplicatedAttachState.LocalNormal = UKismetMathLibrary::InverseTransformDirection(Comp->K2_GetComponentToWorld(), _this->AttachedInfo.Hit.Normal);
    OwningVehicle->OnRep_ReplicatedAttachState();

    printf("[Ballers] Comp: %s, Owner %s\n", Comp->Name.ToString().c_str(), Comp->GetOwner()->Name.ToString().c_str());
    printf("[Ballers] Location: %f %f %f [World] -> ", _this->AttachedInfo.Hit.Location.X, _this->AttachedInfo.Hit.Location.Y, _this->AttachedInfo.Hit.Location.Z);
    printf("%f %f %f [Local]\n", OwningVehicle->ReplicatedAttachState.LocalLocation.X, OwningVehicle->ReplicatedAttachState.LocalLocation.Y, OwningVehicle->ReplicatedAttachState.LocalLocation.Z);
    printf("[Ballers] Normal: %f %f %f [World] -> ", _this->AttachedInfo.Hit.Normal.X, _this->AttachedInfo.Hit.Normal.Y, _this->AttachedInfo.Hit.Normal.Z);
    printf("%f %f %f [Local]\n", OwningVehicle->ReplicatedAttachState.LocalNormal.X, OwningVehicle->ReplicatedAttachState.LocalNormal.Y, OwningVehicle->ReplicatedAttachState.LocalNormal.Z);
    //printf("CALLED!!!!\n");
}

// ---------------------------------------------------------------------------
// Mounted cannon -> player launch
//
// ServerFireActorInCannon belongs to the cannon *weapon* - the thing the
// operator holds while they are pushing the cannon around - and not to the
// cannon itself. So the exec hook's Context is a weapon actor, while every
// piece of state the launch needs (the seats, OnPreLaunchPawn/OnLaunchPawn, the
// multicast that plays the effect) lives one hop away on the host vehicle.
// Bridging that gap is most of the work here: weapon -> the pawn holding it ->
// the vehicle that pawn is riding.
//
// The other half is deciding who gets fired. The cannon pushes from seat 0 and
// launches out of the barrel, which is seat 1, so a rider in the barrel is the
// friend the operator is launching and an empty barrel means the operator is
// firing themselves. Both are the same RPC.
// ---------------------------------------------------------------------------
namespace
{
    // Seat 0 pushes and aims. A couple of indices past the barrel are scanned
    // as well, since nothing guarantees a build orders its seats the same way.
    constexpr int32 CannonMaxSeatIndex = 4;

    AFortPlayerPawnAthena* GetCannonPawnAtSeat(AActor* Vehicle, int32 SeatIndex)
    {
        if (!Vehicle || SeatIndex < 0)
            return nullptr;

        if (auto* GetPawnAtSeatFunction = Vehicle->GetFunction("GetPawnAtSeat"))
        {
            auto* Occupant = Vehicle->Call<AActor*>(GetPawnAtSeatFunction, SeatIndex);
            return Occupant ? Occupant->Cast<AFortPlayerPawnAthena>() : nullptr;
        }

        // Builds that do not reflect GetPawnAtSeat still keep the same slot
        // array on the seat component.
        auto* SeatComponent = (UFortVehicleSeatComponent*)Vehicle->GetComponentByClass(
            UFortVehicleSeatComponent::StaticClass());

        if (!SeatComponent || !SeatComponent->HasPlayerSlots() ||
            SeatIndex >= SeatComponent->PlayerSlots.Num())
        {
            return nullptr;
        }

        return SeatComponent->PlayerSlots
            .Get(SeatIndex, FAthenaCarPlayerSlot::Size()).Player;
    }

    // Identified by behaviour rather than by class name: OnLaunchPawn is what
    // actually throws a player out of a cannon, so probing for it keeps this
    // working on builds that call the class something other than
    // FortAthenaSKPushCannon.
    bool IsCannonVehicle(AActor* Vehicle)
    {
        return Vehicle && Vehicle->GetFunction("OnLaunchPawn") != nullptr;
    }

    // A weapon is owned by the pawn holding it, but the chain is walked rather
    // than assumed: some builds hang a mounted weapon off the controller, with
    // the pawn a further step up.
    AFortPlayerPawnAthena* GetWeaponHolder(AActor* Weapon)
    {
        AActor* Current = Weapon;

        for (int32 Step = 0; Current && Step < 4; ++Step)
        {
            if (auto* Pawn = Current->Cast<AFortPlayerPawnAthena>())
                return Pawn;

            if (auto* Controller = Current->Cast<AFortPlayerControllerAthena>())
            {
                return (Controller->HasMyFortPawn() && Controller->MyFortPawn)
                    ? Controller->MyFortPawn
                    : Controller->Pawn;
            }

            Current = Current->HasOwner() ? Current->Owner : nullptr;
        }

        return nullptr;
    }

    // Base speed of a direct cannon launch, before the per-axis multipliers.
    // Horizontal is deliberately not symmetric and vertical carries the most,
    // which is the shape a cannon shot has: mostly up, and further along the
    // barrel than across it.
    constexpr double CannonLaunchForwardSpeed = 6000.0;
    constexpr double CannonLaunchLateralSpeed = 5000.0;
    constexpr double CannonLaunchVerticalSpeed = 7500.0;

    double SanitizeCannonMultiplier(float Value)
    {
        return (std::isfinite(Value) && Value >= 0.f) ? (double)Value : 1.0;
    }

    // ServerOnExitVehicle takes a single enum on old builds and a 0x30-byte
    // struct from FN29 on, and it returns the vehicle that was left. Calling it
    // through the generated Call<AActor*>() with no arguments hands ProcessEvent
    // an 8-byte return slot as the entire parameter block, so native reads past
    // it and writes the return value somewhere further up this frame. Give it a
    // buffer the size the running build actually asks for instead.
    void ForcePawnOutOfCannon(AFortPlayerPawnAthena* Pawn)
    {
        auto* ExitFunction = Pawn ? Pawn->GetFunction("ServerOnExitVehicle") : nullptr;

        if (!ExitFunction)
            return;

        // Zeroed, which is exit behaviour "none" and no vehicle destruction -
        // what an ordinary dismount passes.
        alignas(16) std::array<uint8, 0x100> ExitParams{};

        if (ExitFunction->GetParamsNamed().Size > ExitParams.size())
            return;

        Pawn->ProcessEvent(ExitFunction, ExitParams.data());
    }
}

void AFortWeaponRangedMountedCannon::ServerFireActorInCannon(UObject* Context, FFrame& Stack)
{
    FVector LaunchDir{};

    // Read the signature rather than assuming it: stepping a parameter the
    // build does not declare consumes bytecode belonging to the caller. A build
    // that will not answer keeps the benefit of the doubt, since every one seen
    // so far sends the aim direction here.
    bool bHasLaunchDir = true;

    if (auto* FireFunction = Stack.GetCurrentNativeFunction())
    {
        bHasLaunchDir = false;

        for (const auto& Param : FireFunction->GetParamsNamed().NameOffsetMap)
        {
            // Return values are not on the wire.
            if ((Param.PropertyFlags & 0x400) == 0)
            {
                bHasLaunchDir = true;
                break;
            }
        }
    }

    if (bHasLaunchDir)
        Stack.StepCompiledIn(&LaunchDir);

    Stack.IncrementCode();

    auto* Weapon = (AActor*)Context;

    if (!Weapon)
        return;

    // Context is the cannon weapon, so the cannon has to be found through the
    // operator holding it. That association is the server's own, which is why
    // no actor named by the caller is trusted here.
    auto* Operator = GetWeaponHolder(Weapon);
    auto* Cannon = GetRiddenVehicle(Operator);

    if (!IsCannonVehicle(Cannon))
        return;

    // A rider in the barrel is the friend being launched; nobody in it means
    // the operator is launching themselves. Skipping the operator while
    // scanning covers the build that seats them in the barrel first, since that
    // falls through to the same answer.
    AFortPlayerPawnAthena* TargetPawn = nullptr;

    for (int32 SeatIndex = 1; SeatIndex <= CannonMaxSeatIndex && !TargetPawn; ++SeatIndex)
    {
        auto* Occupant = GetCannonPawnAtSeat(Cannon, SeatIndex);

        if (Occupant && Occupant != Operator)
            TargetPawn = Occupant;
    }

    if (!TargetPawn)
        TargetPawn = Operator;

    if (!TargetPawn)
        return;

    auto* PushCannon = (AFortAthenaSKPushCannon*)Cannon;

    PushCannon->OnPreLaunchPawn(TargetPawn, LaunchDir);
    ForcePawnOutOfCannon(TargetPawn);

    if (FConfiguration::bCannonLaunchAnimations)
    {
        // Native owns the whole launch: the animation, and the arc it decides
        // the aim direction deserves. The multipliers do not apply here because
        // there is nothing to multiply - the velocity never passes through us.
        PushCannon->OnLaunchPawn(TargetPawn, LaunchDir);
    }
    else
    {
        LaunchPawnWithVelocity(
            TargetPawn,
            FVector(
                LaunchDir.X * CannonLaunchForwardSpeed *
                    SanitizeCannonMultiplier(FConfiguration::CannonLaunchXMultiplier),
                LaunchDir.Y * CannonLaunchLateralSpeed *
                    SanitizeCannonMultiplier(FConfiguration::CannonLaunchYMultiplier),
                LaunchDir.Z * CannonLaunchVerticalSpeed *
                    SanitizeCannonMultiplier(FConfiguration::CannonLaunchZMultiplier)));
    }

    // Same hazard as the exit above: a no-argument Call<void>() passes
    // ProcessEvent a null parameter block, which a build whose multicast does
    // declare parameters would read through.
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
        //.ScanFor({ 0x48, 0x8B, 0xFF, 0xE8 }, false, 0, 1, 2048, true).RelativeOffset(4).Get();

        Utils::Hook<AFortOctopusTowhookAttachableProjectile>(OnRep_ReplicatedAttachedInfoIdx, OnRep_ReplicatedAttachedInfo, OnRep_ReplicatedAttachedInfoOG);
    }

    // The RPC hangs off the cannon weapon rather than off any vehicle, so this
    // resolves FortWeaponRangedMountedCannon. A build that names that class
    // anything else would install nothing and fail silently, so the RPC is also
    // looked up by name on its own - and either way the outcome is logged,
    // because "the cannon does nothing" and "the hook never went on" look
    // identical from in game.
    auto MountedCannonWeapon = AFortWeaponRangedMountedCannon::GetDefaultObj();
    auto FireActorInCannonFn = MountedCannonWeapon
        ? MountedCannonWeapon->GetFunction("ServerFireActorInCannon")
        : nullptr;

    if (!FireActorInCannonFn)
    {
        FireActorInCannonFn = (UFunction*)TUObjectArray::FindObject(
            "ServerFireActorInCannon", 0, FindClass("Function"));
    }

    if (FireActorInCannonFn)
    {
        Utils::ExecHook(
            FireActorInCannonFn,
            AFortWeaponRangedMountedCannon::ServerFireActorInCannon);
    }

    SDK::DbgLog(
        "[Cannon] ServerFireActorInCannon %s (class=%p)\n",
        FireActorInCannonFn ? "hooked" : "not present on this build",
        (void*)MountedCannonWeapon);
}
