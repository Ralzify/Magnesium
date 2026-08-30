#include "pch.h"
#include "../Public/FortVehicleSeatWeaponComponent.h"
#include "../Public/FortWeapon.h"

#include <algorithm>
#include <cctype>

namespace
{
    bool NameContains(const FName& Name, const char* Token)
    {
        if (!Name.IsValid())
            return false;

        auto Value = Name.ToString();
        std::transform(Value.begin(), Value.end(), Value.begin(), [](unsigned char Character)
            {
                return static_cast<char>(std::tolower(Character));
            });
        return Value.find(Token) != decltype(Value)::npos;
    }

    bool VehicleNameContains(AActor* Vehicle, const char* Token)
    {
        return NameContains(Vehicle->Name, Token) ||
            (Vehicle->Class && NameContains(Vehicle->Class->Name, Token));
    }

    const UFortWeaponItemDefinition* LoadWeaponDefinition(
        const UFortWeaponItemDefinition*& Cache, const wchar_t* Path)
    {
        if (!Cache)
            Cache = FindObject<UFortWeaponItemDefinition>(Path);
        return Cache;
    }

    struct FGrantedVehicleWeaponSlots
    {
        FWeakObjectPtr* Item = nullptr;
        FWeakObjectPtr* Weapon = nullptr;
    };

    bool ResolveGrantedVehicleWeaponSlots(FWeaponSeatDefinition& Definition,
        FGrantedVehicleWeaponSlots& OutSlots)
    {
        OutSlots = {};
        if (!FWeaponSeatDefinition::HasLastEquippedVehicleWeapon())
            return false;

        const int32 StructSize = FWeaponSeatDefinition::Size();
        const int32 BaseOffset = FWeaponSeatDefinition::LastEquippedVehicleWeapon__Offset;
        const int32 SlotSize = static_cast<int32>(sizeof(FWeakObjectPtr));
        const int32 Inc = VersionInfo.FortniteVersion >= 20 ? 4 : 0;
        const int32 ItemOffset = BaseOffset + 0x8 + Inc;
        const int32 WeaponOffset = BaseOffset + 0x10 + Inc;
        if (BaseOffset < 0 || StructSize <= 0 || WeaponOffset > StructSize - SlotSize)
            return false;

        auto Base = reinterpret_cast<uint8*>(&Definition);
        if (!SDK::MemReadable(Base + ItemOffset, SlotSize) ||
            !SDK::MemReadable(Base + WeaponOffset, SlotSize))
        {
            return false;
        }

        OutSlots.Item = reinterpret_cast<FWeakObjectPtr*>(Base + ItemOffset);
        OutSlots.Weapon = reinterpret_cast<FWeakObjectPtr*>(Base + WeaponOffset);
        return true;
    }
}

UFortWeaponItemDefinition* FortVehicleSeatWeapons::ResolveSeatWeaponDefinition(
    const FWeaponSeatDefinition& Definition)
{
    if (FWeaponSeatDefinition::HasVehicleWeaponOverride() && Definition.VehicleWeaponOverride)
        return Definition.VehicleWeaponOverride;

    return FWeaponSeatDefinition::HasVehicleWeapon() ? Definition.VehicleWeapon : nullptr;
}

UFortWeaponItemDefinition* FortVehicleSeatWeapons::ResolveLegacyVehicleWeapon(AActor* Vehicle,
    int32 SeatIndex)
{
    if (!Vehicle || !SDK::MemReadable(Vehicle, sizeof(UObject)))
        return nullptr;

    static const UFortWeaponItemDefinition* FerretWeapon = nullptr;
    static const UFortWeaponItemDefinition* OctopusWeapon = nullptr;
    static const UFortWeaponItemDefinition* CannonWeapon = nullptr;
    static const UFortWeaponItemDefinition* InCannonWeapon = nullptr;
    static const UFortWeaponItemDefinition* TurretWeapon = nullptr;

    const UFortWeaponItemDefinition* Weapon = nullptr;
    if (VehicleNameContains(Vehicle, "octopus"))
    {
        Weapon = LoadWeaponDefinition(OctopusWeapon,
            L"/Game/Athena/Items/Weapons/Vehicles/WID_Octopus_Weapon.WID_Octopus_Weapon");
    }
    else if (VehicleNameContains(Vehicle, "cannon"))
    {
        Weapon = SeatIndex == 1 ? LoadWeaponDefinition(InCannonWeapon,
                L"/Game/Athena/Items/Weapons/Vehicles/ShipCannon_Weapon_InCannon."
                L"ShipCannon_Weapon_InCannon") : LoadWeaponDefinition(CannonWeapon,
                L"/Game/Athena/Items/Weapons/Vehicles/ShipCannon_Weapon.ShipCannon_Weapon");
    }
    else if (VehicleNameContains(Vehicle, "mountedturret"))
    {
        Weapon = LoadWeaponDefinition(TurretWeapon,
            L"/Game/Athena/Items/Traps/MountedTurret/MountedTurret_Weapon.MountedTurret_Weapon");
    }
    else if (SeatIndex <= 0 && VehicleNameContains(Vehicle, "ferret"))
    {
        Weapon = LoadWeaponDefinition(FerretWeapon,
            L"/Game/Athena/Items/Weapons/Ferret_Weapon.Ferret_Weapon");
    }

    return const_cast<UFortWeaponItemDefinition*>(Weapon);
}

bool FortVehicleSeatWeapons::PublishGrantedVehicleWeapon(
    UFortVehicleSeatWeaponComponent* SeatWeaponComponent, FWeaponSeatDefinition& Definition,
    UFortWorldItem* VehicleItem, AFortWeapon* Weapon)
{
    if (!SeatWeaponComponent || !Weapon || !SDK::MemReadable(SeatWeaponComponent, sizeof(UObject)))
    {
        return false;
    }

    FGrantedVehicleWeaponSlots Slots{};
    const bool bWroteSlots = ResolveGrantedVehicleWeaponSlots(Definition, Slots);
    if (bWroteSlots)
    {
        *Slots.Item = FWeakObjectPtr(VehicleItem);
        *Slots.Weapon = FWeakObjectPtr(Weapon);
    }

    if (SeatWeaponComponent->HasCachedWeapon())
        SeatWeaponComponent->CachedWeapon = static_cast<AActor*>(Weapon);
    if (SeatWeaponComponent->HasCachedWeaponDef() && Weapon->HasWeaponData() && Weapon->WeaponData)
    {
        SeatWeaponComponent->CachedWeaponDef = Weapon->WeaponData;
    }
    if (SeatWeaponComponent->HasbWeaponEquipped())
        SeatWeaponComponent->bWeaponEquipped = true;

    auto SeatWeapon = ResolveSeatWeaponDefinition(Definition);
    if (SeatWeapon && FWeaponSeatDefinition::HasLastEquippedVehicleWeapon())
        Definition.LastEquippedVehicleWeapon = SeatWeapon;

    static uint32 GrantLogCount = 0;
    if (GrantLogCount++ < 32)
    {
        SDK::DbgLog("[VehicleWeapons] granted seat=%d weapon=%s item=%p "
            "granted-slots=%d cached=%d equipped=%d\n",
            FWeaponSeatDefinition::HasSeatIndex() ? Definition.SeatIndex : -1,
            Weapon->Name.ToString().c_str(), VehicleItem, bWroteSlots,
            SeatWeaponComponent->HasCachedWeapon(), SeatWeaponComponent->HasbWeaponEquipped());
    }

    return bWroteSlots;
}
