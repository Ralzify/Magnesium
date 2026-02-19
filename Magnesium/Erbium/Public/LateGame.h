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
	return GUI::SelectedPlaylist == static_cast<int>(Playlist::OneShotSolos) || GUI::SelectedPlaylist == static_cast<int>(Playlist::OneShotDuos) || GUI::SelectedPlaylist == static_cast<int>(Playlist::OneShotSquads);
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