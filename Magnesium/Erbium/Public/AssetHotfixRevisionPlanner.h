#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

// Pure-text transaction planner for accepted DefaultGame.ini revisions.
//
// This component has no scheduler, reflection, or UObject dependency. Callers
// first feed the exact accepted DefaultGame payload through Unreal normally.
// For a DeltaNative plan they may then temporarily install DeltaIni, invoke one
// native PatchAssetsFromIniFiles pass, and install RestoreIni. RestoreIni is a
// synthetic AssetHotfix section containing every active directive from the
// accepted revision; unrelated DefaultGame sections are never replayed.
//
// BaselineRestorations maps the canonical identity returned by
// TryGetDirectiveIdentity to one exact, previously accepted RowUpdate line.
// The line is parsed again and must resolve to the same identity. Missing or
// ambiguous restoration input is reported and omitted; the delta still clears
// the active arrays and applies known changes, captured restorations, and
// retention rows without inventing a native value.
namespace AssetHotfixRevisionPlanner
{
	using FBaselineRestorationMap =
		std::map<std::string, std::string>;
	// Proof that a current directive's row can be resolved by this runtime.
	// This is intentionally independent from native baseline ownership: entries
	// may guide forced-reference retention, but must never restore removed rows.
	using FKnownResolvableIdentitySet = std::set<std::string>;

	enum class ERevisionMode : std::uint8_t
	{
		ByteIdentical,
		NoAssetChange,
		FullNative,
		RecoveryDelta,
		DeltaNative
	};

	struct FRevisionPlan
	{
		ERevisionMode Mode = ERevisionMode::FullNative;
		std::uint64_t Token = 0;
		std::string DeltaIni;
		std::string RestoreIni;
		std::size_t AddedOrChanged = 0;
		std::size_t Restored = 0;
		std::size_t RetentionReapplied = 0;
		std::size_t MissingBaselines = 0;
		std::vector<std::string> MissingBaselineIdentities;
		// Exact current directives whose canonical identities were absent from
		// the committed revision. With no committed revision, this contains all
		// current directives. The planner makes no claim that capture is pristine.
		std::vector<std::string> BaselineCaptureCandidates;
		// Exact current CurveTable directives which may be checked after a
		// successful native application. The integration commits only identities
		// that a bounded resident-only probe proves resolvable.
		std::vector<std::string> ResolvableProbeCandidates;
		bool AcceptedRevisionSafe = false;
		bool BaselineRestorationsSafe = true;
	};

	// Parses one complete +CurveTable/+DataTable RowUpdate line and returns its
	// canonical identity. This is the only identity construction callers should
	// use when populating FBaselineRestorationMap.
	bool TryGetDirectiveIdentity(
		const std::string& ExactDirectiveLine,
		std::string& OutIdentity) noexcept;

	// Serialized caller only. The byte-identical fast path compares against the
	// last committed exact payload and performs no parsing or delta construction.
	// ForceFull is intended for the first observed pass and lifecycle transitions.
	bool PrepareRevision(
		const std::string& ExactAcceptedDefaultGame,
		const FBaselineRestorationMap& BaselineRestorations,
		const FKnownResolvableIdentitySet& KnownResolvableIdentities,
		bool ForceFull,
		FRevisionPlan& OutPlan) noexcept;

	// Commit only after the selected native operation returned and, for a delta,
	// RestoreIni was accepted. Abort leaves the committed active set unchanged.
	void CommitRevision(std::uint64_t Token) noexcept;
	void AbortRevision(std::uint64_t Token) noexcept;
}
