#include "../Erbium/Private/ArenaTelemetryWire.h"
#include "../Erbium/Private/ArenaTelemetryPolicy.h"

#include <cassert>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

int main()
{
    wchar_t ArenaBootstrap[96]{};
    assert(ArenaTelemetryWire::FormatArenaPresentationBootstrapV1(
        1, true, 2080, -40,
        ArenaBootstrap,
        sizeof(ArenaBootstrap) / sizeof(wchar_t)));
    assert(std::wstring(ArenaBootstrap) ==
        L"ATLAS_ARENA_BOOTSTRAP_V1:0000000000000001:A:1:0000002080:-000040");
    assert(std::wstring(ArenaBootstrap).size() ==
        ArenaTelemetryWire::kArenaBootstrapMessageLength);
    assert(ArenaTelemetryWire::FormatArenaPresentationBootstrapV1(
        0xABCDEF, false, 0, 0,
        ArenaBootstrap,
        sizeof(ArenaBootstrap) / sizeof(wchar_t)));
    assert(std::wstring(ArenaBootstrap) ==
        L"ATLAS_ARENA_BOOTSTRAP_V1:0000000000ABCDEF:A:0:0000000000:+000000");
    assert(!ArenaTelemetryWire::FormatArenaPresentationBootstrapV1(
        0, true, 100, -10,
        ArenaBootstrap,
        sizeof(ArenaBootstrap) / sizeof(wchar_t)));
    assert(ArenaBootstrap[0] == L'\0');
    assert(!ArenaTelemetryWire::FormatArenaPresentationBootstrapV1(
        1, true, -1, -10,
        ArenaBootstrap,
        sizeof(ArenaBootstrap) / sizeof(wchar_t)));
    assert(!ArenaTelemetryWire::FormatArenaPresentationBootstrapV1(
        1, true, 100, 1,
        ArenaBootstrap,
        sizeof(ArenaBootstrap) / sizeof(wchar_t)));
    wchar_t ShortArenaBootstrap[32]{};
    assert(!ArenaTelemetryWire::FormatArenaPresentationBootstrapV1(
        1, true, 100, -10,
        ShortArenaBootstrap,
        sizeof(ShortArenaBootstrap) / sizeof(wchar_t)));

    using EArenaEvent =
        ArenaTelemetryWire::EArenaPresentationEventKind;
    wchar_t ArenaEvent[128]{};
    assert(ArenaTelemetryWire::FormatArenaPresentationEventV1(
        1, 2, EArenaEvent::Elimination,
        20, 2100, 0, true,
        ArenaEvent,
        sizeof(ArenaEvent) / sizeof(wchar_t)));
    assert(std::wstring(ArenaEvent) ==
        L"ATLAS_ARENA_EVENT_V1:0000000000000001:0000000000000002:E:+000020:0000002100:000:1");
    assert(std::wstring(ArenaEvent).size() ==
        ArenaTelemetryWire::kArenaEventMessageLength);
    assert(ArenaTelemetryWire::FormatArenaPresentationEventV1(
        1, 3, EArenaEvent::BusFare,
        -40, 2060, 0, true,
        ArenaEvent,
        sizeof(ArenaEvent) / sizeof(wchar_t)));
    assert(std::wstring(ArenaEvent).find(L":B:-000040:0000002060:000:1") !=
        std::wstring::npos);
    assert(ArenaTelemetryWire::FormatArenaPresentationEventV1(
        1, 4, EArenaEvent::BusFare,
        0, 0, 0, true,
        ArenaEvent,
        sizeof(ArenaEvent) / sizeof(wchar_t)));
    assert(std::wstring(ArenaEvent).find(L":B:+000000:0000000000:000:1") !=
        std::wstring::npos);
    assert(ArenaTelemetryWire::FormatArenaPresentationEventV1(
        1, 5, EArenaEvent::Placement,
        60, 2120, 25, false,
        ArenaEvent,
        sizeof(ArenaEvent) / sizeof(wchar_t)));
    assert(ArenaTelemetryWire::FormatArenaPresentationEventV1(
        1, 6, EArenaEvent::Victory,
        60, 2180, 1, true,
        ArenaEvent,
        sizeof(ArenaEvent) / sizeof(wchar_t)));
    assert(!ArenaTelemetryWire::FormatArenaPresentationEventV1(
        1, 6, EArenaEvent::Elimination,
        20, 2200, 1, true,
        ArenaEvent,
        sizeof(ArenaEvent) / sizeof(wchar_t)));
    assert(ArenaEvent[0] == L'\0');
    assert(!ArenaTelemetryWire::FormatArenaPresentationEventV1(
        1, 6, EArenaEvent::BusFare,
        20, 2200, 0, true,
        ArenaEvent,
        sizeof(ArenaEvent) / sizeof(wchar_t)));
    assert(!ArenaTelemetryWire::FormatArenaPresentationEventV1(
        1, 6, EArenaEvent::Placement,
        60, 2200, 1, true,
        ArenaEvent,
        sizeof(ArenaEvent) / sizeof(wchar_t)));
    assert(!ArenaTelemetryWire::FormatArenaPresentationEventV1(
        1, 6, EArenaEvent::Victory,
        60, 2200, 2, true,
        ArenaEvent,
        sizeof(ArenaEvent) / sizeof(wchar_t)));

    wchar_t ArenaEnd[64]{};
    assert(ArenaTelemetryWire::FormatArenaPresentationEndV1(
        0x12AB, ArenaEnd,
        sizeof(ArenaEnd) / sizeof(wchar_t)));
    assert(std::wstring(ArenaEnd) ==
        L"ATLAS_ARENA_END_V1:00000000000012AB");
    assert(std::wstring(ArenaEnd).size() ==
        ArenaTelemetryWire::kArenaEndMessageLength);
    assert(!ArenaTelemetryWire::FormatArenaPresentationEndV1(
        0, ArenaEnd,
        sizeof(ArenaEnd) / sizeof(wchar_t)));
    assert(ArenaEnd[0] == L'\0');
    wchar_t ShortArenaEnd[32]{};
    assert(!ArenaTelemetryWire::FormatArenaPresentationEndV1(
        1, ShortArenaEnd,
        sizeof(ShortArenaEnd) / sizeof(wchar_t)));
    assert(ShortArenaEnd[0] == L'\0');

    wchar_t PlacementBridge[64]{};
    assert(ArenaTelemetryWire::FormatPlacementPresentationBridgeV2(
        101, 5, 120, false,
        PlacementBridge, sizeof(PlacementBridge) / sizeof(wchar_t)));
    assert(std::wstring(PlacementBridge) ==
        L"ATLAS_ARENA_PLACEMENT_V2:101:005:000120:0");
    assert(ArenaTelemetryWire::FormatPlacementPresentationBridgeV2(
        5, 1, 60, true,
        PlacementBridge, sizeof(PlacementBridge) / sizeof(wchar_t)));
    assert(std::wstring(PlacementBridge) ==
        L"ATLAS_ARENA_PLACEMENT_V2:005:001:000060:1");

    // Reject malformed/non-descending transitions before they can reach the
    // client's hidden transport parser. A failed format also clears a reused
    // output buffer so stale valid markers cannot accidentally be sent.
    assert(!ArenaTelemetryWire::FormatPlacementPresentationBridgeV2(
        5, 5, 30, false,
        PlacementBridge, sizeof(PlacementBridge) / sizeof(wchar_t)));
    assert(PlacementBridge[0] == L'\0');
    assert(!ArenaTelemetryWire::FormatPlacementPresentationBridgeV2(
        5, 6, 30, false,
        PlacementBridge, sizeof(PlacementBridge) / sizeof(wchar_t)));
    assert(!ArenaTelemetryWire::FormatPlacementPresentationBridgeV2(
        102, 5, 30, false,
        PlacementBridge, sizeof(PlacementBridge) / sizeof(wchar_t)));
    assert(!ArenaTelemetryWire::FormatPlacementPresentationBridgeV2(
        6, 5, 0, false,
        PlacementBridge, sizeof(PlacementBridge) / sizeof(wchar_t)));
    wchar_t ShortPlacementBridge[16]{};
    assert(!ArenaTelemetryWire::FormatPlacementPresentationBridgeV2(
        6, 5, 30, false,
        ShortPlacementBridge,
        sizeof(ShortPlacementBridge) / sizeof(wchar_t)));
    assert(ShortPlacementBridge[0] == L'\0');

    assert(ArenaTelemetryWire::FormatFortniteVersion(1.72) == "1.72");
    assert(ArenaTelemetryWire::FormatFortniteVersion(8.20) == "8.20");
    assert(ArenaTelemetryWire::FormatFortniteVersion(20.40) == "20.40");
    assert(ArenaTelemetryWire::FormatFortniteVersion(30.00) == "30.00");

    const auto Envelope = ArenaTelemetryWire::BuildSessionEnvelope(
        "server-test",
        "match-test",
        8.20,
        "/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Solo.Playlist_ShowdownAlt_Solo",
        1);

    assert(Envelope["fortniteVersion"].is_string());
    assert(Envelope["fortniteVersion"] == "8.20");
    assert(Envelope["events"].is_array());
    assert(Envelope["events"].empty());

    const std::string Serialized = Envelope.dump();
    assert(Serialized.find(
        "\"fortniteVersion\":\"8.20\"") != std::string::npos);
    assert(Serialized.find("\"events\":[]") != std::string::npos);

    const std::vector<std::string> BatchIds = {
        "server-test:match-test:1",
        "server-test:match-test:2"
    };
    assert(ArenaTelemetryWire::AcknowledgesEveryEvent(
        R"({"acceptedEventIds":["server-test:match-test:1"],"duplicateEventIds":["server-test:match-test:2"],"rejectedEvents":[]})",
        BatchIds));
    assert(!ArenaTelemetryWire::AcknowledgesEveryEvent(
        R"({"acceptedEventIds":["server-test:match-test:1"],"duplicateEventIds":[],"rejectedEvents":[{"id":"server-test:match-test:2"}]})",
        BatchIds));
    assert(!ArenaTelemetryWire::AcknowledgesEveryEvent(
        R"({"acceptedEventIds":["server-test:match-test:1"],"discardedEventIds":["server-test:match-test:2"]})",
        BatchIds));

    assert(ArenaTelemetryPolicy::GetPlacementPoints(25) == 60);
    assert(ArenaTelemetryPolicy::GetPlacementPoints(15) == 30);
    assert(ArenaTelemetryPolicy::GetPlacementPoints(5) == 30);
    assert(ArenaTelemetryPolicy::GetPlacementPoints(1) == 60);
    assert(ArenaTelemetryPolicy::GetPlacementPoints(50) == 0);
    assert(ArenaTelemetryPolicy::GetPlacementPoints(2) == 0);
    assert(ArenaTelemetryPolicy::GetPlacementPoints(4) == 0);

    // Bootstrap roster changes are not deaths and cannot arm or award Arena
    // placement. A legitimate four-player live match is different: its first
    // finalized death crosses the 25/15/5 authored tiers, and the winner later
    // crosses the 1-player tier.
    ArenaTelemetryPolicy::FPlacementProgressionState SmallPlacement{};
    assert(ArenaTelemetryPolicy::ConsumeCrossedPlacementPoints(
        4, 3, false, &SmallPlacement) == 0);
    assert(!SmallPlacement.LiveProgressionStarted);
    assert(ArenaTelemetryPolicy::ConsumeCrossedPlacementPoints(
        4, 3, true, &SmallPlacement) == 120);
    assert(SmallPlacement.LiveProgressionStarted);
    assert(ArenaTelemetryPolicy::ConsumeCrossedPlacementPoints(
        3, 2, true, &SmallPlacement) == 0);
    assert(ArenaTelemetryPolicy::ConsumeCrossedPlacementPoints(
        2, 1, true, &SmallPlacement) == 60);

    // A large population jump may cross several authored milestones in one
    // finalized update. Every tier is consumed once, rather than requiring an
    // exact observed population that can be skipped by multi-removal frames.
    ArenaTelemetryPolicy::FPlacementProgressionState CrossedPlacement{};
    assert(ArenaTelemetryPolicy::ConsumeCrossedPlacementPoints(
        100, 26, true, &CrossedPlacement) == 0);
    assert(ArenaTelemetryPolicy::ConsumeCrossedPlacementPoints(
        26, 14, true, &CrossedPlacement) == 90);
    assert(ArenaTelemetryPolicy::ConsumeCrossedPlacementPoints(
        14, 4, true, &CrossedPlacement) == 30);
    assert(ArenaTelemetryPolicy::ConsumeCrossedPlacementPoints(
        4, 1, true, &CrossedPlacement) == 60);

    // Duplicate callbacks and repopulation never replay a consumed tier.
    assert(ArenaTelemetryPolicy::ConsumeCrossedPlacementPoints(
        4, 1, true, &CrossedPlacement) == 0);
    assert(ArenaTelemetryPolicy::ConsumeCrossedPlacementPoints(
        1, 5, true, &CrossedPlacement) == 0);
    assert(ArenaTelemetryPolicy::ConsumeCrossedPlacementPoints(
        5, 4, true, &CrossedPlacement) == 0);

    ArenaTelemetryPolicy::FPlacementProgressionState InvalidPlacement{};
    assert(ArenaTelemetryPolicy::ConsumeCrossedPlacementPoints(
        0, 0, true, &InvalidPlacement) == 0);
    assert(ArenaTelemetryPolicy::ConsumeCrossedPlacementPoints(
        3, 3, true, &InvalidPlacement) == 0);
    assert(ArenaTelemetryPolicy::ConsumeCrossedPlacementPoints(
        3, -1, true, &InvalidPlacement) == 0);
    assert(ArenaTelemetryPolicy::ConsumeCrossedPlacementPoints(
        3, 2, true, nullptr) == 0);

    // A pawn weak-object identity is a life identity. The first finalized,
    // live elimination wins; duplicate death callbacks for that life are
    // rejected before either persistence or reflected presentation.
    std::unordered_set<std::uint64_t> SeenVictimLives;
    assert(!ArenaTelemetryPolicy::ConsumeFinalizedVictimLife(
        false, true, 0x100000001ull, &SeenVictimLives));
    assert(!ArenaTelemetryPolicy::ConsumeFinalizedVictimLife(
        true, false, 0x100000001ull, &SeenVictimLives));
    assert(!ArenaTelemetryPolicy::ConsumeFinalizedVictimLife(
        true, true, 0, &SeenVictimLives));
    assert(ArenaTelemetryPolicy::ConsumeFinalizedVictimLife(
        true, true, 0x100000001ull, &SeenVictimLives));
    assert(!ArenaTelemetryPolicy::ConsumeFinalizedVictimLife(
        true, true, 0x100000001ull, &SeenVictimLives));
    assert(ArenaTelemetryPolicy::ConsumeFinalizedVictimLife(
        true, true, 0x100000002ull, &SeenVictimLives));

    using EModifierPresence =
        ArenaTelemetryPolicy::ETournamentModifierPresence;
    using EModifierAction =
        ArenaTelemetryPolicy::ETournamentModifierAction;

    // Pre-ProcessEvent registration failures may retry a bounded number of
    // times. Only that fully unambiguous failure state enables the reflected
    // presentation owner.
    ArenaTelemetryPolicy::FTournamentModifierActivationState FailedModifier{};
    for (int Attempt = 0; Attempt < 3; ++Attempt)
    {
        assert(ArenaTelemetryPolicy::ObserveTournamentModifier(
            EModifierPresence::Missing, &FailedModifier) ==
            EModifierAction::RequestRegistration);
        ArenaTelemetryPolicy::CompleteTournamentModifierRegistrationDispatch(
            false, &FailedModifier);
    }
    assert(FailedModifier.RegistrationUnavailable);
    assert(ArenaTelemetryPolicy::ShouldUseSyntheticTournamentPresentation(
        FailedModifier));
    assert(ArenaTelemetryPolicy::ObserveTournamentModifier(
        EModifierPresence::Missing, &FailedModifier) ==
        EModifierAction::None);

    // Once ProcessEvent begins, its one-way result is ambiguous: never issue a
    // duplicate registration and never activate the reflected fallback. A
    // later active-list observation can positively confirm native ownership.
    ArenaTelemetryPolicy::FTournamentModifierActivationState DispatchedModifier{};
    assert(ArenaTelemetryPolicy::ObserveTournamentModifier(
        EModifierPresence::Missing, &DispatchedModifier) ==
        EModifierAction::RequestRegistration);
    ArenaTelemetryPolicy::CompleteTournamentModifierRegistrationDispatch(
        true, &DispatchedModifier);
    assert(DispatchedModifier.RegistrationDispatched);
    assert(!ArenaTelemetryPolicy::ShouldUseSyntheticTournamentPresentation(
        DispatchedModifier));
    assert(ArenaTelemetryPolicy::ObserveTournamentModifier(
        EModifierPresence::Missing, &DispatchedModifier) ==
        EModifierAction::None);
    assert(ArenaTelemetryPolicy::ObserveTournamentModifier(
        EModifierPresence::Found, &DispatchedModifier) ==
        EModifierAction::None);
    assert(DispatchedModifier.NativePresentationConfirmed);

    ArenaTelemetryPolicy::FTournamentModifierActivationState UnknownModifier{};
    assert(ArenaTelemetryPolicy::ObserveTournamentModifier(
        EModifierPresence::Unavailable, &UnknownModifier) ==
        EModifierAction::None);
    assert(!ArenaTelemetryPolicy::ShouldUseSyntheticTournamentPresentation(
        UnknownModifier));

    using ERuntimeEffect =
        ArenaTelemetryPolicy::ETournamentRuntimeEffect;
    // Pausing Save Arena Hype must affect only durable progression. Native or
    // reflected scoring presentation remains a normal part of this match.
    assert(ArenaTelemetryPolicy::IsTournamentRuntimeEffectEnabled(
        ERuntimeEffect::SessionLocalPresentation, true));
    assert(ArenaTelemetryPolicy::IsTournamentRuntimeEffectEnabled(
        ERuntimeEffect::SessionLocalPresentation, false));
    assert(ArenaTelemetryPolicy::IsTournamentRuntimeEffectEnabled(
        ERuntimeEffect::SavedProgression, true));
    assert(!ArenaTelemetryPolicy::IsTournamentRuntimeEffectEnabled(
        ERuntimeEffect::SavedProgression, false));
    assert(ArenaTelemetryPolicy::ResolveSessionLocalStartingHype(
        true, 2080) == 2080);
    assert(ArenaTelemetryPolicy::ResolveSessionLocalStartingHype(
        false, 2080) == 0);
    assert(ArenaTelemetryPolicy::ResolveSessionLocalStartingHype(
        true, -1) == 0);
    assert(ArenaTelemetryPolicy::ApplySessionLocalHypeDelta(
        2080, -40) == 2040);
    assert(ArenaTelemetryPolicy::ApplySessionLocalHypeDelta(
        10, -40) == 0);
    assert(ArenaTelemetryPolicy::ApplySessionLocalHypeDelta(
        0, 20) == 20);
    assert(ArenaTelemetryPolicy::ApplySessionLocalHypeDelta(
        2147483640, 20) == 2147483647);

    using EEntryFeeStageAction =
        ArenaTelemetryPolicy::EArenaEntryFeeStageAction;
    assert(!ArenaTelemetryPolicy::CanAttemptArenaMatchEntryNotification(
        false, false));
    assert(ArenaTelemetryPolicy::CanAttemptArenaMatchEntryNotification(
        true, false));
    assert(!ArenaTelemetryPolicy::CanAttemptArenaMatchEntryNotification(
        true, true));
    // The client must always see the pre-fare bootstrap before a one-shot
    // fare mutation, and no positive award may overtake that stage.
    assert(ArenaTelemetryPolicy::ResolveArenaEntryFeeStageAction(
        false, true, false, -40, true) == EEntryFeeStageAction::Wait);
    assert(ArenaTelemetryPolicy::ResolveArenaEntryFeeStageAction(
        true, false, false, -40, true) == EEntryFeeStageAction::Wait);
    assert(ArenaTelemetryPolicy::ResolveArenaEntryFeeStageAction(
        true, true, false, -40, true) == EEntryFeeStageAction::Dispatch);
    assert(ArenaTelemetryPolicy::ResolveArenaEntryFeeStageAction(
        true, true, false, -40, false) == EEntryFeeStageAction::Wait);
    assert(ArenaTelemetryPolicy::ResolveArenaEntryFeeStageAction(
        true, true, false, 0, false) ==
        EEntryFeeStageAction::Wait);
    assert(ArenaTelemetryPolicy::ResolveArenaEntryFeeStageAction(
        true, true, false, 0, true) ==
        EEntryFeeStageAction::Dispatch);
    assert(ArenaTelemetryPolicy::ResolveArenaEntryFeeStageAction(
        true, true, true, -40, true) == EEntryFeeStageAction::Wait);
    assert(ArenaTelemetryPolicy::ResolveArenaEntryFeeStageAction(
        true, true, false, 40, true) == EEntryFeeStageAction::Wait);
    assert(!ArenaTelemetryPolicy::CanFlushArenaPresentationAwards(
        false, false));
    assert(!ArenaTelemetryPolicy::CanFlushArenaPresentationAwards(
        true, false));
    assert(ArenaTelemetryPolicy::CanFlushArenaPresentationAwards(
        true, true));

    using EEndAction =
        ArenaTelemetryPolicy::EArenaPresentationEndAction;
    assert(ArenaTelemetryPolicy::ResolveArenaPresentationEndAction(
        false, true, true, true, 0) == EEndAction::Send);
    assert(ArenaTelemetryPolicy::ResolveArenaPresentationEndAction(
        false, false, true, true, 0) == EEndAction::Wait);
    assert(ArenaTelemetryPolicy::ResolveArenaPresentationEndAction(
        false, true, false, true, 0) == EEndAction::Wait);
    assert(ArenaTelemetryPolicy::ResolveArenaPresentationEndAction(
        false, true, true, false, 0) == EEndAction::Wait);
    assert(ArenaTelemetryPolicy::ResolveArenaPresentationEndAction(
        false, true, true, true, 1) == EEndAction::Wait);
    // A successful END is terminal even if transient readiness later changes.
    assert(ArenaTelemetryPolicy::ResolveArenaPresentationEndAction(
        true, false, false, false, 5) == EEndAction::Complete);

    // Exact regression for the observed 27.11 drift: bootstrap 2,840, apply
    // fare once, then the authored elimination/placement/victory awards.
    int OrderedHype = 2840;
    OrderedHype = ArenaTelemetryPolicy::ApplySessionLocalHypeDelta(
        OrderedHype, -40);
    assert(OrderedHype == 2800);
    OrderedHype = ArenaTelemetryPolicy::ApplySessionLocalHypeDelta(
        OrderedHype, 20);
    OrderedHype = ArenaTelemetryPolicy::ApplySessionLocalHypeDelta(
        OrderedHype, 120);
    for (int Elimination = 0; Elimination < 4; ++Elimination)
    {
        OrderedHype = ArenaTelemetryPolicy::ApplySessionLocalHypeDelta(
            OrderedHype, 20);
    }
    OrderedHype = ArenaTelemetryPolicy::ApplySessionLocalHypeDelta(
        OrderedHype, 60);
    assert(OrderedHype == 3080);

    // Saving-off sessions bootstrap at zero and release one score-neutral
    // fare control frame at the aircraft boundary before match-local awards.
    int EphemeralHype =
        ArenaTelemetryPolicy::ResolveSessionLocalStartingHype(false, 2840);
    assert(EphemeralHype == 0);
    assert(ArenaTelemetryPolicy::ResolveArenaEntryFeeStageAction(
        true, true, false, 0, false) ==
        EEntryFeeStageAction::Wait);
    assert(ArenaTelemetryPolicy::ResolveArenaEntryFeeStageAction(
        true, true, false, 0, true) ==
        EEntryFeeStageAction::Dispatch);
    EphemeralHype = ArenaTelemetryPolicy::ApplySessionLocalHypeDelta(
        EphemeralHype, 20);
    assert(EphemeralHype == 20);
    assert(ArenaTelemetryPolicy::ShouldStageSavedProgression(
        0, false));
    assert(ArenaTelemetryPolicy::ShouldStageSavedProgression(
        1, true));
    assert(!ArenaTelemetryPolicy::ShouldStageSavedProgression(
        1, false));

    assert(ArenaTelemetryPolicy::ShouldCreditTeamElimination(3, 3));
    assert(!ArenaTelemetryPolicy::ShouldCreditTeamElimination(3, 4));
    assert(!ArenaTelemetryPolicy::ShouldCreditTeamElimination(-1, -1));
    assert(!ArenaTelemetryPolicy::ShouldCreditTeamElimination(0, 0));
    assert(!ArenaTelemetryPolicy::ShouldCreditTeamElimination(2, 2));
    assert(!ArenaTelemetryPolicy::ShouldCreditTeamElimination(250, 250));
    assert(!ArenaTelemetryPolicy::ShouldCreditTeamElimination(255, 255));
    assert(ArenaTelemetryPolicy::IsValidPointsDelta(15000));
    assert(ArenaTelemetryPolicy::IsValidPointsDelta(-15000));
    assert(ArenaTelemetryPolicy::IsValidPointsDelta(1000000));
    assert(!ArenaTelemetryPolicy::IsValidPointsDelta(1000001));

    const char* CanonicalPath = nullptr;
    assert(ArenaTelemetryPolicy::ResolveCanonicalPlaylist(
        "Playlist_ShowdownAlt_Solo", 8.20, &CanonicalPath));
    assert(std::string(CanonicalPath) ==
        "/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Solo.Playlist_ShowdownAlt_Solo");
    assert(!ArenaTelemetryPolicy::ResolveCanonicalPlaylist(
        "Playlist_ShowdownAlt_Solo", 8.19));
    assert(ArenaTelemetryPolicy::ResolveCanonicalPlaylist(
        "Playlist_ShowdownAlt_NoBuildBR_Duos", 20.00));
    assert(ArenaTelemetryPolicy::ResolveCanonicalPlaylist(
        "Playlist_ShowdownAlt_NoBuildBR_Squads", 30.40));
    assert(!ArenaTelemetryPolicy::ResolveCanonicalPlaylist(
        "Playlist_ShowdownAlt_NoBuildBR_Duos", 19.40));
    assert(!ArenaTelemetryPolicy::ResolveCanonicalPlaylist(
        "Playlist_ShowdownAlt_NoBuildBR_Duos", 31.00));
    assert(!ArenaTelemetryPolicy::ResolveCanonicalPlaylist(
        "Playlist_DefaultSolo", 20.40));

    ArenaTelemetryPolicy::FTournamentIdentity Tournament{};
    assert(ArenaTelemetryPolicy::BuildArenaTournamentIdentity(
        17.30, 2, &Tournament));
    assert(std::string(Tournament.EventId) == "epicgames_Arena_S17_Solo");
    assert(std::string(Tournament.EventWindowId) ==
        "Arena_S17_Division2_Solo");
    assert(std::string(Tournament.EventGroupId) == "Arena_S17");
    assert(std::string(Tournament.EventSubGroupId).empty());
    assert(!ArenaTelemetryPolicy::BuildArenaTournamentIdentity(
        8.19, 1, &Tournament));
    assert(!ArenaTelemetryPolicy::BuildArenaTournamentIdentity(
        17.30, 11, &Tournament));

    int Division = 0;
    assert(ArenaTelemetryPolicy::ParseArenaDivisionToken(
        "ARENA_S17_Division2", 17.30, &Division));
    assert(Division == 2);
    assert(!ArenaTelemetryPolicy::ParseArenaDivisionToken(
        "ARENA_S16_Division2", 17.30, &Division));
    assert(!ArenaTelemetryPolicy::ParseArenaDivisionToken(
        "ARENA_S17_DivisionsClimbed2", 17.30, &Division));

    ArenaTelemetryPolicy::FTournamentIdentity LiveResponseIdentity{};
    std::int32_t LiveSavedHype = 0;
    std::int32_t LiveEntryFee = 0;
    bool LiveEntryFeeKnown = false;
    assert(ArenaTelemetryWire::ResolveArenaTournamentIdentityFromPlayerResponse(
        R"({"player":{"persistentScores":{"Hype":920},"arenaEntryFee":-10,"tokens":["ARENA_S17_Division2","ARENA_S17_DivisionsClimbed2"]}})",
        17.30,
        &LiveResponseIdentity,
        &LiveSavedHype,
        &LiveEntryFee,
        &LiveEntryFeeKnown));
    assert(LiveSavedHype == 920);
    assert(LiveEntryFee == -10);
    assert(LiveEntryFeeKnown);
    assert(std::string(LiveResponseIdentity.EventId) ==
        "epicgames_Arena_S17_Solo");
    assert(std::string(LiveResponseIdentity.EventWindowId) ==
        "Arena_S17_Division2_Solo");
    ArenaTelemetryPolicy::FTournamentIdentity PlayerEndpointIdentity{};
    std::int32_t PlayerEndpointSavedHype = 0;
    std::int32_t PlayerEndpointEntryFee = 0;
    bool PlayerEndpointEntryFeeKnown = false;
    assert(ArenaTelemetryWire::ResolveArenaTournamentIdentityFromPlayerResponse(
        R"({"persistentScores":{"Hype":920},"arenaEntryFee":-10,"tokens":["ARENA_S17_Division2","ARENA_S17_DivisionsClimbed2"]})",
        17.30,
        &PlayerEndpointIdentity,
        &PlayerEndpointSavedHype,
        &PlayerEndpointEntryFee,
        &PlayerEndpointEntryFeeKnown));
    assert(PlayerEndpointSavedHype == 920);
    assert(PlayerEndpointEntryFee == -10);
    assert(PlayerEndpointEntryFeeKnown);
    assert(std::string(PlayerEndpointIdentity.EventId) ==
        "epicgames_Arena_S17_Solo");
    assert(std::string(PlayerEndpointIdentity.EventWindowId) ==
        "Arena_S17_Division2_Solo");

    std::int32_t ZeroSavedHype = -1;
    std::int32_t ZeroEntryFee = 1;
    bool ZeroEntryFeeKnown = false;
    assert(ArenaTelemetryWire::ResolveArenaTournamentIdentityFromPlayerResponse(
        R"({"persistentScores":{"Hype":0},"arenaEntryFee":0,"tokens":["ARENA_S17_Division1"]})",
        17.30,
        nullptr,
        &ZeroSavedHype,
        &ZeroEntryFee,
        &ZeroEntryFeeKnown));
    assert(ZeroSavedHype == 0);
    assert(ZeroEntryFee == 0);
    assert(ZeroEntryFeeKnown);

    // Compatibility with a pre-contract backend remains intact, but callers
    // can distinguish an absent fee from an authored zero-fee division.
    std::int32_t MissingEntryFee = 123;
    bool MissingEntryFeeKnown = true;
    assert(ArenaTelemetryWire::ResolveArenaTournamentIdentityFromPlayerResponse(
        R"({"persistentScores":{"Hype":0},"tokens":["ARENA_S17_Division1"]})",
        17.30,
        nullptr,
        nullptr,
        &MissingEntryFee,
        &MissingEntryFeeKnown));
    assert(MissingEntryFee == 0);
    assert(!MissingEntryFeeKnown);

    assert(!ArenaTelemetryWire::ResolveArenaTournamentIdentityFromPlayerResponse(
        R"({"player":{"tokens":["ARENA_S16_Division2"]},"tokens":["ARENA_S17_Division2"]})",
        17.30));

    std::int32_t MaximumSavedHype = 0;
    assert(ArenaTelemetryWire::ResolveArenaTournamentIdentityFromPlayerResponse(
        R"({"persistentScores":{"Hype":2147483647},"tokens":["ARENA_S17_Division10"]})",
        17.30,
        nullptr,
        &MaximumSavedHype));
    assert(MaximumSavedHype == 2147483647);

    const auto RejectSavedHype = [](const char* Response)
    {
        ArenaTelemetryPolicy::FTournamentIdentity Identity{};
        std::int32_t SavedHype = 123;
        const bool Resolved = ArenaTelemetryWire::
            ResolveArenaTournamentIdentityFromPlayerResponse(
                Response, 17.30, &Identity, &SavedHype);
        assert(!Resolved);
        assert(SavedHype == 0);
        assert(std::string(Identity.EventId).empty());
    };
    RejectSavedHype(
        R"({"tokens":["ARENA_S17_Division2"]})");
    RejectSavedHype(
        R"({"persistentScores":{},"tokens":["ARENA_S17_Division2"]})");
    RejectSavedHype(
        R"({"persistentScores":{"Hype":"920"},"tokens":["ARENA_S17_Division2"]})");
    RejectSavedHype(
        R"({"persistentScores":{"Hype":920.0},"tokens":["ARENA_S17_Division2"]})");
    RejectSavedHype(
        R"({"persistentScores":{"Hype":-1},"tokens":["ARENA_S17_Division2"]})");
    RejectSavedHype(
        R"({"persistentScores":{"Hype":2147483648},"tokens":["ARENA_S17_Division2"]})");
    RejectSavedHype(
        R"({"player":{"persistentScores":{"Hype":-1},"tokens":["ARENA_S17_Division2"]},"persistentScores":{"Hype":920},"tokens":["ARENA_S17_Division2"]})");

    const auto RejectEntryFee = [](const char* Response)
    {
        ArenaTelemetryPolicy::FTournamentIdentity Identity{};
        std::int32_t SavedHype = 123;
        std::int32_t EntryFee = 123;
        bool HasEntryFee = true;
        const bool Resolved = ArenaTelemetryWire::
            ResolveArenaTournamentIdentityFromPlayerResponse(
                Response, 17.30, &Identity, &SavedHype,
                &EntryFee, &HasEntryFee);
        assert(!Resolved);
        assert(SavedHype == 0);
        assert(EntryFee == 0);
        assert(!HasEntryFee);
        assert(std::string(Identity.EventId).empty());
    };
    RejectEntryFee(
        R"({"persistentScores":{"Hype":920},"arenaEntryFee":"-10","tokens":["ARENA_S17_Division2"]})");
    RejectEntryFee(
        R"({"persistentScores":{"Hype":920},"arenaEntryFee":-10.0,"tokens":["ARENA_S17_Division2"]})");
    RejectEntryFee(
        R"({"persistentScores":{"Hype":920},"arenaEntryFee":10,"tokens":["ARENA_S17_Division2"]})");
    RejectEntryFee(
        R"({"persistentScores":{"Hype":920},"arenaEntryFee":-1000001,"tokens":["ARENA_S17_Division2"]})");

    assert(ArenaTelemetryPolicy::ResolveTournamentStatSchema(
        0, nullptr, 0, 0, nullptr, 0) ==
        ArenaTelemetryPolicy::ETournamentStatSchema::
            LegacyZeroParameter);

    const ArenaTelemetryPolicy::FTournamentStatParameterSchema
        TournamentStatParameter[] = {
            { "StatInfo", 0, 0x20, 0x80 },
        };
    const ArenaTelemetryPolicy::FTournamentStatParameterSchema
        CompactTournamentStatParameter[] = {
            { "TournamentStatInfo", 0, 0x18, 0x80 },
        };
    const ArenaTelemetryPolicy::FTournamentStatFieldSchema
        LegacyFullNameStatFields[] = {
            { "StatName", 0x0, 0x10 },
            { "StatDisplayName", 0x10, 0x8 },
            { "StatValue", 0x18, 0x4 },
        };
    assert(ArenaTelemetryPolicy::ResolveTournamentStatSchema(
        0x20, TournamentStatParameter, 1, 0x20,
        LegacyFullNameStatFields, 3) ==
        ArenaTelemetryPolicy::ETournamentStatSchema::LegacyFullName);

    const ArenaTelemetryPolicy::FTournamentStatFieldSchema
        LegacyCompactNameStatFields[] = {
            { "StatName", 0x0, 0x10 },
            { "StatDisplayName", 0x10, 0x4 },
            { "StatValue", 0x18, 0x4 },
        };
    assert(ArenaTelemetryPolicy::ResolveTournamentStatSchema(
        0x20, TournamentStatParameter, 1, 0x20,
        LegacyCompactNameStatFields, 3) ==
        ArenaTelemetryPolicy::ETournamentStatSchema::LegacyCompactName);
    // The installed 21.00 client has no bIsDeltaCount field. Its native exec
    // thunk allocates a 0x18-byte payload and reads the absolute StatValue
    // immediately after the four-byte FName at 0x14. Runtime layout, not the
    // release number, selects this compact ABI.
    const ArenaTelemetryPolicy::FTournamentStatFieldSchema
        CompactNoDeltaTournamentStatFields[] = {
            { "StatName", 0x0, 0x10 },
            { "StatDisplayName", 0x10, 0x4 },
            { "StatValue", 0x14, 0x4 },
        };
    assert(ArenaTelemetryPolicy::ResolveTournamentStatSchema(
        0x18, CompactTournamentStatParameter, 1, 0x18,
        CompactNoDeltaTournamentStatFields, 3) ==
        ArenaTelemetryPolicy::ETournamentStatSchema::
            CompactNameWithoutDelta);
    assert(ArenaTelemetryPolicy::TournamentStatParameterSize(
        ArenaTelemetryPolicy::ETournamentStatSchema::
            CompactNameWithoutDelta) == 0x18);
    assert(ArenaTelemetryPolicy::TournamentStatValueOffset(
        ArenaTelemetryPolicy::ETournamentStatSchema::
            CompactNameWithoutDelta) == 0x14);

    const ArenaTelemetryPolicy::FTournamentStatFieldSchema
        ModernTournamentStatFields[] = {
            { "StatName", 0x0, 0x10 },
            { "StatDisplayName", 0x10, 0x4 },
            { "StatValue", 0x18, 0x4 },
            { "bIsDeltaCount", 0x14, 0x1 },
        };
    assert(ArenaTelemetryPolicy::ResolveTournamentStatSchema(
        0x20, TournamentStatParameter, 1, 0x20,
        ModernTournamentStatFields, 4) ==
        ArenaTelemetryPolicy::ETournamentStatSchema::
            ModernCompactNameWithDelta);
    assert(ArenaTelemetryPolicy::TournamentStatParameterSize(
        ArenaTelemetryPolicy::ETournamentStatSchema::
            ModernCompactNameWithDelta) == 0x20);
    assert(ArenaTelemetryPolicy::TournamentStatValueOffset(
        ArenaTelemetryPolicy::ETournamentStatSchema::
            ModernCompactNameWithDelta) == 0x18);
    // Runtime reflection, rather than a guessed release boundary, selects
    // the exact layout when bIsDeltaCount appears in 22.40+ clients.

    ArenaTelemetryPolicy::FTournamentStatFieldSchema
        WrongModernTournamentStatFields[] = {
            { "StatName", 0x0, 0x10 },
            { "StatDisplayName", 0x10, 0x4 },
            { "StatValue", 0x18, 0x4 },
            { "bIsDeltaCount", 0x15, 0x1 },
        };
    assert(ArenaTelemetryPolicy::ResolveTournamentStatSchema(
        0x20, TournamentStatParameter, 1, 0x20,
        WrongModernTournamentStatFields, 4) ==
        ArenaTelemetryPolicy::ETournamentStatSchema::Unsupported);
    const ArenaTelemetryPolicy::FTournamentStatParameterSchema
        OutTournamentStatParameter[] = {
            { "StatInfo", 0, 0x20, 0x80 | 0x100 },
        };
    assert(ArenaTelemetryPolicy::ResolveTournamentStatSchema(
        0x20, OutTournamentStatParameter, 1, 0x20,
        ModernTournamentStatFields, 4) ==
        ArenaTelemetryPolicy::ETournamentStatSchema::Unsupported);
    // Unreal reflects a non-const input reference as Parm|OutParm|
    // ReferenceParm. Accept that exact input-reference contract while still
    // rejecting a plain output parameter above.
    const ArenaTelemetryPolicy::FTournamentStatParameterSchema
        ReferenceTournamentStatParameter[] = {
            { "StatInfo", 0, 0x20, 0x80 | 0x100 | 0x8000000 },
        };
    assert(ArenaTelemetryPolicy::ResolveTournamentStatSchema(
        0x20, ReferenceTournamentStatParameter, 1, 0x20,
        ModernTournamentStatFields, 4) ==
        ArenaTelemetryPolicy::ETournamentStatSchema::
            ModernCompactNameWithDelta);
    assert(ArenaTelemetryPolicy::ResolveTournamentStatSchema(
        0x20, TournamentStatParameter, 1, 0x20,
        ModernTournamentStatFields, 4) ==
        ArenaTelemetryPolicy::ETournamentStatSchema::
            ModernCompactNameWithDelta);
    assert(ArenaTelemetryPolicy::ResolveTournamentStatSchema(
        0x24, TournamentStatParameter, 1, 0x20,
        ModernTournamentStatFields, 4) ==
        ArenaTelemetryPolicy::ETournamentStatSchema::Unsupported);
    assert(ArenaTelemetryPolicy::ResolveTournamentStatSchema(
        0x20, TournamentStatParameter, 1, 0x20,
        CompactNoDeltaTournamentStatFields, 3) ==
        ArenaTelemetryPolicy::ETournamentStatSchema::Unsupported);
    assert(ArenaTelemetryPolicy::ResolveTournamentStatSchema(
        0x18, CompactTournamentStatParameter, 1, 0x20,
        CompactNoDeltaTournamentStatFields, 3) ==
        ArenaTelemetryPolicy::ETournamentStatSchema::Unsupported);
    assert(ArenaTelemetryPolicy::ResolveTournamentStatSchema(
        0x18, CompactTournamentStatParameter, 1, 0x18,
        LegacyCompactNameStatFields, 3) ==
        ArenaTelemetryPolicy::ETournamentStatSchema::Unsupported);

    assert(ArenaTelemetryPolicy::ResolveTeamEliminationVisualValue(
        false, 0, 3) == 3);
    assert(ArenaTelemetryPolicy::ResolveTeamEliminationVisualValue(
        true, 7, 3) == 7);
    assert(ArenaTelemetryPolicy::ResolveTeamEliminationVisualValue(
        true, 0, 3) == 3);
    const auto EliminationVisual =
        ArenaTelemetryPolicy::BuildTeamEliminationVisual(7);
    assert(std::wstring(EliminationVisual.StatName) ==
        L"TEAM_ELIMS_STAT_INDEX");
    assert(std::wstring(EliminationVisual.DisplayName) ==
        L"Eliminations");
    assert(EliminationVisual.AbsoluteValue == 7);
    assert(EliminationVisual.DeltaValue == 1);
    assert(EliminationVisual.PreferDelta);
    const auto LegacyEliminationPayload =
        ArenaTelemetryPolicy::ResolveTournamentStatPayload(
            ArenaTelemetryPolicy::ETournamentStatSchema::
                LegacyFullName,
            EliminationVisual);
    assert(LegacyEliminationPayload.Value == 7);
    assert(!LegacyEliminationPayload.IsDelta);
    const auto CompactEliminationPayload =
        ArenaTelemetryPolicy::ResolveTournamentStatPayload(
            ArenaTelemetryPolicy::ETournamentStatSchema::
                CompactNameWithoutDelta,
            EliminationVisual);
    assert(CompactEliminationPayload.Value == 7);
    assert(!CompactEliminationPayload.IsDelta);
    const auto ModernEliminationPayload =
        ArenaTelemetryPolicy::ResolveTournamentStatPayload(
            ArenaTelemetryPolicy::ETournamentStatSchema::
                ModernCompactNameWithDelta,
            EliminationVisual);
    assert(ModernEliminationPayload.Value == 1);
    assert(ModernEliminationPayload.IsDelta);

    const auto PlacementVisual =
        ArenaTelemetryPolicy::BuildPlacementVisual(5);
    assert(std::wstring(PlacementVisual.StatName) ==
        L"PLACEMENT_STAT_INDEX");
    assert(std::wstring(PlacementVisual.DisplayName) ==
        L"Placement");
    assert(PlacementVisual.AbsoluteValue == 5);
    assert(PlacementVisual.DeltaValue == 0);
    assert(!PlacementVisual.PreferDelta);
    const auto LegacyPlacementPayload =
        ArenaTelemetryPolicy::ResolveTournamentStatPayload(
            ArenaTelemetryPolicy::ETournamentStatSchema::
                LegacyFullName,
            PlacementVisual);
    assert(LegacyPlacementPayload.Value == 5);
    assert(!LegacyPlacementPayload.IsDelta);
    const auto CompactPlacementPayload =
        ArenaTelemetryPolicy::ResolveTournamentStatPayload(
            ArenaTelemetryPolicy::ETournamentStatSchema::
                CompactNameWithoutDelta,
            PlacementVisual);
    assert(CompactPlacementPayload.Value == 5);
    assert(!CompactPlacementPayload.IsDelta);
    const auto ModernPlacementPayload =
        ArenaTelemetryPolicy::ResolveTournamentStatPayload(
            ArenaTelemetryPolicy::ETournamentStatSchema::
                ModernCompactNameWithDelta,
            PlacementVisual);
    assert(ModernPlacementPayload.Value == 5);
    assert(!ModernPlacementPayload.IsDelta);
    assert(ArenaTelemetryPolicy::BuildPlacementVisual(-1).
        AbsoluteValue == 0);

    const auto EntryFeeVisual =
        ArenaTelemetryPolicy::BuildMatchEntryFeeVisual();
    assert(std::wstring(EntryFeeVisual.StatName) ==
        L"MATCH_PLAYED_STAT");
    assert(std::wstring(EntryFeeVisual.DisplayName) ==
        L"MatchEntryFee");
    const auto ModernEntryFeePayload =
        ArenaTelemetryPolicy::ResolveTournamentStatPayload(
            ArenaTelemetryPolicy::ETournamentStatSchema::
                ModernCompactNameWithDelta,
            EntryFeeVisual);
    assert(ModernEntryFeePayload.Value == 1);
    assert(ModernEntryFeePayload.IsDelta);

    const ArenaTelemetryPolicy::FTournamentStatParameterSchema
        PlacementParameters[] = {
            { "PointsEarned", 4, 4, 0x80 },
            { "Placement", 0, 4, 0x80 },
        };
    assert(ArenaTelemetryPolicy::ResolveTournamentPlacementPointsSchema(
        8, PlacementParameters, 2));
    const ArenaTelemetryPolicy::FTournamentStatParameterSchema
        UnsafePlacementParameters[] = {
            { "Placement", 0, 4, 0x80 },
            { "PointsEarned", 4, 4, 0x80 | 0x100 },
        };
    assert(!ArenaTelemetryPolicy::ResolveTournamentPlacementPointsSchema(
        8, UnsafePlacementParameters, 2));
    assert(!ArenaTelemetryPolicy::ResolveTournamentPlacementPointsSchema(
        12, PlacementParameters, 2));

    using EVisualOutcome =
        ArenaTelemetryPolicy::ETournamentVisualDispatchOutcome;
    // Canonical Arena keeps its single native elimination presentation owner.
    assert(!ArenaTelemetryPolicy::
        ShouldDispatchDeathPipelineEliminationVisual(
            true, false));
    assert(!ArenaTelemetryPolicy::
        ShouldDispatchDeathPipelineEliminationVisual(
            true, true));
    // Preserve the pre-existing reflected visual for standalone tournament
    // playlists, which do not participate in persistent canonical Arena.
    assert(ArenaTelemetryPolicy::
        ShouldDispatchDeathPipelineEliminationVisual(
            false, true));
    assert(!ArenaTelemetryPolicy::
        ShouldDispatchDeathPipelineEliminationVisual(
            false, false));
    assert(!ArenaTelemetryPolicy::ShouldFallbackEliminationPresentation(
        true, EVisualOutcome::Sent));
    assert(ArenaTelemetryPolicy::ShouldFallbackEliminationPresentation(
        true, EVisualOutcome::SchemaUnsupported));
    assert(!ArenaTelemetryPolicy::ShouldFallbackEliminationPresentation(
        true, EVisualOutcome::ProcessEventFailed));
    // Tournament-mode points are not persisted Arena Hype and must not be
    // mirrored through the Arena-specific +20 placement fallback.
    assert(!ArenaTelemetryPolicy::ShouldFallbackEliminationPresentation(
        false, EVisualOutcome::SchemaUnsupported));
    assert(!ArenaTelemetryPolicy::ShouldFallbackEliminationPresentation(
        false, EVisualOutcome::ProcessEventFailed));
    // The dedicated placement-points RPC is primary. A successful send must
    // never also dispatch the typed placement stat.
    assert(!ArenaTelemetryPolicy::ShouldFallbackPlacementToTypedStat(
        EVisualOutcome::Sent));
    // Only reflection/schema rejection before ProcessEvent permits the typed
    // PLACEMENT_STAT_INDEX fallback.
    assert(ArenaTelemetryPolicy::ShouldFallbackPlacementToTypedStat(
        EVisualOutcome::SchemaUnsupported));
    // A ProcessEvent failure is ambiguous for a one-way RPC and therefore
    // fails closed instead of risking a duplicate presentation.
    assert(!ArenaTelemetryPolicy::ShouldFallbackPlacementToTypedStat(
        EVisualOutcome::ProcessEventFailed));

    const ArenaTelemetryPolicy::FMatchEntryParameterSchema LegacySchema[] = {
        { "EventId", 0, 16, 0x80 },
        { "EventWindowId", 16, 16, 0x80 },
    };
    assert(ArenaTelemetryPolicy::IsLegacyMatchEntrySchema(
        32, 16, LegacySchema, 2));
    assert(ArenaTelemetryPolicy::ResolveMatchEntrySchema(
        32, 16, LegacySchema, 2) ==
        ArenaTelemetryPolicy::EMatchEntrySchema::EventAndWindow);
    const ArenaTelemetryPolicy::FMatchEntryParameterSchema GroupSchema[] = {
        { "EventId", 0, 16, 0x80 },
        { "EventWindowId", 16, 16, 0x80 },
        { "EventGroupId", 32, 16, 0x80 },
    };
    assert(ArenaTelemetryPolicy::ResolveMatchEntrySchema(
        48, 16, GroupSchema, 3) ==
        ArenaTelemetryPolicy::EMatchEntrySchema::EventWindowAndGroup);
    const ArenaTelemetryPolicy::FMatchEntryParameterSchema ShuffledGroupSchema[] = {
        { "EventGroupId", 32, 16, 0x80 },
        { "EventId", 0, 16, 0x80 },
        { "EventWindowId", 16, 16, 0x80 },
    };
    assert(ArenaTelemetryPolicy::ResolveMatchEntrySchema(
        48, 16, ShuffledGroupSchema, 3) ==
        ArenaTelemetryPolicy::EMatchEntrySchema::EventWindowAndGroup);
    const ArenaTelemetryPolicy::FMatchEntryParameterSchema ModernSchema[] = {
        { "EventIds", 0, 64, 0x80 },
    };
    assert(ArenaTelemetryPolicy::ResolveMatchEntrySchema(
        64, 16, ModernSchema, 1) ==
        ArenaTelemetryPolicy::EMatchEntrySchema::TournamentIds);
    assert(!ArenaTelemetryPolicy::IsLegacyMatchEntrySchema(
        64, 16, ModernSchema, 1));
    const ArenaTelemetryPolicy::FMatchEntryParameterSchema OldSingleSchema[] = {
        { "EventIds", 0, 16, 0x80 },
    };
    // 8.20-8.51's one-FString ABI cannot encode both event and division
    // window. It stays deliberately fail-closed until a documented wire
    // serialization exists; guessing here would corrupt client state.
    assert(ArenaTelemetryPolicy::ResolveMatchEntrySchema(
        16, 16, OldSingleSchema, 1) ==
        ArenaTelemetryPolicy::EMatchEntrySchema::Unsupported);
    const ArenaTelemetryPolicy::FMatchEntryParameterSchema WindowOnlySchema[] = {
        { "EventWindowId", 0, 16, 0x80 },
    };
    // 9.41's reflected ABI is unambiguous: its one FString is the selected
    // event window, so it can be supported without inventing an EventIds
    // serialization or branching on a release number.
    assert(ArenaTelemetryPolicy::ResolveMatchEntrySchema(
        16, 16, WindowOnlySchema, 1) ==
        ArenaTelemetryPolicy::EMatchEntrySchema::EventWindowOnly);

    const ArenaTelemetryPolicy::FMatchEntryParameterSchema ReturnSchema[] = {
        { "EventIds", 0, 64, 0x80 | 0x400 },
    };
    assert(ArenaTelemetryPolicy::ResolveMatchEntrySchema(
        64, 16, ReturnSchema, 1) ==
        ArenaTelemetryPolicy::EMatchEntrySchema::Unsupported);

    assert(!ArenaTelemetryPolicy::CanResolveTournamentLookup(1, 0, true));
    assert(!ArenaTelemetryPolicy::CanResolveTournamentLookup(0, 1, true));
    assert(!ArenaTelemetryPolicy::CanResolveTournamentLookup(0, 0, false));
    assert(ArenaTelemetryPolicy::CanResolveTournamentLookup(0, 0, true));

    bool ReadinessObserved = false;
    std::uint64_t ReadinessSinceMs = 0;
    assert(!ArenaTelemetryPolicy::AdvanceMatchEntryReadiness(
        false, 100, &ReadinessObserved, &ReadinessSinceMs));
    assert(!ReadinessObserved);
    assert(ReadinessSinceMs == 0);
    assert(!ArenaTelemetryPolicy::AdvanceMatchEntryReadiness(
        true, 200, &ReadinessObserved, &ReadinessSinceMs));
    assert(ReadinessObserved);
    assert(ReadinessSinceMs == 200);
    assert(!ArenaTelemetryPolicy::AdvanceMatchEntryReadiness(
        true, 1199, &ReadinessObserved, &ReadinessSinceMs));
    assert(ArenaTelemetryPolicy::AdvanceMatchEntryReadiness(
        true, 1200, &ReadinessObserved, &ReadinessSinceMs));
    assert(!ArenaTelemetryPolicy::AdvanceMatchEntryReadiness(
        false, 1201, &ReadinessObserved, &ReadinessSinceMs));
    assert(!ReadinessObserved);
    assert(ReadinessSinceMs == 0);
    assert(ArenaTelemetryPolicy::AdvanceMatchEntryReadiness(
        true, 1300, &ReadinessObserved, &ReadinessSinceMs, 0));
    assert(!ArenaTelemetryPolicy::AdvanceMatchEntryReadiness(
        true, 1299, &ReadinessObserved, &ReadinessSinceMs));
    assert(ReadinessSinceMs == 1299);
    assert(!ArenaTelemetryPolicy::AdvanceMatchEntryReadiness(
        true, 2300, nullptr, &ReadinessSinceMs));
    assert(!ArenaTelemetryPolicy::AdvanceMatchEntryReadiness(
        true, 2300, &ReadinessObserved, nullptr));

    // Readiness may stabilize on spawn island without authorizing the RPC.
    // Once the aircraft trigger arrives, the already-ready native route can
    // dispatch inside the bridge's bounded ownership window.
    bool PrewarmedReadinessObserved = false;
    std::uint64_t PrewarmedReadinessSinceMs = 0;
    assert(!ArenaTelemetryPolicy::AdvanceMatchEntryReadiness(
        true, 4000, &PrewarmedReadinessObserved,
        &PrewarmedReadinessSinceMs));
    assert(!ArenaTelemetryPolicy::CanAttemptArenaMatchEntryNotification(
        false, false));
    assert(ArenaTelemetryPolicy::AdvanceMatchEntryReadiness(
        true, 5000, &PrewarmedReadinessObserved,
        &PrewarmedReadinessSinceMs));
    assert(!ArenaTelemetryPolicy::CanAttemptArenaMatchEntryNotification(
        false, false));
    assert(ArenaTelemetryPolicy::CanAttemptArenaMatchEntryNotification(
        true, false));

    using EPresentationOwnership = ArenaTelemetryPolicy::
        EMatchEntryPresentationOwnership;
    assert(!ArenaTelemetryPolicy::HasInFlightMatchEntryCandidate(
        false, false));
    assert(ArenaTelemetryPolicy::HasInFlightMatchEntryCandidate(
        true, false));
    assert(ArenaTelemetryPolicy::HasInFlightMatchEntryCandidate(
        false, true));
    assert(ArenaTelemetryPolicy::HasInFlightMatchEntryCandidate(
        true, true));
    static_assert(
        ArenaTelemetryPolicy::kMatchEntryPostTriggerCandidateWaitMs == 250);
    // A real post-trigger native candidate gets only the bounded 250 ms
    // opportunity to finish. The exact deadline transfers ownership.
    assert(ArenaTelemetryPolicy::ResolveMatchEntryPresentationOwnership(
        true, false, false, false, true, 1249, 1000) ==
        EPresentationOwnership::AwaitNative);
    assert(ArenaTelemetryPolicy::ResolveMatchEntryPresentationOwnership(
        true, false, false, false, true, 1250, 1000) ==
        EPresentationOwnership::DirectBridge);
    // Eligibility alone is not evidence that a native RPC is resolving.
    assert(ArenaTelemetryPolicy::ResolveMatchEntryPresentationOwnership(
        true, false, false, false, false, 1000, 1000, 250) ==
        EPresentationOwnership::DirectBridge);
    // A notification that already won remains authoritative even when no
    // lookup/cached candidate is left.
    assert(ArenaTelemetryPolicy::ResolveMatchEntryPresentationOwnership(
        true, true, false, false, false, 1250, 1000, 250) ==
        EPresentationOwnership::Native);
    assert(ArenaTelemetryPolicy::ResolveMatchEntryPresentationOwnership(
        true, true, true, false, false, 1250, 1000, 250) ==
        EPresentationOwnership::Native);
    assert(ArenaTelemetryPolicy::ResolveMatchEntryPresentationOwnership(
        false, false, false, false, true, 1000, 1000, 250) ==
        EPresentationOwnership::DirectBridge);
    assert(ArenaTelemetryPolicy::ResolveMatchEntryPresentationOwnership(
        true, false, true, false, true, 1000, 1000, 250) ==
        EPresentationOwnership::DirectBridge);
    assert(ArenaTelemetryPolicy::ResolveMatchEntryPresentationOwnership(
        true, false, false, true, true, 1000, 1000, 250) ==
        EPresentationOwnership::DirectBridge);
    // Missing start and a regressed clock fail closed while a real candidate
    // remains in flight; the live trigger always supplies a non-zero start.
    assert(ArenaTelemetryPolicy::ResolveMatchEntryPresentationOwnership(
        true, false, false, false, true, 1000, 0, 250) ==
        EPresentationOwnership::AwaitNative);
    assert(ArenaTelemetryPolicy::ResolveMatchEntryPresentationOwnership(
        true, false, false, false, true, 999, 1000, 250) ==
        EPresentationOwnership::AwaitNative);

    assert(ArenaTelemetryPolicy::CanUsePrivateArenaPresentationChannel(
        true, true, true));
    assert(!ArenaTelemetryPolicy::CanUsePrivateArenaPresentationChannel(
        false, true, true));
    assert(!ArenaTelemetryPolicy::CanUsePrivateArenaPresentationChannel(
        true, false, false));
    assert(!ArenaTelemetryPolicy::CanUsePrivateArenaPresentationChannel(
        true, true, false));

    assert(ArenaTelemetryPolicy::CanNotifyMatchEntryForPlaylist(
        "/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Solo.Playlist_ShowdownAlt_Solo"));
    assert(!ArenaTelemetryPolicy::CanNotifyMatchEntryForPlaylist(
        "/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Duos.Playlist_ShowdownAlt_Duos"));

    return 0;
}
