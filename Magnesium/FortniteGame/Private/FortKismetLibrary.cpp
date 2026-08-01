#include "pch.h"
#include "../Public/FortKismetLibrary.h"
#include "../Public/FortInventory.h"
#include "../Public/FortLootPackage.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortAthenaMutator.h"
#include <cmath>
#include <limits>
#include <vector>

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
void UFortKismetLibrary::GiveItemToInventoryOwner(UObject* Object, FFrame& Stack)
{
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

		if (AFortInventory::ShouldBypassItemConsumption(
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

		if (AFortInventory::ShouldBypassItemConsumption(
				PlayerController,
				AmountToRemove,
				bForceRemoval))
		{
			auto ItemEntry =
				PlayerController->WorldInventory
					->Inventory.ReplicatedEntries.Search(
						[&](FFortItemEntry& Entry)
						{
							return Entry.ItemGuid == ItemGuid;
						},
						FFortItemEntry::Size());
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
