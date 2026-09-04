#include "pch.h"
#include "../Public/Utils.h"
#include <thread>
#include <iostream>
#include "../Public/Finders.h"
#include "../../FortniteGame/Public/FortInventory.h"
#include <chrono>
#include "../Public/Configuration.h"
#include "../Public/AutoHosting.h"
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

namespace
{
    constexpr wchar_t TransportMappingName[] = L"Local\\Magnesium.Transport.v1";
    constexpr DWORD TransportMagic = 0x4D475450u;
    constexpr DWORD TransportSchema = 1u;
    constexpr DWORD TransportCommitted = 1u;
    constexpr DWORD TransportModeGenericLegacy = 0u;
    constexpr DWORD TransportModeIris = 1u;

    struct FTransportManifest
    {
        volatile LONG Sequence;
        DWORD Magic;
        DWORD Schema;
        DWORD StructSize;
        DWORD PublisherPid;
        DWORD FortniteVersionHundredths;
        DWORD ServerPort;
        DWORD Committed;
        DWORD Mode;
    };

    static_assert(sizeof(FTransportManifest) == 36);
    static_assert(offsetof(FTransportManifest, Sequence) == 0);
    static_assert(offsetof(FTransportManifest, Magic) == 4);
    static_assert(offsetof(FTransportManifest, Mode) == 32);

    HANDLE TransportMapping = nullptr;
    FTransportManifest* TransportManifest = nullptr;

    DWORD GetFortniteVersionHundredths()
    {
        return static_cast<DWORD>(VersionInfo.FortniteVersion * 100.0 + 0.5);
    }

    bool IsDurianLegacyTransport(DWORD FortniteVersionHundredths)
    {
        static constexpr wchar_t DurianPlaylist[] =
            L"/DurianPlaylist/Playlist/Playlist_Durian.Playlist_Durian";
        return FortniteVersionHundredths == 2711u && FConfiguration::Playlist &&
            wcscmp(FConfiguration::Playlist, DurianPlaylist) == 0;
    }

    bool PublishTransportManifest(DWORD FortniteVersionHundredths, bool bUseIris)
    {
        if (!TransportMapping)
        {
            TransportMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                static_cast<DWORD>(sizeof(FTransportManifest)), TransportMappingName);
            if (!TransportMapping)
            {
                SDK::DbgLog("[Transport] CreateFileMapping failed error=%lu\n", GetLastError());
                return false;
            }
        }

        if (!TransportManifest)
        {
            TransportManifest = static_cast<FTransportManifest*>(MapViewOfFile(TransportMapping,
                    FILE_MAP_ALL_ACCESS, 0, 0, sizeof(FTransportManifest)));
            if (!TransportManifest)
            {
                SDK::DbgLog("[Transport] MapViewOfFile failed error=%lu\n", GetLastError());
                return false;
            }
        }

        const LONG CurrentSequence = InterlockedCompareExchange(&TransportManifest->Sequence, 0, 0);
        const LONG OddSequence = (CurrentSequence & 1) ? CurrentSequence + 2 : CurrentSequence + 1;
        InterlockedExchange(&TransportManifest->Sequence, OddSequence);

        TransportManifest->Magic = TransportMagic;
        TransportManifest->Schema = TransportSchema;
        TransportManifest->StructSize = sizeof(FTransportManifest);
        TransportManifest->PublisherPid = GetCurrentProcessId();
        TransportManifest->FortniteVersionHundredths = FortniteVersionHundredths;
        const int ConfiguredPort = FConfiguration::Port.load(std::memory_order_acquire);
        TransportManifest->ServerPort = ConfiguredPort > 0 && ConfiguredPort <= 65535
                ? static_cast<DWORD>(ConfiguredPort) : 7777u;
        TransportManifest->Committed = TransportCommitted;
        TransportManifest->Mode = bUseIris ? TransportModeIris : TransportModeGenericLegacy;

        MemoryBarrier();
        InterlockedExchange(&TransportManifest->Sequence, OddSequence + 1);

        SDK::DbgLog("[Transport] published name=%ls pid=%lu version=%lu "
            "port=%lu mode=%s\n", TransportMappingName, TransportManifest->PublisherPid,
            TransportManifest->FortniteVersionHundredths, TransportManifest->ServerPort,
            bUseIris ? "Iris" : "GenericLegacy");
        return true;
    }

    void FinalizeTransportPolicy()
    {
        const DWORD FortniteVersionHundredths = GetFortniteVersionHundredths();
        const bool bDurianLegacy = IsDurianLegacyTransport(FortniteVersionHundredths);
        const bool bIrisRequested = FConfiguration::bEnableIris.load(std::memory_order_acquire);
        const bool bUseIris = VersionInfo.EngineVersion >= 5.3 && bIrisRequested && !bDurianLegacy;

        FConfiguration::bEnableIris.store(bUseIris, std::memory_order_release);
        PublishTransportManifest(FortniteVersionHundredths, bUseIris);
    }

    using UE421FrontendRenderTask = void (*)(void*, void*);

    UE421FrontendRenderTask UE421FrontendRenderTaskOG = nullptr;
    volatile LONG UE421SkippedNullRenderTasks = 0;

    void UE421FrontendRenderTaskHook(void* Task, void* Context)
    {
        // On a UE 4.21 NullRHI host this task can be queued without the render object at +0x28.
        if (!Task || !*reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(Task) + 0x28))
        {
            if (InterlockedIncrement(&UE421SkippedNullRenderTasks) == 1)
                SDK::DbgLog("[Startup] UE4.21/FN %.2f: skipped invalid NullRHI frontend render task\n",
                    VersionInfo.FortniteVersion);
            return;
        }

        UE421FrontendRenderTaskOG(Task, Context);
    }

    bool InstallUE421FrontendRenderGuard()
    {
        const auto Target = Memcury::Scanner::FindPattern(
            "48 89 5C 24 ? 48 89 74 24 ? 57 48 81 EC 90 00 00 00 "
            "48 8B D9 48 8D 51 08 48 8B 49 28 E8 ? ? ? ?", false).Get();

        if (!Target)
        {
            SDK::DbgLog("[Startup] UE4.21/FN %.2f: frontend render guard target not found\n",
                VersionInfo.FortniteVersion);
            return false;
        }

        const auto InitializeStatus = MH_Initialize();
        if (InitializeStatus != MH_OK && InitializeStatus != MH_ERROR_ALREADY_INITIALIZED)
        {
            SDK::DbgLog(
                "[Startup] UE4.21/FN %.2f: frontend render guard MH_Initialize failed: %s\n",
                VersionInfo.FortniteVersion, MH_StatusToString(InitializeStatus));
            return false;
        }

        const auto CreateStatus = MH_CreateHook(reinterpret_cast<LPVOID>(Target),
            UE421FrontendRenderTaskHook, reinterpret_cast<LPVOID*>(&UE421FrontendRenderTaskOG));
        if (CreateStatus != MH_OK)
        {
            SDK::DbgLog(
                "[Startup] UE4.21/FN %.2f: frontend render guard MH_CreateHook failed: %s\n",
                VersionInfo.FortniteVersion, MH_StatusToString(CreateStatus));
            return false;
        }

        const auto EnableStatus = MH_EnableHook(reinterpret_cast<LPVOID>(Target));
        if (EnableStatus != MH_OK)
        {
            SDK::DbgLog(
                "[Startup] UE4.21/FN %.2f: frontend render guard MH_EnableHook failed: %s\n",
                VersionInfo.FortniteVersion, MH_StatusToString(EnableStatus));
            return false;
        }

        SDK::DbgLog("[Startup] UE4.21/FN %.2f: frontend render guard enabled at RVA 0x%llX\n",
            VersionInfo.FortniteVersion, Target - ImageBase);
        return true;
    }

    bool ShouldPrepareCompatibilityBeforeStart()
    {
        // Every Season 5/6 build on UE 4.21 shares this NullRHI frontend path, not just 6.21.
        return VersionInfo.EngineVersion == 4.21;
    }

    struct FCompatibilityPatchCounts
    {
        size_t Nulls = 0;
        size_t RetTrues = 0;
    };

    FCompatibilityPatchCounts ApplyResolvedCompatibilityPatches()
    {
        FCompatibilityPatchCounts Counts;

        for (auto& NullFunc : NullFuncs)
        {
            if (NullFunc)
            {
                Utils::Patch<uint8_t>(NullFunc, 0xc3);
                ++Counts.Nulls;
            }
        }

        for (auto& RetTrueFunc : RetTrueFuncs)
        {
            if (!RetTrueFunc)
                continue;

            Utils::Patch<uint32_t>(RetTrueFunc, 0xc0ffc031);
            Utils::Patch<uint8_t>(RetTrueFunc + 4, 0xc3);
            ++Counts.RetTrues;
        }

        return Counts;
    }

    void PrepareCompatibilityPatches(bool UseStockErbiumFastPath)
    {
        FindNullsAndRetTrues();

        if (UseStockErbiumFastPath)
        {
            const auto Counts = ApplyResolvedCompatibilityPatches();
            SDK::DbgLog(
                "[Startup] UE4.21/FN %.2f: Erbium fast path applied %zu/%zu Null and %zu/%zu RetTrue patches\n",
                VersionInfo.FortniteVersion, Counts.Nulls, NullFuncs.size(), Counts.RetTrues,
                RetTrueFuncs.size());
            SDK::DbgLog("Main: cp7 (Null/RetTrue patches applied)\n");
            return;
        }

        ApplyResolvedCompatibilityPatches();
        SDK::DbgLog("Main: cp7 (Null/RetTrue patches applied)\n");
    }
}

static bool PinMagnesiumModule()
{
    HMODULE Module = nullptr;
    return GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&PinMagnesiumModule),
        &Module) != FALSE;
}

void Main()
{
    // Live hooks, detached GUI work, and process-lifetime hook state cannot
    // be safely torn down by a mid-session FreeLibrary. Pin before any SDK,
    // hook, GUI, or worker initialization so every callback target remains
    // mapped until process exit.
    if (!PinMagnesiumModule())
    {
        OutputDebugStringW(
            L"Magnesium: Could not pin the module; initialization was stopped.\n");
        return;
    }

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

    printf("Initializing SDK...\n");
    if (!SDK::Init())
    {
        MessageBoxW(nullptr, L"This Magnesium build supports Fortnite releases through 30.x only.",
            L"Unsupported Fortnite version", MB_OK | MB_ICONERROR);
        return;
    }

    if constexpr (FConfiguration::bCustomCrashReporter)
        FCrashReporter::Register();

    AutoHosting::Initialize();

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

    bool bCompatibilityPatchesPrepared = false;
    if (ShouldPrepareCompatibilityBeforeStart())
    {
        SDK::DbgLog("[Startup] UE4.21/FN %.2f: preparing compatibility before Start gate\n",
            VersionInfo.FortniteVersion);
        InstallUE421FrontendRenderGuard();
        PrepareCompatibilityPatches(true);
        bCompatibilityPatchesPrepared = true;
        SDK::DbgLog("[Startup] UE4.21/FN %.2f: compatibility ready; waiting for Start button\n",
            VersionInfo.FortniteVersion);
    }

    if constexpr (FConfiguration::bGUI)
    {
        // Texture loading must run on the game thread, so install this after 4.21's frontend guard.
        if (!Misc::InstallPreStartSafeZoneTick())
            SDK::DbgLog("[SafeZoneMap] pre-Start pump unavailable; disk/numeric fallback remains active\n");
    }

    if constexpr (FConfiguration::bGUI)
    {
        while (!FConfiguration::bReadyToStart)
        {
            Sleep(100);
        }
    }

    SDK::DbgLog("Main: start pressed\n");
    FinalizeTransportPolicy();

    if (VersionInfo.FortniteVersion <= 2.50)
        FConfiguration::bMovingBus = false;

    if (!FConfiguration::IsGliderRedeploySupportedBuild())
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
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(),
            FString(L"log LogSpecialRelevancyHealthComponent None"), nullptr);
    }
    SDK::DbgLog("Main: cp1 (pre EV5.1 / console cmds ok)\n");
    if (VersionInfo.EngineVersion >= 5.1)
    {
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(),
            FString(L"net.AllowEncryption 0"), nullptr);

        SDK::DbgLog("Main: cp2 (net.AllowEncryption ok, pre CurieGlobals)\n");
        auto CurieClass = FindClass("CurieGlobals");
        auto DefaultCurieGlobals = CurieClass ? CurieClass->GetDefaultObj() : nullptr;

        if (DefaultCurieGlobals)
        {
            uint32 Offset = DefaultCurieGlobals->GetOffset("bEnableCurie");
        }
    }
    SDK::DbgLog("Main: cp3 (pre Iris block)\n");
    if (VersionInfo.EngineVersion >= 5.3)
    {
        const bool bUseIris = FConfiguration::bEnableIris.load(std::memory_order_acquire);
        auto IrisBool = FindCVar<uint32_t>(L"net.Iris.UseIrisReplication");
        if (IrisBool)
            *IrisBool = bUseIris ? 1u : 0u;

        SDK::DbgLog("[Transport] net.Iris.UseIrisReplication=%u cvar=%p\n", bUseIris ? 1u : 0u,
            static_cast<void*>(IrisBool));

        if (bUseIris)
        {
            UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(),
                FString(L"log LogIris None"), nullptr);
            UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(),
                FString(L"log LogIrisRpc None"), nullptr);
            UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(),
                FString(L"log LogIrisBridge None"), nullptr);
        }

        SDK::DbgLog("Main: cp4 (Iris cvars done, pre FilterConfigs)\n");
        if (bUseIris && VersionInfo.FortniteVersion >= 29)
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
    }
    SDK::DbgLog("Main: cp5 (Iris block done, pre EV5.4)\n");
    if (VersionInfo.EngineVersion >= 5.4)
    {
        // sprint fix
        auto SprintCVar = FindCVar<uint32_t>(L"Fort.MME.TacticalSprint");
        auto HurdleCVar = FindCVar<uint32_t>(L"Fort.MME.Hurdle");
        auto SlideCVar = FindCVar<uint32_t>(L"Fort.MME.Sliding");
        auto MantleCVar = FindCVar<uint32_t>(L"Fort.MME.Clambering");

        if (FConfiguration::IsKnownS27CustomMapPlaylist() && HurdleCVar)
            *HurdleCVar = false;

        if (SlideCVar)
            *SlideCVar = false;

        if (MantleCVar)
            *MantleCVar = false;
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(),
            FString(L"Fort.MME.TacticalSprint 0"), nullptr);
        if (FConfiguration::IsKnownS27CustomMapPlaylist())
            UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(),
                FString(L"Fort.MME.Hurdle 0"), nullptr);
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(),
            FString(L"Fort.MME.Sliding 0"), nullptr);
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(),
            FString(L"Fort.MME.Clambering 0"), nullptr);
    }

    if constexpr (FConfiguration::WebhookURL && *FConfiguration::WebhookURL)
        curl_global_init(CURL_GLOBAL_ALL);

    SDK::DbgLog("Main: cp6 (pre Hooking / FindNullsAndRetTrues)\n");
    sprintf_s(GUI::windowTitle,
        VersionInfo.EngineVersion >= 5.0 ? "Magnesium (FN %.2f, UE %.1f)" : (VersionInfo.FortniteVersion >= 5.00 || VersionInfo.FortniteVersion < 1.2 ? "Magnesium (FN %.2f, UE %.2f)" : "Magnesium (FN %.1f, UE %.2f)"),
        VersionInfo.FortniteVersion, VersionInfo.EngineVersion);
    SetConsoleTitleA(GUI::windowTitle);

    printf("Hooking & finding offsets... (this may take a while)\n");

    if (!bCompatibilityPatchesPrepared)
    {
        PrepareCompatibilityPatches(false);
    }
    else
    {
        SDK::DbgLog("[Startup] UE4.21/FN %.2f: reusing pre-Start compatibility patches\n",
            VersionInfo.FortniteVersion);
    }

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
    Misc::ActivateServerGetMaxTickRate();
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
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(),
            FString(L"Mole.WorstCasePlayerCount 1"), nullptr);
        terrainOpen = L"open Mole_UnderBase_Parent";
    }
    else if (wcsstr(FConfiguration::Playlist, L"/Game/Gav/Levels/GM_1v1/Playlist_Arena_DefaultSolo_Respawn.Playlist_Arena_DefaultSolo_Respawn"))
    {
        terrainOpen = L"open /Game/Gav/Levels/GM_1v1/Gav_1v1.Gav_1v1";
    }
    else if (wcsstr(FConfiguration::Playlist, L"/Buddy/Playlist/Playlist_Retrac_1v1.Playlist_Retrac_1v1"))
    {
        terrainOpen = L"open /Game/1v1s/Retrac_1v1";
    }
    else if (wcsstr(FConfiguration::Playlist, L"/Buddy/Playlist/Playlist_Retrac_Turtle.Playlist_Retrac_Turtle"))
    {
        terrainOpen = L"open /Game/Turtle/Retrac_Turtle";
    }
    else if (wcsstr(FConfiguration::Playlist, L"/Game/Jett/Playlist_OnlyUp_Jett.Playlist_OnlyUp_Jett"))
    {
        terrainOpen = L"open /Game/Jett/OnlyUp";
    }
    else if (wcsstr(FConfiguration::Playlist, L"/Game/Jett/TiltedZW/Playlist_TiltedZW_Jett.Playlist_TiltedZW_Jett"))
    {
        terrainOpen = L"open /Game/Jett/TiltedZW/TiltedZW_Jett";
    }
    else if (wcsstr(FConfiguration::Playlist, L"/Buddy/Playlists/Playlist_1v1Twine.Playlist_1v1Twine"))
    {
        terrainOpen = L"open /Game/Twine/GameModes/1v1/GameMode_1v1";
    }
    else if (wcsstr(FConfiguration::Playlist, L"/Game/Retrac/Playlists/Playlist_ShowdownAlt_Solo_Retrac.Playlist_ShowdownAlt_Solo_Retrac"))
    {
        terrainOpen = L"open /Game/Retrac/Maps/WaterMap";
    }
    else if (wcsstr(FConfiguration::Playlist, L"/Game/Athena/Playlists/Respawn/Playlist_Respawn_Solo.Playlist_Respawn_Solo") &&
        VersionInfo.FortniteVersion == 30.00 &&
        GUI::GetSelectedPlaylist() == static_cast<int>(Playlist::Boxfight))
    {
        terrainOpen = L"open /Game/Sawyer/Maps/BoxfightFFA.BoxfightFFA";
    }
    else if (VersionInfo.FortniteVersion == 7.40 && GUI::GetSelectedPlaylist() == static_cast<int>(Playlist::Backrooms))
    {
        terrainOpen = L"open /Game/Crow/Backrooms/Backrooms";
    }
    else if (FConfiguration::bIsCustomMap && FConfiguration::CustomMap &&
        FConfiguration::CustomMap[0] != L'\0')
    {
        terrainOpen = FConfiguration::CustomMap;
    }
    else if (VersionInfo.FortniteVersion >= 12.00 && wcsstr(FConfiguration::Playlist, L"/Game/Athena/Playlists/Creative/Playlist_PlaygroundV2.Playlist_PlaygroundV2"))
        terrainOpen = L"open Creative_NoApollo_Terrain";
    else
    {
        if (VersionInfo.FortniteVersion >= 27.00)
        {
            if (VersionInfo.FortniteVersion >= 28.00)
                terrainOpen = L"open Helios_Terrain";
        }
        else if (VersionInfo.FortniteVersion >= 23.00)
            terrainOpen = L"open Asteria_Terrain";
        else if (VersionInfo.FortniteVersion >= 19.00)
            terrainOpen = L"open Artemis_Terrain";
        else if (VersionInfo.FortniteVersion >= 11.00)
            terrainOpen = L"open Apollo_Terrain";
    }

    // UE 4.16 cannot keep ticking its front end once GIsClient is cleared, so 1.7.2 travels immediately.
    const bool bDeferTerrainOpen = VersionInfo.EngineVersion > 4.16;
    if (bDeferTerrainOpen)
    {
        SDK::DbgLog("Main: cp15 terrain-open DEFERRED (%ls)\n", terrainOpen);
    }
    else
    {
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(terrainOpen), nullptr);
        SDK::DbgLog("Main: cp15 terrain-open issued early for UE %.2f (%ls)\n",
            VersionInfo.EngineVersion, terrainOpen);
    }

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
    SDK::DbgLog("Main: cp19 all hooks installed; terrain-open %s (%ls)\n",
        bDeferTerrainOpen ? "pending" : "already issued", terrainOpen);

    if (bDeferTerrainOpen)
    {
        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(terrainOpen), nullptr);
        SDK::DbgLog("Main: cp20 deferred terrain-open issued - Main() COMPLETE\n");
    }
    else
    {
        SDK::DbgLog("Main: cp20 early terrain-open retained - Main() COMPLETE\n");
    }
}

static DWORD WINAPI MainBootstrap(LPVOID)
{
    Main();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(hModule);
        HANDLE Bootstrap = CreateThread(
            nullptr, 0, &MainBootstrap, nullptr, 0, nullptr);
        if (!Bootstrap)
            return FALSE;
        CloseHandle(Bootstrap);
        break;
    }
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}
