#pragma once
#include "../../pch.h"
#include "FortPhysicsPawn.h"

class AFortWeapon;

namespace FortVehicleSeatWeapons
{
    UFortWeaponItemDefinition* ResolveSeatWeaponDefinition(const FWeaponSeatDefinition& Definition);

    // Pre-9.00 vehicles carry no seat weapon component, so the weapon is looked up off the vehicle.
    UFortWeaponItemDefinition* ResolveLegacyVehicleWeapon(AActor* Vehicle, int32 SeatIndex);

    bool PublishGrantedVehicleWeapon(UFortVehicleSeatWeaponComponent* SeatWeaponComponent,
        FWeaponSeatDefinition& Definition, UFortWorldItem* VehicleItem, AFortWeapon* Weapon);
}
