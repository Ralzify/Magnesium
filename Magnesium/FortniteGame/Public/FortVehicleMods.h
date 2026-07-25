#pragma once

class AFortAthenaVehicle;

namespace SDK
{
    struct FTransform;
    class UClass;
    class UObject;
}

namespace FortVehicleMods
{
    // Uses FN30's deferred-spawn -> OnConstructVehicle -> finish-spawn path.
    // Returns null if the reflected native path is unavailable or rejected.
    AFortAthenaVehicle* SpawnVehicleWithConstruction(
        const SDK::UObject* VehicleSpawner,
        const SDK::UClass* VehicleClass,
        const SDK::FTransform& Transform);

    // Retains the spawner/forced-mod data and schedules one native construction
    // call outside the synchronous world-initialization loop.
    void RegisterSpawnedVehicle(
        AFortAthenaVehicle* Vehicle,
        const SDK::UObject* SpawnSource = nullptr);

    // Re-enters Fortnite's native Season 30 vehicle construction path once.
    // A mod event can use this as a fallback for an untracked spawn path.
    bool InitializeSpawnedVehicle(
        AFortAthenaVehicle* Vehicle,
        const SDK::UObject* SpawnSource = nullptr);

    // Re-enables the native finite-fuel configuration on raw-spawned FN30
    // vehicles. Returns false for vehicle types without a fuel component.
    bool InitializeFiniteFuel(
        AFortAthenaVehicle* Vehicle);

    // Amortizes registered vehicle construction at no more than one vehicle
    // every 100 ms.
    void TickPendingConstruction();

    // Adds event-driven fallbacks for vehicles spawned outside Magnesium's
    // known vehicle-spawner paths.
    void InstallHooks();
}
