#include "pch.h"
#include "../Public/LateGame.h"
#include "../Public/Utils.h"
#include "../../FortniteGame/Public/FortInventory.h"
#include "../../FortniteGame/Public/FortPlayerControllerAthena.h"
#include "../Magnesium/Erbium/Public/Misc.h"
#include "../Public/Configuration.h"

#include <random>
#include <chrono>

TArray<TArray<TPair<FString, int>>> LateGame::GetLoadout()
{
    std::random_device rd;
    std::mt19937 rng(rd());

    TArray<TArray<TPair<FString, int>>> Slots;

    // Slot 1 (Assault Rifles)
    TArray<TPair<FString, int>> Slot1;

    Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Auto_Athena_R_Ore_T03.WID_Assault_Auto_Athena_R_Ore_T03"), 1));
    Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_AutoHigh_Athena_VR_Ore_T03.WID_Assault_AutoHigh_Athena_VR_Ore_T03"), 1));
    Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_AutoHigh_Athena_SR_Ore_T03.WID_Assault_AutoHigh_Athena_SR_Ore_T03"), 1));

    if (VersionInfo.FortniteVersion >= 2.50)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_LMG_Athena_VR_Ore_T03.WID_Assault_LMG_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_LMG_Athena_SR_Ore_T03.WID_Assault_LMG_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 5.40)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Suppressed_Athena_VR_Ore_T03.WID_Assault_Suppressed_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Suppressed_Athena_SR_Ore_T03.WID_Assault_Suppressed_Athena_SR_Ore_T03"), 1));
    }

    /*if (VersionInfo.FortniteVersion >= 8.40)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Infantry_Athena_VR.WID_Assault_Infantry_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Infantry_Athena_SR.WID_Assault_Infantry_Athena_SR"), 1));
    }*/

    if (VersionInfo.FortniteVersion >= 9.01)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_PistolCaliber_AR_Athena_R_Ore_T03.WID_Assault_PistolCaliber_AR_Athena_R_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_PistolCaliber_AR_Athena_VR_Ore_T03.WID_Assault_PistolCaliber_AR_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_PistolCaliber_AR_Athena_SR_Ore_T03.WID_Assault_PistolCaliber_AR_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 12.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_Adventure_AR.WID_Boss_Adventure_AR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_Hos_MG.WID_Boss_Hos_MG"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_Midas.WID_Boss_Midas"), 1));
    }

    if (VersionInfo.FortniteVersion >= 12.30 && VersionInfo.FortniteVersion < 13.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Donut.WID_Pistol_Donut"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 14)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/Tomato_Rifle/WID_Assault_Stark_Athena_R_Ore_T03.WID_Assault_Stark_Athena_R_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/Tomato_Rifle/WID_Assault_Stark_Athena_VR_Ore_T03.WID_Assault_Stark_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/Tomato_Rifle/WID_Assault_Stark_Athena_SR_Ore_T03.WID_Assault_Stark_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 15.20)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WaffleTruck/WID_WaffleTruck_HopRockDualies.WID_WaffleTruck_HopRockDualies"), 1));
    }

    if (VersionInfo.FortniteVersion >= 19.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/RedDotAR/WID_Assault_RedDotAR_Athena_R.WID_Assault_RedDotAR_Athena_R"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/RedDotAR/WID_Assault_RedDotAR_Athena_VR.WID_Assault_RedDotAR_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/RedDotAR/WID_Assault_RedDotAR_Athena_SR.WID_Assault_RedDotAR_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreAR/WID_Assault_CoreAR_Athena_SR.WID_Assault_CoreAR_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreAR/WID_Assault_CoreAR_Athena_VR.WID_Assault_CoreAR_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 20.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/RedDotBurst/WID_Assault_RedDotBurstAR_Athena_SR.WID_Assault_RedDotBurstAR_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/RedDotBurst/WID_Assault_RedDotBurstAR_Athena_VR.WID_Assault_RedDotBurstAR_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 21.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Assault/WID_Assault_Heavy_Recoil_Athena_SR.WID_Assault_Heavy_Recoil_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Assault/WID_Assault_Heavy_Recoil_Athena_VR.WID_Assault_Heavy_Recoil_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 23.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/MusterCoreWeapons/Items/Weapons/MusterScopedAR/WID_Assault_MusterScoped_Athena_SR.WID_Assault_MusterScoped_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/MusterCoreWeapons/Items/Weapons/MusterScopedAR/WID_Assault_MusterScoped_Athena_VR.WID_Assault_MusterScoped_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 24.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/RadicalCoreAR/WID_Assault_Radical_CoreAR_Athena_SR.WID_Assault_Radical_CoreAR_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/RadicalCoreAR/WID_Assault_Radical_CoreAR_Athena_VR.WID_Assault_Radical_CoreAR_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/PulseRifleMMObj/WID_Assault_PastaRipper_Athena_MMObj.WID_Assault_PastaRipper_Athena_MMObj"), 1));
    }

    if (VersionInfo.FortniteVersion >= 25.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/PanRifle/WID_Assault_Chrono_Pan_Rifle_Athena_SR.WID_Assault_Chrono_Pan_Rifle_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/PanRifle/WID_Assault_Chrono_Pan_Rifle_Athena_VR.WID_Assault_Chrono_Pan_Rifle_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 26.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/HopscotchWeaponsGameplay/Items/FlipmagAR/WID_Assault_FlipMag_Athena_SR.WID_Assault_FlipMag_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/HopscotchWeaponsGameplay/Items/FlipmagAR/WID_Assault_FlipMag_Athena_VR.WID_Assault_FlipMag_Athena_VR"), 1));
    }

    // will add ch5 ars once the damage is fixed

    Slots.Add(Slot1);

    TArray<TPair<FString, int>> Slot2;
    Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_C_Ore_T03.WID_Shotgun_Standard_Athena_C_Ore_T03"), 1));
    Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_UC_Ore_T03.WID_Shotgun_Standard_Athena_UC_Ore_T03"), 1));

    if (VersionInfo.FortniteVersion <= 8.51)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_SemiAuto_Athena_R_Ore_T03.WID_Shotgun_SemiAuto_Athena_R_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_SemiAuto_Athena_VR_Ore_T03.WID_Shotgun_SemiAuto_Athena_VR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 3.31)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_SlugFire_Athena_VR.WID_Shotgun_SlugFire_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_SlugFire_Athena_SR.WID_Shotgun_SlugFire_Athena_SR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 6.31)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_VR_Ore_T03.WID_Shotgun_Standard_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_SR_Ore_T03.WID_Shotgun_Standard_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 9.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Combat_Athena_R_Ore_T03.WID_Shotgun_Combat_Athena_R_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Combat_Athena_VR_Ore_T03.WID_Shotgun_Combat_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Combat_Athena_SR_Ore_T03.WID_Shotgun_Combat_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 9.40)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_HighSemiAuto_Athena_VR_Ore_T03.WID_Shotgun_HighSemiAuto_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_HighSemiAuto_Athena_SR_Ore_T03.WID_Shotgun_HighSemiAuto_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 15.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WaffleTruck/WID_WaffleTruck_Dub.WID_WaffleTruck_Dub"), 1));
    }

    if (VersionInfo.FortniteVersion >= 19.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/BurstShotgun/WID_Shotgun_CoreBurst_Athena_UC.WID_Shotgun_CoreBurst_Athena_UC"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/BurstShotgun/WID_Shotgun_CoreBurst_Athena_R.WID_Shotgun_CoreBurst_Athena_R"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/BurstShotgun/WID_Shotgun_CoreBurst_Athena_VR.WID_Shotgun_CoreBurst_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/BurstShotgun/WID_Shotgun_CoreBurst_Athena_SR.WID_Shotgun_CoreBurst_Athena_SR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 20.20)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/BreakActionShotgun/WID_Shotgun_Break_Action_Athena_SR_Ore_T03.WID_Shotgun_Break_Action_Athena_SR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/BreakActionShotgun/WID_Shotgun_Break_Action_Athena_VR_Ore_T03.WID_Shotgun_Break_Action_Athena_VR_Ore_T03"), 1));
    }
    if (VersionInfo.FortniteVersion >= 21.30)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Shotguns/OverLoadShotgun/WID_Shotgun_OverLoad_Athena_SR.WID_Shotgun_OverLoad_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Shotguns/OverLoadShotgun/WID_Shotgun_OverLoad_Athena_VR.WID_Shotgun_OverLoad_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 23.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/MusterCoreWeapons/Items/Weapons/MusterPumpShotgun/WID_Shotgun_MusterPump_Athena_SR.WID_Shotgun_MusterPump_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/MusterCoreWeapons/Items/Weapons/MusterPumpShotgun/WID_Shotgun_MusterPump_Athena_VR.WID_Shotgun_MusterPump_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 24.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/RadicalShotgunPump/WID_Shotgun_RadicalPump_Athena_UR.WID_Shotgun_RadicalPump_Athena_UR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/RadicalShotgunPump/WID_Shotgun_RadicalPump_Athena_SR.WID_Shotgun_RadicalPump_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/RadicalShotgunPump/WID_Shotgun_RadicalPump_Athena_VR.WID_Shotgun_RadicalPump_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 25.11)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/ChronoShotgun/WID_Shotgun_Chrono_Athena_SR.WID_Shotgun_Chrono_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/ChronoShotgun/WID_Shotgun_Chrono_Athena_VR.WID_Shotgun_Chrono_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 28.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_Pump/WID_Shotgun_Pump_Paprika_Athena_UR_Boss.WID_Shotgun_Pump_Paprika_Athena_UR_Boss"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_Pump/WID_Shotgun_Pump_Paprika_Athena_SR.WID_Shotgun_Pump_Paprika_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_Pump/WID_Shotgun_Pump_Paprika_Athena_VR.WID_Shotgun_Pump_Paprika_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_DPS/WID_Shotgun_Auto_Paprika_Athena_UR_Boss.WID_Shotgun_Auto_Paprika_Athena_UR_Boss"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_DPS/WID_Shotgun_Auto_Paprika_Athena_SR.WID_Shotgun_Auto_Paprika_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_DPS/WID_Shotgun_Auto_Paprika_Athena_VR.WID_Shotgun_Auto_Paprika_Athena_VR"), 1));
    }
    
    if (VersionInfo.FortniteVersion >= 29.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/CerberusSG/WID_Shotgun_Break_Cerberus_Athena_UR.WID_Shotgun_Break_Cerberus_Athena_UR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/CerberusSG/WID_Shotgun_Break_Cerberus_Athena_SR.WID_Shotgun_Break_Cerberus_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/CerberusSG/WID_Shotgun_Break_Cerberus_Athena_VR.WID_Shotgun_Break_Cerberus_Athena_VR"), 1));
    }

    Slots.Add(Slot2);
    // Slot 2 End

    // Slot 3 (Snipers)
    TArray<TPair<FString, int>> Slot3;
    Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_R_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_R_Ore_T03"), 1));
    Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_VR_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_VR_Ore_T03"), 1));
    Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03"), 1));

    if (VersionInfo.FortniteVersion >= 3.10)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_NoScope_Athena_UC_Ore_T03.WID_Sniper_NoScope_Athena_UC_Ore_T03"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_NoScope_Athena_R_Ore_T03.WID_Sniper_NoScope_Athena_R_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 5.21)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Heavy_Athena_VR_Ore_T03.WID_Sniper_Heavy_Athena_VR_Ore_T03"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Heavy_Athena_SR_Ore_T03.WID_Sniper_Heavy_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 7.10)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Suppressed_Scope_Athena_VR_Ore_T03.WID_Sniper_Suppressed_Scope_Athena_VR_Ore_T03"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Suppressed_Scope_Athena_SR_Ore_T03.WID_Sniper_Suppressed_Scope_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 13.00)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/LTM/WID_Sniper_NoScope_Athena_VR_Ore_T03.WID_Sniper_NoScope_Athena_VR_Ore_T03"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/LTM/WID_Sniper_NoScope_Athena_SR_Ore_T03.WID_Sniper_NoScope_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 15.10)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Cowboy_Athena_UC.WID_Sniper_Cowboy_Athena_UC"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Cowboy_Athena_R.WID_Sniper_Cowboy_Athena_R"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Cowboy_Athena_VR.WID_Sniper_Cowboy_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 17.40)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Suppressed_Scope_Athena_R_Ore_T03.WID_Sniper_Suppressed_Scope_Athena_R_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 19.00)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_UC.WID_Sniper_CoreSniper_Athena_UC"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_R.WID_Sniper_CoreSniper_Athena_R"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_VR.WID_Sniper_CoreSniper_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_SR.WID_Sniper_CoreSniper_Athena_SR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 20.10)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Heavy_Athena_R_Ore_T03.WID_Sniper_Heavy_Athena_R_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 21.00)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Heavy_Athena_UR_Ore_T03.WID_Sniper_Heavy_Athena_UR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 25.11)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/ExplosiveRepeater/WID_Sniper_ExplosiveRepeater_Athena_SR.WID_Sniper_ExplosiveRepeater_Athena_SR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/ExplosiveRepeater/WID_Sniper_ExplosiveRepeater_Athena_VR.WID_Sniper_ExplosiveRepeater_Athena_VR"), 1));
    }

    Slots.Add(Slot3);
    // Slot 3 End

    // Slot 4 (Heals)
    TArray<TPair<FString, int>> Slot4;
    Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Medkit/Athena_Medkit.Athena_Medkit"), 3));
    Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Shields/Athena_Shields.Athena_Shields"), 3));

	if (VersionInfo.FortniteVersion >= 1.80)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/PurpleStuff/Athena_PurpleStuff.Athena_PurpleStuff"), 2));
    }

    if (VersionInfo.FortniteVersion >= 1.11)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/ShieldSmall/Athena_ShieldSmall.Athena_ShieldSmall"), 6));
    }

    if (VersionInfo.FortniteVersion >= 2.30)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/SuperMedkit/Athena_SuperMedkit.Athena_SuperMedkit"), 1));
    }

    if (VersionInfo.FortniteVersion >= 9.30)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/ChillBronco/Athena_ChillBronco.Athena_ChillBronco"), 6));
    }

    if (VersionInfo.FortniteVersion >= 11.00)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Flopper/WID_Athena_Flopper.WID_Athena_Flopper"), 4));
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Flopper/Effective/WID_Athena_Flopper_Effective.WID_Athena_Flopper_Effective"), 3));
    }

    if (VersionInfo.FortniteVersion >= 13.00)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/BottomlessChugJug/WID_Athena_BottomlessChugJug.WID_Athena_BottomlessChugJug"), 1));
    }

    if (VersionInfo.FortniteVersion >= 14.00)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Flopper/HopFlopper/WID_Athena_Flopper_HopFlopper.WID_Athena_Flopper_HopFlopper"), 3));
    }

    if (VersionInfo.FortniteVersion >= 15.00)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Flopper/RiftFlopper/WID_Athena_Flopper_Rift.WID_Athena_Flopper_Rift"), 2));
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Flopper/ZeroFlopper/WID_Athena_Flopper_Zero.WID_Athena_Flopper_Zero"), 3));
    }

    if (VersionInfo.FortniteVersion >= 23.00)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/MusterConsumables/Items/EnergyDrink/Items/WID_Athena_EnergyDrink.WID_Athena_EnergyDrink"), 6));
    }

    Slots.Add(Slot4);
    // Slot 4 End

    // Slot 5 (Consumables)
    TArray<TPair<FString, int>> Slot5;

    Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Rocket_Athena_R_Ore_T03.WID_Launcher_Rocket_Athena_R_Ore_T03"), 1));
    Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Rocket_Athena_VR_Ore_T03.WID_Launcher_Rocket_Athena_VR_Ore_T03"), 1));
    Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Rocket_Athena_SR_Ore_T03.WID_Launcher_Rocket_Athena_SR_Ore_T03"), 1));
    Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Grenade_Athena_R_Ore_T03.WID_Launcher_Grenade_Athena_R_Ore_T03"), 1));
    Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Grenade_Athena_VR_Ore_T03.WID_Launcher_Grenade_Athena_VR_Ore_T03"), 1));
    Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Grenade_Athena_SR_Ore_T03.WID_Launcher_Grenade_Athena_SR_Ore_T03"), 1));
    Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/KnockGrenade/Athena_KnockGrenade.Athena_KnockGrenade"), 9));

    if (VersionInfo.FortniteVersion >= 3.4 && VersionInfo.FortniteVersion <= 7.10)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_RC_Rocket_Athena_VR_T03.WID_RC_Rocket_Athena_VR_T03"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_RC_Rocket_Athena_SR_T03.WID_RC_Rocket_Athena_SR_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 3.5)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/TowerGrenade/Athena_TowerGrenade.Athena_TowerGrenade"), 2));
    }

    if (VersionInfo.FortniteVersion >= 5.30)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/RiftItem/Athena_Rift_Item.Athena_Rift_Item"), 2));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/ShockwaveGrenade/Athena_ShockGrenade.Athena_ShockGrenade"), 6));
    }

    if (VersionInfo.FortniteVersion >= 5.40)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Hook_Gun_VR_Ore_T03.WID_Hook_Gun_VR_Ore_T03"), 1));
	}   

    if (VersionInfo.FortniteVersion >= 5.41)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/SuperTowerGrenade/Levels/PortAFort_A/Athena_SuperTowerGrenade_A.Athena_SuperTowerGrenade_A"), 1));
    }

    if (VersionInfo.FortniteVersion >= 6.02)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Military_Athena_VR_Ore_T03.WID_Launcher_Military_Athena_VR_Ore_T03"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Military_Athena_SR_Ore_T03.WID_Launcher_Military_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 7.30)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/IceGrenade/Athena_IceGrenade.Athena_IceGrenade"), 6));
    }

    if (VersionInfo.FortniteVersion >= 8.11)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Flintlock_Athena_C.WID_Pistol_Flintlock_Athena_C"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Flintlock_Athena_UC.WID_Pistol_Flintlock_Athena_UC"), 1));
    }

    if (VersionInfo.FortniteVersion >= 9.20)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/DeployableStorm/Athena_DogSweater.Athena_DogSweater"), 1));
    }

    if (VersionInfo.FortniteVersion >= 10.20)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/SilverBlazer/Athena_SilverBlazer_V2.Athena_SilverBlazer_V2"), 2));
    }

    if (VersionInfo.FortniteVersion >= 11.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Bucket/WID_Athena_Bucket_Old.WID_Athena_Bucket_Old"), 4));
    }

    if (VersionInfo.FortniteVersion >= 12.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_Adventure_GH.WID_Boss_Adventure_GH"), 1));
    }

    if (VersionInfo.FortniteVersion >= 12.30)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/AppleSun/WID_Athena_AppleSun.WID_Athena_AppleSun"), 6));
    }

    if (VersionInfo.FortniteVersion >= 13.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_GrapplingHoot.WID_Boss_GrapplingHoot"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 14)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/Repulsors/CoreBR/WID_HighTower_Tomato_Repulsor_CoreBR.WID_HighTower_Tomato_Repulsor_CoreBR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/RepulsorCannon/CoreBR/WID_HighTower_Tomato_RepulsorCannon_CoreBR.WID_HighTower_Tomato_RepulsorCannon_CoreBR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/HighTower/Items/Date/ChainLightning/CoreBR/WID_HighTower_Date_ChainLightning_CoreBR.WID_HighTower_Date_ChainLightning_CoreBR"), 1));
    }

    if (VersionInfo.FortniteVersion == 15.50 || VersionInfo.FortniteVersion == 17.30)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/LTM/Builder/Gameplay/Yeetknock_pistol/Builder_WID_YEETknock_UR.Builder_WID_YEETknock_UR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 17.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/Scooter/WID_Athena_Mother_Scooter.WID_Athena_Mother_Scooter"), 1));
    }

    if (VersionInfo.FortniteVersion >= 18.30)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/CorruptionGameplay/Gameplay/Items/Consumables/IcyGrapple/WID_Athena_IcyGrapple.WID_Athena_IcyGrapple"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 19)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/ParallelGameplay/Items/WestSausage/WID_WestSausage_Parallel.WID_WestSausage_Parallel"), 1));
    }

    if (VersionInfo.FortniteVersion >= 19.01)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/ShieldGenerator/WID_Athena_ShieldGenerator.WID_Athena_ShieldGenerator"), 2));
    }

    if (VersionInfo.FortniteVersion >= 21.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/GrappleGloves/Items/GrappleGloves/WID_GrappleGloves.WID_GrappleGloves"), 1));
    }

    if (VersionInfo.FortniteVersion >= 13.20)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Prototype/WID_FringePlank_Athena_Prototype.WID_FringePlank_Athena_Prototype"), 1));
    }

    if (VersionInfo.FortniteVersion >= 21.10)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Fringeplank_Firework/Items/WIDs/WID_Firework_Gun.WID_Firework_Gun"), 1));
    }

    if (VersionInfo.FortniteVersion >= 22.10)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/LaunchPadItemGameplay/Items/WID/WID_Athena_LaunchPadThrown.WID_Athena_LaunchPadThrown"), 2));
    }

    if (VersionInfo.FortniteVersion >= 23.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/ShockwaveMace/Items/WID_Muster_ShockwaveMace.WID_Muster_ShockwaveMace"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/ShockwaveMace/Items/WID_Muster_ShockwaveMace_Mythic.WID_Muster_ShockwaveMace_Mythic"), 1));
    }

    if (VersionInfo.FortniteVersion >= 24.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/KatanaGameplay/Items/Katana/WID_Melee_Katana.WID_Melee_Katana"), 1));
    }

    if (VersionInfo.FortniteVersion == 24.20)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/DryBox/Items/NyxGlass/AGID_NyxGlass.AGID_NyxGlass"), 1));
    }

    if (VersionInfo.FortniteVersion >= 26.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/HopscotchWeaponsGameplay/Items/Proto/WID_Athena_AppleSunSmall.WID_Athena_AppleSunSmall"), 6));
        Slot5.Add(TPair<FString, int>(TEXT("/RocketRamGameplay/Items/RocketRam/WID_RocketRam.WID_RocketRam"), 1));
    }

    if (VersionInfo.FortniteVersion >= 29.01)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/ShieldBubble/Gameplay/ShieldBubbleJr/Athena_SilverBlazer_Mini_UC.Athena_SilverBlazer_Mini_UC"), 4));
    }

    Slots.Add(Slot5);
    // Slot 5 End

    // Slot 6 (Traps)
    TArray<TPair<FString, int>> Traps;
    Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_Floor_Player_Launch_Pad_Athena.TID_Floor_Player_Launch_Pad_Athena"), 2));
    Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_Context_BouncePad_Athena.TID_Context_BouncePad_Athena"), 3));
    Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_ContextTrap_Athena.TID_ContextTrap_Athena"), 2));
    Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_Context_Reinforced_Athena.TID_Context_Reinforced_Athena"), 2));

    Slots.Add(Traps);
    // Slot 6 End

    // Slot 7 (Materials)
    std::uniform_int_distribution<int> Mats(186, 646);
    std::uniform_int_distribution<int> Gold(1200, 7500);

    TArray<TPair<FString, int>> Materials;
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/WoodItemData.WoodItemData"), Mats(rng)));
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/StoneItemData.StoneItemData"), Mats(rng)));
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/MetalItemData.MetalItemData"), Mats(rng)));

    if (VersionInfo.FortniteVersion >= 15)
    {
        Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/Athena_WadsItemData.Athena_WadsItemData"), Gold(rng)));
    }

    Slots.Add(Materials);
    // Slot 7 End

    // Slot 8 (Ammo)
    std::uniform_int_distribution<int> Heavy(50, 186);
    std::uniform_int_distribution<int> Shells(87, 576);
    std::uniform_int_distribution<int> Medium(124, 824);
    std::uniform_int_distribution<int> Light(186, 824);
    std::uniform_int_distribution<int> Rockets(3, 12);
    std::uniform_int_distribution<int> STW(186, 999);
    std::uniform_int_distribution<int> Arrows(12, 30);

    TArray<TPair<FString, int>> Ammo;
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsHeavy.AthenaAmmoDataBulletsHeavy"), Heavy(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataShells.AthenaAmmoDataShells"), Shells(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsMedium.AthenaAmmoDataBulletsMedium"), Medium(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsLight.AthenaAmmoDataBulletsLight"), Light(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AmmoDataRockets.AmmoDataRockets"), 12));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataExplosive.AmmoDataExplosive"), STW(rng))); // make these and below optional???
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataEnergyCell.AmmoDataEnergyCell"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataBulletsHeavy.AmmoDataBulletsHeavy"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataBulletsMedium.AmmoDataBulletsMedium"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataBulletsLight.AmmoDataBulletsLight"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataShells.AmmoDataShells"), STW(rng)));

    if (VersionInfo.FortniteVersion >= 16.00)
    {
        Ammo.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Ammo/AthenaAmmoDataArrows.AthenaAmmoDataArrows"), Arrows(rng)));
    }

    if (VersionInfo.FortniteVersion >= 17.00)
    {
        Ammo.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/Scooter/Ammo_Athena_Mother_Scooter.Ammo_Athena_Mother_Scooter"), 999));
    }

    Slots.Add(Ammo);
    // Slot 8 End

    return Slots;
}

TArray<TArray<TPair<FString, int>>> LateGame::GetVersionizedLoadout()
{
    std::random_device rd;
    std::mt19937 rng(rd());

    static std::mt19937 Randomization(std::random_device{}());
    std::uniform_int_distribution<int> Distribution(1, 5);

    TArray<TArray<TPair<FString, int>>> Slots;

    // Slot 1 (Assault Rifles)
    TArray<TPair<FString, int>> Slot1;

    if ((VersionInfo.FortniteVersion >= 1.6 && VersionInfo.FortniteVersion < 19.00) || (std::floor(VersionInfo.FortniteVersion) == 27))
    {
        int Roll = Distribution(Randomization);

        if (Roll == 1)
        {
            Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Auto_Athena_UC_Ore_T03.WID_Assault_Auto_Athena_UC_Ore_T03"), 1));
        }

        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Auto_Athena_R_Ore_T03.WID_Assault_Auto_Athena_R_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 1.6 && VersionInfo.FortniteVersion < 19.00) || (std::floor(VersionInfo.FortniteVersion) == 23) || (std::floor(VersionInfo.FortniteVersion) == 27) || (VersionInfo.FortniteVersion >= 28.00 /* temporary measures */))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_AutoHigh_Athena_VR_Ore_T03.WID_Assault_AutoHigh_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_AutoHigh_Athena_SR_Ore_T03.WID_Assault_AutoHigh_Athena_SR_Ore_T03"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion == 23))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_AutoHigh_Athena_UC_Ore_T03.WID_Assault_AutoHigh_Athena_UC_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_AutoHigh_Athena_R_Ore_T03.WID_Assault_AutoHigh_Athena_R_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 1.6 && VersionInfo.FortniteVersion < 7.10) || (VersionInfo.FortniteVersion == 9.30) || (VersionInfo.FortniteVersion >= 11.00 && VersionInfo.FortniteVersion < 15.00) || (VersionInfo.FortniteVersion >= 17.00 && VersionInfo.FortniteVersion < 19.00) || (VersionInfo.FortniteVersion >= 23.10 && VersionInfo.FortniteVersion < 24.00))
    {
        int Roll = Distribution(Randomization);

        if (Roll == 1)
        {
            Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_SemiAuto_Athena_UC_Ore_T03.WID_Assault_SemiAuto_Athena_UC_Ore_T03"), 1));
            Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_SemiAuto_Athena_R_Ore_T03.WID_Assault_SemiAuto_Athena_R_Ore_T03"), 1));
        }
	}

    if ((VersionInfo.FortniteVersion >= 4.2 && VersionInfo.FortniteVersion < 7.30) || (VersionInfo.FortniteVersion == 9.30) || (VersionInfo.FortniteVersion >= 11.00 && VersionInfo.FortniteVersion < 15.00) || (VersionInfo.FortniteVersion >= 17.00 && VersionInfo.FortniteVersion < 19.00) || (VersionInfo.FortniteVersion >= 23.10 && VersionInfo.FortniteVersion < 24.00))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_SemiAuto_Athena_VR_Ore_T03.WID_Assault_SemiAuto_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_SemiAuto_Athena_SR_Ore_T03.WID_Assault_SemiAuto_Athena_SR_Ore_T03"), 1));
	}

    if (VersionInfo.FortniteVersion >= 1.63 && VersionInfo.FortniteVersion < 11.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Surgical_Athena_R_Ore_T03.WID_Assault_Surgical_Athena_R_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Surgical_Athena_VR_Ore_T03.WID_Assault_Surgical_Athena_VR_Ore_T03"), 1));
	}

    if (VersionInfo.FortniteVersion >= 14.00 && VersionInfo.FortniteVersion < 14.20)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Surgical_Athena_UC_Ore_T03.WID_Assault_Surgical_Athena_UC_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Surgical_Athena_R_Ore_T03.WID_Assault_Surgical_Athena_R_Ore_T03"), 1));
    }

    if ((std::floor(VersionInfo.FortniteVersion) == 14) || (std::floor(VersionInfo.FortniteVersion) == 27))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Surgical_Athena_VR_Ore_T03.WID_Assault_Surgical_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Surgical_Athena_SR_Ore_T03.WID_Assault_Surgical_Athena_SR_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 2.50 && VersionInfo.FortniteVersion < 11.00) || (std::floor(VersionInfo.FortniteVersion) == 12) || (VersionInfo.FortniteVersion == 27.10 || VersionInfo.FortniteVersion == 27.11))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_LMG_Athena_VR_Ore_T03.WID_Assault_LMG_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_LMG_Athena_SR_Ore_T03.WID_Assault_LMG_Athena_SR_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 3.50 && VersionInfo.FortniteVersion < 6.00) || (VersionInfo.FortniteVersion == 9.30) || (std::floor(VersionInfo.FortniteVersion) == 14) || (VersionInfo.FortniteVersion == 27.00))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_LMGSAW_Athena_R_Ore_T03.WID_Assault_LMGSAW_Athena_R_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_LMGSAW_Athena_VR_Ore_T03.WID_Assault_LMGSAW_Athena_VR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 20.20 && VersionInfo.FortniteVersion < 21.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_LMGSAW_Athena_R_Ore_T03.WID_Assault_LMGSAW_Athena_R_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_LMGSAW_Athena_VR_Ore_T03.WID_Assault_LMGSAW_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_LMGSAW_Athena_SR_Ore_T03.WID_Assault_LMGSAW_Athena_SR_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 4.40 && VersionInfo.FortniteVersion < 7.20) || (VersionInfo.FortniteVersion >= 7.30 && VersionInfo.FortniteVersion < 9.00) || (VersionInfo.FortniteVersion == 9.30) || (std::floor(VersionInfo.FortniteVersion) == 20.00))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Surgical_Thermal_Athena_VR_Ore_T03.WID_Assault_Surgical_Thermal_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Surgical_Thermal_Athena_SR_Ore_T03.WID_Assault_Surgical_Thermal_Athena_SR_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 4.50 && VersionInfo.FortniteVersion < 5.40) || (VersionInfo.FortniteVersion >= 8.51 && VersionInfo.FortniteVersion < 10.20) || (VersionInfo.FortniteVersion == 11.31) || (VersionInfo.FortniteVersion == 27.11))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_AutoDrum_Athena_UC_Ore_T03.WID_Assault_AutoDrum_Athena_UC_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_AutoDrum_Athena_R_Ore_T03.WID_Assault_AutoDrum_Athena_R_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 5.40 && VersionInfo.FortniteVersion < 9.00) || (VersionInfo.FortniteVersion >= 10.20 && VersionInfo.FortniteVersion < 11.00) || (VersionInfo.FortniteVersion == 11.3) || (std::floor(VersionInfo.FortniteVersion) == 12) || (VersionInfo.FortniteVersion == 17.40) || (std::floor(VersionInfo.FortniteVersion) == 18) || (std::floor(VersionInfo.FortniteVersion) == 27))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Suppressed_Athena_VR_Ore_T03.WID_Assault_Suppressed_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Suppressed_Athena_SR_Ore_T03.WID_Assault_Suppressed_Athena_SR_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 6.22 && VersionInfo.FortniteVersion < 11.00) || (VersionInfo.FortniteVersion >= 11.40 && VersionInfo.FortniteVersion < 13.00) || (std::floor(VersionInfo.FortniteVersion) == 15) || (std::floor(VersionInfo.FortniteVersion) == 17) || (std::floor(VersionInfo.FortniteVersion) == 27))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Heavy_Athena_R_Ore_T03.WID_Assault_Heavy_Athena_R_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 6.22 && VersionInfo.FortniteVersion < 8.10) || (VersionInfo.FortniteVersion >= 11.40 && VersionInfo.FortniteVersion < 13.00) || (std::floor(VersionInfo.FortniteVersion) == 15) || (std::floor(VersionInfo.FortniteVersion) == 17) || (std::floor(VersionInfo.FortniteVersion) == 27))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Heavy_Athena_VR_Ore_T03.WID_Assault_Heavy_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Heavy_Athena_SR_Ore_T03.WID_Assault_Heavy_Athena_SR_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 7.40 && VersionInfo.FortniteVersion < 11.00) || (VersionInfo.FortniteVersion == 11.31) || (VersionInfo.FortniteVersion >= 16.30 && VersionInfo.FortniteVersion < 17.00) || (VersionInfo.FortniteVersion == 23.50) || (VersionInfo.FortniteVersion == 27.11))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Infantry_Athena_R.WID_Assault_Infantry_Athena_R"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 8.40 && VersionInfo.FortniteVersion < 11.00) || (VersionInfo.FortniteVersion == 11.3) || (VersionInfo.FortniteVersion >= 16.30 && VersionInfo.FortniteVersion < 17.00) || (VersionInfo.FortniteVersion == 23.50) || (VersionInfo.FortniteVersion == 27.11))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Infantry_Athena_VR.WID_Assault_Infantry_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_Infantry_Athena_SR.WID_Assault_Infantry_Athena_SR"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 9.01 && VersionInfo.FortniteVersion < 10.00) || (VersionInfo.FortniteVersion == 11.3) || (std::floor(VersionInfo.FortniteVersion) == 15) || (std::floor(VersionInfo.FortniteVersion) == 23))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_PistolCaliber_AR_Athena_R_Ore_T03.WID_Assault_PistolCaliber_AR_Athena_R_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_PistolCaliber_AR_Athena_VR_Ore_T03.WID_Assault_PistolCaliber_AR_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_PistolCaliber_AR_Athena_SR_Ore_T03.WID_Assault_PistolCaliber_AR_Athena_SR_Ore_T03"), 1));
    }

    // todo: add first order blaster rifle

    if (std::floor(VersionInfo.FortniteVersion) == 12)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_Adventure_AR.WID_Boss_Adventure_AR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_Hos_MG.WID_Boss_Hos_MG"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_Midas.WID_Boss_Midas"), 1));
    }

    if (VersionInfo.FortniteVersion >= 12.30 && VersionInfo.FortniteVersion < 13.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Donut.WID_Pistol_Donut"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 13)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_Midas.WID_Boss_Midas"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Assault_SemiAuto_Athena_UR_Ore_T03.WID_Assault_SemiAuto_Athena_UR_Ore_T03"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 14)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/Tomato_Rifle/WID_Assault_Stark_Athena_R_Ore_T03.WID_Assault_Stark_Athena_R_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/Tomato_Rifle/WID_Assault_Stark_Athena_VR_Ore_T03.WID_Assault_Stark_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/Tomato_Rifle/WID_Assault_Stark_Athena_SR_Ore_T03.WID_Assault_Stark_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion == 14.40)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_GhostMidas.WID_Boss_GhostMidas"), 1));
    }

    if ((VersionInfo.FortniteVersion == 11.3) || (std::floor(VersionInfo.FortniteVersion) == 15) || (VersionInfo.FortniteVersion == 20.30) || (VersionInfo.FortniteVersion >= 21.10 && VersionInfo.FortniteVersion < 22.00) || (VersionInfo.FortniteVersion == 22.30) || (VersionInfo.FortniteVersion == 29.40))
    {
        if (std::floor(VersionInfo.FortniteVersion) == 15)
        {
            Slot1.Add(TPair<FString, int>(TEXT("/CosmosGameplay/Items/CosmoAssaultRifle/WID_Cosmos_AR_Athena_UC.WID_Cosmos_AR_Athena_UC"), 1));
        }
        else
        {
            Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Prototype/Galileo_Bun/WID_Galileo_Bun_Athena_UC.WID_Galileo_Bun_Athena_UC"), 1));
        }
    }

    if ((VersionInfo.FortniteVersion >= 15.20 && VersionInfo.FortniteVersion < 17.00) || (VersionInfo.FortniteVersion == 19.00) || (VersionInfo.FortniteVersion == 23.40))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WaffleTruck/WID_WaffleTruck_HopRockDualies.WID_WaffleTruck_HopRockDualies"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 17)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/Exotics/PastaRipper/WID_Assault_PastaRipper_Athena_R_Ore_T03.WID_Assault_PastaRipper_Athena_R_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/Exotics/PastaRipper/WID_Assault_PastaRipper_Athena_VR_Ore_T03.WID_Assault_PastaRipper_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/Exotics/PastaRipper/WID_Assault_PastaRipper_Athena_SR_Ore_T03.WID_Assault_PastaRipper_Athena_SR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/Exotics/PastaRipper/WID_Assault_PastaRipper_Athena_Boss.WID_Assault_PastaRipper_Athena_Boss"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/LlamaRoaster/WID_Assault_LlamaRoaster_Boss.WID_Assault_LlamaRoaster_Boss"), 1));
    }

    if (VersionInfo.FortniteVersion >= 17.40 && VersionInfo.FortniteVersion < 18.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/Exotics/PastaRipper/WID_Assault_PastaRipper_E.WID_Assault_PastaRipper_E"), 1));
	}

    if ((std::floor(VersionInfo.FortniteVersion) == 18) || (VersionInfo.FortniteVersion == 20.40))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/CorruptionItems/Gameplay/Items/PowerUp/LMG/WID_Assault_LMG_Powerup_Athena_VR.WID_Assault_LMG_Powerup_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/CorruptionItems/Gameplay/Items/PowerUp/LMG/WID_Assault_LMG_Powerup_Athena_SR.WID_Assault_LMG_Powerup_Athena_SR"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 18.20 && VersionInfo.FortniteVersion < 19.00) || (VersionInfo.FortniteVersion == 20.00) || (VersionInfo.FortniteVersion >= 21.20 && VersionInfo.FortniteVersion < 22.00))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/CorruptionItems/Gameplay/Items/ar/WID_Assault_Recoil_Athena_R.WID_Assault_Recoil_Athena_R"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/CorruptionItems/Gameplay/Items/ar/WID_Assault_Recoil_Athena_VR.WID_Assault_Recoil_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/CorruptionItems/Gameplay/Items/ar/WID_Assault_Recoil_Athena_SR.WID_Assault_Recoil_Athena_SR"), 1));
	}

    if (VersionInfo.FortniteVersion >= 19 && VersionInfo.FortniteVersion < 23.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreAR/WID_Assault_CoreAR_Athena_SR.WID_Assault_CoreAR_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreAR/WID_Assault_CoreAR_Athena_VR.WID_Assault_CoreAR_Athena_VR"), 1));
    }

    if ((std::floor(VersionInfo.FortniteVersion) == 19) || (VersionInfo.FortniteVersion >= 20.00 && VersionInfo.FortniteVersion < 21.00))
    {
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/RedDotAR/WID_Assault_RedDotAR_Athena_R.WID_Assault_RedDotAR_Athena_R"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/RedDotAR/WID_Assault_RedDotAR_Athena_VR.WID_Assault_RedDotAR_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/RedDotAR/WID_Assault_RedDotAR_Athena_SR.WID_Assault_RedDotAR_Athena_SR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 20.00 && VersionInfo.FortniteVersion < 22.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/RedDotBurst/WID_Assault_RedDotBurstAR_Athena_SR.WID_Assault_RedDotBurstAR_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/RedDotBurst/WID_Assault_RedDotBurstAR_Athena_VR.WID_Assault_RedDotBurstAR_Athena_VR"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 22)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/DistortedWeaponsGameplay/Chrome/Assault/WMID_Assault_Chrome_Athena_R.WMID_Assault_Chrome_Athena_R"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/DistortedWeaponsGameplay/Chrome/Assault/WMID_Assault_Chrome_Athena_VR.WMID_Assault_Chrome_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/DistortedWeaponsGameplay/Chrome/Assault/WMID_Assault_Chrome_Athena_SR.WMID_Assault_Chrome_Athena_SR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 21.00 && VersionInfo.FortniteVersion < 23.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Assault/WID_Assault_Heavy_Recoil_Athena_SR.WID_Assault_Heavy_Recoil_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Assault/WID_Assault_Heavy_Recoil_Athena_VR.WID_Assault_Heavy_Recoil_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 23.00 && VersionInfo.FortniteVersion < 25.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/MusterCoreWeapons/Items/Weapons/MusterScopedAR/WID_Assault_MusterScoped_Athena_SR.WID_Assault_MusterScoped_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/MusterCoreWeapons/Items/Weapons/MusterScopedAR/WID_Assault_MusterScoped_Athena_VR.WID_Assault_MusterScoped_Athena_VR"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 24)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/PulseRifleMMObj/WID_Assault_PastaRipper_Athena_MMObj.WID_Assault_PastaRipper_Athena_MMObj"), 1));
    }

    if (VersionInfo.FortniteVersion >= 24.00 && VersionInfo.FortniteVersion < 27.00)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/RadicalCoreAR/WID_Assault_Radical_CoreAR_Athena_SR.WID_Assault_Radical_CoreAR_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/RadicalCoreAR/WID_Assault_Radical_CoreAR_Athena_VR.WID_Assault_Radical_CoreAR_Athena_VR"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 25)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/PanRifle/WID_Assault_Chrono_Pan_Rifle_Athena_SR.WID_Assault_Chrono_Pan_Rifle_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/PanRifle/WID_Assault_Chrono_Pan_Rifle_Athena_VR.WID_Assault_Chrono_Pan_Rifle_Athena_VR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/RedDotARS25/WID_Assault_Chrono_RedDotAR_Athena_SR.WID_Assault_Chrono_RedDotAR_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/RedDotARS25/WID_Assault_Chrono_RedDotAR_Athena_VR.WID_Assault_Chrono_RedDotAR_Athena_VR"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 26)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/HopscotchWeaponsGameplay/Items/FlipmagAR/WID_Assault_FlipMag_Athena_SR.WID_Assault_FlipMag_Athena_SR"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/HopscotchWeaponsGameplay/Items/FlipmagAR/WID_Assault_FlipMag_Athena_VR.WID_Assault_FlipMag_Athena_VR"), 1));
    }

    // will add ch5 ars once the damage is fixed

    Slots.Add(Slot1);

    TArray<TPair<FString, int>> Slot2;

    if ((VersionInfo.FortniteVersion >= 1.6 && VersionInfo.FortniteVersion < 9.00) || (VersionInfo.FortniteVersion >= 9.30 && VersionInfo.FortniteVersion < 13.00) || (std::floor(VersionInfo.FortniteVersion) == 14) || (VersionInfo.FortniteVersion >= 16.00 && VersionInfo.FortniteVersion < 19.00) || (std::floor(VersionInfo.FortniteVersion) == 27))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_C_Ore_T03.WID_Shotgun_Standard_Athena_C_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_UC_Ore_T03.WID_Shotgun_Standard_Athena_UC_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 6.31 && VersionInfo.FortniteVersion < 9.00) || (VersionInfo.FortniteVersion >= 9.30 && VersionInfo.FortniteVersion < 13.00) || (std::floor(VersionInfo.FortniteVersion) == 14) || (VersionInfo.FortniteVersion >= 16.00 && VersionInfo.FortniteVersion < 19.00) || (std::floor(VersionInfo.FortniteVersion) == 27))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_VR_Ore_T03.WID_Shotgun_Standard_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Standard_Athena_SR_Ore_T03.WID_Shotgun_Standard_Athena_SR_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 1.6 && VersionInfo.FortniteVersion < 14.00) || (std::floor(VersionInfo.FortniteVersion) == 15) || (VersionInfo.FortniteVersion >= 16.30 && VersionInfo.FortniteVersion < 18.00) || (std::floor(VersionInfo.FortniteVersion) == 27))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_SemiAuto_Athena_R_Ore_T03.WID_Shotgun_SemiAuto_Athena_R_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_SemiAuto_Athena_VR_Ore_T03.WID_Shotgun_SemiAuto_Athena_VR_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 3.31 && VersionInfo.FortniteVersion < 7.30) || (VersionInfo.FortniteVersion == 9.30))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_SlugFire_Athena_VR.WID_Shotgun_SlugFire_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_SlugFire_Athena_SR.WID_Shotgun_SlugFire_Athena_SR"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 5.20 && VersionInfo.FortniteVersion < 7.00) || (VersionInfo.FortniteVersion == 9.30) || (VersionInfo.FortniteVersion >= 10.00 && VersionInfo.FortniteVersion < 10.31) || (VersionInfo.FortniteVersion == 11.31) || (VersionInfo.FortniteVersion == 27.00))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_BreakBarrel_Athena_VR_Ore_T03.WID_Shotgun_BreakBarrel_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_BreakBarrel_Athena_SR_Ore_T03.WID_Shotgun_BreakBarrel_Athena_SR_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 9.00 && VersionInfo.FortniteVersion < 10.22) || (std::floor(VersionInfo.FortniteVersion) == 14) || (VersionInfo.FortniteVersion >= 18.30 && VersionInfo.FortniteVersion < 19.00) || (VersionInfo.FortniteVersion >= 23.00 && VersionInfo.FortniteVersion < 25.00) || (VersionInfo.FortniteVersion == 27.10))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Combat_Athena_R_Ore_T03.WID_Shotgun_Combat_Athena_R_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Combat_Athena_VR_Ore_T03.WID_Shotgun_Combat_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Combat_Athena_SR_Ore_T03.WID_Shotgun_Combat_Athena_SR_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 9.30 && VersionInfo.FortniteVersion < 10.22) || (VersionInfo.FortniteVersion == 11.31) || (std::floor(VersionInfo.FortniteVersion) == 20) || (std::floor(VersionInfo.FortniteVersion) == 25) || (VersionInfo.FortniteVersion == 27.11))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_AutoDrum_Athena_UC_Ore_T03.WID_Shotgun_AutoDrum_Athena_UC_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_AutoDrum_Athena_R_Ore_T03.WID_Shotgun_AutoDrum_Athena_R_Ore_T03"), 1));
    }

    if ((std::floor(VersionInfo.FortniteVersion) == 20) || (std::floor(VersionInfo.FortniteVersion) == 25))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_AutoDrum_Athena_VR_Ore_T03.WID_Shotgun_AutoDrum_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_AutoDrum_Athena_SR_Ore_T03.WID_Shotgun_AutoDrum_Athena_SR_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 9.40 && VersionInfo.FortniteVersion < 14.00) || (std::floor(VersionInfo.FortniteVersion) == 15) || (VersionInfo.FortniteVersion >= 16.30 && VersionInfo.FortniteVersion < 18.00) || (std::floor(VersionInfo.FortniteVersion) == 27))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_HighSemiAuto_Athena_VR_Ore_T03.WID_Shotgun_HighSemiAuto_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_HighSemiAuto_Athena_SR_Ore_T03.WID_Shotgun_HighSemiAuto_Athena_SR_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 13.00 && VersionInfo.FortniteVersion < 14.40) || (std::floor(VersionInfo.FortniteVersion)) == 15 || (std::floor(VersionInfo.FortniteVersion) == 18) || (VersionInfo.FortniteVersion >= 24.10 && VersionInfo.FortniteVersion < 25.00))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Charge_Athena_VR_Ore_T03.WID_Shotgun_Charge_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Charge_Athena_SR_Ore_T03.WID_Shotgun_Charge_Athena_SR_Ore_T03"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 13)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Charge_Athena_UR_Ore_T03.WID_Shotgun_Charge_Athena_UR_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 15.00  && VersionInfo.FortniteVersion < 16.40) || (VersionInfo.FortniteVersion >= 18.00 && VersionInfo.FortniteVersion < 23.40) || (VersionInfo.FortniteVersion == 23.50))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WaffleTruck/WID_WaffleTruck_Dub.WID_WaffleTruck_Dub"), 1));
    }

    if (VersionInfo.FortniteVersion >= 19.00 && VersionInfo.FortniteVersion < 22.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/BurstShotgun/WID_Shotgun_CoreBurst_Athena_UC.WID_Shotgun_CoreBurst_Athena_UC"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/BurstShotgun/WID_Shotgun_CoreBurst_Athena_R.WID_Shotgun_CoreBurst_Athena_R"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/BurstShotgun/WID_Shotgun_CoreBurst_Athena_VR.WID_Shotgun_CoreBurst_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/BurstShotgun/WID_Shotgun_CoreBurst_Athena_SR.WID_Shotgun_CoreBurst_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/DPSShotgun/WID_Shotgun_CoreDPS_Athena_VR.WID_Shotgun_CoreDPS_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/DPSShotgun/WID_Shotgun_CoreDPS_Athena_SR.WID_Shotgun_CoreDPS_Athena_SR"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 19.20 && VersionInfo.FortniteVersion < 20.00) || (VersionInfo.FortniteVersion >= 23.10 && VersionInfo.FortniteVersion < 24.00))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Heavy_Athena_R.WID_Shotgun_Heavy_Athena_R"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Heavy_Athena_VR.WID_Shotgun_Heavy_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Shotgun_Heavy_Athena_SR.WID_Shotgun_Heavy_Athena_SR"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 20.20 && VersionInfo.FortniteVersion < 21.00) || (VersionInfo.FortniteVersion >= 22.00 && VersionInfo.FortniteVersion < 23.00))
    {
        Slot2.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/BreakActionShotgun/WID_Shotgun_Break_Action_Athena_SR_Ore_T03.WID_Shotgun_Break_Action_Athena_SR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/BreakActionShotgun/WID_Shotgun_Break_Action_Athena_VR_Ore_T03.WID_Shotgun_Break_Action_Athena_VR_Ore_T03"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 21)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Shotguns/TwoShotShotgun/WID_Shotgun_TwoShot_Pump_Athena_VR.WID_Shotgun_TwoShot_Pump_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Shotguns/TwoShotShotgun/WID_Shotgun_TwoShot_Pump_Athena_SR.WID_Shotgun_TwoShot_Pump_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Shotguns/TwoShotShotgun/WID_Shotgun_TwoShot_Pump_Athena_UR.WID_Shotgun_TwoShot_Pump_Athena_UR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 21.30 && VersionInfo.FortniteVersion < 23.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Shotguns/OverLoadShotgun/WID_Shotgun_OverLoad_Athena_SR.WID_Shotgun_OverLoad_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Shotguns/OverLoadShotgun/WID_Shotgun_OverLoad_Athena_VR.WID_Shotgun_OverLoad_Athena_VR"), 1));

        if (VersionInfo.FortniteVersion >= 22.00)
        {
            Slot2.Add(TPair<FString, int>(TEXT("/DaisyWeaponGameplay/Items/Weapons/Shotguns/OverLoadShotgun/WID_Shotgun_OverLoad_Athena_UR.WID_Shotgun_OverLoad_Athena_UR"), 1));
        }
    }

    if (std::floor(VersionInfo.FortniteVersion) == 22)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/DistortedWeaponsGameplay/Chrome/Shotgun/WMID_Shotgun_Chrome_Athena_VR.WMID_Shotgun_Chrome_Athena_VR"), 1));
		Slot2.Add(TPair<FString, int>(TEXT("/DistortedWeaponsGameplay/Chrome/Shotgun/WMID_Shotgun_Chrome_Athena_SR.WMID_Shotgun_Chrome_Athena_SR"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 23)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/MusterCoreWeapons/Items/Weapons/MusterPumpShotgun/WID_Shotgun_MusterPump_Athena_SR.WID_Shotgun_MusterPump_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/MusterCoreWeapons/Items/Weapons/MusterPumpShotgun/WID_Shotgun_MusterPump_Athena_VR.WID_Shotgun_MusterPump_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 23.00 && VersionInfo.FortniteVersion < 27.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/MusterCoreWeapons/Items/Weapons/MusterDPSShotgun/WID_Shotgun_MusterDPS_Athena_SR.WID_Shotgun_MusterDPS_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/MusterCoreWeapons/Items/Weapons/MusterDPSShotgun/WID_Shotgun_MusterDPS_Athena_VR.WID_Shotgun_MusterDPS_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 24.00 && VersionInfo.FortniteVersion < 26.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/RadicalShotgunPump/WID_Shotgun_RadicalPump_Athena_UR.WID_Shotgun_RadicalPump_Athena_UR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/RadicalShotgunPump/WID_Shotgun_RadicalPump_Athena_SR.WID_Shotgun_RadicalPump_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/RadicalWeaponsGameplay/Weapons/RadicalShotgunPump/WID_Shotgun_RadicalPump_Athena_VR.WID_Shotgun_RadicalPump_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 25.11 && VersionInfo.FortniteVersion < 27.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/ChronoShotgun/WID_Shotgun_Chrono_Athena_SR.WID_Shotgun_Chrono_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/ChronoShotgun/WID_Shotgun_Chrono_Athena_VR.WID_Shotgun_Chrono_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/ChronoShotgun/WID_Shotgun_Chrono_Athena_R.WID_Shotgun_Chrono_Athena_R"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 26)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/HopscotchWeaponsGameplay/Items/HopscotchShotgun/WID_Shotgun_HopScotch_Athena_VR.WID_Shotgun_HopScotch_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/HopscotchWeaponsGameplay/Items/HopscotchShotgun/WID_Shotgun_HopScotch_Athena_SR.WID_Shotgun_HopScotch_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/HopscotchWeaponsGameplay/Items/HopscotchShotgun/WID_Shotgun_HopScotch_Athena_UR_MMO.WID_Shotgun_HopScotch_Athena_UR_MMO"), 1));
    }

    if (VersionInfo.FortniteVersion == 26.30)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/VampireStakeGameplay/Items/StakeLauncher/WID_VampireStake_Shotgun_R.WID_VampireStake_Shotgun_R"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/VampireStakeGameplay/Items/StakeLauncher/WID_VampireStake_Shotgun_VR.WID_VampireStake_Shotgun_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/VampireStakeGameplay/Items/StakeLauncher/WID_VampireStake_Shotgun_SR.WID_VampireStake_Shotgun_SR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 28.00 && VersionInfo.FortniteVersion < 32.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_Pump/WID_Shotgun_Pump_Paprika_Athena_SR.WID_Shotgun_Pump_Paprika_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_Pump/WID_Shotgun_Pump_Paprika_Athena_VR.WID_Shotgun_Pump_Paprika_Athena_VR"), 1));

        if (std::floor(VersionInfo.FortniteVersion) == 28)
        {
            Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_Pump/WID_Shotgun_Pump_Paprika_Athena_UR_Boss.WID_Shotgun_Pump_Paprika_Athena_UR_Boss"), 1));
		}
    }

    if (VersionInfo.FortniteVersion >= 28.00 && VersionInfo.FortniteVersion < 30.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_DPS/WID_Shotgun_Auto_Paprika_Athena_SR.WID_Shotgun_Auto_Paprika_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_DPS/WID_Shotgun_Auto_Paprika_Athena_VR.WID_Shotgun_Auto_Paprika_Athena_VR"), 1));

        if (std::floor(VersionInfo.FortniteVersion) == 28)
        {
            Slot2.Add(TPair<FString, int>(TEXT("/PaprikaCoreWeapons/Items/Weapons/PaprikaShotgun_DPS/WID_Shotgun_Auto_Paprika_Athena_UR_Boss.WID_Shotgun_Auto_Paprika_Athena_UR_Boss"), 1));
		}
    }

    if (VersionInfo.FortniteVersion >= 29.00 && VersionInfo.FortniteVersion < 32.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/CerberusSG/WID_Shotgun_Break_Cerberus_Athena_SR.WID_Shotgun_Break_Cerberus_Athena_SR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/CerberusSG/WID_Shotgun_Break_Cerberus_Athena_VR.WID_Shotgun_Break_Cerberus_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/CerberusSG/WID_Shotgun_Break_Cerberus_Athena_R.WID_Shotgun_Break_Cerberus_Athena_R"), 1));

        if (VersionInfo.FortniteVersion >= 29.00 && VersionInfo.FortniteVersion < 31.00)
        {
            Slot2.Add(TPair<FString, int>(TEXT("/SunRoseWeaponsGameplay/Items/Weapons/CerberusSG/WID_Shotgun_Break_Cerberus_Athena_UR.WID_Shotgun_Break_Cerberus_Athena_UR"), 1));
        }
    }

    Slots.Add(Slot2);
    // Slot 2 End

    // Slot 3 (Snipers)
    TArray<TPair<FString, int>> Slot3;

    if ((VersionInfo.FortniteVersion >= 1.6 && VersionInfo.FortniteVersion < 7.30) || (VersionInfo.FortniteVersion >= 9.30 && VersionInfo.FortniteVersion < 12.00) || (VersionInfo.FortniteVersion >= 13.00 && VersionInfo.FortniteVersion < 16.00) || (VersionInfo.FortniteVersion >= 17.00 && VersionInfo.FortniteVersion < 17.40) || (VersionInfo.FortniteVersion == 17.50) || (VersionInfo.FortniteVersion == 27.00) || (VersionInfo.FortniteVersion >= 28.00))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_R_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_R_Ore_T03"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_VR_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_VR_Ore_T03"), 1));

        if ((VersionInfo.FortniteVersion >= 1.6 && VersionInfo.FortniteVersion < 5.40) || (VersionInfo.FortniteVersion >= 9.30 && VersionInfo.FortniteVersion < 12.00) || (VersionInfo.FortniteVersion >= 13.00 && VersionInfo.FortniteVersion < 16.00) || (VersionInfo.FortniteVersion >= 17.00 && VersionInfo.FortniteVersion < 17.40) || (VersionInfo.FortniteVersion == 17.50) || (VersionInfo.FortniteVersion == 27.00) || (VersionInfo.FortniteVersion >= 28.00))
        {
            Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03"), 1));
        }
    }

    if ((VersionInfo.FortniteVersion >= 1.6 && VersionInfo.FortniteVersion < 6.21) || (VersionInfo.FortniteVersion >= 9.10 && VersionInfo.FortniteVersion < 10.00) || (VersionInfo.FortniteVersion == 27.00))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Standard_Scope_Athena_VR_Ore_T03.WID_Sniper_Standard_Scope_Athena_VR_Ore_T03"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Standard_Scope_Athena_SR_Ore_T03.WID_Sniper_Standard_Scope_Athena_SR_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 3.10 && VersionInfo.FortniteVersion < 9.20) || (VersionInfo.FortniteVersion >= 10.00 && VersionInfo.FortniteVersion < 11.00) || (VersionInfo.FortniteVersion == 11.31) || (std::floor(VersionInfo.FortniteVersion) == 13) || (VersionInfo.FortniteVersion == 20.30) || (VersionInfo.FortniteVersion >= 27.00 && VersionInfo.FortniteVersion < 27.11))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_NoScope_Athena_UC_Ore_T03.WID_Sniper_NoScope_Athena_UC_Ore_T03"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_NoScope_Athena_R_Ore_T03.WID_Sniper_NoScope_Athena_R_Ore_T03"), 1));
    }

    if ((std::floor(VersionInfo.FortniteVersion) == 13) || (VersionInfo.FortniteVersion == 20.30) || (VersionInfo.FortniteVersion >= 27.00 && VersionInfo.FortniteVersion < 27.11))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/LTM/WID_Sniper_NoScope_Athena_VR_Ore_T03.WID_Sniper_NoScope_Athena_VR_Ore_T03"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/LTM/WID_Sniper_NoScope_Athena_SR_Ore_T03.WID_Sniper_NoScope_Athena_SR_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 5.21 && VersionInfo.FortniteVersion < 11.00) || (VersionInfo.FortniteVersion == 11.31) || (std::floor(VersionInfo.FortniteVersion) == 12) || (VersionInfo.FortniteVersion >= 20.10 && VersionInfo.FortniteVersion < 22.00) || (VersionInfo.FortniteVersion == 23.40) || (VersionInfo.FortniteVersion >= 24.00 && VersionInfo.FortniteVersion < 25.11) || (VersionInfo.FortniteVersion == 27.11))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Heavy_Athena_VR_Ore_T03.WID_Sniper_Heavy_Athena_VR_Ore_T03"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Heavy_Athena_SR_Ore_T03.WID_Sniper_Heavy_Athena_SR_Ore_T03"), 1));

        if ((VersionInfo.FortniteVersion >= 20.10 && VersionInfo.FortniteVersion < 22.00) || (VersionInfo.FortniteVersion == 23.40) || (VersionInfo.FortniteVersion >= 24.00 && VersionInfo.FortniteVersion < 25.11))
        {
            Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Heavy_Athena_R_Ore_T03.WID_Sniper_Heavy_Athena_R_Ore_T03"), 1));
        }
    }

    if ((VersionInfo.FortniteVersion >= 7.10 && VersionInfo.FortniteVersion < 9.40) || (std::floor(VersionInfo.FortniteVersion) == 12) || (VersionInfo.FortniteVersion == 17.40) || (std::floor(VersionInfo.FortniteVersion) == 26) || (VersionInfo.FortniteVersion == 27.1))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Suppressed_Scope_Athena_VR_Ore_T03.WID_Sniper_Suppressed_Scope_Athena_VR_Ore_T03"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Suppressed_Scope_Athena_SR_Ore_T03.WID_Sniper_Suppressed_Scope_Athena_SR_Ore_T03"), 1));

        if (VersionInfo.FortniteVersion == 17.40)
        {
            Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Suppressed_Scope_Athena_R_Ore_T03.WID_Sniper_Suppressed_Scope_Athena_R_Ore_T03"), 1));
        }
    }

    if (std::floor(VersionInfo.FortniteVersion) == 16)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Bows/Shockwave/WID_Bow_Shockwave_Athena_VR.WID_Bow_Shockwave_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Bows/Shockwave/WID_Bow_Shockwave_Athena_SR.WID_Bow_Shockwave_Athena_SR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Bows/Metal/WID_Bow_Metal_Athena_R.WID_Bow_Metal_Athena_R"), 1));

        if (VersionInfo.FortniteVersion >= 16.20)
        {
            Slot3.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Exotics/GrapplerBow/WID_Athena_Bow_Grappler.WID_Athena_Bow_Grappler"), 1));
        }

        if (VersionInfo.FortniteVersion >= 16.30)
        {
            Slot3.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Exotics/UnstableBow/WMID_Athena_UnstableBow.WMID_Athena_UnstableBow"), 1));
        }
    }

    if (VersionInfo.FortniteVersion >= 9.41 && VersionInfo.FortniteVersion < 10.20)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Weather_Athena_VR.WID_Sniper_Weather_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Weather_Athena_SR.WID_Sniper_Weather_Athena_SR"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 15.00 && VersionInfo.FortniteVersion < 16.00) || (std::floor(VersionInfo.FortniteVersion) == 17) || (std::floor(VersionInfo.FortniteVersion) == 20))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WaffleTruck/WID_WaffleTruck_Sniper_StormScout.WID_WaffleTruck_Sniper_StormScout"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 15.10 && VersionInfo.FortniteVersion < 16.00) || (VersionInfo.FortniteVersion >= 18.00 && VersionInfo.FortniteVersion < 20.00) || (VersionInfo.FortniteVersion >= 24.00 && VersionInfo.FortniteVersion < 26))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WaffleTruck/WID_WaffleTruck_Sniper_DragonBreath.WID_WaffleTruck_Sniper_DragonBreath"), 1));
    }

    if (VersionInfo.FortniteVersion >= 15.10 && VersionInfo.FortniteVersion < 16.00)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Cowboy_Athena_UC.WID_Sniper_Cowboy_Athena_UC"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Cowboy_Athena_R.WID_Sniper_Cowboy_Athena_R"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Cowboy_Athena_VR.WID_Sniper_Cowboy_Athena_VR"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 19.00 && VersionInfo.FortniteVersion < 21.00) || (std::floor(VersionInfo.FortniteVersion) == 22))
    {
        Slot3.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_UC.WID_Sniper_CoreSniper_Athena_UC"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_R.WID_Sniper_CoreSniper_Athena_R"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_VR.WID_Sniper_CoreSniper_Athena_VR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_SR.WID_Sniper_CoreSniper_Athena_SR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 25.11 && VersionInfo.FortniteVersion < 26.00)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/ExplosiveRepeater/WID_Sniper_ExplosiveRepeater_Athena_SR.WID_Sniper_ExplosiveRepeater_Athena_SR"), 1));
        Slot3.Add(TPair<FString, int>(TEXT("/ChronoWeaponGameplay/Items/ExplosiveRepeater/WID_Sniper_ExplosiveRepeater_Athena_VR.WID_Sniper_ExplosiveRepeater_Athena_VR"), 1));
    }

    Slots.Add(Slot3);
    // Slot 3 End

    // Slot 4 (Heals)
    TArray<TPair<FString, int>> Slot4;

    if (VersionInfo.FortniteVersion >= 1.6 && VersionInfo.FortniteVersion < 28.00)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Medkit/Athena_Medkit.Athena_Medkit"), 3));
    }

    if (VersionInfo.FortniteVersion >= 1.6 && VersionInfo.FortniteVersion < 28.00)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Shields/Athena_Shields.Athena_Shields"), 3));
    }

    if ((VersionInfo.FortniteVersion >= 1.80 && VersionInfo.FortniteVersion < 11.00) || (VersionInfo.FortniteVersion == 23.30) || (VersionInfo.FortniteVersion >= 24.00 && VersionInfo.FortniteVersion < 27.00))
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/PurpleStuff/Athena_PurpleStuff.Athena_PurpleStuff"), 2));
    }

    if (VersionInfo.FortniteVersion >= 1.11 && VersionInfo.FortniteVersion < 28.00)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/ShieldSmall/Athena_ShieldSmall.Athena_ShieldSmall"), 6));
    }

    if ((VersionInfo.FortniteVersion >= 2.30 && VersionInfo.FortniteVersion < 11.00) || (std::floor(VersionInfo.FortniteVersion) == 27))
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/SuperMedkit/Athena_SuperMedkit.Athena_SuperMedkit"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 9.30 && VersionInfo.FortniteVersion < 11.00) || (VersionInfo.FortniteVersion >= 13.00 && VersionInfo.FortniteVersion < 16.00) || (VersionInfo.FortniteVersion >= 18.10 && VersionInfo.FortniteVersion < 25.00) || (VersionInfo.FortniteVersion >= 26.10 && VersionInfo.FortniteVersion < 27.00) || (VersionInfo.FortniteVersion == 27.11) || (std::floor(VersionInfo.FortniteVersion) == 29))
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/ChillBronco/Athena_ChillBronco.Athena_ChillBronco"), 6));
    }

    if ((VersionInfo.FortniteVersion >= 11.00 && VersionInfo.FortniteVersion < 27.00) || (VersionInfo.FortniteVersion >= 28.00))
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Flopper/WID_Athena_Flopper.WID_Athena_Flopper"), 4));
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Flopper/Effective/WID_Athena_Flopper_Effective.WID_Athena_Flopper_Effective"), 3));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 13)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/BottomlessChugJug/WID_Athena_BottomlessChugJug.WID_Athena_BottomlessChugJug"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 14.00 && VersionInfo.FortniteVersion < 15.00) || (VersionInfo.FortniteVersion >= 16.00 && VersionInfo.FortniteVersion < 18.00))
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Flopper/HopFlopper/WID_Athena_Flopper_HopFlopper.WID_Athena_Flopper_HopFlopper"), 3));
    }

    if (VersionInfo.FortniteVersion >= 15.00 && VersionInfo.FortniteVersion < 16.00)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Flopper/RiftFlopper/WID_Athena_Flopper_Rift.WID_Athena_Flopper_Rift"), 2));

        if (VersionInfo.FortniteVersion >= 15.10)
        {
            Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Flopper/ZeroFlopper/WID_Athena_Flopper_Zero.WID_Athena_Flopper_Zero"), 3));
        }
    }

    if (VersionInfo.FortniteVersion >= 19.00 && VersionInfo.FortniteVersion < 27.00)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/HealSpray/WID_Athena_HealSpray.WID_Athena_HealSpray"), 1));
    }

    if (VersionInfo.FortniteVersion >= 23.00 && VersionInfo.FortniteVersion < 26.10)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/MusterConsumables/Items/EnergyDrink/Items/WID_Athena_EnergyDrink.WID_Athena_EnergyDrink"), 6));
    }

    if (VersionInfo.FortniteVersion >= 28.00) // paprika items
    {
        Slot4.Add(TPair<FString, int>(TEXT("/PaprikaConsumables/Gameplay/BandageRefresh/WID_Paprika_Bandage.WID_Paprika_Bandage"), 15));
        Slot4.Add(TPair<FString, int>(TEXT("/PaprikaConsumables/Gameplay/ShieldSmallRefresh/WID_Paprika_ShieldSmall.WID_Paprika_ShieldSmall"), 6));
        Slot4.Add(TPair<FString, int>(TEXT("/PaprikaConsumables/Gameplay/ShieldRefresh/WID_Paprika_ShieldPot.WID_Paprika_ShieldPot"), 3));
        Slot4.Add(TPair<FString, int>(TEXT("/PaprikaConsumables/Gameplay/MedRefresh/WID_Paprika_MedBox.WID_Paprika_MedBox"), 3));
    }

    if (VersionInfo.FortniteVersion >= 28.01 && VersionInfo.FortniteVersion < 32.00)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/PaprikaConsumables/Gameplay/TeamSpray/WID_Paprika_TeamSpray_LowGrav.WID_Paprika_TeamSpray_LowGrav"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 30)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/DurableDawnTuna/Gameplay/WID_DurableDawnTuna.WID_DurableDawnTuna"), 2));
    }

    Slots.Add(Slot4);
    // Slot 4 End

    // Slot 5 (Consumables)
    TArray<TPair<FString, int>> Slot5;

    if ((VersionInfo.FortniteVersion >= 1.6 && VersionInfo.FortniteVersion < 11.10) || (VersionInfo.FortniteVersion >= 11.11 && VersionInfo.FortniteVersion < 12.00))
    {
        if (VersionInfo.FortniteVersion < 5.40 || VersionInfo.FortniteVersion >= 11.00)
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Rocket_Athena_R_Ore_T03.WID_Launcher_Rocket_Athena_R_Ore_T03"), 1));
        }

        if (VersionInfo.FortniteVersion >= 11.00)
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Rocket_Athena_UC_Ore_T03.WID_Launcher_Rocket_Athena_UC_Ore_T03"), 1));
        }
    }

    if ((VersionInfo.FortniteVersion >= 1.6 && VersionInfo.FortniteVersion < 11.10) || (VersionInfo.FortniteVersion >= 11.11 && VersionInfo.FortniteVersion < 14.40) || (VersionInfo.FortniteVersion >= 14.50 && VersionInfo.FortniteVersion < 18.21) || (VersionInfo.FortniteVersion >= 18.30 && VersionInfo.FortniteVersion < 19.00) || (std::floor(VersionInfo.FortniteVersion) == 23) || (std::floor(VersionInfo.FortniteVersion) == 27))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Rocket_Athena_VR_Ore_T03.WID_Launcher_Rocket_Athena_VR_Ore_T03"), 1));

        if (VersionInfo.FortniteVersion != 14.00 || VersionInfo.FortniteVersion != 14.20) // dude like why
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Rocket_Athena_SR_Ore_T03.WID_Launcher_Rocket_Athena_SR_Ore_T03"), 1));
        }
    }

    if ((VersionInfo.FortniteVersion == 11.10) || (VersionInfo.FortniteVersion == 14.40) || (VersionInfo.FortniteVersion == 18.21) || (VersionInfo.FortniteVersion == 22.20) || (VersionInfo.FortniteVersion == 26.30))
    {
        if (VersionInfo.FortniteVersion == 18.21)
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Pumpkin_Athena_VR_Ore_T03.WID_Launcher_Pumpkin_Athena_VR_Ore_T03"), 1));
        }
        else
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Pumpkin_Athena_R_Ore_T03.WID_Launcher_Pumpkin_Athena_R_Ore_T03"), 1));
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Pumpkin_Athena_VR_Ore_T03.WID_Launcher_Pumpkin_Athena_VR_Ore_T03"), 1));
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Pumpkin_Athena_SR_Ore_T03.WID_Launcher_Pumpkin_Athena_SR_Ore_T03"), 1));

            if (VersionInfo.FortniteVersion == 14.40)
            {
                Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Pumpkin_Athena_UC_Ore_T03.WID_Launcher_Pumpkin_Athena_UC_Ore_T03"), 1));
            }
        }
    }

    if ((VersionInfo.FortniteVersion >= 1.6 && VersionInfo.FortniteVersion < 11.00) || (VersionInfo.FortniteVersion >= 19.10 && VersionInfo.FortniteVersion < 20.00) || (std::floor(VersionInfo.FortniteVersion) == 27))
    {
        if (VersionInfo.FortniteVersion < 19.10 || (std::floor(VersionInfo.FortniteVersion) == 27))
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Grenade_Athena_R_Ore_T03.WID_Launcher_Grenade_Athena_R_Ore_T03"), 1));
        }

        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Grenade_Athena_VR_Ore_T03.WID_Launcher_Grenade_Athena_VR_Ore_T03"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Grenade_Athena_SR_Ore_T03.WID_Launcher_Grenade_Athena_SR_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion == 11.3) || (VersionInfo.FortniteVersion == 19.01) || (VersionInfo.FortniteVersion == 23.10) || (VersionInfo.FortniteVersion == 28.0))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Snowball_Athena_R_Ore_T03.WID_Launcher_Snowball_Athena_R_Ore_T03"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Snowball_Athena_VR_Ore_T03.WID_Launcher_Snowball_Athena_VR_Ore_T03"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Snowball_Athena_SR_Ore_T03.WID_Launcher_Snowball_Athena_SR_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion == 16.10) || (VersionInfo.FortniteVersion == 20.10) || (VersionInfo.FortniteVersion == 24.10))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Egg_Athena_R_Ore_T03.WID_Launcher_Egg_Athena_R_Ore_T03"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Egg_Athena_VR_Ore_T03.WID_Launcher_Egg_Athena_VR_Ore_T03"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Seasonal/WID_Launcher_Egg_Athena_SR_Ore_T03.WID_Launcher_Egg_Athena_SR_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 1.6 && VersionInfo.FortniteVersion < 7.40) || (VersionInfo.FortniteVersion >= 9.00 && VersionInfo.FortniteVersion < 26.00) || (std::floor(VersionInfo.FortniteVersion) == 27))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Grenade/Athena_Grenade.Athena_Grenade"), 6));
    }

    if ((VersionInfo.FortniteVersion >= 2.5 && VersionInfo.FortniteVersion < 6.00) || (VersionInfo.FortniteVersion >= 8.11 && VersionInfo.FortniteVersion < 9.30) || (VersionInfo.FortniteVersion >= 21.30 && VersionInfo.FortniteVersion < 22.00) || (std::floor(VersionInfo.FortniteVersion) == 23) || (VersionInfo.FortniteVersion == 27.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/KnockGrenade/Athena_KnockGrenade.Athena_KnockGrenade"), 9));
    }

    if ((VersionInfo.FortniteVersion >= 3.00 && VersionInfo.FortniteVersion < 11.00) || (VersionInfo.FortniteVersion == 11.31) || (VersionInfo.FortniteVersion == 15.40) || (std::floor(VersionInfo.FortniteVersion) == 17) || (VersionInfo.FortniteVersion == 23.40) || (std::floor(VersionInfo.FortniteVersion) == 27))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_HandCannon_Athena_VR_Ore_T03.WID_Pistol_HandCannon_Athena_VR_Ore_T03"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_HandCannon_Athena_SR_Ore_T03.WID_Pistol_HandCannon_Athena_SR_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion == 3.40) || (VersionInfo.FortniteVersion >= 5.10 && VersionInfo.FortniteVersion < 6.21))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_RC_Rocket_Athena_VR_T03.WID_RC_Rocket_Athena_VR_T03"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_RC_Rocket_Athena_SR_T03.WID_RC_Rocket_Athena_SR_T03"), 1));
    }

    static std::mt19937 FixedRNG(std::random_device{}());
    std::uniform_int_distribution<int> Dist(1, 100);

    if (VersionInfo.FortniteVersion >= 11.00 && VersionInfo.FortniteVersion < 19.00)
    {
        int Roll = Dist(FixedRNG);

        if (Roll == 1)
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Bucket/Nice/WID_Athena_Bucket_Nice.WID_Athena_Bucket_Nice"), 1));
        }
    }

    if ((VersionInfo.FortniteVersion >= 3.30 && VersionInfo.FortniteVersion < 6.00) || (std::floor(VersionInfo.FortniteVersion) == 12) || (std::floor(VersionInfo.FortniteVersion) == 20) || (VersionInfo.FortniteVersion >= 26 && VersionInfo.FortniteVersion < 28.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/C4/Athena_C4.Athena_C4"), 6));
    }

    if ((VersionInfo.FortniteVersion >= 1.11 && VersionInfo.FortniteVersion < 11.00) || (VersionInfo.FortniteVersion == 11.31) || (std::floor(VersionInfo.FortniteVersion) == 14) || (VersionInfo.FortniteVersion == 20.20) || (VersionInfo.FortniteVersion >= 21.30 && VersionInfo.FortniteVersion < 22.10) || (VersionInfo.FortniteVersion == 27.00) || (VersionInfo.FortniteVersion == 30.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/DanceGrenade/Athena_DanceGrenade.Athena_DanceGrenade"), 3));
    }

    if ((VersionInfo.FortniteVersion >= 3.5 && VersionInfo.FortniteVersion < 7.00) || (std::floor(VersionInfo.FortniteVersion) == 14) || (VersionInfo.FortniteVersion >= 21.20 && VersionInfo.FortniteVersion < 22.00) || (VersionInfo.FortniteVersion == 27.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/TowerGrenade/Athena_TowerGrenade.Athena_TowerGrenade"), 2));
    }

    if ((VersionInfo.FortniteVersion >= 5.30 && VersionInfo.FortniteVersion < 9.30) || (VersionInfo.FortniteVersion == 17.40) || (VersionInfo.FortniteVersion == 18.00) || (VersionInfo.FortniteVersion >= 20.20 && VersionInfo.FortniteVersion < 22.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/RiftItem/Athena_Rift_Item.Athena_Rift_Item"), 2));
    }

    if ((VersionInfo.FortniteVersion >= 5.30 && VersionInfo.FortniteVersion < 7.00) || (VersionInfo.FortniteVersion >= 9.00 && VersionInfo.FortniteVersion < 11.00) || (VersionInfo.FortniteVersion == 11.31) || (VersionInfo.FortniteVersion >= 14.00 && VersionInfo.FortniteVersion < 17.00) || (VersionInfo.FortniteVersion >= 20.00 && VersionInfo.FortniteVersion < 23.00) || (VersionInfo.FortniteVersion >= 26.00 && VersionInfo.FortniteVersion < 32.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/ShockwaveGrenade/Athena_ShockGrenade.Athena_ShockGrenade"), 6));
    }

    if ((VersionInfo.FortniteVersion >= 5.40 && VersionInfo.FortniteVersion < 7.20) || (VersionInfo.FortniteVersion >= 18.30 && VersionInfo.FortniteVersion < 19.00) || (std::floor(VersionInfo.FortniteVersion) == 27) || (VersionInfo.FortniteVersion == 28.01))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Hook_Gun_VR_Ore_T03.WID_Hook_Gun_VR_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 5.41 && VersionInfo.FortniteVersion < 7.20) || (VersionInfo.FortniteVersion == 27.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/SuperTowerGrenade/Levels/PortAFort_A/Athena_SuperTowerGrenade_A.Athena_SuperTowerGrenade_A"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 6.02 && VersionInfo.FortniteVersion < 7.20) || (VersionInfo.FortniteVersion == 9.30) || (VersionInfo.FortniteVersion == 27.10))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Military_Athena_VR_Ore_T03.WID_Launcher_Military_Athena_VR_Ore_T03"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Military_Athena_SR_Ore_T03.WID_Launcher_Military_Athena_SR_Ore_T03"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 6.20 && VersionInfo.FortniteVersion < 7.10) || (VersionInfo.FortniteVersion == 9.30) || (VersionInfo.FortniteVersion >= 10.00 && VersionInfo.FortniteVersion < 10.31) || (VersionInfo.FortniteVersion == 23.50) || (VersionInfo.FortniteVersion == 27.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Revolver_SingleAction_Athena_UC.WID_Pistol_Revolver_SingleAction_Athena_UC"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Revolver_SingleAction_Athena_R.WID_Pistol_Revolver_SingleAction_Athena_R"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Revolver_SingleAction_Athena_VR.WID_Pistol_Revolver_SingleAction_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 6.21 && VersionInfo.FortniteVersion < 7.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Balloons/Athena_Balloons.Athena_Balloons"), 20));
    }

    if ((VersionInfo.FortniteVersion >= 7.00 && VersionInfo.FortniteVersion < 9.00) || (VersionInfo.FortniteVersion >= 20.30 && VersionInfo.FortniteVersion < 21.00) || (VersionInfo.FortniteVersion == 22.00) || (VersionInfo.FortniteVersion >= 26.10 && VersionInfo.FortniteVersion < 26.30) || (VersionInfo.FortniteVersion == 27.10))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Balloons/Athena_Balloons_Consumable.Athena_Balloons_Consumable"), 10));
    }

    if ((VersionInfo.FortniteVersion >= 7.30 && VersionInfo.FortniteVersion < 8.00) || (VersionInfo.FortniteVersion == 19.01))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/IceGrenade/Athena_IceGrenade.Athena_IceGrenade"), 6));
    }

    if ((VersionInfo.FortniteVersion >= 8.11 && VersionInfo.FortniteVersion < 10.00) || (VersionInfo.FortniteVersion == 10.4) || (VersionInfo.FortniteVersion == 11.31) || (VersionInfo.FortniteVersion == 15.40) || (VersionInfo.FortniteVersion == 18.4) || (VersionInfo.FortniteVersion == 20.30) || (VersionInfo.FortniteVersion >= 24.10 && VersionInfo.FortniteVersion < 25.00) || (VersionInfo.FortniteVersion == 27.1))
    {
        if (VersionInfo.FortniteVersion >= 24.10 && VersionInfo.FortniteVersion < 25.00)
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Flintlock_Athena_UC.WID_Pistol_Flintlock_Athena_UC"), 1));
        }
        else
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Flintlock_Athena_C.WID_Pistol_Flintlock_Athena_C"), 1));
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Pistol_Flintlock_Athena_UC.WID_Pistol_Flintlock_Athena_UC"), 1));
        }
    }

    if ((VersionInfo.FortniteVersion >= 8.20 && VersionInfo.FortniteVersion < 9.30) || (VersionInfo.FortniteVersion == 11.31))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_ExplosiveBow_Athena_SR.WID_ExplosiveBow_Athena_SR"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 9.20 && VersionInfo.FortniteVersion < 10.20) || (VersionInfo.FortniteVersion == 27.11))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/DeployableStorm/Athena_DogSweater.Athena_DogSweater"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 10.20 && VersionInfo.FortniteVersion < 11.00) || (VersionInfo.FortniteVersion == 20.30) || (VersionInfo.FortniteVersion == 21.50) || (VersionInfo.FortniteVersion >= 22.10 && VersionInfo.FortniteVersion < 23.00) || (std::floor(VersionInfo.FortniteVersion) == 24))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/SilverBlazer/Athena_SilverBlazer_V2.Athena_SilverBlazer_V2"), 2));
    }

    if (VersionInfo.FortniteVersion == 10.40)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Badger_Grape_VR.WID_Badger_Grape_VR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Prototype/Badger_Bangs/WID_Athena_BadgerBangsNew.WID_Athena_BadgerBangsNew"), 10));
    }

    if (VersionInfo.FortniteVersion >= 11.00 && VersionInfo.FortniteVersion < 27.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Bucket/WID_Athena_Bucket_Old.WID_Athena_Bucket_Old"), 4));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 12)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_Adventure_GH.WID_Boss_Adventure_GH"), 1));
    }

    if (VersionInfo.FortniteVersion >= 12.30 && VersionInfo.FortniteVersion < 12.60)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Mantis/Items/UncleBrolly/WID_UncleBrolly.WID_UncleBrolly"), 1));
    }

    if ((VersionInfo.FortniteVersion == 11.3) || (VersionInfo.FortniteVersion == 12.50) || (VersionInfo.FortniteVersion == 20.30))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Gameplay/GalileoLobster/WID_Athena_Galileo_Lobster_Kayak.WID_Athena_Galileo_Lobster_Kayak"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Gameplay/GalileoLobster/WID_Athena_Galileo_Lobster_Limo.WID_Athena_Galileo_Lobster_Limo"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Gameplay/GalileoLobster/WID_Athena_Galileo_Lobster_Moped.WID_Athena_Galileo_Lobster_Moped"), 1));

        if (VersionInfo.FortniteVersion != 20.30)
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Gameplay/GalileoLobster/WID_Athena_Galileo_Lobster_Rocket.WID_Athena_Galileo_Lobster_Rocket"), 1));
        }
        else
        {
            Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Gameplay/GalileoLobster/WID_Athena_Galileo_Lobster_Noble.WID_Athena_Galileo_Lobster_Noble"), 1));
        }
    }

    if ((VersionInfo.FortniteVersion >= 12.30 && VersionInfo.FortniteVersion < 15.00) || (VersionInfo.FortniteVersion >= 21.30 && VersionInfo.FortniteVersion < 22.00) || (VersionInfo.FortniteVersion >= 22.20 && VersionInfo.FortniteVersion < 23.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/AppleSun/WID_Athena_AppleSun.WID_Athena_AppleSun"), 6));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 13)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_GrapplingHoot.WID_Boss_GrapplingHoot"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Shockwave_Athena_UR_Ore_T03.WID_Launcher_Shockwave_Athena_UR_Ore_T03"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 14)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/Repulsors/CoreBR/WID_HighTower_Tomato_Repulsor_CoreBR.WID_HighTower_Tomato_Repulsor_CoreBR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/HighTower/Items/Tomato/RepulsorCannon/CoreBR/WID_HighTower_Tomato_RepulsorCannon_CoreBR.WID_HighTower_Tomato_RepulsorCannon_CoreBR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/HighTower/Items/Date/ChainLightning/CoreBR/WID_HighTower_Date_ChainLightning_CoreBR.WID_HighTower_Date_ChainLightning_CoreBR"), 1));
    }

    if (VersionInfo.FortniteVersion == 15.50 || VersionInfo.FortniteVersion == 17.30)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/LTM/Builder/Gameplay/Yeetknock_pistol/Builder_WID_YEETknock_UR.Builder_WID_YEETknock_UR"), 1));
    }

    if ((VersionInfo.FortniteVersion == 19.30) || (VersionInfo.FortniteVersion >= 23.00 && VersionInfo.FortniteVersion < 23.40) || (VersionInfo.FortniteVersion == 23.50))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Bows/Shockwave/WID_Bow_Shockwave_Athena_VR.WID_Bow_Shockwave_Athena_VR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Bows/Shockwave/WID_Bow_Shockwave_Athena_SR.WID_Bow_Shockwave_Athena_SR"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 17.00 && VersionInfo.FortniteVersion < 19.00) || (VersionInfo.FortniteVersion == 20.40))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/Scooter/WID_Athena_Mother_Scooter.WID_Athena_Mother_Scooter"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 18)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/CorruptionGameplay/Gameplay/Items/Weapons/Melee/Scythe/WID_PowerUp_Scythe_VR.WID_PowerUp_Scythe_VR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/CorruptionGameplay/Gameplay/Items/Weapons/Melee/Scythe/WID_PowerUp_Scythe_SR.WID_PowerUp_Scythe_SR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/CorruptionGameplay/Gameplay/Items/Weapons/Melee/Scythe/WID_PowerUp_Scythe_UR.WID_PowerUp_Scythe_UR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Launcher_Shockwave_Athena_SR.WID_Launcher_Shockwave_Athena_SR"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 18.30 && VersionInfo.FortniteVersion < 19.00) || (VersionInfo.FortniteVersion == 28.01))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/CorruptionGameplay/Gameplay/Items/Consumables/IcyGrapple/WID_Athena_IcyGrapple.WID_Athena_IcyGrapple"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 19)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/ParallelGameplay/Items/WestSausage/WID_WestSausage_Parallel.WID_WestSausage_Parallel"), 1));
    }

    if (VersionInfo.FortniteVersion >= 19.01 && VersionInfo.FortniteVersion < 25.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/ShieldGenerator/WID_Athena_ShieldGenerator.WID_Athena_ShieldGenerator"), 2));
    }

    if ((std::floor(VersionInfo.FortniteVersion) == 20) || (VersionInfo.FortniteVersion >= 28.10 && VersionInfo.FortniteVersion < 29.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/HomingRocket/WIDs/WID_Launcher_HomingRocket_Athena_R.WID_Launcher_HomingRocket_Athena_R"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/HomingRocket/WIDs/WID_Launcher_HomingRocket_Athena_VR.WID_Launcher_HomingRocket_Athena_VR"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/ResolveGameplay/Items/Guns/HomingRocket/WIDs/WID_Launcher_HomingRocket_Athena_SR.WID_Launcher_HomingRocket_Athena_SR"), 1));
    }

    if ((std::floor(VersionInfo.FortniteVersion) == 21) || (VersionInfo.FortniteVersion == 22.10) || (VersionInfo.FortniteVersion >= 25.20 && VersionInfo.FortniteVersion < 26.00) || (std::floor(VersionInfo.FortniteVersion) == 27))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/GrappleGloves/Items/GrappleGloves/WID_GrappleGloves.WID_GrappleGloves"), 1));
    }

    if ((VersionInfo.FortniteVersion >= 13.20 && VersionInfo.FortniteVersion < 14.00) || (VersionInfo.FortniteVersion == 16.40) || (VersionInfo.FortniteVersion >= 19.01 && VersionInfo.FortniteVersion < 20.00) || (VersionInfo.FortniteVersion >= 24.30 && VersionInfo.FortniteVersion < 25.00) || (VersionInfo.FortniteVersion == 25.10))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Prototype/WID_FringePlank_Athena_Prototype.WID_FringePlank_Athena_Prototype"), 1));
    }

    if (VersionInfo.FortniteVersion >= 21.10 && VersionInfo.FortniteVersion < 22.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/SawbladeGun/Items/SawbladeGun/WID_Sawblade_Athena.WID_Sawblade_Athena"), 1));
    }

    if ((VersionInfo.FortniteVersion == 21.10) || (VersionInfo.FortniteVersion == 22.00) || (VersionInfo.FortniteVersion == 25.11))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Fringeplank_Firework/Items/WIDs/WID_Firework_Gun.WID_Firework_Gun"), 1));
    }

    if (VersionInfo.FortniteVersion >= 22.10 && VersionInfo.FortniteVersion < 23.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/UnstableLiquidGun/Items/UnstableLiquidGun/WID_NitroglycerineGun.WID_NitroglycerineGun"), 1));
    }

    if (VersionInfo.FortniteVersion >= 22.10 && VersionInfo.FortniteVersion < 24.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/LaunchPadItemGameplay/Items/WID/WID_Athena_LaunchPadThrown.WID_Athena_LaunchPadThrown"), 2));
    }

    if (VersionInfo.FortniteVersion == 22.40)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/GrappleGlider/Items/WID_GrappleGlider.WID_GrappleGlider"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 23)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Exotics/GrapplerBow/WID_Athena_Bow_Grappler.WID_Athena_Bow_Grappler"), 1));
    }

    if (VersionInfo.FortniteVersion >= 23.00 && VersionInfo.FortniteVersion < 23.50)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/ShockwaveMace/Items/WID_Muster_ShockwaveMace.WID_Muster_ShockwaveMace"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/ShockwaveMace/Items/WID_Muster_ShockwaveMace_Mythic.WID_Muster_ShockwaveMace_Mythic"), 1));
    }

    if (VersionInfo.FortniteVersion >= 24.00 && VersionInfo.FortniteVersion < 24.40)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/KatanaGameplay/Items/Katana/WID_Melee_Katana.WID_Melee_Katana"), 1));
    }

    if (VersionInfo.FortniteVersion == 24.20)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/DryBox/Items/NyxGlass/AGID_NyxGlass.AGID_NyxGlass"), 1));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 25)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/AncientWeapon/Gameplay/WID_Chrono_AncientWeapon.WID_Chrono_AncientWeapon"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/AloeCroutonWeapon/Items/AloeCrouton/AGID_AloeCrouton_Launcher_Athena.AGID_AloeCrouton_Launcher_Athena"), 1));
    }

    if (VersionInfo.FortniteVersion >= 25.10 && VersionInfo.FortniteVersion < 26.00)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/ChronoCloak/Cloak/WID_Chrono_Cloak.WID_Chrono_Cloak"), 1));
        Slot5.Add(TPair<FString, int>(TEXT("/ChronoCloak/Cloak/WID_Chrono_Cloak_UR_JungBoss.WID_Chrono_Cloak_UR_JungBoss"), 1));
    }

    if ((std::floor(VersionInfo.FortniteVersion) == 26) || (VersionInfo.FortniteVersion >= 28.01 && VersionInfo.FortniteVersion < 29.00))
    {
        Slot5.Add(TPair<FString, int>(TEXT("/HopscotchWeaponsGameplay/Items/Proto/WID_Athena_AppleSunSmall.WID_Athena_AppleSunSmall"), 6));
    }

    if (std::floor(VersionInfo.FortniteVersion) == 26)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/RocketRamGameplay/Items/RocketRam/WID_RocketRam.WID_RocketRam"), 1));
    }

    if (VersionInfo.FortniteVersion == 26.30)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/VampireKatanaGameplay/Items/VampireKatana/WID_Melee_Vampire_Katana.WID_Melee_Vampire_Katana"), 1));
    }

    if (VersionInfo.FortniteVersion >= 29.01)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/ShieldBubble/Gameplay/ShieldBubbleJr/Athena_SilverBlazer_Mini_UC.Athena_SilverBlazer_Mini_UC"), 4));
    }

    Slots.Add(Slot5);
    // Slot 5 End

    // Slot 6 (Traps)
    TArray<TPair<FString, int>> Traps;

    if ((VersionInfo.FortniteVersion >= 1.9 && VersionInfo.FortniteVersion < 11.00) || (VersionInfo.FortniteVersion >= 11.50 && VersionInfo.FortniteVersion < 14.00) || (VersionInfo.FortniteVersion >= 17.00 && VersionInfo.FortniteVersion < 21.00) || (std::floor(VersionInfo.FortniteVersion) == 27))
    {
        Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_Floor_Player_Launch_Pad_Athena.TID_Floor_Player_Launch_Pad_Athena"), 2));
    }

    if ((VersionInfo.FortniteVersion >= 4.30 && VersionInfo.FortniteVersion < 6.00) || (VersionInfo.FortniteVersion == 10.40) || (VersionInfo.FortniteVersion >= 14.00 && VersionInfo.FortniteVersion < 16.00) || (VersionInfo.FortniteVersion == 16.40) || (VersionInfo.FortniteVersion == 17.40) || (VersionInfo.FortniteVersion == 19.40) || (std::floor(VersionInfo.FortniteVersion) == 27))
    {
        Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_Context_BouncePad_Athena.TID_Context_BouncePad_Athena"), 3));
    }

    if ((VersionInfo.FortniteVersion >= 1.6 && VersionInfo.FortniteVersion < 12.00) || (std::floor(VersionInfo.FortniteVersion) == 27))
    {
        Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_ContextTrap_Athena.TID_ContextTrap_Athena"), 2));
    }

    if (VersionInfo.FortniteVersion >= 18.00 && VersionInfo.FortniteVersion < 23.00)
    {
        Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_Context_Reinforced_Athena.TID_Context_Reinforced_Athena"), 2));
    }

    Slots.Add(Traps);
    // Slot 6 End

    // Slot 7 (Materials)
    std::uniform_int_distribution<int> Mats(186, 646);
    std::uniform_int_distribution<int> Gold(1200, 7500);

    TArray<TPair<FString, int>> Materials;
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/WoodItemData.WoodItemData"), Mats(rng)));
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/StoneItemData.StoneItemData"), Mats(rng)));
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/MetalItemData.MetalItemData"), Mats(rng)));

    if (VersionInfo.FortniteVersion >= 15)
    {
        Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/Athena_WadsItemData.Athena_WadsItemData"), Gold(rng)));
    }

    Slots.Add(Materials);
    // Slot 7 End

    // Slot 8 (Ammo)
    std::uniform_int_distribution<int> Heavy(50, 186);
    std::uniform_int_distribution<int> Shells(87, 576);
    std::uniform_int_distribution<int> Medium(124, 824);
    std::uniform_int_distribution<int> Light(186, 824);
    std::uniform_int_distribution<int> Rockets(3, 12);
    std::uniform_int_distribution<int> STW(186, 999);
    std::uniform_int_distribution<int> Arrows(12, 30);

    TArray<TPair<FString, int>> Ammo;
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsHeavy.AthenaAmmoDataBulletsHeavy"), Heavy(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataShells.AthenaAmmoDataShells"), Shells(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsMedium.AthenaAmmoDataBulletsMedium"), Medium(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsLight.AthenaAmmoDataBulletsLight"), Light(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AmmoDataRockets.AmmoDataRockets"), 12));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataExplosive.AmmoDataExplosive"), STW(rng))); // make these and below optional???
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataEnergyCell.AmmoDataEnergyCell"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataBulletsHeavy.AmmoDataBulletsHeavy"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataBulletsMedium.AmmoDataBulletsMedium"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataBulletsLight.AmmoDataBulletsLight"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataShells.AmmoDataShells"), STW(rng)));

    if (VersionInfo.FortniteVersion >= 16.00)
    {
        Ammo.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Ammo/AthenaAmmoDataArrows.AthenaAmmoDataArrows"), Arrows(rng)));
    }

    if (VersionInfo.FortniteVersion >= 17.00)
    {
        Ammo.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/Scooter/Ammo_Athena_Mother_Scooter.Ammo_Athena_Mother_Scooter"), 999));
    }

    Slots.Add(Ammo);
    // Slot 8 End

    return Slots;
}

TArray<TArray<TPair<FString, int>>> LateGame::GetOSLoadout()
{
    std::random_device rd;
    std::mt19937 rng(rd());

    TArray<TArray<TPair<FString, int>>> Slots;

    // Slot 1 (Huntings)
    TArray<TPair<FString, int>> Slot1;
    Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_NoScope_Athena_UC_Ore_T03.WID_Sniper_NoScope_Athena_UC_Ore_T03"), 1));
    Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_NoScope_Athena_R_Ore_T03.WID_Sniper_NoScope_Athena_R_Ore_T03"), 1));

    if (VersionInfo.FortniteVersion >= 12)
    {
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/LTM/WID_Sniper_NoScope_Athena_VR_Ore_T03.WID_Sniper_NoScope_Athena_VR_Ore_T03"), 1));
        Slot1.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/LTM/WID_Sniper_NoScope_Athena_SR_Ore_T03.WID_Sniper_NoScope_Athena_SR_Ore_T03"), 1));
    }

    Slots.Add(Slot1);
    // Slot 1 End

    // Slot 2 (No-Scope Sniper)
    TArray<TPair<FString, int>> Slot2;
    Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_R_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_R_Ore_T03"), 1));
    Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_VR_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_VR_Ore_T03"), 1));
    Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03"), 1));

    if (VersionInfo.FortniteVersion >= 5.21)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Heavy_Athena_VR_Ore_T03.WID_Sniper_Heavy_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Heavy_Athena_SR_Ore_T03.WID_Sniper_Heavy_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 7.10)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Suppressed_Scope_Athena_VR_Ore_T03.WID_Sniper_Suppressed_Scope_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Suppressed_Scope_Athena_SR_Ore_T03.WID_Sniper_Suppressed_Scope_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 11.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_BoltAction_Scope_Athena_UC_Ore_T03.WID_Sniper_BoltAction_Scope_Athena_UC_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 12.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/LTM/WID_Sniper_NoScope_Athena_VR_Ore_T03.WID_Sniper_NoScope_Athena_VR_Ore_T03"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/LTM/WID_Sniper_NoScope_Athena_SR_Ore_T03.WID_Sniper_NoScope_Athena_SR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion >= 15.10)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Cowboy_Athena_UC.WID_Sniper_Cowboy_Athena_UC"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Cowboy_Athena_R.WID_Sniper_Cowboy_Athena_R"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Sniper_Cowboy_Athena_VR.WID_Sniper_Cowboy_Athena_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 19.00)
    {
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_UC.WID_Sniper_CoreSniper_Athena_UC"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_R.WID_Sniper_CoreSniper_Athena_R"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_VR.WID_Sniper_CoreSniper_Athena_VR"), 1));
        Slot2.Add(TPair<FString, int>(TEXT("/FlipperGameplay/Items/Weapons/CoreSniper/WID_Sniper_CoreSniper_Athena_SR.WID_Sniper_CoreSniper_Athena_SR"), 1));
    }

    Slots.Add(Slot2);
    // Slot 2 End

    // Slot 3 (Grappler)
    TArray<TPair<FString, int>> Slot3;

    if (VersionInfo.FortniteVersion >= 5.40)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Hook_Gun_VR_Ore_T03.WID_Hook_Gun_VR_Ore_T03"), 1));
    }

    if (VersionInfo.FortniteVersion == 10.40)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/WID_Badger_Grape_VR.WID_Badger_Grape_VR"), 1));
    }

    if (VersionInfo.FortniteVersion >= 12.00)
    {
        Slot3.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Weapons/Boss/WID_Boss_Adventure_GH.WID_Boss_Adventure_GH"), 1));
    }

    Slots.Add(Slot3);
    // Slot 3 End

    // Slot 4 (Consumables + Bandage)
    TArray<TPair<FString, int>> Slot4;

    Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/KnockGrenade/Athena_KnockGrenade.Athena_KnockGrenade"), 9));
    Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/Bandage/Athena_Bandage.Athena_Bandage"), 15));

    if (VersionInfo.FortniteVersion >= 5.30)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/RiftItem/Athena_Rift_Item.Athena_Rift_Item"), 2));
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/ShockwaveGrenade/Athena_ShockGrenade.Athena_ShockGrenade"), 6));
    }

    if (VersionInfo.FortniteVersion >= 7.30)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/IceGrenade/Athena_IceGrenade.Athena_IceGrenade"), 6));
    }

    if (VersionInfo.FortniteVersion >= 10.20)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/SilverBlazer/Athena_SilverBlazer_V2.Athena_SilverBlazer_V2"), 2));
    }

    if (VersionInfo.FortniteVersion >= 12.30)
    {
        Slot4.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/AppleSun/WID_Athena_AppleSun.WID_Athena_AppleSun"), 6));
    }

    Slots.Add(Slot4);
    // Slot 4 End

    // Slot 4 (Consumables)
    TArray<TPair<FString, int>> Slot5;

    Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/KnockGrenade/Athena_KnockGrenade.Athena_KnockGrenade"), 9));

    if (VersionInfo.FortniteVersion >= 5.30)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/RiftItem/Athena_Rift_Item.Athena_Rift_Item"), 2));
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/ShockwaveGrenade/Athena_ShockGrenade.Athena_ShockGrenade"), 6));
    }

    if (VersionInfo.FortniteVersion >= 7.30)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/IceGrenade/Athena_IceGrenade.Athena_IceGrenade"), 6));
    }

    if (VersionInfo.FortniteVersion >= 10.20)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/SilverBlazer/Athena_SilverBlazer_V2.Athena_SilverBlazer_V2"), 2));
    }

    if (VersionInfo.FortniteVersion >= 12.30)
    {
        Slot5.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Consumables/AppleSun/WID_Athena_AppleSun.WID_Athena_AppleSun"), 6));
    }

    Slots.Add(Slot5);
    // Slot 5 End

    // Slot 6 (Traps)
    TArray<TPair<FString, int>> Traps;
    Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_Floor_Player_Launch_Pad_Athena.TID_Floor_Player_Launch_Pad_Athena"), 2));
    Traps.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Traps/TID_Context_BouncePad_Athena.TID_Context_BouncePad_Athena"), 3));

    Slots.Add(Traps);
    // Slot 6 End

    // Slot 7 (Materials)
    std::uniform_int_distribution<int> Gold(1200, 7500);

    TArray<TPair<FString, int>> Materials;
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/WoodItemData.WoodItemData"), 70));
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/StoneItemData.StoneItemData"), 50));
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/MetalItemData.MetalItemData"), 30));

    if (VersionInfo.FortniteVersion >= 15)
    {
        Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/Athena_WadsItemData.Athena_WadsItemData"), Gold(rng)));
    }

    Slots.Add(Materials);
    // Slot 7 End

    // Slot 8 (Ammo)
    std::uniform_int_distribution<int> Heavy(50, 186);

    TArray<TPair<FString, int>> Ammo;
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsHeavy.AthenaAmmoDataBulletsHeavy"), Heavy(rng)));

    if (VersionInfo.FortniteVersion >= 17.00)
    {
        Ammo.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/Scooter/Ammo_Athena_Mother_Scooter.Ammo_Athena_Mother_Scooter"), 999));
    }

    Slots.Add(Ammo);
    // Slot 8 End

    return Slots;
}

TArray<TArray<TPair<FString, int>>> LateGame::GetCustomLoadout()
{
    std::random_device rd;
    std::mt19937 rng(rd());

    TArray<TArray<TPair<FString, int>>> Slots;

    TArray<TPair<FString, int>> Slot1;

    auto PrimaryItemDefinition = FindObject<UFortWorldItemDefinition>(FConfiguration::Primary.CStr());

    if (!PrimaryItemDefinition)
        PrimaryItemDefinition = TUObjectArray::FindObject<UFortWorldItemDefinition>(FString(FConfiguration::Primary).ToUtf8().c_str());

    if (PrimaryItemDefinition)
    {
        FString PrimaryFullPath = UKismetSystemLibrary::GetPathName((UObject*)PrimaryItemDefinition);
        Slot1.Add(TPair<FString, int>(PrimaryFullPath, FConfiguration::PrimaryAmount));
    }

    Slots.Add(Slot1);

    TArray<TPair<FString, int>> Slot2;

    auto SecondaryItemDefinition = FindObject<UFortWorldItemDefinition>(FConfiguration::Secondary.CStr());

    if (!SecondaryItemDefinition)
        SecondaryItemDefinition = TUObjectArray::FindObject<UFortWorldItemDefinition>(FString(FConfiguration::Secondary).ToUtf8().c_str());

    if (SecondaryItemDefinition)
    {
        FString SecondaryFullPath = UKismetSystemLibrary::GetPathName((UObject*)SecondaryItemDefinition);
        Slot2.Add(TPair<FString, int>(SecondaryFullPath, FConfiguration::SecondaryAmount));
    }

    Slots.Add(Slot2);

    TArray<TPair<FString, int>> Slot3;

    auto TertiaryItemDefinition = FindObject<UFortWorldItemDefinition>(FConfiguration::Tertiary.CStr());

    if (!TertiaryItemDefinition)
        TertiaryItemDefinition = TUObjectArray::FindObject<UFortWorldItemDefinition>(FString(FConfiguration::Tertiary).ToUtf8().c_str());

    if (TertiaryItemDefinition)
    {
        FString TertiaryFullPath = UKismetSystemLibrary::GetPathName((UObject*)TertiaryItemDefinition);
        Slot3.Add(TPair<FString, int>(TertiaryFullPath, FConfiguration::TertiaryAmount));
    }

    Slots.Add(Slot3);

    TArray<TPair<FString, int>> Slot4;

    auto QuaternaryItemDefinition = FindObject<UFortWorldItemDefinition>(FConfiguration::Quaternary.CStr());

    if (!QuaternaryItemDefinition)
        QuaternaryItemDefinition = TUObjectArray::FindObject<UFortWorldItemDefinition>(FString(FConfiguration::Quaternary).ToUtf8().c_str());

    if (QuaternaryItemDefinition)
    {
        FString QuaternaryFullPath = UKismetSystemLibrary::GetPathName((UObject*)QuaternaryItemDefinition);
        Slot4.Add(TPair<FString, int>(QuaternaryFullPath, FConfiguration::QuaternaryAmount));
    }

    Slots.Add(Slot4);

    TArray<TPair<FString, int>> Slot5;

    auto QuinaryItemDefinition = FindObject<UFortWorldItemDefinition>(FConfiguration::Quinary.CStr());

    if (!QuinaryItemDefinition)
        QuinaryItemDefinition = TUObjectArray::FindObject<UFortWorldItemDefinition>(FString(FConfiguration::Quinary).ToUtf8().c_str());

    if (QuinaryItemDefinition)
    {
        FString QuinaryFullPath = UKismetSystemLibrary::GetPathName((UObject*)QuinaryItemDefinition);
        Slot5.Add(TPair<FString, int>(QuinaryFullPath, FConfiguration::QuinaryAmount));
    }
    
    Slots.Add(Slot5);

    TArray<TPair<FString, int>> Slot6;

    auto TrapsItemDefinition = FindObject<UFortWorldItemDefinition>(FConfiguration::Traps.CStr());

    if (!TrapsItemDefinition)
        TrapsItemDefinition = TUObjectArray::FindObject<UFortWorldItemDefinition>(FString(FConfiguration::Traps).ToUtf8().c_str());

    if (TrapsItemDefinition)
    {
        FString TrapsFullPath = UKismetSystemLibrary::GetPathName((UObject*)TrapsItemDefinition);
        Slot6.Add(TPair<FString, int>(TrapsFullPath, FConfiguration::TrapsAmount));
    }

    Slots.Add(Slot6);

    std::uniform_int_distribution<int> Mats(186, 646);
    std::uniform_int_distribution<int> Gold(1200, 7500);

    TArray<TPair<FString, int>> Materials;
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/WoodItemData.WoodItemData"), Mats(rng)));
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/StoneItemData.StoneItemData"), Mats(rng)));
    Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/MetalItemData.MetalItemData"), Mats(rng)));

    if (VersionInfo.FortniteVersion >= 15)
    {
        Materials.Add(TPair<FString, int>(TEXT("/Game/Items/ResourcePickups/Athena_WadsItemData.Athena_WadsItemData"), Gold(rng)));
    }

    Slots.Add(Materials);
    // Slot 7 End

    // Slot 8 (Ammo)
    std::uniform_int_distribution<int> Heavy(50, 186);
    std::uniform_int_distribution<int> Shells(87, 576);
    std::uniform_int_distribution<int> Medium(124, 824);
    std::uniform_int_distribution<int> Light(186, 824);
    std::uniform_int_distribution<int> Rockets(3, 12);
    std::uniform_int_distribution<int> STW(186, 999);
    std::uniform_int_distribution<int> Arrows(12, 30);

    TArray<TPair<FString, int>> Ammo;
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsHeavy.AthenaAmmoDataBulletsHeavy"), Heavy(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataShells.AthenaAmmoDataShells"), Shells(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsMedium.AthenaAmmoDataBulletsMedium"), Medium(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AthenaAmmoDataBulletsLight.AthenaAmmoDataBulletsLight"), Light(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Athena/Items/Ammo/AmmoDataRockets.AmmoDataRockets"), 12));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataExplosive.AmmoDataExplosive"), STW(rng))); // make these and below optional???
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataEnergyCell.AmmoDataEnergyCell"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataBulletsHeavy.AmmoDataBulletsHeavy"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataBulletsMedium.AmmoDataBulletsMedium"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataBulletsLight.AmmoDataBulletsLight"), STW(rng)));
    Ammo.Add(TPair<FString, int>(TEXT("/Game/Items/Ammo/AmmoDataShells.AmmoDataShells"), STW(rng)));

    if (VersionInfo.FortniteVersion >= 16.00)
    {
        Ammo.Add(TPair<FString, int>(TEXT("/PrimalGameplay/Items/Ammo/AthenaAmmoDataArrows.AthenaAmmoDataArrows"), Arrows(rng)));
    }

    if (VersionInfo.FortniteVersion >= 17.00)
    {
        Ammo.Add(TPair<FString, int>(TEXT("/MotherGameplay/Items/Scooter/Ammo_Athena_Mother_Scooter.Ammo_Athena_Mother_Scooter"), 999));
    }

    Slots.Add(Ammo);
    // Slot 8 End

    return Slots;
}

void LateGame::EquipLoadout(AFortPlayerControllerAthena* PlayerController)
{
    if (!PlayerController || !PlayerController->HasAuthority())
        return;

    auto WorldInventory = PlayerController->WorldInventory;

    if (!WorldInventory)
        return;

    TArray<TArray<TPair<FString, int>>> Slots;

    if (IsOneShot())
    {
        Slots = LateGame::GetOSLoadout();
    }
    else if (FConfiguration::bUseCustomLoadout)
    {
        Slots = LateGame::GetCustomLoadout();
    }
    else if (FConfiguration::bUseVersionizedLoadout)
    {
        Slots = LateGame::GetVersionizedLoadout();
    }
    else
    {
        Slots = LateGame::GetLoadout();
    }

    std::random_device rd;
    std::mt19937 rng(rd());

    for (int SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex)
    {
        const TArray<TPair<FString, int>>& Slot = Slots[SlotIndex];

        if (Slot.Num() == 0)
            continue;

        if (SlotIndex < 6)
        {
            std::uniform_int_distribution<int> dist(0, Slot.Num() - 1);
            int RandomIndex = dist(rng);
            const TPair<FString, int>& RandomItem = Slot[RandomIndex];

            const FString& Path = RandomItem.Key();
            int Count = RandomItem.Value();

            std::string ConvertedPath = FStringToStdString(Path);
			printf("Slot %d: Trying to equip '%s' x%d\n", SlotIndex + 1, ConvertedPath.c_str(), Count);

            const UFortWorldItemDefinition* ItemDef = FindObject<UFortWorldItemDefinition>(Path.CStr());

            if (!ItemDef)
            {
				printf("Failed to find item: %s\n", ConvertedPath.c_str());
                continue;
            }

            int32 ClipSize = 0;
            int32 PhantomReserveAmmo = 0;

            auto ItemEntry = WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& Entry) { return Entry.ItemDefinition == ItemDef; }, FFortItemEntry::Size());

            if (auto WeaponDef = ItemDef->Cast<UFortWeaponItemDefinition>())
            {
                auto Stats = AFortInventory::GetStats(WeaponDef);

                if (Stats && Stats != (void*)-1)
                {
                    ClipSize = Stats->ClipSize;

                    if (WeaponDef->HasbUsesPhantomReserveAmmo() && WeaponDef->bUsesPhantomReserveAmmo)
                        PhantomReserveAmmo = (Stats->InitialClips - 1) * Stats->ClipSize;
                }
            }

            WorldInventory->GiveItem(ItemDef, Count, ClipSize, 0, true, true, PhantomReserveAmmo, {});
        }
        else
        {
            for (int i = 0; i < Slot.Num(); ++i)
            {
                const auto& Item = Slot[i];

                const FString& Path = Item.Key();
                int Count = Item.Value();

                std::string ConvertedPath = FStringToStdString(Path);
				printf("Slot %d: Trying to equip '%s' x%d\n", SlotIndex + 1, ConvertedPath.c_str(), Count);

                const UFortWorldItemDefinition* ItemDef = FindObject<UFortWorldItemDefinition>(Path.CStr());

                if (!ItemDef)
                {
					printf("Failed to find item: %s\n", ConvertedPath.c_str());
                    continue;
                }

                int32 ClipSize = 0;
                int32 PhantomReserveAmmo = 0;

                auto ItemEntry = WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& Entry) { return Entry.ItemDefinition == ItemDef; }, FFortItemEntry::Size());

                if (auto WeaponDef = ItemDef->Cast<UFortWeaponItemDefinition>())
                {
                    auto Stats = AFortInventory::GetStats(WeaponDef);

                    if (Stats && Stats != (void*)-1)
                    {
                        ClipSize = Stats->ClipSize;

                        if (WeaponDef->HasbUsesPhantomReserveAmmo() && WeaponDef->bUsesPhantomReserveAmmo)
                            PhantomReserveAmmo = (Stats->InitialClips - 1) * Stats->ClipSize;
                    }
                }

                WorldInventory->GiveItem(ItemDef, Count, ClipSize, 0, true, true, PhantomReserveAmmo, {});
            }
        }
    }
}