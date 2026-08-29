#pragma once
#include "../../pch.h"
#include "../../json.hpp"
#include "Utils.h"

#include <ShlObj.h>
#include <filesystem>
#include <fstream>

inline std::string FStringToStdString(const FString& UnrealStr)
{
    return std::string(TCHAR_TO_UTF8(*UnrealStr));
}

// these dont really fit into a class
class Misc
{
public:
    static inline bool bHookedAll = false;

    static int GetNetMode();
    DefHookOg(void*, SendRequestNow, void*, void*, int);
    static bool InstallPreStartSafeZoneTick();
    static void ActivateServerGetMaxTickRate();
    DefHookOg(float, SafeZoneTickGetMaxTickRate, UEngine*, float, bool);
    DefHookOg(float, GetMaxTickRate, UEngine*, float, bool);
    static inline bool bSafeZoneTickHookInstalled = false;
    static inline uint64 SafeZoneTickHookTarget = 0;
    static inline std::atomic<bool> bServerGetMaxTickRateActive{ false };
    static uint32 CheckCheckpointHeartBeat();
    DefHookOg(void, ApplyHomebaseEffectsOnPlayerSetup, __int64*, __int64, __int64, __int64,
        UObject*, char, unsigned __int8);
    static void InitClient();
    static const std::unordered_map<std::string, std::string> ItemNames;
    static const std::unordered_map<std::string, std::string> ObjectNames;

    InitHooks;
};

namespace LoadoutManager
{
    inline std::string GetLoadoutDirectory()
    {
        char path[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, path)))
        {
            std::string fullPath = std::string(path) + "\\Magnesium\\loadouts";
            std::filesystem::create_directories(fullPath);
            return fullPath;
        }
        return "";
    }

    inline std::string GetLoadoutFilePath()
    {
        return GetLoadoutDirectory() + "\\savedloadout.json";
    }

    inline bool SaveLoadout(const char* Primary, int PrimaryAmount, const char* Secondary,
        int SecondaryAmount, const char* Tertiary, int TertiaryAmount, const char* Quaternary,
        int QuaternaryAmount, const char* Quinary, int QuinaryAmount, const char* Traps,
        int TrapsAmount)
    {
        try
        {
            nlohmann::json j;
            j["primary"] = Primary;
            j["primaryAmount"] = PrimaryAmount;
            j["secondary"] = Secondary;
            j["secondaryAmount"] = SecondaryAmount;
            j["tertiary"] = Tertiary;
            j["tertiaryAmount"] = TertiaryAmount;
            j["quaternary"] = Quaternary;
            j["quaternaryAmount"] = QuaternaryAmount;
            j["quinary"] = Quinary;
            j["quinaryAmount"] = QuinaryAmount;
            j["traps"] = Traps;
            j["trapsAmount"] = TrapsAmount;

            std::ofstream file(GetLoadoutFilePath());

            if (file.is_open())
            {
                file << j.dump(4);
                file.close();
                return true;
            }
        }
        catch (const std::exception& e)
        {
            printf("Failed to save loadout: %s\n", e.what());
        }

        return false;
    }

    inline bool LoadLoadout(char* Primary, int& PrimaryAmount, char* Secondary,
        int& SecondaryAmount, char* Tertiary, int& TertiaryAmount, char* Quaternary,
        int& QuaternaryAmount, char* Quinary, int& QuinaryAmount, char* Traps, int& TrapsAmount)
    {
        try
        {
            std::ifstream file(GetLoadoutFilePath());
            if (file.is_open())
            {
                nlohmann::json j;
                file >> j;
                file.close();

                strcpy_s(Primary, 256, j.value("primary", "").c_str());
                PrimaryAmount = j.value("primaryAmount", 1);
                strcpy_s(Secondary, 256, j.value("secondary", "").c_str());
                SecondaryAmount = j.value("secondaryAmount", 1);
                strcpy_s(Tertiary, 256, j.value("tertiary", "").c_str());
                TertiaryAmount = j.value("tertiaryAmount", 1);
                strcpy_s(Quaternary, 256, j.value("quaternary", "").c_str());
                QuaternaryAmount = j.value("quaternaryAmount", 1);
                strcpy_s(Quinary, 256, j.value("quinary", "").c_str());
                QuinaryAmount = j.value("quinaryAmount", 1);
                strcpy_s(Traps, 256, j.value("traps", "").c_str());
                TrapsAmount = j.value("trapsAmount", 6);

                return true;
            }
        }
        catch (const std::exception& e)
        {
            printf("Failed to load loadout: %s\n", e.what());
        }

        return false;
    }
}
