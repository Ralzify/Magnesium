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
    AFortAthenaVehicle* SpawnVehicleWithConstruction(const SDK::UObject* VehicleSpawner,
        const SDK::UClass* VehicleClass, const SDK::FTransform& Transform);

    void RegisterSpawnedVehicle(AFortAthenaVehicle* Vehicle,
        const SDK::UObject* SpawnSource = nullptr);

    bool InitializeSpawnedVehicle(AFortAthenaVehicle* Vehicle,
        const SDK::UObject* SpawnSource = nullptr);

    bool InitializeFiniteFuel(AFortAthenaVehicle* Vehicle);

    void TickPendingConstruction();

    void InstallHooks();
}
