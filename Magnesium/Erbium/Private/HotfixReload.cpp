#include "pch.h"
#include "../Public/HotfixReload.h"
#include "../Public/AssetHotfixRollback.h"

#include "../../Engine/Public/NetDriver.h"
#include "../Support/Public/FaultGuard.h"
#include "../Support/Public/VersionFeatureAdapter.h"

namespace
{
	// The one-second cadence runs only the engine's lightweight title-file
	// revision check. AssetHotfixRollback skips PatchAssets before its row walk
	// when the accepted DefaultGame bytes are unchanged.
	constexpr ULONGLONG InitialRefreshDelayMs = 1000ULL;
	constexpr ULONGLONG RefreshIntervalMs = 1000ULL;
	constexpr ULONGLONG InFlightRecheckMs = 100ULL;
	constexpr ULONGLONG ClassCycleRetryMs = 15000ULL;
	constexpr ULONGLONG ManagerScanRetryMs = 3000ULL;
	constexpr ULONGLONG ManagerScanMaxRetryMs = 30000ULL;
	constexpr ULONGLONG ManagerObjectGrowthCheckMs = 500ULL;
	constexpr ULONGLONG InvocationRetryMs = 5000ULL;
	constexpr int32 ClassObjectsPerTick = 1024;
	constexpr int32 ManagerObjectsPerTick = 512;
	constexpr uint32 MaximumInvocationFaults = 3;
	constexpr int32 InvalidInternalObjectFlags = 0x20;
	constexpr int32 DestroyedObjectFlags = 0x01800000;
	static_assert(ClassObjectsPerTick > 0 && ClassObjectsPerTick <= 1024);
	static_assert(ManagerObjectsPerTick > 0 && ManagerObjectsPerTick <= 1024);

	constexpr const wchar_t* ManagerClassNames[] =
	{
		L"OnlineHotfixManager",
		L"FortOnlineHotfixManager",
		L"FortHotfixManager"
	};
	constexpr uint32 ManagerClassNameCount =
		sizeof(ManagerClassNames) / sizeof(ManagerClassNames[0]);

	struct FReflectedBoolField
	{
		int32 Offset = -1;
		uint8 Mask = 0;
	};

	struct FHotfixReloadState
	{
		const UWorld* World = nullptr;
		const UNetDriver* Driver = nullptr;
		const UClass* ManagerClass = nullptr;
		const UObject* Manager = nullptr;
		UFunction* StartHotfixProcess = nullptr;
		ULONGLONG NextRefreshMs = 0;
		ULONGLONG NextClassResolveMs = 0;
		ULONGLONG NextManagerScanMs = 0;
		ULONGLONG NextManagerFullScanMs = 0;
		ULONGLONG NextManagerGrowthCheckMs = 0;
		FName ClassMetaName{};
		FName ManagerClassFNames[ManagerClassNameCount]{};
		const UClass* DiscoveredManagerClasses[ManagerClassNameCount]{};
		FReflectedBoolField HotfixingInProgressField{};
		int32 ClassScanIndex = -1;
		int32 ManagerScanIndex = -1;
		int32 ManagerScanFloorIndex = 0;
		int32 ManagerScanSnapshotObjectCount = 0;
		int32 LastCompletedManagerScanObjectCount = 0;
		uint32 ManagerScanMissCount = 0;
		uint32 InvocationFaultCount = 0;
		uint64 InvocationFaultManagerIdentity = 0;
		uint64 HotfixingInProgressManagerIdentity = 0;
		bool bClassNamesInitialized = false;
		bool bHotfixingInProgressFieldResolved = false;
		bool bReportedUnavailable = false;
		bool bReportedFirstRefresh = false;
		bool bReportedExactOnceUnavailable = false;
		bool bInvocationActive = false;
		bool bDisabledForGeneration = false;
		bool bManagerScanBackoffActive = false;
		bool bManagerGrowthScanActive = false;
	};

	FHotfixReloadState GHotfixReloadState{};

	bool IsUsableObject(const UObject* Object)
	{
		return VersionFeatureAdapter::IsLiveObject(Object) &&
			!(Object->ObjectFlags & DestroyedObjectFlags);
	}

	bool IsUsableManager(
		const UObject* Manager, const UClass* ManagerClass)
	{
		return IsUsableObject(Manager) &&
			IsUsableObject(ManagerClass) &&
			!Manager->IsDefaultObject() &&
			Manager->IsA(ManagerClass);
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
					(static_cast<uint64>(
						static_cast<uint32>(Index)) << 32) |
					static_cast<uint32>(Item->SerialNumber);
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
		--GGuardedNativeCallDepth;
		return Result;
	}

	bool ResolveReflectedBoolFieldGuarded(
		const UObject* Object,
		const char* PropertyName,
		FReflectedBoolField& OutField,
		bool& bCompleted)
	{
		OutField = {};
		bCompleted = false;
		if (!IsUsableObject(Object) || !PropertyName || !Object->Class ||
			Offsets::ElementSize < sizeof(int32))
		{
			return false;
		}

		++GGuardedNativeCallDepth;
		bool bResult = false;
		__try
		{
			const UField* Property =
				Object->GetProperty(PropertyName, 0x20000);
			if (!Property)
			{
				// A missing optional busy flag is a supported schema outcome.
				bCompleted = true;
			}
			else
			{
				uint32 RequiredMetadataOffset = Offsets::Offset_Internal;
				if (Offsets::ElementSize > RequiredMetadataOffset)
					RequiredMetadataOffset = Offsets::ElementSize;
				if (Offsets::FieldMask > RequiredMetadataOffset)
					RequiredMetadataOffset = Offsets::FieldMask;

				if (SDK::MemReadable(
						Property,
						static_cast<size_t>(RequiredMetadataOffset) +
							sizeof(uint32)))
				{
					const uint32 Offset = SDK::ReadPropertyOffset(
						GetFromOffset<uint32>(
							Property, Offsets::Offset_Internal));
					const uint32 ElementSize = GetFromOffset<uint32>(
						Property, Offsets::ElementSize);
					const int32 ArrayDimension = GetFromOffset<int32>(
						Property,
						Offsets::ElementSize - sizeof(int32));
					const uint8 FieldMask = Property->GetFieldMask();
					const int32 ObjectSize =
						Object->Class->GetPropertiesSize();
					if (Offset != static_cast<uint32>(-1) &&
						Offset < 0x10000 &&
						ElementSize == sizeof(uint8) &&
						ArrayDimension == 1 && FieldMask &&
						ObjectSize > 0 &&
						Offset < static_cast<uint32>(ObjectSize) &&
						SDK::MemReadable(
							reinterpret_cast<const uint8*>(Object) +
								Offset,
							sizeof(uint8)))
					{
						OutField.Offset = static_cast<int32>(Offset);
						OutField.Mask = FieldMask;
						bResult = true;
					}
				}
				bCompleted = bResult;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
		--GGuardedNativeCallDepth;
		return bResult;
	}

	bool ReadReflectedBoolFieldGuarded(
		const UObject* Object,
		const FReflectedBoolField& Field,
		bool& OutValue)
	{
		OutValue = false;
		if (Field.Offset < 0)
			return true;
		if (!IsUsableObject(Object) || !Field.Mask)
			return false;

		++GGuardedNativeCallDepth;
		bool bResult = false;
		__try
		{
			const int32 ObjectSize = Object->Class
				? Object->Class->GetPropertiesSize()
				: 0;
			const uint8* Address =
				reinterpret_cast<const uint8*>(Object) + Field.Offset;
			if (ObjectSize > 0 && Field.Offset < ObjectSize &&
				SDK::MemReadable(Address, sizeof(uint8)))
			{
				OutValue = (*Address & Field.Mask) != 0;
				bResult = true;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
		--GGuardedNativeCallDepth;
		return bResult;
	}

	bool InitializeClassNamesGuarded(FHotfixReloadState& State)
	{
		if (State.bClassNamesInitialized)
			return true;

		++GGuardedNativeCallDepth;
		bool bResult = false;
		__try
		{
			State.ClassMetaName = FName(L"Class");
			for (uint32 Index = 0; Index < ManagerClassNameCount; ++Index)
				State.ManagerClassFNames[Index] = FName(ManagerClassNames[Index]);
			State.bClassNamesInitialized = true;
			bResult = true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
		--GGuardedNativeCallDepth;
		return bResult;
	}

	int32 GetObjectCountGuarded()
	{
		++GGuardedNativeCallDepth;
		int32 Result = 0;
		__try
		{
			Result = TUObjectArray::Num();
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
		--GGuardedNativeCallDepth;
		return Result;
	}

	const UObject* GetManagerCandidateGuarded(
		int32 Index, const UClass* ManagerClass)
	{
		++GGuardedNativeCallDepth;
		const UObject* Result = nullptr;
		__try
		{
			auto Item = TUObjectArray::GetItemByIndex(Index);
			const UObject* Candidate = Item ? Item->GetObject() : nullptr;
			if (Candidate && !(Item->GetFlags() & InvalidInternalObjectFlags) &&
				Candidate->Class && !Candidate->IsDefaultObject() &&
				Candidate->IsA(ManagerClass))
			{
				Result = Candidate;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
		--GGuardedNativeCallDepth;
		return Result;
	}

	const UClass* GetManagerClassCandidateGuarded(
		int32 Index,
		const FHotfixReloadState& State,
		uint32* MatchedNameIndex)
	{
		if (MatchedNameIndex)
			*MatchedNameIndex = 0;
		++GGuardedNativeCallDepth;
		const UClass* Result = nullptr;
		__try
		{
			auto Item = TUObjectArray::GetItemByIndex(Index);
			const UObject* Candidate = Item ? Item->GetObject() : nullptr;
			if (Candidate && !(Item->GetFlags() & InvalidInternalObjectFlags) &&
				Candidate->Class &&
				Candidate->Class->Name == State.ClassMetaName)
			{
				for (uint32 NameIndex = 0;
					NameIndex < ManagerClassNameCount; ++NameIndex)
				{
					if (Candidate->Name == State.ManagerClassFNames[NameIndex])
					{
						if (MatchedNameIndex)
							*MatchedNameIndex = NameIndex;
						Result = reinterpret_cast<const UClass*>(Candidate);
						break;
					}
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
		--GGuardedNativeCallDepth;
		return Result;
	}

	UFunction* FindStartHotfixProcessGuarded(const UObject* Manager)
	{
		++GGuardedNativeCallDepth;
		UFunction* Result = nullptr;
		__try
		{
			Result = Manager
				? Manager->GetFunction("StartHotfixProcess")
				: nullptr;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
		--GGuardedNativeCallDepth;
		return Result;
	}

	bool HasZeroParameterContractGuarded(const UFunction* Function)
	{
		if (!Function)
			return false;

		++GGuardedNativeCallDepth;
		bool bResult = false;
		__try
		{
			// UFunction::PropertiesSize is the reflected parameter-structure
			// size on both legacy UProperty and modern FField engines. This avoids
			// choosing a metadata linked-list layout by build number.
			bResult = Function->GetPropertiesSize() == 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			bResult = false;
		}
		--GGuardedNativeCallDepth;
		return bResult;
	}

	bool HasUsableProcessEventSlot(const UObject* Object)
	{
		const uint64 VftIndex = Offsets::ProcessEventVft;
		if (!Object || !Object->Vft || VftIndex == 0 || VftIndex >= 0x100)
			return false;

		auto Slot = Object->Vft + VftIndex;
		return SDK::MemReadable(Slot, sizeof(*Slot)) &&
			*Slot && SDK::MemReadable(*Slot, 1);
	}

	bool InvokeZeroParameterFunctionGuarded(
		const UObject* Object, UFunction* Function)
	{
		if (!Object || !Function)
			return false;

		++GGuardedNativeCallDepth;
		bool bResult = false;
		__try
		{
			// The reflected contract was validated as parameterless. Keep a
			// generously sized, aligned zero buffer for engine variants that still
			// inspect the parameter address, without walking metadata outside this
			// guard.
			alignas(16) uint8 Parameters[0x1000]{};
			Object->ProcessEvent(Function, Parameters);
			bResult = true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
		--GGuardedNativeCallDepth;
		return bResult;
	}

	bool ReadNativeHotfixBusyState(
		FHotfixReloadState& State, bool& bReliable)
	{
		bReliable = true;
		if (!State.Manager || !IsUsableObject(State.Manager))
			return false;

		const uint64 ManagerIdentity =
			GetObjectIdentityGuarded(State.Manager);
		if (!ManagerIdentity)
		{
			bReliable = false;
			return false;
		}

		if (State.HotfixingInProgressManagerIdentity != ManagerIdentity)
		{
			State.HotfixingInProgressManagerIdentity = ManagerIdentity;
			State.HotfixingInProgressField = {};
			State.bHotfixingInProgressFieldResolved = false;
		}

		if (!State.bHotfixingInProgressFieldResolved)
		{
			bool bCompleted = false;
			ResolveReflectedBoolFieldGuarded(
				State.Manager,
				"bHotfixingInProgress",
				State.HotfixingInProgressField,
				bCompleted);
			if (!bCompleted)
			{
				bReliable = false;
				return false;
			}
			State.bHotfixingInProgressFieldResolved = true;
		}
		if (State.HotfixingInProgressField.Offset < 0)
		{
			// In stock OnlineSubsystemHotfix this is commonly a private native
			// bool rather than a reflected UPROPERTY. Absence is not evidence that
			// the manager is idle.
			bReliable = false;
			return false;
		}

		bool bBusy = false;
		if (!ReadReflectedBoolFieldGuarded(
				State.Manager,
				State.HotfixingInProgressField,
				bBusy))
		{
			bReliable = false;
			return false;
		}
		return bBusy;
	}

	void ReportUnavailable(FHotfixReloadState& State)
	{
		if (State.bReportedUnavailable)
			return;

		SDK::DbgLog(
			"[HotfixReload] native hotfix manager unavailable; "
			"refresh skipped and discovery will continue\n");
		State.bReportedUnavailable = true;
	}

	void ResetManager(FHotfixReloadState& State)
	{
		State.Manager = nullptr;
		State.StartHotfixProcess = nullptr;
		State.HotfixingInProgressField = {};
		State.HotfixingInProgressManagerIdentity = 0;
		State.bHotfixingInProgressFieldResolved = false;
		State.ManagerScanIndex = -1;
		State.ManagerScanFloorIndex = 0;
		State.bManagerGrowthScanActive = false;
	}

	void ResetManagerScanBackoff(FHotfixReloadState& State)
	{
		State.ManagerScanMissCount = 0;
		State.ManagerScanFloorIndex = 0;
		State.ManagerScanSnapshotObjectCount = 0;
		State.LastCompletedManagerScanObjectCount = 0;
		State.NextManagerFullScanMs = 0;
		State.NextManagerGrowthCheckMs = 0;
		State.bManagerScanBackoffActive = false;
		State.bManagerGrowthScanActive = false;
	}

	ULONGLONG ConsumeManagerScanBackoff(FHotfixReloadState& State)
	{
		const uint32 Shift =
			State.ManagerScanMissCount < 4
				? State.ManagerScanMissCount
				: 4;
		if (State.ManagerScanMissCount < 5)
			++State.ManagerScanMissCount;

		const ULONGLONG Delay = ManagerScanRetryMs << Shift;
		return Delay < ManagerScanMaxRetryMs
			? Delay
			: ManagerScanMaxRetryMs;
	}

	void BeginManagerScan(
		FHotfixReloadState& State, ULONGLONG Now)
	{
		ResetManager(State);
		ResetManagerScanBackoff(State);
		const int32 ObjectCount = GetObjectCountGuarded();
		State.ManagerScanIndex = ObjectCount - 1;
		State.ManagerScanFloorIndex = 0;
		State.ManagerScanSnapshotObjectCount = ObjectCount;
		// Class discovery and instance discovery never spend both budgets in the
		// same game-thread tick.
		State.NextManagerScanMs = Now + 1ULL;
	}

	void ResetClassDiscovery(
		FHotfixReloadState& State,
		ULONGLONG Now,
		ULONGLONG RetryDelayMs)
	{
		State.ManagerClass = nullptr;
		State.ClassScanIndex = -1;
		State.ManagerScanIndex = -1;
		for (auto& DiscoveredClass : State.DiscoveredManagerClasses)
			DiscoveredClass = nullptr;
		ResetManager(State);
		ResetManagerScanBackoff(State);
		State.NextClassResolveMs = Now + RetryDelayMs;
	}

	void ResolveManagerClass(
		FHotfixReloadState& State, ULONGLONG Now)
	{
		if (State.ManagerClass || Now < State.NextClassResolveMs)
			return;

		if (!InitializeClassNamesGuarded(State))
		{
			State.NextClassResolveMs = Now + ManagerScanRetryMs;
			ReportUnavailable(State);
			return;
		}

		const int32 ObjectCount = GetObjectCountGuarded();
		if (ObjectCount <= 0)
		{
			State.NextClassResolveMs = Now + ManagerScanRetryMs;
			ReportUnavailable(State);
			return;
		}

		if (State.ClassScanIndex < 0 ||
			State.ClassScanIndex >= ObjectCount)
		{
			State.ClassScanIndex = ObjectCount - 1;
		}

		int32 Processed = 0;
		while (State.ClassScanIndex >= 0 &&
			Processed++ < ClassObjectsPerTick)
		{
			const int32 Index = State.ClassScanIndex--;
			uint32 MatchedNameIndex = 0;
			const UClass* Candidate =
				GetManagerClassCandidateGuarded(
					Index, State, &MatchedNameIndex);
			if (!IsUsableObject(Candidate))
				continue;

			// Scanning newest-to-oldest means the first class for a given name
			// is the newest live generation. Keep it while completing the
			// bounded pass so the base OnlineHotfixManager can win priority and
			// match every Fortnite subclass.
			if (!State.DiscoveredManagerClasses[MatchedNameIndex])
				State.DiscoveredManagerClasses[MatchedNameIndex] = Candidate;
		}

		if (State.ClassScanIndex < 0)
		{
			for (uint32 NameIndex = 0;
				NameIndex < ManagerClassNameCount; ++NameIndex)
			{
				if (IsUsableObject(
						State.DiscoveredManagerClasses[NameIndex]))
				{
					State.ManagerClass =
						State.DiscoveredManagerClasses[NameIndex];
					break;
				}
			}
			for (auto& DiscoveredClass : State.DiscoveredManagerClasses)
				DiscoveredClass = nullptr;

			if (State.ManagerClass)
			{
				BeginManagerScan(State, Now);
				return;
			}

			// Unsupported builds retry from the newest objects later. The complete
			// registry is never searched by one server frame.
			State.NextClassResolveMs = Now + ClassCycleRetryMs;
			ReportUnavailable(State);
		}
	}

	void ResolveManager(
		FHotfixReloadState& State, ULONGLONG Now)
	{
		if (State.Manager || !State.ManagerClass)
		{
			return;
		}

		// A growth-only pass may span several frames. Once the independently
		// scheduled full-pass deadline arrives, promote the next slice to a full
		// scan so continuous UObject churn can never postpone it.
		if (State.bManagerGrowthScanActive &&
			State.NextManagerFullScanMs &&
			Now >= State.NextManagerFullScanMs)
		{
			State.ManagerScanIndex = -1;
			State.ManagerScanFloorIndex = 0;
			State.ManagerScanSnapshotObjectCount = 0;
			State.NextManagerScanMs = Now;
			State.NextManagerFullScanMs = 0;
			State.bManagerScanBackoffActive = false;
			State.bManagerGrowthScanActive = false;
		}

		if (Now < State.NextManagerScanMs)
		{
			if (!State.bManagerScanBackoffActive ||
				Now < State.NextManagerGrowthCheckMs)
			{
				return;
			}

			State.NextManagerGrowthCheckMs =
				Now + ManagerObjectGrowthCheckMs;
			const int32 CurrentObjectCount = GetObjectCountGuarded();
			if (CurrentObjectCount <=
				State.LastCompletedManagerScanObjectCount)
			{
				return;
			}

			State.ManagerScanIndex = CurrentObjectCount - 1;
			State.ManagerScanFloorIndex =
				State.LastCompletedManagerScanObjectCount;
			State.ManagerScanSnapshotObjectCount = CurrentObjectCount;
			State.bManagerScanBackoffActive = false;
			State.bManagerGrowthScanActive = true;
			State.NextManagerScanMs = Now;
		}

		if (!IsUsableObject(State.ManagerClass))
		{
			ResetClassDiscovery(
				State, Now, ManagerScanRetryMs);
			return;
		}

		const int32 ObjectCount = GetObjectCountGuarded();
		if (ObjectCount <= 0)
		{
			State.NextManagerScanMs = Now + ManagerScanRetryMs;
			ReportUnavailable(State);
			return;
		}

		if (State.ManagerScanIndex < State.ManagerScanFloorIndex ||
			State.ManagerScanIndex >= ObjectCount)
		{
			State.ManagerScanIndex = ObjectCount - 1;
			State.ManagerScanFloorIndex = 0;
			State.ManagerScanSnapshotObjectCount = ObjectCount;
			State.NextManagerFullScanMs = 0;
			State.bManagerScanBackoffActive = false;
			State.bManagerGrowthScanActive = false;
		}

		int32 Processed = 0;
		while (State.ManagerScanIndex >= State.ManagerScanFloorIndex &&
			Processed++ < ManagerObjectsPerTick)
		{
			const int32 Index = State.ManagerScanIndex--;
			const UObject* Candidate =
				GetManagerCandidateGuarded(Index, State.ManagerClass);
			if (!Candidate ||
				!IsUsableManager(Candidate, State.ManagerClass))
			{
				continue;
			}

			UFunction* StartHotfixProcess =
				FindStartHotfixProcessGuarded(Candidate);
			if (!IsUsableObject(StartHotfixProcess))
				continue;

			if (!HasZeroParameterContractGuarded(StartHotfixProcess))
			{
				SDK::DbgLog(
					"[HotfixReload] StartHotfixProcess signature is "
					"incompatible; disabled for this world\n");
				State.bDisabledForGeneration = true;
				return;
			}

			const uint64 ManagerIdentity =
				GetObjectIdentityGuarded(Candidate);
			if (!ManagerIdentity)
				continue;
			if (State.InvocationFaultManagerIdentity != ManagerIdentity)
			{
				State.InvocationFaultManagerIdentity = ManagerIdentity;
				State.InvocationFaultCount = 0;
			}

			State.Manager = Candidate;
			State.StartHotfixProcess = StartHotfixProcess;
			ResetManagerScanBackoff(State);
			return;
		}

		if (State.ManagerScanIndex < State.ManagerScanFloorIndex)
		{
			const bool bCompletedGrowthScan =
				State.bManagerGrowthScanActive;
			const int32 CompletedObjectCount =
				State.ManagerScanSnapshotObjectCount > 0
					? State.ManagerScanSnapshotObjectCount
					: ObjectCount;
			State.LastCompletedManagerScanObjectCount =
				CompletedObjectCount;
			State.ManagerScanIndex = -1;
			State.ManagerScanFloorIndex = 0;
			State.ManagerScanSnapshotObjectCount = 0;
			State.bManagerGrowthScanActive = false;

			if (!bCompletedGrowthScan)
			{
				State.NextManagerFullScanMs =
					Now + ConsumeManagerScanBackoff(State);
			}

			if (ObjectCount > CompletedObjectCount)
			{
				State.ManagerScanIndex = ObjectCount - 1;
				State.ManagerScanFloorIndex = CompletedObjectCount;
				State.ManagerScanSnapshotObjectCount = ObjectCount;
				State.bManagerScanBackoffActive = false;
				State.bManagerGrowthScanActive = true;
				State.NextManagerScanMs = Now + 1ULL;
			}
			else if (State.NextManagerFullScanMs &&
				Now >= State.NextManagerFullScanMs)
			{
				State.NextManagerScanMs = Now + 1ULL;
				State.NextManagerFullScanMs = 0;
				State.bManagerScanBackoffActive = false;
			}
			else
			{
				State.bManagerScanBackoffActive = true;
				State.NextManagerScanMs = State.NextManagerFullScanMs;
			}
			State.NextManagerGrowthCheckMs =
				Now + ManagerObjectGrowthCheckMs;
			ReportUnavailable(State);
		}
	}

	void HandleInvocationFault(
		FHotfixReloadState& State,
		ULONGLONG Now,
		const char* Reason)
	{
		const uint64 ManagerIdentity =
			GetObjectIdentityGuarded(State.Manager);
		if (ManagerIdentity &&
			State.InvocationFaultManagerIdentity != ManagerIdentity)
		{
			State.InvocationFaultManagerIdentity = ManagerIdentity;
			State.InvocationFaultCount = 0;
		}

		if (State.InvocationFaultCount < MaximumInvocationFaults)
			++State.InvocationFaultCount;

		if (State.InvocationFaultCount >= MaximumInvocationFaults)
		{
			State.bDisabledForGeneration = true;
			ResetManager(State);
			SDK::DbgLog(
				"[HotfixReload] disabled reason=%s faults=%u\n",
				Reason ? Reason : "invocation-fault",
				State.InvocationFaultCount);
			return;
		}

		const ULONGLONG RetryDelay =
			InvocationRetryMs << (State.InvocationFaultCount - 1);
		ResetManager(State);
		ResetManagerScanBackoff(State);
		State.ManagerScanIndex = -1;
		State.NextManagerScanMs = Now + RetryDelay;
		State.NextRefreshMs = Now + RetryDelay;
		SDK::DbgLog(
			"[HotfixReload] invocation fault reason=%s "
			"faults=%u retry-ms=%llu\n",
			Reason ? Reason : "unknown",
			State.InvocationFaultCount,
			static_cast<unsigned long long>(RetryDelay));
	}
}

void HotfixReload::Tick(UWorld* World, UNetDriver* Driver)
{
	if (!World || !Driver || World->NetDriver != Driver)
		return;

	auto& State = GHotfixReloadState;
	const ULONGLONG Now = GetTickCount64();
	if (State.World != World || State.Driver != Driver)
	{
		if (!IsUsableObject(World) || !IsUsableObject(Driver))
			return;

		State = {};
		State.World = World;
		State.Driver = Driver;
		State.NextRefreshMs = Now + InitialRefreshDelayMs;
		State.NextClassResolveMs = Now;
		AssetHotfixRollback::OnWorldChanged(Now);
		SDK::DbgLog(
			"[HotfixReload] scheduled lightweight native hotfix check every 1 second\n");
		return;
	}

	if (!State.bDisabledForGeneration)
	{
		ResolveManagerClass(State, Now);
		ResolveManager(State, Now);
	}
	if (State.bDisabledForGeneration || Now < State.NextRefreshMs)
		return;

	if (State.Manager && State.ManagerClass)
	{
		AssetHotfixRollback::EnsureManagerIniObservation(
			State.Manager, State.ManagerClass);
	}

	bool bHotfixBusyStateReliable = false;
	const bool bHotfixBusy = ReadNativeHotfixBusyState(
		State, bHotfixBusyStateReliable);

	if (!State.Manager || !State.StartHotfixProcess)
	{
		// Keep the already-due deadline intact. Incremental discovery continues
		// next frame; finding the manager can trigger immediately.
		ReportUnavailable(State);
		return;
	}

	if (!IsUsableManager(State.Manager, State.ManagerClass) ||
		!IsUsableObject(State.StartHotfixProcess))
	{
		BeginManagerScan(State, Now);
		ReportUnavailable(State);
		return;
	}

	UFunction* CurrentStartHotfixProcess =
		FindStartHotfixProcessGuarded(State.Manager);
	if (CurrentStartHotfixProcess != State.StartHotfixProcess ||
		!IsUsableObject(CurrentStartHotfixProcess) ||
		!HasZeroParameterContractGuarded(CurrentStartHotfixProcess))
	{
		BeginManagerScan(State, Now);
		ReportUnavailable(State);
		return;
	}

	if (!AssetHotfixRollback::IsExactOncePeriodicRefreshReady(
			State.Manager))
	{
		// Hook discovery/derived-manager validation has had the full initial
		// refresh delay to settle. Never start a recurring stock walk unless an
		// unchanged accepted payload can be correlated and skipped exactly once.
		if (!State.bReportedExactOnceUnavailable)
		{
			SDK::DbgLog(
				"[HotfixReload] exact-once asset hooks unavailable; "
				"added periodic refresh disabled "
				"for this world (stock behavior unchanged)\n");
			State.bReportedExactOnceUnavailable = true;
		}
		State.bDisabledForGeneration = true;
		return;
	}
	if (AssetHotfixRollback::IsRefreshInFlight(
			State.Manager, Now, bHotfixBusyStateReliable, bHotfixBusy))
	{
		// Accepted-INI/PatchAssets correlation blocks until completion. Ordinary
		// asynchronous anomalies are retired behind a short cooldown, while a
		// no-callback unchanged check releases from positive reflected idle state
		// or after the bounded fallback on private-busy builds.
		State.NextRefreshMs = Now + InFlightRecheckMs;
		return;
	}

	if (!HasUsableProcessEventSlot(State.Manager))
	{
		SDK::DbgLog(
			"[HotfixReload] ProcessEvent slot is unavailable; "
			"disabled for this world\n");
		State.bDisabledForGeneration = true;
		return;
	}

	if (State.bInvocationActive)
		return;

	// StartHotfixProcess hands off to Fortnite's configured asynchronous title
	// file workflow; this module performs no custom HTTP or file work.
	// Atomically arm first because some engine branches complete the entire
	// refresh inside this ProcessEvent call, including the PatchAssets row walk.
	const uint64 RefreshSequence =
		AssetHotfixRollback::TryBeginRefresh(State.Manager, Now);
	if (!RefreshSequence)
	{
		State.NextRefreshMs = Now + InFlightRecheckMs;
		return;
	}
	State.bInvocationActive = true;
	const bool bStarted = InvokeZeroParameterFunctionGuarded(
		State.Manager, CurrentStartHotfixProcess);
	State.bInvocationActive = false;
	if (!bStarted)
	{
		AssetHotfixRollback::OnRefreshStartFailed(
			State.Manager, RefreshSequence);
		HandleInvocationFault(
			State, Now, "start-hotfix-process-faulted");
		return;
	}

	State.InvocationFaultCount = 0;
	State.InvocationFaultManagerIdentity =
		GetObjectIdentityGuarded(State.Manager);

	// Rate-limit only successful kickoffs. Missing targets and incomplete scan
	// slices never push this deadline farther into the future.
	State.NextRefreshMs = Now + RefreshIntervalMs;
	if (State.bReportedUnavailable)
	{
		SDK::DbgLog("[HotfixReload] native hotfix manager recovered\n");
		State.bReportedUnavailable = false;
	}

	if (!State.bReportedFirstRefresh)
	{
		SDK::DbgLog(
			"[HotfixReload] requested the first native hotfix refresh\n");
		State.bReportedFirstRefresh = true;
	}
}
