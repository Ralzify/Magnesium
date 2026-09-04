#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Bounded, resident-only baseline sampling for one already-parsed AssetHotfix
// RowUpdate directive. This module does not retain UObject pointers and must be
// called on the game thread. It does not install hooks or schedule work. The
// current safe implementation samples CurveTable behavior only; DataTable
// sampling fails closed because the available reflected APIs do not prove the
// association between separately returned row-name and column-value arrays.
namespace AssetHotfixBaselineSampler
{
	enum class EAssetKind : std::uint8_t
	{
		DataTable,
		CurveTable
	};

	enum class ERestorationSemantics : std::uint8_t
	{
		None,
		Exact,
		// EvaluateCurveTableRow observes the value at a time, but not whether a
		// key exists there or its interpolation/tangent data. Applying this line
		// restores sampled behavior only; it is not a structural rollback.
		BehavioralNonStructural
	};

	enum class ESampleStatus : std::uint8_t
	{
		Success,
		InvalidDirective,
		BoundsExceeded,
		LookupUnavailable,
		AssetNotResident,
		TypeMismatch,
		ReflectionUnavailable,
		UnstableSnapshot,
		TargetRowNotFound,
		UnsafeRestorationValue,
		EvaluationFailed,
		Faulted
	};

	struct FParsedRowUpdateDirective
	{
		EAssetKind Kind = EAssetKind::DataTable;
		std::string_view AssetPath;
		std::string_view RowName;
		// DataTable column name, or CurveTable input time.
		std::string_view ColumnName;
	};

	inline constexpr std::size_t kMaximumRestorationDirectiveBytes =
		8192;

	struct FSampleResult
	{
		ESampleStatus Status = ESampleStatus::InvalidDirective;
		ERestorationSemantics Semantics = ERestorationSemantics::None;
		std::array<char, kMaximumRestorationDirectiveBytes>
			RestorationDirective{};
		std::size_t RestorationDirectiveLength = 0;

		std::string_view GetRestorationDirective() const noexcept
		{
			return std::string_view(
				RestorationDirective.data(),
				RestorationDirectiveLength);
		}
	};

	// Returns true only when a stable baseline was sampled and a complete
	// restoration line was produced. Every engine/ABI dependency is validated
	// at the call site; unsupported layouts fail closed through Out.Status.
	bool TrySampleResidentBaseline(
		const FParsedRowUpdateDirective& Directive,
		FSampleResult& Out) noexcept;
}
