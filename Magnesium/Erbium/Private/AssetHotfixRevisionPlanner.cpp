#include "pch.h"

#include "../Public/AssetHotfixRevisionPlanner.h"

#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

namespace AssetHotfixRevisionPlanner
{
namespace
{
	constexpr std::size_t MaximumPayloadBytes = 8 * 1024 * 1024;
	constexpr std::size_t MaximumTokenBytes = 64 * 1024;
	constexpr std::size_t MaximumDirectives = 64 * 1024;

	enum class EDirectiveKind : std::uint8_t
	{
		CurveTable,
		DataTable
	};

	struct FDirective
	{
		EDirectiveKind Kind = EDirectiveKind::CurveTable;
		std::string Identity;
		std::string Path;
		std::string Row;
		std::string FieldOrTime;
		std::string Value;
		std::string ExactLine;
	};

	struct FParsedRevision
	{
		bool Safe = true;
		std::vector<FDirective> OrderedDirectives;
		std::map<std::string, std::size_t> IdentityToIndex;
	};

	struct FPendingRevision
	{
		bool Ready = false;
		std::uint64_t Token = 0;
		std::string ExactPayload;
		FParsedRevision Parsed;
	};

	bool GHasCommittedRevision = false;
	bool GCommittedRevisionSafe = false;
	std::uint64_t GNextRevisionToken = 1;
	std::string GCommittedExactPayload;
	FParsedRevision GCommittedParsed;
	FPendingRevision GPendingRevision;

	std::string_view TrimAscii(std::string_view Value) noexcept
	{
		while (!Value.empty() &&
			(Value.front() == ' ' || Value.front() == '\t'))
		{
			Value.remove_prefix(1);
		}
		while (!Value.empty() &&
			(Value.back() == ' ' || Value.back() == '\t' ||
			 Value.back() == '\r'))
		{
			Value.remove_suffix(1);
		}
		return Value;
	}

	char LowerAscii(char Value) noexcept
	{
		return Value >= 'A' && Value <= 'Z'
			? static_cast<char>(Value - 'A' + 'a')
			: Value;
	}

	bool EqualsAsciiInsensitive(
		std::string_view Left,
		std::string_view Right) noexcept
	{
		if (Left.size() != Right.size())
			return false;
		for (std::size_t Index = 0; Index < Left.size(); ++Index)
		{
			if (LowerAscii(Left[Index]) != LowerAscii(Right[Index]))
				return false;
		}
		return true;
	}

	bool StartsWithAsciiInsensitive(
		std::string_view Value,
		std::string_view Prefix) noexcept
	{
		return Value.size() >= Prefix.size() &&
			EqualsAsciiInsensitive(
				Value.substr(0, Prefix.size()), Prefix);
	}

	std::string NormalizeName(std::string_view Value)
	{
		std::string Result;
		Result.reserve(Value.size());
		for (const char Character : Value)
			Result.push_back(LowerAscii(Character));
		return Result;
	}

	bool IsSafeToken(
		std::string_view Value,
		bool AllowEmpty) noexcept
	{
		if ((!AllowEmpty && Value.empty()) ||
			Value.size() > MaximumTokenBytes)
		{
			return false;
		}
		for (const unsigned char Character : Value)
		{
			if (Character == 0 || Character == ';' ||
				Character == '\r' || Character == '\n')
			{
				return false;
			}
		}
		return true;
	}

	bool ContainsLineBreakOrNull(std::string_view Value) noexcept
	{
		for (const char Character : Value)
		{
			if (Character == '\0' || Character == '\r' ||
				Character == '\n')
			{
				return true;
			}
		}
		return false;
	}

	bool SplitFive(
		std::string_view Value,
		std::string_view (&Tokens)[5]) noexcept
	{
		std::size_t Begin = 0;
		for (std::size_t Token = 0; Token < 4; ++Token)
		{
			const std::size_t Delimiter = Value.find(';', Begin);
			if (Delimiter == std::string_view::npos)
				return false;
			Tokens[Token] = Value.substr(Begin, Delimiter - Begin);
			Begin = Delimiter + 1;
		}
		if (Value.find(';', Begin) != std::string_view::npos)
			return false;
		Tokens[4] = Value.substr(Begin);
		return true;
	}

	bool MakeCurveTimeIdentity(
		std::string_view Token,
		std::string& Out) noexcept
	{
		Out.clear();
		if (Token.empty() || Token.size() > 128)
			return false;
		float Time = 0.0f;
		const char* const Begin = Token.data();
		const char* const End = Begin + Token.size();
		const auto Parsed = std::from_chars(Begin, End, Time);
		if (Parsed.ec != std::errc{} || Parsed.ptr != End ||
			!std::isfinite(Time))
		{
			return false;
		}
		if (Time == 0.0f)
			Time = 0.0f;
		const std::uint32_t Bits =
			std::bit_cast<std::uint32_t>(Time);
		char Buffer[16]{};
		const auto Encoded = std::to_chars(
			Buffer, Buffer + sizeof(Buffer), Bits, 16);
		if (Encoded.ec != std::errc{})
			return false;
		try
		{
			Out.assign(Buffer, Encoded.ptr);
			return true;
		}
		catch (...)
		{
			Out.clear();
			return false;
		}
	}

	bool BuildIdentity(FDirective& Directive) noexcept
	{
		try
		{
			std::string FieldIdentity;
			if (Directive.Kind == EDirectiveKind::CurveTable)
			{
				if (!MakeCurveTimeIdentity(
						Directive.FieldOrTime, FieldIdentity))
				{
					return false;
				}
			}
			else
			{
				FieldIdentity = NormalizeName(Directive.FieldOrTime);
			}

			Directive.Identity.clear();
			Directive.Identity.reserve(
				Directive.Path.size() + Directive.Row.size() +
				FieldIdentity.size() + 16);
			Directive.Identity.push_back(
				Directive.Kind == EDirectiveKind::CurveTable ? 'c' : 'd');
			Directive.Identity.push_back('\x1f');
			Directive.Identity += NormalizeName(Directive.Path);
			Directive.Identity.push_back('\x1f');
			Directive.Identity += NormalizeName(Directive.Row);
			Directive.Identity.push_back('\x1f');
			Directive.Identity += FieldIdentity;
			return !Directive.Identity.empty();
		}
		catch (...)
		{
			Directive.Identity.clear();
			return false;
		}
	}

	bool ParseDirective(
		std::string_view ParseLine,
		std::string_view ExactLine,
		FDirective& Out) noexcept
	{
		constexpr std::string_view CurvePrefix = "+CurveTable=";
		constexpr std::string_view DataPrefix = "+DataTable=";
		ParseLine = TrimAscii(ParseLine);
		std::string_view Body;
		if (StartsWithAsciiInsensitive(ParseLine, CurvePrefix))
		{
			Out.Kind = EDirectiveKind::CurveTable;
			Body = ParseLine.substr(CurvePrefix.size());
		}
		else if (StartsWithAsciiInsensitive(ParseLine, DataPrefix))
		{
			Out.Kind = EDirectiveKind::DataTable;
			Body = ParseLine.substr(DataPrefix.size());
		}
		else
		{
			return false;
		}

		std::string_view Tokens[5]{};
		if (!SplitFive(Body, Tokens))
			return false;
		for (std::size_t Index = 0; Index < 4; ++Index)
			Tokens[Index] = TrimAscii(Tokens[Index]);
		if (!EqualsAsciiInsensitive(Tokens[1], "RowUpdate") ||
			!IsSafeToken(Tokens[0], false) ||
			!IsSafeToken(Tokens[2], false) ||
			!IsSafeToken(Tokens[3], false) ||
			!IsSafeToken(Tokens[4], false) ||
			ExactLine.empty() || ExactLine.size() > MaximumTokenBytes ||
			ContainsLineBreakOrNull(ExactLine))
		{
			return false;
		}

		try
		{
			Out.Path.assign(Tokens[0]);
			Out.Row.assign(Tokens[2]);
			Out.FieldOrTime.assign(Tokens[3]);
			Out.Value.assign(Tokens[4]);
			Out.ExactLine.assign(ExactLine);
			return BuildIdentity(Out);
		}
		catch (...)
		{
			Out = {};
			return false;
		}
	}

	FParsedRevision ParseRevision(const std::string& Payload)
	{
		FParsedRevision Result;
		if (Payload.size() > MaximumPayloadBytes)
		{
			Result.Safe = false;
			return Result;
		}

		bool InAssetSection = false;
		bool SawAssetSection = false;
		std::size_t Begin = 0;
		while (Begin <= Payload.size())
		{
			const std::size_t Newline = Payload.find('\n', Begin);
			const std::size_t End = Newline == std::string::npos
				? Payload.size() : Newline;
			std::string_view ExactLine(Payload.data() + Begin, End - Begin);
			if (!ExactLine.empty() && ExactLine.back() == '\r')
				ExactLine.remove_suffix(1);
			std::string_view Line = TrimAscii(ExactLine);
			if (Begin == 0 && Line.size() >= 3 &&
				static_cast<unsigned char>(Line[0]) == 0xef &&
				static_cast<unsigned char>(Line[1]) == 0xbb &&
				static_cast<unsigned char>(Line[2]) == 0xbf)
			{
				Line.remove_prefix(3);
			}

			if (!Line.empty() && Line.front() == '[' &&
				Line.back() == ']')
			{
				const bool AssetHeader = EqualsAsciiInsensitive(
					TrimAscii(Line.substr(1, Line.size() - 2)),
					"AssetHotfix");
				if (AssetHeader && SawAssetSection)
					Result.Safe = false;
				if (AssetHeader)
					SawAssetSection = true;
				InAssetSection = AssetHeader;
			}
			else if (InAssetSection && !Line.empty() &&
				Line.front() != ';' && Line.front() != '#')
			{
				FDirective Directive;
				if (Result.OrderedDirectives.size() >= MaximumDirectives ||
					!ParseDirective(Line, ExactLine, Directive) ||
					Result.IdentityToIndex.contains(Directive.Identity))
				{
					Result.Safe = false;
				}
				else
				{
					const std::size_t Index =
						Result.OrderedDirectives.size();
					Result.IdentityToIndex.emplace(
						Directive.Identity, Index);
					Result.OrderedDirectives.push_back(
						std::move(Directive));
				}
			}

			if (Newline == std::string::npos)
				break;
			Begin = Newline + 1;
		}
		// Preserve every individually valid, non-ambiguous directive even when an
		// unsupported sibling makes the complete revision unsafe. Safe still gates
		// delta construction; retaining identities only allows callers to taint
		// their pre-mutation ownership and recognize them on a later safe revision.
		return Result;
	}

	std::string BeginSyntheticAssetSection()
	{
		return
			"[AssetHotfix]\r\n"
			"!CurveTable=ClearArray\r\n"
			"!DataTable=ClearArray\r\n"
			"!CurveFloat=ClearArray\r\n";
	}

	bool TryAppendExactDirective(
		std::string& Destination,
		const std::string& ExactLine)
	{
		if (ExactLine.size() > MaximumPayloadBytes ||
			Destination.size() >
				MaximumPayloadBytes - ExactLine.size() ||
			Destination.size() + ExactLine.size() >
				MaximumPayloadBytes - 2)
		{
			return false;
		}
		Destination += ExactLine;
		Destination += "\r\n";
		return true;
	}

	bool DirectivesEquivalent(
		const FDirective& Left,
		const FDirective& Right) noexcept
	{
		return Left.Kind == Right.Kind &&
			Left.Identity == Right.Identity &&
			Left.Path == Right.Path && Left.Row == Right.Row &&
			Left.FieldOrTime == Right.FieldOrTime &&
			Left.Value == Right.Value &&
			Left.ExactLine == Right.ExactLine;
	}

	std::string MakeAssetIdentity(const FDirective& Directive)
	{
		std::string Result;
		Result.reserve(Directive.Path.size() + 2);
		Result.push_back(
			Directive.Kind == EDirectiveKind::CurveTable ? 'c' : 'd');
		Result.push_back('\x1f');
		Result += NormalizeName(Directive.Path);
		return Result;
	}

	bool RevisionsEquivalent(
		const FParsedRevision& Left,
		const FParsedRevision& Right) noexcept
	{
		if (Left.OrderedDirectives.size() !=
			Right.OrderedDirectives.size())
		{
			return false;
		}
		for (const FDirective& Directive : Left.OrderedDirectives)
		{
			const auto OtherIndex =
				Right.IdentityToIndex.find(Directive.Identity);
			if (OtherIndex == Right.IdentityToIndex.end() ||
				!DirectivesEquivalent(
					Directive,
					Right.OrderedDirectives[OtherIndex->second]))
			{
				return false;
			}
		}
		return true;
	}

	std::uint64_t AllocateRevisionToken() noexcept
	{
		std::uint64_t Token = GNextRevisionToken++;
		if (!Token)
			Token = GNextRevisionToken++;
		return Token;
	}

	bool TryParseBaseline(
		const std::string& ExpectedIdentity,
		const std::string& ExactLine,
		FDirective& Out) noexcept
	{
		if (ContainsLineBreakOrNull(ExactLine) ||
			!ParseDirective(ExactLine, ExactLine, Out))
		{
			return false;
		}
		return Out.Identity == ExpectedIdentity;
	}
}

	bool TryGetDirectiveIdentity(
		const std::string& ExactDirectiveLine,
		std::string& OutIdentity) noexcept
	{
		OutIdentity.clear();
		try
		{
			FDirective Directive;
			if (!ParseDirective(
					ExactDirectiveLine,
					ExactDirectiveLine,
					Directive))
			{
				return false;
			}
			OutIdentity = std::move(Directive.Identity);
			return !OutIdentity.empty();
		}
		catch (...)
		{
			OutIdentity.clear();
			return false;
		}
	}

	bool PrepareRevision(
		const std::string& ExactAcceptedDefaultGame,
		const FBaselineRestorationMap& BaselineRestorations,
		const FKnownResolvableIdentitySet& KnownResolvableIdentities,
		bool ForceFull,
		FRevisionPlan& OutPlan) noexcept
	{
		OutPlan = {};
		if (!ForceFull && GHasCommittedRevision &&
			ExactAcceptedDefaultGame == GCommittedExactPayload)
		{
			GPendingRevision = {};
			OutPlan.Mode = ERevisionMode::ByteIdentical;
			OutPlan.AcceptedRevisionSafe = GCommittedRevisionSafe;
			return true;
		}

		GPendingRevision = {};
		try
		{
			FParsedRevision Current =
				ParseRevision(ExactAcceptedDefaultGame);
			OutPlan.AcceptedRevisionSafe = Current.Safe;
			const bool bRecoveryReplayEligible = Current.Safe &&
				GHasCommittedRevision &&
				(ForceFull || !GCommittedRevisionSafe);
			for (const FDirective& Directive :
				Current.OrderedDirectives)
			{
				if (Current.Safe &&
					Directive.Kind == EDirectiveKind::CurveTable)
				{
					OutPlan.ResolvableProbeCandidates.push_back(
						Directive.ExactLine);
				}
				if (bRecoveryReplayEligible ||
					!GHasCommittedRevision ||
					!GCommittedRevisionSafe ||
					!GCommittedParsed.IdentityToIndex.contains(
						Directive.Identity))
				{
					OutPlan.BaselineCaptureCandidates.push_back(
						Directive.ExactLine);
				}
			}
			const std::uint64_t Token = AllocateRevisionToken();
			OutPlan.Token = Token;

			const bool DeltaEligible = !ForceFull &&
				GHasCommittedRevision && GCommittedRevisionSafe &&
				Current.Safe;
			if (bRecoveryReplayEligible)
			{
				std::string Delta = BeginSyntheticAssetSection();
				std::string Restore = BeginSyntheticAssetSection();
				bool bSyntheticPayloadsSafe = true;
				for (const FDirective& Directive :
					Current.OrderedDirectives)
				{
					bSyntheticPayloadsSafe =
						TryAppendExactDirective(
							Delta, Directive.ExactLine) &&
						bSyntheticPayloadsSafe;
					bSyntheticPayloadsSafe =
						TryAppendExactDirective(
							Restore, Directive.ExactLine) &&
						bSyntheticPayloadsSafe;
					++OutPlan.AddedOrChanged;
				}

				for (const FDirective& Previous :
					GCommittedParsed.OrderedDirectives)
				{
					if (Current.IdentityToIndex.contains(Previous.Identity))
						continue;
					const auto Baseline =
						BaselineRestorations.find(Previous.Identity);
					FDirective Restoration;
					if (Baseline == BaselineRestorations.end())
					{
						++OutPlan.MissingBaselines;
						OutPlan.MissingBaselineIdentities.push_back(
							Previous.Identity);
						continue;
					}
					if (!TryParseBaseline(
							Previous.Identity, Baseline->second,
							Restoration))
					{
						OutPlan.BaselineRestorationsSafe = false;
						++OutPlan.MissingBaselines;
						OutPlan.MissingBaselineIdentities.push_back(
							Previous.Identity);
						continue;
					}
					bSyntheticPayloadsSafe =
						TryAppendExactDirective(
							Delta, Restoration.ExactLine) &&
						bSyntheticPayloadsSafe;
					++OutPlan.Restored;
				}

				if (bSyntheticPayloadsSafe &&
					Delta.size() <= MaximumPayloadBytes &&
					Restore.size() <= MaximumPayloadBytes)
				{
					OutPlan.Mode = ERevisionMode::RecoveryDelta;
					OutPlan.DeltaIni = std::move(Delta);
					OutPlan.RestoreIni = std::move(Restore);
				}
			}
			else if (DeltaEligible)
			{
				if (RevisionsEquivalent(GCommittedParsed, Current))
				{
					OutPlan.Mode = ERevisionMode::NoAssetChange;
				}
				else
				{
				std::string Delta = BeginSyntheticAssetSection();
				std::string Restore = BeginSyntheticAssetSection();
				bool bSyntheticPayloadsSafe = true;
				std::set<std::string> DeltaCurrentIdentities;
				for (const FDirective& Directive :
					Current.OrderedDirectives)
				{
					const auto PreviousIndex =
						GCommittedParsed.IdentityToIndex.find(
							Directive.Identity);
					if (PreviousIndex ==
							GCommittedParsed.IdentityToIndex.end() ||
						!DirectivesEquivalent(
							GCommittedParsed.OrderedDirectives[
								PreviousIndex->second],
							Directive))
					{
						bSyntheticPayloadsSafe =
							TryAppendExactDirective(
								Delta, Directive.ExactLine) &&
							bSyntheticPayloadsSafe;
						DeltaCurrentIdentities.emplace(
							Directive.Identity);
						++OutPlan.AddedOrChanged;
					}
					bSyntheticPayloadsSafe =
						TryAppendExactDirective(
							Restore, Directive.ExactLine) &&
						bSyntheticPayloadsSafe;
				}

				for (const FDirective& Previous :
					GCommittedParsed.OrderedDirectives)
				{
					if (Current.IdentityToIndex.contains(Previous.Identity))
						continue;
					const auto Baseline =
						BaselineRestorations.find(Previous.Identity);
					FDirective Restoration;
					if (Baseline == BaselineRestorations.end())
					{
						++OutPlan.MissingBaselines;
						OutPlan.MissingBaselineIdentities.push_back(
							Previous.Identity);
						continue;
					}
					if (!TryParseBaseline(
							Previous.Identity,
							Baseline->second,
							Restoration))
					{
						OutPlan.BaselineRestorationsSafe = false;
						++OutPlan.MissingBaselines;
						OutPlan.MissingBaselineIdentities.push_back(
							Previous.Identity);
						continue;
					}
					bSyntheticPayloadsSafe =
						TryAppendExactDirective(
							Delta, Restoration.ExactLine) &&
						bSyntheticPayloadsSafe;
					++OutPlan.Restored;
				}

				// PatchAssets resets its native forced-reference collection on each
				// pass. Retain every still-active asset with one independent unchanged
				// current directive, even when another row on that asset was touched.
				// Prefer a row proven resolvable after a successful native application.
				// A validated native baseline is weaker fallback proof. Neither source is
				// used by the removal/restoration loop above unless it is independently
				// present in BaselineRestorations.
				std::map<std::string, const FDirective*> RetentionByAsset;
				std::map<std::string, unsigned> RetentionRankByAsset;
				for (const FDirective& Directive : Current.OrderedDirectives)
				{
					if (DeltaCurrentIdentities.contains(Directive.Identity))
						continue;
					const std::string AssetIdentity =
						MakeAssetIdentity(Directive);
					const auto Baseline =
						BaselineRestorations.find(Directive.Identity);
					FDirective ValidatedBaseline;
					const bool bHasValidatedBaseline =
						Baseline != BaselineRestorations.end() &&
						TryParseBaseline(
							Directive.Identity, Baseline->second,
							ValidatedBaseline);
					const unsigned Rank =
						KnownResolvableIdentities.contains(Directive.Identity)
							? 2u : (bHasValidatedBaseline ? 1u : 0u);
					auto Existing = RetentionByAsset.find(AssetIdentity);
					if (Existing == RetentionByAsset.end() ||
						Rank > RetentionRankByAsset[AssetIdentity])
					{
						RetentionByAsset[AssetIdentity] = &Directive;
						RetentionRankByAsset[AssetIdentity] = Rank;
					}
				}
				for (const auto& [AssetIdentity, Directive] :
					RetentionByAsset)
				{
					(void)AssetIdentity;
					bSyntheticPayloadsSafe =
						TryAppendExactDirective(
							Delta, Directive->ExactLine) &&
						bSyntheticPayloadsSafe;
					++OutPlan.RetentionReapplied;
				}

				if (bSyntheticPayloadsSafe &&
					(OutPlan.AddedOrChanged || OutPlan.Restored ||
					 OutPlan.MissingBaselines) &&
					Delta.size() <= MaximumPayloadBytes &&
					Restore.size() <= MaximumPayloadBytes)
				{
					OutPlan.Mode = ERevisionMode::DeltaNative;
					OutPlan.DeltaIni = std::move(Delta);
					OutPlan.RestoreIni = std::move(Restore);
				}
				}
			}

			GPendingRevision.Ready = true;
			GPendingRevision.Token = Token;
			GPendingRevision.ExactPayload =
				ExactAcceptedDefaultGame;
			GPendingRevision.Parsed = std::move(Current);
			return true;
		}
		catch (...)
		{
			OutPlan = {};
			GPendingRevision = {};
			return false;
		}
	}

	void CommitRevision(std::uint64_t Token) noexcept
	{
		if (!Token || !GPendingRevision.Ready ||
			GPendingRevision.Token != Token)
		{
			return;
		}
		try
		{
			GCommittedExactPayload =
				std::move(GPendingRevision.ExactPayload);
			GCommittedParsed = std::move(GPendingRevision.Parsed);
			GCommittedRevisionSafe = GCommittedParsed.Safe;
			GHasCommittedRevision = true;
		}
		catch (...)
		{
			GCommittedExactPayload.clear();
			GCommittedParsed = {};
			GCommittedRevisionSafe = false;
			GHasCommittedRevision = false;
		}
		GPendingRevision = {};
	}

	void AbortRevision(std::uint64_t Token) noexcept
	{
		if (Token && GPendingRevision.Ready &&
			GPendingRevision.Token == Token)
		{
			GPendingRevision = {};
		}
	}
}
