#pragma once
#include "../../pch.h"
#include "../../FortniteGame/Public/BuildingSMActor.h"
#include "../../FortniteGame/Public/FortInventory.h"
#include "../../FortniteGame/Public/FortPlayerControllerAthena.h"
#include "../Public/GUI.h"

struct FLateGameItem
{
    uint32 Count;
    const UFortItemDefinition* Item;
};

enum class EAmmoType : uint8
{
    Assault = 0,
    Shotgun = 1,
    Submachine = 2,
    Rocket = 3,
    Sniper = 4
};

static inline bool IsOneShot()
{
	const int Selected = GUI::GetSelectedPlaylist();
	return Selected == static_cast<int>(Playlist::OneShotSolos) || Selected == static_cast<int>(Playlist::OneShotDuos) || Selected == static_cast<int>(Playlist::OneShotSquads);
}

class LateGame
{
public:
    static TArray<TArray<TPair<FString, int>>> GetLoadout();
    static TArray<TArray<TPair<FString, int>>> GetVersionizedLoadout();
    static TArray<TArray<TPair<FString, int>>> GetOSLoadout();
    static TArray<TArray<TPair<FString, int>>> GetCustomLoadout();

    static void EquipLoadout(AFortPlayerControllerAthena* PlayerController);
};
