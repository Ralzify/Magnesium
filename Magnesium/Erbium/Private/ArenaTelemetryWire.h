#pragma once

#include "../../json.hpp"
#include "ArenaTelemetryPolicy.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

// Small, Unreal-independent wire helpers shared by the reporter and its
// contract test. Fortnite releases are identifiers rather than decimal
// quantities: 8.20 must stay "8.20" on the wire instead of becoming 8.2.
namespace ArenaTelemetryWire
{
    enum class EArenaPresentationEventKind : wchar_t
    {
        BusFare = L'B',
        Elimination = L'E',
        Placement = L'P',
        Victory = L'V'
    };

    constexpr wchar_t kArenaBootstrapPrefix[] =
        L"ATLAS_ARENA_BOOTSTRAP_V1:";
    constexpr wchar_t kArenaEventPrefix[] =
        L"ATLAS_ARENA_EVENT_V1:";
    constexpr wchar_t kArenaEndPrefix[] =
        L"ATLAS_ARENA_END_V1:";
    constexpr std::size_t kArenaBootstrapPrefixLength =
        (sizeof(kArenaBootstrapPrefix) / sizeof(wchar_t)) - 1;
    constexpr std::size_t kArenaEventPrefixLength =
        (sizeof(kArenaEventPrefix) / sizeof(wchar_t)) - 1;
    constexpr std::size_t kArenaEndPrefixLength =
        (sizeof(kArenaEndPrefix) / sizeof(wchar_t)) - 1;
    constexpr std::size_t kArenaBootstrapMessageLength =
        kArenaBootstrapPrefixLength + 16 + 3 + 1 + 1 + 10 + 1 + 7;
    constexpr std::size_t kArenaEventMessageLength =
        kArenaEventPrefixLength + 16 + 1 + 16 + 1 + 1 + 1 + 7 + 1 +
        10 + 1 + 3 + 1 + 1;
    constexpr std::size_t kArenaEndMessageLength =
        kArenaEndPrefixLength + 16;

    inline bool IsArenaPresentationEventKind(
        EArenaPresentationEventKind Kind) noexcept
    {
        return Kind == EArenaPresentationEventKind::BusFare ||
            Kind == EArenaPresentationEventKind::Elimination ||
            Kind == EArenaPresentationEventKind::Placement ||
            Kind == EArenaPresentationEventKind::Victory;
    }

    inline bool IsArenaPresentationEventShapeValid(
        EArenaPresentationEventKind Kind,
        int PointsDelta,
        int Placement) noexcept
    {
        if (!IsArenaPresentationEventKind(Kind) ||
            PointsDelta < -999999 || PointsDelta > 999999)
        {
            return false;
        }

        switch (Kind)
        {
        case EArenaPresentationEventKind::BusFare:
            return PointsDelta <= 0 && Placement == 0;
        case EArenaPresentationEventKind::Elimination:
            return PointsDelta > 0 && Placement == 0;
        case EArenaPresentationEventKind::Placement:
            return PointsDelta > 0 && Placement > 1 && Placement <= 100;
        case EArenaPresentationEventKind::Victory:
            return PointsDelta > 0 && Placement == 1;
        default:
            return false;
        }
    }

    inline bool FormatArenaPresentationBootstrapV1(
        std::uint64_t SessionOrdinal,
        bool SavingEnabled,
        std::int32_t StartingHype,
        std::int32_t ArenaEntryFee,
        wchar_t* OutMessage,
        std::size_t OutMessageCapacity) noexcept
    {
        if (!OutMessage || OutMessageCapacity == 0)
            return false;
        OutMessage[0] = L'\0';

        if (SessionOrdinal == 0 || StartingHype < 0 ||
            ArenaEntryFee > 0 || ArenaEntryFee < -999999 ||
            OutMessageCapacity <= kArenaBootstrapMessageLength)
        {
            return false;
        }

        const int Written = std::swprintf(
            OutMessage,
            OutMessageCapacity,
            L"%ls%016llX:A:%d:%010d:%+07d",
            kArenaBootstrapPrefix,
            static_cast<unsigned long long>(SessionOrdinal),
            SavingEnabled ? 1 : 0,
            StartingHype,
            ArenaEntryFee);
        if (Written != static_cast<int>(kArenaBootstrapMessageLength))
        {
            OutMessage[0] = L'\0';
            return false;
        }
        return true;
    }

    inline bool FormatArenaPresentationEventV1(
        std::uint64_t SessionOrdinal,
        std::uint64_t EventSequence,
        EArenaPresentationEventKind Kind,
        std::int32_t PointsDelta,
        std::int32_t ResultingHype,
        std::int32_t Placement,
        bool NativeGraceExpected,
        wchar_t* OutMessage,
        std::size_t OutMessageCapacity) noexcept
    {
        if (!OutMessage || OutMessageCapacity == 0)
            return false;
        OutMessage[0] = L'\0';

        if (SessionOrdinal == 0 || EventSequence == 0 ||
            ResultingHype < 0 ||
            !IsArenaPresentationEventShapeValid(
                Kind, PointsDelta, Placement) ||
            OutMessageCapacity <= kArenaEventMessageLength)
        {
            return false;
        }

        const int Written = std::swprintf(
            OutMessage,
            OutMessageCapacity,
            L"%ls%016llX:%016llX:%lc:%+07d:%010d:%03d:%d",
            kArenaEventPrefix,
            static_cast<unsigned long long>(SessionOrdinal),
            static_cast<unsigned long long>(EventSequence),
            static_cast<wchar_t>(Kind),
            PointsDelta,
            ResultingHype,
            Placement,
            NativeGraceExpected ? 1 : 0);
        if (Written != static_cast<int>(kArenaEventMessageLength))
        {
            OutMessage[0] = L'\0';
            return false;
        }
        return true;
    }

    inline bool FormatArenaPresentationEndV1(
        std::uint64_t SessionOrdinal,
        wchar_t* OutMessage,
        std::size_t OutMessageCapacity) noexcept
    {
        if (!OutMessage || OutMessageCapacity == 0)
            return false;
        OutMessage[0] = L'\0';

        if (SessionOrdinal == 0 ||
            OutMessageCapacity <= kArenaEndMessageLength)
        {
            return false;
        }

        const int Written = std::swprintf(
            OutMessage,
            OutMessageCapacity,
            L"%ls%016llX",
            kArenaEndPrefix,
            static_cast<unsigned long long>(SessionOrdinal));
        if (Written != static_cast<int>(kArenaEndMessageLength))
        {
            OutMessage[0] = L'\0';
            return false;
        }
        return true;
    }

    inline bool FormatPlacementPresentationBridgeV2(
        int PreviousPlacement,
        int TargetPlacement,
        int PointsEarned,
        bool DedicatedRpcExpected,
        wchar_t* OutMessage,
        std::size_t OutMessageCapacity) noexcept
    {
        if (!OutMessage || OutMessageCapacity == 0)
            return false;
        OutMessage[0] = L'\0';

        // This private ClientMessage transport describes one authoritative
        // population transition. Carrying both ends lets ATLAS Client seed a
        // season's placement consumer with its real previous state instead of
        // relying on an uninitialized/default widget field. PreviousPlacement
        // may be the progression policy's intentional 101 start sentinel; the
        // target remains a real placement. A transition must always move
        // toward first place and can only carry a positive award.
        if (PreviousPlacement <= 0 || PreviousPlacement > 101 ||
            TargetPlacement <= 0 || TargetPlacement > 100 ||
            TargetPlacement >= PreviousPlacement ||
            PointsEarned <= 0 || PointsEarned > 999999)
        {
            return false;
        }

        const int Written = std::swprintf(
            OutMessage,
            OutMessageCapacity,
            L"ATLAS_ARENA_PLACEMENT_V2:%03d:%03d:%06d:%d",
            PreviousPlacement,
            TargetPlacement,
            PointsEarned,
            DedicatedRpcExpected ? 1 : 0);
        if (Written <= 0 ||
            static_cast<std::size_t>(Written) >= OutMessageCapacity)
        {
            OutMessage[0] = L'\0';
            return false;
        }
        return true;
    }

    inline std::string FormatFortniteVersion(double Version)
    {
        if (!std::isfinite(Version) || Version < 0.0)
            return "0.00";

        const long long Hundredths =
            static_cast<long long>(std::llround(Version * 100.0));
        char Text[32]{};
        std::snprintf(
            Text,
            sizeof(Text),
            "%lld.%02lld",
            Hundredths / 100,
            Hundredths % 100);
        return Text;
    }

    inline nlohmann::json BuildSessionEnvelope(
        const char* ServerInstanceId,
        const char* SessionId,
        double FortniteVersion,
        const char* Playlist,
        int SchemaVersion)
    {
        return {
            { "schema", SchemaVersion },
            { "serverInstanceId", ServerInstanceId },
            { "sessionId", SessionId },
            { "fortniteVersion",
              FormatFortniteVersion(FortniteVersion) },
            { "playlist", Playlist },
            { "events", nlohmann::json::array() }
        };
    }

    // A 2xx response is not enough for a score-bearing batch: the backend
    // can reject or discard one event while accepting its siblings. Require
    // every outbound id to be explicitly acknowledged so the reporter can
    // invalidate the whole session instead of later committing a partial
    // score.
    inline bool AcknowledgesEveryEvent(
        const std::string& ResponseBody,
        const std::vector<std::string>& EventIds)
    {
        if (EventIds.empty())
            return true;

        const auto Document = nlohmann::json::parse(
            ResponseBody, nullptr, false);
        if (Document.is_discarded() || !Document.is_object())
            return false;

        std::unordered_set<std::string> Acknowledged;
        const auto Collect = [&](const char* Field)
        {
            if (!Document.contains(Field) ||
                !Document[Field].is_array())
            {
                return;
            }
            for (const auto& Value : Document[Field])
            {
                if (Value.is_string())
                    Acknowledged.insert(Value.get<std::string>());
            }
        };
        Collect("acceptedEventIds");
        Collect("duplicateEventIds");

        for (const auto& Id : EventIds)
        {
            if (Acknowledged.find(Id) == Acknowledged.end())
                return false;
        }
        return true;
    }

    // The dedicated player endpoint returns player fields at the root, while
    // the compete-download endpoint nests the same fields inside `player`.
    // Never fall back to root data when a nested player object was present but
    // malformed, because that could bind an unrelated compatibility token.
    inline bool ResolveArenaTournamentIdentityFromPlayerResponse(
        const std::string& ResponseBody,
        double FortniteVersion,
        ArenaTelemetryPolicy::FTournamentIdentity* OutIdentity = nullptr,
        std::int32_t* OutSavedHype = nullptr,
        std::int32_t* OutArenaEntryFee = nullptr,
        bool* OutHasArenaEntryFee = nullptr)
    {
        if (OutIdentity)
            *OutIdentity = {};
        if (OutSavedHype)
            *OutSavedHype = 0;
        if (OutArenaEntryFee)
            *OutArenaEntryFee = 0;
        if (OutHasArenaEntryFee)
            *OutHasArenaEntryFee = false;

        const auto Document = nlohmann::json::parse(
            ResponseBody, nullptr, false);
        if (Document.is_discarded() || !Document.is_object())
            return false;

        const nlohmann::json* Player = &Document;
        if (Document.contains("player"))
        {
            if (!Document["player"].is_object())
                return false;
            Player = &Document["player"];
        }

        if (!Player->contains("tokens") ||
            !(*Player)["tokens"].is_array() ||
            !Player->contains("persistentScores") ||
            !(*Player)["persistentScores"].is_object() ||
            !(*Player)["persistentScores"].contains("Hype"))
        {
            return false;
        }

        const auto& HypeValue = (*Player)["persistentScores"]["Hype"];
        std::int32_t SavedHype = 0;
        if (HypeValue.is_number_unsigned())
        {
            const auto Value = HypeValue.get<std::uint64_t>();
            if (Value > static_cast<std::uint64_t>(
                    (std::numeric_limits<std::int32_t>::max)()))
            {
                return false;
            }
            SavedHype = static_cast<std::int32_t>(Value);
        }
        else if (HypeValue.is_number_integer())
        {
            const auto Value = HypeValue.get<std::int64_t>();
            if (Value < 0 ||
                Value > static_cast<std::int64_t>(
                    (std::numeric_limits<std::int32_t>::max)()))
            {
                return false;
            }
            SavedHype = static_cast<std::int32_t>(Value);
        }
        else
        {
            return false;
        }

        // Older ATLAS Backend builds did not expose the authored Arena entry
        // fee. Keep identity restore backward compatible, but tell the caller
        // whether a trustworthy fee was actually supplied so the bus-fare
        // presentation can fail closed instead of guessing a division table.
        std::int32_t ArenaEntryFee = 0;
        bool HasArenaEntryFee = false;
        if (Player->contains("arenaEntryFee"))
        {
            const auto& FeeValue = (*Player)["arenaEntryFee"];
            std::int64_t ParsedFee = 0;
            if (FeeValue.is_number_unsigned())
            {
                const auto Value = FeeValue.get<std::uint64_t>();
                if (Value != 0)
                    return false;
            }
            else if (FeeValue.is_number_integer())
            {
                ParsedFee = FeeValue.get<std::int64_t>();
                if (ParsedFee > 0 ||
                    ParsedFee < -static_cast<std::int64_t>(
                        ArenaTelemetryPolicy::kMaximumPointsDelta))
                {
                    return false;
                }
            }
            else
            {
                return false;
            }
            ArenaEntryFee = static_cast<std::int32_t>(ParsedFee);
            HasArenaEntryFee = true;
        }

        int Division = 0;
        for (const auto& Token : (*Player)["tokens"])
        {
            if (!Token.is_string())
                continue;
            if (ArenaTelemetryPolicy::ParseArenaDivisionToken(
                    Token.get_ref<const std::string&>().c_str(),
                    FortniteVersion,
                    &Division))
            {
                break;
            }
        }
        ArenaTelemetryPolicy::FTournamentIdentity Identity{};
        if (Division <= 0 ||
            !ArenaTelemetryPolicy::BuildArenaTournamentIdentity(
                FortniteVersion, Division, &Identity))
        {
            return false;
        }

        if (OutIdentity)
            *OutIdentity = Identity;
        if (OutSavedHype)
            *OutSavedHype = SavedHype;
        if (OutArenaEntryFee)
            *OutArenaEntryFee = ArenaEntryFee;
        if (OutHasArenaEntryFee)
            *OutHasArenaEntryFee = HasArenaEntryFee;
        return true;
    }

}
