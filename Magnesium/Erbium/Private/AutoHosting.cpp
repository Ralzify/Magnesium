#include "pch.h"
#include "../Public/AutoHosting.h"
#include "../Public/Calendar.h"
#include "../Public/Configuration.h"
#include "../Public/GUI.h"
#include "../PlayerAI/Public/MagnesiumPlayerAISettings.h"
#include "../../json.hpp"

#include <ShlObj.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

namespace AutoHosting
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr int SettingsSchemaVersion = 1;
        constexpr ULONGLONG SavePollIntervalMs = 250;
        constexpr ULONGLONG PostMatchShutdownDelayMs = 10000;

        std::atomic<ULONGLONG> GCountdownDeadlineMs{ 0 };
        std::atomic<ULONGLONG> GPostMatchShutdownDeadlineMs{ 0 };
        std::atomic_bool GRestoredPreferences{ false };

        nlohmann::json GDocument = nlohmann::json::object();
        nlohmann::json GDefaultPreferences =
            nlohmann::json::object();
        nlohmann::json GStoredPreferences = nlohmann::json::object();
        std::string GLastSerializedDocument;
        ULONGLONG GNextSavePollMs = 0;

        // FConfiguration stores resolved paths as pointers. These owners keep
        // strings restored from JSON alive for the entire process.
        std::wstring GPlaylistPath;
        std::wstring GCreativePlotPath;
        std::wstring GCustomMapPath;

        template <typename T>
        T ClampValue(T Value, T Minimum, T Maximum)
        {
            return (std::max)(Minimum, (std::min)(Value, Maximum));
        }

        bool ReadBool(
            const nlohmann::json& Object,
            const char* Key,
            bool Fallback)
        {
            const auto It = Object.find(Key);
            return It != Object.end() && It->is_boolean()
                ? It->get<bool>()
                : Fallback;
        }

        int ReadInt(
            const nlohmann::json& Object,
            const char* Key,
            int Fallback)
        {
            const auto It = Object.find(Key);
            return It != Object.end() && It->is_number_integer()
                ? It->get<int>()
                : Fallback;
        }

        float ReadFloat(
            const nlohmann::json& Object,
            const char* Key,
            float Fallback)
        {
            const auto It = Object.find(Key);
            return It != Object.end() && It->is_number()
                ? It->get<float>()
                : Fallback;
        }

        double ReadDouble(
            const nlohmann::json& Object,
            const char* Key,
            double Fallback)
        {
            const auto It = Object.find(Key);
            return It != Object.end() && It->is_number()
                ? It->get<double>()
                : Fallback;
        }

        std::string ReadString(
            const nlohmann::json& Object,
            const char* Key,
            const std::string& Fallback = {})
        {
            const auto It = Object.find(Key);
            return It != Object.end() && It->is_string()
                ? It->get<std::string>()
                : Fallback;
        }

        const nlohmann::json& ReadObject(
            const nlohmann::json& Parent,
            const char* Key)
        {
            static const nlohmann::json Empty =
                nlohmann::json::object();
            const auto It = Parent.find(Key);
            return It != Parent.end() && It->is_object()
                ? *It
                : Empty;
        }

        std::string WideToUtf8(const wchar_t* Value)
        {
            if (!Value || !*Value)
                return {};

            const int WideLength =
                static_cast<int>(wcslen(Value));
            const int Utf8Length = WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                Value,
                WideLength,
                nullptr,
                0,
                nullptr,
                nullptr);
            if (Utf8Length <= 0)
                return {};

            std::string Result(
                static_cast<size_t>(Utf8Length), '\0');
            if (WideCharToMultiByte(
                    CP_UTF8,
                    WC_ERR_INVALID_CHARS,
                    Value,
                    WideLength,
                    Result.data(),
                    Utf8Length,
                    nullptr,
                    nullptr) <= 0)
            {
                return {};
            }
            return Result;
        }

        std::wstring Utf8ToWide(const std::string& Value)
        {
            if (Value.empty())
                return {};

            const int WideLength = MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                Value.data(),
                static_cast<int>(Value.size()),
                nullptr,
                0);
            if (WideLength <= 0)
                return {};

            std::wstring Result(
                static_cast<size_t>(WideLength), L'\0');
            if (MultiByteToWideChar(
                    CP_UTF8,
                    MB_ERR_INVALID_CHARS,
                    Value.data(),
                    static_cast<int>(Value.size()),
                    Result.data(),
                    WideLength) <= 0)
            {
                return {};
            }
            return Result;
        }

        std::string FStringToUtf8(const FString& Value)
        {
            if (!Value.Data || Value.NumElements <= 1)
                return {};
            return WideToUtf8(Value.Data);
        }

        FString Utf8ToFString(const std::string& Value)
        {
            const std::wstring WideValue = Utf8ToWide(Value);
            return FString(WideValue.c_str());
        }

        std::string CurrentProfileKey()
        {
            std::ostringstream Stream;
            Stream.imbue(std::locale::classic());
            Stream << "fn_" << std::fixed << std::setprecision(2)
                << VersionInfo.FortniteVersion;
            return Stream.str();
        }

        fs::path SettingsPath()
        {
            wchar_t LocalAppData[MAX_PATH]{};
            if (FAILED(SHGetFolderPathW(
                    nullptr,
                    CSIDL_LOCAL_APPDATA,
                    nullptr,
                    SHGFP_TYPE_CURRENT,
                    LocalAppData)))
            {
                return {};
            }

            return fs::path(LocalAppData) /
                L"Magnesium" / L"auto_hosting.json";
        }

        nlohmann::json CapturePreferences()
        {
            nlohmann::json Preferences;

            Preferences["selection"] = {
                { "playlist", GUI::SelectedPlaylist },
                { "creative_plot", GUI::SelectedPlot },
                { "custom_map", GUI::SelectedMap }
            };

            Preferences["resolved_paths"] = {
                { "playlist", WideToUtf8(FConfiguration::Playlist) },
                { "creative_plot", WideToUtf8(FConfiguration::CreativePlot) },
                { "custom_map", WideToUtf8(FConfiguration::CustomMap) }
            };

            Preferences["match"] = {
                { "auto_bus_start", FConfiguration::bAutoBusStart.load(std::memory_order_acquire) },
                { "bus_start_delay", FConfiguration::BusStartDelay.load(std::memory_order_acquire) },
                { "bus_settings_user_override", FConfiguration::bBusSettingsUserOverride.load(std::memory_order_acquire) },
                { "auto_dump", FConfiguration::bAutoDump.load(std::memory_order_acquire) },
                { "use_custom_map", FConfiguration::bIsCustomMap.load(std::memory_order_acquire) },
                { "one_kill_ends_game", FConfiguration::AutoEndGame.load(std::memory_order_acquire) },
                { "show_trickshot_tab", FConfiguration::bEnableTrickshotTab.load(std::memory_order_acquire) },
                { "max_tick_rate", FConfiguration::MaxTickRate.load(std::memory_order_acquire) },
                { "port", FConfiguration::Port.load(std::memory_order_acquire) },
                { "player_has_pickaxe", FConfiguration::bHasPickaxe.load(std::memory_order_acquire) }
            };

            Preferences["event"] = {
                { "auto_start", FConfiguration::bAutoStartEvent.load(std::memory_order_acquire) },
                { "start_delay", FConfiguration::EventStartTime.load(std::memory_order_acquire) }
            };

            Preferences["calendar"] = {
                { "snow_on_match_start", FConfiguration::bSnowOnMatchStart.load(std::memory_order_acquire) },
                { "snow_value", FConfiguration::SnowValue.load(std::memory_order_acquire) }
            };

            Preferences["lategame"] = {
                { "enabled", FConfiguration::bLateGame.load(std::memory_order_acquire) },
                { "moving_bus", FConfiguration::bMovingBus.load(std::memory_order_acquire) },
                { "long_zone", FConfiguration::bLateGameLongZone.load(std::memory_order_acquire) },
                { "versionized_loadout", FConfiguration::bUseVersionizedLoadout.load(std::memory_order_acquire) },
                { "custom_loadout", FConfiguration::bUseCustomLoadout.load(std::memory_order_acquire) },
                { "starting_zone", FConfiguration::LateGameZone.load(std::memory_order_acquire) },
                { "custom_safe_zone", FConfiguration::bCustomSafeZone.load(std::memory_order_acquire) },
                { "safe_zone_center_x", FConfiguration::CustomSafeZoneCenter.X },
                { "safe_zone_center_y", FConfiguration::CustomSafeZoneCenter.Y },
                { "safe_zone_center_z", FConfiguration::CustomSafeZoneCenter.Z },
                { "safe_zone_radius", FConfiguration::CustomSafeZoneRadius.load(std::memory_order_acquire) }
            };

            float SafeZoneU = 0.5f;
            float SafeZoneV = 0.5f;
            const bool bHasNormalizedSafeZone =
                GUI::GetNormalizedSafeZoneSelection(
                    SafeZoneU, SafeZoneV);
            Preferences["lategame"]["has_normalized_safe_zone"] =
                bHasNormalizedSafeZone;
            Preferences["lategame"]["safe_zone_u"] = SafeZoneU;
            Preferences["lategame"]["safe_zone_v"] = SafeZoneV;

            Preferences["loadout"] = {
                { "primary", FStringToUtf8(FConfiguration::Primary) },
                { "primary_amount", FConfiguration::PrimaryAmount.load(std::memory_order_acquire) },
                { "secondary", FStringToUtf8(FConfiguration::Secondary) },
                { "secondary_amount", FConfiguration::SecondaryAmount.load(std::memory_order_acquire) },
                { "tertiary", FStringToUtf8(FConfiguration::Tertiary) },
                { "tertiary_amount", FConfiguration::TertiaryAmount.load(std::memory_order_acquire) },
                { "quaternary", FStringToUtf8(FConfiguration::Quaternary) },
                { "quaternary_amount", FConfiguration::QuaternaryAmount.load(std::memory_order_acquire) },
                { "quinary", FStringToUtf8(FConfiguration::Quinary) },
                { "quinary_amount", FConfiguration::QuinaryAmount.load(std::memory_order_acquire) },
                { "traps", FStringToUtf8(FConfiguration::Traps) },
                { "traps_amount", FConfiguration::TrapsAmount.load(std::memory_order_acquire) }
            };

            Preferences["respawns"] = {
                { "enabled", FConfiguration::bForceRespawns.load(std::memory_order_acquire) },
                { "storm_respawns", FConfiguration::PermanentRespawn.load(std::memory_order_acquire) },
                { "keep_inventory", FConfiguration::bKeepInventory.load(std::memory_order_acquire) },
                { "midzone_respawns", FConfiguration::bMidZoneRespawning.load(std::memory_order_acquire) },
                { "join_in_progress", FConfiguration::bJoinInProgress.load(std::memory_order_acquire) },
                { "respawn_time", FConfiguration::RespawnTime.load(std::memory_order_acquire) },
                { "respawn_height", FConfiguration::RespawnHeight.load(std::memory_order_acquire) },
                { "has_custom_point", FConfiguration::HasCustomRespawnPoint.load(std::memory_order_acquire) },
                { "custom_point_x", FConfiguration::CustomRespawnPoint.X },
                { "custom_point_y", FConfiguration::CustomRespawnPoint.Y },
                { "custom_point_z", FConfiguration::CustomRespawnPoint.Z }
            };

            Preferences["ltm_configuration"] = {
                { "food_fight_objective_health",
                    FConfiguration::FoodFightObjectiveHealth.load(
                        std::memory_order_acquire) }
            };

            Preferences["gameplay"] = {
                { "glider_redeploy", FConfiguration::bGliderRedeploy.load(std::memory_order_acquire) },
                { "infinite_materials", FConfiguration::bInfiniteMats.load(std::memory_order_acquire) },
                { "infinite_ammo", FConfiguration::bInfiniteAmmo.load(std::memory_order_acquire) },
                { "cheat_commands", FConfiguration::bEnableCheats.load(std::memory_order_acquire) },
                { "siphon", FConfiguration::bSiphon.load(std::memory_order_acquire) },
                { "siphon_amount", FConfiguration::SiphonAmount.load(std::memory_order_acquire) },
                { "siphon_animation", FConfiguration::SiphonAnimType.load(std::memory_order_acquire) },
                { "dbno", FConfiguration::bEnableDBNO.load(std::memory_order_acquire) }
            };

            Preferences["bots"] = {
                { "enable_ai_players", MagnesiumPlayerAISettings::bEnableAIs.load(std::memory_order_acquire) },
                { "health", FConfiguration::BotHealth.load(std::memory_order_acquire) },
                { "shield", FConfiguration::BotShield.load(std::memory_order_acquire) },
                { "use_custom_names", FConfiguration::UseCustomBotNames.load(std::memory_order_acquire) },
                { "name", FConfiguration::BotName }
            };

            Preferences["trickshot"] = {
                { "swag_lines", FConfiguration::bUseWinLines.load(std::memory_order_acquire) },
                { "infinite_render", FConfiguration::bInfiniteRender.load(std::memory_order_acquire) },
                { "randomize_arena_points", FConfiguration::RandomizeArenaPoints.load(std::memory_order_acquire) },
                { "player_map_icons", FConfiguration::bPlayerMapIcons.load(std::memory_order_acquire) },
                { "auto_god_mode", FConfiguration::bAutoGodMode.load(std::memory_order_acquire) },
                { "auto_god_mode_type", FConfiguration::AutoGodModeType.load(std::memory_order_acquire) },
                { "auto_god_mode_exclude_last_player", FConfiguration::bAutoGodModeExcludeLastPlayer.load(std::memory_order_acquire) },
                { "randomize_kills", FConfiguration::RandomizeKills.load(std::memory_order_acquire) },
                { "randomize_levels", FConfiguration::RandomizeLevels.load(std::memory_order_acquire) },
                { "disable_jump_fatigue", FConfiguration::bDisableJumpFatigue.load(std::memory_order_acquire) },
                { "disable_supply_drops", FConfiguration::bDisableSupplyDrops.load(std::memory_order_acquire) },
                { "vehicle_bump_launch", FConfiguration::bVehicleBumpLaunch.load(std::memory_order_acquire) },
                { "cannon_launch_animations", FConfiguration::bCannonLaunchAnimations.load(std::memory_order_acquire) },
                { "cannon_launch_x_multiplier", FConfiguration::CannonLaunchXMultiplier.load(std::memory_order_acquire) },
                { "cannon_launch_y_multiplier", FConfiguration::CannonLaunchYMultiplier.load(std::memory_order_acquire) },
                { "cannon_launch_z_multiplier", FConfiguration::CannonLaunchZMultiplier.load(std::memory_order_acquire) },
                { "crown_slow_motion", FConfiguration::bCrownSlomo.load(std::memory_order_acquire) },
                { "cancel_velocity_on_win", FConfiguration::bCancelVelocityOnWin.load(std::memory_order_acquire) },
                { "auto_pause_time_of_day", FConfiguration::bAutoPauseTODM.load(std::memory_order_acquire) },
                { "time_of_day", FConfiguration::TODMTime.load(std::memory_order_acquire) }
            };

            return Preferences;
        }

        void RefreshPostStartPreferences()
        {
            // A full snapshot is always taken by either the manual Start
            // action or the Auto Host countdown before bReadyToStart is
            // published. After that point, refresh the options which remain
            // editable in the Match and Trickshot stages. Keeping the
            // selection/path/loadout portions of the launch snapshot intact
            // also avoids sampling their non-atomic owners while the server
            // thread is active.
            if (!GStoredPreferences.is_object() ||
                GStoredPreferences.empty())
            {
                GStoredPreferences = CapturePreferences();
                return;
            }

            auto& Match = GStoredPreferences["match"];
            if (!Match.is_object())
                Match = nlohmann::json::object();
            Match["auto_bus_start"] =
                FConfiguration::bAutoBusStart.load(
                    std::memory_order_acquire);
            Match["bus_start_delay"] =
                FConfiguration::BusStartDelay.load(
                    std::memory_order_acquire);
            Match["bus_settings_user_override"] =
                FConfiguration::bBusSettingsUserOverride.load(
                    std::memory_order_acquire);
            Match["auto_dump"] =
                FConfiguration::bAutoDump.load(
                    std::memory_order_acquire);
            Match["use_custom_map"] =
                FConfiguration::bIsCustomMap.load(
                    std::memory_order_acquire);
            Match["one_kill_ends_game"] =
                FConfiguration::AutoEndGame.load(
                    std::memory_order_acquire);
            Match["show_trickshot_tab"] =
                FConfiguration::bEnableTrickshotTab.load(
                    std::memory_order_acquire);
            Match["max_tick_rate"] =
                FConfiguration::MaxTickRate.load(
                    std::memory_order_acquire);
            Match["port"] =
                FConfiguration::Port.load(
                    std::memory_order_acquire);
            Match["player_has_pickaxe"] =
                FConfiguration::bHasPickaxe.load(
                    std::memory_order_acquire);

            auto& Respawns = GStoredPreferences["respawns"];
            if (!Respawns.is_object())
                Respawns = nlohmann::json::object();
            Respawns["enabled"] =
                FConfiguration::bForceRespawns.load(
                    std::memory_order_acquire);
            Respawns["storm_respawns"] =
                FConfiguration::PermanentRespawn.load(
                    std::memory_order_acquire);
            Respawns["keep_inventory"] =
                FConfiguration::bKeepInventory.load(
                    std::memory_order_acquire);
            Respawns["midzone_respawns"] =
                FConfiguration::bMidZoneRespawning.load(
                    std::memory_order_acquire);
            Respawns["join_in_progress"] =
                FConfiguration::bJoinInProgress.load(
                    std::memory_order_acquire);
            Respawns["respawn_time"] =
                FConfiguration::RespawnTime.load(
                    std::memory_order_acquire);
            Respawns["respawn_height"] =
                FConfiguration::RespawnHeight.load(
                    std::memory_order_acquire);

            auto& LTMConfiguration =
                GStoredPreferences["ltm_configuration"];
            if (!LTMConfiguration.is_object())
                LTMConfiguration = nlohmann::json::object();
            LTMConfiguration["food_fight_objective_health"] =
                FConfiguration::FoodFightObjectiveHealth.load(
                    std::memory_order_acquire);

            auto& Gameplay = GStoredPreferences["gameplay"];
            if (!Gameplay.is_object())
                Gameplay = nlohmann::json::object();
            Gameplay["glider_redeploy"] =
                FConfiguration::bGliderRedeploy.load(
                    std::memory_order_acquire);
            Gameplay["infinite_materials"] =
                FConfiguration::bInfiniteMats.load(
                    std::memory_order_acquire);
            Gameplay["infinite_ammo"] =
                FConfiguration::bInfiniteAmmo.load(
                    std::memory_order_acquire);
            Gameplay["cheat_commands"] =
                FConfiguration::bEnableCheats.load(
                    std::memory_order_acquire);
            Gameplay["siphon"] =
                FConfiguration::bSiphon.load(
                    std::memory_order_acquire);
            Gameplay["siphon_amount"] =
                FConfiguration::SiphonAmount.load(
                    std::memory_order_acquire);
            Gameplay["siphon_animation"] =
                FConfiguration::SiphonAnimType.load(
                    std::memory_order_acquire);
            Gameplay["dbno"] =
                FConfiguration::bEnableDBNO.load(
                    std::memory_order_acquire);

            auto& Trickshot = GStoredPreferences["trickshot"];
            if (!Trickshot.is_object())
                Trickshot = nlohmann::json::object();
            Trickshot["swag_lines"] =
                FConfiguration::bUseWinLines.load(
                    std::memory_order_acquire);
            Trickshot["infinite_render"] =
                FConfiguration::bInfiniteRender.load(
                    std::memory_order_acquire);
            Trickshot["randomize_arena_points"] =
                FConfiguration::RandomizeArenaPoints.load(
                    std::memory_order_acquire);
            Trickshot["player_map_icons"] =
                FConfiguration::bPlayerMapIcons.load(
                    std::memory_order_acquire);
            Trickshot["auto_god_mode"] =
                FConfiguration::bAutoGodMode.load(
                    std::memory_order_acquire);
            Trickshot["auto_god_mode_type"] =
                FConfiguration::AutoGodModeType.load(
                    std::memory_order_acquire);
            Trickshot["auto_god_mode_exclude_last_player"] =
                FConfiguration::bAutoGodModeExcludeLastPlayer.load(
                    std::memory_order_acquire);
            Trickshot["randomize_kills"] =
                FConfiguration::RandomizeKills.load(
                    std::memory_order_acquire);
            Trickshot["randomize_levels"] =
                FConfiguration::RandomizeLevels.load(
                    std::memory_order_acquire);
            Trickshot["disable_jump_fatigue"] =
                FConfiguration::bDisableJumpFatigue.load(
                    std::memory_order_acquire);
            Trickshot["disable_supply_drops"] =
                FConfiguration::bDisableSupplyDrops.load(
                    std::memory_order_acquire);
            Trickshot["vehicle_bump_launch"] =
                FConfiguration::bVehicleBumpLaunch.load(
                    std::memory_order_acquire);
            Trickshot["cannon_launch_animations"] =
                FConfiguration::bCannonLaunchAnimations.load(
                    std::memory_order_acquire);
            Trickshot["cannon_launch_x_multiplier"] =
                FConfiguration::CannonLaunchXMultiplier.load(
                    std::memory_order_acquire);
            Trickshot["cannon_launch_y_multiplier"] =
                FConfiguration::CannonLaunchYMultiplier.load(
                    std::memory_order_acquire);
            Trickshot["cannon_launch_z_multiplier"] =
                FConfiguration::CannonLaunchZMultiplier.load(
                    std::memory_order_acquire);
            Trickshot["crown_slow_motion"] =
                FConfiguration::bCrownSlomo.load(
                    std::memory_order_acquire);
            Trickshot["cancel_velocity_on_win"] =
                FConfiguration::bCancelVelocityOnWin.load(
                    std::memory_order_acquire);
            Trickshot["auto_pause_time_of_day"] =
                FConfiguration::bAutoPauseTODM.load(
                    std::memory_order_acquire);
            Trickshot["time_of_day"] =
                FConfiguration::TODMTime.load(
                    std::memory_order_acquire);

            auto& CalendarPreferences =
                GStoredPreferences["calendar"];
            if (!CalendarPreferences.is_object())
                CalendarPreferences = nlohmann::json::object();
            CalendarPreferences["snow_on_match_start"] =
                FConfiguration::bSnowOnMatchStart.load(
                    std::memory_order_acquire);
            CalendarPreferences["snow_value"] =
                FConfiguration::SnowValue.load(
                    std::memory_order_acquire);
        }

        bool ApplyPreferences(const nlohmann::json& Preferences)
        {
            if (!Preferences.is_object())
                return false;

            const auto& Selection =
                ReadObject(Preferences, "selection");
            const int SelectedPlaylist = ClampValue(
                ReadInt(
                    Selection,
                    "playlist",
                    static_cast<int>(Playlist::Solos)),
                static_cast<int>(Playlist::Solos),
                static_cast<int>(Playlist::ScoreRoyaleSquads));
            const int SelectedPlot = ClampValue(
                ReadInt(
                    Selection,
                    "creative_plot",
                    static_cast<int>(Plot::Temperate)),
                static_cast<int>(Plot::Temperate),
                static_cast<int>(Plot::Custom));
            const int SelectedMap = ClampValue(
                ReadInt(
                    Selection,
                    "custom_map",
                    static_cast<int>(Map::Faceoff)),
                static_cast<int>(Map::Papaya),
                static_cast<int>(Map::PropHunt));

            GUI::SelectedPlaylist = SelectedPlaylist;
            GUI::SelectedPlot = SelectedPlot;
            GUI::SelectedMap = SelectedMap;
            GUI::PublishSelectedPlaylist(SelectedPlaylist);

            const auto& Paths =
                ReadObject(Preferences, "resolved_paths");
            const std::string PlaylistPath =
                ReadString(Paths, "playlist");
            const std::string CreativePlotPath =
                ReadString(Paths, "creative_plot");
            const std::string CustomMapPath =
                ReadString(Paths, "custom_map");
            if (PlaylistPath.empty())
                return false;

            GPlaylistPath = Utf8ToWide(PlaylistPath);
            GCreativePlotPath = Utf8ToWide(CreativePlotPath);
            GCustomMapPath = Utf8ToWide(CustomMapPath);
            if (GPlaylistPath.empty())
                return false;

            FConfiguration::Playlist = GPlaylistPath.c_str();
            FConfiguration::CreativePlot =
                GCreativePlotPath.c_str();
            FConfiguration::CustomMap = GCustomMapPath.c_str();

            const auto& Match =
                ReadObject(Preferences, "match");
            FConfiguration::bAutoBusStart.store(
                ReadBool(Match, "auto_bus_start", true),
                std::memory_order_release);
            FConfiguration::BusStartDelay.store(
                ClampValue(
                    ReadFloat(Match, "bus_start_delay", 90.f),
                    0.f, 300.f),
                std::memory_order_release);
            FConfiguration::bBusSettingsUserOverride.store(
                ReadBool(
                    Match,
                    "bus_settings_user_override",
                    false),
                std::memory_order_release);
            FConfiguration::bAutoDump.store(
                ReadBool(Match, "auto_dump", true),
                std::memory_order_release);
            FConfiguration::bIsCustomMap.store(
                ReadBool(Match, "use_custom_map", false),
                std::memory_order_release);
            FConfiguration::AutoEndGame.store(
                ReadBool(
                    Match,
                    "one_kill_ends_game",
                    false),
                std::memory_order_release);
            FConfiguration::bEnableTrickshotTab.store(
                ReadBool(
                    Match,
                    "show_trickshot_tab",
                    false),
                std::memory_order_release);
            FConfiguration::MaxTickRate.store(
                ClampValue(
                    ReadFloat(Match, "max_tick_rate", 30.f),
                    5.f, 180.f),
                std::memory_order_release);
            FConfiguration::Port.store(
                ClampValue(
                    ReadInt(Match, "port", 7777),
                    1, 65535),
                std::memory_order_release);
            FConfiguration::bHasPickaxe.store(
                ReadBool(
                    Match,
                    "player_has_pickaxe",
                    true),
                std::memory_order_release);

            const auto& Event =
                ReadObject(Preferences, "event");
            FConfiguration::bAutoStartEvent.store(
                ReadBool(Event, "auto_start", false),
                std::memory_order_release);
            FConfiguration::EventStartTime.store(
                ClampValue(
                    ReadFloat(Event, "start_delay", 120.f),
                    30.f, 300.f),
                std::memory_order_release);

            const auto& LateGame =
                ReadObject(Preferences, "lategame");
            FConfiguration::bLateGame.store(
                ReadBool(LateGame, "enabled", true),
                std::memory_order_release);
            FConfiguration::bMovingBus.store(
                ReadBool(LateGame, "moving_bus", true),
                std::memory_order_release);
            FConfiguration::bLateGameLongZone.store(
                ReadBool(LateGame, "long_zone", false),
                std::memory_order_release);
            FConfiguration::bUseVersionizedLoadout.store(
                ReadBool(
                    LateGame,
                    "versionized_loadout",
                    true),
                std::memory_order_release);
            FConfiguration::bUseCustomLoadout.store(
                ReadBool(
                    LateGame,
                    "custom_loadout",
                    false),
                std::memory_order_release);
            FConfiguration::LateGameZone.store(
                ClampValue(
                    ReadInt(LateGame, "starting_zone", 4),
                    1, 7),
                std::memory_order_release);
            FConfiguration::bCustomSafeZone.store(
                ReadBool(
                    LateGame,
                    "custom_safe_zone",
                    false),
                std::memory_order_release);
            FConfiguration::CustomSafeZoneCenter =
                FVector(
                    ReadDouble(
                        LateGame,
                        "safe_zone_center_x",
                        0.0),
                    ReadDouble(
                        LateGame,
                        "safe_zone_center_y",
                        0.0),
                    ReadDouble(
                        LateGame,
                        "safe_zone_center_z",
                        0.0));
            FConfiguration::CustomSafeZoneRadius.store(
                ClampValue(
                    ReadFloat(
                        LateGame,
                        "safe_zone_radius",
                        100000.f),
                    500.f, 100000.f),
                std::memory_order_release);
            GUI::RestoreNormalizedSafeZoneSelection(
                ReadBool(
                    LateGame,
                    "has_normalized_safe_zone",
                    false),
                ClampValue(
                    ReadFloat(
                        LateGame,
                        "safe_zone_u",
                        0.5f),
                    0.f, 1.f),
                ClampValue(
                    ReadFloat(
                        LateGame,
                        "safe_zone_v",
                        0.5f),
                    0.f, 1.f));

            const auto& Loadout =
                ReadObject(Preferences, "loadout");
            FConfiguration::Primary = Utf8ToFString(
                ReadString(Loadout, "primary"));
            FConfiguration::Secondary = Utf8ToFString(
                ReadString(Loadout, "secondary"));
            FConfiguration::Tertiary = Utf8ToFString(
                ReadString(Loadout, "tertiary"));
            FConfiguration::Quaternary = Utf8ToFString(
                ReadString(Loadout, "quaternary"));
            FConfiguration::Quinary = Utf8ToFString(
                ReadString(Loadout, "quinary"));
            FConfiguration::Traps = Utf8ToFString(
                ReadString(Loadout, "traps"));
            FConfiguration::PrimaryAmount.store(
                (std::max)(
                    0,
                    ReadInt(
                        Loadout,
                        "primary_amount",
                        1)),
                std::memory_order_release);
            FConfiguration::SecondaryAmount.store(
                (std::max)(
                    0,
                    ReadInt(
                        Loadout,
                        "secondary_amount",
                        1)),
                std::memory_order_release);
            FConfiguration::TertiaryAmount.store(
                (std::max)(
                    0,
                    ReadInt(
                        Loadout,
                        "tertiary_amount",
                        1)),
                std::memory_order_release);
            FConfiguration::QuaternaryAmount.store(
                (std::max)(
                    0,
                    ReadInt(
                        Loadout,
                        "quaternary_amount",
                        1)),
                std::memory_order_release);
            FConfiguration::QuinaryAmount.store(
                (std::max)(
                    0,
                    ReadInt(
                        Loadout,
                        "quinary_amount",
                        1)),
                std::memory_order_release);
            FConfiguration::TrapsAmount.store(
                (std::max)(
                    0,
                    ReadInt(
                        Loadout,
                        "traps_amount",
                        6)),
                std::memory_order_release);

            const auto& Respawns =
                ReadObject(Preferences, "respawns");
            FConfiguration::bForceRespawns.store(
                ReadBool(Respawns, "enabled", false),
                std::memory_order_release);
            FConfiguration::PermanentRespawn.store(
                ReadBool(
                    Respawns,
                    "storm_respawns",
                    false),
                std::memory_order_release);
            FConfiguration::bKeepInventory.store(
                ReadBool(
                    Respawns,
                    "keep_inventory",
                    false),
                std::memory_order_release);
            FConfiguration::bMidZoneRespawning.store(
                ReadBool(
                    Respawns,
                    "midzone_respawns",
                    false),
                std::memory_order_release);
            FConfiguration::bJoinInProgress.store(
                ReadBool(
                    Respawns,
                    "join_in_progress",
                    false),
                std::memory_order_release);
            FConfiguration::RespawnTime.store(
                ClampValue(
                    ReadInt(Respawns, "respawn_time", 3),
                    1, 10),
                std::memory_order_release);
            FConfiguration::RespawnHeight.store(
                ClampValue(
                    ReadInt(
                        Respawns,
                        "respawn_height",
                        20000),
                    1000, 50000),
                std::memory_order_release);
            FConfiguration::HasCustomRespawnPoint.store(
                ReadBool(
                    Respawns,
                    "has_custom_point",
                    false),
                std::memory_order_release);
            FConfiguration::CustomRespawnPoint =
                FVector(
                    ReadDouble(
                        Respawns,
                        "custom_point_x",
                        0.0),
                    ReadDouble(
                        Respawns,
                        "custom_point_y",
                        0.0),
                    ReadDouble(
                        Respawns,
                        "custom_point_z",
                        0.0));

            const auto& LTMConfiguration =
                ReadObject(Preferences, "ltm_configuration");
            const int FoodFightObjectiveHealth = ReadInt(
                LTMConfiguration,
                "food_fight_objective_health",
                FConfiguration::FoodFightObjectiveHealthAuthored);
            FConfiguration::FoodFightObjectiveHealth.store(
                FoodFightObjectiveHealth ==
                        FConfiguration::
                            FoodFightObjectiveHealthAuthored
                    ? FConfiguration::
                        FoodFightObjectiveHealthAuthored
                    : ClampValue(
                          FoodFightObjectiveHealth,
                          FConfiguration::
                              FoodFightObjectiveHealthMinimum,
                          FConfiguration::
                              GetFoodFightObjectiveHealthMaximum()),
                std::memory_order_release);

            const auto& Gameplay =
                ReadObject(Preferences, "gameplay");
            FConfiguration::bGliderRedeploy.store(
                FConfiguration::
                    IsGliderRedeploySupportedBuild() &&
                ReadBool(
                    Gameplay,
                    "glider_redeploy",
                    false),
                std::memory_order_release);
            FConfiguration::bInfiniteMats.store(
                ReadBool(
                    Gameplay,
                    "infinite_materials",
                    true),
                std::memory_order_release);
            FConfiguration::bInfiniteAmmo.store(
                ReadBool(
                    Gameplay,
                    "infinite_ammo",
                    true),
                std::memory_order_release);
            FConfiguration::bEnableCheats.store(
                ReadBool(
                    Gameplay,
                    "cheat_commands",
                    false),
                std::memory_order_release);
            FConfiguration::bSiphon.store(
                ReadBool(Gameplay, "siphon", false),
                std::memory_order_release);
            FConfiguration::SiphonAmount.store(
                ReadInt(
                    Gameplay,
                    "siphon_amount",
                    50),
                std::memory_order_release);
            FConfiguration::SiphonAnimType.store(
                (std::max)(
                    0,
                    ReadInt(
                        Gameplay,
                        "siphon_animation",
                        0)),
                std::memory_order_release);
            FConfiguration::bEnableDBNO.store(
                ReadBool(Gameplay, "dbno", true),
                std::memory_order_release);

            const auto& Bots =
                ReadObject(Preferences, "bots");
            MagnesiumPlayerAISettings::bEnableAIs.store(
                ReadBool(
                    Bots,
                    "enable_ai_players",
                    false),
                std::memory_order_release);
            FConfiguration::BotHealth.store(
                ReadInt(Bots, "health", 21),
                std::memory_order_release);
            FConfiguration::BotShield.store(
                ReadInt(Bots, "shield", 21),
                std::memory_order_release);
            FConfiguration::UseCustomBotNames.store(
                ReadBool(
                    Bots,
                    "use_custom_names",
                    false),
                std::memory_order_release);
            FConfiguration::BotName =
                ReadString(
                    Bots,
                    "name",
                    "Magnesium Bot ");

            const auto& Trickshot =
                ReadObject(Preferences, "trickshot");
            FConfiguration::bUseWinLines.store(
                ReadBool(
                    Trickshot,
                    "swag_lines",
                    true),
                std::memory_order_release);
            FConfiguration::bInfiniteRender.store(
                ReadBool(
                    Trickshot,
                    "infinite_render",
                    false),
                std::memory_order_release);
            FConfiguration::RandomizeArenaPoints.store(
                ReadBool(
                    Trickshot,
                    "randomize_arena_points",
                    false),
                std::memory_order_release);
            FConfiguration::bPlayerMapIcons.store(
                ReadBool(
                    Trickshot,
                    "player_map_icons",
                    false),
                std::memory_order_release);
            FConfiguration::bAutoGodMode.store(
                ReadBool(
                    Trickshot,
                    "auto_god_mode",
                    false),
                std::memory_order_release);
            FConfiguration::AutoGodModeType.store(
                ClampValue(
                    ReadInt(
                        Trickshot,
                        "auto_god_mode_type",
                        (int)FConfiguration::EAutoGodMode::Maximum),
                    (int)FConfiguration::EAutoGodMode::Maximum,
                    (int)FConfiguration::EAutoGodMode::Minimum),
                std::memory_order_release);
            FConfiguration::bAutoGodModeExcludeLastPlayer.store(
                ReadBool(
                    Trickshot,
                    "auto_god_mode_exclude_last_player",
                    false),
                std::memory_order_release);
            FConfiguration::RandomizeKills.store(
                ReadBool(
                    Trickshot,
                    "randomize_kills",
                    false),
                std::memory_order_release);
            FConfiguration::RandomizeLevels.store(
                ReadBool(
                    Trickshot,
                    "randomize_levels",
                    false),
                std::memory_order_release);
            FConfiguration::bDisableJumpFatigue.store(
                ReadBool(
                    Trickshot,
                    "disable_jump_fatigue",
                    false),
                std::memory_order_release);
            FConfiguration::bDisableSupplyDrops.store(
                ReadBool(
                    Trickshot,
                    "disable_supply_drops",
                    FConfiguration::bEnableTrickshotTab.load(
                        std::memory_order_acquire)),
                std::memory_order_release);
            FConfiguration::bVehicleBumpLaunch.store(
                ReadBool(
                    Trickshot,
                    "vehicle_bump_launch",
                    !FConfiguration::bEnableTrickshotTab.load(
                        std::memory_order_acquire)),
                std::memory_order_release);
            FConfiguration::bCannonLaunchAnimations.store(
                ReadBool(
                    Trickshot,
                    "cannon_launch_animations",
                    true),
                std::memory_order_release);
            FConfiguration::CannonLaunchXMultiplier.store(
                ClampValue(
                    ReadFloat(
                        Trickshot,
                        "cannon_launch_x_multiplier",
                        1.f),
                    0.f, 5.f),
                std::memory_order_release);
            FConfiguration::CannonLaunchYMultiplier.store(
                ClampValue(
                    ReadFloat(
                        Trickshot,
                        "cannon_launch_y_multiplier",
                        1.f),
                    0.f, 5.f),
                std::memory_order_release);
            FConfiguration::CannonLaunchZMultiplier.store(
                ClampValue(
                    ReadFloat(
                        Trickshot,
                        "cannon_launch_z_multiplier",
                        1.f),
                    0.f, 5.f),
                std::memory_order_release);
            FConfiguration::bCrownSlomo.store(
                ReadBool(
                    Trickshot,
                    "crown_slow_motion",
                    true),
                std::memory_order_release);
            FConfiguration::bCancelVelocityOnWin.store(
                ReadBool(
                    Trickshot,
                    "cancel_velocity_on_win",
                    false),
                std::memory_order_release);
            FConfiguration::bAutoPauseTODM.store(
                ReadBool(
                    Trickshot,
                    "auto_pause_time_of_day",
                    false),
                std::memory_order_release);
            FConfiguration::TODMTime.store(
                ClampValue(
                    ReadFloat(
                        Trickshot,
                        "time_of_day",
                        7.f),
                    0.f, 24.f),
                std::memory_order_release);

            // Profiles are per Fortnite version, so a stored snow value always
            // belongs to the model the running build uses.
            const auto& CalendarPreferences =
                ReadObject(Preferences, "calendar");
            const Calendar::FSnowVersionModel SnowModel =
                Calendar::GetSnowVersionModel();
            FConfiguration::bSnowOnMatchStart.store(
                ReadBool(
                    CalendarPreferences,
                    "snow_on_match_start",
                    false),
                std::memory_order_release);
            FConfiguration::SnowValue.store(
                ClampValue(
                    ReadFloat(
                        CalendarPreferences,
                        "snow_value",
                        0.f),
                    SnowModel.Min,
                    SnowModel.Max),
                std::memory_order_release);

            return true;
        }

        nlohmann::json BuildDocument(
            bool ForcePreferenceSnapshot)
        {
            nlohmann::json Document =
                GDocument.is_object()
                    ? GDocument
                    : nlohmann::json::object();
            Document["schema_version"] =
                SettingsSchemaVersion;

            if (!Document["profiles"].is_object())
                Document["profiles"] = nlohmann::json::object();

            auto& Profile =
                Document["profiles"][CurrentProfileKey()];
            if (!Profile.is_object())
                Profile = nlohmann::json::object();

            Profile["auto_host"] = {
                {
                    "enabled",
                    FConfiguration::bAutoHost.load(
                        std::memory_order_acquire)
                },
                {
                    "delay_seconds",
                    ClampValue(
                        FConfiguration::
                            AutoHostDelaySeconds.load(
                                std::memory_order_acquire),
                        1, 60)
                }
            };

            const bool bAutoHostEnabled =
                FConfiguration::bAutoHost.load(
                    std::memory_order_acquire);
            const bool bReadyToStart =
                FConfiguration::bReadyToStart.load(
                    std::memory_order_acquire);
            if (ForcePreferenceSnapshot ||
                (bAutoHostEnabled && !bReadyToStart))
            {
                GStoredPreferences = CapturePreferences();
            }
            else if (bAutoHostEnabled)
            {
                RefreshPostStartPreferences();
            }

            if (GStoredPreferences.is_object() &&
                !GStoredPreferences.empty())
            {
                Profile["preferences"] =
                    GStoredPreferences;
            }

            return Document;
        }

        bool WriteDocument(
            const nlohmann::json& Document,
            const std::string& Serialized)
        {
            const fs::path Path = SettingsPath();
            if (Path.empty())
            {
                SDK::DbgLog(
                    "[AutoHosting] Local AppData is unavailable; settings were not saved\n");
                return false;
            }

            std::error_code Error;
            fs::create_directories(
                Path.parent_path(), Error);
            if (Error)
            {
                SDK::DbgLog(
                    "[AutoHosting] Failed to create settings directory: %s\n",
                    Error.message().c_str());
                return false;
            }

            fs::path TemporaryPath = Path;
            TemporaryPath += L".tmp";
            {
                std::ofstream File(
                    TemporaryPath,
                    std::ios::binary | std::ios::trunc);
                if (!File)
                {
                    SDK::DbgLog(
                        "[AutoHosting] Failed to open temporary settings file\n");
                    return false;
                }

                File << Document.dump(2) << '\n';
                File.flush();
                if (!File)
                {
                    SDK::DbgLog(
                        "[AutoHosting] Failed while writing settings\n");
                    return false;
                }
            }

            if (!MoveFileExW(
                    TemporaryPath.c_str(),
                    Path.c_str(),
                    MOVEFILE_REPLACE_EXISTING |
                        MOVEFILE_WRITE_THROUGH))
            {
                SDK::DbgLog(
                    "[AutoHosting] Failed to commit settings (error %lu)\n",
                    GetLastError());
                DeleteFileW(TemporaryPath.c_str());
                return false;
            }

            GDocument = Document;
            GLastSerializedDocument = Serialized;
            return true;
        }

        void SaveInternal(bool ForcePreferenceSnapshot)
        {
            try
            {
                const nlohmann::json Document =
                    BuildDocument(
                        ForcePreferenceSnapshot);
                const std::string Serialized =
                    Document.dump();
                if (Serialized == GLastSerializedDocument)
                    return;

                WriteDocument(Document, Serialized);
            }
            catch (const std::exception& Error)
            {
                SDK::DbgLog(
                    "[AutoHosting] Failed to save settings: %s\n",
                    Error.what());
            }
        }
    }

    void Initialize()
    {
        FConfiguration::bAutoHost.store(
            false, std::memory_order_release);
        FConfiguration::AutoHostDelaySeconds.store(
            FConfiguration::DefaultAutoHostDelaySeconds,
            std::memory_order_release);
        GRestoredPreferences.store(
            false, std::memory_order_release);
        GCountdownDeadlineMs.store(
            0, std::memory_order_release);
        GPostMatchShutdownDeadlineMs.store(
            0, std::memory_order_release);

        // These two defaults are version-owned by the Match UI. Seed them
        // before taking the pristine reset snapshot so Reset Preferences
        // exactly matches a clean launch for the running Fortnite build.
        FConfiguration::LateGameZone.store(
            FConfiguration::IsS27() ? 1 : 4,
            std::memory_order_release);
        FConfiguration::bAutoDump.store(
            VersionInfo.FortniteVersion != 19.20,
            std::memory_order_release);
        GDefaultPreferences = CapturePreferences();

        const fs::path Path = SettingsPath();
        if (Path.empty())
            return;

        std::ifstream File(Path, std::ios::binary);
        if (!File)
            return;

        try
        {
            File >> GDocument;
            if (!GDocument.is_object() ||
                ReadInt(
                    GDocument,
                    "schema_version",
                    -1) != SettingsSchemaVersion)
            {
                SDK::DbgLog(
                    "[AutoHosting] Ignoring unsupported settings schema\n");
                GDocument = nlohmann::json::object();
                return;
            }

            GLastSerializedDocument = GDocument.dump();
            const auto& Profiles =
                ReadObject(GDocument, "profiles");
            const auto ProfileIt =
                Profiles.find(CurrentProfileKey());
            if (ProfileIt == Profiles.end() ||
                !ProfileIt->is_object())
            {
                return;
            }

            const auto& Profile = *ProfileIt;
            const auto& AutoHost =
                ReadObject(Profile, "auto_host");
            const bool bEnabled =
                ReadBool(AutoHost, "enabled", false);
            const int DelaySeconds = ClampValue(
                ReadInt(
                    AutoHost,
                    "delay_seconds",
                    FConfiguration::
                        DefaultAutoHostDelaySeconds),
                1, 60);

            FConfiguration::AutoHostDelaySeconds.store(
                DelaySeconds, std::memory_order_release);

            const auto& Preferences =
                ReadObject(Profile, "preferences");
            if (Preferences.is_object() &&
                !Preferences.empty())
            {
                GStoredPreferences = Preferences;
            }

            if (!bEnabled)
                return;

            // An enabled profile without a complete resolved playlist is not
            // safe to auto-start. Leave Auto Host off and require the user to
            // configure it again.
            if (!ApplyPreferences(Preferences))
            {
                SDK::DbgLog(
                    "[AutoHosting] Enabled profile is incomplete; automatic startup was disabled\n");
                return;
            }

            FConfiguration::bAutoHost.store(
                true, std::memory_order_release);
            GRestoredPreferences.store(
                true, std::memory_order_release);
            SDK::DbgLog(
                "[AutoHosting] Restored %s; countdown ready for %d seconds\n",
                CurrentProfileKey().c_str(),
                DelaySeconds);
        }
        catch (const std::exception& Error)
        {
            SDK::DbgLog(
                "[AutoHosting] Settings are invalid; automatic startup was disabled: %s\n",
                Error.what());
            GDocument = nlohmann::json::object();
            GStoredPreferences =
                nlohmann::json::object();
            GLastSerializedDocument.clear();
            FConfiguration::bAutoHost.store(
                false, std::memory_order_release);
        }
    }

    bool HasRestoredPreferences()
    {
        return GRestoredPreferences.load(
            std::memory_order_acquire);
    }

    void ArmCountdown()
    {
        if (!FConfiguration::bAutoHost.load(
                std::memory_order_acquire) ||
            FConfiguration::bReadyToStart.load(
                std::memory_order_acquire))
        {
            CancelCountdown();
            return;
        }

        const int DelaySeconds = ClampValue(
            FConfiguration::AutoHostDelaySeconds.load(
                std::memory_order_acquire),
            1, 60);
        GCountdownDeadlineMs.store(
            GetTickCount64() +
                static_cast<ULONGLONG>(DelaySeconds) * 1000,
            std::memory_order_release);
    }

    void CancelCountdown()
    {
        GCountdownDeadlineMs.store(
            0, std::memory_order_release);
    }

    bool IsCountdownActive()
    {
        return GCountdownDeadlineMs.load(
                   std::memory_order_acquire) != 0 &&
            FConfiguration::bAutoHost.load(
                std::memory_order_acquire) &&
            !FConfiguration::bReadyToStart.load(
                std::memory_order_acquire);
    }

    int GetRemainingSeconds()
    {
        const ULONGLONG Deadline =
            GCountdownDeadlineMs.load(
                std::memory_order_acquire);
        if (Deadline == 0)
            return 0;

        const ULONGLONG Now = GetTickCount64();
        if (Now >= Deadline)
            return 0;

        return static_cast<int>(
            (Deadline - Now + 999) / 1000);
    }

    void TickCountdown()
    {
        const ULONGLONG Deadline =
            GCountdownDeadlineMs.load(
                std::memory_order_acquire);
        if (Deadline == 0)
            return;

        if (!FConfiguration::bAutoHost.load(
                std::memory_order_acquire) ||
            FConfiguration::bReadyToStart.load(
                std::memory_order_acquire))
        {
            CancelCountdown();
            return;
        }

        if (GetTickCount64() < Deadline)
            return;

        // Save the exact configuration that the server thread will consume,
        // then use the same release-store as the manual Start Server action.
        SaveNow(true);
        CancelCountdown();
        FConfiguration::bReadyToStart.store(
            true, std::memory_order_release);
    }

    void OnAuthoritativeMatchEnded()
    {
        if (!FConfiguration::bAutoHost.load(
                std::memory_order_acquire))
        {
            return;
        }

        ULONGLONG ExpectedDeadline = 0;
        const ULONGLONG Deadline =
            GetTickCount64() + PostMatchShutdownDelayMs;
        if (GPostMatchShutdownDeadlineMs.compare_exchange_strong(
                ExpectedDeadline,
                Deadline,
                std::memory_order_release,
                std::memory_order_relaxed))
        {
            SDK::DbgLog(
                "[AutoHosting] Match ended; full server shutdown armed for 10 seconds\n");
        }
    }

    void TickPostMatchShutdown()
    {
        ULONGLONG Deadline =
            GPostMatchShutdownDeadlineMs.load(
                std::memory_order_acquire);
        if (Deadline == 0 &&
            FConfiguration::bAutoHost.load(
                std::memory_order_acquire) &&
            GUI::gsStatus.load(
                std::memory_order_acquire) == Ended)
        {
            // A winner/death callback can publish Ended after the final
            // NetDriver lifecycle poll, especially on Iris seasons. The GUI
            // keeps polling independently, so let it arm the same idempotent
            // deadline even if replication has already stopped.
            OnAuthoritativeMatchEnded();
            Deadline =
                GPostMatchShutdownDeadlineMs.load(
                    std::memory_order_acquire);
        }
        if (Deadline == 0)
            return;

        if (!FConfiguration::bAutoHost.load(
                std::memory_order_acquire))
        {
            GPostMatchShutdownDeadlineMs.compare_exchange_strong(
                Deadline,
                0,
                std::memory_order_release,
                std::memory_order_relaxed);
            return;
        }

        const ULONGLONG Now = GetTickCount64();
        if (Now < Deadline ||
            !GPostMatchShutdownDeadlineMs.compare_exchange_strong(
                Deadline,
                0,
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            return;
        }

        SDK::DbgLog(
            "[AutoHosting] Post-match delay elapsed; closing the full server process\n");
        if (!TerminateProcess(GetCurrentProcess(), 0))
        {
            SDK::DbgLog(
                "[AutoHosting] Full server shutdown failed (error %lu); retrying\n",
                GetLastError());
            GPostMatchShutdownDeadlineMs.store(
                Now + 1000, std::memory_order_release);
        }
    }

    void SaveIfChanged()
    {
        const ULONGLONG Now = GetTickCount64();
        if (Now < GNextSavePollMs)
            return;

        GNextSavePollMs = Now + SavePollIntervalMs;
        SaveInternal(false);
    }

    void SaveNow(bool ForcePreferenceSnapshot)
    {
        SaveInternal(ForcePreferenceSnapshot);
    }

    void ResetPreferences()
    {
        CancelCountdown();
        GPostMatchShutdownDeadlineMs.store(
            0, std::memory_order_release);
        FConfiguration::bAutoHost.store(
            false, std::memory_order_release);
        FConfiguration::AutoHostDelaySeconds.store(
            FConfiguration::DefaultAutoHostDelaySeconds,
            std::memory_order_release);
        GRestoredPreferences.store(
            false, std::memory_order_release);

        if (!FConfiguration::bReadyToStart.load(
                std::memory_order_acquire))
        {
            if (ApplyPreferences(GDefaultPreferences))
            {
                GUI::ResetPreferenceEditorState();
            }
            else
            {
                SDK::DbgLog(
                    "[AutoHosting] Failed to restore the in-memory default profile\n");
            }
        }

        // Keep other Fortnite-version profiles intact. Only the profile for
        // the running version is replaced by its pristine defaults.
        GStoredPreferences = GDefaultPreferences;
        SaveInternal(false);
    }
}
