#include "pch.h"
#include "../Public/FortKismetLibrary.h"
#include "../Public/FortInventory.h"
#include "../Public/FortLootPackage.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortAthenaMutator.h"
#include <cmath>
#include <limits>
#include <unordered_set>
#include <vector>

extern void CaptureGhostModeCharacterPartsBeforeGrant(
	AFortPlayerControllerAthena* PlayerController);

bool bHasbPickupOnlyRelevantToOwner = false;
bool bHasbToss = false;
bool bHasbRandomRotation = false;
bool bHasbBlockedFromAutoPickup = false;
bool bHasPickupInstigatorHandle2 = false;
bool bHasSourceType = false;
bool bHasSource = false;
bool bHasOptionalOwnerPC = false;

namespace
{
	struct FTimeOfDayControllerResolverState
	{
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<UObject> DaySequenceActor;
		TWeakObjectPtr<UClass> DaySequenceActorClass;
		ULONGLONG NextActorScanTimeMs = 0;
		bool bDaySequenceActorClassLookupAttempted = false;
		bool bLoggedGameStateManager = false;
		bool bLoggedDirectActorFallback = false;
		bool bLoggedKismetManagerFallback = false;
	};

	FTimeOfDayControllerResolverState
		GTimeOfDayControllerResolverState{};

	void ResetTimeOfDayResolverStateForWorld(
		UWorld* World)
	{
		auto& State =
			GTimeOfDayControllerResolverState;
		if (State.World.Get() == World)
			return;

		State = {};
		if (World)
		{
			State.World =
				TWeakObjectPtr<UWorld>(World);
		}
	}

	bool IsLiveTimeOfDayObject(const UObject* Object)
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

		auto Item = TUObjectArray::GetItemByIndex(
			ObjectIndex);
		const int32 InvalidObjectFlags =
			Offsets::bEncryptedObjects
				? 0x10200000
				: 0x20;
		return Item &&
			Item->GetObject() == Object &&
			!(Item->GetFlags() & InvalidObjectFlags) &&
			Object->Class &&
			SDK::MemReadable(
				Object->Class, sizeof(UClass));
	}

	bool IsTimeOfDayObjectOwnedByWorld(
		const UObject* Object,
		const UWorld* World)
	{
		if (!Object || !World)
			return false;

		auto Outer = Object;
		for (int32 Depth = 0;
			Outer && Depth < 32;
			++Depth)
		{
			if (Outer == World)
				return true;
			Outer = Outer->Outer;
		}
		return false;
	}

	UObject* ResolveTimeOfDayManagerFromGameState(
		UWorld* World)
	{
		if (!World ||
			!IsLiveTimeOfDayObject(World->GameState))
		{
			return nullptr;
		}

		auto GameState = World->GameState;
		auto GetManager =
			GameState->GetFunction(
				"GetTimeOfDayManager");
		if (GetManager)
		{
			// Legacy builds return a raw AFortTimeOfDayManager pointer, while
			// newer builds may return a TScriptInterface (UObject first,
			// resolved interface pointer second). A zeroed two-pointer buffer
			// safely covers both reflected layouts.
			struct
			{
				UObject* ObjectPointer = nullptr;
				void* InterfacePointer = nullptr;
			} Params;
			GameState->ProcessEvent(GetManager, &Params);

			// This getter belongs to the current GameState, so its interface
			// result is already scoped to this world. Modern implementations
			// may be subsystem-owned and therefore have no Outer chain back to
			// UWorld; rejecting those is what hid the Chapter 4+ manager.
			if (IsLiveTimeOfDayObject(
					Params.ObjectPointer))
			{
				auto& State =
					GTimeOfDayControllerResolverState;
				if (!State.bLoggedGameStateManager)
				{
					SDK::DbgLog(
						"[TimeOfDay] resolved "
						"GameState manager %s (%s) "
						"on %.2f\n",
						Params.ObjectPointer->Name
							.ToString().c_str(),
						Params.ObjectPointer->Class->Name
							.ToString().c_str(),
						VersionInfo.FortniteVersion);
					State.bLoggedGameStateManager = true;
				}
				return Params.ObjectPointer;
			}
		}

		// Some intermediate builds expose the replicated interface property
		// but omit the Blueprint getter.
		const char* PropertyNames[] = {
			"FortTimeOfDayManager",
			"TimeOfDayManager"
		};
		for (const char* PropertyName : PropertyNames)
		{
			const int32 Offset =
				static_cast<int32>(
					GameState->GetOffset(PropertyName));
			if (Offset < 0 ||
				!SDK::MemReadable(
					reinterpret_cast<const uint8*>(
						GameState) + Offset,
					sizeof(UObject*)))
			{
				continue;
			}

			auto Manager = GetFromOffset<UObject*>(
				GameState, Offset);
			if (IsLiveTimeOfDayObject(Manager))
			{
				auto& State =
					GTimeOfDayControllerResolverState;
				if (!State.bLoggedGameStateManager)
				{
					SDK::DbgLog(
						"[TimeOfDay] resolved "
						"GameState.%s manager %s "
						"(%s) on %.2f\n",
						PropertyName,
						Manager->Name.ToString().c_str(),
						Manager->Class->Name
							.ToString().c_str(),
						VersionInfo.FortniteVersion);
					State.bLoggedGameStateManager = true;
				}
				return Manager;
			}
		}

		return nullptr;
	}

	bool IsCompatibleTimeOfDayManagerGetter(
		UFunction* Getter)
	{
		if (!Getter)
			return false;

		const auto ReflectedParams =
			Getter->GetParamsNamed();
		bool bHasWorldContext = false;
		bool bHasInterfaceReturn = false;
		for (const auto& Parameter :
			ReflectedParams.NameOffsetMap)
		{
			if (Parameter.Name ==
				"WorldContextObject")
			{
				if (Parameter.Offset != 0)
					return false;
				bHasWorldContext = true;
			}
			else if (Parameter.Name ==
				"ReturnValue")
			{
				// The return is a 16-byte TScriptInterface beginning after
				// the 8-byte context pointer.
				if (Parameter.Offset !=
					sizeof(UObject*))
				{
					return false;
				}
				bHasInterfaceReturn = true;
			}
			else
			{
				// Reflected function parameter enumerations contain only
				// parameters here. Reject an unexpected ABI instead of
				// dispatching into a fixed-size buffer.
				return false;
			}
		}
		return bHasWorldContext &&
			bHasInterfaceReturn;
	}

	UObject* ResolveTimeOfDayManagerFromKismet(
		UWorld* World)
	{
		if (!World)
			return nullptr;

		auto Library =
			UFortKismetLibrary::GetDefaultObj();
		if (!Library)
			return nullptr;

		const char* GetterNames[] = {
			// Epic's own modern SetTimeOfDay path resolves the contextual
			// manager first. The global manager may exist but control a
			// different streamed context.
			"GetContextualTimeOfDayManager",
			"GetGlobalTimeOfDayManager"
		};
		for (const char* GetterName : GetterNames)
		{
			auto Getter =
				Library->GetFunction(GetterName);
			if (!IsCompatibleTimeOfDayManagerGetter(
					Getter))
				continue;

			// Both modern Kismet getters use an 8-byte world context followed
			// by a 16-byte TScriptInterface return value.
			struct
			{
				UObject* WorldContextObject = nullptr;
				UObject* ObjectPointer = nullptr;
				void* InterfacePointer = nullptr;
			} Params;
			Params.WorldContextObject = World;
			Library->ProcessEvent(Getter, &Params);
			if (!IsLiveTimeOfDayObject(
					Params.ObjectPointer))
			{
				continue;
			}

			auto& State =
				GTimeOfDayControllerResolverState;
			if (!State.bLoggedKismetManagerFallback)
			{
				SDK::DbgLog(
					"[TimeOfDay] resolved %s manager %s "
					"on %.2f\n",
					GetterName,
					Params.ObjectPointer->Name
						.ToString().c_str(),
					VersionInfo.FortniteVersion);
				State.bLoggedKismetManagerFallback = true;
			}
			return Params.ObjectPointer;
		}
		return nullptr;
	}

	UObject* ResolveDaySequenceFallback(UWorld* World)
	{
		// Battle Royale moved to DaySequence in Chapter 4. Older builds do
		// not load this module, so avoid repeatedly scanning their GObjects.
		if (!World ||
			VersionInfo.FortniteVersion < 23.00)
		{
			return nullptr;
		}

		ResetTimeOfDayResolverStateForWorld(World);
		auto& State =
			GTimeOfDayControllerResolverState;

		auto IsUsableDaySequenceActor =
			[&](UObject* Actor)
			{
				if (!IsLiveTimeOfDayObject(Actor) ||
					Actor->IsDefaultObject() ||
					!IsTimeOfDayObjectOwnedByWorld(
						Actor, World) ||
					!Actor->GetFunction("SetTimeOfDay") ||
					!Actor->GetFunction("GetTimeOfDay") ||
					!Actor->GetFunction("Pause") ||
					!Actor->GetFunction("Play"))
				{
					return false;
				}

				return true;
			};

		auto CachedActor =
			State.DaySequenceActor.Get();
		if (IsUsableDaySequenceActor(CachedActor))
			return CachedActor;
		State.DaySequenceActor = {};

		const ULONGLONG CurrentTimeMs =
			GetTickCount64();
		if (CurrentTimeMs < State.NextActorScanTimeMs)
			return nullptr;
		State.NextActorScanTimeMs =
			CurrentTimeMs + 1000ULL;

		auto DaySequenceActorClass =
			State.DaySequenceActorClass.Get();
		if (!State.bDaySequenceActorClassLookupAttempted)
		{
			State.bDaySequenceActorClassLookupAttempted =
				true;
			DaySequenceActorClass =
				const_cast<UClass*>(
					FindClass("DaySequenceActor"));
			if (DaySequenceActorClass)
			{
				State.DaySequenceActorClass =
					TWeakObjectPtr<UClass>(
						DaySequenceActorClass);
			}
		}

		// A few shipping builds can expose the active DaySequence actor before
		// either Fortnite manager getter is ready. Keep a bounded direct scan
		// only as the final fallback.
		for (int32 Index = 0;
			Index < TUObjectArray::Num();
			++Index)
		{
			auto Item =
				TUObjectArray::GetItemByIndex(Index);
			auto Actor =
				Item
					? const_cast<UObject*>(
						Item->GetObject())
					: nullptr;
			if (!IsUsableDaySequenceActor(Actor))
				continue;
			if (DaySequenceActorClass &&
				!Actor->IsA(DaySequenceActorClass))
			{
				continue;
			}

			State.DaySequenceActor =
				TWeakObjectPtr<UObject>(Actor);
			if (!State.bLoggedDirectActorFallback)
			{
				SDK::DbgLog(
					"[TimeOfDay] resolved direct "
					"DaySequence actor %s on %.2f\n",
					Actor->Name.ToString().c_str(),
					VersionInfo.FortniteVersion);
				State.bLoggedDirectActorFallback = true;
			}
			return Actor;
		}
		return nullptr;
	}

	float NormalizeTimeOfDayHours(float TimeOfDay)
	{
		if (!std::isfinite(TimeOfDay))
			return 0.f;

		TimeOfDay = std::fmod(TimeOfDay, 24.f);
		if (TimeOfDay < 0.f)
			TimeOfDay += 24.f;
		return TimeOfDay;
	}

	bool InvokeSingleFloatSetter(
		UObject* Target,
		UFunction* Function,
		float Value)
	{
		if (!IsLiveTimeOfDayObject(Target) ||
			!Function)
		{
			return false;
		}

		// SetTimeOfDay/SetTimeOfDaySpeed take one float on both the legacy
		// numeric aliases and DaySequence. Validate that exact schema before
		// dispatching: legacy AFortTimeOfDayManager also has a misleadingly
		// named SetTimeOfDay(FString), which must never receive raw float
		// bytes. Honor the bool result exposed by modern interfaces because a
		// streamed manager can legitimately reject an early seek.
		alignas(16) uint8 Params[0x40]{};
		const auto ReflectedParams =
			Function->GetParamsNamed();
		constexpr uint64 CPF_Parm =
			0x0000000000000080ULL;
		constexpr uint64 CPF_OutParm =
			0x0000000000000100ULL;
		constexpr uint64 CPF_ReturnParm =
			0x0000000000000400ULL;
		const bool bReliablePropertyMetadata =
			VersionInfo.FortniteVersion < 32.00;
		uint32 ValueOffset = 0;
		int32 ReturnValueOffset = -1;
		int32 ValueParameterCount = 0;
		for (const auto& Parameter :
			ReflectedParams.NameOffsetMap)
		{
			const bool bReturnParameter =
				Parameter.Name == "ReturnValue" ||
				(bReliablePropertyMetadata &&
					(Parameter.PropertyFlags &
						CPF_ReturnParm));
			if (bReturnParameter)
			{
				if (Parameter.Offset <
						sizeof(Params) &&
					(!bReliablePropertyMetadata ||
						Parameter.ElementSize ==
							sizeof(bool)))
				{
					ReturnValueOffset =
						static_cast<int32>(
							Parameter.Offset);
				}
				else
				{
					return false;
				}
			}
			else
			{
				if (bReliablePropertyMetadata)
				{
					if (!(Parameter.PropertyFlags &
							CPF_Parm))
					{
						continue;
					}
					if ((Parameter.PropertyFlags &
							(CPF_OutParm |
								CPF_ReturnParm)) ||
						Parameter.ElementSize !=
							sizeof(float))
					{
						return false;
					}
				}
				if (Parameter.Offset + sizeof(float) >
						sizeof(Params) ||
					++ValueParameterCount != 1)
				{
					return false;
				}
				ValueOffset = Parameter.Offset;
			}
		}
		if (ValueParameterCount != 1)
			return false;

		memcpy(
			Params + ValueOffset,
			&Value,
			sizeof(Value));
		Target->ProcessEvent(Function, Params);
		return ReturnValueOffset < 0 ||
			Params[ReturnValueOffset] != 0;
	}

	bool InvokeSingleFloatGetter(
		UObject* Target,
		UFunction* Function,
		float& OutValue)
	{
		if (!IsLiveTimeOfDayObject(Target) ||
			!Function)
		{
			return false;
		}

		struct
		{
			float ReturnValue = 0.f;
		} Params;
		Target->ProcessEvent(Function, &Params);
		if (!std::isfinite(Params.ReturnValue))
			return false;

		OutValue = Params.ReturnValue;
		return true;
	}

	bool InvokeBoolGetter(
		UObject* Target,
		UFunction* Function,
		bool& OutValue)
	{
		if (!IsLiveTimeOfDayObject(Target) ||
			!Function)
		{
			return false;
		}

		struct
		{
			uint8 ReturnValue = 0;
			uint8 Padding[7]{};
		} Params;
		Target->ProcessEvent(Function, &Params);
		OutValue = Params.ReturnValue != 0;
		return true;
	}

	bool InvokeWorldFloatSetter(
		const UFortKismetLibrary* Library,
		UFunction* Function,
		UObject* WorldContextObject,
		float Value)
	{
		if (!Library || !Function ||
			!WorldContextObject)
		{
			return false;
		}

		struct
		{
			UObject* WorldContextObject = nullptr;
			float Value = 0.f;
			uint8 Padding[4]{};
		} Params;
		Params.WorldContextObject =
			WorldContextObject;
		Params.Value = Value;
		Library->ProcessEvent(Function, &Params);
		return true;
	}

	bool InvokeWorldFloatGetter(
		const UFortKismetLibrary* Library,
		UFunction* Function,
		UObject* WorldContextObject,
		float& OutValue)
	{
		if (!Library || !Function ||
			!WorldContextObject)
		{
			return false;
		}

		struct
		{
			UObject* WorldContextObject = nullptr;
			float ReturnValue = 0.f;
			uint8 Padding[4]{};
		} Params;
		Params.WorldContextObject =
			WorldContextObject;
		Library->ProcessEvent(Function, &Params);
		if (!std::isfinite(Params.ReturnValue))
			return false;

		OutValue = Params.ReturnValue;
		return true;
	}

	bool IsDaySequenceController(UObject* Controller)
	{
		return IsLiveTimeOfDayObject(Controller) &&
			Controller->GetFunction("Pause") &&
			Controller->GetFunction("Play");
	}

	void EnsureDaySequenceReplication(
		UObject* Controller)
	{
		if (!IsDaySequenceController(Controller))
			return;

		if (auto SetReplicatePlayback =
			Controller->GetFunction(
				"SetReplicatePlayback"))
		{
			struct
			{
				uint8 bReplicatePlayback = 1;
				uint8 Padding[7]{};
			} Params;
			Controller->ProcessEvent(
				SetReplicatePlayback, &Params);
		}
	}

	void ForceDaySequenceReplication(
		UObject* Controller)
	{
		auto Actor = Controller
			? Controller->Cast<AActor>()
			: nullptr;
		if (Actor)
			Actor->ForceNetUpdate();
	}
}

UObject* UFortKismetLibrary::
	GetTimeOfDayControllerCompat(
		UObject* WorldContextObject)
{
	auto World =
		reinterpret_cast<UWorld*>(
			WorldContextObject);
	if (!IsLiveTimeOfDayObject(World))
		return nullptr;
	ResetTimeOfDayResolverStateForWorld(World);

	if (auto Manager =
		ResolveTimeOfDayManagerFromGameState(World))
	{
		return Manager;
	}

	if (auto Manager =
		ResolveTimeOfDayManagerFromKismet(World))
	{
		return Manager;
	}

	return ResolveDaySequenceFallback(World);
}

bool UFortKismetLibrary::GetTimeOfDayCompat(
	UObject* WorldContextObject,
	float& OutTimeOfDay)
{
	auto Controller =
		GetTimeOfDayControllerCompat(
			WorldContextObject);
	if (Controller)
	{
		if (InvokeSingleFloatGetter(
				Controller,
				Controller->GetFunction(
					"GetTimeOfDay"),
				OutTimeOfDay))
		{
			OutTimeOfDay =
				NormalizeTimeOfDayHours(
					OutTimeOfDay);
			return true;
		}
	}

	auto World =
		reinterpret_cast<UWorld*>(
			WorldContextObject);
	auto DaySequenceActor =
		ResolveDaySequenceFallback(World);
	if (DaySequenceActor != Controller &&
		InvokeSingleFloatGetter(
			DaySequenceActor,
			DaySequenceActor
				? DaySequenceActor->GetFunction(
					"GetTimeOfDay")
				: nullptr,
			OutTimeOfDay))
	{
		OutTimeOfDay =
			NormalizeTimeOfDayHours(
				OutTimeOfDay);
		return true;
	}

	auto Library = GetDefaultObj();
	if (!Library ||
		!InvokeWorldFloatGetter(
			Library,
			Library->GetFunction("GetTimeOfDay"),
			WorldContextObject,
			OutTimeOfDay))
	{
		return false;
	}

	OutTimeOfDay =
		NormalizeTimeOfDayHours(OutTimeOfDay);
	return true;
}

bool UFortKismetLibrary::GetTimeOfDaySpeedCompat(
	UObject* WorldContextObject,
	float& OutTimeOfDaySpeed)
{
	auto Controller =
		GetTimeOfDayControllerCompat(
			WorldContextObject);
	if (Controller)
	{
		if (IsDaySequenceController(Controller))
		{
			bool bPaused = false;
			if (InvokeBoolGetter(
					Controller,
					Controller->GetFunction(
						"IsPaused"),
					bPaused))
			{
				if (!bPaused)
				{
					bool bPlaying = true;
					if (InvokeBoolGetter(
							Controller,
							Controller->GetFunction(
								"IsPlaying"),
							bPlaying) &&
						!bPlaying)
					{
						bPaused = true;
					}
				}
				OutTimeOfDaySpeed =
					bPaused ? 0.f : 1.f;
				return true;
			}
		}

		if (InvokeSingleFloatGetter(
				Controller,
				Controller->GetFunction(
					"GetTimeOfDaySpeed"),
				OutTimeOfDaySpeed))
		{
			return true;
		}
	}

	auto World =
		reinterpret_cast<UWorld*>(
			WorldContextObject);
	auto DaySequenceActor =
		ResolveDaySequenceFallback(World);
	if (DaySequenceActor != Controller &&
		IsDaySequenceController(DaySequenceActor))
	{
		bool bPaused = false;
		if (InvokeBoolGetter(
				DaySequenceActor,
				DaySequenceActor->GetFunction(
					"IsPaused"),
				bPaused))
		{
			OutTimeOfDaySpeed =
				bPaused ? 0.f : 1.f;
			return true;
		}
	}

	auto Library = GetDefaultObj();
	return Library &&
		InvokeWorldFloatGetter(
			Library,
			Library->GetFunction(
				"GetTimeOfDaySpeed"),
			WorldContextObject,
			OutTimeOfDaySpeed);
}

bool UFortKismetLibrary::SetTimeOfDayCompat(
	UObject* WorldContextObject,
	float TimeOfDay)
{
	const float NormalizedTime =
		NormalizeTimeOfDayHours(TimeOfDay);
	auto TryApplyTime =
		[&](UObject* Controller)
		{
			if (!Controller)
				return false;

			const bool bDaySequence =
				IsDaySequenceController(Controller);
			if (bDaySequence)
			{
				EnsureDaySequenceReplication(
					Controller);
			}

			// The manager interface already used the plain float setter in
			// 22.40. Probe it on every version: schema validation safely
			// rejects the legacy FString overload before ProcessEvent.
			const char* SetterNames[] = {
				"SetTimeOfDay",
				"SetTimeOfDayFloat",
				"SetTimeOfDayInHours"
			};
			for (const char* SetterName : SetterNames)
			{
				if (SetterName &&
					InvokeSingleFloatSetter(
						Controller,
						Controller->GetFunction(
							SetterName),
						NormalizedTime))
				{
					if (bDaySequence)
					ForceDaySequenceReplication(
						Controller);
					return true;
				}
			}
			return false;
		};

	auto Controller =
		GetTimeOfDayControllerCompat(
			WorldContextObject);
	if (TryApplyTime(Controller))
		return true;

	auto World =
		reinterpret_cast<UWorld*>(
			WorldContextObject);
	auto DaySequenceActor =
		ResolveDaySequenceFallback(World);
	if (DaySequenceActor != Controller &&
		TryApplyTime(DaySequenceActor))
	{
		return true;
	}

	auto Library = GetDefaultObj();
	if (!Library ||
		!InvokeWorldFloatSetter(
			Library,
			Library->GetFunction("SetTimeOfDay"),
			WorldContextObject,
			NormalizedTime))
	{
		return false;
	}

	// The modern static Kismet setter is void and reports success even if it
	// found no contextual manager. Require readback on those builds so the
	// live policy keeps retrying instead of accepting a silent no-op.
	if (VersionInfo.FortniteVersion >= 22.40)
	{
		float ObservedTime = 0.f;
		if (!GetTimeOfDayCompat(
				WorldContextObject,
				ObservedTime))
		{
			return false;
		}
		const float Difference =
			std::fabs(ObservedTime - NormalizedTime);
		return (std::min)(
				Difference,
				24.f - Difference) <= 0.05f;
	}
	return true;
}

bool UFortKismetLibrary::
	SetResolvedTimeOfDaySpeedCompat(
	UObject* Controller,
	float TimeOfDaySpeed)
{
	if (!IsLiveTimeOfDayObject(Controller))
		return false;

	if (IsDaySequenceController(Controller))
	{
		EnsureDaySequenceReplication(
			Controller);
		auto PlaybackFunction =
			Controller->GetFunction(
				std::fabs(TimeOfDaySpeed) <=
						std::numeric_limits<float>::
							epsilon()
					? "Pause"
					: "Play");
		if (!PlaybackFunction)
			return false;

		Controller->ProcessEvent(
			PlaybackFunction, nullptr);
		ForceDaySequenceReplication(
			Controller);
		return true;
	}

	const char* SpeedSetterNames[] = {
		"SetTimeOfDaySpeed",
		"SetTimeOfDaySpeedFloat"
	};
	for (const char* SetterName :
		SpeedSetterNames)
	{
		if (InvokeSingleFloatSetter(
				Controller,
				Controller->GetFunction(
					SetterName),
				TimeOfDaySpeed))
		{
			return true;
		}
	}
	return false;
}

bool UFortKismetLibrary::SetTimeOfDaySpeedCompat(
	UObject* WorldContextObject,
	float TimeOfDaySpeed)
{
	auto Controller =
		GetTimeOfDayControllerCompat(
			WorldContextObject);
	if (SetResolvedTimeOfDaySpeedCompat(
			Controller, TimeOfDaySpeed))
	{
		return true;
	}

	auto World =
		reinterpret_cast<UWorld*>(
			WorldContextObject);
	auto DaySequenceActor =
		ResolveDaySequenceFallback(World);
	if (DaySequenceActor != Controller &&
		SetResolvedTimeOfDaySpeedCompat(
			DaySequenceActor,
			TimeOfDaySpeed))
	{
		return true;
	}

	auto Library = GetDefaultObj();
	if (!Library ||
		!InvokeWorldFloatSetter(
			Library,
			Library->GetFunction(
				"SetTimeOfDaySpeed"),
			WorldContextObject,
			TimeOfDaySpeed))
	{
		return false;
	}
	if (VersionInfo.FortniteVersion >= 22.40)
	{
		float ObservedSpeed = 0.f;
		return GetTimeOfDaySpeedCompat(
				WorldContextObject,
				ObservedSpeed) &&
			std::fabs(
				ObservedSpeed -
				TimeOfDaySpeed) <= 0.01f;
	}
	return true;
}

void UFortKismetLibrary::K2_SpawnPickupInWorld(UObject* Object, FFrame& Stack, AFortPickupAthena** Ret)
{
	class UObject* WorldContextObject;
	class UFortItemDefinition* ItemDefinition;
	int32 NumberToSpawn;
	FVector Position;
	FVector Direction;
	int32 OverrideMaxStackCount;
	bool bToss = true;
	bool bRandomRotation = true;
	bool bBlockedFromAutoPickup = false;
	int32 PickupInstigatorHandle = 0;
	uint8 SourceType = 0;
	uint8 Source = 0;
	class AFortPlayerControllerAthena* OptionalOwnerPC = nullptr;
	bool bPickupOnlyRelevantToOwner = false;
	Stack.StepCompiledIn(&WorldContextObject);
	Stack.StepCompiledIn(&ItemDefinition);
	Stack.StepCompiledIn(&NumberToSpawn);
	Stack.StepCompiledIn(&Position);
	Stack.StepCompiledIn(&Direction);
	Stack.StepCompiledIn(&OverrideMaxStackCount);
	if (bHasbToss)
		Stack.StepCompiledIn(&bToss);
	if (bHasbRandomRotation)
		Stack.StepCompiledIn(&bRandomRotation);
	if (bHasbBlockedFromAutoPickup)
		Stack.StepCompiledIn(&bBlockedFromAutoPickup);
	if (bHasPickupInstigatorHandle2)
		Stack.StepCompiledIn(&PickupInstigatorHandle);
	if (bHasSourceType)
		Stack.StepCompiledIn(&SourceType);
	if (bHasSource)
		Stack.StepCompiledIn(&Source);
	if (bHasOptionalOwnerPC)
		Stack.StepCompiledIn(&OptionalOwnerPC);
	if (bHasbPickupOnlyRelevantToOwner)
		Stack.StepCompiledIn(&bPickupOnlyRelevantToOwner);
	Stack.IncrementCode();

	if (FFortAthenaNativeLTMCompatibility::
			ShouldSuppressAshtonLeaderWorldPickup(
				ItemDefinition))
	{
		*Ret = nullptr;
		SDK::DbgLog(
			"[Ashton1040] suppressed leader world pickup "
			"definition=%s\n",
			ItemDefinition
				? ItemDefinition->Name.ToString().c_str()
				: "null");
		return;
	}

	*Ret = AFortInventory::SpawnPickup(Position, ItemDefinition, NumberToSpawn, -1, SourceType, Source, OptionalOwnerPC ? OptionalOwnerPC->MyFortPawn : nullptr, bToss, bRandomRotation);
}


bool bHasItemVariantGuid2 = false;
bool bHasItemLevel = false;
bool bHasPickupInstigatorHandle = false;
bool bHasbUseItemPickupAnalyticEvent = false;
bool bHasWeaponAmmoOverride = false;

namespace
{
	constexpr uint64 CPF_Parm = 0x0000000000000080;
	constexpr uint64 CPF_ReturnParm = 0x0000000000000400;
	constexpr size_t MaxGhostFunctionBytes = 0x8000;
	constexpr size_t MaxGhostCallGraphFunctions = 64;

	enum class EGhostGiveItemAbi
	{
		None,
		Legacy,
		WithVariantGuid
	};

	struct FGhostBranch
	{
		uintptr_t Address = 0;
		uintptr_t Target = 0;
		uintptr_t OwnerStart = 0;
		uintptr_t OwnerEnd = 0;
		uint8 Length = 0;
		uint8 DisplacementOffset = 0;
	};

	EGhostGiveItemAbi GGhostGiveItemAbi =
		EGhostGiveItemAbi::None;
	bool GGiveItemToInventoryOwnerReturnsItem = false;
	bool GGhostGiveItemCallsitePatched = false;
	std::unordered_set<AFortPlayerControllerAthena*>
		GGhostCleanupInProgress;

	bool IsGhostModeCompatibilityBuild()
	{
		return VersionInfo.FortniteVersion >= 5.30 &&
			VersionInfo.FortniteVersion <= 8.00;
	}

	bool GetMainImageRange(
		uintptr_t& OutStart,
		uintptr_t& OutEnd)
	{
		OutStart = 0;
		OutEnd = 0;
		if (!ImageBase ||
			!SDK::MemReadable(
				reinterpret_cast<void*>(ImageBase),
				sizeof(IMAGE_DOS_HEADER)))
		{
			return false;
		}

		auto Dos = reinterpret_cast<IMAGE_DOS_HEADER*>(ImageBase);
		if (Dos->e_magic != IMAGE_DOS_SIGNATURE ||
			Dos->e_lfanew <= 0 ||
			Dos->e_lfanew > 0x1000)
		{
			return false;
		}

		auto NtAddress =
			ImageBase + static_cast<uintptr_t>(Dos->e_lfanew);
		if (!SDK::MemReadable(
				reinterpret_cast<void*>(NtAddress),
				sizeof(IMAGE_NT_HEADERS64)))
		{
			return false;
		}

		auto Nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(NtAddress);
		if (Nt->Signature != IMAGE_NT_SIGNATURE ||
			Nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
			Nt->OptionalHeader.SizeOfImage == 0)
		{
			return false;
		}

		OutStart = ImageBase;
		OutEnd =
			ImageBase + Nt->OptionalHeader.SizeOfImage;
		return OutEnd > OutStart;
	}

	bool IsExecutableImageRange(
		uintptr_t Address,
		size_t Size)
	{
		uintptr_t ImageStart = 0;
		uintptr_t ImageEnd = 0;
		if (!GetMainImageRange(ImageStart, ImageEnd) ||
			Address < ImageStart || Address >= ImageEnd ||
			Size > ImageEnd - Address ||
			!SDK::MemReadable(
				reinterpret_cast<void*>(Address), Size))
		{
			return false;
		}

		MEMORY_BASIC_INFORMATION MemoryInfo{};
		if (!VirtualQuery(
				reinterpret_cast<void*>(Address),
				&MemoryInfo,
				sizeof(MemoryInfo)) ||
			MemoryInfo.State != MEM_COMMIT ||
			(MemoryInfo.Protect & PAGE_GUARD))
		{
			return false;
		}

		const DWORD Protection =
			MemoryInfo.Protect & 0xFF;
		return Protection == PAGE_EXECUTE ||
			Protection == PAGE_EXECUTE_READ ||
			Protection == PAGE_EXECUTE_READWRITE ||
			Protection == PAGE_EXECUTE_WRITECOPY;
	}

	bool ResolveFunctionRange(
		uintptr_t Address,
		uintptr_t& OutStart,
		uintptr_t& OutEnd)
	{
		OutStart = 0;
		OutEnd = 0;
		if (!IsExecutableImageRange(Address, 1))
			return false;

		DWORD64 FunctionImageBase = 0;
		auto RuntimeFunction = RtlLookupFunctionEntry(
			static_cast<DWORD64>(Address),
			&FunctionImageBase,
			nullptr);
		if (RuntimeFunction && FunctionImageBase)
		{
			const uintptr_t Start =
				static_cast<uintptr_t>(FunctionImageBase) +
				RuntimeFunction->BeginAddress;
			const uintptr_t End =
				static_cast<uintptr_t>(FunctionImageBase) +
				RuntimeFunction->EndAddress;
			if (End > Start &&
				End - Start <= MaxGhostFunctionBytes &&
				Address >= Start && Address < End &&
				IsExecutableImageRange(
					Start, static_cast<size_t>(End - Start)))
			{
				OutStart = Start;
				OutEnd = End;
				return true;
			}
			return false;
		}

		// Small generated exec/tail thunks can be leaf functions and therefore
		// have no unwind entry. Bound those to their first return or padding byte;
		// never let this fallback become an open-ended image scan.
		for (uintptr_t Cursor = Address;
			Cursor < Address + 0x100;
			++Cursor)
		{
			if (!IsExecutableImageRange(Cursor, 1))
				return false;
			const uint8 Byte =
				*reinterpret_cast<const uint8*>(Cursor);
			if (Byte == 0xC3 || Byte == 0xC2 || Byte == 0xCC)
			{
				OutStart = Address;
				OutEnd = Cursor + 1;
				return true;
			}
		}
		return false;
	}

	bool DecodeGhostBranch(
		uintptr_t Address,
		uintptr_t FunctionEnd,
		FGhostBranch& OutBranch)
	{
		OutBranch = {};
		if (Address >= FunctionEnd ||
			!IsExecutableImageRange(Address, 1))
		{
			return false;
		}

		const auto Bytes =
			reinterpret_cast<const uint8*>(Address);
		uint8 Length = 0;
		uint8 DisplacementOffset = 0;
		if (Bytes[0] == 0xE8 || Bytes[0] == 0xE9)
		{
			Length = 5;
			DisplacementOffset = 1;
		}
		else if (Address + 2 <= FunctionEnd &&
			Bytes[0] == 0x0F &&
			(Bytes[1] & 0xF0) == 0x80)
		{
			Length = 6;
			DisplacementOffset = 2;
		}
		else
		{
			return false;
		}

		if (Address + Length > FunctionEnd ||
			!IsExecutableImageRange(Address, Length))
		{
			return false;
		}

		const int32 Relative =
			*reinterpret_cast<const int32*>(
				Address + DisplacementOffset);
		const uintptr_t Target =
			static_cast<uintptr_t>(
				static_cast<int64>(Address + Length) +
				static_cast<int64>(Relative));
		if (!IsExecutableImageRange(Target, 1))
			return false;

		OutBranch.Address = Address;
		OutBranch.Target = Target;
		OutBranch.Length = Length;
		OutBranch.DisplacementOffset =
			DisplacementOffset;
		return true;
	}

	bool IsTrivialNullReturnStub(uintptr_t Address)
	{
		uintptr_t StubStart = 0;
		uintptr_t StubEnd = 0;
		if (!ResolveFunctionRange(
				Address, StubStart, StubEnd) ||
			StubStart != Address || StubEnd <= StubStart ||
			StubEnd - StubStart > 8 ||
			!IsExecutableImageRange(Address, 1))
			return false;
		const auto Bytes =
			reinterpret_cast<const uint8*>(Address);

		if (Bytes[0] == 0xC3)
		{
			return true;
		}
		if (Bytes[0] == 0xC2)
			return IsExecutableImageRange(Address, 3);
		if (!IsExecutableImageRange(Address, 3))
			return false;
		if ((Bytes[0] == 0x33 || Bytes[0] == 0x31) &&
			Bytes[1] == 0xC0 &&
			(Bytes[2] == 0xC3 || Bytes[2] == 0xC2))
		{
			return Bytes[2] == 0xC3 ||
				IsExecutableImageRange(Address, 5);
		}
		if (!IsExecutableImageRange(Address, 4))
			return false;
		return Bytes[0] == 0x48 &&
			(Bytes[1] == 0x33 || Bytes[1] == 0x31) &&
			Bytes[2] == 0xC0 &&
			(Bytes[3] == 0xC3 ||
			 (Bytes[3] == 0xC2 &&
			  IsExecutableImageRange(Address, 6)));
	}

	void CollectGhostStubBranches(
		uintptr_t Function,
		int Depth,
		std::unordered_set<uintptr_t>& VisitedFunctions,
		std::vector<FGhostBranch>& OutBranches)
	{
		if (!Function || Depth < 0 ||
			VisitedFunctions.size() >=
				MaxGhostCallGraphFunctions)
		{
			return;
		}

		uintptr_t FunctionStart = 0;
		uintptr_t FunctionEnd = 0;
		if (!ResolveFunctionRange(
				Function, FunctionStart, FunctionEnd) ||
			!VisitedFunctions.insert(FunctionStart).second)
		{
			return;
		}

		std::vector<uintptr_t> ChildFunctions;
		for (uintptr_t Cursor = FunctionStart;
			Cursor < FunctionEnd;
			++Cursor)
		{
			FGhostBranch Branch{};
			if (!DecodeGhostBranch(
					Cursor, FunctionEnd, Branch))
			{
				continue;
			}

			if (IsTrivialNullReturnStub(Branch.Target))
			{
				Branch.OwnerStart = FunctionStart;
				Branch.OwnerEnd = FunctionEnd;
				const bool bAlreadyCollected =
					std::any_of(
						OutBranches.begin(),
						OutBranches.end(),
						[&](const FGhostBranch& Existing)
						{
							return Existing.Address ==
								Branch.Address;
						});
				if (!bAlreadyCollected)
					OutBranches.push_back(Branch);
				continue;
			}

			// Recurse only through direct calls/tail jumps. Conditional branches
			// stay within the current function unless their target is the stripped
			// helper itself, which was handled above.
			const uint8 Opcode =
				*reinterpret_cast<const uint8*>(Cursor);
			if (Depth > 0 &&
				(Opcode == 0xE8 || Opcode == 0xE9))
			{
				ChildFunctions.push_back(Branch.Target);
			}
		}

		for (const uintptr_t Child : ChildFunctions)
		{
			CollectGhostStubBranches(
				Child,
				Depth - 1,
				VisitedFunctions,
				OutBranches);
		}
	}

	bool ResolveGhostStartImplementation(
		UFunction* StartGhostMode,
		uintptr_t& OutImplementation)
	{
		OutImplementation = 0;
		if (!StartGhostMode || !StartGhostMode->ExecFunction)
			return false;

		const uintptr_t Thunk =
			reinterpret_cast<uintptr_t>(
				StartGhostMode->ExecFunction);
		uintptr_t ThunkStart = 0;
		uintptr_t ThunkEnd = 0;
		if (!ResolveFunctionRange(
				Thunk, ThunkStart, ThunkEnd))
		{
			return false;
		}

		// A native UFunction stores its generated exec thunk. The reflected
		// implementation is the thunk's final direct call/tail-jump target, after
		// parameter unmarshalling. Anchor there before looking for a stripped
		// helper so unrelated null-return branches in the thunk cannot qualify.
		FGhostBranch FinalHandoff{};
		for (uintptr_t Cursor = ThunkStart;
			Cursor < ThunkEnd;
			++Cursor)
		{
			FGhostBranch Branch{};
			if (!DecodeGhostBranch(
					Cursor, ThunkEnd, Branch))
			{
				continue;
			}

			const uint8 Opcode =
				*reinterpret_cast<const uint8*>(Cursor);
			if ((Opcode != 0xE8 && Opcode != 0xE9) ||
				(Branch.Target >= ThunkStart &&
				 Branch.Target < ThunkEnd) ||
				IsTrivialNullReturnStub(Branch.Target))
			{
				continue;
			}

			uintptr_t TargetStart = 0;
			uintptr_t TargetEnd = 0;
			if (!ResolveFunctionRange(
					Branch.Target,
					TargetStart,
					TargetEnd) ||
				TargetStart != Branch.Target)
			{
				continue;
			}
			FinalHandoff = Branch;
		}

		// Generated exec thunks hand off at their tail. Reject a coincidental
		// earlier call instead of widening the search when that invariant is not
		// present on a build.
		if (!FinalHandoff.Address ||
			ThunkEnd - FinalHandoff.Address > 0x80)
		{
			return false;
		}

		OutImplementation = FinalHandoff.Target;
		return true;
	}

	void* AllocateGhostRelayNear(uintptr_t BranchAddress)
	{
		SYSTEM_INFO SystemInfo{};
		GetSystemInfo(&SystemInfo);
		const uintptr_t Granularity =
			SystemInfo.dwAllocationGranularity
				? SystemInfo.dwAllocationGranularity
				: 0x10000;
		const uintptr_t Base =
			BranchAddress & ~(Granularity - 1);
		const uintptr_t Minimum =
			reinterpret_cast<uintptr_t>(
				SystemInfo.lpMinimumApplicationAddress);
		const uintptr_t Maximum =
			reinterpret_cast<uintptr_t>(
				SystemInfo.lpMaximumApplicationAddress);
		constexpr uintptr_t RelativeReach =
			0x7FFF0000ull;

		for (uintptr_t Distance = Granularity;
			Distance <= RelativeReach;
			Distance += Granularity)
		{
			const uintptr_t Candidates[] = {
				Base >= Distance ? Base - Distance : 0,
				Base <= Maximum - Distance
					? Base + Distance
					: 0
			};
			for (const uintptr_t Candidate : Candidates)
			{
				if (!Candidate || Candidate < Minimum ||
					Candidate > Maximum - 0x20)
				{
					continue;
				}
				const int64 Relative =
					static_cast<int64>(Candidate) -
					static_cast<int64>(BranchAddress + 6);
				if (Relative <
						(std::numeric_limits<int32>::min)() ||
					Relative >
						(std::numeric_limits<int32>::max)())
				{
					continue;
				}

				if (void* Relay = VirtualAlloc(
						reinterpret_cast<void*>(Candidate),
						0x20,
						MEM_COMMIT | MEM_RESERVE,
						PAGE_EXECUTE_READWRITE))
				{
					return Relay;
				}
			}
		}
		return nullptr;
	}

	bool PatchGhostBranch(
		const FGhostBranch& Branch,
		void* Detour)
	{
		FGhostBranch Current{};
		uintptr_t FunctionStart = 0;
		uintptr_t FunctionEnd = 0;
		if (!Branch.Address || !Detour ||
			!ResolveFunctionRange(
				Branch.Address,
				FunctionStart,
				FunctionEnd) ||
			!DecodeGhostBranch(
				Branch.Address,
				FunctionEnd,
				Current) ||
			Current.Target != Branch.Target ||
			Current.Length != Branch.Length ||
			Current.DisplacementOffset !=
				Branch.DisplacementOffset ||
			Branch.Address < Branch.OwnerStart ||
			Branch.Address >= Branch.OwnerEnd ||
			FunctionStart != Branch.OwnerStart ||
			FunctionEnd != Branch.OwnerEnd ||
			!IsTrivialNullReturnStub(Current.Target))
		{
			return false;
		}

		void* Relay =
			AllocateGhostRelayNear(Branch.Address);
		if (!Relay)
			return false;

		uint8 RelayCode[14] = {
			0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00,
			0x00, 0x00, 0x00, 0x00
		};
		memcpy(
			RelayCode + 6,
			&Detour,
			sizeof(Detour));
		memcpy(Relay, RelayCode, sizeof(RelayCode));
		FlushInstructionCache(
			GetCurrentProcess(), Relay, sizeof(RelayCode));

		const int64 Relative64 =
			static_cast<int64>(
				reinterpret_cast<uintptr_t>(Relay)) -
			static_cast<int64>(
				Branch.Address + Branch.Length);
		if (Relative64 <
				(std::numeric_limits<int32>::min)() ||
			Relative64 >
				(std::numeric_limits<int32>::max)())
		{
			VirtualFree(Relay, 0, MEM_RELEASE);
			return false;
		}
		const int32 Relative =
			static_cast<int32>(Relative64);
		void* DisplacementAddress =
			reinterpret_cast<void*>(
				Branch.Address +
				Branch.DisplacementOffset);
		DWORD OriginalProtection = 0;
		if (!VirtualProtect(
				DisplacementAddress,
				sizeof(Relative),
				PAGE_EXECUTE_READWRITE,
				&OriginalProtection))
		{
			VirtualFree(Relay, 0, MEM_RELEASE);
			return false;
		}
		memcpy(
			DisplacementAddress,
			&Relative,
			sizeof(Relative));
		FlushInstructionCache(
			GetCurrentProcess(),
			DisplacementAddress,
			sizeof(Relative));
		DWORD IgnoredProtection = 0;
		VirtualProtect(
			DisplacementAddress,
			sizeof(Relative),
			OriginalProtection,
			&IgnoredProtection);
		return true;
	}

	bool ValidateGhostModeReflection(
		const AFortPlayerControllerAthena* ControllerDefault,
		UFunction* StartGhostMode)
	{
		if (!ControllerDefault || !StartGhostMode ||
			!StartGhostMode->ExecFunction ||
			!IsExecutableImageRange(
				reinterpret_cast<uintptr_t>(
					StartGhostMode->ExecFunction),
				1))
		{
			return false;
		}

		const auto StartParams =
			StartGhostMode->GetParamsNamed();
		int InputCount = 0;
		bool bHasGhostDefinition = false;
		for (const auto& Parameter :
			StartParams.NameOffsetMap)
		{
			if (!(Parameter.PropertyFlags & CPF_Parm) ||
				(Parameter.PropertyFlags & CPF_ReturnParm))
			{
				continue;
			}
			++InputCount;
			if (Parameter.Name ==
					"ItemProvidingGhostMode" &&
				Parameter.ElementSize ==
					sizeof(UFortWorldItemDefinition*))
			{
				bHasGhostDefinition = true;
			}
		}
		if (InputCount != 1 || !bHasGhostDefinition)
			return false;

		auto GhostModeData = FindObject<UStruct>(
			L"/Script/FortniteGame.GhostModeRepData");
		if (!GhostModeData ||
			GhostModeData->GetPropertiesSize() < 0x18 ||
			GhostModeData->GetPropertiesSize() > 0x40)
		{
			return false;
		}
		const auto InGhostMode =
			GhostModeData->GetProperty("bInGhostMode");
		const auto GhostModeItemDef =
			GhostModeData->GetProperty("GhostModeItemDef");
		const auto PreviousFocusedSlot =
			GhostModeData->GetProperty("PreviousFocusedSlot");
		const auto TimeExitedGhostMode =
			GhostModeData->GetProperty("TimeExitedGhostMode");
		const auto ControllerGhostModeData =
			ControllerDefault->GetProperty("GhostModeRepData");
		if (!InGhostMode || !GhostModeItemDef ||
			!PreviousFocusedSlot || !TimeExitedGhostMode ||
			!ControllerGhostModeData)
		{
			return false;
		}

		const uint32 ControllerDataSize =
			GetFromOffset<uint32>(
				ControllerGhostModeData,
				Offsets::ElementSize);
		const uint32 ControllerDataOffset =
			SDK::DecryptPropOffset(GetFromOffset<uint32>(
				ControllerGhostModeData,
				Offsets::Offset_Internal));
		const int32 ControllerPropertiesSize =
			ControllerDefault->Class->GetPropertiesSize();
		if (ControllerPropertiesSize <= 0)
			return false;
		const uint32 ControllerPropertiesSizeUnsigned =
			static_cast<uint32>(ControllerPropertiesSize);
		return ControllerDataSize ==
				static_cast<uint32>(
					GhostModeData->GetPropertiesSize()) &&
			ControllerDataOffset <
				ControllerPropertiesSizeUnsigned &&
			ControllerDataSize <=
				ControllerPropertiesSizeUnsigned -
				ControllerDataOffset;
	}

	EGhostGiveItemAbi ResolveGhostGiveItemAbi(
		UFunction* GiveItemFunction)
	{
		GGiveItemToInventoryOwnerReturnsItem = false;
		if (!GiveItemFunction)
			return EGhostGiveItemAbi::None;

		const auto Parameters =
			GiveItemFunction->GetParamsNamed();
		const UFunction::ParamNamed* InventoryOwner = nullptr;
		const UFunction::ParamNamed* ItemDefinition = nullptr;
		const UFunction::ParamNamed* ItemVariantGuid = nullptr;
		const UFunction::ParamNamed* NumberToGive = nullptr;
		const UFunction::ParamNamed* NotifyPlayer = nullptr;
		const UFunction::ParamNamed* ReturnValue = nullptr;
		bool bUnknownInput = false;

		for (const auto& Parameter :
			Parameters.NameOffsetMap)
		{
			if (Parameter.PropertyFlags & CPF_ReturnParm)
			{
				if (Parameter.Name == "ReturnValue" &&
					Parameter.ElementSize ==
						sizeof(UFortWorldItem*))
				{
					ReturnValue = &Parameter;
				}
				else
				{
					return EGhostGiveItemAbi::None;
				}
				continue;
			}
			if (!(Parameter.PropertyFlags & CPF_Parm))
				continue;

			if (Parameter.Name == "InventoryOwner")
				InventoryOwner = &Parameter;
			else if (Parameter.Name == "ItemDefinition")
				ItemDefinition = &Parameter;
			else if (Parameter.Name == "ItemVariantGuid")
				ItemVariantGuid = &Parameter;
			else if (Parameter.Name == "NumberToGive")
				NumberToGive = &Parameter;
			else if (Parameter.Name == "bNotifyPlayer")
				NotifyPlayer = &Parameter;
			else if (Parameter.Name != "ItemLevel" &&
				Parameter.Name != "PickupInstigatorHandle" &&
				Parameter.Name != "bUseItemPickupAnalyticEvent" &&
				Parameter.Name != "WeaponAmmoOverride")
			{
				bUnknownInput = true;
			}
		}

		if (bUnknownInput || !InventoryOwner ||
			!ItemDefinition || !NumberToGive ||
			!NotifyPlayer ||
			InventoryOwner->ElementSize !=
				sizeof(TScriptInterface<
					IFortInventoryOwnerInterface>) ||
			ItemDefinition->ElementSize !=
				sizeof(UFortWorldItemDefinition*) ||
			NumberToGive->ElementSize != sizeof(int32) ||
			NotifyPlayer->ElementSize != sizeof(bool) ||
			!(InventoryOwner->Offset < ItemDefinition->Offset) ||
			!(ItemDefinition->Offset < NumberToGive->Offset) ||
			!(NumberToGive->Offset < NotifyPlayer->Offset))
		{
			return EGhostGiveItemAbi::None;
		}

		// GiveItemToInventoryOwner is reflected as void in 6.21 even though the
		// native helper used by StartGhostMode has the same x64 argument layout
		// and may return the granted item in RAX.  A return value is therefore
		// optional for ABI selection; the caller simply ignores it on void builds.
		GGiveItemToInventoryOwnerReturnsItem = ReturnValue != nullptr;
		if (!ItemVariantGuid)
			return EGhostGiveItemAbi::Legacy;

		if (ItemVariantGuid->ElementSize != sizeof(FGuid) ||
			!(ItemDefinition->Offset < ItemVariantGuid->Offset) ||
			!(ItemVariantGuid->Offset < NumberToGive->Offset))
		{
			GGiveItemToInventoryOwnerReturnsItem = false;
			return EGhostGiveItemAbi::None;
		}
		return EGhostGiveItemAbi::WithVariantGuid;
	}

	UFortWorldItem* FindGhostModeItemInstance(
		AFortInventory* Inventory,
		const UFortItemDefinition* Definition)
	{
		if (!Inventory || !Definition)
			return nullptr;
		auto Item = Inventory->Inventory.ItemInstances.Search(
			[&](UFortWorldItem* Candidate)
			{
				return Candidate &&
					Candidate->ItemEntry.ItemDefinition ==
						Definition;
			});
		return Item ? *Item : nullptr;
	}

	UFortWorldItem* GiveGhostModeItem(
		TScriptInterface<IFortInventoryOwnerInterface>*
			InventoryOwner,
		UFortWorldItemDefinition* ItemDefinition,
		int32 NumberToGive)
	{
		if (!InventoryOwner ||
			!SDK::MemReadable(
				InventoryOwner,
				sizeof(*InventoryOwner)) ||
			!UFortKismetLibrary::
				IsGhostModeItemDefinition(ItemDefinition))
		{
			return nullptr;
		}

		auto OwnerObject = const_cast<UObject*>(
			InventoryOwner->ObjectPointer);
		if (!OwnerObject ||
			!SDK::MemReadable(
				OwnerObject, sizeof(UObject)) ||
			!OwnerObject->Class ||
			!SDK::MemReadable(
				OwnerObject->Class, sizeof(UClass)))
		{
			return nullptr;
		}

		auto PlayerController =
			OwnerObject->Cast<AFortPlayerControllerAthena>();
		if (!PlayerController ||
			!PlayerController->WorldInventory ||
			NumberToGive != 1)
		{
			return nullptr;
		}

		CaptureGhostModeCharacterPartsBeforeGrant(
			PlayerController);

		auto Item = FindGhostModeItemInstance(
			PlayerController->WorldInventory,
			ItemDefinition);
		const bool bAlreadyHadBackingItem = Item != nullptr;
		if (!Item)
		{
			Item = PlayerController->WorldInventory->GiveItem(
				ItemDefinition,
				max(NumberToGive, 1),
				0,
				0,
				false);
		}
		if (!Item)
			return nullptr;
		if (bAlreadyHadBackingItem)
		{
			// StartGhostMode can reach the stripped server-assets grant branch
			// twice during one transition on 6.21. The first call already created,
			// focused, and executed this exact backing gadget. Executing it again
			// starts a second movement-cancellable action and can strand the ghost
			// visual after the item is removed.
			SDK::DbgLog(
				"[GhostMode] coalesced duplicate backing-item grant "
				"controller=%p item=%p definition=%s\n",
				static_cast<void*>(PlayerController),
				static_cast<void*>(Item),
				ItemDefinition->Name.ToString().c_str());
			return Item;
		}

		PlayerController->ClientEquipItem(
			Item->ItemEntry.ItemGuid, true);
		PlayerController->ServerExecuteInventoryItem(
			Item->ItemEntry.ItemGuid);
		AFortInventory::TrackGhostModeActivation(
			PlayerController,
			ItemDefinition,
			Item->ItemEntry.ItemGuid);
		SDK::DbgLog(
			"[GhostMode] granted and focused backing item "
			"controller=%p item=%p definition=%s count=%d\n",
			static_cast<void*>(PlayerController),
			static_cast<void*>(Item),
			ItemDefinition->Name.ToString().c_str(),
			max(NumberToGive, 1));
		return Item;
	}

	UFortWorldItem* GiveGhostModeItemLegacy(
		TScriptInterface<IFortInventoryOwnerInterface>*
			InventoryOwner,
		UFortWorldItemDefinition* ItemDefinition,
		int32 NumberToGive)
	{
		return GiveGhostModeItem(
			InventoryOwner,
			ItemDefinition,
			NumberToGive);
	}

	UFortWorldItem* GiveGhostModeItemWithVariant(
		TScriptInterface<IFortInventoryOwnerInterface>*
			InventoryOwner,
		UFortWorldItemDefinition* ItemDefinition,
		const FGuid* ItemVariantGuid,
		int32 NumberToGive)
	{
		(void)ItemVariantGuid;
		return GiveGhostModeItem(
			InventoryOwner,
			ItemDefinition,
			NumberToGive);
	}

	bool InstallGhostModeGiveItemPatch(
		UFunction* GiveItemFunction)
	{
		if (!IsGhostModeCompatibilityBuild())
			return false;

		auto ControllerDefault =
			AFortPlayerControllerAthena::GetDefaultObj();
		auto StartGhostMode =
			ControllerDefault
				? ControllerDefault->GetFunction(
					"StartGhostMode")
				: nullptr;
		if (!ValidateGhostModeReflection(
				ControllerDefault, StartGhostMode))
		{
			SDK::DbgLog(
				"[GhostMode] native grant patch unavailable: "
				"reflection contract mismatch (FN %.2f)\n",
				VersionInfo.FortniteVersion);
			return false;
		}

		GGhostGiveItemAbi =
			ResolveGhostGiveItemAbi(GiveItemFunction);
		void* Detour = nullptr;
		if (GGhostGiveItemAbi ==
				EGhostGiveItemAbi::Legacy)
		{
			Detour = reinterpret_cast<void*>(
				&GiveGhostModeItemLegacy);
		}
		else if (GGhostGiveItemAbi ==
				 EGhostGiveItemAbi::WithVariantGuid)
		{
			Detour = reinterpret_cast<void*>(
				&GiveGhostModeItemWithVariant);
		}
		else
		{
			SDK::DbgLog(
				"[GhostMode] native grant patch unavailable: "
				"GiveItemToInventoryOwner ABI mismatch "
				"(FN %.2f)\n",
				VersionInfo.FortniteVersion);
			return false;
		}

		std::unordered_set<uintptr_t> VisitedFunctions;
		std::vector<FGhostBranch> Candidates;
		uintptr_t StartGhostModeImplementation = 0;
		if (!ResolveGhostStartImplementation(
				StartGhostMode,
				StartGhostModeImplementation))
		{
			SDK::DbgLog(
				"[GhostMode] native grant patch unavailable: "
				"could not anchor StartGhostMode implementation "
				"(FN %.2f)\n",
				VersionInfo.FortniteVersion);
			return false;
		}
		CollectGhostStubBranches(
			StartGhostModeImplementation,
			2,
			VisitedFunctions,
			Candidates);
		if (Candidates.size() != 1)
		{
			SDK::DbgLog(
				"[GhostMode] native grant patch unavailable: "
				"expected one stripped branch, found %zu "
				"(FN %.2f)\n",
				Candidates.size(),
				VersionInfo.FortniteVersion);
			return false;
		}

		const bool bPatched =
			PatchGhostBranch(Candidates[0], Detour);
		SDK::DbgLog(
			"[GhostMode] native grant patch %s "
			"abi=%s callsite=0x%llX stub=0x%llX "
			"impl=0x%llX (FN %.2f)\n",
			bPatched ? "installed" : "failed",
			GGhostGiveItemAbi ==
					EGhostGiveItemAbi::WithVariantGuid
				? "variant-guid"
				: "legacy",
			static_cast<unsigned long long>(
				Candidates[0].Address - ImageBase),
			static_cast<unsigned long long>(
				Candidates[0].Target - ImageBase),
			static_cast<unsigned long long>(
				StartGhostModeImplementation - ImageBase),
			VersionInfo.FortniteVersion);
		return bPatched;
	}

	bool HasExactNoInputBoolReturn(UFunction* Function)
	{
		if (!Function)
			return false;
		int InputCount = 0;
		int BoolReturnCount = 0;
		for (const auto& Parameter :
			Function->GetParamsNamed().NameOffsetMap)
		{
			if (!(Parameter.PropertyFlags & CPF_Parm))
				continue;
			if (Parameter.PropertyFlags & CPF_ReturnParm)
			{
				if (Parameter.ElementSize == sizeof(bool))
					++BoolReturnCount;
			}
			else
			{
				++InputCount;
			}
		}
		return InputCount == 0 && BoolReturnCount == 1;
	}

	bool HasNoReflectedParameters(UFunction* Function)
	{
		if (!Function)
			return false;
		for (const auto& Parameter :
			Function->GetParamsNamed().NameOffsetMap)
		{
			if (Parameter.PropertyFlags & CPF_Parm)
				return false;
		}
		return true;
	}

	bool HasExactGhostDefinitionParameter(UFunction* Function)
	{
		if (!Function)
			return false;
		int InputCount = 0;
		bool bValidDefinition = false;
		for (const auto& Parameter :
			Function->GetParamsNamed().NameOffsetMap)
		{
			if (!(Parameter.PropertyFlags & CPF_Parm) ||
				(Parameter.PropertyFlags & CPF_ReturnParm))
			{
				continue;
			}
			++InputCount;
			bValidDefinition =
				Parameter.Name == "GhostModeItemDef" &&
				Parameter.ElementSize ==
					sizeof(UFortWorldItemDefinition*);
		}
		return InputCount == 1 && bValidDefinition;
	}

	bool ClearLegacyGhostModePlayerState(
		AFortPlayerControllerAthena* PlayerController)
	{
		if (!IsGhostModeCompatibilityBuild() ||
			!PlayerController || !PlayerController->PlayerState)
		{
			return false;
		}

		auto PlayerState = PlayerController->PlayerState
			->Cast<AFortPlayerStateAthena>();
		if (!PlayerState || !PlayerState->HasbInGhostMode() ||
			!PlayerState->bInGhostMode)
		{
			return false;
		}

		PlayerState->bInGhostMode = false;
		auto OnRepInGhostMode = PlayerState->GetFunction(
			"OnRep_InGhostMode");
		if (HasNoReflectedParameters(OnRepInGhostMode))
			PlayerState->Call<void>(OnRepInGhostMode);
		PlayerState->ForceNetUpdate();
		SDK::DbgLog(
			"[GhostMode] cleared legacy player-state flag "
			"controller=%p playerState=%p\n",
			static_cast<void*>(PlayerController),
			static_cast<void*>(PlayerState));
		return true;
	}
}

bool UFortKismetLibrary::IsGhostModeItemDefinition(
	const UFortItemDefinition* ItemDefinition)
{
	if (!IsGhostModeCompatibilityBuild() ||
		!ItemDefinition ||
		ItemDefinition->Name.ToWString() !=
			L"AGID_SpookyMist" ||
		!ItemDefinition->Cast<UFortGadgetItemDefinition>())
	{
		return false;
	}

	const bool bForceFocused =
		ItemDefinition->HasbForceFocusWhenAdded() &&
		ItemDefinition->bForceFocusWhenAdded;
	const auto WorldDefinition =
		ItemDefinition->Cast<UFortWorldItemDefinition>();
	const bool bStayInOverflow =
		WorldDefinition &&
		WorldDefinition->HasbForceStayInOverflow() &&
		WorldDefinition->bForceStayInOverflow;
	const bool bForceIntoOverflow =
		ItemDefinition->HasbForceIntoOverflow() &&
		ItemDefinition->bForceIntoOverflow;
	return bForceFocused || bStayInOverflow ||
		bForceIntoOverflow;
}

void UFortKismetLibrary::NotifyGhostModeItemRemoved(
	AFortPlayerControllerAthena* PlayerController,
	const UFortItemDefinition* ItemDefinition)
{
	if (!PlayerController ||
		!IsGhostModeItemDefinition(ItemDefinition) ||
		GGhostCleanupInProgress.contains(PlayerController))
	{
		return;
	}

	auto CheckRemoved = PlayerController->GetFunction(
		"CheckGhostModeItemRemoved");
	if (HasExactGhostDefinitionParameter(CheckRemoved))
	{
		auto WorldDefinition =
			const_cast<UFortWorldItemDefinition*>(
				ItemDefinition->Cast<
					UFortWorldItemDefinition>());
		PlayerController->Call<void>(
			CheckRemoved, WorldDefinition);
		SDK::DbgLog(
			"[GhostMode] notified native removal controller=%p "
			"definition=%s\n",
			static_cast<void*>(PlayerController),
			ItemDefinition->Name.ToString().c_str());
	}

	// CheckGhostModeItemRemoved is only a conditional bridge into
	// EndGhostMode.  On 6.21 its native inventory lookup can observe the
	// already-removed backing row and return without completing the controller
	// transition.  Restoring character parts while GhostModeRepData still says
	// that the player is a ghost is ineffective: the owning client keeps the
	// ghost visualization authoritative and immediately wins over the part
	// update.  Verify the postcondition and use the public terminal transition
	// as the narrow fallback.  This runs only after AGID_SpookyMist's terminal
	// stack has been removed, so its authored low gravity remains active for the
	// complete Ghost Mode lifetime.
	bool bForcedEnd = false;
	auto IsInGhostMode = PlayerController->GetFunction(
		"IsInGhostMode");
	if (HasExactNoInputBoolReturn(IsInGhostMode) &&
		PlayerController->Call<bool>(IsInGhostMode))
	{
		auto EndGhostMode = PlayerController->GetFunction(
			"EndGhostMode");
		if (HasNoReflectedParameters(EndGhostMode))
		{
			PlayerController->Call<void>(EndGhostMode);
			bForcedEnd = true;
		}
	}

	// Some early Athena builds replicate a second ghost flag on PlayerState.
	// The controller's native exit does not always clear it on a custom server,
	// leaving the owning client rendered as the ghost after the gadget is gone.
	ClearLegacyGhostModePlayerState(PlayerController);
	PlayerController->ForceNetUpdate();
	if (bForcedEnd)
	{
		SDK::DbgLog(
			"[GhostMode] completed terminal controller transition "
			"after backing-item removal controller=%p\n",
			static_cast<void*>(PlayerController));
	}
}

bool UFortKismetLibrary::CleanupGhostMode(
	AFortPlayerControllerAthena* PlayerController,
	bool bRemoveBackingItem)
{
	if (!IsGhostModeCompatibilityBuild() ||
		!PlayerController ||
		!GGhostCleanupInProgress.insert(
			PlayerController).second)
	{
		return false;
	}

	bool bChanged = false;
	bool bEndedGhostModeNatively = false;
	auto IsInGhostMode = PlayerController->GetFunction(
		"IsInGhostMode");
	const bool bWasInGhostMode =
		HasExactNoInputBoolReturn(IsInGhostMode) &&
		PlayerController->Call<bool>(IsInGhostMode);
	if (bWasInGhostMode)
	{
		auto EndGhostMode = PlayerController->GetFunction(
			"EndGhostMode");
		if (HasNoReflectedParameters(EndGhostMode))
		{
			PlayerController->Call<void>(EndGhostMode);
			bChanged = true;
			bEndedGhostModeNatively = true;
		}
	}

	std::vector<FGuid> GhostItemGuids;
	std::vector<const UFortItemDefinition*>
		GhostItemDefinitions;
	if (bRemoveBackingItem &&
		PlayerController->WorldInventory)
	{
		auto& Entries = PlayerController->WorldInventory
			->Inventory.ReplicatedEntries;
		GhostItemGuids.reserve(Entries.Num());
		GhostItemDefinitions.reserve(Entries.Num());
		for (int32 Index = 0; Index < Entries.Num(); ++Index)
		{
			auto& Entry = Entries.Get(
				Index, FFortItemEntry::Size());
			if (IsGhostModeItemDefinition(
					Entry.ItemDefinition))
			{
				GhostItemGuids.push_back(Entry.ItemGuid);
				GhostItemDefinitions.push_back(
					Entry.ItemDefinition);
			}
		}

		for (const auto& Guid : GhostItemGuids)
		{
			PlayerController->WorldInventory->Remove(Guid);
			bChanged = true;
		}
	}

	GGhostCleanupInProgress.erase(PlayerController);
	// EndGhostMode and CheckGhostModeItemRemoved are two entry points into the
	// same terminal transition. Use the removal callback only as the fallback;
	// issuing both can replay the legacy exit state and strand its cosmetics.
	if (!bEndedGhostModeNatively)
	{
		for (const auto Definition : GhostItemDefinitions)
		{
			NotifyGhostModeItemRemoved(
				PlayerController, Definition);
		}
	}
	ClearLegacyGhostModePlayerState(PlayerController);

	if (bChanged)
	{
		SDK::DbgLog(
			"[GhostMode] cleanup controller=%p active=%d "
			"removed=%zu\n",
			static_cast<void*>(PlayerController),
			bWasInGhostMode ? 1 : 0,
			GhostItemGuids.size());
	}
	return bChanged;
}

void UFortKismetLibrary::GiveItemToInventoryOwner(
	UObject* Object,
	FFrame& Stack,
	UFortWorldItem** Ret)
{
	if (Ret)
		*Ret = nullptr;
	TScriptInterface<class IFortInventoryOwnerInterface> InventoryOwner;
	UFortItemDefinition* ItemDefinition;
	FGuid ItemVariantGuid;
	int32 NumberToGive;
	bool bNotifyPlayer;
	int32 ItemLevel = -1;
	int32 PickupInstigatorHandle = 0;
	bool bUseItemPickupAnalyticEvent = false;
	int32 WeaponAmmoOverride = -1;
	Stack.StepCompiledIn(&InventoryOwner);
	Stack.StepCompiledIn(&ItemDefinition);
	if (bHasItemVariantGuid2)
		Stack.StepCompiledIn(&ItemVariantGuid);
	Stack.StepCompiledIn(&NumberToGive);
	Stack.StepCompiledIn(&bNotifyPlayer);
	if (bHasItemLevel)
		Stack.StepCompiledIn(&ItemLevel);
	if (bHasPickupInstigatorHandle)
		Stack.StepCompiledIn(&PickupInstigatorHandle);
	if (bHasbUseItemPickupAnalyticEvent)
		Stack.StepCompiledIn(&bUseItemPickupAnalyticEvent);
	if (bHasWeaponAmmoOverride)
		Stack.StepCompiledIn(&WeaponAmmoOverride);
	Stack.IncrementCode();

	if (!InventoryOwner.ObjectPointer || !ItemDefinition || NumberToGive <= 0)
		return;

	auto PlayerController = InventoryOwner.ObjectPointer->Cast<AFortPlayerControllerAthena>();
	if (!PlayerController || !PlayerController->WorldInventory)
		return;
	if (FFortAthenaNativeLTMCompatibility::
			ShouldRejectAshtonLeaderGrant(
				PlayerController,
				ItemDefinition))
	{
		SDK::DbgLog(
			"[Ashton1040] rejected non-stone leader transfer "
			"controller=%p definition=%s\n",
			(void*)PlayerController,
			ItemDefinition->Name.ToString().c_str());
		return;
	}

	if (IsGhostModeItemDefinition(ItemDefinition))
	{
		auto GhostItem = GiveGhostModeItem(
			&InventoryOwner,
			ItemDefinition->Cast<UFortWorldItemDefinition>(),
			NumberToGive);
		if (Ret && GGiveItemToInventoryOwnerReturnsItem)
			*Ret = GhostItem;
		return;
	}

	auto ItemEntry = AFortInventory::MakeItemEntry(ItemDefinition, NumberToGive, ItemLevel);
	if (!ItemEntry)
		return;

	if (WeaponAmmoOverride != -1)
		ItemEntry->LoadedAmmo = WeaponAmmoOverride;
	PlayerController->InternalPickup(ItemEntry);
	free(ItemEntry);
}


namespace
{
	struct FInventoryStackSnapshot
	{
		FGuid Guid{};
		int32 Count = 0;
	};

	UFunction* K2GetItemQuantityOnPlayerFn = nullptr;
	UFunction* K2RemoveItemFromPlayerFn = nullptr;
	UFunction* K2RemoveItemFromPlayerByGuidFn = nullptr;

	UFunction* ResolveExecFunction(
		FFrame& Stack,
		UFunction* CachedFunction)
	{
		if (CachedFunction)
			return CachedFunction;

		auto Function = Stack.GetCurrentNativeFunction();
		return Function ? Function : Stack.Node;
	}

	int32 ClampInventoryResult(int64 Value)
	{
		const int64 Maximum =
			static_cast<int64>(
				(std::numeric_limits<int32>::max)());
		return static_cast<int32>(min(Value, Maximum));
	}

	int32 GetItemQuantityOnPlayer(
		AFortPlayerControllerAthena* PlayerController,
		UFortItemDefinition* ItemDefinition)
	{
		if (!PlayerController || !PlayerController->WorldInventory ||
			!ItemDefinition)
		{
			return 0;
		}

		int64 Quantity = 0;
		auto& Entries =
			PlayerController->WorldInventory->Inventory.ReplicatedEntries;
		for (int32 Index = 0; Index < Entries.Num(); Index++)
		{
			auto& Entry =
				Entries.Get(Index, FFortItemEntry::Size());
			if (Entry.ItemDefinition == ItemDefinition &&
				Entry.Count > 0)
			{
				Quantity += Entry.Count;
			}
		}

		return ClampInventoryResult(Quantity);
	}

	int32 RemoveItemFromPlayer(
		AFortPlayerControllerAthena* PlayerController,
		UFortItemDefinition* ItemDefinition,
		const FGuid& ItemVariantGuid,
		int32 AmountToRemove,
		bool bForceRemoval)
	{
		if (!PlayerController || !PlayerController->WorldInventory ||
			!ItemDefinition || AmountToRemove == 0)
		{
			return 0;
		}

		if (!UFortKismetLibrary::IsGhostModeItemDefinition(
				ItemDefinition) &&
			AFortInventory::ShouldBypassItemConsumption(
				PlayerController,
				AmountToRemove,
				bForceRemoval))
		{
			return min(
				GetItemQuantityOnPlayer(
					PlayerController, ItemDefinition),
				AmountToRemove);
		}

		// Variant GUID and force-removal were added to some later signatures.
		// Lower builds do not expose variants, and the inventory layout has no
		// safe cross-version variant accessor. Consume both reflected params
		// while preserving the historical definition-based behavior.
		(void)ItemVariantGuid;
		(void)bForceRemoval;

		auto WorldInventory = PlayerController->WorldInventory;
		std::vector<FInventoryStackSnapshot> Stacks;
		auto& Entries = WorldInventory->Inventory.ReplicatedEntries;
		Stacks.reserve(Entries.Num());
		for (int32 Index = 0; Index < Entries.Num(); Index++)
		{
			auto& Entry =
				Entries.Get(Index, FFortItemEntry::Size());
			if (Entry.ItemDefinition == ItemDefinition)
			{
				Stacks.push_back(
					{ Entry.ItemGuid, max(Entry.Count, 0) });
			}
		}

		const bool bRemoveAll = AmountToRemove < 0;
		int32 Remaining = max(AmountToRemove, 0);
		int64 Removed = 0;
		for (auto& StackSnapshot : Stacks)
		{
			if (!bRemoveAll && Remaining <= 0)
				break;

			const int32 Requested =
				bRemoveAll
					? -1
					: min(StackSnapshot.Count, Remaining);
			if (!bRemoveAll && Requested <= 0)
				continue;

			const int32 RemovedFromStack =
				WorldInventory->RemoveItem(
					StackSnapshot.Guid,
					Requested);
			Removed += RemovedFromStack;
			if (!bRemoveAll)
				Remaining -= RemovedFromStack;
		}

		return ClampInventoryResult(Removed);
	}

	int32 RemoveItemFromPlayerByGuid(
		AFortPlayerControllerAthena* PlayerController,
		FGuid ItemGuid,
		int32 AmountToRemove,
		bool bForceRemoval)
	{
		if (!PlayerController || !PlayerController->WorldInventory ||
			AmountToRemove == 0)
		{
			return 0;
		}

		auto ItemEntry =
			PlayerController->WorldInventory
				->Inventory.ReplicatedEntries.Search(
					[&](FFortItemEntry& Entry)
					{
						return Entry.ItemGuid == ItemGuid;
					},
					FFortItemEntry::Size());

		if ((!ItemEntry ||
			 !UFortKismetLibrary::IsGhostModeItemDefinition(
				 ItemEntry->ItemDefinition)) &&
			AFortInventory::ShouldBypassItemConsumption(
				PlayerController,
				AmountToRemove,
				bForceRemoval))
		{
			return ItemEntry
				? min(max(ItemEntry->Count, 0), AmountToRemove)
				: 0;
		}

		return PlayerController->WorldInventory->RemoveItem(
			ItemGuid,
			AmountToRemove < 0 ? -1 : AmountToRemove);
	}
}

void UFortKismetLibrary::K2_GetItemQuantityOnPlayer(
	UObject* Context,
	FFrame& Stack,
	int32* Ret)
{
	(void)Context;
	if (Ret)
		*Ret = 0;

	auto Function =
		ResolveExecFunction(
			Stack,
			K2GetItemQuantityOnPlayerFn);
	if (!Function)
		return;

	AFortPlayerControllerAthena* PlayerController = nullptr;
	UFortItemDefinition* ItemDefinition = nullptr;
	for (auto& Param :
		Function->GetParamsNamed().NameOffsetMap)
	{
		if (Param.Name == "PlayerController")
			Stack.StepCompiledIn(&PlayerController);
		else if (Param.Name == "ItemDefinition")
			Stack.StepCompiledIn(&ItemDefinition);
		else if (Param.Name != "ReturnValue")
		{
			SDK::DbgLog(
				"[FortKismetLibrary] K2_GetItemQuantityOnPlayer: "
				"discarding unknown parameter %s\n",
				Param.Name.c_str());
			Stack.StepCompiledIn();
		}
	}
	Stack.IncrementCode();

	if (Ret)
		*Ret =
			GetItemQuantityOnPlayer(
				PlayerController,
				ItemDefinition);
}

void UFortKismetLibrary::K2_RemoveItemFromPlayer(
	UObject* Context,
	FFrame& Stack,
	int32* Ret)
{
	(void)Context;
	if (Ret)
		*Ret = 0;

	auto Function =
		ResolveExecFunction(
			Stack,
			K2RemoveItemFromPlayerFn);
	if (!Function)
		return;

	AFortPlayerControllerAthena* PlayerController = nullptr;
	UFortItemDefinition* ItemDefinition = nullptr;
	FGuid ItemVariantGuid{};
	int32 AmountToRemove = 0;
	bool bForceRemoval = false;
	for (auto& Param :
		Function->GetParamsNamed().NameOffsetMap)
	{
		if (Param.Name == "PlayerController")
			Stack.StepCompiledIn(&PlayerController);
		else if (Param.Name == "ItemDefinition")
			Stack.StepCompiledIn(&ItemDefinition);
		else if (Param.Name == "ItemVariantGuid")
			Stack.StepCompiledIn(&ItemVariantGuid);
		else if (Param.Name == "AmountToRemove")
			Stack.StepCompiledIn(&AmountToRemove);
		else if (Param.Name == "bForceRemoval")
			Stack.StepCompiledIn(&bForceRemoval);
		else if (Param.Name != "ReturnValue")
		{
			SDK::DbgLog(
				"[FortKismetLibrary] K2_RemoveItemFromPlayer: "
				"discarding unknown parameter %s\n",
				Param.Name.c_str());
			Stack.StepCompiledIn();
		}
	}
	Stack.IncrementCode();

	if (Ret)
		*Ret =
			RemoveItemFromPlayer(
				PlayerController,
				ItemDefinition,
				ItemVariantGuid,
				AmountToRemove,
				bForceRemoval);
}

void UFortKismetLibrary::K2_RemoveItemFromPlayerByGuid(
	UObject* Context,
	FFrame& Stack,
	int32* Ret)
{
	(void)Context;
	if (Ret)
		*Ret = 0;

	auto Function =
		ResolveExecFunction(
			Stack,
			K2RemoveItemFromPlayerByGuidFn);
	if (!Function)
		return;

	AFortPlayerControllerAthena* PlayerController = nullptr;
	FGuid ItemGuid{};
	int32 AmountToRemove = 0;
	bool bForceRemoval = false;
	for (auto& Param :
		Function->GetParamsNamed().NameOffsetMap)
	{
		if (Param.Name == "PlayerController")
			Stack.StepCompiledIn(&PlayerController);
		else if (Param.Name == "ItemGuid")
			Stack.StepCompiledIn(&ItemGuid);
		else if (Param.Name == "AmountToRemove")
			Stack.StepCompiledIn(&AmountToRemove);
		else if (Param.Name == "bForceRemoval")
			Stack.StepCompiledIn(&bForceRemoval);
		else if (Param.Name != "ReturnValue")
		{
			SDK::DbgLog(
				"[FortKismetLibrary] "
				"K2_RemoveItemFromPlayerByGuid: discarding "
				"unknown parameter %s\n",
				Param.Name.c_str());
			Stack.StepCompiledIn();
		}
	}
	Stack.IncrementCode();

	if (Ret)
		*Ret =
			RemoveItemFromPlayerByGuid(
				PlayerController,
				ItemGuid,
				AmountToRemove,
				bForceRemoval);
}

void UFortKismetLibrary::SpawnItemVariantPickupInWorld(UObject* Object, FFrame& Stack, AFortPickupAthena** Ret)
{
	UObject* WorldContextObject;
	FSpawnItemVariantParams Params;

	Stack.StepCompiledIn(&WorldContextObject);
	Stack.StepCompiledIn(&Params);
	Stack.IncrementCode();

	*Ret = AFortInventory::SpawnPickup(FSpawnItemVariantParams::HasPosition() ? Params.Position : Params.position, Params.WorldItemDefinition, Params.NumberToSpawn, -1, Params.SourceType, Params.Source, nullptr, Params.bToss, Params.bRandomRotation);
}

bool bHasOptionalLootTags = false;
bool bHasWorldContextObject2 = false;
void UFortKismetLibrary::PickLootDrops(UObject* Object, FFrame& Stack, bool* Ret)
{
	UObject* WorldContextObject;
	FName TierGroupName;
	int32 WorldLevel;
	int32 ForcedLootTier;
	FGameplayTagContainer OptionalLootTags{};

	if (bHasWorldContextObject2)
		Stack.StepCompiledIn(&WorldContextObject);
	auto& OutLootToDrop = Stack.StepCompiledInRef<TArray<FFortItemEntry>>();
	Stack.StepCompiledIn(&TierGroupName);
	Stack.StepCompiledIn(&WorldLevel);
	Stack.StepCompiledIn(&ForcedLootTier);
	if (bHasOptionalLootTags)
		Stack.StepCompiledIn(&OptionalLootTags);
	Stack.IncrementCode();

	TArray<FFortItemEntry*> LootDrops{};

	UFortLootPackage::ChooseLootForContainer(LootDrops, TierGroupName, ForcedLootTier, WorldLevel);

	for (auto& LootDrop : LootDrops)
	{
		OutLootToDrop.Add(*LootDrop, FFortItemEntry::Size());
		free(LootDrop);
	}

	*Ret = LootDrops.Num() > 0;
}


void UFortKismetLibrary::K2_SpawnPickupInWorldWithClassAndItemEntry(UObject* Context, FFrame& Stack, AFortPickupAthena** Ret)
{
	UObject* WorldContextObject;
	auto Entry = (FFortItemEntry*)malloc(FFortItemEntry::Size());
	memset(Entry, 0, FFortItemEntry::Size());
	TSubclassOf<AFortPickupAthena> PickupClass;
	FVector Position;
	FVector Direction;
	int32 OverrideMaxStackCount;
	bool bToss;
	bool bRandomRotation;
	bool bBlockedFromAutoPickup;
	uint8_t SourceType;
	uint8_t Source;
	class AFortPlayerControllerAthena* OptionalOwnerPC;
	bool bPickupOnlyRelevantToOwner;

	Stack.StepCompiledIn(&WorldContextObject);
	Stack.StepCompiledIn(Entry);
	Stack.StepCompiledIn(&PickupClass);
	Stack.StepCompiledIn(&Position);
	Stack.StepCompiledIn(&Direction);
	Stack.StepCompiledIn(&OverrideMaxStackCount);
	Stack.StepCompiledIn(&bToss);
	Stack.StepCompiledIn(&bRandomRotation);
	Stack.StepCompiledIn(&bBlockedFromAutoPickup);
	Stack.StepCompiledIn(&SourceType);
	Stack.StepCompiledIn(&Source);
	Stack.StepCompiledIn(&OptionalOwnerPC);
	Stack.StepCompiledIn(&bPickupOnlyRelevantToOwner);
	Stack.IncrementCode();

	// 4th arg is LoadedAmmo, not Level - keep the entry's own ammo
	*Ret = AFortInventory::SpawnPickup(Position, Entry->ItemDefinition, Entry->Count, Entry->LoadedAmmo, SourceType, Source, OptionalOwnerPC ? OptionalOwnerPC->MyFortPawn : nullptr, bToss, bRandomRotation);
	free(Entry);
}

class ABuildingWall : public AActor
{
public:
	UCLASS_COMMON_MEMBERS(ABuildingWall);

	DEFINE_BITFIELD_PROP(bDoorOpen);

	DEFINE_FUNC(OnRep_bDoorOpen, void);
};

auto SetIsDoorOpen = (void(*)(AActor*, uint8_t, AFortPlayerPawnAthena*))nullptr;
void UFortKismetLibrary::OpenActor_(UObject* Context, FFrame& Stack)
{
	AActor* OpenableInterfaceActor;
	AFortPlayerControllerAthena* OptionalControllerInstigator;
	bool bRequestFastOpen = false;

	Stack.StepCompiledIn(&OpenableInterfaceActor);
	Stack.StepCompiledIn(&OptionalControllerInstigator);
	Stack.StepCompiledIn(&bRequestFastOpen);
	Stack.IncrementCode();

	printf("OpenActor %s %s\n", OpenableInterfaceActor->Name.ToString().c_str(), OptionalControllerInstigator ? OptionalControllerInstigator->Name.ToString().c_str() : "<nullptr>");
	if (SetIsDoorOpen && OpenableInterfaceActor->IsA<ABuildingWall>())
		SetIsDoorOpen(OpenableInterfaceActor, bRequestFastOpen ? 1 : 0, OptionalControllerInstigator ? OptionalControllerInstigator->Pawn : nullptr);
	else
		return callOG(UFortKismetLibrary::GetDefaultObj(), Stack.GetCurrentNativeFunction(), OpenActor, OpenableInterfaceActor, OptionalControllerInstigator, bRequestFastOpen);
}

void UFortKismetLibrary::CloseActor_(UObject* Context, FFrame& Stack)
{
	AActor* OpenableInterfaceActor;
	AFortPlayerControllerAthena* OptionalControllerInstigator;

	Stack.StepCompiledIn(&OpenableInterfaceActor);
	Stack.StepCompiledIn(&OptionalControllerInstigator);
	Stack.IncrementCode();

	printf("CloseActor %s %s\n", OpenableInterfaceActor->Name.ToString().c_str(), OptionalControllerInstigator ? OptionalControllerInstigator->Name.ToString().c_str() : "<nullptr>");
	if (SetIsDoorOpen && OpenableInterfaceActor->IsA<ABuildingWall>())
		SetIsDoorOpen(OpenableInterfaceActor, 3, OptionalControllerInstigator ? OptionalControllerInstigator->Pawn : nullptr);
	else
		return callOG(UFortKismetLibrary::GetDefaultObj(), Stack.GetCurrentNativeFunction(), CloseActor, OpenableInterfaceActor, OptionalControllerInstigator);
}

void UFortKismetLibrary::Hook()
{
	auto K2_SpawnPickupInWorldFn = GetDefaultObj()->GetFunction("K2_SpawnPickupInWorld");
	if (K2_SpawnPickupInWorldFn)
		for (auto& Param : K2_SpawnPickupInWorldFn->GetParamsNamed().NameOffsetMap)
		{
			if (Param.Name == "bPickupOnlyRelevantToOwner")
				bHasbPickupOnlyRelevantToOwner = true;
			else if (Param.Name == "bToss")
				bHasbToss = true;
			else if (Param.Name == "bRandomRotation")
				bHasbRandomRotation = true;
			else if (Param.Name == "bBlockedFromAutoPickup")
				bHasbBlockedFromAutoPickup = true;
			else if (Param.Name == "PickupInstigatorHandle")
				bHasPickupInstigatorHandle2 = true;
			else if (Param.Name == "SourceType")
				bHasSourceType = true;
			else if (Param.Name == "Source")
				bHasSource = true;
			else if (Param.Name == "OptionalOwnerPC")
				bHasOptionalOwnerPC = true;
		}
	Utils::ExecHook(K2_SpawnPickupInWorldFn, K2_SpawnPickupInWorld);

	Utils::ExecHook(GetDefaultObj()->GetFunction("K2_SpawnPickupInWorldWithClassAndItemEntry"), K2_SpawnPickupInWorldWithClassAndItemEntry);

	Utils::ExecHook(GetDefaultObj()->GetFunction("SpawnItemVariantPickupInWorld"), SpawnItemVariantPickupInWorld);

	auto PickLootDropsFn = GetDefaultObj()->GetFunction("PickLootDrops");

	if (PickLootDropsFn)
		for (auto& Param : PickLootDropsFn->GetParamsNamed().NameOffsetMap)
		{
			if (Param.Name == "OptionalLootTags")
				bHasOptionalLootTags = true;
			else if (Param.Name == "WorldContextObject")
				bHasWorldContextObject2 = true;
		}
	Utils::ExecHook(PickLootDropsFn, PickLootDrops);
}

void UFortKismetLibrary::PostLoadHook()
{
	auto GiveItemToInventoryOwnerFn = GetDefaultObj()->GetFunction("GiveItemToInventoryOwner");
	if (GiveItemToInventoryOwnerFn)
		for (auto& Param : GiveItemToInventoryOwnerFn->GetParamsNamed().NameOffsetMap)
		{
			if (Param.Name == "ItemVariantGuid")
				bHasItemVariantGuid2 = true;
			else if (Param.Name == "ItemLevel")
				bHasItemLevel = true;
			else if (Param.Name == "PickupInstigatorHandle")
				bHasPickupInstigatorHandle = true;
			else if (Param.Name == "bUseItemPickupAnalyticEvent")
				bHasbUseItemPickupAnalyticEvent = true;
			else if (Param.Name == "WeaponAmmoOverride")
				bHasWeaponAmmoOverride = true;
		}
	if (!GGhostGiveItemCallsitePatched)
	{
		GGhostGiveItemCallsitePatched =
			InstallGhostModeGiveItemPatch(
				GiveItemToInventoryOwnerFn);
	}
	Utils::ExecHook(GiveItemToInventoryOwnerFn, GiveItemToInventoryOwner);

	K2GetItemQuantityOnPlayerFn =
		GetDefaultObj()->GetFunction(
			"K2_GetItemQuantityOnPlayer");
	K2RemoveItemFromPlayerFn =
		GetDefaultObj()->GetFunction(
			"K2_RemoveItemFromPlayer");
	K2RemoveItemFromPlayerByGuidFn =
		GetDefaultObj()->GetFunction(
			"K2_RemoveItemFromPlayerByGuid");

	Utils::ExecHook(
		K2GetItemQuantityOnPlayerFn,
		K2_GetItemQuantityOnPlayer);
	Utils::ExecHook(
		K2RemoveItemFromPlayerFn,
		K2_RemoveItemFromPlayer);
	Utils::ExecHook(
		K2RemoveItemFromPlayerByGuidFn,
		K2_RemoveItemFromPlayerByGuid);

	SetIsDoorOpen = decltype(SetIsDoorOpen)(FindSetIsDoorOpen());
	Utils::ExecHook(GetDefaultObj()->GetFunction("OpenActor"), OpenActor_, OpenActor_OG);
	Utils::ExecHook(GetDefaultObj()->GetFunction("CloseActor"), CloseActor_, CloseActor_OG);
}
