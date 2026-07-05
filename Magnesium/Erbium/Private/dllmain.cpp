// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"
#include "../Public/Utils.h"
#include <thread>
#include <iostream>
#include "../Public/Finders.h"
#include "../../FortniteGame/Public/FortInventory.h"
#include <chrono>
#include "../Public/Configuration.h"
#include "../Public/Misc.h"
#include "../Public/GUI.h"
#include "../../Erbium/Plugins/CrashReporter/Public/CrashReporter.h"
#include "../../FortniteGame/Public/FortPlayerControllerAthena.h"
#include "../../Engine/Public/NetDriver.h"
#pragma comment(lib, "libcurl/libcurl.lib")
#pragma comment(lib, "libcurl/zlib.lib")
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Wldap32.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Normaliz.lib")

void Main()
{
    if constexpr (!FConfiguration::bGUI)
        AllocConsole();

    if constexpr (!FConfiguration::bGUI || !FConfiguration::bUseStdoutLog)
    {
        if (!FConfiguration::bGUI || GetConsoleWindow())
        {
            FILE* s;
            freopen_s(&s, "CONOUT$", "w", stdout);
            freopen_s(&s, "CONOUT$", "w+", stderr);
            freopen_s(&s, "CONIN$", "r", stdin);
        }
    }

    if constexpr (FConfiguration::bCustomCrashReporter)
        FCrashReporter::Register();

    printf("Initializing SDK...\n");
    SDK::Init();

    if (VersionInfo.FortniteVersion >= 32.00)
        SDK::DumpObjArrayDiag();

    if constexpr (FConfiguration::bGUI)
    {
        if constexpr (FConfiguration::bUseStdoutLog)
        {
            FILE* s;
            freopen_s(&s, "stdout.log", "w", stdout);
            freopen_s(&s, "stdout.log", "w+", stderr);
        }

        CreateThread(0, 0, (LPTHREAD_START_ROUTINE)GUI::Init, 0, 0, 0);
    }

    if constexpr (FConfiguration::bGUI)
    {
        while (!FConfiguration::bReadyToStart)
        {
            Sleep(100);
        }
    }

    SDK::DbgLog("Main: start pressed\n");

    if (wcscmp(FConfiguration::Playlist, L"/DurianPlaylist/Playlist/Playlist_Durian.Playlist_Durian") == 0)
        FConfiguration::bEnableIris = false;

    if (VersionInfo.FortniteVersion <= 2.50)
        FConfiguration::bMovingBus = false;

    if (VersionInfo.FortniteVersion <= 5.41)
        FConfiguration::bGliderRedeploy = false;

    if (VersionInfo.EngineVersion >= 5.0)
    {
        auto _w0 = UWorld::GetWorld();
        SDK::DbgLog("Main: cp0a GetWorld=%p\n", (void*)_w0);
        auto _cmd0 = FString(L"log LogFortUIDirector None");
        SDK::DbgLog("Main: cp0a2 FString built (len=%d)\n", _cmd0.Num());
        UKismetSystemLibrary::ExecuteConsoleCommand(_w0, _cmd0, nullptr);
        SDK::DbgLog("Main: cp0b first ExecuteConsoleCommand ok\n");
        UKismetSystemLibrary::ExecuteConsoleCommand(_w0, FString(L"log LogFortUIManager None"), nullptr);
    }
    if (VersionInfo.FortniteVersion == 20.40)
    {
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"log LogSpecialRelevancyHealthComponent None"), nullptr);
    }
    SDK::DbgLog("Main: cp1 (pre EV5.1 / console cmds ok)\n");
    if (VersionInfo.EngineVersion >= 5.1)
    {
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"net.AllowEncryption 0"), nullptr);

        if (VersionInfo.FortniteVersion >= 32.00)
        {
            // 32.11: BlastBerry is a WorldPartition map. On the injected listen server the server-side
            // streaming thrashes (constant cell add/remove -> ABuildingActor physics re-init) which
            // grinds the shared game thread. Disable server streaming so cells load once and stay.
            UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"wp.Runtime.EnableServerStreaming 0"), nullptr);
            UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"wp.Runtime.EnableServerStreamingOut 0"), nullptr);
            SDK::DbgLog("Main: cp2a (wp server streaming disabled for 32.11)\n");
        }

        SDK::DbgLog("Main: cp2 (net.AllowEncryption ok, pre CurieGlobals)\n");
        auto CurieClass = FindClass("CurieGlobals");
        auto DefaultCurieGlobals = CurieClass ? CurieClass->GetDefaultObj() : nullptr;

        if (DefaultCurieGlobals)
        {
            uint32 Offset = DefaultCurieGlobals->GetOffset("bEnableCurie");

            //if (Offset != -1)
            //    *(bool*)(uintptr_t(DefaultCurieGlobals) + Offset) = false;
        }
    }
    SDK::DbgLog("Main: cp3 (pre Iris block)\n");
    if (VersionInfo.EngineVersion >= 5.3 && FConfiguration::bEnableIris)
    {
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"log LogIris None"), nullptr);
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"log LogIrisRpc None"), nullptr);
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"log LogIrisBridge None"), nullptr);
        /*auto IrisBool = Memcury::Scanner::FindPattern("83 3D ? ? ? ? ? 0F 8E ? ? ? ? 49 8B B9").RelativeOffset(2, 1).Get();
        if (IrisBool)
            *(uint32_t*)IrisBool = true;
        else
        {
            IrisBool = Memcury::Scanner::FindPattern("44 39 25 ? ? ? ? 0F 9F C0 45 84 FF").RelativeOffset(3).Get();

            if (IrisBool)
                *(uint32_t*)IrisBool = true;
        }*/
        auto IrisBool = FindCVar<uint32_t>(L"net.Iris.UseIrisReplication");

        if (IrisBool)
            *IrisBool = true;

        SDK::DbgLog("Main: cp4 (Iris cvars done, pre FilterConfigs)\n");
        if (VersionInfo.FortniteVersion >= 29 && VersionInfo.FortniteVersion < 32.00) // 32.11: FObjectReplicationBridgeFilterConfig::Size() is wrong here -> loop hangs; needs the real 32.11 struct layout
        {
            auto ReplicationBridgeConfig = UObjectReplicationBridgeConfig::GetDefaultObj();

            auto FortInventoryName = FName(L"/Script/FortniteGame.FortInventory");
            for (int i = 0; i < ReplicationBridgeConfig->FilterConfigs.Num(); i++)
            {
                auto& FilterConfig = ReplicationBridgeConfig->FilterConfigs.Get(i, FObjectReplicationBridgeFilterConfig::Size());

                if (FilterConfig.ClassName == FortInventoryName)
                {
                    FilterConfig.DynamicFilterName = FName(0);
                    break;
                }
            }
        }
        //UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"net.Iris.UseIrisReplication 1"), nullptr);
    }
    SDK::DbgLog("Main: cp5 (Iris block done, pre EV5.4)\n");
    if (VersionInfo.EngineVersion >= 5.4)
    {
        // sprint fix
        auto SprintCVar = FindCVar<uint32_t>(L"Fort.MME.TacticalSprint");
        auto HurdleCVar = FindCVar<uint32_t>(L"Fort.MME.Hurdle");
        auto SlideCVar = FindCVar<uint32_t>(L"Fort.MME.Sliding");
        auto MantleCVar = FindCVar<uint32_t>(L"Fort.MME.Clambering");

        //if (SprintCVar)
        //    *SprintCVar = false;

        if (FConfiguration::IsKnownS27CustomMapPlaylist() && HurdleCVar)
            *HurdleCVar = false;

        if (SlideCVar)
            *SlideCVar = false;

        if (MantleCVar)
            *MantleCVar = false;
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"Fort.MME.TacticalSprint 0"), nullptr);
        if (FConfiguration::IsKnownS27CustomMapPlaylist())
            UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"Fort.MME.Hurdle 0"), nullptr);
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"Fort.MME.Sliding 0"), nullptr);
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"Fort.MME.Clambering 0"), nullptr);
    }

#ifdef CLIENT
    Misc::InitClient();

    return;
#endif

    if constexpr (FConfiguration::WebhookURL && *FConfiguration::WebhookURL)
        curl_global_init(CURL_GLOBAL_ALL);

    SDK::DbgLog("Main: cp6 (pre Hooking / FindNullsAndRetTrues)\n");
    sprintf_s(GUI::windowTitle, VersionInfo.EngineVersion >= 5.0 ? "Magnesium (FN %.2f, UE %.1f)" : (VersionInfo.FortniteVersion >= 5.00 || VersionInfo.FortniteVersion < 1.2 ? "Magnesium (FN %.2f, UE %.2f)" : "Magnesium (FN %.1f, UE %.2f)"), VersionInfo.FortniteVersion, VersionInfo.EngineVersion);
    SetConsoleTitleA(GUI::windowTitle);


    //if constexpr (!FConfiguration::bGUI)
    //    Sleep(2000);

    printf("Hooking & finding offsets... (this may take a while)\n");

    FindNullsAndRetTrues();

    for (auto& NullFunc : NullFuncs)
        if (NullFunc != 0)
        {
            Utils::Patch<uint8_t>(NullFunc, 0xc3);
        }

    for (auto& RetTrueFunc : RetTrueFuncs)
    {
        if (RetTrueFunc == 0)
            continue;

        Utils::Patch<uint32_t>(RetTrueFunc, 0xc0ffc031);
        Utils::Patch<uint8_t>(RetTrueFunc + 4, 0xc3);
    }
    SDK::DbgLog("Main: cp7 (Null/RetTrue patches applied)\n");

    auto GameSessionPatch = FindGameSessionPatch();
    if (GameSessionPatch)
        Utils::Patch<uint8_t>(GameSessionPatch, 0x85);
    SDK::DbgLog("Main: cp8 (GameSessionPatch=%p)\n", (void*)GameSessionPatch);

    MH_Initialize();
    SDK::DbgLog("Main: cp9 (MH_Initialize done, installing %zu hooks)\n", _HookFuncs.size());

    {
        int _hi = 0;
        HMODULE _mod = GetModuleHandleW(L"Magnesium.dll");
        for (auto& HookFunc : _HookFuncs)
        {
            SDK::DbgLog("Main: hook[%d] %s pre\n", _hi,
                _hi < (int)_HookNames.size() ? _HookNames[_hi] : "?");
            HookFunc();
            SDK::DbgLog("Main: hook[%d] post\n", _hi);
            _hi++;
        }
    }
    SDK::DbgLog("Main: cp10 (all _HookFuncs installed)\n");

    MH_EnableHook(MH_ALL_HOOKS);
    SDK::DbgLog("Main: cp11 (hooks enabled)\n");

    auto _gic = FindGIsClient();
    auto _gis = FindGIsServer();
    SDK::DbgLog("Main: GIsClient=%p GIsServer=%p\n", (void*)_gic, (void*)_gis);
    if (_gic)
        *(bool*)_gic = false;
    SDK::DbgLog("Main: cp12 (GIsClient set)\n");
    if (VersionInfo.EngineVersion > 4.20 && _gis) // 3.6 and below have a crash on ALandscapeProxy
        *(bool*)_gis = true;
    SDK::DbgLog("Main: cp13 (GIsServer set)\n");

    srand((uint32_t)time(0));

    if (UWorld::GetWorld()->OwningGameInstance->LocalPlayers.Num() > 0)
    {
        UWorld::GetWorld()->OwningGameInstance->LocalPlayers.Remove(0);
    }
    SDK::DbgLog("Main: cp14 (LocalPlayers handled)\n");

    const wchar_t* terrainOpen = L"open Athena_Terrain";

    if (wcsstr(FConfiguration::Playlist, L"/MoleGame/Playlists/Playlist_MoleGame"))
    {
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"Mole.WorstCasePlayerCount 1"), nullptr);
        terrainOpen = L"open Mole_UnderBase_Parent";
    }
    else if (wcsstr(FConfiguration::Playlist, L"/Game/Gav/Levels/GM_1v1/Playlist_Arena_DefaultSolo_Respawn.Playlist_Arena_DefaultSolo_Respawn"))
    {
        terrainOpen = L"open /Game/Gav/Levels/GM_1v1/Gav_1v1.Gav_1v1";
        FConfiguration::bSiphon = true;
		FConfiguration::SiphonAmount = 200;
        FConfiguration::bInfiniteAmmo = true;
		FConfiguration::bInfiniteMats = true;
        FConfiguration::bJoinInProgress = true;
        FConfiguration::bKeepInventory = true;
		FConfiguration::MaxTickRate = 60.f;
    }
    else if (wcsstr(FConfiguration::Playlist, L"/Buddy/Playlist/Playlist_Retrac_1v1.Playlist_Retrac_1v1"))
    {
        terrainOpen = L"open /Game/1v1s/Retrac_1v1";
        FConfiguration::bSiphon = true;
        FConfiguration::SiphonAmount = 200;
        FConfiguration::bInfiniteAmmo = true;
        FConfiguration::bInfiniteMats = true;
        FConfiguration::bJoinInProgress = true;
        FConfiguration::bKeepInventory = true;
        FConfiguration::MaxTickRate = 60.f;
    }
    else if (wcsstr(FConfiguration::Playlist, L"/Buddy/Playlist/Playlist_Retrac_Turtle.Playlist_Retrac_Turtle"))
    {
        terrainOpen = L"open /Game/Turtle/Retrac_Turtle";
        FConfiguration::bSiphon = true;
        FConfiguration::SiphonAmount = 200;
        FConfiguration::bInfiniteAmmo = true;
        FConfiguration::bInfiniteMats = true;
        FConfiguration::bJoinInProgress = true;
        FConfiguration::bKeepInventory = true;
        FConfiguration::MaxTickRate = 60.f;
    }
    else if (wcsstr(FConfiguration::Playlist, L"/Game/Jett/Playlist_OnlyUp_Jett.Playlist_OnlyUp_Jett"))
    {
        terrainOpen = L"open /Game/Jett/OnlyUp";
        FConfiguration::bEnableCheats = false;
        FConfiguration::bInfiniteAmmo = false;
        FConfiguration::bInfiniteMats = false;
        FConfiguration::bJoinInProgress = true;
        FConfiguration::MaxTickRate = 60.f;
    }
    else if (wcsstr(FConfiguration::Playlist, L"/Game/Jett/TiltedZW/Playlist_TiltedZW_Jett.Playlist_TiltedZW_Jett"))
    {
        terrainOpen = L"open /Game/Jett/TiltedZW/TiltedZW_Jett";
        FConfiguration::bSiphon = true;
        FConfiguration::SiphonAmount = 200;
        FConfiguration::bInfiniteAmmo = true;
        FConfiguration::bInfiniteMats = true;
        FConfiguration::bJoinInProgress = true;
        FConfiguration::bKeepInventory = true;
        FConfiguration::MaxTickRate = 60.f;
    }
    else if (wcsstr(FConfiguration::Playlist, L"/Buddy/Playlists/Playlist_1v1Twine.Playlist_1v1Twine"))
    {
        terrainOpen = L"open /Game/Twine/GameModes/1v1/GameMode_1v1";
        FConfiguration::bSiphon = true;
        FConfiguration::SiphonAmount = 200;
        FConfiguration::bInfiniteAmmo = true;
        FConfiguration::bInfiniteMats = true;
        FConfiguration::bJoinInProgress = true;
        FConfiguration::bKeepInventory = true;
        FConfiguration::MaxTickRate = 60.f;
    }
    else if (wcsstr(FConfiguration::Playlist, L"/Game/Retrac/Playlists/Playlist_ShowdownAlt_Solo_Retrac.Playlist_ShowdownAlt_Solo_Retrac"))
    {
        terrainOpen = L"open /Game/Retrac/Maps/WaterMap";
        FConfiguration::bSiphon = true;
        FConfiguration::SiphonAmount = 200;
        FConfiguration::bInfiniteAmmo = true;
        FConfiguration::bInfiniteMats = true;
        FConfiguration::bJoinInProgress = true;
        FConfiguration::bKeepInventory = true;
        FConfiguration::MaxTickRate = 60.f;
    }
    else if (wcsstr(FConfiguration::Playlist, L"/Game/Athena/Playlists/Respawn/Playlist_Respawn_Solo.Playlist_Respawn_Solo") && VersionInfo.FortniteVersion == 30.00 && GUI::SelectedPlaylist == static_cast<int>(Playlist::Boxfight))
    {
        terrainOpen = L"open /Game/Sawyer/Maps/BoxfightFFA.BoxfightFFA";
        FConfiguration::bSiphon = true;
        FConfiguration::SiphonAmount = 500;
        FConfiguration::bAutoBusStart = false;
        FConfiguration::bInfiniteAmmo = true;
        FConfiguration::bInfiniteMats = true;
        FConfiguration::bJoinInProgress = true;
        FConfiguration::bKeepInventory = true;
        FConfiguration::MaxTickRate = 60.f;
    }
    else if (VersionInfo.FortniteVersion == 7.40 && GUI::SelectedPlaylist == static_cast<int>(Playlist::Backrooms))
    {
        terrainOpen = L"open /Game/Crow/Backrooms/Backrooms";
        FConfiguration::bJoinInProgress = true;
        FConfiguration::bForceRespawns = true;
        FConfiguration::PermanentRespawn = true;
        FConfiguration::HasCustomRespawnPoint = true;
        FConfiguration::CustomRespawnPoint = FVector(0.f, 0.f, 85.275009f);
    }
    else if (FConfiguration::bIsCustomMap && FConfiguration::CustomMap && FConfiguration::CustomMap[0] != L'\0')
    {
        terrainOpen = FConfiguration::CustomMap;
	}
    else if (VersionInfo.FortniteVersion >= 12.00 && wcsstr(FConfiguration::Playlist, L"/Game/Athena/Playlists/Creative/Playlist_PlaygroundV2.Playlist_PlaygroundV2"))
        terrainOpen = L"open Creative_NoApollo_Terrain";
    else
    {
        if (VersionInfo.FortniteVersion >= 27.00)
        {
            if (VersionInfo.FortniteVersion >= 32.00)
                terrainOpen = L"open BlastBerry_Terrain"; // Ch5 S4 (32.11) island — net mode forced to DedicatedServer via AttemptDeriveFromURL hook
            else if (VersionInfo.FortniteVersion >= 28.00)
                terrainOpen = L"open Helios_Terrain";
        }
        else if (VersionInfo.FortniteVersion >= 23.00)
            terrainOpen = L"open Asteria_Terrain";
        else if (VersionInfo.FortniteVersion >= 19.00)
            terrainOpen = L"open Artemis_Terrain";
        else if (VersionInfo.FortniteVersion >= 11.00)
            terrainOpen = L"open Apollo_Terrain";
    }

    // DIAGNOSTIC: defer the map load until AFTER all hooks are installed, so it doesn't run
    // concurrently on the game thread while Main is still installing post-load hooks.
    SDK::DbgLog("Main: cp15 terrain-open DEFERRED (%ls)\n", terrainOpen);

    auto EncryptionPatch = FindEncryptionPatch();
    if (EncryptionPatch)
        Utils::Patch<uint8_t>(EncryptionPatch, 0x74);
    else
        printf("Matchmaking is NOT supported on this version, please make a github issue.\n");
    SDK::DbgLog("Main: cp17 EncryptionPatch=%p\n", (void*)EncryptionPatch);

    SDK::DbgLog("Main: cp17b installing %zu post-load hooks\n", _PostLoadHookFuncs.size());
    {
        int _pi = 0;
        for (auto& HookFunc : _PostLoadHookFuncs)
        {
            SDK::DbgLog("Main: plhook[%d] %s pre\n", _pi,
                _pi < (int)_PostLoadHookNames.size() ? _PostLoadHookNames[_pi] : "?");
            HookFunc();
            SDK::DbgLog("Main: plhook[%d] post\n", _pi);
            _pi++;
        }
    }
    SDK::DbgLog("Main: cp18 PostLoadHooks done\n");

    MH_EnableHook(MH_ALL_HOOKS);

    Misc::bHookedAll = true;
    SDK::DbgLog("Main: cp19 all hooks installed; issuing deferred terrain-open (%ls)\n", terrainOpen);

    // 32.11 ground-truth crash/boot patches (Remix, verified on CL 38202817). These are raw byte
    // patches whose signatures don't exist in Magnesium's universal scanner; version-gated so no
    // other build is touched. The two ChangeGameSessionID rets fix the netmode-init read-null AV.
    if (VersionInfo.FortniteVersion >= 32.00)
    {
        auto base = Memcury::PE::GetModuleBase();
        auto P8  = [&](uintptr_t rva, uint8_t  v) { if (SDK::MemReadable((void*)(base + rva), 1)) Utils::Patch<uint8_t>(base + rva, v); };
        auto P16 = [&](uintptr_t rva, uint16_t v) { if (SDK::MemReadable((void*)(base + rva), 2)) Utils::Patch<uint16_t>(base + rva, v); };
        auto P32 = [&](uintptr_t rva, uint32_t v) { if (SDK::MemReadable((void*)(base + rva), 4)) Utils::Patch<uint32_t>(base + rva, v); };
        SDK::DbgLog("Main: cp19b applying 32.11 crash patches (base=%p)\n", (void*)base);
        P8(0x537F4A0, 0xC3); // UnsafeEnvironment
        P8(0x4336BAC, 0xC3); // RequestExit
        P8(0x27B3958, 0xC3); // ChangeGameSessionID crash 1
        P8(0x27B3598, 0xC3); // ChangeGameSessionID crash 2 (netmode-init read-null AV)
        P8(0x1D91EEC, 0xC3); // GFX crash
        P8(0x2C7D6F0, 0xC3); // Pedestal BeginPlay
        P8(0x1BED9A8, 0xC3); // KickPlayer
        P8(0x338C8F8, 0x01); // SpawnServerActor patch
        P8(0x721A50C, 0xEB);
        P8(0x2C7736C, 0xC3); // controller disconnected
        P8(0x3E2C464, 0xC3); // update required
        P8(0x2C78CDC, 0xC3); // another controller
        P8(0x3E6E09D, 0xEB); // goofy crash
        P8(0x27306AC, 0xC3); // more crash
        P8(0xAF29848, 0xC3); // pawn crash
        P8(0x19D7D70, 0xC3); // weapon crash
        P8(0x2CAF22C, 0xC3); // widget crash
        P8(0x2CB06C8, 0xC3); // widget crash
        P8(0x1E349CB, 0x85); // gamephase step
        P16(0xA6E634D, 0xE990); // respawn kick
        P32(0x20AEF8C, 0xC0FFC031); // localplayer spawnplayactor
        P8(0x20AEF90, 0xC3);
        P8(0x9B2A080, 0xC3); // entitlement crash
        P32(0x7C86D88, 0xC0FFC031); // canactivateability
        P8(0x7C86D8C, 0xC3);
        P8(0x6D3E210, 0xC3); // cosmetic crash
        P8(0xC6A3B28, 0xC3); // fire spread fix
        if (SDK::MemReadable((void*)(base + 0x12D4E1CA), 1)) *(bool*)(base + 0x12D4E1CA) = false;
        if (SDK::MemReadable((void*)(base + 0x12D4E166), 1)) *(bool*)(base + 0x12D4E166) = true;
        SDK::DbgLog("Main: cp19c 32.11 crash patches done\n");
    }

    UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(terrainOpen), nullptr);
    SDK::DbgLog("Main: cp20 terrain-open issued — Main() COMPLETE\n");
}

BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        std::thread(Main).detach();
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
