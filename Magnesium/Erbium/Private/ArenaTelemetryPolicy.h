#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

// Unreal-independent Arena policy shared by the live reporter and its
// contract test. Keeping the allowlist here prevents the GUI playlist list,
// wire protocol, and backend ingestion range from silently drifting apart.
namespace ArenaTelemetryPolicy
{
    constexpr int kMaximumPointsDelta = 1000000;
    constexpr std::uint64_t kMatchEntryReadinessStabilizationMs = 1000;
    // Native ClientNotifyMatchEntered remains preferable when it is already
    // resolving at the gameplay trigger, but it must not make bus fare feel
    // late. Give only a demonstrably in-flight candidate two existing 100 ms
    // retry intervals plus scheduling margin before the bridge permanently
    // takes presentation ownership.
    constexpr std::uint64_t kMatchEntryPostTriggerCandidateWaitMs = 250;

    inline bool CanUsePrivateArenaPresentationChannel(
        bool HasOwningConnection,
        bool HasPawn,
        bool AcknowledgedPawnMatches) noexcept
    {
        // Controller ClientMessage is a reliable client RPC and does not need
        // the PlayerState actor channel used by ClientNotifyMatchEntered. An
        // acknowledged match pawn proves that the owning client completed the
        // travel far enough to receive the private Arena bridge in this world.
        return HasOwningConnection && HasPawn &&
            AcknowledgedPawnMatches;
    }

    inline bool IsValidPointsDelta(int PointsDelta) noexcept
    {
        return PointsDelta >= -kMaximumPointsDelta &&
            PointsDelta <= kMaximumPointsDelta;
    }

    inline int FortniteVersionHundredths(double Version) noexcept
    {
        if (!std::isfinite(Version) || Version < 0.0)
            return -1;
        return static_cast<int>(std::llround(Version * 100.0));
    }

    inline int GetPlacementPoints(
        int PlayersRemaining) noexcept
    {
        // ATLAS uses one portable Solo Hype economy across every supported
        // Arena build. These are the incremental Season X Solo milestones;
        // do not turn this into per-player placement scoring.
        switch (PlayersRemaining)
        {
        case 25: return 60;
        case 15: return 30;
        case 5: return 30;
        case 1: return 60;
        default: return 0;
        }
    }

    struct FPlacementProgressionState
    {
        // Tournament placement begins above every authored reward tier. The
        // first *finalized live death* advances from this sentinel to the
        // authoritative post-death population. This supports valid small
        // private matches without mistaking pre-match roster bootstrap counts
        // for placement awards.
        int LowestProcessedPopulation = 101;
        std::uint8_t AwardedTierMask = 0;
        bool LiveProgressionStarted = false;
    };

    inline int ConsumeCrossedPlacementPoints(
        int PlayersBeforeDeath,
        int PlayersAfterDeath,
        bool IsFinalizedLiveDeath,
        FPlacementProgressionState* State) noexcept
    {
        if (!State || !IsFinalizedLiveDeath ||
            PlayersBeforeDeath <= 0 || PlayersAfterDeath < 0 ||
            PlayersAfterDeath >= PlayersBeforeDeath)
        {
            return 0;
        }

        if (!State->LiveProgressionStarted)
        {
            State->LiveProgressionStarted = true;
            State->LowestProcessedPopulation = 101;
        }

        // Once the population has descended, a join/repopulation event may not
        // move the scoring cursor backwards or replay an earlier reward.
        const int FromPopulation = State->LowestProcessedPopulation;
        if (PlayersAfterDeath >= FromPopulation)
        {
            return 0;
        }

        struct FTier
        {
            int Population;
            int Points;
            std::uint8_t Mask;
        };
        static constexpr FTier Tiers[] = {
            { 25, 60, 1u << 0 },
            { 15, 30, 1u << 1 },
            { 5, 30, 1u << 2 },
            { 1, 60, 1u << 3 }
        };

        int Points = 0;
        for (const auto& Tier : Tiers)
        {
            if ((State->AwardedTierMask & Tier.Mask) == 0 &&
                FromPopulation > Tier.Population &&
                PlayersAfterDeath <= Tier.Population)
            {
                State->AwardedTierMask |= Tier.Mask;
                Points += Tier.Points;
            }
        }
        State->LowestProcessedPopulation = PlayersAfterDeath;
        return Points;
    }

    template <typename TSeenLifeSet>
    inline bool ConsumeFinalizedVictimLife(
        bool MatchWasLive,
        bool VictimWasAliveParticipant,
        std::uint64_t VictimLifeId,
        TSeenLifeSet* SeenVictimLives)
    {
        if (!MatchWasLive || !VictimWasAliveParticipant ||
            VictimLifeId == 0 || !SeenVictimLives)
        {
            return false;
        }
        return SeenVictimLives->insert(VictimLifeId).second;
    }

    enum class ETournamentModifierPresence : std::uint8_t
    {
        Unavailable,
        Missing,
        Found
    };

    enum class ETournamentModifierAction : std::uint8_t
    {
        None,
        RequestRegistration
    };

    struct FTournamentModifierActivationState
    {
        std::uint8_t FailedRegistrationDispatches = 0;
        bool RegistrationDispatched = false;
        bool NativePresentationConfirmed = false;
        bool RegistrationUnavailable = false;
    };

    inline ETournamentModifierAction ObserveTournamentModifier(
        ETournamentModifierPresence Presence,
        FTournamentModifierActivationState* State) noexcept
    {
        if (!State)
            return ETournamentModifierAction::None;
        if (Presence == ETournamentModifierPresence::Found)
        {
            State->NativePresentationConfirmed = true;
            return ETournamentModifierAction::None;
        }
        if (Presence != ETournamentModifierPresence::Missing ||
            State->RegistrationDispatched ||
            State->RegistrationUnavailable)
        {
            return ETournamentModifierAction::None;
        }
        return ETournamentModifierAction::RequestRegistration;
    }

    inline void CompleteTournamentModifierRegistrationDispatch(
        bool ProcessEventInvoked,
        FTournamentModifierActivationState* State,
        std::uint8_t MaximumFailedDispatches = 3) noexcept
    {
        if (!State || State->RegistrationDispatched ||
            State->RegistrationUnavailable)
        {
            return;
        }
        if (ProcessEventInvoked)
        {
            // RegisterGameplayModifier is not idempotent. Once ProcessEvent
            // begins, no fallback or repeated registration is safe until the
            // active-modifier list positively confirms native ownership.
            State->RegistrationDispatched = true;
            return;
        }
        if (State->FailedRegistrationDispatches < 0xff)
            ++State->FailedRegistrationDispatches;
        if (MaximumFailedDispatches == 0 ||
            State->FailedRegistrationDispatches >= MaximumFailedDispatches)
        {
            State->RegistrationUnavailable = true;
        }
    }

    inline bool ShouldUseSyntheticTournamentPresentation(
        const FTournamentModifierActivationState& State) noexcept
    {
        return !State.NativePresentationConfirmed &&
            State.RegistrationUnavailable &&
            !State.RegistrationDispatched;
    }

    enum class ETournamentRuntimeEffect : std::uint8_t
    {
        SessionLocalPresentation,
        SavedProgression
    };

    inline bool IsTournamentRuntimeEffectEnabled(
        ETournamentRuntimeEffect Effect,
        bool CaptureEnabled) noexcept
    {
        // Save Arena Hype controls ATLAS persistence/restoration only. The
        // match's authored scoring and HUD presentation must keep running when
        // persistence is paused, just as they do without ATLAS attached.
        return Effect == ETournamentRuntimeEffect::
                SessionLocalPresentation ||
            CaptureEnabled;
    }

    inline int ResolveSessionLocalStartingHype(
        bool SavingEnabled,
        int SavedHype) noexcept
    {
        return SavingEnabled && SavedHype > 0
            ? SavedHype
            : 0;
    }

    inline int ApplySessionLocalHypeDelta(
        int CurrentHype,
        int PointsDelta) noexcept
    {
        const std::int64_t SafeCurrent = CurrentHype > 0
            ? CurrentHype
            : 0;
        const std::int64_t Sum = SafeCurrent +
            static_cast<std::int64_t>(PointsDelta);
        if (Sum <= 0)
            return 0;
        const auto Maximum = static_cast<std::int64_t>(
            (std::numeric_limits<std::int32_t>::max)());
        return static_cast<int>(Sum > Maximum ? Maximum : Sum);
    }

    enum class EArenaEntryFeeStageAction : std::uint8_t
    {
        Wait,
        Dispatch
    };

    inline bool CanAttemptArenaMatchEntryNotification(
        bool EntryFeeVisualRequested,
        bool EntryFeeStageComplete) noexcept
    {
        // Identity lookup and local channel-readiness stabilization may finish
        // on spawn island, but the authored match-entry RPC owns the bus-fare
        // presentation. Keep only that dispatch behind the aircraft/first-
        // award trigger so the native RPC and ordered B frame remain adjacent.
        return EntryFeeVisualRequested && !EntryFeeStageComplete;
    }

    inline EArenaEntryFeeStageAction ResolveArenaEntryFeeStageAction(
        bool BootstrapSent,
        bool EntryFeeKnown,
        bool EntryFeeStageComplete,
        int EntryFee,
        bool PresentationTriggered) noexcept
    {
        // The bootstrap establishes the pre-fare total.  Every positive
        // presentation event must remain behind the one-shot fare stage so a
        // late lookup or a missing aircraft callback cannot make the client
        // drift from the backend's session total.
        if (!BootstrapSent || !EntryFeeKnown || EntryFeeStageComplete)
            return EArenaEntryFeeStageAction::Wait;
        // A zero-fare match still emits one ordered B:0 control frame after
        // the aircraft trigger. It gives the client a universal native-HUD
        // ownership boundary without presenting or mutating Hype.
        if (EntryFee == 0)
        {
            return PresentationTriggered
                ? EArenaEntryFeeStageAction::Dispatch
                : EArenaEntryFeeStageAction::Wait;
        }
        return EntryFee < 0 && PresentationTriggered
            ? EArenaEntryFeeStageAction::Dispatch
            : EArenaEntryFeeStageAction::Wait;
    }

    inline bool CanFlushArenaPresentationAwards(
        bool BootstrapSent,
        bool EntryFeeStageComplete) noexcept
    {
        return BootstrapSent && EntryFeeStageComplete;
    }

    enum class EArenaPresentationEndAction : std::uint8_t
    {
        Wait,
        Send,
        Complete
    };

    inline EArenaPresentationEndAction ResolveArenaPresentationEndAction(
        bool EndSent,
        bool ChannelReady,
        bool BootstrapSent,
        bool EntryFeeStageComplete,
        std::size_t PendingPresentationEvents) noexcept
    {
        // END is an ordered terminal barrier. Once it succeeds it must never
        // be replayed, and it may not overtake the pre-fare bootstrap, fare,
        // or any queued elimination/placement/victory presentation.
        if (EndSent)
            return EArenaPresentationEndAction::Complete;
        return ChannelReady && BootstrapSent && EntryFeeStageComplete &&
                PendingPresentationEvents == 0
            ? EArenaPresentationEndAction::Send
            : EArenaPresentationEndAction::Wait;
    }

    inline bool ShouldStageSavedProgression(
        std::uint64_t CaptureRevision,
        bool CaptureEnabled) noexcept
    {
        // Revision zero means the start handshake has not yet reported the
        // authoritative toggle. Preserve the fact for that ordered handshake;
        // once a revision is known, a paused session must not enqueue score.
        return CaptureRevision == 0 || CaptureEnabled;
    }

    // Arena's elimination rule is authored against
    // TEAM_ELIMS_STAT_INDEX. Every human participant on the killer's team
    // receives the Hype delta, while only the killer's personal elimination
    // counter advances.
    inline bool IsValidHumanTeamIndex(
        int TeamIndex) noexcept
    {
        // Fortnite reserves the low team slots for spectators/neutral actors
        // and the high sentinels for unassigned state. Magnesium's human BR
        // allocator uses [3, 249].
        return TeamIndex >= 3 && TeamIndex < 250;
    }

    inline bool ShouldCreditTeamElimination(
        int KillerTeamIndex,
        int ParticipantTeamIndex) noexcept
    {
        return IsValidHumanTeamIndex(KillerTeamIndex) &&
            IsValidHumanTeamIndex(ParticipantTeamIndex) &&
            KillerTeamIndex == ParticipantTeamIndex;
    }

    struct FPlaylistPolicy
    {
        const char* ObjectName;
        const char* CanonicalPath;
        int MinimumVersionHundredths;
    };

    inline bool ResolveCanonicalPlaylist(
        const char* ActivePlaylistObjectName,
        double FortniteVersion,
        const char** CanonicalPath = nullptr) noexcept
    {
        if (CanonicalPath)
            *CanonicalPath = nullptr;
        if (!ActivePlaylistObjectName ||
            !*ActivePlaylistObjectName)
        {
            return false;
        }

        const int VersionHundredths =
            FortniteVersionHundredths(FortniteVersion);
        if (VersionHundredths < 820 ||
            VersionHundredths >= 3100)
        {
            return false;
        }

        static constexpr FPlaylistPolicy Policies[] = {
            {
                "Playlist_ShowdownAlt_Solo",
                "/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Solo.Playlist_ShowdownAlt_Solo",
                820
            },
            {
                "Playlist_ShowdownAlt_Duos",
                "/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Duos.Playlist_ShowdownAlt_Duos",
                820
            },
            {
                "Playlist_ShowdownAlt_Trios",
                "/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Trios.Playlist_ShowdownAlt_Trios",
                820
            },
            {
                "Playlist_ShowdownAlt_Squads",
                "/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Squads.Playlist_ShowdownAlt_Squads",
                820
            },
            {
                "Playlist_ShowdownAlt_NoBuildBR_Solo",
                "/Game/Athena/Playlists/NoBuildBR/Competitive/Playlist_ShowdownAlt_NoBuildBR_Solo.Playlist_ShowdownAlt_NoBuildBR_Solo",
                2000
            },
            {
                "Playlist_ShowdownAlt_NoBuildBR_Duos",
                "/Game/Athena/Playlists/NoBuildBR/Competitive/Playlist_ShowdownAlt_NoBuildBR_Duos.Playlist_ShowdownAlt_NoBuildBR_Duos",
                2000
            },
            {
                "Playlist_ShowdownAlt_NoBuildBR_Trios",
                "/Game/Athena/Playlists/NoBuildBR/Competitive/Playlist_ShowdownAlt_NoBuildBR_Trios.Playlist_ShowdownAlt_NoBuildBR_Trios",
                2000
            },
            {
                "Playlist_ShowdownAlt_NoBuildBR_Squads",
                "/Game/Athena/Playlists/NoBuildBR/Competitive/Playlist_ShowdownAlt_NoBuildBR_Squads.Playlist_ShowdownAlt_NoBuildBR_Squads",
                2000
            }
        };

        for (const auto& Policy : Policies)
        {
            if (VersionHundredths >=
                    Policy.MinimumVersionHundredths &&
                std::strcmp(
                    ActivePlaylistObjectName,
                    Policy.ObjectName) == 0)
            {
                if (CanonicalPath)
                    *CanonicalPath = Policy.CanonicalPath;
                return true;
            }
        }
        return false;
    }

    // This is deliberately identity-only.  The match-entry notification tells
    // the client which already-restored tournament state to display; it never
    // grants, subtracts, or otherwise changes Hype.
    struct FTournamentIdentity
    {
        char EventId[64]{};
        char EventWindowId[80]{};
        char EventGroupId[32]{};
        char EventSubGroupId[1]{};
    };

    inline bool BuildArenaTournamentIdentity(
        double FortniteVersion,
        int Division,
        FTournamentIdentity* OutIdentity = nullptr) noexcept
    {
        if (OutIdentity)
            *OutIdentity = {};

        const int VersionHundredths =
            FortniteVersionHundredths(FortniteVersion);
        if (VersionHundredths < 820 || VersionHundredths >= 3100 ||
            Division < 1 || Division > 10)
        {
            return false;
        }

        const int Season = VersionHundredths / 100;
        if (Season <= 0)
            return false;

        if (OutIdentity)
        {
            const int EventWritten = sprintf_s(
                OutIdentity->EventId,
                "epicgames_Arena_S%d_Solo", Season);
            const int WindowWritten = sprintf_s(
                OutIdentity->EventWindowId,
                "Arena_S%d_Division%d_Solo", Season, Division);
            const int GroupWritten = sprintf_s(
                OutIdentity->EventGroupId,
                "Arena_S%d", Season);
            if (EventWritten <= 0 || WindowWritten <= 0 ||
                GroupWritten <= 0)
            {
                *OutIdentity = {};
                return false;
            }
        }
        return true;
    }

    inline bool ParseArenaDivisionToken(
        const char* Token,
        double FortniteVersion,
        int* OutDivision = nullptr) noexcept
    {
        if (OutDivision)
            *OutDivision = 0;
        if (!Token || !*Token)
            return false;

        const int VersionHundredths =
            FortniteVersionHundredths(FortniteVersion);
        if (VersionHundredths < 820 || VersionHundredths >= 3100)
            return false;

        char Prefix[48]{};
        const int PrefixWritten = sprintf_s(
            Prefix, "ARENA_S%d_Division", VersionHundredths / 100);
        if (PrefixWritten <= 0)
            return false;

        const size_t PrefixLength = strlen(Prefix);
        if (strncmp(Token, Prefix, PrefixLength) != 0)
            return false;

        const char* Number = Token + PrefixLength;
        char* End = nullptr;
        const long Division = strtol(Number, &End, 10);
        if (!End || *End != '\0' || Division < 1 || Division > 10)
            return false;

        if (OutDivision)
            *OutDivision = static_cast<int>(Division);
        return true;
    }

    // Presentation-only schema for the typed tournament-stat client RPC.
    // Values here are raw tournament-stat counts consumed by the event's
    // client-side display rules, never Hype deltas or persistence decisions:
    // the backend reporter remains the sole authority for saved Arena points.
    struct FTournamentStatParameterSchema
    {
        const char* Name = nullptr;
        uint32_t Offset = 0;
        uint32_t ElementSize = 0;
        uint64_t PropertyFlags = 0;
    };

    struct FTournamentStatFieldSchema
    {
        const char* Name = nullptr;
        uint32_t Offset = 0;
        uint32_t ElementSize = 0;
    };

    enum class ETournamentStatSchema : uint8_t
    {
        Unsupported,
        LegacyZeroParameter,
        LegacyFullName,
        LegacyCompactName,
        CompactNameWithoutDelta,
        ModernCompactNameWithDelta
    };

    inline uint32_t TournamentStatNamePropertySize(
        ETournamentStatSchema Schema) noexcept
    {
        return Schema == ETournamentStatSchema::LegacyFullName ? 8u :
            (Schema == ETournamentStatSchema::LegacyCompactName ||
             Schema == ETournamentStatSchema::CompactNameWithoutDelta ||
             Schema == ETournamentStatSchema::ModernCompactNameWithDelta
                ? 4u
                : 0u);
    }

    inline uint32_t TournamentStatParameterSize(
        ETournamentStatSchema Schema) noexcept
    {
        return Schema == ETournamentStatSchema::CompactNameWithoutDelta
            ? 0x18u
            : (Schema == ETournamentStatSchema::LegacyFullName ||
               Schema == ETournamentStatSchema::LegacyCompactName ||
               Schema == ETournamentStatSchema::ModernCompactNameWithDelta
                ? 0x20u
                : 0u);
    }

    inline uint32_t TournamentStatValueOffset(
        ETournamentStatSchema Schema) noexcept
    {
        return Schema == ETournamentStatSchema::CompactNameWithoutDelta
            ? 0x14u
            : (Schema == ETournamentStatSchema::LegacyFullName ||
               Schema == ETournamentStatSchema::LegacyCompactName ||
               Schema == ETournamentStatSchema::ModernCompactNameWithDelta
                ? 0x18u
                : 0u);
    }

    inline bool TournamentStatSchemaHasDelta(
        ETournamentStatSchema Schema) noexcept
    {
        return Schema ==
            ETournamentStatSchema::ModernCompactNameWithDelta;
    }

    inline ETournamentStatSchema ResolveTournamentStatSchema(
        uint32_t FunctionParameterSize,
        const FTournamentStatParameterSchema* FunctionParameters,
        size_t FunctionParameterCount,
        uint32_t StatInfoSize,
        const FTournamentStatFieldSchema* Fields,
        size_t FieldCount) noexcept
    {
        constexpr uint64_t CpfParm = 0x80;
        constexpr uint64_t CpfOutParm = 0x100;
        constexpr uint64_t CpfReturnParm = 0x400;
        constexpr uint64_t CpfReferenceParm = 0x8000000;
        constexpr uint32_t CompactStatInfoSize = 0x18;
        constexpr uint32_t ExtendedStatInfoSize = 0x20;

        // The original Arena RPC carries no explicit payload. Selecting this
        // contract from its complete reflected function layout lets legacy
        // clients share the same dispatch path without a release boundary.
        if (FunctionParameterSize == 0 && FunctionParameterCount == 0)
            return ETournamentStatSchema::LegacyZeroParameter;

        if (!FunctionParameters || FunctionParameterCount != 1 ||
            !Fields ||
            FunctionParameterSize != StatInfoSize ||
            (StatInfoSize != CompactStatInfoSize &&
             StatInfoSize != ExtendedStatInfoSize))
        {
            return ETournamentStatSchema::Unsupported;
        }

        const auto& Parameter = FunctionParameters[0];
        const bool IsOutReference =
            (Parameter.PropertyFlags & CpfOutParm) != 0 &&
            (Parameter.PropertyFlags & CpfReferenceParm) != 0;
        if (!Parameter.Name || !*Parameter.Name ||
            Parameter.Offset != 0 ||
            Parameter.ElementSize != StatInfoSize ||
            (Parameter.PropertyFlags & CpfParm) == 0 ||
            (Parameter.PropertyFlags & CpfReturnParm) != 0 ||
            ((Parameter.PropertyFlags & CpfOutParm) != 0 &&
             !IsOutReference))
        {
            return ETournamentStatSchema::Unsupported;
        }

        if (FieldCount != 3 && FieldCount != 4)
            return ETournamentStatSchema::Unsupported;

        const auto HasField = [&](const char* Name,
                                  uint32_t Offset,
                                  uint32_t ElementSize) noexcept
        {
            for (size_t Index = 0; Index < FieldCount; ++Index)
            {
                const auto& Field = Fields[Index];
                if (Field.Name && strcmp(Field.Name, Name) == 0 &&
                    Field.Offset == Offset &&
                    Field.ElementSize == ElementSize)
                {
                    return true;
                }
            }
            return false;
        };

        if (!HasField("StatName", 0x0, 0x10))
        {
            return ETournamentStatSchema::Unsupported;
        }

        // Some Arena-era clients reflect the compact, no-delta payload as
        // exactly 0x18 bytes. FName occupies four bytes at 0x10 and the
        // absolute value follows immediately at 0x14. Select this ABI only
        // from the complete live layout; no release number is assumed.
        if (StatInfoSize == CompactStatInfoSize && FieldCount == 3 &&
            HasField("StatDisplayName", 0x10, 0x4) &&
            HasField("StatValue", 0x14, 0x4))
        {
            return ETournamentStatSchema::CompactNameWithoutDelta;
        }

        if (StatInfoSize != ExtendedStatInfoSize ||
            !HasField("StatValue", 0x18, 0x4))
        {
            return ETournamentStatSchema::Unsupported;
        }

        if (FieldCount == 3 &&
            HasField("StatDisplayName", 0x10, 0x8))
        {
            return ETournamentStatSchema::LegacyFullName;
        }
        if (FieldCount == 3 &&
            HasField("StatDisplayName", 0x10, 0x4))
        {
            return ETournamentStatSchema::LegacyCompactName;
        }
        if (FieldCount == 4 &&
            HasField("StatDisplayName", 0x10, 0x4) &&
            HasField("bIsDeltaCount", 0x14, 0x1))
        {
            return ETournamentStatSchema::ModernCompactNameWithDelta;
        }
        return ETournamentStatSchema::Unsupported;
    }

    struct FTournamentStatVisual
    {
        const wchar_t* StatName = nullptr;
        const wchar_t* DisplayName = nullptr;
        int32_t AbsoluteValue = 0;
        int32_t DeltaValue = 0;
        bool PreferDelta = false;
    };

    struct FResolvedTournamentStatPayload
    {
        int32_t Value = 0;
        bool IsDelta = false;
    };

    inline FResolvedTournamentStatPayload ResolveTournamentStatPayload(
        ETournamentStatSchema Schema,
        const FTournamentStatVisual& Visual) noexcept
    {
        FResolvedTournamentStatPayload Payload{};
        if (TournamentStatSchemaHasDelta(Schema) &&
            Visual.PreferDelta)
        {
            Payload.Value = Visual.DeltaValue;
            Payload.IsDelta = true;
            return Payload;
        }
        Payload.Value = Visual.AbsoluteValue;
        return Payload;
    };

    inline int ResolveTeamEliminationVisualValue(
        bool HasTeamKillScore,
        int TeamKillScore,
        int PersonalKillScore) noexcept
    {
        const int Personal = PersonalKillScore > 0
            ? PersonalKillScore
            : 0;
        if (!HasTeamKillScore)
            return Personal;
        const int Team = TeamKillScore > 0 ? TeamKillScore : 0;
        return Team > Personal ? Team : Personal;
    }

    inline FTournamentStatVisual BuildTeamEliminationVisual(
        int AbsoluteTeamEliminations) noexcept
    {
        FTournamentStatVisual Visual{};
        Visual.StatName = L"TEAM_ELIMS_STAT_INDEX";
        Visual.DisplayName = L"Eliminations";
        Visual.AbsoluteValue = AbsoluteTeamEliminations > 0
            ? AbsoluteTeamEliminations
            : 0;
        Visual.DeltaValue = 1;
        Visual.PreferDelta = true;
        return Visual;
    }

    inline FTournamentStatVisual BuildPlacementVisual(
        int Placement) noexcept
    {
        FTournamentStatVisual Visual{};
        Visual.StatName = L"PLACEMENT_STAT_INDEX";
        Visual.DisplayName = L"Placement";
        Visual.AbsoluteValue = Placement > 0 ? Placement : 0;
        Visual.PreferDelta = false;
        return Visual;
    }

    inline FTournamentStatVisual BuildMatchEntryFeeVisual() noexcept
    {
        FTournamentStatVisual Visual{};
        Visual.StatName = L"MATCH_PLAYED_STAT";
        Visual.DisplayName = L"MatchEntryFee";
        Visual.AbsoluteValue = 1;
        Visual.DeltaValue = 1;
        Visual.PreferDelta = true;
        return Visual;
    }

    // The stat and placement RPCs are one-way client presentation calls. A
    // successful ProcessEvent has no acknowledgement, so issuing a second RPC
    // after Sent could double the client's visible/local score. Even an SEH
    // failure during ProcessEvent can occur after the packet was queued, so
    // the placement fallback is legal only when schema validation rejected
    // before ProcessEvent began.
    enum class ETournamentVisualDispatchOutcome : uint8_t
    {
        Sent,
        SchemaUnsupported,
        ProcessEventFailed
    };

    // Canonical Arena drives elimination presentation from its native
    // death/stat path. Mirroring that kill from this hook shows the same Hype
    // award twice on supported clients (most visibly on the first kill).
    // Keep this reflected call solely for Magnesium's separate tournament
    // playlist behavior. Canonical placement has its own fallback ownership
    // contract because native placement presentation is not guaranteed when
    // modifier registration fails before ProcessEvent begins.
    inline bool ShouldDispatchDeathPipelineEliminationVisual(
        bool IsCanonicalArena,
        bool IsStandaloneTournament) noexcept
    {
        return !IsCanonicalArena && IsStandaloneTournament;
    }

    inline bool ShouldFallbackEliminationPresentation(
        bool IsCanonicalArena,
        ETournamentVisualDispatchOutcome Outcome) noexcept
    {
        return IsCanonicalArena &&
            Outcome ==
                ETournamentVisualDispatchOutcome::SchemaUnsupported;
    }

    inline bool ShouldFallbackPlacementToTypedStat(
        ETournamentVisualDispatchOutcome Outcome) noexcept
    {
        return Outcome ==
            ETournamentVisualDispatchOutcome::SchemaUnsupported;
    }

    // Presentation-only contract for
    // ClientReportTournamentPlacementPointsScored(int32 Placement,
    // int32 PointsEarned). Generated SDKs expose this exact RPC throughout
    // the Arena support range, but the live path still validates reflection
    // before constructing a parameter buffer.
    inline bool ResolveTournamentPlacementPointsSchema(
        uint32_t FunctionParameterSize,
        const FTournamentStatParameterSchema* FunctionParameters,
        size_t FunctionParameterCount) noexcept
    {
        constexpr uint64_t CpfParm = 0x80;
        constexpr uint64_t CpfOutParm = 0x100;
        constexpr uint64_t CpfReturnParm = 0x400;
        constexpr uint64_t CpfReferenceParm = 0x8000000;
        if (FunctionParameterSize != 8 || !FunctionParameters ||
            FunctionParameterCount != 2)
        {
            return false;
        }

        const auto HasInput = [&](const char* Name,
                                  uint32_t Offset) noexcept
        {
            for (size_t Index = 0; Index < FunctionParameterCount; ++Index)
            {
                const auto& Parameter = FunctionParameters[Index];
                if (Parameter.Name && strcmp(Parameter.Name, Name) == 0 &&
                    Parameter.Offset == Offset &&
                    Parameter.ElementSize == 4 &&
                    (Parameter.PropertyFlags & CpfParm) != 0 &&
                    (Parameter.PropertyFlags &
                        (CpfOutParm | CpfReturnParm | CpfReferenceParm)) == 0)
                {
                    return true;
                }
            }
            return false;
        };
        return HasInput("Placement", 0) &&
            HasInput("PointsEarned", 4);
    }

    // A reflection-only description of the ClientNotifyMatchEntered RPC.
    // Keeping this independent of Unreal types makes the guard regression
    // testable without a live game process.
    struct FMatchEntryParameterSchema
    {
        const char* Name = nullptr;
        uint32_t Offset = 0;
        uint32_t ElementSize = 0;
        uint64_t PropertyFlags = 0;
    };

    enum class EMatchEntrySchema : uint8_t
    {
        Unsupported,
        EventWindowOnly,
        EventAndWindow,
        EventWindowAndGroup,
        TournamentIds
    };

    inline EMatchEntrySchema ResolveMatchEntrySchema(
        uint32_t TotalSize,
        uint32_t FStringSize,
        const FMatchEntryParameterSchema* Parameters,
        size_t ParameterCount) noexcept
    {
        if (!Parameters || FStringSize == 0)
        {
            return EMatchEntrySchema::Unsupported;
        }

        const auto Matches = [&](const char* Name,
                                 uint32_t Offset,
                                 uint32_t ElementSize) noexcept
        {
            constexpr uint64_t CpfParm = 0x80;
            constexpr uint64_t CpfOutParm = 0x100;
            constexpr uint64_t CpfReturnParm = 0x400;
            for (size_t Index = 0; Index < ParameterCount; ++Index)
            {
                const auto& Parameter = Parameters[Index];
                if (Parameter.Name && strcmp(Parameter.Name, Name) == 0 &&
                    Parameter.Offset == Offset &&
                    Parameter.ElementSize == ElementSize &&
                    (Parameter.PropertyFlags & CpfParm) != 0 &&
                    (Parameter.PropertyFlags &
                        (CpfOutParm | CpfReturnParm)) == 0)
                {
                    return true;
                }
            }
            return false;
        };

        // 9.41 exposes only the selected division window. Select this from
        // the exact reflected property name and FString layout; the older
        // plural `EventIds` single-string ABI has different, undocumented
        // semantics and must remain unsupported.
        if (ParameterCount == 1 && TotalSize == FStringSize &&
            Matches("EventWindowId", 0, FStringSize))
        {
            return EMatchEntrySchema::EventWindowOnly;
        }

        // 10.00, 10.40, 11.31, and 12.41:
        // ClientNotifyMatchEntered(FString EventId, FString EventWindowId)
        if (ParameterCount == 2 && TotalSize == FStringSize * 2 &&
            Matches("EventId", 0, FStringSize) &&
            Matches("EventWindowId", FStringSize, FStringSize))
        {
            return EMatchEntrySchema::EventAndWindow;
        }

        // 13.40:
        // ClientNotifyMatchEntered(EventId, EventWindowId, EventGroupId)
        if (ParameterCount == 3 && TotalSize == FStringSize * 3 &&
            Matches("EventId", 0, FStringSize) &&
            Matches("EventWindowId", FStringSize, FStringSize) &&
            Matches("EventGroupId", FStringSize * 2, FStringSize))
        {
            return EMatchEntrySchema::EventWindowAndGroup;
        }

        // 14.30+ (including 17.30):
        // ClientNotifyMatchEntered(FEventTournamentIds EventIds), where the
        // reflected parameter is a single 0x40-byte struct consisting of
        // EventId, WindowId, GroupId, and SubGroupId FStrings at 0x0/10/20/30.
        if (ParameterCount == 1 && TotalSize == FStringSize * 4 &&
            Matches("EventIds", 0, FStringSize * 4))
        {
            return EMatchEntrySchema::TournamentIds;
        }

        // 8.51's single FString EventIds is deliberately unsupported: it
        // cannot carry the requested event *and* division window without
        // inventing an undocumented serialization format.
        return EMatchEntrySchema::Unsupported;
    }

    inline bool CanResolveTournamentLookup(
        size_t PendingMessages,
        size_t PendingAborts,
        bool ReporterQueueEmpty) noexcept
    {
        // Restored division state must be fetched only after every older
        // score-bearing message (especially the prior match's End) has been
        // acknowledged. The producer ring is checked separately because an
        // End can arrive after the worker's initial drain. Otherwise a fast
        // requeue can bind stale Hype for the entire next match.
        return PendingMessages == 0 && PendingAborts == 0 &&
            ReporterQueueEmpty;
    }

    inline bool AdvanceMatchEntryReadiness(
        bool PrerequisitesReady,
        std::uint64_t NowMs,
        bool* InOutObserved,
        std::uint64_t* InOutStableSinceMs,
        std::uint64_t StabilizationMs =
            kMatchEntryReadinessStabilizationMs) noexcept
    {
        if (!InOutObserved || !InOutStableSinceMs)
            return false;

        if (!PrerequisitesReady)
        {
            *InOutObserved = false;
            *InOutStableSinceMs = 0;
            return false;
        }

        if (!*InOutObserved || NowMs < *InOutStableSinceMs)
        {
            *InOutObserved = true;
            *InOutStableSinceMs = NowMs;
            return StabilizationMs == 0;
        }

        return NowMs - *InOutStableSinceMs >= StabilizationMs;
    }

    enum class EMatchEntryPresentationOwnership : std::uint8_t
    {
        AwaitNative,
        Native,
        DirectBridge
    };

    inline bool HasInFlightMatchEntryCandidate(
        bool LookupQueued,
        bool IdentityCached) noexcept
    {
        // A queued lookup can still supply the exact event identity. A cached
        // identity means the game-thread notification path is actively
        // waiting on its reflected channel/readiness checks. Mere playlist
        // eligibility is not an in-flight native candidate.
        return LookupQueued || IdentityCached;
    }

    inline EMatchEntryPresentationOwnership
        ResolveMatchEntryPresentationOwnership(
            bool MatchEntryEligible,
            bool NativeNotificationSent,
            bool NativeNotificationUnavailable,
            bool DirectBridgeAlreadyOwnsPresentation,
            bool NativeCandidateInFlight,
            std::uint64_t NowMs,
            std::uint64_t WaitStartedAtMs,
            std::uint64_t MaximumWaitMs =
                kMatchEntryPostTriggerCandidateWaitMs) noexcept
    {
        // Once ProcessEvent has reported success, the native route owns this
        // participant even if another participant later proves the schema
        // unavailable. The bridge still sends B with native grace for repair.
        if (NativeNotificationSent)
            return EMatchEntryPresentationOwnership::Native;
        if (!MatchEntryEligible || NativeNotificationUnavailable ||
            DirectBridgeAlreadyOwnsPresentation)
        {
            return EMatchEntryPresentationOwnership::DirectBridge;
        }
        if (!NativeCandidateInFlight)
            return EMatchEntryPresentationOwnership::DirectBridge;

        // A regressed clock is not evidence that the native route timed out.
        // The live caller establishes a non-zero start only when gameplay
        // triggers the fare stage; this policy can never release B pre-jump.
        if (!WaitStartedAtMs || NowMs < WaitStartedAtMs ||
            NowMs - WaitStartedAtMs < MaximumWaitMs)
        {
            return EMatchEntryPresentationOwnership::AwaitNative;
        }
        return EMatchEntryPresentationOwnership::DirectBridge;
    }

    inline bool CanNotifyMatchEntryForPlaylist(
        const char* CanonicalPlaylist) noexcept
    {
        // The backend currently publishes an exact Solo event/window
        // contract. Never bind that identity to a Duo/Trio/Squad/No-Build
        // match; those formats remain score-captured but fail closed for the
        // client notification until matching event contracts are published.
        return CanonicalPlaylist && strcmp(
            CanonicalPlaylist,
            "/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Solo.Playlist_ShowdownAlt_Solo") == 0;
    }

    inline bool IsLegacyMatchEntrySchema(
        uint32_t TotalSize,
        uint32_t FStringSize,
        const FMatchEntryParameterSchema* Parameters,
        size_t ParameterCount) noexcept
    {
        return ResolveMatchEntrySchema(
            TotalSize, FStringSize, Parameters, ParameterCount) ==
            EMatchEntrySchema::EventAndWindow;
    }
}
