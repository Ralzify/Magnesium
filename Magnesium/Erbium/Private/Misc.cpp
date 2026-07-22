#include "pch.h"
#include "../Public/Misc.h"
#include "../Public/Finders.h"
#include <algorithm>
#include <tlhelp32.h>
#include "../Public/Configuration.h"
#include "../Public/GUI.h"
#include "../../FortniteGame/Public/FortPlayerControllerAthena.h"
#include "../../Engine/Public/NetDriver.h"
#include "../../FortniteGame/Public/FortGameMode.h"
#include "../../FortniteGame/Public/FortWeapon.h"
#include "../../FortniteGame/Public/FortKismetLibrary.h"

const std::unordered_map<std::string, std::string> Misc::ItemNames = {
	{ "ar", "WID_Assault_Auto_Athena_R_Ore_T03" },
	{ "ar_c", "WID_Assault_Auto_Athena_C_Ore_T03" },
	{ "ar_uc", "WID_Assault_Auto_Athena_UC_Ore_T03" },
	{ "ar_r", "WID_Assault_Auto_Athena_R_Ore_T03" },
	{ "ar_sr", "WID_Assault_AutoHigh_Athena_SR_Ore_T03" },
	{ "ar_ur", "WID_Boss_Adventure_AR" },
	{ "ar_vr", "WID_Assault_AutoHigh_Athena_VR_Ore_T03" },
	{ "anvil_r", "WID_Launcher_HomingRocket_Athena_R" },
	{ "anvil_vr", "WID_Launcher_HomingRocket_Athena_VR" },
	{ "anvil_sr", "WID_Launcher_HomingRocket_Athena_SR" },
	{ "anvil", "WID_Launcher_HomingRocket_Athena_SR" },
	{ "balloons", std::floor(VersionInfo.FortniteVersion) < 7 ? "Athena_Balloons_Consumable" : "Athena_Balloons" },
	{ "bandage", "Athena_Bandage" },
	{ "bandages", "Athena_Bandage" },
	{ "batarang", "WID_Athena_BadgerBangsNew" },
	{ "batarangs", "WID_Athena_BadgerBangsNew" },
	{ "batgrap", "WID_Badger_Grape_VR" },
	{ "batman", "WID_Badger_Grape_VR" },
	{ "birthday", "Athena_BirthdayGiftBox" },
	{ "birthdaypresent", "Athena_BirthdayGiftBox" },
	{ "birthdaypresents", "Athena_BirthdayGiftBox" },
	{ "bolt", "WID_Sniper_BoltAction_Scope_Athena_R_Ore_T03" },
	{ "bolt_c", "WID_Sniper_BoltAction_Scope_Athena_C_Ore_T03" },
	{ "bolt_r", "WID_Sniper_BoltAction_Scope_Athena_R_Ore_T03" },
	{ "bolt_sr", "WID_Sniper_BoltAction_Scope_Athena_SR_Ore_T03" },
	{ "bolt_uc", "WID_Sniper_BoltAction_Scope_Athena_UC_Ore_T03" },
	{ "bolt_vr", "WID_Sniper_BoltAction_Scope_Athena_VR_Ore_T03" },
	{ "boogie", "Athena_DanceGrenade" },
	{ "boogiebomb", "Athena_DanceGrenade" },
	{ "boogies", "Athena_DanceGrenade" },
	{ "boom", "WID_WaffleTruck_BoomSniper" },
	{ "boomsniper", "WID_WaffleTruck_BoomSniper" },
	{ "boombox", "BoomBox_Athena" },
	{ "bouncer", "TID_Context_BouncePad_Athena" },
	{ "bouncers", "TID_Context_BouncePad_Athena" },
	{ "brick", "StoneItemData" },
	{ "brutus", "WID_Boss_Hos_MG" },
	{ "brutusminigun", "WID_Boss_Hos_MG" },
	{ "burst", "WID_Assault_SemiAuto_Athena_SR_Ore_T03" },
	{ "burst_c", "WID_Assault_SemiAuto_Athena_C_Ore_T02" },
	{ "burst_uc", "WID_Assault_SemiAuto_Athena_UC_Ore_T03" },
	{ "burst_r", "WID_Assault_SemiAuto_Athena_R_Ore_T03" },
	{ "burst_vr", "WID_Assault_SemiAuto_Athena_VR_Ore_T03" },
	{ "burst_sr", "WID_Assault_SemiAuto_Athena_SR_Ore_T03" },
	{ "ca", "AGID_AshtonPack_Chicago" },
	{ "can", "WID_Athena_Bucket_Old" },
	{ "cans", "WID_Athena_Bucket_Old" },
	{ "captainamerica", "AGID_AshtonPack_Chicago" },
	{ "chiller", "Athena_IceGrenade" },
	{ "chillergrenade", "Athena_IceGrenade" },
	{ "chillers", "Athena_IceGrenade" },
	{ "chillertrap", "TID_Context_Freeze_Athena" },
	{ "chugs", "Athena_ChillBronco" },
	{ "chugsplash", "Athena_ChillBronco" },
	{ "crash", "WID_Athena_AppleSun" },
	{ "crashes", "WID_Athena_AppleSun" },
	{ "crashpad", "WID_Athena_AppleSun" },
	{ "crashpads", "WID_Athena_AppleSun" },
	{ "creative", "WID_CreativeTool" },
	{ "crown", "AGID_VictoryCrown" },
	{ "cybertron", "AGID_AloeCrouton_Launcher_Athena" },
	{ "deagle", "WID_Pistol_HandCannon_Athena_SR_Ore_T03" },
	{ "deagle_sr", "WID_Pistol_HandCannon_Athena_SR_Ore_T03" },
	{ "deagle_vr", "WID_Pistol_HandCannon_Athena_VR_Ore_T03" },
	{ "directional", "TID_Floor_Player_Jump_Pad_Free_Direction_Athena" },
	{ "directionals", "TID_Floor_Player_Jump_Pad_Free_Direction_Athena" },
	{ "directionalpad", "TID_Floor_Player_Jump_Pad_Free_Direction_Athena" },
	{ "directionalpads", "TID_Floor_Player_Jump_Pad_Free_Direction_Athena" },
	{ "doom", "WID_HighTower_Date_ChainLightning_CoreBR" },
	{ "doomgauntlets", "WID_HighTower_Date_ChainLightning_CoreBR" },
	{ "dragonsbreath", "WID_WaffleTruck_Sniper_DragonBreath" },
	{ "dualies", "WID_WaffleTruck_HopRockDualies" },
	{ "dub", "WID_WaffleTruck_Dub" },
	{ "dynamite", "Athena_TNT" },
	{ "exoticstormscout", "WID_WaffleTruck_Sniper_StormScout" },
	{ "exstormscout", "WID_WaffleTruck_Sniper_StormScout" },
	{ "friendlybotgrenade", "WID_Athena_FrenchYedoc_JWFriendly" },
	{ "fbg", "WID_Athena_FrenchYedoc_JWFriendly" },
	{ "fireflies", "WID_Athena_Grenade_Molotov" },
	{ "firefly", "WID_Athena_Grenade_Molotov" },
	{ "firesniper", "WID_WaffleTruck_Sniper_DragonBreath" },
	{ "flare", "WID_FringePlank_Athena_Prototype" },
	{ "flint", "WID_Pistol_Flintlock_Athena_UC" },
	{ "flint_c", "WID_Pistol_Flintlock_Athena_C" },
	{ "flint_uc", "WID_Pistol_Flintlock_Athena_UC" },
	{ "fortress", "Athena_SuperTowerGrenade_A" },
	{ "gascan", "WID_Launcher_Petrol" },
	{ "gas", "WID_Launcher_Petrol" },
	{ "gav_grap", "/Game/Gav/Weapons/WID_Hook_Gun_Ride.WID_Hook_Gun_Ride" },
	{ "gav_jules", "/Game/Gav/Weapons/WID_Boss_GrapplingHoot_Ride.WID_Boss_GrapplingHoot_Ride" },
	{ "gav_anvil", "/Game/Gav/Weapons/WID_Launcher_HomingRocket_Athena_Ride.WID_Launcher_HomingRocket_Athena_Ride" },
	{ "gav_quad", "/Game/Gav/Weapons/WID_Launcher_Military_Athena_Ride.WID_Launcher_Military_Athena_Ride" },
	{ "gav_proxy", "/Game/Gav/Weapons/WID_GrenadeLauncher_Prox_Athena_Ride.WID_GrenadeLauncher_Prox_Athena_Ride" },
	{ "gav_bazooka", "/Game/Gav/Weapons/AGID_Lotus_Mustache_Ride.AGID_Lotus_Mustache_Ride" },
	{ "gav_bandage", "/Game/Gav/Weapons/AGID_Lotus_Mustache_Ride.AGID_Lotus_Mustache_Ride" },
	{ "gav_lg_grap", "/Game/Gav/Weapons/WID_Hook_Gun_Ride_Low.WID_Hook_Gun_Ride_Low" },
	{ "gav_lg_jules", "/Game/Gav/Weapons/WID_Boss_GrapplingHoot_Ride_Low.WID_Boss_GrapplingHoot_Ride_Low" },
	{ "gav_ogbolt", "/Game/Gav/Weapons/OG/WID_Sniper_BoltAction_Scope_Athena_OG.WID_Sniper_BoltAction_Scope_Athena_OG" },
	{ "gav_og_bolt", "/Game/Gav/Weapons/OG/WID_Sniper_BoltAction_Scope_Athena_OG.WID_Sniper_BoltAction_Scope_Athena_OG" },
	{ "gav_flintsmg", "/Game/Gav/Weapons/WID_Pistol_Scavenger_Athena_Flint.WID_Pistol_Scavenger_Athena_Flint" },
	{ "gav_flint_smg", "/Game/Gav/Weapons/WID_Pistol_Scavenger_Athena_Flint.WID_Pistol_Scavenger_Athena_Flint" },
	{ "gav_selfrocket", "/Game/Gav/Weapons/WID_Launcher_Rocket_Athena_Self.WID_Launcher_Rocket_Athena_Self" },
	{ "gav_self_rocket", "/Game/Gav/Weapons/WID_Launcher_Rocket_Athena_Self.WID_Launcher_Rocket_Athena_Self" },
	{ "gav_selfquad", "/Game/Gav/Weapons/WID_Launcher_Military_Athena_Ride_Self.WID_Launcher_Military_Athena_Ride_Self" },
	{ "gav_self_quad", "/Game/Gav/Weapons/WID_Launcher_Military_Athena_Ride_Self.WID_Launcher_Military_Athena_Ride_Self" },
	{ "gl", "WID_Launcher_Grenade_Athena_SR_Ore_T03" },
	{ "gl_r", "WID_Launcher_Grenade_Athena_R_Ore_T03" },
	{ "gl_sr", "WID_Launcher_Grenade_Athena_SR_Ore_T03" },
	{ "gl_vr", "WID_Launcher_Grenade_Athena_VR_Ore_T03" },
	{ "gliders", "Athena_Glider_Item" },
	{ "glider", "Athena_Glider_Item" },
	{ "gold", "Athena_WadsItemData" },
	{ "god", VersionInfo.FortniteVersion <= 17 ? "/SaveTheWorld/Items/Weapons/Ranged/WIP/TestGod.TestGod" : "/Game/Items/Weapons/Ranged/WIP/TestGod.TestGod" },
	{ "goldfish", "WID_Athena_Bucket_Nice" },
	{ "grabitron", "WID_GravyGoblinV2_Athena" },
	{ "grap_n", "WID_Hook_Gun_VR_Ore_T03" },
	{ "grapple_n", "WID_Hook_Gun_VR_Ore_T03" },
	{ "grappler_n", "WID_Hook_Gun_VR_Ore_T03" },
	{ "grappler", "logic_grappler" },
	{ "grapple", "logic_grappler" },
	{ "grap", "logic_grappler" },
	{ "grenade", "Athena_Grenade" },
	{ "grenades", "Athena_Grenade" },
	{ "guided", "WID_RC_Rocket_Athena_SR_T03" },
	{ "guided_sr", "WID_RC_Rocket_Athena_SR_T03" },
	{ "guided_vr", "WID_RC_Rocket_Athena_VR_T03" },
	{ "guidedmissile_sr", "WID_RC_Rocket_Athena_SR_T03" },
	{ "guidedmissile_vr", "WID_RC_Rocket_Athena_VR_T03" },
	{ "harpoon", VersionInfo.FortniteVersion < 13.40 ? "WID_Athena_HappyGhost_Infinite" : "WID_Athena_HappyGhost" },
	{ "heavy", "WID_Sniper_Heavy_Athena_SR_Ore_T03" },
	{ "heavy_r", "WID_Sniper_Heavy_Athena_R_Ore_T03" },
	{ "heavy_sr", "WID_Sniper_Heavy_Athena_SR_Ore_T03" },
	{ "heavy_ur", "WID_Sniper_Heavy_Athena_UR_Ore_T03" },
	{ "heavy_vr", "WID_Sniper_Heavy_Athena_VR_Ore_T03" },
	{ "heavyammo", "AthenaAmmoDataBulletsHeavy" },
	{ "hopflop", "WID_Athena_Flopper_HopFlopper" },
	{ "hopflopper", "WID_Athena_Flopper_HopFlopper" },
	{ "hoprockdualies", "WID_WaffleTruck_HopRockDualies" },
	{ "hunterbolt", "WID_Sniper_CoreSniper_Athena_SR" },
	{ "hunterbolt_r", "WID_Sniper_CoreSniper_Athena_R" },
	{ "hunterbolt_sr", "WID_Sniper_CoreSniper_Athena_SR" },
	{ "hunterbolt_uc", "WID_Sniper_CoreSniper_Athena_UC" },
	{ "hunterbolt_vr", "WID_Sniper_CoreSniper_Athena_VR" },
	{ "hunting", VersionInfo.FortniteVersion < 13.00 ? "WID_Sniper_NoScope_Athena_SR_Ore_T03" : "WID_Sniper_NoScope_Athena_R_Ore_T03" },
	{ "hunting_r", "WID_Sniper_NoScope_Athena_R_Ore_T03" },
	{ "hunting_sr", "WID_Sniper_NoScope_Athena_SR_Ore_T03" },
	{ "hunting_uc", "WID_Sniper_NoScope_Athena_UC_Ore_T03" },
	{ "hunting_vr", "WID_Sniper_NoScope_Athena_VR_Ore_T03" },
	{ "icy", "/CorruptionGameplay/Gameplay/Items/Consumables/IcyGrapple/WID_Athena_IcyGrapple.WID_Athena_IcyGrapple" },
	{ "icygrap", "/CorruptionGameplay/Gameplay/Items/Consumables/IcyGrapple/WID_Athena_IcyGrapple.WID_Athena_IcyGrapple" },
	{ "icygrapple", "/CorruptionGameplay/Gameplay/Items/Consumables/IcyGrapple/WID_Athena_IcyGrapple.WID_Athena_IcyGrapple" },
	{ "icygrappler", "/CorruptionGameplay/Gameplay/Items/Consumables/IcyGrapple/WID_Athena_IcyGrapple.WID_Athena_IcyGrapple" },
	{ "impulse", "Athena_KnockGrenade" },
	{ "impulsegrenade", "Athena_KnockGrenade" },
	{ "impulsegrenades", "Athena_KnockGrenade" },
	{ "impulses", "Athena_KnockGrenade" },
	{ "infantry_c", "WID_Assault_Infantry_Athena_C" },
	{ "infantry_uc", "WID_Assault_Infantry_Athena_UC" },
	{ "infantry_r", "WID_Assault_Infantry_Athena_R" },
	{ "infantry_vr", "WID_Assault_Infantry_Athena_VR" },
	{ "infantry_sr", "WID_Assault_Infantry_Athena_SR" },
	{ "infantry", "WID_Assault_Infantry_Athena_SR" },
	{ "ironman", std::floor(VersionInfo.FortniteVersion) == 14 ? "AGID_AshtonPack_Indigo" : "WID_HighTower_Tomato_Repulsor_CoreBR" },
	{ "jules", "WID_Boss_GrapplingHoot" },
	{ "julesgrap", "WID_Boss_GrapplingHoot" },
	{ "julesgrappler", "WID_Boss_GrapplingHoot" },
	{ "jumpad", "TID_Floor_Player_Jump_Pad_Athena" },
	{ "jumppad", "TID_Floor_Player_Jump_Pad_Athena" },
	{ "keg", "WID_Athena_ShieldGenerator" },
	{ "kegs", "WID_Athena_ShieldGenerator" },
	{ "kits", "WID_Launcher_Shockwave_Athena_UR_Ore_T03" },
	{ "kitslauncher", "WID_Launcher_Shockwave_Athena_UR_Ore_T03" },
	{ "kineticblade", "WID_Melee_Katana" },
	{ "kinetic_blade", "WID_Melee_Katana" },
	{ "kinetic", "WID_Melee_Katana" },
	{ "launch", "TID_Floor_Player_Launch_Pad_Athena" },
	{ "launches", "TID_Floor_Player_Launch_Pad_Athena" },
	{ "launchpad", "TID_Floor_Player_Launch_Pad_Athena" },
	{ "lever", "WID_Sniper_Cowboy_Athena_VR" },
	{ "lever_r", "WID_Sniper_Cowboy_Athena_R" },
	{ "lever_sr", "WID_Sniper_Cowboy_Athena_SR" },
	{ "lever_uc", "WID_Sniper_Cowboy_Athena_UC" },
	{ "lever_vr", "WID_Sniper_Cowboy_Athena_VR" },
	{ "light", "AthenaAmmoDataBulletsLight" },
	{ "lightammo", "AthenaAmmoDataBulletsLight" },
	{ "mammoth_uc", "WID_Pistol_Chrono_Athena_UC" },
	{ "mammoth_r", "WID_Pistol_Chrono_Athena_R" },
	{ "mammoth_vr", "WID_Pistol_Chrono_Athena_VR" },
	{ "mammoth_sr", "WID_Pistol_Chrono_Athena_SR" },
	{ "medium", "AthenaAmmoDataBulletsMedium" },
	{ "mediumammo", "AthenaAmmoDataBulletsMedium" },
	{ "metal", "MetalItemData" },
	{ "mini", "Athena_ShieldSmall" },
	{ "minigun", "WID_Assault_LMG_Athena_SR_Ore_T03" },
	{ "minigun_sr", "WID_Assault_LMG_Athena_SR_Ore_T03" },
	{ "minigun_ur", "WID_Boss_Hos_MG" },
	{ "minigun_vr", "WID_Assault_LMG_Athena_VR_Ore_T03" },
	{ "mini", "Athena_ShieldSmall" },
	{ "minis", "Athena_ShieldSmall" },
	{ "missile_sr", "WID_RC_Rocket_Athena_SR_T03" },
	{ "missile_vr", "WID_RC_Rocket_Athena_VR_T03" },
	{ "mythicfish", "WID_Athena_Bucket_Nice" },
	{ "mythicgoldfish", "WID_Athena_Bucket_Nice" },
	{ "nimbus", "/StaminaGameplay/Gameplay/Items/Alaska/WID_Stamina_Alaska.WID_Stamina_Alaska" },
	{ "pad", "TID_Floor_Player_Launch_Pad_Athena" },
	{ "paf", "Athena_TowerGrenade" },
	{ "portafort", "Athena_TowerGrenade" },
	{ "portafortress", "Athena_SuperTowerGrenade_A" },
	{ "paft", "Athena_SuperTowerGrenade_A" },
	{ "phone", "WID_CreativeTool" },
	{ "pump", VersionInfo.FortniteVersion < 6.31 ? "WID_Shotgun_Standard_Athena_SR_Ore_T03" : "WID_Shotgun_Standard_Athena_UC_Ore_T03" },
	{ "pump_r", "WID_Shotgun_Standard_Athena_UC_Ore_T03" },
	{ "pump_sr", "WID_Shotgun_Standard_Athena_SR_Ore_T03" },
	{ "pump_uc", "WID_Shotgun_Standard_Athena_C_Ore_T03" },
	{ "pump_c", "WID_Shotgun_Standard_Athena_Common" },
	{ "pump_vr", "WID_Shotgun_Standard_Athena_VR_Ore_T03" },
	{ "pumpkin", "WID_Launcher_Pumpkin_Athena_SR_Ore_T03" },
	{ "pumpkin_r", "WID_Launcher_Pumpkin_Athena_R_Ore_T03" },
	{ "pumpkin_sr", "WID_Launcher_Pumpkin_Athena_SR_Ore_T03" },
	{ "pumpkin_uc", "WID_Launcher_Pumpkin_Athena_UC_Ore_T03" },
	{ "pumpkin_vr", "WID_Launcher_Pumpkin_Athena_VR_Ore_T03" },
	{ "present", "Athena_GiftBox" },
	{ "presents", "Athena_GiftBox" },
	{ "ch2present", "Athena_HolidayGiftBox" },
	{ "ch2presents", "Athena_HolidayGiftBox" },
	{ "quad", "WID_Launcher_Military_Athena_SR_Ore_T03" },
	{ "quad_sr", "WID_Launcher_Military_Athena_SR_Ore_T03" },
	{ "quad_vr", "WID_Launcher_Military_Athena_VR_Ore_T03" },
	{ "quadlauncher", "WID_Launcher_Military_Athena_SR_Ore_T03" },
	{ "reaper_uc", "WID_Sniper_Paprika_Athena_UC" },
	{ "reaper_r", "WID_Sniper_Paprika_Athena_R" },
	{ "reaper_vr", "WID_Sniper_Paprika_Athena_VR" },
	{ "reaper_sr", "WID_Sniper_Paprika_Athena_SR" },
	{ "recon", "AGID_Athena_Scooter" },
	{ "rift", "Athena_Rift_Item" },
	{ "rifts", "Athena_Rift_Item" },
	{ "ripsaw", "WID_Sawblade_Athena" },
	{ "chainsaw", "WID_Sawblade_Athena" },
	{ "rocket", "WID_Launcher_Rocket_Athena_SR_Ore_T03" },
	{ "rocket_r", "WID_Launcher_Rocket_Athena_R_Ore_T03" },
	{ "rocket_sr", "WID_Launcher_Rocket_Athena_SR_Ore_T03" },
	{ "rocket_vr", "WID_Launcher_Rocket_Athena_VR_Ore_T03" },
	{ "rocketammo", "AmmoDataRockets" },
	{ "rockets", "AmmoDataRockets" },
	{ "rocketram", "/RocketRamGameplay/Items/RocketRam/WID_RocketRam.WID_RocketRam" },
	{ "rustycan", "WID_Athena_Bucket_Old" },
	{ "scar", "WID_Assault_AutoHigh_Athena_SR_Ore_T03" },
	{ "scar_sr", "WID_Assault_AutoHigh_Athena_SR_Ore_T03" },
	{ "scar_ur", "WID_Boss_Adventure_AR" },
	{ "scar_vr", "WID_Assault_AutoHigh_Athena_VR_Ore_T03" },
	{ "semi", "WID_Sniper_Standard_Scope_Athena_SR_Ore_T03" },
	{ "semi_r", "WID_Sniper_Standard_Scope_Athena_SR_Ore_T03" },
	{ "semi_uc", "WID_Sniper_Standard_Scope_Athena_VR_Ore_T03" },
	{ "shells", "AthenaAmmoDataShells" },
	{ "shield", "AGID_AshtonPack_Chicago" },
	{ "shieldbubble", "Athena_SilverBlazer_V2" },
	{ "shieldkeg", "WID_Athena_ShieldGenerator" },
	{ "shock", "Athena_ShockGrenade" },
	{ "shocks", "Athena_ShockGrenade" },
	{ "shockwave", "Athena_ShockGrenade" },
	{ "shockwavegrenade", "Athena_ShockGrenade" },
	{ "shockwaves", "Athena_ShockGrenade" },
	{ "shock_hammer", "WID_Muster_ShockwaveMace" },
	{ "shockhammer", "WID_Muster_ShockwaveMace" },
	{ "skye", "WID_Boss_Adventure_GH" },
	{ "skyear", "WID_Boss_Adventure_AR" },
	{ "skyegrap", "WID_Boss_Adventure_GH" },
	{ "skyesar", "WID_Boss_Adventure_AR" },
	{ "skyesgrap", "WID_Boss_Adventure_GH" },
	{ "skyesgrappler", "WID_Boss_Adventure_GH" },
	{ "slurpfish", "WID_Athena_Flopper_Effective" },
	{ "snowman", VersionInfo.FortniteVersion < 11.31 ? "AGID_SneakySnowmanV2" : "Athena_SneakySnowman" },
	{ "spaz", "WID_Shotgun_Standard_Athena_SR_Ore_T03" },
	{ "spaz_sr", "WID_Shotgun_Standard_Athena_SR_Ore_T03" },
	{ "spaz_vr", "WID_Shotgun_Standard_Athena_VR_Ore_T03" },
	{ "spiderman", "WID_WestSausage_Parallel" },
	{ "stark_r", "WID_Assault_Stark_Athena_R_Ore_T03" },
	{ "stark_vr", "WID_Assault_Stark_Athena_VR_Ore_T03" },
	{ "stark_sr", "WID_Assault_Stark_Athena_SR_Ore_T03" },
	{ "stink", "Athena_GasGrenade" },
	{ "stinkbomb", "Athena_GasGrenade" },
	{ "stinks", "Athena_GasGrenade" },
	{ "stone", "StoneItemData" },
	{ "stormscout", "WID_Sniper_Weather_Athena_SR" },
	{ "stormscout_sr", "WID_Sniper_Weather_Athena_SR" },
	{ "stormscout_vr", "WID_Sniper_Weather_Athena_VR" },
	{ "stormflip", "Athena_DogSweater" },
	{ "stwbow", "WID_Sniper_Neon_Bow_SR_Crystal_T04" },
	{ "suppressed", "WID_Sniper_Suppressed_Scope_Athena_SR_Ore_T03" },
	{ "suppressed_sr", "WID_Sniper_Suppressed_Scope_Athena_SR_Ore_T03" },
	{ "suppressed_vr", "WID_Sniper_Suppressed_Scope_Athena_VR_Ore_T03" },
	{ "suppressed_r", "WID_Sniper_Suppressed_Scope_Athena_R_Ore_T03" },
	{ "suppressed_ar_r", "WID_Sniper_Suppressed_Scope_Athena_R_Ore_T03" },
	{ "suppressed_ar_vr", "WID_Sniper_Suppressed_Scope_Athena_VR_Ore_T03" },
	{ "suppressed_ar_sr", "WID_Sniper_Suppressed_Scope_Athena_SR_Ore_T03" },
	{ "tac", "WID_Shotgun_HighSemiAuto_Athena_SR_Ore_T03" },
	{ "tac_r", "WID_Shotgun_SemiAuto_Athena_VR_Ore_T03" },
	{ "tac_sr", "WID_Shotgun_HighSemiAuto_Athena_SR_Ore_T03" },
	{ "tac_uc", "WID_Shotgun_SemiAuto_Athena_R_Ore_T03" },
	{ "tac_vr", "WID_Shotgun_HighSemiAuto_Athena_VR_Ore_T03" },
	{ "tac_ar_c", "WID_Assault_PistolCaliber_AR_Athena_C_Ore_T03" },
	{ "tac_ar_uc", "WID_Assault_PistolCaliber_AR_Athena_UC_Ore_T03" },
	{ "tac_ar_r", "WID_Assault_PistolCaliber_AR_Athena_R_Ore_T03" },
	{ "tac_ar_vr", "WID_Assault_PistolCaliber_AR_Athena_VR_Ore_T03" },
	{ "tac_ar_sr", "WID_Assault_PistolCaliber_AR_Athena_SR_Ore_T03" },
	{ "thor", "AGID_AshtonPack_Turbo" },
	{ "thors", "AGID_AshtonPack_Turbo" },
	{ "thorshammer", "AGID_AshtonPack_Turbo" },
	{ "tire", "ID_ValetMod_Tires_OffRoad_Thrown" },
	{ "tires", "ID_ValetMod_Tires_OffRoad_Thrown" },
	{ "trash", "WID_Launcher_Scavenger_SR_Ore_T05" },
	{ "trashcannon", "WID_Launcher_Scavenger_SR_Ore_T05" },
	{ "tnt", "Athena_TNT" },
	{ "tyre", "ID_ValetMod_Tires_OffRoad_Thrown" },
	{ "upward", "TID_Floor_Player_Jump_Pad_Athena" },
	{ "upwards", "TID_Floor_Player_Jump_Pad_Athena" },
	{ "upwardpad", "TID_Floor_Player_Jump_Pad_Athena" },
	{ "upwardpads", "TID_Floor_Player_Jump_Pad_Athena" },
	{ "wood", "WoodItemData" },
	{ "xenon", "/SaveTheWorld/Items/Weapons/Ranged/Sniper/Neon_Bow/WID_Sniper_Neon_Bow_SR_Crystal_T04.WID_Sniper_Neon_Bow_SR_Crystal_T04" },
	{ "xenonbow", "/SaveTheWorld/Items/Weapons/Ranged/Sniper/Neon_Bow/WID_Sniper_Neon_Bow_SR_Crystal_T04.WID_Sniper_Neon_Bow_SR_Crystal_T04" },
	{ "zero", "WID_Athena_Flopper_Zero" },
	{ "zeropoint", "WID_Athena_Flopper_Zero" },
	{ "zeropointfish", "WID_Athena_Flopper_Zero" },
	{ "fishingrod", "WID_Athena_FloppingRabbit" },
	{ "broom", "WID_Athena_WitchBroom" },
	{ "witchbroom", "WID_Athena_WitchBroom" },
	{ "grapglove", "WID_GrappleGloves" },
	{ "rod", "WID_Athena_FloppingRabbit" },
	{ "prorod", "WID_Athena_FloppingRabbit_HighTier" },
	{ "profishingrod", "WID_Athena_FloppingRabbit_HighTier" },
	{ "crashpadjr", "WID_Athena_AppleSunSmall" },
	{ "crashjr", "WID_Athena_AppleSunSmall" },
	{ "spytechgrap", "WID_Hook_Gun_Spytech_VR_Ore_T03" },
	{ "spytech", "WID_Hook_Gun_Spytech_VR_Ore_T03" },
	{ "nitro", "WID_Moonflax_NitroSplash" },
	{ "nitrosplash", "WID_Moonflax_NitroSplash" },
	{ "wings", "WID_Athena_SunRose_Wings" },
	{ "nitrohoop", "WID_Moonflax_FlamingHoops_Spawner" },
	{ "oldzapatron", "WID_Sniper_AMR_Athena_SR_Ore_T03" },
	{ "zapatron_sr", "WID_Sniper_AMR_SR_Crystal_T04" },
	{ "zapatron", "WID_Sniper_AMR_SR_Crystal_T04" },
	{ "zapatron_r", "WID_Sniper_AMR_R_Ore_T04" },
	{ "zapatron_vr", "WID_Sniper_AMR_VR_Ore_T05" },
	{ "stwpumpkin", "WID_Launcher_Pumpkin_RPG_SR_Ore_T02" },
	{ "flamethrower", "WID_Thrower_Flame_Athena_SR" },
	{ "expocrossbow", "WID_Explosive_Crossbow_Athena_SR" },
	{ "cupidcrossbow", "WID_Sniper_Valentine_Athena_VR_Ore_T03" },
	{ "huntercrossbow", "WID_Special_FiendHunter_Athena_VR_Ore_T03" },
	{ "nocturno", "WID_Assault_Auto_Founders_SR_Ore_T05" },
	{ "gravedigger", "WID_Assault_Auto_Halloween_SR_Ore_T01" },
	{ "ghostpistol", "WID_Pistol_Halloween_Handcannon_SR_Ore_T02" },
	{ "dragonclaw", "WID_Sniper_Dragon_SR_Crystal_T05" },
	{ "dragonsclaw", "WID_Sniper_Dragon_SR_Crystal_T05" },
	{ "skpistol", "WID_Pistol_Stormking_SR_Ore_T05" },
	{ "skrpg", "WID_Explosive_Stormking_SR_Ore_T05" },
	{ "skar", "WID_Assault_Stormking_SR_Ore_T05" },
	{ "sksword", "WID_Edged_Sword_Stormking_SR_Ore_T05" },
	{ "skcrystalsword", "WID_Edged_Sword_Stormking_SR_Crystal_T05" },
	{ "skhammer", "WID_Blunt_Hammer_Stormking_SR_Ore_T05" },
	{ "skcrystalhammer", "WID_Blunt_Hammer_Stormking_SR_Crystal_T05" },
	{ "gnome", "GnomeGun2" },
};

const std::unordered_map<std::string, std::string> Misc::ObjectNames = {
	{ "mechrocket", "/Game/Athena/DrivableVehicles/Mech/B_Prj_Ostrich_Rocket.B_Prj_Ostrich_Rocket_C" },
	{ "rocket", "/Game/Weapons/FORT_RocketLaunchers/Blueprints/B_Prj_Ranged_Rocket_Athena.B_Prj_Ranged_Rocket_Athena_C" },
	{ "pumpkin", "/Game/Weapons/FORT_RocketLaunchers/Blueprints/B_Prj_Pumpkin_RPG_Athena_LowTier.B_Prj_Pumpkin_RPG_Athena_LowTier_C" },
	{ "ufobeam", "/Nevada/Weapons/EnergyCannon/B_Prj_Ranged_Nevada_EnergyCannon.B_Prj_Ranged_Nevada_EnergyCannon_C" },
	{ "bone", "/Game/Characters/Enemies/Husk/Blueprints/ProjectileHuskRanged.ProjectileHuskRanged_C" },
	{ "brstormking", "/Game/Athena/DADBRO/DADBRO_Pawn.DADBRO_Pawn_C" },
	{ "stwstormking", "/Game/Characters/Enemies/DudeBro/Blueprints/DUDEBRO_Pawn.DUDEBRO_Pawn_C" },
	{ "shadowcube", "/Game/Athena/Items/ForagedItems/SpookyMist/CBGA_SpookyMist.CBGA_SpookyMist_C" },
	{ "tornado", "/Superstorm/Tornado/BP_Tornado.BP_Tornado_C" },
	{ "lightning", "/Superstorm/Lightning/BP_Lightning.BP_Lightning_C" },
	{ "woodfloor", "/Game/Building/ActorBlueprints/Player/Wood/L1/PBWA_W1_Floor.PBWA_W1_Floor_C" },
	{ "stonefloor", "/Game/Building/ActorBlueprints/Player/Stone/L1/PBWA_S1_Floor.PBWA_S1_Floor_C" },
	{ "metalfloor", "/Game/Building/ActorBlueprints/Player/Metal/L1/PBWA_M1_Floor.PBWA_M1_Floor_C" },
	{ "log", "/Game/Athena/Items/PhysicsActors/PhysicsTreeLog/BGA_PhysicsTreeLog.BGA_PhysicsTreeLog_C" },
	{ "rock", "/Game/Athena/Items/PhysicsActors/PhysicsBoulder/Prop_PhysicsBoulder_Granite.Prop_PhysicsBoulder_Granite_C" },
	{ "boulder", "/Game/Athena/Items/PhysicsActors/PhysicsBoulder/Prop_PhysicsBoulder_Granite.Prop_PhysicsBoulder_Granite_C" },
	{ "reboot", "/Game/Athena/Items/EnvironmentalItems/SCMachine/BGA_Athena_SCMachine_Redux.BGA_Athena_SCMachine_Redux_C" },
	{ "van", "/Game/Athena/Items/EnvironmentalItems/SCMachine/BGA_Athena_SCMachine_Redux.BGA_Athena_SCMachine_Redux_C" },
	{ "rebootvan", "/Game/Athena/Items/EnvironmentalItems/SCMachine/BGA_Athena_SCMachine_Redux.BGA_Athena_SCMachine_Redux_C" },
	{ "safe", "/Game/Building/ActorBlueprints/Containers/Tiered_Safe_Athena_Physics.Tiered_Safe_Athena_Physics_C" },
	{ "spiderman", "/ParallelGameplay/Environmental/ParallelChest/B_Chest_Athena_ParallelChest.B_Chest_Athena_ParallelChest_C" },
	{ "chest", "/Game/Building/ActorBlueprints/Containers/Tiered_Chest_Athena.Tiered_Chest_Athena_C" },
	{ "rarechest", "/Game/Building/ActorBlueprints/Containers/AlwaysSpawn_RareChest.AlwaysSpawn_RareChest_C" },
	{ "ammo", "/Game/Building/ActorBlueprints/Containers/Tiered_Ammo_Athena.Tiered_Ammo_Athena_C" },
	{ "ammocrate", "/Game/Building/ActorBlueprints/Containers/Tiered_Ammo_Athena.Tiered_Ammo_Athena_C" },
	{ "cooler", "/FlipperGameplay/Building/Containers/Cooler_Container.Cooler_Container_C" },
	{ "foodbox", "/Game/Building/ActorBlueprints/Containers/FoodBox_Produce_Athena.FoodBox_Produce_Athena_C" },
	{ "stwturret", "/Game/Athena/DrivableVehicles/MountedCannonTurret_STW.MountedCannonTurret_STW_C" },
	{ "movingllama", "/Labrador/Pawn/BP_AIPawn_Labrador.BP_AIPawn_Labrador_C" },
	{ "alien", "/MotherGameplay/Items/AvacadoEaterBird/BGA_Athena_AvacadoEaterBird.BGA_Athena_AvacadoEaterBird_C" },
	{ "junkrift", "/Game/Athena/Items/Consumables/JollyRascal/B_Prj_Athena_JollyRascal.B_Prj_Athena_JollyRascal_C" },
	{ "rift", "/Game/Athena/Items/ForagedItems/Rift/BGA_RiftPortal_Athena_Spawner.BGA_RiftPortal_Athena_Spawner_C" },
	{ "permrift", "/Game/Athena/Prototype/Blueprints/PermaRift/BGA_PermaRift_Athena.BGA_PermaRift_Athena_C" },
	{ "instarift", "/Game/Athena/Items/Consumables/RiftItem/BGA_RiftPortal_Item_Athena.BGA_RiftPortal_Item_Athena_C" },
	{ "booth", "/Game/Athena/Items/EnvironmentalItems/HidingProps/Props/B_HidingProp_Papaya_Booth.B_HidingProp_Papaya_Booth_C" },
	{ "skinbooth", "/Game/Athena/Items/EnvironmentalItems/HidingProps/Props/B_HidingProp_Papaya_Booth.B_HidingProp_Papaya_Booth_C" },
	{ "gaspump", "/Game/Athena/Items/EnvironmentalItems/ExplosiveProps/Apollo_GasPump_Valet.Apollo_GasPump_Valet_C" },
	{ "potty", "/Game/Athena/Items/EnvironmentalItems/HidingProps/Props/B_HidingProp_Portapotty.B_HidingProp_Portapotty_C" },
	{ "hay", "/Game/Athena/Items/EnvironmentalItems/HidingProps/Props/B_HidingProp_HayStack.B_HidingProp_HayStack_C" },
	{ "haystack", "/Game/Athena/Items/EnvironmentalItems/HidingProps/Props/B_HidingProp_HayStack.B_HidingProp_HayStack_C" },
	{ "shock", "/Game/Athena/Items/Consumables/ShockwaveGrenade/B_Prj_Athena_ShockGrenade.B_Prj_Athena_ShockGrenade_C" },
	{ "shockwave", "/Game/Athena/Items/Consumables/ShockwaveGrenade/B_Prj_Athena_ShockGrenade.B_Prj_Athena_ShockGrenade_C" },
	{ "offroadtire", "/ValetMods/Mods/TiresOffRoad/Thrown/B_Prj_ValetMod_OffRoadTire.B_Prj_ValetMod_OffRoadTire_C" },
	{ "tireitem", "/ValetMods/Mods/TiresOffRoad/Thrown/B_Prj_ValetMod_OffRoadTire.B_Prj_ValetMod_OffRoadTire_C" },
	{ "grenade", "/Game/Athena/Items/Consumables/ThrownConsumables/B_Prj_ThrownConsumable.B_Prj_ThrownConsumable_C" },
	{ "gl", "/Game/Athena/Items/Weapons/B_Prj_Ranged_GrenadeLauncher_Athena.B_Prj_Ranged_GrenadeLauncher_Athena_C" },
	{ "grenadelauncher", "/Game/Athena/Items/Weapons/B_Prj_Ranged_GrenadeLauncher_Athena.B_Prj_Ranged_GrenadeLauncher_Athena_C" },
	{ "boombox", "/Game/Athena/Items/Consumables/BoomBox/B_Proj_BoomBox.B_Proj_BoomBox_C" },
	{ "boomsniper", "/Game/Athena/Items/Weapons/WaffleTruck/Blueprint/B_Prj_WaffleTruck_BoomSniper.B_Prj_WaffleTruck_BoomSniper_C" },
	{ "fireball", "/Game/Athena/Items/EnvironmentalItems/ExplosiveProps/B_Prj_Athena_GasPump_Fireballs.B_Prj_Athena_GasPump_Fireballs_C" },
	{ "chicken", "/Irwin/AI/Prey/Nug/Pawns/NPC_Pawn_Irwin_Prey_Nug.NPC_Pawn_Irwin_Prey_Nug_C" },
	{ "boar", "/Irwin/AI/Prey/Burt/Pawns/NPC_Pawn_Irwin_Prey_Burt.NPC_Pawn_Irwin_Prey_Burt_C" },
	{ "wolf", "/Irwin/AI/Predators/Grandma/Pawns/NPC_Pawn_Irwin_Predator_Grandma.NPC_Pawn_Irwin_Predator_Grandma_C" },
	{ "raptor", "/Irwin/AI/Predators/Robert/Pawns/NPC_Pawn_Irwin_Predator_Robert.NPC_Pawn_Irwin_Predator_Robert_C" },
	{ "frog", "/Irwin/AI/Simple/Smackie/Pawns/NPC_Pawn_Irwin_Simple_Smackie.NPC_Pawn_Irwin_Simple_Smackie_C" },
	{ "crow", "/Irwin/AI/Simple/Avian/Pawns/NPC_Pawn_Irwin_Simple_Avian_Crow.NPC_Pawn_Irwin_Simple_Avian_Crow_C" },
	{ "lootcrow", "/Irwin/AI/Simple/Avian/Pawns/NPC_Pawn_Irwin_Simple_Avian_Crow_Loot.NPC_Pawn_Irwin_Simple_Avian_Crow_Loot_C" },
	{ "ioguard", "/IO_Guard/AI/Pawns/BP_IOPlayerPawn_Base.BP_IOPlayerPawn_Base_C" },
	{ "crash", "/Game/Athena/Items/Gameplay/Passives/AppleSun/BGA_AppleSun_Apple_Athena.BGA_AppleSun_Apple_Athena_C" },
	{ "crashpad", "/Game/Athena/Items/Gameplay/Passives/AppleSun/BGA_AppleSun_Apple_Athena.BGA_AppleSun_Apple_Athena_C" },
	{ "glitchconsumable", "/Game/Athena/Items/ForagedItems/Glitch/CBGA_Glitch.CBGA_Glitch_C" },
	{ "s10supplydrone", "/Game/Athena/Items/Consumables/SuperDingo/BGA_Athena_SuperDingo.BGA_Athena_SuperDingo_C" },
	{ "s10drone", "/Game/Athena/Items/Consumables/SuperDingo/BGA_Athena_SuperDingo.BGA_Athena_SuperDingo_C" },
	{ "drone", "/GoldenPOI/Gameplay/SuperDingo/BGA_Athena_SuperDingo_GoldenPOI.BGA_Athena_SuperDingo_GoldenPOI_C" },
	{ "supplydrone", "/GoldenPOI/Gameplay/SuperDingo/BGA_Athena_SuperDingo_GoldenPOI.BGA_Athena_SuperDingo_GoldenPOI_C" },
	{ "s17rune", "/MotherGameplay/Items/Alpaca/BGA_Alpaca_AbductedPOI.BGA_Alpaca_AbductedPOI_C" },
	{ "driftboard", "/Game/Athena/DrivableVehicles/JackalVehicle_Athena.JackalVehicle_Athena_C" },
	{ "hoverboard", "/Game/Athena/DrivableVehicles/JackalVehicle_Athena.JackalVehicle_Athena_C" },
	{ "surfboard", "/Game/Athena/DrivableVehicles/SurfboardVehicle_Athena.SurfboardVehicle_Athena_C" },
	{ "quadcrasher", "/Game/Athena/DrivableVehicles/AntelopeVehicle.AntelopeVehicle_C" },
	{ "quad", "/Game/Athena/DrivableVehicles/AntelopeVehicle.AntelopeVehicle_C" },
	{ "baller", "/Game/Athena/DrivableVehicles/Octopus/OctopusVehicle.OctopusVehicle_C" },
	{ "plane", "/Game/Athena/DrivableVehicles/Biplane/BluePrints/FerretVehicle.FerretVehicle_C" },
	{ "shopping", "/Game/Athena/DrivableVehicles/ShoppingCartVehicleSK.ShoppingCartVehicleSK_C" },
	{ "cart", "/Game/Athena/DrivableVehicles/ShoppingCartVehicleSK.ShoppingCartVehicleSK_C" },
	{ "atk", "/Game/Athena/DrivableVehicles/Golf_Cart/Golf_Cart_Base/Blueprints/GolfCartVehicleSK.GolfCartVehicleSK_C" },
	{ "golfcart", "/Game/Athena/DrivableVehicles/Golf_Cart/Golf_Cart_Base/Blueprints/GolfCartVehicleSK.GolfCartVehicleSK_C" },
	{ "cannon", "/Game/Athena/DrivableVehicles/PushCannon.PushCannon_C" },
	{ "mech", "/Game/Athena/DrivableVehicles/Mech/TestMechVehicle.TestMechVehicle_C" },
	{ "brute", "/Game/Athena/DrivableVehicles/Mech/TestMechVehicle.TestMechVehicle_C" },
	{ "bear", "/Valet/BasicTruck/Valet_BasicTruck_Vehicle.Valet_BasicTruck_Vehicle_C" },
	{ "truck", "/Valet/BasicTruck/Valet_BasicTruck_Vehicle.Valet_BasicTruck_Vehicle_C" },
	{ "prevelant", "/Valet/BasicCar/Valet_BasicCar_Vehicle.Valet_BasicCar_Vehicle_C" },
	{ "car", "/Valet/BasicCar/Valet_BasicCar_Vehicle.Valet_BasicCar_Vehicle_C" },
	{ "whiplash", "/Valet/SportsCar/Valet_SportsCar_Vehicle.Valet_SportsCar_Vehicle_C" },
	{ "sportscar", "/Valet/SportsCar/Valet_SportsCar_Vehicle.Valet_SportsCar_Vehicle_C" },
	{ "taxi", "/Valet/TaxiCab/Valet_TaxiCab_Vehicle.Valet_TaxiCab_Vehicle_C" },
	{ "mudflap", "/Valet/BigRig/Valet_BigRig_Vehicle.Valet_BigRig_Vehicle_C" },
	{ "boat", "/Game/Athena/DrivableVehicles/Meatball/Meatball_Large/MeatballVehicle_L.MeatballVehicle_L_C" },
	{ "heli", "/Hoagie/HoagieVehicle.HoagieVehicle_C" },
	{ "helicopter", "/Hoagie/HoagieVehicle.HoagieVehicle_C" },
	{ "ufo", "/Nevada/Blueprints/Vehicle/Nevada_Vehicle_V2.Nevada_Vehicle_V2_C" },
	{ "ferrari", "/Foray/Vehicle/Foray_Vehicle.Foray_Vehicle_C" },
	{ "armoredbattlebus", "/ArmoredBattleBus/Vehicle/ArmoredBattleBus_Vehicle.ArmoredBattleBus_Vehicle_C" },
	{ "armoredbus", "/ArmoredBattleBus/Vehicle/ArmoredBattleBus_Vehicle.ArmoredBattleBus_Vehicle_C" },
	{ "battlebus", "/Game/Athena/Aircraft/AthenaAircraft.AthenaAircraft_C" },
	{ "warbus", "/MoonFlaxWarBus/Gameplay/Vehicle/MoonFlaxWarBus_Vehicle.MoonFlaxWarBus_Vehicle_C" },
	{ "tank", "/Tank/Vehicle/Tank_Vehicle.Tank_Vehicle_C" },
	{ "octane", "/RockVehicleBR/Vehicle/Rock_Vehicle_BR.Rock_Vehicle_BR_C" },
	{ "rocketracing", "/RockVehicle/Rock_Vehicle.Rock_Vehicle_C" },
	{ "racing", "/RockVehicle/Rock_Vehicle.Rock_Vehicle_C" },
	{ "dirtbike", "/Dirtbike/Vehicle/Motorcycle_DirtBike_Vehicle.Motorcycle_DirtBike_Vehicle_C" },
	{ "supplydrop", "/Game/Athena/SupplyDrops/AthenaSupplyDrop.AthenaSupplyDrop_C" },
	{ "drop", "/Game/Athena/SupplyDrops/AthenaSupplyDrop.AthenaSupplyDrop_C" },
	{ "shark", "/SpicySake/Pawns/NPC_Pawn_SpicySake_Parent.NPC_Pawn_SpicySake_Parent_C" },
	{ "klombo", "/ButterCake/Pawns/NPC_Pawn_ButterCake_Base.NPC_Pawn_ButterCake_Base_C" },
	{ "umbrella", "/Game/Athena/Apollo/Environments/BuildingActors/Papaya/Papaya_BouncyUmbrella_C.Papaya_BouncyUmbrella_C_C" },
	{ "dumpster", "/Game/Athena/Items/EnvironmentalItems/HidingProps/Props/B_HidingProp_Dumpster.B_HidingProp_Dumpster_C" },
	{ "trash", "/Game/Athena/Items/EnvironmentalItems/HidingProps/Props/B_HidingProp_Dumpster.B_HidingProp_Dumpster_C" },
	{ "trashbin", "/Game/Athena/Items/EnvironmentalItems/HidingProps/Props/B_HidingProp_Dumpster.B_HidingProp_Dumpster_C" },
	{ "tire", "/Game/Building/ActorBlueprints/Prop/Prop_TirePile_04.Prop_TirePile_04_C" },
	{ "llama", "/Game/Athena/SupplyDrops/Llama/AthenaSupplyDrop_Llama.AthenaSupplyDrop_Llama_C" },
	{ "paf", "/Game/Athena/Items/Consumables/TowerGrenade/Prop_TirePile_Tower.Prop_TirePile_Tower_C" },
	{ "airvent", "/Game/Athena/Environments/Blueprints/DUDEBRO/BGA_HVAC.BGA_HVAC_C" },
	{ "vent", "/Game/Athena/Environments/Blueprints/DUDEBRO/BGA_HVAC.BGA_HVAC_C" },
	{ "geyser", "/Game/Athena/Environments/Blueprints/DudeBro/BGA_DudeBro_Mini.BGA_DudeBro_Mini_C" },
	{ "ch1zeropoint", "/Game/Athena/Environments/Nexus/Blueprints/BP_ZeroPoint_2Point0.BP_ZeroPoint_2Point0_C" },
	{ "ch2zeropoint", "/Game/Athena/Environments/Nexus/Blueprints/BP_ZeroPoint_Exploding.BP_ZeroPoint_Exploding_C" },
	{ "rune", "/Game/Athena/Prototype/Blueprints/Cube/BGA_Cube_Area_Effect.BGA_Cube_Area_Effect_C" },
	{ "kevin", "/Game/Creative/BuildingActors/Props/CP_Kevin_Cube_02.CP_Kevin_Cube_02_C" },
	{ "meteor", "/Game/Creative/BuildingActors/Props/CP_Prop_Meteor_01.CP_Prop_Meteor_01_C" },
	{ "kineticrock", "/Fortanium/Prop_PhysicsBoulder_Fortanium.Prop_PhysicsBoulder_Fortanium_C" },
	{ "kinetic", "/Fortanium/Prop_PhysicsBoulder_Fortanium.Prop_PhysicsBoulder_Fortanium_C" },
	{ "ufobouncer", "/Game/Athena/Environments/Blueprints/EnvironmentalLaunch/Generic/BGA_DirectionalLaunch_UFO.BGA_DirectionalLaunch_UFO_C" },
	{ "gascan", "/Game/Athena/Items/Weapons/Prototype/PetrolPump/BGA_Petrol_Pickup.BGA_Petrol_Pickup_C" },
	{ "gas", "/Game/Athena/Items/Weapons/Prototype/PetrolPump/BGA_Petrol_Pickup.BGA_Petrol_Pickup_C" },
};

int Misc::GetNetMode()
{
	// CH5 (UE5.4+) runs as a LISTEN server (the injecting client is also the host player).
	// Returning NM_ListenServer(2) — not NM_DedicatedServer(1) — makes the engine create the
	// local host player so the client actually enters the world instead of sitting on "Setting
	// up the server" forever, while still being != NM_Client(3) so the Iris-worker null-deref
	// stays fixed.
	//
	// Applies from 5.4, not just 32.11: on CH5 ReadyToStartMatch is a native call, so the match
	// only starts once a PlayerController exists. The dedicated model gives none unless a second
	// client connects, which is exactly the 31.41 stall (conns=0, MatchState stuck WaitingToStart).
	if (VersionInfo.EngineVersion >= 5.4 && !FConfiguration::UseCH5DedicatedModel())
		return 2;
	return 1;
}

void* Misc::SendRequestNow(void* Arg1, void* MCPData, int)
{
	if (VersionInfo.EngineVersion < 4.23)
		*(int*)(__int64(MCPData) + (VersionInfo.FortniteVersion >= 4.2 ? 0x28 : 0x60)) = 3; // CXC_Public

	return SendRequestNowOG(Arg1, MCPData, 3); // CXC_Public
}

bool Listen(); // fwd decl — defined below; driven from GetMaxTickRate on 32.11

static volatile long g_frameCounter = 0;
static DWORD g_gameThreadId = 0;

static uint64 ResolveGetMaxTickRateTarget()
{
	// 32.11 is the one UE5 build with a verified RVA. Other builds must use
	// their signature result rather than risking an early hook at a stale RVA.
	if (VersionInfo.FortniteVersion == 32.11)
		return Memcury::PE::GetModuleBase() + 0x189FE98;

	return FindGetMaxTickRate();
}
extern volatile long g_tickFlushCounter; // NetDriver.cpp — replication flushes; confirms the net tick is alive

// Watchdog: writes directly to its own file (own fflush) since the game thread that flushes the shared
// debug buffer is the one that hangs. Logs a heartbeat + the RIP where the game thread is stuck.
static void WDLog(const char* fmt, ...)
{
	FILE* f = nullptr;
	fopen_s(&f, "G:\\Fortnite Builds\\32.11\\FortniteGame\\Binaries\\Win64\\watchdog.log", "a");
	if (!f) return;
	va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
	fflush(f); fclose(f);
}
static void WatchdogThread()
{
	WDLog("[WD] started\n");
	long last = 0; int stuck = 0; bool reported = false; int hb = 0;
	while (true)
	{
		Sleep(500);
		long cur = g_frameCounter;
		long delta = cur - last; last = cur;
		if ((hb++ % 4) == 0) WDLog("[WD] hb frame=%ld delta=%ld tickflush=%ld\n", cur, delta, g_tickFlushCounter);
		if (delta <= 2 && g_gameThreadId && !reported) // <= ~4 FPS = effectively frozen (crawl)
		{
			if (++stuck >= 3) // ~1.5s of crawling
			{
				reported = true;
				auto base = Memcury::PE::GetModuleBase();
				HANDLE h = OpenThread(THREAD_ALL_ACCESS, FALSE, g_gameThreadId);
				WDLog("[WD] HANG detected frame=%ld tid=%lu base=%p\n", cur, g_gameThreadId, (void*)base);
				if (h)
				{
					SuspendThread(h);
					CONTEXT ctx; memset(&ctx, 0, sizeof(ctx)); ctx.ContextFlags = CONTEXT_CONTROL;
					if (GetThreadContext(h, &ctx))
					{
						WDLog("[WD] RIP=+0x%llX (RIP=%p RSP=%p)\n", (uint64_t)(ctx.Rip - base), (void*)ctx.Rip, (void*)ctx.Rsp);
						// Walk the stack for return addresses inside the Fortnite module (the call chain).
						uint64_t* sp = (uint64_t*)ctx.Rsp;
						int found = 0;
						for (int i = 0; i < 0x1000 && found < 24; i++)
						{
							if (!SDK::MemReadable(&sp[i], 8)) continue;
							uint64_t v = sp[i];
							if (v >= base && v < base + 0x10000000)
							{
								WDLog("[WD]   stack+0x%X -> +0x%llX\n", i * 8, v - base);
								found++;
							}
						}
					}
					ResumeThread(h);
					CloseHandle(h);
				}
				// Snapshot every thread's RIP — the busy worker (RIP in a hot module func) is the real grind.
				WDLog("[WD] --- all-thread RIP scan ---\n");
				HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
				if (snap != INVALID_HANDLE_VALUE)
				{
					THREADENTRY32 te; te.dwSize = sizeof(te);
					DWORD myPid = GetCurrentProcessId();
					if (Thread32First(snap, &te))
					{
						do {
							if (te.th32OwnerProcessID != myPid) continue;
							if (te.th32ThreadID == GetCurrentThreadId()) continue; // skip watchdog itself
							HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
							if (!th) continue;
							SuspendThread(th);
							CONTEXT c; memset(&c, 0, sizeof(c)); c.ContextFlags = CONTEXT_CONTROL;
							if (GetThreadContext(th, &c))
							{
								if (c.Rip >= base && c.Rip < base + 0x10000000)
									WDLog("[WD] tid=%lu RIP=+0x%llX  <-- in module\n", te.th32ThreadID, (uint64_t)(c.Rip - base));
							}
							ResumeThread(th);
							CloseHandle(th);
						} while (Thread32Next(snap, &te));
					}
					CloseHandle(snap);
				}
				WDLog("[WD] --- scan done ---\n");
			}
		}
		else { stuck = 0; last = cur; }
	}
}

bool Misc::InstallPreStartSafeZoneTick()
{
	if (bSafeZoneTickHookInstalled)
		return true;

	const uint64 Target = ResolveGetMaxTickRateTarget();
	if (!Target || !SDK::MemReadable(reinterpret_cast<void*>(Target), 1))
	{
		SDK::DbgLog("[SafeZoneMap] pre-Start GetMaxTickRate target unavailable\n");
		return false;
	}

	const auto InitializeStatus = MH_Initialize();
	if (InitializeStatus != MH_OK && InitializeStatus != MH_ERROR_ALREADY_INITIALIZED)
	{
		SDK::DbgLog(
			"[SafeZoneMap] pre-Start hook MH_Initialize failed: %s\n",
			MH_StatusToString(InitializeStatus));
		return false;
	}

	const auto CreateStatus = MH_CreateHook(
		reinterpret_cast<LPVOID>(Target),
		SafeZoneTickGetMaxTickRate,
		reinterpret_cast<LPVOID*>(&SafeZoneTickGetMaxTickRateOG));
	if (CreateStatus != MH_OK)
	{
		SDK::DbgLog(
			"[SafeZoneMap] pre-Start hook MH_CreateHook failed: %s\n",
			MH_StatusToString(CreateStatus));
		return false;
	}

	const auto EnableStatus = MH_EnableHook(reinterpret_cast<LPVOID>(Target));
	if (EnableStatus != MH_OK && EnableStatus != MH_ERROR_ENABLED)
	{
		SDK::DbgLog(
			"[SafeZoneMap] pre-Start hook MH_EnableHook failed: %s\n",
			MH_StatusToString(EnableStatus));
		MH_RemoveHook(reinterpret_cast<LPVOID>(Target));
		SafeZoneTickGetMaxTickRateOG = nullptr;
		return false;
	}

	SafeZoneTickHookTarget = Target;
	bSafeZoneTickHookInstalled = true;
	SDK::DbgLog(
		"[SafeZoneMap] pre-Start game-thread pump installed at +0x%llX\n",
		Target - Memcury::PE::GetModuleBase());
	return true;
}

float Misc::SafeZoneTickGetMaxTickRate(
	UEngine* Engine,
	float DeltaTime,
	bool bAllowFrameRateSmoothing)
{
	if (!bServerGetMaxTickRateActive.load(std::memory_order_acquire))
	{
		GUI::SafeZoneMapGameTick();
		return SafeZoneTickGetMaxTickRateOG
			? SafeZoneTickGetMaxTickRateOG(Engine, DeltaTime, bAllowFrameRateSmoothing)
			: FConfiguration::MaxTickRate;
	}

	// Keep using the already-installed hook after Start. This preserves the
	// button boundary while avoiding a second MinHook entry on the same target.
	return GetMaxTickRate(Engine, DeltaTime, bAllowFrameRateSmoothing);
}

void Misc::ActivateServerGetMaxTickRate()
{
	bServerGetMaxTickRateActive.store(true, std::memory_order_release);
	if (bSafeZoneTickHookInstalled)
		SDK::DbgLog("[SafeZoneMap] pre-Start pump switched to server GetMaxTickRate behavior\n");
}

float Misc::GetMaxTickRate(UEngine* Engine, float DeltaTime, bool bAllowFrameRateSmoothing)
{
	// Drain the Custom Safe Zone minimap load request here rather than only in
	// TickFlush: GetMaxTickRate runs on the game thread every frame from engine
	// init, so the load also works while the user is still in the pre-launch
	// GUI (TickFlush only starts once the server is listening).
	GUI::SafeZoneMapGameTick();

	if (VersionInfo.FortniteVersion >= 32.00)
	{
		g_gameThreadId = GetCurrentThreadId();
		_InterlockedIncrement(&g_frameCounter);
	}
	// 32.11: LoadMap's UWorld::Listen is inlined (no callable 0x2C283E0), so hooking it never fires.
	// Drive the manual Listen from this per-frame game-thread hook once the athena game mode is up and
	// no net driver exists yet. SetWorld (0x29664C8) is the correct RVA (Remix uses it).
	if (VersionInfo.FortniteVersion >= 32.00)
	{
		static bool s_listened = false;
		static int  s_frames = 0;
		if (!s_listened)
		{
			auto World = UWorld::GetWorld();
			static auto FrontendMode = FindClass("FortGameModeFrontend");
			if (World && World->AuthorityGameMode && !World->NetDriver &&
				(!FrontendMode || !World->AuthorityGameMode->IsA(FrontendMode)))
			{
				if (++s_frames > 30)
				{
					s_listened = true;
					SDK::DbgLog("[Misc] GetMaxTickRate: triggering FN32 Listen\n");
					bool ok = Listen();
					SDK::DbgLog("[Misc] GetMaxTickRate: Listen() returned %d\n", (int)ok);
				}
			}
		}
	}
	// improper, DS is supposed to do hitching differently
	return FConfiguration::MaxTickRate;
	//return std::clamp(1.f / DeltaTime, 1.f, FConfiguration::MaxTickRate);
}

uint32 Misc::CheckCheckpointHeartBeat()
{
	return -1;
}

void Misc::ApplyHomebaseEffectsOnPlayerSetup(
	__int64* a1,
	__int64 a2,
	__int64 a3,
	__int64 a4,
	UObject* a5,
	char a6,
	unsigned __int8 a7)
{
	auto GameMode = (AFortGameModeAthena*) UWorld::GetWorld()->AuthorityGameMode;
	if (GameMode->HasWarmupRequiredPlayerCount())
	{
		static auto ItemDefOffset = a5->GetOffset("ItemDefinition");
		static auto Commando = FindObject<UObject>(L"/Game/Athena/Heroes/HID_001_Athena_Commando_F.HID_001_Athena_Commando_F");
		static auto Commando2 = FindObject<UObject>(L"/Game/Athena/Heroes/HID_Commando_Athena_01.HID_Commando_Athena_01");
		GetFromOffset<const UObject*>(a5, ItemDefOffset) = Commando ? Commando : Commando2;
	}

	return ApplyHomebaseEffectsOnPlayerSetupOG(a1, a2, a3, a4, a5, a6, a7);
}

bool bEOREnabled = false;
inline void* (*SelectResetOG)(void*) = nullptr;
inline void* (*SelectEditOG)(void*) = nullptr;
inline char (*CompleteBuildingEditInteraction)(void*) = nullptr;

void* SelectEdit(void* a1)
{
	void* result = SelectEditOG(a1);

	if (bEOREnabled)
		CompleteBuildingEditInteraction(a1);

	return result;
}

void* SelectReset(void* a1)
{
	void* result = SelectResetOG(a1);

	if (bEOREnabled)
		CompleteBuildingEditInteraction(a1);

	return result;
}

class UIpNetDriver : public UNetDriver
{
public:
	UCLASS_COMMON_MEMBERS(UIpNetDriver);
};

void PatchAllNetModes(uintptr_t AttemptDeriveFromURL)
{
	Memcury::PE::Address add{ nullptr };

	const auto sizeOfImage = Memcury::PE::GetNTHeaders()->OptionalHeader.SizeOfImage;
	const auto scanBytes = reinterpret_cast<std::uint8_t*>(Memcury::PE::GetModuleBase());

	for (auto i = 0ul; i < sizeOfImage - 5; ++i)
	{
		if (scanBytes[i] == 0xE8 || scanBytes[i] == 0xE9)
		{
			if (Memcury::PE::Address(&scanBytes[i]).RelativeOffset(1).GetAs<void*>() == (void*)AttemptDeriveFromURL)
			{
				add = Memcury::PE::Address(&scanBytes[i]);

				// scan for the read of World->NetDriver

				for (auto j = 0; j > -0x100000; j--) // so we find everything. no func is actually 1mb
				{
					if ((scanBytes[i + j] & 0xF8) == 0x48 && ((scanBytes[i + j + 1] & 0xFC) == 0x80 || (scanBytes[i + j + 1] & 0xF8) == 0x38) && (scanBytes[i + j + 2] & 0xF0) != 0xC0 && (scanBytes[i + j + 2] & 0xF0) != 0xE0 && scanBytes[i + j + 2] != 0x65 && scanBytes[i + j + 2] != 0xBB && scanBytes[i + j + 3] == 0x38 && ((scanBytes[i + j + 1] & 0xFC) != 0x80 || scanBytes[i + j + 4] == 0x0))
					{
						// now, scan for if (NetDriver) return NM_Client;

						bool found = false;
						for (auto k = 4; k < 0x104; k++)
						{
							if (scanBytes[i + j + k] == 0x75)
							{
								auto Scuffness = __int64(&scanBytes[i + j + k + 5]);

								if (*(uint32_t*)Scuffness != 0xF0 && (scanBytes[i + j + k + 4] != 0xC || scanBytes[i + j + k + 5] != 0xB) && scanBytes[i + j + k + 4] != 0x09)
									continue;

								Utils::Patch<uint16_t>(__int64(&scanBytes[i + j + k]), 0x9090);
								if ((scanBytes[i + j + 1] & 0xF8) == 0x38)
									Utils::Patch<uint32_t>(__int64(&scanBytes[i + j]), 0x90909090);
								else if ((scanBytes[i + j + 1] & 0xFC) == 0x80)
								{
									DWORD og;
									VirtualProtect(&scanBytes[i + j], 5, PAGE_EXECUTE_READWRITE, &og);
									*(uint32*)(&scanBytes[i + j]) = 0x90909090;
									*(uint8*)(&scanBytes[i + j + 4]) = 0x90;
									VirtualProtect(&scanBytes[i + j], 5, og, &og);
								}
								FlushInstructionCache(GetCurrentProcess(), &scanBytes[i + j], 5);
								FlushInstructionCache(GetCurrentProcess(), &scanBytes[i + j + k], 2);
								found = true;
								break;
							}
							else if (scanBytes[i + j + k] == 0x74)	
							{
								auto Scuffness = __int64(&scanBytes[i + j + k]);
								Scuffness = (Scuffness + 2) + *(int8_t*)(Scuffness + 1);
								
								if (*(uint32_t*)(Scuffness + 3) != 0xF0 && (*(uint8_t*)(Scuffness + 2) != 0xC || *(uint8_t*)(Scuffness + 3) != 0xB) && *(uint8_t*)(Scuffness + 2) != 0x09)
									continue;

								Utils::Patch<uint8_t>(__int64(&scanBytes[i + j + k]), 0xeb);
								if ((scanBytes[i + j + 1] & 0xF8) == 0x38)
									Utils::Patch<uint32_t>(__int64(&scanBytes[i + j]), 0x90909090);
								else if ((scanBytes[i + j + 1] & 0xFC) == 0x80)
								{
									DWORD og;
									VirtualProtect(&scanBytes[i + j], 5, PAGE_EXECUTE_READWRITE, &og);
									*(uint32*)(&scanBytes[i + j]) = 0x90909090;
									*(uint8*)(&scanBytes[i + j + 4]) = 0x90;
									VirtualProtect(&scanBytes[i + j], 5, og, &og);
								}
								FlushInstructionCache(GetCurrentProcess(), &scanBytes[i + j], 5);
								FlushInstructionCache(GetCurrentProcess(), &scanBytes[i + j + k], 1);
								found = true;
								break;
							}
							else if (scanBytes[i + j + k] == 0x0F  && scanBytes[i + j + k + 1] == 0x85)
							{
								auto Scuffness = __int64(&scanBytes[i + j + k + 9]);

								if (*(uint32_t*)Scuffness != 0xF0 && (scanBytes[i + j + k + 8] != 0xC || scanBytes[i + j + k + 9] != 0xB) && scanBytes[i + j + k + 8] != 0x09)
									continue;

								DWORD og;
								VirtualProtect(&scanBytes[i + j + k], 6, PAGE_EXECUTE_READWRITE, &og);
								*(uint32*)(&scanBytes[i + j + k]) = 0x90909090;
								*(uint16*)(&scanBytes[i + j + k + 4]) = 0x9090;
								VirtualProtect(&scanBytes[i + j + k], 6, og, &og);
								if ((scanBytes[i + j + 1] & 0xF8) == 0x38)
									Utils::Patch<uint32_t>(__int64(&scanBytes[i + j]), 0x90909090);
								else if ((scanBytes[i + j + 1] & 0xFC) == 0x80)
								{
									DWORD og;
									VirtualProtect(&scanBytes[i + j], 5, PAGE_EXECUTE_READWRITE, &og);
									*(uint32*)(&scanBytes[i + j]) = 0x90909090;
									*(uint8*)(&scanBytes[i + j + 4]) = 0x90;
									VirtualProtect(&scanBytes[i + j], 5, og, &og);
								}
								FlushInstructionCache(GetCurrentProcess(), &scanBytes[i + j], 5);
								FlushInstructionCache(GetCurrentProcess(), &scanBytes[i + j + k], 6);
								found = true;
								break;
							}
							else if (scanBytes[i + j + k] == 0x0F && scanBytes[i + j + k + 1] == 0x84)
							{
								auto Scuffness = __int64(&scanBytes[i + j + k]);
								Scuffness = (Scuffness + 6) + *(int32_t*)(Scuffness + 2);

								if (*(uint32_t*)(Scuffness + 3) != 0xF0 && (*(uint8_t*)(Scuffness + 2) != 0xC || *(uint8_t*)(Scuffness + 3) != 0xB) && *(uint8_t*)(Scuffness + 2) != 0x09)
									continue;

								Utils::Patch<uint16_t>(__int64(&scanBytes[i + j + k]), 0xe990);
								if ((scanBytes[i + j + 1] & 0xF8) == 0x38)
									Utils::Patch<uint32_t>(__int64(&scanBytes[i + j]), 0x90909090);
								else if ((scanBytes[i + j + 1] & 0xFC) == 0x80)
								{
									DWORD og;
									VirtualProtect(&scanBytes[i + j], 5, PAGE_EXECUTE_READWRITE, &og);
									*(uint32*)(&scanBytes[i + j]) = 0x90909090;
									*(uint8*)(&scanBytes[i + j + 4]) = 0x90;
									VirtualProtect(&scanBytes[i + j], 5, og, &og);
								}
								FlushInstructionCache(GetCurrentProcess(), &scanBytes[i + j], 5);
								FlushInstructionCache(GetCurrentProcess(), &scanBytes[i + j + k], 2);
								found = true;
								break;
							}
						}
						if (found)
							break;
					}
				}
			}
		}
	}
}

bool RetFalse()
{
	return false;
}

class AFortTeamMemberPedestal : public AActor
{
public:
	UCLASS_COMMON_MEMBERS(AFortTeamMemberPedestal);
};

__int64 (*CrashSomethingOG)(__int64 a1, __int64 a2);
__int64 CrashSomething(__int64 a1, __int64 a2)
{
	if (!a1)
		return 0;

	return CrashSomethingOG(a1, a2);
}

class AFortLightweightProjectileManager : public AActor
{
public:
	UCLASS_COMMON_MEMBERS(AFortLightweightProjectileManager);
};

class AFortLightweightProjectileConfig : public AActor
{
public:
	UCLASS_COMMON_MEMBERS(AFortLightweightProjectileConfig);

	DEFINE_PROP(Speed, FScalableFloat);
	DEFINE_PROP(GravityScale, FScalableFloat);
};

struct FSpawnProjectileParams
{
public:
	USCRIPTSTRUCT_COMMON_MEMBERS(FSpawnProjectileParams);

	DEFINE_STRUCT_PROP(SpawnLocation, FVector);
	DEFINE_STRUCT_PROP(SpawnDirection, FRotator);
	DEFINE_STRUCT_PROP(OptionalAssociatedItemDef, UFortItemDefinition*);
	DEFINE_STRUCT_PROP(InitialSpeed, float);
	DEFINE_STRUCT_PROP(MaxSpeed, float);
	DEFINE_STRUCT_PROP(GravityScale, float);
};

class AFortProjectileAthena : public AActor
{
public:
	UCLASS_COMMON_MEMBERS(AFortProjectileAthena);
};

void (*TestOG)(AFortLightweightProjectileManager* ProjectileManager, TWeakObjectPtr<AFortPlayerPawnAthena> WeakPawn, TWeakObjectPtr<AFortWeapon> WeakWeapon, TSubclassOf<AFortLightweightProjectileConfig>& ConfigClass, FVector Location, FVector Direction, uint8_t Type, int a8, int a9);
void Test(AFortLightweightProjectileManager* ProjectileManager, TWeakObjectPtr<AFortPlayerPawnAthena> WeakPawn, TWeakObjectPtr<AFortWeapon> WeakWeapon, TSubclassOf<AFortLightweightProjectileConfig>& ConfigClass, FVector Location, FVector Direction, uint8_t Type, int a8, int a9)
{
	TestOG(ProjectileManager, WeakPawn, WeakWeapon, ConfigClass, Location, Direction, Type, a8, a9);

	auto Config = (AFortLightweightProjectileConfig*)ConfigClass->GetDefaultObj();

	auto Params = (FSpawnProjectileParams*)malloc(FSpawnProjectileParams::Size());
	memset(Params, 0, FSpawnProjectileParams::Size());

	Params->SpawnLocation = Location;

	const double RAD_TO_DEG = 57.29577951308232;

	auto Weapon = WeakWeapon.Get();

	auto Pitch = asin(Direction.Z) * RAD_TO_DEG;
	auto Yaw = atan2(Direction.Y, Direction.X) * RAD_TO_DEG;
	Params->SpawnDirection.Pitch = Pitch;
	Params->SpawnDirection.Yaw = Yaw;
	Params->SpawnDirection.Roll = 0;
	Params->OptionalAssociatedItemDef = Weapon->WeaponData;
	Params->InitialSpeed = Config->Speed.Evaluate();
	Params->MaxSpeed = Params->InitialSpeed;
	Params->GravityScale = Config->GravityScale.Evaluate();

	UFortKismetLibrary::SpawnProjectileWithParams(AFortProjectileAthena::StaticClass(), Weapon, *Params);
	free(Params);
}

struct FTickFunction
{
public:
	USCRIPTSTRUCT_COMMON_MEMBERS(FTickFunction);

	DEFINE_STRUCT_BITFIELD_PROP(bAllowTickOnDedicatedServer);
};

void (*Test2OG)(FTickFunction* _this, ULevel* Level);
void Test2(FTickFunction* _this, ULevel* Level)
{
	// bAllowTickOnDedicatedServer: FTickFunction+0xA bit3 (Remix layout). Use the raw offset on 32.11
	// where the struct's encrypted reflection can't resolve the bitfield.
	bool allow = VersionInfo.FortniteVersion >= 32.00
		? (*(uint8_t*)((char*)_this + 0xA) & 8) != 0
		: _this->bAllowTickOnDedicatedServer;
	if (!allow)
		return;
	return Test2OG(_this, Level);
}

char GetTickableTickType_FN32()
{
	return 2; // ETickableTickType::Never — matches Remix (disables client-side tickables on the server)
}

// Better-Remix CreateAndConfigureNavigationSystem: configure the Athena nav-system config so the server
// actually builds nav data (stdout shows nav creation failing) instead of retrying. Offsets from the
// UAthenaNavSystemConfig SDK layout; class not present in Magnesium so set by raw offset.
void (*CreateAndConfigureNavigationSystem_OG)(void* ModuleConfig, void* World);
void CreateAndConfigureNavigationSystem_FN32(void* ModuleConfig, void* World)
{
	if (ModuleConfig)
	{
		*(uint8_t*)((char*)ModuleConfig + 0x70) |= 0x40;  // bPrioritizeNavigationAroundSpawners = true
		*(uint8_t*)((char*)ModuleConfig + 0x50) |= 0x04;  // bAutoSpawnMissingNavData = true
		*(uint8_t*)((char*)ModuleConfig + 0x58) |= 0x01;  // bAllowAutoRebuild = true
		*(uint8_t*)((char*)ModuleConfig + 0x71) &= ~0x01; // bSupportRuntimeNavmeshDisabling = false
	}
	CreateAndConfigureNavigationSystem_OG(ModuleConfig, World);
}

// Better-Remix RegisterToLivingWorldManager: installed as a COMPLETE REPLACEMENT (never calls the
// original). Actors registering to the LivingWorldManager are what make its per-frame world tick
// crawl (stdout: "FortWorldManager ...[TickActor] took 204.65ms!"). By no-oping the registration the
// manager stays idle and the server holds full tickrate. Better-Remix additionally hand-spawns Clyde
// vehicles here; that path needs Ch5S4 vehicle SDK types Magnesium lacks, so it's skipped — the map
// just has no living-world vehicles, which is fine for a basic BR server.
void RegisterToLivingWorldManager_FN32(void* IFace, void* Actor)
{
	// intentionally empty — skip LivingWorldManager registration entirely
}

void Ohio(ABuildingProp_LockDevice* _this, AFortPlayerControllerAthena* ControllerInstigator)
{
	printf("Called[LockProp: %s]\n", _this->Name.ToString().c_str());
}

bool Listen()
{
	printf("UWorld::Listen\n");
	auto World = UWorld::GetWorld();
	auto Engine = UEngine::GetEngine();
	auto NetDriverName = FName(L"GameNetDriver");
	auto GameMode = (AFortGameModeAthena*)World->AuthorityGameMode;

	// 32.11 uses Iris; do not force the replication-graph path (matches Remix, which leaves it alone).
	if (GameMode->HasbEnableReplicationGraph() && VersionInfo.FortniteVersion < 32.00)
		GameMode->bEnableReplicationGraph = true;

	UNetDriver* NetDriver = nullptr;
	if (VersionInfo.FortniteVersion >= 16.00)
	{
		void* WorldCtx = ((void* (*)(UEngine*, UWorld*)) FindGetWorldContext())(Engine, World);
		World->NetDriver = NetDriver = ((UNetDriver * (*)(UEngine*, void*, FName, int)) FindCreateNetDriverWorldContext())(Engine, WorldCtx, NetDriverName, 0);
	}
	else
		World->NetDriver = NetDriver = ((UNetDriver * (*)(UEngine*, UWorld*, FName)) FindCreateNetDriver())(Engine, World, NetDriverName);

	if (!NetDriver)
		return false;

	if (VersionInfo.FortniteVersion >= 20)
		NetDriver->NetServerMaxTickRate = 30;

	NetDriver->NetDriverName = NetDriverName;
	NetDriver->World = World;

	if (VersionInfo.EngineVersion >= 5.3 && FConfiguration::bEnableIris && VersionInfo.FortniteVersion < 32.00)
	{
		*(bool*)(__int64(&NetDriver->ReplicationDriver) + 0x11) = true;
	}

	NetDriver->NetDriverName = NetDriverName;
	NetDriver->World = World;

	auto InitListen = (bool (*)(UNetDriver*, UWorld*, FURL*, bool, FString&)) FindInitListen();
	auto SetWorld = (void (*)(UNetDriver*, UWorld*)) FindSetWorld();
	SDK::DbgLog("[Listen] NetDriver=%p InitListen=%p SetWorld=%p\n", (void*)NetDriver, (void*)InitListen, (void*)SetWorld);

	// SetWorld @ 0x29664C8 IS correct (Remix uses the same RVA) — it works when Listen runs during the
	// engine's UWorld::Listen (hooked for 32.11), not from a mistimed tick trigger.
	SetWorld(NetDriver, World);
	SDK::DbgLog("[Listen] pre-InitListen SetWorld done\n");

	// Guard the loop against a garbage Num() (32.11 encrypted reflection) to avoid a runaway.
	int _lcNum = World->LevelCollections.Num();
	for (int i = 0; i < _lcNum && i < 64; i++)
	{
		auto& LevelCollection = World->LevelCollections.Get(i, FLevelCollection::Size());
		LevelCollection.NetDriver = NetDriver;
	}

	size_t urlSize = FURL::Size();
	if (urlSize == 0 || urlSize > 0x1000) // 32.11 encrypted reflection can return garbage -> huge memset AV
		urlSize = 0x100;
	auto URL = (FURL*)malloc(urlSize);
	memset((PBYTE)URL, 0, urlSize);
	URL->Port = FConfiguration::Port;

	SDK::DbgLog("[Listen] calling InitListen Port=%d...\n", (int)FConfiguration::Port);
	FString Err;
	bool ok = InitListen(NetDriver, World, URL, false, Err);
	SDK::DbgLog("[Listen] InitListen returned %d\n", (int)ok);
	if (!ok)
	{
		printf("Failed to listen!");
		free(URL);
		return false;
	}
	SetWorld(NetDriver, World);
	SDK::DbgLog("[Listen] Listen() complete — server listening on port %d\n", (int)FConfiguration::Port);

	free(URL);

	return true;
}

struct FFortBotCosmeticItemSetDataTableRow
{
public:
	USCRIPTSTRUCT_COMMON_MEMBERS(FFortBotCosmeticItemSetDataTableRow);

	DEFINE_STRUCT_PROP(SetTag, FGameplayTag);
	DEFINE_STRUCT_PROP(CharacterAssetId, FPrimaryAssetId);
	DEFINE_STRUCT_PROP(BackpackAssetId, FPrimaryAssetId);
	DEFINE_STRUCT_PROP(Weight, float);
};

class UFortAthenaAIBotCosmeticLibraryData : public UObject
{
public:
	UCLASS_COMMON_MEMBERS(UFortAthenaAIBotCosmeticLibraryData);

	DEFINE_PROP(PredefineSetsDataTable, TSoftObjectPtr<UDataTable>);
};

class UFortAthenaAIBotCharacterCustomization : public UObject
{
public:
	UCLASS_COMMON_MEMBERS(UFortAthenaAIBotCharacterCustomization);

	DEFINE_PROP(CustomizationLoadout, FFortAthenaLoadout);
};

class UFortAthenaAIBotCustomizationData : public UObject
{
public:
	UCLASS_COMMON_MEMBERS(UFortAthenaAIBotCustomizationData);

	DEFINE_PROP(CosmeticCustomizationLibrary, TSoftObjectPtr<UFortAthenaAIBotCosmeticLibraryData>);
	DEFINE_PROP(OverrideCosmeticMode, uint8_t);
	DEFINE_PROP(CharacterCustomization, UFortAthenaAIBotCharacterCustomization*);
};

template <typename T>
std::pair<FName, T*> PickWeighted(UEAllocatedMap<FName, T*>& Map, float (*RandFunc)(float), bool bCheckZero = true)
{
	float TotalWeight = std::accumulate(Map.begin(), Map.end(), 0.0f, [&](float acc, std::pair<FName, T*> p)
		{ return acc + p.second->Weight; });
	float RandomNumber = RandFunc(TotalWeight);

	for (auto& Element : Map)
	{
		float Weight = Element.second->Weight;
		if (bCheckZero && Weight == 0)
			continue;

		if (RandomNumber <= Weight) return Element;

		RandomNumber -= Weight;
	}

	std::pair<FName, T*> None;
	return None;
}

void InitializeCosmeticLoadout(UFortAthenaAIBotCustomizationData* BotData, AFortPlayerPawnAthena* Pawn, FFortAthenaLoadout& OutLoadout, FGameplayTag* PredefinedCosmeticSetTag)
{
	//auto OutLoadout = BotData->CharacterCustomization->CustomizationLoadout;

	if (BotData->OverrideCosmeticMode == 1)
	{
		UEAllocatedMap<FName, FFortBotCosmeticItemSetDataTableRow*> LibraryRowMap;
		FName& CosmeticTag = PredefinedCosmeticSetTag->TagName;

		for (auto& [Key, Val] : (TMap<FName, FFortBotCosmeticItemSetDataTableRow*>&)BotData->CosmeticCustomizationLibrary->PredefineSetsDataTable->RowMap)
		{
			if (Val->SetTag.TagName == CosmeticTag)
				LibraryRowMap[Key] = Val;
		}

		auto LibraryRowPair = PickWeighted(LibraryRowMap, [](float Total)
			{ return ((float)rand() / 32767) * Total; });
		auto& LibraryRow = LibraryRowPair.second;

		OutLoadout.Character = (UAthenaCharacterItemDefinition*)UKismetSystemLibrary::GetObjectFromPrimaryAssetId(LibraryRow->CharacterAssetId);
		if (LibraryRow->BackpackAssetId.PrimaryAssetType.IsValid())
			OutLoadout.Backpack = (UAthenaCharacterPartItemDefinition*)UKismetSystemLibrary::GetObjectFromPrimaryAssetId(LibraryRow->BackpackAssetId);
	}
	else
	{
		OutLoadout.Character = BotData->CharacterCustomization->CustomizationLoadout.Character;
		OutLoadout.Backpack = BotData->CharacterCustomization->CustomizationLoadout.Backpack;
	}

	UEAllocatedMap<uint8_t, const UCustomCharacterPart*> PartMap;

	if (OutLoadout.Character)
		if (auto HeroDefinition = OutLoadout.Character->HeroDefinition)
			for (auto& SoftSpec : HeroDefinition->Specializations)
			{
				auto Specialization = SoftSpec.Get();

				if (Specialization)
					for (auto& PartSoft : Specialization->CharacterParts)
					{
						auto Part = PartSoft.Get();

						PartMap[Part->CharacterPartType] = Part;
					}
			}

	if (OutLoadout.Backpack)
		for (auto& Part : OutLoadout.Backpack->CharacterParts)
			PartMap[Part->CharacterPartType] = Part;

	for (int i = 0; i < OutLoadout.CharacterVariantChannels.Num(); i++)
	{
		auto& VariantChannel = OutLoadout.CharacterVariantChannels.Get(i, FMcpVariantChannelInfo::Size());
		auto CosmeticForVariant = (UAthenaCosmeticItemDefinition*)VariantChannel.ItemVariantIsUsedFor;

		for (auto& ItemVariant : CosmeticForVariant->ItemVariants)
			if (auto PartVariant = ItemVariant->Cast<UFortCosmeticCharacterPartVariant>())
				for (int i = 0; i < PartVariant->PartOptions.Num(); i++)
				{
					auto& PartOption = PartVariant->PartOptions.Get(i, FPartVariantDef::Size());

					if (VariantChannel.ActiveVariantTag.TagName == PartOption.CustomizationVariantTag.TagName)
						for (auto& PartSoft : PartOption.VariantParts)
						{
							auto Part = PartSoft.Get();

							PartMap[Part->CharacterPartType] = Part;
						}
				}
	}

	for (auto& [PartType, Part] : PartMap)
		Pawn->ServerChoosePart(PartType, Part);
}

void Misc::Hook()
{
	if (VersionInfo.FortniteVersion == 23.00 || (VersionInfo.FortniteVersion >= 24.30 && VersionInfo.FortniteVersion != 28.30 && VersionInfo.FortniteVersion != 29.40) || VersionInfo.FortniteVersion >= 30)
	{
		uintptr_t AttemptDeriveFromURL = 0;
		if (VersionInfo.FortniteVersion >= 32.00)
		{
			// 32.11: the prologue uses REX.XB (0x43) for push r12-r14, so the generic 0x41 sigs miss.
			// Remix-confirmed RVAs (CL 38202817): UWorld::AttemptDeriveFromURL @ +0x2196B08, and a
			// second net-mode fn (InternalGetNetMode) @ +0x16562A4 — both forced to NM_DedicatedServer.
			auto base = Memcury::PE::GetModuleBase();
			uintptr_t adfu = base + 0x2196B08;
			if (*(uint32_t*)adfu == 0x245C8948) // verify "48 89 5C 24" prologue (build matches Remix CL)
			{
				AttemptDeriveFromURL = adfu;
				// InternalGetNetMode (+0x16562A4) is ALSO hooked below for 32.11 — the Iris replication
				// worker calls it directly and crashes if it sees NM_Client. See the B2 block.
			}
			else
				SDK::DbgLog("  [Misc] netmode 32.11 RVA mismatch (prologue %08X) — build differs from Remix CL\n", *(uint32_t*)adfu);
		}
		if (!AttemptDeriveFromURL)
			AttemptDeriveFromURL = Memcury::Scanner::FindPattern("48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ? ? ? ? 4C 8B C1").Get();
		if (!AttemptDeriveFromURL)
			AttemptDeriveFromURL = Memcury::Scanner::FindPattern("48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ? ? ? ? 48 8B D1").Get();
		if (!AttemptDeriveFromURL)
			AttemptDeriveFromURL = Memcury::Scanner::FindPattern("48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ? ? ? ? 4C 8B D1").Get();
		if (!AttemptDeriveFromURL)
			AttemptDeriveFromURL = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 41 54 41 55 41 56 41 57 48 81 EC ? ? ? ? 4C 8B D1").Get();

		SDK::DbgLog("  [Misc] A ADFU=%p pre-Hook/PatchAllNetModes\n", (void*)AttemptDeriveFromURL);
		Utils::Hook(AttemptDeriveFromURL, GetNetMode);
		PatchAllNetModes(AttemptDeriveFromURL); // Remix uses this on 32.11; needed for consistent netmode
		if (VersionInfo.FortniteVersion >= 32.00)
		{
			// 32.11: the Iris replication worker calls UWorld::GetNetMode (+0x16562A4) DIRECTLY. Because
			// this process is FortniteClient.exe the real world netmode is NM_Client (3), so that worker
			// takes a client-only branch and null-derefs (crash on a bg thread right after the Iris
			// FortTeamPrivateInfo group setup — disasm: sub_15A17D8 does `if (GetNetMode()==3 && ...) ->
			// [[..+0x48]+0xf0]+0x134`). AttemptDeriveFromURL alone is NOT enough; Better-Remix forces this
			// one to NM_DedicatedServer too. Server-only path (fine for a joinable server).
			Utils::Hook(Memcury::PE::GetModuleBase() + 0x16562A4, GetNetMode);
			SDK::DbgLog("  [Misc] B2 InternalGetNetMode (+0x16562A4) forced to server netmode\n");
		}
		SDK::DbgLog("  [Misc] B netmode hook+patch done\n");

		if (VersionInfo.FortniteVersion >= 32.00)
		{
			// 32.11: skip client-only tick functions on the dedicated server (Remix/Better-Remix). Testing
			// in isolation — this is the prime suspect for taming the FortWorldManager tick grind/freeze.
			auto base = Memcury::PE::GetModuleBase();
			Utils::Hook(base + 0x19F7AFC, Test2, Test2OG); // RegisterTickFunction
			Utils::Hook(base + 0x4139B6C, GetTickableTickType_FN32); // GetTickableTickType -> Never
			Utils::Hook(base + 0xA7EAB6C, CreateAndConfigureNavigationSystem_FN32, CreateAndConfigureNavigationSystem_OG); // nav config
			Utils::Hook(base + 0xB4E282C, RegisterToLivingWorldManager_FN32); // no-op LivingWorldManager registration (kills 204ms FortWorldManager tick)
			SDK::DbgLog("  [Misc] B3 tick+nav+livingworld hooks installed (Test2OG=%p NavOG=%p)\n", (void*)Test2OG, (void*)CreateAndConfigureNavigationSystem_OG);
			CreateThread(nullptr, 0, (LPTHREAD_START_ROUTINE)WatchdogThread, nullptr, 0, nullptr); // pinpoint hang
			SDK::DbgLog("  [Misc] B3b watchdog thread started\n");
		}
	}
	else if (VersionInfo.FortniteVersion >= 28)
	{
		auto AttemptDeriveFromURL = Memcury::Scanner::FindPattern("48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ? ? ? ? 4C 8B C1").Get();
		if (!AttemptDeriveFromURL)
			AttemptDeriveFromURL = Memcury::Scanner::FindPattern("48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ? ? ? ? 48 8B D1").Get();
		if (!AttemptDeriveFromURL)
			AttemptDeriveFromURL = Memcury::Scanner::FindPattern("48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ? ? ? ? 4C 8B D1").Get();

		Utils::Hook(AttemptDeriveFromURL, GetNetMode);
		PatchAllNetModes(AttemptDeriveFromURL);

		Utils::Hook(FindGetNetMode(), GetNetMode);
	}
	else
		Utils::Hook(FindGetNetMode(), GetNetMode);

	SDK::DbgLog("  [Misc] C pre-SendRequestNow\n");
	const uint64 GetMaxTickRateTarget = bSafeZoneTickHookInstalled
		? SafeZoneTickHookTarget
		: ResolveGetMaxTickRateTarget();
	// 32.11: sigs miss (43-vs-41 REX). Remix RVAs. GetMaxTickRate is critical — the real one reads
	// World->NetDriver->NetServerMaxTickRate, which faults (read null) while NetDriver is still null
	// during dedicated-server init; hooking it (return a constant) avoids that.
	if (VersionInfo.FortniteVersion >= 32.00)
	{
		auto base = Memcury::PE::GetModuleBase();
		Utils::Hook(base + 0x7DE4ED0, SendRequestNow, SendRequestNowOG); // SendRequestNow
		SDK::DbgLog("  [Misc] D pre-GetMaxTickRate (32.11 RVA)\n");
	}
	else
	{
		Utils::Hook(FindSendRequestNow(), SendRequestNow, SendRequestNowOG);
		SDK::DbgLog("  [Misc] D pre-GetMaxTickRate\n");
	}
	if (bSafeZoneTickHookInstalled && GetMaxTickRateTarget == SafeZoneTickHookTarget)
		SDK::DbgLog("  [Misc] reusing pre-Start GetMaxTickRate hook\n");
	else
		Utils::Hook(GetMaxTickRateTarget, GetMaxTickRate);
	SDK::DbgLog("  [Misc] E GetMaxTickRate done\n");
	if (VersionInfo.FortniteVersion >= 32.00)
	{
		// 32.11: heartbeat prologue sigs miss (43-vs-41 REX). Remix RVA for the matchmaking
		// checkpoint-heartbeat; hooking it (return -1) stops the server-init blocking on MM.
		auto hb = Memcury::PE::GetModuleBase() + 0x521AFC0;
		SDK::DbgLog("  [Misc] heartbeat 32.11 RVA=%p prologue=%08X\n", (void*)hb, *(uint32_t*)hb);
		Utils::Hook(hb, CheckCheckpointHeartBeat);
	}
	else if (VersionInfo.FortniteVersion >= 17)
	{
		auto pattern = Memcury::Scanner::FindPattern("48 89 5C 24 10 48 89 6C 24 20 56 57 41 54 41 56 41 57 48 81 EC ? ? ? ? 65 48 8B 04 25 ? ? ? ? 4C 8B F9").Get();

		if (!pattern)
			pattern = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 41 54 41 55 41 56 41 57 48 81 EC ? ? ? ? 65 48 8B 04 25 ? ? ? ? 4C 8B E9").Get();

		if (!pattern)
			pattern = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 6C 24 ? 56 57 41 54 41 55 41 56 48 81 EC ? ? ? ? 65 48 8B 04 25").Get();

		Utils::Hook(pattern, CheckCheckpointHeartBeat);
	}
	SDK::DbgLog("  [Misc] F checkpoint-heartbeat done\n");
	if (VersionInfo.EngineVersion < 4.20)
	{
		auto ApplyHomebaseEffectsOnPlayerSetupAddr = Memcury::Scanner::FindPattern("40 55 53 57 41 54 41 56 41 57 48 8D 6C 24 ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 45 00 4C 8B").Get();

		Utils::Hook(ApplyHomebaseEffectsOnPlayerSetupAddr, ApplyHomebaseEffectsOnPlayerSetup, ApplyHomebaseEffectsOnPlayerSetupOG);
	}
	if (VersionInfo.FortniteVersion >= 25 && VersionInfo.FortniteVersion < 28)
	{
		Utils::Hook(Memcury::Scanner::FindPattern("48 89 5C ? ? 57 48 83 EC ? 48 8B D1 48 85 C9 74 ?").Get(), RetFalse);
	}


	auto PedestalBeginPlay = Memcury::Scanner::FindStringRef(L"AFortTeamMemberPedestal::BeginPlay - Begun play on pedestal %s", true, 0, VersionInfo.EngineVersion >= 5.0).Get();

	if (PedestalBeginPlay)
	{
		uint64_t RealBeginPlay = 0;
		for (int i = 0; i < 1000; i++)
		{
			auto Ptr = (uint8_t*)(PedestalBeginPlay - i);

			if (*Ptr == 0x48 && *(Ptr + 1) == 0x89 && *(Ptr + 2) == 0x5c)
			{
				RealBeginPlay = (uint64_t)Ptr;
				break;
			}
			else if (*Ptr == 0x40 && *(Ptr + 1) == 0x53 && *(Ptr + 2) == 0x41 && *(Ptr + 3) == 0x56)
			{
				RealBeginPlay = (uint64_t)Ptr;
				break;
			}
		}

		auto ActorVft = AFortTeamMemberPedestal::GetDefaultObj()->Vft;

		for (int i = 0; i < 0x500; i++)
		{
			if (ActorVft[i] == (void*)RealBeginPlay)
			{
				Utils::Hook<AFortTeamMemberPedestal>(uint32_t(i), AActor::GetDefaultObj()->Vft[i]);
				break;
			}
		}
	}
	
	SDK::DbgLog("  [Misc] G pedestal block done\n");
	if (VersionInfo.FortniteVersion >= 23)
	{
		auto pattern = Memcury::Scanner::FindPattern("48 8B 01 FF 90 ? ? ? ? 48 8B 8B ? ? ? ? 48 85 C9 74 ? 48 8B 01 FF 90 ? ? ? ? 48 8D 8B");

		auto patchPoint = pattern.ScanFor(VersionInfo.EngineVersion < 5.5 ? std::vector<uint8_t>{ 0x48, 0x89, 0x5C } : std::vector<uint8_t>{ 0x40, 0x53 }, false).ScanFor({0x83, 0xF8, 0x02}).Get();

		if (patchPoint)
			Utils::Patch<uint8_t>(patchPoint + 2, 0x1);
		else
			SDK::DbgLog("  [Misc] G! playercount patch scan failed — skipped\n");
	}
	SDK::DbgLog("  [Misc] H playercount patch done\n");

	if (VersionInfo.FortniteVersion >= 24.30)
	{
		auto sig = VersionInfo.EngineVersion == 5.3 ? Memcury::Scanner::FindPattern("40 53 48 83 EC ? 48 8B DA 49 8B D0 E8 ? ? ? ? 48 85 C0 0F 85 ? ? ? ? 48 39 83").Get() : Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 41 56 41 57 48 83 EC ? 4C 8D B1 ? ? ? ? 33 DB 49 8D 7E").Get();

		if (!sig)
			sig = Memcury::Scanner::FindPattern("40 53 48 83 EC ? 48 8B DA 48 8B D1 48 81 C1 ? ? ? ? E8 ? ? ? ? 48 85 C0 74 ? 4C 8B 0B 45 33 C0").Get();

		Utils::Hook(sig, CrashSomething, CrashSomethingOG);
	}
	SDK::DbgLog("  [Misc] I CrashSomething done; Misc::Hook complete\n");

	//Utils::Hook(ImageBase + 0x1CE85F4, Test);
	//Utils::Hook(ImageBase + 0x2788BEC, Test, TestOG);
	//Utils::Hook(Memcury::Scanner::FindPattern("48 89 5C 24 ?? 57 48 83 EC ?? 48 8B DA 48 8B F9 E8 ?? ?? ?? ?? 84 C0 75 ?? 48 83 79").Get(), Test2, Test2OG);
	/*if (ABuildingProp_LockDevice::StaticClass())
	{
		auto Fn = ABuildingProp_LockDevice::GetDefaultObj()->GetFunction("UnlockObject");

		Utils::Hook<ABuildingProp_LockDevice>(Fn->GetVTableIndex(), Ohio);
	}

	auto ListenCall = FindListenCall();

	if (ListenCall)
	{
		auto OverrideFunc = __int64(DefaultObjImpl("FortHUDContext")->GetFunction("EnterCameraMode")->ExecFunction);

		Utils::Hook(OverrideFunc, Listen);

		auto NewRel = uint32(OverrideFunc - (ListenCall + 5));

		Utils::Patch<uint32>(ListenCall + 1, NewRel);
	}*/

	if (VersionInfo.FortniteVersion >= 11 && VersionInfo.FortniteVersion < 16)
	{
		auto NearGetOverrideCosmeticLoadout = Memcury::Scanner::FindPattern("4D 8B CD 4C 8D 45 ? 48 8B D6");

		if (!NearGetOverrideCosmeticLoadout.IsValid())
			NearGetOverrideCosmeticLoadout = Memcury::Scanner::FindPattern("4D 8B CD 4C 8D 85 ? ? ? ? 48 8B D6");

		if (!NearGetOverrideCosmeticLoadout.IsValid())
			NearGetOverrideCosmeticLoadout = Memcury::Scanner::FindPattern("4C 8D 45 ? 48 8B D3 48 8B CF E8 ? ? ? ? 0F B6 57");

		if (!NearGetOverrideCosmeticLoadout.IsValid())
			NearGetOverrideCosmeticLoadout = Memcury::Scanner::FindPattern("4C 8D 45 ? 48 8B D3 48 8B CF E8 ? ? ? ? 0F B6 4F");

		if (NearGetOverrideCosmeticLoadout.IsValid())
		{
			auto Rel32 = NearGetOverrideCosmeticLoadout.ScanFor({ 0xE8 }).Get();

			auto OverrideFunc = __int64(DefaultObjImpl("FortHUDContext")->GetFunction("EnterCursorMode")->ExecFunction);

			Utils::Hook(OverrideFunc, InitializeCosmeticLoadout);

			auto NewRel = uint32(OverrideFunc - (Rel32 + 5));

			Utils::Patch<uint32>(Rel32 + 1, NewRel);
		}
	}
}
