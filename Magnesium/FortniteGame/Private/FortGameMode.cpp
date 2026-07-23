#include "pch.h"
#include "../Public/FortGameMode.h"
#include "../Public/LevelStreamingDynamic.h"
#include "../../Erbium/Public/Finders.h"
#include "../../Engine/Public/NetDriver.h"
#include "../../Engine/Public/AbilitySystemComponent.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortKismetLibrary.h"
#include "../../Engine/Public/CurveTable.h"
#include "../Public/FortSafeZoneIndicator.h"
#include "../../Engine/Public/DataTableFunctionLibrary.h"
#include "../../Erbium/Public/Configuration.h"
#include "../../Erbium/PlayerAI/Public/MagnesiumPlayerAIIntegration.h"
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
#include "../Public/FortPhysicsPawn.h"

#include <sstream>
#include <fstream>
#include <cmath>

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

    bool ShouldRepairLateSeasonTeams()
    {
        return VersionInfo.FortniteVersion >= 17.0 && VersionInfo.FortniteVersion < 19.0;
    }

    uint8 GetPlaylistFirstTeam(const UFortPlaylistAthena* Playlist)
    {
        if (Playlist)
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

    bool IsSaneObject(UObject* Object)
    {
        if (!Object || !SDK::MemReadable(Object, sizeof(UObject)))
            return false;

        const int32 ObjectIndex = Object->Index;
        if (ObjectIndex < 0 || ObjectIndex >= TUObjectArray::Num())
            return false;

        auto Item = TUObjectArray::GetItemByIndex(ObjectIndex);
        return Item && Item->Object == Object && !(Item->Flags & 0x20) &&
            Object->Class && SDK::MemReadable(Object->Class, sizeof(UClass));
    }

    int32 GetLateSeasonIntProperty(
        const UObject* Object,
        const char* Name,
        int32 Fallback)
    {
        if (!Object)
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

    void SyncLateSeasonTeamSettings(
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
                GameMode->GameSession, "MaxPartySize", MaxTeamSize);

        SDK::DbgLog(
            "[Teams] FN17-18 playlist team settings: TeamSize=%d(%d) TeamCount=%d(%d) MaxPartySize=%d\n",
            MaxTeamSize,
            bSetTeamSize ? 1 : 0,
            MaxTeamCount,
            bSetTeamCount ? 1 : 0,
            bSetPartySize ? 1 : 0);
    }

    bool IsLateSeasonTeamPlaylist(const UFortPlaylistAthena* Playlist)
    {
        return Playlist && Playlist->HasMaxSquadSize() &&
            Playlist->MaxSquadSize > 1;
    }

    bool DoesLateSeasonPlaylistAllowDBNO(
        const UFortPlaylistAthena* Playlist)
    {
        if (!Playlist)
            return false;

        const int32 DBNOTypeOffset = (int32)Playlist->GetOffset("DBNOType");
        if (DBNOTypeOffset < 0 ||
            !SDK::MemReadable(
                (const uint8*)Playlist + DBNOTypeOffset, sizeof(uint8)))
        {
            // Older assets without DBNOType use the original Erbium rule:
            // team playlist means DBNO is permitted.
            return true;
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
                : FConfiguration::bForceRespawns;
            return !bRespawning;
        }
        default:
            return true;
        }
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
        const bool bTeamMode = IsLateSeasonTeamPlaylist(Playlist);
        bool bDBNOEnabled =
            FConfiguration::bEnableDBNO && bTeamMode &&
            DoesLateSeasonPlaylistAllowDBNO(Playlist);

        if (GameMode->HasbDBNOEnabled())
            GameMode->bDBNOEnabled = bDBNOEnabled;
        if (GameMode->HasbAlwaysDBNO())
            GameMode->bAlwaysDBNO = false;
        if (GameState->HasbDBNOEnabledForGameMode())
            GameState->bDBNOEnabledForGameMode = bDBNOEnabled;

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
}

void SetupPlaylist(AFortGameMode* GameMode, AFortGameStateAthena* GameState)
{
    auto Playlist = FindObject<UFortPlaylistAthena>(FConfiguration::Playlist);

    if (!Playlist)
        Playlist = FindObject<UFortPlaylistAthena>(L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo");

    if (Playlist)
    {
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
        if (FConfiguration::bForceRespawns || FConfiguration::bJoinInProgress)
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
        if (GameState->HasCurrentPlaylistInfo())
        {
            //if (VersionInfo.EngineVersion >= 4.27)
            GameState->CurrentPlaylistInfo.BasePlaylist = Playlist;
            if (ShouldRepairLateSeasonTeams() &&
                FPlaylistPropertyArray::HasOverridePlaylist())
            {
                GameState->CurrentPlaylistInfo.OverridePlaylist = Playlist;
            }
            GameState->CurrentPlaylistInfo.PlaylistReplicationKey++;
            GameState->CurrentPlaylistInfo.MarkArrayDirty();
            GameState->OnRep_CurrentPlaylistInfo();
        }
        else if (GameState->HasCurrentPlaylistData())
        {
            GameState->CurrentPlaylistData = Playlist;
            GameState->OnRep_CurrentPlaylistData();
        }

        GameMode->CurrentPlaylistId = Playlist->PlaylistId;
        if (GameState->HasCurrentPlaylistId())
            GameState->CurrentPlaylistId = Playlist->PlaylistId;
        if (GameMode->HasCurrentPlaylistName())
            GameMode->CurrentPlaylistName = Playlist->PlaylistName;

        if (GameMode->GameSession->HasMaxPlayers())
            GameMode->GameSession->MaxPlayers = Playlist->MaxPlayers;


        if (GameState->HasAirCraftBehavior() && Playlist->HasAirCraftBehavior())
            GameState->AirCraftBehavior = Playlist->AirCraftBehavior;
        if (GameState->HasCachedSafeZoneStartUp() && Playlist->HasSafeZoneStartUp())
            GameState->CachedSafeZoneStartUp = Playlist->SafeZoneStartUp;

        // Configure each reflected DBNO property independently because their
        // availability and names vary by game version. FN17/18 is finalized
        // below after its playlist team graph has been initialized.
        bool bDBNOOn = ShouldRepairLateSeasonTeams()
            ? (FConfiguration::bEnableDBNO &&
                IsLateSeasonTeamPlaylist(Playlist) &&
                DoesLateSeasonPlaylistAllowDBNO(Playlist))
            : true;
        bool bAlwaysDBNO = false;
        if (!ShouldRepairLateSeasonTeams() &&
            GameMode->HasbEnableDBNO())
            GameMode->bEnableDBNO = bDBNOOn;
        if (GameMode->HasbDBNOEnabled())
            GameMode->bDBNOEnabled = bDBNOOn;
        if (GameState->HasbDBNOEnabledForGameMode())
            GameState->bDBNOEnabledForGameMode = bDBNOOn;
        if (!ShouldRepairLateSeasonTeams() &&
            GameState->HasbDBNODeathEnabled())
            GameState->bDBNODeathEnabled = bDBNOOn;
        // Let native rules decide whether a solo player can be downed.
        if (GameMode->HasbAlwaysDBNO())
            GameMode->bAlwaysDBNO = bAlwaysDBNO;

        bIsLargeTeamGame = Playlist->bIsLargeTeamGame;

        if (ShouldRepairLateSeasonTeams())
        {
            SyncLateSeasonTeamSettings(GameMode, GameState, Playlist);
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

    // Keep any locations the build did initialize and extend from the last
    // valid center; using one
    // center for the missing tail preserves contained, concentric late-game
    // circles instead of making later phases jump across the map.
    const int RequiredLocationCount = GetLegacySafeZoneLocationCount(GameMode);
    const int ExistingLocationCount = GameMode->SafeZoneLocations.Num();
    FVector Center;
    bool bHasCenter = false;

    if (bUseCustomCenter)
    {
        Center = FConfiguration::CustomSafeZoneCenter;
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

    for (int Index = 0; Index < ExistingLocationCount; Index++)
    {
        auto& ExistingCenter = GameMode->SafeZoneLocations.Get(Index, FVector::Size());
        if (bUseCustomCenter || !std::isfinite(ExistingCenter.X) ||
            !std::isfinite(ExistingCenter.Y) || ExistingCenter.IsZero())
            ExistingCenter = Center;
    }

    for (int Index = ExistingLocationCount; Index < RequiredLocationCount; Index++)
        GameMode->SafeZoneLocations.Add(Center, FVector::Size());

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
            Center.X, Center.Y, Center.Z, FConfiguration::CustomSafeZoneRadius);
    else if (ExistingLocationCount < RequiredLocationCount)
        SDK::DbgLog("[SafeZone] initialized pre-S6 native locations %d -> %d at (%.1f, %.1f, %.1f)\n",
            ExistingLocationCount, GameMode->SafeZoneLocations.Num(), Center.X, Center.Y, Center.Z);
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

    static bool setup = false;
    if (GameMode->HasWarmupRequiredPlayerCount() ? GameMode->WarmupRequiredPlayerCount != 1 : !setup)
    {
        setup = true;

        //if (!FindListenCall())
        {
            SDK::DbgLog("[GameMode] ReadyToStart Listen: ENTER setup=%d\n", (int)setup);
            auto World = UWorld::GetWorld();
            auto Engine = UEngine::GetEngine();
            auto NetDriverName = FName(L"GameNetDriver");

            if (GameMode->HasbEnableReplicationGraph())
                GameMode->bEnableReplicationGraph = true;

            UNetDriver* NetDriver = nullptr;
            if (VersionInfo.FortniteVersion >= 16.00)
            {
                SDK::DbgLog("[GameMode] Listen: GetWorldContext=%p CreateNetDriver=%p\n",
                    (void*)FindGetWorldContext(), (void*)FindCreateNetDriverWorldContext());
                void* WorldCtx = ((void* (*)(UEngine*, UWorld*)) FindGetWorldContext())(Engine, World);
                World->NetDriver = NetDriver = ((UNetDriver * (*)(UEngine*, void*, FName, int)) FindCreateNetDriverWorldContext())(Engine, WorldCtx, NetDriverName, 0);
                SDK::DbgLog("[GameMode] Listen: WorldCtx=%p NetDriver=%p\n", WorldCtx, (void*)NetDriver);
            }
            else
                World->NetDriver = NetDriver = ((UNetDriver * (*)(UEngine*, UWorld*, FName)) FindCreateNetDriver())(Engine, World, NetDriverName);
            if (!NetDriver)
            {
                SDK::DbgLog("[GameMode] Listen: NetDriver NULL, abort\n");
                *Ret = false;
                return;
            }
            if (VersionInfo.FortniteVersion >= 20 && NetDriver)
                NetDriver->NetServerMaxTickRate = 30;

            NetDriver->NetDriverName = NetDriverName;
            NetDriver->World = World;

            if (VersionInfo.EngineVersion >= 5.3 && FConfiguration::bEnableIris)
            {
                *(bool*)(__int64(&NetDriver->ReplicationDriver) + 0x11) = true;
            }

            NetDriver->NetDriverName = NetDriverName;
            NetDriver->World = World;

            for (int i = 0; i < World->LevelCollections.Num(); i++)
            {
                auto& LevelCollection = World->LevelCollections.Get(i, FLevelCollection::Size());

                LevelCollection.NetDriver = NetDriver;
            }

            auto URL = (FURL*)malloc(FURL::Size());
            memset((PBYTE)URL, 0, FURL::Size());
            URL->Port = FConfiguration::Port;

            auto InitListen = (bool (*)(UNetDriver*, UWorld*, FURL*, bool, FString&)) FindInitListen();
            auto SetWorld = (void (*)(UNetDriver*, UWorld*)) FindSetWorld();
            SDK::DbgLog("[GameMode] Listen: InitListen=%p SetWorld=%p Port=%d\n",
                (void*)InitListen, (void*)SetWorld, (int)FConfiguration::Port);

            SetWorld(NetDriver, World);
            FString Err;
            bool ok = InitListen(NetDriver, World, URL, false, Err);
            SDK::DbgLog("[GameMode] Listen: InitListen returned %d\n", (int)ok);
            if (ok)
                SetWorld(NetDriver, World);
            else
                printf("Failed to listen!");

            free(URL);

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

        if (Playlist)
        {
            auto AdditionalPlaylistLevelsStreamed__Off = GameState->GetOffset("AdditionalPlaylistLevelsStreamed");
            auto AdditionalLevelStruct = FAdditionalLevelStreamed::StaticStruct();

            if (FConfiguration::IsKnownS27CustomMapPlaylist())
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
                TArray<FPlaylistStreamedLevelData>& AdditionalPlaylistLevels
                    = *(TArray<FPlaylistStreamedLevelData>*)(__int64(GameState) + AdditionalPlaylistLevelsStreamed__Off - 0x10);

                AdditionalPlaylistLevels.Free();

                if (Playlist->HasAdditionalLevels())
                {
                    for (auto& Level : Playlist->AdditionalLevels)
                    {
                        bool Success = false;
                        // ULevelStreamingDynamic::LoadLevelInstanceBySoftObjectPtr(UWorld::GetWorld(), Level, FVector(), FRotator(), &Success, FString(), nullptr);
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
                        // ULevelStreamingDynamic::LoadLevelInstanceBySoftObjectPtr(UWorld::GetWorld(), Level, FVector(), FRotator(), &Success, FString(), nullptr);

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
        }

        if (!FConfiguration::IsKnownS27CustomMapPlaylist())
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

    if (Misc::bHookedAll)
        GUI::gsStatus = Joinable;

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

        auto Playlist = VersionInfo.FortniteVersion >= 3.5 && GameMode->HasWarmupRequiredPlayerCount() ? (GameMode->GameState->HasCurrentPlaylistInfo() ? GameMode->GameState->CurrentPlaylistInfo.BasePlaylist : GameMode->GameState->CurrentPlaylistData) : nullptr;

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

                        UObject* LoaderObject = nullptr;
                        if (Event.LoaderClass)
                            if (const UClass* LoaderClass = FindObject<UClass>(Event.LoaderClass))
                            {
                                TArray<AActor*> AllLoaders;
                                Utils::GetAll(LoaderClass, AllLoaders);
                                LoaderObject = AllLoaders.Num() > 0 ? AllLoaders[0] : nullptr;
                                AllLoaders.Free();
                            }

                        if (Event.LoaderFuncPath != nullptr && LoaderObject)
                            if (const UFunction* LoaderFunction = FindObject<UFunction>(Event.LoaderFuncPath))
                            {
                                int Param = 1;
                                LoaderObject->ProcessEvent(const_cast<UFunction*>(LoaderFunction), &Param);
                                printf("[Events] Loaded event level!\n");
                            }
                            else
                                printf("[Events] Failed to load event level!\n");

                        if (GameMode->HasSafeZoneLocations())
                            GameMode->SafeZoneLocations.Free();
                        else
                            UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bEnableZones = false;
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
            TacticalSprintAbility->AddToRoot();
            AbilitySets.Add(TacticalSprintAbility);

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

            if (GameState->HasDefaultGliderRedeployCanRedeploy())
                GameState->DefaultGliderRedeployCanRedeploy = FConfiguration::bGliderRedeploy ? 1.0f : 0.0f;
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

        static bool bTODMApplied = false;

        if (!bTODMApplied)
        {
            UFortKismetLibrary::SetTimeOfDay(UWorld::GetWorld(), FConfiguration::TODMTime);
            UFortKismetLibrary::SetTimeOfDaySpeed(UWorld::GetWorld(), 0.f);
            bTODMApplied = true;
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
        TArray<AFortPlayerControllerAthena*> PlayerList;
        Utils::GetAll<AFortPlayerControllerAthena>(PlayerList);

        for (auto& PlayerController : PlayerList)
        {
            auto PlayerState = PlayerController->PlayerState;

            if (!PlayerState->bIsSpectator && PlayerController->bReadyToStartMatch)
                ReadyPlayers++;
        }

        PlayerList.Free();

        auto VolumeManager = GameState->HasVolumeManager() ? GameState->VolumeManager : nullptr;

        bool bAllLevelsFinishedStreaming = true;
        if (GameState->HasAdditionalPlaylistLevelsStreamed())
        {
            TArray<FPlaylistStreamedLevelData>& AdditionalPlaylistLevels = *(TArray<FPlaylistStreamedLevelData>*) (__int64(GameState) + GameState->GetOffset("AdditionalPlaylistLevelsStreamed") - 0x10);
            for (int i = 0; i < AdditionalPlaylistLevels.Num(); i++)
            {
                auto& AdditionalPlaylistLevel = AdditionalPlaylistLevels.Get(i, FPlaylistStreamedLevelData::Size());

                if (!AdditionalPlaylistLevel.bIsFinishedStreaming || !AdditionalPlaylistLevel.StreamingLevel || !AdditionalPlaylistLevel.StreamingLevel->LoadedLevel->bIsVisible)
                {
                    bAllLevelsFinishedStreaming = false;
                    break;
                }
            }
        }

        static auto WaitingToStart = FName(L"WaitingToStart");
        *Ret = GameMode->bWorldIsReady && (GameState->HasbPlaylistDataIsLoaded() ? GameState->bPlaylistDataIsLoaded : true) && GameMode->MatchState == WaitingToStart && bAllLevelsFinishedStreaming && (!VolumeManager || !(VolumeManager->HasbInSpawningStartup() ? VolumeManager->bInSpawningStartup : GameState->bInSpawningStartup)) && ReadyPlayers >= (GameMode->HasWarmupRequiredPlayerCount() ? GameMode->WarmupRequiredPlayerCount : 1);
    }
    else
        *Ret = callOGWithRet(GameMode, Stack.GetCurrentNativeFunction(), ReadyToStartMatch);

    if (VersionInfo.FortniteVersion >= 11.00 && VersionInfo.FortniteVersion < 25.20 && !*Ret)
    {
        auto Time = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
        auto WarmupDuration = FConfiguration::BusStartDelay;

        if (GameState->HasWarmupCountdownEndTime()) // gamephaselogic builds
        {
            GameState->WarmupCountdownStartTime = Time;
            GameState->WarmupCountdownEndTime = Time + WarmupDuration;
            GameMode->WarmupCountdownDuration = WarmupDuration;
            GameMode->WarmupEarlyCountdownDuration = WarmupDuration;
        }
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
    FVector Center = FConfiguration::CustomSafeZoneCenter;
    float Radius = FConfiguration::CustomSafeZoneRadius;

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
// radii. Applying the newer blanket override while phases are being skipped
// makes those two timelines disagree. Feed centers through SafeZoneLocations,
// then snap only the live circle once the requested late-game phase is active.
static UWorld* GLegacyCustomZoneAppliedWorld = nullptr;
static AFortSafeZoneIndicator* GLegacyCustomZoneAppliedIndicator = nullptr;
static bool GHasNativeLateGameSafeZonePhaseHook = false;

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
    }

    auto Indicator = GameMode->SafeZoneIndicator;
    if (GLegacyCustomZoneAppliedIndicator == Indicator)
        return;

    FVector Center = FConfiguration::CustomSafeZoneCenter;
    float Radius = FConfiguration::CustomSafeZoneRadius;
    if (!std::isfinite(Center.X) || !std::isfinite(Center.Y) ||
        !std::isfinite(Center.Z) || !std::isfinite(Radius) || Radius <= 0.f)
    {
        SDK::DbgLog("[SafeZoneMap] rejected invalid lower custom circle phase=%d center=(%.1f, %.1f, %.1f) radius=%.1f\n",
            SafeZonePhase, Center.X, Center.Y, Center.Z, Radius);
        return;
    }

    const float NativePreviewRadius = Indicator->HasNextRadius()
        ? Indicator->NextRadius
        : Radius;

    // This native setter updates the physical wall/material as well as the
    // reflected live radius. It exists in 2.5-6.21; 1.x uses the field fallback.
    bool bUsedNativeSetter = false;
    if (auto SetRadiusAndCenterFn = Indicator->GetFunction("SetSafeZoneRadiusAndCenter"))
    {
        // Reflected order on Chapter 1 is (InLocation, InRadius).
        Indicator->Call<void>(SetRadiusAndCenterFn, Center, Radius);
        bUsedNativeSetter = true;
    }

    if (Indicator->HasRadius()) Indicator->Radius = Radius;
    if (Indicator->HasLastCenter()) Indicator->LastCenter = Center;
    if (Indicator->HasLastRadius()) Indicator->LastRadius = Radius;
    if (Indicator->HasPreviousCenter()) Indicator->PreviousCenter = Center;
    if (Indicator->HasPreviousRadius()) Indicator->PreviousRadius = Radius;

    // SafeZoneLocations already keeps every native target at the selected
    // center. Preserve the native smaller preview radius; if an unusually tiny
    // custom circle is selected, cap the preview so the storm never expands.
    if (Indicator->HasNextCenter()) Indicator->NextCenter = Center;
    if (Indicator->HasNextNextCenter()) Indicator->NextNextCenter = Center;
    if (Indicator->HasFutureReplicator() && Indicator->FutureReplicator &&
        Indicator->FutureReplicator->HasNextNextCenter())
    {
        Indicator->FutureReplicator->NextNextCenter = Center;
    }
    if (Indicator->HasNextRadius())
    {
        float PreviewRadius = NativePreviewRadius;
        if (!std::isfinite(PreviewRadius) || PreviewRadius <= 0.f || PreviewRadius > Radius)
            PreviewRadius = Radius;
        Indicator->NextRadius = PreviewRadius;
    }

    Indicator->ForceNetUpdate();
    GLegacyCustomZoneAppliedIndicator = Indicator;

    SDK::DbgLog(
        "[SafeZoneMap] activated lower native custom circle phase=%d center=(%.1f, %.1f, %.1f) radius=%.1f nativePreview=%.1f finalPreview=%.1f setter=%d\n",
        SafeZonePhase, Center.X, Center.Y, Center.Z, Radius,
        NativePreviewRadius,
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

    const auto AlignLegacyFallbackCenter = [&]()
    {
        if (VersionInfo.FortniteVersion >= 7.00 ||
            FConfiguration::bCustomSafeZone || SafeZoneLoc.IsZero())
        {
            return;
        }

        if (Indicator->HasNextCenter())
            Indicator->NextCenter = SafeZoneLoc;
        if (Indicator->HasLastCenter())
            Indicator->LastCenter = SafeZoneLoc;
    };

    // Original Erbium reapplies its fallback anchor after every native phase
    // change, including phases after the selected starting phase.
    if (CurrentPhase != LastAlignedPhase)
    {
        AlignLegacyFallbackCenter();
        LastAlignedPhase = CurrentPhase;
    }

    if (CurrentPhase < TargetPhase)
    {
        if (bPaused)
            return;

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
            Indicator->SafeZoneStartShrinkTime = 676767.f;

        if (VersionInfo.FortniteVersion < 7.00 && FConfiguration::bCustomSafeZone)
            ApplyLegacyCustomSafeZoneAtTargetPhase(GameMode, CurrentPhase);
        else if (FConfiguration::bCustomSafeZone)
            ApplyCustomSafeZoneState(GameMode, "late-game-phase-fallback");
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

void AFortGameMode::HandlePostSafeZonePhaseChanged(AFortGameMode* GameMode, int NewSafeZonePhase_Inp)
{
    if (!GameMode->SafeZoneIndicator)
        return;

    auto NewSafeZonePhase = NewSafeZonePhase_Inp >= 0 ? NewSafeZonePhase_Inp : ((GameMode->HasSafeZonePhase() ? GameMode->SafeZonePhase : GameMode->SafeZoneIndicator->CurrentPhase) + 1);
    auto GameState = (AFortGameStateAthena*)GameMode->GameState;

    float TimeSeconds = (float)UGameplayStatics::GetTimeSeconds(GameState);

    if (VersionInfo.FortniteVersion >= 21.10)
    {
        if (HandlePostSafeZonePhaseChangedOG)
            HandlePostSafeZonePhaseChangedOG(GameMode, NewSafeZonePhase_Inp);

        return;
    }


    constexpr static std::array<float, 8> LateGameDurations{
        0.f,
        120.f,
        90.f,
        60.f,
        50.f,
        35.f,
        30.f,
        40.f,
    };

    constexpr static std::array<float, 8> LateGameHoldDurations{
        0.f,
        90.f,
        75.f,
        60.f,
        45.f,
        30.f,
        0.f,
        0.f,
    };

    static auto DurationsOffset = 0;
    if (DurationsOffset == 0)
    {
        DurationsOffset = 0x258;

        if (VersionInfo.FortniteVersion >= 18)
            DurationsOffset = 0x248;
        else if (VersionInfo.FortniteVersion < 15.20)
            DurationsOffset = 0x1f8;
    }

    auto SafeZoneDefinition = &GameState->MapInfo->SafeZoneDefinition;
    TArray<float>& Durations = *(TArray<float>*)(SafeZoneDefinition + DurationsOffset);
    TArray<float>& HoldDurations = *(TArray<float>*)(SafeZoneDefinition + DurationsOffset - 0x10);

    if (VersionInfo.FortniteVersion >= 13.00)
    {


        static bool bSetDurations = false;
        if (!bSetDurations)
        {
            bSetDurations = true;

            auto GameData = GameMode->HasAthenaGameDataTable() ? GameMode->AthenaGameDataTable : GameState->AthenaGameDataTable;

            auto ShrinkTime = FName(L"Default.SafeZone.ShrinkTime");
            auto HoldTime = FName(L"Default.SafeZone.WaitTime");

            for (int i = 0; i < Durations.Num(); i++)
            {
                UDataTableFunctionLibrary::EvaluateCurveTableRow(GameData, ShrinkTime, (float)i, nullptr, &Durations[i], FString());
            }
            for (int i = 0; i < HoldDurations.Num(); i++)
            {
                UDataTableFunctionLibrary::EvaluateCurveTableRow(GameData, HoldTime, (float)i, nullptr, &HoldDurations[i], FString());
            }
        }

        if (!FConfiguration::bLateGame || GameMode->SafeZonePhase > FConfiguration::LateGameZone)
        {
            auto Duration = Durations[NewSafeZonePhase];
            auto HoldDuration = HoldDurations[NewSafeZonePhase];

            GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = TimeSeconds + HoldDuration;
            GameMode->SafeZoneIndicator->SafeZoneFinishShrinkTime = GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime + Duration;
        }
    }

    HandlePostSafeZonePhaseChangedOG(GameMode, NewSafeZonePhase_Inp);

    // Keep the original Erbium/native zone data untouched on Chapter 1 builds.
    // The custom-zone path writes every circle representation at once, which is
    // incompatible with the old indicator's distinct current/preview fields.
    if (VersionInfo.FortniteVersion >= 7.00 && FConfiguration::bLateGame && FConfiguration::bCustomSafeZone)
        ApplyCustomSafeZoneState(GameMode, "native-phase-change");

    /*if (FConfiguration::bLateGame && GameMode->SafeZonePhase > FConfiguration::LateGameZone)
    {
        auto newIdx = GameMode->SafeZonePhase - FConfiguration::LateGameZone + 1;
        auto Duration = newIdx >= LateGameDurations.size() ? 0.f : LateGameDurations[newIdx];
        auto HoldDuration = newIdx >= LateGameHoldDurations.size() ? 0.f : LateGameHoldDurations[newIdx];

        GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = TimeSeconds + HoldDuration;
        GameMode->SafeZoneIndicator->SafeZoneFinishShrinkTime = GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime + Duration;
    }*/


    if (FConfiguration::bLateGame && GameMode->SafeZonePhase < FConfiguration::LateGameZone)
    {
        GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = TimeSeconds;
        GameMode->SafeZoneIndicator->SafeZoneFinishShrinkTime = GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime + 0.15f;
        return;
    }
    else if (FConfiguration::bLateGame && GameMode->SafeZonePhase == FConfiguration::LateGameZone)
    {
        //auto Duration = Durations[FConfiguration::LateGameZone];
        //auto HoldDuration = HoldDurations[FConfiguration::LateGameZone];

        if (FConfiguration::bLateGame && FConfiguration::bLateGameLongZone)
            GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = 676767.f;
        if (VersionInfo.FortniteVersion >= 13)
            GameMode->SafeZoneIndicator->SafeZoneFinishShrinkTime = GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime + Durations[FConfiguration::LateGameZone];
    }

    // Original Erbium applies the selected late-game center only after the
    // native fast-forward reaches its target phase. Applying it to each skipped
    // phase makes the old client's current and preview circles disagree.
    if (VersionInfo.FortniteVersion < 7.00 && FConfiguration::bLateGame &&
        FConfiguration::bCustomSafeZone)
    {
        const int ActiveSafeZonePhase = GameMode->HasSafeZonePhase()
            ? GameMode->SafeZonePhase
            : NewSafeZonePhase;
        ApplyLegacyCustomSafeZoneAtTargetPhase(GameMode, ActiveSafeZonePhase);
    }
    else if (FConfiguration::bLateGame &&
        (SafeZoneLoc.X != 0 || SafeZoneLoc.Y != 0 || SafeZoneLoc.Z != 0))
    {
        GameMode->SafeZoneIndicator->NextCenter = SafeZoneLoc;
        GameMode->SafeZoneIndicator->LastCenter = SafeZoneLoc;
    }

    if (NewSafeZonePhase > (FConfiguration::bLateGame ? FConfiguration::LateGameZone : 1))
    {
        for (auto& UncastedPlayer : GameMode->AlivePlayers)
        {
            auto PlayerController = (AFortPlayerControllerAthena*)UncastedPlayer;

            PlayerController->GetQuestManager(1)->SendStatEvent(PlayerController, EFortQuestObjectiveStatEvent::GetStormPhase(), 1, false);
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
        auto Playlist = GameState->HasCurrentPlaylistInfo()
            ? GameState->CurrentPlaylistInfo.BasePlaylist
            : GameState->CurrentPlaylistData;
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

    if (wcsstr(FConfiguration::Playlist, L"/Game/Athena/Playlists/Creative/Playlist_PlaygroundV2.Playlist_PlaygroundV2"))
        AFortAthenaCreativePortal::Create(NewPlayer);

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
            auto Playlist = GameState->HasCurrentPlaylistInfo()
                ? GameState->CurrentPlaylistInfo.BasePlaylist
                : GameState->CurrentPlaylistData;
            ApplyLateSeasonDBNOSettings(
                GameMode, GameState, Playlist, "post-native");
        }
    }

    return;
}


uint8_t AFortGameMode::PickTeam(AFortGameMode* GameMode, uint8_t PreferredTeam, AFortPlayerControllerAthena* Controller)
{
    if (!GameMode->HasWarmupRequiredPlayerCount())
        return 0;

    auto Playlist = VersionInfo.FortniteVersion >= 3.5 && GameMode->HasWarmupRequiredPlayerCount() ? (GameMode->GameState->HasCurrentPlaylistInfo() ? GameMode->GameState->CurrentPlaylistInfo.BasePlaylist : GameMode->GameState->CurrentPlaylistData) : nullptr;

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
    auto GameState = (AFortGameStateAthena*)GameMode->GameState;
    GLegacyCustomZoneAppliedWorld = UWorld::GetWorld();
    GLegacyCustomZoneAppliedIndicator = nullptr;

    if (!FConfiguration::bCustomSafeZone)
        SafeZoneLoc = FVector{};

    if (FConfiguration::bCustomSafeZone && GameState && GameState->HasMapInfo() && GameState->MapInfo)
        GUI::ResolveCustomSafeZoneForMap(GameState->MapInfo);

    // This must precede the original: early Athena reads the array while
    // StartAircraftPhase is creating/configuring its native safe-zone actor.
    EnsureLegacyLateGameSafeZoneLocations(GameMode);

    auto Ret = StartAircraftPhaseOG(GameMode, a2);

    // Some 4.x/5.x native implementations regenerate this array inside the
    // original call and do not expose bSafeZoneLocationsInitialized. Repair the
    // source again before aircraft placement and later phase callbacks consume it.
    if (VersionInfo.FortniteVersion < 7.00 && FConfiguration::bCustomSafeZone)
        EnsureLegacyLateGameSafeZoneLocations(GameMode);
    
    auto Playlist = VersionInfo.FortniteVersion >= 3.5 && GameMode->HasWarmupRequiredPlayerCount() ? (GameMode->GameState->HasCurrentPlaylistInfo() ? GameMode->GameState->CurrentPlaylistInfo.BasePlaylist : GameMode->GameState->CurrentPlaylistData) : nullptr;
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
        printf("[Events] Auto-start event being called. Will begin in %.1f seconds.\n", FConfiguration::EventStartTime);
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
        bool bScuffed = false;
        if (GameMode->SafeZoneLocations.Num() < 4)
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

            bScuffed = true;

            if (!FindLegacyLateGameSafeZoneCenter(Loc))
                return Ret;
            SafeZoneLoc = Loc;

            //FConfiguration::bLateGame = false;
            //printf("LateGame is not supported on this version!\n");
            //return Ret;
        }
        else
        {
            Loc = GameMode->SafeZoneLocations.Get(FConfiguration::LateGameZone + (VersionInfo.FortniteVersion >= 24 ? 3 : 0) - 1, FVector::Size());
        }

        if (FConfiguration::bCustomSafeZone)
        {
            Loc = FConfiguration::CustomSafeZoneCenter;
            // Lower builds consume the pre-seeded native array and the
            // phase-aware custom-radius path. Keeping this fallback clear
            // prevents it from fighting the native current/preview state.
            if (VersionInfo.FortniteVersion >= 7.00)
                SafeZoneLoc = Loc;
        }

        SDK::DbgLog(
            "[LateGame] aircraft zone anchor version=%.2f phase=%d loc=(%.1f, %.1f, %.1f) locations=%d fallback=%d custom=%d\n",
            VersionInfo.FortniteVersion, FConfiguration::LateGameZone,
            Loc.X, Loc.Y, Loc.Z, GameMode->SafeZoneLocations.Num(),
            (int)!SafeZoneLoc.IsZero(), (int)FConfiguration::bCustomSafeZone);

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

    // PlayerAI must be off the aircraft's books before the native exit
    // processing runs: its auto-jump rejects connectionless controllers
    // and kills the AI as leftover passengers instead.
    MagnesiumPlayerAIIntegration::OnAircraftDropZoneEnding();

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

TArray<FFortSafeZonePhaseInfo> Phases;

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
            FFortSafeZoneDefinition& SafeZoneDefinition = GameState->MapInfo->SafeZoneDefinition;
            float SafeZoneCount = SafeZoneDefinition.Count.Evaluate();

            auto& Array = SafeZoneIndicator->HasSafeZonePhases() ? SafeZoneIndicator->SafeZonePhases : Phases;


            if (Array.IsValid())
                Array.Free();

            const float Time = (float)UGameplayStatics::GetTimeSeconds(GameState);

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

                if (FConfiguration::bLateGame && FConfiguration::bCustomSafeZone)
                {
                    PhaseInfo->Center = FConfiguration::CustomSafeZoneCenter;
                    PhaseInfo->Radius = FConfiguration::CustomSafeZoneRadius;
                    if (i == 0.f)
                        SDK::DbgLog("[SafeZoneMap] applying custom zone center=(%.1f, %.1f, %.1f) radius=%.1f\n",
                            PhaseInfo->Center.X, PhaseInfo->Center.Y, PhaseInfo->Center.Z,
                            PhaseInfo->Radius);
                }

                Array.Add(*PhaseInfo, FFortSafeZonePhaseInfo::Size());
                free(PhaseInfo);

                SafeZoneIndicator->PhaseCount++;
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

    if (Array.IsValidIndex(NewSafeZonePhase))
    {
        if (Array.IsValidIndex(NewSafeZonePhase - 1))
        {
            auto& PreviousPhaseInfo = Array.Get(NewSafeZonePhase - 1, FFortSafeZonePhaseInfo::Size());

            GameMode->SafeZoneIndicator->PreviousCenter = PreviousPhaseInfo.Center;
            GameMode->SafeZoneIndicator->PreviousRadius = PreviousPhaseInfo.Radius;
        }

        auto& PhaseInfo = Array.Get(NewSafeZonePhase, FFortSafeZonePhaseInfo::Size());
        if (FConfiguration::bLateGame && FConfiguration::bCustomSafeZone)
        {
            if (GameState->HasMapInfo() && GameState->MapInfo)
                GUI::ResolveCustomSafeZoneForMap(GameState->MapInfo);
            PhaseInfo.Center = FConfiguration::CustomSafeZoneCenter;
            PhaseInfo.Radius = FConfiguration::CustomSafeZoneRadius;
        }

        GameMode->SafeZoneIndicator->NextCenter = PhaseInfo.Center;
        GameMode->SafeZoneIndicator->NextRadius = PhaseInfo.Radius;
        GameMode->SafeZoneIndicator->NextMegaStormGridCellThickness = PhaseInfo.MegaStormGridCellThickness;

        if (Array.IsValidIndex(NewSafeZonePhase + 1))
        {
            auto& NextPhaseInfo = Array.Get(NewSafeZonePhase + 1, FFortSafeZonePhaseInfo::Size());

            GameMode->SafeZoneIndicator->FutureReplicator->NextNextCenter = NextPhaseInfo.Center;
            GameMode->SafeZoneIndicator->FutureReplicator->NextNextRadius = NextPhaseInfo.Radius;

            GameMode->SafeZoneIndicator->NextNextCenter = NextPhaseInfo.Center;
            GameMode->SafeZoneIndicator->NextNextRadius = NextPhaseInfo.Radius;
            GameMode->SafeZoneIndicator->NextNextMegaStormGridCellThickness = NextPhaseInfo.MegaStormGridCellThickness;
        }

        GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = FConfiguration::bLateGame && FConfiguration::bLateGameLongZone ? 676767.f : TimeSeconds + PhaseInfo.WaitTime;
        GameMode->SafeZoneIndicator->SafeZoneFinishShrinkTime = GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime + PhaseInfo.ShrinkTime;

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

        if (FConfiguration::bLateGame && FConfiguration::bCustomSafeZone)
            ApplyCustomSafeZoneState(GameMode, "managed-phase-start");

        if (!bInitial)
            for (auto& UncastedPlayer : GameMode->AlivePlayers)
            {
                auto PlayerController = (AFortPlayerControllerAthena*)UncastedPlayer;

                PlayerController->GetQuestManager(1)->SendStatEvent(PlayerController, EFortQuestObjectiveStatEvent::GetStormPhase(), 1, false);
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

    StartNewSafeZonePhase(GameMode, FConfiguration::bLateGame ? (FConfiguration::LateGameZone + (VersionInfo.FortniteVersion >= 24 ? 3 : 0)) : 1, true);


    //return SpawnInitialSafeZoneOG(GameMode);
}

void (*UpdateSafeZonesPhaseOG)(AFortGameMode* GameMode);
void UpdateSafeZonesPhase(AFortGameMode* GameMode)
{
    auto& Array = GameMode->SafeZoneIndicator && GameMode->SafeZoneIndicator->HasSafeZonePhases() ? GameMode->SafeZoneIndicator->SafeZonePhases : Phases;
    if (GameMode->bSafeZoneActive && UGameplayStatics::GetTimeSeconds(GameMode) >= GameMode->SafeZoneIndicator->SafeZoneFinishShrinkTime && !GameMode->bSafeZonePaused && Array.IsValidIndex(GameMode->SafeZoneIndicator->CurrentPhase + 1))
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

    if (GameState && VersionInfo.EngineVersion >= 4.22 && VersionInfo.EngineVersion < 4.26)
        GameState->OnRep_CurrentPlaylistInfo();


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

    if (AFortAthenaLivingWorldStaticPointProvider::StaticClass())
    {
        TArray<AFortAthenaLivingWorldStaticPointProvider*> Spawners;
        Utils::GetAll<AFortAthenaLivingWorldStaticPointProvider>(Spawners);
        UEAllocatedMap<FName, const UClass*> VehicleSpawnerMap =
        {
            { FName(L"Athena.Vehicle.SpawnLocation.Motorcycle.Dirtbike"), FindObject<UClass>(L"/Dirtbike/Vehicle/Motorcycle_DirtBike_Vehicle.Motorcycle_DirtBike_Vehicle_C") },
            { FName(L"Athena.Vehicle.SpawnLocation.Motorcycle.Sportbike"), FindObject<UClass>(L"/Sportbike/Vehicle/Motorcycle_Sport_Vehicle.Motorcycle_Sport_Vehicle_C") },
            { FName(L"Athena.Vehicle.SpawnLocation.Valet.BasicCar.Taxi"), FindObject<UClass>(L"/Valet/TaxiCab/Valet_TaxiCab_Vehicle.Valet_TaxiCab_Vehicle_C") },
            { FName(L"Athena.Vehicle.SpawnLocation.Valet.BasicCar.Modded"), FindObject<UClass>(L"/ModdedBasicCar/Vehicle/Valet_BasicCar_Vehicle_SuperSedan.Valet_BasicCar_Vehicle_SuperSedan_C") },
            { FName(L"Athena.Vehicle.SpawnLocation.Valet.BasicTruck.Upgraded"), FindObject<UClass>(L"/Valet/BasicTruck/Valet_BasicTruck_Vehicle_Upgrade.Valet_BasicTruck_Vehicle_Upgrade_C") },
            { FName(L"Athena.Vehicle.SpawnLocation.Valet.BigRig.Upgraded"), FindObject<UClass>(L"/Valet/BigRig/Valet_BigRig_Vehicle_Upgrade.Valet_BigRig_Vehicle_Upgrade_C") },
            { FName(L"Athena.Vehicle.SpawnLocation.Valet.SportsCar.Upgraded"), FindObject<UClass>(L"/Valet/SportsCar/Valet_SportsCar_Vehicle_Upgrade.Valet_SportsCar_Vehicle_Upgrade_C") },
            { FName(L"Athena.Vehicle.SpawnLocation.Valet.BasicCar.Upgraded"), FindObject<UClass>(L"/Valet/BasicCar/Valet_BasicCar_Vehicle_Upgrade.Valet_BasicCar_Vehicle_Upgrade_C") }
        };

        for (auto& Spawner : Spawners)
        {
            const UClass* VehicleClass = nullptr;
            for (int i = 0; i < Spawner->FiltersTags.GameplayTags.Num(); i++)
            {
                auto& Tag = Spawner->FiltersTags.GameplayTags.Get(i, FGameplayTag::Size());

                if (VehicleSpawnerMap.contains(Tag.TagName))
                {
                    VehicleClass = VehicleSpawnerMap[Tag.TagName];
                    break;
                }
            }

            if (VehicleClass)
            {
                auto Vehicle = UWorld::SpawnActor<AFortAthenaVehicle>(VehicleClass, Spawner->K2_GetActorLocation(), Spawner->K2_GetActorRotation());

                if (auto Car = Vehicle->Cast<AFortDagwoodVehicle>())
                    Car->SetFuel(100.f);
                //printf("Spawned a %s\n", Spawner->Name.ToString().c_str());
            }
            else
            {
                for (auto& Tag : Spawner->FiltersTags.GameplayTags)
                    printf("Fix: Tag: %s\n", Tag.TagName.ToString().c_str());
            }
        }
        Spawners.Free();
    }
    // not an else here because they still use spawners for boats, and fully on s27
    if (VersionInfo.EngineVersion >= 4.23 && std::floor(VersionInfo.FortniteVersion) != 20 && std::floor(VersionInfo.FortniteVersion) != 21 && std::floor(VersionInfo.FortniteVersion) != 22) // its auto on s20, s21, and s22
    {
        TArray<AFortAthenaVehicleSpawner*> Spawners{};
        Utils::GetAll<AFortAthenaVehicleSpawner>(Spawners);

        for (auto& Spawner : Spawners)
        {
            auto VehicleClass = Spawner->GetVehicleClass();

            if (Spawner->HasCachedFortVehicleItemDef() && (!Spawner->HasbForceSpawnAlways() || !Spawner->bForceSpawnAlways))
            {
                auto VehicleDef = Spawner->CachedFortVehicleItemDef;
                if (!VehicleDef)
                    continue;

                double Min = std::clamp(VehicleDef->VehicleMinSpawnPercent.Evaluate() * 0.01f, 0.0f, 1.0f);
                double Max = std::clamp(VehicleDef->VehicleMaxSpawnPercent.Evaluate() * 0.01f, 0.0f, 1.0f);

                auto SpawnPercent = Min + (Max - Min) * (rand() / (float)RAND_MAX);
                auto bShouldSpawn = (rand() / (float)RAND_MAX) <= SpawnPercent;

                if (!bShouldSpawn)
                    continue;
            }

            auto Vehicle = UWorld::SpawnActor<AFortAthenaVehicle>(Spawner->GetVehicleClass(), Spawner->K2_GetActorLocation(), Spawner->K2_GetActorRotation());

            if (auto Car = Vehicle->Cast<AFortDagwoodVehicle>())
                Car->SetFuel(100.f);
        }

        Spawners.Free();
    }

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

// 32.11: AGameMode::ReadyToStartMatch is a pure-native call, so Magnesium's ExecHook never fires and the
// match is never configured. Hook it by direct RVA (Remix approach): configure playlist/warmup once, then
// defer the actual readiness decision to the original.
// Per-line-flushed logger (survives a game-thread hang, unlike buffered DbgLog) — used to pinpoint
// exactly where ReadyToStartMatch_Direct dies on 32.11.
static void RtsmLog(const char* fmt, ...)
{
    FILE* f = nullptr;
    fopen_s(&f, "G:\\Fortnite Builds\\32.11\\FortniteGame\\Binaries\\Win64\\rtsm.log", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fflush(f); fclose(f);
}

bool (*ReadyToStartMatch_DirectOG)(AFortGameModeAthena*) = nullptr;
bool ReadyToStartMatch_Direct(AFortGameModeAthena* GameMode)
{
    static int s_call = 0;
    int call = ++s_call;
    if (call <= 20) RtsmLog("[RTSM #%d] enter GM=%p\n", call, (void*)GameMode);

    if (!GameMode || !GameMode->GameState)
        return false;
    auto GameState = (AFortGameStateAthena*)GameMode->GameState;

    static bool s_setup = false;
    if (!s_setup)
    {
        s_setup = true;
        FConfiguration::Playlist = L"/BlastBerry/Playlists/Playlist_SunflowerSolo.Playlist_SunflowerSolo";
        if (GameMode->HasWarmupRequiredPlayerCount())
            GameMode->WarmupRequiredPlayerCount = 1;
        SetupPlaylist(GameMode, GameState);
        RtsmLog("[RTSM] SetupPlaylist done\n");
    }

    if (!GameMode->bWorldIsReady)
    {
        if (!(GameState->HasMapInfo() && GameState->MapInfo))
            return false; // MapInfo actor not spawned yet

        // 32.11: ::Get mis-resolves (returns null though the component exists on the game state, confirmed
        // by object-array enum). Use the direct cached lookup instead.
        auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::GetFixed();
        if (!GamePhaseLogic)
            return false;
        RtsmLog("[RTSM] GetFixed GamePhaseLogic=%p — setting up match\n", (void*)GamePhaseLogic);

        // FindInitializeFlightPath misses on 32.11 -> Better-Remix RVA base+0x9101F80.
        auto InitializeFlightPath = (void(*)(AFortAthenaMapInfo*, AFortGameStateAthena*, UFortGameStateComponent_BattleRoyaleGamePhaseLogic*, bool, double, float, float))
            (VersionInfo.FortniteVersion >= 32.00 ? Memcury::PE::GetModuleBase() + 0x9101F80 : FindInitializeFlightPath());
        RtsmLog("[RTSM] pre-InitFlightPath (%p)\n", (void*)InitializeFlightPath);
        if (InitializeFlightPath)
            InitializeFlightPath(GameState->MapInfo, GameState, GamePhaseLogic, false, 0.f, 0.f, 360.f);
        RtsmLog("[RTSM] post-InitFlightPath, pre-GenerateStormCircles\n");
        UFortGameStateComponent_BattleRoyaleGamePhaseLogic::GenerateStormCircles(GameState->MapInfo);
        RtsmLog("[RTSM] post-GenerateStormCircles\n");

        auto Playlist = GameState->HasCurrentPlaylistInfo() ? GameState->CurrentPlaylistInfo.BasePlaylist : GameState->CurrentPlaylistData;
        if (Playlist && Playlist->HasbSkipWarmup())
            UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bSkipWarmup = Playlist->bSkipWarmup;
        if (Playlist && Playlist->HasbSkipAircraft())
            UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bSkipAircraft = Playlist->bSkipAircraft;

        GameMode->bWorldIsReady = true;
        RtsmLog("[RTSM] bWorldIsReady=true set\n");
    }

    static auto WaitingToStart = FName(L"WaitingToStart");
    if (call <= 20) RtsmLog("[RTSM #%d] pre-GetAll PlayerControllers\n", call);
    int ReadyPlayers = 0;
    TArray<AFortPlayerControllerAthena*> PlayerList;
    Utils::GetAll<AFortPlayerControllerAthena>(PlayerList);
    if (call <= 20) RtsmLog("[RTSM #%d] GetAll done, num=%d\n", call, PlayerList.Num());
    for (auto& PC : PlayerList)
        if (PC && PC->PlayerState && !PC->PlayerState->bIsSpectator && PC->bReadyToStartMatch)
            ReadyPlayers++;
    PlayerList.Free();

    bool ready = GameMode->bWorldIsReady
        && GameMode->MatchState == WaitingToStart
        && ReadyPlayers >= (GameMode->HasWarmupRequiredPlayerCount() ? GameMode->WarmupRequiredPlayerCount : 1);

    if (call <= 20) RtsmLog("[RTSM #%d] worldReady=%d ReadyPlayers=%d ret=%d\n", call, GameMode->bWorldIsReady, ReadyPlayers, ready);
    return ready;
}

void AFortGameMode::Hook()
{
    if (VersionInfo.FortniteVersion >= 32.00)
    {
        auto rtsm = Memcury::PE::GetModuleBase() + 0x918B180;
        SDK::DbgLog("[GameMode] 32.11 direct ReadyToStartMatch hook @ %p (prologue=%08X)\n", (void*)rtsm, *(uint32_t*)rtsm);
        Utils::Hook(rtsm, ReadyToStartMatch_Direct, ReadyToStartMatch_DirectOG);
    }
    else
    {
        auto _rtsmFn = GetDefaultObj()->GetFunction("ReadyToStartMatch");
        SDK::DbgLog("[GameMode] Hook: ReadyToStartMatch UFunction=%p (CDO=%p)\n", (void*)_rtsmFn, (void*)GetDefaultObj());
        Utils::ExecHook(_rtsmFn, ReadyToStartMatch_, ReadyToStartMatch_OG);
    }
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
            Utils::Hook(FindSpawnInitialSafeZone(), SpawnInitialSafeZone, SpawnInitialSafeZoneOG);
            Utils::Hook(FindUpdateSafeZonesPhase(), UpdateSafeZonesPhase, UpdateSafeZonesPhaseOG);
        }
        Utils::ExecHook(L"/Script/FortniteGame.FortSafeZoneIndicator.GetPhaseInfo", GetPhaseInfo);
    }

    //if (VersionInfo.FortniteVersion >= 15)
//    Utils::ExecHook(AFortGameModeAthena::GetDefaultObj()->GetFunction("PlayerCanRestart"), PlayerCanRestart);
}
