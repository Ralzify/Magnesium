#include "pch.h"
#include "../Public/Misc.h"
#include "../Public/Finders.h"
#include <algorithm>
#include "../Public/Configuration.h"
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
	{ "fireflies", "WID_Athena_Grenade_Molotov" },
	{ "firefly", "WID_Athena_Grenade_Molotov" },
	{ "firesniper", "WID_WaffleTruck_Sniper_DragonBreath" },
	{ "flare", "WID_FringePlank_Athena_Prototype" },
	{ "flint", "WID_Pistol_Flintlock_Athena_UC" },
	{ "flint_c", "WID_Pistol_Flintlock_Athena_C" },
	{ "flint_uc", "WID_Pistol_Flintlock_Athena_UC" },
	{ "fortress", "Athena_SuperTowerGrenade_A" },
	{ "gl", "WID_Launcher_Grenade_Athena_SR_Ore_T03" },
	{ "gl_r", "WID_Launcher_Grenade_Athena_R_Ore_T03" },
	{ "gl_sr", "WID_Launcher_Grenade_Athena_SR_Ore_T03" },
	{ "gl_vr", "WID_Launcher_Grenade_Athena_VR_Ore_T03" },
	{ "gliders", "Athena_Glider_Item" },
	{ "glider", "Athena_Glider_Item" },
	{ "gold", "Athena_WadsItemData" },
	{ "goldfish", "WID_Athena_Bucket_Nice" },
	{ "grabitron", "WID_GravyGoblinV2_Athena" },
	{ "grap_n", "WID_Hook_Gun_VR_Ore_T03" },
	{ "grapple_n", "WID_Hook_Gun_VR_Ore_T03" },
	{ "grappler_n", "WID_Hook_Gun_VR_Ore_T03" },
	{ "grappler", "WID_Hook_Gun_Slide" },
	{ "grapple", "WID_Hook_Gun_Slide" },
	{ "grap", "WID_Hook_Gun_Slide" },
	{ "guided", "WID_RC_Rocket_Athena_SR_T03" },
	{ "guided_sr", "WID_RC_Rocket_Athena_SR_T03" },
	{ "guided_vr", "WID_RC_Rocket_Athena_VR_T03" },
	{ "guidedmissile_sr", "WID_RC_Rocket_Athena_SR_T03" },
	{ "guidedmissile_vr", "WID_RC_Rocket_Athena_VR_T03" },
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
	{ "hunting_r", "WID_Sniper_NoScope_Athena_R_Ore_T03" },
	{ "hunting_sr", "WID_Sniper_NoScope_Athena_SR_Ore_T03" },
	{ "hunting_uc", "WID_Sniper_NoScope_Athena_UC_Ore_T03" },
	{ "hunting_vr", "WID_Sniper_NoScope_Athena_VR_Ore_T03" },
	{ "impulse", "Athena_KnockGrenade" },
	{ "impulsegrenade", "Athena_KnockGrenade" },
	{ "impulses", "Athena_KnockGrenade" },
	{ "infantry_c", "WID_Assault_Infantry_Athena_C" },
	{ "infantry_uc", "WID_Assault_Infantry_Athena_UC" },
	{ "infantry_r", "WID_Assault_Infantry_Athena_R" },
	{ "infantry_vr", "WID_Assault_Infantry_Athena_VR" },
	{ "infantry_sr", "WID_Assault_Infantry_Athena_SR" },
	{ "infantry", "WID_Assault_Infantry_Athena_SR" },
	{ "jules", "WID_Boss_GrapplingHoot" },
	{ "julesgrap", "WID_Boss_GrapplingHoot" },
	{ "julesgrappler", "WID_Boss_GrapplingHoot" },
	{ "jumpad", "TID_Floor_Player_Jump_Pad_Athena" },
	{ "jumppad", "TID_Floor_Player_Jump_Pad_Athena" },
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
	{ "nimbus", "WID_Stamina_Hawaii" },
	{ "pad", "TID_Floor_Player_Launch_Pad_Athena" },
	{ "paf", "Athena_TowerGrenade" },
	{ "portafort", "Athena_TowerGrenade" },
	{ "portafortress", "Athena_SuperTowerGrenade_A" },
	{ "paft", "Athena_SuperTowerGrenade_A" },
	{ "phone", "WID_CreativeTool" },
	{ "pump_r", "WID_Shotgun_Standard_Athena_UC_Ore_T03" },
	{ "pump_sr", "WID_Shotgun_Standard_Athena_SR_Ore_T03" },
	{ "pump_uc", "WID_Shotgun_Standard_Athena_C_Ore_T03" },
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
	{ "rocket", "WID_Launcher_Rocket_Athena_SR_Ore_T03" },
	{ "rocket_r", "WID_Launcher_Rocket_Athena_R_Ore_T03" },
	{ "rocket_sr", "WID_Launcher_Rocket_Athena_SR_Ore_T03" },
	{ "rocket_vr", "WID_Launcher_Rocket_Athena_VR_Ore_T03" },
	{ "rocketammo", "AmmoDataRockets" },
	{ "rockets", "AmmoDataRockets" },
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
	{ "ch1snowman", "Athena_SneakySnowman" },
	{ "ch2snowman", "AGID_SneakySnowmanV2" },
	{ "snowman", "AGID_SneakySnowmanV2" },
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
};

const std::unordered_map<std::string, std::string> Misc::ObjectNames = {
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
	{ "battlebus", "/ArmoredBattleBus/Vehicle/ArmoredBattleBus_Vehicle.ArmoredBattleBus_Vehicle_C" },
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
	return 1;
}

void* Misc::SendRequestNow(void* Arg1, void* MCPData, int)
{
	if (VersionInfo.EngineVersion < 4.23)
		*(int*)(__int64(MCPData) + (VersionInfo.FortniteVersion >= 4.2 ? 0x28 : 0x60)) = 3; // CXC_Public

	return SendRequestNowOG(Arg1, MCPData, 3); // CXC_Public
}

float Misc::GetMaxTickRate(UEngine* Engine, float DeltaTime, bool bAllowFrameRateSmoothing)
{
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
	if (!_this->bAllowTickOnDedicatedServer)
	{
		return;
	}
	return Test2OG(_this, Level);
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

	if (GameMode->HasbEnableReplicationGraph())
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

	if (VersionInfo.EngineVersion >= 5.3 && FConfiguration::bEnableIris)
	{
		*(bool*)(__int64(&NetDriver->ReplicationDriver) + 0x11) = true;
	}

	NetDriver->NetDriverName = NetDriverName;
	NetDriver->World = World;

	auto InitListen = (bool (*)(UNetDriver*, UWorld*, FURL*, bool, FString&)) FindInitListen();
	auto SetWorld = (void (*)(UNetDriver*, UWorld*)) FindSetWorld();

	SetWorld(NetDriver, World);
	for (int i = 0; i < World->LevelCollections.Num(); i++)
	{
		auto& LevelCollection = World->LevelCollections.Get(i, FLevelCollection::Size());

		LevelCollection.NetDriver = NetDriver;
	}

	auto URL = (FURL*)malloc(FURL::Size());
	memset((PBYTE)URL, 0, FURL::Size());
	URL->Port = FConfiguration::Port;


	FString Err;
	if (!InitListen(NetDriver, World, URL, false, Err))
	{
		printf("Failed to listen!");

		free(URL);

		return false;
	}
	SetWorld(NetDriver, World);

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
			if (Val->SetTag.TagName == CosmeticTag)
				LibraryRowMap[Key] = Val;

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
		auto AttemptDeriveFromURL = Memcury::Scanner::FindPattern("48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ? ? ? ? 4C 8B C1").Get();
		if (!AttemptDeriveFromURL)
			AttemptDeriveFromURL = Memcury::Scanner::FindPattern("48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ? ? ? ? 48 8B D1").Get();
		if (!AttemptDeriveFromURL)
			AttemptDeriveFromURL = Memcury::Scanner::FindPattern("48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ? ? ? ? 4C 8B D1").Get();
		if (!AttemptDeriveFromURL)
			AttemptDeriveFromURL = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 41 54 41 55 41 56 41 57 48 81 EC ? ? ? ? 4C 8B D1").Get();

		Utils::Hook(AttemptDeriveFromURL, GetNetMode);
		PatchAllNetModes(AttemptDeriveFromURL);
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

	Utils::Hook(FindSendRequestNow(), SendRequestNow, SendRequestNowOG);
	Utils::Hook(FindGetMaxTickRate(), GetMaxTickRate);
	if (VersionInfo.FortniteVersion >= 17)
	{
		auto pattern = Memcury::Scanner::FindPattern("48 89 5C 24 10 48 89 6C 24 20 56 57 41 54 41 56 41 57 48 81 EC ? ? ? ? 65 48 8B 04 25 ? ? ? ? 4C 8B F9").Get();

		if (!pattern)
			pattern = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 41 54 41 55 41 56 41 57 48 81 EC ? ? ? ? 65 48 8B 04 25 ? ? ? ? 4C 8B E9").Get();

		if (!pattern)
			pattern = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 6C 24 ? 56 57 41 54 41 55 41 56 48 81 EC ? ? ? ? 65 48 8B 04 25").Get();

		Utils::Hook(pattern, CheckCheckpointHeartBeat);
	}
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
	
	if (VersionInfo.FortniteVersion >= 23)
	{
		auto pattern = Memcury::Scanner::FindPattern("48 8B 01 FF 90 ? ? ? ? 48 8B 8B ? ? ? ? 48 85 C9 74 ? 48 8B 01 FF 90 ? ? ? ? 48 8D 8B");

		auto patchPoint = pattern.ScanFor(VersionInfo.EngineVersion < 5.5 ? std::vector<uint8_t>{ 0x48, 0x89, 0x5C } : std::vector<uint8_t>{ 0x40, 0x53 }, false).ScanFor({0x83, 0xF8, 0x02}).Get();

		Utils::Patch<uint8_t>(patchPoint + 2, 0x1);
	}

	if (VersionInfo.FortniteVersion >= 24.30)
	{
		auto sig = VersionInfo.EngineVersion == 5.3 ? Memcury::Scanner::FindPattern("40 53 48 83 EC ? 48 8B DA 49 8B D0 E8 ? ? ? ? 48 85 C0 0F 85 ? ? ? ? 48 39 83").Get() : Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 41 56 41 57 48 83 EC ? 4C 8D B1 ? ? ? ? 33 DB 49 8D 7E").Get();

		if (!sig)
			sig = Memcury::Scanner::FindPattern("40 53 48 83 EC ? 48 8B DA 48 8B D1 48 81 C1 ? ? ? ? E8 ? ? ? ? 48 85 C0 74 ? 4C 8B 0B 45 33 C0").Get();

		Utils::Hook(sig, CrashSomething, CrashSomethingOG);
	}

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