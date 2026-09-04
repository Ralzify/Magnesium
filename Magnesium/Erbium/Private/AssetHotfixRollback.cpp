#include "pch.h"
#include "../Public/AssetHotfixRollback.h"
#include "../Public/AssetHotfixBaselineSampler.h"
#include "../Public/AssetHotfixRevisionPlanner.h"

#include "../Support/Public/FaultGuard.h"
#include "../Support/Public/VersionFeatureAdapter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <unordered_set>

namespace
{
	constexpr size_t MaximumFileNameCharacters = 4096;
	constexpr size_t MaximumDefaultGameCharacters = 8 * 1024 * 1024;
	constexpr ULONGLONG AcceptedIniPatchWindowMs = 2000;
	constexpr ULONGLONG RefreshAcceptanceWindowMs = 3000;
	constexpr ULONGLONG ReliableNoCallbackRecycleMs = 250;
	constexpr ULONGLONG UnreliableNoCallbackRecycleMs = 1000;
	constexpr ULONGLONG CorrelationRecoveryCooldownMs = 1000;
	constexpr uint32 MaximumRecoveryDiagnosticLogs = 8;
	constexpr int32 DestroyedObjectFlags = 0x01800000;

	using FHotfixIniFile = bool(__fastcall*)(
		UObject* Manager,
		const FString& FileName,
		const FString& IniData);
	using FPatchAssetsFromIniFiles = void(__fastcall*)(UObject* Manager);

	enum class EPreparedPatchState : uint8
	{
		Unclaimed,
		Running,
		Completed
	};

	struct FResolvableProbeCandidate
	{
		std::string ExactLine;
		std::string Identity;
	};

	struct FPreparedAssetRevision
	{
		std::shared_ptr<const std::wstring> Payload;
		std::shared_ptr<const std::wstring> FileName;
		FHotfixIniFile IniOriginal = nullptr;
		AssetHotfixRevisionPlanner::ERevisionMode Mode =
			AssetHotfixRevisionPlanner::ERevisionMode::FullNative;
		uint64 PlannerToken = 0;
		uint64 ManagerIdentity = 0;
		uint64 ManagerAuthoritySerial = 0;
		uint64 RefreshSequence = 0;
		DWORD IniThreadId = 0;
		std::wstring DeltaIni;
		std::wstring RestoreIni;
		std::vector<std::string> IdentitiesToTaint;
		std::vector<FResolvableProbeCandidate> ResolvableProbeCandidates;
		std::vector<std::string> ResolvableProbeIdentitiesAttempted;
		std::vector<std::string> ConfirmedResolvableIdentities;
		size_t AddedOrChanged = 0;
		size_t Restored = 0;
		size_t MissingBaselines = 0;
		size_t RetentionReapplied = 0;
		size_t BaselinesCaptured = 0;
		size_t BaselineSamplesAttempted = 0;
		bool bBaselineRestorationsSafe = true;
		bool bAcceptedRevisionSafe = false;
		EPreparedPatchState PatchState = EPreparedPatchState::Unclaimed;
		bool bIniReturned = false;
		bool bIniAccepted = false;
		bool bPatchSucceeded = false;
		bool bPatchUsedDelta = false;
		bool bPatchUsedFullFallback = false;
		bool bRestoreRecovered = false;
		bool bPatchClaimedBeforeIniReturned = false;
	};

	struct FExactOnceState
	{
		// Windows SRWLOCK avoids depending on the host process's MSVCP mutex ABI.
		SRWLOCK Lock = SRWLOCK_INIT;
		std::shared_ptr<const std::wstring> PendingPayload;
		std::shared_ptr<const std::wstring> LastAppliedPayload;
		std::shared_ptr<FPreparedAssetRevision> PreparedRevision;
		AssetHotfixRevisionPlanner::FBaselineRestorationMap
			BaselineRestorations;
		AssetHotfixRevisionPlanner::FKnownResolvableIdentitySet
			KnownResolvableIdentities;
		std::unordered_set<std::string> AttemptedResolvableIdentities;
		std::unordered_set<std::string> TaintedBaselineIdentities;
		uint64 ManagerIdentity = 0;
		uint64 ManagerAuthoritySerial = 1;
		uint64 RefreshSequenceSerial = 0;
		uint64 ArmedRefreshSequence = 0;
		uint64 ArmedManagerIdentity = 0;
		uint64 ObservedDefaultGameRefreshSequence = 0;
		uint64 ForwardedObservedRefreshSequence = 0;
		uint64 ExpectedPublicationSerial = 0;
		uint64 ExpectedManagerIdentity = 0;
		uint64 ExpectedManagerAuthoritySerial = 0;
		uint64 ActiveTrackedIniDispatches = 0;
		uint64 ActiveTrackedPatchDispatches = 0;
		uint64 PublicationSerial = 0;
		uint64 PlannerPreparationSerial = 0;
		ULONGLONG ArmedRefreshDeadlineMs = 0;
		ULONGLONG ExpectedPatchDeadlineMs = 0;
		ULONGLONG UnreliableNoCallbackRecycleDeadlineMs = 0;
		ULONGLONG RecoveryCooldownDeadlineMs = 0;
		DWORD ArmedRefreshThreadId = 0;
		DWORD UnreliableNoCallbackRecycleThreadId = 0;
		bool bExpectedPatchMaySkip = false;
		bool bForcePlannerFull = true;
		bool bPlannerPreparationActive = false;
		bool bReportedFirstAppliedPass = false;
		bool bReportedFirstUnchangedSkip = false;
		uint32 RecoveryDiagnosticCount = 0;
	};

	class FExclusiveLock
	{
	public:
		explicit FExclusiveLock(SRWLOCK& Lock) noexcept
			: LockAddress(&Lock)
		{
			AcquireSRWLockExclusive(LockAddress);
		}

		~FExclusiveLock() noexcept
		{
			ReleaseSRWLockExclusive(LockAddress);
		}

		FExclusiveLock(const FExclusiveLock&) = delete;
		FExclusiveLock& operator=(const FExclusiveLock&) = delete;

	private:
		SRWLOCK* LockAddress;
	};

	FExactOnceState& GetExactOnceState()
	{
		// Hooks can remain in flight during process shutdown. Keep the protected
		// state alive instead of relying on static destruction order.
		static FExactOnceState* const State = new FExactOnceState();
		return *State;
	}

	void AdvanceNonZero(uint64& Serial)
	{
		++Serial;
		if (!Serial)
			Serial = 1;
	}

	void ClearArmedRefreshLocked(FExactOnceState& State)
	{
		State.ArmedRefreshSequence = 0;
		State.ArmedManagerIdentity = 0;
		State.ArmedRefreshDeadlineMs = 0;
		State.ArmedRefreshThreadId = 0;
		State.ObservedDefaultGameRefreshSequence = 0;
		State.ForwardedObservedRefreshSequence = 0;
	}

	void ClearUnreliableRecycleLocked(FExactOnceState& State)
	{
		State.UnreliableNoCallbackRecycleDeadlineMs = 0;
		State.UnreliableNoCallbackRecycleThreadId = 0;
	}

	void ClearExpectedPatchLocked(FExactOnceState& State)
	{
		State.PendingPayload.reset();
		State.ExpectedPublicationSerial = 0;
		State.ExpectedManagerIdentity = 0;
		State.ExpectedManagerAuthoritySerial = 0;
		State.ExpectedPatchDeadlineMs = 0;
		State.bExpectedPatchMaySkip = false;
	}

	void ClearPreparedRevisionLocked(FExactOnceState& State) noexcept
	{
		if (State.PreparedRevision &&
			State.PreparedRevision->PlannerToken)
		{
			AssetHotfixRevisionPlanner::AbortRevision(
				State.PreparedRevision->PlannerToken);
		}
		State.PreparedRevision.reset();
	}

	void InvalidatePlannerPreparationLocked(FExactOnceState& State) noexcept
	{
		AdvanceNonZero(State.PlannerPreparationSerial);
		State.bPlannerPreparationActive = false;
	}

	void InvalidateSamplingTrustLocked(FExactOnceState& State) noexcept
	{
		State.bForcePlannerFull = true;
		State.KnownResolvableIdentities.clear();
		State.AttemptedResolvableIdentities.clear();
	}

	void InvalidateSamplingTrustForManager(uint64 ManagerIdentity) noexcept
	{
		if (!ManagerIdentity)
			return;
		auto& State = GetExactOnceState();
		FExclusiveLock Lock(State.Lock);
		if (State.ManagerIdentity == ManagerIdentity)
			InvalidateSamplingTrustLocked(State);
	}

	bool BeginCorrelationRecoveryLocked(
		FExactOnceState& State,
		uint64 ManagerIdentity,
		ULONGLONG Now)
	{
		if (!ManagerIdentity || State.ManagerIdentity != ManagerIdentity)
			return false;
		ClearUnreliableRecycleLocked(State);
		ClearArmedRefreshLocked(State);
		ClearExpectedPatchLocked(State);
		ClearPreparedRevisionLocked(State);
		InvalidatePlannerPreparationLocked(State);
		State.LastAppliedPayload.reset();
		InvalidateSamplingTrustLocked(State);
		State.RecoveryCooldownDeadlineMs = (std::max)(
			State.RecoveryCooldownDeadlineMs,
			Now + CorrelationRecoveryCooldownMs);
		if (State.RecoveryDiagnosticCount >= MaximumRecoveryDiagnosticLogs)
			return false;
		++State.RecoveryDiagnosticCount;
		return true;
	}

	class FScopedTrackedIniBoundary
	{
	public:
		bool Begin(
			uint64 ManagerIdentity,
			ULONGLONG Now)
		{
			auto& State = GetExactOnceState();
			FExclusiveLock Lock(State.Lock);
			if (!ManagerIdentity ||
				State.ManagerIdentity != ManagerIdentity)
			{
				return false;
			}
			++State.ActiveTrackedIniDispatches;
			ManagerIdentity_ = ManagerIdentity;
			ManagerAuthoritySerial_ = State.ManagerAuthoritySerial;
			if (State.ArmedRefreshSequence &&
				State.ArmedManagerIdentity == ManagerIdentity)
			{
				CapturedArmedRefreshSequence_ =
					State.ArmedRefreshSequence;
				bCapturedArmWithinDeadline_ =
					Now <= State.ArmedRefreshDeadlineMs;
			}
			bCapturedRecycle_ =
				State.UnreliableNoCallbackRecycleDeadlineMs != 0;
			bActive_ = true;
			return true;
		}

		uint64 GetManagerAuthoritySerial() const noexcept
		{
			return ManagerAuthoritySerial_;
		}

		uint64 GetCapturedArmedRefreshSequence() const noexcept
		{
			return CapturedArmedRefreshSequence_;
		}

		uint64 GetCorrelatedArmedRefreshSequence() const noexcept
		{
			return bCapturedArmWithinDeadline_
				? CapturedArmedRefreshSequence_
				: 0;
		}

		bool WasAttributedTooLate() const noexcept
		{
			return bCapturedRecycle_ ||
				(CapturedArmedRefreshSequence_ != 0 &&
					!bCapturedArmWithinDeadline_);
		}

		void MarkDefaultGameObserved() noexcept
		{
			if (!bActive_ || !CapturedArmedRefreshSequence_)
				return;
			auto& State = GetExactOnceState();
			FExclusiveLock Lock(State.Lock);
			if (State.ManagerIdentity == ManagerIdentity_ &&
				State.ManagerAuthoritySerial == ManagerAuthoritySerial_ &&
				State.ArmedRefreshSequence == CapturedArmedRefreshSequence_ &&
				State.ArmedManagerIdentity == ManagerIdentity_)
			{
				// Publish this before entering native HotfixIniFile. Some engine
				// branches synchronously reach PatchAssets from inside that call.
				// The patch detour must not mistake that changed request for the
				// no-DefaultGame/unchanged completion path.
				State.ObservedDefaultGameRefreshSequence =
					CapturedArmedRefreshSequence_;
			}
		}

		~FScopedTrackedIniBoundary() noexcept
		{
			if (!bActive_)
				return;
			auto& State = GetExactOnceState();
			FExclusiveLock Lock(State.Lock);
			if (State.ManagerIdentity == ManagerIdentity_ &&
				State.ManagerAuthoritySerial == ManagerAuthoritySerial_ &&
				State.ActiveTrackedIniDispatches)
			{
				--State.ActiveTrackedIniDispatches;
			}
		}

	private:
		uint64 ManagerIdentity_ = 0;
		uint64 ManagerAuthoritySerial_ = 0;
		uint64 CapturedArmedRefreshSequence_ = 0;
		bool bCapturedArmWithinDeadline_ = false;
		bool bCapturedRecycle_ = false;
		bool bActive_ = false;
	};

	class FScopedTrackedPatchBoundary
	{
	public:
		bool Begin(uint64 ManagerIdentity)
		{
			auto& State = GetExactOnceState();
			FExclusiveLock Lock(State.Lock);
			if (!ManagerIdentity || State.ManagerIdentity != ManagerIdentity)
				return false;
			++State.ActiveTrackedPatchDispatches;
			ManagerIdentity_ = ManagerIdentity;
			ManagerAuthoritySerial_ = State.ManagerAuthoritySerial;
			bActive_ = true;
			return true;
		}

		~FScopedTrackedPatchBoundary() noexcept
		{
			if (!bActive_)
				return;
			auto& State = GetExactOnceState();
			FExclusiveLock Lock(State.Lock);
			if (State.ManagerIdentity == ManagerIdentity_ &&
				State.ManagerAuthoritySerial == ManagerAuthoritySerial_ &&
				State.ActiveTrackedPatchDispatches)
			{
				--State.ActiveTrackedPatchDispatches;
			}
		}

	private:
		uint64 ManagerIdentity_ = 0;
		uint64 ManagerAuthoritySerial_ = 0;
		bool bActive_ = false;
	};

	bool IsUsableObject(const UObject* Object)
	{
		return VersionFeatureAdapter::IsLiveObject(Object) &&
			!(Object->ObjectFlags & DestroyedObjectFlags);
	}

	uint64 GetObjectIdentityGuarded(const UObject* Object)
	{
		if (!IsUsableObject(Object))
			return 0;

		++GGuardedNativeCallDepth;
		uint64 Result = 0;
		__try
		{
			const int32 Index = Object->Index;
			auto Item = TUObjectArray::GetItemByIndex(Index);
			if (Item && Item->GetObject() == Object)
			{
				Result =
					(static_cast<uint64>(static_cast<uint32>(Index)) << 32) |
					static_cast<uint32>(Item->SerialNumber);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
		--GGuardedNativeCallDepth;
		return Result;
	}

	enum class EFStringCopyResult : uint8
	{
		Copied,
		Oversize,
		Failure
	};

	int32 ReadFStringCountGuarded(const FString& Source)
	{
		int32 Count = 0;
		++GGuardedNativeCallDepth;
		__try
		{
			Count = Source.Num();
			const int32 Maximum = Source.Max();
			if (Count <= 0 || Maximum < Count)
				Count = 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			Count = 0;
		}
		--GGuardedNativeCallDepth;
		return Count;
	}

	bool CopyFStringDataGuarded(
		const FString& Source,
		int32 Count,
		wchar_t* Destination,
		size_t Length)
	{
		bool bCopied = false;
		++GGuardedNativeCallDepth;
		__try
		{
			const int32 CurrentCount = Source.Num();
			const int32 CurrentMaximum = Source.Max();
			const wchar_t* Data = Source.GetData();
			if (CurrentCount == Count && CurrentMaximum >= CurrentCount && Data &&
				SDK::MemReadable(
					Data, static_cast<size_t>(CurrentCount) * sizeof(wchar_t)) &&
				Data[CurrentCount - 1] == L'\0' &&
				!wmemchr(Data, L'\0', Length))
			{
				if (Length)
					memcpy(Destination, Data, Length * sizeof(wchar_t));
				bCopied = true;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
		--GGuardedNativeCallDepth;
		return bCopied;
	}

	EFStringCopyResult CopyFStringExactGuarded(
		const FString& Source,
		size_t MaximumCharacters,
		std::wstring& OutValue)
	{
		OutValue.clear();
		const int32 Count = ReadFStringCountGuarded(Source);
		if (!Count)
			return EFStringCopyResult::Failure;
		const size_t Length = static_cast<size_t>(Count - 1);
		if (Length > MaximumCharacters)
			return EFStringCopyResult::Oversize;

		try
		{
			OutValue.resize(Length);
		}
		catch (...)
		{
			OutValue.clear();
			return EFStringCopyResult::Failure;
		}
		if (!CopyFStringDataGuarded(
				Source, Count, OutValue.data(), Length))
		{
			OutValue.clear();
			return EFStringCopyResult::Failure;
		}
		return EFStringCopyResult::Copied;
	}

	bool IsDefaultGameIniFile(const std::wstring& FileName)
	{
		constexpr wchar_t Suffix[] = L"defaultgame.ini";
		constexpr size_t SuffixLength = std::size(Suffix) - 1;
		if (FileName.size() < SuffixLength)
			return false;
		const size_t BasenameOffset = FileName.size() - SuffixLength;
		if (BasenameOffset &&
			FileName[BasenameOffset - 1] != L'\\' &&
			FileName[BasenameOffset - 1] != L'/')
		{
			return false;
		}
		return _wcsicmp(
			FileName.c_str() + BasenameOffset, Suffix) == 0;
	}

	bool ExactPayloadsEqual(
		const std::shared_ptr<const std::wstring>& Left,
		const std::shared_ptr<const std::wstring>& Right)
	{
		return Left && Right &&
			Left->size() == Right->size() &&
			(Left->empty() || memcmp(
				Left->data(), Right->data(),
				Left->size() * sizeof(wchar_t)) == 0);
	}

	bool WideToUtf8Exact(
		const std::wstring& Value,
		std::string& Out) noexcept
	{
		Out.clear();
		if (Value.empty())
			return true;
		if (Value.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
			return false;
		const int Required = WideCharToMultiByte(
			CP_UTF8, WC_ERR_INVALID_CHARS, Value.data(),
			static_cast<int>(Value.size()), nullptr, 0, nullptr, nullptr);
		if (Required <= 0)
			return false;
		try
		{
			Out.resize(static_cast<size_t>(Required));
		}
		catch (...)
		{
			return false;
		}
		if (WideCharToMultiByte(
				CP_UTF8, WC_ERR_INVALID_CHARS, Value.data(),
				static_cast<int>(Value.size()), Out.data(), Required,
				nullptr, nullptr) != Required)
		{
			Out.clear();
			return false;
		}
		return true;
	}

	bool Utf8ToWideExact(
		const std::string& Value,
		std::wstring& Out) noexcept
	{
		Out.clear();
		if (Value.empty())
			return true;
		if (Value.size() > static_cast<size_t>((std::numeric_limits<int>::max)()))
			return false;
		const int Required = MultiByteToWideChar(
			CP_UTF8, MB_ERR_INVALID_CHARS, Value.data(),
			static_cast<int>(Value.size()), nullptr, 0);
		if (Required <= 0)
			return false;
		try
		{
			Out.resize(static_cast<size_t>(Required));
		}
		catch (...)
		{
			return false;
		}
		if (MultiByteToWideChar(
				CP_UTF8, MB_ERR_INVALID_CHARS, Value.data(),
				static_cast<int>(Value.size()), Out.data(), Required) != Required)
		{
			Out.clear();
			return false;
		}
		return true;
	}

	FString MakeBorrowedFString(const std::wstring& Value) noexcept
	{
		FString Result;
		Result.Data = const_cast<wchar_t*>(Value.c_str());
		Result.NumElements = static_cast<int32>(Value.size() + 1);
		Result.MaxElements = Result.NumElements;
		return Result;
	}

	FHotfixIniFile GOriginalBaseHotfixIniFile = nullptr;
	FHotfixIniFile GOriginalManagerHotfixIniFile = nullptr;
	FPatchAssetsFromIniFiles GOriginalPatchAssetsFromIniFiles = nullptr;
	uintptr_t GBaseHotfixIniFileTarget = 0;
	uintptr_t GManagerHotfixIniFileTarget = 0;
	uintptr_t GBasePatchAssetsFromIniFilesTarget = 0;
	std::atomic<uint64> GIniReadyManagerIdentity{0};
	std::atomic<bool> GBaseHotfixIniHookEnabled{false};
	std::atomic<bool> GBasePatchAssetsHookEnabled{false};
	const char* GPatchAssetsResolutionDiagnostic = "not-scanned";
	SRWLOCK GManagerHookInstallLock = SRWLOCK_INIT;
	bool GReportedManagerIniHookFailure = false;
	thread_local uint32 GHotfixIniDispatchDepth = 0;
	thread_local uint32 GPatchAssetsDispatchDepth = 0;

	enum class ENestedPatchPolicy : uint8
	{
		ForwardNormally,
		ForwardAtMostOnce,
		Suppress
	};

	struct FActiveIniRevisionContext
	{
		FPreparedAssetRevision* Revision = nullptr;
		bool bTopLevelPatchConsumed = false;
	};

	thread_local FActiveIniRevisionContext* GActiveIniRevisionContext = nullptr;
	thread_local ENestedPatchPolicy GNestedPatchPolicy =
		ENestedPatchPolicy::ForwardNormally;
	thread_local bool GNestedPatchForwarded = false;

	class FScopedActiveIniRevision
	{
	public:
		explicit FScopedActiveIniRevision(
			FPreparedAssetRevision* Revision) noexcept
			: Previous(GActiveIniRevisionContext)
		{
			Context.Revision = Revision;
			GActiveIniRevisionContext = &Context;
		}

		~FScopedActiveIniRevision() noexcept
		{
			GActiveIniRevisionContext = Previous;
		}

	private:
		FActiveIniRevisionContext Context;
		FActiveIniRevisionContext* Previous = nullptr;
	};

	class FScopedNestedPatchPolicy
	{
	public:
		explicit FScopedNestedPatchPolicy(
			ENestedPatchPolicy Policy) noexcept
			: PreviousPolicy(GNestedPatchPolicy),
			  bPreviousForwarded(GNestedPatchForwarded)
		{
			GNestedPatchPolicy = Policy;
			GNestedPatchForwarded = false;
		}

		~FScopedNestedPatchPolicy() noexcept
		{
			GNestedPatchPolicy = PreviousPolicy;
			GNestedPatchForwarded = bPreviousForwarded;
		}

		bool WasForwarded() const noexcept
		{
			return GNestedPatchForwarded;
		}

	private:
		ENestedPatchPolicy PreviousPolicy;
		bool bPreviousForwarded = false;
	};

	struct FPreparedPatchResult
	{
		bool bSucceeded = false;
		bool bUsedDelta = false;
		bool bUsedFullFallback = false;
		bool bRestoreRecovered = false;
		bool bNativeAssetPassExecuted = false;
	};

	struct FPreparedFinalizeReport
	{
		bool bFinalized = false;
		bool bRecovery = false;
		bool bByteIdentical = false;
		bool bNoAssetChange = false;
		bool bDelta = false;
		bool bRecoveryDelta = false;
		bool bFullNative = false;
		bool bFullFallback = false;
		bool bRestoreRecovered = false;
		size_t AddedOrChanged = 0;
		size_t Restored = 0;
		size_t RetentionReapplied = 0;
		size_t MissingBaselines = 0;
		size_t BaselinesCaptured = 0;
		size_t BaselineSamplesAttempted = 0;
		bool bBaselineRestorationsSafe = true;
	};

	bool CallPreparedIni(
		const FPreparedAssetRevision& Revision,
		UObject* Manager,
		const std::wstring& Payload,
		ENestedPatchPolicy NestedPatchPolicy,
		bool* bOutNestedPatchForwarded = nullptr)
	{
		if (bOutNestedPatchForwarded)
			*bOutNestedPatchForwarded = false;
		if (!Revision.IniOriginal || !Revision.FileName)
			return false;
		const FString FileName = MakeBorrowedFString(*Revision.FileName);
		const FString IniData = MakeBorrowedFString(Payload);
		FScopedNestedPatchPolicy PolicyScope(NestedPatchPolicy);
		const bool bAccepted = Revision.IniOriginal(
			Manager, FileName, IniData);
		if (bOutNestedPatchForwarded)
			*bOutNestedPatchForwarded = PolicyScope.WasForwarded();
		return bAccepted;
	}

	bool RestorePreparedConfig(
		const FPreparedAssetRevision& Revision,
		UObject* Manager,
		bool& bOutRecovered)
	{
		bOutRecovered = false;
		if (CallPreparedIni(
				Revision, Manager, Revision.RestoreIni,
				ENestedPatchPolicy::Suppress))
		{
			return true;
		}

		// Re-feed the exact accepted payload so unrelated DefaultGame sections
		// remain applied, then retry the AssetHotfix-only clear-and-restore. Both
		// calls suppress any nested PatchAssets callback: recovery is config-only.
		if (Revision.Payload)
		{
			CallPreparedIni(
				Revision, Manager, *Revision.Payload,
				ENestedPatchPolicy::Suppress);
		}
		const bool bRestored = CallPreparedIni(
			Revision, Manager, Revision.RestoreIni,
			ENestedPatchPolicy::Suppress);
		bOutRecovered = bRestored;
		return bRestored;
	}

	FPreparedPatchResult ExecutePreparedPatch(
		const std::shared_ptr<FPreparedAssetRevision>& Revision,
		UObject* Manager,
		FPatchAssetsFromIniFiles Original,
		bool bIniHadReturnedAtClaim)
	{
		FPreparedPatchResult Result;
		if (!Revision || !Original)
			return Result;

		switch (Revision->Mode)
		{
		case AssetHotfixRevisionPlanner::ERevisionMode::ByteIdentical:
		case AssetHotfixRevisionPlanner::ERevisionMode::NoAssetChange:
			Result.bSucceeded = true;
			return Result;

		case AssetHotfixRevisionPlanner::ERevisionMode::FullNative:
			Original(Manager);
			Result.bNativeAssetPassExecuted = true;
			Result.bSucceeded = true;
			return Result;

		case AssetHotfixRevisionPlanner::ERevisionMode::RecoveryDelta:
		case AssetHotfixRevisionPlanner::ERevisionMode::DeltaNative:
			break;
		}

		if (GetCurrentThreadId() != Revision->IniThreadId)
		{
			// A native PatchAssets callback may legitimately publish from an older
			// branch's asynchronous path. Do not call HotfixIni or mutate GConfig on
			// a different thread than the accepted DefaultGame transaction.
			Original(Manager);
			Result.bNativeAssetPassExecuted = true;
			// A stock full pass cannot apply trusted rollback rows which are
			// intentionally absent from the accepted config. Preserve the prior
			// planner state and retry the one-pass recovery transaction instead of
			// committing those removals as completed.
			Result.bSucceeded = Revision->Restored == 0;
			Result.bUsedFullFallback = true;
			return Result;
		}

		const bool bInsideOwningIniDispatch =
			GActiveIniRevisionContext &&
			GActiveIniRevisionContext->Revision == Revision.get();
		if (!bIniHadReturnedAtClaim && !bInsideOwningIniDispatch)
		{
			// Never mutate GConfig concurrently with the still-running accepted INI
			// call. Preserve the stock callback once on this ambiguous cross-thread
			// boundary and commit the accepted revision when that INI returns.
			Original(Manager);
			Result.bNativeAssetPassExecuted = true;
			Result.bSucceeded = Revision->Restored == 0;
			Result.bUsedFullFallback = true;
			return Result;
		}

		bool bNestedPatchForwarded = false;
		const bool bDeltaAccepted = CallPreparedIni(
			*Revision, Manager, Revision->DeltaIni,
			ENestedPatchPolicy::ForwardAtMostOnce,
			&bNestedPatchForwarded);
		if (bDeltaAccepted)
		{
			if (!bNestedPatchForwarded)
				Original(Manager);
			Result.bNativeAssetPassExecuted = true;
			Result.bUsedDelta = true;
			Result.bSucceeded = RestorePreparedConfig(
				*Revision, Manager, Result.bRestoreRecovered);
			return Result;
		}

		// A rejected synthetic delta may have partially combined config or may
		// have synchronously consumed the one native pass. Restore the exact
		// active section first. If no nested pass ran, forward one full native
		// pass on that restored state; never execute a second pass.
		Result.bUsedFullFallback = true;
		if (!RestorePreparedConfig(
				*Revision, Manager, Result.bRestoreRecovered))
		{
			return Result;
		}
		if (bNestedPatchForwarded)
		{
			Result.bNativeAssetPassExecuted = true;
			// The one allowed native pass saw a rejected/possibly partial
			// synthetic config. Do not claim or commit this revision, and never run
			// a second pass in the same boundary. Correlation recovery schedules a
			// clean full retry after the quiet cooldown.
			return Result;
		}
		Original(Manager);
		Result.bNativeAssetPassExecuted = true;
		Result.bSucceeded = Revision->Restored == 0;
		return Result;
	}

	bool ParseBaselineSampleDirective(
		const std::string& ExactLine,
		AssetHotfixBaselineSampler::FParsedRowUpdateDirective& Out) noexcept
	{
		Out = {};
		std::string_view Line(ExactLine);
		while (!Line.empty() &&
			(Line.front() == ' ' || Line.front() == '\t'))
		{
			Line.remove_prefix(1);
		}
		while (!Line.empty() &&
			(Line.back() == ' ' || Line.back() == '\t' ||
			 Line.back() == '\r'))
		{
			Line.remove_suffix(1);
		}

		constexpr std::string_view CurvePrefix = "+CurveTable=";
		constexpr std::string_view DataPrefix = "+DataTable=";
		auto StartsWithInsensitive = [](
			std::string_view Value,
			std::string_view Prefix) noexcept
		{
			if (Value.size() < Prefix.size())
				return false;
			for (size_t Index = 0; Index < Prefix.size(); ++Index)
			{
				const auto Lower = [](char Character) noexcept
				{
					return Character >= 'A' && Character <= 'Z'
						? static_cast<char>(Character - 'A' + 'a')
						: Character;
				};
				if (Lower(Value[Index]) != Lower(Prefix[Index]))
					return false;
			}
			return true;
		};

		std::string_view Body;
		if (StartsWithInsensitive(Line, CurvePrefix))
		{
			Out.Kind = AssetHotfixBaselineSampler::EAssetKind::CurveTable;
			Body = Line.substr(CurvePrefix.size());
		}
		else if (StartsWithInsensitive(Line, DataPrefix))
		{
			Out.Kind = AssetHotfixBaselineSampler::EAssetKind::DataTable;
			Body = Line.substr(DataPrefix.size());
		}
		else
		{
			return false;
		}

		std::string_view Tokens[5]{};
		size_t Begin = 0;
		for (size_t Token = 0; Token < 4; ++Token)
		{
			const size_t Delimiter = Body.find(';', Begin);
			if (Delimiter == std::string_view::npos)
				return false;
			Tokens[Token] = Body.substr(Begin, Delimiter - Begin);
			Begin = Delimiter + 1;
		}
		if (Body.find(';', Begin) != std::string_view::npos)
			return false;
		Tokens[4] = Body.substr(Begin);
		auto Trim = [](std::string_view Value) noexcept
		{
			while (!Value.empty() &&
				(Value.front() == ' ' || Value.front() == '\t'))
			{
				Value.remove_prefix(1);
			}
			while (!Value.empty() &&
				(Value.back() == ' ' || Value.back() == '\t'))
			{
				Value.remove_suffix(1);
			}
			return Value;
		};
		for (size_t Index = 0; Index < 4; ++Index)
			Tokens[Index] = Trim(Tokens[Index]);
		if (!StartsWithInsensitive(Tokens[1], "RowUpdate") ||
			Tokens[1].size() != std::string_view("RowUpdate").size())
		{
			return false;
		}
		Out.AssetPath = Tokens[0];
		Out.RowName = Tokens[2];
		Out.ColumnName = Tokens[3];
		return !Out.AssetPath.empty() && !Out.RowName.empty() &&
			!Out.ColumnName.empty();
	}

	struct FBaselineSampleCandidate
	{
		std::string ExactLine;
		std::string Identity;
	};

	void ProbePreparedResolvableCandidates(
		const std::shared_ptr<FPreparedAssetRevision>& Revision,
		const FPreparedPatchResult& PatchResult) noexcept
	{
		constexpr size_t MaximumProbesPerRevision = 64;
		constexpr ULONGLONG MaximumProbeMilliseconds = 8;
		if (!Revision || !Revision->bAcceptedRevisionSafe ||
			!PatchResult.bSucceeded ||
			!PatchResult.bNativeAssetPassExecuted ||
			PatchResult.bUsedFullFallback ||
			GetCurrentThreadId() != Revision->IniThreadId)
		{
			return;
		}

		try
		{
			LARGE_INTEGER Started{};
			LARGE_INTEGER Frequency{};
			if (!QueryPerformanceCounter(&Started) ||
				!QueryPerformanceFrequency(&Frequency) ||
				Frequency.QuadPart <= 0)
			{
				return;
			}
			for (const FResolvableProbeCandidate& Candidate :
				Revision->ResolvableProbeCandidates)
			{
				LARGE_INTEGER Now{};
				if (Revision->ResolvableProbeIdentitiesAttempted.size() >=
						MaximumProbesPerRevision ||
					!QueryPerformanceCounter(&Now) ||
					(Now.QuadPart - Started.QuadPart) * 1000 >=
						Frequency.QuadPart * static_cast<LONGLONG>(
							MaximumProbeMilliseconds))
				{
					break;
				}

				AssetHotfixBaselineSampler::FParsedRowUpdateDirective Parsed;
				if (!ParseBaselineSampleDirective(Candidate.ExactLine, Parsed) ||
					Parsed.Kind != AssetHotfixBaselineSampler::
						EAssetKind::CurveTable)
				{
					continue;
				}
				Revision->ResolvableProbeIdentitiesAttempted.push_back(
					Candidate.Identity);
				AssetHotfixBaselineSampler::FSampleResult Sample;
				if (!AssetHotfixBaselineSampler::TrySampleResidentBaseline(
						Parsed, Sample))
				{
					continue;
				}
				std::string SampleIdentity;
				if (AssetHotfixRevisionPlanner::TryGetDirectiveIdentity(
						std::string(Sample.GetRestorationDirective()),
						SampleIdentity) &&
					SampleIdentity == Candidate.Identity)
				{
					Revision->ConfirmedResolvableIdentities.push_back(
						Candidate.Identity);
				}
			}
		}
		catch (...)
		{
			// Probing is advisory. The already-completed native transaction remains
			// authoritative and no unproven identity is published.
		}
	}

	std::shared_ptr<FPreparedAssetRevision> PrepareAssetRevision(
		FHotfixIniFile IniOriginal,
		uint64 ManagerIdentity,
		uint64 ManagerAuthoritySerial,
		uint64 RefreshSequence,
		const std::shared_ptr<const std::wstring>& FileName,
		const std::shared_ptr<const std::wstring>& Payload,
		const std::string& Utf8Payload) noexcept
	{
		constexpr size_t MaximumSamplesPerRevision = 64;
		constexpr ULONGLONG MaximumSamplingMilliseconds = 8;
		uint64 ReservationSerial = 0;
		uint64 PendingPlannerToken = 0;
		bool bForceFull = true;
		bool bSamplingAllowed = false;
		AssetHotfixRevisionPlanner::FRevisionPlan PreliminaryPlan;
		std::vector<FBaselineSampleCandidate> Candidates;
		try
		{
			{
				auto& State = GetExactOnceState();
				FExclusiveLock Lock(State.Lock);
				if (!IniOriginal || !ManagerIdentity || !FileName || !Payload ||
					State.PreparedRevision ||
					State.ManagerIdentity != ManagerIdentity ||
					State.ManagerAuthoritySerial != ManagerAuthoritySerial)
				{
					return nullptr;
				}
				if (State.bPlannerPreparationActive)
				{
					InvalidatePlannerPreparationLocked(State);
					InvalidateSamplingTrustLocked(State);
					return nullptr;
				}
				const bool bCorrelatedRefresh = RefreshSequence != 0 &&
					State.ArmedRefreshSequence == RefreshSequence &&
					State.ArmedManagerIdentity == ManagerIdentity;
				const bool bPristineBootstrap = RefreshSequence == 0 &&
					State.bForcePlannerFull && !State.LastAppliedPayload &&
					!State.ExpectedPublicationSerial &&
					State.ActiveTrackedIniDispatches == 1 &&
					!State.ActiveTrackedPatchDispatches;
				if (!bCorrelatedRefresh && !bPristineBootstrap)
					return nullptr;

				AdvanceNonZero(State.PlannerPreparationSerial);
				ReservationSerial = State.PlannerPreparationSerial;
				State.bPlannerPreparationActive = true;
				bForceFull = State.bForcePlannerFull ||
					!State.LastAppliedPayload;
				// Hook installation can occur after the engine's startup hotfix. Never
				// treat a first observed full payload as pristine and accidentally save
				// its already-overridden values as native baselines. Sampling begins only
				// after a safely committed revision establishes an absent->present edge.
				bSamplingAllowed = bCorrelatedRefresh &&
					!State.bForcePlannerFull &&
					State.LastAppliedPayload != nullptr &&
					State.ArmedRefreshThreadId == GetCurrentThreadId();
				if (!AssetHotfixRevisionPlanner::PrepareRevision(
						Utf8Payload, State.BaselineRestorations,
						State.KnownResolvableIdentities,
						bForceFull, PreliminaryPlan))
				{
					InvalidateSamplingTrustLocked(State);
					State.bPlannerPreparationActive = false;
					return nullptr;
				}
				// A full native pass can include rows already applied by an unsafe or
				// untracked predecessor. Never capture its candidates as native. They
				// are tainted below, while a successful tracked full pass still seeds
				// planner state so an unchanged poll can be skipped.
				bSamplingAllowed = bSamplingAllowed &&
					PreliminaryPlan.AcceptedRevisionSafe &&
					PreliminaryPlan.Mode !=
						AssetHotfixRevisionPlanner::ERevisionMode::FullNative &&
					PreliminaryPlan.Mode !=
						AssetHotfixRevisionPlanner::ERevisionMode::RecoveryDelta;
				PendingPlannerToken = PreliminaryPlan.Token;
				AssetHotfixRevisionPlanner::AbortRevision(
					PendingPlannerToken);
				PendingPlannerToken = 0;

				if (bSamplingAllowed)
				{
					for (const std::string& ExactLine :
						PreliminaryPlan.BaselineCaptureCandidates)
					{
						if (Candidates.size() >= MaximumSamplesPerRevision)
							break;
						std::string Identity;
						AssetHotfixBaselineSampler::FParsedRowUpdateDirective
							Parsed;
						if (!AssetHotfixRevisionPlanner::TryGetDirectiveIdentity(
								ExactLine, Identity) ||
							State.BaselineRestorations.contains(Identity) ||
							State.TaintedBaselineIdentities.contains(Identity) ||
							!ParseBaselineSampleDirective(ExactLine, Parsed) ||
							Parsed.Kind != AssetHotfixBaselineSampler::
								EAssetKind::CurveTable)
						{
							continue;
						}
						Candidates.push_back({
							ExactLine, std::move(Identity) });
					}
				}
			}

			// DataTable sampling is disabled until its reflected row/value ordering
			// can be proven. Curve evaluations are resident-only and bounded here.
			AssetHotfixRevisionPlanner::FBaselineRestorationMap Sampled;
			size_t SamplesAttempted = 0;
			LARGE_INTEGER SamplingStarted{};
			LARGE_INTEGER SamplingFrequency{};
			const bool bPreciseBudgetAvailable =
				QueryPerformanceCounter(&SamplingStarted) &&
				QueryPerformanceFrequency(&SamplingFrequency) &&
				SamplingFrequency.QuadPart > 0;
			for (const FBaselineSampleCandidate& Candidate : Candidates)
			{
				LARGE_INTEGER SamplingNow{};
				if (!bPreciseBudgetAvailable ||
					!QueryPerformanceCounter(&SamplingNow) ||
					SamplesAttempted >= MaximumSamplesPerRevision ||
					(SamplingNow.QuadPart - SamplingStarted.QuadPart) * 1000 >=
						SamplingFrequency.QuadPart *
							static_cast<LONGLONG>(MaximumSamplingMilliseconds))
				{
					break;
				}
				AssetHotfixBaselineSampler::FParsedRowUpdateDirective Parsed;
				if (!ParseBaselineSampleDirective(Candidate.ExactLine, Parsed))
					continue;
				++SamplesAttempted;
				AssetHotfixBaselineSampler::FSampleResult Sample;
				if (!AssetHotfixBaselineSampler::TrySampleResidentBaseline(
						Parsed, Sample))
				{
					continue;
				}
				std::string Restoration(Sample.GetRestorationDirective());
				std::string RestorationIdentity;
				if (!AssetHotfixRevisionPlanner::TryGetDirectiveIdentity(
						Restoration, RestorationIdentity) ||
					RestorationIdentity != Candidate.Identity)
				{
					continue;
				}
				Sampled.emplace(Candidate.Identity, std::move(Restoration));
			}

			auto& State = GetExactOnceState();
			FExclusiveLock Lock(State.Lock);
			if (!State.bPlannerPreparationActive ||
				State.PlannerPreparationSerial != ReservationSerial ||
				State.ManagerIdentity != ManagerIdentity ||
				State.ManagerAuthoritySerial != ManagerAuthoritySerial ||
				State.PreparedRevision)
			{
				if (State.bPlannerPreparationActive &&
					State.PlannerPreparationSerial == ReservationSerial)
				{
					State.bPlannerPreparationActive = false;
					InvalidateSamplingTrustLocked(State);
				}
				return nullptr;
			}
			for (auto& [Identity, Restoration] : Sampled)
			{
				if (!State.TaintedBaselineIdentities.contains(Identity))
				{
					State.BaselineRestorations.try_emplace(
						Identity, std::move(Restoration));
				}
			}

			AssetHotfixRevisionPlanner::FRevisionPlan Plan;
			if (!AssetHotfixRevisionPlanner::PrepareRevision(
					Utf8Payload, State.BaselineRestorations,
					State.KnownResolvableIdentities,
					bForceFull, Plan))
			{
				InvalidateSamplingTrustLocked(State);
				State.bPlannerPreparationActive = false;
				return nullptr;
			}
			PendingPlannerToken = Plan.Token;
			auto Revision = std::make_shared<FPreparedAssetRevision>();
			Revision->Payload = Payload;
			Revision->FileName = FileName;
			Revision->IniOriginal = IniOriginal;
			Revision->Mode = Plan.Mode;
			Revision->PlannerToken = Plan.Token;
			Revision->ManagerIdentity = ManagerIdentity;
			Revision->ManagerAuthoritySerial = ManagerAuthoritySerial;
			Revision->RefreshSequence = RefreshSequence;
			Revision->IniThreadId = GetCurrentThreadId();
			Revision->AddedOrChanged = Plan.AddedOrChanged;
			Revision->Restored = Plan.Restored;
			Revision->MissingBaselines = Plan.MissingBaselines;
			Revision->RetentionReapplied = Plan.RetentionReapplied;
			Revision->BaselinesCaptured = Sampled.size();
			Revision->BaselineSamplesAttempted = SamplesAttempted;
			Revision->bBaselineRestorationsSafe =
				Plan.BaselineRestorationsSafe;
			Revision->bAcceptedRevisionSafe = Plan.AcceptedRevisionSafe;
			for (const std::string& ExactLine :
				Plan.ResolvableProbeCandidates)
			{
				if (Revision->ResolvableProbeCandidates.size() >=
						MaximumSamplesPerRevision)
				{
					break;
				}
				std::string Identity;
				AssetHotfixBaselineSampler::FParsedRowUpdateDirective Parsed;
				if (!AssetHotfixRevisionPlanner::TryGetDirectiveIdentity(
						ExactLine, Identity) ||
					State.KnownResolvableIdentities.contains(Identity) ||
					State.AttemptedResolvableIdentities.contains(Identity) ||
					!ParseBaselineSampleDirective(ExactLine, Parsed) ||
					Parsed.Kind != AssetHotfixBaselineSampler::
						EAssetKind::CurveTable)
				{
					continue;
				}
				Revision->ResolvableProbeCandidates.push_back({
					ExactLine, std::move(Identity) });
			}
			std::unordered_set<std::string> TaintSet;
			for (const std::string& ExactLine :
				PreliminaryPlan.BaselineCaptureCandidates)
			{
				std::string Identity;
				if (AssetHotfixRevisionPlanner::TryGetDirectiveIdentity(
						ExactLine, Identity) &&
					!State.BaselineRestorations.contains(Identity) &&
					TaintSet.emplace(Identity).second)
				{
					Revision->IdentitiesToTaint.push_back(
						std::move(Identity));
				}
			}
			for (const std::string& Identity :
				Plan.MissingBaselineIdentities)
			{
				if (TaintSet.emplace(Identity).second)
				{
					Revision->IdentitiesToTaint.push_back(Identity);
				}
			}
			if ((Plan.Mode ==
					 AssetHotfixRevisionPlanner::ERevisionMode::DeltaNative ||
				 Plan.Mode ==
					 AssetHotfixRevisionPlanner::ERevisionMode::RecoveryDelta) &&
				(!Utf8ToWideExact(Plan.DeltaIni, Revision->DeltaIni) ||
				 !Utf8ToWideExact(Plan.RestoreIni, Revision->RestoreIni)))
			{
				AssetHotfixRevisionPlanner::AbortRevision(Plan.Token);
				PendingPlannerToken = 0;
				InvalidateSamplingTrustLocked(State);
				State.bPlannerPreparationActive = false;
				return nullptr;
			}
			State.PreparedRevision = Revision;
			State.bPlannerPreparationActive = false;
			PendingPlannerToken = 0;
			return Revision;
		}
		catch (...)
		{
			AssetHotfixRevisionPlanner::AbortRevision(PendingPlannerToken);
			auto& State = GetExactOnceState();
			FExclusiveLock Lock(State.Lock);
			if (State.bPlannerPreparationActive &&
				State.PlannerPreparationSerial == ReservationSerial)
			{
				State.bPlannerPreparationActive = false;
				InvalidateSamplingTrustLocked(State);
			}
			return nullptr;
		}
	}

	void FinalizePreparedRevisionLocked(
		FExactOnceState& State,
		const std::shared_ptr<FPreparedAssetRevision>& Revision,
		ULONGLONG Now,
		FPreparedFinalizeReport& Report)
	{
		if (!Revision || State.PreparedRevision != Revision ||
			!Revision->bIniReturned ||
			Revision->PatchState != EPreparedPatchState::Completed)
		{
			return;
		}

		Report.bFinalized = true;
		Report.AddedOrChanged = Revision->AddedOrChanged;
		Report.Restored = Revision->Restored;
		Report.RetentionReapplied = Revision->RetentionReapplied;
		Report.MissingBaselines = Revision->MissingBaselines;
		Report.BaselinesCaptured = Revision->BaselinesCaptured;
		Report.BaselineSamplesAttempted =
			Revision->BaselineSamplesAttempted;
		Report.bBaselineRestorationsSafe =
			Revision->bBaselineRestorationsSafe;
		Report.bFullFallback = Revision->bPatchUsedFullFallback;
		Report.bRestoreRecovered = Revision->bRestoreRecovered;
		if (!Revision->bIniAccepted || !Revision->bPatchSucceeded)
		{
			Report.bRecovery = BeginCorrelationRecoveryLocked(
				State, Revision->ManagerIdentity, Now);
			return;
		}

		AssetHotfixRevisionPlanner::CommitRevision(
			Revision->PlannerToken);
		// A committed mutation without a trusted pre-mutation snapshot must
		// never be sampled later as though its now-overridden value were native.
		// Retire any stale/unusable entry at the same serialized commit point.
		for (const std::string& Identity : Revision->IdentitiesToTaint)
		{
			State.BaselineRestorations.erase(Identity);
			State.TaintedBaselineIdentities.emplace(Identity);
		}
		State.LastAppliedPayload = Revision->Payload;
		const bool bRequiresSafeReseed =
			Revision->bPatchUsedFullFallback;
		if (bRequiresSafeReseed)
		{
			InvalidateSamplingTrustLocked(State);
		}
		else
		{
			if (!Revision->bAcceptedRevisionSafe)
			{
				State.KnownResolvableIdentities.clear();
				State.AttemptedResolvableIdentities.clear();
			}
			else
			{
				for (const std::string& Identity :
					Revision->ResolvableProbeIdentitiesAttempted)
				{
					State.AttemptedResolvableIdentities.emplace(Identity);
				}
				for (const std::string& Identity :
					Revision->ConfirmedResolvableIdentities)
				{
					State.KnownResolvableIdentities.emplace(Identity);
				}
			}
			State.bForcePlannerFull = false;
		}
		ClearExpectedPatchLocked(State);
		ClearArmedRefreshLocked(State);
		ClearUnreliableRecycleLocked(State);
		if (Revision->bPatchClaimedBeforeIniReturned)
		{
			State.RecoveryCooldownDeadlineMs = (std::max)(
				State.RecoveryCooldownDeadlineMs,
				Now + CorrelationRecoveryCooldownMs);
		}

		Report.bByteIdentical = Revision->Mode ==
			AssetHotfixRevisionPlanner::ERevisionMode::ByteIdentical;
		Report.bNoAssetChange = Revision->Mode ==
			AssetHotfixRevisionPlanner::ERevisionMode::NoAssetChange;
		Report.bDelta = Revision->Mode ==
			AssetHotfixRevisionPlanner::ERevisionMode::DeltaNative &&
			Revision->bPatchUsedDelta;
		Report.bRecoveryDelta = Revision->Mode ==
			AssetHotfixRevisionPlanner::ERevisionMode::RecoveryDelta &&
			Revision->bPatchUsedDelta;
		Report.bFullNative = Revision->Mode ==
			AssetHotfixRevisionPlanner::ERevisionMode::FullNative;
		State.PreparedRevision.reset();
	}

	void LogPreparedFinalizeReport(
		const FPreparedFinalizeReport& Report)
	{
		if (!Report.bFinalized)
			return;
		if (Report.bRecovery)
		{
			SDK::DbgLog(
				"[AssetHotfixReload] planned revision could not complete "
				"safely; stock behavior remains fail-open and polling will "
				"retry after a short cooldown\n");
			return;
		}
		if (Report.bByteIdentical)
		{
			SDK::DbgLog(
				"[AssetHotfixReload] byte-identical DefaultGame; "
				"PatchAssets row walk skipped\n");
		}
		else if (Report.bNoAssetChange)
		{
			SDK::DbgLog(
				"[AssetHotfixReload] DefaultGame config changed without "
				"AssetHotfix changes; PatchAssets row walk skipped\n");
		}
		else if (Report.bRecoveryDelta)
		{
			SDK::DbgLog(
				"[AssetHotfixReload] safe full-replay delta applied rows=%zu "
				"restored=%zu missing-baselines=%zu captured=%zu sampled=%zu "
				"baselines-safe=%s restore-recovered=%s\n",
				Report.AddedOrChanged, Report.Restored,
				Report.MissingBaselines, Report.BaselinesCaptured,
				Report.BaselineSamplesAttempted,
				Report.bBaselineRestorationsSafe ? "true" : "false",
				Report.bRestoreRecovered ? "true" : "false");
		}
		else if (Report.bDelta)
		{
			SDK::DbgLog(
				"[AssetHotfixReload] AssetHotfix delta applied rows=%zu "
				"restored=%zu retention=%zu missing-baselines=%zu "
				"captured=%zu sampled=%zu baselines-safe=%s "
				"restore-recovered=%s\n",
				Report.AddedOrChanged, Report.Restored,
				Report.RetentionReapplied, Report.MissingBaselines,
				Report.BaselinesCaptured,
				Report.BaselineSamplesAttempted,
				Report.bBaselineRestorationsSafe ? "true" : "false",
				Report.bRestoreRecovered ? "true" : "false");
		}
		else if (Report.bFullFallback)
		{
			SDK::DbgLog(
				"[AssetHotfixReload] planned delta used one fail-open full "
				"native pass\n");
		}
		else if (Report.bFullNative)
		{
			SDK::DbgLog(
				"[AssetHotfixReload] full native AssetHotfix pass completed "
				"missing-baselines=%zu\n",
				Report.MissingBaselines);
		}
	}

	void WriteStartupDiagnostic(const char* Format, ...)
	{
		std::array<wchar_t, 32768> AppData{};
		const DWORD AppDataLength = GetEnvironmentVariableW(
			L"APPDATA", AppData.data(), static_cast<DWORD>(AppData.size()));
		if (!AppDataLength || AppDataLength >= AppData.size())
			return;
		std::wstring Directory(AppData.data(), AppDataLength);
		if (!Directory.empty() && Directory.back() != L'\\' &&
			Directory.back() != L'/')
		{
			Directory.push_back(L'\\');
		}
		Directory.append(L"ATLAS");
		if (!CreateDirectoryW(Directory.c_str(), nullptr) &&
			GetLastError() != ERROR_ALREADY_EXISTS)
		{
			return;
		}
		const std::wstring Path = Directory + L"\\diagnostics.log";
		HANDLE File = CreateFileW(
			Path.c_str(), FILE_APPEND_DATA,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (File == INVALID_HANDLE_VALUE)
			return;

		char Message[1024]{};
		va_list Arguments;
		va_start(Arguments, Format);
		const int MessageLength = vsnprintf(
			Message, sizeof(Message), Format, Arguments);
		va_end(Arguments);
		if (MessageLength >= 0)
		{
			SYSTEMTIME Time{};
			GetLocalTime(&Time);
			char Line[1280]{};
			const int LineLength = snprintf(
				Line, sizeof(Line),
				"%02u:%02u:%02u.%03u pid=%lu tid=%lu %s\r\n",
				Time.wHour, Time.wMinute, Time.wSecond, Time.wMilliseconds,
				GetCurrentProcessId(), GetCurrentThreadId(), Message);
			if (LineLength > 0)
			{
				const DWORD Bytes = static_cast<DWORD>(
					LineLength < static_cast<int>(sizeof(Line))
						? LineLength
						: sizeof(Line) - 1);
				DWORD Written = 0;
				WriteFile(File, Line, Bytes, &Written, nullptr);
			}
		}
		CloseHandle(File);
	}

	class FScopedHotfixIniDispatch
	{
	public:
		FScopedHotfixIniDispatch() noexcept
		{
			++GHotfixIniDispatchDepth;
		}
		~FScopedHotfixIniDispatch() noexcept
		{
			--GHotfixIniDispatchDepth;
		}
	};

	bool DispatchHotfixIniFile(
		FHotfixIniFile Original,
		UObject* Manager,
		const FString& FileName,
		const FString& IniData)
	{
		if (!Original)
			return false;
		if (GHotfixIniDispatchDepth)
		{
			// A derived override can delegate to the hooked base virtual. Treat the
			// nested Super call as part of the outer accepted transaction.
			return Original(Manager, FileName, IniData);
		}
		FScopedHotfixIniDispatch DispatchScope;

		const uint64 ManagerIdentity = GetObjectIdentityGuarded(Manager);
		std::wstring FileNameCopy;
		const bool bPotentialDefaultGame = ManagerIdentity &&
			CopyFStringExactGuarded(
				FileName, MaximumFileNameCharacters, FileNameCopy) ==
					EFStringCopyResult::Copied &&
			IsDefaultGameIniFile(FileNameCopy);
		if (bPotentialDefaultGame)
		{
			// The hooked base HotfixIniFile itself proves this is a hotfix manager.
			// Seed startup ownership before the tick-side derived-hook observation so
			// the normal startup full PatchAssets pass can initialize planner state.
			auto& State = GetExactOnceState();
			FExclusiveLock Lock(State.Lock);
			if (!State.ManagerIdentity)
			{
				AdvanceNonZero(State.ManagerAuthoritySerial);
				InvalidatePlannerPreparationLocked(State);
				State.ManagerIdentity = ManagerIdentity;
				ClearArmedRefreshLocked(State);
				ClearExpectedPatchLocked(State);
				ClearPreparedRevisionLocked(State);
				ClearUnreliableRecycleLocked(State);
				State.RecoveryCooldownDeadlineMs = 0;
				State.LastAppliedPayload.reset();
				InvalidateSamplingTrustLocked(State);
			}
		}

		const ULONGLONG DispatchStartedMs = GetTickCount64();
		FScopedTrackedIniBoundary TrackedIniBoundary;
		const bool bTrackedManager =
			TrackedIniBoundary.Begin(ManagerIdentity, DispatchStartedMs);
		const uint64 ManagerAuthoritySerial =
			TrackedIniBoundary.GetManagerAuthoritySerial();
		const uint64 CapturedArmedRefreshSequence =
			TrackedIniBoundary.GetCapturedArmedRefreshSequence();

		std::wstring PayloadCopy;
		const bool bDefaultGame =
			bTrackedManager && bPotentialDefaultGame;
		const bool bPayloadCopied = bDefaultGame &&
			CopyFStringExactGuarded(
				IniData, MaximumDefaultGameCharacters, PayloadCopy) ==
					EFStringCopyResult::Copied;
		const bool bAttributedAtEntry =
			CapturedArmedRefreshSequence != 0 ||
			TrackedIniBoundary.WasAttributedTooLate();
		std::shared_ptr<const std::wstring> Payload;
		std::shared_ptr<const std::wstring> RetainedFileName;
		std::string Utf8Payload;
		bool bPayloadRetained = false;
		bool bPayloadConverted = false;
		if (bPayloadCopied)
		{
			try
			{
				Payload = std::make_shared<const std::wstring>(
					std::move(PayloadCopy));
				RetainedFileName = std::make_shared<const std::wstring>(
					FileNameCopy);
				bPayloadRetained = true;
				bPayloadConverted = WideToUtf8Exact(
					*Payload, Utf8Payload);
			}
			catch (...)
			{
				Payload.reset();
				RetainedFileName.reset();
			}
		}

		std::shared_ptr<FPreparedAssetRevision> PreparedRevision;
		if (bPayloadRetained && bPayloadConverted)
		{
			PreparedRevision = PrepareAssetRevision(
				GOriginalBaseHotfixIniFile, ManagerIdentity,
				ManagerAuthoritySerial,
				TrackedIniBoundary.GetCorrelatedArmedRefreshSequence(),
				RetainedFileName, Payload, Utf8Payload);
		}

		if (bDefaultGame)
			TrackedIniBoundary.MarkDefaultGameObserved();
		FScopedActiveIniRevision ActiveRevisionScope(
			PreparedRevision.get());
		const bool bAccepted = Original(Manager, FileName, IniData);
		if (!bTrackedManager || !bDefaultGame)
			return bAccepted;

		auto& State = GetExactOnceState();
		if (PreparedRevision)
		{
			FPreparedFinalizeReport Report;
			{
				FExclusiveLock Lock(State.Lock);
				if (State.PreparedRevision == PreparedRevision &&
					State.ManagerIdentity == ManagerIdentity &&
					State.ManagerAuthoritySerial == ManagerAuthoritySerial)
				{
					PreparedRevision->bIniReturned = true;
					PreparedRevision->bIniAccepted = bAccepted;
					if (!bAccepted && PreparedRevision->PatchState !=
							EPreparedPatchState::Completed)
					{
						Report.bFinalized = true;
						Report.bRecovery = BeginCorrelationRecoveryLocked(
							State, ManagerIdentity, GetTickCount64());
					}
					else if (PreparedRevision->PatchState ==
							EPreparedPatchState::Completed)
					{
						FinalizePreparedRevisionLocked(
							State, PreparedRevision, GetTickCount64(), Report);
					}
					else if (PreparedRevision->PatchState ==
							EPreparedPatchState::Unclaimed && bAccepted)
					{
						AdvanceNonZero(State.PublicationSerial);
						State.PendingPayload = Payload;
						State.ExpectedPublicationSerial =
							State.PublicationSerial;
						State.ExpectedManagerIdentity = ManagerIdentity;
						State.ExpectedManagerAuthoritySerial =
							ManagerAuthoritySerial;
						State.ExpectedPatchDeadlineMs = GetTickCount64() +
							AcceptedIniPatchWindowMs;
						State.bExpectedPatchMaySkip = true;
						ClearUnreliableRecycleLocked(State);
						if (CapturedArmedRefreshSequence)
							ClearArmedRefreshLocked(State);
					}
				}
			}
			GIniReadyManagerIdentity.store(
				ManagerIdentity, std::memory_order_release);
			LogPreparedFinalizeReport(Report);
			return bAccepted;
		}

		if (!bAccepted || !bPayloadRetained)
		{
			bool bReportRecovery = false;
			{
				FExclusiveLock Lock(State.Lock);
				if (State.ManagerIdentity == ManagerIdentity &&
					State.ManagerAuthoritySerial == ManagerAuthoritySerial &&
					(bAttributedAtEntry ||
						(CapturedArmedRefreshSequence &&
							(State.ObservedDefaultGameRefreshSequence ==
								CapturedArmedRefreshSequence ||
							 State.ForwardedObservedRefreshSequence ==
								CapturedArmedRefreshSequence))))
				{
					bReportRecovery = BeginCorrelationRecoveryLocked(
						State, ManagerIdentity, GetTickCount64());
				}
			}
			if (bReportRecovery)
			{
				SDK::DbgLog(
					"[AssetHotfixReload] DefaultGame was not safely accepted; "
					"stock work remains forwarded and periodic polling will "
					"retry after a short cooldown\n");
			}
			return bAccepted;
		}

		const ULONGLONG Now = GetTickCount64();
		bool bLateAttributedAccepted = false;
		bool bPublicationConflict = false;
		bool bObservedPatchAlreadyForwarded = false;
		bool bReportRecovery = false;
		bool bReportApplied = false;
		{
			FExclusiveLock Lock(State.Lock);
			if (State.ManagerIdentity != ManagerIdentity ||
				State.ManagerAuthoritySerial != ManagerAuthoritySerial)
			{
				return bAccepted;
			}
			bObservedPatchAlreadyForwarded =
				CapturedArmedRefreshSequence != 0 &&
				State.ForwardedObservedRefreshSequence ==
					CapturedArmedRefreshSequence;
			bPublicationConflict =
				!bObservedPatchAlreadyForwarded &&
				State.ExpectedPublicationSerial != 0;
			const bool bCapturedArmStillCurrent =
				TrackedIniBoundary.GetCorrelatedArmedRefreshSequence() != 0 &&
				State.ArmedRefreshSequence == CapturedArmedRefreshSequence &&
				State.ArmedManagerIdentity == ManagerIdentity;
			bLateAttributedAccepted =
				!bObservedPatchAlreadyForwarded &&
				(TrackedIniBoundary.WasAttributedTooLate() ||
				(CapturedArmedRefreshSequence != 0 &&
					!bCapturedArmStillCurrent) ||
				(State.RecoveryCooldownDeadlineMs != 0 &&
					CapturedArmedRefreshSequence == 0));
			if (bObservedPatchAlreadyForwarded)
			{
				// PatchAssets was reached synchronously (or raced this callback)
				// after the request had already observed DefaultGame. Its native
				// row walk has completed, so commit those exact bytes without
				// waiting for a second PatchAssets callback that will never come.
				State.LastAppliedPayload = std::move(Payload);
				InvalidateSamplingTrustLocked(State);
				ClearUnreliableRecycleLocked(State);
				ClearExpectedPatchLocked(State);
				ClearArmedRefreshLocked(State);
				State.RecoveryCooldownDeadlineMs = (std::max)(
					State.RecoveryCooldownDeadlineMs,
					Now + CorrelationRecoveryCooldownMs);
				if (!State.bReportedFirstAppliedPass)
				{
					State.bReportedFirstAppliedPass = true;
					bReportApplied = true;
				}
			}
			else if (bPublicationConflict)
			{
				// Do not overwrite an in-flight payload token. Both native workflows
				// remain forwarded; retire the added request and wait for a quiet
				// window before polling again.
				bReportRecovery = BeginCorrelationRecoveryLocked(
					State, ManagerIdentity, Now);
			}
			else if (bLateAttributedAccepted)
			{
				bReportRecovery = BeginCorrelationRecoveryLocked(
					State, ManagerIdentity, Now);
			}
			else
			{
				const bool bCorrelatedAddedRefresh =
					bCapturedArmStillCurrent;
				AdvanceNonZero(State.PublicationSerial);
				State.PendingPayload = std::move(Payload);
				State.ExpectedPublicationSerial = State.PublicationSerial;
				State.ExpectedManagerIdentity = ManagerIdentity;
				State.ExpectedManagerAuthoritySerial = ManagerAuthoritySerial;
				State.ExpectedPatchDeadlineMs =
					Now + AcceptedIniPatchWindowMs;
				State.bExpectedPatchMaySkip =
					bCorrelatedAddedRefresh && !State.bForcePlannerFull;
				ClearUnreliableRecycleLocked(State);
				if (bCorrelatedAddedRefresh)
					ClearArmedRefreshLocked(State);
			}
		}
		GIniReadyManagerIdentity.store(
			ManagerIdentity, std::memory_order_release);
		if (bObservedPatchAlreadyForwarded)
		{
			if (bReportApplied)
			{
				SDK::DbgLog(
					"[AssetHotfixReload] observed DefaultGame reached its "
					"native PatchAssets pass before publication; exact bytes "
					"committed and polling will continue\n");
			}
			return bAccepted;
		}
		if (bPublicationConflict || bLateAttributedAccepted)
		{
			if (bReportRecovery)
			{
				if (bPublicationConflict)
				{
					SDK::DbgLog(
						"[AssetHotfixReload] overlapping DefaultGame publication "
						"was left to stock behavior; periodic polling will retry "
						"after a short cooldown\n");
				}
				else
				{
					SDK::DbgLog(
						"[AssetHotfixReload] late DefaultGame callback was left "
						"to stock behavior; periodic polling will retry after a "
						"short cooldown\n");
				}
			}
			return bAccepted;
		}
		return bAccepted;
	}

	bool BaseHotfixIniFileDetour(
		UObject* Manager,
		const FString& FileName,
		const FString& IniData)
	{
		return DispatchHotfixIniFile(
			GOriginalBaseHotfixIniFile, Manager, FileName, IniData);
	}

	bool ManagerHotfixIniFileDetour(
		UObject* Manager,
		const FString& FileName,
		const FString& IniData)
	{
		return DispatchHotfixIniFile(
			GOriginalManagerHotfixIniFile, Manager, FileName, IniData);
	}

	class FScopedPatchAssetsDispatch
	{
	public:
		FScopedPatchAssetsDispatch() noexcept
		{
			++GPatchAssetsDispatchDepth;
		}
		~FScopedPatchAssetsDispatch() noexcept
		{
			--GPatchAssetsDispatchDepth;
		}
	};

	void PatchAssetsFromIniFilesDetour(UObject* Manager)
	{
		const FPatchAssetsFromIniFiles Original =
			GOriginalPatchAssetsFromIniFiles;
		if (!Original)
			return;
		if (GPatchAssetsDispatchDepth)
		{
			if (GNestedPatchPolicy == ENestedPatchPolicy::Suppress)
				return;
			if (GNestedPatchPolicy ==
					ENestedPatchPolicy::ForwardAtMostOnce)
			{
				if (GNestedPatchForwarded)
					return;
				GNestedPatchForwarded = true;
			}
			Original(Manager);
			return;
		}
		if (GActiveIniRevisionContext &&
			GActiveIniRevisionContext->Revision)
		{
			if (GActiveIniRevisionContext->bTopLevelPatchConsumed)
				return;
			GActiveIniRevisionContext->bTopLevelPatchConsumed = true;
		}
		FScopedPatchAssetsDispatch DispatchScope;

		const uint64 ManagerIdentity = GetObjectIdentityGuarded(Manager);
		FScopedTrackedPatchBoundary TrackedPatchBoundary;
		TrackedPatchBoundary.Begin(ManagerIdentity);
		const ULONGLONG Now = GetTickCount64();
		std::shared_ptr<const std::wstring> Payload;
		std::shared_ptr<const std::wstring> LastAppliedPayload;
		uint64 PublicationSerial = 0;
		uint64 ManagerAuthoritySerial = 0;
		bool bExpectedPatch = false;
		bool bMaySkip = false;
		bool bOwnedUnchangedPatch = false;
		bool bOwnedPreparedDuplicate = false;
		bool bObservedDefaultGamePatch = false;
		std::shared_ptr<FPreparedAssetRevision> PreparedRevision;
		bool bPreparedIniHadReturnedAtClaim = false;
		bool bReportRecovery = false;
		const char* RecoveryReason = nullptr;
		{
			auto& State = GetExactOnceState();
			FExclusiveLock Lock(State.Lock);
			const bool bTrackedManager =
				ManagerIdentity && State.ManagerIdentity == ManagerIdentity;
			const bool bArmedForTrackedManager = bTrackedManager &&
				State.ArmedRefreshSequence != 0 &&
				State.ArmedManagerIdentity == ManagerIdentity;
			const DWORD DispatchThreadId = GetCurrentThreadId();
			const bool bNoIniDispatchActive =
				State.ActiveTrackedIniDispatches == 0;
			const bool bObservedForArmedRefresh =
				bArmedForTrackedManager &&
				State.ObservedDefaultGameRefreshSequence ==
					State.ArmedRefreshSequence;
			const bool bPreparedForManager = bTrackedManager &&
				State.PreparedRevision &&
				State.PreparedRevision->ManagerIdentity == ManagerIdentity &&
				State.PreparedRevision->ManagerAuthoritySerial ==
					State.ManagerAuthoritySerial;
			const bool bPreparedIniCorrelation = bPreparedForManager &&
				!State.PreparedRevision->bIniReturned &&
				State.ActiveTrackedIniDispatches != 0 &&
				(bObservedForArmedRefresh ||
				 State.PreparedRevision->RefreshSequence == 0);
			const bool bPreparedTlsCorrelation = bPreparedForManager &&
				GActiveIniRevisionContext &&
				GActiveIniRevisionContext->Revision ==
					State.PreparedRevision.get();
			const bool bActivePreparedIni = bPreparedForManager &&
				State.PreparedRevision->PatchState ==
					EPreparedPatchState::Unclaimed &&
				(bPreparedTlsCorrelation || bPreparedIniCorrelation);
			const bool bPreparedPatchAlreadyClaimed =
				bPreparedForManager &&
				State.PreparedRevision->PatchState !=
					EPreparedPatchState::Unclaimed &&
				(bPreparedTlsCorrelation || bPreparedIniCorrelation ||
				 State.PreparedRevision->PatchState ==
					EPreparedPatchState::Running);
			if (bPreparedPatchAlreadyClaimed)
			{
				bOwnedPreparedDuplicate = true;
			}
			else if (bActivePreparedIni)
			{
				PreparedRevision = State.PreparedRevision;
				bPreparedIniHadReturnedAtClaim =
					PreparedRevision->bIniReturned;
				PreparedRevision->bPatchClaimedBeforeIniReturned =
					!PreparedRevision->bIniReturned;
				PreparedRevision->PatchState = EPreparedPatchState::Running;
				if (bObservedForArmedRefresh)
				{
					State.ForwardedObservedRefreshSequence =
						State.ArmedRefreshSequence;
				}
				ClearUnreliableRecycleLocked(State);
			}
			else if (bObservedForArmedRefresh)
			{
				// HotfixIniFile has already identified DefaultGame for this exact
				// request. A synchronous/racing PatchAssets callback is therefore
				// changed work, never the no-callback unchanged completion. Forward
				// it once and let the INI detour commit the retained bytes when the
				// native callback returns.
				bObservedDefaultGamePatch = true;
				State.ForwardedObservedRefreshSequence =
					State.ArmedRefreshSequence;
				ClearUnreliableRecycleLocked(State);
			}
			else if (bArmedForTrackedManager && bNoIniDispatchActive &&
				State.ArmedRefreshThreadId == DispatchThreadId &&
				!State.ExpectedPublicationSerial &&
				Now <= State.ArmedRefreshDeadlineMs)
			{
				// An unchanged title-file check does not call HotfixIniFile again,
				// but stock still closes the workflow through PatchAssets. This
				// armed boundary belongs to our request, so consuming it without
				// the native row walk is the exact-once unchanged fast path.
				bOwnedUnchangedPatch = true;
				ClearArmedRefreshLocked(State);
				ClearUnreliableRecycleLocked(State);
			}
			else if (bArmedForTrackedManager)
			{
				RecoveryReason = "unmatched armed PatchAssets";
				bReportRecovery = BeginCorrelationRecoveryLocked(
					State, ManagerIdentity, Now);
			}
			else if (bTrackedManager)
			{
				if (State.ExpectedPublicationSerial &&
					Now > State.ExpectedPatchDeadlineMs)
				{
					RecoveryReason = "late accepted PatchAssets";
					bReportRecovery = BeginCorrelationRecoveryLocked(
						State, ManagerIdentity, Now);
				}
				else if (!State.ExpectedPublicationSerial &&
					State.UnreliableNoCallbackRecycleDeadlineMs)
				{
					if (bNoIniDispatchActive &&
						State.UnreliableNoCallbackRecycleThreadId ==
							DispatchThreadId &&
						Now <=
						State.UnreliableNoCallbackRecycleDeadlineMs)
					{
						// Private/unavailable native busy state uses this short
						// recycle window to retain ownership of a late unchanged
						// completion. It is safe to consume for the same reason as
						// the directly armed case above.
						bOwnedUnchangedPatch = true;
						ClearUnreliableRecycleLocked(State);
					}
					else
					{
						RecoveryReason = "unmatched recycled PatchAssets";
						bReportRecovery = BeginCorrelationRecoveryLocked(
							State, ManagerIdentity, Now);
					}
				}
				if (!bOwnedUnchangedPatch && !RecoveryReason &&
					State.ExpectedPublicationSerial)
				{
					bExpectedPatch = State.PendingPayload &&
						State.ExpectedManagerIdentity == ManagerIdentity &&
						State.ExpectedManagerAuthoritySerial ==
							State.ManagerAuthoritySerial &&
						Now <= State.ExpectedPatchDeadlineMs;
					if (bExpectedPatch)
					{
						if (State.PreparedRevision &&
							State.PreparedRevision->PatchState ==
								EPreparedPatchState::Unclaimed &&
							State.PreparedRevision->ManagerIdentity ==
								ManagerIdentity &&
							State.PreparedRevision->ManagerAuthoritySerial ==
								State.ExpectedManagerAuthoritySerial)
						{
							PreparedRevision = State.PreparedRevision;
							bPreparedIniHadReturnedAtClaim =
								PreparedRevision->bIniReturned;
							PreparedRevision->bPatchClaimedBeforeIniReturned =
								!PreparedRevision->bIniReturned;
							PreparedRevision->PatchState =
								EPreparedPatchState::Running;
						}
						else
						{
							Payload = State.PendingPayload;
							LastAppliedPayload = State.LastAppliedPayload;
							PublicationSerial =
								State.ExpectedPublicationSerial;
							ManagerAuthoritySerial =
								State.ExpectedManagerAuthoritySerial;
							bMaySkip = State.bExpectedPatchMaySkip;
						}
						// The scoped top-level Patch boundary remains active through the
						// complete native row walk/skip on every dispatching thread.
						ClearExpectedPatchLocked(State);
					}
					else
					{
						RecoveryReason =
							"uncorrelated accepted PatchAssets";
						bReportRecovery = BeginCorrelationRecoveryLocked(
							State, ManagerIdentity, Now);
					}
				}
				else if (!bOwnedUnchangedPatch && !RecoveryReason &&
					State.RecoveryCooldownDeadlineMs)
				{
					// A late native callback inside the quiet window is never
					// suppressed. Extend the window so a successor request cannot
					// claim another callback from the same old workflow.
					State.RecoveryCooldownDeadlineMs = (std::max)(
						State.RecoveryCooldownDeadlineMs,
						Now + CorrelationRecoveryCooldownMs);
				}
			}
		}

		if (bOwnedPreparedDuplicate)
			return;

		if (PreparedRevision)
		{
			const FPreparedPatchResult PatchResult = ExecutePreparedPatch(
				PreparedRevision, Manager, Original,
				bPreparedIniHadReturnedAtClaim);
			ProbePreparedResolvableCandidates(
				PreparedRevision, PatchResult);
			FPreparedFinalizeReport Report;
			{
				auto& State = GetExactOnceState();
				FExclusiveLock Lock(State.Lock);
				if (State.PreparedRevision == PreparedRevision &&
					State.ManagerIdentity == ManagerIdentity &&
					State.ManagerAuthoritySerial ==
						PreparedRevision->ManagerAuthoritySerial)
				{
					PreparedRevision->bPatchSucceeded =
						PatchResult.bSucceeded;
					PreparedRevision->bPatchUsedDelta =
						PatchResult.bUsedDelta;
					PreparedRevision->bPatchUsedFullFallback =
						PatchResult.bUsedFullFallback;
					PreparedRevision->bRestoreRecovered =
						PatchResult.bRestoreRecovered;
					PreparedRevision->PatchState =
						EPreparedPatchState::Completed;
					FinalizePreparedRevisionLocked(
						State, PreparedRevision, GetTickCount64(), Report);
				}
			}
			LogPreparedFinalizeReport(Report);
			return;
		}

		if (bOwnedUnchangedPatch)
		{
			bool bReportSkipped = false;
			{
				auto& State = GetExactOnceState();
				FExclusiveLock Lock(State.Lock);
				if (State.ManagerIdentity == ManagerIdentity &&
					!State.bReportedFirstUnchangedSkip)
				{
					State.bReportedFirstUnchangedSkip = true;
					bReportSkipped = true;
				}
			}
			if (bReportSkipped)
			{
				SDK::DbgLog(
					"[AssetHotfixReload] unchanged native check completed "
					"without a new DefaultGame callback; PatchAssets row walk "
					"skipped\n");
			}
			return;
		}

		if (bObservedDefaultGamePatch)
		{
			// This is the native CurveTable/DataTable walk for the observed
			// changed DefaultGame. The INI detour commits its bytes afterward.
			Original(Manager);
			InvalidateSamplingTrustForManager(ManagerIdentity);
			return;
		}

		if (RecoveryReason)
		{
			Original(Manager);
			InvalidateSamplingTrustForManager(ManagerIdentity);
			if (bReportRecovery)
			{
				SDK::DbgLog(
					"[AssetHotfixReload] %s was forwarded to stock behavior; "
					"periodic polling will retry after a short cooldown\n",
					RecoveryReason);
			}
			return;
		}

		if (!bExpectedPatch)
		{
			Original(Manager);
			InvalidateSamplingTrustForManager(ManagerIdentity);
			return;
		}

		const bool bByteIdentical =
			bMaySkip && ExactPayloadsEqual(Payload, LastAppliedPayload);
		if (!bByteIdentical)
			Original(Manager);

		bool bReportApplied = false;
		bool bReportSkipped = false;
		{
			auto& State = GetExactOnceState();
			FExclusiveLock Lock(State.Lock);
			if (State.ManagerIdentity == ManagerIdentity &&
				State.ManagerAuthoritySerial == ManagerAuthoritySerial &&
				PublicationSerial != 0 &&
				State.PublicationSerial == PublicationSerial)
			{
				State.LastAppliedPayload = std::move(Payload);
				if (!bByteIdentical)
					InvalidateSamplingTrustLocked(State);
				if (bByteIdentical &&
					!State.bReportedFirstUnchangedSkip)
				{
					State.bReportedFirstUnchangedSkip = true;
					bReportSkipped = true;
				}
				else if (!bByteIdentical &&
					!State.bReportedFirstAppliedPass)
				{
					State.bReportedFirstAppliedPass = true;
					bReportApplied = true;
				}
			}
		}
		if (bReportApplied)
		{
			SDK::DbgLog(
				"[AssetHotfixReload] accepted DefaultGame bytes changed; "
				"one stock PatchAssets pass completed\n");
		}
		if (bReportSkipped)
		{
			SDK::DbgLog(
				"[AssetHotfixReload] byte-identical DefaultGame; "
				"PatchAssets row walk skipped\n");
		}
	}

	bool IsExecutableAddress(const void* Address) noexcept
	{
		if (!Address)
			return false;
		MEMORY_BASIC_INFORMATION Region{};
		if (VirtualQuery(Address, &Region, sizeof(Region)) != sizeof(Region))
			return false;
		constexpr DWORD Executable = PAGE_EXECUTE | PAGE_EXECUTE_READ |
			PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
		return Region.State == MEM_COMMIT &&
			(Region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
			(Region.Protect & Executable) != 0;
	}

	bool IsReadableMemoryRange(
		const void* Address,
		size_t Size) noexcept
	{
		if (!Address || !Size)
			return false;
		const uintptr_t Begin = reinterpret_cast<uintptr_t>(Address);
		if (Begin > (std::numeric_limits<uintptr_t>::max)() - Size)
			return false;
		const uintptr_t End = Begin + Size;
		uintptr_t Cursor = Begin;
		while (Cursor < End)
		{
			MEMORY_BASIC_INFORMATION Region{};
			if (VirtualQuery(
					reinterpret_cast<const void*>(Cursor),
					&Region, sizeof(Region)) != sizeof(Region) ||
				Region.State != MEM_COMMIT ||
				(Region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
			{
				return false;
			}
			const uintptr_t RegionBegin =
				reinterpret_cast<uintptr_t>(Region.BaseAddress);
			if (RegionBegin >
				(std::numeric_limits<uintptr_t>::max)() - Region.RegionSize)
			{
				return false;
			}
			const uintptr_t RegionEnd = RegionBegin + Region.RegionSize;
			if (RegionEnd <= Cursor)
				return false;
			Cursor = (std::min)(RegionEnd, End);
		}
		return true;
	}

	void* ResolveFunctionContainingReference(uintptr_t Reference) noexcept
	{
		if (!Reference || !IsExecutableAddress(
				reinterpret_cast<void*>(Reference)))
		{
			return nullptr;
		}
		DWORD64 RuntimeImageBase = 0;
		const PRUNTIME_FUNCTION RuntimeFunction = RtlLookupFunctionEntry(
			static_cast<DWORD64>(Reference), &RuntimeImageBase, nullptr);
		if (!RuntimeFunction || !RuntimeImageBase)
			return nullptr;

		RUNTIME_FUNCTION Current = *RuntimeFunction;
		const DWORD64 ImmediateBegin =
			RuntimeImageBase + Current.BeginAddress;
		const DWORD64 ImmediateEnd =
			RuntimeImageBase + Current.EndAddress;
		if (Reference < ImmediateBegin || Reference >= ImmediateEnd ||
			ImmediateBegin >= ImmediateEnd ||
			ImmediateEnd - ImmediateBegin > 1024 * 1024)
		{
			return nullptr;
		}

		for (unsigned Depth = 0; Depth < 8; ++Depth)
		{
			const DWORD UnwindRva = Current.UnwindData & ~1u;
			const auto* Unwind = reinterpret_cast<const uint8*>(
				RuntimeImageBase + UnwindRva);
			if (!UnwindRva || !IsReadableMemoryRange(Unwind, 4))
				return nullptr;
			const uint8 Version = Unwind[0] & 0x07;
			const uint8 Flags = Unwind[0] >> 3;
			const uint8 CodeCount = Unwind[2];
			if (Version != 1)
				return nullptr;
			constexpr uint8 ChainedInfo = 0x04;
			if ((Flags & ChainedInfo) == 0)
				break;
			const size_t ChainOffset = 4 +
				((static_cast<size_t>(CodeCount) + 1) & ~size_t(1)) * 2;
			const auto* Chained =
				reinterpret_cast<const RUNTIME_FUNCTION*>(
					Unwind + ChainOffset);
			if (!IsReadableMemoryRange(Chained, sizeof(*Chained)) ||
				Chained->BeginAddress >= Chained->EndAddress)
			{
				return nullptr;
			}
			Current = *Chained;
		}

		const DWORD64 Begin = RuntimeImageBase + Current.BeginAddress;
		const DWORD64 End = RuntimeImageBase + Current.EndAddress;
		if (Begin >= End || End - Begin > 1024 * 1024 ||
			!IsExecutableAddress(reinterpret_cast<void*>(Begin)))
		{
			return nullptr;
		}
		return reinterpret_cast<void*>(Begin);
	}

	struct FNativeAnchorRoots
	{
		const wchar_t* WideText = nullptr;
		const char* NarrowText = nullptr;
		size_t WideBytes = 0;
		size_t NarrowBytes = 0;
		uint64 WidePrefix = 0;
		uint64 NarrowPrefix = 0;
		std::unordered_set<void*> Roots;
		bool bOverflowed = false;
	};

	bool ReadAnchorPrefix(uintptr_t Target, uint64& Prefix) noexcept
	{
		Prefix = 0;
		if (!Target)
			return false;
		__try
		{
			memcpy(&Prefix, reinterpret_cast<const void*>(Target),
				sizeof(Prefix));
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool CollectNativeAnchorRoots(
		FNativeAnchorRoots* Anchors,
		size_t AnchorCount) noexcept
	{
		if (!Anchors || !AnchorCount)
			return false;
		constexpr size_t MaximumRootsPerAnchor = 64;
		__try
		{
			const auto Module = reinterpret_cast<const uint8*>(
				GetModuleHandleW(nullptr));
			if (!Module)
				return false;
			const auto* Dos =
				reinterpret_cast<const IMAGE_DOS_HEADER*>(Module);
			if (Dos->e_magic != IMAGE_DOS_SIGNATURE || Dos->e_lfanew <= 0)
				return false;
			const auto* Nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
				Module + Dos->e_lfanew);
			if (Nt->Signature != IMAGE_NT_SIGNATURE ||
				Nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
				Nt->FileHeader.NumberOfSections == 0 ||
				Nt->FileHeader.NumberOfSections > 96 ||
				Nt->OptionalHeader.SizeOfImage < 0x1000)
			{
				return false;
			}
			const size_t ImageSize = Nt->OptionalHeader.SizeOfImage;
			const uintptr_t ImageBegin =
				reinterpret_cast<uintptr_t>(Module);
			const uintptr_t ImageEnd = ImageBegin + ImageSize;
			if (ImageEnd <= ImageBegin)
				return false;

			for (size_t Index = 0; Index < AnchorCount; ++Index)
			{
				auto& Anchor = Anchors[Index];
				const bool bHasWide = Anchor.WideText && *Anchor.WideText;
				const bool bHasNarrow =
					Anchor.NarrowText && *Anchor.NarrowText;
				if (!bHasWide && !bHasNarrow)
					return false;
				if (bHasWide)
				{
					Anchor.WideBytes =
						(wcslen(Anchor.WideText) + 1) * sizeof(wchar_t);
					if (Anchor.WideBytes < sizeof(uint64))
						return false;
					memcpy(&Anchor.WidePrefix,
						Anchor.WideText, sizeof(uint64));
				}
				if (bHasNarrow)
				{
					Anchor.NarrowBytes = strlen(Anchor.NarrowText) + 1;
					if (Anchor.NarrowBytes < sizeof(uint64))
						return false;
					memcpy(&Anchor.NarrowPrefix,
						Anchor.NarrowText, sizeof(uint64));
				}
			}

			const IMAGE_SECTION_HEADER* Section = IMAGE_FIRST_SECTION(Nt);
			for (WORD SectionIndex = 0;
				SectionIndex < Nt->FileHeader.NumberOfSections;
				++SectionIndex, ++Section)
			{
				if ((Section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
					continue;
				const size_t SectionRva = Section->VirtualAddress;
				const size_t SectionSize = (std::min)(
					static_cast<size_t>(Section->Misc.VirtualSize),
					ImageSize > SectionRva ? ImageSize - SectionRva : 0);
				if (SectionSize < 7 || SectionRva >= ImageSize)
					continue;
				const uint8* Code = Module + SectionRva;
				if (!IsReadableMemoryRange(Code, SectionSize))
					continue;

				for (size_t Offset = 0; Offset + 7 <= SectionSize; ++Offset)
				{
					const uint8 Rex = Code[Offset];
					if ((Rex & 0xf8) != 0x48 ||
						Code[Offset + 1] != 0x8d ||
						(Code[Offset + 2] & 0xc7) != 0x05)
					{
						continue;
					}
					int32 Displacement = 0;
					memcpy(&Displacement, Code + Offset + 3,
						sizeof(Displacement));
					const uintptr_t InstructionEnd =
						reinterpret_cast<uintptr_t>(Code + Offset + 7);
					const uintptr_t Target = static_cast<uintptr_t>(
						static_cast<intptr_t>(InstructionEnd) + Displacement);
					if (Target < ImageBegin || Target >= ImageEnd)
						continue;
					uint64 TargetPrefix = 0;
					if (!ReadAnchorPrefix(Target, TargetPrefix))
						continue;

					for (size_t AnchorIndex = 0;
						AnchorIndex < AnchorCount; ++AnchorIndex)
					{
						auto& Anchor = Anchors[AnchorIndex];
						if (Anchor.bOverflowed)
							continue;
						const bool bWideMatch = Anchor.WideBytes &&
							TargetPrefix == Anchor.WidePrefix &&
							Target <= ImageEnd - Anchor.WideBytes &&
							IsReadableMemoryRange(
								reinterpret_cast<const void*>(Target),
								Anchor.WideBytes) &&
							memcmp(reinterpret_cast<const void*>(Target),
								Anchor.WideText, Anchor.WideBytes) == 0;
						const bool bNarrowMatch = Anchor.NarrowBytes &&
							TargetPrefix == Anchor.NarrowPrefix &&
							Target <= ImageEnd - Anchor.NarrowBytes &&
							IsReadableMemoryRange(
								reinterpret_cast<const void*>(Target),
								Anchor.NarrowBytes) &&
							memcmp(reinterpret_cast<const void*>(Target),
								Anchor.NarrowText, Anchor.NarrowBytes) == 0;
						if (!bWideMatch && !bNarrowMatch)
							continue;
						void* Root = ResolveFunctionContainingReference(
							reinterpret_cast<uintptr_t>(Code + Offset));
						if (!Root)
							continue;
						if (Anchor.Roots.size() >= MaximumRootsPerAnchor &&
							!Anchor.Roots.contains(Root))
						{
							Anchor.bOverflowed = true;
							Anchor.Roots.clear();
							continue;
						}
						Anchor.Roots.insert(Root);
					}
				}
			}
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	void* ResolveUniqueSharedRoot(
		const FNativeAnchorRoots* const* Anchors,
		size_t Count) noexcept
	{
		if (!Anchors || Count < 2 || !Anchors[0] ||
			Anchors[0]->bOverflowed)
		{
			return nullptr;
		}
		void* Result = nullptr;
		for (void* Candidate : Anchors[0]->Roots)
		{
			bool bShared = true;
			for (size_t Index = 1; Index < Count; ++Index)
			{
				if (!Anchors[Index] || Anchors[Index]->bOverflowed ||
					!Anchors[Index]->Roots.contains(Candidate))
				{
					bShared = false;
					break;
				}
			}
			if (!bShared)
				continue;
			if (Result && Result != Candidate)
				return nullptr;
			Result = Candidate;
		}
		return Result;
	}

	void* ResolveUniqueVotedRoot(
		const FNativeAnchorRoots* const* Anchors,
		size_t Count,
		size_t RequiredVotes) noexcept
	{
		if (!Anchors || Count < 2 || RequiredVotes < 2 ||
			RequiredVotes > Count)
		{
			return nullptr;
		}
		for (size_t Index = 0; Index < Count; ++Index)
		{
			if (!Anchors[Index] || Anchors[Index]->bOverflowed)
				return nullptr;
		}

		void* Result = nullptr;
		for (size_t SourceIndex = 0; SourceIndex < Count; ++SourceIndex)
		{
			for (void* Candidate : Anchors[SourceIndex]->Roots)
			{
				bool bAlreadyVisited = false;
				for (size_t Previous = 0; Previous < SourceIndex; ++Previous)
				{
					if (Anchors[Previous]->Roots.contains(Candidate))
					{
						bAlreadyVisited = true;
						break;
					}
				}
				if (bAlreadyVisited)
					continue;
				size_t Votes = 0;
				for (size_t Index = 0; Index < Count; ++Index)
				{
					if (Anchors[Index]->Roots.contains(Candidate))
						++Votes;
				}
				if (Votes < RequiredVotes)
					continue;
				if (Result && Result != Candidate)
					return nullptr;
				Result = Candidate;
			}
		}
		return Result;
	}

	const char* DescribePatchResolution(unsigned Schemes) noexcept
	{
		switch (Schemes)
		{
		case 0x1: return "stable";
		case 0x2: return "legacy";
		case 0x3: return "stable+legacy";
		case 0x4: return "current";
		case 0x5: return "stable+current";
		case 0x6: return "legacy+current";
		case 0x7: return "stable+legacy+current";
		default: return "no-agreement";
		}
	}

	void ResolveNativeHotfixTargets(
		uintptr_t& OutHotfixIni,
		uintptr_t& OutPatchAssets)
	{
		OutHotfixIni = 0;
		OutPatchAssets = 0;
		FNativeAnchorRoots Anchors[] =
		{
			{ L"Specified per-object class %s was not found",
				"Specified per-object class %s was not found" },
			{ L"Updating config from %s took %f seconds and reloaded %d objects",
				"Updating config from %s took %f seconds and reloaded %d objects" },
			{ L"[AssetHotfix]", "[AssetHotfix]" },
			{ L"[/Script/", "[/Script/" },
			{ L"[/Game/", "[/Game/" },
			{ L"UOnlineHotfixManager::PatchAssetsFromIniFiles",
				"UOnlineHotfixManager::PatchAssetsFromIniFiles" },
			{ L"Expected a hotfix type of RowUpdate with 5 tokens or TableUpdate with 3 tokens.",
				"Expected a hotfix type of RowUpdate with 5 tokens or TableUpdate with 3 tokens." },
			{ L"Checking for assets to be patched using data from 'AssetHotfix' section in the Game .ini file",
				"Checking for assets to be patched using data from 'AssetHotfix' section in the Game .ini file" },
			{ L"Wasn't able to parse the data with semicolon separated values. Expecting 3 or 5 arguments.",
				"Wasn't able to parse the data with semicolon separated values. Expecting 3 or 5 arguments." },
			{ L"No assets were found in the 'AssetHotfix' section in the Game .ini file.  No patching needed.",
				"No assets were found in the 'AssetHotfix' section in the Game .ini file.  No patching needed." },
			{ L"Wasn't able to parse the data with semicolon separated values. Expecting 3 or 5 arguments but parsed %d.",
				"Wasn't able to parse the data with semicolon separated values. Expecting 3 or 5 arguments but parsed %d." },
			{ L"No assets were found in the 'AssetHotfix' section in the Game .ini file. No patching needed.",
				"No assets were found in the 'AssetHotfix' section in the Game .ini file. No patching needed." }
		};
		GPatchAssetsResolutionDiagnostic = "scan-failed";
		if (!CollectNativeAnchorRoots(Anchors, _countof(Anchors)))
			return;

		const FNativeAnchorRoots* IniPrimary[] =
			{ &Anchors[0], &Anchors[1] };
		OutHotfixIni = reinterpret_cast<uintptr_t>(
			ResolveUniqueSharedRoot(IniPrimary, _countof(IniPrimary)));
		if (!OutHotfixIni)
		{
			const FNativeAnchorRoots* IniFallback[] =
				{ &Anchors[2], &Anchors[3], &Anchors[4] };
			OutHotfixIni = reinterpret_cast<uintptr_t>(
				ResolveUniqueSharedRoot(
					IniFallback, _countof(IniFallback)));
		}

		const FNativeAnchorRoots* StablePatch[] =
			{ &Anchors[5], &Anchors[6] };
		const FNativeAnchorRoots* LegacyPatch[] =
			{ &Anchors[7], &Anchors[8], &Anchors[9] };
		const FNativeAnchorRoots* CurrentPatch[] =
			{ &Anchors[7], &Anchors[10], &Anchors[11] };
		void* Stable = ResolveUniqueSharedRoot(
			StablePatch, _countof(StablePatch));
		void* Legacy = ResolveUniqueVotedRoot(
			LegacyPatch, _countof(LegacyPatch), 2);
		void* Current = ResolveUniqueVotedRoot(
			CurrentPatch, _countof(CurrentPatch), 2);
		void* ResolvedPatch = nullptr;
		unsigned Schemes = 0;
		bool bConflict = false;
		const auto Merge = [
			&ResolvedPatch, &Schemes, &bConflict](
				void* Candidate, unsigned Scheme)
		{
			if (!Candidate || bConflict)
				return;
			if (ResolvedPatch && ResolvedPatch != Candidate)
			{
				ResolvedPatch = nullptr;
				bConflict = true;
				return;
			}
			ResolvedPatch = Candidate;
			Schemes |= Scheme;
		};
		Merge(Stable, 0x1);
		Merge(Legacy, 0x2);
		Merge(Current, 0x4);
		OutPatchAssets = bConflict
			? 0
			: reinterpret_cast<uintptr_t>(ResolvedPatch);
		GPatchAssetsResolutionDiagnostic = bConflict
			? "conflict"
			: DescribePatchResolution(Schemes);
		if (OutPatchAssets == OutHotfixIni)
		{
			OutPatchAssets = 0;
			GPatchAssetsResolutionDiagnostic = "target-collision";
		}
	}

	bool ReadVtableEntryGuarded(
		const UObject* Object,
		size_t Index,
		void*& OutEntry)
	{
		OutEntry = nullptr;
		if (!Object || Index >= 512)
			return false;
		++GGuardedNativeCallDepth;
		bool bResult = false;
		__try
		{
			if (Object->Vft &&
				SDK::MemReadable(Object->Vft + Index, sizeof(void*)))
			{
				OutEntry = Object->Vft[Index];
				bResult = OutEntry != nullptr;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
		--GGuardedNativeCallDepth;
		return bResult;
	}

	UObject* GetDefaultObjectGuarded(const UClass* ObjectClass)
	{
		if (!ObjectClass)
			return nullptr;
		UObject* Result = nullptr;
		++GGuardedNativeCallDepth;
		__try
		{
			Result = const_cast<UClass*>(ObjectClass)->GetDefaultObj();
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			Result = nullptr;
		}
		--GGuardedNativeCallDepth;
		return Result;
	}

	void EnsureManagerIniObservationInternal(
		const UObject* Manager,
		const UClass* BaseManagerClass)
	{
		const uint64 ManagerIdentity = GetObjectIdentityGuarded(Manager);
		if (!GOriginalBaseHotfixIniFile || !GBaseHotfixIniFileTarget ||
			!ManagerIdentity || !IsUsableObject(Manager) ||
			!IsUsableObject(BaseManagerClass) ||
			GIniReadyManagerIdentity.load(std::memory_order_acquire) ==
				ManagerIdentity)
		{
			return;
		}

		UObject* BaseDefault = GetDefaultObjectGuarded(BaseManagerClass);
		if (!IsUsableObject(BaseDefault))
			return;

		size_t Slot = 512;
		for (size_t Index = 0; Index < 512; ++Index)
		{
			void* Entry = nullptr;
			if (!ReadVtableEntryGuarded(BaseDefault, Index, Entry))
				break;
			if (reinterpret_cast<uintptr_t>(Entry) ==
				GBaseHotfixIniFileTarget)
			{
				Slot = Index;
				break;
			}
		}
		if (Slot >= 512)
			return;

		void* DispatchTarget = nullptr;
		if (!ReadVtableEntryGuarded(Manager, Slot, DispatchTarget) ||
			!IsExecutableAddress(DispatchTarget))
		{
			return;
		}
		if (reinterpret_cast<uintptr_t>(DispatchTarget) ==
			GBaseHotfixIniFileTarget)
		{
			GIniReadyManagerIdentity.store(
				ManagerIdentity, std::memory_order_release);
			return;
		}

		FExclusiveLock InstallLock(GManagerHookInstallLock);
		if (GManagerHotfixIniFileTarget ==
			reinterpret_cast<uintptr_t>(DispatchTarget) &&
			GOriginalManagerHotfixIniFile)
		{
			GIniReadyManagerIdentity.store(
				ManagerIdentity, std::memory_order_release);
			return;
		}
		if (GManagerHotfixIniFileTarget)
			return;

		const MH_STATUS CreateStatus = MH_CreateHook(
			DispatchTarget,
			reinterpret_cast<LPVOID>(&ManagerHotfixIniFileDetour),
			reinterpret_cast<LPVOID*>(&GOriginalManagerHotfixIniFile));
		if (CreateStatus != MH_OK || !GOriginalManagerHotfixIniFile)
		{
			GOriginalManagerHotfixIniFile = nullptr;
			if (!GReportedManagerIniHookFailure)
			{
				GReportedManagerIniHookFailure = true;
				SDK::DbgLog(
					"[AssetHotfixReload] derived HotfixIniFile hook "
					"unavailable (%s); added polling remains disabled\n",
					MH_StatusToString(CreateStatus));
			}
			return;
		}
		const MH_STATUS EnableStatus = MH_EnableHook(DispatchTarget);
		if (EnableStatus != MH_OK && EnableStatus != MH_ERROR_ENABLED)
		{
			MH_RemoveHook(DispatchTarget);
			GOriginalManagerHotfixIniFile = nullptr;
			return;
		}
		GManagerHotfixIniFileTarget =
			reinterpret_cast<uintptr_t>(DispatchTarget);
		GIniReadyManagerIdentity.store(
			ManagerIdentity, std::memory_order_release);
		SDK::DbgLog(
			"[AssetHotfixReload] derived HotfixIniFile dispatch observed "
			"at resolved base slot %zu\n", Slot);
	}

	class FAssetHotfixReloadHookInstaller
	{
	public:
		InitHooks;
	};

	void FAssetHotfixReloadHookInstaller::Hook()
	{
		uintptr_t IniTarget = 0;
		uintptr_t PatchTarget = 0;
		ResolveNativeHotfixTargets(IniTarget, PatchTarget);
		auto ReportReadiness = [IniTarget, PatchTarget]()
		{
			WriteStartupDiagnostic(
				"hotfix-server hook-readiness build=%s_%s "
				"mode=delta-native-captured-baseline-rollback "
				"payload=accepted-defaultgame-exact patch-scheme=%s "
				"patch-target=%p patch-enabled=%u ini-target=%p ini-enabled=%u",
				__DATE__, __TIME__, GPatchAssetsResolutionDiagnostic,
				reinterpret_cast<void*>(PatchTarget),
				GBasePatchAssetsHookEnabled.load(
					std::memory_order_acquire) ? 1u : 0u,
				reinterpret_cast<void*>(IniTarget),
				GBaseHotfixIniHookEnabled.load(
					std::memory_order_acquire) ? 1u : 0u);
		};
		if (!IsExecutableAddress(reinterpret_cast<void*>(PatchTarget)) ||
			!IsExecutableAddress(reinterpret_cast<void*>(IniTarget)) ||
			PatchTarget == IniTarget)
		{
			ReportReadiness();
			SDK::DbgLog(
				"[AssetHotfixReload] exact-once targets unresolved "
				"patch-scheme=%s; added polling is inert\n",
				GPatchAssetsResolutionDiagnostic);
			return;
		}

		const MH_STATUS PatchCreateStatus = MH_CreateHook(
			reinterpret_cast<LPVOID>(PatchTarget),
			reinterpret_cast<LPVOID>(&PatchAssetsFromIniFilesDetour),
			reinterpret_cast<LPVOID*>(
				&GOriginalPatchAssetsFromIniFiles));
		if (PatchCreateStatus != MH_OK ||
			!GOriginalPatchAssetsFromIniFiles)
		{
			GOriginalPatchAssetsFromIniFiles = nullptr;
			ReportReadiness();
			return;
		}
		GBasePatchAssetsFromIniFilesTarget = PatchTarget;
		const MH_STATUS PatchEnableStatus = MH_EnableHook(
			reinterpret_cast<LPVOID>(PatchTarget));
		if (PatchEnableStatus != MH_OK &&
			PatchEnableStatus != MH_ERROR_ENABLED)
		{
			ReportReadiness();
			return;
		}
		GBasePatchAssetsHookEnabled.store(true, std::memory_order_release);

		const MH_STATUS IniCreateStatus = MH_CreateHook(
			reinterpret_cast<LPVOID>(IniTarget),
			reinterpret_cast<LPVOID>(&BaseHotfixIniFileDetour),
			reinterpret_cast<LPVOID*>(&GOriginalBaseHotfixIniFile));
		if (IniCreateStatus != MH_OK || !GOriginalBaseHotfixIniFile)
		{
			GOriginalBaseHotfixIniFile = nullptr;
			ReportReadiness();
			return;
		}
		GBaseHotfixIniFileTarget = IniTarget;
		const MH_STATUS IniEnableStatus = MH_EnableHook(
			reinterpret_cast<LPVOID>(IniTarget));
		if (IniEnableStatus != MH_OK && IniEnableStatus != MH_ERROR_ENABLED)
		{
			ReportReadiness();
			return;
		}
		GBaseHotfixIniHookEnabled.store(true, std::memory_order_release);
		ReportReadiness();
		SDK::DbgLog(
			"[AssetHotfixReload] delta-native exact-once hooks enabled "
			"patch-scheme=%s\n",
			GPatchAssetsResolutionDiagnostic);
	}
}

void AssetHotfixRollback::OnWorldChanged(std::uint64_t Now)
{
	(void)Now;
	auto& State = GetExactOnceState();
	FExclusiveLock Lock(State.Lock);
	AdvanceNonZero(State.ManagerAuthoritySerial);
	InvalidatePlannerPreparationLocked(State);
	State.ManagerIdentity = 0;
	ClearArmedRefreshLocked(State);
	ClearExpectedPatchLocked(State);
	ClearPreparedRevisionLocked(State);
	State.ActiveTrackedIniDispatches = 0;
	State.ActiveTrackedPatchDispatches = 0;
	ClearUnreliableRecycleLocked(State);
	State.RecoveryCooldownDeadlineMs = 0;
	State.LastAppliedPayload.reset();
	InvalidateSamplingTrustLocked(State);
	State.bReportedFirstAppliedPass = false;
	State.bReportedFirstUnchangedSkip = false;
	State.RecoveryDiagnosticCount = 0;
	GIniReadyManagerIdentity.store(0, std::memory_order_release);
}

void AssetHotfixRollback::EnsureManagerIniObservation(
	const UObject* Manager,
	const UClass* BaseManagerClass)
{
	const uint64 ManagerIdentity = GetObjectIdentityGuarded(Manager);
	{
		auto& State = GetExactOnceState();
		FExclusiveLock Lock(State.Lock);
		if (State.ManagerIdentity != ManagerIdentity)
		{
			AdvanceNonZero(State.ManagerAuthoritySerial);
			InvalidatePlannerPreparationLocked(State);
			State.ManagerIdentity = ManagerIdentity;
			ClearArmedRefreshLocked(State);
			ClearExpectedPatchLocked(State);
			ClearPreparedRevisionLocked(State);
			State.ActiveTrackedIniDispatches = 0;
			State.ActiveTrackedPatchDispatches = 0;
			ClearUnreliableRecycleLocked(State);
			State.RecoveryCooldownDeadlineMs = 0;
			State.LastAppliedPayload.reset();
			InvalidateSamplingTrustLocked(State);
			State.bReportedFirstAppliedPass = false;
			State.bReportedFirstUnchangedSkip = false;
			State.RecoveryDiagnosticCount = 0;
			GIniReadyManagerIdentity.store(0, std::memory_order_release);
		}
	}
	EnsureManagerIniObservationInternal(Manager, BaseManagerClass);
}

bool AssetHotfixRollback::IsExactOncePeriodicRefreshReady(
	const UObject* Manager)
{
	const uint64 ManagerIdentity = GetObjectIdentityGuarded(Manager);
	if (!ManagerIdentity || !GOriginalPatchAssetsFromIniFiles ||
		!GOriginalBaseHotfixIniFile ||
		!GBasePatchAssetsHookEnabled.load(std::memory_order_acquire) ||
		!GBaseHotfixIniHookEnabled.load(std::memory_order_acquire) ||
		GIniReadyManagerIdentity.load(std::memory_order_acquire) !=
			ManagerIdentity)
	{
		return false;
	}
	auto& State = GetExactOnceState();
	FExclusiveLock Lock(State.Lock);
	return State.ManagerIdentity == ManagerIdentity;
}

bool AssetHotfixRollback::IsRefreshInFlight(
	const UObject* Manager,
	std::uint64_t Now,
	bool bNativeBusyStateReliable,
	bool bNativeHotfixBusy)
{
	const uint64 ManagerIdentity = GetObjectIdentityGuarded(Manager);
	if (!ManagerIdentity)
		return false;
	bool bReportRecoveredTimeout = false;
	bool bInFlight = false;
	{
		auto& State = GetExactOnceState();
		FExclusiveLock Lock(State.Lock);
		if (State.ManagerIdentity != ManagerIdentity)
			return false;

		if (State.ExpectedPublicationSerial &&
			Now > State.ExpectedPatchDeadlineMs)
		{
			// The native callback can legitimately cross ticks or arrive in an
			// unexpected order on older branches. Retire our suppression token,
			// preserve stock behavior for any late boundary, and resume only after
			// a quiet cooldown instead of disabling all later edits.
			bReportRecoveredTimeout = BeginCorrelationRecoveryLocked(
				State, ManagerIdentity, Now);
		}

		const bool bNativeBoundaryActive =
			State.ActiveTrackedIniDispatches != 0 ||
			State.ActiveTrackedPatchDispatches != 0;
		if (bNativeBoundaryActive || State.bPlannerPreparationActive)
		{
			bInFlight = true;
		}

		if (State.ArmedRefreshSequence &&
			bNativeBusyStateReliable && bNativeHotfixBusy)
		{
			// Positive native busy state, not an arbitrary asset boundary, keeps
			// the request's acceptance deadline live.
			State.ArmedRefreshDeadlineMs =
				Now + RefreshAcceptanceWindowMs;
			bInFlight = true;
		}
		else if (!bNativeBoundaryActive && State.ArmedRefreshSequence)
		{
			if (bNativeBusyStateReliable)
			{
				// Reflected false is positive completion evidence for a valid
				// unchanged/no-callback pass. Leave an expired tombstone until the
				// next begin is committed atomically, so a late callback cannot fall
				// into a check-to-arm gap.
				const DWORD RefreshThreadId =
					State.ArmedRefreshThreadId;
				ClearArmedRefreshLocked(State);
				State.UnreliableNoCallbackRecycleDeadlineMs =
					Now + ReliableNoCallbackRecycleMs;
				State.UnreliableNoCallbackRecycleThreadId =
					RefreshThreadId;
				bInFlight = true;
			}
			else if (Now > State.ArmedRefreshDeadlineMs)
			{
				// With a private busy bit there is no positive completion signal.
				// Keep a short bounded recycle before permitting the next request.
				const DWORD RefreshThreadId =
					State.ArmedRefreshThreadId;
				ClearArmedRefreshLocked(State);
				State.UnreliableNoCallbackRecycleDeadlineMs =
					Now + UnreliableNoCallbackRecycleMs;
				State.UnreliableNoCallbackRecycleThreadId =
					RefreshThreadId;
				bInFlight = true;
			}
			else
			{
				bInFlight = true;
			}
		}

		if (!bNativeBoundaryActive &&
			State.UnreliableNoCallbackRecycleDeadlineMs)
		{
			if (bNativeBusyStateReliable && bNativeHotfixBusy)
				bInFlight = true;
			else if (Now <
				State.UnreliableNoCallbackRecycleDeadlineMs)
				bInFlight = true;
		}

		if (State.RecoveryCooldownDeadlineMs)
		{
			if (bNativeBusyStateReliable && bNativeHotfixBusy)
			{
				State.RecoveryCooldownDeadlineMs = (std::max)(
					State.RecoveryCooldownDeadlineMs,
					Now + CorrelationRecoveryCooldownMs);
				bInFlight = true;
			}
			else if (Now < State.RecoveryCooldownDeadlineMs)
			{
				bInFlight = true;
			}
		}

		if (State.ExpectedPublicationSerial ||
			(bNativeBusyStateReliable && bNativeHotfixBusy))
		{
			bInFlight = true;
		}
	}

	if (bReportRecoveredTimeout)
	{
		SDK::DbgLog(
			"[AssetHotfixReload] accepted DefaultGame did not reach "
			"PatchAssets within the correlation window; its late stock work "
			"will be forwarded and periodic polling will retry\n");
	}
	return bInFlight;
}

std::uint64_t AssetHotfixRollback::TryBeginRefresh(
	const UObject* Manager,
	std::uint64_t Now)
{
	const uint64 ManagerIdentity = GetObjectIdentityGuarded(Manager);
	auto& State = GetExactOnceState();
	FExclusiveLock Lock(State.Lock);
	if (!ManagerIdentity || State.ManagerIdentity != ManagerIdentity ||
		State.ArmedRefreshSequence ||
		State.ExpectedPublicationSerial ||
		State.PreparedRevision ||
		State.bPlannerPreparationActive ||
		State.ActiveTrackedIniDispatches ||
		State.ActiveTrackedPatchDispatches ||
		(State.RecoveryCooldownDeadlineMs &&
			Now < State.RecoveryCooldownDeadlineMs) ||
		(State.UnreliableNoCallbackRecycleDeadlineMs &&
			Now < State.UnreliableNoCallbackRecycleDeadlineMs))
	{
		return 0;
	}
	// Retire only an expired recycle tombstone in the same critical section that
	// installs its successor arm. Callbacks before this point see the tombstone;
	// callbacks after it see the new sequence.
	ClearUnreliableRecycleLocked(State);
	State.RecoveryCooldownDeadlineMs = 0;
	AdvanceNonZero(State.RefreshSequenceSerial);
	State.ArmedRefreshSequence = State.RefreshSequenceSerial;
	State.ArmedManagerIdentity = ManagerIdentity;
	State.ArmedRefreshDeadlineMs = Now + RefreshAcceptanceWindowMs;
	State.ArmedRefreshThreadId = GetCurrentThreadId();
	return State.ArmedRefreshSequence;
}

void AssetHotfixRollback::OnRefreshStartFailed(
	const UObject* Manager,
	std::uint64_t RefreshSequence)
{
	const uint64 ManagerIdentity = GetObjectIdentityGuarded(Manager);
	auto& State = GetExactOnceState();
	FExclusiveLock Lock(State.Lock);
	if (!RefreshSequence ||
		State.ArmedRefreshSequence != RefreshSequence ||
		State.ArmedManagerIdentity != ManagerIdentity ||
		State.ManagerIdentity != ManagerIdentity)
	{
		return;
	}
	ClearArmedRefreshLocked(State);
}
