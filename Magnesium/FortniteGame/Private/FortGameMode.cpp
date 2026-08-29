#include "pch.h"
#include "../Public/FortGameMode.h"
#include "../Public/CustomSafeZoneRuntime.h"
#include "../Public/LevelStreamingDynamic.h"
#include "../../Erbium/Public/Finders.h"
#include "../../Engine/Public/NetDriver.h"
#include "../../Engine/Public/AbilitySystemComponent.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortKismetLibrary.h"
#include "../../Engine/Public/CurveTable.h"
#include "../Public/FortSafeZoneIndicator.h"
#include "../../Engine/Public/DataTableFunctionLibrary.h"
#include "../../Erbium/Public/Calendar.h"
#include "../../Erbium/Public/Configuration.h"
#include "../../Erbium/Support/Public/VersionFeatureAdapter.h"
#include "../Public/FortLootPackage.h"
#include "../Public/BuildingFoundation.h"
#include "../../Erbium/Public/LateGame.h"
#include "../Public/BuildingItemCollectorActor.h"
#include "../../Erbium/Public/GUI.h"
#include <random>
#include "../../Erbium/Public/Misc.h"
#include "../../Erbium/Public/Events.h"
#include "../Public/BattleRoyaleGamePhaseLogic.h"
#include "../Public/FortAthenaCreativePortal.h"
#include "../Public/FortAthenaMutator.h"
#include "../Public/FortPhysicsPawn.h"
#include "../Public/FortVehicleMods.h"

#include <sstream>
#include <fstream>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

void ShowFoundation(const ABuildingFoundation* Foundation)
{
    if (!Foundation) return;

    Foundation->SetDynamicFoundationEnabled(true);
}

bool bIsLargeTeamGame = false;
uint64_t NotifyGameMemberAdded_ = 0;

namespace
{
    struct FLateSeasonHumanTeamState
    {
        AFortGameMode* GameMode = nullptr;
        UWorld* World = nullptr;
        const UFortPlaylistAthena* Playlist = nullptr;
        uint8 FirstTeam = 3;
        uint8 NextTeam = 3;
        int32 PlayersOnTeam = 0;

        struct FAssignment
        {
            AFortPlayerStateAthena* PlayerState = nullptr;
            uint8 Team = 3;
        };

        std::unordered_map<AFortPlayerControllerAthena*, FAssignment> AssignedTeams;
    };

    FLateSeasonHumanTeamState GLateSeasonHumanTeams;

    bool IsSaneObject(UObject* Object)
    {
        if (!Object || !SDK::MemReadable(Object, sizeof(UObject)))
            return false;

        const int32 ObjectIndex = Object->Index;
        if (ObjectIndex < 0 || ObjectIndex >= TUObjectArray::Num())
            return false;

        auto Item = TUObjectArray::GetItemByIndex(ObjectIndex);
        constexpr int32 InvalidObjectFlags = 0x20;
        return Item && Item->GetObject() == Object &&
            !(Item->GetFlags() & InvalidObjectFlags) &&
            Object->Class && SDK::MemReadable(Object->Class, sizeof(UClass));
    }

    bool IsSanePlaylist(const UFortPlaylistAthena* Playlist)
    {
        auto MutablePlaylist =
            const_cast<UFortPlaylistAthena*>(Playlist);
        auto PlaylistClass = UFortPlaylistAthena::StaticClass();
        return IsSaneObject(MutablePlaylist) && PlaylistClass &&
            MutablePlaylist->IsA(PlaylistClass);
    }

    bool ShouldRepairLateSeasonTeams()
    {
        return VersionInfo.FortniteVersion >= 17.0 && VersionInfo.FortniteVersion < 19.0;
    }

    uint8 GetPlaylistFirstTeam(const UFortPlaylistAthena* Playlist)
    {
        if (IsSanePlaylist(Playlist))
        {
            const int32 Offset = (int32)Playlist->GetOffset("DefaultFirstTeam");
            if (Offset >= 0 && SDK::MemReadable((const uint8*)Playlist + Offset, sizeof(uint8)))
            {
                const uint8 FirstTeam = GetFromOffset<uint8>(Playlist, Offset);
                if (FirstTeam >= 3 && FirstTeam < 250)
                    return FirstTeam;
            }
        }

        return 3;
    }

    void ResetLateSeasonHumanTeams(AFortGameMode* GameMode, const UFortPlaylistAthena* Playlist)
    {
        const uint8 FirstTeam = GetPlaylistFirstTeam(Playlist);

        GLateSeasonHumanTeams.GameMode = GameMode;
        GLateSeasonHumanTeams.World = UWorld::GetWorld();
        GLateSeasonHumanTeams.Playlist = Playlist;
        GLateSeasonHumanTeams.FirstTeam = FirstTeam;
        GLateSeasonHumanTeams.NextTeam = FirstTeam;
        GLateSeasonHumanTeams.PlayersOnTeam = 0;
        GLateSeasonHumanTeams.AssignedTeams.clear();

        // Keep the legacy PickTeam fallback in sync in case a particular
        // 17/18 build still reaches it for one of its join paths.
        AFortGameMode::CurrentTeam = FirstTeam;
        AFortGameMode::PlayersOnCurTeam = 0;
    }

    uint8 ReserveLateSeasonHumanTeam(
        AFortGameMode* GameMode,
        AFortPlayerControllerAthena* Controller,
        AFortPlayerStateAthena* PlayerState,
        const UFortPlaylistAthena* Playlist)
    {
        if (GLateSeasonHumanTeams.GameMode != GameMode ||
            GLateSeasonHumanTeams.World != UWorld::GetWorld() ||
            GLateSeasonHumanTeams.Playlist != Playlist)
        {
            ResetLateSeasonHumanTeams(GameMode, Playlist);
        }

        if (auto Existing = GLateSeasonHumanTeams.AssignedTeams.find(Controller);
            Existing != GLateSeasonHumanTeams.AssignedTeams.end())
        {
            if (Existing->second.PlayerState == PlayerState)
            {
                AFortGameMode::CurrentTeam =
                    GLateSeasonHumanTeams.NextTeam;
                AFortGameMode::PlayersOnCurTeam =
                    GLateSeasonHumanTeams.PlayersOnTeam;
                return Existing->second.Team;
            }

            GLateSeasonHumanTeams.AssignedTeams.erase(Existing);
        }

        const uint8 AssignedTeam = GLateSeasonHumanTeams.NextTeam;
        GLateSeasonHumanTeams.AssignedTeams.emplace(
            Controller,
            FLateSeasonHumanTeamState::FAssignment{ PlayerState, AssignedTeam });

        if (Playlist && Playlist->HasbIsLargeTeamGame() && Playlist->bIsLargeTeamGame)
        {
            GLateSeasonHumanTeams.NextTeam =
                AssignedTeam == GLateSeasonHumanTeams.FirstTeam
                ? (uint8)(GLateSeasonHumanTeams.FirstTeam + 1)
                : GLateSeasonHumanTeams.FirstTeam;
        }
        else
        {
            int32 SquadSize = Playlist && Playlist->HasMaxSquadSize()
                ? Playlist->MaxSquadSize
                : 1;
            if (SquadSize < 1)
                SquadSize = 1;
            else if (SquadSize > 100)
                SquadSize = 100;

            if (++GLateSeasonHumanTeams.PlayersOnTeam >= SquadSize)
            {
                GLateSeasonHumanTeams.NextTeam++;
                GLateSeasonHumanTeams.PlayersOnTeam = 0;
            }
        }

        // PickTeam is still used directly by synthetic controllers such as
        // `spawnbot`. Keep its legacy counters at the same position as the
        // repaired 17/18 allocator so bots cannot be placed on a full human
        // duo/squad merely because native human joins skipped PickTeam.
        AFortGameMode::CurrentTeam = GLateSeasonHumanTeams.NextTeam;
        AFortGameMode::PlayersOnCurTeam =
            GLateSeasonHumanTeams.PlayersOnTeam;

        return AssignedTeam;
    }

    UObject* GetLiveObjectByIndex(int32 ObjectIndex)
    {
        auto Item = TUObjectArray::GetItemByIndex(ObjectIndex);
        constexpr int32 InvalidObjectFlags = 0x20;
        if (!Item || (Item->GetFlags() & InvalidObjectFlags))
            return nullptr;

        auto Object = const_cast<UObject*>(Item->GetObject());
        return Object && Object->Class ? Object : nullptr;
    }

    int32 GetLateSeasonIntProperty(
        const UObject* Object,
        const char* Name,
        int32 Fallback)
    {
        if (!IsSaneObject(const_cast<UObject*>(Object)))
            return Fallback;

        const int32 Offset = (int32)Object->GetOffset(Name);
        if (Offset < 0 ||
            !SDK::MemReadable((const uint8*)Object + Offset, sizeof(int32)))
        {
            return Fallback;
        }

        return GetFromOffset<int32>(Object, Offset);
    }

    bool SetLateSeasonIntProperty(
        UObject* Object,
        const char* Name,
        int32 Value)
    {
        if (!IsSaneObject(Object))
            return false;

        const int32 Offset = (int32)Object->GetOffset(Name);
        if (Offset < 0 ||
            !SDK::MemReadable((uint8*)Object + Offset, sizeof(int32)))
        {
            return false;
        }

        GetFromOffset<int32>(Object, Offset) = Value;
        return true;
    }

    void SyncPlaylistTeamSettings(
        AFortGameMode* GameMode,
        AFortGameStateAthena* GameState,
        const UFortPlaylistAthena* Playlist)
    {
        if (!IsSaneObject(GameMode) || !IsSaneObject(GameState) || !Playlist)
            return;

        int32 MaxSquadSize = Playlist->HasMaxSquadSize()
            ? Playlist->MaxSquadSize
            : 1;
        if (MaxSquadSize < 1)
            MaxSquadSize = 1;

        int32 MaxTeamSize =
            GetLateSeasonIntProperty(Playlist, "MaxTeamSize", MaxSquadSize);
        if (MaxTeamSize < 1)
            MaxTeamSize = MaxSquadSize;

        int32 MaxTeamCount = GetLateSeasonIntProperty(
            Playlist,
            "MaxTeamCount",
            (Playlist->MaxPlayers + MaxTeamSize - 1) / MaxTeamSize);
        if (MaxTeamCount < 1)
            MaxTeamCount = (Playlist->MaxPlayers + MaxTeamSize - 1) / MaxTeamSize;

        const bool bSetTeamSize =
            SetLateSeasonIntProperty(GameState, "TeamSize", MaxTeamSize);
        const bool bSetTeamCount =
            SetLateSeasonIntProperty(GameState, "TeamCount", MaxTeamCount);
        const bool bSetPartySize = GameMode->GameSession &&
            SetLateSeasonIntProperty(
                GameMode->GameSession, "MaxPartySize", MaxSquadSize);

        SDK::DbgLog(
            "[Teams] playlist team settings: "
            "TeamSize=%d(%d) TeamCount=%d(%d) "
            "MaxPartySize=%d(%d)\n",
            MaxTeamSize,
            bSetTeamSize ? 1 : 0,
            MaxTeamCount,
            bSetTeamCount ? 1 : 0,
            MaxSquadSize,
            bSetPartySize ? 1 : 0);
    }

    bool IsTeamPlaylist(const UFortPlaylistAthena* Playlist)
    {
        return Playlist && Playlist->HasMaxSquadSize() &&
            Playlist->MaxSquadSize > 1;
    }

    bool DoesPlaylistAllowDBNO(
        const UFortPlaylistAthena* Playlist)
    {
        if (!Playlist)
            return false;

        const int32 DBNOTypeOffset = (int32)Playlist->GetOffset("DBNOType");
        if (DBNOTypeOffset < 0 ||
            !SDK::MemReadable(
                (const uint8*)Playlist + DBNOTypeOffset, sizeof(uint8)))
        {
            // Older playlists use an inverse reflected bit instead of
            // DBNOType. Read its field mask: treating the containing byte as
            // a bool would make adjacent playlist flags affect DBNO.
            auto MutablePlaylist =
                const_cast<UFortPlaylistAthena*>(Playlist);
            auto NoDBNOProperty =
                MutablePlaylist->GetProperty("bNoDBNO", 0x20000);
            if (!NoDBNOProperty)
                return true;

            const uint32 NoDBNOOffset =
                GetFromOffset<uint32>(
                    NoDBNOProperty, Offsets::Offset_Internal);
            if (NoDBNOOffset == uint32(-1) ||
                !SDK::MemReadable(
                    (const uint8*)Playlist + NoDBNOOffset,
                    sizeof(uint8)))
            {
                return true;
            }

            const uint8 FieldMask =
                NoDBNOProperty->GetFieldMask();
            const uint8 Value =
                GetFromOffset<uint8>(Playlist, NoDBNOOffset);
            const bool bNoDBNO = FieldMask
                ? (Value & FieldMask) != 0
                : Value != 0;
            return !bNoDBNO;
        }

        const uint8 DBNOType =
            GetFromOffset<uint8>(Playlist, DBNOTypeOffset);
        switch (DBNOType)
        {
        case 0: // EDBNOType::On
            return true;
        case 1: // EDBNOType::Off
            return false;
        case 2: // EDBNOType::NotWhenRespawning
        {
            const bool bRespawning =
                Playlist->HasRespawnType()
                ? Playlist->RespawnType > 0
                : FConfiguration::bForceRespawns.load();
            return !bRespawning;
        }
        default:
            return true;
        }
    }

    void SetDBNODeathEnabled(
        AFortGameStateAthena* GameState,
        bool bEnabled)
    {
        if (!GameState)
            return;

        // Some builds expose this protected replicated state through a native
        // setter. Prefer it so the matching side effects/replication run;
        // retain the reflected field fallback for older layouts.
        auto Setter =
            GameState->GetFunction("SetIsDBNODeathEnabled");
        bool bCalledSetter = false;
        if (Setter)
        {
            const auto Params = Setter->GetParamsNamed();
            const auto* EnabledParam =
                Params.NameOffsetMap.size() == 1
                    ? &Params.NameOffsetMap[0]
                    : nullptr;
            constexpr uint64 CPF_Parm = 0x80;
            constexpr uint64 CPF_OutParm = 0x100;
            constexpr uint64 CPF_ReturnParm = 0x400;
            const bool bSchemaValid =
                Params.Size == sizeof(bool) &&
                Setter->GetPropertiesSize() == sizeof(bool) &&
                EnabledParam && EnabledParam->Offset == 0 &&
                EnabledParam->ElementSize == sizeof(bool) &&
                (EnabledParam->PropertyFlags & CPF_Parm) &&
                !(EnabledParam->PropertyFlags & CPF_OutParm) &&
                !(EnabledParam->PropertyFlags & CPF_ReturnParm);
            if (bSchemaValid)
            {
                bool ParamsValue = bEnabled;
                GameState->ProcessEvent(Setter, &ParamsValue);
                bCalledSetter = true;
            }
        }

        if (!bCalledSetter && GameState->HasbDBNODeathEnabled())
            GameState->bDBNODeathEnabled = bEnabled;
    }

    void ApplyLateSeasonDBNOSettings(
        AFortGameMode* GameMode,
        AFortGameStateAthena* GameState,
        const UFortPlaylistAthena* Playlist,
        const char* Stage)
    {
        if (!ShouldRepairLateSeasonTeams() ||
            !IsSaneObject(GameMode) || !IsSaneObject(GameState))
        {
            return;
        }

        // Native Athena only enables DBNO for actual team playlists. Keep
        // bAlwaysDBNO disabled so the last living member of a team is
        // eliminated instead of being knocked with nobody able to revive.
        const bool bTeamMode = IsTeamPlaylist(Playlist);
        bool bDBNOEnabled =
            FConfiguration::bEnableDBNO && bTeamMode &&
            DoesPlaylistAllowDBNO(Playlist);

        if (GameMode->HasbEnableDBNO())
            GameMode->bEnableDBNO = bDBNOEnabled;
        if (GameMode->HasbDBNOEnabled())
            GameMode->bDBNOEnabled = bDBNOEnabled;
        if (GameMode->HasbAlwaysDBNO())
            GameMode->bAlwaysDBNO = false;
        if (GameState->HasbDBNOEnabledForGameMode())
            GameState->bDBNOEnabledForGameMode = bDBNOEnabled;
        SetDBNODeathEnabled(GameState, bDBNOEnabled);

        GameState->ForceNetUpdate();
        SDK::DbgLog(
            "[Teams] FN17-18 DBNO %s: TeamMode=%d Enabled=%d\n",
            Stage,
            bTeamMode ? 1 : 0,
            bDBNOEnabled ? 1 : 0);
    }

    UObject* FindLateSeasonTeamInfo(
        AFortGameStateAthena* GameState,
        uint8 TeamIndex)
    {
        if (!GameState)
            return nullptr;

        const int32 TeamsOffset = (int32)GameState->GetOffset("Teams");
        if (TeamsOffset < 0 ||
            !SDK::MemReadable((const uint8*)GameState + TeamsOffset, sizeof(TArray<UObject*>)))
        {
            return nullptr;
        }

        auto& Teams = GetFromOffset<TArray<UObject*>>(GameState, TeamsOffset);
        const int32 TeamCount = Teams.Num();
        if (TeamCount <= 0 || TeamCount > 256 ||
            !SDK::MemReadable(Teams.GetData(), sizeof(UObject*) * TeamCount))
        {
            return nullptr;
        }

        // Match the replicated Team value instead of assuming an array base.
        // Reserved team slots differ between builds.
        for (int32 Index = 0; Index < TeamCount; Index++)
        {
            auto TeamInfo = Teams[Index];
            if (!IsSaneObject(TeamInfo))
                continue;

            const int32 TeamOffset = (int32)TeamInfo->GetOffset("Team");
            if (TeamOffset >= 0 &&
                SDK::MemReadable((const uint8*)TeamInfo + TeamOffset, sizeof(uint8)) &&
                GetFromOffset<uint8>(TeamInfo, TeamOffset) == TeamIndex)
            {
                return TeamInfo;
            }
        }

        return nullptr;
    }

    void EnsureLateSeasonTeamObjects(
        AFortGameMode* GameMode,
        AFortGameStateAthena* GameState,
        uint8 FirstTeam)
    {
        if (!IsSaneObject(GameMode) || !IsSaneObject(GameState) ||
            FindLateSeasonTeamInfo(GameState, FirstTeam))
        {
            return;
        }

        static int32 InitializeTeamsVftIndex = -2;
        if (InitializeTeamsVftIndex == -2)
        {
            InitializeTeamsVftIndex = -1;
            auto StringRef = Memcury::Scanner::FindStringRef(
                L"InitializeTeams()", false);
            const uintptr_t FunctionAddress = StringRef.IsValid()
                ? StringRef.FindFunctionBoundary(false).Get()
                : 0;

            if (FunctionAddress && GameMode->Vft)
            {
                for (int32 Index = 0; Index < 1024; Index++)
                {
                    if (!SDK::MemReadable(
                        &GameMode->Vft[Index], sizeof(void*)))
                    {
                        break;
                    }

                    if ((uintptr_t)GameMode->Vft[Index] == FunctionAddress)
                    {
                        InitializeTeamsVftIndex = Index;
                        break;
                    }
                }
            }

            SDK::DbgLog(
                "[Teams] FN17-18 InitializeTeams resolver: Address=%p VFT=%d\n",
                (void*)FunctionAddress,
                InitializeTeamsVftIndex);
        }

        if (InitializeTeamsVftIndex < 0 || !GameMode->Vft ||
            !SDK::MemReadable(
                &GameMode->Vft[InitializeTeamsVftIndex], sizeof(void*)))
        {
            return;
        }

        auto InitializeTeams =
            (void(*)(AFortGameMode*))GameMode->Vft[InitializeTeamsVftIndex];
        if (!SDK::MemReadable((void*)InitializeTeams, 16))
            return;

        InitializeTeams(GameMode);
        SDK::DbgLog(
            "[Teams] FN17-18 InitializeTeams invoked: FirstTeam=%u TeamInfo=%p\n",
            (unsigned)FirstTeam,
            (void*)FindLateSeasonTeamInfo(GameState, FirstTeam));
    }

    TArray<AActor*>* GetLateSeasonTeamMembers(UObject* TeamInfo)
    {
        if (!IsSaneObject(TeamInfo))
            return nullptr;

        const int32 Offset = (int32)TeamInfo->GetOffset("TeamMembers");
        if (Offset < 0 ||
            !SDK::MemReadable((const uint8*)TeamInfo + Offset, sizeof(TArray<AActor*>)))
        {
            return nullptr;
        }

        auto& Members = GetFromOffset<TArray<AActor*>>(TeamInfo, Offset);
        if (Members.Num() < 0 || Members.Num() > 256 ||
            Members.Max() < Members.Num() || Members.Max() > 4096)
        {
            return nullptr;
        }

        if (Members.Num() > 0 &&
            !SDK::MemReadable(Members.GetData(), sizeof(AActor*) * Members.Num()))
        {
            return nullptr;
        }

        return &Members;
    }

    void RemoveLateSeasonTeamMember(UObject* TeamInfo, AActor* Controller)
    {
        auto Members = GetLateSeasonTeamMembers(TeamInfo);
        if (!Members)
            return;

        for (int32 Index = Members->Num() - 1; Index >= 0; Index--)
        {
            if ((*Members)[Index] == Controller)
                Members->Remove(Index);
        }
    }

    void RemoveLateSeasonMemberFromOtherTeams(
        AFortGameStateAthena* GameState,
        UObject* DesiredTeam,
        AActor* Controller)
    {
        if (!IsSaneObject(GameState))
            return;

        const int32 TeamsOffset = (int32)GameState->GetOffset("Teams");
        if (TeamsOffset < 0 ||
            !SDK::MemReadable((const uint8*)GameState + TeamsOffset, sizeof(TArray<UObject*>)))
        {
            return;
        }

        auto& Teams = GetFromOffset<TArray<UObject*>>(GameState, TeamsOffset);
        if (Teams.Num() <= 0 || Teams.Num() > 256 ||
            !SDK::MemReadable(Teams.GetData(), sizeof(UObject*) * Teams.Num()))
        {
            return;
        }

        for (auto TeamInfo : Teams)
        {
            if (TeamInfo != DesiredTeam && IsSaneObject(TeamInfo))
                RemoveLateSeasonTeamMember(TeamInfo, Controller);
        }
    }

    bool ApplyLateSeasonHumanTeam(
        AFortGameStateAthena* GameState,
        AFortPlayerControllerAthena* Controller,
        AFortPlayerStateAthena* PlayerState,
        uint8 TeamIndex,
        uint8 FirstTeam,
        bool bNotify,
        const char* Stage)
    {
        if (!IsSaneObject(GameState) || !IsSaneObject(Controller) ||
            !IsSaneObject(PlayerState) ||
            !PlayerState->HasTeamIndex())
        {
            return false;
        }

        UObject* TeamInfo = FindLateSeasonTeamInfo(GameState, TeamIndex);
        auto TeamMembers = GetLateSeasonTeamMembers(TeamInfo);
        const int32 PlayerTeamOffset = (int32)PlayerState->GetOffset("PlayerTeam");
        const int32 PlayerTeamPrivateOffset =
            (int32)PlayerState->GetOffset("PlayerTeamPrivate");
        const int32 PrivateInfoOffset = TeamInfo
            ? (int32)TeamInfo->GetOffset("PrivateInfo")
            : -1;

        if (!TeamInfo || !TeamMembers ||
            PlayerTeamOffset < 0 || PlayerTeamPrivateOffset < 0 ||
            PrivateInfoOffset < 0 ||
            !SDK::MemReadable(
                (const uint8*)PlayerState + PlayerTeamOffset,
                sizeof(UObject*)) ||
            !SDK::MemReadable(
                (const uint8*)PlayerState + PlayerTeamPrivateOffset,
                sizeof(UObject*)) ||
            !SDK::MemReadable(
                (const uint8*)TeamInfo + PrivateInfoOffset,
                sizeof(UObject*)))
        {
            SDK::DbgLog(
                "[Teams] FN17-18 %s could not resolve complete TeamInfo graph for Team=%u (TeamInfo=%p Members=%p)\n",
                Stage,
                (unsigned)TeamIndex,
                (void*)TeamInfo,
                (void*)TeamMembers);
            return false;
        }

        auto PrivateInfo = GetFromOffset<UObject*>(TeamInfo, PrivateInfoOffset);
        if (!IsSaneObject(PrivateInfo))
        {
            SDK::DbgLog(
                "[Teams] FN17-18 %s rejected invalid PrivateInfo=%p for Team=%u\n",
                Stage,
                (void*)PrivateInfo,
                (unsigned)TeamIndex);
            return false;
        }

        const uint8 OldTeamIndex = PlayerState->TeamIndex;
        uint8 SquadId = TeamIndex >= FirstTeam
            ? (uint8)(TeamIndex - FirstTeam)
            : 0;

        auto& PlayerTeam = GetFromOffset<UObject*>(
            PlayerState, PlayerTeamOffset);
        auto OldPlayerTeam = PlayerTeam;
        if (OldPlayerTeam != TeamInfo && IsSaneObject(OldPlayerTeam))
            RemoveLateSeasonTeamMember(OldPlayerTeam, Controller);
        if (bNotify)
            RemoveLateSeasonMemberFromOtherTeams(GameState, TeamInfo, Controller);

        PlayerState->TeamIndex = TeamIndex;
        if (PlayerState->HasSquadId())
            PlayerState->SquadId = SquadId;
        PlayerTeam = TeamInfo;
        GetFromOffset<UObject*>(PlayerState, PlayerTeamPrivateOffset) = PrivateInfo;

        if (!TeamMembers->Contains((AActor*)Controller))
            TeamMembers->Add((AActor*)Controller);
        const int32 TeamMemberCount = TeamMembers->Num();

        if (bNotify)
        {
            if (auto OnRepPlayerTeam =
                PlayerState->GetFunction("OnRep_PlayerTeam"))
            {
                PlayerState->Call<void>(OnRepPlayerTeam);
            }
            if (auto OnRepPlayerTeamPrivate =
                PlayerState->GetFunction("OnRep_PlayerTeamPrivate"))
            {
                PlayerState->Call<void>(OnRepPlayerTeamPrivate);
            }
            if (auto OnRepTeamIndex =
                PlayerState->GetFunction("OnRep_TeamIndex"))
            {
                PlayerState->Call<void>(OnRepTeamIndex, OldTeamIndex);
            }
            if (PlayerState->HasSquadId())
                PlayerState->OnRep_SquadId();

            PlayerState->ForceNetUpdate();
            Controller->ForceNetUpdate();
            GameState->ForceNetUpdate();
        }

        SDK::DbgLog(
            "[Teams] FN17-18 %s PC=%p PS=%p Team=%u Squad=%u TeamInfo=%p Members=%d\n",
            Stage,
            (void*)Controller,
            (void*)PlayerState,
            (unsigned)TeamIndex,
            (unsigned)SquadId,
            (void*)TeamInfo,
            TeamMemberCount);
        return true;
    }

    bool IsLateSeasonUniqueIdValid(const FUniqueNetIdRepl& UniqueId)
    {
        static bool bValidFunctionInitialized = false;
        static UFunction* ValidFunction = nullptr;
        auto Library = UFortKismetLibrary::GetDefaultObj();
        if (Library)
        {
            if (!bValidFunctionInitialized)
            {
                bValidFunctionInitialized = true;
                ValidFunction =
                    Library->GetFunction("IsValid_UniqueNetIdRepl");
            }

            if (ValidFunction)
                return Library->Call<bool>(ValidFunction, UniqueId);
        }

        if (FUniqueNetIdRepl::HasReplicationBytes())
        {
            const auto& Bytes = UniqueId.ReplicationBytes;
            return Bytes.Num() > 0 && Bytes.Num() <= 1024 &&
                SDK::MemReadable(Bytes.GetData(), Bytes.Num());
        }

        return true;
    }

    bool LateSeasonUniqueIdsMatch(
        const FUniqueNetIdRepl& Left,
        const FUniqueNetIdRepl& Right)
    {
        if (!IsLateSeasonUniqueIdValid(Left) ||
            !IsLateSeasonUniqueIdValid(Right))
        {
            return false;
        }

        static bool bEqualFunctionInitialized = false;
        static UFunction* EqualFunction = nullptr;
        auto Library = UFortKismetLibrary::GetDefaultObj();
        if (Library)
        {
            if (!bEqualFunctionInitialized)
            {
                bEqualFunctionInitialized = true;
                EqualFunction = Library->GetFunction(
                    "EqualEqual_UniqueNetIdReplUniqueNetIdRepl");
            }

            if (EqualFunction)
                return Library->Call<bool>(EqualFunction, Left, Right);
        }

        if (FUniqueNetIdRepl::HasReplicationBytes())
        {
            const auto& LeftBytes = Left.ReplicationBytes;
            const auto& RightBytes = Right.ReplicationBytes;
            if (LeftBytes.Num() != RightBytes.Num())
                return false;
            if (LeftBytes.Num() > 0 && LeftBytes.Num() <= 1024 &&
                SDK::MemReadable(LeftBytes.GetData(), LeftBytes.Num()) &&
                SDK::MemReadable(RightBytes.GetData(), RightBytes.Num()))
            {
                return memcmp(
                    LeftBytes.GetData(),
                    RightBytes.GetData(),
                    LeftBytes.Num()) == 0;
            }
        }

        int32 IdSize = FUniqueNetIdRepl::Size();
        if (IdSize <= 0)
            return false;
        if (IdSize > (int32)sizeof(FUniqueNetIdRepl))
            IdSize = sizeof(FUniqueNetIdRepl);

        return memcmp(&Left, &Right, IdSize) == 0;
    }

    void UpsertLateSeasonGameMemberInfo(
        AFortGameStateAthena* GameState,
        AFortPlayerStateAthena* PlayerState)
    {
        if (!IsSaneObject(GameState) || !IsSaneObject(PlayerState) ||
            !GameState->HasGameMemberInfoArray())
        {
            return;
        }

        auto& Members = GameState->GameMemberInfoArray.Members;
        const int32 MemberCount = Members.Num();
        const int32 MemberSize = FGameMemberInfo::Size();
        if (MemberCount < 0 || MemberCount > 256 || MemberSize <= 0 ||
            Members.Max() < MemberCount || Members.Max() > 4096 ||
            (MemberCount > 0 &&
                !SDK::MemReadable(
                    Members.GetData(),
                    (size_t)MemberSize * MemberCount)))
        {
            return;
        }

        auto& PlayerUniqueId =
            PlayerState->HasUniqueID() ? PlayerState->UniqueID : PlayerState->UniqueId;
        int32 KeptIndex = -1;

        for (int32 Index = 0; Index < MemberCount; Index++)
        {
            auto& Member = Members.Get(Index, MemberSize);
            if (LateSeasonUniqueIdsMatch(Member.MemberUniqueId, PlayerUniqueId))
            {
                KeptIndex = Index;
                break;
            }
        }

        int32 RemovedDuplicates = 0;
        if (KeptIndex >= 0)
        {
            for (int32 Index = Members.Num() - 1; Index > KeptIndex; Index--)
            {
                auto& Member = Members.Get(Index, MemberSize);
                if (LateSeasonUniqueIdsMatch(
                    Member.MemberUniqueId, PlayerUniqueId))
                {
                    Members.Remove(Index, MemberSize);
                    RemovedDuplicates++;
                }
            }
        }

        FGameMemberInfo* StoredMember = nullptr;
        if (KeptIndex >= 0)
        {
            StoredMember = &Members.Get(KeptIndex, MemberSize);
        }
        else
        {
            auto Member = (FGameMemberInfo*)malloc(MemberSize);
            if (!Member)
                return;

            memset(Member, 0, MemberSize);
            Member->MostRecentArrayReplicationKey = -1;
            Member->ReplicationID = -1;
            Member->ReplicationKey = -1;
            Member->MemberUniqueId = PlayerUniqueId;
            StoredMember = &Members.Add(*Member, MemberSize);
            free(Member);
        }

        StoredMember->TeamIndex = PlayerState->TeamIndex;
        StoredMember->SquadId = PlayerState->HasSquadId()
            ? PlayerState->SquadId
            : (uint8)0;
        GameState->GameMemberInfoArray.MarkItemDirty(*StoredMember);
        if (RemovedDuplicates > 0)
            GameState->GameMemberInfoArray.MarkArrayDirty();

        auto NotifyGameMemberAdded =
            (void(*)(AFortGameStateAthena*, uint8_t, uint8_t, FUniqueNetIdRepl*))
            NotifyGameMemberAdded_;
        if (KeptIndex < 0 && NotifyGameMemberAdded)
        {
            NotifyGameMemberAdded(
                GameState,
                StoredMember->SquadId,
                StoredMember->TeamIndex,
                &StoredMember->MemberUniqueId);
        }

        SDK::DbgLog(
            "[Teams] FN17-18 GameMemberInfo upsert Team=%u Squad=%u Existing=%d DuplicatesRemoved=%d\n",
            (unsigned)StoredMember->TeamIndex,
            (unsigned)StoredMember->SquadId,
            KeptIndex >= 0 ? 1 : 0,
            RemovedDuplicates);
    }

    using FHeistStartEndGamePhase =
        bool(*)(
            AFortGameModeAthena*,
            AFortPlayerControllerAthena*,
            AActor*,
            const UFortWeaponItemDefinition*,
            uint8);

    FHeistStartEndGamePhase GHeistStartEndGamePhaseOG = nullptr;

    struct FHeistTeamWinNotificationState
    {
        UWorld* World = nullptr;
        AFortGameModeAthena* GameMode = nullptr;
        AFortPlayerControllerAthena* WinningPlayer = nullptr;
        std::vector<AFortPlayerControllerAthena*> NotifiedControllers;
    };

    FHeistTeamWinNotificationState GHeistTeamWinNotifications;

    bool IsActiveHeistMatch(AFortGameModeAthena* GameMode)
    {
        if (!FFortAthenaHeistCompatibility::IsSupportedBuild() ||
            !IsSaneObject(GameMode) ||
            !IsSaneObject(GameMode->GameState))
        {
            return false;
        }

        auto GameState = GameMode->GameState;
        if (GameState->HasCurrentPlaylistInfo() &&
            FPlaylistPropertyArray::StaticStruct())
        {
            const UFortPlaylistAthena* OverridePlaylist =
                FPlaylistPropertyArray::HasOverridePlaylist()
                    ? GameState->CurrentPlaylistInfo.OverridePlaylist
                    : nullptr;
            const UFortPlaylistAthena* BasePlaylist =
                FPlaylistPropertyArray::HasBasePlaylist()
                    ? GameState->CurrentPlaylistInfo.BasePlaylist
                    : nullptr;
            if ((IsSaneObject(
                     const_cast<UFortPlaylistAthena*>(
                         OverridePlaylist)) &&
                    FFortAthenaHeistCompatibility::IsHeistPlaylist(
                        OverridePlaylist)) ||
                (IsSaneObject(
                     const_cast<UFortPlaylistAthena*>(
                         BasePlaylist)) &&
                    FFortAthenaHeistCompatibility::IsHeistPlaylist(
                        BasePlaylist)))
            {
                return true;
            }
        }

        if (GameState->HasCurrentPlaylistData())
        {
            const UFortPlaylistAthena* Playlist =
                GameState->CurrentPlaylistData;
            return IsSaneObject(
                       const_cast<UFortPlaylistAthena*>(Playlist)) &&
                FFortAthenaHeistCompatibility::IsHeistPlaylist(Playlist);
        }

        return false;
    }

    std::vector<AFortPlayerControllerAthena*>
    CaptureConnectedHeistTeammates(
        AFortPlayerControllerAthena* WinningPlayer)
    {
        std::vector<AFortPlayerControllerAthena*> Teammates;
        if (!IsSaneObject(WinningPlayer) ||
            !IsSaneObject(WinningPlayer->PlayerState))
        {
            return Teammates;
        }

        auto WinningPlayerState =
            static_cast<AFortPlayerStateAthena*>(
                WinningPlayer->PlayerState);
        if (!WinningPlayerState->HasTeamIndex() ||
            WinningPlayerState->TeamIndex < 3 ||
            WinningPlayerState->TeamIndex >= 250)
        {
            return Teammates;
        }

        auto World = UWorld::GetWorld();
        if (!IsSaneObject(World))
            return Teammates;

        auto Driver =
            static_cast<UNetDriver*>(World->NetDriver);
        if (!IsSaneObject(Driver))
            return Teammates;

        auto& Connections = Driver->ClientConnections;
        if (Connections.Num() < 0 || Connections.Num() > 256 ||
            Connections.Max() < Connections.Num() ||
            Connections.Max() > 4096 ||
            (Connections.Num() > 0 &&
                !SDK::MemReadable(
                    Connections.GetData(),
                    sizeof(UNetConnection*) * Connections.Num())))
        {
            return Teammates;
        }

        for (int32 Index = 0; Index < Connections.Num(); ++Index)
        {
            auto Connection = Connections[Index];
            if (!IsSaneObject(Connection))
                continue;

            auto Controller = Connection->PlayerController;
            if (Controller == WinningPlayer ||
                !IsSaneObject(Controller) ||
                !IsSaneObject(Controller->PlayerState))
            {
                continue;
            }

            auto PlayerState =
                static_cast<AFortPlayerStateAthena*>(
                    Controller->PlayerState);
            if (!PlayerState->HasTeamIndex() ||
                PlayerState->TeamIndex !=
                    WinningPlayerState->TeamIndex)
            {
                continue;
            }

            if (std::find(
                    Teammates.begin(), Teammates.end(), Controller) ==
                Teammates.end())
            {
                Teammates.push_back(Controller);
            }
        }

        return Teammates;
    }

    bool NotifyHeistTeammateOfWin(
        AFortPlayerControllerAthena* Controller,
        AActor* FinisherPawn,
        const UFortWeaponItemDefinition* FinishingWeapon,
        uint8 DeathCause)
    {
        if (!IsSaneObject(Controller))
            return false;

        UFunction* Function =
            Controller->GetFunction("ClientNotifyTeamWon");
        if (!Function)
            return false;

        const auto Parameters = Function->GetParamsNamed();
        if (Parameters.Size == 0 || Parameters.Size > 0x100 ||
            Parameters.NameOffsetMap.size() != 3)
        {
            return false;
        }

        constexpr uint64 CPF_Parm = 0x0000000000000080;
        constexpr uint64 CPF_ReturnParm = 0x0000000000000400;
        uint32 FinisherPawnOffset = uint32(-1);
        uint32 FinishingWeaponOffset = uint32(-1);
        uint32 DeathCauseOffset = uint32(-1);

        for (const auto& Parameter : Parameters.NameOffsetMap)
        {
            if (!(Parameter.PropertyFlags & CPF_Parm) ||
                (Parameter.PropertyFlags & CPF_ReturnParm))
            {
                return false;
            }

            uint32 ExpectedSize = 0;
            uint32* DestinationOffset = nullptr;
            if (Parameter.Name == "FinisherPawn")
            {
                ExpectedSize = sizeof(AActor*);
                DestinationOffset = &FinisherPawnOffset;
            }
            else if (Parameter.Name == "FinishingWeapon")
            {
                ExpectedSize = sizeof(UFortWeaponItemDefinition*);
                DestinationOffset = &FinishingWeaponOffset;
            }
            else if (Parameter.Name == "DeathCause")
            {
                ExpectedSize = sizeof(uint8);
                DestinationOffset = &DeathCauseOffset;
            }
            else
            {
                return false;
            }

            if (*DestinationOffset != uint32(-1) ||
                Parameter.ElementSize != ExpectedSize ||
                Parameter.Offset > Parameters.Size ||
                ExpectedSize > Parameters.Size - Parameter.Offset)
            {
                return false;
            }

            *DestinationOffset = Parameter.Offset;
        }

        if (FinisherPawnOffset == uint32(-1) ||
            FinishingWeaponOffset == uint32(-1) ||
            DeathCauseOffset == uint32(-1))
        {
            return false;
        }

        void* Memory = FMemory::Malloc(Parameters.Size);
        if (!Memory)
            return false;

        memset(Memory, 0, Parameters.Size);
        auto MutableFinishingWeapon =
            const_cast<UFortWeaponItemDefinition*>(FinishingWeapon);
        memcpy(
            static_cast<uint8*>(Memory) + FinisherPawnOffset,
            &FinisherPawn,
            sizeof(FinisherPawn));
        memcpy(
            static_cast<uint8*>(Memory) + FinishingWeaponOffset,
            &MutableFinishingWeapon,
            sizeof(MutableFinishingWeapon));
        memcpy(
            static_cast<uint8*>(Memory) + DeathCauseOffset,
            &DeathCause,
            sizeof(DeathCause));

        Controller->ProcessEvent(Function, Memory);
        FMemory::Free(Memory);
        return true;
    }

    bool HeistStartEndGamePhase(
        AFortGameModeAthena* GameMode,
        AFortPlayerControllerAthena* WinningPlayer,
        AActor* FinisherPawn,
        const UFortWeaponItemDefinition* FinishingWeapon,
        uint8 DeathCause)
    {
        if (!GHeistStartEndGamePhaseOG)
            return false;

        const bool bActiveHeistMatch =
            IsActiveHeistMatch(GameMode);
        static const FName WaitingPostMatchState(L"WaitingPostMatch");
        const bool bBeginningEndGame =
            IsSaneObject(GameMode) &&
            GameMode->HasMatchState() &&
            GameMode->MatchState != WaitingPostMatchState;
        auto MatchWorld = UWorld::GetWorld();
        auto Teammates = bActiveHeistMatch
            ? CaptureConnectedHeistTeammates(WinningPlayer)
            : std::vector<AFortPlayerControllerAthena*>{};

        const bool bResult = GHeistStartEndGamePhaseOG(
            GameMode,
            WinningPlayer,
            FinisherPawn,
            FinishingWeapon,
            DeathCause);

        if (!bActiveHeistMatch)
            return bResult;

        auto World = UWorld::GetWorld();
        if (World != MatchWorld || !IsSaneObject(World))
            return bResult;

        auto MarkControllerWon =
            [](AFortPlayerControllerAthena* Controller)
            {
                if (!IsSaneObject(Controller) ||
                    !IsSaneObject(Controller->PlayerState))
                {
                    return;
                }

                auto PlayerState =
                    static_cast<AFortPlayerStateAthena*>(
                        Controller->PlayerState);
                if (PlayerState->HasbHasWonAGame())
                {
                    PlayerState->bHasWonAGame = true;
                    PlayerState->ForceNetUpdate();
                }
            };

        MarkControllerWon(WinningPlayer);
        for (auto Controller : Teammates)
            MarkControllerWon(Controller);

        if (Teammates.empty())
            return bResult;

        if (GHeistTeamWinNotifications.World != World ||
            GHeistTeamWinNotifications.GameMode != GameMode ||
            GHeistTeamWinNotifications.WinningPlayer != WinningPlayer ||
            bBeginningEndGame)
        {
            GHeistTeamWinNotifications.World = World;
            GHeistTeamWinNotifications.GameMode = GameMode;
            GHeistTeamWinNotifications.WinningPlayer = WinningPlayer;
            GHeistTeamWinNotifications.NotifiedControllers.clear();
        }

        int32 NotifiedCount = 0;
        for (auto Controller : Teammates)
        {
            if (!IsSaneObject(Controller) ||
                !IsSaneObject(Controller->PlayerState) ||
                std::find(
                    GHeistTeamWinNotifications.NotifiedControllers.begin(),
                    GHeistTeamWinNotifications.NotifiedControllers.end(),
                    Controller) !=
                    GHeistTeamWinNotifications.NotifiedControllers.end())
            {
                continue;
            }

            if (NotifyHeistTeammateOfWin(
                    Controller,
                    FinisherPawn,
                    FinishingWeapon,
                    DeathCause))
            {
                GHeistTeamWinNotifications.NotifiedControllers.push_back(
                    Controller);
                ++NotifiedCount;
            }
        }

        SDK::DbgLog(
            "[Heist] StartEndGamePhase notified %d/%d connected teammate(s)\n",
            NotifiedCount,
            static_cast<int32>(Teammates.size()));
        return bResult;
    }

    struct FHeistUnwindInfoHeader
    {
        uint8 VersionAndFlags;
        uint8 PrologSize;
        uint8 CodeCount;
        uint8 FrameRegisterAndOffset;
    };

    static_assert(
        sizeof(FHeistUnwindInfoHeader) == 4,
        "Unexpected x64 unwind header size");

    uintptr_t FindHeistRuntimeFunctionStart(
        uintptr_t ControlPc,
        uintptr_t TextStart,
        uintptr_t TextEnd)
    {
        if (!ControlPc || ControlPc < TextStart || ControlPc >= TextEnd)
            return 0;

        DWORD64 RuntimeImageBase = 0;
        PRUNTIME_FUNCTION Entry = RtlLookupFunctionEntry(
            static_cast<DWORD64>(ControlPc),
            &RuntimeImageBase,
            nullptr);
        const uintptr_t ExpectedImageBase =
            Memcury::PE::GetModuleBase();
        if (!Entry ||
            !RuntimeImageBase ||
            static_cast<uintptr_t>(RuntimeImageBase) !=
                ExpectedImageBase)
        {
            return 0;
        }

        constexpr uint8 UnwindFlagChainInfo = 0x4;
        struct FRuntimeFunctionIdentity
        {
            DWORD BeginAddress;
            DWORD EndAddress;
            DWORD UnwindData;
        };
        FRuntimeFunctionIdentity VisitedEntries[16]{};
        int32 VisitedEntryCount = 0;

        for (int32 Depth = 0; Depth < 16; ++Depth)
        {
            if (!SDK::MemReadable(Entry, sizeof(*Entry)))
                return 0;

            const FRuntimeFunctionIdentity Identity{
                Entry->BeginAddress,
                Entry->EndAddress,
                Entry->UnwindData};
            for (int32 Index = 0; Index < VisitedEntryCount; ++Index)
            {
                const auto& Visited = VisitedEntries[Index];
                if (Visited.BeginAddress == Identity.BeginAddress &&
                    Visited.EndAddress == Identity.EndAddress &&
                    Visited.UnwindData == Identity.UnwindData)
                {
                    return 0;
                }
            }
            VisitedEntries[VisitedEntryCount++] = Identity;

            if (Identity.BeginAddress >= Identity.EndAddress)
                return 0;

            const uintptr_t BeginAddress =
                ExpectedImageBase + Identity.BeginAddress;
            const uintptr_t EndAddress =
                ExpectedImageBase + Identity.EndAddress;
            if (BeginAddress < ExpectedImageBase ||
                EndAddress < ExpectedImageBase ||
                BeginAddress < TextStart ||
                EndAddress > TextEnd)
            {
                return 0;
            }

            const uintptr_t UnwindInfoAddress =
                ExpectedImageBase + Identity.UnwindData;
            if (UnwindInfoAddress < ExpectedImageBase)
                return 0;

            auto UnwindInfo =
                reinterpret_cast<const FHeistUnwindInfoHeader*>(
                    UnwindInfoAddress);
            if (!SDK::MemReadable(
                    UnwindInfo, sizeof(FHeistUnwindInfoHeader)) ||
                (UnwindInfo->VersionAndFlags & 0x7) != 1)
            {
                return 0;
            }

            const uint8 Flags =
                UnwindInfo->VersionAndFlags >> 3;
            if (!(Flags & UnwindFlagChainInfo))
                return BeginAddress;
            if (Flags != UnwindFlagChainInfo)
                return 0;

            const size_t AlignedCodeCount =
                (static_cast<size_t>(UnwindInfo->CodeCount) + 1u) &
                ~size_t(1);
            const uintptr_t ChainedEntryAddress =
                UnwindInfoAddress +
                sizeof(FHeistUnwindInfoHeader) +
                AlignedCodeCount * sizeof(uint16);
            if (ChainedEntryAddress < UnwindInfoAddress)
                return 0;

            auto ChainedEntry =
                reinterpret_cast<PRUNTIME_FUNCTION>(
                    ChainedEntryAddress);
            if (!SDK::MemReadable(
                    ChainedEntry, sizeof(*ChainedEntry)))
            {
                return 0;
            }

            Entry = ChainedEntry;
        }

        return 0;
    }

    uintptr_t FindHeistStartEndGamePhase()
    {
        if (!FFortAthenaHeistCompatibility::IsSupportedBuild())
            return 0;

        auto StringReference = Memcury::Scanner::FindStringRef(
            L"FortGameModeAthena: %s won the match!", false);
        if (!StringReference.IsValid())
            return 0;

        const uintptr_t ReferenceAddress = StringReference.Get();
        auto TextSection =
            Memcury::PE::Section::GetSection(".text");
        const uintptr_t TextStart =
            TextSection.GetSectionStart().Get();
        const uintptr_t TextEnd =
            TextSection.GetSectionEnd().Get();
        if (!ReferenceAddress ||
            ReferenceAddress < TextStart ||
            ReferenceAddress >= TextEnd ||
            !SDK::MemReadable((void*)ReferenceAddress, 7))
        {
            return 0;
        }

        const uintptr_t FunctionAddress =
            FindHeistRuntimeFunctionStart(
                ReferenceAddress, TextStart, TextEnd);
        const uintptr_t ValidatedFunctionAddress =
            FunctionAddress
                ? FindHeistRuntimeFunctionStart(
                      FunctionAddress, TextStart, TextEnd)
                : 0;

        constexpr uintptr_t MaxFunctionSpan = 0x10000;
        if (!FunctionAddress ||
            ValidatedFunctionAddress != FunctionAddress ||
            FunctionAddress < TextStart ||
            FunctionAddress >= TextEnd ||
            FunctionAddress > ReferenceAddress ||
            ReferenceAddress - FunctionAddress > MaxFunctionSpan ||
            FunctionAddress + 16 > TextEnd ||
            !SDK::MemReadable((void*)FunctionAddress, 16))
        {
            SDK::DbgLog(
                "[Heist] rejected StartEndGamePhase resolver ref=%p function=%p text=[%p,%p)\n",
                (void*)ReferenceAddress,
                (void*)FunctionAddress,
                (void*)TextStart,
                (void*)TextEnd);
            return 0;
        }

        return FunctionAddress;
    }

    void InstallHeistStartEndGamePhaseHook()
    {
        if (!FFortAthenaHeistCompatibility::IsSupportedBuild())
            return;

        const uintptr_t Address = FindHeistStartEndGamePhase();
        if (!Address)
        {
            SDK::DbgLog(
                "[Heist] StartEndGamePhase resolver unavailable; team-win bridge disabled\n");
            return;
        }

        Utils::Hook(
            Address,
            HeistStartEndGamePhase,
            GHeistStartEndGamePhaseOG);
        SDK::DbgLog(
            "[Heist] StartEndGamePhase team-win bridge installed at %p original=%p\n",
            (void*)Address,
            (void*)GHeistStartEndGamePhaseOG);
    }

    bool IsCarminePlaylist(const UFortPlaylistAthena* Playlist)
    {
        if (VersionInfo.FortniteVersion != 4.20 || !Playlist)
            return false;

        static const FName CarminePlaylistName(L"Playlist_Carmine");
        return Playlist->Name == CarminePlaylistName;
    }

    uintptr_t ValidateCarminePlaylistFunction(uintptr_t Address)
    {
        if (!Address)
            return 0;

        auto TextSection =
            Memcury::PE::Section::GetSection(".text");
        const uintptr_t TextStart =
            TextSection.GetSectionStart().Get();
        const uintptr_t TextEnd =
            TextSection.GetSectionEnd().Get();
        if (!TextStart || TextEnd <= TextStart ||
            Address < TextStart || Address >= TextEnd)
        {
            return 0;
        }

        return FindHeistRuntimeFunctionStart(
                   Address, TextStart, TextEnd) == Address &&
                SDK::MemReadable((void*)Address, 16)
            ? Address
            : 0;
    }

    uintptr_t FindCarminePlaylistDataLoader()
    {
        static bool bInitialized = false;
        static uintptr_t Address = 0;
        if (bInitialized)
            return Address;

        bInitialized = true;
        if (VersionInfo.FortniteVersion != 4.20)
            return 0;

        auto StringReference =
            Memcury::Scanner::FindStringRef(
                L"PLAYLIST: Playlist Object is loading its assets in "
                L"AFortGameStateAthena::LoadCurrentPlaylistData(), "
                L"PlaylistName is %s (Server Side)",
                false);
        if (!StringReference.IsValid())
        {
            StringReference =
                Memcury::Scanner::FindStringRef(
                    L"PLAYLIST: Playlist Object is loading its assets in "
                    L"AFortGameStateAthena::LoadCurrentPlaylistData(), "
                    L"PlaylistName is %s (Client Side)",
                    false);
        }

        if (StringReference.IsValid())
        {
            auto TextSection =
                Memcury::PE::Section::GetSection(".text");
            Address = FindHeistRuntimeFunctionStart(
                StringReference.Get(),
                TextSection.GetSectionStart().Get(),
                TextSection.GetSectionEnd().Get());
            Address = ValidateCarminePlaylistFunction(Address);
        }

        SDK::DbgLog(
            "[Carmine] LoadCurrentPlaylistData resolver=%p\n",
            (void*)Address);
        return Address;
    }

    uintptr_t FindCarminePlaylistDataInitializer()
    {
        static bool bInitialized = false;
        static uintptr_t Address = 0;
        if (bInitialized)
            return Address;

        bInitialized = true;
        if (VersionInfo.FortniteVersion != 4.20)
            return 0;

        Address =
            Memcury::Scanner::FindPattern(
                "40 53 48 83 EC ? 48 8B D9 48 8B 89 ? ? ? ? "
                "48 85 C9 74 ? 80 BB",
                false)
                .Get();
        Address = ValidateCarminePlaylistFunction(Address);

        SDK::DbgLog(
            "[Carmine] InitializePlaylistDataPreDataLoad resolver=%p\n",
            (void*)Address);
        return Address;
    }

    void LoadCarminePlaylistData(
        AFortGameStateAthena* GameState,
        const UFortPlaylistAthena* Playlist)
    {
        if (!IsCarminePlaylist(Playlist) ||
            !IsSaneObject(GameState))
        {
            return;
        }

        // Core publishes the ID before entering the native data loader.
        // CurrentPlaylistInfo alone does not start Carmine's playlist-owned
        // assets on 4.20.
        if (auto OnRepCurrentPlaylistId =
                GameState->GetFunction("OnRep_CurrentPlaylistId"))
        {
            GameState->ProcessEvent(OnRepCurrentPlaylistId, nullptr);
        }
        else
        {
            SDK::DbgLog(
                "[Carmine] OnRep_CurrentPlaylistId unavailable; "
                "playlist data load skipped\n");
            return;
        }

        if (!GameState->HasbPlaylistDataIsLoaded() ||
            !GameState->HasbPlaylistDataIsActivelyLoading())
        {
            SDK::DbgLog(
                "[Carmine] playlist load-state properties unavailable; "
                "native data load skipped\n");
            return;
        }

        if (GameState->bPlaylistDataIsLoaded ||
            GameState->bPlaylistDataIsActivelyLoading)
        {
            SDK::DbgLog(
                "[Carmine] playlist data already loaded/loading\n");
            return;
        }

        auto ReflectedInitialize =
            GameState->GetFunction(
                "InitializePlaylistDataPreDataLoad");
        auto ReflectedLoad =
            GameState->GetFunction("LoadCurrentPlaylistData");
        if (ReflectedInitialize && ReflectedLoad)
        {
            auto MatchWorld = UWorld::GetWorld();
            GameState->ProcessEvent(ReflectedInitialize, nullptr);
            if (UWorld::GetWorld() != MatchWorld ||
                !IsSaneObject(GameState))
            {
                return;
            }

            GameState->ProcessEvent(ReflectedLoad, nullptr);
            SDK::DbgLog(
                "[Carmine] reflected playlist-data load requested\n");
            return;
        }

        const uintptr_t InitializeAddress =
            FindCarminePlaylistDataInitializer();
        const uintptr_t LoadAddress =
            FindCarminePlaylistDataLoader();
        if (!InitializeAddress || !LoadAddress)
        {
            SDK::DbgLog(
                "[Carmine] native playlist-data pipeline unresolved; "
                "load skipped\n");
            return;
        }

        auto MatchWorld = UWorld::GetWorld();
        auto InitializePlaylistData =
            reinterpret_cast<void(*)(AFortGameStateAthena*)>(
                InitializeAddress);
        auto LoadCurrentPlaylistData =
            reinterpret_cast<void(*)(AFortGameStateAthena*)>(
                LoadAddress);

        InitializePlaylistData(GameState);
        if (UWorld::GetWorld() != MatchWorld ||
            !IsSaneObject(GameState))
        {
            return;
        }

        LoadCurrentPlaylistData(GameState);
        SDK::DbgLog(
            "[Carmine] native playlist-data load requested\n");
    }

    struct FNative1040PlaylistPublishState
    {
        UWorld* World = nullptr;
        AFortGameStateAthena* GameState = nullptr;
        const UFortPlaylistAthena* Playlist = nullptr;
        bool bPublished = false;
    };

    FNative1040PlaylistPublishState
        GNative1040PlaylistPublishState;

    void RefreshPlaylistReflectionCache(
        AFortGameStateAthena* GameState)
    {
        if (!GameState || !GameState->Class)
            return;

        const int32 PreviousInfoOffset =
            AFortGameStateAthena::CurrentPlaylistInfo__Offset;
        const int32 PreviousDataOffset =
            AFortGameStateAthena::CurrentPlaylistData__Offset;

        AFortGameStateAthena::CurrentPlaylistInfo__Offset =
            static_cast<int32>(
                GameState->GetOffset("CurrentPlaylistInfo"));
        AFortGameStateAthena::CurrentPlaylistData__Offset =
            static_cast<int32>(
                GameState->GetOffset("CurrentPlaylistData"));

        const auto PlaylistInfoStruct =
            FindStruct("PlaylistPropertyArray");
        if (PlaylistInfoStruct)
        {
            FPlaylistPropertyArray::BasePlaylist__Offset =
                static_cast<int32>(
                    PlaylistInfoStruct->GetOffset("BasePlaylist"));
            FPlaylistPropertyArray::OverridePlaylist__Offset =
                static_cast<int32>(
                    PlaylistInfoStruct->GetOffset(
                        "OverridePlaylist"));
            FPlaylistPropertyArray::PlaylistReplicationKey__Offset =
                static_cast<int32>(
                    PlaylistInfoStruct->GetOffset(
                        "PlaylistReplicationKey"));
        }
        else
        {
            FPlaylistPropertyArray::BasePlaylist__Offset = -1;
            FPlaylistPropertyArray::OverridePlaylist__Offset = -1;
            FPlaylistPropertyArray::PlaylistReplicationKey__Offset = -1;
        }

        const auto ClassName = GameState->Class->Name.ToString();
        SDK::DbgLog(
            "[Playlist] reflection refresh class=%s previousInfo=%d "
            "previousData=%d info=%d data=%d struct=%p base=%d "
            "override=%d key=%d FN=%.2f UE=%.2f\n",
            ClassName.c_str(),
            PreviousInfoOffset,
            PreviousDataOffset,
            AFortGameStateAthena::CurrentPlaylistInfo__Offset,
            AFortGameStateAthena::CurrentPlaylistData__Offset,
            (void*)PlaylistInfoStruct,
            FPlaylistPropertyArray::BasePlaylist__Offset,
            FPlaylistPropertyArray::OverridePlaylist__Offset,
            FPlaylistPropertyArray::PlaylistReplicationKey__Offset,
            VersionInfo.FortniteVersion,
            VersionInfo.EngineVersion);
    }

    const UFortPlaylistAthena* GetPublishedPlaylist(
        AFortGameStateAthena* GameState)
    {
        if (!IsSaneObject(GameState))
            return nullptr;

        const bool bHasPlaylistInfo =
            GameState->HasCurrentPlaylistInfo();
        const bool bHasBasePlaylist =
            FPlaylistPropertyArray::BasePlaylist__Offset >= 0;
        const bool bHasOverridePlaylist =
            FPlaylistPropertyArray::OverridePlaylist__Offset >= 0;

        if (VersionInfo.EngineVersion < 4.24 &&
            bHasPlaylistInfo && bHasBasePlaylist)
        {
            const auto BasePlaylist =
                GameState->CurrentPlaylistInfo.BasePlaylist;
            if (IsSanePlaylist(BasePlaylist))
                return BasePlaylist;
        }

        if (bHasPlaylistInfo &&
            (bHasBasePlaylist || bHasOverridePlaylist))
        {
            if (bHasOverridePlaylist)
            {
                if (const auto OverridePlaylist =
                        GameState->CurrentPlaylistInfo
                            .OverridePlaylist)
                {
                    if (IsSanePlaylist(OverridePlaylist))
                        return OverridePlaylist;
                }
            }
            if (bHasBasePlaylist)
            {
                const auto BasePlaylist =
                    GameState->CurrentPlaylistInfo.BasePlaylist;
                if (IsSanePlaylist(BasePlaylist))
                    return BasePlaylist;
            }
        }

        if (GameState->HasCurrentPlaylistData())
        {
            const auto Playlist = GameState->CurrentPlaylistData;
            if (IsSanePlaylist(Playlist))
                return Playlist;
        }

        return nullptr;
    }

    const UFortPlaylistAthena* ResolveActivePlaylist(
        AFortGameStateAthena* GameState)
    {
        if (const auto Published = GetPublishedPlaylist(GameState))
            return Published;

        // Several event builds, including 18.40, expose
        // CurrentPlaylistInfo without exposing a readable BasePlaylist member
        // and omit CurrentPlaylistData entirely. Never fall through from one
        // missing reflected slot into an unchecked DEFINE_PROP read. The
        // configured asset is the authoritative safe fallback on those dumps.
        auto Configured =
            FindObject<UFortPlaylistAthena>(FConfiguration::Playlist);
        if (IsSanePlaylist(Configured))
            return Configured;

        auto DefaultPlaylist = FindObject<UFortPlaylistAthena>(
            L"/Game/Athena/Playlists/"
            L"Playlist_DefaultSolo.Playlist_DefaultSolo");
        return IsSanePlaylist(DefaultPlaylist)
            ? DefaultPlaylist
            : nullptr;
    }

    bool PublishNative1040Playlist(
        AFortGameStateAthena* GameState,
        const UFortPlaylistAthena* Playlist)
    {
        if (!IsSaneObject(GameState) ||
            !FFortAthenaNativeLTMCompatibility::
                IsTargetPlaylist(Playlist))
        {
            return false;
        }

        UWorld* World = UWorld::GetWorld();
        auto& State = GNative1040PlaylistPublishState;
        if (State.World != World ||
            State.GameState != GameState ||
            State.Playlist != Playlist)
        {
            State = {};
            State.World = World;
            State.GameState = GameState;
            State.Playlist = Playlist;
        }
        if (State.bPublished)
            return true;

        // Exact 10.40 initializes playlist-owned mission generators,
        // modifiers, and persistent effects only after MapInfo exists.
        if (GameState->HasMapInfo() && !GameState->MapInfo)
            return false;

        // OnRep_CurrentPlaylistInfo is not idempotent: it creates and
        // registers the playlist's configured mutators. Mark this publication
        // complete before either callback so a NetDriver tick re-entering this
        // function cannot create a second set.
        State.bPublished = true;
        FFortAthenaNativeLTMCompatibility::
            BeginPlaylistPublication(GameState, Playlist);
        GameState->OnRep_CurrentPlaylistId();
        GameState->OnRep_CurrentPlaylistInfo();
        FFortAthenaNativeLTMCompatibility::
            EndPlaylistPublication(GameState, Playlist);

        SDK::DbgLog(
            "[NativeLTM] published 10.40 playlist once after MapInfo "
            "(id=%d name=%s)\n",
            Playlist->PlaylistId,
            Playlist->Name.ToString().c_str());
        FFortAthenaNativeLTMCompatibility::PreparePlaylist(
            GameState, Playlist);
        return true;
    }
}

const UFortPlaylistAthena* AFortGameMode::GetActivePlaylist(
    AFortGameStateAthena* GameState)
{
    return ResolveActivePlaylist(GameState);
}

void SetupPlaylist(AFortGameMode* GameMode, AFortGameStateAthena* GameState)
{
    auto Playlist = FindObject<UFortPlaylistAthena>(FConfiguration::Playlist);

    if (!Playlist)
        Playlist = FindObject<UFortPlaylistAthena>(L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo");

    if (Playlist)
    {
        RefreshPlaylistReflectionCache(GameState);

        // Capture the authored values before this function applies any
        // pre-start override. The same game-thread policy is reasserted after
        // native mutators initialize and can therefore restore these values if
        // the user disables the toggle during the match.
        AFortPlayerControllerAthena::
            ApplyConfiguredRespawnPolicy();

        const bool bIsCarminePlaylist =
            IsCarminePlaylist(Playlist);
        const bool bIsNative1040LTM =
            FFortAthenaNativeLTMCompatibility::
                IsTargetPlaylist(Playlist);
        const bool bIsOriginalFoodFight =
            FFortAthenaNativeLTMCompatibility::
                IsOriginalFoodFightPlaylist(Playlist);

        // Respawn options are user gameplay policy even for native LTMs.
        // Apply the selected values after loading the authored playlist while
        // leaving its teams, inventory, and objective data untouched.
        if (FConfiguration::bForceRespawns)
        {
            if (Playlist->HasbRespawnInAir())
                Playlist->bRespawnInAir = true;
            if (Playlist->HasRespawnHeight())
            {
                Playlist->RespawnHeight.Curve.CurveTable = nullptr;
                Playlist->RespawnHeight.Curve.RowName = FName();
                Playlist->RespawnHeight.Value = FConfiguration::RespawnHeight;
            }
            if (Playlist->HasRespawnTime())
            {
                Playlist->RespawnTime.Curve.CurveTable = nullptr;
                Playlist->RespawnTime.Curve.RowName = FName();
                Playlist->RespawnTime.Value = FConfiguration::RespawnTime;
            }

            if (Playlist->HasRespawnType())
            {
                if (FConfiguration::PermanentRespawn)
                    Playlist->RespawnType = 1; // InfiniteRespawn
                else
                    Playlist->RespawnType = 2; // InfiniteRespawnExceptStorm
            }

            //if (Playlist->HasbForceRespawnLocationInsideOfVolume())
            //    Playlist->bForceRespawnLocationInsideOfVolume = true;
        }
        if ((FConfiguration::bForceRespawns ||
                FConfiguration::bJoinInProgress))
        {
            if (Playlist->HasbAllowJoinInProgress())
                Playlist->bAllowJoinInProgress = true;
            if (Playlist->HasJoinInProgressMatchType())
                Playlist->JoinInProgressMatchType = UKismetTextLibrary::Conv_StringToText(FString(L"Creative"));
        }
        //if (VersionInfo.FortniteVersion >= 16)
        {
            if (Playlist->HasGarbageCollectionFrequency())
                Playlist->GarbageCollectionFrequency = 9999999999999999.f; // easier than hooking collectgarbage
            if (GameMode->HasPlaylistHotfixOriginalGCFrequency())
                GameMode->PlaylistHotfixOriginalGCFrequency = 9999999999999999.f;
            if (GameMode->HasbDisableGCOnServerDuringMatch())
                GameMode->bDisableGCOnServerDuringMatch = true;
            if (GameMode->HasbPlaylistHotfixChangedGCDisabling())
                GameMode->bPlaylistHotfixChangedGCDisabling = true;
        }
        const bool bHasCurrentPlaylistInfo =
            GameState->HasCurrentPlaylistInfo();
        const bool bHasPlaylistInfoLayout =
            bHasCurrentPlaylistInfo &&
            FPlaylistPropertyArray::BasePlaylist__Offset >= 0 &&
            FPlaylistPropertyArray::PlaylistReplicationKey__Offset >= 0;
        const bool bUseLegacyPlaylistInfo =
            VersionInfo.EngineVersion < 4.24 &&
            bHasPlaylistInfoLayout;
        SDK::DbgLog(
            "[Playlist] setup gameState=%p playlist=%p name=%s "
            "hasInfo=%d hasData=%d infoOffset=%d dataOffset=%d "
            "baseOffset=%d keyOffset=%d legacy=%d FN=%.2f UE=%.2f\n",
            (void*)GameState,
            (void*)Playlist,
            Playlist->Name.ToString().c_str(),
            (int)bHasCurrentPlaylistInfo,
            (int)GameState->HasCurrentPlaylistData(),
            AFortGameStateAthena::CurrentPlaylistInfo__Offset,
            AFortGameStateAthena::CurrentPlaylistData__Offset,
            FPlaylistPropertyArray::BasePlaylist__Offset,
            FPlaylistPropertyArray::PlaylistReplicationKey__Offset,
            (int)bUseLegacyPlaylistInfo,
            VersionInfo.FortniteVersion,
            VersionInfo.EngineVersion);
        if (bUseLegacyPlaylistInfo)
        {
            GameState->CurrentPlaylistInfo.BasePlaylist = Playlist;
            if ((ShouldRepairLateSeasonTeams() ||
                    FFortAthenaHeistCompatibility::IsHeistPlaylist(
                        Playlist) ||
                    bIsNative1040LTM ||
                    bIsOriginalFoodFight ||
                    bIsCarminePlaylist) &&
                FPlaylistPropertyArray::HasOverridePlaylist())
            {
                GameState->CurrentPlaylistInfo.OverridePlaylist = Playlist;
            }
            GameState->CurrentPlaylistInfo.PlaylistReplicationKey++;
            GameState->CurrentPlaylistInfo.MarkArrayDirty();
            if (!bIsNative1040LTM)
                GameState->OnRep_CurrentPlaylistInfo();

            SDK::DbgLog(
                "[Playlist] legacy publication gameState=%p playlist=%p "
                "name=%s key=%d FN=%.2f UE=%.2f\n",
                (void*)GameState,
                (void*)Playlist,
                Playlist->Name.ToString().c_str(),
                GameState->CurrentPlaylistInfo.PlaylistReplicationKey,
                VersionInfo.FortniteVersion,
                VersionInfo.EngineVersion);
        }
        else
        {
            const bool bUseReflectedPlaylistInfo =
                bHasPlaylistInfoLayout;
            if (bUseReflectedPlaylistInfo)
            {
                GameState->CurrentPlaylistInfo.BasePlaylist = Playlist;
                if ((ShouldRepairLateSeasonTeams() ||
                        FFortAthenaHeistCompatibility::IsHeistPlaylist(
                            Playlist) ||
                        bIsNative1040LTM ||
                        bIsOriginalFoodFight ||
                        bIsCarminePlaylist) &&
                    FPlaylistPropertyArray::HasOverridePlaylist())
                {
                    GameState->CurrentPlaylistInfo.OverridePlaylist = Playlist;
                }
                GameState->CurrentPlaylistInfo.PlaylistReplicationKey++;
                GameState->CurrentPlaylistInfo.MarkArrayDirty();
                if (!bIsNative1040LTM)
                    GameState->OnRep_CurrentPlaylistInfo();
            }
            else if (GameState->HasCurrentPlaylistData())
            {
                GameState->CurrentPlaylistData = Playlist;
                GameState->OnRep_CurrentPlaylistData();
            }
        }

        GameMode->CurrentPlaylistId = Playlist->PlaylistId;
        if (GameState->HasCurrentPlaylistId())
        {
            GameState->CurrentPlaylistId = Playlist->PlaylistId;
            if (bIsCarminePlaylist)
            {
                LoadCarminePlaylistData(GameState, Playlist);
            }
        }
        if (GameMode->HasCurrentPlaylistName())
            GameMode->CurrentPlaylistName = Playlist->PlaylistName;

        if (GameMode->GameSession->HasMaxPlayers())
            GameMode->GameSession->MaxPlayers = Playlist->MaxPlayers;


        if (GameState->HasAirCraftBehavior() && Playlist->HasAirCraftBehavior())
            GameState->AirCraftBehavior = Playlist->AirCraftBehavior;
        if (GameState->HasCachedSafeZoneStartUp() && Playlist->HasSafeZoneStartUp())
            GameState->CachedSafeZoneStartUp = Playlist->SafeZoneStartUp;

        const bool bIsHeistPlaylist =
            FFortAthenaHeistCompatibility::IsHeistPlaylist(Playlist);
        if (bIsHeistPlaylist || bIsNative1040LTM ||
            bIsOriginalFoodFight ||
            ShouldRepairLateSeasonTeams())
        {
            SyncPlaylistTeamSettings(GameMode, GameState, Playlist);
        }

        // Configure each reflected DBNO property independently because their
        // availability and names vary by game version. Never force DBNO on in
        // solo or when the playlist's own DBNO rules disallow it: several
        // builds (notably 15.30) can take the native downed branch without a
        // valid teammate/DBNO transition, leaving a possessed pawn alive at
        // zero health. The playlist exposes either DBNOType or the older
        // inverse bNoDBNO bit, so use whichever capability exists instead of
        // a season gate.
        bool bDBNOOn =
            FConfiguration::bEnableDBNO &&
            IsTeamPlaylist(Playlist) &&
            DoesPlaylistAllowDBNO(Playlist);
        bool bAlwaysDBNO = false;
        if (GameMode->HasbEnableDBNO())
            GameMode->bEnableDBNO = bDBNOOn;
        if (GameMode->HasbDBNOEnabled())
            GameMode->bDBNOEnabled = bDBNOOn;
        if (GameState->HasbDBNOEnabledForGameMode())
            GameState->bDBNOEnabledForGameMode = bDBNOOn;
        SetDBNODeathEnabled(GameState, bDBNOOn);
        // Last living teammates must eliminate instead of entering an
        // unrecoverable downed state.
        if (GameMode->HasbAlwaysDBNO())
            GameMode->bAlwaysDBNO = bAlwaysDBNO;

        bIsLargeTeamGame = Playlist->bIsLargeTeamGame;

        if (ShouldRepairLateSeasonTeams())
        {
            EnsureLateSeasonTeamObjects(
                GameMode, GameState, GetPlaylistFirstTeam(Playlist));
            ApplyLateSeasonDBNOSettings(
                GameMode, GameState, Playlist, "playlist");
            if (GLateSeasonHumanTeams.GameMode != GameMode ||
                GLateSeasonHumanTeams.World != UWorld::GetWorld() ||
                GLateSeasonHumanTeams.Playlist != Playlist)
            {
                ResetLateSeasonHumanTeams(GameMode, Playlist);
            }
            SDK::DbgLog(
                "[Teams] FN17-18 allocator reset: FirstTeam=%u MaxSquadSize=%d LargeTeam=%d\n",
                (unsigned)GLateSeasonHumanTeams.FirstTeam,
                Playlist->HasMaxSquadSize() ? Playlist->MaxSquadSize : 1,
                Playlist->HasbIsLargeTeamGame() ? (int)Playlist->bIsLargeTeamGame : 0);
        }

        FFortAthenaHeistCompatibility::PreparePlaylist(
            GameState, Playlist);
        FFortAthenaScoreRoyaleCompatibility::PreparePlaylist(
            GameState, Playlist);
        if (bIsNative1040LTM)
            PublishNative1040Playlist(GameState, Playlist);
        FFortAthenaNativeLTMCompatibility::PreparePlaylist(
            GameState, Playlist);

        // if (GameState->HasAdditionalPlaylistLevelsStreamed())
            // GameState->OnRep_AdditionalPlaylistLevelsStreamed();
    }
    else
    {
        GameState->CurrentPlaylistId = GameMode->CurrentPlaylistId = 0;

        if (GameMode->GameSession->HasMaxPlayers())
            GameMode->GameSession->MaxPlayers = 100;
    }
}

// PlaylistDataLoaded is what fills AFortGameMode::SafeZoneLocations in the
// newer Athena flow, but Seasons 5 and earlier never run that setup here.
// Late game used to choose a fallback center only *after*
// StartAircraftPhase had already consumed the empty array.  Seed the native
// array first so the indicator, map preview and server storm all use the same
// center from the beginning.
static bool FindLegacyLateGameSafeZoneCenter(FVector& OutCenter)
{
    TArray<ABuildingFoundation*> Foundations;
    Utils::GetAll<ABuildingFoundation>(Foundations);

    const int FoundationCount = Foundations.Num();
    if (FoundationCount <= 0)
    {
        Foundations.Free();
        return false;
    }

    const int StartIndex = rand() % FoundationCount;
    bool bFound = false;
    for (int Offset = 0; Offset < FoundationCount; Offset++)
    {
        auto Foundation = Foundations[(StartIndex + Offset) % FoundationCount];
        if (!Foundation)
            continue;

        auto Candidate = Foundation->K2_GetActorLocation();
        if (!std::isfinite(Candidate.X) || !std::isfinite(Candidate.Y) || Candidate.IsZero())
            continue;

        OutCenter = Candidate;
        bFound = true;
        break;
    }

    Foundations.Free();
    return bFound;
}

static const UFortPlaylistAthena* GetLegacySafeZonePlaylist(AFortGameMode* GameMode)
{
    if (GameMode && GameMode->GameState && VersionInfo.FortniteVersion >= 3.5 &&
        GameMode->HasWarmupRequiredPlayerCount())
    {
        auto GameState = (AFortGameStateAthena*)GameMode->GameState;
        if (GameState->HasCurrentPlaylistInfo())
            return GameState->CurrentPlaylistInfo.BasePlaylist;
        if (GameState->HasCurrentPlaylistData())
            return GameState->CurrentPlaylistData;
    }

    auto Playlist = FindObject<UFortPlaylistAthena>(FConfiguration::Playlist);
    return Playlist ? Playlist : FindObject<UFortPlaylistAthena>(
        L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo");
}

static const UFortPlaylistAthena* GetConfiguredSafeZonePlaylist()
{
    // ReadyToStartMatch runs before SetupPlaylist publishes the selected
    // playlist to GameState. Resolve the same configured asset and fallback
    // used by SetupPlaylist so preflight never validates a stale/default
    // CurrentPlaylistInfo from the travel world.
    auto Playlist =
        FindObject<UFortPlaylistAthena>(FConfiguration::Playlist);
    return Playlist ? Playlist : FindObject<UFortPlaylistAthena>(
        L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo");
}

static int GetLegacySafeZoneLocationCount(AFortGameMode* GameMode)
{
    int RequiredLocationCount = 0;
    auto Playlist = GetLegacySafeZonePlaylist(GameMode);
    if (Playlist && Playlist->HasLastSafeZoneIndex() && Playlist->LastSafeZoneIndex >= 0 &&
        Playlist->LastSafeZoneIndex < 32)
    {
        RequiredLocationCount = Playlist->LastSafeZoneIndex + 1;
    }

    auto GameState = GameMode ? (AFortGameStateAthena*)GameMode->GameState : nullptr;
    auto MapInfo = GameState && GameState->HasMapInfo() ? GameState->MapInfo : nullptr;
    if (RequiredLocationCount <= 0 && MapInfo)
    {
        if (MapInfo->HasSafeZoneDefinitions() && MapInfo->SafeZoneDefinitions.Num() > 0)
            RequiredLocationCount = MapInfo->SafeZoneDefinitions.Num();
        else if (MapInfo->HasSafeZoneDefinition() && FFortSafeZoneDefinition::HasCount())
            RequiredLocationCount = (int)MapInfo->SafeZoneDefinition.Count.Evaluate(0.0f);
    }

    if (RequiredLocationCount <= 0 || RequiredLocationCount > 32)
        RequiredLocationCount = 12;

    // The existing late-game aircraft path intentionally requires one entry
    // beyond the selected phase before it trusts the native array.
    const int MinimumLocationCount = FConfiguration::LateGameZone + 1;
    if (RequiredLocationCount < MinimumLocationCount)
        RequiredLocationCount = MinimumLocationCount;
    return RequiredLocationCount;
}

// FN21.10-25.19 may keep its managed phase schedule outside the indicator.
// Tag that process-static fallback with its exact world/map generation so a
// previous travel's smaller array cannot falsely constrain startup preflight.
TArray<FFortSafeZonePhaseInfo> Phases;
static TWeakObjectPtr<UWorld> GManagedSafeZonePhasesWorld;
static TWeakObjectPtr<AFortAthenaMapInfo> GManagedSafeZonePhasesMapInfo;

int32 AFortGameMode::ResolveMovingSafeZonePreflightCapacity(
	AFortGameMode* GameMode,
	AFortAthenaMapInfo* MapInfo)
{
	int32 Capacity =
		CustomSafeZoneRuntime::ResolveNativePhaseCapacity(MapInfo);

	// Native-owned early builds can expose their already-sized center array
	// instead of the later SafeZoneDefinition.Count field. Only a populated
	// native array is exact enough for this strict gate; the legacy helper's
	// synthetic 12-entry fallback must never be treated as capacity evidence.
	if (VersionInfo.FortniteVersion < 21.10f && GameMode &&
		GameMode->HasSafeZoneLocations() &&
		GameMode->SafeZoneLocations.IsValid())
	{
		const int32 LocationCount =
			GameMode->SafeZoneLocations.Num();
		if (LocationCount > 0 && LocationCount <= 64)
		{
			Capacity = Capacity > 0
				? (std::min)(Capacity, LocationCount)
				: LocationCount;
		}
	}

	// LastSafeZoneIndex is inclusive. A non-negative playlist override can
	// shorten the phases reachable from the map's schedule, but -1 explicitly
	// means that the live map Count remains authoritative.
	const auto Playlist = GetConfiguredSafeZonePlaylist();
	if (Playlist && Playlist->HasLastSafeZoneIndex() &&
		Playlist->LastSafeZoneIndex >= 0 &&
		Playlist->LastSafeZoneIndex < 64)
	{
		const int32 PlaylistCapacity =
			Playlist->LastSafeZoneIndex + 1;
		Capacity = Capacity > 0
			? (std::min)(Capacity, PlaylistCapacity)
			: PlaylistCapacity;
	}

	auto TightenToPopulatedArray = [&](const int32 Count)
	{
		if (Count <= 0 || Count > 64)
			return;
		Capacity = Capacity > 0
			? (std::min)(Capacity, Count)
			: Count;
	};

	AFortSafeZoneIndicator* Indicator = nullptr;
	if (VersionInfo.FortniteVersion >= 25.20f)
	{
		auto PhaseLogic =
			CustomSafeZoneRuntime::ResolveLiveComponentPhaseLogic(
				UWorld::GetWorld());
		if (PhaseLogic && PhaseLogic->HasSafeZoneIndicator())
			Indicator = PhaseLogic->SafeZoneIndicator;
	}
	else if (GameMode && GameMode->HasSafeZoneIndicator())
	{
		Indicator = GameMode->SafeZoneIndicator;
	}

	if (Indicator && Indicator->HasSafeZonePhases() &&
		Indicator->SafeZonePhases.IsValid())
	{
		TightenToPopulatedArray(Indicator->SafeZonePhases.Num());
	}
	else if (VersionInfo.FortniteVersion >= 21.10f &&
		VersionInfo.FortniteVersion < 25.20f && Phases.IsValid() &&
		GManagedSafeZonePhasesWorld.Get() == UWorld::GetWorld() &&
		GManagedSafeZonePhasesMapInfo.Get() == MapInfo)
	{
		TightenToPopulatedArray(Phases.Num());
	}

	return Capacity;
}

static float GetLegacySafeZonePhaseRadius(
    AFortGameMode* GameMode,
    int PhaseIndex)
{
    // Used only when an early build did not publish its native phase centers.
    // Prefer the map's authored radius curve; the fallback mirrors the legacy
    // phase table already used by Magnesium's component-owned storm path.
    constexpr static std::array<float, 13> FallbackRadii{
        150000.f, 120000.f, 95000.f, 70000.f, 55000.f,
        32500.f, 20000.f, 10000.f, 5000.f, 2500.f,
        1650.f, 1090.f, 0.f,
    };

    if (PhaseIndex < 0)
        return 0.f;

    auto GameState =
        GameMode && GameMode->GameState
            ? (AFortGameStateAthena*)GameMode->GameState
            : nullptr;
    auto MapInfo =
        GameState && GameState->HasMapInfo()
            ? GameState->MapInfo
            : nullptr;
    if (MapInfo && MapInfo->HasSafeZoneDefinition() &&
        FFortSafeZoneDefinition::HasRadius())
    {
        auto& RadiusDefinition =
            MapInfo->SafeZoneDefinition.Radius;
        const float Radius =
            RadiusDefinition.Evaluate((float)PhaseIndex);
        const bool bHasAuthoredRadius =
            RadiusDefinition.Value > 0.f ||
            (RadiusDefinition.Curve.CurveTable &&
                RadiusDefinition.Curve.RowName.IsValid());
        const int LastPhase =
            GetLegacySafeZoneLocationCount(GameMode) - 1;
        if (bHasAuthoredRadius && std::isfinite(Radius) &&
            (Radius > 0.f ||
                (Radius == 0.f && PhaseIndex >= LastPhase)))
        {
            return Radius;
        }
    }

    if (PhaseIndex < (int)FallbackRadii.size())
        return FallbackRadii[PhaseIndex];

    return 0.f;
}

static FVector GenerateContainedLegacySafeZoneCenter(
    AFortGameMode* GameMode,
    const FVector& PreviousCenter,
    int PreviousPhase,
    int NextPhase)
{
    const float PreviousRadius =
        GetLegacySafeZonePhaseRadius(GameMode, PreviousPhase);
    const float NextRadius =
        GetLegacySafeZonePhaseRadius(GameMode, NextPhase);
    if (!std::isfinite(PreviousRadius) ||
        !std::isfinite(NextRadius) ||
        PreviousRadius <= 0.f ||
        NextRadius < 0.f ||
        NextRadius >= PreviousRadius)
    {
        return PreviousCenter;
    }

    // Retain the project's original slight-drift character, but cap the
    // displacement by the radius difference so the white circle is always
    // fully contained by the current blue circle.
    const float MaximumOffset = (std::min)(
        PreviousRadius - NextRadius,
        PreviousRadius * 0.4f);
    if (!std::isfinite(MaximumOffset) ||
        MaximumOffset <= KINDA_SMALL_NUMBER)
    {
        return PreviousCenter;
    }

    static std::mt19937 Generator(std::random_device{}());
    static std::uniform_real_distribution<float> Unit(0.f, 1.f);
    constexpr float TwoPi = 6.28318530717958647692f;
    const float Angle = Unit(Generator) * TwoPi;
    const float Distance =
        std::sqrt(Unit(Generator)) * MaximumOffset;

    FVector Center = PreviousCenter;
    Center.X += std::cos(Angle) * Distance;
    Center.Y += std::sin(Angle) * Distance;
    return std::isfinite(Center.X) &&
        std::isfinite(Center.Y)
            ? Center
            : PreviousCenter;
}

static void ClampLegacySafeZoneCenterToContainment(
    const FVector& PreviousCenter,
    FVector& NextCenter,
    float PreviousRadius,
    float NextRadius)
{
    if (!std::isfinite(PreviousRadius) ||
        !std::isfinite(NextRadius) ||
        PreviousRadius <= 0.f ||
        NextRadius < 0.f ||
        NextRadius > PreviousRadius)
    {
        return;
    }

    if (NextRadius == PreviousRadius)
    {
        NextCenter.X = PreviousCenter.X;
        NextCenter.Y = PreviousCenter.Y;
        NextCenter.Z = PreviousCenter.Z;
        return;
    }

    const double DeltaX = NextCenter.X - PreviousCenter.X;
    const double DeltaY = NextCenter.Y - PreviousCenter.Y;
    const double Distance = std::hypot(DeltaX, DeltaY);
    const double MaximumOffset =
        (double)PreviousRadius - (double)NextRadius;
    if (!std::isfinite(Distance))
    {
        NextCenter.X = PreviousCenter.X;
        NextCenter.Y = PreviousCenter.Y;
        NextCenter.Z = PreviousCenter.Z;
        return;
    }

    if (Distance <= MaximumOffset)
        return;

    if (Distance <= KINDA_SMALL_NUMBER ||
        MaximumOffset <= KINDA_SMALL_NUMBER)
    {
        NextCenter.X = PreviousCenter.X;
        NextCenter.Y = PreviousCenter.Y;
        NextCenter.Z = PreviousCenter.Z;
        return;
    }

    // Keep a small numerical margin so float-backed Chapter 1 replication
    // cannot round an exactly tangent inner circle outside the live wall.
    const double Scale =
        MaximumOffset * 0.98 / Distance;
    NextCenter.X = PreviousCenter.X + DeltaX * Scale;
    NextCenter.Y = PreviousCenter.Y + DeltaY * Scale;
    NextCenter.Z = PreviousCenter.Z;
}

struct FLegacyFallbackSafeZonePlan
{
    UWorld* World = nullptr;
    AFortGameMode* GameMode = nullptr;
    int AnchorIndex = -1;
    int LastLoggedPhase = -1;
    std::vector<FVector> Centers;
};

static FLegacyFallbackSafeZonePlan
    GLegacyFallbackSafeZonePlan;

static void ResetLegacyFallbackSafeZonePlan()
{
    GLegacyFallbackSafeZonePlan =
        FLegacyFallbackSafeZonePlan{};
}

static void BuildLegacyFallbackSafeZonePlan(
    AFortGameMode* GameMode,
    const FVector& Anchor)
{
    ResetLegacyFallbackSafeZonePlan();
    if (!GameMode || FConfiguration::bCustomSafeZone ||
        !std::isfinite(Anchor.X) ||
        !std::isfinite(Anchor.Y) ||
        !std::isfinite(Anchor.Z) ||
        Anchor.IsZero())
    {
        return;
    }

    auto World = UWorld::GetWorld();
    const int CenterCount =
        GetLegacySafeZoneLocationCount(GameMode);
    if (!World || CenterCount <= 0 || CenterCount > 32)
        return;

    auto& Plan = GLegacyFallbackSafeZonePlan;
    Plan.World = World;
    Plan.GameMode = GameMode;
    Plan.AnchorIndex = std::clamp(
        FConfiguration::LateGameZone.load() - 1,
        0, CenterCount - 1);
    Plan.Centers.clear();
    Plan.Centers.reserve(CenterCount);
    for (int Index = 0; Index < CenterCount; ++Index)
        Plan.Centers.push_back(Anchor);

    for (int Index = Plan.AnchorIndex + 1;
        Index < CenterCount;
        ++Index)
    {
        Plan.Centers[Index] =
            GenerateContainedLegacySafeZoneCenter(
                GameMode, Plan.Centers[Index - 1],
                Index - 1, Index);
    }

    const int PreviewIndex = (std::min)(
        Plan.AnchorIndex + 1, CenterCount - 1);
    const FVector& Preview = Plan.Centers[PreviewIndex];
    SDK::DbgLog(
        "[SafeZone] built fallback center plan phase=%d anchor=(%.1f, %.1f, %.1f) preview=(%.1f, %.1f, %.1f) offset=%.1f\n",
        Plan.AnchorIndex + 1,
        Anchor.X, Anchor.Y, Anchor.Z,
        Preview.X, Preview.Y, Preview.Z,
        (float)std::hypot(
            Preview.X - Anchor.X,
            Preview.Y - Anchor.Y));
}

static bool ApplyLegacyFallbackSafeZonePlan(
    AFortGameMode* GameMode,
    int SafeZonePhase)
{
    auto& Plan = GLegacyFallbackSafeZonePlan;
    auto World = UWorld::GetWorld();
    auto Indicator =
        GameMode && GameMode->HasSafeZoneIndicator()
            ? GameMode->SafeZoneIndicator
            : nullptr;
    if (!World || !GameMode || !Indicator ||
        FConfiguration::bCustomSafeZone ||
        Plan.World != World || Plan.GameMode != GameMode ||
        Plan.Centers.empty())
    {
        return false;
    }

    const int LastIndex =
        static_cast<int>(Plan.Centers.size()) - 1;
    const int CurrentIndex = std::clamp(
        SafeZonePhase - 1, 0, LastIndex);
    const int NextIndex = std::clamp(
        SafeZonePhase, 0, LastIndex);
    const int NextNextIndex = std::clamp(
        SafeZonePhase + 1, 0, LastIndex);

    float CurrentRadius =
        GetLegacySafeZonePhaseRadius(
            GameMode, CurrentIndex);
    float NextRadius =
        GetLegacySafeZonePhaseRadius(
            GameMode, NextIndex);
    bool bUsedLiveRadii = false;
    float LiveCurrentRadius = -1.f;
    if (Indicator->HasRadius() &&
        std::isfinite(Indicator->Radius) &&
        Indicator->Radius > 0.f)
    {
        LiveCurrentRadius = Indicator->Radius;
    }
    else if (Indicator->HasLastRadius() &&
        std::isfinite(Indicator->LastRadius) &&
        Indicator->LastRadius > 0.f)
    {
        LiveCurrentRadius = Indicator->LastRadius;
    }

    if (Indicator->HasNextRadius() &&
        std::isfinite(Indicator->NextRadius) &&
        LiveCurrentRadius > 0.f &&
        Indicator->NextRadius >= 0.f &&
        Indicator->NextRadius <= LiveCurrentRadius)
    {
        CurrentRadius = LiveCurrentRadius;
        NextRadius = Indicator->NextRadius;
        bUsedLiveRadii = true;
    }

    if (CurrentIndex != NextIndex)
    {
        ClampLegacySafeZoneCenterToContainment(
            Plan.Centers[CurrentIndex],
            Plan.Centers[NextIndex],
            CurrentRadius, NextRadius);
    }

    if (NextIndex != NextNextIndex)
    {
        float NextNextRadius =
            GetLegacySafeZonePhaseRadius(
                GameMode, NextNextIndex);
        if (Indicator->HasNextNextRadius() &&
            std::isfinite(Indicator->NextNextRadius) &&
            Indicator->NextNextRadius >= 0.f &&
            Indicator->NextNextRadius <= NextRadius)
        {
            NextNextRadius = Indicator->NextNextRadius;
        }
        else if (Indicator->HasFutureReplicator() &&
            Indicator->FutureReplicator &&
            Indicator->FutureReplicator->HasNextNextRadius() &&
            std::isfinite(
                Indicator->FutureReplicator->NextNextRadius) &&
            Indicator->FutureReplicator->NextNextRadius >= 0.f &&
            Indicator->FutureReplicator->NextNextRadius <= NextRadius)
        {
            NextNextRadius =
                Indicator->FutureReplicator->NextNextRadius;
        }

        ClampLegacySafeZoneCenterToContainment(
            Plan.Centers[NextIndex],
            Plan.Centers[NextNextIndex],
            NextRadius, NextNextRadius);
    }

    if (Indicator->HasLastCenter())
        Indicator->LastCenter = Plan.Centers[CurrentIndex];
    if (Indicator->HasNextCenter())
        Indicator->NextCenter = Plan.Centers[NextIndex];
    if (Indicator->HasNextNextCenter())
        Indicator->NextNextCenter =
            Plan.Centers[NextNextIndex];
    if (Indicator->HasFutureReplicator() &&
        Indicator->FutureReplicator &&
        Indicator->FutureReplicator->HasNextNextCenter())
    {
        Indicator->FutureReplicator->NextNextCenter =
            Plan.Centers[NextNextIndex];
    }

    Indicator->ForceNetUpdate();

    if (Plan.LastLoggedPhase != SafeZonePhase)
    {
        Plan.LastLoggedPhase = SafeZonePhase;
        const FVector& Current = Plan.Centers[CurrentIndex];
        const FVector& Next = Plan.Centers[NextIndex];
        SDK::DbgLog(
            "[SafeZone] applied fallback centers phase=%d currentIndex=%d nextIndex=%d offset=%.1f radii=%.1f->%.1f live=%d contained=%d\n",
            SafeZonePhase, CurrentIndex, NextIndex,
            (float)std::hypot(
                Next.X - Current.X,
                Next.Y - Current.Y),
            CurrentRadius, NextRadius,
            (int)bUsedLiveRadii,
            (int)(CurrentRadius >= NextRadius +
                std::hypot(
                    Next.X - Current.X,
                    Next.Y - Current.Y)));
    }

    return true;
}

static bool UsesLegacySafeZoneLocFallback()
{
    return VersionInfo.FortniteVersion == 1.10 ||
        VersionInfo.FortniteVersion == 1.72 ||
        VersionInfo.FortniteVersion == 2.50;
}

static void EnsureLegacySafeZoneDamageEffect(AFortGameMode* GameMode)
{
    if (!GameMode || VersionInfo.FortniteVersion >= 7.00 || !FConfiguration::bLateGame)
        return;

    static UClass* OutsideSafeZoneEffect = nullptr;
    if (!OutsideSafeZoneEffect)
        OutsideSafeZoneEffect = const_cast<UClass*>(FindObject<UClass>(
            L"/Game/Athena/SafeZone/GE_OutsideSafeZoneDamage.GE_OutsideSafeZoneDamage_C"));

    if (!OutsideSafeZoneEffect)
        return;

    bool bAssigned = false;
    if (GameMode->HasGE_OutsideSafeZone() && !GameMode->GE_OutsideSafeZone)
    {
        GameMode->GE_OutsideSafeZone = OutsideSafeZoneEffect;
        bAssigned = true;
    }

    auto DefaultGameMode = GameMode->Class
        ? (AFortGameMode*)GameMode->Class->GetDefaultObj()
        : nullptr;
    if (DefaultGameMode && DefaultGameMode->HasGE_OutsideSafeZone() &&
        !DefaultGameMode->GE_OutsideSafeZone)
    {
        DefaultGameMode->GE_OutsideSafeZone = OutsideSafeZoneEffect;
        bAssigned = true;
    }

    if (bAssigned)
        SDK::DbgLog("[SafeZone] assigned legacy native outside effect actor=%p cdo=%p effect=%p\n",
            (void*)GameMode, (void*)DefaultGameMode, (void*)OutsideSafeZoneEffect);
}

static bool GHasNativeLateGameSafeZonePhaseHook = false;
static bool GHasManagedMovingSafeZonePhaseHooks = false;

static bool HasNativeMovingSafeZonePhasePublisher()
{
	return GHasNativeLateGameSafeZonePhaseHook ||
		VersionInfo.FortniteVersion == 2.50 ||
		VersionInfo.FortniteVersion == 7.30;
}

bool AFortGameMode::ProbeMovingSafeZonePhasePublisher()
{
	static double CachedVersion = -1.0;
	static bool bCachedAvailable = false;
	if (CachedVersion == VersionInfo.FortniteVersion)
		return bCachedAvailable;

	CachedVersion = VersionInfo.FortniteVersion;
	if (VersionInfo.FortniteVersion >= 25.20)
	{
		bCachedAvailable = true;
	}
	else if (VersionInfo.FortniteVersion >= 21.10)
	{
		bCachedAvailable =
			FindSpawnInitialSafeZone() != 0 &&
			FindUpdateSafeZonesPhase() != 0;
	}
	else
	{
		bCachedAvailable =
			VersionInfo.FortniteVersion == 2.50 ||
			VersionInfo.FortniteVersion == 7.30 ||
			FindHandlePostSafeZonePhaseChanged() != 0;
	}
	return bCachedAvailable;
}

static void EnsureLegacyLateGameSafeZoneLocations(AFortGameMode* GameMode)
{
    if (!GameMode || VersionInfo.FortniteVersion >= 7.00 || !FConfiguration::bLateGame)
        return;

    // Damage lifecycle setup is needed through Season 6. Location generation
    // itself is already native there, so keep the geometry workaround pre-S6.
    EnsureLegacySafeZoneDamageEffect(GameMode);

    if (!UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bEnableZones ||
        !GameMode->HasSafeZoneLocations())
        return;

	const bool bUseCustomCenter = FConfiguration::bCustomSafeZone;
    if (bUseCustomCenter)
    {
        auto GameState = GameMode->GameState
            ? (AFortGameStateAthena*)GameMode->GameState
            : nullptr;
		if (GameState && GameState->HasMapInfo() && GameState->MapInfo)
			GUI::ResolveCustomSafeZoneForMap(GameState->MapInfo);
	}
	const auto LegacyCustomZone =
		FConfiguration::GetLegacyCustomSafeZoneNodeSnapshot();

	if (bUseCustomCenter &&
		CustomSafeZoneRuntime::IsMovingModeRequested())
	{
		auto GameState = GameMode->GameState
			? (AFortGameStateAthena*)GameMode->GameState
			: nullptr;
		const int RequiredLocationCount =
			GetLegacySafeZoneLocationCount(GameMode);
		if (CustomSafeZoneRuntime::ApplyToLegacyLocations(
				GameMode,
				GameState && GameState->HasMapInfo()
					? GameState->MapInfo
					: nullptr,
				RequiredLocationCount,
				HasNativeMovingSafeZonePhasePublisher()))
		{
			AFortGameMode::SafeZoneLoc = FVector{};
			return;
		}
	}

    // Season 6 already receives native playlist locations. Leave those alone
    // unless the user explicitly selected a custom circle; custom geometry has
    // to replace the native source before StartAircraftPhase consumes it.
    if (VersionInfo.FortniteVersion >= 6.00 && !bUseCustomCenter)
        return;

    // 1.10, 1.7.2 and 2.50 build their real indicator location only after
    // aircraft exit. Pre-filling the empty array makes the bus use a temporary
    // foundation while native SafeZones later regenerates a different center.
    // Preserve the original Erbium SafeZoneLoc fallback on those exact builds.
    // Custom zones remain deliberately concentric and may seed every entry.
    if (UsesLegacySafeZoneLocFallback() && !bUseCustomCenter)
        return;

    // Keep every location the build did initialize. Missing entries through
    // the selected current phase retain the chosen foundation anchor so the
    // aircraft remains over the blue circle; the white preview and subsequent
    // phases receive stable contained offsets from that anchor.
    const int RequiredLocationCount = GetLegacySafeZoneLocationCount(GameMode);
    const int ExistingLocationCount = GameMode->SafeZoneLocations.Num();
    const int AnchorIndex = std::clamp(
        FConfiguration::LateGameZone.load() - 1,
        0, RequiredLocationCount - 1);
    FVector Center;
    bool bHasCenter = false;

    if (bUseCustomCenter)
    {
        Center = FVector(LegacyCustomZone.Center);
        // (0, 0) is a valid deliberate map selection. Only reject corrupt
        // coordinates; the generated-center path below still rejects zero.
        bHasCenter = std::isfinite(Center.X) && std::isfinite(Center.Y) &&
            std::isfinite(Center.Z);
    }

    for (int Index = ExistingLocationCount - 1; !bHasCenter && Index >= 0; Index--)
    {
        auto Candidate = GameMode->SafeZoneLocations.Get(Index, FVector::Size());
        if (std::isfinite(Candidate.X) && std::isfinite(Candidate.Y) && !Candidate.IsZero())
        {
            Center = Candidate;
            bHasCenter = true;
            break;
        }
    }

    if (!bHasCenter && !FindLegacyLateGameSafeZoneCenter(Center))
    {
        SDK::DbgLog("[SafeZone] pre-S6 location setup deferred: no loaded foundation\n");
        return;
    }

    if (!std::isfinite(Center.X) || !std::isfinite(Center.Y) ||
        (!bUseCustomCenter && Center.IsZero()))
        return;

    FVector PreviousCenter = Center;
    for (int Index = 0; Index < ExistingLocationCount; Index++)
    {
        auto& ExistingCenter = GameMode->SafeZoneLocations.Get(Index, FVector::Size());
        const bool bInvalidCenter =
            !std::isfinite(ExistingCenter.X) ||
            !std::isfinite(ExistingCenter.Y) ||
            ExistingCenter.IsZero();
        if (bUseCustomCenter)
        {
            ExistingCenter = Center;
        }
        else if (bInvalidCenter)
        {
            ExistingCenter =
                Index > AnchorIndex && Index > 0
                    ? GenerateContainedLegacySafeZoneCenter(
                        GameMode, PreviousCenter,
                        Index - 1, Index)
                    : PreviousCenter;
        }

        PreviousCenter = ExistingCenter;
    }

    for (int Index = ExistingLocationCount; Index < RequiredLocationCount; Index++)
    {
        FVector NextCenter = PreviousCenter;
        if (!bUseCustomCenter &&
            Index > AnchorIndex && Index > 0)
        {
            NextCenter =
                GenerateContainedLegacySafeZoneCenter(
                    GameMode, PreviousCenter,
                    Index - 1, Index);
        }

        GameMode->SafeZoneLocations.Add(
            NextCenter, FVector::Size());
        PreviousCenter = NextCenter;
    }

    if (GameMode->HasbSafeZoneLocationsInitialized())
        GameMode->bSafeZoneLocationsInitialized = true;

    // A populated native array means the old post-aircraft fallback must stay
    // inactive; otherwise HandlePostSafeZonePhaseChanged would overwrite the
    // native wall and recreate the offset circle. The custom radius is applied
    // separately, once the native fast-forward reaches its target phase.
    AFortGameMode::SafeZoneLoc = FVector{};

    if (bUseCustomCenter)
        SDK::DbgLog("[SafeZoneMap] initialized lower native custom locations %d -> %d at (%.1f, %.1f, %.1f) radius=%.1f\n",
            ExistingLocationCount, GameMode->SafeZoneLocations.Num(),
            Center.X, Center.Y, Center.Z,
            LegacyCustomZone.RadiusCm);
    else if (ExistingLocationCount < RequiredLocationCount)
    {
        const int CurrentIndex = (std::min)(
            AnchorIndex,
            GameMode->SafeZoneLocations.Num() - 1);
        const int PreviewIndex = (std::min)(
            AnchorIndex + 1,
            GameMode->SafeZoneLocations.Num() - 1);
        const FVector& CurrentCenter =
            GameMode->SafeZoneLocations.Get(
                CurrentIndex, FVector::Size());
        const FVector& PreviewCenter =
            GameMode->SafeZoneLocations.Get(
                PreviewIndex, FVector::Size());
        SDK::DbgLog(
            "[SafeZone] initialized pre-S6 native locations %d -> %d current=(%.1f, %.1f, %.1f) preview=(%.1f, %.1f, %.1f) offset=%.1f\n",
            ExistingLocationCount,
            GameMode->SafeZoneLocations.Num(),
            CurrentCenter.X, CurrentCenter.Y, CurrentCenter.Z,
            PreviewCenter.X, PreviewCenter.Y, PreviewCenter.Z,
            (float)std::hypot(
                PreviewCenter.X - CurrentCenter.X,
                PreviewCenter.Y - CurrentCenter.Y));
    }
}


void (*VendWobble__FinishedFuncOG)(UObject* Context, FFrame& Stack);
void VendWobble__FinishedFunc(UObject* Context, FFrame& Stack)
{
    auto CollectorActor = (ABuildingItemCollectorActor*)Context;
    auto PlayerController = CollectorActor->ControllingPlayer;

    if (!PlayerController)
        return VendWobble__FinishedFuncOG(Context, Stack);

    auto Collection = CollectorActor->ItemCollections.Search([&](FCollectorUnitInfo& Coll)
        {
            return Coll.InputItem == CollectorActor->ClientPausedActiveInputItem;
        }, FCollectorUnitInfo::Size());

    if (!Collection)
        return VendWobble__FinishedFuncOG(Context, Stack);

    CollectorActor->ClientPausedActiveInputItem = nullptr;

    float Cost = Collection->InputCount.Evaluate();

    auto VMLoc = CollectorActor->K2_GetActorLocation();
    auto& SpawnLocation = CollectorActor->LootSpawnLocation;
    auto Loc = VMLoc + (CollectorActor->GetActorForwardVector() * SpawnLocation.X) + (CollectorActor->GetActorRightVector() * SpawnLocation.Y) + (CollectorActor->GetActorUpVector() * SpawnLocation.Z);

    for (int i = 0; i < Collection->OutputItemEntry.Num(); i++)
    {
        auto& Item = Collection->OutputItemEntry.Get(i, FFortItemEntry::Size());

        AFortInventory::SpawnPickup(Loc, Item);
        if (CollectorActor->HasPickupSpawned())
            CollectorActor->PickupSpawned.Process();
    }

    /*if (Cost == 0)
    {
        CollectorActor->DoVendDeath();
        CollectorActor->K2_DestroyActor();
    }*/

    return VendWobble__FinishedFuncOG(Context, Stack);
}

std::unordered_map<int, float> WeightMap;
float Sum = 0;
float Weight;
float TotalWeight;

class AFortAthenaLivingWorldStaticPointProvider : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaLivingWorldStaticPointProvider);

    DEFINE_PROP(FiltersTags, FGameplayTagContainer);
    DEFINE_PROP(SpawnPoints, TArray<FTransform>);
    DEFINE_PROP(bStartEnabled, bool);
    DEFINE_PROP(bRandomizeStartPoint, bool);
    DEFINE_PROP(bRandomizePointRotation, bool);
};

class UFortVehicleItemDefinition : public UObject
{
public:
    UCLASS_COMMON_MEMBERS(UFortVehicleItemDefinition);

    DEFINE_PROP(VehicleMinSpawnPercent, FScalableFloat);
    DEFINE_PROP(VehicleMaxSpawnPercent, FScalableFloat);
};

class AFortAthenaVehicleSpawner : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaVehicleSpawner);

    DEFINE_PROP(CachedFortVehicleItemDef, UFortVehicleItemDefinition*);
    DEFINE_PROP(bForceSpawnAlways, bool);

    DEFINE_FUNC(GetVehicleClass, UClass*);
};

class AFortAthenaMutator_SupplyDrop : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortAthenaMutator_SupplyDrop);

    DEFINE_PROP(
        SafeZoneMutatorData,
        FSupplyDropSpawnDataArrayHeader);
};

namespace
{
    struct FSupplyDropSuppressionState
    {
        TWeakObjectPtr<UWorld> World;
        TWeakObjectPtr<UFortPlaylistAthena> Playlist;
        TWeakObjectPtr<
            UFortGameStateComponent_BattleRoyaleGamePhaseLogic>
            GamePhaseLogic;
        TWeakObjectPtr<AFortAthenaMutator_SupplyDrop>
            SupplyDropMutator;
        ULONGLONG NextPlaylistResolveTimeMs = 0;
        ULONGLONG NextGamePhaseLogicResolveTimeMs = 0;
        ULONGLONG NextSupplyDropMutatorResolveTimeMs = 0;
        bool bOriginalUseDefaultSupplyDrops = true;
        bool bHasPlaylistSupplyDropFlag = false;
        bool bLockedForMatch = false;
        bool bApplied = false;
        bool bLoggedEnabled = false;
        bool bLoggedNativeDeliveryPreserved = false;
    };

    FSupplyDropSuppressionState GSupplyDropSuppressionState{};

    bool IsSaneSupplyDropArrayHeader(
        const FSupplyDropSpawnDataArrayHeader& Header)
    {
        if (Header.NumElements < 0 ||
            Header.MaxElements < Header.NumElements ||
            Header.MaxElements > 100000)
        {
            return false;
        }

        return Header.NumElements == 0 ||
            (Header.Data && SDK::MemReadable(Header.Data, 1));
    }

    bool IsOffsetWithinStruct(
        int32 Offset, int32 ValueSize, int32 StructSize)
    {
        return Offset >= 0 &&
            ValueSize > 0 &&
            StructSize > 0 &&
            Offset <= StructSize - ValueSize;
    }

    struct FSupplyDropRuntimeLayout
    {
        int32 EntrySize = 0;
        int32 ZoneDataOffset = -1;
        int32 ZoneDataSize = 0;
        int32 DropsRemainingOffset = -1;
        int32 NextWaveTimeOffset = -1;
        int32 ZoneNextSpawnTimeOffset = -1;
        int32 ItemDataOffset = -1;
        int32 ItemDataSize = 0;
        int32 ItemsToDeliverOffset = -1;
        int32 QueuedTimesOffset = -1;
        int32 ItemNextSpawnTimeOffset = -1;
        int32 InitialSpawnsOffset = -1;
        ULONGLONG NextResolveTimeMs = 0;
        bool bValid = false;
    };

    FSupplyDropRuntimeLayout GSupplyDropRuntimeLayout{};

    bool ResolveSupplyDropRuntimeLayout()
    {
        auto& Layout = GSupplyDropRuntimeLayout;
        if (Layout.bValid)
            return true;

        const ULONGLONG CurrentTimeMs = GetTickCount64();
        if (CurrentTimeMs < Layout.NextResolveTimeMs)
            return false;
        Layout.NextResolveTimeMs = CurrentTimeMs + 1000ULL;

        auto EntryStruct = FindStruct("SupplyDropSpawnData");
        auto ZoneStruct =
            FindStruct("SupplyDropZoneBasedSpawnData");
        auto ItemStruct =
            FindStruct("SupplyDropItemDeliverySpawnData");
        if (!EntryStruct || !ZoneStruct || !ItemStruct)
            return false;

        FSupplyDropRuntimeLayout Candidate{};
        Candidate.EntrySize = EntryStruct->GetPropertiesSize();
        Candidate.ZoneDataOffset =
            (int32)EntryStruct->GetOffset("ZoneBasedData");
        Candidate.ZoneDataSize = ZoneStruct->GetPropertiesSize();
        Candidate.DropsRemainingOffset = (int32)
            ZoneStruct->GetOffset(
                "NumDropsRemainingInWave", 0x80);
        Candidate.NextWaveTimeOffset = (int32)
            ZoneStruct->GetOffset("NextWaveSpawnTime", 0x100);
        Candidate.ZoneNextSpawnTimeOffset = (int32)
            ZoneStruct->GetOffset("NextSpawnTime", 0x100);
        Candidate.ItemDataOffset =
            (int32)EntryStruct->GetOffset("ItemDeliveryData");
        Candidate.ItemDataSize = ItemStruct->GetPropertiesSize();
        Candidate.ItemsToDeliverOffset = (int32)
            ItemStruct->GetOffset("NumItemsToDeliver", 0x80);
        Candidate.QueuedTimesOffset =
            (int32)ItemStruct->GetOffset("QueuedSpawnTimes");
        Candidate.ItemNextSpawnTimeOffset = (int32)
            ItemStruct->GetOffset("NextSpawnTime", 0x100);
        Candidate.InitialSpawnsOffset = (int32)
            ItemStruct->GetOffset("NumInitialSpawns", 0x80);

        if (Candidate.EntrySize <= 0 ||
            Candidate.EntrySize > 0x400 ||
            Candidate.ZoneDataSize <= 0 ||
            Candidate.ZoneDataSize > 0x200 ||
            Candidate.ItemDataSize <= 0 ||
            Candidate.ItemDataSize > 0x200 ||
            !IsOffsetWithinStruct(
                Candidate.ZoneDataOffset,
                Candidate.ZoneDataSize,
                Candidate.EntrySize) ||
            !IsOffsetWithinStruct(
                Candidate.DropsRemainingOffset,
                sizeof(int32),
                Candidate.ZoneDataSize) ||
            !IsOffsetWithinStruct(
                Candidate.NextWaveTimeOffset,
                sizeof(float),
                Candidate.ZoneDataSize) ||
            !IsOffsetWithinStruct(
                Candidate.ZoneNextSpawnTimeOffset,
                sizeof(float),
                Candidate.ZoneDataSize) ||
            !IsOffsetWithinStruct(
                Candidate.ItemDataOffset,
                Candidate.ItemDataSize,
                Candidate.EntrySize) ||
            !IsOffsetWithinStruct(
                Candidate.ItemsToDeliverOffset,
                sizeof(int32),
                Candidate.ItemDataSize) ||
            !IsOffsetWithinStruct(
                Candidate.QueuedTimesOffset,
                sizeof(FSupplyDropSpawnDataArrayHeader),
                Candidate.ItemDataSize) ||
            !IsOffsetWithinStruct(
                Candidate.ItemNextSpawnTimeOffset,
                sizeof(float),
                Candidate.ItemDataSize) ||
            !IsOffsetWithinStruct(
                Candidate.InitialSpawnsOffset,
                sizeof(int32),
                Candidate.ItemDataSize))
        {
            return false;
        }

        Candidate.bValid = true;
        Layout = Candidate;
        return true;
    }

    void RestoreSupplyDropSuppressionState()
    {
        auto& State = GSupplyDropSuppressionState;

        // Runtime schedules and mutator actors are world-owned and suppression
        // is match-locked. Re-enabling either can release overdue drops, so only
        // restore the shared playlist asset when the tracked world changes.
        auto Playlist = State.Playlist.Get();
        if (IsSaneObject(Playlist) &&
            State.bHasPlaylistSupplyDropFlag &&
            Playlist->HasbUseDefaultSupplyDrops() &&
            !Playlist->bUseDefaultSupplyDrops)
        {
            Playlist->bUseDefaultSupplyDrops =
                State.bOriginalUseDefaultSupplyDrops;
        }

        if (State.bApplied)
        {
            SDK::DbgLog(
                "[SupplyDrops] suppression state released\n");
        }

        State = FSupplyDropSuppressionState{};
    }

    template <typename TOwner>
    bool SuppressSupplyDropRuntimeArray(
        TOwner* Owner, int32& SuppressedEntryCount)
    {
        if (!IsSaneObject(Owner) ||
            !Owner->HasSupplyDropSpawnDataList() ||
            !ResolveSupplyDropRuntimeLayout())
        {
            return false;
        }

        auto& Header = Owner->GetSupplyDropSpawnDataList();
        if (!IsSaneSupplyDropArrayHeader(Header))
            return false;

        const auto& Layout = GSupplyDropRuntimeLayout;
        const size_t EntryBytes =
            (size_t)Header.NumElements *
            (size_t)Layout.EntrySize;
        if (Header.NumElements > 0 &&
            (!Header.Data ||
                !SDK::MemReadable(Header.Data, EntryBytes)))
        {
            return false;
        }

        constexpr float NeverSpawn =
            (std::numeric_limits<float>::max)();
        for (int32 Index = 0; Index < Header.NumElements; ++Index)
        {
            auto Entry =
                (uint8*)Header.Data +
                (size_t)Index * (size_t)Layout.EntrySize;
            auto ZoneData = Entry + Layout.ZoneDataOffset;
            GetFromOffset<int32>(
                ZoneData,
                Layout.DropsRemainingOffset) = 0;
            GetFromOffset<float>(
                ZoneData,
                Layout.NextWaveTimeOffset) = NeverSpawn;
            GetFromOffset<float>(
                ZoneData,
                Layout.ZoneNextSpawnTimeOffset) = NeverSpawn;

            auto ItemData = Entry + Layout.ItemDataOffset;
            GetFromOffset<int32>(
                ItemData,
                Layout.ItemsToDeliverOffset) = 0;
            GetFromOffset<float>(
                ItemData,
                Layout.ItemNextSpawnTimeOffset) = NeverSpawn;
            GetFromOffset<int32>(
                ItemData,
                Layout.InitialSpawnsOffset) = 0;

            auto& QueuedTimes =
                GetFromOffset<FSupplyDropSpawnDataArrayHeader>(
                    ItemData,
                    Layout.QueuedTimesOffset);
            if (!IsSaneSupplyDropArrayHeader(QueuedTimes))
                continue;

            const size_t QueuedTimeBytes =
                (size_t)QueuedTimes.NumElements * sizeof(float);
            if (QueuedTimes.NumElements > 0 &&
                (!QueuedTimes.Data ||
                    !SDK::MemReadable(
                        QueuedTimes.Data,
                        QueuedTimeBytes)))
            {
                continue;
            }

            auto Times = (float*)QueuedTimes.Data;
            for (int32 TimeIndex = 0;
                TimeIndex < QueuedTimes.NumElements;
                ++TimeIndex)
            {
                Times[TimeIndex] = NeverSpawn;
            }
        }

        SuppressedEntryCount += Header.NumElements;
        return true;
    }

    UFortPlaylistAthena* ResolveSupplyDropPlaylist(UWorld* World)
    {
        if (World && IsSaneObject(World->GameState))
        {
            auto GameState =
                static_cast<AFortGameStateAthena*>(World->GameState);
            auto CurrentPlaylist =
                const_cast<UFortPlaylistAthena*>(
                    GetPublishedPlaylist(GameState));
            if (IsSaneObject(CurrentPlaylist))
                return CurrentPlaylist;
        }

        auto& State = GSupplyDropSuppressionState;
        if (auto CachedPlaylist = State.Playlist.Get())
        {
            if (IsSaneObject(CachedPlaylist))
                return CachedPlaylist;

            State.Playlist = {};
            State.bHasPlaylistSupplyDropFlag = false;
        }

        const ULONGLONG CurrentTimeMs = GetTickCount64();
        if (CurrentTimeMs < State.NextPlaylistResolveTimeMs)
            return nullptr;
        State.NextPlaylistResolveTimeMs = CurrentTimeMs + 1000ULL;

        auto Playlist = const_cast<UFortPlaylistAthena*>(
            FindObject<UFortPlaylistAthena>(
                FConfiguration::Playlist));
        if (!Playlist)
        {
            Playlist = const_cast<UFortPlaylistAthena*>(
                FindObject<UFortPlaylistAthena>(
                    L"/Game/Athena/Playlists/"
                    L"Playlist_DefaultSolo.Playlist_DefaultSolo"));
        }
        return Playlist;
    }

    bool IsNativeSupplyDeliveryPlaylist(
        const UFortPlaylistAthena* Playlist)
    {
        if (!FFortAthenaNativeLTMCompatibility::
                IsTargetPlaylist(Playlist))
        {
            return false;
        }

        // IsTargetPlaylist has already verified exact 10.40 canonical asset
        // identity. Only Getaway and Food Fight depend on their authored
        // item-delivery data here; Arsenal intentionally remains suppressible.
        const auto ObjectName = Playlist->Name.ToWString();
        return ObjectName == L"Playlist_Bling_Solo" ||
            ObjectName == L"Playlist_Bling_Duos" ||
            ObjectName == L"Playlist_Bling_Squads" ||
            ObjectName == L"Playlist_Barrier" ||
            ObjectName == L"Playlist_Barrier_16_B_Lava";
    }

    const UFortPlaylistAthena*
        ResolveNativeSupplyDeliveryPlaylist(
            UWorld* World,
            const UFortPlaylistAthena* ResolvedPlaylist)
    {
        // Prefer either published playlist slot. During native 10.40
        // publication one can become valid a frame before the other, and the
        // generic resolver otherwise favors BasePlaylist exclusively.
        if (World && IsSaneObject(World->GameState))
        {
            auto GameState =
                static_cast<AFortGameStateAthena*>(World->GameState);
            if (GameState->HasCurrentPlaylistInfo() &&
                FPlaylistPropertyArray::StaticStruct())
            {
                if (FPlaylistPropertyArray::HasOverridePlaylist())
                {
                    auto OverridePlaylist =
                        GameState->CurrentPlaylistInfo.OverridePlaylist;
                    if (IsNativeSupplyDeliveryPlaylist(
                            OverridePlaylist))
                    {
                        return OverridePlaylist;
                    }
                }

                if (FPlaylistPropertyArray::HasBasePlaylist())
                {
                    auto BasePlaylist =
                        GameState->CurrentPlaylistInfo.BasePlaylist;
                    if (IsNativeSupplyDeliveryPlaylist(BasePlaylist))
                        return BasePlaylist;
                }
            }

            if (GameState->HasCurrentPlaylistData())
            {
                auto CurrentPlaylist =
                    GameState->CurrentPlaylistData;
                if (IsNativeSupplyDeliveryPlaylist(CurrentPlaylist))
                    return CurrentPlaylist;
            }
        }

        return IsNativeSupplyDeliveryPlaylist(ResolvedPlaylist)
            ? ResolvedPlaylist
            : nullptr;
    }

    bool IsOwnedByCurrentGameState(
        const UObject* Object, UWorld* World)
    {
        if (!Object || !World || !World->GameState)
            return false;

        auto Outer = Object->Outer;
        for (int32 Depth = 0; Outer && Depth < 16; ++Depth)
        {
            if (Outer == World->GameState)
                return true;
            Outer = Outer->Outer;
        }

        return false;
    }

    bool IsOwnedByWorld(const UObject* Object, UWorld* World)
    {
        if (!Object || !World)
            return false;

        auto Outer = Object;
        for (int32 Depth = 0; Outer && Depth < 32; ++Depth)
        {
            if (Outer == World)
                return true;
            Outer = Outer->Outer;
        }

        return false;
    }

    UFortGameStateComponent_BattleRoyaleGamePhaseLogic*
        ResolveCurrentGamePhaseLogic(
            UWorld* World, bool bForceDiscovery)
    {
        // The component-owned phase runtime is a modern Athena path. Legacy
        // builds such as FN 10.40 keep the supply-drop cache on GameMode and/or
        // an active mutator; walking every live UObject once per second can
        // never find this component there and stalls the same game thread that
        // processes lethal damage and winner replication.
        if (VersionInfo.FortniteVersion < 25.20)
            return nullptr;

        auto& State = GSupplyDropSuppressionState;
        auto Cached = State.GamePhaseLogic.Get();
        if (IsSaneObject(Cached) &&
            IsOwnedByCurrentGameState(Cached, World))
            return Cached;

        State.GamePhaseLogic = {};
        const ULONGLONG CurrentTimeMs = GetTickCount64();
        if (!bForceDiscovery && CurrentTimeMs <
            State.NextGamePhaseLogicResolveTimeMs)
        {
            return nullptr;
        }
        State.NextGamePhaseLogicResolveTimeMs =
            CurrentTimeMs + 1000ULL;

        const UClass* ComponentClass =
            FindClass(
                "FortGameStateComponent_BattleRoyaleGamePhaseLogic");
        auto GameState = World && IsSaneObject(World->GameState)
            ? static_cast<AFortGameStateAthena*>(World->GameState)
            : nullptr;
        if (!ComponentClass || !GameState)
            return nullptr;

        auto Component =
            UFortGameStateComponent_BattleRoyaleGamePhaseLogic::
                Get(World);
        if ((!IsSaneObject(Component) ||
                !IsOwnedByCurrentGameState(Component, World)) &&
            IsSaneObject(GameState))
        {
            Component = reinterpret_cast<
                UFortGameStateComponent_BattleRoyaleGamePhaseLogic*>(
                    GameState->GetComponentByClass(ComponentClass));
        }
        if (IsSaneObject(Component) &&
            !Component->IsDefaultObject() &&
            IsOwnedByCurrentGameState(Component, World))
        {
            State.GamePhaseLogic = TWeakObjectPtr<
                UFortGameStateComponent_BattleRoyaleGamePhaseLogic>(
                    Component);
            return Component;
        }

        return nullptr;
    }

    struct FSupplyDropMutatorLayout
    {
        int32 MutatorDataSize = 0;
        int32 ItemDeliveryArrayOffset = -1;
        int32 ItemDeliveryDataSize = 0;
        int32 ShouldApplyOffset = -1;
        ULONGLONG NextResolveTimeMs = 0;
        bool bValid = false;
    };

    FSupplyDropMutatorLayout GSupplyDropMutatorLayout{};

    bool ResolveSupplyDropMutatorLayout()
    {
        auto& Layout = GSupplyDropMutatorLayout;
        if (Layout.bValid)
            return true;

        const ULONGLONG CurrentTimeMs = GetTickCount64();
        if (CurrentTimeMs < Layout.NextResolveTimeMs)
            return false;
        Layout.NextResolveTimeMs = CurrentTimeMs + 1000ULL;

        auto MutatorDataStruct =
            FindStruct("FortSupplyDropMutatorData");
        auto ItemDeliveryDataStruct =
            FindStruct("FortItemDeliverySupplyDropMutatorData");
        if (!MutatorDataStruct || !ItemDeliveryDataStruct)
            return false;

        const int32 MutatorDataSize =
            MutatorDataStruct->GetPropertiesSize();
        const int32 ItemDeliveryDataSize =
            ItemDeliveryDataStruct->GetPropertiesSize();
        const int32 ItemDeliveryArrayOffset = (int32)
            MutatorDataStruct->GetOffset(
                "ItemDeliveryMutatorPerSafeZonePhase");
        const int32 ShouldApplyOffset = (int32)
            ItemDeliveryDataStruct->GetOffset(
                "bShouldApplyMutator", 0x20000);

        if (MutatorDataSize <= 0 || MutatorDataSize > 0x400 ||
            ItemDeliveryDataSize <= 0 ||
            ItemDeliveryDataSize > 0x400 ||
            !IsOffsetWithinStruct(
                ItemDeliveryArrayOffset,
                sizeof(FSupplyDropSpawnDataArrayHeader),
                MutatorDataSize) ||
            !IsOffsetWithinStruct(
                ShouldApplyOffset,
                sizeof(bool),
                ItemDeliveryDataSize))
        {
            return false;
        }

        Layout.MutatorDataSize = MutatorDataSize;
        Layout.ItemDeliveryArrayOffset =
            ItemDeliveryArrayOffset;
        Layout.ItemDeliveryDataSize =
            ItemDeliveryDataSize;
        Layout.ShouldApplyOffset = ShouldApplyOffset;
        Layout.bValid = true;
        return true;
    }

    AFortAthenaMutator_SupplyDrop*
        ResolveCurrentSupplyDropMutator(
            UWorld* World, bool bForceDiscovery)
    {
        auto& State = GSupplyDropSuppressionState;
        auto Cached = State.SupplyDropMutator.Get();
        if (IsSaneObject(Cached) && IsOwnedByWorld(Cached, World))
            return Cached;

        State.SupplyDropMutator = {};
        const ULONGLONG CurrentTimeMs = GetTickCount64();
        if (!bForceDiscovery && CurrentTimeMs <
            State.NextSupplyDropMutatorResolveTimeMs)
        {
            return nullptr;
        }
        State.NextSupplyDropMutatorResolveTimeMs =
            CurrentTimeMs + 15000ULL;

        const UClass* MutatorClass =
            FindClass("FortAthenaMutator_SupplyDrop");
        if (!MutatorClass)
            return nullptr;

        bool bInspectedAuthoritativeMutatorList = false;
        if (World && IsSaneObject(World->GameState))
        {
            auto GameState =
                static_cast<AFortGameStateAthena*>(World->GameState);
            if (GameState->HasGameplayMutators())
            {
                bInspectedAuthoritativeMutatorList = true;
                auto& ActiveMutators = GameState->GetGameplayMutators();
                const auto& ActiveMutatorHeader =
                    reinterpret_cast<
                        const FSupplyDropSpawnDataArrayHeader&>(
                            ActiveMutators);
                const size_t ActiveMutatorBytes =
                    (size_t)ActiveMutatorHeader.NumElements *
                    sizeof(AFortAthenaMutator*);
                if (IsSaneSupplyDropArrayHeader(
                        ActiveMutatorHeader) &&
                    (ActiveMutatorHeader.NumElements == 0 ||
                        SDK::MemReadable(
                            ActiveMutatorHeader.Data,
                            ActiveMutatorBytes)))
                {
                    for (int32 Index = 0;
                        Index < ActiveMutators.Num();
                        ++Index)
                    {
                        auto Mutator = ActiveMutators[Index];
                        if (!IsSaneObject(Mutator) ||
                            !Mutator->IsA(MutatorClass) ||
                            !IsOwnedByWorld(Mutator, World))
                        {
                            continue;
                        }

                        auto SupplyDropMutator = reinterpret_cast<
                            AFortAthenaMutator_SupplyDrop*>(Mutator);
                        State.SupplyDropMutator =
                            TWeakObjectPtr<
                                AFortAthenaMutator_SupplyDrop>(
                                SupplyDropMutator);
                        return SupplyDropMutator;
                    }
                }
            }
        }

        // Registered gameplay mutators are authoritative once that reflected
        // list exists. A loaded-but-unregistered UObject cannot schedule a
        // supply drop, so a normal live tick must not fall through to a global
        // registry scan. Forced discovery at startup/phase transitions retains
        // the fallback for builds whose list is populated unusually late.
        if (bInspectedAuthoritativeMutatorList && !bForceDiscovery)
            return nullptr;

        for (int32 Index = 0; Index < TUObjectArray::Num(); ++Index)
        {
            auto Object = GetLiveObjectByIndex(Index);
            if (!Object || Object->IsDefaultObject() ||
                !Object->IsA(MutatorClass) ||
                !IsSaneObject(Object) ||
                !IsOwnedByWorld(Object, World))
            {
                continue;
            }

            auto Mutator = reinterpret_cast<
                AFortAthenaMutator_SupplyDrop*>(Object);
            State.SupplyDropMutator =
                TWeakObjectPtr<AFortAthenaMutator_SupplyDrop>(
                    Mutator);
            return Mutator;
        }

        return nullptr;
    }

    bool SuppressSupplyDropMutator(
        AFortAthenaMutator_SupplyDrop* Mutator,
        int32& SuppressedPhaseCount)
    {
        if (!IsSaneObject(Mutator) ||
            !Mutator->HasSafeZoneMutatorData() ||
            !ResolveSupplyDropMutatorLayout())
        {
            return false;
        }

        auto& Layout = GSupplyDropMutatorLayout;
        auto& MutatorData = Mutator->GetSafeZoneMutatorData();
        if (!IsSaneSupplyDropArrayHeader(MutatorData))
            return false;

        const size_t MutatorDataBytes =
            (size_t)MutatorData.NumElements *
            (size_t)Layout.MutatorDataSize;
        if (MutatorData.NumElements > 0 &&
            (!MutatorData.Data ||
                !SDK::MemReadable(
                    MutatorData.Data,
                    MutatorDataBytes)))
        {
            return false;
        }

        for (int32 MutatorIndex = 0;
            MutatorIndex < MutatorData.NumElements;
            ++MutatorIndex)
        {
            auto MutatorEntry =
                (uint8*)MutatorData.Data +
                (size_t)MutatorIndex *
                    (size_t)Layout.MutatorDataSize;
            auto& DeliveryPhases =
                GetFromOffset<FSupplyDropSpawnDataArrayHeader>(
                    MutatorEntry,
                    Layout.ItemDeliveryArrayOffset);
            if (!IsSaneSupplyDropArrayHeader(DeliveryPhases))
                continue;

            const size_t DeliveryPhaseBytes =
                (size_t)DeliveryPhases.NumElements *
                (size_t)Layout.ItemDeliveryDataSize;
            if (DeliveryPhases.NumElements > 0 &&
                (!DeliveryPhases.Data ||
                    !SDK::MemReadable(
                        DeliveryPhases.Data,
                        DeliveryPhaseBytes)))
            {
                continue;
            }

            for (int32 PhaseIndex = 0;
                PhaseIndex < DeliveryPhases.NumElements;
                ++PhaseIndex)
            {
                auto PhaseEntry =
                    (uint8*)DeliveryPhases.Data +
                    (size_t)PhaseIndex *
                        (size_t)Layout.ItemDeliveryDataSize;
                auto& bShouldApply =
                    GetFromOffset<bool>(
                        PhaseEntry,
                        Layout.ShouldApplyOffset);
                if (bShouldApply)
                {
                    bShouldApply = false;
                    SuppressedPhaseCount++;
                }
            }
        }

        return true;
    }

    void ApplySupplyDropSuppression(
        UWorld* World, bool bForceDiscovery)
    {
        if (!World)
        {
            if (!GSupplyDropSuppressionState.bLockedForMatch)
                RestoreSupplyDropSuppressionState();
            return;
        }

        auto& State = GSupplyDropSuppressionState;
        if (State.World.Get() != World)
        {
            RestoreSupplyDropSuppressionState();
            State.World = TWeakObjectPtr<UWorld>(World);
        }

        auto Playlist = ResolveSupplyDropPlaylist(World);
        auto NativeDeliveryPlaylist =
            ResolveNativeSupplyDeliveryPlaylist(World, Playlist);
        if (NativeDeliveryPlaylist)
        {
            if (FConfiguration::bDisableSupplyDrops &&
                FConfiguration::bReadyToStart)
            {
                State.bLockedForMatch = true;
            }

            const bool bShouldSuppress =
                FConfiguration::bDisableSupplyDrops ||
                State.bLockedForMatch;
            if (!bShouldSuppress)
            {
                if (State.bApplied)
                {
                    RestoreSupplyDropSuppressionState();
                    State.World = TWeakObjectPtr<UWorld>(World);
                }
                return;
            }

            // The runtime arrays also contain Getaway escape-van delivery and
            // Food Fight objective delivery. Honor the user's toggle by
            // disabling only the playlist's generic/default supply drops and
            // leave those native item-delivery schedules intact.
            const bool bHasStaleRuntimeSuppression =
                State.GamePhaseLogic.Get() ||
                State.SupplyDropMutator.Get();
            const bool bTracksDifferentPlaylist =
                State.Playlist.Get() &&
                State.Playlist.Get() != NativeDeliveryPlaylist;
            if (bHasStaleRuntimeSuppression ||
                bTracksDifferentPlaylist)
            {
                const bool bLockedForMatch =
                    State.bLockedForMatch;
                RestoreSupplyDropSuppressionState();
                State.World = TWeakObjectPtr<UWorld>(World);
                State.bLockedForMatch = bLockedForMatch;
            }

            if (State.Playlist.Get() != NativeDeliveryPlaylist)
            {
                State.Playlist =
                    TWeakObjectPtr<UFortPlaylistAthena>(
                        const_cast<UFortPlaylistAthena*>(
                            NativeDeliveryPlaylist));
                State.bHasPlaylistSupplyDropFlag =
                    NativeDeliveryPlaylist
                        ->HasbUseDefaultSupplyDrops();
                if (State.bHasPlaylistSupplyDropFlag)
                {
                    State.bOriginalUseDefaultSupplyDrops =
                        NativeDeliveryPlaylist
                            ->bUseDefaultSupplyDrops;
                }
            }

            if (State.bHasPlaylistSupplyDropFlag)
            {
                const_cast<UFortPlaylistAthena*>(
                    NativeDeliveryPlaylist)
                    ->bUseDefaultSupplyDrops = false;
            }

            State.bApplied =
                State.bHasPlaylistSupplyDropFlag;
            if (!State.bLoggedNativeDeliveryPreserved)
            {
                SDK::DbgLog(
                    "[SupplyDrops] default drops suppressed; "
                    "native item delivery preserved playlist=%s "
                    "playlistFlag=%d\n",
                    NativeDeliveryPlaylist->Name
                        .ToString().c_str(),
                    (int)State.bApplied);
                State.bLoggedNativeDeliveryPreserved = true;
            }
            return;
        }

        if (FConfiguration::bDisableSupplyDrops &&
            FConfiguration::bReadyToStart)
        {
            State.bLockedForMatch = true;
        }

        const bool bShouldSuppress =
            FConfiguration::bDisableSupplyDrops ||
            State.bLockedForMatch;
        if (!bShouldSuppress)
        {
            if (State.bApplied)
            {
                RestoreSupplyDropSuppressionState();
                State.World = TWeakObjectPtr<UWorld>(World);
            }
            return;
        }

        int32 SuppressedEntryCount = 0;
        bool bSuppressedPlaylist = false;
        if (Playlist)
        {
            if (State.Playlist.Get() != Playlist)
            {
                auto PreviousPlaylist = State.Playlist.Get();
                if (IsSaneObject(PreviousPlaylist) &&
                    State.bHasPlaylistSupplyDropFlag &&
                    PreviousPlaylist->HasbUseDefaultSupplyDrops() &&
                    !PreviousPlaylist->bUseDefaultSupplyDrops)
                {
                    PreviousPlaylist->bUseDefaultSupplyDrops =
                        State.bOriginalUseDefaultSupplyDrops;
                }

                State.Playlist =
                    TWeakObjectPtr<UFortPlaylistAthena>(Playlist);
                State.bHasPlaylistSupplyDropFlag =
                    Playlist->HasbUseDefaultSupplyDrops();
                if (State.bHasPlaylistSupplyDropFlag)
                {
                    State.bOriginalUseDefaultSupplyDrops =
                        Playlist->bUseDefaultSupplyDrops;
                }
            }

            if (State.bHasPlaylistSupplyDropFlag)
            {
                Playlist->bUseDefaultSupplyDrops = false;
                bSuppressedPlaylist = true;
            }
        }

        int32 SuppressedMutatorPhaseCount = 0;
        bool bSuppressedGameModeCache = false;
        bool bSuppressedComponentCache = false;
        bool bSuppressedMutator = false;
        if (State.bLockedForMatch)
        {
            auto GameMode = World->AuthorityGameMode
                ? (AFortGameMode*)World->AuthorityGameMode
                : nullptr;
            bSuppressedGameModeCache =
                SuppressSupplyDropRuntimeArray(
                    GameMode, SuppressedEntryCount);

            auto GamePhaseLogic =
                ResolveCurrentGamePhaseLogic(
                    World, bForceDiscovery);
            bSuppressedComponentCache =
                SuppressSupplyDropRuntimeArray(
                    GamePhaseLogic, SuppressedEntryCount);

            bSuppressedMutator =
                SuppressSupplyDropMutator(
                    ResolveCurrentSupplyDropMutator(
                        World, bForceDiscovery),
                    SuppressedMutatorPhaseCount);
        }

        State.bApplied =
            bSuppressedPlaylist ||
            bSuppressedGameModeCache ||
            bSuppressedComponentCache ||
            bSuppressedMutator;
        if (!State.bLoggedEnabled && State.bApplied)
        {
            SDK::DbgLog(
                "[SupplyDrops] suppression enabled playlist=%d "
                "gameModeCache=%d componentCache=%d mutator=%d "
                "entries=%d phases=%d\n",
                (int)bSuppressedPlaylist,
                (int)bSuppressedGameModeCache,
                (int)bSuppressedComponentCache,
                (int)bSuppressedMutator,
                SuppressedEntryCount,
                SuppressedMutatorPhaseCount);
            State.bLoggedEnabled = true;
        }
    }

    struct FDeferredVehicleSpawnState
    {
        TWeakObjectPtr<UWorld> World;
        TWeakObjectPtr<UClass> ProviderClass;
        TWeakObjectPtr<UClass> VehicleSpawnerClass;
        std::vector<TWeakObjectPtr<UObject>> ProcessedSources;
        std::vector<TWeakObjectPtr<
            AFortAthenaLivingWorldStaticPointProvider>>
                CachedProviders;
        std::vector<TWeakObjectPtr<AFortAthenaVehicleSpawner>>
            CachedVehicleSpawners;
        ULONGLONG NextAttemptTimeMs = 0;
        ULONGLONG DeadlineTimeMs = 0;
        ULONGLONG NextProviderRefreshTimeMs = 0;
        ULONGLONG NextSpawnerRefreshTimeMs = 0;
        uint32 PassCount = 0;
        int32 TotalSpawned = 0;
        int32 ProviderCursor = 0;
        int32 VehicleSpawnerCursor = 0;
        bool Started = false;
        bool Active = false;
    };

    FDeferredVehicleSpawnState GDeferredVehicleSpawnState{};

    constexpr ULONGLONG VehicleSpawnRetryIntervalMs = 2000ULL;
    constexpr ULONGLONG VehicleSpawnRetryWindowMs = 45000ULL;
    constexpr ULONGLONG VehicleInitialDiscoveryDelayMs = 12000ULL;
    constexpr ULONGLONG VehicleSourceRefreshIntervalMs = 15000ULL;
    constexpr int32 VehicleSpawnBudgetPerPass = 8;
    constexpr ULONGLONG VehicleClassLoadRetryIntervalMs = 15000ULL;
    constexpr float LivingWorldVehicleSpawnProbability = 0.35f;

    struct FVehicleProviderClassCacheEntry
    {
        const wchar_t* GameplayTagName;
        const wchar_t* ObjectPath;
        int32 SelectionPriority = 10;
        FName GameplayTag{};
        TWeakObjectPtr<UClass> VehicleClass;
        ULONGLONG NextLoadAttemptTimeMs = 0;
        uint32 LastResolveEpoch = 0;
        bool TagInitialized = false;
    };

    FVehicleProviderClassCacheEntry GVehicleProviderClassCache[] =
    {
        {
            L"Athena.Vehicle.SpawnLocation.Motorcycle.Dirtbike",
            L"/Dirtbike/Vehicle/Motorcycle_DirtBike_Vehicle."
                L"Motorcycle_DirtBike_Vehicle_C",
            100
        },
        {
            L"Athena.Vehicle.SpawnLocation.Motorcycle.Sportbike",
            L"/Sportbike/Vehicle/Motorcycle_Sport_Vehicle."
                L"Motorcycle_Sport_Vehicle_C",
            100
        },
        {
            L"Athena.Vehicle.SpawnLocation.Valet.BasicCar.Base",
            L"/Valet/BasicCar/Valet_BasicCar_Vehicle."
                L"Valet_BasicCar_Vehicle_C",
            100
        },
        {
            L"Athena.Vehicle.SpawnLocation.Valet.BasicCar",
            L"/Valet/BasicCar/Valet_BasicCar_Vehicle."
                L"Valet_BasicCar_Vehicle_C",
            100
        },
        {
            L"Athena.Vehicle.SpawnLocation.Valet.BasicCar.Taxi",
            L"/Valet/TaxiCab/Valet_TaxiCab_Vehicle."
                L"Valet_TaxiCab_Vehicle_C",
            50
        },
        {
            L"Athena.Vehicle.SpawnLocation.Valet.BasicCar.Modded",
            L"/ModdedBasicCar/Vehicle/"
                L"Valet_BasicCar_Vehicle_SuperSedan."
                L"Valet_BasicCar_Vehicle_SuperSedan_C",
            50
        },
        {
            L"Athena.Vehicle.SpawnLocation.Valet.BasicSUV",
            L"/BasicSUV/Vehicle/Valet_BasicSUV_Vehicle."
                L"Valet_BasicSUV_Vehicle_C",
            100
        },
        {
            L"Athena.Vehicle.SpawnLocation.Valet.BasicTruck.Base",
            L"/Valet/BasicTruck/Valet_BasicTruck_Vehicle."
                L"Valet_BasicTruck_Vehicle_C",
            100
        },
        {
            L"Athena.Vehicle.SpawnLocation.Valet.BasicTruck",
            L"/Valet/BasicTruck/Valet_BasicTruck_Vehicle."
                L"Valet_BasicTruck_Vehicle_C",
            100
        },
        {
            L"Athena.Vehicle.SpawnLocation.Valet."
                L"BasicTruck.Upgraded",
            L"/Valet/BasicTruck/"
                L"Valet_BasicTruck_Vehicle_Upgrade."
                L"Valet_BasicTruck_Vehicle_Upgrade_C",
            10
        },
        {
            L"Athena.Vehicle.SpawnLocation.Valet.BigRig.Base",
            L"/Valet/BigRig/Valet_BigRig_Vehicle."
                L"Valet_BigRig_Vehicle_C",
            100
        },
        {
            L"Athena.Vehicle.SpawnLocation.Valet.BigRig",
            L"/Valet/BigRig/Valet_BigRig_Vehicle."
                L"Valet_BigRig_Vehicle_C",
            100
        },
        {
            L"Athena.Vehicle.SpawnLocation.Valet.BigRig.Upgraded",
            L"/Valet/BigRig/Valet_BigRig_Vehicle_Upgrade."
                L"Valet_BigRig_Vehicle_Upgrade_C",
            10
        },
        {
            L"Athena.Vehicle.SpawnLocation.Valet.SportsCar.Base",
            L"/Valet/SportsCar/Valet_SportsCar_Vehicle."
                L"Valet_SportsCar_Vehicle_C",
            100
        },
        {
            L"Athena.Vehicle.SpawnLocation.Valet.SportsCar",
            L"/Valet/SportsCar/Valet_SportsCar_Vehicle."
                L"Valet_SportsCar_Vehicle_C",
            100
        },
        {
            L"Athena.Vehicle.SpawnLocation.Valet."
                L"SportsCar.Upgraded",
            L"/Valet/SportsCar/"
                L"Valet_SportsCar_Vehicle_Upgrade."
                L"Valet_SportsCar_Vehicle_Upgrade_C",
            10
        },
        {
            L"Athena.Vehicle.SpawnLocation.Valet.BasicCar.Upgraded",
            L"/Valet/BasicCar/"
                L"Valet_BasicCar_Vehicle_Upgrade."
                L"Valet_BasicCar_Vehicle_Upgrade_C",
            10
        }
    };
    uint32 GVehicleProviderResolveEpoch = 0;

    bool UsesDeferredVehicleSpawnDiscovery()
    {
        return VersionInfo.FortniteVersion >= 30.00 &&
            VersionInfo.FortniteVersion < 31.00;
    }

    bool IsVehicleSpawnSourceProcessed(const UObject* Source)
    {
        if (!Source)
            return true;

        for (const auto& Processed :
            GDeferredVehicleSpawnState.ProcessedSources)
        {
            if (Processed.Get() == Source)
                return true;
        }
        return false;
    }

    void MarkVehicleSpawnSourceProcessed(UObject* Source)
    {
        if (!Source || IsVehicleSpawnSourceProcessed(Source))
            return;

        GDeferredVehicleSpawnState.ProcessedSources.emplace_back(
            TWeakObjectPtr<UObject>(Source));
    }

    bool ResolveVehicleProviderClass(
        const FName& GameplayTag,
        uint32 ResolveEpoch,
        const UClass*& OutVehicleClass,
        int32& OutSelectionPriority)
    {
        OutVehicleClass = nullptr;
        OutSelectionPriority = 0;
        for (auto& Entry : GVehicleProviderClassCache)
        {
            if (!Entry.TagInitialized)
            {
                Entry.GameplayTag =
                    FName(Entry.GameplayTagName);
                Entry.TagInitialized = true;
            }
            if (Entry.GameplayTag != GameplayTag)
                continue;

            OutSelectionPriority = Entry.SelectionPriority;
            if (auto* CachedClass = Entry.VehicleClass.Get())
            {
                OutVehicleClass = CachedClass;
                return true;
            }
            if (Entry.LastResolveEpoch == ResolveEpoch)
                return true;

            Entry.LastResolveEpoch = ResolveEpoch;
            const ULONGLONG CurrentTimeMs = GetTickCount64();
            auto* FoundClass = static_cast<const UClass*>(
                SDK::StaticFindObject(
                    Entry.ObjectPath,
                    UClass::StaticClass()));
            if (!FoundClass &&
                CurrentTimeMs >=
                    Entry.NextLoadAttemptTimeMs)
            {
                // Only a tag that actually exists in this world may trigger
                // a load, and a missing path is retried sparsely. Routine
                // two-second discovery passes remain lookup-only.
                FoundClass =
                    FindObject<UClass>(Entry.ObjectPath);
                Entry.NextLoadAttemptTimeMs =
                    CurrentTimeMs +
                        VehicleClassLoadRetryIntervalMs;
            }
            if (FoundClass)
            {
                Entry.VehicleClass =
                    TWeakObjectPtr<UClass>(
                        const_cast<UClass*>(FoundClass));
                OutVehicleClass = FoundClass;
            }
            return true;
        }
        return false;
    }

    int32 RunVehicleSpawnDiscoveryPass()
    {
        auto* World = UWorld::GetWorld();
        if (!World ||
            GDeferredVehicleSpawnState.World.Get() != World)
        {
            return 0;
        }

        int32 SpawnedThisPass = 0;
        int32 ProviderCount = 0;
        int32 MappedProviderCount = 0;
        int32 ProviderSpawnFailureCount = 0;
        int32 UnresolvedProviderCount = 0;
        int32 ProviderDensitySkippedCount = 0;
        int32 SpawnAttemptsThisPass = 0;
        const uint32 ResolveEpoch =
            ++GVehicleProviderResolveEpoch;
        const bool LimitSpawnWork =
            UsesDeferredVehicleSpawnDiscovery();
        const ULONGLONG CurrentTimeMs = GetTickCount64();

        // Resolve these dynamically on every bounded retry. SDK StaticClass
        // helpers permanently cache an early miss, while FN30 can stream the
        // provider class and its vehicle assets well after world init.
        const UClass* ProviderClass =
            GDeferredVehicleSpawnState.ProviderClass.Get();
        if (!ProviderClass)
        {
            ProviderClass =
                SDK::FindClass(
                    LimitSpawnWork
                        ? "FortAthenaLivingWorldVehiclePointProvider"
                        : "FortAthenaLivingWorldStaticPointProvider");
            if (ProviderClass)
            {
                GDeferredVehicleSpawnState.ProviderClass =
                    TWeakObjectPtr<UClass>(
                        const_cast<UClass*>(ProviderClass));
            }
        }
        if (ProviderClass)
        {
            if (CurrentTimeMs >= GDeferredVehicleSpawnState
                    .NextProviderRefreshTimeMs)
            {
                TArray<AFortAthenaLivingWorldStaticPointProvider*>
                    Providers{};
                Utils::GetAll(ProviderClass, Providers);
                GDeferredVehicleSpawnState.CachedProviders.clear();
                GDeferredVehicleSpawnState.CachedProviders.reserve(
                    Providers.Num());
                for (auto Provider : Providers)
                {
                    if (Provider)
                    {
                        GDeferredVehicleSpawnState.CachedProviders
                            .emplace_back(Provider);
                    }
                }
                Providers.Free();
                GDeferredVehicleSpawnState
                    .NextProviderRefreshTimeMs =
                        CurrentTimeMs +
                        VehicleSourceRefreshIntervalMs;
            }
            ProviderCount = static_cast<int32>(
                GDeferredVehicleSpawnState.CachedProviders.size());

            const int32 ProviderStartIndex =
                ProviderCount > 0
                    ? GDeferredVehicleSpawnState.ProviderCursor %
                        ProviderCount
                    : 0;
            constexpr int32 ReservedClassicSpawnAttempts = 4;
            const int32 ProviderAttemptLimit =
                VehicleSpawnBudgetPerPass -
                    ReservedClassicSpawnAttempts;
            for (int32 Visited = 0;
                Visited < ProviderCount;
                ++Visited)
            {
                if (LimitSpawnWork &&
                    SpawnAttemptsThisPass >=
                        ProviderAttemptLimit)
                {
                    break;
                }
                const int32 ProviderIndex =
                    (ProviderStartIndex + Visited) %
                        ProviderCount;
                auto* Provider =
                    GDeferredVehicleSpawnState.CachedProviders[
                        ProviderIndex].Get();
                if (!Provider ||
                    IsVehicleSpawnSourceProcessed(Provider))
                {
                    continue;
                }

                const UClass* VehicleClass = nullptr;
                int32 VehicleClassPriority = 0;
                int32 HighestRecognizedPriority = 0;
                bool ForceSpawn = false;
                bool RecognizedTag = false;
                static const FName AlwaysSpawnTag(
                    L"Athena.Vehicle.SpawnLocation.AlwaysSpawn");
                for (int32 TagIndex = 0;
                    TagIndex <
                        Provider->FiltersTags.GameplayTags.Num();
                    ++TagIndex)
                {
                    auto& Tag =
                        Provider->FiltersTags.GameplayTags.Get(
                            TagIndex,
                            FGameplayTag::Size());
                    if (Tag.TagName == AlwaysSpawnTag)
                        ForceSpawn = true;

                    const UClass* CandidateClass = nullptr;
                    int32 CandidatePriority = 0;
                    if (!ResolveVehicleProviderClass(
                            Tag.TagName,
                            ResolveEpoch,
                            CandidateClass,
                            CandidatePriority))
                    {
                        continue;
                    }

                    RecognizedTag = true;
                    if (CandidatePriority >
                        HighestRecognizedPriority)
                    {
                        HighestRecognizedPriority =
                            CandidatePriority;
                    }
                    if (CandidateClass &&
                        CandidatePriority >
                            VehicleClassPriority)
                    {
                        VehicleClass = CandidateClass;
                        VehicleClassPriority =
                            CandidatePriority;
                    }
                }

                // FN30's upgraded Lager event requires both the Base and
                // Upgraded location tags. The old first-match map omitted
                // Base entirely, which turned practically every compatible
                // point into the fixed red/blue Whiplash VR-1. Wait for and
                // prefer the ordinary gameplay class whenever a Base tag is
                // present.
                if (VehicleClassPriority <
                    HighestRecognizedPriority)
                {
                    VehicleClass = nullptr;
                }
                if (!RecognizedTag || !VehicleClass)
                {
                    ++UnresolvedProviderCount;
                    continue;
                }

                // These actors are candidate points consumed by Lager, not a
                // list of vehicles that all exist simultaneously. Since the
                // private server has to replace the unavailable Lager spawn
                // action, preserve AlwaysSpawn points and make one bounded,
                // one-shot density decision for every other point. This
                // avoids constructing and replicating all 220+ candidates
                // during the opening seconds of a match.
                if (!ForceSpawn &&
                    (rand() / static_cast<float>(RAND_MAX)) >
                        LivingWorldVehicleSpawnProbability)
                {
                    MarkVehicleSpawnSourceProcessed(Provider);
                    ++ProviderDensitySkippedCount;
                    continue;
                }

                ++MappedProviderCount;
                ++SpawnAttemptsThisPass;
                GDeferredVehicleSpawnState.ProviderCursor =
                    (ProviderIndex + 1) % ProviderCount;
                auto* Vehicle =
                    UWorld::SpawnActor<AFortAthenaVehicle>(
                        VehicleClass,
                        Provider->K2_GetActorLocation(),
                        Provider->K2_GetActorRotation());
                if (!Vehicle)
                {
                    ++ProviderSpawnFailureCount;
                    continue;
                }

                MarkVehicleSpawnSourceProcessed(Provider);
                FortVehicleMods::RegisterSpawnedVehicle(
                    Vehicle,
                    Provider);
                ++SpawnedThisPass;
            }
        }

        int32 SpawnerCount = 0;
        int32 MissingClassCount = 0;
        int32 ChanceSkippedCount = 0;
        int32 SpawnFailureCount = 0;
        const bool UsesClassicVehicleSpawners =
            VersionInfo.EngineVersion >= 4.23 &&
            std::floor(VersionInfo.FortniteVersion) != 20 &&
            std::floor(VersionInfo.FortniteVersion) != 21 &&
            std::floor(VersionInfo.FortniteVersion) != 22;
        const UClass* VehicleSpawnerClass =
            UsesClassicVehicleSpawners
                ? GDeferredVehicleSpawnState
                    .VehicleSpawnerClass.Get()
                : nullptr;
        if (UsesClassicVehicleSpawners &&
            !VehicleSpawnerClass)
        {
            VehicleSpawnerClass =
                SDK::FindClass("FortAthenaVehicleSpawner");
            if (VehicleSpawnerClass)
            {
                GDeferredVehicleSpawnState.VehicleSpawnerClass =
                    TWeakObjectPtr<UClass>(
                        const_cast<UClass*>(
                            VehicleSpawnerClass));
            }
        }
        if (VehicleSpawnerClass)
        {
            if (CurrentTimeMs >= GDeferredVehicleSpawnState
                    .NextSpawnerRefreshTimeMs)
            {
                TArray<AFortAthenaVehicleSpawner*> Spawners{};
                Utils::GetAll(VehicleSpawnerClass, Spawners);
                GDeferredVehicleSpawnState
                    .CachedVehicleSpawners.clear();
                GDeferredVehicleSpawnState
                    .CachedVehicleSpawners.reserve(Spawners.Num());
                for (auto Spawner : Spawners)
                {
                    if (Spawner)
                    {
                        GDeferredVehicleSpawnState
                            .CachedVehicleSpawners.emplace_back(
                                Spawner);
                    }
                }
                Spawners.Free();
                GDeferredVehicleSpawnState
                    .NextSpawnerRefreshTimeMs =
                        CurrentTimeMs +
                        VehicleSourceRefreshIntervalMs;
            }
            SpawnerCount = static_cast<int32>(
                GDeferredVehicleSpawnState
                    .CachedVehicleSpawners.size());

            const int32 SpawnerStartIndex =
                SpawnerCount > 0
                    ? GDeferredVehicleSpawnState
                        .VehicleSpawnerCursor % SpawnerCount
                    : 0;
            for (int32 Visited = 0;
                Visited < SpawnerCount;
                ++Visited)
            {
                if (LimitSpawnWork &&
                    SpawnAttemptsThisPass >=
                        VehicleSpawnBudgetPerPass)
                {
                    break;
                }
                const int32 SpawnerIndex =
                    (SpawnerStartIndex + Visited) %
                        SpawnerCount;
                auto* Spawner =
                    GDeferredVehicleSpawnState
                        .CachedVehicleSpawners[SpawnerIndex].Get();
                if (!Spawner ||
                    IsVehicleSpawnSourceProcessed(Spawner))
                {
                    continue;
                }

                auto* VehicleClass = Spawner->GetVehicleClass();
                if (!VehicleClass)
                {
                    ++MissingClassCount;
                    continue;
                }

                if (Spawner->HasCachedFortVehicleItemDef() &&
                    (!Spawner->HasbForceSpawnAlways() ||
                        !Spawner->bForceSpawnAlways))
                {
                    auto* VehicleDef =
                        Spawner->CachedFortVehicleItemDef;
                    if (!VehicleDef)
                    {
                        ++MissingClassCount;
                        continue;
                    }

                    const double Min = std::clamp(
                        VehicleDef->VehicleMinSpawnPercent
                            .Evaluate() * 0.01f,
                        0.0f,
                        1.0f);
                    const double Max = std::clamp(
                        VehicleDef->VehicleMaxSpawnPercent
                            .Evaluate() * 0.01f,
                        0.0f,
                        1.0f);
                    const auto SpawnPercent =
                        Min + (Max - Min) *
                            (rand() / (float)RAND_MAX);
                    const bool ShouldSpawn =
                        (rand() / (float)RAND_MAX) <=
                            SpawnPercent;
                    if (!ShouldSpawn)
                    {
                        // Preserve the original one-shot probability
                        // decision; retries must not eventually turn every
                        // rejected source into a guaranteed vehicle.
                        MarkVehicleSpawnSourceProcessed(Spawner);
                        ++ChanceSkippedCount;
                        continue;
                    }
                }

                ++SpawnAttemptsThisPass;
                GDeferredVehicleSpawnState.VehicleSpawnerCursor =
                    (SpawnerIndex + 1) % SpawnerCount;
                auto* Vehicle =
                    UWorld::SpawnActor<AFortAthenaVehicle>(
                        VehicleClass,
                        Spawner->K2_GetActorLocation(),
                        Spawner->K2_GetActorRotation());
                if (!Vehicle)
                {
                    ++SpawnFailureCount;
                    continue;
                }

                MarkVehicleSpawnSourceProcessed(Spawner);
                FortVehicleMods::RegisterSpawnedVehicle(
                    Vehicle,
                    Spawner);
                ++SpawnedThisPass;

                if (auto* Car =
                    Vehicle->Cast<AFortDagwoodVehicle>())
                {
                    Car->SetFuel(100.f);
                }
            }
        }

        ++GDeferredVehicleSpawnState.PassCount;
        GDeferredVehicleSpawnState.TotalSpawned +=
            SpawnedThisPass;
        const uint32 Pass =
            GDeferredVehicleSpawnState.PassCount;
        if (Pass <= 2 ||
            SpawnedThisPass > 0 ||
            Pass % 5 == 0)
        {
            SDK::DbgLog(
                "[VehicleSpawn] pass=%u providers=%d mapped=%d "
                "provider-unresolved=%d density-skipped=%d "
                "provider-failed=%d "
                "spawners=%d missing-class=%d chance-skipped=%d "
                "spawner-failed=%d attempts=%d new=%d total=%d "
                "processed=%d\n",
                Pass,
                ProviderCount,
                MappedProviderCount,
                UnresolvedProviderCount,
                ProviderDensitySkippedCount,
                ProviderSpawnFailureCount,
                SpawnerCount,
                MissingClassCount,
                ChanceSkippedCount,
                SpawnFailureCount,
                SpawnAttemptsThisPass,
                SpawnedThisPass,
                GDeferredVehicleSpawnState.TotalSpawned,
                static_cast<int32>(
                    GDeferredVehicleSpawnState
                        .ProcessedSources.size()));
        }
        return SpawnedThisPass;
    }

    void BeginDeferredVehicleSpawnDiscovery()
    {
        auto* World = UWorld::GetWorld();
        if (!World)
            return;

        if (GDeferredVehicleSpawnState.World.Get() != World)
        {
            GDeferredVehicleSpawnState.ProcessedSources.clear();
            GDeferredVehicleSpawnState.CachedProviders.clear();
            GDeferredVehicleSpawnState.CachedVehicleSpawners.clear();
            GDeferredVehicleSpawnState.World =
                TWeakObjectPtr<UWorld>(World);
            GDeferredVehicleSpawnState.NextAttemptTimeMs = 0;
            GDeferredVehicleSpawnState.DeadlineTimeMs = 0;
            GDeferredVehicleSpawnState.NextProviderRefreshTimeMs = 0;
            GDeferredVehicleSpawnState.NextSpawnerRefreshTimeMs = 0;
            GDeferredVehicleSpawnState.PassCount = 0;
            GDeferredVehicleSpawnState.TotalSpawned = 0;
            GDeferredVehicleSpawnState.ProviderCursor = 0;
            GDeferredVehicleSpawnState.VehicleSpawnerCursor = 0;
            GDeferredVehicleSpawnState.Started = false;
            GDeferredVehicleSpawnState.Active = false;
        }

        if (GDeferredVehicleSpawnState.Started)
            return;

        const ULONGLONG CurrentTimeMs = GetTickCount64();
        GDeferredVehicleSpawnState.Started = true;
        GDeferredVehicleSpawnState.Active =
            UsesDeferredVehicleSpawnDiscovery();
        GDeferredVehicleSpawnState.NextAttemptTimeMs =
            CurrentTimeMs + VehicleInitialDiscoveryDelayMs;
        GDeferredVehicleSpawnState.DeadlineTimeMs =
            CurrentTimeMs + VehicleSpawnRetryWindowMs;
    }

    struct FJumpFatiguePolicySnapshot
    {
        TWeakObjectPtr<UFortMovementComp_CharacterAthena>
            MovementComponent;
        float OriginalResetTime = 0.f;
    };

    struct FPlayerResourcePolicySnapshot
    {
        TWeakObjectPtr<AFortPlayerControllerAthena>
            PlayerController;
        bool bHasBuildFree = false;
        bool bOriginalBuildFree = false;
        bool bHasInfiniteAmmo = false;
        bool bOriginalInfiniteAmmo = false;
    };

    struct FGliderRedeployPawnSnapshot
    {
        TWeakObjectPtr<AFortPlayerPawnAthena> Pawn;
        bool bHasAllowedRow = false;
        FScalableFloat OriginalAllowedRow{};
        bool bHasHeightLimitRow = false;
        FScalableFloat OriginalHeightLimitRow{};
        bool bHasLateralVelocityMultRow = false;
        FScalableFloat OriginalLateralVelocityMultRow{};
    };

    struct FGameplayConfigurationPolicyState
    {
        TWeakObjectPtr<UWorld> World;
        TWeakObjectPtr<AFortGameStateAthena> GameState;
        std::vector<FJumpFatiguePolicySnapshot>
            JumpFatigueSnapshots;
        std::vector<FPlayerResourcePolicySnapshot>
            PlayerResourceSnapshots;
        bool bGliderRedeployCaptured = false;
        float OriginalGliderRedeploy = 0.f;
        bool bGliderHeightLimitCaptured = false;
        float OriginalGliderHeightLimit = 0.f;
        bool bGliderLateralVelocityMultCaptured = false;
        float OriginalGliderLateralVelocityMult = 0.f;
        bool bGliderRowValuesResolved = false;
        float DesiredGliderHeightLimit = 0.f;
        float DesiredGliderLateralVelocityMult = 0.f;
        std::vector<FGliderRedeployPawnSnapshot>
            GliderRedeployPawnSnapshots;
        bool bTODMApplied = false;
        bool bOriginalTODMSpeedCaptured = false;
        float OriginalTODMSpeed = 0.f;
        TWeakObjectPtr<UObject> TODMController;
        float LastTODMTime = -1.f;
        float TODMReapplySeconds = 0.f;
        bool bLoggedTODMSuccess = false;
        bool bLoggedTODMFailure = false;
        bool bManualBusRelease = false;
        bool bAircraftStartRequested = false;
        bool bBusCountdownArmed = false;
        bool bLoggedBlockedAircraftStart = false;
        int32 BusPolicyMode = -1;
        float BusPolicyDelay = -1.f;
        float BusScheduleStartTime = 0.f;
        float BusScheduleEndTime = 0.f;
        float BusNextStartAttemptTime = 0.f;
    };

    FGameplayConfigurationPolicyState
        GGameplayConfigurationPolicyState{};

    bool IsAuthoritativeBusPolicySelection()
    {
        // Special event/custom-map flows keep their authored schedule by
        // default. Once the user changes Auto Bus Start or Bus Start Delay,
        // the visible GUI setting becomes authoritative for the session.
        switch (static_cast<Playlist>(
            GUI::GetSelectedPlaylist()))
        {
        case Playlist::Gav:
        case Playlist::Retrac1v1:
        case Playlist::RetracTurtle:
        case Playlist::RetracWater:
        case Playlist::Creative:
        case Playlist::OnlyUp:
        case Playlist::TiltedZW:
        case Playlist::Twine1v1:
        case Playlist::Boxfight:
        case Playlist::Backrooms:
        case Playlist::Event:
            return FConfiguration::bBusSettingsUserOverride.load(
                std::memory_order_acquire);
        default:
            return true;
        }
    }

    void RestoreJumpFatiguePolicy()
    {
        for (const auto& Snapshot :
            GGameplayConfigurationPolicyState
                .JumpFatigueSnapshots)
        {
            auto Movement = Snapshot.MovementComponent.Get();
            if (IsSaneObject(Movement) &&
                Movement->HasJumpPenaltyResetTime())
            {
                float OriginalResetTime =
                    Snapshot.OriginalResetTime;
                Movement->JumpPenaltyResetTime =
                    OriginalResetTime;
            }
        }

        GGameplayConfigurationPolicyState
            .JumpFatigueSnapshots.clear();
    }

    void RestorePlayerResourcePolicy()
    {
        auto& Snapshots =
            GGameplayConfigurationPolicyState
                .PlayerResourceSnapshots;
        for (const auto& Snapshot : Snapshots)
        {
            auto PlayerController =
                Snapshot.PlayerController.Get();
            if (!IsSaneObject(PlayerController))
                continue;

            bool bChanged = false;
            if (Snapshot.bHasBuildFree &&
                PlayerController->HasbBuildFree() &&
                PlayerController->bBuildFree !=
                    Snapshot.bOriginalBuildFree)
            {
                PlayerController->bBuildFree =
                    Snapshot.bOriginalBuildFree;
                bChanged = true;
            }
            if (Snapshot.bHasInfiniteAmmo &&
                PlayerController->HasbInfiniteAmmo() &&
                PlayerController->bInfiniteAmmo !=
                    Snapshot.bOriginalInfiniteAmmo)
            {
                PlayerController->bInfiniteAmmo =
                    Snapshot.bOriginalInfiniteAmmo;
                bChanged = true;
            }
            if (bChanged)
                PlayerController->ForceNetUpdate();
        }
        Snapshots.clear();
    }

    void RestoreGliderRedeployPawnPolicy()
    {
        auto& Snapshots =
            GGameplayConfigurationPolicyState
                .GliderRedeployPawnSnapshots;
        for (auto& Snapshot : Snapshots)
        {
            auto Pawn = Snapshot.Pawn.Get();
            if (!IsSaneObject(Pawn))
                continue;

            if (Snapshot.bHasAllowedRow &&
                Pawn->HasGliderRedeployAllowedRow())
            {
                Pawn->GliderRedeployAllowedRow =
                    Snapshot.OriginalAllowedRow;
            }
            if (Snapshot.bHasHeightLimitRow &&
                Pawn->HasGliderRedeployHeighLimitRow())
            {
                Pawn->GliderRedeployHeighLimitRow =
                    Snapshot.OriginalHeightLimitRow;
            }
            if (Snapshot.bHasLateralVelocityMultRow &&
                Pawn
                    ->HasGliderRedeployLateralVelocityMultiplierRow())
            {
                Pawn
                    ->GliderRedeployLateralVelocityMultiplierRow =
                    Snapshot.OriginalLateralVelocityMultRow;
            }
        }
        Snapshots.clear();
    }

    void ResetGameplayConfigurationPolicy(
        bool bRestoreCurrentWorld)
    {
        auto& State = GGameplayConfigurationPolicyState;
        if (bRestoreCurrentWorld)
        {
            RestoreJumpFatiguePolicy();
            RestorePlayerResourcePolicy();
            RestoreGliderRedeployPawnPolicy();

            auto GameState = State.GameState.Get();
            if (State.bGliderRedeployCaptured &&
                IsSaneObject(GameState) &&
                GameState
                    ->HasDefaultGliderRedeployCanRedeploy())
            {
                GameState
                    ->DefaultGliderRedeployCanRedeploy =
                    State.OriginalGliderRedeploy;
                if (State.bGliderHeightLimitCaptured &&
                    GameState
                        ->HasDefaultRedeployGliderHeightLimit())
                {
                    GameState
                        ->DefaultRedeployGliderHeightLimit =
                        State.OriginalGliderHeightLimit;
                }
                if (State
                        .bGliderLateralVelocityMultCaptured &&
                    GameState
                        ->HasDefaultRedeployGliderLateralVelocityMult())
                {
                    GameState
                        ->DefaultRedeployGliderLateralVelocityMult =
                        State
                            .OriginalGliderLateralVelocityMult;
                }
                GameState->ForceNetUpdate();
            }

            auto World = State.World.Get();
            if (State.bTODMApplied &&
                State.bOriginalTODMSpeedCaptured &&
                IsSaneObject(World))
            {
                if (!UFortKismetLibrary::
                        SetResolvedTimeOfDaySpeedCompat(
                            State.TODMController.Get(),
                            State.OriginalTODMSpeed))
                {
                    UFortKismetLibrary::
                        SetTimeOfDaySpeedCompat(
                            World,
                            State.OriginalTODMSpeed);
                }
            }
        }

        State = FGameplayConfigurationPolicyState{};
        FConfiguration::GliderRedeployRuntimeSupport.store(
            -1, std::memory_order_release);
    }

    void ApplyJumpFatiguePolicy(
        AFortGameMode* GameMode)
    {
        auto& State = GGameplayConfigurationPolicyState;
        if (!FConfiguration::bDisableJumpFatigue)
        {
            if (!State.JumpFatigueSnapshots.empty())
                RestoreJumpFatiguePolicy();
            return;
        }

        for (auto It =
            State.JumpFatigueSnapshots.begin();
            It != State.JumpFatigueSnapshots.end();)
        {
            if (!IsSaneObject(It->MovementComponent.Get()))
            {
                It = State.JumpFatigueSnapshots.erase(It);
                continue;
            }
            ++It;
        }

        auto ApplyController =
            [&](AFortPlayerControllerAthena*
                    PlayerController)
            {
            if (!IsSaneObject(PlayerController))
                return;
            auto Pawn =
                PlayerController &&
                    IsSaneObject(
                        PlayerController->MyFortPawn)
                    ? PlayerController->MyFortPawn
                    : nullptr;
            if (!Pawn || !Pawn->HasCharacterMovement())
                return;

            auto Movement =
                (UFortMovementComp_CharacterAthena*)
                    Pawn->GetCharacterMovement();
            if (!IsSaneObject(Movement) ||
                !Movement->HasJumpPenaltyResetTime())
            {
                return;
            }

            auto Existing = std::find_if(
                State.JumpFatigueSnapshots.begin(),
                State.JumpFatigueSnapshots.end(),
                [&](const FJumpFatiguePolicySnapshot&
                        Snapshot)
                {
                    return Snapshot
                        .MovementComponent.Get() ==
                        Movement;
                });
            if (Existing ==
                State.JumpFatigueSnapshots.end())
            {
                State.JumpFatigueSnapshots.push_back(
                    {
                        TWeakObjectPtr<
                            UFortMovementComp_CharacterAthena>(
                                Movement),
                        Movement->JumpPenaltyResetTime
                    });
            }

            if (Movement->JumpPenaltyResetTime != 0.f)
                Movement->JumpPenaltyResetTime = 0.f;
            };

        if (GameMode->HasAlivePlayers())
        {
            for (auto UncastedController :
                GameMode->AlivePlayers)
            {
                ApplyController(
                    IsSaneObject(UncastedController)
                        ? UncastedController->Cast<
                            AFortPlayerControllerAthena>()
                        : nullptr);
            }
        }

        auto World = UWorld::GetWorld();
        auto Driver =
            World
                ? (UNetDriver*)World->NetDriver
                : nullptr;
        if (!IsSaneObject(Driver))
            return;

        for (int32 Index = 0;
            Index < Driver->ClientConnections.Num();
            ++Index)
        {
            auto Connection =
                Driver->ClientConnections[Index];
            ApplyController(
                IsSaneObject(Connection) &&
                    IsSaneObject(
                        Connection->PlayerController)
                    ? Connection->PlayerController
                        ->Cast<
                            AFortPlayerControllerAthena>()
                    : nullptr);
            if (IsSaneObject(Connection) &&
                Connection->HasChildren())
            {
                for (int32 ChildIndex = 0;
                    ChildIndex <
                        Connection->Children.Num();
                    ++ChildIndex)
                {
                    auto Child =
                        Connection->Children[
                            ChildIndex];
                    ApplyController(
                        IsSaneObject(Child) &&
                            IsSaneObject(
                                Child->PlayerController)
                            ? Child->PlayerController
                                ->Cast<
                                    AFortPlayerControllerAthena>()
                            : nullptr);
                }
            }
        }

        auto GameInstance =
            World ? World->OwningGameInstance : nullptr;
        if (IsSaneObject(GameInstance) &&
            GameInstance->HasLocalPlayers())
        {
            for (int32 Index = 0;
                Index < GameInstance->LocalPlayers.Num();
                ++Index)
            {
                auto LocalPlayer =
                    GameInstance->LocalPlayers[Index];
                ApplyController(
                    IsSaneObject(LocalPlayer) &&
                        IsSaneObject(
                            LocalPlayer
                                ->PlayerController)
                        ? LocalPlayer->PlayerController
                            ->Cast<
                                AFortPlayerControllerAthena>()
                        : nullptr);
            }
        }
    }

    void ApplyPlayerResourcePolicy(UWorld* World)
    {
        auto& Snapshots =
            GGameplayConfigurationPolicyState
                .PlayerResourceSnapshots;
        for (auto It = Snapshots.begin();
            It != Snapshots.end();)
        {
            if (!IsSaneObject(
                    It->PlayerController.Get()))
            {
                It = Snapshots.erase(It);
                continue;
            }
            ++It;
        }

        auto Driver =
            World
                ? (UNetDriver*)World->NetDriver
                : nullptr;
        if (!IsSaneObject(Driver))
            return;

        auto ApplyController =
            [&](AFortPlayerControllerAthena*
                    PlayerController)
            {
            if (!IsSaneObject(PlayerController))
                return;

            auto Existing = std::find_if(
                Snapshots.begin(), Snapshots.end(),
                [&](const FPlayerResourcePolicySnapshot&
                        Snapshot)
                {
                    return Snapshot
                        .PlayerController.Get() ==
                        PlayerController;
                });
            if (Existing == Snapshots.end())
            {
                FPlayerResourcePolicySnapshot Snapshot;
                Snapshot.PlayerController =
                    TWeakObjectPtr<
                        AFortPlayerControllerAthena>(
                            PlayerController);
                Snapshot.bHasBuildFree =
                    PlayerController->HasbBuildFree();
                if (Snapshot.bHasBuildFree)
                {
                    Snapshot.bOriginalBuildFree =
                        PlayerController->bBuildFree;
                }
                Snapshot.bHasInfiniteAmmo =
                    PlayerController
                        ->HasbInfiniteAmmo();
                if (Snapshot.bHasInfiniteAmmo)
                {
                    Snapshot.bOriginalInfiniteAmmo =
                        PlayerController
                            ->bInfiniteAmmo;
                }
                Snapshots.push_back(Snapshot);
            }

            bool bChanged = false;
            if (PlayerController->HasbBuildFree() &&
                PlayerController->bBuildFree !=
                    FConfiguration::bInfiniteMats)
            {
                PlayerController->bBuildFree =
                    FConfiguration::bInfiniteMats;
                bChanged = true;
            }
            if (PlayerController->HasbInfiniteAmmo() &&
                PlayerController->bInfiniteAmmo !=
                    FConfiguration::bInfiniteAmmo)
            {
                PlayerController->bInfiniteAmmo =
                    FConfiguration::bInfiniteAmmo;
                bChanged = true;
            }
            if (bChanged)
                PlayerController->ForceNetUpdate();
            };

        auto ApplyConnection =
            [&](UNetConnection* Connection)
            {
                if (!IsSaneObject(Connection))
                    return;

                ApplyController(
                    IsSaneObject(
                        Connection->PlayerController)
                        ? Connection->PlayerController
                            ->Cast<
                                AFortPlayerControllerAthena>()
                        : nullptr);

                if (!Connection->HasChildren())
                    return;
                for (int32 ChildIndex = 0;
                    ChildIndex <
                        Connection->Children.Num();
                    ++ChildIndex)
                {
                    auto Child =
                        Connection->Children[
                            ChildIndex];
                    ApplyController(
                        IsSaneObject(Child) &&
                            IsSaneObject(
                                Child->PlayerController)
                            ? Child->PlayerController
                                ->Cast<
                                    AFortPlayerControllerAthena>()
                            : nullptr);
                }
            };

        for (int32 Index = 0;
            Index < Driver->ClientConnections.Num();
            ++Index)
        {
            ApplyConnection(
                Driver->ClientConnections[Index]);
        }

        auto GameInstance =
            World ? World->OwningGameInstance : nullptr;
        if (IsSaneObject(GameInstance) &&
            GameInstance->HasLocalPlayers())
        {
            for (int32 Index = 0;
                Index < GameInstance->LocalPlayers.Num();
                ++Index)
            {
                auto LocalPlayer =
                    GameInstance->LocalPlayers[Index];
                ApplyController(
                    IsSaneObject(LocalPlayer) &&
                        IsSaneObject(
                            LocalPlayer
                                ->PlayerController)
                        ? LocalPlayer->PlayerController
                            ->Cast<
                                AFortPlayerControllerAthena>()
                        : nullptr);
            }
        }
    }

    float EvaluateGliderRedeployRow(FScalableFloat& Row)
    {
        const float BaseValue = Row.Value;
        auto CurveTable = (UObject*)Row.Curve.CurveTable;
        if (!CurveTable || !IsSaneObject(CurveTable) ||
            !Row.Curve.RowName.IsValid())
        {
            return std::isfinite(BaseValue)
                ? BaseValue
                : 0.f;
        }

        auto Library =
            UDataTableFunctionLibrary::StaticClass();
        auto Defaults =
            Library ? Library->GetDefaultObj() : nullptr;
        if (!IsSaneObject(Defaults) ||
            !Defaults->GetFunction(
                "EvaluateCurveTableRow"))
        {
            return std::isfinite(BaseValue)
                ? BaseValue
                : 0.f;
        }

        const float Value = Row.Evaluate();
        return std::isfinite(Value) ? Value : 0.f;
    }

    AFortPlayerPawnAthena* ResolveGliderRedeployPawnDefaults(
        AFortGameMode* GameMode)
    {
        auto PawnBaseClass =
            AFortPlayerPawnAthena::StaticClass();
        if (!PawnBaseClass)
            return nullptr;

        auto PawnClass =
            IsSaneObject(GameMode) &&
                GameMode->HasDefaultPawnClass()
                ? GameMode->DefaultPawnClass
                : nullptr;
        if (IsSaneObject((UObject*)PawnClass))
        {
            auto Defaults = PawnClass->GetDefaultObj();
            if (IsSaneObject(Defaults) &&
                Defaults->IsA(PawnBaseClass))
            {
                return (AFortPlayerPawnAthena*)Defaults;
            }
        }

        auto AthenaDefaults =
            (AFortPlayerPawnAthena*)
                AFortPlayerPawnAthena::GetDefaultObj();
        return IsSaneObject((UObject*)AthenaDefaults)
            ? AthenaDefaults
            : nullptr;
    }

    void ResolveGliderRedeployPolicyValues(
        AFortGameMode* GameMode,
        float& OutHeightLimit,
        float& OutLateralVelocityMult)
    {
        OutHeightLimit = 0.f;
        OutLateralVelocityMult = 0.f;

        auto Defaults =
            ResolveGliderRedeployPawnDefaults(GameMode);
        if (!Defaults)
            return;

        if (Defaults->HasGliderRedeployHeighLimitRow())
        {
            OutHeightLimit = EvaluateGliderRedeployRow(
                Defaults->GliderRedeployHeighLimitRow);
        }
        if (Defaults
                ->HasGliderRedeployLateralVelocityMultiplierRow())
        {
            OutLateralVelocityMult =
                EvaluateGliderRedeployRow(
                    Defaults
                        ->GliderRedeployLateralVelocityMultiplierRow);
        }
    }

    float SelectGliderRedeployValue(
        float AuthoredValue,
        float OriginalValue,
        float FallbackValue)
    {
        if (std::isfinite(AuthoredValue) &&
            AuthoredValue > 0.f)
        {
            return AuthoredValue;
        }
        if (std::isfinite(OriginalValue) &&
            OriginalValue > 0.f)
        {
            return OriginalValue;
        }
        return FallbackValue;
    }

    void WriteGliderRedeployRow(
        FScalableFloat& Row,
        float Value)
    {
        if (Row.Value == Value && !Row.Curve.CurveTable)
            return;

        Row.Value = Value;
        Row.Curve.CurveTable = nullptr;
        Row.Curve.RowName.ComparisonIndex = 0;
        Row.Curve.RowName.Number = 0;
    }

    void ApplyGliderRedeployPawnPolicy(
        AFortGameMode* GameMode,
        UWorld* World,
        bool bEnabled,
        float HeightLimit,
        float LateralVelocityMult)
    {
        auto& Snapshots =
            GGameplayConfigurationPolicyState
                .GliderRedeployPawnSnapshots;
        for (auto It = Snapshots.begin();
            It != Snapshots.end();)
        {
            if (!IsSaneObject(It->Pawn.Get()))
            {
                It = Snapshots.erase(It);
                continue;
            }
            ++It;
        }

        if (!bEnabled)
        {
            if (!Snapshots.empty())
                RestoreGliderRedeployPawnPolicy();
            return;
        }

        auto ApplyPawn =
            [&](AFortPlayerPawnAthena* Pawn)
            {
            if (!IsSaneObject(Pawn))
                return;

            const bool bHasAllowedRow =
                Pawn->HasGliderRedeployAllowedRow();
            const bool bHasHeightLimitRow =
                Pawn->HasGliderRedeployHeighLimitRow();
            const bool bHasLateralVelocityMultRow =
                Pawn
                    ->HasGliderRedeployLateralVelocityMultiplierRow();
            if (!bHasAllowedRow && !bHasHeightLimitRow &&
                !bHasLateralVelocityMultRow)
            {
                return;
            }

            auto Existing = std::find_if(
                Snapshots.begin(), Snapshots.end(),
                [&](const FGliderRedeployPawnSnapshot&
                        Snapshot)
                {
                    return Snapshot.Pawn.Get() == Pawn;
                });
            if (Existing == Snapshots.end())
            {
                FGliderRedeployPawnSnapshot Snapshot;
                Snapshot.Pawn =
                    TWeakObjectPtr<AFortPlayerPawnAthena>(
                        Pawn);
                Snapshot.bHasAllowedRow = bHasAllowedRow;
                if (bHasAllowedRow)
                {
                    Snapshot.OriginalAllowedRow =
                        Pawn->GliderRedeployAllowedRow;
                }
                Snapshot.bHasHeightLimitRow =
                    bHasHeightLimitRow;
                if (bHasHeightLimitRow)
                {
                    Snapshot.OriginalHeightLimitRow =
                        Pawn->GliderRedeployHeighLimitRow;
                }
                Snapshot.bHasLateralVelocityMultRow =
                    bHasLateralVelocityMultRow;
                if (bHasLateralVelocityMultRow)
                {
                    Snapshot
                        .OriginalLateralVelocityMultRow =
                        Pawn
                            ->GliderRedeployLateralVelocityMultiplierRow;
                }
                Snapshots.push_back(Snapshot);
            }

            if (bHasAllowedRow)
            {
                WriteGliderRedeployRow(
                    Pawn->GliderRedeployAllowedRow, 1.f);
            }
            if (bHasHeightLimitRow && HeightLimit > 0.f)
            {
                WriteGliderRedeployRow(
                    Pawn->GliderRedeployHeighLimitRow,
                    HeightLimit);
            }
            if (bHasLateralVelocityMultRow &&
                LateralVelocityMult > 0.f)
            {
                WriteGliderRedeployRow(
                    Pawn
                        ->GliderRedeployLateralVelocityMultiplierRow,
                    LateralVelocityMult);
            }
            };

        auto ApplyController =
            [&](AFortPlayerControllerAthena*
                    PlayerController)
            {
                if (!IsSaneObject(PlayerController))
                    return;

                ApplyPawn(
                    IsSaneObject(
                        PlayerController->MyFortPawn)
                        ? PlayerController->MyFortPawn
                        : nullptr);
            };

        ApplyPawn(
            ResolveGliderRedeployPawnDefaults(GameMode));

        if (IsSaneObject(GameMode) &&
            GameMode->HasAlivePlayers())
        {
            for (auto UncastedController :
                GameMode->AlivePlayers)
            {
                ApplyController(
                    IsSaneObject(UncastedController)
                        ? UncastedController->Cast<
                            AFortPlayerControllerAthena>()
                        : nullptr);
            }
        }

        auto Driver =
            World
                ? (UNetDriver*)World->NetDriver
                : nullptr;
        if (!IsSaneObject(Driver))
            return;

        auto ApplyConnection =
            [&](UNetConnection* Connection)
            {
                if (!IsSaneObject(Connection))
                    return;

                ApplyController(
                    IsSaneObject(
                        Connection->PlayerController)
                        ? Connection->PlayerController
                            ->Cast<
                                AFortPlayerControllerAthena>()
                        : nullptr);

                if (!Connection->HasChildren())
                    return;
                for (int32 ChildIndex = 0;
                    ChildIndex <
                        Connection->Children.Num();
                    ++ChildIndex)
                {
                    auto Child =
                        Connection->Children[ChildIndex];
                    ApplyController(
                        IsSaneObject(Child) &&
                            IsSaneObject(
                                Child->PlayerController)
                            ? Child->PlayerController
                                ->Cast<
                                    AFortPlayerControllerAthena>()
                            : nullptr);
                }
            };

        for (int32 Index = 0;
            Index < Driver->ClientConnections.Num();
            ++Index)
        {
            ApplyConnection(
                Driver->ClientConnections[Index]);
        }
    }

    UFortGameStateComponent_BattleRoyaleGamePhaseLogic*
        ResolveGameplayPolicyPhaseLogic(UWorld* World)
    {
        auto PhaseLogicClass =
            UFortGameStateComponent_BattleRoyaleGamePhaseLogic::
                StaticClass();
        auto DefaultObject =
            PhaseLogicClass
                ? (const UFortGameStateComponent_BattleRoyaleGamePhaseLogic*)
                    PhaseLogicClass->GetDefaultObj()
                : nullptr;
        if (!DefaultObject ||
            !DefaultObject->GetFunction("Get"))
        {
            return nullptr;
        }

        auto PhaseLogic =
            UFortGameStateComponent_BattleRoyaleGamePhaseLogic::
                Get(World);
        return IsSaneObject(PhaseLogic)
            ? PhaseLogic
            : nullptr;
    }

    void ApplyWarmupSchedule(
        AFortGameMode* GameMode,
        AFortGameStateAthena* GameState,
        UFortGameStateComponent_BattleRoyaleGamePhaseLogic*
            PhaseLogic,
        float StartTime,
        float EndTime,
        float Duration)
    {
        bool bGameStateChanged = false;
        if (GameState->HasWarmupCountdownStartTime() &&
            std::fabs(
                GameState->WarmupCountdownStartTime -
                    StartTime) > 0.01f)
        {
            GameState->WarmupCountdownStartTime =
                StartTime;
            bGameStateChanged = true;
        }
        if (GameState->HasWarmupCountdownEndTime() &&
            std::fabs(
                GameState->WarmupCountdownEndTime -
                    EndTime) > 0.01f)
        {
            GameState->WarmupCountdownEndTime = EndTime;
            bGameStateChanged = true;
        }
        if (GameMode->HasWarmupCountdownDuration())
            GameMode->WarmupCountdownDuration = Duration;
        if (GameMode->HasWarmupEarlyCountdownDuration())
            GameMode->WarmupEarlyCountdownDuration =
                Duration;

        if (PhaseLogic)
        {
            if (PhaseLogic->HasWarmupCountdownStartTime())
            {
                PhaseLogic->WarmupCountdownStartTime =
                    StartTime;
            }
            if (PhaseLogic->HasWarmupCountdownEndTime())
            {
                PhaseLogic->WarmupCountdownEndTime =
                    EndTime;
            }
            if (PhaseLogic->HasWarmupCountdownDuration())
            {
                PhaseLogic->WarmupCountdownDuration =
                    Duration;
            }
            if (PhaseLogic
                ->HasWarmupEarlyCountdownDuration())
            {
                PhaseLogic
                    ->WarmupEarlyCountdownDuration =
                    Duration;
            }
        }

        if (bGameStateChanged)
            GameState->ForceNetUpdate();
    }
}

void AFortGameMode::TickSupplyDropSuppression(bool bForceDiscovery)
{
    ApplySupplyDropSuppression(
        UWorld::GetWorld(), bForceDiscovery);
}

void AFortGameMode::TickGameplayConfigurationPolicy(
    float DeltaSeconds)
{
    auto World = UWorld::GetWorld();
    auto GameMode =
        World && World->AuthorityGameMode
            ? (AFortGameMode*)World->AuthorityGameMode
            : nullptr;
    auto GameState =
        GameMode
            ? (AFortGameStateAthena*)GameMode->GameState
            : nullptr;
    if (!IsSaneObject(World) ||
        !IsSaneObject(GameMode) ||
        !IsSaneObject(GameState))
    {
        ResetGameplayConfigurationPolicy(true);
        return;
    }

    auto& State = GGameplayConfigurationPolicyState;
    if (State.World.Get() != World ||
        State.GameState.Get() != GameState)
    {
        ResetGameplayConfigurationPolicy(true);
        State.World = TWeakObjectPtr<UWorld>(World);
        State.GameState =
            TWeakObjectPtr<AFortGameStateAthena>(GameState);
        FConfiguration::bStartBusRequested.store(
            false, std::memory_order_release);
    }

    auto Playlist = ResolveSupplyDropPlaylist(World);

    // Glider Redeploy used to write this replicated UObject directly from
    // ImGui's render thread. Reconcile both checked and unchecked states here
    // after native LTM ticks so a playlist-authored true value cannot make the
    // visible OFF state ineffective. Restore the authored value only when the
    // policy no longer applies (world/mode/version transition).
    const bool bGliderBuildSupported =
        FConfiguration::IsGliderRedeploySupportedBuild();
    const bool bGliderPolicyAvailable =
        bGliderBuildSupported &&
        GUI::UsesDefaultMatchSettings(
            GUI::GetSelectedPlaylist());
    if (!bGliderBuildSupported &&
        FConfiguration::bGliderRedeploy.load(
            std::memory_order_acquire))
    {
        // A hidden value restored from another build must not opt a modern or
        // legacy client into an implementation its UI deliberately gates out.
        FConfiguration::bGliderRedeploy.store(
            false, std::memory_order_release);
    }
    if (bGliderBuildSupported)
    {
        auto PawnDefaults =
            ResolveGliderRedeployPawnDefaults(GameMode);
        const bool bRuntimeSupported =
            GameState
                ->HasDefaultGliderRedeployCanRedeploy() ||
            (PawnDefaults &&
                PawnDefaults
                    ->HasGliderRedeployAllowedRow());
        FConfiguration::GliderRedeployRuntimeSupport.store(
            bRuntimeSupported ? 1 : 0,
            std::memory_order_release);
    }
    if (bGliderPolicyAvailable &&
        !State.bGliderRedeployCaptured &&
        GameState
            ->HasDefaultGliderRedeployCanRedeploy())
    {
        State.OriginalGliderRedeploy =
            GameState->DefaultGliderRedeployCanRedeploy;
        State.bGliderRedeployCaptured = true;

        State.bGliderHeightLimitCaptured =
            GameState
                ->HasDefaultRedeployGliderHeightLimit();
        if (State.bGliderHeightLimitCaptured)
        {
            State.OriginalGliderHeightLimit =
                GameState
                    ->DefaultRedeployGliderHeightLimit;
        }
        State.bGliderLateralVelocityMultCaptured =
            GameState
                ->HasDefaultRedeployGliderLateralVelocityMult();
        if (State.bGliderLateralVelocityMultCaptured)
        {
            State.OriginalGliderLateralVelocityMult =
                GameState
                    ->DefaultRedeployGliderLateralVelocityMult;
        }

        if (State.OriginalGliderRedeploy > 0.f)
        {
            FConfiguration::bGliderRedeploy.store(
                true, std::memory_order_release);
        }
    }
    if (bGliderPolicyAvailable &&
        !State.bGliderRowValuesResolved)
    {
        State.bGliderRowValuesResolved = true;

        const bool bHasStateHeightLimit =
            GameState
                ->HasDefaultRedeployGliderHeightLimit();
        const float StateHeightLimit = bHasStateHeightLimit
            ? GameState->DefaultRedeployGliderHeightLimit
            : 0.f;
        const bool bHasStateLateralVelocityMult =
            GameState
                ->HasDefaultRedeployGliderLateralVelocityMult();
        const float StateLateralVelocityMult =
            bHasStateLateralVelocityMult
                ? GameState
                    ->DefaultRedeployGliderLateralVelocityMult
                : 0.f;

        float AuthoredHeightLimit = 0.f;
        float AuthoredLateralVelocityMult = 0.f;
        ResolveGliderRedeployPolicyValues(
            GameMode,
            AuthoredHeightLimit,
            AuthoredLateralVelocityMult);
        const float GroundTraceDistance =
            GameState
                ->HasDefaultParachuteDeployTraceForGroundDistance()
                ? GameState
                    ->DefaultParachuteDeployTraceForGroundDistance
                : 0.f;
        State.DesiredGliderHeightLimit =
            SelectGliderRedeployValue(
                AuthoredHeightLimit,
                StateHeightLimit,
                SelectGliderRedeployValue(
                    GroundTraceDistance,
                    0.f,
                    FConfiguration::
                        GliderRedeployFallbackHeightLimit));
        State.DesiredGliderLateralVelocityMult =
            SelectGliderRedeployValue(
                AuthoredLateralVelocityMult,
                StateLateralVelocityMult,
                FConfiguration::
                    GliderRedeployFallbackLateralVelocityMult);
        SDK::DbgLog(
            "  [GliderRedeploy] FN=%.2f authored height=%.2f lateral=%.2f "
            "state height=%.2f lateral=%.2f trace=%.2f -> "
            "height=%.2f lateral=%.2f\n",
            VersionInfo.FortniteVersion,
            AuthoredHeightLimit,
            AuthoredLateralVelocityMult,
            StateHeightLimit,
            StateLateralVelocityMult,
            GroundTraceDistance,
            State.DesiredGliderHeightLimit,
            State.DesiredGliderLateralVelocityMult);
    }
    const bool bGliderRedeployEnabled =
        bGliderPolicyAvailable &&
        FConfiguration::bGliderRedeploy.load(
            std::memory_order_acquire);
    if (bGliderPolicyAvailable &&
        GameState
            ->HasDefaultGliderRedeployCanRedeploy())
    {
        float DesiredRedeploy =
            bGliderRedeployEnabled ? 1.f : 0.f;
        bool bGliderStateChanged = false;
        if (GameState
                ->DefaultGliderRedeployCanRedeploy !=
            DesiredRedeploy)
        {
            GameState
                ->DefaultGliderRedeployCanRedeploy =
                DesiredRedeploy;
            bGliderStateChanged = true;
        }
        if (State.bGliderHeightLimitCaptured)
        {
            float DesiredHeightLimit =
                bGliderRedeployEnabled
                    ? State.DesiredGliderHeightLimit
                    : State.OriginalGliderHeightLimit;
            if (GameState
                    ->DefaultRedeployGliderHeightLimit !=
                DesiredHeightLimit)
            {
                GameState
                    ->DefaultRedeployGliderHeightLimit =
                    DesiredHeightLimit;
                bGliderStateChanged = true;
            }
        }
        if (State.bGliderLateralVelocityMultCaptured)
        {
            float DesiredLateralVelocityMult =
                bGliderRedeployEnabled
                    ? State
                        .DesiredGliderLateralVelocityMult
                    : State
                        .OriginalGliderLateralVelocityMult;
            if (GameState
                    ->DefaultRedeployGliderLateralVelocityMult !=
                DesiredLateralVelocityMult)
            {
                GameState
                    ->DefaultRedeployGliderLateralVelocityMult =
                    DesiredLateralVelocityMult;
                bGliderStateChanged = true;
            }
        }
        if (bGliderStateChanged)
            GameState->ForceNetUpdate();
    }
    else if (State.bGliderRedeployCaptured)
    {
        if (GameState
            ->HasDefaultGliderRedeployCanRedeploy())
        {
            GameState
                ->DefaultGliderRedeployCanRedeploy =
                State.OriginalGliderRedeploy;
            if (State.bGliderHeightLimitCaptured &&
                GameState
                    ->HasDefaultRedeployGliderHeightLimit())
            {
                GameState
                    ->DefaultRedeployGliderHeightLimit =
                    State.OriginalGliderHeightLimit;
            }
            if (State.bGliderLateralVelocityMultCaptured &&
                GameState
                    ->HasDefaultRedeployGliderLateralVelocityMult())
            {
                GameState
                    ->DefaultRedeployGliderLateralVelocityMult =
                    State.OriginalGliderLateralVelocityMult;
            }
            GameState->ForceNetUpdate();
        }
        State.bGliderRedeployCaptured = false;
        State.bGliderHeightLimitCaptured = false;
        State.bGliderLateralVelocityMultCaptured = false;
    }

    ApplyGliderRedeployPawnPolicy(
        GameMode,
        World,
        bGliderRedeployEnabled,
        State.DesiredGliderHeightLimit,
        State.DesiredGliderLateralVelocityMult);

    ApplyJumpFatiguePolicy(GameMode);
    // Infinite materials and ammo are enforced at their consumption sites so
    // inventory entries retain ordinary numeric counts. Do not mirror those
    // settings into the native bBuildFree/bInfiniteAmmo controller flags:
    // legacy clients such as 12.61 render those flags as sentinel HUD values
    // (an infinity icon and an extremely large material count).

    // Time of day is a live toggle. Legacy seasons expose a concrete manager;
    // modern seasons expose a contextual interface-backed manager, with the
    // streamed DaySequence actor retained as a last-resort fallback. Track the
    // resolved controller because it can be replaced without world travel.
    const float SafeDeltaSeconds =
        std::isfinite(DeltaSeconds) &&
            DeltaSeconds > 0.f
            ? (std::min)(DeltaSeconds, 1.f)
            : 0.f;
    State.TODMReapplySeconds += SafeDeltaSeconds;
    const bool bAutoPauseTODM =
        FConfiguration::bAutoPauseTODM.load(
            std::memory_order_acquire);
    auto TODMController =
        bAutoPauseTODM
            ? UFortKismetLibrary::
                GetTimeOfDayControllerCompat(World)
            : State.TODMController.Get();
    auto PreviousTODMController =
        State.TODMController.Get();
    if (bAutoPauseTODM &&
        PreviousTODMController != TODMController)
    {
        if (PreviousTODMController &&
            State.bTODMApplied &&
            State.bOriginalTODMSpeedCaptured)
        {
            UFortKismetLibrary::
                SetResolvedTimeOfDaySpeedCompat(
                    PreviousTODMController,
                    State.OriginalTODMSpeed);
        }
        State.TODMController =
            TODMController
                ? TWeakObjectPtr<UObject>(
                    TODMController)
                : TWeakObjectPtr<UObject>{};
        State.bTODMApplied = false;
        State.bOriginalTODMSpeedCaptured = false;
        State.LastTODMTime = -1.f;
        State.TODMReapplySeconds = 0.f;
        State.bLoggedTODMSuccess = false;
        State.bLoggedTODMFailure = false;
    }

    if (bAutoPauseTODM)
    {
        if (!State.bTODMApplied &&
            !State.bOriginalTODMSpeedCaptured)
        {
            float OriginalSpeed = 0.f;
            if (UFortKismetLibrary::
                    GetTimeOfDaySpeedCompat(
                        World, OriginalSpeed) &&
                std::isfinite(OriginalSpeed))
            {
                State.OriginalTODMSpeed =
                    OriginalSpeed;
                State.bOriginalTODMSpeedCaptured =
                    true;
            }
        }

        float DesiredTime =
            FConfiguration::TODMTime.load(
                std::memory_order_acquire);
        if (!std::isfinite(DesiredTime))
            DesiredTime = 7.f;
        DesiredTime =
            std::fmod(DesiredTime, 24.f);
        if (DesiredTime < 0.f)
            DesiredTime += 24.f;

        const bool bPeriodicReassert =
            State.TODMReapplySeconds >= 0.5f;
        bool bNeedsTimeSeek =
            !State.bTODMApplied ||
            std::fabs(
                State.LastTODMTime -
                DesiredTime) > 0.001f;

        // Do not seek every half-second. It can retrigger phase events on
        // DaySequence. Only seek again when the manager drifted, the slider
        // changed, or no streamed manager has resolved yet.
        if (!bNeedsTimeSeek &&
            bPeriodicReassert)
        {
            float CurrentTime = 0.f;
            if (!TODMController ||
                !UFortKismetLibrary::
                    GetTimeOfDayCompat(
                        World, CurrentTime) ||
                (std::min)(
                    std::fabs(
                        CurrentTime -
                        DesiredTime),
                    24.f - std::fabs(
                        CurrentTime -
                        DesiredTime)) >
                    0.02f)
            {
                bNeedsTimeSeek = true;
            }
        }

        bool bTimeApplied = true;
        if (bNeedsTimeSeek)
        {
            bTimeApplied =
                UFortKismetLibrary::
                    SetTimeOfDayCompat(
                        World, DesiredTime);
        }

        bool bPauseApplied = true;
        if (!State.bTODMApplied ||
            bNeedsTimeSeek ||
            bPeriodicReassert)
        {
            bPauseApplied =
                UFortKismetLibrary::
                    SetTimeOfDaySpeedCompat(
                        World, 0.f);
        }

        if (bTimeApplied && bPauseApplied)
        {
            State.bTODMApplied = true;
            if (bNeedsTimeSeek)
            {
                State.LastTODMTime =
                    DesiredTime;
            }
            if (!State.bLoggedTODMSuccess)
            {
                float ObservedTime = -1.f;
                float ObservedSpeed = -1.f;
                const bool bHasTimeReadback =
                    UFortKismetLibrary::
                        GetTimeOfDayCompat(
                            World, ObservedTime);
                const bool bHasSpeedReadback =
                    UFortKismetLibrary::
                        GetTimeOfDaySpeedCompat(
                            World, ObservedSpeed);
                SDK::DbgLog(
                    "[TimeOfDay] auto-pause applied "
                    "version=%.2f controller=%p name=%s "
                    "requested=%.2f observed=%s%.2f "
                    "speed=%s%.2f\n",
                    VersionInfo.FortniteVersion,
                    (void*)TODMController,
                    TODMController
                        ? TODMController->Name
                            .ToString().c_str()
                        : "static-fallback",
                    DesiredTime,
                    bHasTimeReadback ? "" : "unavailable/",
                    ObservedTime,
                    bHasSpeedReadback ? "" : "unavailable/",
                    ObservedSpeed);
                State.bLoggedTODMSuccess = true;
            }
        }
        else if (!State.bLoggedTODMFailure)
        {
            SDK::DbgLog(
                "[TimeOfDay] auto-pause pending "
                "version=%.2f controller=%p "
                "timeApplied=%d pauseApplied=%d\n",
                VersionInfo.FortniteVersion,
                (void*)TODMController,
                bTimeApplied ? 1 : 0,
                bPauseApplied ? 1 : 0);
            State.bLoggedTODMFailure = true;
        }

        if (bPeriodicReassert)
        {
            State.TODMReapplySeconds = 0.f;
        }
    }
    else if (State.bTODMApplied)
    {
        // Restore the actual authored speed when the build exposes a getter.
        // If it does not, stop enforcing rather than inventing 1.0 and
        // overwriting an event or playlist-owned day/night cycle.
        if (State.bOriginalTODMSpeedCaptured)
        {
            if (!UFortKismetLibrary::
                    SetResolvedTimeOfDaySpeedCompat(
                        State.TODMController.Get(),
                        State.OriginalTODMSpeed))
            {
                UFortKismetLibrary::
                    SetTimeOfDaySpeedCompat(
                        World,
                        State.OriginalTODMSpeed);
            }
        }
        State.bTODMApplied = false;
        State.bOriginalTODMSpeedCaptured = false;
        State.TODMController = {};
        State.LastTODMTime = -1.f;
        State.TODMReapplySeconds = 0.f;
        State.bLoggedTODMSuccess = false;
        State.bLoggedTODMFailure = false;
    }

    const bool bWarmupActive =
        GUI::gsStatus == Joinable &&
        (!GameState->HasGamePhase() ||
            GameState->GamePhase <
                (uint8)EAthenaGamePhase::Aircraft);
    const bool bSkipAircraft =
        Playlist && Playlist->HasbSkipAircraft() &&
        Playlist->bSkipAircraft;
    const bool bOwnsAutomaticBusSchedule =
        IsAuthoritativeBusPolicySelection();
    if (bWarmupActive && !bSkipAircraft &&
        FConfiguration::bStartBusRequested.exchange(
            false, std::memory_order_acq_rel))
    {
        // Repeated clicks must not continuously restart the same ten-second
        // manual countdown.
        if (!State.bManualBusRelease)
        {
            State.bManualBusRelease = true;
            State.BusPolicyMode = -1;
        }
    }

    if (bWarmupActive && !bSkipAircraft &&
        (bOwnsAutomaticBusSchedule ||
            State.bManualBusRelease))
    {
        const int32 DesiredBusMode =
            State.bManualBusRelease
                ? 2
                : (FConfiguration::bAutoBusStart.load()
                    ? 1
                    : 0);
        const float DesiredDelay =
            DesiredBusMode == 2
                ? 10.f
                : (DesiredBusMode == 1
                    ? std::clamp(
                        FConfiguration::BusStartDelay.load(),
                        0.f, 300.f)
                    : 99999999.f);
        const float CurrentTime =
            (float)UGameplayStatics::GetTimeSeconds(
                World);
        static const FName InProgress(L"InProgress");
        const bool bCanStartAircraft =
            GameMode->MatchState == InProgress &&
            GameMode->AlivePlayers.Num() > 0;
        const bool bPolicyChanged =
            State.BusPolicyMode != DesiredBusMode ||
            std::fabs(
                State.BusPolicyDelay -
                    DesiredDelay) > 0.01f;
        if (bPolicyChanged)
        {
            State.BusPolicyMode = DesiredBusMode;
            State.BusPolicyDelay = DesiredDelay;
            State.bBusCountdownArmed = false;
            State.bAircraftStartRequested = false;
            State.bLoggedBlockedAircraftStart = false;
            State.BusNextStartAttemptTime = 0.f;
            State.BusScheduleStartTime = CurrentTime;
            State.BusScheduleEndTime =
                CurrentTime + 99999999.f;
        }

        if (DesiredBusMode != 0 &&
            bCanStartAircraft &&
            !State.bBusCountdownArmed)
        {
            State.bBusCountdownArmed = true;
            State.bAircraftStartRequested = false;
            State.BusNextStartAttemptTime = 0.f;
            State.BusScheduleStartTime = CurrentTime;
            State.BusScheduleEndTime =
                CurrentTime + DesiredDelay;
            SDK::DbgLog(
                "[GameplayPolicy] bus countdown armed mode=%d "
                "delay=%.2f FN=%.2f\n",
                DesiredBusMode, DesiredDelay,
                VersionInfo.FortniteVersion);
        }
        else if ((!bCanStartAircraft ||
            DesiredBusMode == 0) &&
            State.bBusCountdownArmed)
        {
            State.bBusCountdownArmed = false;
            State.bAircraftStartRequested = false;
            State.BusNextStartAttemptTime = 0.f;
            State.BusScheduleStartTime = CurrentTime;
            State.BusScheduleEndTime =
                CurrentTime + 99999999.f;
        }

        const float AppliedDuration =
            State.bBusCountdownArmed
                ? DesiredDelay
                : 99999999.f;

        auto PhaseLogic =
            ResolveGameplayPolicyPhaseLogic(World);
        ApplyWarmupSchedule(
            GameMode, GameState, PhaseLogic,
            State.BusScheduleStartTime,
            State.BusScheduleEndTime,
            AppliedDuration);

        // The legacy NetDriver auto-start branch begins at Season 11. Fill the
        // earlier supported versions from this same authoritative schedule.
        // Do not consume the one-shot request while the native GameMode still
        // has no real passenger; otherwise it can never retry after a join.
        if (VersionInfo.FortniteVersion < 11.00 &&
            DesiredBusMode != 0 &&
            bCanStartAircraft &&
            State.bBusCountdownArmed &&
            CurrentTime >= State.BusScheduleEndTime &&
            CurrentTime >=
                State.BusNextStartAttemptTime)
        {
            State.bAircraftStartRequested = true;
            State.BusNextStartAttemptTime =
                CurrentTime + 1.f;
            UKismetSystemLibrary::ExecuteConsoleCommand(
                World, FString(L"startaircraft"), nullptr);
        }
    }
    else
    {
        State.BusPolicyMode = -1;
        State.BusPolicyDelay = -1.f;
        State.bBusCountdownArmed = false;
        State.bAircraftStartRequested = false;
        State.bLoggedBlockedAircraftStart = false;
        State.BusNextStartAttemptTime = 0.f;
        if (!bWarmupActive)
            State.bManualBusRelease = false;
    }
}

void AFortGameMode::TickPendingVehicleSpawns()
{
    if (!UsesDeferredVehicleSpawnDiscovery() ||
        !GDeferredVehicleSpawnState.Started ||
        !GDeferredVehicleSpawnState.Active)
    {
        return;
    }

    auto* World = UWorld::GetWorld();
    if (!World ||
        GDeferredVehicleSpawnState.World.Get() != World)
    {
        GDeferredVehicleSpawnState.Active = false;
        return;
    }

    const ULONGLONG CurrentTimeMs = GetTickCount64();
    if (CurrentTimeMs <
        GDeferredVehicleSpawnState.NextAttemptTimeMs)
    {
        return;
    }
    const bool FinalPass =
        CurrentTimeMs >=
            GDeferredVehicleSpawnState.DeadlineTimeMs;
    RunVehicleSpawnDiscoveryPass();
    if (FinalPass)
    {
        GDeferredVehicleSpawnState.Active = false;
        SDK::DbgLog(
            "[VehicleSpawn] discovery complete passes=%u "
            "spawned=%d processed=%d\n",
            GDeferredVehicleSpawnState.PassCount,
            GDeferredVehicleSpawnState.TotalSpawned,
            static_cast<int32>(
                GDeferredVehicleSpawnState
                    .ProcessedSources.size()));
        return;
    }

    GDeferredVehicleSpawnState.NextAttemptTimeMs =
        CurrentTimeMs + VehicleSpawnRetryIntervalMs;
}

namespace
{
    struct FManagedPlaylistStreamingState
    {
        UWorld* World = nullptr;
        const UFortPlaylistAthena* Playlist = nullptr;
        ULONGLONG FirstObservedTimeMs = 0;
        ULONGLONG NextRetryTimeMs = 0;
        bool bLoggedInvalidTracker = false;
        bool bLoggedExternalFallback = false;
        std::vector<FName> LevelNamesByTrackerIndex;
        std::vector<ULevelStreamingDynamic*> StreamingObjectsByTrackerIndex;
    };

    FManagedPlaylistStreamingState GManagedPlaylistStreamingState;

    bool TryGetPlaylistLevelName(
        const FSoftObjectPtr& SoftLevel,
        FName& OutLevelName)
    {
        OutLevelName = FName();
        if (VersionInfo.FortniteVersion < 23)
        {
            if (!SoftLevel.ObjectID.AssetPathName.IsValid())
                return false;
            OutLevelName = SoftLevel.ObjectID.AssetPathName;
            return true;
        }

        // UE5 split FSoftObjectPath into package/asset names and UE5.3 also
        // moved those fields. Reading ObjectID.AssetPathName on 27.11 reads
        // the weak-pointer bytes instead of the Durian level path.
        const uint8* Value =
            reinterpret_cast<const uint8*>(&SoftLevel);
        const uint32 PackageOffset =
            VersionInfo.EngineVersion < 5.3 ? 0x10 : 0x08;
        const uint32 AssetOffset =
            VersionInfo.EngineVersion < 5.3 ? 0x14 : 0x0C;
        if (!SDK::MemReadable(
                Value + PackageOffset, sizeof(FName)) ||
            !SDK::MemReadable(
                Value + AssetOffset, sizeof(FName)))
        {
            return false;
        }

        const auto& PackageName =
            *reinterpret_cast<const FName*>(
                Value + PackageOffset);
        const auto& AssetName =
            *reinterpret_cast<const FName*>(
                Value + AssetOffset);
        if (!PackageName.IsValid())
            return false;

        auto FullPath = PackageName.ToWString();
        if (AssetName.IsValid())
        {
            FullPath += L".";
            FullPath += AssetName.ToWString();
        }
        if (FullPath.empty() || FullPath[0] != L'/')
            return false;

        OutLevelName = FName(FullPath.c_str());
        return OutLevelName.IsValid();
    }

    bool IsSaneReflectedArray(
        const void* Data,
        int32 Num,
        int32 Max,
        int32 ElementSize,
        int32 MaximumElements)
    {
        if (Num < 0 || Max < Num || Max > MaximumElements ||
            ElementSize <= 0)
        {
            return false;
        }

        if (Max == 0)
            return Data == nullptr;

        return Data &&
            SDK::MemReadable(
                Data,
                static_cast<size_t>(Max) *
                    static_cast<size_t>(ElementSize));
    }

    bool GetInternalPlaylistStreamingTracker(
        AFortGameStateAthena* GameState,
        const TArray<FPlaylistStreamedLevelData>** OutTracker,
        int32* OutElementSize)
    {
        if (OutTracker)
            *OutTracker = nullptr;
        if (OutElementSize)
            *OutElementSize = 0;
        if (!GameState ||
            !GameState->HasAdditionalPlaylistLevelsStreamed())
        {
            return false;
        }

        const int32 ReplicatedLevelsOffset =
            GameState->GetOffset(
                "AdditionalPlaylistLevelsStreamed");
        int32 TrackerOffset =
            GameState->GetOffset("AdditionalPlaylistLevels");
        const UStruct* TrackerStruct =
            FPlaylistStreamedLevelData::StaticStruct();
        if (ReplicatedLevelsOffset < 0x10 || !TrackerStruct ||
            !FPlaylistStreamedLevelData::
                HasbIsFinishedStreaming())
        {
            return false;
        }

        const int32 ElementSize =
            TrackerStruct->GetPropertiesSize();
        if (ElementSize < static_cast<int32>(sizeof(void*)) ||
            ElementSize > 0x100)
        {
            return false;
        }

        // This tracker is owned by Fortnite's asynchronous playlist loader.
        // Some older dumps omit its property even though the field is adjacent
        // to the replicated array. The adjacent fallback is observation-only:
        // callers receive a const array and must never add, remove, clear, or
        // update entries that a native async completion may still reference.
        const bool bKnownAdjacentTrackerLayout =
            std::fabs(
                VersionInfo.FortniteVersion - 9.40) < 0.001 ||
            std::fabs(
                VersionInfo.FortniteVersion - 9.41) < 0.001 ||
            std::fabs(
                VersionInfo.FortniteVersion - 18.40) < 0.001 ||
            std::fabs(
                VersionInfo.FortniteVersion - 27.11) < 0.001;
        const bool bUsingAdjacentTracker =
            TrackerOffset < 0 && bKnownAdjacentTrackerLayout;
        if (bUsingAdjacentTracker)
            TrackerOffset = ReplicatedLevelsOffset - 0x10;
        const int32 GameStateSize =
            GameState->Class
                ? GameState->Class->GetPropertiesSize()
                : 0;
        if (TrackerOffset < 0 ||
            TrackerOffset == ReplicatedLevelsOffset ||
            GameStateSize <= 0 ||
            TrackerOffset >
                GameStateSize -
                    static_cast<int32>(sizeof(TArray<void*>)) ||
            ReplicatedLevelsOffset >
                GameStateSize -
                    static_cast<int32>(sizeof(TArray<void*>)) ||
            (bUsingAdjacentTracker &&
             TrackerOffset +
                     static_cast<int32>(sizeof(TArray<void*>)) !=
                 ReplicatedLevelsOffset))
        {
            return false;
        }

        const auto Tracker =
            reinterpret_cast<const TArray<FPlaylistStreamedLevelData>*>(
                reinterpret_cast<uint8*>(GameState) +
                TrackerOffset);
        if (!SDK::MemReadable(Tracker, sizeof(*Tracker)) ||
            !IsSaneReflectedArray(
                Tracker->Data,
                Tracker->Num(),
                Tracker->Max(),
                ElementSize,
                256))
        {
            return false;
        }

        if (OutTracker)
            *OutTracker = Tracker;
        if (OutElementSize)
            *OutElementSize = ElementSize;
        return true;
    }

    bool EnsureReplicatedPlaylistLevel(
        AFortGameStateAthena* GameState,
        const FName& LevelName,
        bool bServerOnly,
        bool* OutAdded)
    {
        if (OutAdded)
            *OutAdded = false;
        if (!GameState || !LevelName.IsValid() ||
            !GameState->HasAdditionalPlaylistLevelsStreamed())
        {
            return false;
        }

        auto& StreamedLevels =
            GameState->AdditionalPlaylistLevelsStreamed;
        const UStruct* AdditionalLevelStruct =
            FAdditionalLevelStreamed::StaticStruct();
        if (!AdditionalLevelStruct)
        {
            auto& LegacyLevels =
                reinterpret_cast<TArray<FName>&>(
                    StreamedLevels);
            if (!IsSaneReflectedArray(
                    LegacyLevels.Data,
                    LegacyLevels.Num(),
                    LegacyLevels.Max(),
                    static_cast<int32>(sizeof(FName)),
                    256))
            {
                return false;
            }

            for (int32 Index = 0;
                 Index < LegacyLevels.Num(); ++Index)
            {
                if (LegacyLevels[Index] == LevelName)
                    return true;
            }

            FName MutableLevelName = LevelName;
            LegacyLevels.Add(MutableLevelName);
            if (OutAdded)
                *OutAdded = true;
            return true;
        }

        const int32 ElementSize =
            AdditionalLevelStruct->GetPropertiesSize();
        if (ElementSize <= 0 || ElementSize > 0x100 ||
            !FAdditionalLevelStreamed::HasLevelName() ||
            !FAdditionalLevelStreamed::HasbIsServerOnly() ||
            !IsSaneReflectedArray(
                StreamedLevels.Data,
                StreamedLevels.Num(),
                StreamedLevels.Max(),
                ElementSize,
                256))
        {
            return false;
        }

        for (int32 Index = 0;
             Index < StreamedLevels.Num(); ++Index)
        {
            auto& Existing =
                StreamedLevels.Get(Index, ElementSize);
            if (Existing.LevelName == LevelName)
                return true;
        }

        void* Memory = FMemory::Malloc(ElementSize);
        if (!Memory)
            return false;
        memset(Memory, 0, ElementSize);

        auto NewLevel =
            static_cast<FAdditionalLevelStreamed*>(Memory);
        FName MutableLevelName = LevelName;
        NewLevel->LevelName = MutableLevelName;
        NewLevel->bIsServerOnly = bServerOnly;
        StreamedLevels.Add(*NewLevel, ElementSize);
        FMemory::Free(Memory);
        if (OutAdded)
            *OutAdded = true;
        return true;
    }

    bool MaintainManagedPlaylistLevels(
        AFortGameStateAthena* GameState,
        const UFortPlaylistAthena* Playlist,
        bool bForceRequest)
    {
        UWorld* World = UWorld::GetWorld();
        if (!World || !GameState || !Playlist)
            return false;

        auto ValidateSoftLevelArray =
            [](const TArray<TSoftObjectPtr<UWorld>>& Levels)
            {
                return IsSaneReflectedArray(
                    Levels.Data,
                    Levels.Num(),
                    Levels.Max(),
                    static_cast<int32>(FSoftObjectPtr::Size()),
                    128);
            };

        const bool bHasClientLevels =
            Playlist->HasAdditionalLevels();
        const bool bHasServerLevels =
            Playlist->HasAdditionalLevelsServerOnly();
        if ((bHasClientLevels &&
             !ValidateSoftLevelArray(
                 Playlist->AdditionalLevels)) ||
            (bHasServerLevels &&
             !ValidateSoftLevelArray(
                 Playlist->AdditionalLevelsServerOnly)))
        {
            return false;
        }

        const int32 ClientLevelCount =
            bHasClientLevels
                ? Playlist->AdditionalLevels.Num()
                : 0;
        const int32 ServerLevelCount =
            bHasServerLevels
                ? Playlist->AdditionalLevelsServerOnly.Num()
                : 0;
        const int32 DeclaredLevelCount =
            ClientLevelCount + ServerLevelCount;
        if (DeclaredLevelCount == 0)
            return true;

        const TArray<FPlaylistStreamedLevelData>* Tracker = nullptr;
        int32 TrackerElementSize = 0;
        const bool bHasNativeTracker =
            GetInternalPlaylistStreamingTracker(
                GameState, &Tracker, &TrackerElementSize);

        const bool bNewPlaylist =
            GManagedPlaylistStreamingState.World != World ||
            GManagedPlaylistStreamingState.Playlist != Playlist;
        if (bNewPlaylist)
        {
            GManagedPlaylistStreamingState = {};
            GManagedPlaylistStreamingState.World = World;
            GManagedPlaylistStreamingState.Playlist = Playlist;
            GManagedPlaylistStreamingState.FirstObservedTimeMs =
                GetTickCount64();
        }

        const ULONGLONG CurrentTimeMs = GetTickCount64();
        const bool bAllowExternalFallback =
            std::fabs(
                VersionInfo.FortniteVersion - 27.11) < 0.001;
        const bool bFallbackGraceExpired =
            CurrentTimeMs >=
                GManagedPlaylistStreamingState
                    .FirstObservedTimeMs + 8000ULL;
        const bool bMayRequest =
            bAllowExternalFallback &&
            bFallbackGraceExpired &&
            (bForceRequest ||
             CurrentTimeMs >=
                 GManagedPlaylistStreamingState
                     .NextRetryTimeMs);
        if (!bHasNativeTracker &&
            !GManagedPlaylistStreamingState.bLoggedInvalidTracker)
        {
            GManagedPlaylistStreamingState.bLoggedInvalidTracker = true;
            SDK::DbgLog(
                "[PlaylistLevels] native tracker unavailable; "
                "%s\n",
                bAllowExternalFallback
                    ? "waiting before isolated 27.11 fallback"
                    : "leaving playlist streaming to Fortnite");
        }
        bool bNeedsRetry = false;
        bool bAddedReplicatedLevel = false;
        bool bAllRequestsReady = true;
        int32 TrackerIndex = 0;

        auto MaintainLevel =
            [&](TSoftObjectPtr<UWorld>& Level,
                bool bServerOnly)
            {
                const size_t MappingIndex =
                    static_cast<size_t>(TrackerIndex++);
                FName LevelName;
                if (!TryGetPlaylistLevelName(Level, LevelName))
                {
                    bAllRequestsReady = false;
                    bNeedsRetry = true;
                    return;
                }

                if (GManagedPlaylistStreamingState
                        .LevelNamesByTrackerIndex.size() <= MappingIndex)
                {
                    GManagedPlaylistStreamingState
                        .LevelNamesByTrackerIndex.resize(MappingIndex + 1);
                    GManagedPlaylistStreamingState
                        .StreamingObjectsByTrackerIndex.resize(
                            MappingIndex + 1, nullptr);
                }
                auto& TrackedLevelName =
                    GManagedPlaylistStreamingState
                        .LevelNamesByTrackerIndex[MappingIndex];
                auto& ManagedStreamingLevel =
                    GManagedPlaylistStreamingState
                        .StreamingObjectsByTrackerIndex[MappingIndex];
                if (!TrackedLevelName.IsValid() ||
                    TrackedLevelName != LevelName)
                {
                    // This is DLL-owned fallback state only. Never clear or
                    // repurpose Fortnite's engine-owned tracker entry.
                    TrackedLevelName = LevelName;
                    ManagedStreamingLevel = nullptr;
                }

                const UClass* StreamingClass =
                    ULevelStreamingDynamic::StaticClass();
                if (ManagedStreamingLevel &&
                    (!StreamingClass ||
                     !IsSaneObject(ManagedStreamingLevel) ||
                     !ManagedStreamingLevel->IsA(StreamingClass)))
                {
                    ManagedStreamingLevel = nullptr;
                }

                ULevelStreamingDynamic* NativeStreamingLevel = nullptr;
                if (bHasNativeTracker &&
                    MappingIndex <
                        static_cast<size_t>(Tracker->Num()))
                {
                    const auto& NativeEntry =
                        Tracker->Get(
                            static_cast<int32>(MappingIndex),
                            TrackerElementSize);
                    auto Candidate = NativeEntry.StreamingLevel;
                    if (Candidate && StreamingClass &&
                        IsSaneObject(Candidate) &&
                        Candidate->IsA(StreamingClass))
                    {
                        NativeStreamingLevel = Candidate;
                    }
                }

                auto StreamingLevel = NativeStreamingLevel
                    ? NativeStreamingLevel
                    : ManagedStreamingLevel;
                if (!StreamingLevel && bMayRequest)
                {
                    if (!GManagedPlaylistStreamingState
                             .bLoggedExternalFallback)
                    {
                        GManagedPlaylistStreamingState
                            .bLoggedExternalFallback = true;
                        SDK::DbgLog(
                            "[PlaylistLevels] using isolated 27.11 "
                            "streaming fallback after native grace period\n");
                    }
                    bool bSuccess = false;
                    StreamingLevel =
                        ULevelStreamingDynamic::
                            LoadLevelInstanceBySoftObjectPtr(
                                World,
                                Level,
                                FVector(),
                                FRotator(),
                                &bSuccess,
                                FString(),
                                nullptr);
                    if (!bSuccess || !StreamingLevel ||
                        !IsSaneObject(StreamingLevel))
                    {
                        StreamingLevel = nullptr;
                        bNeedsRetry = true;
                        SDK::DbgLog(
                            "[PlaylistLevels] request failed: %s\n",
                            LevelName.ToString().c_str());
                    }
                    ManagedStreamingLevel = StreamingLevel;
                }

                if (!StreamingLevel)
                {
                    // 18.40 and loader-backed legacy events stay entirely on
                    // Fortnite's native/Blueprint pipeline. If a native entry
                    // exists, the read-only visibility pass below will hold
                    // readiness; otherwise event startup retries its actors.
                    if (bAllowExternalFallback)
                    {
                        bAllRequestsReady = false;
                        bNeedsRetry = true;
                    }
                    return;
                }

                if (!NativeStreamingLevel)
                {
                    bool bAdded = false;
                    if (!EnsureReplicatedPlaylistLevel(
                            GameState,
                            LevelName,
                            bServerOnly,
                            &bAdded))
                    {
                        bAllRequestsReady = false;
                        bNeedsRetry = true;
                        return;
                    }
                    bAddedReplicatedLevel |= bAdded;
                }

                if (!StreamingLevel->HasLoadedLevel() ||
                    !IsSaneObject(StreamingLevel->LoadedLevel) ||
                    !StreamingLevel->LoadedLevel->HasbIsVisible() ||
                    !StreamingLevel->LoadedLevel->bIsVisible)
                {
                    bAllRequestsReady = false;
                }
            };

        if (bHasClientLevels)
        {
            for (int32 Index = 0;
                 Index < ClientLevelCount; ++Index)
            {
                auto& Level =
                    Playlist->AdditionalLevels.Get(
                        Index,
                        FSoftObjectPtr::Size());
                MaintainLevel(Level, false);
            }
        }
        if (bHasServerLevels)
        {
            for (int32 Index = 0;
                 Index < ServerLevelCount; ++Index)
            {
                auto& Level =
                    Playlist->AdditionalLevelsServerOnly.Get(
                        Index,
                        FSoftObjectPtr::Size());
                MaintainLevel(Level, true);
            }
        }


        GManagedPlaylistStreamingState
            .LevelNamesByTrackerIndex.resize(
                static_cast<size_t>(DeclaredLevelCount));
        GManagedPlaylistStreamingState
            .StreamingObjectsByTrackerIndex.resize(
                static_cast<size_t>(DeclaredLevelCount));

        if (bAddedReplicatedLevel)
            GameState->ForceNetUpdate();
        if (bNeedsRetry && bMayRequest)
        {
            GManagedPlaylistStreamingState.NextRetryTimeMs =
                CurrentTimeMs + 1000ULL;
        }
        return bAllRequestsReady;
    }

}

void AFortGameMode::ReadyToStartMatch_(UObject* Context, FFrame& Stack, bool* Ret)
{
    Stack.IncrementCode();

    static bool _rtsmLogged = false;
    if (!_rtsmLogged) { _rtsmLogged = true; SDK::DbgLog("[GameMode] ReadyToStartMatch_ FIRST FIRE Context=%p\n", Context); }

    static auto FrontendMode = FindClass("FortGameModeFrontend");
    if (Context->IsA(FrontendMode))
    {
        *Ret = callOGWithRet(((AFortGameMode*)Context), Stack.GetCurrentNativeFunction(), ReadyToStartMatch);
        return;
    }
    auto GameMode = Context->Cast<AFortGameMode>();

    auto GameState = GameMode->GameState;
    if (!GameState)
    {
        *Ret = false;
        return;
    }
	auto World = UWorld::GetWorld();

	// A reused UWorld has no second listen boundary. Its lifecycle tick keeps
	// retrying the same strict moving-zone preflight while this native readiness
	// hook holds the restarted match in pregame for visible editor correction.
	// Scope the latch to the exact world/GameMode/GameState generation so travel
	// can never inherit a rejected restart's hold.
	if (CustomSafeZoneRuntime::IsMatchRestartPreflightPending(World))
	{
		*Ret = false;
		return;
	}

    // Pumped here as well as from TickFlush: this runs from the moment the
    // athena game mode has a game state, so the Calendar tab's snow value is
    // already being applied through warmup rather than only once the net
    // driver starts ticking.
    Calendar::TickSnow();

    // Apply before native match setup can create or schedule a drop. The same
    // helper is also ticked later so suppression stays enforced on builds with
    // component-owned or mutator-owned scheduling.
    TickSupplyDropSuppression();

    static bool setup = false;
    static bool bListenSucceeded = false;
    static TWeakObjectPtr<UWorld> SetupWorld;
    static ULONGLONG NextListenRetryTimeMs = 0;
    static ULONGLONG MovingZonePreflightPendingSinceMs = 0;
    if (SetupWorld.Get() != World)
    {
        SetupWorld = World
            ? TWeakObjectPtr<UWorld>(World)
            : TWeakObjectPtr<UWorld>{};
        setup = false;
        bListenSucceeded = false;
        NextListenRetryTimeMs = 0;
        MovingZonePreflightPendingSinceMs = 0;
    }

    auto InitializeListenDriver =
        [&](UNetDriver* NetDriver) -> bool
    {
        if (!World || !NetDriver)
            return false;

        auto NetDriverName = FName(L"GameNetDriver");
        if (VersionInfo.FortniteVersion >= 20)
        {
            NetDriver->NetServerMaxTickRate = static_cast<int32>(
                FConfiguration::GetClampedMaxTickRate());
        }
        NetDriver->NetDriverName = NetDriverName;
        NetDriver->World = World;

        if (VersionInfo.EngineVersion >= 5.3 &&
            FConfiguration::bEnableIris)
        {
            *(bool*)(__int64(&NetDriver->ReplicationDriver) + 0x11) = true;
        }

        const int32 LevelCollectionCount =
            World->LevelCollections.Num();
        for (int32 Index = 0;
            Index < LevelCollectionCount && Index < 64;
            ++Index)
        {
            auto& LevelCollection =
                World->LevelCollections.Get(
                    Index, FLevelCollection::Size());
            LevelCollection.NetDriver = NetDriver;
        }

        auto InitListen =
            (bool (*)(UNetDriver*, UWorld*, FURL*, bool, FString&))
                FindInitListen();
        auto SetWorld =
            (void (*)(UNetDriver*, UWorld*))FindSetWorld();
        if (!InitListen || !SetWorld)
            return false;

        size_t URLSize = FURL::Size();
        if (URLSize == 0 || URLSize > 0x1000)
            return false;
        auto URL = (FURL*)malloc(URLSize);
        if (!URL)
            return false;
        memset((PBYTE)URL, 0, URLSize);
        URL->Port =
            FConfiguration::Port.load(std::memory_order_relaxed);

        SDK::DbgLog(
            "[GameMode] Listen: InitListen=%p SetWorld=%p Port=%d\n",
            (void*)InitListen,
            (void*)SetWorld,
            FConfiguration::Port.load(std::memory_order_relaxed));
        SetWorld(NetDriver, World);
        FString Err;
        const bool bInitialized =
            InitListen(NetDriver, World, URL, false, Err);
        free(URL);
        if (bInitialized)
            SetWorld(NetDriver, World);
        return bInitialized;
    };

    const ULONGLONG CurrentListenTimeMs = GetTickCount64();
    if (setup && !bListenSucceeded && World)
    {
        if (CurrentListenTimeMs >= NextListenRetryTimeMs)
        {
            if (World->NetDriver)
            {
                bListenSucceeded = InitializeListenDriver(
                    static_cast<UNetDriver*>(World->NetDriver));
            }
            else
            {
                // Some native InitListen failure paths remove the partially
                // initialized driver. Re-enter creation instead of leaving
                // this world permanently stuck behind the setup sentinel.
                setup = false;
            }
            NextListenRetryTimeMs = CurrentListenTimeMs + 2000ULL;
            SDK::DbgLog(
                "[GameMode] Listen retry returned %d recreate=%d\n",
                (int)bListenSucceeded,
                (int)!setup);
        }
    }

    // Do not use WarmupRequiredPlayerCount as a setup sentinel. Newer builds
    // can initialize it to one natively, which skipped server creation forever.
    if (!setup && CurrentListenTimeMs >= NextListenRetryTimeMs)
    {
		// An exact phase capacity does not exist in the frontend. Validate against
		// the live Athena map and playlist here, before a NetDriver is created or
		// any native phase geometry is touched. Invalid drafts return control to
		// the still-live editor; pending MapInfo simply retries next frame.
		if (!FConfiguration::bReadyToStart.load(
				std::memory_order_acquire))
		{
			*Ret = false;
			return;
		}
		if (CustomSafeZoneRuntime::IsMovingModeRequested())
		{
			auto AthenaGameState = GameState &&
				GameState->IsA(
					AFortGameStateAthena::StaticClass())
				? (AFortGameStateAthena*)GameState
				: nullptr;
			auto MapInfo = AthenaGameState &&
				AthenaGameState->HasMapInfo()
					? AthenaGameState->MapInfo
					: nullptr;
			if (MovingZonePreflightPendingSinceMs == 0)
			{
				MovingZonePreflightPendingSinceMs =
					CurrentListenTimeMs;
			}
			const bool bPreflightTimedOut =
				MovingZonePreflightPendingSinceMs != 0 &&
				CurrentListenTimeMs -
					MovingZonePreflightPendingSinceMs >= 15000ULL;
			const int32 PhaseCapacity = MapInfo
				? AFortGameMode::ResolveMovingSafeZonePreflightCapacity(
					GameMode, MapInfo)
				: 0;
			const auto Preflight =
				CustomSafeZoneRuntime::PreflightForServerStart(
					World, GameMode, MapInfo, PhaseCapacity,
					bPreflightTimedOut);
			if (Preflight ==
				ECustomSafeZonePreflightResult::Pending)
			{
				*Ret = false;
				return;
			}
			if (Preflight ==
				ECustomSafeZonePreflightResult::Invalid)
			{
				// Travel, playlist, and transport settings are already committed.
				// Keep that launch transaction intact; the GUI selectively unlocks
				// only this sequence until the next preflight succeeds.
				*Ret = false;
				return;
			}
			MovingZonePreflightPendingSinceMs = 0;
		}
		else
		{
			MovingZonePreflightPendingSinceMs = 0;
		}

        setup = true;

        //if (!FindListenCall())
        {
            SDK::DbgLog("[GameMode] ReadyToStart Listen: ENTER setup=%d\n", (int)setup);
            auto Engine = UEngine::GetEngine();
            auto NetDriverName = FName(L"GameNetDriver");

            if (!World || !Engine)
            {
                setup = false;
                NextListenRetryTimeMs =
                    CurrentListenTimeMs + 2000ULL;
                *Ret = false;
                return;
            }

            if (GameMode->HasbEnableReplicationGraph())
                GameMode->bEnableReplicationGraph = true;

            UNetDriver* NetDriver = nullptr;
            if (VersionInfo.FortniteVersion >= 16.00)
            {
                auto GetWorldContext =
                    (void* (*)(UEngine*, UWorld*))FindGetWorldContext();
                auto CreateNetDriver =
                    (UNetDriver * (*)(UEngine*, void*, FName, int))
                        FindCreateNetDriverWorldContext();
                SDK::DbgLog("[GameMode] Listen: GetWorldContext=%p CreateNetDriver=%p\n",
                    (void*)GetWorldContext, (void*)CreateNetDriver);
                void* WorldCtx = GetWorldContext
                    ? GetWorldContext(Engine, World)
                    : nullptr;
                if (WorldCtx && CreateNetDriver)
                {
                    World->NetDriver = NetDriver =
                        CreateNetDriver(
                            Engine, WorldCtx, NetDriverName, 0);
                }
                SDK::DbgLog("[GameMode] Listen: WorldCtx=%p NetDriver=%p\n", WorldCtx, (void*)NetDriver);
            }
            else
            {
                auto CreateNetDriver =
                    (UNetDriver * (*)(UEngine*, UWorld*, FName))
                        FindCreateNetDriver();
                if (CreateNetDriver)
                {
                    World->NetDriver = NetDriver =
                        CreateNetDriver(Engine, World, NetDriverName);
                }
            }
            if (!NetDriver)
            {
                SDK::DbgLog("[GameMode] Listen: NetDriver NULL, abort\n");
                setup = false;
                NextListenRetryTimeMs =
                    CurrentListenTimeMs + 2000ULL;
                *Ret = false;
                return;
            }
            bListenSucceeded = InitializeListenDriver(NetDriver);
            SDK::DbgLog("[GameMode] Listen: InitListen returned %d\n", (int)bListenSucceeded);
            if (!bListenSucceeded)
            {
                printf("Failed to listen!");
                NextListenRetryTimeMs = GetTickCount64() + 2000ULL;
            }

            if (!FConfiguration::bReadyToStart)
            {
                while (!FConfiguration::bReadyToStart)
                {
                    Sleep(100);
                }
            }
        }

        if (GameMode->HasWarmupRequiredPlayerCount())
            GameMode->WarmupRequiredPlayerCount = 1;

        if (VersionInfo.FortniteVersion > 4.0 /*&& (VersionInfo.EngineVersion != 4.25)*/)
            SetupPlaylist(GameMode, GameState);

        auto Playlist = FindObject<UFortPlaylistAthena>(FConfiguration::Playlist);

        if (!Playlist)
            Playlist = FindObject<UFortPlaylistAthena>(L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo");

        bool bHeistPlaylist = false;
        bool bNative1040LTMPlaylist = false;
        bool bManagedPlaylistStreamingSetup = false;
        if (Playlist)
        {
            auto AdditionalPlaylistLevelsStreamed__Off = GameState->GetOffset("AdditionalPlaylistLevelsStreamed");
            auto AdditionalLevelStruct = FAdditionalLevelStreamed::StaticStruct();

            bHeistPlaylist =
                FFortAthenaHeistCompatibility::
                    IsHeistPlaylist(Playlist);
            bNative1040LTMPlaylist =
                FFortAthenaNativeLTMCompatibility::
                    IsTargetPlaylist(Playlist);
            if (bNative1040LTMPlaylist)
            {
                // The requested 10.40 assets declare no AdditionalLevels.
                // Their reflected field is directly TArray<FName>; the
                // generic legacy branch below subtracts 0x10 and would free
                // unrelated GameState memory on this build. Playlist data
                // publication owns this empty native array instead.
            }
            else if (bHeistPlaylist)
            {
                // The guarded Heist loader owns both normal and server-only
                // level arrays for 5.40-6.00. A failed first attempt is
                // retried from Tick; still publish the current (possibly
                // empty) list instead of falling into the legacy branch that
                // does not actually request its levels.
                if (!FFortAthenaHeistCompatibility::
                        LoadAdditionalPlaylistLevels(
                            GameState, Playlist))
                {
                    GameState->OnRep_AdditionalPlaylistLevelsStreamed();
                }
            }
            else if (FConfiguration::IsKnownS27CustomMapPlaylist())
            {
                if (Playlist->HasAdditionalLevels())
                {
                    for (auto& Level : Playlist->AdditionalLevels)
                    {
                        bool Success = false;
                        ULevelStreamingDynamic::LoadLevelInstanceBySoftObjectPtr(UWorld::GetWorld(), Level, FVector(), FRotator(), &Success, FString(), nullptr);
                        if (AdditionalLevelStruct)
                        {
                            auto level = (FAdditionalLevelStreamed*)malloc(FAdditionalLevelStreamed::Size());
                            memset((PBYTE)level, 0, FAdditionalLevelStreamed::Size());
                            level->bIsServerOnly = false;
                            level->LevelName = Level.ObjectID.AssetPathName;
                            if (Success)
                                GameState->AdditionalPlaylistLevelsStreamed.Add(*level, FAdditionalLevelStreamed::Size());
                            free(level);
                        }
                        else
                            GetFromOffset<TArray<FName>>(GameState, AdditionalPlaylistLevelsStreamed__Off).Add(Level.ObjectID.AssetPathName);
                    }
                }

                if (Playlist->HasAdditionalLevelsServerOnly())
                {
                    for (auto& Level : Playlist->AdditionalLevelsServerOnly)
                    {
                        bool Success = false;
                        ULevelStreamingDynamic::LoadLevelInstanceBySoftObjectPtr(UWorld::GetWorld(), Level, FVector(), FRotator(), &Success, FString(), nullptr);

                        if (AdditionalLevelStruct)
                        {

                            auto level = (FAdditionalLevelStreamed*)malloc(FAdditionalLevelStreamed::Size());
                            memset((PBYTE)level, 0, FAdditionalLevelStreamed::Size());
                            level->bIsServerOnly = true;
                            level->LevelName = Level.ObjectID.AssetPathName;
                            if (Success)
                                GameState->AdditionalPlaylistLevelsStreamed.Add(*level, FAdditionalLevelStreamed::Size());
                            free(level);
                        }
                        else
                            GetFromOffset<TArray<FName>>(GameState, AdditionalPlaylistLevelsStreamed__Off).Add(Level.ObjectID.AssetPathName);
                    }
                }
            }
            else if (AdditionalPlaylistLevelsStreamed__Off != -1)
            {
                if (VersionInfo.EngineVersion < 4.24)
                {
                    TArray<FPlaylistStreamedLevelData>& AdditionalPlaylistLevels
                        = *(TArray<FPlaylistStreamedLevelData>*)(__int64(GameState) + AdditionalPlaylistLevelsStreamed__Off - 0x10);

                    AdditionalPlaylistLevels.Free();

                    if (Playlist->HasAdditionalLevels())
                    {
                        for (auto& Level : Playlist->AdditionalLevels)
                        {
                            bool Success = false;
                            if (AdditionalLevelStruct)
                            {
                                auto level = (FAdditionalLevelStreamed*)malloc(FAdditionalLevelStreamed::Size());
                                memset((PBYTE)level, 0, FAdditionalLevelStreamed::Size());
                                level->bIsServerOnly = false;
                                level->LevelName = Level.ObjectID.AssetPathName;
                                if (Success)
                                    GameState->AdditionalPlaylistLevelsStreamed.Add(*level, FAdditionalLevelStreamed::Size());
                                free(level);
                            }
                            else
                                GetFromOffset<TArray<FName>>(GameState, AdditionalPlaylistLevelsStreamed__Off).Add(Level.ObjectID.AssetPathName);
                        }
                    }

                    if (Playlist->HasAdditionalLevelsServerOnly())
                    {
                        for (auto& Level : Playlist->AdditionalLevelsServerOnly)
                        {
                            bool Success = false;
                            if (AdditionalLevelStruct)
                            {
                                auto level = (FAdditionalLevelStreamed*)malloc(FAdditionalLevelStreamed::Size());
                                memset((PBYTE)level, 0, FAdditionalLevelStreamed::Size());
                                level->bIsServerOnly = true;
                                level->LevelName = Level.ObjectID.AssetPathName;
                                if (Success)
                                    GameState->AdditionalPlaylistLevelsStreamed.Add(*level, FAdditionalLevelStreamed::Size());
                                free(level);
                            }
                            else
                                GetFromOffset<TArray<FName>>(GameState, AdditionalPlaylistLevelsStreamed__Off).Add(Level.ObjectID.AssetPathName);
                        }
                    }
                }
                else
                {
                    bManagedPlaylistStreamingSetup = true;
                }
            }
        }

        if (!FConfiguration::IsKnownS27CustomMapPlaylist() &&
            !bHeistPlaylist &&
            !bManagedPlaylistStreamingSetup)
            GameState->OnRep_AdditionalPlaylistLevelsStreamed();

        // misc C1 poi things
        if (VersionInfo.FortniteVersion >= 6 && VersionInfo.FortniteVersion < 7)
        {
            if (VersionInfo.FortniteVersion > 6.10)
                ShowFoundation(VersionInfo.FortniteVersion <= 6.21 ? FindObject<ABuildingFoundation>(L"/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_Lake1") : FindObject<ABuildingFoundation>(L"/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_Lake2"));
            else
                ShowFoundation(FindObject<ABuildingFoundation>(L"/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_Athena_StreamingTest12"));

            ShowFoundation(VersionInfo.FortniteVersion <= 6.10 ? FindObject<ABuildingFoundation>(L"/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_Athena_StreamingTest13") : FindObject<ABuildingFoundation>(L"/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_FloatingIsland"));

            auto IslandScripting = TUObjectArray::FindFirstObject("BP_IslandScripting_C");
            if (IslandScripting)
            {
                auto UpdateMapOffset = IslandScripting->GetOffset("UpdateMap");
                if (UpdateMapOffset != -1)
                {
                    *(bool*)(__int64(IslandScripting) + UpdateMapOffset) = true;
                    IslandScripting->ProcessEvent(IslandScripting->GetFunction("OnRep_UpdateMap"), nullptr);
                }
            }
        }
        else if (VersionInfo.FortniteVersion >= 7 && VersionInfo.FortniteVersion < 8)
        {
            ShowFoundation(FindObject<ABuildingFoundation>("/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_Athena_POI_25x36"));
            ShowFoundation(FindObject<ABuildingFoundation>("/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.ShopsNew"));
        }
        else if (VersionInfo.FortniteVersion >= 8 && VersionInfo.FortniteVersion < 10)
            ShowFoundation(FindObject<ABuildingFoundation>(L"/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_Athena_POI_50x53_Volcano"));
        else if (VersionInfo.FortniteVersion >= 10.20 && VersionInfo.FortniteVersion < 11)
            ShowFoundation(FindObject<ABuildingFoundation>(L"/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.LF_Athena_POI_50x53_Volcano"));

        if (VersionInfo.FortniteVersion >= 7 && VersionInfo.FortniteVersion <= 10)
            ShowFoundation(FindObject<ABuildingFoundation>(L"/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.SLAB_2"));
        else if (VersionInfo.EngineVersion == 4.23) // rest of S10
            ShowFoundation(FindObject<ABuildingFoundation>(L"/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.SLAB_4"));

        bool bEvent = false;
        if (Playlist && Playlist->HasGameplayTagContainer())
        {
            for (int i = 0; i < Playlist->GameplayTagContainer.GameplayTags.Num(); i++)
            {
                auto& PlaylistTag = Playlist->GameplayTagContainer.GameplayTags.Get(i, FGameplayTag::Size());

                const auto PlaylistTagName = PlaylistTag.TagName.ToString();
                if (PlaylistTagName == "Athena.Playlist.SpecialEvent" ||
                    PlaylistTagName == "Athena.Playlist.Concert")
                {
                    bEvent = true;
                    if (VersionInfo.FortniteVersion == 7.30)
                        ShowFoundation(FindObject<ABuildingFoundation>("/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.PleasentParkFestivus"));

                    break;
                }
            }
        }

        if (VersionInfo.FortniteVersion == 12.41)
        {
            ShowFoundation(FindObject<ABuildingFoundation>("/Game/Athena/Apollo/Maps/Apollo_POI_Foundations.Apollo_POI_Foundations.PersistentLevel.LF_Athena_POI_19x19_2"));
            ShowFoundation(FindObject<ABuildingFoundation>("/Game/Athena/Apollo/Maps/Apollo_POI_Foundations.Apollo_POI_Foundations.PersistentLevel.BP_Jerky_Head6_18"));
            ShowFoundation(FindObject<ABuildingFoundation>("/Game/Athena/Apollo/Maps/Apollo_POI_Foundations.Apollo_POI_Foundations.PersistentLevel.BP_Jerky_Head5_14"));
            ShowFoundation(FindObject<ABuildingFoundation>("/Game/Athena/Apollo/Maps/Apollo_POI_Foundations.Apollo_POI_Foundations.PersistentLevel.BP_Jerky_Head3_8"));
            ShowFoundation(FindObject<ABuildingFoundation>("/Game/Athena/Apollo/Maps/Apollo_POI_Foundations.Apollo_POI_Foundations.PersistentLevel.BP_Jerky_Head_2"));
            ShowFoundation(FindObject<ABuildingFoundation>("/Game/Athena/Apollo/Maps/Apollo_POI_Foundations.Apollo_POI_Foundations.PersistentLevel.BP_Jerky_Head4_11"));
        }

        if (VersionInfo.FortniteVersion == 7.30 && !bEvent)
            ShowFoundation(FindObject<ABuildingFoundation>("/Game/Athena/Maps/Athena_POI_Foundations.Athena_POI_Foundations.PersistentLevel.PleasentParkDefault"));


        if (VersionInfo.FortniteVersion == 17.50)
        {
            //ShowFoundation(FindObject<ABuildingFoundation>(L"/Game/Athena/Apollo/Maps/Apollo_Mother.Apollo_Mother.PersistentLevel.farmbase_2"));
            //ShowFoundation(FindObject<ABuildingFoundation>(L"/Game/Athena/Apollo/Maps/Apollo_Mother.Apollo_Mother.PersistentLevel.Farm_Phase_03"));
        }

        if (VersionInfo.EngineVersion >= 4.27 && std::floor(VersionInfo.FortniteVersion) != 20) // on 20 it does some weird stuff
        {
            auto MeshNetworkSubsystem = TUObjectArray::FindFirstObject("MeshNetworkSubsystem");

            if (MeshNetworkSubsystem)
                *(uint8_t*)(__int64(MeshNetworkSubsystem) + MeshNetworkSubsystem->GetOffset("NodeType")) = 2;
        }

        if (std::floor(VersionInfo.FortniteVersion) == 10)
        {
            int GeyserAmount = 1;

            auto GeyserClass = FindObject<UClass>(UEAllocatedWString(L"/Game/Athena/Environments/Blueprints/DudeBro/BGA_DudeBro_Mini.BGA_DudeBro_Mini_C"));

            if (!GeyserClass) 
                return;

            std::vector<FVector> GeyserLocations = 
            {
                FVector(89883.523438, 2428.856934, 1721.958008),   // 1: Lazy lagoon mountain
                FVector(61597.550781, 16848.650391, -0.744186),   // 2: Volcano 1
                FVector(58946.187500, 24417.082031, 1928.725952),  // 3: Volcano 2
                FVector(58226.332031, 35153.121094, 2737.305664),  // 4: Volcano 4
                FVector(99996.023438, 36478.667969, 9136.631836),  // 5: Infront of volcano hill
                FVector(72324.421875, 46690.652344, 9223.631836),  // 6: Zipline hill
                FVector(48260.160156, 48935.687500, 3387.915283),  // 7: Left of s7 mountain
                FVector(35108.128906, 63084.425781, 1170.028320),  // 8: Trailer park
                FVector(-790.447754, 68462.484375, 3304.623535),   // 9: Desert race track mountain
                FVector(593.079712, 51143.253906, 2322.515381),    // 10: Desert house + gas station
                FVector(68796.078125, -5714.037598, 1946.025513),  // 11: Lazy lagoon shack 1
                FVector(74499.101562, -1986.378906, 1325.798218),  // 12: Lazy lagoon shack 2
                FVector(56631.031250, -9915.774414, 513.797668),   // 13: Dusty depot
                FVector(75186.226562, 78506.148438, 2416.323242),  // 14: Middle left edge field
                FVector(102538.367188, 78399.929688, 2883.602539), // 15: Face mountain
                FVector(114640.531250, 62411.414062, 1057.379883), // 16: Infront of sunny steps
                FVector(121277.882812, 43351.671875, 2909.387695), // 17: Between and below zip
                FVector(94820.890625, 24539.779297, 8486.580078),  // 18: Top left of volcano
                FVector(80984.421875, 22242.648438, 9638.948242),  // 19: Left of volcano
                FVector(124880.695312, 17783.384766, 429.139496),  // 20: Brown/orange lake 1
                FVector(122058.492188, 22701.984375, 407.454712),  // 21: Brown/orange lake 2
                FVector(113105.117188, 24883.089844, 1560.164185), // 22: Brown/orange lake 3
                FVector(110117.648438, 19830.042969, 1570.611206), // 23: Brown/orange lake 4
                FVector(119679.210938, 11784.166016, -251.755707), // 24: Brown/orange lake 5
                FVector(119306.156250, 16606.320312, 403.438782),  // 25: Brown/orange lake 6
                FVector(106046.093750, 24800.246094, 4615.074707)  // 26: Brown/orange lake 7
            };

            for (size_t i = 0; i < GeyserLocations.size(); i++)
            {
                FVector AdjustedLocation = GeyserLocations[i];

                AdjustedLocation.Z -= 150.0f;

                FTransform SpawnTransform;
                SpawnTransform.Translation = AdjustedLocation;
                SpawnTransform.GetScale3D() = FVector(1, 1, 1);

                auto SpawnedActor = UWorld::GetWorld()->SpawnActor<AActor>(GeyserClass, SpawnTransform);

                if (SpawnedActor)
                {
                    SpawnedActor->ForceNetUpdate();
                    printf("Spawned Geyser %zu at location %zu\n", i + 1, i);
                }
            }
        }

        if (!GameMode->AIDirector)
        {
            auto AIDirectorClass = GameMode->HasWarmupRequiredPlayerCount() ? FindClass("AthenaAIDirector") : FindObject<UClass>("/Game/AIDirector/AIDirector_Fortnite.AIDirector_Fortnite_C");
            if (!AIDirectorClass)
                AIDirectorClass = FindClass("FortAIDirector");

            GameMode->AIDirector = UWorld::SpawnActor(AIDirectorClass, FVector{}, GameMode);
            if (GameMode->AIDirector)
                GameMode->AIDirector->Call(GameMode->AIDirector->GetFunction("Activate"));
        }

        if (GameMode->HasServerBotManager())
        {
            if (auto BotManager = (UFortServerBotManagerAthena*)UGameplayStatics::SpawnObject(UFortServerBotManagerAthena::StaticClass(), GameMode))
            {
                GameMode->ServerBotManager = BotManager;
                BotManager->CachedGameState = GameState;
                BotManager->CachedGameMode = GameMode;
            }
            else
            {
                printf("BotManager is nullptr!\n");
            }
        }

        if (!GameMode->AIGoalManager)
        {
            auto GoalManagerClass = GameMode->HasWarmupRequiredPlayerCount() ? FindClass("FortAIGoalManager") : FindObject<UClass>("/Game/AI/GoalSelection/AIGoalManager.AIGoalManager_C");
            GameMode->AIGoalManager = UWorld::SpawnActor(GoalManagerClass, FVector{}, GameMode);
        }

        if (GameMode->HasSpawningPolicyManager() && !GameMode->SpawningPolicyManager)
        {
            auto SpawningPolicyManager = UWorld::SpawnActor<AFortAthenaSpawningPolicyManager>(
                AFortAthenaSpawningPolicyManager::StaticClass(), {});
            if (SpawningPolicyManager)
            {
                GameMode->SpawningPolicyManager = SpawningPolicyManager;
                SpawningPolicyManager->GameStateAthena = GameState;
                SpawningPolicyManager->GameModeAthena = GameMode;
            }
        }

        auto MissionManagerClass = GameMode->HasWarmupRequiredPlayerCount() ? nullptr : FindObject<UClass>("/Game/Blueprints/MissionManager.MissionManager_C");

        if (MissionManagerClass)
        {
            GameState->MissionManager = UWorld::SpawnActor(MissionManagerClass, FVector{}, GameState);
            GameState->OnRep_MissionManager();

            auto MissionInfo = FindObject<UFortMissionInfo>(L"/Game/Missions/Primary/EvacuateTheSurvivors/EvacuteTheSurvivors.EvacuteTheSurvivors");

            if (!MissionInfo)
                MissionInfo = FindObject<UFortMissionInfo>(L"/SaveTheWorld/Missions/Primary/EvacuateTheSurvivors/EvacuteTheSurvivors.EvacuteTheSurvivors");

            if (MissionInfo)
            {
                MissionInfo->bStartPlayingOnLoad = true; // bad hack, we should find a better way to do this later
                // startplayingmission

                UFortMissionLibrary::LoadMission(UWorld::GetWorld(), MissionInfo);
            }
            // we need to spawn bluglo manager too?
        }

        /* if (VersionInfo.EngineVersion == 4.16)
         {
             if (!UWorld::GetWorld()->NavigationSystem)
             {
                 UWorld::GetWorld()->NavigationSystem = UGameplayStatics::SpawnObject(FindClass("FortNavSystem"), UWorld::GetWorld());
                 auto OnWorldInitDone = (void(*)(UObject*, char))(ImageBase + 0x1f6fc40);
                 OnWorldInitDone(UWorld::GetWorld()->NavigationSystem, 1);
             }
         }*/

         //if (!GameMode->HasWarmupRequiredPlayerCount())
         //    UWorld::SpawnActor(FindClass("FortPlayerStart"), FVector{0, 0, 3000});

        *Ret = false;
        return;
    }

    if (!GameMode->bWorldIsReady)
    {
        static auto WarmupStartClass = FindClass("PlayerStart");
        TArray<AActor*> Starts;
        Utils::GetAll(WarmupStartClass, Starts);
        auto StartsNum = Starts.Num();
        Starts.Free();

        if (StartsNum == 0 || !Misc::bHookedAll)
        {
            *Ret = false;
            return;
        }

        TArray<AFortAthenaMapInfo*> AllMapInfos;
        Utils::GetAll<AFortAthenaMapInfo>(AllMapInfos);

        // HasMapInfo() must guard the property read. A missing reflected property
        // is represented by offset -1, so reading MapInfo unconditionally would
        // dereference one byte before GameState and crash during world startup.
        const bool bMapInfoPending =
            AllMapInfos.Num() > 0 && GameState->HasMapInfo() && !GameState->MapInfo;
        AllMapInfos.Free();
        if (bMapInfoPending)
        {
            *Ret = false;
            return;
        }

        if (const auto Native1040Playlist =
                GetPublishedPlaylist(GameState);
            FFortAthenaNativeLTMCompatibility::
                IsTargetPlaylist(Native1040Playlist) &&
            !PublishNative1040Playlist(
                GameState, Native1040Playlist))
        {
            *Ret = false;
            return;
        }

        if ((VersionInfo.FortniteVersion >= 3.5 && VersionInfo.FortniteVersion <= 4.0))
            SetupPlaylist(GameMode, GameState);

        if (VersionInfo.FortniteVersion >= 25.20 && GameState->HasMapInfo() && GameState->MapInfo)
        {
            auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(GameState);

            if (GamePhaseLogic)
            {
                auto InitializeFlightPath = (void(*)(AFortAthenaMapInfo*, AFortGameStateAthena*, UFortGameStateComponent_BattleRoyaleGamePhaseLogic*, bool, double, float, float)) FindInitializeFlightPath();
                if (InitializeFlightPath)
                    InitializeFlightPath(GameState->MapInfo, GameState, GamePhaseLogic, false, 0.f, 0.f, 360.f);
                UFortGameStateComponent_BattleRoyaleGamePhaseLogic::GenerateStormCircles(GameState->MapInfo);
            }
        }

        UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bEnableZones = true;

        const auto Playlist =
            VersionInfo.FortniteVersion >= 3.5 &&
            GameMode->HasWarmupRequiredPlayerCount()
                ? ResolveActivePlaylist(GameState)
                : nullptr;

        if (Playlist && Playlist->HasbSkipWarmup())
            UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bSkipWarmup = Playlist->bSkipWarmup;
        if (Playlist && Playlist->HasbSkipAircraft())
            UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bSkipAircraft = Playlist->bSkipAircraft;

        if (Playlist && Playlist->HasGameplayTagContainer())
        {

            for (int i = 0; i < Playlist->GameplayTagContainer.GameplayTags.Num(); i++)
            {
                auto& PlaylistTag = Playlist->GameplayTagContainer.GameplayTags.Get(i, FGameplayTag::Size());

                const auto PlaylistTagName = PlaylistTag.TagName.ToString();
                if (PlaylistTagName == "Athena.Playlist.SpecialEvent" ||
                    PlaylistTagName == "Athena.Playlist.Concert")
                {
                    for (auto& Event : Events::EventsArray)
                    {
                        if (Event.EventVersion != VersionInfo.FortniteVersion)
                            continue;

                        // Use the same reflected, idempotent loader lifecycle
                        // as the Start Event request. Calling the loader here
                        // and again from the GUI used to race the streamed
                        // Cattus level on 9.41.
                        Events::PrepareEventContent();

                        UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bEnableZones = false;
                        SDK::DbgLog(
                            "[Events] preserving native safe-zone locations count=%d FN=%.2f\n",
                            GameMode->HasSafeZoneLocations()
                                ? GameMode->SafeZoneLocations.Num()
                                : -1,
                            VersionInfo.FortniteVersion);
                        break;
                    }

                    break;
                }
            }
        }

        auto AbilitySet = VersionInfo.FortniteVersion > 8.30 ? FindObject<UFortAbilitySet>(L"/Game/Abilities/Player/Generic/Traits/DefaultPlayer/GAS_AthenaPlayer.GAS_AthenaPlayer") : FindObject<UFortAbilitySet>(L"/Game/Abilities/Player/Generic/Traits/DefaultPlayer/GAS_DefaultPlayer.GAS_DefaultPlayer");
        AbilitySet->AddToRoot();
        AbilitySets.Add(AbilitySet);

        if (VersionInfo.FortniteVersion >= 20)
        {
            auto TacticalSprintAbility = FindObject<UFortAbilitySet>(L"/TacticalSprintGame/Gameplay/AS_TacticalSprint.AS_TacticalSprint");

            if (!TacticalSprintAbility)
                TacticalSprintAbility = FindObject<UFortAbilitySet>(L"/TacticalSprint/Gameplay/AS_TacticalSprint.AS_TacticalSprint");
            AFortGameMode::TacticalSprintAbilitySet = TacticalSprintAbility;
            if (TacticalSprintAbility)
            {
                TacticalSprintAbility->AddToRoot();
                AbilitySets.Add(TacticalSprintAbility);
            }

            auto AscenderAbility = FindObject<UFortAbilitySet>(L"/Ascender/Gameplay/Ascender/AS_Ascender.AS_Ascender");
            AscenderAbility->AddToRoot();
            AbilitySets.Add(AscenderAbility);

            auto DoorBashAbility = FindObject<UFortAbilitySet>(L"/DoorBashContent/Gameplay/AS_DoorBash.AS_DoorBash");
            DoorBashAbility->AddToRoot();
            AbilitySets.Add(DoorBashAbility);

            auto HillScrambleAbility = FindObject<UFortAbilitySet>(L"/HillScramble/Gameplay/AS_HillScramble.AS_HillScramble");
            HillScrambleAbility->AddToRoot();
            AbilitySets.Add(HillScrambleAbility);

            auto SlideImpulseAbility = FindObject<UFortAbilitySet>(L"/SlideImpulse/Gameplay/AS_SlideImpulse.AS_SlideImpulse");
            SlideImpulseAbility->AddToRoot();
            AbilitySets.Add(SlideImpulseAbility);

            if (std::floor(VersionInfo.FortniteVersion) == 21)
            {
                auto RealitySaplingAbility = FindObject<UFortAbilitySet>(L"/RealitySeedGameplay/Environment/Foliage/GAS_Athena_RealitySapling.GAS_Athena_RealitySapling");
                AbilitySets.Add(RealitySaplingAbility);
            }
        }

        for (auto& Set : AbilitySets)
            if (Set)
                Set->AddToRoot();

        if (Playlist && Playlist->HasModifierList())
            for (int i = 0; i < Playlist->ModifierList.Num(); i++)
            {
                auto Modifier = Playlist->ModifierList.Get(i, FSoftObjectPtr::Size()).Get();

                if (!Modifier)
                    continue;

                for (int j = 0; j < Modifier->PersistentAbilitySets.Num(); j++)
                {
                    auto& DeliveryInfo = Modifier->PersistentAbilitySets.Get(j, FFortAbilitySetDeliveryInfo::Size());

                    if (!DeliveryInfo.DeliveryRequirements.bApplyToPlayerPawns)
                        continue;

                    for (int k = 0; k < DeliveryInfo.AbilitySets.Num(); k++)
                    {
                        auto AbilitySet = DeliveryInfo.AbilitySets.Get(k, FSoftObjectPtr::Size()).Get();

                        AbilitySets.Add(AbilitySet);
                    }
                }
            }


        /*if (floor(VersionInfo.FortniteVersion) != 20)
        {
            UFortLootPackage::SpawnFloorLootForContainer(FindObject<UClass>(L"/Game/Athena/Environments/Blueprints/Tiered_Athena_FloorLoot_Warmup.Tiered_Athena_FloorLoot_Warmup_C"));
            UFortLootPackage::SpawnFloorLootForContainer(FindObject<UClass>(L"/Game/Athena/Environments/Blueprints/Tiered_Athena_FloorLoot_01.Tiered_Athena_FloorLoot_01_C"));
        }

        auto ConsumableSpawners = Utils::GetAll<ABGAConsumableSpawner>();

        for (auto& Spawner : ConsumableSpawners)
            UFortLootPackage::SpawnConsumableActor(Spawner);*/

        if (VersionInfo.EngineVersion >= 4.27)
        {
            if (GameState->HasDefaultParachuteDeployTraceForGroundDistance())
                GameState->DefaultParachuteDeployTraceForGroundDistance = 10000;
        }

        if (VersionInfo.FortniteVersion >= 27)
        {
            // fix grind rails
            auto GameData = FindObject<UCurveTable>("/GrindRail/DataTables/GrindRailGameData.GrindRailGameData");

            if (GameData)
            {
                static FName UseGrindingMME = FName(L"Default.GrindRails.UseGrindingMME");

                for (const auto& [RowName, RowPtr] : GameData->RowMap)
                {
                    if (RowName != UseGrindingMME)
                        continue;

                    FSimpleCurve* Row = (FSimpleCurve*)RowPtr;

                    if (!Row)
                        continue;

                    for (int i = 0; i < Row->Keys.Num(); i++)
                    {
                        auto& Key = Row->Keys.Get(i, FSimpleCurveKey::Size());

                        Key.Value = 0.f;
                    }
                }
            }
        }

        if (GameState->HasMapInfo() && GameState->MapInfo)
        {
            if (VersionInfo.FortniteVersion >= 3.4)
            {
                GameData = Playlist ? Playlist->GameData : nullptr;

                if (!GameData)
                {
                    if (GameState->HasAthenaGameDataTable())
                        GameData = GameState->AthenaGameDataTable;
                }

                if (!GameData)
                    GameData = FindObject<UCurveTable>(L"/Game/Athena/Balance/DataTables/AthenaGameData.AthenaGameData");

                for (int i = 0; i < 6; i++)
                {
                    float Weight;
                    UDataTableFunctionLibrary::EvaluateCurveTableRow(GameState->MapInfo->VendingMachineRarityCount.Curve.CurveTable, GameState->MapInfo->VendingMachineRarityCount.Curve.RowName, (float)i, nullptr, &Weight, FString());

                    WeightMap[i] = Weight;
                    Sum += Weight;
                }

                UDataTableFunctionLibrary::EvaluateCurveTableRow(GameState->MapInfo->VendingMachineRarityCount.Curve.CurveTable, GameState->MapInfo->VendingMachineRarityCount.Curve.RowName, 0.f, nullptr, &Weight, FString());

                TotalWeight = std::accumulate(WeightMap.begin(), WeightMap.end(), 0.0f, [&](float acc, const std::pair<int, float>& p)
                    { return acc + p.second; });
            }

            if (VersionInfo.FortniteVersion >= 3.3 && VersionInfo.FortniteVersion < 17 && GameState->MapInfo->LlamaClass)
            {
                auto PickSupplyDropLocation = (FVector * (*)(AFortAthenaMapInfo*, FVector*, FVector*, float, bool, float, float)) FindPickSupplyDropLocation();

                if (PickSupplyDropLocation)
                {
                    FFortSafeZoneDefinition& SafeZoneDefinition = GameState->MapInfo->SafeZoneDefinition;

                    auto LlamaMin = GameState->MapInfo->LlamaQuantityMin.Evaluate();
                    auto LlamaMax = GameState->MapInfo->LlamaQuantityMax.Evaluate();
                    auto LlamaCount = UKismetMathLibrary::RandomIntegerInRange((int)LlamaMin, (int)LlamaMax);
                    auto Radius = GameState->MapInfo->HasSafeZoneDefinition() ? SafeZoneDefinition.Radius.Evaluate(0) : 0;

                    if (Radius == 0)
                        Radius = 120000;
                    auto Center = GameState->MapInfo->GetMapCenter();
                    Center.Z = 10000;

                    for (int i = 0; i < LlamaCount; i++)
                    {
                        FVector Loc(0, 0, 0);
                        PickSupplyDropLocation(GameState->MapInfo, &Loc, &Center, Radius, 0, -1, -1);

                        if (Loc.X != 0 || Loc.Y != 0 || Loc.Z != 0)
                        {
                            FRotator Rot{};
                            Rot.Yaw = (float)rand() * 0.010986663f;

                            auto NewLlama = UWorld::SpawnActorUnfinished(GameState->MapInfo->LlamaClass, Loc, Rot);

                            static auto FindGroundLocationAt = NewLlama->GetFunction("FindGroundLocationAt");
                            auto GroundLoc = NewLlama->Call<FVector>(FindGroundLocationAt, Loc);

                            UWorld::FinishSpawnActor(NewLlama, GroundLoc, Rot);
                        }
                    }
                }
            }
        }

        GameMode->DefaultPawnClass = FindObject<UClass>(L"/Game/Athena/PlayerPawn_Athena.PlayerPawn_Athena_C");

        if (VersionInfo.EngineVersion == 4.16 && VersionInfo.FortniteVersion < 1.9)
        {
            auto sRef = Memcury::Scanner::FindStringRef(L"CollectGarbageInternal() is flushing async loading").Get();
            uint64_t CollectGarbage = 0;

            if (sRef)
            {
                for (int i = 0; i < 1000; i++)
                {
                    auto Ptr = (uint8_t*)(sRef - i);

                    if (*Ptr == 0x48 && *(Ptr + 1) == 0x89 && *(Ptr + 2) == 0x5C)
                    {
                        CollectGarbage = uint64_t(Ptr);
                        break;
                    }
                    else if (*Ptr == 0x40 && *(Ptr + 1) == 0x55)
                    {
                        CollectGarbage = uint64_t(Ptr);
                        break;
                    }
                    else if (*Ptr == 0x48 && *(Ptr + 1) == 0x8B && *(Ptr + 2) == 0xC4)
                    {
                        CollectGarbage = uint64_t(Ptr);
                        break;
                    }
                }

                Utils::Patch<uint8_t>(CollectGarbage, 0xC3);
            }
        }
        else if (VersionInfo.EngineVersion <= 4.20)
        {
            auto pattern = VersionInfo.FortniteVersion > 3.2 ? Memcury::Scanner::FindPattern("E8 ? ? ? ? EB 26 40 38 3D ? ? ? ?") : Memcury::Scanner::FindPattern("E8 ? ? ? ? F0 FF 0D ? ? ? ? 0F B6 C3");

            if (pattern.IsValid())
                Utils::Patch<uint8_t>(pattern.RelativeOffset(1).Get(), 0xC3);
        }

        if (GameState->HasAllPlayerBuildableClassesIndexLookup())
            for (auto& [Class, Handle] : GameState->AllPlayerBuildableClassesIndexLookup)
                AFortGameStateAthena::BuildingClassMap[Handle] = Class;

        if (FConfiguration::bAutoDump)
        {
            std::stringstream ss;

            ss << "Generated by Erbium (https://github.com/plooshi/Erbium)\n";
            char version[6];

            sprintf_s(version, VersionInfo.FortniteVersion >= 5.00 || VersionInfo.FortniteVersion < 1.2 ? "%.2f" : "%.1f", VersionInfo.FortniteVersion);
            ss << "Fortnite Version: " << version << "\n\n";

            auto RarityEnum = EFortRarity::StaticEnum();
            for (int i = 0; i < TUObjectArray::Num(); i++)
            {
                auto Object = TUObjectArray::GetObjectByIndex(i);
                if (!Object || !Object->Class || Object->IsDefaultObject() || !Object->IsA<UFortWorldItemDefinition>())
                    continue;
                auto Item = (UFortWorldItemDefinition*)Object;

                FString Name = UKismetTextLibrary::Conv_TextToString(Item->HasDisplayName() ? Item->DisplayName : Item->ItemName);

                ss << "- " << UKismetSystemLibrary::GetPathName(Item).ToString() << "\n";
                ss << "-     Name: " << (Name.GetData() ? Name.ToString() : "None") << "\n";

                auto Names = *(TArray<TPair<FName, int64>>*)(__int64(RarityEnum) + 0x40);

                for (int j = 0; j < Names.Num(); j++)
                {
                    auto& Pair = Names[j];
                    auto& NameVal = Pair.Key();
                    auto& Value = Pair.Value();

                    if (Value == Item->Rarity)
                    {
                        auto str = NameVal.ToString();
                        auto colcolIdx = str.find_last_of("::");

                        auto RealName = colcolIdx == -1 ? str : str.substr(colcolIdx + 1);

                        ss << "-     Rarity: " << RealName << "\n";
                    }
                }
            }

            std::ofstream of("DumpedItems.txt", std::ios::trunc);

            of << ss.str();
            of.close();

            std::stringstream ss2;

            ss2 << "Generated by Erbium (https://github.com/plooshi/Erbium)\n";
            char version2[6];

            sprintf_s(version2, VersionInfo.FortniteVersion >= 5.00 || VersionInfo.FortniteVersion < 1.2 ? "%.2f" : "%.1f", VersionInfo.FortniteVersion);
            ss2 << "Fortnite Version: " << version2 << "\n\n";

            for (int i = 0; i < TUObjectArray::Num(); i++)
            {
                auto Object = TUObjectArray::GetObjectByIndex(i);
                if (!Object || !Object->Class || Object->IsDefaultObject() || !Object->IsA<UFortPlaylistAthena>())
                    continue;
                auto Playlist = (UFortPlaylistAthena*)Object;

                FString Name = UKismetTextLibrary::Conv_TextToString(Playlist->UIDisplayName);

                ss2 << "- " << UKismetSystemLibrary::GetPathName(Playlist).ToString() << "\n";
                ss2 << "-     Name: " << (Name.GetData() ? Name.ToString() : "None") << "\n";
                if (Playlist->HasMaxPlayers())
                    ss2 << "-     Max Players: " << std::to_string(Playlist->MaxPlayers) << "\n";
                if (Playlist->HasMaxSquadSize())
                    ss2 << "-     Squad Size: " << std::to_string(Playlist->MaxSquadSize) << "\n";
            }

            std::ofstream of2("DumpedPlaylists.txt", std::ios::trunc);

            of2 << ss2.str();
            of2.close();
        }

        if constexpr (FConfiguration::WebhookURL && *FConfiguration::WebhookURL)
        {
            auto curl = curl_easy_init();

            curl_easy_setopt(curl, CURLOPT_URL, FConfiguration::WebhookURL);
            curl_slist* headers = curl_slist_append(NULL, "Content-Type: application/json");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

            char version[6];

            sprintf_s(version, VersionInfo.FortniteVersion >= 5.00 || VersionInfo.FortniteVersion < 1.2 ? "%.2f" : "%.1f", VersionInfo.FortniteVersion);

            auto payload = UEAllocatedString("{\"embeds\": [{\"title\": \"Server is joinable!\", \"fields\": [{\"name\":\"Version\",\"value\":\"") + version + "\"}, {\"name\":\"Playlist\",\"value\":\"" + (Playlist ? Playlist->PlaylistName.ToString() : "Playlist_DefaultSolo") + "\"}], \"color\": " + "\"7237230\", \"footer\": {\"text\":\"Magnesium\", \"icon_url\":\"https://cdn.discordapp.com/attachments/1341168629378584698/1436803905119064105/L0WnFa.png.png?ex=6910ef69&is=690f9de9&hm=01a0888b46647959b38ee58df322048ab49e2a5a678e52d4502d9c5e3978d805&\"}, \"timestamp\":\"" + iso8601() + "\"}] }";

            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());

            curl_easy_perform(curl);

            curl_easy_cleanup(curl);
        }

        // for some reason it doesnt like when u do it earlier
        if (!Playlist && VersionInfo.FortniteVersion <= 4)
            if (GameMode->GameSession->HasMaxPlayers())
                GameMode->GameSession->MaxPlayers = 100;

        EnsureLegacyLateGameSafeZoneLocations(GameMode);
        GameMode->bWorldIsReady = true;
    }

    if (VersionInfo.EngineVersion >= 4.24 && GameMode->IsA<AFortGameModeAthena>())
    {
        int ReadyPlayers = 0;
        std::array<AFortPlayerControllerAthena*, 256>
            CountedReadyControllers{};
        size_t CountedReadyControllerCount = 0;
        auto CountReadyController =
            [&](AFortPlayerControllerAthena* PlayerController)
        {
            if (!PlayerController || !PlayerController->PlayerState)
                return;
            for (size_t Index = 0;
                Index < CountedReadyControllerCount;
                ++Index)
            {
                if (CountedReadyControllers[Index] == PlayerController)
                    return;
            }
            if (CountedReadyControllerCount <
                CountedReadyControllers.size())
            {
                CountedReadyControllers[
                    CountedReadyControllerCount++] =
                        PlayerController;
            }
            auto PlayerState = PlayerController->PlayerState;
            if (!PlayerState->bIsSpectator && PlayerController->bReadyToStartMatch)
                ++ReadyPlayers;
        };
        if (GameMode->HasAlivePlayers())
        {
            for (auto Actor : GameMode->AlivePlayers)
            {
                CountReadyController(
                    Actor
                        ? Actor->Cast<
                            AFortPlayerControllerAthena>()
                        : nullptr);
            }
        }
        auto ReadinessNetDriver = World
            ? static_cast<UNetDriver*>(World->NetDriver)
            : nullptr;
        if (ReadinessNetDriver)
        {
            for (auto Connection :
                ReadinessNetDriver->ClientConnections)
            {
                if (!Connection)
                    continue;
                CountReadyController(Connection->PlayerController);
                for (auto Child : Connection->Children)
                {
                    if (Child)
                        CountReadyController(Child->PlayerController);
                }
            }
        }

        auto VolumeManager = GameState->HasVolumeManager() ? GameState->VolumeManager : nullptr;

        const UFortPlaylistAthena* ReadinessPlaylist =
            GetPublishedPlaylist(GameState);
        const UFortPlaylistAthena* ActiveReadinessPlaylist =
            ReadinessPlaylist
                ? ReadinessPlaylist
                : ResolveActivePlaylist(GameState);
        const bool bUsesManagedPlaylistStreaming =
            ReadinessPlaylist &&
            !FConfiguration::IsKnownS27CustomMapPlaylist() &&
            !FFortAthenaHeistCompatibility::
                IsHeistPlaylist(ReadinessPlaylist) &&
            !FFortAthenaNativeLTMCompatibility::
                IsTargetPlaylist(ReadinessPlaylist);
        const bool bManagedRequestsReady =
            !bUsesManagedPlaylistStreaming ||
            MaintainManagedPlaylistLevels(
                GameState, ReadinessPlaylist, false);
        // MaintainManagedPlaylistLevels already verifies every level declared
        // by the selected playlist.  Do not also require every entry in the
        // engine's process-lifetime tracker: stale/unrelated entries can stay
        // unloaded forever and used to leave an otherwise valid server stuck
        // in "Setting up".
        const bool bAllLevelsFinishedStreaming =
            bManagedRequestsReady;

        static auto WaitingToStart = FName(L"WaitingToStart");
        const bool bPlaylistReady =
            GameState->HasbPlaylistDataIsLoaded()
                ? GameState->bPlaylistDataIsLoaded
                : true;
        const bool bStartupSpawning =
            VolumeManager &&
            (VolumeManager->HasbInSpawningStartup()
                ? VolumeManager->bInSpawningStartup
                : GameState->bInSpawningStartup);
        const bool bServerJoinable =
            bListenSucceeded &&
            Misc::bHookedAll &&
            GameMode->bWorldIsReady &&
            ActiveReadinessPlaylist != nullptr &&
            GameMode->MatchState == WaitingToStart;

        if (bServerJoinable)
            GUI::MarkServerJoinable();

        *Ret =
            bListenSucceeded &&
            GameMode->bWorldIsReady &&
            bPlaylistReady &&
            GameMode->MatchState == WaitingToStart &&
            bAllLevelsFinishedStreaming &&
            !bStartupSpawning &&
            ReadyPlayers >=
                (GameMode->HasWarmupRequiredPlayerCount()
                    ? GameMode->WarmupRequiredPlayerCount
                    : 1);
    }
    else
    {
        *Ret = callOGWithRet(
            GameMode,
            Stack.GetCurrentNativeFunction(),
            ReadyToStartMatch);

        const auto Native1040Playlist =
            GetPublishedPlaylist(GameState);
        const bool bNative1040LTMReady =
            FFortAthenaNativeLTMCompatibility::
                IsReadyForMatch(
                    GameState, Native1040Playlist);
        if (!bNative1040LTMReady)
        {
            // Always call the 4.23 original first so the shipped playlist
            // pipeline gets its normal initialization opportunity. Cattus
            // content is owned solely by its idempotent Blueprint loader;
            // duplicating it through the generic level-instance API crashes
            // the native async loader.
            *Ret = false;
        }

        static auto WaitingToStartLegacy = FName(L"WaitingToStart");
        // A listening 4.23 server is joinable before the native match-start
        // predicate becomes true: that predicate normally waits for a client.
        // Requiring *Ret here creates a circular wait (the GUI remains in
        // "Setting up", so no client can join to make *Ret true).
        if (bNative1040LTMReady &&
            bListenSucceeded &&
            Misc::bHookedAll &&
            GameMode->bWorldIsReady &&
            GameMode->MatchState == WaitingToStartLegacy)
        {
            GUI::MarkServerJoinable();
        }
    }

    if (VersionInfo.FortniteVersion < 25.20 &&
        IsAuthoritativeBusPolicySelection() &&
        (!*Ret ||
            VersionInfo.FortniteVersion < 11.00))
    {
        auto Time = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
        // Legacy GameMode-driven seasons do not pass through the newer
        // GamePhaseLogic warmup policy. Mirror that policy here: disabling
        // Auto Bus Start must hold the native countdown until Start Bus Early
        // explicitly replaces these timestamps.
        float WarmupDuration =
            FConfiguration::bAutoBusStart.load()
                ? std::clamp(
                    FConfiguration::BusStartDelay.load(),
                    0.f, 300.f)
                : 99999999.0f;

        if (GameState->HasWarmupCountdownEndTime()) // gamephaselogic builds
        {
            if (GameState->HasWarmupCountdownStartTime())
                GameState->WarmupCountdownStartTime = Time;
            GameState->WarmupCountdownEndTime = Time + WarmupDuration;
            if (GameMode->HasWarmupCountdownDuration())
                GameMode->WarmupCountdownDuration = WarmupDuration;
            if (GameMode->HasWarmupEarlyCountdownDuration())
            {
                GameMode->WarmupEarlyCountdownDuration =
                    WarmupDuration;
            }
        }
    }

    // Older native ReadyToStartMatch implementations populate the runtime
    // schedule during the original call above. Force one post-native discovery
    // for each GameState, then leave the normal throttled tick to maintain it.
    static TWeakObjectPtr<AFortGameStateAthena>
        ForcedSupplyDropDiscoveryGameState;
    if (ForcedSupplyDropDiscoveryGameState.Get() != GameState)
    {
        ForcedSupplyDropDiscoveryGameState =
            TWeakObjectPtr<AFortGameStateAthena>(GameState);
        TickSupplyDropSuppression(true);
    }
    return;
}

int AFortGameMode::GetLateSafeZoneIndex()
{
    auto GameState = (AFortGameStateAthena*)(UWorld::GetWorld()->GameState);

    if (!GameState)
        return 3;

    return GameState->PlayerArray.Num() >= 25 ? 3 : 4;
}

auto SpawnDefaultPawnForIdx = 0;
uint64_t ApplyCharacterCustomization;

void AFortGameMode::SpawnDefaultPawnFor(UObject* Context, FFrame& Stack, AActor** Ret)
{
    AFortPlayerControllerAthena* NewPlayer;
    AActor* StartSpot;
    Stack.StepCompiledIn(&NewPlayer);
    Stack.StepCompiledIn(&StartSpot);
    Stack.IncrementCode();
    auto GameMode = (AFortGameMode*)Context;

    if (!NewPlayer || !StartSpot)
        return;

    auto GameState = GameMode->GameState;
    AFortPlayerPawnAthena* Pawn = nullptr;
   
    Pawn = (AFortPlayerPawnAthena*)UWorld::SpawnActor(GameMode->GetDefaultPawnClassForController(NewPlayer), StartSpot->GetTransform(), NewPlayer, 3);

    while (!Pawn)
    {
        auto PlayerStart = FConfiguration::IsKnownS27CustomMapPlaylist() ? GameMode->ChoosePlayerStart() : GameMode->ChoosePlayerStart(NewPlayer);
        if (PlayerStart)
            Pawn = (AFortPlayerPawnAthena*)UWorld::SpawnActor(GameMode->GetDefaultPawnClassForController(NewPlayer), PlayerStart->GetTransform(), NewPlayer, 3);
    }

    // they only stripped it on athena for some reason
    /*static auto FortGMSpawnDefaultPawnFor = (AFortPlayerPawnAthena * (*)(AFortGameMode*, AFortPlayerControllerAthena*, AActor*)) DefaultObjImpl("FortGameMode")->Vft[SpawnDefaultPawnForIdx];
    Pawn = FortGMSpawnDefaultPawnFor(GameMode, NewPlayer, StartSpot);

    if (!Pawn)
    {
        auto Transform = StartSpot->GetTransform();
        Transform.Translation.Z += 200.f;
        Pawn = GameMode->SpawnDefaultPawnAtTransform(NewPlayer, Transform);
    }*/

    *Ret = Pawn;

    auto Num = NewPlayer->WorldInventory ? NewPlayer->WorldInventory->Inventory.ReplicatedEntries.Num() : 0;
    if (Num == 0)
    {
        if (VersionInfo.FortniteVersion <= 1.91 && VersionInfo.FortniteVersion != 1.1 && VersionInfo.FortniteVersion != 1.11 && NewPlayer->HasStrongMyHero())
        {
            static auto HeroCharPartsOffset = NewPlayer->StrongMyHero->GetOffset("CharacterParts");
            auto& HeroCharParts = GetFromOffset<TArray<UObject*>>(NewPlayer->StrongMyHero, HeroCharPartsOffset);
            static auto CharacterPartsOffset = NewPlayer->PlayerState->GetOffset("CharacterParts");
            auto& CharacterParts = GetFromOffset<const UObject * [0x6]>(NewPlayer->PlayerState, CharacterPartsOffset);
            
            if (HeroCharParts.Num() > 0)
            {
                for (auto& Part : HeroCharParts)
                {
                    static auto PartTypeOffset = Part->GetOffset("CharacterPartType");
                    CharacterParts[GetFromOffset<uint8>(Part, PartTypeOffset)] = Part;
                }
            }
            else
            {

                static auto Head = FindObject<UObject>(L"/Game/Characters/CharacterParts/Female/Medium/Heads/F_Med_Head1.F_Med_Head1");
                static auto Body = FindObject<UObject>(L"/Game/Characters/CharacterParts/Female/Medium/Bodies/F_Med_Soldier_01.F_Med_Soldier_01");
                static auto Backpack = FindObject<UObject>(L"/Game/Characters/CharacterParts/Backpacks/NoBackpack.NoBackpack");

                CharacterParts[0] = Head;
                CharacterParts[1] = Body;
                CharacterParts[3] = Backpack;
            }
        }

        if (NewPlayer->HasXPComponent())
        {
            if (NewPlayer->XPComponent->HasbRegisteredWithQuestManager())
            {
                NewPlayer->XPComponent->bRegisteredWithQuestManager = true;
                NewPlayer->XPComponent->OnRep_bRegisteredWithQuestManager();
            }

            if (NewPlayer->PlayerState->HasSeasonLevelUIDisplay())
            {
                if (FConfiguration::RandomizeLevels)
                {
                    int Level = VersionInfo.FortniteVersion < 11.00 ? std::rand() % 100 + 1 : std::rand() % 389 + 1;

                    NewPlayer->PlayerState->SeasonLevelUIDisplay = Level;
                    NewPlayer->PlayerState->OnRep_SeasonLevelUIDisplay();
                }
                else
                {
                    NewPlayer->PlayerState->SeasonLevelUIDisplay = NewPlayer->XPComponent->CurrentLevel;
                    NewPlayer->PlayerState->OnRep_SeasonLevelUIDisplay();
                }
            }
            //NewPlayer->XPComponent->OnProfileUpdated();
        }

        const UObject* BattleBusDef = nullptr;
        const UClass* SupplyDropClass = nullptr;
        if (VersionInfo.FortniteVersion == 18.40)
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_HeadbandBus.BBID_HeadbandBus");
        else if (VersionInfo.FortniteVersion == 1.11 || VersionInfo.FortniteVersion == 7.30 || VersionInfo.FortniteVersion == 11.31 || VersionInfo.FortniteVersion == 15.10 || VersionInfo.FortniteVersion == 19.01 ||
                 VersionInfo.FortniteVersion == 28.01)
        {
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_WinterBus.BBID_WinterBus");

            if (VersionInfo.FortniteVersion == 1.11)
                SupplyDropClass = FindObject<UClass>(L"/Game/Athena/SupplyDrops/B_AthenaSupplyDrop_Gift.B_AthenaSupplyDrop_Gift_C");
            else
                SupplyDropClass = FindObject<UClass>(L"/Game/Athena/SupplyDrops/AthenaSupplyDrop_Holiday.AthenaSupplyDrop_Holiday_C");
        }
        else if (VersionInfo.FortniteVersion == 23.10)
        {
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_BattleBus_Booster_Winter.BBID_BattleBus_Booster_Winter");
            SupplyDropClass = FindObject<UClass>(L"/Game/Athena/SupplyDrops/AthenaSupplyDrop_Holiday.AthenaSupplyDrop_Holiday_C");
        }
        else if (VersionInfo.FortniteVersion == 5.10 || VersionInfo.FortniteVersion == 9.41 || VersionInfo.FortniteVersion == 14.20 || VersionInfo.FortniteVersion == 18.00 || VersionInfo.FortniteVersion == 22.00 ||
                 VersionInfo.FortniteVersion == 26.20)
        {
            if (VersionInfo.FortniteVersion == 5.10)
                BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_BirthdayBus.BBID_BirthdayBus");
            else if (VersionInfo.FortniteVersion == 9.41)
                BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_BirthdayBus2nd.BBID_BirthdayBus2nd");
            else if (VersionInfo.FortniteVersion == 14.20)
                BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_BirthdayBus3rd.BBID_BirthdayBus3rd");
            else if (VersionInfo.FortniteVersion == 18.00)
                BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_BirthdayBus4th.BBID_BirthdayBus4th");
            else if (VersionInfo.FortniteVersion == 22.00)
                BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_BirthdayBus5th.BBID_BirthdayBus5th");
            else if (VersionInfo.FortniteVersion == 26.20)
                BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_BirthdayBus6th.BBID_BirthdayBus6th");

            SupplyDropClass = FindObject<UClass>(L"/Game/Athena/SupplyDrops/AthenaSupplyDrop_BDay.AthenaSupplyDrop_BDay_C");
        }
        else if (VersionInfo.FortniteVersion == 6.20 || VersionInfo.FortniteVersion == 6.21 || VersionInfo.FortniteVersion == 11.10 || VersionInfo.FortniteVersion == 14.40 || VersionInfo.FortniteVersion == 18.21)
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_HalloweenBus.BBID_HalloweenBus");
        else if (VersionInfo.FortniteVersion == 26.30)
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_HalloweenBus_Booster.BBID_HalloweenBus_Booster");
        else if (VersionInfo.FortniteVersion == 14.30)
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_BusUpgrade1.BBID_BusUpgrade1");
        else if (VersionInfo.FortniteVersion == 14.50)
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_BusUpgrade2.BBID_BusUpgrade2");
        else if (VersionInfo.FortniteVersion == 14.60)
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_BusUpgrade3.BBID_BusUpgrade3");
        else if (VersionInfo.FortniteVersion >= 12.30 && VersionInfo.FortniteVersion <= 12.61)
        {
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_DonutBus.BBID_DonutBus");
            SupplyDropClass = FindObject<UClass>(L"/Game/Athena/SupplyDrops/AthenaSupplyDrop_Donut.AthenaSupplyDrop_Donut_C");
        }
        else if (VersionInfo.FortniteVersion == 9.30)
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_WorldCupBus.BBID_WorldCupBus");
        else if (VersionInfo.FortniteVersion == 21.00)
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_CelebrationBus.BBID_CelebrationBus");
        else if (std::floor(VersionInfo.FortniteVersion) == 27)
            BattleBusDef = FindObject<UObject>(L"/Game/Athena/Items/Cosmetics/BattleBuses/BBID_DefaultBus.BBID_DefaultBus");

        if (BattleBusDef)
        {
            if (GameState->HasDefaultBattleBus())
                GameState->DefaultBattleBus = BattleBusDef;

            TArray<AFortAthenaAircraft*> Aircrafts;
            Utils::GetAll<AFortAthenaAircraft>(Aircrafts);
            for (auto& Aircraft : Aircrafts)
            {
                Aircraft->DefaultBusSkin = BattleBusDef;

                if (Aircraft->SpawnedCosmeticActor)
                {
                    static auto Offset = Aircraft->SpawnedCosmeticActor->GetOffset("ActiveSkin");

                    GetFromOffset<const UObject*>(Aircraft->SpawnedCosmeticActor, Offset) = BattleBusDef;
                }
            }
            Aircrafts.Free();
        }

        if (GameState->HasMapInfo() && GameState->MapInfo)
        {
            if (SupplyDropClass)
            {
                if (GameState->MapInfo->HasSupplyDropInfoList())
                    for (auto& Info : GameState->MapInfo->SupplyDropInfoList)
                        Info->SupplyDropClass = SupplyDropClass;
                else
                    GameState->MapInfo->SupplyDropClass = SupplyDropClass;
            }
        }
    }
    else
    {
        //NewPlayer->WorldInventory->Inventory.ReplicatedEntries.ResetNum();
        //NewPlayer->WorldInventory->Inventory.ItemInstances.ResetNum();

        /*for (int i = 0; i < NewPlayer->WorldInventory->Inventory.ItemInstances.Num(); i++)
        {
            auto& Entry = NewPlayer->WorldInventory->Inventory.ItemInstances[i]->ItemEntry;

            if (AFortInventory::IsPrimaryQuickbar(Entry.ItemDefinition) || Entry.ItemDefinition->IsA(AmmoClass) || Entry.ItemDefinition->IsA(ResourceClass))
            {
                NewPlayer->WorldInventory->Inventory.ItemInstances.Remove(i);
                i--;
            }
        }

        NewPlayer->WorldInventory->Update(nullptr);*/
    }
}


static void ApplyCustomSafeZoneState(AFortGameMode* GameMode, const char* source)
{
	if (!GameMode || !GameMode->SafeZoneIndicator ||
		!FConfiguration::bLateGame || !FConfiguration::bCustomSafeZone)
        return;

    auto GameState = (AFortGameStateAthena*)GameMode->GameState;
    if (GameState && GameState->HasMapInfo() && GameState->MapInfo)
        GUI::ResolveCustomSafeZoneForMap(GameState->MapInfo);

    AFortSafeZoneIndicator* Indicator = GameMode->SafeZoneIndicator;
    const auto LegacyCustomZone =
        FConfiguration::GetLegacyCustomSafeZoneNodeSnapshot();
    FVector Center(LegacyCustomZone.Center);
    float Radius = LegacyCustomZone.RadiusCm;

    // Old native storm code interpolates Last/Previous -> Next, while newer
    // versions also replicate NextNext (and sometimes a separate current
    // Radius). Keep every representation on the chosen circle so a generated
    // phase cannot pull the visible zone away during a transition.
    if (Indicator->HasLastCenter()) Indicator->LastCenter = Center;
    if (Indicator->HasPreviousCenter()) Indicator->PreviousCenter = Center;
    if (Indicator->HasNextCenter()) Indicator->NextCenter = Center;
    if (Indicator->HasNextNextCenter()) Indicator->NextNextCenter = Center;
    if (Indicator->HasLastRadius()) Indicator->LastRadius = Radius;
    if (Indicator->HasPreviousRadius()) Indicator->PreviousRadius = Radius;
    if (Indicator->HasNextRadius()) Indicator->NextRadius = Radius;
    if (Indicator->HasNextNextRadius()) Indicator->NextNextRadius = Radius;
    if (Indicator->HasRadius()) Indicator->Radius = Radius;

    if (Indicator->HasFutureReplicator() && Indicator->FutureReplicator)
    {
        if (Indicator->FutureReplicator->HasNextNextCenter())
            Indicator->FutureReplicator->NextNextCenter = Center;
        if (Indicator->FutureReplicator->HasNextNextRadius())
            Indicator->FutureReplicator->NextNextRadius = Radius;
    }

    // If this build exposes its phase array, update it as well. This makes the
    // override persistent instead of having to repair every later transition.
    if (Indicator->HasSafeZonePhases())
    {
        auto& SafeZonePhases = Indicator->SafeZonePhases;
        const int phaseCount = SafeZonePhases.IsValid() ? SafeZonePhases.Num() : 0;
        for (int i = 0; i < phaseCount && i < 100; ++i)
        {
            auto& Phase = SafeZonePhases.Get(i, FFortSafeZonePhaseInfo::Size());
            Phase.Center = Center;
            Phase.Radius = Radius;
        }
    }

    Indicator->ForceNetUpdate();

    const int phase = Indicator->HasCurrentPhase() ? Indicator->CurrentPhase : -1;
    SDK::DbgLog(
        "[SafeZoneMap] active custom zone source=%s phase=%d center=(%.1f, %.1f, %.1f) radius=%.1f\n",
        source ? source : "unknown", phase,
        Center.X, Center.Y, Center.Z, Radius);
}

// Chapter 1 uses a native indicator with separate live-wall and white-preview
// fields. Wait until fast-forward reaches the requested phase, then keep every
// representation on the same custom circle. Native timers still advance, but
// equal current/preview/future geometry makes those countdowns stationary.
static UWorld* GLegacyCustomZoneAppliedWorld = nullptr;
static AFortSafeZoneIndicator* GLegacyCustomZoneAppliedIndicator = nullptr;
static int GLegacyCustomZoneAppliedPhase = -1;
static void ApplyLegacyCustomSafeZoneAtTargetPhase(AFortGameMode* GameMode,
	int SafeZonePhase)
{
	if (!GameMode || VersionInfo.FortniteVersion >= 7.00 ||
		!FConfiguration::bLateGame || !FConfiguration::bCustomSafeZone ||
        SafeZonePhase < FConfiguration::LateGameZone || !GameMode->SafeZoneIndicator)
    {
        return;
    }

    auto World = UWorld::GetWorld();
    if (GLegacyCustomZoneAppliedWorld != World)
    {
        GLegacyCustomZoneAppliedWorld = World;
        GLegacyCustomZoneAppliedIndicator = nullptr;
        GLegacyCustomZoneAppliedPhase = -1;
    }

    auto Indicator = GameMode->SafeZoneIndicator;
    if (GLegacyCustomZoneAppliedIndicator == Indicator &&
        GLegacyCustomZoneAppliedPhase == SafeZonePhase)
        return;

    auto GameState = GameMode->GameState
        ? (AFortGameStateAthena*)GameMode->GameState
        : nullptr;
    if (GameState && GameState->HasMapInfo() && GameState->MapInfo)
        GUI::ResolveCustomSafeZoneForMap(GameState->MapInfo);

    const auto LegacyCustomZone =
        FConfiguration::GetLegacyCustomSafeZoneNodeSnapshot();
    FVector Center(LegacyCustomZone.Center);
    float Radius = LegacyCustomZone.RadiusCm;
    if (!std::isfinite(Center.X) || !std::isfinite(Center.Y) ||
        !std::isfinite(Center.Z) || !std::isfinite(Radius) || Radius <= 0.f)
    {
        SDK::DbgLog("[SafeZoneMap] rejected invalid lower custom circle phase=%d center=(%.1f, %.1f, %.1f) radius=%.1f\n",
            SafeZonePhase, Center.X, Center.Y, Center.Z, Radius);
        return;
    }

    const float PreviousPreviewRadius = Indicator->HasNextRadius()
        ? Indicator->NextRadius
        : Radius;

    // This native setter updates the physical wall/material as well as the
    // reflected live radius. It exists in 2.5-6.21; 1.x uses the field fallback.
    bool bUsedNativeSetter = false;
    bUsedNativeSetter =
        Indicator->TrySetSafeZoneRadiusAndCenter(
            Radius, Center);

    ApplyCustomSafeZoneState(
        GameMode, "legacy-native-phase");
    GLegacyCustomZoneAppliedIndicator = Indicator;
    GLegacyCustomZoneAppliedPhase = SafeZonePhase;

    SDK::DbgLog(
        "[SafeZoneMap] activated stationary lower custom circle phase=%d center=(%.1f, %.1f, %.1f) radius=%.1f oldPreview=%.1f finalPreview=%.1f setter=%d\n",
        SafeZonePhase, Center.X, Center.Y, Center.Z, Radius,
        PreviousPreviewRadius,
        Indicator->HasNextRadius() ? Indicator->NextRadius : -1.f,
        (int)bUsedNativeSetter);
}

void AFortGameMode::TickLateGameSafeZonePhaseFallback(UNetDriver* Driver)
{
    static UWorld* LastWorld = nullptr;
    static AFortGameMode* LastGameMode = nullptr;
    static AFortSafeZoneIndicator* LastIndicator = nullptr;
    static int LastTargetPhase = -1;
    static int LastAcceleratedPhase = -1;
    static int LastAlignedPhase = -1;
    static bool bTargetActionApplied = false;
    static bool bLoggedTarget = false;

    const auto ResetState = [&]()
    {
        LastWorld = nullptr;
        LastGameMode = nullptr;
        LastIndicator = nullptr;
        LastTargetPhase = -1;
        LastAcceleratedPhase = -1;
        LastAlignedPhase = -1;
        bTargetActionApplied = false;
        bLoggedTarget = false;
    };

    // These two releases do not resolve Erbium's native
    // HandlePostSafeZonePhaseChanged hook. Without that callback, LateGameZone
    // moves the bus but never fast-forwards the actual storm beyond phase 0.
    // Keep the workaround exact-version and self-disable if a finder succeeds.
    const bool bAffectedVersion =
        VersionInfo.FortniteVersion == 2.50 || VersionInfo.FortniteVersion == 7.30;
    if (!bAffectedVersion || GHasNativeLateGameSafeZonePhaseHook)
        return;

    if (!FConfiguration::bLateGame)
    {
        ResetState();
        return;
    }

    auto World = UWorld::GetWorld();
    auto GameMode = World ? (AFortGameMode*)World->AuthorityGameMode : nullptr;
    auto GameState = World ? (AFortGameStateAthena*)World->GameState : nullptr;
    auto Indicator = GameMode && GameMode->HasSafeZoneIndicator()
        ? GameMode->SafeZoneIndicator
        : nullptr;

    // Other transient/demo net drivers must not reset the active world's
    // one-shot target state between real server ticks.
    if (World && Driver != World->NetDriver)
        return;

    if (!World || !GameMode || !GameState || !Indicator ||
        !GameState->HasGamePhase() || GameState->GamePhase != 4 ||
        !GameMode->HasSafeZonePhase() ||
        !Indicator->HasSafeZoneStartShrinkTime() ||
        !Indicator->HasSafeZoneFinishShrinkTime())
    {
        ResetState();
        return;
    }

    const int TargetPhase = FConfiguration::LateGameZone;
    const int CurrentPhase = GameMode->SafeZonePhase;
    if (LastWorld != World || LastGameMode != GameMode ||
        LastIndicator != Indicator || LastTargetPhase != TargetPhase ||
        CurrentPhase < LastAcceleratedPhase)
    {
        LastWorld = World;
        LastGameMode = GameMode;
        LastIndicator = Indicator;
        LastTargetPhase = TargetPhase;
        LastAcceleratedPhase = -1;
        LastAlignedPhase = -1;
        bTargetActionApplied = false;
        bLoggedTarget = false;
    }

    if (TargetPhase < 1 || CurrentPhase < 0)
        return;

    const float TimeSeconds = (float)UGameplayStatics::GetTimeSeconds(World);
    const bool bPaused =
        UFortGameStateComponent_BattleRoyaleGamePhaseLogic::IsSafeZonePaused();

    // The pause sync runs before and after this compatibility tick. Do not let
    // any custom/legacy alignment writer replace the frozen snapshot between
    // those two passes; it will be applied normally after the user resumes.
    if (bPaused)
        return;

    const auto AlignLegacyFallbackCenter = [&]()
    {
        if (VersionInfo.FortniteVersion >= 7.00 ||
            FConfiguration::bCustomSafeZone || SafeZoneLoc.IsZero())
        {
            return;
        }

        if (ApplyLegacyFallbackSafeZonePlan(GameMode, CurrentPhase))
            return;

        if (Indicator->HasNextCenter())
            Indicator->NextCenter = SafeZoneLoc;
        if (Indicator->HasLastCenter())
            Indicator->LastCenter = SafeZoneLoc;
    };

    // Original Erbium reapplies its fallback anchor after every native phase
    // change, including phases after the selected starting phase.
	if (CurrentPhase != LastAlignedPhase)
	{
		const bool bMovingCustomZone =
			CustomSafeZoneRuntime::IsMovingModeRequested();
		const int32 CustomAlignmentPhase = bMovingCustomZone
			? CustomSafeZoneRuntime::ResolveRuntimeStartPhase() - 1
			: TargetPhase;
		if (FConfiguration::bCustomSafeZone &&
			CurrentPhase >= CustomAlignmentPhase)
		{
			auto ActiveMapInfo =
				GameState->HasMapInfo()
					? GameState->MapInfo
					: nullptr;
			bool bMovingApplied = false;
			if (bMovingCustomZone)
			{
				bMovingApplied = CustomSafeZoneRuntime::ApplyNativePhase(
					GameMode, ActiveMapInfo, CurrentPhase,
					TimeSeconds,
					"late-game-phase-fallback", true);
			}
			if (!bMovingApplied &&
				VersionInfo.FortniteVersion < 7.00)
			{
                ApplyLegacyCustomSafeZoneAtTargetPhase(
                    GameMode, CurrentPhase);
            }
            else if (!bMovingApplied)
            {
                ApplyCustomSafeZoneState(
                    GameMode,
                    "late-game-phase-fallback");
            }
        }
        else
        {
            AlignLegacyFallbackCenter();
        }
        LastAlignedPhase = CurrentPhase;
    }

    if (CurrentPhase < TargetPhase)
    {
        // A spawned indicator in the SafeZones game phase is active. 7.30 can
        // miss this native gate together with the phase callback, which would
        // otherwise make expired timestamps inert.
        if (GameMode->HasbSafeZoneActive() && !GameMode->bSafeZoneActive)
            GameMode->bSafeZoneActive = true;

        // Mirror Erbium's native hook exactly: start a short, real shrink once
        // per observed phase. Expiring both timestamps immediately advances the
        // counter without giving Radius/LastRadius a frame to interpolate.
        if (CurrentPhase == LastAcceleratedPhase)
            return;

        Indicator->SafeZoneStartShrinkTime = (float)TimeSeconds;
        Indicator->SafeZoneFinishShrinkTime = TimeSeconds + 0.15f;
        LastAcceleratedPhase = CurrentPhase;

        SDK::DbgLog(
            "[SafeZone] fallback fast-forward version=%.2f phase=%d -> target=%d start=%.2f finish=%.2f radius=%.1f last=%.1f next=%.1f\n",
            VersionInfo.FortniteVersion, CurrentPhase, TargetPhase,
            Indicator->SafeZoneStartShrinkTime,
            Indicator->SafeZoneFinishShrinkTime,
            Indicator->HasRadius() ? Indicator->Radius : -1.f,
            Indicator->HasLastRadius() ? Indicator->LastRadius : -1.f,
            Indicator->HasNextRadius() ? Indicator->NextRadius : -1.f);
        return;
    }

    if (!bTargetActionApplied)
    {
        bTargetActionApplied = true;
        if (FConfiguration::bLateGameLongZone)
        {
            UFortGameStateComponent_BattleRoyaleGamePhaseLogic::
                SetSafeZonePaused(true);
        }
    }

    if (!bLoggedTarget)
    {
        bLoggedTarget = true;
        SDK::DbgLog(
            "[SafeZone] fallback reached target version=%.2f phase=%d target=%d radius=%.1f last=%.1f next=%.1f\n",
            VersionInfo.FortniteVersion, CurrentPhase, TargetPhase,
            Indicator->HasRadius() ? Indicator->Radius : -1.f,
            Indicator->HasLastRadius() ? Indicator->LastRadius : -1.f,
            Indicator->HasNextRadius() ? Indicator->NextRadius : -1.f);
        if (Indicator->HasLastCenter() && Indicator->HasNextCenter())
        {
            SDK::DbgLog(
                "[SafeZone] fallback target centers last=(%.1f, %.1f, %.1f) next=(%.1f, %.1f, %.1f) anchor=(%.1f, %.1f, %.1f)\n",
                Indicator->LastCenter.X, Indicator->LastCenter.Y, Indicator->LastCenter.Z,
                Indicator->NextCenter.X, Indicator->NextCenter.Y, Indicator->NextCenter.Z,
                SafeZoneLoc.X, SafeZoneLoc.Y, SafeZoneLoc.Z);
        }
    }
}

namespace
{
    bool IsSaneLegacySafeZoneDurationArrayHeader(
        const TArray<float>& Durations)
    {
        constexpr int32 MaxSafeZonePhaseCount = 32;
        if (Durations.Num() < 0 ||
            Durations.Max() < Durations.Num() ||
            Durations.Max() > MaxSafeZonePhaseCount)
        {
            return false;
        }

        if (Durations.Num() == 0)
        {
            return Durations.Max() == 0
                ? Durations.Data == nullptr
                : Durations.Data && SDK::MemReadable(
                    Durations.Data,
                    sizeof(float) * (size_t)Durations.Max());
        }

        return Durations.Data && SDK::MemReadable(
            Durations.Data,
            sizeof(float) * (size_t)Durations.Num());
    }

    bool HydrateLegacyNativeSafeZoneDurations(
        AFortGameMode* GameMode,
        AFortGameStateAthena* GameState)
    {
        // 13.00-20.x caches its phase wait/shrink curves in two native arrays
        // inside SafeZoneDefinition. Native transitions can receive empty
        // arrays on a listen server, leaving every deadline already expired.
        if (VersionInfo.FortniteVersion < 13.00 ||
            VersionInfo.FortniteVersion >= 21.10 ||
            !GameMode || !GameState ||
            !GameState->HasMapInfo() || !GameState->MapInfo ||
            !GameState->MapInfo->HasSafeZoneDefinition())
        {
            return false;
        }

        int32 ShrinkDurationsOffset = 0x258;
        if (VersionInfo.FortniteVersion >= 17.00)
            ShrinkDurationsOffset = 0x248;
        else if (VersionInfo.FortniteVersion < 15.20)
            ShrinkDurationsOffset = 0x1f8;

        constexpr int32 HoldDurationsDelta = 0x10;
        const int32 HoldDurationsOffset =
            ShrinkDurationsOffset - HoldDurationsDelta;

        auto DefinitionStruct = FFortSafeZoneDefinition::StaticStruct();
        if (!DefinitionStruct)
            return false;

        const int32 DefinitionSize =
            DefinitionStruct->GetPropertiesSize();
        if (DefinitionSize <
                ShrinkDurationsOffset +
                    (int32)sizeof(TArray<float>) ||
            DefinitionSize > 0x1000)
        {
            return false;
        }

        auto MapInfo = GameState->MapInfo;
        auto DefinitionBytes =
            reinterpret_cast<uint8*>(&MapInfo->SafeZoneDefinition);
        if (!SDK::MemReadable(
                DefinitionBytes + HoldDurationsOffset,
                sizeof(TArray<float>)) ||
            !SDK::MemReadable(
                DefinitionBytes + ShrinkDurationsOffset,
                sizeof(TArray<float>)))
        {
            return false;
        }

        auto& HoldDurations = *reinterpret_cast<TArray<float>*>(
            DefinitionBytes + HoldDurationsOffset);
        auto& ShrinkDurations = *reinterpret_cast<TArray<float>*>(
            DefinitionBytes + ShrinkDurationsOffset);
        if (!IsSaneLegacySafeZoneDurationArrayHeader(HoldDurations) ||
            !IsSaneLegacySafeZoneDurationArrayHeader(ShrinkDurations))
        {
            return false;
        }

        if (!FFortSafeZoneDefinition::HasCount() ||
            !FFortSafeZoneDefinition::HasWaitTime() ||
            !FFortSafeZoneDefinition::HasShrinkTime())
        {
            return false;
        }

        auto& Definition = MapInfo->SafeZoneDefinition;
        const float EvaluatedPhaseCount = Definition.Count.Evaluate();
        const float RoundedPhaseCount = std::round(EvaluatedPhaseCount);
        if (!std::isfinite(EvaluatedPhaseCount) ||
            std::abs(EvaluatedPhaseCount - RoundedPhaseCount) > 0.01f ||
            RoundedPhaseCount < 1.f || RoundedPhaseCount > 32.f)
        {
            return false;
        }

        const int32 PhaseCount = (int32)RoundedPhaseCount;
        const int32 ShrinkCount = PhaseCount;
        const int32 HoldCount = PhaseCount;
        std::vector<float> EvaluatedShrinkDurations(
            (size_t)ShrinkCount);
        std::vector<float> EvaluatedHoldDurations(
            (size_t)HoldCount);

        const auto EvaluateDurations = [](
            FScalableFloat& Source,
            std::vector<float>& OutDurations)
        {
            for (int32 Index = 0;
                Index < (int32)OutDurations.size(); ++Index)
            {
                const float EvaluatedDuration =
                    Source.Evaluate((float)Index);
                if (!std::isfinite(EvaluatedDuration) ||
                    EvaluatedDuration < 0.f)
                {
                    return false;
                }

                OutDurations[(size_t)Index] =
                    EvaluatedDuration;
            }

            return true;
        };

        // Evaluate everything before mutating native state. A missing curve row
        // must not leave half of either schedule updated.
        if (!EvaluateDurations(
                Definition.ShrinkTime,
                EvaluatedShrinkDurations) ||
            !EvaluateDurations(
                Definition.WaitTime,
                EvaluatedHoldDurations))
        {
            SDK::DbgLog(
                "[SafeZone] native duration hydration failed "
                "version=%.2f phases=%d/%d\n",
                VersionInfo.FortniteVersion,
                HoldCount, ShrinkCount);
            return false;
        }

        const auto CommitDurations = [](
            TArray<float>& NativeDurations,
            const std::vector<float>& EvaluatedDurations)
        {
            if (NativeDurations.Num() !=
                (int32)EvaluatedDurations.size())
            {
                NativeDurations.ResetNum();
            }

            if (NativeDurations.Num() == 0)
            {
                for (const float Duration : EvaluatedDurations)
                    NativeDurations.Add(Duration);
            }
            else
            {
                for (int32 Index = 0;
                    Index < NativeDurations.Num(); ++Index)
                {
                    NativeDurations[Index] =
                        EvaluatedDurations[(size_t)Index];
                }
            }
        };

        CommitDurations(
            ShrinkDurations, EvaluatedShrinkDurations);
        CommitDurations(
            HoldDurations, EvaluatedHoldDurations);
        if (!IsSaneLegacySafeZoneDurationArrayHeader(HoldDurations) ||
            !IsSaneLegacySafeZoneDurationArrayHeader(ShrinkDurations) ||
            HoldDurations.Num() != HoldCount ||
            ShrinkDurations.Num() != ShrinkCount)
        {
            return false;
        }

        const int32 ProbePhase = std::clamp(
            FConfiguration::LateGameZone.load(),
            0,
            (std::min)(
                ShrinkDurations.Num(),
                HoldDurations.Num()) - 1);
        static UWorld* LastLoggedWorld = nullptr;
        static AFortAthenaMapInfo* LastLoggedMapInfo = nullptr;
        auto World = UWorld::GetWorld();
        if (LastLoggedWorld != World ||
            LastLoggedMapInfo != MapInfo)
        {
            LastLoggedWorld = World;
            LastLoggedMapInfo = MapInfo;
            SDK::DbgLog(
                "[SafeZone] hydrated native phase durations "
                "version=%.2f offsets=0x%X/0x%X phases=%d/%d "
                "target=%d wait=%.2f shrink=%.2f\n",
                VersionInfo.FortniteVersion,
                HoldDurationsOffset, ShrinkDurationsOffset,
                HoldDurations.Num(), ShrinkDurations.Num(),
                ProbePhase,
                HoldDurations[ProbePhase],
                ShrinkDurations[ProbePhase]);
        }
        return true;
    }

    bool TryEvaluateLegacySafeZonePhaseDurations(
        AFortGameStateAthena* GameState,
        int32 Phase,
        float& OutWaitDuration,
        float& OutShrinkDuration)
    {
        OutWaitDuration = 0.f;
        OutShrinkDuration = 0.f;
        if (VersionInfo.FortniteVersion < 13.00 ||
            VersionInfo.FortniteVersion >= 21.10 ||
            !GameState || !GameState->HasMapInfo() ||
            !GameState->MapInfo ||
            !GameState->MapInfo->HasSafeZoneDefinition() ||
            !FFortSafeZoneDefinition::HasCount() ||
            !FFortSafeZoneDefinition::HasWaitTime() ||
            !FFortSafeZoneDefinition::HasShrinkTime())
        {
            return false;
        }

        auto& Definition = GameState->MapInfo->SafeZoneDefinition;
        const float EvaluatedPhaseCount = Definition.Count.Evaluate();
        const float RoundedPhaseCount = std::round(EvaluatedPhaseCount);
        if (!std::isfinite(EvaluatedPhaseCount) ||
            std::abs(EvaluatedPhaseCount - RoundedPhaseCount) > 0.01f ||
            RoundedPhaseCount < 1.f || RoundedPhaseCount > 32.f ||
            Phase < 0 || Phase >= (int32)RoundedPhaseCount)
        {
            return false;
        }

        OutWaitDuration = Definition.WaitTime.Evaluate((float)Phase);
        OutShrinkDuration = Definition.ShrinkTime.Evaluate((float)Phase);
        return std::isfinite(OutWaitDuration) &&
            std::isfinite(OutShrinkDuration) &&
            OutWaitDuration >= 0.f && OutShrinkDuration >= 0.f;
    }

    int32 ResolveLegacySafeZonePhaseAfterTransition(
        AFortGameMode* GameMode,
        AFortGameStateAthena* GameState,
        AFortSafeZoneIndicator* Indicator,
        int32 RequestedPhase)
    {
        struct FPhaseFallbackState
        {
            UWorld* World = nullptr;
            AFortGameMode* GameMode = nullptr;
            AFortSafeZoneIndicator* Indicator = nullptr;
            int32 LastPhase = -1;
            bool bObservedPhase = false;
        };
        static FPhaseFallbackState State;

        auto World = UWorld::GetWorld();
        if (State.World != World || State.GameMode != GameMode ||
            State.Indicator != Indicator)
        {
            State = {};
            State.World = World;
            State.GameMode = GameMode;
            State.Indicator = Indicator;
        }

        const auto IsSanePhase = [](int32 Phase)
        {
            return Phase >= 0 && Phase <= 31;
        };

        int32 Phase = -1;
        if (GameMode && GameMode->HasSafeZonePhase())
        {
            const int32 Candidate = GameMode->SafeZonePhase;
            if (IsSanePhase(Candidate))
                Phase = Candidate;
        }
        if (!IsSanePhase(Phase) &&
            GameState && GameState->HasSafeZonePhase())
        {
            const int32 Candidate = GameState->SafeZonePhase;
            if (IsSanePhase(Candidate))
                Phase = Candidate;
        }
        if (!IsSanePhase(Phase) &&
            Indicator && Indicator->HasCurrentPhase())
        {
            const int32 Candidate = Indicator->CurrentPhase;
            if (IsSanePhase(Candidate))
                Phase = Candidate;
        }
        if (!IsSanePhase(Phase) && IsSanePhase(RequestedPhase))
            Phase = RequestedPhase;
        if (!IsSanePhase(Phase))
            Phase = State.bObservedPhase ? State.LastPhase + 1 : 0;

        if (!IsSanePhase(Phase))
            return -1;

        State.LastPhase = Phase;
        State.bObservedPhase = true;
        return Phase;
    }
}

void AFortGameMode::HandlePostSafeZonePhaseChanged(AFortGameMode* GameMode, int NewSafeZonePhase_Inp)
{
    if (!GameMode || !GameMode->SafeZoneIndicator)
        return;

    auto GameState = (AFortGameStateAthena*)GameMode->GameState;

    if (VersionInfo.FortniteVersion >= 21.10)
    {
        if (HandlePostSafeZonePhaseChangedOG)
            HandlePostSafeZonePhaseChangedOG(GameMode, NewSafeZonePhase_Inp);

        return;
    }

    // Restore the native curve cache before it calculates this phase. Keep the
    // native transition as the sole owner of geometry; any missing absolute
    // countdown is repaired only after this callback so 15.30 cannot have its
    // native schedule replaced before its geometry transition.
    HydrateLegacyNativeSafeZoneDurations(GameMode, GameState);
    HandlePostSafeZonePhaseChangedOG(GameMode, NewSafeZonePhase_Inp);

    const int32 ActiveSafeZonePhase =
        ResolveLegacySafeZonePhaseAfterTransition(
            GameMode, GameState, GameMode->SafeZoneIndicator,
            NewSafeZonePhase_Inp);
	float TimeSeconds =
		(float)UGameplayStatics::GetTimeSeconds(GameState);
	auto ActiveMapInfo = GameState && GameState->HasMapInfo()
		? GameState->MapInfo
		: nullptr;
	const auto TryPublishMovingZone = [&]()
	{
		return CustomSafeZoneRuntime::IsMovingModeRequested() &&
			CustomSafeZoneRuntime::ApplyNativePhase(
				GameMode, ActiveMapInfo, ActiveSafeZonePhase,
				TimeSeconds, "native-phase-change", true);
	};
	bool bMovingCustomZone = false;

    if (FConfiguration::bLateGame &&
        ActiveSafeZonePhase >= 0 &&
        ActiveSafeZonePhase < FConfiguration::LateGameZone)
    {
        GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = TimeSeconds;
        GameMode->SafeZoneIndicator->SafeZoneFinishShrinkTime = GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime + 0.15f;
        GameMode->SafeZoneIndicator->ForceNetUpdate();
		bMovingCustomZone = TryPublishMovingZone();
		if (!bMovingCustomZone &&
			VersionInfo.FortniteVersion >= 7.00 &&
			FConfiguration::bLateGame &&
			FConfiguration::bCustomSafeZone)
		{
			ApplyCustomSafeZoneState(
				GameMode, "native-phase-change-fallback");
		}
        return;
    }

    // The native 13.00-20.x callback advances phase/geometry but does not arm
    // the indicator's absolute countdown on a listen server. The previous
    // fast-forward deadline would otherwise remain expired at the target and
    // cascade through every remaining phase in the same tick. Arm the new
    // phase once, after native geometry has settled, using the map's own timing
    // curves so playlist and LTM overrides remain intact.
    if (ActiveSafeZonePhase >= 0 &&
        (!FConfiguration::bLateGame ||
            ActiveSafeZonePhase >= FConfiguration::LateGameZone))
    {
        auto Indicator = GameMode->SafeZoneIndicator;
        const bool bHasNativeFutureDeadline =
            Indicator->HasSafeZoneStartShrinkTime() &&
            Indicator->HasSafeZoneFinishShrinkTime() &&
            std::isfinite(Indicator->SafeZoneStartShrinkTime) &&
            std::isfinite(Indicator->SafeZoneFinishShrinkTime) &&
            Indicator->SafeZoneFinishShrinkTime > TimeSeconds &&
            Indicator->SafeZoneFinishShrinkTime >=
                Indicator->SafeZoneStartShrinkTime;
        if (!bHasNativeFutureDeadline)
        {
            float WaitDuration = 0.f;
            float ShrinkDuration = 0.f;
            if (TryEvaluateLegacySafeZonePhaseDurations(
                    GameState, ActiveSafeZonePhase,
                    WaitDuration, ShrinkDuration))
            {
                Indicator->SafeZoneStartShrinkTime =
                    TimeSeconds + WaitDuration;
                Indicator->SafeZoneFinishShrinkTime =
                    Indicator->SafeZoneStartShrinkTime + ShrinkDuration;
                Indicator->ForceNetUpdate();
                SDK::DbgLog(
                    "[SafeZone] armed missing native countdown "
                    "version=%.2f phase=%d wait=%.2f shrink=%.2f "
                    "start=%.2f finish=%.2f\n",
                    VersionInfo.FortniteVersion, ActiveSafeZonePhase,
                    WaitDuration, ShrinkDuration,
                    Indicator->SafeZoneStartShrinkTime,
                    Indicator->SafeZoneFinishShrinkTime);
            }
        }
    }

	// Native timing is now known (or repaired). Overlay explicit authored zero
	// and non-zero edge durations only after this point so repair cannot erase
	// them, while omitted values inherit the native schedule.
	bMovingCustomZone = TryPublishMovingZone();
	if (!bMovingCustomZone &&
		VersionInfo.FortniteVersion >= 7.00 &&
		FConfiguration::bLateGame &&
		FConfiguration::bCustomSafeZone)
	{
		ApplyCustomSafeZoneState(
			GameMode, "native-phase-change-fallback");
	}

    // Original Erbium applies the selected late-game center only after the
    // native fast-forward reaches its target phase. Applying it to each skipped
    // phase makes the old client's current and preview circles disagree.
	if (!bMovingCustomZone &&
		VersionInfo.FortniteVersion < 7.00 && FConfiguration::bLateGame &&
        FConfiguration::bCustomSafeZone)
    {
        ApplyLegacyCustomSafeZoneAtTargetPhase(GameMode, ActiveSafeZonePhase);
    }
	else if (!bMovingCustomZone &&
		VersionInfo.FortniteVersion < 7.00 &&
        FConfiguration::bLateGame &&
        (SafeZoneLoc.X != 0 || SafeZoneLoc.Y != 0 || SafeZoneLoc.Z != 0))
    {
        if (!ApplyLegacyFallbackSafeZonePlan(
            GameMode, ActiveSafeZonePhase))
        {
            if (GameMode->SafeZoneIndicator->HasNextCenter())
                GameMode->SafeZoneIndicator->NextCenter = SafeZoneLoc;
            if (GameMode->SafeZoneIndicator->HasLastCenter())
                GameMode->SafeZoneIndicator->LastCenter = SafeZoneLoc;
        }
    }

    if (FConfiguration::bLateGame &&
        ActiveSafeZonePhase == FConfiguration::LateGameZone &&
        FConfiguration::bLateGameLongZone)
    {
        // Pause only after every custom/legacy geometry writer has finished so
        // the resumable snapshot belongs to the circle the player actually sees.
        UFortGameStateComponent_BattleRoyaleGamePhaseLogic::
            SetSafeZonePaused(true);
    }

    if (ActiveSafeZonePhase >
        (FConfiguration::bLateGame.load()
            ? FConfiguration::LateGameZone.load()
            : 1))
    {
        for (auto& UncastedPlayer : GameMode->AlivePlayers)
        {
            auto PlayerController = (AFortPlayerControllerAthena*)UncastedPlayer;

            UFortQuestManager::TrySendStatEvent(PlayerController,
                EFortQuestObjectiveStatEvent::GetStormPhase(), 1, false);
        }
    }
}


int16_t WorldPlayerId = 0;
void AFortGameMode::HandleStartingNewPlayer_(UObject* Context, FFrame& Stack)
{
    AFortPlayerControllerAthena* NewPlayer;
    Stack.StepCompiledIn(&NewPlayer);
    Stack.IncrementCode();
    auto GameMode = (AFortGameMode*)Context;
    auto GameState = (AFortGameStateAthena*)GameMode->GameState;
    AFortPlayerStateAthena* PlayerState = (AFortPlayerStateAthena*)NewPlayer->PlayerState;
    const bool bIsBot = PlayerState && PlayerState->HasbIsABot() &&
        PlayerState->bIsABot;
    const bool bRepairLateSeasonTeam =
        ShouldRepairLateSeasonTeams() && !bIsBot;
    uint8 ReservedLateSeasonTeam = 0;

    if (bRepairLateSeasonTeam)
    {
        auto Playlist = ResolveActivePlaylist(GameState);
        ReservedLateSeasonTeam =
            ReserveLateSeasonHumanTeam(
                GameMode, NewPlayer, PlayerState, Playlist);
        ApplyLateSeasonHumanTeam(
            GameState,
            NewPlayer,
            PlayerState,
            ReservedLateSeasonTeam,
            GLateSeasonHumanTeams.FirstTeam,
            false,
            "pre-native");
    }

    if (VersionInfo.FortniteVersion <= 2.5)
    {
        NewPlayer->QuickBars = UWorld::SpawnActor<AFortQuickBars>(FVector{});
        NewPlayer->QuickBars->SetOwner(NewPlayer);
    }

    if (!bRepairLateSeasonTeam && PlayerState->HasSquadId())
    {
        PlayerState->SquadId = PlayerState->TeamIndex - 3;
        PlayerState->OnRep_SquadId();
    }

    if (!bRepairLateSeasonTeam && GameState->HasGameMemberInfoArray())
    {
        auto Member = (FGameMemberInfo*)malloc(FGameMemberInfo::Size());
        memset((PBYTE)Member, 0, FGameMemberInfo::Size());

        Member->MostRecentArrayReplicationKey = -1;
        Member->ReplicationID = -1;
        Member->ReplicationKey = -1;
        Member->TeamIndex = PlayerState->TeamIndex;
        Member->SquadId = PlayerState->SquadId;
        Member->MemberUniqueId = PlayerState->HasUniqueID() ? PlayerState->UniqueID : PlayerState->UniqueId;

        auto& NewMember = GameState->GameMemberInfoArray.Members.Add(*Member, FGameMemberInfo::Size());
        GameState->GameMemberInfoArray.MarkItemDirty(NewMember);
        
        auto NotifyGameMemberAdded = (void(*)(AFortGameStateAthena*, uint8_t, uint8_t, FUniqueNetIdRepl*)) NotifyGameMemberAdded_;
        if (NotifyGameMemberAdded)
            NotifyGameMemberAdded(GameState, Member->SquadId, Member->TeamIndex, &Member->MemberUniqueId);

        free(Member);
    }

    if (!NewPlayer->WorldInventory)
    {
        NewPlayer->WorldInventory = UWorld::SpawnActor<AFortInventory>(NewPlayer->WorldInventoryClass, FVector{}, FRotator{}, NewPlayer);
        NewPlayer->WorldInventory->InventoryType = 0;
    }

    if (PlayerState->HasWorldPlayerId())
        PlayerState->WorldPlayerId = WorldPlayerId++;

    if (wcsstr(FConfiguration::Playlist, L"/Game/Athena/Playlists/Creative/Playlist_PlaygroundV2.Playlist_PlaygroundV2"))
        AFortAthenaCreativePortal::Create(NewPlayer);

    callOG(GameMode, Stack.GetCurrentNativeFunction(), HandleStartingNewPlayer, NewPlayer);

    // Native 17/18 startup does not consistently call the PickTeam routine
    // Magnesium hooks. Reapply after it returns in case it restored a default
    // PlayerTeam, then correct any GameMemberInfo entry it created.
    if (bRepairLateSeasonTeam && IsSaneObject(NewPlayer) &&
        NewPlayer->PlayerState)
    {
        GameState = (AFortGameStateAthena*)GameMode->GameState;
        PlayerState = (AFortPlayerStateAthena*)NewPlayer->PlayerState;
        const bool bAppliedTeam = ApplyLateSeasonHumanTeam(
            GameState,
            NewPlayer,
            PlayerState,
            ReservedLateSeasonTeam,
            GLateSeasonHumanTeams.FirstTeam,
            true,
            "post-native");
        if (bAppliedTeam)
        {
            UpsertLateSeasonGameMemberInfo(GameState, PlayerState);
            auto Playlist = ResolveActivePlaylist(GameState);
            ApplyLateSeasonDBNOSettings(
                GameMode, GameState, Playlist, "post-native");
        }
    }

    return;
}


bool AFortGameMode::AssignCheatBotIsolatedTeam(
    AFortGameMode* GameMode,
    AFortPlayerControllerAthena* BotController,
    AActor* Instigator,
    uint8& OutTeamIndex)
{
    OutTeamIndex = 0;
    // Playlist-backed team graphs did not exist before FN 3.5. Let the
    // caller use its established numeric-team compatibility path there.
    if (VersionInfo.FortniteVersion < 3.5 ||
        !IsSaneObject(GameMode) || !IsSaneObject(BotController))
    {
        return false;
    }

    auto RawPlayerState = BotController->PlayerState;
    if (!IsSaneObject(RawPlayerState))
        return false;

    auto PlayerState =
        RawPlayerState->Cast<AFortPlayerStateAthena>();
    auto GameState = GameMode->GameState;
    if (!IsSaneObject(PlayerState) || !IsSaneObject(GameState) ||
        !PlayerState->HasTeamIndex())
    {
        return false;
    }

    // A cheat bot is intentionally not a squad member or revive partner. Find
    // an existing playable team object unused by every other live controller;
    // choosing from the native Teams graph also avoids requesting a team that
    // this particular build never initialized.
    std::array<bool, 256> UsedTeams{};
    auto MarkControllerTeam =
        [&](AActor* Actor)
        {
            if (!IsSaneObject(Actor) ||
                (void*)Actor == (void*)BotController)
            {
                return;
            }

            auto Controller =
                Actor->Cast<AFortPlayerControllerAthena>();
            auto RawState = IsSaneObject(Controller)
                ? Controller->PlayerState
                : nullptr;
            auto State = IsSaneObject(RawState)
                ? RawState->Cast<AFortPlayerStateAthena>()
                : nullptr;
            if (IsSaneObject(State) && State->HasTeamIndex())
                UsedTeams[State->TeamIndex] = true;
        };
    if (GameMode->HasAlivePlayers())
    {
        for (auto Actor : GameMode->AlivePlayers)
            MarkControllerTeam(Actor);
    }
    if (GameMode->HasAliveBots())
    {
        for (auto Actor : GameMode->AliveBots)
            MarkControllerTeam(Actor);
    }
    if (GameState->HasPlayerArray())
    {
        for (auto State : GameState->PlayerArray)
        {
            if (IsSaneObject(State) && State != PlayerState &&
                State->HasTeamIndex())
            {
                UsedTeams[State->TeamIndex] = true;
            }
        }
    }

    // Resolve through the capability-checked shared path. In particular,
    // never read CurrentPlaylistData merely because CurrentPlaylistInfo is
    // absent: pre-playlist builds have neither property, and DEFINE_PROP's
    // missing offset would otherwise manufacture a garbage pointer.
    auto Playlist = GetPublishedPlaylist(GameState);
    const uint8 FirstTeam = GetPlaylistFirstTeam(Playlist);
    int32 LastTeamExclusive = 250;
    bool bHasActiveTeamBound = false;
    if (Playlist)
    {
        const int32 LastTeamOffset =
            (int32)Playlist->GetOffset("DefaultLastTeam");
        if (LastTeamOffset >= 0 && SDK::MemReadable(
                (const uint8*)Playlist + LastTeamOffset,
                sizeof(uint8)))
        {
            const uint8 LastTeam = GetFromOffset<uint8>(
                Playlist, LastTeamOffset);
            if (LastTeam >= FirstTeam && LastTeam < 250)
            {
                LastTeamExclusive = (int32)LastTeam + 1;
                bHasActiveTeamBound = true;
            }
        }
    }

    int32 PlaylistTeamCount = GetLateSeasonIntProperty(
        Playlist, "MaxTeamCount", 0);
    int32 GameStateTeamCount = GetLateSeasonIntProperty(
        GameState, "TeamCount", 0);
    auto ApplyTeamCountBound = [&](int32 TeamCount)
        {
            if (TeamCount < 1 || TeamCount > 247)
                return;
            LastTeamExclusive = std::min<int32>(
                LastTeamExclusive,
                (int32)FirstTeam + TeamCount);
            bHasActiveTeamBound = true;
        };
    ApplyTeamCountBound(PlaylistTeamCount);
    ApplyTeamCountBound(GameStateTeamCount);
    if (!bHasActiveTeamBound && Playlist)
    {
        int32 TeamSize = Playlist->HasMaxSquadSize()
            ? Playlist->MaxSquadSize
            : 1;
        if (TeamSize < 1)
            TeamSize = 1;
        if (Playlist->HasMaxPlayers() && Playlist->MaxPlayers > 0)
        {
            const int32 DerivedTeamCount =
                (Playlist->MaxPlayers + TeamSize - 1) / TeamSize;
            ApplyTeamCountBound(DerivedTeamCount);
        }
    }
    if (!bHasActiveTeamBound ||
        LastTeamExclusive <= FirstTeam)
    {
        SDK::DbgLog(
            "[SpawnBot] no validated active team range first=%u "
            "playlistCount=%d gameStateCount=%d version=%.2f\n",
            (unsigned)FirstTeam,
            PlaylistTeamCount,
            GameStateTeamCount,
            VersionInfo.FortniteVersion);
        return false;
    }

    uint8 CandidateTeam = 0;
    UObject* CandidateTeamInfo = nullptr;
    const int32 TeamsOffset = (int32)GameState->GetOffset("Teams");
    if (TeamsOffset >= 0 && SDK::MemReadable(
            (const uint8*)GameState + TeamsOffset,
            sizeof(TArray<UObject*>)))
    {
        auto& Teams = GetFromOffset<TArray<UObject*>>(
            GameState,
            TeamsOffset);
        if (Teams.Num() > 0 && Teams.Num() <= 256 &&
            SDK::MemReadable(
                Teams.GetData(),
                sizeof(UObject*) * Teams.Num()))
        {
            for (auto TeamInfo : Teams)
            {
                if (!IsSaneObject(TeamInfo))
                    continue;
                const int32 TeamOffset =
                    (int32)TeamInfo->GetOffset("Team");
                if (TeamOffset < 0 || !SDK::MemReadable(
                        (const uint8*)TeamInfo + TeamOffset,
                        sizeof(uint8)))
                {
                    continue;
                }

                const uint8 Team =
                    GetFromOffset<uint8>(TeamInfo, TeamOffset);
                auto Members = GetLateSeasonTeamMembers(TeamInfo);
                if (Team >= FirstTeam &&
                    Team < LastTeamExclusive &&
                    !UsedTeams[Team] && Members &&
                    Members->Num() == 0 &&
                    (!CandidateTeam || Team < CandidateTeam))
                {
                    CandidateTeam = Team;
                    CandidateTeamInfo = TeamInfo;
                }
            }
        }
    }
    if (CandidateTeam < FirstTeam || !IsSaneObject(CandidateTeamInfo))
        return false;

    struct FObjectReferenceField
    {
        uint32 Offset = uint32(-1);
        bool bWeak = false;
    };

    auto ResolveObjectReferenceField =
        [](UObject* Owner, const char* Name,
            FObjectReferenceField& OutField)
        {
            OutField = {};
            if (!IsSaneObject(Owner))
            {
                return false;
            }

            auto Property = Owner->GetProperty(Name, 0x8010000);
            if (!Property || !SDK::MemReadable(
                    Property, std::max<size_t>(
                        Offsets::Offset_Internal + sizeof(uint32),
                        Offsets::ElementSize + sizeof(uint32))))
            {
                return false;
            }

            uint64 FieldFlags = 0;
            if (VersionInfo.FortniteVersion >= 12.10)
            {
                if (!SDK::MemReadable(
                        (const uint8*)Property + 0x8,
                        sizeof(void*)))
                {
                    return false;
                }
                auto FieldClass = GetFromOffset<void*>(Property, 0x8);
                if (!FieldClass || !SDK::MemReadable(
                        (const uint8*)FieldClass + 0x10,
                        sizeof(uint64)))
                {
                    return false;
                }
                FieldFlags = GetFromOffset<uint64>(FieldClass, 0x10);
            }
            else
            {
                if (!Property->Class || !SDK::MemReadable(
                        Property->Class, sizeof(UClass)))
                {
                    return false;
                }
                FieldFlags = Property->Class->GetCastFlags();
            }

            constexpr uint64 ObjectReferenceFlags = 0x8010000;
            constexpr uint64 WeakObjectPropertyFlag = 0x8000000;
            if (!(FieldFlags & ObjectReferenceFlags))
                return false;

            const uint32 Offset = SDK::ReadPropertyOffset(
                GetFromOffset<uint32>(
                    Property, Offsets::Offset_Internal));
            const uint32 ElementSize = GetFromOffset<uint32>(
                Property, Offsets::ElementSize);
            const int32 OwnerSize = Owner->Class->GetPropertiesSize();
            if (Offset == uint32(-1) ||
                ElementSize != sizeof(void*) ||
                (OwnerSize > 0 &&
                    Offset + sizeof(void*) > (uint32)OwnerSize) ||
                !SDK::MemReadable(
                    (const uint8*)Owner + Offset,
                    sizeof(void*)))
            {
                return false;
            }

            OutField.Offset = Offset;
            OutField.bWeak =
                (FieldFlags & WeakObjectPropertyFlag) != 0;
            return true;
        };

    auto ReadObjectReference =
        [](UObject* Owner, const FObjectReferenceField& Field,
            UObject*& OutObject)
        {
            OutObject = nullptr;
            if (!IsSaneObject(Owner) ||
                Field.Offset == uint32(-1) ||
                !SDK::MemReadable(
                    (const uint8*)Owner + Field.Offset,
                    sizeof(void*)))
            {
                return false;
            }

            if (Field.bWeak)
            {
                const auto Weak =
                    GetFromOffset<TWeakObjectPtr<UObject>>(
                        Owner, Field.Offset);
                OutObject = Weak.Get();
            }
            else
            {
                OutObject = GetFromOffset<UObject*>(
                    Owner, Field.Offset);
            }
            return !OutObject || IsSaneObject(OutObject);
        };

    auto WriteObjectReference =
        [](UObject* Owner, const FObjectReferenceField& Field,
            UObject* Value)
        {
            if (!IsSaneObject(Owner) ||
                Field.Offset == uint32(-1) ||
                !SDK::MemReadable(
                    (const uint8*)Owner + Field.Offset,
                    sizeof(void*)))
            {
                return false;
            }

            if (Field.bWeak)
            {
                GetFromOffset<TWeakObjectPtr<UObject>>(
                    Owner, Field.Offset) =
                    TWeakObjectPtr<UObject>(Value);
            }
            else
            {
                GetFromOffset<UObject*>(Owner, Field.Offset) = Value;
            }
            return true;
        };

    FObjectReferenceField PlayerTeamField{};
    const bool bCanReadPlayerTeam = ResolveObjectReferenceField(
        PlayerState, "PlayerTeam", PlayerTeamField);
    FObjectReferenceField PlayerTeamPrivateField{};
    FObjectReferenceField TeamPrivateInfoField{};
    const bool bCanReadPlayerTeamPrivate =
        ResolveObjectReferenceField(
            PlayerState, "PlayerTeamPrivate",
            PlayerTeamPrivateField);
    const bool bCanReadTeamPrivateInfo =
        ResolveObjectReferenceField(
            CandidateTeamInfo, "PrivateInfo",
            TeamPrivateInfoField);
    UObject* OriginalPlayerTeam = nullptr;
    const bool bReadOriginalPlayerTeam = bCanReadPlayerTeam &&
        ReadObjectReference(
            PlayerState, PlayerTeamField, OriginalPlayerTeam);
    UObject* OriginalPlayerTeamPrivate = nullptr;
    const bool bReadOriginalPlayerTeamPrivate =
        bCanReadPlayerTeamPrivate &&
        ReadObjectReference(
            PlayerState, PlayerTeamPrivateField,
            OriginalPlayerTeamPrivate);
    UObject* CandidatePrivateInfo = nullptr;
    const bool bReadCandidatePrivateInfo =
        bCanReadTeamPrivateInfo &&
        ReadObjectReference(
            CandidateTeamInfo, TeamPrivateInfoField,
            CandidatePrivateInfo) &&
        IsSaneObject(CandidatePrivateInfo);
    const uint8 OriginalTeamIndex = PlayerState->TeamIndex;
    const uint8 OriginalSquadId = PlayerState->HasSquadId()
        ? PlayerState->SquadId
        : 0;
    auto OriginalTeamMembers =
        GetLateSeasonTeamMembers(OriginalPlayerTeam);
    const bool bWasInOriginalTeam = OriginalTeamMembers &&
        OriginalTeamMembers->Contains((AActor*)BotController);
    auto OriginalPlayerState = PlayerState;
    auto OriginalPawn = BotController->Pawn;

    auto TeamGraphMatches = [&]()
        {
            if (!IsSaneObject(BotController) ||
                BotController->Pawn != OriginalPawn ||
                !IsSaneObject(OriginalPawn) ||
                BotController->PlayerState != OriginalPlayerState)
            {
                return false;
            }

            PlayerState = BotController->PlayerState
                ->Cast<AFortPlayerStateAthena>();
            auto Members =
                GetLateSeasonTeamMembers(CandidateTeamInfo);
            if (!IsSaneObject(PlayerState) ||
                !PlayerState->HasTeamIndex() ||
                PlayerState->TeamIndex != CandidateTeam ||
                !Members ||
                !Members->Contains((AActor*)BotController))
            {
                return false;
            }

            if (bCanReadPlayerTeam)
            {
                UObject* CurrentPlayerTeam = nullptr;
                if (!ReadObjectReference(
                        PlayerState, PlayerTeamField,
                        CurrentPlayerTeam) ||
                    CurrentPlayerTeam != CandidateTeamInfo)
                {
                    return false;
                }
            }
            if (bReadCandidatePrivateInfo &&
                bCanReadPlayerTeamPrivate)
            {
                UObject* CurrentPlayerTeamPrivate = nullptr;
                if (!ReadObjectReference(
                        PlayerState, PlayerTeamPrivateField,
                        CurrentPlayerTeamPrivate) ||
                    CurrentPlayerTeamPrivate !=
                        CandidatePrivateInfo)
                {
                    return false;
                }
            }
            return true;
        };

    auto TeamGraphChanged = [&]()
        {
            if (!IsSaneObject(BotController) ||
                BotController->PlayerState != OriginalPlayerState ||
                BotController->Pawn != OriginalPawn)
            {
                return true;
            }
            PlayerState = BotController->PlayerState
                ->Cast<AFortPlayerStateAthena>();
            if (!IsSaneObject(PlayerState) ||
                !PlayerState->HasTeamIndex() ||
                PlayerState->TeamIndex != OriginalTeamIndex)
            {
                return true;
            }
            auto Members =
                GetLateSeasonTeamMembers(CandidateTeamInfo);
            if (Members &&
                Members->Contains((AActor*)BotController))
            {
                return true;
            }
            auto CurrentOriginalMembers =
                GetLateSeasonTeamMembers(OriginalPlayerTeam);
            const bool bCurrentlyInOriginalTeam =
                CurrentOriginalMembers &&
                CurrentOriginalMembers->Contains(
                    (AActor*)BotController);
            if (bCurrentlyInOriginalTeam != bWasInOriginalTeam)
                return true;
            if (bReadOriginalPlayerTeam)
            {
                UObject* CurrentPlayerTeam = nullptr;
                if (!ReadObjectReference(
                        PlayerState, PlayerTeamField,
                        CurrentPlayerTeam) ||
                    CurrentPlayerTeam != OriginalPlayerTeam)
                {
                    return true;
                }
            }
            if (bReadOriginalPlayerTeamPrivate)
            {
                UObject* CurrentPlayerTeamPrivate = nullptr;
                if (!ReadObjectReference(
                        PlayerState, PlayerTeamPrivateField,
                        CurrentPlayerTeamPrivate) ||
                    CurrentPlayerTeamPrivate !=
                        OriginalPlayerTeamPrivate)
                {
                    return true;
                }
            }
            return false;
        };

    auto FinalizeTeamAssignment = [&](const char* Method)
        {
            if (!TeamGraphMatches())
                return false;

            // Native team changes are moves. Discard any stale membership the
            // implementation retained in the old team before publication.
            RemoveLateSeasonMemberFromOtherTeams(
                GameState, CandidateTeamInfo,
                (AActor*)BotController);
            if (!TeamGraphMatches())
                return false;

            if (PlayerState->HasSquadId())
            {
                PlayerState->SquadId = CandidateTeam >= FirstTeam
                    ? (uint8)(CandidateTeam - FirstTeam)
                    : 0;
                PlayerState->OnRep_SquadId();
                VersionFeatureAdapter::MarkReplicatedPropertyDirty(
                    PlayerState, L"SquadId");
            }
            VersionFeatureAdapter::MarkReplicatedPropertyDirty(
                PlayerState, L"TeamIndex");
            VersionFeatureAdapter::MarkReplicatedPropertyDirty(
                PlayerState, L"PlayerTeam");
            VersionFeatureAdapter::MarkReplicatedPropertyDirty(
                PlayerState, L"PlayerTeamPrivate");
            PlayerState->ForceNetUpdate();
            BotController->ForceNetUpdate();
            GameState->ForceNetUpdate();
            OutTeamIndex = CandidateTeam;
            SDK::DbgLog(
                "[SpawnBot] isolated team assigned bot=%p team=%u "
                "method=%s version=%.2f\n",
                (void*)BotController,
                (unsigned)CandidateTeam,
                Method,
                VersionInfo.FortniteVersion);
            return true;
        };

    constexpr uint64 CPF_Parm = 0x80;
    constexpr uint64 CPF_OutParm = 0x100;
    constexpr uint64 CPF_ReturnParm = 0x400;
    auto IsInputParam =
        [&](const UFunction::ParamNamed* Param,
            uint32 Offset, uint32 Size)
        {
            return Param && Param->Offset == Offset &&
                Param->ElementSize == Size &&
                (Param->PropertyFlags & CPF_Parm) &&
                !(Param->PropertyFlags &
                    (CPF_OutParm | CPF_ReturnParm));
        };

    constexpr uint32 ChangeTeamParamsSize = 0x38;
    auto Library = UFortKismetLibrary::GetDefaultObj();
    auto ChangeTeamFunction =
        IsSaneObject((UObject*)Library)
            ? Library->GetFunction("ChangeTeam")
            : nullptr;
    bool bChangeTeamSchemaValid = false;
    if (ChangeTeamFunction)
    {
        const auto Params = ChangeTeamFunction->GetParamsNamed();
        const UFunction::ParamNamed* PlayerParam = nullptr;
        const UFunction::ParamNamed* InstigatorParam = nullptr;
        const UFunction::ParamNamed* TeamParam = nullptr;
        const UFunction::ParamNamed* TagsParam = nullptr;
        for (const auto& Param : Params.NameOffsetMap)
        {
            if (Param.Name == "PlayerToSwitch")
                PlayerParam = &Param;
            else if (Param.Name == "Instigator")
                InstigatorParam = &Param;
            else if (Param.Name == "NewTeam")
                TeamParam = &Param;
            else if (Param.Name == "ChangeTeamTags")
                TagsParam = &Param;
        }
        bChangeTeamSchemaValid =
            Params.NameOffsetMap.size() == 4 &&
            IsInputParam(PlayerParam, 0x00, sizeof(AActor*)) &&
            IsInputParam(InstigatorParam, 0x08, sizeof(AActor*)) &&
            IsInputParam(TeamParam, 0x10, sizeof(uint8)) &&
            IsInputParam(
                TagsParam, 0x18,
                sizeof(FGameplayTagContainer)) &&
            Params.Size == ChangeTeamParamsSize &&
            ChangeTeamFunction->GetPropertiesSize() ==
                ChangeTeamParamsSize;
        if (!bChangeTeamSchemaValid)
        {
            SDK::DbgLog(
                "[SpawnBot] native ChangeTeam schema rejected "
                "function=%p size=0x%X fields=%d version=%.2f\n",
                (void*)ChangeTeamFunction,
                Params.Size,
                (int)Params.NameOffsetMap.size(),
                VersionInfo.FortniteVersion);
        }
    }

    auto InvokeChangeTeam = [&](AActor* PlayerToSwitch,
        const char* TargetKind)
        {
            if (!bChangeTeamSchemaValid ||
                !IsSaneObject(PlayerToSwitch))
            {
                return false;
            }

            std::array<uint8, ChangeTeamParamsSize> ParamMemory{};
            AActor* ChangeInstigator = IsSaneObject(Instigator)
                ? Instigator
                : PlayerToSwitch;
            memcpy(ParamMemory.data() + 0x00,
                &PlayerToSwitch, sizeof(PlayerToSwitch));
            memcpy(ParamMemory.data() + 0x08,
                &ChangeInstigator, sizeof(ChangeInstigator));
            memcpy(ParamMemory.data() + 0x10,
                &CandidateTeam, sizeof(CandidateTeam));
            Library->ProcessEvent(
                ChangeTeamFunction, ParamMemory.data());
            const bool bApplied = TeamGraphMatches();
            SDK::DbgLog(
                "[SpawnBot] native ChangeTeam target=%s bot=%p "
                "requested=%u applied=%d mutated=%d version=%.2f\n",
                TargetKind,
                (void*)BotController,
                (unsigned)CandidateTeam,
                (int)bApplied,
                (int)TeamGraphChanged(),
                VersionInfo.FortniteVersion);
            return bApplied;
        };

    // TeamInfo membership is controller-based on every inspected layout. Try
    // the canonical controller actor again now that the synthetic identity is
    // ready and the candidate is an actually empty, active team. The previous
    // FN30 call happened before readiness and targeted the highest team (102).
    auto BotPawn = BotController->Pawn
        ? BotController->Pawn->Cast<AFortPlayerPawnAthena>()
        : nullptr;
    bool bGraphMutated = false;
    if (InvokeChangeTeam((AActor*)BotController, "controller"))
    {
        if (FinalizeTeamAssignment("ChangeTeam(controller)"))
            return true;
        bGraphMutated = true;
    }
    bGraphMutated = bGraphMutated || TeamGraphChanged();

    // ServerSetTeam is deliberately NOT used here, even though it is the
    // controller-native team path on the middle and modern layouts.
    //
    // It is a `WithValidation` client->server RPC, and a connectionless
    // synthetic controller is exactly the state its checks reject. UE records
    // that rejection in one process-global slot which FObjectReplicator reads
    // after the *received* RPC currently being dispatched returns - and cheat
    // commands are dispatched from the commanding client's own ServerCheat
    // bunch, so the engine closes that player's connection. Observed on FN
    // 7.40, 17.30 and 26.30: the spawning player is dropped and, if they were
    // alone, the match immediately ends.
    //
    // Clearing the slot afterwards only works where it can be located, and it
    // cannot be on every build (FN 7.40 has no discoverable reference to the
    // validate-name literals), so the call itself has to go. Nothing is lost:
    // it never once established the graph on any inspected build - the
    // reflected path below is what actually assigns the team - and asking a
    // controller with no client to request a team change was never meaningful.

    // ChangeTeam accepts a generic actor and several builds resolve that actor
    // through its possessed pawn. Preserve the pawn variant as the last native
    // attempt, again only after a proven no-op.
    if (!bGraphMutated &&
        InvokeChangeTeam((AActor*)BotPawn, "pawn"))
    {
        if (FinalizeTeamAssignment("ChangeTeam(pawn)"))
            return true;
        bGraphMutated = true;
    }
    bGraphMutated = bGraphMutated || TeamGraphChanged();

    auto ApplyReflectedTeamGraph = [&]()
        {
            if (!IsSaneObject(BotController) ||
                BotController->PlayerState != OriginalPlayerState ||
                BotController->Pawn != OriginalPawn ||
                !IsSaneObject(OriginalPlayerState) ||
                !IsSaneObject(OriginalPawn) ||
                !bCanReadPlayerTeam ||
                !bReadOriginalPlayerTeam ||
                !bCanReadPlayerTeamPrivate ||
                !bReadOriginalPlayerTeamPrivate ||
                !bReadCandidatePrivateInfo)
            {
                return false;
            }

            auto CandidateMembers =
                GetLateSeasonTeamMembers(CandidateTeamInfo);
            if (!CandidateMembers)
                return false;

            RemoveLateSeasonMemberFromOtherTeams(
                GameState, CandidateTeamInfo,
                (AActor*)BotController);
            PlayerState->TeamIndex = CandidateTeam;
            if (PlayerState->HasSquadId())
            {
                PlayerState->SquadId = CandidateTeam >= FirstTeam
                    ? (uint8)(CandidateTeam - FirstTeam)
                    : 0;
            }
            const bool bWroteTeam = WriteObjectReference(
                PlayerState, PlayerTeamField,
                CandidateTeamInfo);
            const bool bWrotePrivate = WriteObjectReference(
                PlayerState, PlayerTeamPrivateField,
                CandidatePrivateInfo);
            if (!CandidateMembers->Contains(
                    (AActor*)BotController))
            {
                CandidateMembers->Add((AActor*)BotController);
            }

            bool bApplied = bWroteTeam && bWrotePrivate &&
                TeamGraphMatches();
            if (!bApplied)
            {
                RemoveLateSeasonMemberFromOtherTeams(
                    GameState, nullptr,
                    (AActor*)BotController);
                PlayerState->GetTeamIndex() = OriginalTeamIndex;
                if (PlayerState->HasSquadId())
                    PlayerState->GetSquadId() = OriginalSquadId;
                WriteObjectReference(
                    PlayerState, PlayerTeamField,
                    OriginalPlayerTeam);
                WriteObjectReference(
                    PlayerState, PlayerTeamPrivateField,
                    OriginalPlayerTeamPrivate);
                if (bWasInOriginalTeam && OriginalTeamMembers &&
                    !OriginalTeamMembers->Contains(
                        (AActor*)BotController))
                {
                    OriginalTeamMembers->Add(
                        (AActor*)BotController);
                }
                return false;
            }

            auto CallNoParameterRepNotify =
                [&](const char* FunctionName)
                {
                    auto Function =
                        PlayerState->GetFunction(FunctionName);
                    if (!Function)
                        return;
                    const auto Params = Function->GetParamsNamed();
                    if (Params.NameOffsetMap.empty() &&
                        Params.Size == 0 &&
                        Function->GetPropertiesSize() == 0)
                    {
                        PlayerState->ProcessEvent(Function, nullptr);
                    }
                };
            CallNoParameterRepNotify("OnRep_PlayerTeam");
            CallNoParameterRepNotify("OnRep_PlayerTeamPrivate");

            auto OnRepTeamIndex =
                PlayerState->GetFunction("OnRep_TeamIndex");
            if (OnRepTeamIndex)
            {
                const auto Params =
                    OnRepTeamIndex->GetParamsNamed();
                if (Params.NameOffsetMap.empty() &&
                    Params.Size == 0 &&
                    OnRepTeamIndex->GetPropertiesSize() == 0)
                {
                    PlayerState->ProcessEvent(
                        OnRepTeamIndex, nullptr);
                }
                else if (Params.NameOffsetMap.size() == 1 &&
                    IsInputParam(
                        &Params.NameOffsetMap[0], 0,
                        sizeof(uint8)) &&
                    Params.Size == sizeof(uint8) &&
                    OnRepTeamIndex->GetPropertiesSize() ==
                        sizeof(uint8))
                {
                    uint8 PreviousTeam = OriginalTeamIndex;
                    PlayerState->ProcessEvent(
                        OnRepTeamIndex, &PreviousTeam);
                }
            }
            return TeamGraphMatches();
        };

    // Manual repair starts only from the exact pre-native snapshot. A partial
    // native mutation is intentionally failed closed; layering raw writes over
    // an in-progress engine transition can publish a mixed team generation.
    if (!bGraphMutated && ApplyReflectedTeamGraph() &&
        FinalizeTeamAssignment("reflected graph"))
    {
        return true;
    }

    RemoveLateSeasonMemberFromOtherTeams(
        GameState, nullptr, (AActor*)BotController);
    SDK::DbgLog(
        "[SpawnBot] isolated team assignment failed bot=%p "
        "requested=%u nativeMutated=%d version=%.2f\n",
        (void*)BotController,
        (unsigned)CandidateTeam,
        (int)bGraphMutated,
        VersionInfo.FortniteVersion);
    return false;
}


uint8_t AFortGameMode::PickTeam(AFortGameMode* GameMode, uint8_t PreferredTeam, AFortPlayerControllerAthena* Controller)
{
    if (!GameMode->HasWarmupRequiredPlayerCount())
        return 0;

    auto Playlist =
        VersionInfo.FortniteVersion >= 3.5 &&
            GameMode->HasWarmupRequiredPlayerCount()
        ? GetPublishedPlaylist(
            (AFortGameStateAthena*)GameMode->GameState)
        : nullptr;

    // FN17/18's native human join path can bypass this hook. Route any later
    // direct PickTeam call (notably `spawnbot`) through the same idempotent
    // allocator so every participant consumes the correct squad slot.
    if (ShouldRepairLateSeasonTeams() && Controller &&
        Controller->PlayerState)
    {
        auto PlayerState =
            (AFortPlayerStateAthena*)Controller->PlayerState;
        const uint8 AssignedTeam = ReserveLateSeasonHumanTeam(
            GameMode, Controller, PlayerState, Playlist);
        printf(
            "Picked repaired team %d %d\n",
            AssignedTeam,
            Playlist && Playlist->HasMaxSquadSize()
                ? Playlist->MaxSquadSize
                : 1);
        return AssignedTeam;
    }

    uint8_t ret = CurrentTeam;

    if (wcscmp(FConfiguration::Playlist, L"/DurianPlaylist/Playlist/Playlist_Durian.Playlist_Durian") == 0)
    {
        CurrentTeam++;
        return ret;
    }

    printf("Picked team %d %d\n", ret, Playlist ? Playlist->MaxSquadSize : 1);
    if (bIsLargeTeamGame)
    {
        if (CurrentTeam == 4)
            CurrentTeam = 3;
        else
            CurrentTeam = 4;
    }
    else
    {
        if (++PlayersOnCurTeam >= (Playlist ? Playlist->MaxSquadSize : 1))
        {
            CurrentTeam++;
            PlayersOnCurTeam = 0;
        }
    }

    return ret;
}

bool AFortGameMode::StartAircraftPhase(AFortGameMode* GameMode, char a2)
{
    auto World = UWorld::GetWorld();
    auto GameState = (AFortGameStateAthena*)GameMode->GameState;
    const uint8 PreviousGamePhase =
        GameState ? GameState->GamePhase : 0;
    const auto PolicyPlaylist =
        ResolveSupplyDropPlaylist(World);
    const bool bSkipAircraft =
        PolicyPlaylist &&
        PolicyPlaylist->HasbSkipAircraft() &&
        PolicyPlaylist->bSkipAircraft;
    const bool bBeforeAircraft =
        !GameState || !GameState->HasGamePhase() ||
        GameState->GamePhase <
            (uint8)EAthenaGamePhase::Aircraft;
    const bool bChapterOnePolicyManagedStart =
        VersionInfo.FortniteVersion < 11.00 &&
        bBeforeAircraft &&
        !bSkipAircraft &&
        IsAuthoritativeBusPolicySelection();

    // Chapter 1's native GameMode can call this directly without consulting
    // the replicated warmup timestamps. Permit that transition only after the
    // game-thread bus policy reaches the configured automatic/manual deadline.
    // Skip-aircraft and hidden event/custom-map phase flows retain native
    // ownership.
    if (bChapterOnePolicyManagedStart)
    {
        auto& State =
            GGameplayConfigurationPolicyState;
        const bool bAuthorized =
            State.World.Get() == World &&
            State.GameState.Get() == GameState &&
            State.bAircraftStartRequested;
        if (!bAuthorized)
        {
            if (!State.bLoggedBlockedAircraftStart)
            {
                State.bLoggedBlockedAircraftStart = true;
                SDK::DbgLog(
                    "[GameplayPolicy] blocked premature "
                    "Chapter 1 aircraft start\n");
            }
            return false;
        }
    }

    GLegacyCustomZoneAppliedWorld = World;
    GLegacyCustomZoneAppliedIndicator = nullptr;
    GLegacyCustomZoneAppliedPhase = -1;
    ResetLegacyFallbackSafeZonePlan();

    if (!FConfiguration::bCustomSafeZone)
        SafeZoneLoc = FVector{};

    if (FConfiguration::bCustomSafeZone && GameState && GameState->HasMapInfo() && GameState->MapInfo)
        GUI::ResolveCustomSafeZoneForMap(GameState->MapInfo);

    // This must precede the original: early Athena reads the array while
    // StartAircraftPhase is creating/configuring its native safe-zone actor.
    EnsureLegacyLateGameSafeZoneLocations(GameMode);
    // 13.00-20.x can enter the first native storm update from inside the
    // original aircraft transition. Populate its cached wait/shrink curves
    // before that update starts; repairing them only from the phase callback is
    // too late when an empty schedule makes every phase expire in one tick.
    HydrateLegacyNativeSafeZoneDurations(GameMode, GameState);
    TickSupplyDropSuppression();

    if (GameState &&
        PreviousGamePhase <
            (uint8)EAthenaGamePhase::Aircraft)
    {
        // Establish this match's one-shot cleanup epoch before native
        // StartAircraftPhase can invoke EnterAircraft for each passenger.
        AFortPlayerControllerAthena::
            BeginAircraftInventoryCleanupForMatch(
                GameState);

        for (auto& Player : GameMode->AlivePlayers)
        {
            AFortPlayerControllerAthena::
                ClearWarmupShieldForAircraft(
                    (AFortPlayerControllerAthena*)Player,
                    "legacy-phase-before");
        }
    }

    auto Ret = StartAircraftPhaseOG(GameMode, a2);

    // A legacy console dispatch can be dropped or native setup can reject a
    // transient attempt. Do not publish StartedMatch or consume the release
    // until the original reports success or the authoritative phase actually
    // advances; the policy tick will retry after its short backoff.
    const bool bAircraftPhaseObserved =
        GameState &&
        GameState->GamePhase >=
            (uint8)EAthenaGamePhase::Aircraft;
    if (bChapterOnePolicyManagedStart &&
        !Ret &&
        !bAircraftPhaseObserved)
    {
        GGameplayConfigurationPolicyState
            .bAircraftStartRequested = false;
        SDK::DbgLog(
            "[GameplayPolicy] Chapter 1 aircraft start "
            "was not accepted; retrying\n");
        return Ret;
    }

    // EnterAircraft is a private native call and its signature/finder varies
    // between seasons. Once the authoritative GameState confirms that the
    // aircraft phase actually began, clear every real passenger's warmup loot
    // from the controller-owned inventory. This remains valid even though the
    // native call may already have destroyed the spawn-island pawn.
    if (GameState &&
        GameState->GamePhase ==
            (uint8)EAthenaGamePhase::Aircraft)
    {
        for (auto& Player : GameMode->AlivePlayers)
        {
            auto PlayerController =
                (AFortPlayerControllerAthena*)Player;

            AFortPlayerControllerAthena::
                ClearWarmupShieldForAircraft(
                    PlayerController,
                    "legacy-phase-after");
            AFortPlayerControllerAthena::
                ClearDroppableInventoryForAircraft(
                    PlayerController,
                    "legacy-phase");
        }
    }

    TickSupplyDropSuppression(true);

    // Some 4.x/5.x native implementations regenerate this array inside the
    // original call and do not expose bSafeZoneLocationsInitialized. Repair the
    // source again before aircraft placement and later phase callbacks consume it.
    if (VersionInfo.FortniteVersion < 7.00 && FConfiguration::bCustomSafeZone)
        EnsureLegacyLateGameSafeZoneLocations(GameMode);
    
    const auto Playlist =
        VersionInfo.FortniteVersion >= 3.5 &&
        GameMode->HasWarmupRequiredPlayerCount()
            ? ResolveActivePlaylist(GameState)
            : nullptr;
    if constexpr (FConfiguration::WebhookURL && *FConfiguration::WebhookURL)
    {
        auto curl = curl_easy_init();

        curl_easy_setopt(curl, CURLOPT_URL, FConfiguration::WebhookURL);
        curl_slist* headers = curl_slist_append(NULL, "Content-Type: application/json");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        char version[6];

        sprintf_s(version, VersionInfo.FortniteVersion >= 5.00 || VersionInfo.FortniteVersion < 1.2 ? "%.2f" : "%.1f", VersionInfo.FortniteVersion);

        auto payload = UEAllocatedString("{\"embeds\": [{\"title\": \"Match has started!\", \"fields\": [{\"name\":\"Version\",\"value\":\"") + version + "\"}, {\"name\":\"Playlist\",\"value\":\"" + (Playlist ? Playlist->PlaylistName.ToString() : "Playlist_DefaultSolo") + "\"},{\"name\":\"Players\",\"value\":\"" + std::to_string(GameMode->AlivePlayers.Num()).c_str() + "\"}], \"color\": " + "\"7237230\", \"footer\": {\"text\":\"Magnesium\", \"icon_url\":\"https://cdn.discordapp.com/attachments/1341168629378584698/1436803905119064105/L0WnFa.png.png?ex=6910ef69&is=690f9de9&hm=01a0888b46647959b38ee58df322048ab49e2a5a678e52d4502d9c5e3978d805&\"}, \"timestamp\":\"" + iso8601() + "\"}] }";

        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());

        curl_easy_perform(curl);

        curl_easy_cleanup(curl);
    }
    GUI::gsStatus = StartedMatch;

    if (FConfiguration::bAutoStartEvent)
    {
        FConfiguration::EventStartBaseTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
        FConfiguration::bEventStarted = false;
        printf("[Events] Auto-start event being called. Will begin in %.1f seconds.\n",
            FConfiguration::EventStartTime.load());
    }

    // credit to heliato
    if (FConfiguration::bJoinInProgress || (Playlist && (Playlist->HasbAllowJoinInProgress() ? Playlist->bAllowJoinInProgress : false)))
        *(bool*)(uint64_t(&GameMode->WarmupRequiredPlayerCount) - 4) = false;

    if (FConfiguration::bLateGame && VersionInfo.FortniteVersion < 25.20)
    {
        auto Aircraft = GameState->HasAircrafts() ? (GameState->Aircrafts.Num() > 0 ? GameState->Aircrafts[0] : nullptr) : GameState->Aircraft;

        if (!Aircraft)
            return Ret;

        FVector Loc;
        const int AnchorIndex =
            CustomSafeZoneRuntime::ResolveRuntimeStartPhase() - 1;
        const int PreviewIndex = AnchorIndex + 1;
        const bool bHasAnchorLocation =
            GameMode->SafeZoneLocations.IsValidIndex(
                AnchorIndex);

        if (!bHasAnchorLocation)
        {
            // Never move the pre-S6 indicator after native aircraft setup. If
            // the early initialization could not find a center, leaving the
            // native state untouched is safer than creating two storm centers.
            if (VersionInfo.FortniteVersion < 6.00 &&
                !UsesLegacySafeZoneLocFallback())
            {
                SDK::DbgLog("[SafeZone] pre-S6 aircraft started without four native locations\n");
                return Ret;
            }

            if (!FindLegacyLateGameSafeZoneCenter(Loc))
                return Ret;
            SafeZoneLoc = Loc;
            if (!FConfiguration::bCustomSafeZone &&
                VersionInfo.FortniteVersion < 7.00)
            {
                BuildLegacyFallbackSafeZonePlan(
                    GameMode, Loc);
            }

            //FConfiguration::bLateGame = false;
            //printf("LateGame is not supported on this version!\n");
            //return Ret;
        }
        else
        {
            Loc = GameMode->SafeZoneLocations.Get(
                AnchorIndex, FVector::Size());

            // A few exact early builds can publish the selected current
            // circle without the following white-preview entry. Preserve
            // their real anchor and synthesize only the missing future chain.
            if (!FConfiguration::bCustomSafeZone &&
                VersionInfo.FortniteVersion < 7.00 &&
                !GameMode->SafeZoneLocations.IsValidIndex(
                    PreviewIndex))
            {
                SafeZoneLoc = Loc;
                BuildLegacyFallbackSafeZonePlan(
                    GameMode, Loc);
            }
        }

		if (FConfiguration::bCustomSafeZone)
		{
			const auto LegacyCustomZone =
				FConfiguration::GetLegacyCustomSafeZoneNodeSnapshot();
			Loc = FVector(LegacyCustomZone.Center);
			if (CustomSafeZoneRuntime::IsMovingModeRequested())
			{
				float SourceRadius = 0.f;
				CustomSafeZoneRuntime::TryGetSourceCircle(
					World,
					GameState && GameState->HasMapInfo()
						? GameState->MapInfo
						: nullptr,
					Loc, SourceRadius,
					GameMode->SafeZoneLocations.Num(),
					VersionInfo.FortniteVersion >= 21.10 ||
						HasNativeMovingSafeZonePhasePublisher());
			}
			// Lower builds consume the pre-seeded native array and the
            // phase-aware custom-radius path. Keeping this fallback clear
            // prevents it from fighting the native current/preview state.
            if (VersionInfo.FortniteVersion >= 7.00)
                SafeZoneLoc = Loc;
        }

        SDK::DbgLog(
            "[LateGame] aircraft zone anchor version=%.2f phase=%d loc=(%.1f, %.1f, %.1f) locations=%d fallback=%d custom=%d\n",
            VersionInfo.FortniteVersion,
            FConfiguration::LateGameZone.load(),
            Loc.X, Loc.Y, Loc.Z, GameMode->SafeZoneLocations.Num(),
            (int)!SafeZoneLoc.IsZero(),
            (int)FConfiguration::bCustomSafeZone.load());

        if (FConfiguration::bMovingBus)
        {
            bool IsSmallZone = FConfiguration::IsS27() ? GameMode->GetLateSafeZoneIndex() > 3 : GameMode->GetLateSafeZoneIndex() > 4;

            if (GameState->HasDefaultParachuteDeployTraceForGroundDistance())
            {
                GameState->DefaultParachuteDeployTraceForGroundDistance = 2500.f;
            }

            if (Aircraft->HasFlightInfo())
            {
                Aircraft->FlightInfo.FlightStartLocation = Loc;
                Aircraft->FlightInfo.FlightStartLocation.Z = 25000.f;
                Aircraft->FlightInfo.FlightSpeed /= IsSmallZone ? 10 : 5;
                Aircraft->FlightInfo.TimeTillFlightEnd = 10.f;
                Aircraft->FlightInfo.TimeTillDropStart = 0.f;
                Aircraft->FlightInfo.TimeTillDropEnd = 10.f /*-= ((Aircraft->FlightInfo.TimeTillDropEnd - Aircraft->FlightInfo.TimeTillDropStart) / 2)*/;

            }
            else
            {
                Aircraft->FlightSpeed /= IsSmallZone ? 10 : 5;

                Aircraft->FlightStartLocation = Loc;
                Aircraft->FlightStartLocation.Z = 25000.f;

                if (Aircraft->HasTimeTillFlightEnd())
                {
                    Aircraft->TimeTillFlightEnd = 10.f;
                    Aircraft->TimeTillDropEnd = 10.f /*-= ((Aircraft->FlightInfo.TimeTillDropEnd - Aircraft->FlightInfo.TimeTillDropStart) / 2)*/;
                    Aircraft->TimeTillDropStart = 0.f;
                }
            }
        }
        else
        {
            Loc.Z = 17500.f;

            if (GameState->HasDefaultParachuteDeployTraceForGroundDistance())
            {
                GameState->DefaultParachuteDeployTraceForGroundDistance = 2500.f;
            }

            if (Aircraft->HasFlightInfo())
            {
                Aircraft->FlightInfo.FlightSpeed = 0.f;

                Aircraft->FlightInfo.FlightStartLocation = Loc;

                Aircraft->FlightInfo.TimeTillFlightEnd = 7.f;
                Aircraft->FlightInfo.TimeTillDropEnd = 7.f;
                Aircraft->FlightInfo.TimeTillDropStart = 0.f;
            }
            else
            {
                Aircraft->FlightSpeed = 0.f;

                Aircraft->FlightStartLocation = Loc;
            }

            if (Aircraft->HasTimeTillFlightEnd())
            {
                Aircraft->TimeTillFlightEnd = 7.f;
                Aircraft->TimeTillDropEnd = 7.f;
                Aircraft->TimeTillDropStart = 0.f;
            }
        }

        Aircraft->DropStartTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
        Aircraft->DropEndTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld()) + 7.f;
        Aircraft->FlightStartTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
        Aircraft->FlightEndTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld()) + 7.f;
        //GameState->SafeZonesStartTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld()) + 7.6f;
    }

    return Ret;
}


void AFortGameMode::OnAircraftExitedDropZone_(UObject* Context, FFrame& Stack)
{
    AFortAthenaAircraft* Aircraft;
    Stack.StepCompiledIn(&Aircraft);
    Stack.IncrementCode();

    auto GameMode = (AFortGameMode*)Context;
    auto GameState = (AFortGameStateAthena*)GameMode->GameState;

    // A passenger that never sent the jump RPC is about to be ejected by the
    // native drop-zone path. Restrict this fallback to controllers still
    // aboard so players who jumped early and already looted are never wiped.
    for (auto& Player : GameMode->AlivePlayers)
    {
        auto PlayerController =
            (AFortPlayerControllerAthena*)Player;
        if (PlayerController &&
            PlayerController->IsInAircraft())
        {
            AFortPlayerControllerAthena::
                ClearDroppableInventoryForAircraft(
                    PlayerController, "drop-zone-exit", true);
        }
    }

    if (FConfiguration::bLateGame)
    {
        static auto CompClass = FindClass("FortControllerComponent_Aircraft");

        if (CompClass)
        {
            for (auto& Player : GameMode->AlivePlayers)
            {
                if (((AFortPlayerControllerAthena*)Player)->IsInAircraft())
                {
                    ((AFortPlayerControllerAthena*)Player)->GetAircraftComponent()->ServerAttemptAircraftJump(FRotator{});
                }
            }
        }
        else
        {
            for (auto& Player : GameMode->AlivePlayers)
            {
                if (((AFortPlayerControllerAthena*)Player)->IsInAircraft())
                {
                    ((AFortPlayerControllerAthena*)Player)->ServerAttemptAircraftJump(FRotator{});
                }
            }
        }
    }

    // Through Season 6, native OnAircraftExitedDropZone must observe the
    // Aircraft phase so it can run StartSafeZonesPhase and arm the recurring
    // SafeZoneInsideChecks timer. Newer Erbium paths retain their old ordering.
    const bool bDeferLegacyLateGamePhase =
        FConfiguration::bLateGame && VersionInfo.FortniteVersion < 7.00;
    const auto PreviousGamePhase = GameState->GamePhase;

    if (FConfiguration::bLateGame && !bDeferLegacyLateGamePhase)
    {
        GameState->GamePhase = 4;
        GameState->GamePhaseStep = 7;
        GameState->OnRep_GamePhase(3);
    }

    // This native call starts the safe-zone scheduler on affected builds.
    // Refresh immediately beforehand as well as during aircraft setup so a
    // playlist/map initialization pass cannot leave a zero-duration cache.
    HydrateLegacyNativeSafeZoneDurations(GameMode, GameState);
    callOG(GameMode, Stack.GetCurrentNativeFunction(), OnAircraftExitedDropZone, Aircraft);

    // Normally the old native transition reaches SafeZones itself.  Retain the
    // late-game fallback only for a build that did not transition, and do it
    // after native setup so its timer/effect lifecycle cannot be skipped.
    if (bDeferLegacyLateGamePhase && GameState->GamePhase != 4)
    {
        GameState->GamePhase = 4;
        GameState->OnRep_GamePhase(PreviousGamePhase);
    }

    if (bDeferLegacyLateGamePhase)
    {
        // Native lower-season playlists otherwise retain their ordinary
        // roughly 60-second first-zone delay.  Override only after the native
        // aircraft-exit path has initialized its indicator/timers.
        GameState->GamePhaseStep = 7;
        float SafeZonesStartTime = -1.f;
        if (GameState->HasSafeZonesStartTime())
        {
            SafeZonesStartTime = (float)UGameplayStatics::GetTimeSeconds(GameState);
            GameState->SafeZonesStartTime = SafeZonesStartTime;
        }

        SDK::DbgLog("[SafeZone] legacy lategame aircraft-exit phase %d -> %d step=%d start=%.2f indicator=%p active=%d effect=%p\n",
            (int)PreviousGamePhase, (int)GameState->GamePhase, (int)GameState->GamePhaseStep,
            SafeZonesStartTime,
            (void*)(GameMode->HasSafeZoneIndicator() ? GameMode->SafeZoneIndicator : nullptr),
            (int)(GameMode->HasbSafeZoneActive() ? GameMode->bSafeZoneActive : false),
            (void*)(GameMode->HasGE_OutsideSafeZone() ? GameMode->GE_OutsideSafeZone : nullptr));
    }
}

AFortSafeZoneIndicator* SetupSafeZoneIndicator(AFortGameMode* GameMode)
{
    // thanks heliato
    auto GameState = (AFortGameStateAthena*)GameMode->GameState;

    if (!GameMode->SafeZoneIndicator)
    {
        AFortSafeZoneIndicator* SafeZoneIndicator = UWorld::SpawnActor<AFortSafeZoneIndicator>(GameMode->SafeZoneIndicatorClass, FVector{});

        if (SafeZoneIndicator)
        {
            if (FConfiguration::bCustomSafeZone && GameState->HasMapInfo())
                GUI::ResolveCustomSafeZoneForMap(GameState->MapInfo);
            const auto LegacyCustomZone =
                FConfiguration::GetLegacyCustomSafeZoneNodeSnapshot();
            FFortSafeZoneDefinition& SafeZoneDefinition = GameState->MapInfo->SafeZoneDefinition;
            float SafeZoneCount = SafeZoneDefinition.Count.Evaluate();

            auto& Array = SafeZoneIndicator->HasSafeZonePhases() ? SafeZoneIndicator->SafeZonePhases : Phases;
			const bool bUsesStableProcessArray = &Array == &Phases;
			// Setup is the sole producer of the fallback array. Clear its tag
			// while rebuilding so preflight cannot consume partial or stale data.
			GManagedSafeZonePhasesWorld = TWeakObjectPtr<UWorld>{};
			GManagedSafeZonePhasesMapInfo =
				TWeakObjectPtr<AFortAthenaMapInfo>{};


            if (Array.IsValid())
                Array.Free();

            const float Time = (float)UGameplayStatics::GetTimeSeconds(GameState);

			const bool bMovingCustomZone =
				CustomSafeZoneRuntime::IsMovingModeRequested();
			for (float i = 0; i < SafeZoneCount; i++)
            {
                auto PhaseInfo = (FFortSafeZonePhaseInfo*)malloc(FFortSafeZonePhaseInfo::Size());
                memset((PBYTE)PhaseInfo, 0, FFortSafeZonePhaseInfo::Size());

                PhaseInfo->Radius = SafeZoneDefinition.Radius.Evaluate(i);
                PhaseInfo->WaitTime = SafeZoneDefinition.WaitTime.Evaluate(i);
                PhaseInfo->ShrinkTime = SafeZoneDefinition.ShrinkTime.Evaluate(i);
                PhaseInfo->PlayerCap = (int)SafeZoneDefinition.PlayerCapSolo.Evaluate(i);

                UDataTableFunctionLibrary::EvaluateCurveTableRow(GameState->AthenaGameDataTable, FName(L"Default.SafeZone.Damage"), i, nullptr, &PhaseInfo->DamageInfo.Damage, FString());
                if (i == 0.f)
                    PhaseInfo->DamageInfo.Damage = 0.01f;
                PhaseInfo->DamageInfo.bPercentageBasedDamage = true;
                PhaseInfo->TimeBetweenStormCapDamage = GameMode->TimeBetweenStormCapDamage.Evaluate(i);
                PhaseInfo->StormCapDamagePerTick = GameMode->StormCapDamagePerTick.Evaluate(i);
                PhaseInfo->StormCampingIncrementTimeAfterDelay = GameMode->StormCampingIncrementTimeAfterDelay.Evaluate(i);
                PhaseInfo->StormCampingInitialDelayTime = GameMode->StormCampingInitialDelayTime.Evaluate(i);
                PhaseInfo->MegaStormGridCellThickness = (int)SafeZoneDefinition.MegaStormGridCellThickness.Evaluate(i);

                if (FFortSafeZonePhaseInfo::HasUsePOIStormCenter())
                    PhaseInfo->UsePOIStormCenter = false;

                if (GameMode->SafeZoneLocations.GetData() && GameMode->SafeZoneLocations.Num() > i)
                    PhaseInfo->Center = GameMode->SafeZoneLocations.Get((int)i, FVector::Size());

				if (FConfiguration::bLateGame &&
					FConfiguration::bCustomSafeZone &&
					!bMovingCustomZone)
                {
                    PhaseInfo->Center = FVector(LegacyCustomZone.Center);
                    PhaseInfo->Radius = float(LegacyCustomZone.RadiusCm);
                    if (i == 0.f)
                        SDK::DbgLog("[SafeZoneMap] applying custom zone center=(%.1f, %.1f, %.1f) radius=%.1f\n",
                            PhaseInfo->Center.X, PhaseInfo->Center.Y, PhaseInfo->Center.Z,
                            PhaseInfo->Radius);
                }

                Array.Add(*PhaseInfo, FFortSafeZonePhaseInfo::Size());
                free(PhaseInfo);

				SafeZoneIndicator->PhaseCount++;
			}
			if (bUsesStableProcessArray)
			{
				GManagedSafeZonePhasesWorld =
					TWeakObjectPtr<UWorld>(UWorld::GetWorld());
				GManagedSafeZonePhasesMapInfo =
					TWeakObjectPtr<AFortAthenaMapInfo>(GameState->MapInfo);
			}

			bool bMovingCustomZoneApplied = false;
			if (bMovingCustomZone)
			{
				bMovingCustomZoneApplied =
					CustomSafeZoneRuntime::ApplyToPhaseArray(
					UWorld::GetWorld(), GameState->MapInfo,
					SafeZoneIndicator, Array,
					"gamemode-managed-setup",
					bUsesStableProcessArray);
			}
			if (bMovingCustomZone && !bMovingCustomZoneApplied &&
				FConfiguration::bLateGame &&
				FConfiguration::bCustomSafeZone)
			{
				for (int32 PhaseIndex = 0;
					PhaseIndex < Array.Num(); ++PhaseIndex)
				{
					auto& PhaseInfo = Array.Get(
						PhaseIndex, FFortSafeZonePhaseInfo::Size());
					PhaseInfo.Center =
						FVector(LegacyCustomZone.Center);
					PhaseInfo.Radius =
						float(LegacyCustomZone.RadiusCm);
				}
			}

            SafeZoneIndicator->OnRep_PhaseCount();

            SafeZoneIndicator->SafeZoneStartShrinkTime = Time + Array[0].WaitTime;
            SafeZoneIndicator->SafeZoneFinishShrinkTime = SafeZoneIndicator->SafeZoneStartShrinkTime + Array[0].ShrinkTime;

            SafeZoneIndicator->CurrentPhase = 0;
            SafeZoneIndicator->OnRep_CurrentPhase();
        }

        GameMode->SafeZoneIndicator = SafeZoneIndicator;
        GameState->SafeZoneIndicator = SafeZoneIndicator;
        GameState->OnRep_SafeZoneIndicator();
    }

    return GameMode->SafeZoneIndicator;
}

void StartNewSafeZonePhase(AFortGameMode* GameMode, int NewSafeZonePhase, bool bInitial = false)
{
    auto GameState = (AFortGameStateAthena*)GameMode->GameState;
	float TimeSeconds = (float)UGameplayStatics::GetTimeSeconds(GameState);
	auto& Array = GameMode->SafeZoneIndicator->HasSafeZonePhases() ? GameMode->SafeZoneIndicator->SafeZonePhases : Phases;
	const bool bMovingCustomZoneRequested =
		CustomSafeZoneRuntime::IsMovingModeRequested();
	bool bMovingCustomZone = bMovingCustomZoneRequested;

    if (Array.IsValidIndex(NewSafeZonePhase))
    {
        if (Array.IsValidIndex(NewSafeZonePhase - 1))
        {
            auto& PreviousPhaseInfo = Array.Get(NewSafeZonePhase - 1, FFortSafeZonePhaseInfo::Size());

            GameMode->SafeZoneIndicator->PreviousCenter = PreviousPhaseInfo.Center;
            GameMode->SafeZoneIndicator->PreviousRadius = PreviousPhaseInfo.Radius;
        }

        auto& PhaseInfo = Array.Get(NewSafeZonePhase, FFortSafeZonePhaseInfo::Size());
		if (FConfiguration::bLateGame &&
			FConfiguration::bCustomSafeZone &&
			!bMovingCustomZone)
        {
            if (GameState->HasMapInfo() && GameState->MapInfo)
                GUI::ResolveCustomSafeZoneForMap(GameState->MapInfo);
            const auto LegacyCustomZone =
                FConfiguration::GetLegacyCustomSafeZoneNodeSnapshot();
            PhaseInfo.Center = FVector(LegacyCustomZone.Center);
            PhaseInfo.Radius = float(LegacyCustomZone.RadiusCm);
        }

        GameMode->SafeZoneIndicator->NextCenter = PhaseInfo.Center;
        GameMode->SafeZoneIndicator->NextRadius = PhaseInfo.Radius;
        GameMode->SafeZoneIndicator->NextMegaStormGridCellThickness = PhaseInfo.MegaStormGridCellThickness;

        if (Array.IsValidIndex(NewSafeZonePhase + 1))
        {
            auto& NextPhaseInfo = Array.Get(NewSafeZonePhase + 1, FFortSafeZonePhaseInfo::Size());

            if (GameMode->SafeZoneIndicator->HasFutureReplicator() &&
                GameMode->SafeZoneIndicator->FutureReplicator)
            {
                if (GameMode->SafeZoneIndicator->FutureReplicator
                    ->HasNextNextCenter())
                {
                    GameMode->SafeZoneIndicator->FutureReplicator
                        ->NextNextCenter = NextPhaseInfo.Center;
                }
                if (GameMode->SafeZoneIndicator->FutureReplicator
                    ->HasNextNextRadius())
                {
                    GameMode->SafeZoneIndicator->FutureReplicator
                        ->NextNextRadius = NextPhaseInfo.Radius;
                }
            }

            GameMode->SafeZoneIndicator->NextNextCenter = NextPhaseInfo.Center;
            GameMode->SafeZoneIndicator->NextNextRadius = NextPhaseInfo.Radius;
			GameMode->SafeZoneIndicator->NextNextMegaStormGridCellThickness = NextPhaseInfo.MegaStormGridCellThickness;
		}

		if (bMovingCustomZoneRequested)
		{
			bMovingCustomZone =
				CustomSafeZoneRuntime::PublishManagedPhase(
				UWorld::GetWorld(),
				GameState && GameState->HasMapInfo()
					? GameState->MapInfo
					: nullptr,
				GameMode->SafeZoneIndicator,
				NewSafeZonePhase,
				"gamemode-managed-phase-start",
				&Array,
				&Array == &Phases);
		}

        GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = TimeSeconds + PhaseInfo.WaitTime;
        GameMode->SafeZoneIndicator->SafeZoneFinishShrinkTime = GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime + PhaseInfo.ShrinkTime;
		if (bMovingCustomZone)
		{
			VersionFeatureAdapter::MarkReplicatedPropertyDirty(
				GameMode->SafeZoneIndicator,
				L"SafeZoneStartShrinkTime");
			VersionFeatureAdapter::MarkReplicatedPropertyDirty(
				GameMode->SafeZoneIndicator,
				L"SafeZoneFinishShrinkTime");
			// Publish after both deadlines have been assigned. ForceNetUpdate alone
			// is insufficient for unmarked push-model/Iris properties.
			GameMode->SafeZoneIndicator->ForceNetUpdate();
		}

        GameMode->SafeZoneIndicator->CurrentDamageInfo = PhaseInfo.DamageInfo;
        GameMode->SafeZoneIndicator->OnRep_CurrentDamageInfo();

        GameMode->SafeZoneIndicator->CurrentPhase = NewSafeZonePhase;
        GameMode->SafeZoneIndicator->OnRep_CurrentPhase();

        GameMode->SafeZoneIndicator->OnSafeZonePhaseChanged.Process();

        auto& SafeZoneState = *(uint8_t*)(__int64(&GameMode->SafeZoneIndicator->FutureReplicator) - 0x4);
        SafeZoneState = 2;

        GameMode->SafeZoneIndicator->OnSafeZoneStateChange(2, false);
        if (GameMode->SafeZoneIndicator->HasSafezoneStateChangedDelegate())
            GameMode->SafeZoneIndicator->SafezoneStateChangedDelegate.Process(GameMode->SafeZoneIndicator, 2);

		if (FConfiguration::bLateGame &&
			FConfiguration::bCustomSafeZone &&
			!bMovingCustomZone)
            ApplyCustomSafeZoneState(GameMode, "managed-phase-start");

        if (FConfiguration::bLateGame &&
            FConfiguration::bLateGameLongZone)
        {
            UFortGameStateComponent_BattleRoyaleGamePhaseLogic::
                SetSafeZonePaused(true);
        }

        if (!bInitial)
            for (auto& UncastedPlayer : GameMode->AlivePlayers)
            {
                auto PlayerController = (AFortPlayerControllerAthena*)UncastedPlayer;

                UFortQuestManager::TrySendStatEvent(PlayerController,
                    EFortQuestObjectiveStatEvent::GetStormPhase(), 1, false);
            }
    }
}

void (*SpawnInitialSafeZoneOG)(AFortGameMode* GameMode);
void SpawnInitialSafeZone(AFortGameMode* GameMode)
{
    //return;
    GameMode->bSafeZoneActive = true;
    auto SafeZoneIndicator = SetupSafeZoneIndicator(GameMode);

    SafeZoneIndicator->OnSafeZonePhaseChanged.Bind(GameMode, FName(L"HandlePostSafeZonePhaseChanged"));
    GameMode->OnSafeZoneIndicatorSpawned.Process(SafeZoneIndicator);

    StartNewSafeZonePhase(
        GameMode,
        FConfiguration::bLateGame
            ? CustomSafeZoneRuntime::ResolveRuntimeStartPhase()
            : 1,
        true);


    //return SpawnInitialSafeZoneOG(GameMode);
}

void (*UpdateSafeZonesPhaseOG)(AFortGameMode* GameMode);
void UpdateSafeZonesPhase(AFortGameMode* GameMode)
{
    auto& Array = GameMode->SafeZoneIndicator && GameMode->SafeZoneIndicator->HasSafeZonePhases() ? GameMode->SafeZoneIndicator->SafeZonePhases : Phases;
    if (GameMode->bSafeZoneActive && UGameplayStatics::GetTimeSeconds(GameMode) >= GameMode->SafeZoneIndicator->SafeZoneFinishShrinkTime && !UFortGameStateComponent_BattleRoyaleGamePhaseLogic::IsSafeZonePaused() && Array.IsValidIndex(GameMode->SafeZoneIndicator->CurrentPhase + 1))
        StartNewSafeZonePhase(GameMode, GameMode->SafeZoneIndicator->CurrentPhase + 1);

    return UpdateSafeZonesPhaseOG(GameMode);
}


void GetPhaseInfo(UObject* Context, FFrame& Stack, bool* Ret)
{
    auto& OutSafeZonePhase = Stack.StepCompiledInRef<FFortSafeZonePhaseInfo>();
    int32 InPhaseToGet;
    Stack.StepCompiledIn(&InPhaseToGet);
    Stack.IncrementCode();
    auto SafeZoneIndicator = (AFortSafeZoneIndicator*)Context;
    auto& Array = SafeZoneIndicator->HasSafeZonePhases() ? SafeZoneIndicator->SafeZonePhases : Phases;

    if (Array.IsValidIndex(InPhaseToGet))
    {
        OutSafeZonePhase = Array[InPhaseToGet];

        *Ret = true;
        return;
    }
    *Ret = false;
}

class AFortNavMesh : public AActor
{
public:
    UCLASS_COMMON_MEMBERS(AFortNavMesh);

    DEFINE_PROP(HotSpotManager, const UObject*);
};
void (*OnWorldInitDoneOG)(UNavigationSystem* NavSys, char Mode);
void OnWorldInitDone(UNavigationSystem* NavSys, char Mode)
{
    printf("OnWorldInitDone\n");
    /*NavSys->bAutoCreateNavigationData = true;
    NavSys->bAllowClientSideNavigation = true;
    NavSys->bSupportRebuilding = true;

    OnWorldInitDoneOG(NavSys, Mode);

    auto AllBounds = Utils::GetAll(FindClass("NavMeshBoundsVolume"));
    auto AllNavmeshes = Utils::GetAll<AFortNavMesh>();
    auto HotSpotMgr = TUObjectArray::FindFirstObject("FortAIHotSpotManager");

    //auto Test = (void(*)(UNavigationSystem*)) (ImageBase + 0x1F5C290);
    //Test(NavSys);

    NavSys->OnNavigationBoundsUpdated(AllBounds[0]);
    AllNavmeshes[0]->HotSpotManager = HotSpotMgr;
    //printf("NavGraphData: %llx, AllBounds.Num() = %d\n", NavSys->NavGraphData, AllBounds.Num());
    AllBounds.Free();
    AllNavmeshes.Free();*/
}

void AFortGameMode::FinishWorldInitialization(AFortGameMode* _this, AActor* WorldManager)
{
    auto GameMode = (AFortGameModeAthena*)_this;
    auto GameState = (AFortGameStateAthena*)GameMode->GameState;

    //if (VersionInfo.EngineVersion == 4.25)
    //    SetupPlaylist(GameMode, GameState);

    SDK::DbgLog("[GameMode] FinishWorldInitialization entered (this=%p GameState=%p) pre-OG\n", (void*)_this, (void*)GameState);
    FinishWorldInitializationOG(_this, WorldManager);
    SDK::DbgLog("[GameMode] FinishWorldInitialization post-OG\n");

    if (GameState && VersionInfo.EngineVersion >= 4.22 &&
        VersionInfo.EngineVersion < 4.26)
    {
        const auto Playlist = GetPublishedPlaylist(GameState);
        if (FFortAthenaNativeLTMCompatibility::
                IsTargetPlaylist(Playlist))
        {
            // The exact 10.40 LTM path was already published once after
            // MapInfo became available. This idempotent call also covers the
            // unlikely case where FinishWorldInitialization got there first.
            PublishNative1040Playlist(GameState, Playlist);
            SDK::DbgLog(
                "[NativeLTM] skipped duplicate post-world playlist "
                "callback\n");
        }
        else
        {
            GameState->OnRep_CurrentPlaylistInfo();
        }
    }


    auto AddToTierData = [&](const UDataTable* Table, TArray<FFortLootTierData*>& TempArr)
        {
            if (!Table)
                return;

            Table->AddToRoot();
            if (VersionInfo.FortniteVersion >= 20)
            {
                if (auto CompositeTable = Table->Cast<UCompositeDataTable>())
                    for (auto& ParentTable : CompositeTable->ParentTables)
                        if (ParentTable)
                            for (auto& [Key, Val] : *(TMap<int32, FFortLootTierData*>*) (__int64(ParentTable) + 0x30))
                                TempArr.Add(Val);

                for (auto& [Key, Val] : *(TMap<int32, FFortLootTierData*>*) (__int64(Table) + 0x30))
                {
                    bool bFound = false;

                    for (auto& TierData : TempArr)
                        if (TierData->TierGroup == Val->TierGroup && TierData->LootPackage == Val->LootPackage)
                        {
                            TierData = Val;
                            bFound = true;
                            break;
                        }

                    if (!bFound)
                        TempArr.Add(Val);
                }
            }
            else
            {
                if (auto CompositeTable = Table->Cast<UCompositeDataTable>())
                    for (auto& ParentTable : CompositeTable->ParentTables)
                        if (ParentTable)
                            for (auto& [Key, Val] : (TMap<FName, FFortLootTierData*>) ParentTable->RowMap)
                                TempArr.Add(Val);

                for (auto& [Key, Val] : (TMap<FName, FFortLootTierData*>) Table->RowMap)
                {
                    bool bFound = false;

                    for (auto& TierData : TempArr)
                        if (TierData->TierGroup == Val->TierGroup && TierData->LootPackage == Val->LootPackage)
                        {
                            TierData = Val;
                            bFound = true;
                            break;
                        }

                    if (!bFound)
                        TempArr.Add(Val);
                }
            }
        };

    auto AddToPackages = [&](const UDataTable* Table, std::unordered_map<int32, FFortLootPackageData*>& TempArr)
        {
            if (!Table)
                return;

            Table->AddToRoot();
            if (VersionInfo.FortniteVersion >= 20)
            {
                if (auto CompositeTable = Table->Cast<UCompositeDataTable>())
                    for (auto& ParentTable : CompositeTable->ParentTables)
                        if (ParentTable)
                            for (auto& [Key, Val] : *(TMap<int32, FFortLootPackageData*>*) (__int64(ParentTable) + 0x30))
                                TempArr[Key] = Val;

                for (auto& [Key, Val] : *(TMap<int32, FFortLootPackageData*>*) (__int64(Table) + 0x30))
                    TempArr[Key] = Val;
            }
            else
            {
                if (auto CompositeTable = Table->Cast<UCompositeDataTable>())
                    for (auto& ParentTable : CompositeTable->ParentTables)
                        if (ParentTable)
                            for (auto& [Key, Val] : (TMap<FName, FFortLootPackageData*>) ParentTable->RowMap)
                                TempArr[Key.ComparisonIndex] = Val;

                for (auto& [Key, Val] : (TMap<FName, FFortLootPackageData*>) Table->RowMap)
                {
                    TempArr[Key.ComparisonIndex] = Val;
                }
            }
        };

    auto Playlist = FindObject<UFortPlaylistAthena>(FConfiguration::Playlist);

    if (!Playlist)
        Playlist = FindObject<UFortPlaylistAthena>(L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo");

    TArray<FFortLootTierData*> LootTierDataTempArr;
    auto LootTierData = Playlist ? Playlist->LootTierData.Get() : nullptr;
    if (!LootTierData)
        LootTierData = FindObject<UDataTable>(GameMode->HasWarmupRequiredPlayerCount() ? L"/Game/Items/Datatables/AthenaLootTierData_Client.AthenaLootTierData_Client" : L"/Game/Items/Datatables/LootTierData_Client.LootTierData_Client");
    if (LootTierData)
        AddToTierData(LootTierData, LootTierDataTempArr);

    for (auto& Val : LootTierDataTempArr)
        TierDataMap[Val->TierGroup.ComparisonIndex].Add(Val);

    std::unordered_map<int32, FFortLootPackageData*> LootPackageTempArr;
    auto LootPackages = Playlist ? Playlist->LootPackages.Get() : nullptr;
    if (!LootPackages)
        LootPackages = FindObject<UDataTable>(GameMode->HasWarmupRequiredPlayerCount() ? L"/Game/Items/Datatables/AthenaLootPackages_Client.AthenaLootPackages_Client" : L"/Game/Items/Datatables/LootPackages_Client.LootPackages_Client");
    if (LootPackages)
        AddToPackages(LootPackages, LootPackageTempArr);

    for (auto& [_, Val] : LootPackageTempArr)
        LootPackageMap[Val->LootPackageID.ComparisonIndex].Add(Val);

    auto GameFeatureDataClass = FindClass("FortGameFeatureData");
    if (GameFeatureDataClass)
        for (int i = 0; i < TUObjectArray::Num(); i++)
        {
            auto Object = TUObjectArray::GetObjectByIndex(i);

            if (!Object || !Object->Class || Object->IsDefaultObject())
                continue;

            if (Object->IsA(GameFeatureDataClass))
            {
                static auto DefaultLootTableDataOffset = Object->GetOffset("DefaultLootTableData");
                static auto PlaylistOverrideLootTableDataOffset = Object->GetOffset("PlaylistOverrideLootTableData");

                auto& LootTableData = GetFromOffset<FFortGameFeatureLootTableData>(Object, DefaultLootTableDataOffset);
                auto& LootTableDataUE53 = GetFromOffset<FFortGameFeatureLootTableData_UE53>(Object, DefaultLootTableDataOffset);
                auto& PlaylistOverrideLootTableData = GetFromOffset<TMap<FGameplayTag, FFortGameFeatureLootTableData>>(Object, PlaylistOverrideLootTableDataOffset);
                auto& PlaylistOverrideLootTableDataLWC = GetFromOffset<TMap<int32, FFortGameFeatureLootTableData>>(Object, PlaylistOverrideLootTableDataOffset);
                auto& PlaylistOverrideLootTableDataUE53 = GetFromOffset<TMap<int32, FFortGameFeatureLootTableData_UE53>>(Object, PlaylistOverrideLootTableDataOffset);
                auto LTDFeatureData = VersionInfo.EngineVersion >= 5.3 ? LootTableDataUE53.LootTierData.Get() : LootTableData.LootTierData.Get();
                auto LootPackageData = VersionInfo.EngineVersion >= 5.3 ? LootTableDataUE53.LootPackageData.Get() : LootTableData.LootPackageData.Get();

                if (LTDFeatureData)
                {
                    TArray<FFortLootTierData*> LTDTempData;

                    AddToTierData(LTDFeatureData, LTDTempData);

                    if (Playlist)
                    {
                        if (VersionInfo.EngineVersion >= 5.3)
                        {
                            /*for (auto& Tag : Playlist->GameplayTagContainer.GameplayTags)
                                for (auto& Override : PlaylistOverrideLootTableDataUE52)
                                    if (Tag.TagName.ComparisonIndex == Override.First)
                                        AddToTierData(Override.Second.LootTierData.Get(), LTDTempData);*/
                        }
                        else if (VersionInfo.FortniteVersion < 20.00)
                        {
                            for (auto& Tag : Playlist->GameplayTagContainer.GameplayTags)
                                for (auto& Override : PlaylistOverrideLootTableData)
                                    if (Tag.TagName == Override.First.TagName)
                                        AddToTierData(Override.Second.LootTierData.Get(), LTDTempData);
                        }
                        else
                            for (auto& Tag : Playlist->GameplayTagContainer.GameplayTags)
                                for (auto& Override : PlaylistOverrideLootTableDataLWC)
                                    if (Tag.TagName.ComparisonIndex == Override.First)
                                        AddToTierData(Override.Second.LootTierData.Get(), LTDTempData);
                    }

                    //for (auto& [_, Val] : LTDTempData)
                    //    TierDataAllGroups.Add(Val);

                    for (auto& Val : LTDTempData)
                        TierDataMap[Val->TierGroup.ComparisonIndex].Add(Val);
                }

                if (LootPackageData)
                {
                    std::unordered_map<int32, FFortLootPackageData*> LPTempData;

                    AddToPackages(LootPackageData, LPTempData);

                    if (Playlist)
                    {
                        if (VersionInfo.EngineVersion >= 5.3)
                        {
                            /*for (auto& Tag : Playlist->GameplayTagContainer.GameplayTags)
                                for (auto& Override : PlaylistOverrideLootTableDataUE52)
                                    if (Tag.TagName.ComparisonIndex == Override.First)
                                        AddToPackages(Override.Second.LootPackageData.Get(), LPTempData);*/
                        }
                        else if (VersionInfo.FortniteVersion < 20.00)
                        {
                            for (auto& Tag : Playlist->GameplayTagContainer.GameplayTags)
                                for (auto& Override : PlaylistOverrideLootTableData)
                                    if (Tag.TagName == Override.First.TagName)
                                        AddToPackages(Override.Second.LootPackageData.Get(), LPTempData);
                        }
                        else
                            for (auto& Tag : Playlist->GameplayTagContainer.GameplayTags)
                                for (auto& Override : PlaylistOverrideLootTableDataLWC)
                                    if (Tag.TagName.ComparisonIndex == Override.First)
                                        AddToPackages(Override.Second.LootPackageData.Get(), LPTempData);
                    }


                    for (auto& [_, Val] : LPTempData)
                        LootPackageMap[Val->LootPackageID.ComparisonIndex].Add(Val);
                }
            }
        }

    if (_this->HasOnPlaylistLootTablesAppliedDelegate())
    {
        *(bool*)(__int64(&_this->OnPlaylistLootTablesAppliedDelegate) + 0x10) = true;
        _this->OnPlaylistLootTablesAppliedDelegate.Process();
    }

    UFortLootPackage::SpawnFloorLootForContainer(FindObject<UClass>(L"/Game/Athena/Environments/Blueprints/Tiered_Athena_FloorLoot_Warmup.Tiered_Athena_FloorLoot_Warmup_C"));
    UFortLootPackage::SpawnFloorLootForContainer(FindObject<UClass>(L"/Game/Athena/Environments/Blueprints/Tiered_Athena_FloorLoot_01.Tiered_Athena_FloorLoot_01_C"));

    TArray<ABGAConsumableSpawner*> ConsumableSpawners{};
    Utils::GetAll<ABGAConsumableSpawner>(ConsumableSpawners);

    for (auto& Spawner : ConsumableSpawners)
        UFortLootPackage::SpawnConsumableActor(Spawner);

    ConsumableSpawners.Free();

    // FN30 streams its Living World vehicle providers asynchronously. On
    // some starts UpdateVehicleSpawns runs around eleven seconds after this
    // callback, so a one-shot scan permanently misses every source. Begin a
    // delayed, sparse, per-source-idempotent discovery window after streaming
    // settles instead of sweeping the whole world throughout server startup.
    BeginDeferredVehicleSpawnDiscovery();

    if (VersionInfo.FortniteVersion > 3.4)
    {
        TArray<ABuildingItemCollectorActor*> Collectors{};
        Utils::GetAll<ABuildingItemCollectorActor>(Collectors);
        for (auto& CollectorActor : Collectors)
        {
            if (Sum > Weight)
            {
            PickNum:
                auto RandomNum = (float)rand() / (RAND_MAX / TotalWeight);

                int Rarity = 0;
                bool found = false;

                for (auto& Element : WeightMap)
                {
                    float Weight = Element.second;

                    if (Weight == 0)
                        continue;

                    if (RandomNum <= Weight)
                    {
                        Rarity = Element.first;

                        found = true;
                        break;
                    }

                    RandomNum -= Weight;
                }

                if (!found)
                    goto PickNum;

                if (Rarity == 0)
                {
                    CollectorActor->K2_DestroyActor();
                    continue;
                }

                int AttemptsToGetItem = 0;
                for (int i = 0; i < CollectorActor->ItemCollections.Num(); i++)
                {
                    if (AttemptsToGetItem > 10)
                    {
                        AttemptsToGetItem = 0;
                        goto PickNum;
                    }

                    auto& Collection = CollectorActor->ItemCollections.Get(i, FCollectorUnitInfo::Size());

                    if (Collection.bUseDefinedOutputItem)
                        continue;

                    TArray<FFortItemEntry*> LootDrops{};

                    UFortLootPackage::ChooseLootForContainer(LootDrops, CollectorActor->DefaultItemLootTierGroupName, Rarity);

                    if (Collection.OutputItemEntry.Num() > 0)
                    {
                        Collection.OutputItemEntry.ResetNum();
                        Collection.OutputItem = nullptr;
                    }

                    for (auto& LootDrop : LootDrops)
                    {
                        if (!Collection.OutputItem && AFortInventory::IsPrimaryQuickbar(LootDrop->ItemDefinition))
                            Collection.OutputItem = LootDrop->ItemDefinition;

                        Collection.OutputItemEntry.Add(*LootDrop, FFortItemEntry::Size());
                        free(LootDrop);
                    }

                    if (!Collection.OutputItem)
                    {
                        i--;
                        AttemptsToGetItem++;

                        continue;
                    }

                    AttemptsToGetItem = 0;
                }

                CollectorActor->StartingGoalLevel = Rarity;
            }
            else
                CollectorActor->K2_DestroyActor();
        }
        Collectors.Free();

        Utils::ExecHook((UFunction*)FindObject<UFunction>(L"/Game/Athena/Items/Gameplay/VendingMachine/B_Athena_VendingMachine.B_Athena_VendingMachine_C:VendWobble__FinishedFunc"), VendWobble__FinishedFunc, VendWobble__FinishedFuncOG);
    }
    //Utils::ExecHook((UFunction*)FindObject<UFunction>(L"/Game/Athena/Items/Consumables/Parents/GA_Athena_MedConsumable_Parent.GA_Athena_MedConsumable_Parent_C:Triggered_4C02BFB04B18D9E79F84848FFE6D2C32"), AFortPlayerPawnAthena::Athena_MedConsumable_Triggered, AFortPlayerPawnAthena::Athena_MedConsumable_TriggeredOG);
}

void PlayerCanRestart(UObject* Context, FFrame& Stack, bool* Ret)
{
    AFortPlayerControllerAthena* Controller;

    Stack.StepCompiledIn(&Controller);
    Stack.IncrementCode();

    *Ret = true;
}

void AFortGameMode::Hook()
{
    auto _rtsmFn = GetDefaultObj()->GetFunction("ReadyToStartMatch");
    SDK::DbgLog("[GameMode] Hook: ReadyToStartMatch UFunction=%p (CDO=%p)\n", (void*)_rtsmFn, (void*)GetDefaultObj());
    Utils::ExecHook(_rtsmFn, ReadyToStartMatch_, ReadyToStartMatch_OG);
    SDK::DbgLog("[GameMode] Hook: FindFinishWorldInitialization=%p\n", (void*)FindFinishWorldInitialization());
    Utils::Hook(FindFinishWorldInitialization(), FinishWorldInitialization, FinishWorldInitializationOG);
    //if (VersionInfo.EngineVersion == 4.16)
    //    Utils::Hook(Memcury::Scanner::FindPattern("40 55 53 56 41 56 48 8D 6C 24 ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 45 ? 48 8B 01 48 8B F1").Get(), OnWorldInitDone, OnWorldInitDoneOG);
}

void AFortGameMode::PostLoadHook()
{
    ApplyCharacterCustomization = FindApplyCharacterCustomization();
    NotifyGameMemberAdded_ = FindNotifyGameMemberAdded();

    auto spdf = GetDefaultObj()->GetFunction("SpawnDefaultPawnFor");
    SpawnDefaultPawnForIdx = spdf->GetVTableIndex();

    Utils::ExecHook(spdf, SpawnDefaultPawnFor);
    Utils::ExecHook(GetDefaultObj()->GetFunction("HandleStartingNewPlayer"), HandleStartingNewPlayer_, HandleStartingNewPlayer_OG);
    Utils::Hook(FindPickTeam(), PickTeam, PickTeamOG);
    if (FFortAthenaHeistCompatibility::IsSupportedBuild())
    {
        // Seven local 5.41 dumps traced to the supplemental end-game
        // trampoline. Leave the native function untouched; Getaway gameplay
        // does not require the teammate notification bridge to run.
        SDK::DbgLog(
            "[Heist] native StartEndGamePhase left unhooked\n");
    }
    if (VersionInfo.FortniteVersion < 25.20)
    {
        Utils::Hook(FindStartAircraftPhase(), StartAircraftPhase, StartAircraftPhaseOG);
        auto HandlePostSafeZonePhaseChangedAddress = FindHandlePostSafeZonePhaseChanged();
        if (HandlePostSafeZonePhaseChangedAddress)
        {
            const auto ModuleBase = Memcury::PE::GetModuleBase();
            const auto ModuleEnd = ModuleBase +
                Memcury::PE::GetNTHeaders()->OptionalHeader.SizeOfImage;
            if (HandlePostSafeZonePhaseChangedAddress < ModuleBase ||
                HandlePostSafeZonePhaseChangedAddress >= ModuleEnd ||
                !SDK::MemReadable((void*)HandlePostSafeZonePhaseChangedAddress, 6))
            {
                SDK::DbgLog(
                    "[SafeZone] rejected invalid native phase handler address=%p\n",
                    (void*)HandlePostSafeZonePhaseChangedAddress);
                HandlePostSafeZonePhaseChangedAddress = 0;
            }
        }
        if (HandlePostSafeZonePhaseChangedAddress)
        {
            Utils::Hook(HandlePostSafeZonePhaseChangedAddress,
                HandlePostSafeZonePhaseChanged, HandlePostSafeZonePhaseChangedOG);
        }
        GHasNativeLateGameSafeZonePhaseHook = HandlePostSafeZonePhaseChangedOG != nullptr;
        if (!GHasNativeLateGameSafeZonePhaseHook &&
            (VersionInfo.FortniteVersion == 2.50 || VersionInfo.FortniteVersion == 7.30))
        {
            SDK::DbgLog(
                "[SafeZone] native phase hook unavailable on %.2f; using bounded late-game fallback\n",
                VersionInfo.FortniteVersion);
        }
    }
    Utils::ExecHook(AFortGameModeAthena::GetDefaultObj()->GetFunction("OnAircraftExitedDropZone"), OnAircraftExitedDropZone_, OnAircraftExitedDropZone_OG);

    if (VersionInfo.FortniteVersion >= 21.10)
    {
        if (VersionInfo.FortniteVersion < 25.20)
        {
            const auto SpawnInitialSafeZoneAddress =
                FindSpawnInitialSafeZone();
            const auto UpdateSafeZonesPhaseAddress =
                FindUpdateSafeZonesPhase();
            Utils::Hook(
                SpawnInitialSafeZoneAddress,
                SpawnInitialSafeZone,
                SpawnInitialSafeZoneOG);
            Utils::Hook(
                UpdateSafeZonesPhaseAddress,
                UpdateSafeZonesPhase,
                UpdateSafeZonesPhaseOG);
            GHasManagedMovingSafeZonePhaseHooks =
                SpawnInitialSafeZoneAddress != 0 &&
                UpdateSafeZonesPhaseAddress != 0 &&
                SpawnInitialSafeZoneOG != nullptr &&
                UpdateSafeZonesPhaseOG != nullptr;
            if (!GHasManagedMovingSafeZonePhaseHooks)
            {
                SDK::DbgLog(
                    "[CustomSafeZonePlan] managed phase owner unavailable "
                    "version=%.2f spawn=%p update=%p spawnOG=%p updateOG=%p\n",
                    VersionInfo.FortniteVersion,
                    (void*)SpawnInitialSafeZoneAddress,
                    (void*)UpdateSafeZonesPhaseAddress,
                    (void*)SpawnInitialSafeZoneOG,
                    (void*)UpdateSafeZonesPhaseOG);
            }
        }
        Utils::ExecHook(L"/Script/FortniteGame.FortSafeZoneIndicator.GetPhaseInfo", GetPhaseInfo);
    }

    const bool bHasAuthoritativeMovingZonePublisher =
        VersionInfo.FortniteVersion >= 25.20
            ? true
            : (VersionInfo.FortniteVersion >= 21.10
                ? GHasManagedMovingSafeZonePhaseHooks
                : HasNativeMovingSafeZonePhasePublisher());
    ECustomSafeZoneLegacyPhaseOwnerPath LegacyOwnerPath =
        ECustomSafeZoneLegacyPhaseOwnerPath::Unavailable;
    if (VersionInfo.FortniteVersion < 21.10)
    {
        if (GHasNativeLateGameSafeZonePhaseHook)
        {
            LegacyOwnerPath =
                ECustomSafeZoneLegacyPhaseOwnerPath::NativePhaseHook;
        }
        else if (VersionInfo.FortniteVersion == 2.50 ||
            VersionInfo.FortniteVersion == 7.30)
        {
            LegacyOwnerPath =
                ECustomSafeZoneLegacyPhaseOwnerPath::DeadlineFallback;
        }
    }
    CustomSafeZoneRuntime::SetLegacyPhaseOwnerPath(LegacyOwnerPath);
    CustomSafeZoneRuntime::SetAuthoritativePhasePublisherAvailable(
        bHasAuthoritativeMovingZonePublisher);

    //if (VersionInfo.FortniteVersion >= 15)
//    Utils::ExecHook(AFortGameModeAthena::GetDefaultObj()->GetFunction("PlayerCanRestart"), PlayerCanRestart);
}
