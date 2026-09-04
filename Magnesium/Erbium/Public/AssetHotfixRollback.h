#pragma once

#include <cstdint>

namespace SDK
{
	class UObject;
	class UClass;
}

namespace AssetHotfixRollback
{
	// Historical namespace retained to avoid project/include churn. The module is
	// now a bounded exact-once delta gate. It can retain exact text baselines and
	// captured CurveTable behavior, but never retains UObject pointers or raw
	// reflected row storage.
	void OnWorldChanged(std::uint64_t Now);

	// HotfixIniFile is virtual. Resolve its slot from the discovered base class
	// and observe a derived Fortnite override without a fixed slot or RVA.
	void EnsureManagerIniObservation(
		const SDK::UObject* Manager,
		const SDK::UClass* BaseManagerClass);

	// False means the required native hooks are unavailable for this manager.
	// Stock startup hotfix behavior always remains forwarded.
	bool IsExactOncePeriodicRefreshReady(
		const SDK::UObject* Manager);

	// Prevent asynchronous StartHotfixProcess calls from overlapping. A reliable
	// reflected native busy bit is positive completion evidence. Generations
	// without it use a short bounded recycle after a legitimate no-callback
	// result. Timing/correlation anomalies retire the request, forward native
	// work, and use a short cooldown before polling continues.
	bool IsRefreshInFlight(
		const SDK::UObject* Manager,
		std::uint64_t Now,
		bool bNativeBusyStateReliable,
		bool bNativeHotfixBusy);

	// Atomically retire an expired no-callback tombstone and arm its successor.
	// A zero token means the caller must not invoke StartHotfixProcess.
	std::uint64_t TryBeginRefresh(
		const SDK::UObject* Manager,
		std::uint64_t Now);

	void OnRefreshStartFailed(
		const SDK::UObject* Manager,
		std::uint64_t RefreshSequence);
}
