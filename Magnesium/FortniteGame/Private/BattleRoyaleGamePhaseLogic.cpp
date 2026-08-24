#include "pch.h"
#include "../Public/BattleRoyaleGamePhaseLogic.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../../Erbium/Public/Configuration.h"
#include "../../Erbium/Public/GUI.h"
#include "../../Erbium/Support/Public/VersionFeatureAdapter.h"

uint64_t SetGamePhase_ = 0;

bool AFortSafeZoneIndicator::TrySetSafeZoneRadiusAndCenter(
	float InRadius, const FVector& InLocation) const
{
	auto Function = GetFunction("SetSafeZoneRadiusAndCenter");
	if (!Function || !std::isfinite(InRadius) || InRadius < 0.f)
		return false;

	const auto Parameters = Function->GetParamsNamed();
	if (Parameters.Size == 0 || Parameters.Size > 0x100)
		return false;

	constexpr uint64 CPF_Parm = 0x0000000000000080;
	constexpr uint64 CPF_ReturnParm = 0x0000000000000400;
	uint32 RadiusOffset = UINT32_MAX;
	uint32 LocationOffset = UINT32_MAX;
	const uint32 LocationSize = (uint32)FVector::Size();
	for (const auto& Parameter : Parameters.NameOffsetMap)
	{
		if (!(Parameter.PropertyFlags & CPF_Parm) ||
			(Parameter.PropertyFlags & CPF_ReturnParm))
		{
			continue;
		}

		uint32 ExpectedSize = 0;
		uint32* DestinationOffset = nullptr;
		if (Parameter.Name == "InRadius")
		{
			ExpectedSize = sizeof(float);
			DestinationOffset = &RadiusOffset;
		}
		else if (Parameter.Name == "InLocation")
		{
			ExpectedSize = LocationSize;
			DestinationOffset = &LocationOffset;
		}
		else
		{
			return false;
		}

		if (*DestinationOffset != UINT32_MAX ||
			Parameter.ElementSize != ExpectedSize ||
			Parameter.Offset > Parameters.Size ||
			ExpectedSize > Parameters.Size - Parameter.Offset)
		{
			return false;
		}
		*DestinationOffset = Parameter.Offset;
	}

	if (RadiusOffset == UINT32_MAX ||
		LocationOffset == UINT32_MAX)
	{
		return false;
	}

	void* Memory = FMemory::Malloc(Parameters.Size);
	if (!Memory)
		return false;
	memset(Memory, 0, Parameters.Size);
	memcpy((PBYTE)Memory + RadiusOffset,
		&InRadius, sizeof(InRadius));
	memcpy((PBYTE)Memory + LocationOffset,
		&InLocation, LocationSize);
	ProcessEvent(Function, Memory);
	FMemory::Free(Memory);
	return true;
}

namespace
{
	struct FSafeZoneFloatRestore
	{
		UObject* Owner = nullptr;
		const char* PropertyName = nullptr;
		const wchar_t* ReplicatedPropertyName = nullptr;
		float Value = 0.f;
		bool bHasValue = false;
	};

	struct FSafeZonePauseState
	{
		UWorld* World = nullptr;
		AFortGameStateAthena* GameState = nullptr;
		AFortSafeZoneIndicator* Indicator = nullptr;
		UObject* SafeZonesStartOwner = nullptr;
		bool bHasRequest = false;
		bool bRequestedPaused = false;
		bool bHasStartOffset = false;
		bool bHasFinishOffset = false;
		bool bHasSafeZonesStartOffset = false;
		float StartOffset = 0.f;
		float FinishOffset = 0.f;
		float SafeZonesStartOffset = 0.f;
		float PauseWorldTime = 0.f;
		bool bPhasePauseGateApplied = false;
		bool bWallPauseGateApplied = false;
		bool bUsingLegacyGeometryFallback = false;
		bool bLegacyNativeSetterApplied = false;
		bool bHasFrozenCenter = false;
		bool bHasFrozenRadius = false;
		FVector FrozenCenter{};
		float FrozenRadius = 0.f;
		bool bHadLastCenter = false;
		bool bHadPreviousCenter = false;
		bool bHadNextCenter = false;
		bool bHadLastRadius = false;
		bool bHadPreviousRadius = false;
		bool bHadNextRadius = false;
		FVector SavedLastCenter{};
		FVector SavedPreviousCenter{};
		FVector SavedNextCenter{};
		float SavedLastRadius = 0.f;
		float SavedPreviousRadius = 0.f;
		float SavedNextRadius = 0.f;
		std::array<FSafeZoneFloatRestore, 16>
			FloatRestores{};
		int32 FloatRestoreCount = 0;
		bool bLoggedWallPause = false;
		bool bLoggedBackstop = false;
	};

	FSafeZonePauseState GSafeZonePauseState{};
	std::atomic<int32> GSafeZonePauseRequest{ -1 };
	std::atomic_bool GSafeZonePauseSnapshot{ false };

	void ResetSafeZonePauseStateForWorld(UWorld* World)
	{
		auto GameState = World
			? (AFortGameStateAthena*)World->GameState
			: nullptr;
		if (GSafeZonePauseState.World == World &&
			GSafeZonePauseState.GameState == GameState)
		{
			return;
		}

		GSafeZonePauseState = {};
		GSafeZonePauseState.World = World;
		GSafeZonePauseState.GameState = GameState;
		UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bPausedZone = false;
		GSafeZonePauseSnapshot.store(
			false, std::memory_order_release);
	}

	bool TryReadSafeZonePauseFlag(
		UObject* Object, const char* PropertyName, bool& OutValue)
	{
		if (!Object)
			return false;

		auto Property = Object->GetProperty(PropertyName, 0x20000);
		if (!Property)
			return false;

		const uint32 Offset = SDK::ReadPropertyOffset(
			GetFromOffset<uint32>(
				Property, Offsets::Offset_Internal));
		if (Offset == UINT32_MAX ||
			Offset >= 0x10000)
		{
			return false;
		}
		auto Address = (PBYTE)Object + Offset;
		if (!SDK::MemReadable(Address, sizeof(uint8_t)))
			return false;

		const uint8_t Mask = Property->GetFieldMask();
		const uint8_t Byte = *Address;
		OutValue = Mask ? (Byte & Mask) != 0 : Byte != 0;
		return true;
	}

	bool TryWriteSafeZonePauseFlag(
		UObject* Object,
		const char* PropertyName,
		bool Value,
		bool* OutChanged = nullptr)
	{
		if (OutChanged)
			*OutChanged = false;
		if (!Object)
			return false;

		auto Property = Object->GetProperty(PropertyName, 0x20000);
		if (!Property)
			return false;

		const uint32 Offset = SDK::ReadPropertyOffset(
			GetFromOffset<uint32>(
				Property, Offsets::Offset_Internal));
		if (Offset == UINT32_MAX ||
			Offset >= 0x10000)
		{
			return false;
		}
		auto Address = (PBYTE)Object + Offset;
		if (!SDK::MemReadable(Address, sizeof(uint8_t)))
			return false;

		const uint8_t Mask = Property->GetFieldMask();
		const uint8_t PreviousByte = *Address;
		if (Mask)
		{
			if (Value)
				*Address |= Mask;
			else
				*Address &= ~Mask;
		}
		else
		{
			*Address = Value ? 1 : 0;
		}

		const bool bChanged = PreviousByte != *Address;
		if (OutChanged)
			*OutChanged = bChanged;
		return true;
	}

	bool TryReadSafeZoneFloat(
		UObject* Object,
		const char* PropertyName,
		float& OutValue)
	{
		if (!Object || !PropertyName)
			return false;

		auto Property = Object->GetProperty(
			PropertyName, GUESS_PROP_FLAGS(float));
		if (!Property)
			return false;

		const uint32 Offset = SDK::ReadPropertyOffset(
			GetFromOffset<uint32>(
				Property, Offsets::Offset_Internal));
		if (Offset == UINT32_MAX || Offset >= 0x10000)
			return false;

		auto Address = (const float*)((const PBYTE)Object + Offset);
		if (!SDK::MemReadable(Address, sizeof(float)))
			return false;

		OutValue = *Address;
		return std::isfinite(OutValue);
	}

	bool TryWriteSafeZoneFloat(
		UObject* Object,
		const char* PropertyName,
		float Value,
		bool* OutChanged = nullptr)
	{
		if (OutChanged)
			*OutChanged = false;
		if (!Object || !PropertyName || !std::isfinite(Value))
			return false;

		auto Property = Object->GetProperty(
			PropertyName, GUESS_PROP_FLAGS(float));
		if (!Property)
			return false;

		const uint32 Offset = SDK::ReadPropertyOffset(
			GetFromOffset<uint32>(
				Property, Offsets::Offset_Internal));
		if (Offset == UINT32_MAX || Offset >= 0x10000)
			return false;

		auto Address = (float*)((PBYTE)Object + Offset);
		if (!SDK::MemReadable(Address, sizeof(float)))
			return false;

		const bool bChanged = *Address != Value;
		*Address = Value;
		if (OutChanged)
			*OutChanged = bChanged;
		return true;
	}

	bool IsFiniteSafeZoneVector(const FVector& Value)
	{
		return std::isfinite(Value.X) &&
			std::isfinite(Value.Y) &&
			std::isfinite(Value.Z);
	}

	bool IsLiveSafeZoneObject(const UObject* Object)
	{
		if (!Object ||
			!SDK::MemReadable(Object, sizeof(UObject)))
		{
			return false;
		}

		const int32 ObjectIndex = Object->Index;
		if (ObjectIndex < 0 ||
			ObjectIndex >= TUObjectArray::Num())
		{
			return false;
		}

		auto Item =
			TUObjectArray::GetItemByIndex(ObjectIndex);
		constexpr int32 InvalidObjectFlags = 0x20;
		return Item &&
			Item->GetObject() == Object &&
			!(Item->GetFlags() & InvalidObjectFlags) &&
			Object->Class;
	}

	bool IsOwnedBySafeZoneGameState(
		const UObject* Object,
		const AFortGameStateAthena* GameState)
	{
		const UObject* Current = Object;
		for (int32 Depth = 0;
			Current && Depth < 16;
			Depth++)
		{
			if (Current == GameState)
				return true;
			if (!IsLiveSafeZoneObject(Current))
				return false;
			Current = Current->Outer;
		}
		return false;
	}

	UFortGameStateComponent_BattleRoyaleGamePhaseLogic*
		GetCurrentSafeZonePhaseLogic(UWorld* World)
	{
		if (!World ||
			VersionInfo.FortniteVersion < 25.20)
		{
			return nullptr;
		}

		auto Candidate =
			UFortGameStateComponent_BattleRoyaleGamePhaseLogic::
				Get(World);
		auto GameState =
			(AFortGameStateAthena*)World->GameState;
		return IsLiveSafeZoneObject(Candidate) &&
			IsOwnedBySafeZoneGameState(
				Candidate, GameState)
			? Candidate
			: nullptr;
	}

	AFortSafeZoneIndicator* GetCurrentSafeZoneIndicator(
		UWorld* World, AFortGameMode* GameMode)
	{
		// Prefer the newest authoritative owner first. Superseded indicators can
		// remain live for a short time during a phase-owner handoff; choosing the
		// GameMode pointer first would keep pausing the old wall while the new one
		// advanced (and then appeared to teleport at the end of its close).
		auto PhaseLogic =
			GetCurrentSafeZonePhaseLogic(World);
		if (PhaseLogic &&
			PhaseLogic->HasSafeZoneIndicator() &&
			IsLiveSafeZoneObject(
				PhaseLogic->SafeZoneIndicator))
		{
			return PhaseLogic->SafeZoneIndicator;
		}

		auto GameState = World
			? (AFortGameStateAthena*)World->GameState
			: nullptr;
		if (GameState && GameState->HasSafeZoneIndicator() &&
			IsLiveSafeZoneObject(GameState->SafeZoneIndicator))
		{
			return (AFortSafeZoneIndicator*)GameState->SafeZoneIndicator;
		}

		if (GameMode && GameMode->HasSafeZoneIndicator() &&
			IsLiveSafeZoneObject(GameMode->SafeZoneIndicator))
		{
			return GameMode->SafeZoneIndicator;
		}

		return nullptr;
	}

	void MarkSafeZoneIndicatorDirty(
		AFortSafeZoneIndicator* Indicator,
		const wchar_t* PropertyName)
	{
		if (Indicator && PropertyName)
		{
			VersionFeatureAdapter::
				MarkReplicatedPropertyDirty(
					Indicator, PropertyName);
		}
	}

	void RememberAndWriteSafeZoneFloat(
		UObject* Owner,
		const char* PropertyName,
		const wchar_t* ReplicatedPropertyName,
		float Value)
	{
		if (!Owner || !PropertyName || !std::isfinite(Value))
			return;

		auto& State = GSafeZonePauseState;
		FSafeZoneFloatRestore* Restore = nullptr;
		for (int32 Index = 0;
			Index < State.FloatRestoreCount;
			Index++)
		{
			auto& Existing = State.FloatRestores[Index];
			if (Existing.Owner == Owner &&
				Existing.PropertyName &&
				strcmp(Existing.PropertyName,
					PropertyName) == 0)
			{
				Restore = &Existing;
				break;
			}
		}

		float PreviousValue = 0.f;
		if (!Restore &&
			State.FloatRestoreCount <
				(int32)State.FloatRestores.size() &&
			TryReadSafeZoneFloat(
				Owner, PropertyName, PreviousValue))
		{
			Restore = &State.FloatRestores[
				State.FloatRestoreCount++];
			Restore->Owner = Owner;
			Restore->PropertyName = PropertyName;
			Restore->ReplicatedPropertyName =
				ReplicatedPropertyName;
			Restore->Value = PreviousValue;
			Restore->bHasValue = true;
		}
		// Never overwrite bookkeeping that we cannot put back exactly. This also
		// protects an unexpected schema/type mismatch and the fixed snapshot
		// capacity from leaving a permanent pause value behind.
		if (!Restore)
			return;

		bool bChanged = false;
		if (TryWriteSafeZoneFloat(
			Owner, PropertyName, Value, &bChanged) &&
			bChanged && ReplicatedPropertyName)
		{
			VersionFeatureAdapter::
				MarkReplicatedPropertyDirty(
					Owner,
					ReplicatedPropertyName);
		}
	}

	void ApplySafeZonePauseBookkeeping(
		AFortGameMode* GameMode,
		AFortGameStateAthena* GameState,
		UFortGameStateComponent_BattleRoyaleGamePhaseLogic*
			PhaseLogic,
		AFortSafeZoneIndicator* Indicator)
	{
		auto& State = GSafeZonePauseState;
		// Match native PauseSafeZone: this field stores time to the end of the
		// phase, while SafeZonePauseTime and the captured start offset preserve
		// the visible holding countdown separately.
		const float TimeRemaining =
			State.bHasFinishOffset
				? (std::max)(0.f, State.FinishOffset)
				: 0.f;

		RememberAndWriteSafeZoneFloat(
			GameMode,
			"TimeRemainingWhenPhasePaused",
			L"TimeRemainingWhenPhasePaused",
			TimeRemaining);
		RememberAndWriteSafeZoneFloat(
			GameState,
			"TimeRemainingWhenPhasePaused",
			L"TimeRemainingWhenPhasePaused",
			TimeRemaining);
		RememberAndWriteSafeZoneFloat(
			PhaseLogic,
			"TimeRemainingWhenPhasePaused",
			L"TimeRemainingWhenPhasePaused",
			TimeRemaining);
		RememberAndWriteSafeZoneFloat(
			Indicator,
			"TimeRemainingWhenPhasePaused",
			L"TimeRemainingWhenPhasePaused",
			TimeRemaining);

		RememberAndWriteSafeZoneFloat(
			GameState,
			"SafeZonePauseTime",
			L"SafeZonePauseTime",
			State.PauseWorldTime);
		RememberAndWriteSafeZoneFloat(
			PhaseLogic,
			"SafeZonePauseTime",
			L"SafeZonePauseTime",
			State.PauseWorldTime);
		RememberAndWriteSafeZoneFloat(
			Indicator,
			"SafeZonePauseTime",
			L"SafeZonePauseTime",
			State.PauseWorldTime);

		if (GameState)
			GameState->ForceNetUpdate();
		if (Indicator)
			Indicator->ForceNetUpdate();
	}

	void RestoreSafeZonePauseBookkeeping(
		AFortGameStateAthena* GameState,
		AFortSafeZoneIndicator* Indicator)
	{
		auto& State = GSafeZonePauseState;
		for (int32 Index = State.FloatRestoreCount - 1;
			Index >= 0;
			Index--)
		{
			auto& Restore = State.FloatRestores[Index];
			if (!Restore.bHasValue ||
				!IsLiveSafeZoneObject(Restore.Owner))
			{
				continue;
			}

			bool bChanged = false;
			if (TryWriteSafeZoneFloat(
				Restore.Owner,
				Restore.PropertyName,
				Restore.Value,
				&bChanged) &&
				bChanged &&
				Restore.ReplicatedPropertyName)
			{
				VersionFeatureAdapter::
					MarkReplicatedPropertyDirty(
						Restore.Owner,
						Restore.ReplicatedPropertyName);
			}
		}

		State.FloatRestoreCount = 0;
		State.FloatRestores = {};
		if (GameState)
			GameState->ForceNetUpdate();
		if (Indicator)
			Indicator->ForceNetUpdate();
	}

	void RestoreSafeZonePauseBookkeepingForOwner(
		UObject* Owner)
	{
		if (!Owner)
			return;

		auto& State = GSafeZonePauseState;
		int32 WriteIndex = 0;
		for (int32 ReadIndex = 0;
			ReadIndex < State.FloatRestoreCount;
			ReadIndex++)
		{
			auto Restore = State.FloatRestores[ReadIndex];
			if (Restore.Owner != Owner)
			{
				State.FloatRestores[WriteIndex++] = Restore;
				continue;
			}

			if (Restore.bHasValue &&
				IsLiveSafeZoneObject(Restore.Owner))
			{
				bool bChanged = false;
				if (TryWriteSafeZoneFloat(
					Restore.Owner,
					Restore.PropertyName,
					Restore.Value,
					&bChanged) &&
					bChanged &&
					Restore.ReplicatedPropertyName)
				{
					VersionFeatureAdapter::
						MarkReplicatedPropertyDirty(
							Restore.Owner,
							Restore.ReplicatedPropertyName);
				}
			}
		}

		for (int32 Index = WriteIndex;
			Index < State.FloatRestoreCount;
			Index++)
		{
			State.FloatRestores[Index] = {};
		}
		State.FloatRestoreCount = WriteIndex;
	}

	void ResetLegacySafeZoneGeometrySnapshot()
	{
		auto& State = GSafeZonePauseState;
		State.bUsingLegacyGeometryFallback = false;
		State.bLegacyNativeSetterApplied = false;
		State.bHasFrozenCenter = false;
		State.bHasFrozenRadius = false;
		State.bHadLastCenter = false;
		State.bHadPreviousCenter = false;
		State.bHadNextCenter = false;
		State.bHadLastRadius = false;
		State.bHadPreviousRadius = false;
		State.bHadNextRadius = false;
	}

	void CaptureLegacySafeZoneGeometry(
		AFortSafeZoneIndicator* Indicator)
	{
		ResetLegacySafeZoneGeometrySnapshot();
		if (!Indicator)
			return;

		auto& State = GSafeZonePauseState;
		State.bHadLastCenter = Indicator->HasLastCenter();
		State.bHadPreviousCenter =
			Indicator->HasPreviousCenter();
		State.bHadNextCenter = Indicator->HasNextCenter();
		State.bHadLastRadius = Indicator->HasLastRadius();
		State.bHadPreviousRadius =
			Indicator->HasPreviousRadius();
		State.bHadNextRadius = Indicator->HasNextRadius();

		if (State.bHadLastCenter)
			State.SavedLastCenter = Indicator->LastCenter;
		if (State.bHadPreviousCenter)
			State.SavedPreviousCenter = Indicator->PreviousCenter;
		if (State.bHadNextCenter)
			State.SavedNextCenter = Indicator->NextCenter;
		if (State.bHadLastRadius)
			State.SavedLastRadius = Indicator->LastRadius;
		if (State.bHadPreviousRadius)
			State.SavedPreviousRadius = Indicator->PreviousRadius;
		if (State.bHadNextRadius)
			State.SavedNextRadius = Indicator->NextRadius;

		float Alpha = 0.f;
		const float SegmentDuration =
			State.FinishOffset - State.StartOffset;
		if (State.bHasStartOffset &&
			State.bHasFinishOffset &&
			std::isfinite(SegmentDuration) &&
			SegmentDuration > 0.001f)
		{
			Alpha = (std::clamp)(
				-State.StartOffset / SegmentDuration,
				0.f, 1.f);
		}

		// Prefer the physical wall's authoritative live state. Endpoint math is
		// only a last resort because old builds may apply easing or a slightly
		// different clock when updating the material actor.
		if (auto GetCenter =
			Indicator->GetFunction("GetSafeZoneCenter"))
		{
			FVector LiveCenter =
				Indicator->Call<FVector>(GetCenter);
			if (IsFiniteSafeZoneVector(LiveCenter))
			{
				State.FrozenCenter = LiveCenter;
				State.bHasFrozenCenter = true;
			}
		}

		if (!State.bHasFrozenCenter)
		{
			FVector ActorCenter =
				Indicator->K2_GetActorLocation();
			if (IsFiniteSafeZoneVector(ActorCenter))
			{
				State.FrozenCenter = ActorCenter;
				State.bHasFrozenCenter = true;
			}
		}

		const FVector* SourceCenter = nullptr;
		if (State.bHadLastCenter &&
			IsFiniteSafeZoneVector(State.SavedLastCenter))
		{
			SourceCenter = &State.SavedLastCenter;
		}
		else if (State.bHadPreviousCenter &&
			IsFiniteSafeZoneVector(State.SavedPreviousCenter))
		{
			SourceCenter = &State.SavedPreviousCenter;
		}

		if (!State.bHasFrozenCenter &&
			SourceCenter && State.bHadNextCenter &&
			IsFiniteSafeZoneVector(State.SavedNextCenter))
		{
			State.FrozenCenter =
				*SourceCenter +
				(State.SavedNextCenter - *SourceCenter) * Alpha;
			State.bHasFrozenCenter =
				IsFiniteSafeZoneVector(State.FrozenCenter);
		}
		const float* SourceRadius = nullptr;
		if (State.bHadLastRadius &&
			std::isfinite(State.SavedLastRadius) &&
			State.SavedLastRadius >= 0.f)
		{
			SourceRadius = &State.SavedLastRadius;
		}
		else if (State.bHadPreviousRadius &&
			std::isfinite(State.SavedPreviousRadius) &&
			State.SavedPreviousRadius >= 0.f)
		{
			SourceRadius = &State.SavedPreviousRadius;
		}

		if (Indicator->HasRadius() &&
			std::isfinite(Indicator->Radius) &&
			Indicator->Radius >= 0.f)
		{
			State.FrozenRadius = Indicator->Radius;
			State.bHasFrozenRadius = true;
		}
		else if (SourceRadius && State.bHadNextRadius &&
			std::isfinite(State.SavedNextRadius) &&
			State.SavedNextRadius >= 0.f)
		{
			State.FrozenRadius =
				*SourceRadius +
				(State.SavedNextRadius - *SourceRadius) * Alpha;
			State.bHasFrozenRadius =
				std::isfinite(State.FrozenRadius) &&
				State.FrozenRadius >= 0.f;
		}
		// A partial snapshot is not an exact pause: a closing radius with a fixed
		// center (or vice versa) still moves the physical wall. Every supported
		// legacy schema exposes enough live data for both dimensions, so fail
		// closed instead of advertising a partially frozen circle.
		State.bUsingLegacyGeometryFallback =
			State.bHasFrozenCenter &&
			State.bHasFrozenRadius;
	}

	void ApplyLegacySafeZoneGeometryFreeze(
		AFortSafeZoneIndicator* Indicator)
	{
		auto& State = GSafeZonePauseState;
		if (!Indicator || Indicator != State.Indicator ||
			!State.bUsingLegacyGeometryFallback)
		{
			return;
		}

		// 2.5-era indicators expose a native setter that updates the physical
		// wall/material immediately. The 1.x builds do not, so the reflected
		// endpoint + actor fallback below remains the universal path.
		if (!State.bLegacyNativeSetterApplied)
		{
			State.bLegacyNativeSetterApplied =
				Indicator->TrySetSafeZoneRadiusAndCenter(
					State.FrozenRadius,
					State.FrozenCenter);
		}

		if (State.bHasFrozenCenter)
		{
			if (State.bHadLastCenter)
			{
				Indicator->LastCenter = State.FrozenCenter;
				MarkSafeZoneIndicatorDirty(
					Indicator, L"LastCenter");
			}
			if (State.bHadPreviousCenter)
			{
				Indicator->PreviousCenter = State.FrozenCenter;
				MarkSafeZoneIndicatorDirty(
					Indicator, L"PreviousCenter");
			}
			if (State.bHadNextCenter)
			{
				Indicator->NextCenter = State.FrozenCenter;
				MarkSafeZoneIndicatorDirty(
					Indicator, L"NextCenter");
			}
			Indicator->K2_SetActorLocation(
				State.FrozenCenter, false, nullptr, true);
		}

		if (State.bHasFrozenRadius)
		{
			if (State.bHadLastRadius)
			{
				Indicator->LastRadius = State.FrozenRadius;
				MarkSafeZoneIndicatorDirty(
					Indicator, L"LastRadius");
			}
			if (State.bHadPreviousRadius)
			{
				Indicator->PreviousRadius = State.FrozenRadius;
				MarkSafeZoneIndicatorDirty(
					Indicator, L"PreviousRadius");
			}
			if (State.bHadNextRadius)
			{
				Indicator->NextRadius = State.FrozenRadius;
				MarkSafeZoneIndicatorDirty(
					Indicator, L"NextRadius");
			}
			if (Indicator->HasRadius())
				Indicator->Radius = State.FrozenRadius;
		}

		Indicator->ForceNetUpdate();
	}

	void PrepareLegacySafeZoneResume(
		AFortSafeZoneIndicator* Indicator,
		float TimeSeconds)
	{
		auto& State = GSafeZonePauseState;
		if (!Indicator || Indicator != State.Indicator ||
			!State.bUsingLegacyGeometryFallback ||
			!std::isfinite(TimeSeconds))
		{
			return;
		}

		// Seed the native physical wall at the frozen circle before restoring the
		// saved preview endpoint. On 2.5-6.x, reflected fields alone do not update
		// the wall material/collision state immediately.
		Indicator->TrySetSafeZoneRadiusAndCenter(
			State.FrozenRadius,
			State.FrozenCenter);

		if (State.bHasFrozenCenter)
		{
			if (State.bHadLastCenter)
			{
				Indicator->LastCenter = State.FrozenCenter;
				MarkSafeZoneIndicatorDirty(
					Indicator, L"LastCenter");
			}
			if (State.bHadPreviousCenter)
			{
				Indicator->PreviousCenter = State.FrozenCenter;
				MarkSafeZoneIndicatorDirty(
					Indicator, L"PreviousCenter");
			}
			if (State.bHadNextCenter)
			{
				Indicator->NextCenter = State.SavedNextCenter;
				MarkSafeZoneIndicatorDirty(
					Indicator, L"NextCenter");
			}
			Indicator->K2_SetActorLocation(
				State.FrozenCenter, false, nullptr, true);
		}

		if (State.bHasFrozenRadius)
		{
			if (State.bHadLastRadius)
			{
				Indicator->LastRadius = State.FrozenRadius;
				MarkSafeZoneIndicatorDirty(
					Indicator, L"LastRadius");
			}
			if (State.bHadPreviousRadius)
			{
				Indicator->PreviousRadius = State.FrozenRadius;
				MarkSafeZoneIndicatorDirty(
					Indicator, L"PreviousRadius");
			}
			if (State.bHadNextRadius)
			{
				Indicator->NextRadius = State.SavedNextRadius;
				MarkSafeZoneIndicatorDirty(
					Indicator, L"NextRadius");
			}
			if (Indicator->HasRadius())
				Indicator->Radius = State.FrozenRadius;
		}

		float ResumeStart = TimeSeconds;
		float ResumeFinish = TimeSeconds + 0.05f;
		if (State.bHasStartOffset &&
			State.bHasFinishOffset &&
			State.StartOffset > 0.f)
		{
			ResumeStart = TimeSeconds + State.StartOffset;
			ResumeFinish = TimeSeconds +
				(std::max)(State.FinishOffset,
					State.StartOffset + 0.05f);
		}
		else if (State.bHasFinishOffset &&
			State.FinishOffset > 0.f)
		{
			ResumeFinish = TimeSeconds +
				(std::max)(State.FinishOffset, 0.05f);
		}

		if (Indicator->HasSafeZoneStartShrinkTime())
		{
			Indicator->SafeZoneStartShrinkTime = ResumeStart;
			MarkSafeZoneIndicatorDirty(
				Indicator, L"SafeZoneStartShrinkTime");
		}
		if (Indicator->HasSafeZoneFinishShrinkTime())
		{
			Indicator->SafeZoneFinishShrinkTime = ResumeFinish;
			MarkSafeZoneIndicatorDirty(
				Indicator, L"SafeZoneFinishShrinkTime");
		}

		Indicator->ForceNetUpdate();
	}

	bool TrySetIndicatorPauseState(
		AFortSafeZoneIndicator* Indicator,
		bool bPaused)
	{
		if (!IsLiveSafeZoneObject(Indicator))
			return false;

		bool bTouchedIndicator = false;
		// Preserve the engine's preview bookkeeping when available, but do not
		// mistake this for the authoritative BR wall gate: modern indicators
		// expose a separate replicated bPaused bit.
		auto Function = Indicator->GetFunction(
			"SetSafeZonePausedForPreview");
		if (Function)
		{
			struct
			{
				uint8 bSetTo = 0;
				uint8 Padding[7]{};
			} Params;
			Params.bSetTo = bPaused ? 1 : 0;
			Indicator->ProcessEvent(Function, &Params);
			bTouchedIndicator = true;
		}

		bool bWallPauseChanged = false;
		bool bPreviewPauseChanged = false;
		const bool bWroteWallPause =
			TryWriteSafeZonePauseFlag(
				Indicator,
				"bPaused",
				bPaused,
				&bWallPauseChanged);
		const bool bWrotePreviewPause =
			TryWriteSafeZonePauseFlag(
				Indicator,
				"bPausedForPreview",
				bPaused,
				&bPreviewPauseChanged);
		if (bWallPauseChanged)
		{
			VersionFeatureAdapter::
				MarkReplicatedPropertyDirty(
					Indicator, L"bPaused");
		}
		if (bPreviewPauseChanged)
		{
			VersionFeatureAdapter::
				MarkReplicatedPropertyDirty(
					Indicator,
					L"bPausedForPreview");
		}
		if (bTouchedIndicator ||
			bWroteWallPause ||
			bWrotePreviewPause)
		{
			Indicator->ForceNetUpdate();
		}

		bool bReadBackPaused = false;
		return bWroteWallPause &&
			TryReadSafeZonePauseFlag(
				Indicator,
				"bPaused",
				bReadBackPaused) &&
			bReadBackPaused == bPaused;
	}

	bool ApplySafeZoneOwnerPauseFlags(
		AFortGameMode* GameMode,
		AFortGameStateAthena* GameState,
		UFortGameStateComponent_BattleRoyaleGamePhaseLogic*
			PhaseLogic,
		bool bPaused)
	{
		bool bApplied = false;
		bool bGameStateChanged = false;
		bool bPhaseLogicChanged = false;
		bApplied =
			TryWriteSafeZonePauseFlag(
				GameMode,
				"bSafeZonePaused",
				bPaused) ||
			bApplied;
		bApplied =
			TryWriteSafeZonePauseFlag(
				GameState,
				"bSafeZonePaused",
				bPaused,
				&bGameStateChanged) ||
			bApplied;
		bApplied =
			TryWriteSafeZonePauseFlag(
				PhaseLogic,
				"bSafeZonePaused",
				bPaused,
				&bPhaseLogicChanged) ||
			bApplied;
		if (bGameStateChanged && GameState)
		{
			VersionFeatureAdapter::
				MarkReplicatedPropertyDirty(
					GameState,
					L"bSafeZonePaused");
			GameState->ForceNetUpdate();
		}
		if (bPhaseLogicChanged && PhaseLogic)
		{
			VersionFeatureAdapter::
				MarkReplicatedPropertyDirty(
					PhaseLogic,
					L"bSafeZonePaused");
			if (GameState)
				GameState->ForceNetUpdate();
		}
		return bApplied;
	}

	void CaptureSafeZonePauseOffsets(
		AFortSafeZoneIndicator* Indicator, float TimeSeconds)
	{
		auto& State = GSafeZonePauseState;
		State.Indicator = Indicator;
		State.bHasStartOffset = false;
		State.bHasFinishOffset = false;

		if (!Indicator)
			return;

		if (Indicator->HasSafeZoneStartShrinkTime())
		{
			const float StartTime = Indicator->SafeZoneStartShrinkTime;
			if (std::isfinite(StartTime) &&
				StartTime >= 0.f)
			{
				State.StartOffset = StartTime - TimeSeconds;
				State.bHasStartOffset = true;
			}
		}

		if (Indicator->HasSafeZoneFinishShrinkTime())
		{
			const float FinishTime = Indicator->SafeZoneFinishShrinkTime;
			if (std::isfinite(FinishTime) &&
				FinishTime >= 0.f)
			{
				State.FinishOffset = FinishTime - TimeSeconds;
				State.bHasFinishOffset = true;
			}
		}
	}

	void PinSafeZonesStartTime(
		AFortGameStateAthena* GameState,
		UFortGameStateComponent_BattleRoyaleGamePhaseLogic*
			PhaseLogic,
		float TimeSeconds)
	{
		auto& State = GSafeZonePauseState;
		UObject* Owner = nullptr;
		float CurrentStartTime = -1.f;

		// Component-driven seasons own this schedule on the component. Legacy
		// seasons publish it on the GameState instead.
		if (PhaseLogic &&
			PhaseLogic->HasSafeZonesStartTime())
		{
			Owner = PhaseLogic;
			CurrentStartTime =
				PhaseLogic->SafeZonesStartTime;
		}
		else if (GameState &&
			GameState->HasSafeZonesStartTime())
		{
			Owner = GameState;
			CurrentStartTime =
				GameState->SafeZonesStartTime;
		}

		const bool bCurrentStartIsUsable =
			Owner &&
			std::isfinite(CurrentStartTime) &&
			CurrentStartTime >= 0.f;
		// Do not recapture remaining time when ownership migrates from GameState
		// to the component (or while that component is temporarily unresolved).
		// The value captured at the pause edge is the countdown the user expects
		// to resume with.
		if (Owner && State.SafeZonesStartOwner != Owner)
		{
			State.SafeZonesStartOwner = Owner;
			if (!State.bHasSafeZonesStartOffset &&
				bCurrentStartIsUsable)
			{
				State.bHasSafeZonesStartOffset = true;
				State.SafeZonesStartOffset =
					CurrentStartTime - TimeSeconds;
			}
		}
		else if (Owner &&
			!State.bHasSafeZonesStartOffset &&
			bCurrentStartIsUsable)
		{
			State.bHasSafeZonesStartOffset = true;
			State.SafeZonesStartOffset =
				CurrentStartTime - TimeSeconds;
		}

		if (!Owner || !State.bHasSafeZonesStartOffset)
			return;

		float PinnedStartTime =
			TimeSeconds + State.SafeZonesStartOffset;
		if (Owner == PhaseLogic)
		{
			PhaseLogic->SafeZonesStartTime =
				PinnedStartTime;
			VersionFeatureAdapter::
				MarkReplicatedPropertyDirty(
					PhaseLogic,
					L"SafeZonesStartTime");
			if (GameState)
				GameState->ForceNetUpdate();
		}
		else if (Owner == GameState)
		{
			GameState->SafeZonesStartTime =
				PinnedStartTime;
			VersionFeatureAdapter::
				MarkReplicatedPropertyDirty(
					GameState,
					L"SafeZonesStartTime");
			GameState->ForceNetUpdate();
		}
	}

	void RestoreSafeZoneIndicatorPauseOffsets(
		AFortSafeZoneIndicator* Indicator,
		float TimeSeconds)
	{
		auto& State = GSafeZonePauseState;
		if (!std::isfinite(TimeSeconds) ||
			Indicator != State.Indicator ||
			!IsLiveSafeZoneObject(Indicator))
		{
			return;
		}

		if (State.bHasStartOffset &&
			Indicator->HasSafeZoneStartShrinkTime())
		{
			Indicator->SafeZoneStartShrinkTime =
				TimeSeconds + State.StartOffset;
			MarkSafeZoneIndicatorDirty(
				Indicator, L"SafeZoneStartShrinkTime");
		}
		if (State.bHasFinishOffset &&
			Indicator->HasSafeZoneFinishShrinkTime())
		{
			Indicator->SafeZoneFinishShrinkTime =
				TimeSeconds + State.FinishOffset;
			MarkSafeZoneIndicatorDirty(
				Indicator, L"SafeZoneFinishShrinkTime");
		}
		Indicator->ForceNetUpdate();
	}

	void RestoreSafeZonePauseOffsets(
		AFortSafeZoneIndicator* Indicator,
		AFortGameStateAthena* GameState,
		UFortGameStateComponent_BattleRoyaleGamePhaseLogic*
			PhaseLogic,
		float TimeSeconds)
	{
		auto& State = GSafeZonePauseState;
		if (!std::isfinite(TimeSeconds))
			return;

		RestoreSafeZoneIndicatorPauseOffsets(
			Indicator, TimeSeconds);

		if (State.bHasSafeZonesStartOffset)
		{
			float RestoredStartTime =
				TimeSeconds +
				State.SafeZonesStartOffset;
			if (State.SafeZonesStartOwner ==
				PhaseLogic)
			{
				PhaseLogic->SafeZonesStartTime =
					RestoredStartTime;
				VersionFeatureAdapter::
					MarkReplicatedPropertyDirty(
						PhaseLogic,
						L"SafeZonesStartTime");
				if (GameState)
					GameState->ForceNetUpdate();
			}
			else if (State.SafeZonesStartOwner ==
				GameState)
			{
				GameState->SafeZonesStartTime =
					RestoredStartTime;
				VersionFeatureAdapter::
					MarkReplicatedPropertyDirty(
						GameState,
						L"SafeZonesStartTime");
				GameState->ForceNetUpdate();
			}
		}

		if (Indicator)
			Indicator->ForceNetUpdate();
	}

	void PinSafeZoneIndicatorDeadlines(
		AFortSafeZoneIndicator* Indicator,
		float TimeSeconds)
	{
		auto& State = GSafeZonePauseState;
		if (!Indicator || Indicator != State.Indicator ||
			!std::isfinite(TimeSeconds))
		{
			return;
		}

		if (State.bHasStartOffset &&
			Indicator->HasSafeZoneStartShrinkTime())
		{
			Indicator->SafeZoneStartShrinkTime =
				TimeSeconds + State.StartOffset;
			MarkSafeZoneIndicatorDirty(
				Indicator, L"SafeZoneStartShrinkTime");
		}
		if (State.bHasFinishOffset &&
			Indicator->HasSafeZoneFinishShrinkTime())
		{
			Indicator->SafeZoneFinishShrinkTime =
				TimeSeconds + State.FinishOffset;
			MarkSafeZoneIndicatorDirty(
				Indicator, L"SafeZoneFinishShrinkTime");
		}

		Indicator->ForceNetUpdate();
	}

	void RetirePausedSafeZoneIndicator(
		AFortSafeZoneIndicator* Indicator,
		float TimeSeconds)
	{
		auto& State = GSafeZonePauseState;
		if (!Indicator || Indicator != State.Indicator)
			return;

		if (IsLiveSafeZoneObject(Indicator))
		{
			// Use the same ordering as a normal resume: rebuild the schedule and
			// legacy target, clear the native gate, then repair anything the preview
			// helper changed and finally restore that actor's bookkeeping.
			RestoreSafeZoneIndicatorPauseOffsets(
				Indicator, TimeSeconds);
			PrepareLegacySafeZoneResume(
				Indicator, TimeSeconds);
			TrySetIndicatorPauseState(
				Indicator, false);
			RestoreSafeZoneIndicatorPauseOffsets(
				Indicator, TimeSeconds);
			PrepareLegacySafeZoneResume(
				Indicator, TimeSeconds);
		}
		RestoreSafeZonePauseBookkeepingForOwner(
			Indicator);
		if (IsLiveSafeZoneObject(Indicator))
			Indicator->ForceNetUpdate();
		State.bWallPauseGateApplied = false;
	}

	void AdoptPausedSafeZoneIndicator(
		AFortSafeZoneIndicator* Indicator,
		AFortGameMode* GameMode,
		AFortGameStateAthena* GameState,
		UFortGameStateComponent_BattleRoyaleGamePhaseLogic*
			PhaseLogic,
		float TimeSeconds)
	{
		auto& State = GSafeZonePauseState;
		State.Indicator = Indicator;
		State.bHasStartOffset = false;
		State.bHasFinishOffset = false;
		ResetLegacySafeZoneGeometrySnapshot();

		if (std::isfinite(TimeSeconds))
		{
			CaptureSafeZonePauseOffsets(
				Indicator, TimeSeconds);
			PinSafeZonesStartTime(
				GameState,
				PhaseLogic,
				TimeSeconds);
		}

		ApplySafeZonePauseBookkeeping(
			GameMode,
			GameState,
			PhaseLogic,
			Indicator);
		State.bWallPauseGateApplied =
			Indicator &&
			TrySetIndicatorPauseState(
				Indicator, true);
		if (!State.bWallPauseGateApplied && Indicator)
		{
			CaptureLegacySafeZoneGeometry(Indicator);
			ApplyLegacySafeZoneGeometryFreeze(Indicator);
			PinSafeZoneIndicatorDeadlines(
				Indicator, TimeSeconds);
		}
	}
}

bool UFortGameStateComponent_BattleRoyaleGamePhaseLogic::IsSafeZonePaused()
{
	auto World = UWorld::GetWorld();
	ResetSafeZonePauseStateForWorld(World);
	if (GSafeZonePauseState.bHasRequest)
	{
		GSafeZonePauseSnapshot.store(
			GSafeZonePauseState.bRequestedPaused,
			std::memory_order_release);
		return GSafeZonePauseState.bRequestedPaused;
	}

	auto GameMode = World ? (AFortGameMode*)World->AuthorityGameMode : nullptr;
	bool bNativePaused = false;
	if (TryReadSafeZonePauseFlag(
		GameMode, "bSafeZonePaused", bNativePaused))
	{
		// The native GameMode flag is scoped to the current match. Mirroring it
		// also prevents the process-static UI flag from leaking across travel.
		bPausedZone = bNativePaused;
		GSafeZonePauseSnapshot.store(
			bNativePaused,
			std::memory_order_release);
		return bNativePaused;
	}

	GSafeZonePauseSnapshot.store(
		bPausedZone,
		std::memory_order_release);
	return bPausedZone;
}

bool UFortGameStateComponent_BattleRoyaleGamePhaseLogic::
	GetSafeZonePausedSnapshot()
{
	return GSafeZonePauseSnapshot.load(
		std::memory_order_acquire);
}

void UFortGameStateComponent_BattleRoyaleGamePhaseLogic::
	RequestSafeZonePaused(bool bPaused)
{
	// ImGui runs from the render hook. Only publish intent here; Unreal object
	// traversal, ProcessEvent, replication and console commands are consumed
	// by TickSafeZonePause on the game/network thread.
	GSafeZonePauseRequest.store(
		bPaused ? 1 : 0,
		std::memory_order_release);
}

void UFortGameStateComponent_BattleRoyaleGamePhaseLogic::SetSafeZonePaused(bool bPaused)
{
	auto World = UWorld::GetWorld();
	ResetSafeZonePauseStateForWorld(World);
	auto& State = GSafeZonePauseState;
	const bool bStateTransition =
		!State.bHasRequest ||
		State.bRequestedPaused != bPaused;
	auto GameMode = World
		? (AFortGameMode*)World->AuthorityGameMode
		: nullptr;
	auto GameState = World
		? (AFortGameStateAthena*)World->GameState
		: nullptr;
	auto PhaseLogic =
		GetCurrentSafeZonePhaseLogic(World);
	auto Indicator =
		GetCurrentSafeZoneIndicator(World, GameMode);
	const float TimeSeconds =
		World
			? (float)UGameplayStatics::
				GetTimeSeconds(World)
			: 0.f;

	// An indicator can be replaced between the final paused tick and a resume
	// request. Migrate the paused snapshot first so the request always resumes
	// the authoritative wall rather than clearing an uncaptured replacement and
	// leaving the old actor collapsed/paused.
	if (State.bHasRequest &&
		State.bRequestedPaused &&
		Indicator != State.Indicator)
	{
		RetirePausedSafeZoneIndicator(
			State.Indicator, TimeSeconds);
		AdoptPausedSafeZoneIndicator(
			Indicator,
			GameMode,
			GameState,
			PhaseLogic,
			TimeSeconds);
	}

	if (bPaused)
	{
		if (bStateTransition)
		{
			State.Indicator = Indicator;
			State.bHasStartOffset = false;
			State.bHasFinishOffset = false;
			State.SafeZonesStartOwner = nullptr;
			State.bHasSafeZonesStartOffset = false;
			State.FloatRestoreCount = 0;
			State.FloatRestores = {};
			ResetLegacySafeZoneGeometrySnapshot();
			State.PauseWorldTime =
				std::isfinite(TimeSeconds)
					? TimeSeconds
					: 0.f;
			if (std::isfinite(TimeSeconds))
			{
				CaptureSafeZonePauseOffsets(
					Indicator, TimeSeconds);
				PinSafeZonesStartTime(
					GameState,
					PhaseLogic,
					TimeSeconds);
			}
		}

		// Bookkeeping must be ready before either pause bit changes. Builds in
		// the 13-19 range use these values to keep their HUD and phase handoff
		// from jumping when the wall resumes.
		ApplySafeZonePauseBookkeeping(
			GameMode,
			GameState,
			PhaseLogic,
			Indicator);

		State.bPhasePauseGateApplied =
			ApplySafeZoneOwnerPauseFlags(
				GameMode,
				GameState,
				PhaseLogic,
				true);
		State.bWallPauseGateApplied =
			Indicator &&
			TrySetIndicatorPauseState(
				Indicator, true);

		if (!State.bWallPauseGateApplied && Indicator)
		{
			if (!State.bUsingLegacyGeometryFallback)
				CaptureLegacySafeZoneGeometry(Indicator);
			ApplyLegacySafeZoneGeometryFreeze(Indicator);
			PinSafeZoneIndicatorDeadlines(
				Indicator, TimeSeconds);
		}
		else if (State.bWallPauseGateApplied)
		{
			ResetLegacySafeZoneGeometrySnapshot();
		}
	}
	else if (bStateTransition)
	{
		// Rebuild the complete schedule and, on the oldest builds, restore the
		// intended target before any phase/wall gate is allowed to clear.
		RestoreSafeZonePauseOffsets(
			Indicator,
			GameState,
			PhaseLogic,
			TimeSeconds);
		PrepareLegacySafeZoneResume(
			Indicator, TimeSeconds);

		const bool bWallCleared =
			!Indicator ||
			TrySetIndicatorPauseState(
				Indicator, false);
		ApplySafeZoneOwnerPauseFlags(
			GameMode,
			GameState,
			PhaseLogic,
			false);

		// The preview helper on some modern indicators performs its own
		// deadline bookkeeping. Our captured pair is authoritative, so apply it
		// once more after that helper and before returning to the world tick.
		RestoreSafeZonePauseOffsets(
			Indicator,
			GameState,
			PhaseLogic,
			TimeSeconds);
		PrepareLegacySafeZoneResume(
			Indicator, TimeSeconds);
		RestoreSafeZonePauseBookkeeping(
			GameState, Indicator);

		State.bPhasePauseGateApplied = false;
		if (State.bWallPauseGateApplied &&
			Indicator && !bWallCleared)
		{
			SDK::DbgLog(
				"[SafeZone] warning: replicated wall pause "
				"did not acknowledge resume indicator=%p\n",
				(void*)Indicator);
		}
		State.bWallPauseGateApplied = false;
		State.bUsingLegacyGeometryFallback = false;
	}
	else
	{
		// A duplicate resume request is still allowed to repair stale reflected
		// bits without disturbing an already-running timeline.
		ApplySafeZoneOwnerPauseFlags(
			GameMode,
			GameState,
			PhaseLogic,
			false);
		if (Indicator)
			TrySetIndicatorPauseState(
				Indicator, false);
	}

	State.bHasRequest = true;
	State.bRequestedPaused = bPaused;
	State.Indicator = Indicator;
	State.bLoggedWallPause = false;
	State.bLoggedBackstop = false;
	bPausedZone = bPaused;
	GSafeZonePauseSnapshot.store(
		bPaused,
		std::memory_order_release);

	if (Indicator)
	{
		Indicator->ForceNetUpdate();
	}
}

void UFortGameStateComponent_BattleRoyaleGamePhaseLogic::
	TickSafeZonePause()
{
	auto World = UWorld::GetWorld();
	ResetSafeZonePauseStateForWorld(World);
	if (World)
	{
		const int32 RequestedPause =
			GSafeZonePauseRequest.exchange(
				-1, std::memory_order_acq_rel);
		if (RequestedPause >= 0)
		{
			SetSafeZonePaused(
				RequestedPause != 0);
		}
	}
	auto& State = GSafeZonePauseState;
	if (!World || !State.bHasRequest ||
		!State.bRequestedPaused)
	{
		return;
	}

	auto GameMode =
		(AFortGameMode*)World->AuthorityGameMode;

	auto GameState =
		(AFortGameStateAthena*)World->GameState;
	auto PhaseLogic =
		GetCurrentSafeZonePhaseLogic(World);
	State.bPhasePauseGateApplied =
		ApplySafeZoneOwnerPauseFlags(
			GameMode,
			GameState,
			PhaseLogic,
			true);

	auto Indicator =
		GetCurrentSafeZoneIndicator(World, GameMode);
	if (Indicator != State.Indicator)
	{
		const float TimeSeconds =
			(float)UGameplayStatics::
				GetTimeSeconds(World);
		RetirePausedSafeZoneIndicator(
			State.Indicator, TimeSeconds);
		AdoptPausedSafeZoneIndicator(
			Indicator,
			GameMode,
			GameState,
			PhaseLogic,
			TimeSeconds);
		State.bLoggedWallPause = false;
	}
	else
	{
		bool bIndicatorWallPauseChanged = false;
		bool bIndicatorPreviewPauseChanged = false;
		const bool bIndicatorWallPauseApplied =
			Indicator &&
			TryWriteSafeZonePauseFlag(
				Indicator,
				"bPaused",
				true,
				&bIndicatorWallPauseChanged);
		if (Indicator)
		{
			TryWriteSafeZonePauseFlag(
				Indicator,
				"bPausedForPreview",
				true,
				&bIndicatorPreviewPauseChanged);
			if (bIndicatorWallPauseChanged)
			{
				VersionFeatureAdapter::
					MarkReplicatedPropertyDirty(
						Indicator,
						L"bPaused");
			}
			if (bIndicatorPreviewPauseChanged)
			{
				VersionFeatureAdapter::
					MarkReplicatedPropertyDirty(
						Indicator,
						L"bPausedForPreview");
			}
			if (bIndicatorWallPauseChanged ||
				bIndicatorPreviewPauseChanged)
			{
				Indicator->ForceNetUpdate();
			}
		}
		bool bReadBackWallPaused = false;
		State.bWallPauseGateApplied =
			bIndicatorWallPauseApplied &&
			TryReadSafeZonePauseFlag(
				Indicator,
				"bPaused",
				bReadBackWallPaused) &&
			bReadBackWallPaused;
	}

	ApplySafeZonePauseBookkeeping(
		GameMode,
		GameState,
		PhaseLogic,
		Indicator);

	if (!Indicator)
	{
		const float TimeSeconds =
			(float)UGameplayStatics::
				GetTimeSeconds(World);
		if (std::isfinite(TimeSeconds))
		{
			// Storm formation can be scheduled before the indicator exists.
			// Its owner flag does not consistently freeze this timestamp.
			PinSafeZonesStartTime(
				GameState,
				PhaseLogic,
				TimeSeconds);
		}
		return;
	}

	if (State.bWallPauseGateApplied)
	{
		if (!State.bLoggedWallPause)
		{
			State.bLoggedWallPause = true;
			SDK::DbgLog(
				"[SafeZone] replicated wall pause armed "
				"version=%.2f indicator=%p phaseGate=%d\n",
				VersionInfo.FortniteVersion,
				(void*)Indicator,
				State.bPhasePauseGateApplied ? 1 : 0);
		}
		return;
	}

	const float TimeSeconds =
		(float)UGameplayStatics::GetTimeSeconds(World);
	if (!std::isfinite(TimeSeconds))
		return;

	PinSafeZonesStartTime(
		(AFortGameStateAthena*)World->GameState,
		PhaseLogic,
		TimeSeconds);

	if (!State.bHasStartOffset &&
		!State.bHasFinishOffset)
	{
		CaptureSafeZonePauseOffsets(
			Indicator, TimeSeconds);
	}

	if (!State.bUsingLegacyGeometryFallback)
		CaptureLegacySafeZoneGeometry(Indicator);
	ApplyLegacySafeZoneGeometryFreeze(Indicator);
	PinSafeZoneIndicatorDeadlines(
		Indicator, TimeSeconds);

	if (!State.bLoggedBackstop)
	{
		State.bLoggedBackstop = true;
		SDK::DbgLog(
			"[SafeZone] exact legacy pause fallback armed "
			"version=%.2f indicator=%p phaseGate=%d "
			"startOffset=%.2f finishOffset=%.2f\n",
			VersionInfo.FortniteVersion,
			(void*)Indicator,
			State.bPhasePauseGateApplied ? 1 : 0,
			State.bHasStartOffset ? State.StartOffset : -1.f,
			State.bHasFinishOffset ? State.FinishOffset : -1.f);
	}
}

void UFortGameStateComponent_BattleRoyaleGamePhaseLogic::SetGamePhase(EAthenaGamePhase GamePhase)
{
	auto SetGamePhaseInternal = (void(*)(UFortGameStateComponent_BattleRoyaleGamePhaseLogic*, EAthenaGamePhase)) SetGamePhase_;

	if (SetGamePhaseInternal)
		return SetGamePhaseInternal(this, GamePhase);
	else
	{
		static auto GamePhaseOffset = this->GetOffset("GamePhase");
		auto& _GamePhase = *(EAthenaGamePhase*)(__int64(this) + GamePhaseOffset);

		auto OldGamePhase = _GamePhase;
		_GamePhase = GamePhase;
		OnRep_GamePhase(OldGamePhase);
	}
}

void UFortGameStateComponent_BattleRoyaleGamePhaseLogic::SetGamePhaseStep(EAthenaGamePhaseStep GamePhaseStep)
{
	static auto GamePhaseStepOffset = this->GetOffset("GamePhaseStep");
	auto& _GamePhaseStep = *(EAthenaGamePhaseStep*)(__int64(this) + GamePhaseStepOffset);

	_GamePhaseStep = GamePhaseStep;
	HandleGamePhaseStepChanged(GamePhaseStep);
}


void UFortGameStateComponent_BattleRoyaleGamePhaseLogic::HandleMatchHasStarted(AFortGameMode* GameMode)
{
	AFortGameMode::TickSupplyDropSuppression();
	HandleMatchHasStartedOG(GameMode);
	AFortGameMode::TickSupplyDropSuppression(true);
	auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(GameMode);

	if (!bSkipWarmup)
	{
		auto Time = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
		const float WarmupDuration =
			FConfiguration::bAutoBusStart.load()
				? FConfiguration::BusStartDelay.load()
				: 99999999.f;

		GamePhaseLogic->WarmupCountdownStartTime = Time;
		GamePhaseLogic->WarmupCountdownEndTime = Time + WarmupDuration;
		GamePhaseLogic->WarmupCountdownDuration = 10.f;
		GamePhaseLogic->WarmupEarlyCountdownDuration = WarmupDuration - 10.f;

		GamePhaseLogic->SetGamePhase(EAthenaGamePhase::Warmup);
		GamePhaseLogic->SetGamePhaseStep(EAthenaGamePhaseStep::Warmup);
	}
	else
	{
		printf("[GamePhaseLogic] Skipping warmup\n");
		GamePhaseLogic->StartAircraftPhase();
	}
}

// thanks mariki
struct FStormCircle
{
	FVector Center;
	float Radius;
};

std::vector<FStormCircle> StormCircles;

struct FVector2D
{
public:
	double X;
	double Y;
};

inline FVector2D GetSafeNormal(FVector2D v)
{
	double sizeSq = v.X * v.X + v.Y * v.Y;
	if (sizeSq > KINDA_SMALL_NUMBER)
	{
		double inv = 1. / sqrt(sizeSq);
		return FVector2D(v.X * inv, v.Y * inv);
	}
	return FVector2D(0.f, 0.f);
}

inline FVector GetSafeNormal(FVector v)
{
	double sizeSq = v.X * v.X + v.Y * v.Y + v.Z * v.Z;
	if (sizeSq > KINDA_SMALL_NUMBER)
	{
		double inv = 1. / sqrt(sizeSq);
		return FVector(v.X * inv, v.Y * inv, v.Z * inv);
	}
	return FVector(0.f, 0.f, 0.f);
}

inline bool IsNearlyZero(FVector2D v)
{
	return (v.X * v.X + v.Y * v.Y) < KINDA_SMALL_NUMBER * KINDA_SMALL_NUMBER;
}

FVector ClampToPlayableBounds(const FVector& Candidate, float Radius, const FBoxSphereBounds& Bounds)
{
	FVector Clamped = Candidate;

	Clamped.X = std::clamp(Clamped.X, Bounds.Origin.X - Bounds.BoxExtent.X + Radius, Bounds.Origin.X + Bounds.BoxExtent.X - Radius);
	Clamped.Y = std::clamp(Clamped.Y, Bounds.Origin.Y - Bounds.BoxExtent.Y + Radius, Bounds.Origin.Y + Bounds.BoxExtent.Y - Radius);

	return Clamped;
}

#define INV_PI			(0.31830988618f)
#define HALF_PI			(1.57079632679f)
#define PI 					(3.1415926535897932f)

float RadiansToDegrees(float Radians)
{
	return Radians * (180.0f / PI);
}
inline void SinCos(float* ScalarSin, float* ScalarCos, float  Value)
{
	// Map Value to y in [-pi,pi], x = 2*pi*quotient + remainder.
	float quotient = (INV_PI * 0.5f) * Value;
	if (Value >= 0.0f)
	{
		quotient = (float)((int)(quotient + 0.5f));
	}
	else
	{
		quotient = (float)((int)(quotient - 0.5f));
	}
	float y = Value - (2.0f * PI) * quotient;

	// Map y to [-pi/2,pi/2] with sin(y) = sin(Value).
	float sign;
	if (y > HALF_PI)
	{
		y = PI - y;
		sign = -1.0f;
	}
	else if (y < -HALF_PI)
	{
		y = -PI - y;
		sign = -1.0f;
	}
	else
	{
		sign = +1.0f;
	}

	float y2 = y * y;

	// 11-degree minimax approximation
	*ScalarSin = (((((-2.3889859e-08f * y2 + 2.7525562e-06f) * y2 - 0.00019840874f) * y2 + 0.0083333310f) * y2 - 0.16666667f) * y2 + 1.0f) * y;

	// 10-degree minimax approximation
	float p = ((((-2.6051615e-07f * y2 + 2.4760495e-05f) * y2 - 0.0013888378f) * y2 + 0.041666638f) * y2 - 0.5f) * y2 + 1.0f;
	*ScalarCos = sign * p;
}
struct FQuat FRotator::Quaternion() const
{
#if PLATFORM_ENABLE_VECTORINTRINSICS
	const VectorRegister Angles = MakeVectorRegister(Rotator.Pitch, Rotator.Yaw, Rotator.Roll, 0.0f);
	const VectorRegister HalfAngles = VectorMultiply(Angles, DEG_TO_RAD_HALF);

	VectorRegister SinAngles, CosAngles;
	VectorSinCos(&SinAngles, &CosAngles, &HalfAngles);

	// Vectorized conversion, measured 20% faster than using scalar version after VectorSinCos.
	// Indices within VectorRegister (for shuffles): P=0, Y=1, R=2
	const VectorRegister SR = VectorReplicate(SinAngles, 2);
	const VectorRegister CR = VectorReplicate(CosAngles, 2);

	const VectorRegister SY_SY_CY_CY_Temp = VectorShuffle(SinAngles, CosAngles, 1, 1, 1, 1);

	const VectorRegister SP_SP_CP_CP = VectorShuffle(SinAngles, CosAngles, 0, 0, 0, 0);
	const VectorRegister SY_CY_SY_CY = VectorShuffle(SY_SY_CY_CY_Temp, SY_SY_CY_CY_Temp, 0, 2, 0, 2);

	const VectorRegister CP_CP_SP_SP = VectorShuffle(CosAngles, SinAngles, 0, 0, 0, 0);
	const VectorRegister CY_SY_CY_SY = VectorShuffle(SY_SY_CY_CY_Temp, SY_SY_CY_CY_Temp, 2, 0, 2, 0);

	const uint32 Neg = uint32(1 << 31);
	const uint32 Pos = uint32(0);
	const VectorRegister SignBitsLeft = MakeVectorRegister(Pos, Neg, Pos, Pos);
	const VectorRegister SignBitsRight = MakeVectorRegister(Neg, Neg, Neg, Pos);
	const VectorRegister LeftTerm = VectorBitwiseXor(SignBitsLeft, VectorMultiply(CR, VectorMultiply(SP_SP_CP_CP, SY_CY_SY_CY)));
	const VectorRegister RightTerm = VectorBitwiseXor(SignBitsRight, VectorMultiply(SR, VectorMultiply(CP_CP_SP_SP, CY_SY_CY_SY)));

	FQuat RotationQuat;
	const VectorRegister Result = VectorAdd(LeftTerm, RightTerm);
	VectorStoreAligned(Result, &RotationQuat);
#else
	const float DEG_TO_RAD = PI / (180.f);
	const float DIVIDE_BY_2 = DEG_TO_RAD / 2.f;
	float SP, SY, SR;
	float CP, CY, CR;

	SinCos(&SP, &CP, Pitch * DIVIDE_BY_2);
	SinCos(&SY, &CY, Yaw * DIVIDE_BY_2);
	SinCos(&SR, &CR, Roll * DIVIDE_BY_2);

	FQuat RotationQuat{};
	RotationQuat.X = CR * SP * SY - SR * CP * CY;
	RotationQuat.Y = -CR * SP * CY - SR * CP * SY;
	RotationQuat.Z = CR * CP * SY - SR * SP * CY;
	RotationQuat.W = CR * CP * CY + SR * SP * SY;
#endif // PLATFORM_ENABLE_VECTORINTRINSICS

#if ENABLE_NAN_DIAGNOSTIC || DO_CHECK
	// Very large inputs can cause NaN's. Want to catch this here
	ensureMsgf(!RotationQuat.ContainsNaN(), TEXT("Invalid input to FRotator::Quaternion - generated NaN output: %s"), *RotationQuat.ToString());
#endif

	return RotationQuat;
}

void UFortGameStateComponent_BattleRoyaleGamePhaseLogic::GenerateStormCircles(AFortAthenaMapInfo* MapInfo)
{
	if (FConfiguration::bCustomSafeZone)
		GUI::ResolveCustomSafeZoneForMap(MapInfo);

	if (StormCircles.size() > 0)
		return;

	StormCircles.clear();

	auto FRandSeeded = [&]() { return (float) rand() / 32767.f; };

	float Radii[13] = { 150000, 120000, 95000, 70000, 55000, 32500, 20000, 10000, 5000, 2500, 1650, 1090, 0 };

	FVector FirstCenter = MapInfo->GetMapCenter();
	StormCircles.push_back(FStormCircle{ FirstCenter, Radii[0] });

	float DirAngle = FRandSeeded() * 2.f * PI;
	float DirSin, DirCos;
	SinCos(&DirSin, &DirCos, DirAngle);
	FVector2D Dir(DirCos, DirSin);
	/*for (int i = 1; i < 5; ++i)
	{
		FVector PrevCenter = StormCircles[i - 1].Center;
		float rPrev = StormCircles[i - 1].Radius;
		float rNew = Radii[i];

		double baseAngle = atan2(Dir.Y, Dir.X);
		float delta = (FRandSeeded() - 0.5f) * (PI / 36.f);
		double angle = baseAngle + delta;

		FVector2D MoveDir(cos(angle), sin(angle));
		MoveDir = GetSafeNormal(MoveDir);

		float f_i = 0.4f + FRandSeeded() * 0.5f;
		float Offset = f_i * (rPrev - rNew);

		FVector NewCenter = PrevCenter + FVector(MoveDir.X, MoveDir.Y, 0.f) * Offset;

		StormCircles.push_back({ NewCenter, rNew });
		Dir = MoveDir;
	}*/

	/*for (int i = 5; i < 8; ++i)
	{
		FVector PrevCenter = StormCircles[i - 1].Center;
		float rPrev = StormCircles[i - 1].Radius;
		float rNew = Radii[i];

		float angle = FRandSeeded() * 2.f * PI;
		float s, c; SinCos(&s, &c, angle);
		FVector2D RandDir(c, s);
		RandDir = GetSafeNormal(RandDir);

		float d = sqrt(rPrev * rPrev - rNew * rNew);;
		FVector NewCenter = PrevCenter + FVector(RandDir.X, RandDir.Y, 0.f) * d;

		NewCenter = ClampToPlayableBounds(NewCenter, rNew, MapInfo->CachedPlayableBoundsForClients);

		StormCircles.push_back(FStormCircle{ NewCenter, rNew });
	}*/

	//FVector RefCenter = StormCircles[6].Center;
	//float RefRadius = StormCircles[6].Radius;

	//for (int i = 8; i < 13; ++i)
	for (int i = 1; i < 7; ++i)
	{
		FVector RefCenter = StormCircles[i - 1].Center;
		float RefRadius = StormCircles[i - 1].Radius;
		float angle = FRandSeeded() * 2.f * PI;
		float s, c; SinCos(&s, &c, angle);
		float Dist = FRandSeeded() * RefRadius * 0.4f;
		FVector2D RandDir(c, s);
		RandDir = GetSafeNormal(RandDir);

		FVector NewCenter = RefCenter + FVector(RandDir.X, RandDir.Y, 0.f) * Dist;

		NewCenter = ClampToPlayableBounds(NewCenter, Radii[i], MapInfo->CachedPlayableBoundsForClients);

		StormCircles.push_back(FStormCircle{ NewCenter, Radii[i] });
	}

	for (int i = 7; i < 13; ++i)
	{
		FVector PrevCenter = StormCircles[i - 1].Center;
		float rPrev = StormCircles[i - 1].Radius;
		float rNew = Radii[i];

		float angle = FRandSeeded() * 2.f * PI;
		float s, c; SinCos(&s, &c, angle);
		FVector2D RandDir(c, s);
		RandDir = GetSafeNormal(RandDir);

		float d = sqrt(rPrev * rPrev - rNew * rNew);;
		FVector NewCenter = PrevCenter + FVector(RandDir.X, RandDir.Y, 0.f) * d;

		NewCenter = ClampToPlayableBounds(NewCenter, rNew, MapInfo->CachedPlayableBoundsForClients);

		StormCircles.push_back(FStormCircle{ NewCenter, rNew });
	}
}
// end

AFortSafeZoneIndicator* UFortGameStateComponent_BattleRoyaleGamePhaseLogic::SetupSafeZoneIndicator()
{
	// thanks heliato

	if (!this->SafeZoneIndicator)
	{
		AFortSafeZoneIndicator* SafeZoneIndicator = UWorld::SpawnActor<AFortSafeZoneIndicator>(SafeZoneIndicatorClass, FVector{});

		if (SafeZoneIndicator)
		{
			auto GameState = (AFortGameStateAthena*) UWorld::GetWorld()->GameState;
			if (FConfiguration::bCustomSafeZone)
				GUI::ResolveCustomSafeZoneForMap(GameState->MapInfo);
			FFortSafeZoneDefinition& SafeZoneDefinition = GameState->MapInfo->SafeZoneDefinition;
			float SafeZoneCount = SafeZoneDefinition.Count.Evaluate();

			auto& Array = SafeZoneIndicator->SafeZonePhases;

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
				PhaseInfo->TimeBetweenStormCapDamage = TimeBetweenStormCapDamage.Evaluate(i);
				PhaseInfo->StormCapDamagePerTick = StormCapDamagePerTick.Evaluate(i);
				PhaseInfo->StormCampingIncrementTimeAfterDelay = StormCampingIncrementTimeAfterDelay.Evaluate(i);
				PhaseInfo->StormCampingInitialDelayTime = StormCampingInitialDelayTime.Evaluate(i);
				PhaseInfo->MegaStormGridCellThickness = (int)SafeZoneDefinition.MegaStormGridCellThickness.Evaluate(i);

				if (FFortSafeZonePhaseInfo::HasUsePOIStormCenter())
					PhaseInfo->UsePOIStormCenter = false;

				PhaseInfo->Center = StormCircles[(int)i].Center;

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

		this->SafeZoneIndicator = SafeZoneIndicator;
		OnRep_SafeZoneIndicator();
	}

	return this->SafeZoneIndicator;
}

void UFortGameStateComponent_BattleRoyaleGamePhaseLogic::StartNewSafeZonePhase(int NewSafeZonePhase, bool bInitial)
{
	float TimeSeconds = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
	auto& Array = SafeZoneIndicator->SafeZonePhases;

	if (Array.IsValidIndex(NewSafeZonePhase))
	{
		if (Array.IsValidIndex(NewSafeZonePhase - 1))
		{
			auto& PreviousPhaseInfo = Array.Get(NewSafeZonePhase - 1, FFortSafeZonePhaseInfo::Size());

			SafeZoneIndicator->PreviousCenter = PreviousPhaseInfo.Center;
			SafeZoneIndicator->PreviousRadius = PreviousPhaseInfo.Radius;
		}

		auto& PhaseInfo = Array.Get(NewSafeZonePhase, FFortSafeZonePhaseInfo::Size());
		if (FConfiguration::bLateGame && FConfiguration::bCustomSafeZone)
		{
			auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;
			if (GameState && GameState->HasMapInfo() && GameState->MapInfo)
				GUI::ResolveCustomSafeZoneForMap(GameState->MapInfo);
			PhaseInfo.Center = FConfiguration::CustomSafeZoneCenter;
			PhaseInfo.Radius = FConfiguration::CustomSafeZoneRadius;
		}

		SafeZoneIndicator->NextCenter = PhaseInfo.Center;
		SafeZoneIndicator->NextRadius = PhaseInfo.Radius;
		SafeZoneIndicator->NextMegaStormGridCellThickness = PhaseInfo.MegaStormGridCellThickness;

		if (Array.IsValidIndex(NewSafeZonePhase + 1))
		{
			auto& NextPhaseInfo = Array.Get(NewSafeZonePhase + 1, FFortSafeZonePhaseInfo::Size());

			if (SafeZoneIndicator->FutureReplicator)
			{
				SafeZoneIndicator->FutureReplicator->NextNextCenter = NextPhaseInfo.Center;
				SafeZoneIndicator->FutureReplicator->NextNextRadius = NextPhaseInfo.Radius;
			}

			SafeZoneIndicator->NextNextCenter = NextPhaseInfo.Center;
			SafeZoneIndicator->NextNextRadius = NextPhaseInfo.Radius;
			SafeZoneIndicator->NextNextMegaStormGridCellThickness = NextPhaseInfo.MegaStormGridCellThickness;
		}

		SafeZoneIndicator->SafeZoneStartShrinkTime = TimeSeconds + PhaseInfo.WaitTime;
		SafeZoneIndicator->SafeZoneFinishShrinkTime = SafeZoneIndicator->SafeZoneStartShrinkTime + PhaseInfo.ShrinkTime;

		SafeZoneIndicator->CurrentDamageInfo = PhaseInfo.DamageInfo;
		SafeZoneIndicator->OnRep_CurrentDamageInfo();

		auto OldPhase = SafeZoneIndicator->CurrentPhase;
		SafeZoneIndicator->CurrentPhase = NewSafeZonePhase;
		SafeZoneIndicator->OnRep_CurrentPhase();

		SafeZoneIndicator->OnSafeZonePhaseChanged.Process();

		auto& SafeZoneState = *(uint8_t*)(__int64(&SafeZoneIndicator->FutureReplicator) - 0x4);
		SafeZoneState = 2;
		bool bInitial = OldPhase <= 0;

		SafeZoneIndicator->OnSafeZoneStateChange(2, bInitial);
		SafeZoneIndicator->SafezoneStateChangedDelegate.Process(SafeZoneIndicator, 2);

		if (FConfiguration::bLateGame && FConfiguration::bCustomSafeZone)
		{
			FVector Center = FConfiguration::CustomSafeZoneCenter;
			float Radius = FConfiguration::CustomSafeZoneRadius;
			if (SafeZoneIndicator->HasLastCenter()) SafeZoneIndicator->LastCenter = Center;
			if (SafeZoneIndicator->HasPreviousCenter()) SafeZoneIndicator->PreviousCenter = Center;
			if (SafeZoneIndicator->HasNextCenter()) SafeZoneIndicator->NextCenter = Center;
			if (SafeZoneIndicator->HasNextNextCenter()) SafeZoneIndicator->NextNextCenter = Center;
			if (SafeZoneIndicator->HasLastRadius()) SafeZoneIndicator->LastRadius = Radius;
			if (SafeZoneIndicator->HasPreviousRadius()) SafeZoneIndicator->PreviousRadius = Radius;
			if (SafeZoneIndicator->HasNextRadius()) SafeZoneIndicator->NextRadius = Radius;
			if (SafeZoneIndicator->HasNextNextRadius()) SafeZoneIndicator->NextNextRadius = Radius;
			if (SafeZoneIndicator->HasRadius()) SafeZoneIndicator->Radius = Radius;
			if (SafeZoneIndicator->HasFutureReplicator() && SafeZoneIndicator->FutureReplicator)
			{
				if (SafeZoneIndicator->FutureReplicator->HasNextNextCenter())
					SafeZoneIndicator->FutureReplicator->NextNextCenter = Center;
				if (SafeZoneIndicator->FutureReplicator->HasNextNextRadius())
					SafeZoneIndicator->FutureReplicator->NextNextRadius = Radius;
			}
			SafeZoneIndicator->ForceNetUpdate();
			SDK::DbgLog(
				"[SafeZoneMap] active component zone phase=%d center=(%.1f, %.1f, %.1f) radius=%.1f\n",
				NewSafeZonePhase, Center.X, Center.Y, Center.Z, Radius);
		}

		SetGamePhaseStep(EAthenaGamePhaseStep::StormHolding);
		if (FConfiguration::bLateGame &&
			FConfiguration::bLateGameLongZone)
		{
			SetSafeZonePaused(true);
		}

		auto GameMode = (AFortGameModeAthena*)UWorld::GetWorld()->AuthorityGameMode;
		if (!bInitial)
			for (auto& UncastedPlayer : GameMode->AlivePlayers)
			{
				auto PlayerController = (AFortPlayerControllerAthena*)UncastedPlayer;

				UFortQuestManager::TrySendStatEvent(PlayerController,
					EFortQuestObjectiveStatEvent::GetStormPhase(), 1, false);
			}
	}
}

void UFortGameStateComponent_BattleRoyaleGamePhaseLogic::StartAircraftPhase()
{
	static auto GamePhaseOffset = this->GetOffset("GamePhase");
	auto& _GamePhase = *(EAthenaGamePhase*)(__int64(this) + GamePhaseOffset);

	if (_GamePhase >= EAthenaGamePhase::Aircraft)
		return;

	auto Time = UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());

	auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
	auto Playlist = AFortGameMode::GetActivePlaylist(GameMode->GameState);
	
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

	if (FConfiguration::bJoinInProgress || (Playlist && Playlist->bAllowJoinInProgress))
		*(bool*)(uint64_t(&GameMode->WarmupRequiredPlayerCount) - 4) = false;

	if (bSkipAircraft)
	{
		printf("[GamePhaseLogic] Skipping aircraft\n");
		SetGamePhase(FConfiguration::IsKnownS27CustomMapPlaylist() && bSkipWarmup ? EAthenaGamePhase::None : EAthenaGamePhase::SafeZones);
		SetGamePhaseStep(EAthenaGamePhaseStep::StormForming);

		return;
	}

	auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;

	if (GameState->MapInfo->FlightInfos.Num() > 0)
	{
		//TArray<TWeakObjectPtr<AFortAthenaAircraft>> Aircrafts;
		auto& FlightInfo = GameState->MapInfo->FlightInfos[0];

		if (FConfiguration::bLateGame)
		{
			/*if (VersionInfo.FortniteVersion < 16)
			{
				GameState->GamePhase = 4;
				GameState->GamePhaseStep = 7;
				GameState->OnRep_GamePhase(3);
			}*/

			GameState->DefaultParachuteDeployTraceForGroundDistance = 2500.f;

			GenerateStormCircles(GameState->MapInfo);
			
			if (StormCircles.size() < FConfiguration::LateGameZone)
			{
				printf("LateGame is not supported on this version!\n");
				return;
			}

			FVector Loc = StormCircles[FConfiguration::LateGameZone + 2].Center;

			if (FConfiguration::bCustomSafeZone)
				Loc = FConfiguration::CustomSafeZoneCenter;

			Loc.Z = 25000.f;

			if (FConfiguration::bMovingBus)
			{
				bool IsSmallZone = FConfiguration::IsS27() ? GameMode->GetLateSafeZoneIndex() > 3 : GameMode->GetLateSafeZoneIndex() > 4;
				auto OffsetRotation = FlightInfo.FlightStartRotation + FRotator(0, 180, 0);
				auto OffsetDirection = OffsetRotation.Vector();

				FlightInfo.FlightStartLocation = Loc;
				FlightInfo.FlightSpeed /= IsSmallZone ? 10 : 5;
				FlightInfo.TimeTillFlightEnd = 10.f;
				FlightInfo.TimeTillDropStart = 0.f;
				FlightInfo.TimeTillDropEnd = 10.f /*-= ((Aircraft->FlightInfo.TimeTillDropEnd - Aircraft->FlightInfo.TimeTillDropStart) / 2)*/;
				//GameState->SafeZonesStartTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld()) + 8.f;
			}
			else
			{
				FlightInfo.FlightSpeed = 0.f;

				FlightInfo.FlightStartLocation = Loc;
				FlightInfo.TimeTillFlightEnd = 5.f;
				FlightInfo.TimeTillDropEnd = 5.f;
				FlightInfo.TimeTillDropStart = 0.f;
				//GameState->bAircraftIsLocked = false;
				//GameState->SafeZonesStartTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld()) + 8.f;
			}
		}

		if (!GameState->MapInfo->AircraftClass.Get())
			return;
		auto Aircraft = AFortAthenaAircraft::SpawnAircraft(UWorld::GetWorld(), GameState->MapInfo->AircraftClass, FlightInfo);

		if (!Aircraft)
			return;
		
		Aircraft->FlightElapsedTime = 0;
		Aircraft->DropStartTime = (float)Time + FlightInfo.TimeTillDropStart;
		Aircraft->DropEndTime = (float)Time + FlightInfo.TimeTillDropEnd;
		Aircraft->FlightStartTime = (float)Time;
		Aircraft->FlightEndTime = (float)Time + FlightInfo.TimeTillFlightEnd;
		Aircraft->ReplicatedFlightTimestamp = (float)Time;
		bAircraftIsLocked = false;

		AFortPlayerControllerAthena::
			BeginAircraftInventoryCleanupForMatch(GameState);

		for (auto& Player__Uncasted : ((AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode)->AlivePlayers)
		{
			auto Player = (AFortPlayerControllerAthena*)Player__Uncasted;

			if (!Player)
				continue;

			auto Pawn = (AFortPlayerPawnAthena*)Player->Pawn;

			// Component-driven seasons do not pass through the legacy
			// AFortGameMode::StartAircraftPhase hook. Clear warmup loot at the
			// same authoritative transition before destroying the island pawn.
			AFortPlayerControllerAthena::
				ClearWarmupShieldForAircraft(
					Player, "component-phase");
			AFortPlayerControllerAthena::
				ClearDroppableInventoryForAircraft(
					Player, "component-phase");

			if (Pawn)
			{
				if (Pawn->Role == 3)
				{
					if (Pawn->bIsInAnyStorm)
					{
						Pawn->bIsInAnyStorm = false;
						Pawn->OnRep_IsInAnyStorm();
					}
				}
				Pawn->bIsInsideSafeZone = true;
				Pawn->OnRep_IsInsideSafeZone();
				Pawn->OnEnteredAircraft.Process();
			}

			Player->ClientActivateSlot(0, 0, 0.f, true, true);
			if (Pawn)
				Pawn->K2_DestroyActor();
			auto Reset = (void (*)(AFortPlayerControllerAthena*)) FindReset();
			Reset(Player);
			Player->ClientGotoState(FName(L"Spectating"));
		}

		Aircrafts_GameMode.Add(Aircraft);
		Aircrafts_GameState.Add(Aircraft);
		//SetAircrafts(Aircrafts);
		//OnRep_Aircrafts();
	}

	SetGamePhase(EAthenaGamePhase::Aircraft);
	SetGamePhaseStep(EAthenaGamePhaseStep::BusLocked);
}

uint64_t Reset_ = 0;
uint64_t IsInsideSafeZone = 0;
void UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Tick()
{
	auto Time = UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
	static auto GamePhaseOffset = this->GetOffset("GamePhase");
	auto& _GamePhase = *(EAthenaGamePhase*)(__int64(this) + GamePhaseOffset);

	struct FPhaseTickState
	{
		const void* Owner = nullptr;
		EAthenaGamePhase LastPhase = EAthenaGamePhase::None;
		bool bHasLastPhase = false;
		bool finishedFlight = false;
		bool gettingReady = false;
		bool busUnlocked = false;
		bool startedForming = false;
		bool formedZone = false;
		bool bUpdatedPhase = false;
	};

	static FPhaseTickState TickState;
	const bool bReturnedToWarmup =
		TickState.Owner == this &&
		TickState.bHasLastPhase &&
		_GamePhase <= EAthenaGamePhase::Warmup &&
		TickState.LastPhase > EAthenaGamePhase::Warmup;

	if (TickState.Owner != this || bReturnedToWarmup)
	{
		TickState = {};
		TickState.Owner = this;
	}

	TickState.LastPhase = _GamePhase;
	TickState.bHasLastPhase = true;

	auto& finishedFlight = TickState.finishedFlight;
	auto& gettingReady = TickState.gettingReady;
	auto& busUnlocked = TickState.busUnlocked;
	auto& startedForming = TickState.startedForming;
	auto& formedZone = TickState.formedZone;
	auto& bUpdatedPhase = TickState.bUpdatedPhase;

	if (!bSkipAircraft)
	{
		if (_GamePhase <= EAthenaGamePhase::Warmup)
		{
			if (!bStartAircraft && !gettingReady)
			{
				if (((AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode)->AlivePlayers.Num() > 0 && WarmupEarlyCountdownDuration != -1 && WarmupEarlyCountdownDuration < Time)
				{
					gettingReady = true;

					SetGamePhaseStep(EAthenaGamePhaseStep::GetReady);
					return;
				}
			}

			if (bStartAircraft || gettingReady)
			{
				if (bStartAircraft || (((AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode)->AlivePlayers.Num() > 0 && WarmupCountdownEndTime != -1 && WarmupCountdownEndTime < Time))
				{
					StartAircraftPhase();
					return;
				}
			}
		}

		if (_GamePhase == EAthenaGamePhase::Aircraft)
		{
			if (!busUnlocked)
			{
				if (((AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode)->AlivePlayers.Num() > 0 && Aircrafts_GameState[0].Get() && Aircrafts_GameState[0]->DropStartTime < Time)
				{
					busUnlocked = true;

					bAircraftIsLocked = false;
					SetGamePhaseStep(EAthenaGamePhaseStep::BusFlying);
					return;
				}
			}

			if (!startedForming)
			{
				if (((AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode)->AlivePlayers.Num() > 0 && Aircrafts_GameState[0].Get() && Aircrafts_GameState[0]->DropEndTime != -1 && Aircrafts_GameState[0]->DropEndTime < Time)
				{
					startedForming = true;
					auto GameState = (AFortGameStateAthena*)UWorld::GetWorld()->GameState;
					auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;

					for (auto& Player__Uncasted : GameMode->AlivePlayers)
					{
						auto Player = (AFortPlayerControllerAthena*)Player__Uncasted;
						if (!Player->PlayerState->bIsABot && Player->IsInAircraft())
						{
							Player->GetAircraftComponent()->ServerAttemptAircraftJump(FRotator{});
						}
					}

					/*if (bLateGame)
					{
						GameState->GamePhase = EAthenaGamePhase::SafeZones;
						GameState->GamePhaseStep = EAthenaGamePhaseStep::StormHolding;
						GameState->OnRep_GamePhase(EAthenaGamePhase::Aircraft);
					}*/
					if (FConfiguration::bLateGame)
						SafeZonesStartTime = (float)Time;
					else
						SafeZonesStartTime = (float)Time + 60.f;

					SetGamePhase(bSkipWarmup ? EAthenaGamePhase::None : EAthenaGamePhase::SafeZones);
					SetGamePhaseStep(EAthenaGamePhaseStep::StormForming);
					return;
				}
			}
		}

		if (!finishedFlight)
		{
			if (((AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode)->AlivePlayers.Num() > 0 && Aircrafts_GameState[0].Get() && Aircrafts_GameState[0]->FlightEndTime != -1 && Aircrafts_GameState[0]->FlightEndTime < Time)
			{
				finishedFlight = true;
				auto Aircraft = Aircrafts_GameState[0].Get();

				Aircraft->K2_DestroyActor();
				Aircrafts_GameState.Clear();
				Aircrafts_GameMode.Clear();
				return;
			}
		}
	}
	else
	{
		if (!finishedFlight)
		{
			finishedFlight = true;

			if (!FConfiguration::IsKnownS27CustomMapPlaylist())
				SetGamePhase(EAthenaGamePhase::SafeZones);
			SetGamePhaseStep(EAthenaGamePhaseStep::StormForming);
		}
	}

	if (bEnableZones)
	{
		if (_GamePhase == EAthenaGamePhase::SafeZones)
		{
			if (!bPausedZone && finishedFlight && !formedZone)
			{
				if (((AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode)->AlivePlayers.Num() > 0 && SafeZonesStartTime != -1 && SafeZonesStartTime < Time)
				{
					formedZone = true;
					auto SafeZoneIndicator = SetupSafeZoneIndicator();
					StartNewSafeZonePhase(FConfiguration::bLateGame ? FConfiguration::LateGameZone + 3 : 1, true);
					return;
				}
			}

			if (formedZone && SafeZoneIndicator)
			{
				if (SafeZoneIndicator->SafeZonePhases.IsValidIndex(SafeZoneIndicator->CurrentPhase))
				{
					bool bStartedNewPhase = false;
					if (!bPausedZone && !bUpdatedPhase && SafeZoneIndicator->SafeZoneStartShrinkTime < Time)
					{
						bUpdatedPhase = true;

						auto& SafeZoneState = *(uint8_t*)(__int64(&SafeZoneIndicator->FutureReplicator) - 0x4);
						SafeZoneState = 3;

						SafeZoneIndicator->OnSafeZoneStateChange(3, false);
						SafeZoneIndicator->SafezoneStateChangedDelegate.Process(SafeZoneIndicator, 3);

						SetGamePhaseStep(EAthenaGamePhaseStep::StormShrinking);
					}
					else if (!bPausedZone && SafeZoneIndicator->SafeZoneFinishShrinkTime < Time)
					{
						bStartedNewPhase = true;

						if (SafeZoneIndicator->SafeZonePhases.IsValidIndex(SafeZoneIndicator->CurrentPhase + 1))
						{
							StartNewSafeZonePhase(SafeZoneIndicator->CurrentPhase + 1);
							bUpdatedPhase = false;
						}
					}
				}

				auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
				static auto ZoneEffect = FindObject<UClass>(L"/Game/Athena/SafeZone/GE_OutsideSafeZoneDamage.GE_OutsideSafeZoneDamage_C");

				for (auto& UncastedPlayer : GameMode->AlivePlayers)
				{
					auto Player = (AFortPlayerControllerAthena*)UncastedPlayer;
					if (!Player)
						continue;

					auto PlayerState = (AFortPlayerStateAthena*)Player->PlayerState;
					if (PlayerState && PlayerState->HasbIsABot() && PlayerState->bIsABot)
						continue;

					if (auto Pawn = Player->MyFortPawn)
					{
						bool bInZone = IsInCurrentSafeZone(Player->MyFortPawn->K2_GetActorLocation(), false);

						if (!IsInCurrentSafeZone__Ptr)
							bInZone = GameMode->IsInCurrentSafeZone(Player->MyFortPawn->K2_GetActorLocation(), false);

						if (Pawn->bIsInsideSafeZone != bInZone || Pawn->bIsInAnyStorm != !bInZone)
						{
							printf("Pawn %s new storm status: %s\n", Pawn->Name.ToString().c_str(), bInZone ? "true" : "false");
							Pawn->bIsInAnyStorm = !bInZone;
							Pawn->OnRep_IsInAnyStorm();
							Pawn->bIsInsideSafeZone = bInZone;
							Pawn->OnRep_IsInsideSafeZone();

							/*auto AbilitySystemComponent = Player->PlayerState->AbilitySystemComponent;
							if (AbilitySystemComponent)
							{
								for (int i = 0; i < AbilitySystemComponent->ActiveGameplayEffects.GameplayEffects_Internal.Num(); i++)
								{
									auto& Effect = AbilitySystemComponent->ActiveGameplayEffects.GameplayEffects_Internal.Get(i, FActiveGameplayEffect::Size());

									printf("%s %s\n", Effect.Spec.Def->Name.ToString().c_str(), Effect.Spec.Def->Class->Name.ToString().c_str());
									if (Effect.Spec.Def->Class == ZoneEffect)
									{
										auto Handle = *(FActiveGameplayEffectHandle*)(__int64(&Effect) + 0xc);

										AbilitySystemComponent->SetActiveGameplayEffectLevel(Handle, SafeZoneIndicator->CurrentPhase);

										// 1.f should be max(InStormDamageIncrementValue, 1.f)
										AbilitySystemComponent->UpdateActiveGameplayEffectSetByCallerMagnitude(Handle, FGameplayTag(FName(L"SetByCaller.StormCampingDamage")), 1.f);
										AbilitySystemComponent->UpdateActiveGameplayEffectSetByCallerMagnitude(Handle, FGameplayTag(FName(L"SetByCaller.StormShieldDamage")), 1.f);
										printf("found\n");
										break;
									}
								}
							}*/
						}

					}
				}
			}
		}
	}
}


void UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Hook()
{
	if (!GetDefaultObj())
		return;

	Reset_ = FindReset();
	SetGamePhase_ = FindSetGamePhase();

	Utils::Hook(FindHandleMatchHasStarted(), HandleMatchHasStarted, HandleMatchHasStartedOG);
}
