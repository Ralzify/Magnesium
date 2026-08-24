#include "pch.h"
#include "../Public/Events.h"
#include "../Public/Configuration.h"
#include "../Support/Public/VersionFeatureAdapter.h"
#include "../../Engine/Public/NetDriver.h"
#include "../../FortniteGame/Public/FortGameMode.h"
#include "../../FortniteGame/Public/BattleRoyaleGamePhaseLogic.h"
#include <limits>

struct FPhaseDataLayerEntry
{
public:
	USCRIPTSTRUCT_COMMON_MEMBERS(FPhaseDataLayerEntry);

	DEFINE_STRUCT_PROP(DataLayerAsset, UObject*);
	DEFINE_STRUCT_PROP(bIsRecursive, bool);
};

struct FPhaseInfo
{
public:
	USCRIPTSTRUCT_COMMON_MEMBERS(FPhaseInfo);

	DEFINE_STRUCT_PROP(DataLayers, TArray<FPhaseDataLayerEntry>);
};

class ASpecialEventScript : public AActor
{
public:
	UCLASS_COMMON_MEMBERS(ASpecialEventScript);

	DEFINE_PROP(DelayAfterConentLoad, float);
	DEFINE_PROP(ReplicatedActivePhaseIndex, int);
	DEFINE_PROP(DelayAfterWarmup, float);
	DEFINE_PROP(PhaseInfoArray, TArray<FPhaseInfo>);

	DEFINE_FUNC(OnRep_ReplicatedActivePhaseIndex, void);
};

class ASpecialEventScriptMeshActor : public AActor
{
public:
	UCLASS_COMMON_MEMBERS(ASpecialEventScriptMeshActor);

	DEFINE_FUNC(MeshRootStartEvent, void);
	DEFINE_FUNC(OnRep_RootStartTime, void);
};

namespace
{
	constexpr uint64 CPF_Parm = 0x0000000000000080;
	constexpr uint64 CPF_OutParm = 0x0000000000000100;
	constexpr uint64 CPF_ReturnParm = 0x0000000000000400;
	constexpr uint64 CPF_ReferenceParm = 0x0000000008000000;
	constexpr uint64 CastClassClassProperty = 0x0000000000000400;
	constexpr uint64 CastClassIntProperty = 0x0000000000000080;
	constexpr uint64 CastClassFloatProperty = 0x0000000000000100;
	constexpr uint64 CastClassNameProperty = 0x0000000000002000;
	constexpr uint64 CastClassObjectProperty = 0x0000000000010000;
	constexpr uint64 CastClassBoolProperty = 0x0000000000020000;
	constexpr uint64 CastClassStructProperty = 0x0000000000100000;
	constexpr uint64 CastClassDoubleProperty = 0x0000000100000000;
	constexpr uint32 InvalidParamOffset =
		(std::numeric_limits<uint32>::max)();
	constexpr int32 UnobservedPhase =
		(std::numeric_limits<int32>::min)();

	enum class EEventStartStage : uint8
	{
		Idle,
		LoadContent,
		WaitForLoader,
		WaitForTargets,
		Started,
	};

	struct FEventObjectIdentity
	{
		int32 ObjectIndex = -1;
		int32 SerialNumber = 0;

		bool operator==(const FEventObjectIdentity& Other) const
		{
			return ObjectIndex == Other.ObjectIndex &&
				SerialNumber == Other.SerialNumber;
		}
	};

	struct FTrackedGameplayEffect
	{
		FEventObjectIdentity AbilitySystemIdentity{};
		FActiveGameplayEffectHandle Handle{};
	};

	struct FKiwiVortexState
	{
		FEventObjectIdentity PawnIdentity{};
		FVector LastGroundedLocation{};
		float IslandSurfaceZ = 0.f;
		bool bHasGroundedLocation = false;
		bool bActive = false;
	};

	struct FEventRuntimeState
	{
		UWorld* World = nullptr;
		AFortGameMode* GameMode = nullptr;
		AFortGameStateAthena* GameState = nullptr;
		ASpecialEventScript* Script = nullptr;
		const FEvent* Event = nullptr;
		EEventStartStage Stage = EEventStartStage::Idle;
		double LoaderReadyTime = 0.0;
		double NextAttemptTime = 0.0;
		double NextStatusLogTime = 0.0;
		double NextPhasePollTime = 0.0;
		double NextRiftTourRepairTime = 0.0;
		double NextRiftTourRepairLogTime = 0.0;
		double RiftTourPhaseEnteredTime = 0.0;
		double NextKiwiPlayerSetupTime = 0.0;
		double NextKiwiVortexUpdateTime = 0.0;
		double NextKiwiSetupLogTime = 0.0;
		double NextMeshPollTime = 0.0;
		double NextActivatorPollTime = 0.0;
		double UnvaultDueTime = 0.0;
		float KiwiIslandSurfaceZ = 0.f;
		int32 ObservedPhaseIndex = UnobservedPhase;
		int32 AuthoritativePhaseIndex = UnobservedPhase;
		bool bAutoRequestIssued = false;
		bool bMeshInitializationPending = false;
		bool bActivatorGrantPending = false;
		bool bUnvaultPending = false;
		bool bRiftTourSlideRepairComplete = false;
		bool bRiftTourStarsRepairComplete = false;
		bool bRiftTourStarsServerActivationInvoked = false;
		bool bRiftTourStarsClientActivationInvoked = false;
		bool bRiftTourStarsFloatingStarted = false;
		bool bRiftTourBubbleRepairComplete = false;
		bool bRiftTourBubbleRelevancyApplied = false;
		bool bRiftTourEscherTeleportInvoked = false;
		bool bRiftTourEscherRepairComplete = false;
		std::vector<FEventObjectIdentity> InitializedMeshActors;
		std::vector<FEventObjectIdentity> GrantedActivatorPlayers;
		std::vector<FEventObjectIdentity> EquippedActivatorPlayers;
		std::vector<FEventObjectIdentity> RiftTourSlidePlayers;
		std::vector<FEventObjectIdentity> RiftTourStarsPlayers;
		std::vector<FEventObjectIdentity> RiftTourStarsFloatingPlayers;
		std::vector<FEventObjectIdentity> RiftTourBubblePlayers;
		std::vector<FTrackedGameplayEffect> RiftTourStarsEffects;
		std::vector<FTrackedGameplayEffect> KiwiLowGravityEffects;
		std::vector<FKiwiVortexState> KiwiVortexPlayers;
		std::vector<FEventObjectIdentity> KiwiBackpackPlayers;
		std::vector<FEventObjectIdentity> KiwiPrisonPlayers;
		FEventObjectIdentity RiftTourStarsFallbackGoal{};
	};

	struct FEventContext
	{
		UWorld* World = nullptr;
		AFortGameMode* GameMode = nullptr;
		AFortGameStateAthena* GameState = nullptr;
		const UFortPlaylistAthena* Playlist = nullptr;
	};

	struct FResolvedEventActors
	{
		AActor* Loader = nullptr;
		AActor* Script = nullptr;
	};

	struct FOnReadyLayout
	{
		uint32 Size = 0;
		uint32 GameStateOffset = InvalidParamOffset;
		uint32 PlaylistOffset = InvalidParamOffset;
		uint32 ContextTagsOffset = InvalidParamOffset;
	};

	struct FStartAtIndexLayout
	{
		uint32 Size = 0;
		uint32 IndexOffset = InvalidParamOffset;
		uint32 TimeOffset = InvalidParamOffset;
		uint32 TimeSize = 0;
	};

	std::atomic_bool GStartRequested{ false };
	std::atomic<UWorld*> GActiveEventWorld{ nullptr };
	std::atomic<UWorld*> GStartRequestWorld{ nullptr };
	FEventRuntimeState GEventState{};
	UWorld* GPreparedLoaderWorld = nullptr;
	AFortGameMode* GPreparedLoaderGameMode = nullptr;
	AFortGameStateAthena* GPreparedLoaderGameState = nullptr;
	double GPreparedLoaderVersion = 0.0;
	double GPreparedLoaderReadyTime = 0.0;
	double GPreparedLoaderFallbackTime = 0.0;

	bool IsLiveEventObject(const UObject* Object)
	{
		if (!Object || !SDK::MemReadable(Object, sizeof(UObject)))
			return false;

		const int32 ObjectIndex = Object->Index;
		if (ObjectIndex < 0 || ObjectIndex >= TUObjectArray::Num())
			return false;

		auto Item = TUObjectArray::GetItemByIndex(ObjectIndex);
		constexpr int32 InvalidObjectFlags = 0x20;
		return Item && Item->GetObject() == Object &&
			!(Item->GetFlags() & InvalidObjectFlags) &&
			Object->Class &&
			SDK::MemReadable(Object->Class, sizeof(UClass));
	}

	int32 SendDurianFinalPhaseCue()
	{
		auto World = UWorld::GetWorld();
		if (!IsLiveEventObject(World) ||
			!World->HasNetDriver() ||
			!IsLiveEventObject(World->NetDriver))
		{
			return 0;
		}

		auto Driver = static_cast<UNetDriver*>(World->NetDriver);
		if (!Driver->HasClientConnections())
			return 0;

		auto& Connections = Driver->ClientConnections;
		if (Connections.Num() < 0 || Connections.Num() > 256 ||
			Connections.Max() < Connections.Num() ||
			Connections.Max() > 4096 ||
			(Connections.Num() > 0 &&
				!SDK::MemReadable(
					Connections.GetData(),
					sizeof(UNetConnection*) * Connections.Num())))
		{
			return 0;
		}

		auto ControllerClass =
			AFortPlayerControllerAthena::StaticClass();
		if (!IsLiveEventObject(ControllerClass))
			return 0;

		FString Cue(L"ATLAS_DURIAN_PHASE:5");
		int32 RecipientCount = 0;
		for (int32 Index = 0; Index < Connections.Num(); ++Index)
		{
			auto Connection = Connections[Index];
			if (!IsLiveEventObject(Connection) ||
				!Connection->HasPlayerController())
			{
				continue;
			}

			auto PlayerController = Connection->PlayerController;
			if (!IsLiveEventObject(PlayerController) ||
				!PlayerController->IsA(ControllerClass))
			{
				continue;
			}

			PlayerController->ClientMessage(Cue, FName(), 0.f);
			++RecipientCount;
		}
		Cue.Free();
		return RecipientCount;
	}

	bool TryGetEventObjectIdentity(
		const UObject* Object, FEventObjectIdentity& OutIdentity)
	{
		OutIdentity = {};
		if (!IsLiveEventObject(Object))
			return false;
		const int32 ObjectIndex = Object->Index;
		auto Item = TUObjectArray::GetItemByIndex(ObjectIndex);
		if (!Item || Item->GetObject() != Object)
			return false;
		OutIdentity.ObjectIndex = ObjectIndex;
		OutIdentity.SerialNumber = Item->SerialNumber;
		return true;
	}

	bool HasTrackedIdentity(
		const std::vector<FEventObjectIdentity>& Identities,
		const FEventObjectIdentity& Identity)
	{
		return std::find(
			Identities.begin(), Identities.end(), Identity) !=
			Identities.end();
	}

	UObject* ResolveEventObjectIdentity(
		const FEventObjectIdentity& Identity)
	{
		if (Identity.ObjectIndex < 0 ||
			Identity.ObjectIndex >= TUObjectArray::Num())
		{
			return nullptr;
		}

		auto Item = TUObjectArray::GetItemByIndex(
			Identity.ObjectIndex);
		if (!Item || Item->SerialNumber != Identity.SerialNumber)
			return nullptr;

		auto Object = const_cast<UObject*>(Item->GetObject());
		return IsLiveEventObject(Object) ? Object : nullptr;
	}

	bool IsSaneArray(
		const void* Data,
		int32 Num,
		int32 Max,
		size_t ElementSize,
		int32 MaximumElements)
	{
		if (Num < 0 || Max < Num || Max > MaximumElements ||
			ElementSize == 0 || ElementSize > 0x1000)
		{
			return false;
		}
		if (Num == 0)
			return true;
		if (!Data || static_cast<size_t>(Num) >
			(std::numeric_limits<size_t>::max)() / ElementSize)
		{
			return false;
		}
		return SDK::MemReadable(
			Data, static_cast<size_t>(Num) * ElementSize);
	}

	bool IsInputParameter(const UFunction::ParamNamed& Parameter)
	{
		if (!(Parameter.PropertyFlags & CPF_Parm) ||
			(Parameter.PropertyFlags & CPF_ReturnParm))
		{
			return false;
		}
		return !(Parameter.PropertyFlags & CPF_OutParm) ||
			(Parameter.PropertyFlags & CPF_ReferenceParm);
	}

	bool HasParameterPropertyType(
		UFunction* Function,
		const UFunction::ParamNamed& Parameter,
		uint64 CastFlags)
	{
		if (!Function || Parameter.Name.empty())
			return false;
		const auto NamedProperty = Function->GetProperty(
			Parameter.Name.c_str());
		const auto TypedProperty = Function->GetProperty(
			Parameter.Name.c_str(), CastFlags);
		return NamedProperty && NamedProperty == TypedProperty;
	}

	bool ValidateParameterBuffer(UFunction* Function)
	{
		if (!IsLiveEventObject(Function))
			return false;

		const auto Parameters = Function->GetParamsNamed();
		if (Parameters.Size > 0x1000)
			return false;

		for (const auto& Parameter : Parameters.NameOffsetMap)
		{
			if (!(Parameter.PropertyFlags & CPF_Parm))
				continue;
			if (Parameter.ElementSize == 0 ||
				Parameter.Offset > Parameters.Size ||
				Parameter.ElementSize > Parameters.Size - Parameter.Offset)
			{
				return false;
			}
		}
		return true;
	}

	bool CanInvokeWithZeroedScalarInput(UFunction* Function)
	{
		if (!ValidateParameterBuffer(Function))
			return false;

		int32 InputCount = 0;
		const auto Parameters = Function->GetParamsNamed();
		for (const auto& Parameter : Parameters.NameOffsetMap)
		{
			if (!IsInputParameter(Parameter))
				continue;

			++InputCount;
			if (InputCount > 1)
				return false;

			const bool bSupportedScalar =
				(Parameter.ElementSize == sizeof(uint8) &&
					HasParameterPropertyType(
						Function, Parameter, CastClassBoolProperty)) ||
				(Parameter.ElementSize == sizeof(int32) &&
					HasParameterPropertyType(
						Function, Parameter, CastClassIntProperty)) ||
				(Parameter.ElementSize == sizeof(float) &&
					HasParameterPropertyType(
						Function, Parameter, CastClassFloatProperty)) ||
				(Parameter.ElementSize == sizeof(double) &&
					HasParameterPropertyType(
						Function, Parameter, CastClassDoubleProperty));
			if (!bSupportedScalar)
				return false;
		}
		return true;
	}

	bool InvokeZeroed(UObject* Target, UFunction* Function)
	{
		if (!IsLiveEventObject(Target) ||
			!CanInvokeWithZeroedScalarInput(Function))
		{
			return false;
		}

		const auto Parameters = Function->GetParamsNamed();
		const uint32 Size = Parameters.Size;
		if (Size == 0)
		{
			Target->ProcessEvent(Function, nullptr);
			return true;
		}

		void* Memory = FMemory::Malloc(Size);
		if (!Memory)
			return false;
		memset(Memory, 0, Size);
		Target->ProcessEvent(Function, Memory);
		FMemory::Free(Memory);
		return true;
	}

	bool InvokeContentLoader(UObject* Target, UFunction* Function)
	{
		if (!IsLiveEventObject(Target) ||
			!ValidateParameterBuffer(Function))
		{
			return false;
		}

		const auto Parameters = Function->GetParamsNamed();
		uint32 InputOffset = InvalidParamOffset;
		uint32 InputSize = 0;
		int32 InputCount = 0;
		bool bBoolInput = false;
		bool bIntInput = false;
		for (const auto& Parameter : Parameters.NameOffsetMap)
		{
			if (!IsInputParameter(Parameter))
				continue;
			++InputCount;
			InputOffset = Parameter.Offset;
			InputSize = Parameter.ElementSize;
			bBoolInput =
				InputSize == sizeof(uint8) &&
				HasParameterPropertyType(
					Function, Parameter, CastClassBoolProperty);
			bIntInput =
				InputSize == sizeof(int32) &&
				HasParameterPropertyType(
					Function, Parameter, CastClassIntProperty);
		}
		if (InputCount > 1 ||
			(InputCount == 1 && !bBoolInput && !bIntInput))
		{
			return false;
		}

		if (Parameters.Size == 0)
		{
			if (InputCount != 0)
				return false;
			Target->ProcessEvent(Function, nullptr);
			return true;
		}

		void* Memory = FMemory::Malloc(Parameters.Size);
		if (!Memory)
			return false;
		memset(Memory, 0, Parameters.Size);
		if (InputCount == 1)
		{
			if (bBoolInput)
			{
				const uint8 Enabled = 1;
				memcpy((PBYTE)Memory + InputOffset,
					&Enabled, sizeof(Enabled));
			}
			else
			{
				// The original server setup supplied an integer 1 to these
				// legacy loader events. Preserve that value while honoring the
				// reflected offset and buffer bounds.
				const int32 Enabled = 1;
				memcpy((PBYTE)Memory + InputOffset,
					&Enabled, sizeof(Enabled));
			}
		}
		Target->ProcessEvent(Function, Memory);
		FMemory::Free(Memory);
		return true;
	}

	bool InvokeBoolNoInput(
		UObject* Target, UFunction* Function, bool& OutValue)
	{
		OutValue = false;
		if (!IsLiveEventObject(Target) ||
			!ValidateParameterBuffer(Function))
		{
			return false;
		}

		const auto Parameters = Function->GetParamsNamed();
		uint32 ReturnOffset = InvalidParamOffset;
		for (const auto& Parameter : Parameters.NameOffsetMap)
		{
			if (!(Parameter.PropertyFlags & CPF_Parm))
				continue;
			if (Parameter.PropertyFlags & CPF_ReturnParm)
			{
				if (ReturnOffset != InvalidParamOffset ||
					Parameter.ElementSize != sizeof(uint8) ||
					!HasParameterPropertyType(
						Function, Parameter, CastClassBoolProperty))
				{
					return false;
				}
				ReturnOffset = Parameter.Offset;
				continue;
			}
			return false;
		}
		if (ReturnOffset == InvalidParamOffset || Parameters.Size == 0)
			return false;

		void* Memory = FMemory::Malloc(Parameters.Size);
		if (!Memory)
			return false;
		memset(Memory, 0, Parameters.Size);
		Target->ProcessEvent(Function, Memory);
		OutValue =
			*reinterpret_cast<const uint8*>(
				(PBYTE)Memory + ReturnOffset) != 0;
		FMemory::Free(Memory);
		return true;
	}

	bool ResolveOnReadyLayout(
		UFunction* Function, FOnReadyLayout& OutLayout)
	{
		OutLayout = {};
		OutLayout.GameStateOffset = InvalidParamOffset;
		OutLayout.PlaylistOffset = InvalidParamOffset;
		OutLayout.ContextTagsOffset = InvalidParamOffset;
		if (!ValidateParameterBuffer(Function))
			return false;

		const auto Parameters = Function->GetParamsNamed();
		OutLayout.Size = Parameters.Size;
		for (const auto& Parameter : Parameters.NameOffsetMap)
		{
			if (!IsInputParameter(Parameter))
				continue;

			uint32* Destination = nullptr;
			uint32 ExpectedSize = 0;
			uint64 ExpectedCastFlags = 0;
			if (Parameter.Name == "GameState")
			{
				Destination = &OutLayout.GameStateOffset;
				ExpectedSize = sizeof(UObject*);
				ExpectedCastFlags = CastClassObjectProperty;
			}
			else if (Parameter.Name == "Playlist")
			{
				Destination = &OutLayout.PlaylistOffset;
				ExpectedSize = sizeof(UFortPlaylistAthena*);
				ExpectedCastFlags = CastClassObjectProperty;
			}
			else if (Parameter.Name == "PlaylistContextTags")
			{
				Destination = &OutLayout.ContextTagsOffset;
				ExpectedSize = sizeof(FGameplayTagContainer);
				ExpectedCastFlags = CastClassStructProperty;
			}
			else
			{
				return false;
			}

			if (*Destination != InvalidParamOffset ||
				Parameter.ElementSize != ExpectedSize ||
				!HasParameterPropertyType(
					Function, Parameter, ExpectedCastFlags))
			{
				return false;
			}
			*Destination = Parameter.Offset;
		}

		return OutLayout.GameStateOffset != InvalidParamOffset &&
			OutLayout.PlaylistOffset != InvalidParamOffset &&
			OutLayout.ContextTagsOffset != InvalidParamOffset;
	}

	bool InvokeOnReady(
		UObject* Target,
		UFunction* Function,
		const FEventContext& Context)
	{
		FOnReadyLayout Layout{};
		if (!IsLiveEventObject(Target) ||
			!IsLiveEventObject(Context.GameState) ||
			!IsLiveEventObject(Context.Playlist) ||
			!ResolveOnReadyLayout(Function, Layout))
		{
			return false;
		}

		void* Memory = FMemory::Malloc(Layout.Size);
		if (!Memory)
			return false;
		memset(Memory, 0, Layout.Size);

		UObject* GameState = Context.GameState;
		const UFortPlaylistAthena* Playlist = Context.Playlist;
		FGameplayTagContainer ContextTags{};
		if (Playlist->HasGameplayTagContainer())
			ContextTags = Playlist->GameplayTagContainer;

		memcpy((PBYTE)Memory + Layout.GameStateOffset,
			&GameState, sizeof(GameState));
		memcpy((PBYTE)Memory + Layout.PlaylistOffset,
			&Playlist, sizeof(Playlist));
		memcpy((PBYTE)Memory + Layout.ContextTagsOffset,
			&ContextTags, sizeof(ContextTags));
		Target->ProcessEvent(Function, Memory);
		FMemory::Free(Memory);
		return true;
	}

	bool ResolveStartAtIndexLayout(
		UFunction* Function, FStartAtIndexLayout& OutLayout)
	{
		OutLayout = {};
		OutLayout.IndexOffset = InvalidParamOffset;
		OutLayout.TimeOffset = InvalidParamOffset;
		if (!ValidateParameterBuffer(Function))
			return false;

		const auto Parameters = Function->GetParamsNamed();
		OutLayout.Size = Parameters.Size;
		for (const auto& Parameter : Parameters.NameOffsetMap)
		{
			if (!IsInputParameter(Parameter))
				continue;

			const char* Name = Parameter.Name.c_str();
			if (Name && strstr(Name, "Index"))
			{
				if (OutLayout.IndexOffset != InvalidParamOffset ||
					Parameter.ElementSize != sizeof(int32) ||
					!HasParameterPropertyType(
						Function, Parameter, CastClassIntProperty))
				{
					return false;
				}
				OutLayout.IndexOffset = Parameter.Offset;
			}
			else if (Name &&
				(strstr(Name, "TimeOffset") ||
				 strstr(Name, "SequenceTime")))
			{
				if (OutLayout.TimeOffset != InvalidParamOffset ||
					(Parameter.ElementSize != sizeof(float) &&
					 Parameter.ElementSize != sizeof(double)) ||
					(Parameter.ElementSize == sizeof(float)
						? !HasParameterPropertyType(
							Function, Parameter,
							CastClassFloatProperty)
						: !HasParameterPropertyType(
							Function, Parameter,
							CastClassDoubleProperty)))
				{
					return false;
				}
				OutLayout.TimeOffset = Parameter.Offset;
				OutLayout.TimeSize = Parameter.ElementSize;
			}
			else
			{
				return false;
			}
		}

		return OutLayout.IndexOffset != InvalidParamOffset;
	}

	bool InvokeStartAtIndex(UObject* Target, UFunction* Function)
	{
		FStartAtIndexLayout Layout{};
		if (!IsLiveEventObject(Target) ||
			!ResolveStartAtIndexLayout(Function, Layout) ||
			Layout.Size == 0)
		{
			return false;
		}

		void* Memory = FMemory::Malloc(Layout.Size);
		if (!Memory)
			return false;
		memset(Memory, 0, Layout.Size);

		const int32 StartingIndex = 0;
		memcpy((PBYTE)Memory + Layout.IndexOffset,
			&StartingIndex, sizeof(StartingIndex));
		if (Layout.TimeOffset != InvalidParamOffset)
		{
			if (Layout.TimeSize == sizeof(double))
			{
				const double SequenceTimeOffset = 0.0;
				memcpy((PBYTE)Memory + Layout.TimeOffset,
					&SequenceTimeOffset, sizeof(SequenceTimeOffset));
			}
			else
			{
				const float SequenceTimeOffset = 0.f;
				memcpy((PBYTE)Memory + Layout.TimeOffset,
					&SequenceTimeOffset, sizeof(SequenceTimeOffset));
			}
		}

		Target->ProcessEvent(Function, Memory);
		FMemory::Free(Memory);
		return true;
	}

	bool InvokeOptionalName(
		UObject* Target, UFunction* Function, const FName& Value)
	{
		if (!IsLiveEventObject(Target) ||
			!ValidateParameterBuffer(Function))
		{
			return false;
		}

		const auto Parameters = Function->GetParamsNamed();
		uint32 NameOffset = InvalidParamOffset;
		int32 InputCount = 0;
		for (const auto& Parameter : Parameters.NameOffsetMap)
		{
			if (!IsInputParameter(Parameter))
				continue;
			++InputCount;
			if (Parameter.ElementSize != sizeof(FName) ||
				!HasParameterPropertyType(
					Function, Parameter, CastClassNameProperty))
				return false;
			NameOffset = Parameter.Offset;
		}
		if (InputCount > 1)
			return false;

		if (Parameters.Size == 0)
		{
			if (InputCount != 0)
				return false;
			Target->ProcessEvent(Function, nullptr);
			return true;
		}

		void* Memory = FMemory::Malloc(Parameters.Size);
		if (!Memory)
			return false;
		memset(Memory, 0, Parameters.Size);
		if (InputCount == 1)
			memcpy((PBYTE)Memory + NameOffset, &Value, sizeof(Value));
		Target->ProcessEvent(Function, Memory);
		FMemory::Free(Memory);
		return true;
	}

	const FEvent* FindCurrentEvent()
	{
		for (const auto& Event : Events::EventsArray)
		{
			if (fabs(Event.EventVersion -
				VersionInfo.FortniteVersion) < 0.001)
			{
				return &Event;
			}
		}
		return nullptr;
	}

	bool UsesRiftTour1730Compatibility(const FEvent* Event)
	{
		return Event &&
			fabs(Event->EventVersion - 17.30) < 0.001 &&
			Event->ScriptingClass &&
			wcscmp(
				Event->ScriptingClass,
				L"/Buffet/Gameplay/Blueprints/Buffet_SpecialEventScript.Buffet_SpecialEventScript_C") == 0;
	}

	bool UsesKiwi1750Compatibility(const FEvent* Event)
	{
		return Event &&
			fabs(Event->EventVersion - 17.50) < 0.001 &&
			Event->ScriptingClass &&
			wcscmp(
				Event->ScriptingClass,
				L"/Kiwi/Gameplay/Kiwi_EventScript.Kiwi_EventScript_C") == 0;
	}

	bool RiftTourMeshPredicateTrue()
	{
		return true;
	}

	bool RiftTourMeshPredicateFalse()
	{
		return false;
	}

	uint8 RiftTourMeshNodeTypeEdge(void*)
	{
		return 2;
	}

	template <size_t ByteCount>
	bool MatchesEventCode(
		uint64 Address,
		const uint8 (&Expected)[ByteCount])
	{
		return Address &&
			SDK::MemReadable(
				reinterpret_cast<const void*>(Address),
				ByteCount) &&
			memcmp(
				reinterpret_cast<const void*>(Address),
				Expected,
				ByteCount) == 0;
	}

	void InstallRiftTourMeshPredicateHooks()
	{
		if (fabs(VersionInfo.FortniteVersion - 17.30) >= 0.001)
			return;

		constexpr uint64 MeshNodeTypeRva = 0x3E07910;
		constexpr uint64 RootPredicateRva = 0x3DED158;
		constexpr uint64 EdgeWorldPredicateRva = 0x3DECFC8;
		constexpr uint64 EdgeActorPredicateRva = 0x3DED050;
		constexpr uint64 ClientPredicateRva = 0x3DECF40;
		constexpr uint8 MeshNodeTypeCode[] =
		{
			0x40, 0x53, 0x48, 0x83, 0xEC, 0x20,
			0x48, 0x8B, 0x81, 0xA8, 0x00, 0x00, 0x00,
		};
		constexpr uint8 RootPredicateCode[] =
		{
			0x48, 0x83, 0xEC, 0x28,
			0x48, 0x8B, 0x89, 0x28, 0x02, 0x00, 0x00,
			0x48, 0x85, 0xC9, 0x74,
		};
		constexpr uint8 EdgeWorldPredicateCode[] =
		{
			0x48, 0x89, 0x5C, 0x24, 0x08,
			0x48, 0x89, 0x74, 0x24, 0x10,
			0x57, 0x48, 0x83, 0xEC, 0x20,
			0x48, 0x8B, 0xF1, 0x33, 0xDB,
			0x48, 0x8B, 0x89, 0x70, 0x02, 0x00, 0x00,
		};
		constexpr uint8 EdgeActorPredicateCode[] =
		{
			0x48, 0x89, 0x5C, 0x24, 0x08,
			0x48, 0x89, 0x74, 0x24, 0x10,
			0x57, 0x48, 0x83, 0xEC, 0x20,
			0x48, 0x8B, 0xF1, 0x33, 0xDB,
			0x48, 0x8B, 0x89, 0x28, 0x02, 0x00, 0x00,
		};

		const uint64 MeshNodeType =
			ImageBase + MeshNodeTypeRva;
		const uint64 RootPredicate =
			ImageBase + RootPredicateRva;
		const uint64 EdgeWorldPredicate =
			ImageBase + EdgeWorldPredicateRva;
		const uint64 EdgeActorPredicate =
			ImageBase + EdgeActorPredicateRva;
		const uint64 ClientPredicate =
			ImageBase + ClientPredicateRva;
		const bool bMeshNodeMatched = MatchesEventCode(
			MeshNodeType, MeshNodeTypeCode);
		const bool bRootPredicateMatched = MatchesEventCode(
			RootPredicate, RootPredicateCode);
		const bool bEdgeWorldPredicateMatched = MatchesEventCode(
			EdgeWorldPredicate, EdgeWorldPredicateCode);
		const bool bEdgeActorPredicateMatched = MatchesEventCode(
			EdgeActorPredicate, EdgeActorPredicateCode);
		const bool bClientPredicateMatched = MatchesEventCode(
			ClientPredicate, EdgeWorldPredicateCode);

		if (bMeshNodeMatched)
		{
			Utils::Hook(
				MeshNodeType,
				RiftTourMeshNodeTypeEdge);
		}
		if (bRootPredicateMatched)
		{
			Utils::Hook(
				RootPredicate,
				RiftTourMeshPredicateTrue);
		}
		if (bEdgeWorldPredicateMatched)
		{
			Utils::Hook(
				EdgeWorldPredicate,
				RiftTourMeshPredicateTrue);
		}
		if (bEdgeActorPredicateMatched)
		{
			Utils::Hook(
				EdgeActorPredicate,
				RiftTourMeshPredicateTrue);
		}
		if (bClientPredicateMatched)
		{
			Utils::Hook(
				ClientPredicate,
				RiftTourMeshPredicateFalse);
		}
		SDK::DbgLog(
			"[Events] Rift Tour compatibility hooks mesh=%d root=%d "
			"edgeWorld=%d edgeActor=%d client=%d\n",
			bMeshNodeMatched ? 1 : 0,
			bRootPredicateMatched ? 1 : 0,
			bEdgeWorldPredicateMatched ? 1 : 0,
			bEdgeActorPredicateMatched ? 1 : 0,
			bClientPredicateMatched ? 1 : 0);
	}

	AActor* FindLiveActor(const wchar_t* ClassPath)
	{
		if (!ClassPath)
			return nullptr;
		auto ActorClass = FindObject<UClass>(ClassPath);
		if (!IsLiveEventObject(ActorClass))
			return nullptr;

		TArray<AActor*> Actors;
		Utils::GetAll(ActorClass, Actors);
		AActor* Result = nullptr;
		if (IsSaneArray(
			Actors.Data, Actors.Num(), Actors.Max(),
			sizeof(AActor*), 4096))
		{
			for (int32 Index = 0; Index < Actors.Num(); ++Index)
			{
				auto Actor = Actors[Index];
				if (IsLiveEventObject(Actor))
				{
					Result = Actor;
					break;
				}
			}
		}
		Actors.Free();
		return Result;
	}

	template <typename ValueType>
	bool ReadEventProperty(
		UObject* Object,
		const char* PropertyName,
		uint64 CastFlags,
		ValueType& OutValue)
	{
		OutValue = {};
		if (!IsLiveEventObject(Object) || !PropertyName)
			return false;

		const uint32 Offset = Object->GetOffset(
			PropertyName, CastFlags);
		const int32 ObjectSize = Object->Class->GetPropertiesSize();
		if (Offset == InvalidParamOffset || ObjectSize <= 0 ||
			Offset > static_cast<uint32>(ObjectSize) ||
			sizeof(ValueType) >
				static_cast<uint32>(ObjectSize) - Offset)
		{
			return false;
		}

		auto Address = reinterpret_cast<const uint8*>(Object) + Offset;
		if (!SDK::MemReadable(Address, sizeof(ValueType)))
			return false;
		memcpy(&OutValue, Address, sizeof(ValueType));
		return true;
	}

	template <typename ValueType>
	bool WriteEventProperty(
		UObject* Object,
		const char* PropertyName,
		uint64 CastFlags,
		const ValueType& Value)
	{
		if (!IsLiveEventObject(Object) || !PropertyName)
			return false;

		const uint32 Offset = Object->GetOffset(
			PropertyName, CastFlags);
		const int32 ObjectSize = Object->Class->GetPropertiesSize();
		if (Offset == InvalidParamOffset || ObjectSize <= 0 ||
			Offset > static_cast<uint32>(ObjectSize) ||
			sizeof(ValueType) >
				static_cast<uint32>(ObjectSize) - Offset)
		{
			return false;
		}

		auto Address = reinterpret_cast<uint8*>(Object) + Offset;
		if (!SDK::MemReadable(Address, sizeof(ValueType)))
			return false;
		memcpy(Address, &Value, sizeof(ValueType));
		return true;
	}

	bool InvokeSingleInput(
		UObject* Target,
		UFunction* Function,
		const void* Value,
		uint32 ValueSize,
		uint64 CastFlags = 0)
	{
		if (!IsLiveEventObject(Target) || !Value || !ValueSize ||
			!ValidateParameterBuffer(Function))
		{
			return false;
		}

		const auto Parameters = Function->GetParamsNamed();
		uint32 InputOffset = InvalidParamOffset;
		int32 InputCount = 0;
		for (const auto& Parameter : Parameters.NameOffsetMap)
		{
			if (!IsInputParameter(Parameter))
				continue;
			++InputCount;
			if (Parameter.ElementSize != ValueSize ||
				(CastFlags && !HasParameterPropertyType(
					Function, Parameter, CastFlags)))
			{
				return false;
			}
			InputOffset = Parameter.Offset;
		}
		if (InputCount != 1 || InputOffset == InvalidParamOffset ||
			Parameters.Size == 0)
		{
			return false;
		}

		void* Memory = FMemory::Malloc(Parameters.Size);
		if (!Memory)
			return false;
		memset(Memory, 0, Parameters.Size);
		memcpy(reinterpret_cast<uint8*>(Memory) + InputOffset,
			Value, ValueSize);
		Target->ProcessEvent(Function, Memory);
		FMemory::Free(Memory);
		return true;
	}

	bool InvokeSmallIntegerInput(
		UObject* Target,
		UFunction* Function,
		int32 Value)
	{
		if (!IsLiveEventObject(Target) ||
			!ValidateParameterBuffer(Function))
		{
			return false;
		}

		const auto Parameters = Function->GetParamsNamed();
		uint32 InputOffset = InvalidParamOffset;
		uint32 InputSize = 0;
		int32 InputCount = 0;
		for (const auto& Parameter : Parameters.NameOffsetMap)
		{
			if (!IsInputParameter(Parameter))
				continue;
			++InputCount;
			if (Parameter.ElementSize != sizeof(uint8) &&
				Parameter.ElementSize != sizeof(uint16) &&
				Parameter.ElementSize != sizeof(int32))
			{
				return false;
			}
			InputOffset = Parameter.Offset;
			InputSize = Parameter.ElementSize;
		}
		if (InputCount != 1 || InputOffset == InvalidParamOffset ||
			Parameters.Size == 0)
		{
			return false;
		}

		void* Memory = FMemory::Malloc(Parameters.Size);
		if (!Memory)
			return false;
		memset(Memory, 0, Parameters.Size);
		memcpy(reinterpret_cast<uint8*>(Memory) + InputOffset,
			&Value, InputSize);
		Target->ProcessEvent(Function, Memory);
		FMemory::Free(Memory);
		return true;
	}

	bool InvokeObjectInputReturningObject(
		UObject* Target,
		UFunction* Function,
		const void* InputValue,
		uint64 InputCastFlags,
		UObject*& OutValue)
	{
		OutValue = nullptr;
		if (!IsLiveEventObject(Target) || !InputValue ||
			!ValidateParameterBuffer(Function))
		{
			return false;
		}

		const auto Parameters = Function->GetParamsNamed();
		uint32 InputOffset = InvalidParamOffset;
		uint32 ReturnOffset = InvalidParamOffset;
		int32 InputCount = 0;
		int32 ReturnCount = 0;
		for (const auto& Parameter : Parameters.NameOffsetMap)
		{
			if (Parameter.PropertyFlags & CPF_ReturnParm)
			{
				++ReturnCount;
				if (Parameter.ElementSize != sizeof(UObject*) ||
					!HasParameterPropertyType(
						Function, Parameter,
						CastClassObjectProperty))
				{
					return false;
				}
				ReturnOffset = Parameter.Offset;
				continue;
			}
			if (!IsInputParameter(Parameter))
				continue;
			++InputCount;
			if (Parameter.ElementSize != sizeof(UObject*) ||
				!HasParameterPropertyType(
					Function, Parameter, InputCastFlags))
			{
				return false;
			}
			InputOffset = Parameter.Offset;
		}
		if (InputCount != 1 || ReturnCount != 1 ||
			InputOffset == InvalidParamOffset ||
			ReturnOffset == InvalidParamOffset ||
			Parameters.Size == 0)
		{
			return false;
		}

		void* Memory = FMemory::Malloc(Parameters.Size);
		if (!Memory)
			return false;
		memset(Memory, 0, Parameters.Size);
		memcpy(reinterpret_cast<uint8*>(Memory) + InputOffset,
			InputValue, sizeof(UObject*));
		Target->ProcessEvent(Function, Memory);
		memcpy(&OutValue,
			reinterpret_cast<uint8*>(Memory) + ReturnOffset,
			sizeof(OutValue));
		FMemory::Free(Memory);
		return true;
	}

	bool InvokeObjectAndFloatInputs(
		UObject* Target,
		UFunction* Function,
		UObject* ObjectValue,
		float FloatValue)
	{
		if (!IsLiveEventObject(Target) ||
			!IsLiveEventObject(ObjectValue) ||
			!ValidateParameterBuffer(Function))
		{
			return false;
		}

		const auto Parameters = Function->GetParamsNamed();
		uint32 ObjectOffset = InvalidParamOffset;
		uint32 FloatOffset = InvalidParamOffset;
		int32 InputCount = 0;
		for (const auto& Parameter : Parameters.NameOffsetMap)
		{
			if (!IsInputParameter(Parameter))
				continue;
			++InputCount;
			if (Parameter.ElementSize == sizeof(UObject*) &&
				HasParameterPropertyType(
					Function, Parameter,
					CastClassObjectProperty))
			{
				if (ObjectOffset != InvalidParamOffset)
					return false;
				ObjectOffset = Parameter.Offset;
			}
			else if (Parameter.ElementSize == sizeof(float) &&
				HasParameterPropertyType(
					Function, Parameter,
					CastClassFloatProperty))
			{
				if (FloatOffset != InvalidParamOffset)
					return false;
				FloatOffset = Parameter.Offset;
			}
			else
			{
				return false;
			}
		}
		if (InputCount != 2 ||
			ObjectOffset == InvalidParamOffset ||
			FloatOffset == InvalidParamOffset ||
			Parameters.Size == 0)
		{
			return false;
		}

		void* Memory = FMemory::Malloc(Parameters.Size);
		if (!Memory)
			return false;
		memset(Memory, 0, Parameters.Size);
		memcpy(reinterpret_cast<uint8*>(Memory) + ObjectOffset,
			&ObjectValue, sizeof(ObjectValue));
		memcpy(reinterpret_cast<uint8*>(Memory) + FloatOffset,
			&FloatValue, sizeof(FloatValue));
		Target->ProcessEvent(Function, Memory);
		FMemory::Free(Memory);
		return true;
	}

	bool InvokeObjectInputReturningTransform(
		UObject* Target,
		UFunction* Function,
		UObject* InputValue,
		FTransform& OutValue)
	{
		OutValue = {};
		if (!IsLiveEventObject(Target) ||
			!IsLiveEventObject(InputValue) ||
			!ValidateParameterBuffer(Function))
		{
			return false;
		}

		const int32 TransformSize = FTransform::Size();
		if (TransformSize <= 0 ||
			TransformSize > static_cast<int32>(sizeof(FTransform)))
		{
			return false;
		}

		const auto Parameters = Function->GetParamsNamed();
		uint32 InputOffset = InvalidParamOffset;
		uint32 ReturnOffset = InvalidParamOffset;
		int32 InputCount = 0;
		int32 ReturnCount = 0;
		for (const auto& Parameter : Parameters.NameOffsetMap)
		{
			if (Parameter.PropertyFlags & CPF_ReturnParm)
			{
				++ReturnCount;
				if (Parameter.ElementSize != TransformSize ||
					!HasParameterPropertyType(
						Function, Parameter,
						CastClassStructProperty))
				{
					return false;
				}
				ReturnOffset = Parameter.Offset;
				continue;
			}
			if (IsInputParameter(Parameter))
			{
				++InputCount;
				if (Parameter.ElementSize != sizeof(UObject*) ||
					!HasParameterPropertyType(
						Function, Parameter,
						CastClassObjectProperty))
				{
					return false;
				}
				InputOffset = Parameter.Offset;
				continue;
			}
			if (Parameter.PropertyFlags & CPF_Parm)
				return false;
		}
		if (InputCount != 1 || ReturnCount != 1 ||
			InputOffset == InvalidParamOffset ||
			ReturnOffset == InvalidParamOffset ||
			Parameters.Size == 0)
		{
			return false;
		}

		void* Memory = FMemory::Malloc(Parameters.Size);
		if (!Memory)
			return false;
		memset(Memory, 0, Parameters.Size);
		memcpy(reinterpret_cast<uint8*>(Memory) + InputOffset,
			&InputValue, sizeof(InputValue));
		Target->ProcessEvent(Function, Memory);
		memcpy(&OutValue,
			reinterpret_cast<uint8*>(Memory) + ReturnOffset,
			TransformSize);
		FMemory::Free(Memory);
		return
			FPlatformMath::IsFinite(OutValue.Translation.X) &&
			FPlatformMath::IsFinite(OutValue.Translation.Y) &&
			FPlatformMath::IsFinite(OutValue.Translation.Z);
	}

	bool InvokeFloatNoInput(
		UObject* Target,
		UFunction* Function,
		float& OutValue)
	{
		OutValue = 0.f;
		if (!IsLiveEventObject(Target) ||
			!ValidateParameterBuffer(Function))
		{
			return false;
		}

		const auto Parameters = Function->GetParamsNamed();
		uint32 ReturnOffset = InvalidParamOffset;
		int32 ReturnCount = 0;
		for (const auto& Parameter : Parameters.NameOffsetMap)
		{
			if (IsInputParameter(Parameter))
				return false;
			if (!(Parameter.PropertyFlags & CPF_ReturnParm))
				continue;
			++ReturnCount;
			if (Parameter.ElementSize != sizeof(float) ||
				!HasParameterPropertyType(
					Function, Parameter, CastClassFloatProperty))
			{
				return false;
			}
			ReturnOffset = Parameter.Offset;
		}
		if (ReturnCount != 1 || ReturnOffset == InvalidParamOffset ||
			Parameters.Size == 0)
		{
			return false;
		}

		void* Memory = FMemory::Malloc(Parameters.Size);
		if (!Memory)
			return false;
		memset(Memory, 0, Parameters.Size);
		Target->ProcessEvent(Function, Memory);
		memcpy(&OutValue,
			reinterpret_cast<uint8*>(Memory) + ReturnOffset,
			sizeof(OutValue));
		FMemory::Free(Memory);
		return FPlatformMath::IsFinite(OutValue);
	}

	UObject* InvokeAddComponentByClass(
		AActor* Actor,
		const UClass* ComponentClass)
	{
		if (!IsLiveEventObject(Actor) ||
			!IsLiveEventObject(ComponentClass))
		{
			return nullptr;
		}

		auto Function = Actor->GetFunction("AddComponentByClass");
		if (!ValidateParameterBuffer(Function))
			return nullptr;

		const auto Parameters = Function->GetParamsNamed();
		uint32 ClassOffset = InvalidParamOffset;
		uint32 ManualAttachmentOffset = InvalidParamOffset;
		uint32 TransformOffset = InvalidParamOffset;
		uint32 DeferredFinishOffset = InvalidParamOffset;
		uint32 ReturnOffset = InvalidParamOffset;
		int32 InputCount = 0;
		int32 ReturnCount = 0;
		for (const auto& Parameter : Parameters.NameOffsetMap)
		{
			if (Parameter.PropertyFlags & CPF_ReturnParm)
			{
				++ReturnCount;
				if (Parameter.ElementSize != sizeof(UObject*) ||
					!HasParameterPropertyType(
						Function, Parameter,
						CastClassObjectProperty))
				{
					return nullptr;
				}
				ReturnOffset = Parameter.Offset;
				continue;
			}
			if (!IsInputParameter(Parameter))
				continue;

			++InputCount;
			if (Parameter.Name == "Class" ||
				Parameter.Name == "ComponentClass")
			{
				if (ClassOffset != InvalidParamOffset ||
					Parameter.ElementSize != sizeof(UClass*) ||
					!HasParameterPropertyType(
						Function, Parameter,
						CastClassClassProperty))
				{
					return nullptr;
				}
				ClassOffset = Parameter.Offset;
			}
			else if (Parameter.Name == "bManualAttachment")
			{
				if (Parameter.ElementSize != sizeof(uint8) ||
					!HasParameterPropertyType(
						Function, Parameter,
						CastClassBoolProperty))
				{
					return nullptr;
				}
				ManualAttachmentOffset = Parameter.Offset;
			}
			else if (Parameter.Name == "RelativeTransform")
			{
				if (Parameter.ElementSize != FTransform::Size() ||
					!HasParameterPropertyType(
						Function, Parameter,
						CastClassStructProperty))
				{
					return nullptr;
				}
				TransformOffset = Parameter.Offset;
			}
			else if (Parameter.Name == "bDeferredFinish")
			{
				if (Parameter.ElementSize != sizeof(uint8) ||
					!HasParameterPropertyType(
						Function, Parameter,
						CastClassBoolProperty))
				{
					return nullptr;
				}
				DeferredFinishOffset = Parameter.Offset;
			}
			else
			{
				return nullptr;
			}
		}
		if (InputCount != 4 || ReturnCount != 1 ||
			ClassOffset == InvalidParamOffset ||
			ManualAttachmentOffset == InvalidParamOffset ||
			TransformOffset == InvalidParamOffset ||
			DeferredFinishOffset == InvalidParamOffset ||
			ReturnOffset == InvalidParamOffset ||
			Parameters.Size == 0)
		{
			return nullptr;
		}

		void* Memory = FMemory::Malloc(Parameters.Size);
		if (!Memory)
			return nullptr;
		memset(Memory, 0, Parameters.Size);
		const UClass* ClassValue = ComponentClass;
		const uint8 Disabled = 0;
		FTransform RelativeTransform{};
		memcpy(reinterpret_cast<uint8*>(Memory) + ClassOffset,
			&ClassValue, sizeof(ClassValue));
		memcpy(reinterpret_cast<uint8*>(Memory) + ManualAttachmentOffset,
			&Disabled, sizeof(Disabled));
		memcpy(reinterpret_cast<uint8*>(Memory) + TransformOffset,
			&RelativeTransform, FTransform::Size());
		memcpy(reinterpret_cast<uint8*>(Memory) + DeferredFinishOffset,
			&Disabled, sizeof(Disabled));
		Actor->ProcessEvent(Function, Memory);

		UObject* Component = nullptr;
		memcpy(&Component,
			reinterpret_cast<uint8*>(Memory) + ReturnOffset,
			sizeof(Component));
		FMemory::Free(Memory);
		return IsLiveEventObject(Component) &&
			Component->IsA(ComponentClass)
			? Component
			: nullptr;
	}

	UObject* FindRiftTourComponent(
		AActor* Actor,
		const UClass* ComponentClass)
	{
		if (!IsLiveEventObject(Actor) ||
			!IsLiveEventObject(ComponentClass))
		{
			return nullptr;
		}

		UObject* Component = nullptr;
		auto GetComponentFunction =
			Actor->GetFunction("GetComponentByClass");
		const UClass* ClassValue = ComponentClass;
		if (InvokeObjectInputReturningObject(
				Actor,
				GetComponentFunction,
				&ClassValue,
				CastClassClassProperty,
				Component) &&
			IsLiveEventObject(Component) &&
			Component->IsA(ComponentClass))
		{
			return Component;
		}
		return nullptr;
	}

	UObject* EnsureRiftTourComponent(
		AActor* Actor,
		const UClass* ComponentClass)
	{
		auto Component = FindRiftTourComponent(
			Actor, ComponentClass);
		if (Component)
			return Component;

		return InvokeAddComponentByClass(Actor, ComponentClass);
	}

	void SetRiftTourComponentEnabled(
		UObject* Component, bool bEnabled)
	{
		if (!IsLiveEventObject(Component))
			return;

		const uint8 Enabled = bEnabled ? 1 : 0;
		if (bEnabled)
		{
			InvokeSingleInput(
				Component,
				Component->GetFunction("SetIsReplicated"),
				&Enabled,
				sizeof(Enabled),
				CastClassBoolProperty);
		}
		InvokeSingleInput(
			Component,
			Component->GetFunction("SetComponentTickEnabled"),
			&Enabled,
			sizeof(Enabled),
			CastClassBoolProperty);
		if (bEnabled)
		{
			InvokeSingleInput(
				Component,
				Component->GetFunction("Activate"),
				&Enabled,
				sizeof(Enabled),
				CastClassBoolProperty);
		}
		else
		{
			InvokeZeroed(
				Component,
				Component->GetFunction("Deactivate"));
		}
	}

	bool ResolveKiwiComponentClasses(
		const UClass*& OutControllerComponentClass,
		const UClass*& OutPawnComponentClass)
	{
		OutControllerComponentClass = nullptr;
		OutPawnComponentClass = nullptr;

		auto Mutator = FindLiveActor(
			L"/Script/KiwiPlaylistRuntime.FortAthenaMutator_Kiwi");
		if (!IsLiveEventObject(Mutator))
		{
			Mutator = FindLiveActor(
				L"/Script/KiwiRuntime.FortAthenaMutator_Kiwi");
		}
		if (!IsLiveEventObject(Mutator))
		{
			Mutator = FindLiveActor(
				L"/Script/FortniteGame.FortAthenaMutator_Kiwi");
		}
		if (IsLiveEventObject(Mutator))
		{
			ReadEventProperty(
				Mutator,
				"KiwiControllerComponentClass",
				CastClassObjectProperty,
				OutControllerComponentClass);
			ReadEventProperty(
				Mutator,
				"KiwiPawnComponentClass",
				CastClassObjectProperty,
				OutPawnComponentClass);
		}

		if (!IsLiveEventObject(OutControllerComponentClass))
		{
			constexpr const wchar_t* ControllerClassPaths[] =
			{
				L"/Script/KiwiPlaylistRuntime.KiwiControllerComponent",
				L"/Script/KiwiRuntime.KiwiControllerComponent",
				L"/Script/FortniteGame.KiwiControllerComponent",
			};
			for (const auto ClassPath : ControllerClassPaths)
			{
				OutControllerComponentClass =
					FindObject<UClass>(ClassPath);
				if (IsLiveEventObject(OutControllerComponentClass))
					break;
			}
		}
		if (!IsLiveEventObject(OutPawnComponentClass))
		{
			constexpr const wchar_t* PawnClassPaths[] =
			{
				L"/Script/KiwiPlaylistRuntime.KiwiPawnComponent",
				L"/Script/KiwiRuntime.KiwiPawnComponent",
				L"/Script/FortniteGame.KiwiPawnComponent",
			};
			for (const auto ClassPath : PawnClassPaths)
			{
				OutPawnComponentClass = FindObject<UClass>(ClassPath);
				if (IsLiveEventObject(OutPawnComponentClass))
					break;
			}
		}

		return IsLiveEventObject(OutControllerComponentClass) &&
			IsLiveEventObject(OutPawnComponentClass);
	}

	bool EnsureKiwiPlayerComponents(
		AFortPlayerControllerAthena* PlayerController,
		AFortPlayerPawnAthena* Pawn,
		const UClass* ControllerComponentClass,
		const UClass* PawnComponentClass)
	{
		if (!IsLiveEventObject(PlayerController) ||
			!IsLiveEventObject(Pawn) ||
			!IsLiveEventObject(ControllerComponentClass) ||
			!IsLiveEventObject(PawnComponentClass))
		{
			return false;
		}

		auto ControllerComponent = EnsureRiftTourComponent(
			PlayerController, ControllerComponentClass);
		auto PawnComponent = EnsureRiftTourComponent(
			Pawn, PawnComponentClass);
		if (!IsLiveEventObject(ControllerComponent) ||
			!IsLiveEventObject(PawnComponent))
		{
			return false;
		}

		SetRiftTourComponentEnabled(ControllerComponent, true);
		SetRiftTourComponentEnabled(PawnComponent, true);
		return true;
	}

	bool TryGetKiwiPrisonStartTag(
		AActor* SquadStart,
		FGameplayTag& OutBlockTag)
	{
		OutBlockTag = {};
		FGameplayTagContainer Tags{};
		if (!ReadEventProperty(
				SquadStart,
				"GameplayTags",
				CastClassStructProperty,
				Tags) ||
			!IsSaneArray(
				Tags.GameplayTags.Data,
				Tags.GameplayTags.Num(),
				Tags.GameplayTags.Max(),
				FGameplayTag::Size(),
				64))
		{
			return false;
		}

		bool bIsPrisonStart = false;
		for (int32 TagIndex = 0;
			 TagIndex < Tags.GameplayTags.Num(); ++TagIndex)
		{
			const auto& Tag = Tags.GameplayTags.Get(
				TagIndex, FGameplayTag::Size());
			const std::string TagName =
				Tag.TagName.ToString().c_str();
			if (TagName == "Kiwi.Prison.Start")
			{
				bIsPrisonStart = true;
			}
			else if (TagName.rfind("Kiwi.Prison.Block.", 0) == 0)
			{
				OutBlockTag = Tag;
			}
		}
		return bIsPrisonStart &&
			OutBlockTag.TagName.ToString().rfind(
				"Kiwi.Prison.Block.", 0) == 0;
	}

	bool CollectKiwiPrisonStarts(
		const UClass* SquadStartClass,
		std::vector<AActor*>& OutStarts)
	{
		OutStarts.clear();
		if (!IsLiveEventObject(SquadStartClass))
			return false;

		TArray<AActor*> Starts;
		Utils::GetAll(SquadStartClass, Starts);
		if (IsSaneArray(
			Starts.Data, Starts.Num(), Starts.Max(),
			sizeof(AActor*), 1024))
		{
			for (int32 StartIndex = 0;
				 StartIndex < Starts.Num(); ++StartIndex)
			{
				auto Start = Starts[StartIndex];
				FGameplayTag BlockTag{};
				if (IsLiveEventObject(Start) &&
					Start->IsA(SquadStartClass) &&
					TryGetKiwiPrisonStartTag(Start, BlockTag))
				{
					OutStarts.push_back(Start);
				}
			}
		}
		Starts.Free();
		std::sort(
			OutStarts.begin(), OutStarts.end(),
			[](const AActor* Left, const AActor* Right)
			{
				return Left->Name.ToString() < Right->Name.ToString();
			});
		return !OutStarts.empty();
	}

	bool EnsureKiwiPrisonStart(
		AFortPlayerControllerAthena* PlayerController,
		AFortPlayerPawnAthena* Pawn,
		const UClass* ControllerComponentClass,
		int32 PlayerIndex,
		bool bPlaceInChamber)
	{
		if (!IsLiveEventObject(PlayerController) ||
			!IsLiveEventObject(Pawn) ||
			!IsLiveEventObject(ControllerComponentClass))
		{
			return false;
		}

		auto ControllerComponent = FindRiftTourComponent(
			PlayerController, ControllerComponentClass);
		auto SquadStartClass = FindObject<UClass>(
			L"/Script/FortniteGame.FortSquadStart");
		if (!IsLiveEventObject(ControllerComponent) ||
			!IsLiveEventObject(SquadStartClass))
		{
			return false;
		}

		UObject* ExistingStart = nullptr;
		ReadEventProperty(
			ControllerComponent,
			"PrisonTeleportSquadStart",
			CastClassObjectProperty,
			ExistingStart);
		AActor* SelectedStart =
			IsLiveEventObject(ExistingStart) &&
			ExistingStart->IsA(SquadStartClass)
				? static_cast<AActor*>(ExistingStart)
				: nullptr;

		bool bAssignedStart = false;
		if (!SelectedStart)
		{
			std::vector<AActor*> PrisonStarts;
			if (!CollectKiwiPrisonStarts(
					SquadStartClass, PrisonStarts))
			{
				return false;
			}
			const size_t StartIndex =
				static_cast<size_t>(PlayerIndex >= 0 ? PlayerIndex : 0) %
				PrisonStarts.size();
			SelectedStart = PrisonStarts[StartIndex];
			UObject* SelectedStartObject = SelectedStart;
			bAssignedStart = WriteEventProperty(
				ControllerComponent,
				"PrisonTeleportSquadStart",
				CastClassObjectProperty,
				SelectedStartObject);
			if (!bAssignedStart)
				return false;
		}

		FGameplayTag BlockTag{};
		if (TryGetKiwiPrisonStartTag(SelectedStart, BlockTag))
		{
			WriteEventProperty(
				ControllerComponent,
				"PrisonBlockIdentificationTag",
				CastClassStructProperty,
				BlockTag);
		}

		if (bAssignedStart)
		{
			SDK::DbgLog(
				"[Events] Kiwi authored prison start assigned "
				"controller=%p component=%p start=%s block=%s\n",
				static_cast<void*>(PlayerController),
				static_cast<void*>(ControllerComponent),
				SelectedStart->Name.ToString().c_str(),
				BlockTag.TagName.ToString().c_str());
		}

		if (!bPlaceInChamber)
			return true;

		FEventObjectIdentity PawnIdentity{};
		if (!TryGetEventObjectIdentity(Pawn, PawnIdentity))
			return false;
		if (HasTrackedIdentity(
			GEventState.KiwiPrisonPlayers, PawnIdentity))
		{
			return true;
		}

		FTransform PlayerStartTransform{};
		if (!InvokeObjectInputReturningTransform(
				SelectedStart,
				SelectedStart->GetFunction("GetPlayerStartTransform"),
				PlayerController,
				PlayerStartTransform))
		{
			return false;
		}

		const bool bPlaced = Pawn->K2_TeleportTo(
			PlayerStartTransform.Translation,
			PlayerStartTransform.Rotation.Rotator());
		if (!bPlaced)
			return false;

		InvokeZeroed(Pawn, Pawn->GetFunction("ForceNetUpdate"));
		InvokeZeroed(
			PlayerController,
			PlayerController->GetFunction("ForceNetUpdate"));
		GEventState.KiwiPrisonPlayers.push_back(PawnIdentity);
		SDK::DbgLog(
			"[Events] Kiwi player placed at authored chamber "
			"controller=%p pawn=%p start=%s location="
			"(%.3f, %.3f, %.3f)\n",
			static_cast<void*>(PlayerController),
			static_cast<void*>(Pawn),
			SelectedStart->Name.ToString().c_str(),
			static_cast<double>(PlayerStartTransform.Translation.X),
			static_cast<double>(PlayerStartTransform.Translation.Y),
			static_cast<double>(PlayerStartTransform.Translation.Z));
		return true;
	}

	FGameplayTag MakeKiwiGameplayCueTag(const wchar_t* TagName)
	{
		FGameplayTag CueTag{};
		CueTag.TagName = FName(TagName);
		return CueTag;
	}

	bool InvokeKiwiGameplayCueOperation(
		UAbilitySystemComponent* AbilitySystemComponent,
		const char* FunctionName,
		const FGameplayTag& CueTag,
		const FGameplayEffectContextHandle* EffectContext)
	{
		if (!IsLiveEventObject(AbilitySystemComponent) ||
			!FunctionName)
		{
			return false;
		}

		auto Function =
			AbilitySystemComponent->GetFunction(FunctionName);
		if (!ValidateParameterBuffer(Function))
			return false;

		const auto Parameters = Function->GetParamsNamed();
		uint32 CueTagOffset = InvalidParamOffset;
		uint32 CueTagSize = 0;
		uint32 EffectContextOffset = InvalidParamOffset;
		uint32 EffectContextSize = 0;
		int32 InputCount = 0;
		for (const auto& Parameter : Parameters.NameOffsetMap)
		{
			if (!IsInputParameter(Parameter))
				continue;
			++InputCount;

			if (Parameter.Name.find("GameplayCueTag") !=
					std::string::npos &&
				Parameter.ElementSize <= sizeof(FGameplayTag) &&
				HasParameterPropertyType(
					Function,
					Parameter,
					CastClassStructProperty))
			{
				if (CueTagOffset != InvalidParamOffset)
					return false;
				CueTagOffset = Parameter.Offset;
				CueTagSize = Parameter.ElementSize;
				continue;
			}

			if (EffectContext &&
				Parameter.Name.find("EffectContext") !=
					std::string::npos &&
				Parameter.ElementSize <=
					sizeof(FGameplayEffectContextHandle) &&
				HasParameterPropertyType(
					Function,
					Parameter,
					CastClassStructProperty))
			{
				if (EffectContextOffset != InvalidParamOffset)
					return false;
				EffectContextOffset = Parameter.Offset;
				EffectContextSize = Parameter.ElementSize;
				continue;
			}

			return false;
		}

		const int32 ExpectedInputs = EffectContext ? 2 : 1;
		if (InputCount != ExpectedInputs ||
			CueTagOffset == InvalidParamOffset ||
			CueTagSize == 0 ||
			(EffectContext &&
				(EffectContextOffset == InvalidParamOffset ||
				 EffectContextSize == 0)) ||
			Parameters.Size == 0)
		{
			return false;
		}

		void* Memory = FMemory::Malloc(Parameters.Size);
		if (!Memory)
			return false;
		memset(Memory, 0, Parameters.Size);
		memcpy(
			reinterpret_cast<uint8*>(Memory) + CueTagOffset,
			&CueTag,
			CueTagSize);
		if (EffectContext)
		{
			memcpy(
				reinterpret_cast<uint8*>(Memory) +
					EffectContextOffset,
				EffectContext,
				EffectContextSize);
		}
		AbilitySystemComponent->ProcessEvent(Function, Memory);
		FMemory::Free(Memory);
		return true;
	}

	bool RemoveKiwiInjectedCue(
		UAbilitySystemComponent* AbilitySystemComponent,
		const wchar_t* TagName)
	{
		const bool bRemoved = InvokeKiwiGameplayCueOperation(
			AbilitySystemComponent,
			"RemoveGameplayCue",
			MakeKiwiGameplayCueTag(TagName),
			nullptr);
		if (bRemoved && AbilitySystemComponent->HasOwnerActor() &&
			IsLiveEventObject(AbilitySystemComponent->OwnerActor))
		{
			AbilitySystemComponent->OwnerActor->ForceNetUpdate();
		}
		return bRemoved;
	}

	int32 RemoveKiwiLegacyLowGravityLayers(
		UAbilitySystemComponent* AbilitySystemComponent)
	{
		if (!IsLiveEventObject(AbilitySystemComponent))
			return 0;

		RemoveKiwiInjectedCue(
			AbilitySystemComponent,
			L"GameplayCue.Skyfire.Phase.Arena.LowGrav.Trail");
		RemoveKiwiInjectedCue(
			AbilitySystemComponent,
			L"GameplayCue.Athena.Alpaca.Loop");

		const auto LegacyEffectClass = FindObject<UClass>(
			L"/Game/Abilities/GameplayModifiers/Mutations/Misc/"
			L"GE_GM_SpeedUp_LowGravity."
			L"GE_GM_SpeedUp_LowGravity_C");
		if (!IsLiveEventObject(LegacyEffectClass) ||
			!AbilitySystemComponent->HasActiveGameplayEffects())
		{
			return 0;
		}

		auto& ActiveEffects =
			AbilitySystemComponent->ActiveGameplayEffects;
		if (!ActiveEffects.HasGameplayEffects_Internal())
			return 0;

		std::vector<FActiveGameplayEffectHandle> HandlesToRemove;
		auto& Effects = ActiveEffects.GameplayEffects_Internal;
		const int32 EffectCount = Effects.Num();
		if (EffectCount < 0 || EffectCount >= 100000)
			return 0;

		for (int32 EffectIndex = 0;
			 EffectIndex < EffectCount; ++EffectIndex)
		{
			auto& Effect = Effects.Get(
				EffectIndex, FActiveGameplayEffect::Size());
			if (!Effect.HasSpec() || !Effect.Spec.HasDef())
				continue;
			if (!Effect.Spec.Def ||
				!Effect.Spec.Def->IsA(LegacyEffectClass))
			{
				continue;
			}

			const auto Handle =
				*reinterpret_cast<FActiveGameplayEffectHandle*>(
					reinterpret_cast<uint8*>(&Effect) + 0xc);
			if (Handle.Handle > 0)
				HandlesToRemove.push_back(Handle);
		}

		auto RemoveFunction = AbilitySystemComponent->GetFunction(
			"RemoveActiveGameplayEffect");
		if (!IsLiveEventObject(RemoveFunction))
			return 0;

		int32 RemovedEffects = 0;
		for (const auto& Handle : HandlesToRemove)
		{
			if (AbilitySystemComponent->Call<bool>(
					RemoveFunction, Handle, -1))
			{
				++RemovedEffects;
			}
		}
		return RemovedEffects;
	}

	bool SetKiwiVortexActive(
		FKiwiVortexState& State,
		AFortPlayerPawnAthena* Pawn,
		bool bActive)
	{
		if (State.bActive == bActive)
			return true;

		if (!IsLiveEventObject(Pawn))
		{
			auto PawnObject = ResolveEventObjectIdentity(
				State.PawnIdentity);
			if (IsLiveEventObject(PawnObject) &&
				PawnObject->IsA(AFortPlayerPawnAthena::StaticClass()))
			{
				Pawn = static_cast<AFortPlayerPawnAthena*>(PawnObject);
			}
		}
		if (!IsLiveEventObject(Pawn) ||
			!IsLiveEventObject(Pawn->GetFunction("SetInVortex")))
		{
			if (!bActive)
				State.bActive = false;
			return false;
		}

		if (bActive &&
			IsLiveEventObject(Pawn->CharacterMovement) &&
			Pawn->CharacterMovement->Velocity.Z < 0.f)
		{
			Pawn->CharacterMovement->Velocity.Z = 0.f;
		}
		Pawn->SetInVortex(bActive);
		if (!bActive)
		{
			const bool bStillSkydiving =
				(Pawn->HasbIsSkydiving() && Pawn->bIsSkydiving) ||
				(Pawn->HasbIsSkydivingFromBus() &&
				 Pawn->bIsSkydivingFromBus);
			if (bStillSkydiving)
				InvokeZeroed(
					Pawn, Pawn->GetFunction("EndSkydiving"));
		}
		Pawn->ForceNetUpdate();
		State.bActive = bActive;
		SDK::DbgLog(
			"[Events] Kiwi island vortex skydive %s pawn=%p "
			"surfaceZ=%.1f\n",
			bActive ? "enabled" : "disabled",
			static_cast<void*>(Pawn),
			static_cast<double>(State.IslandSurfaceZ));
		return true;
	}

	bool ApplyKiwiLowGravity(
		AFortPlayerControllerAthena* PlayerController,
		AFortPlayerPawnAthena* Pawn)
	{
		if (!IsLiveEventObject(PlayerController) ||
			!IsLiveEventObject(Pawn) ||
			!IsLiveEventObject(PlayerController->PlayerState) ||
			!IsLiveEventObject(
				PlayerController->PlayerState->AbilitySystemComponent))
		{
			return false;
		}

		auto AbilitySystemComponent =
			PlayerController->PlayerState->AbilitySystemComponent;
		FEventObjectIdentity AbilitySystemIdentity{};
		if (!TryGetEventObjectIdentity(
				AbilitySystemComponent, AbilitySystemIdentity))
		{
			return false;
		}
		const bool bHasGravityEffect = std::find_if(
				GEventState.KiwiLowGravityEffects.begin(),
				GEventState.KiwiLowGravityEffects.end(),
				[&AbilitySystemIdentity](
					const FTrackedGameplayEffect& Effect)
				{
					return Effect.AbilitySystemIdentity ==
						AbilitySystemIdentity;
				}) != GEventState.KiwiLowGravityEffects.end();
		if (bHasGravityEffect)
			return true;

		const int32 RemovedLegacyEffects =
			RemoveKiwiLegacyLowGravityLayers(
				AbilitySystemComponent);

		auto GameplayEffect = FindObject<UClass>(
			L"/MotherGameplay/Items/Alpaca/"
			L"GE_Alpaca_LowGrav_NoJump."
			L"GE_Alpaca_LowGrav_NoJump_C");
		if (!IsLiveEventObject(GameplayEffect))
			return false;

		FGameplayEffectContextHandle EffectContext =
			AbilitySystemComponent->MakeEffectContext();
		EffectContext.Instigator = PlayerController;
		EffectContext.Causer = Pawn;
		EffectContext.AddSourceObject(Pawn);
		const FActiveGameplayEffectHandle Handle =
			AbilitySystemComponent->BP_ApplyGameplayEffectToSelf(
				GameplayEffect, 1.0f, EffectContext);
		if (Handle.Handle <= 0)
			return false;

		GEventState.KiwiLowGravityEffects.push_back(
			{ AbilitySystemIdentity, Handle });
		SDK::DbgLog(
			"[Events] Kiwi island low gravity applied "
			"controller=%p pawn=%p handle=%d effect=%p "
			"removedLegacy=%d\n",
			static_cast<void*>(PlayerController),
			static_cast<void*>(Pawn),
			Handle.Handle,
			static_cast<const void*>(GameplayEffect),
			RemovedLegacyEffects);
		return true;
	}

	void CleanupKiwiLowGravity()
	{
		int32 DisabledVortexes = 0;
		for (auto& VortexState : GEventState.KiwiVortexPlayers)
		{
			if (VortexState.bActive &&
				SetKiwiVortexActive(VortexState, nullptr, false))
				++DisabledVortexes;
		}
		GEventState.KiwiVortexPlayers.clear();

		int32 RemovedEffects = 0;
		for (const auto& TrackedEffect :
			 GEventState.KiwiLowGravityEffects)
		{
			auto AbilitySystemObject = ResolveEventObjectIdentity(
				TrackedEffect.AbilitySystemIdentity);
			if (!IsLiveEventObject(AbilitySystemObject) ||
				TrackedEffect.Handle.Handle <= 0)
			{
				continue;
			}

			auto AbilitySystemComponent =
				static_cast<UAbilitySystemComponent*>(
					AbilitySystemObject);
			RemoveKiwiLegacyLowGravityLayers(
				AbilitySystemComponent);
			auto RemoveFunction = AbilitySystemComponent->GetFunction(
				"RemoveActiveGameplayEffect");
			if (IsLiveEventObject(RemoveFunction) &&
				AbilitySystemComponent->Call<bool>(
					RemoveFunction,
					TrackedEffect.Handle,
					-1))
			{
				++RemovedEffects;
			}
		}
		if (!GEventState.KiwiLowGravityEffects.empty() ||
			DisabledVortexes > 0)
		{
			SDK::DbgLog(
				"[Events] Kiwi island low gravity removed "
				"effects=%d vortexes=%d tracked=%zu\n",
				RemovedEffects,
				DisabledVortexes,
				GEventState.KiwiLowGravityEffects.size());
		}
		GEventState.KiwiLowGravityEffects.clear();
	}

	void DestroyRiftTourComponent(UObject* Component)
	{
		if (!IsLiveEventObject(Component))
			return;

		SetRiftTourComponentEnabled(Component, false);
		auto DestroyFunction =
			Component->GetFunction("K2_DestroyComponent");
		UObject* ComponentObject = Component;
		if (!InvokeSingleInput(
				Component,
				DestroyFunction,
				&ComponentObject,
				sizeof(ComponentObject),
				CastClassObjectProperty))
		{
			InvokeZeroed(Component, DestroyFunction);
		}
	}

	void StopRiftTourSplineMovement(UObject* MovementComponent)
	{
		if (!IsLiveEventObject(MovementComponent))
			return;

		const uint8 IsMoving = 0;
		WriteEventProperty(
			MovementComponent,
			"bIsMovingAlongSpline",
			CastClassBoolProperty,
			IsMoving);
		InvokeSingleInput(
			MovementComponent,
			MovementComponent->GetFunction("SetIsMovingAlongSpline"),
			&IsMoving,
			sizeof(IsMoving),
			CastClassBoolProperty);
		InvokeZeroed(
			MovementComponent,
			MovementComponent->GetFunction(
				"OnRep_bIsMovingAlongSpline"));
		InvokeZeroed(
			MovementComponent,
			MovementComponent->GetFunction(
				"OnRep_IsMovingAlongSpline"));
	}

	AActor* FindRiftTourPaintScript()
	{
		auto Script = FindLiveActor(
			L"/Buffet/Gameplay/Blueprints/WrapWorldPrototype/BP_Buffet_PhaseScripting_Paint.BP_Buffet_PhaseScripting_Paint_C");
		if (!Script)
		{
			Script = FindLiveActor(
				L"/Buffet/Gameplay/Blueprints/BP_Buffet_PhaseScripting_Paint.BP_Buffet_PhaseScripting_Paint_C");
		}
		if (!Script)
		{
			Script = const_cast<AActor*>(FindObject<AActor>(
				L"/Buffet/Levels/Buffet_Part_4.Buffet_Part_4.PersistentLevel.BP_Buffet_PhaseScripting_Paint_4"));
			if (!IsLiveEventObject(Script))
				Script = nullptr;
		}
		return Script;
	}

	bool ConfigureRiftTourSlide(double Now)
	{
		auto Script = FindRiftTourPaintScript();
		AActor* SplineActor = nullptr;
		if (!Script ||
			!ReadEventProperty(
				Script,
				"SplineActor",
				CastClassObjectProperty,
				SplineActor) ||
			!IsLiveEventObject(SplineActor) ||
			!WriteEventProperty(
				Script,
				"PawnLocation",
				CastClassObjectProperty,
				SplineActor))
		{
			return false;
		}

		auto PlayerComponentClass = FindObject<UClass>(
			L"/Buffet/Gameplay/Blueprints/WrapWorldPrototype/BP_Buffet_Paint_PlayerComponent.BP_Buffet_Paint_PlayerComponent_C");
		auto MovementComponentClass = FindObject<UClass>(
			L"/Buffet/Gameplay/Blueprints/WrapWorldPrototype/BP_Buffet_Paint_MovementComponent.BP_Buffet_Paint_MovementComponent_C");
		if (!IsLiveEventObject(PlayerComponentClass) ||
			!IsLiveEventObject(MovementComponentClass) ||
			!IsLiveEventObject(GEventState.GameMode) ||
			!GEventState.GameMode->HasAlivePlayers())
		{
			return false;
		}

		auto& Players = GEventState.GameMode->AlivePlayers;
		if (!IsSaneArray(
			Players.Data, Players.Num(), Players.Max(),
			sizeof(AActor*), 256))
		{
			return false;
		}

		float StartServerWorldTime = static_cast<float>(Now);
		if (IsLiveEventObject(GEventState.GameState))
		{
			InvokeFloatNoInput(
				GEventState.GameState,
				GEventState.GameState->GetFunction(
					"GetServerWorldTimeSeconds"),
				StartServerWorldTime);
		}

		const UClass* ControllerClass =
			AFortPlayerControllerAthena::StaticClass();
		int32 TargetPlayers = 0;
		int32 ConfiguredPlayers = 0;
		for (int32 PlayerIndex = 0;
			 PlayerIndex < Players.Num(); ++PlayerIndex)
		{
			auto PlayerController =
				static_cast<AFortPlayerControllerAthena*>(
					Players[PlayerIndex]);
			if (!IsLiveEventObject(PlayerController) ||
				!ControllerClass ||
				!PlayerController->IsA(ControllerClass) ||
				!IsLiveEventObject(PlayerController->MyFortPawn))
			{
				continue;
			}

			auto Pawn = PlayerController->MyFortPawn;
			FEventObjectIdentity PawnIdentity{};
			if (!TryGetEventObjectIdentity(Pawn, PawnIdentity))
				continue;
			++TargetPlayers;
			if (HasTrackedIdentity(
					GEventState.RiftTourSlidePlayers,
					PawnIdentity))
			{
				auto MovementComponent = FindRiftTourComponent(
					Pawn, MovementComponentClass);
				if (IsLiveEventObject(MovementComponent))
				{
					const uint8 IsMoving = 1;
					SetRiftTourComponentEnabled(
						MovementComponent, true);
					WriteEventProperty(
						MovementComponent,
						"bIsMovingAlongSpline",
						CastClassBoolProperty,
						IsMoving);
					InvokeSingleInput(
						MovementComponent,
						MovementComponent->GetFunction(
							"SetIsMovingAlongSpline"),
						&IsMoving,
						sizeof(IsMoving),
						CastClassBoolProperty);
					++ConfiguredPlayers;
				}
				continue;
			}

			auto PlayerComponent = EnsureRiftTourComponent(
				Pawn, PlayerComponentClass);
			auto MovementComponent = EnsureRiftTourComponent(
				Pawn, MovementComponentClass);
			if (!IsLiveEventObject(PlayerComponent) ||
				!IsLiveEventObject(MovementComponent))
			{
				continue;
			}
			SetRiftTourComponentEnabled(PlayerComponent, true);
			SetRiftTourComponentEnabled(MovementComponent, true);

			UObject* WrapManager = nullptr;
			UObject* PawnObject = Pawn;
			InvokeObjectInputReturningObject(
				Script,
				Script->GetFunction("GetWrapManagerForPlayer"),
				&PawnObject,
				CastClassObjectProperty,
				WrapManager);

			const float TotalSplineTime = 59.793846f;
			const float SplineInterpStrength = 1.3f;
			UObject* SplineObject = SplineActor;
			const uint8 IsMoving = 1;
			const bool bConfigured =
				WriteEventProperty(
					PlayerComponent,
					"OwningPlayerController",
					CastClassObjectProperty,
					PlayerController) &&
				WriteEventProperty(
					MovementComponent,
					"bIsMovingAlongSpline",
					CastClassBoolProperty,
					IsMoving) &&
				WriteEventProperty(
					MovementComponent,
					"ReplicatedTotalSplineTime",
					CastClassFloatProperty,
					TotalSplineTime) &&
				WriteEventProperty(
					MovementComponent,
					"TargetSplineActor",
					CastClassObjectProperty,
					SplineActor) &&
				InvokeSingleInput(
					MovementComponent,
					MovementComponent->GetFunction("SetSplineActor"),
					&SplineObject,
					sizeof(SplineObject),
					CastClassObjectProperty) &&
				WriteEventProperty(
					MovementComponent,
					"ReplicatedSplineInterpStrength",
					CastClassFloatProperty,
					SplineInterpStrength) &&
				WriteEventProperty(
					MovementComponent,
					"StartServerWorldTime",
					CastClassFloatProperty,
					StartServerWorldTime) &&
				InvokeSingleInput(
					MovementComponent,
					MovementComponent->GetFunction(
						"SetIsMovingAlongSpline"),
					&IsMoving,
					sizeof(IsMoving),
					CastClassBoolProperty) &&
				InvokeZeroed(
					MovementComponent,
					MovementComponent->GetFunction(
						"OnRep_TargetSplineActor")) &&
				WriteEventProperty(
					PlayerComponent,
					"MovementComponent",
					CastClassObjectProperty,
					MovementComponent) &&
				InvokeSmallIntegerInput(
					Pawn,
					Pawn->GetFunction("SetStasisMode"),
					3);
			if (!bConfigured)
				continue;
			InvokeZeroed(
				MovementComponent,
				MovementComponent->GetFunction(
					"OnRep_bIsMovingAlongSpline"));
			InvokeZeroed(
				MovementComponent,
				MovementComponent->GetFunction(
					"OnRep_IsMovingAlongSpline"));

			if (IsLiveEventObject(WrapManager))
			{
				WriteEventProperty(
					PlayerComponent,
					"WrapManager",
					CastClassObjectProperty,
					WrapManager);
			}
			InvokeZeroed(Pawn, Pawn->GetFunction("ForceNetUpdate"));
			InvokeZeroed(
				PlayerController,
				PlayerController->GetFunction("ForceNetUpdate"));
			GEventState.RiftTourSlidePlayers.push_back(
				PawnIdentity);
			++ConfiguredPlayers;
			SDK::DbgLog(
				"[Events] Rift Tour slide attached controller=%p "
				"pawn=%p spline=%p playerComponent=%p "
				"movementComponent=%p wrapManager=%p start=%.3f\n",
				static_cast<void*>(PlayerController),
				static_cast<void*>(Pawn),
				static_cast<void*>(SplineActor),
				static_cast<void*>(PlayerComponent),
				static_cast<void*>(MovementComponent),
				static_cast<void*>(WrapManager),
				StartServerWorldTime);
		}
		return TargetPlayers > 0 &&
			ConfiguredPlayers == TargetPlayers;
	}

	AActor* FindRiftTourStarsScript()
	{
		return FindLiveActor(
			L"/Buffet/Gameplay/Blueprints/BP_Buffet_PhaseScripting_Stars.BP_Buffet_PhaseScripting_Stars_C");
	}

	AActor* FindRiftTourStarsGoal()
	{
		return FindLiveActor(
			L"/Buffet/Gameplay/Blueprints/Stars/BP_Buffet_Stars_GoalPlayerPosition.BP_Buffet_Stars_GoalPlayerPosition_C");
	}

	bool PrimeRiftTourStarsGoal(
		AActor* Goal,
		bool* OutTickEnabled = nullptr,
		bool* OutPositionPrimed = nullptr,
		AActor** OutStarsCamera = nullptr)
	{
		if (OutTickEnabled)
			*OutTickEnabled = false;
		if (OutPositionPrimed)
			*OutPositionPrimed = false;
		if (OutStarsCamera)
			*OutStarsCamera = nullptr;
		if (!IsLiveEventObject(Goal))
			return false;

		const uint8 TickEnabled = 1;
		const bool bTickEnabled = InvokeSingleInput(
			Goal,
			Goal->GetFunction("SetActorTickEnabled"),
			&TickEnabled,
			sizeof(TickEnabled),
			CastClassBoolProperty);
		const float DeltaSeconds = 0.f;
		const bool bPositionPrimed = InvokeSingleInput(
			Goal,
			Goal->GetFunction("ReceiveTick"),
			&DeltaSeconds,
			sizeof(DeltaSeconds),
			CastClassFloatProperty);
		AActor* StarsCamera = nullptr;
		ReadEventProperty(
			Goal,
			"StarsCamera",
			CastClassObjectProperty,
			StarsCamera);
		if (OutTickEnabled)
			*OutTickEnabled = bTickEnabled;
		if (OutPositionPrimed)
			*OutPositionPrimed = bPositionPrimed;
		if (OutStarsCamera && IsLiveEventObject(StarsCamera))
			*OutStarsCamera = StarsCamera;
		return bPositionPrimed;
	}

	AActor* EnsureRiftTourStarsGoal(const FVector& InitialLocation)
	{
		auto Goal = FindRiftTourStarsGoal();
		if (IsLiveEventObject(Goal))
		{
			PrimeRiftTourStarsGoal(Goal);
			return Goal;
		}

		auto TrackedGoal = ResolveEventObjectIdentity(
			GEventState.RiftTourStarsFallbackGoal);
		if (IsLiveEventObject(TrackedGoal) &&
			TrackedGoal->IsA(AActor::StaticClass()))
		{
			Goal = static_cast<AActor*>(TrackedGoal);
			PrimeRiftTourStarsGoal(Goal);
			return Goal;
		}
		GEventState.RiftTourStarsFallbackGoal = {};

		auto GoalClass = FindObject<UClass>(
			L"/Buffet/Gameplay/Blueprints/Stars/BP_Buffet_Stars_GoalPlayerPosition.BP_Buffet_Stars_GoalPlayerPosition_C");
		if (!IsLiveEventObject(GoalClass))
			return nullptr;

		Goal = UWorld::SpawnActor<AActor>(
			GoalClass, InitialLocation, {}, GEventState.Script);
		FEventObjectIdentity GoalIdentity{};
		if (!IsLiveEventObject(Goal) ||
			!Goal->IsA(GoalClass) ||
			!TryGetEventObjectIdentity(Goal, GoalIdentity))
		{
			if (IsLiveEventObject(Goal))
				Goal->K2_DestroyActor();
			return nullptr;
		}

		GEventState.RiftTourStarsFallbackGoal = GoalIdentity;
		bool bTickEnabled = false;
		bool bPositionPrimed = false;
		AActor* StarsCamera = nullptr;
		PrimeRiftTourStarsGoal(
			Goal,
			&bTickEnabled,
			&bPositionPrimed,
			&StarsCamera);
		InvokeZeroed(Goal, Goal->GetFunction("ForceNetUpdate"));
		SDK::DbgLog(
			"[Events] Rift Tour stars authored goal fallback spawned "
			"goal=%p camera=%p tick=%d primed=%d\n",
			static_cast<void*>(Goal),
			static_cast<void*>(StarsCamera),
			bTickEnabled ? 1 : 0,
			bPositionPrimed ? 1 : 0);
		return Goal;
	}

	bool HasRiftTourStarsEffect(
		const FEventObjectIdentity& AbilitySystemIdentity)
	{
		return std::find_if(
			GEventState.RiftTourStarsEffects.begin(),
			GEventState.RiftTourStarsEffects.end(),
			[&AbilitySystemIdentity](const FTrackedGameplayEffect& Effect)
			{
				return Effect.AbilitySystemIdentity ==
					AbilitySystemIdentity;
			}) != GEventState.RiftTourStarsEffects.end();
	}

	bool ApplyRiftTourStarsEffect(
		AFortPlayerControllerAthena* PlayerController,
		AFortPlayerPawnAthena* Pawn)
	{
		if (!IsLiveEventObject(PlayerController) ||
			!IsLiveEventObject(Pawn) ||
			!IsLiveEventObject(PlayerController->PlayerState) ||
			!IsLiveEventObject(
				PlayerController->PlayerState->AbilitySystemComponent))
		{
			return false;
		}

		auto AbilitySystemComponent =
			PlayerController->PlayerState->AbilitySystemComponent;
		FEventObjectIdentity AbilitySystemIdentity{};
		if (!TryGetEventObjectIdentity(
				AbilitySystemComponent, AbilitySystemIdentity))
		{
			return false;
		}
		if (HasRiftTourStarsEffect(AbilitySystemIdentity))
			return true;

		auto GameplayEffect = FindObject<UClass>(
			L"/Buffet/Gameplay/Blueprints/Stars/GE_Buffet_Stars_SpaceMovementEffect.GE_Buffet_Stars_SpaceMovementEffect_C");
		if (!IsLiveEventObject(GameplayEffect))
			return false;

		FGameplayEffectContextHandle Context =
			AbilitySystemComponent->MakeEffectContext();
		Context.Instigator = PlayerController;
		Context.Causer = Pawn;
		Context.AddSourceObject(Pawn);
		const FActiveGameplayEffectHandle Handle =
			AbilitySystemComponent->BP_ApplyGameplayEffectToSelf(
				GameplayEffect, 1.0f, Context);
		if (Handle.Handle <= 0)
			return false;

		GEventState.RiftTourStarsEffects.push_back(
			{ AbilitySystemIdentity, Handle });
		SDK::DbgLog(
			"[Events] Rift Tour stars authored pull effect applied "
			"controller=%p pawn=%p asc=%p handle=%d\n",
			static_cast<void*>(PlayerController),
			static_cast<void*>(Pawn),
			static_cast<void*>(AbilitySystemComponent),
			Handle.Handle);
		return true;
	}

	bool EnsureRiftTourStarsPullAbility(
		UAbilitySystemComponent* AbilitySystemComponent)
	{
		auto PullAbilityClass = FindObject<UClass>(
			L"/Buffet/Gameplay/Blueprints/Stars/GA_Buffet_Stars_PullTowardsCenter.GA_Buffet_Stars_PullTowardsCenter_C");
		if (!IsLiveEventObject(AbilitySystemComponent) ||
			!IsLiveEventObject(PullAbilityClass) ||
			!AbilitySystemComponent->HasActivatableAbilities())
		{
			return false;
		}

		auto& AbilitySpecs =
			AbilitySystemComponent->ActivatableAbilities.Items;
		const int32 AbilitySpecSize = FGameplayAbilitySpec::Size();
		if (AbilitySpecSize <= 0 || AbilitySpecSize > 0x400 ||
			!IsSaneArray(
				AbilitySpecs.Data,
				AbilitySpecs.Num(),
				AbilitySpecs.Max(),
				static_cast<size_t>(AbilitySpecSize),
				2048))
		{
			return false;
		}

		FGameplayAbilitySpecHandle PullHandle{};
		for (int32 AbilityIndex = AbilitySpecs.Num() - 1;
			 AbilityIndex >= 0; --AbilityIndex)
		{
			auto& AbilitySpec = AbilitySpecs.Get(
				AbilityIndex, AbilitySpecSize);
			if (!IsLiveEventObject(AbilitySpec.Ability) ||
				!AbilitySpec.Ability->IsA(PullAbilityClass) ||
				AbilitySpec.Handle.Handle <= 0)
			{
				continue;
			}

			if (AbilitySpec.HasActiveCount() &&
				AbilitySpec.ActiveCount > 0)
			{
				return true;
			}
			PullHandle = AbilitySpec.Handle;
			break;
		}
		if (PullHandle.Handle <= 0)
			return false;

		const uint32 PredictionKeySize = FPredictionKey::Size();
		if (PredictionKeySize == 0 || PredictionKeySize > 0x100)
			return false;

		auto PredictionKey = static_cast<FPredictionKey*>(
			FMemory::Malloc(PredictionKeySize));
		if (!PredictionKey)
			return false;
		memset(PredictionKey, 0, PredictionKeySize);
		UAbilitySystemComponent::InternalServerTryActivateAbility(
			AbilitySystemComponent,
			PullHandle,
			true,
			PredictionKey,
			nullptr);
		FMemory::Free(PredictionKey);

		if (!IsSaneArray(
				AbilitySpecs.Data,
				AbilitySpecs.Num(),
				AbilitySpecs.Max(),
				static_cast<size_t>(AbilitySpecSize),
				2048))
		{
			return false;
		}
		for (int32 AbilityIndex = 0;
			 AbilityIndex < AbilitySpecs.Num(); ++AbilityIndex)
		{
			auto& AbilitySpec = AbilitySpecs.Get(
				AbilityIndex, AbilitySpecSize);
			if (AbilitySpec.Handle.Handle != PullHandle.Handle)
				continue;

			const bool bActive = !AbilitySpec.HasActiveCount() ||
				AbilitySpec.ActiveCount > 0;
			if (bActive)
			{
				SDK::DbgLog(
					"[Events] Rift Tour stars authored pull ability "
					"active asc=%p handle=%d\n",
					static_cast<void*>(AbilitySystemComponent),
					PullHandle.Handle);
			}
			return bActive;
		}
		return false;
	}

	bool ConfigureRiftTourStars(double PhaseElapsed)
	{
		constexpr double FloatingCueTime = 669600.0 / 24000.0;
		auto Script = FindRiftTourStarsScript();
		auto StarsComponentClass = FindObject<UClass>(
			L"/Buffet/Gameplay/Blueprints/Stars/BP_Buffet_Stars_PlayerComponent.BP_Buffet_Stars_PlayerComponent_C");
		auto MovementMutatorClass = FindObject<UClass>(
			L"/BuffetPlaylist/Playlist/BuffetCharacterMovementMutatorComponent.BuffetCharacterMovementMutatorComponent_C");
		if (!IsLiveEventObject(Script) ||
			!IsLiveEventObject(StarsComponentClass) ||
			!IsLiveEventObject(MovementMutatorClass) ||
			!IsLiveEventObject(GEventState.GameMode) ||
			!GEventState.GameMode->HasAlivePlayers())
		{
			return false;
		}

		auto& Players = GEventState.GameMode->AlivePlayers;
		if (!IsSaneArray(
			Players.Data, Players.Num(), Players.Max(),
			sizeof(AActor*), 256))
		{
			return false;
		}

		bool bMissingAuthoredComponent = false;
		for (int32 PlayerIndex = 0;
			 PlayerIndex < Players.Num(); ++PlayerIndex)
		{
			auto PlayerController =
				static_cast<AFortPlayerControllerAthena*>(
					Players[PlayerIndex]);
			if (!IsLiveEventObject(PlayerController) ||
				!IsLiveEventObject(PlayerController->MyFortPawn))
			{
				continue;
			}
			if (!FindRiftTourComponent(
					PlayerController->MyFortPawn,
					StarsComponentClass))
			{
				bMissingAuthoredComponent = true;
				break;
			}
		}

		if (bMissingAuthoredComponent &&
			!GEventState.bRiftTourStarsServerActivationInvoked)
		{
			GEventState.bRiftTourStarsServerActivationInvoked =
				InvokeZeroed(
					Script,
					Script->GetFunction("OnPhaseActivation_Server"));
			SDK::DbgLog(
				"[Events] Rift Tour stars authored server activation "
				"repair invoked=%d script=%p\n",
				GEventState.bRiftTourStarsServerActivationInvoked
					? 1 : 0,
				static_cast<void*>(Script));
		}
		if (!GEventState.bRiftTourStarsClientActivationInvoked)
		{
			GEventState.bRiftTourStarsClientActivationInvoked =
				InvokeZeroed(
					Script,
					Script->GetFunction("OnPhaseActivation_Client"));
			SDK::DbgLog(
				"[Events] Rift Tour stars authored client activation "
				"repair invoked=%d script=%p\n",
				GEventState.bRiftTourStarsClientActivationInvoked
					? 1 : 0,
				static_cast<void*>(Script));
		}

		int32 TargetPlayers = 0;
		int32 ConfiguredPlayers = 0;
		int32 AnchoredPlayers = 0;
		int32 EffectPlayers = 0;
		int32 PullPlayers = 0;
		int32 FloatingPlayers = 0;
		FVector GoalSpawnLocation = Script->K2_GetActorLocation();
		for (int32 PlayerIndex = 0;
			 PlayerIndex < Players.Num(); ++PlayerIndex)
		{
			auto PlayerController =
				static_cast<AFortPlayerControllerAthena*>(
					Players[PlayerIndex]);
			if (IsLiveEventObject(PlayerController) &&
				IsLiveEventObject(PlayerController->MyFortPawn))
			{
				GoalSpawnLocation =
					PlayerController->MyFortPawn->K2_GetActorLocation();
				break;
			}
		}
		AActor* Goal = nullptr;
		if (PhaseElapsed >= FloatingCueTime)
			Goal = EnsureRiftTourStarsGoal(GoalSpawnLocation);
		for (int32 PlayerIndex = 0;
			 PlayerIndex < Players.Num(); ++PlayerIndex)
		{
			auto PlayerController =
				static_cast<AFortPlayerControllerAthena*>(
					Players[PlayerIndex]);
			if (!IsLiveEventObject(PlayerController) ||
				!IsLiveEventObject(PlayerController->MyFortPawn))
			{
				continue;
			}

			auto Pawn = PlayerController->MyFortPawn;
			FEventObjectIdentity PawnIdentity{};
			if (!TryGetEventObjectIdentity(Pawn, PawnIdentity))
				continue;
			++TargetPlayers;

			auto StarsComponent = EnsureRiftTourComponent(
				Pawn, StarsComponentClass);
			auto MovementMutator = EnsureRiftTourComponent(
				Pawn, MovementMutatorClass);
			if (!IsLiveEventObject(StarsComponent) ||
				!IsLiveEventObject(MovementMutator))
			{
				continue;
			}
			SetRiftTourComponentEnabled(StarsComponent, true);
			SetRiftTourComponentEnabled(MovementMutator, true);
			InvokeSmallIntegerInput(
				Pawn,
				Pawn->GetFunction("SetStasisMode"),
				3);

			WriteEventProperty(
				StarsComponent,
				"OwningPawn",
				CastClassObjectProperty,
				Pawn);
			InvokeZeroed(
				StarsComponent,
				StarsComponent->GetFunction(
					"TryToInitializeWithPhaseScript"));
			InvokeZeroed(
				StarsComponent,
				StarsComponent->GetFunction(
					"TryFindPlayerLocation"));
			AActor* PlayerLocation = nullptr;
			if (ReadEventProperty(
					StarsComponent,
					"PlayerLocation",
					CastClassObjectProperty,
					PlayerLocation) &&
				IsLiveEventObject(PlayerLocation))
			{
				++AnchoredPlayers;
			}

			InvokeZeroed(Pawn, Pawn->GetFunction("ForceNetUpdate"));
			if (!HasTrackedIdentity(
					GEventState.RiftTourStarsPlayers,
					PawnIdentity))
			{
				GEventState.RiftTourStarsPlayers.push_back(
					PawnIdentity);
				SDK::DbgLog(
					"[Events] Rift Tour stars movement attached "
					"controller=%p pawn=%p stars=%p mutator=%p\n",
					static_cast<void*>(PlayerController),
					static_cast<void*>(Pawn),
					static_cast<void*>(StarsComponent),
					static_cast<void*>(MovementMutator));
			}
			++ConfiguredPlayers;
			if (IsLiveEventObject(Goal) &&
				ApplyRiftTourStarsEffect(PlayerController, Pawn))
			{
				++EffectPlayers;
				auto AbilitySystemComponent =
					PlayerController->PlayerState
						? PlayerController->PlayerState
							->AbilitySystemComponent
						: nullptr;
				if (EnsureRiftTourStarsPullAbility(
						AbilitySystemComponent))
				{
					++PullPlayers;
				}

				if (!HasTrackedIdentity(
						GEventState.RiftTourStarsFloatingPlayers,
						PawnIdentity) &&
					InvokeZeroed(
						StarsComponent,
						StarsComponent->GetFunction(
							"StartFloating")))
				{
					GEventState.RiftTourStarsFloatingPlayers
						.push_back(PawnIdentity);
				}
				if (HasTrackedIdentity(
						GEventState.RiftTourStarsFloatingPlayers,
						PawnIdentity))
				{
					++FloatingPlayers;
				}
			}
		}

		if (TargetPlayers == 0 ||
			ConfiguredPlayers != TargetPlayers ||
			PhaseElapsed < FloatingCueTime)
		{
			return false;
		}

		if (!IsLiveEventObject(Goal))
			return false;

		if (!GEventState.bRiftTourStarsFloatingStarted)
		{
			GEventState.bRiftTourStarsFloatingStarted =
				InvokeZeroed(
					Script,
					Script->GetFunction("StartFloating"));
			SDK::DbgLog(
				"[Events] Rift Tour stars floating cue catch-up "
				"invoked=%d elapsed=%.3f script=%p goal=%p\n",
				GEventState.bRiftTourStarsFloatingStarted ? 1 : 0,
				PhaseElapsed,
				static_cast<void*>(Script),
				static_cast<void*>(Goal));
		}

		return GEventState.bRiftTourStarsClientActivationInvoked &&
			GEventState.bRiftTourStarsFloatingStarted &&
			ConfiguredPlayers == TargetPlayers &&
			AnchoredPlayers == TargetPlayers &&
			EffectPlayers == TargetPlayers &&
			PullPlayers == TargetPlayers &&
			FloatingPlayers == TargetPlayers;
	}

	AActor* FindRiftTourReflectScript()
	{
		const wchar_t* Paths[] =
		{
			L"/Buffet/Levels/Buffet_Reflect.Buffet_Reflect:PersistentLevel.BP_Buffet_PhaseScripting_Reflect_2",
			L"/Buffet/Levels/Buffet_Reflect.Buffet_Reflect.PersistentLevel.BP_Buffet_PhaseScripting_Reflect_2",
		};
		for (const auto Path : Paths)
		{
			auto Script = const_cast<AActor*>(
				FindObject<AActor>(Path));
			if (IsLiveEventObject(Script))
				return Script;
		}
		return nullptr;
	}

	bool ApplyRiftTourBubbleRelevancy()
	{
		auto Script = FindRiftTourReflectScript();
		if (!Script)
			return false;

		const char* FunctionNames[] =
		{
			"Set Bubble Player Relevancy",
			"SetBubblePlayerRelevancy",
		};
		for (const auto FunctionName : FunctionNames)
		{
			auto Function = Script->GetFunction(FunctionName);
			if (!IsLiveEventObject(Function))
				continue;
			if (!InvokeZeroed(Script, Function))
				return false;

			InvokeZeroed(Script, Script->GetFunction("ForceNetUpdate"));
			SDK::DbgLog(
				"[Events] Rift Tour bubble relevancy applied "
				"through authored Reflect script=%p function=%s\n",
				static_cast<void*>(Script),
				FunctionName);
			return true;
		}
		return false;
	}

	bool ConfigureRiftTourBubbles()
	{
		auto BubbleMovementClass = FindObject<UClass>(
			L"/Buffet/Gameplay/Blueprints/Bubble/BP_BubblePlayerMovementComponent.BP_BubblePlayerMovementComponent_C");
		if (!IsLiveEventObject(BubbleMovementClass) ||
			!IsLiveEventObject(GEventState.GameMode) ||
			!GEventState.GameMode->HasAlivePlayers())
		{
			return false;
		}

		auto& Players = GEventState.GameMode->AlivePlayers;
		if (!IsSaneArray(
			Players.Data, Players.Num(), Players.Max(),
			sizeof(AActor*), 256))
		{
			return false;
		}

		int32 TargetPlayers = 0;
		int32 ConfiguredPlayers = 0;
		for (int32 PlayerIndex = 0;
			 PlayerIndex < Players.Num(); ++PlayerIndex)
		{
			auto PlayerController =
				static_cast<AFortPlayerControllerAthena*>(
					Players[PlayerIndex]);
			if (!IsLiveEventObject(PlayerController) ||
				!IsLiveEventObject(PlayerController->MyFortPawn))
			{
				continue;
			}

			auto Pawn = PlayerController->MyFortPawn;
			FEventObjectIdentity PawnIdentity{};
			if (!TryGetEventObjectIdentity(Pawn, PawnIdentity))
				continue;
			++TargetPlayers;

			auto BubbleMovement = FindRiftTourComponent(
				Pawn, BubbleMovementClass);

			UObject* AuthoredSpline = nullptr;
			if (!IsLiveEventObject(BubbleMovement) ||
				!ReadEventProperty(
					BubbleMovement,
					"SplineComponent",
					CastClassObjectProperty,
					AuthoredSpline) ||
				!IsLiveEventObject(AuthoredSpline))
			{
				continue;
			}

			const bool bWasTracked = HasTrackedIdentity(
				GEventState.RiftTourBubblePlayers,
				PawnIdentity);
			if (!bWasTracked)
			{
				float StartServerWorldTime = 0.f;
				float TotalSplineTime = 0.f;
				uint8 IsMoving = 0;
				ReadEventProperty(
					BubbleMovement,
					"StartServerWorldTime",
					CastClassFloatProperty,
					StartServerWorldTime);
				ReadEventProperty(
					BubbleMovement,
					"ReplicatedTotalSplineTime",
					CastClassFloatProperty,
					TotalSplineTime);
				ReadEventProperty(
					BubbleMovement,
					"bIsMovingAlongSpline",
					CastClassBoolProperty,
					IsMoving);
				GEventState.RiftTourBubblePlayers.push_back(
					PawnIdentity);
				SDK::DbgLog(
					"[Events] Rift Tour authored bubble movement observed "
					"controller=%p pawn=%p movement=%p spline=%p "
					"start=%.3f duration=%.3f moving=%d\n",
					static_cast<void*>(PlayerController),
					static_cast<void*>(Pawn),
					static_cast<void*>(BubbleMovement),
					static_cast<void*>(AuthoredSpline),
					StartServerWorldTime,
					TotalSplineTime,
					IsMoving ? 1 : 0);
			}
			++ConfiguredPlayers;
		}
		return TargetPlayers > 0 &&
			ConfiguredPlayers == TargetPlayers;
	}

	AActor* FindRiftTourEscherScript()
	{
		return FindLiveActor(
			L"/Buffet/Gameplay/Blueprints/BP_Buffet_PhaseScripting_Escher.BP_Buffet_PhaseScripting_Escher_C");
	}

	bool IsNearRiftTourEscherStart(
		const FVector& PawnLocation,
		const std::vector<AActor*>& StartReferences)
	{
		constexpr double MaximumDistanceSquared = 300.0 * 300.0;
		for (auto StartReference : StartReferences)
		{
			if (!IsLiveEventObject(StartReference))
				continue;
			if ((PawnLocation -
				StartReference->K2_GetActorLocation()).SizeSquared() <=
				MaximumDistanceSquared)
			{
				return true;
			}
		}
		return false;
	}

	bool ConfigureRiftTourEscher(double PhaseElapsed)
	{
		constexpr double AuthoredTeleportDelay = 0.75;
		if (PhaseElapsed < AuthoredTeleportDelay + 0.25 ||
			!IsLiveEventObject(GEventState.GameMode) ||
			!GEventState.GameMode->HasAlivePlayers())
		{
			return false;
		}

		auto Script = FindRiftTourEscherScript();
		if (!IsLiveEventObject(Script))
			return false;

		TArray<AActor*> AuthoredReferences{};
		if (!ReadEventProperty(
				Script,
				"StartingTeleportRefs",
				0,
				AuthoredReferences) ||
			!IsSaneArray(
				AuthoredReferences.Data,
				AuthoredReferences.Num(),
				AuthoredReferences.Max(),
				sizeof(AActor*),
				64))
		{
			return false;
		}

		std::vector<AActor*> StartReferences;
		FVector Center{};
		FRotator Rotation{};
		for (int32 Index = 0;
			 Index < AuthoredReferences.Num(); ++Index)
		{
			auto StartReference = AuthoredReferences[Index];
			if (!IsLiveEventObject(StartReference))
				continue;
			const FVector Location =
				StartReference->K2_GetActorLocation();
			if (!std::isfinite(static_cast<double>(Location.X)) ||
				!std::isfinite(static_cast<double>(Location.Y)) ||
				!std::isfinite(static_cast<double>(Location.Z)))
			{
				continue;
			}
			if (StartReferences.empty())
				Rotation = StartReference->K2_GetActorRotation();
			Center.X += Location.X;
			Center.Y += Location.Y;
			Center.Z += Location.Z;
			StartReferences.push_back(StartReference);
		}
		if (StartReferences.empty())
			return false;

		const auto ReferenceCount =
			static_cast<double>(StartReferences.size());
		Center.X /= ReferenceCount;
		Center.Y /= ReferenceCount;
		Center.Z /= ReferenceCount;

		auto& Players = GEventState.GameMode->AlivePlayers;
		if (!IsSaneArray(
			Players.Data, Players.Num(), Players.Max(),
			sizeof(AActor*), 256))
		{
			return false;
		}

		int32 TargetPlayers = 0;
		int32 PlacedPlayers = 0;
		for (int32 PlayerIndex = 0;
			 PlayerIndex < Players.Num(); ++PlayerIndex)
		{
			auto PlayerController =
				static_cast<AFortPlayerControllerAthena*>(
					Players[PlayerIndex]);
			if (!IsLiveEventObject(PlayerController) ||
				!IsLiveEventObject(PlayerController->MyFortPawn))
			{
				continue;
			}
			++TargetPlayers;
			if (IsNearRiftTourEscherStart(
					PlayerController->MyFortPawn->K2_GetActorLocation(),
					StartReferences))
			{
				++PlacedPlayers;
			}
		}

		if (TargetPlayers == 0)
			return false;
		if (PlacedPlayers == TargetPlayers)
			return true;

		if (!GEventState.bRiftTourEscherTeleportInvoked)
		{
			GEventState.bRiftTourEscherTeleportInvoked =
				InvokeZeroed(
					Script,
					Script->GetFunction("Teleport Players"));
			SDK::DbgLog(
				"[Events] Rift Tour Escher authored teleport "
				"catch-up invoked=%d script=%p refs=%d\n",
				GEventState.bRiftTourEscherTeleportInvoked ? 1 : 0,
				static_cast<void*>(Script),
				static_cast<int32>(StartReferences.size()));
		}

		PlacedPlayers = 0;
		int32 ValidPlayerIndex = 0;
		for (int32 PlayerIndex = 0;
			 PlayerIndex < Players.Num(); ++PlayerIndex)
		{
			auto PlayerController =
				static_cast<AFortPlayerControllerAthena*>(
					Players[PlayerIndex]);
			if (!IsLiveEventObject(PlayerController) ||
				!IsLiveEventObject(PlayerController->MyFortPawn))
			{
				continue;
			}

			auto Pawn = PlayerController->MyFortPawn;
			if (!IsNearRiftTourEscherStart(
					Pawn->K2_GetActorLocation(), StartReferences))
			{
				FVector Destination = Center;
				FRotator DestinationRotation = Rotation;
				if (TargetPlayers > 1)
				{
					auto StartReference = StartReferences[
						static_cast<size_t>(ValidPlayerIndex) %
						StartReferences.size()];
					Destination =
						StartReference->K2_GetActorLocation();
					DestinationRotation =
						StartReference->K2_GetActorRotation();
				}
				const bool bTeleported = Pawn->K2_TeleportTo(
					Destination, DestinationRotation);
				InvokeZeroed(
					Pawn, Pawn->GetFunction("ForceNetUpdate"));
				InvokeZeroed(
					PlayerController,
					PlayerController->GetFunction("ForceNetUpdate"));
				SDK::DbgLog(
					"[Events] Rift Tour Escher live-reference "
					"fallback controller=%p pawn=%p success=%d "
					"destination=(%.3f, %.3f, %.3f)\n",
					static_cast<void*>(PlayerController),
					static_cast<void*>(Pawn),
					bTeleported ? 1 : 0,
					static_cast<double>(Destination.X),
					static_cast<double>(Destination.Y),
					static_cast<double>(Destination.Z));
			}
			if (IsNearRiftTourEscherStart(
					Pawn->K2_GetActorLocation(), StartReferences))
			{
				++PlacedPlayers;
			}
			++ValidPlayerIndex;
		}

		return PlacedPlayers == TargetPlayers;
	}

	void CleanupRiftTourSlide()
	{
		auto PlayerComponentClass = FindObject<UClass>(
			L"/Buffet/Gameplay/Blueprints/WrapWorldPrototype/BP_Buffet_Paint_PlayerComponent.BP_Buffet_Paint_PlayerComponent_C");
		auto MovementComponentClass = FindObject<UClass>(
			L"/Buffet/Gameplay/Blueprints/WrapWorldPrototype/BP_Buffet_Paint_MovementComponent.BP_Buffet_Paint_MovementComponent_C");
		int32 CleanedPlayers = 0;
		for (const auto& PawnIdentity :
			 GEventState.RiftTourSlidePlayers)
		{
			auto PawnObject = ResolveEventObjectIdentity(PawnIdentity);
			if (!IsLiveEventObject(PawnObject) ||
				!PawnObject->IsA(AActor::StaticClass()))
			{
				continue;
			}

			auto Pawn = static_cast<AActor*>(PawnObject);
			auto PlayerComponent = FindRiftTourComponent(
				Pawn, PlayerComponentClass);
			auto MovementComponent = FindRiftTourComponent(
				Pawn, MovementComponentClass);
			InvokeSmallIntegerInput(
				Pawn,
				Pawn->GetFunction("SetStasisMode"),
				0);
			StopRiftTourSplineMovement(MovementComponent);
			DestroyRiftTourComponent(PlayerComponent);
			DestroyRiftTourComponent(MovementComponent);
			InvokeZeroed(Pawn, Pawn->GetFunction("ForceNetUpdate"));
			++CleanedPlayers;
		}

		SDK::DbgLog(
			"[Events] Rift Tour slide components removed players=%d\n",
			CleanedPlayers);
		GEventState.RiftTourSlidePlayers.clear();
		GEventState.bRiftTourSlideRepairComplete = false;
	}

	void CleanupRiftTourStars()
	{
		int32 RemovedEffects = 0;
		for (const auto& TrackedEffect :
			 GEventState.RiftTourStarsEffects)
		{
			auto AbilitySystemObject = ResolveEventObjectIdentity(
				TrackedEffect.AbilitySystemIdentity);
			if (!IsLiveEventObject(AbilitySystemObject) ||
				TrackedEffect.Handle.Handle <= 0)
			{
				continue;
			}

			auto AbilitySystemComponent =
				static_cast<UAbilitySystemComponent*>(
					AbilitySystemObject);
			auto RemoveFunction = AbilitySystemComponent->GetFunction(
				"RemoveActiveGameplayEffect");
			if (IsLiveEventObject(RemoveFunction) &&
				AbilitySystemComponent->Call<bool>(
					RemoveFunction,
					TrackedEffect.Handle,
					-1))
			{
				++RemovedEffects;
			}
		}
		GEventState.RiftTourStarsEffects.clear();

		auto StarsComponentClass = FindObject<UClass>(
			L"/Buffet/Gameplay/Blueprints/Stars/BP_Buffet_Stars_PlayerComponent.BP_Buffet_Stars_PlayerComponent_C");
		int32 CleanedPlayers = 0;
		for (const auto& PawnIdentity :
			 GEventState.RiftTourStarsPlayers)
		{
			auto PawnObject = ResolveEventObjectIdentity(PawnIdentity);
			if (!IsLiveEventObject(PawnObject) ||
				!PawnObject->IsA(AActor::StaticClass()))
			{
				continue;
			}

			auto Pawn = static_cast<AFortPlayerPawnAthena*>(PawnObject);
			InvokeSmallIntegerInput(
				Pawn,
				Pawn->GetFunction("SetStasisMode"),
				0);
			DestroyRiftTourComponent(FindRiftTourComponent(
				Pawn, StarsComponentClass));
			InvokeZeroed(Pawn, Pawn->GetFunction("ForceNetUpdate"));
			++CleanedPlayers;
		}

		SDK::DbgLog(
			"[Events] Rift Tour stars component removed; shared movement "
			"preserved players=%d effects=%d\n",
			CleanedPlayers,
			RemovedEffects);
		GEventState.RiftTourStarsPlayers.clear();
		GEventState.RiftTourStarsFloatingPlayers.clear();
		bool bDestroyedFallbackGoal = false;
		auto FallbackGoalObject = ResolveEventObjectIdentity(
			GEventState.RiftTourStarsFallbackGoal);
		if (IsLiveEventObject(FallbackGoalObject) &&
			FallbackGoalObject->IsA(AActor::StaticClass()))
		{
			static_cast<AActor*>(FallbackGoalObject)->K2_DestroyActor();
			bDestroyedFallbackGoal = true;
		}
		GEventState.RiftTourStarsFallbackGoal = {};
		GEventState.bRiftTourStarsRepairComplete = false;
		GEventState.bRiftTourStarsServerActivationInvoked = false;
		GEventState.bRiftTourStarsClientActivationInvoked = false;
		GEventState.bRiftTourStarsFloatingStarted = false;
		SDK::DbgLog(
			"[Events] Rift Tour stars fallback goal removed=%d\n",
			bDestroyedFallbackGoal ? 1 : 0);
	}

	void CleanupRiftTourBubbles()
	{
		SDK::DbgLog(
			"[Events] Rift Tour released authored bubble tracking players=%d\n",
			static_cast<int32>(
				GEventState.RiftTourBubblePlayers.size()));
		GEventState.RiftTourBubblePlayers.clear();
		GEventState.bRiftTourBubbleRepairComplete = false;
	}

	void TickRiftTourPhaseRepair(double Now, int32 CurrentPhase)
	{
		if (!UsesRiftTour1730Compatibility(GEventState.Event))
			return;

		if (CurrentPhase > 2 &&
			!GEventState.RiftTourSlidePlayers.empty())
		{
			CleanupRiftTourSlide();
		}
		if (CurrentPhase > 6 &&
			(!GEventState.RiftTourStarsPlayers.empty() ||
			 !GEventState.RiftTourStarsEffects.empty() ||
			 GEventState.RiftTourStarsFallbackGoal.ObjectIndex >= 0))
		{
			CleanupRiftTourStars();
		}
		if (CurrentPhase > 8 &&
			!GEventState.RiftTourBubblePlayers.empty())
		{
			CleanupRiftTourBubbles();
		}

		const double PhaseElapsed =
			Now - GEventState.RiftTourPhaseEnteredTime;
		if (CurrentPhase == 7 &&
			!GEventState.bRiftTourBubbleRelevancyApplied &&
			PhaseElapsed >= 26.0)
		{
			GEventState.bRiftTourBubbleRelevancyApplied =
				ApplyRiftTourBubbleRelevancy();
		}
		if ((CurrentPhase != 2 &&
			 CurrentPhase != 6 &&
			 CurrentPhase != 8 &&
			 CurrentPhase != 10) ||
			Now < GEventState.NextRiftTourRepairTime)
		{
			return;
		}
		if ((CurrentPhase == 2 &&
				 GEventState.bRiftTourSlideRepairComplete) ||
			(CurrentPhase == 6 &&
				 GEventState.bRiftTourStarsRepairComplete) ||
			(CurrentPhase == 8 &&
				 GEventState.bRiftTourBubbleRepairComplete) ||
			(CurrentPhase == 10 &&
				 GEventState.bRiftTourEscherRepairComplete))
		{
			return;
		}

		bool* RepairComplete = nullptr;
		const char* PhaseName = nullptr;
		bool bComplete = false;
		if (CurrentPhase == 2)
		{
			RepairComplete =
				&GEventState.bRiftTourSlideRepairComplete;
			PhaseName = "slide";
			bComplete = ConfigureRiftTourSlide(Now);
		}
		else if (CurrentPhase == 6)
		{
			RepairComplete =
				&GEventState.bRiftTourStarsRepairComplete;
			PhaseName = "stars";
			bComplete = ConfigureRiftTourStars(PhaseElapsed);
		}
		else if (CurrentPhase == 8)
		{
			RepairComplete =
				&GEventState.bRiftTourBubbleRepairComplete;
			PhaseName = "bubbles";
			bComplete = ConfigureRiftTourBubbles();
		}
		else
		{
			RepairComplete =
				&GEventState.bRiftTourEscherRepairComplete;
			PhaseName = "Escher";
			bComplete = ConfigureRiftTourEscher(PhaseElapsed);
		}

		GEventState.NextRiftTourRepairTime = Now +
			(*RepairComplete ? 1.0 : 0.25);
		if (bComplete && !*RepairComplete)
		{
			*RepairComplete = true;
			SDK::DbgLog(
				"[Events] Rift Tour phase %d %s repair complete\n",
				CurrentPhase,
				PhaseName);
		}
		else if (!bComplete &&
			Now >= GEventState.NextRiftTourRepairLogTime)
		{
			GEventState.NextRiftTourRepairLogTime = Now + 2.0;
			SDK::DbgLog(
				"[Events] Rift Tour phase %d waiting for authored "
				"%s actors and pawn components\n",
				CurrentPhase,
				PhaseName);
		}
	}

	FResolvedEventActors ResolveEventActors(const FEvent& Event)
	{
		FResolvedEventActors Result{};
		Result.Loader = FindLiveActor(Event.LoaderClass);
		Result.Script = FindLiveActor(Event.ScriptingClass);
		return Result;
	}

	UObject* GetFunctionTarget(
		const FEventFunction& EventFunction,
		const FResolvedEventActors& Actors)
	{
		return EventFunction.bIsLoaderFunction
			? static_cast<UObject*>(Actors.Loader)
			: static_cast<UObject*>(Actors.Script);
	}

	bool ResolveEventContext(FEventContext& OutContext)
	{
		OutContext = {};
		auto World = UWorld::GetWorld();
		if (!IsLiveEventObject(World) ||
			!World->HasAuthorityGameMode())
		{
			return false;
		}

		auto GameMode =
			static_cast<AFortGameMode*>(World->AuthorityGameMode);
		if (!IsLiveEventObject(GameMode))
			return false;

		AFortGameStateAthena* GameState = nullptr;
		if (GameMode->HasGameState())
			GameState = GameMode->GameState;
		if (!IsLiveEventObject(GameState) && World->HasGameState())
			GameState = static_cast<AFortGameStateAthena*>(World->GameState);
		if (!IsLiveEventObject(GameState))
			return false;

		const UFortPlaylistAthena* Playlist = nullptr;
		const UStruct* PlaylistInfoStruct =
			FPlaylistPropertyArray::StaticStruct();
		if (GameState->HasCurrentPlaylistInfo() &&
			PlaylistInfoStruct &&
			FPlaylistPropertyArray::HasBasePlaylist())
		{
			Playlist = GameState->CurrentPlaylistInfo.BasePlaylist;
		}
		if (!IsLiveEventObject(Playlist) &&
			GameState->HasCurrentPlaylistData())
		{
			Playlist = GameState->CurrentPlaylistData;
		}
		if (!IsLiveEventObject(Playlist))
		{
			// Some event dumps (notably 18.40) omit both readable
			// published-playlist slots. Use the configured asset instead of
			// manufacturing a pointer from a missing offset.
			Playlist = FindObject<UFortPlaylistAthena>(
				FConfiguration::Playlist);
		}
		if (!IsLiveEventObject(Playlist))
			Playlist = nullptr;

		OutContext.World = World;
		OutContext.GameMode = GameMode;
		OutContext.GameState = GameState;
		OutContext.Playlist = Playlist;
		return true;
	}

	void ResetEventState(const FEventContext& Context)
	{
		GEventState = {};
		GEventState.World = Context.World;
		GEventState.GameMode = Context.GameMode;
		GEventState.GameState = Context.GameState;
		GEventState.ObservedPhaseIndex = UnobservedPhase;
		if (GPreparedLoaderWorld != Context.World ||
			GPreparedLoaderGameMode != Context.GameMode ||
			GPreparedLoaderGameState != Context.GameState)
		{
			GPreparedLoaderWorld = nullptr;
			GPreparedLoaderGameMode = nullptr;
			GPreparedLoaderGameState = nullptr;
			GPreparedLoaderVersion = 0.0;
			GPreparedLoaderReadyTime = 0.0;
			GPreparedLoaderFallbackTime = 0.0;
		}
		GActiveEventWorld.store(Context.World, std::memory_order_release);
		FConfiguration::bEventStarted.store(
			false, std::memory_order_release);
	}

	void LogWaiting(double Now, const char* Reason)
	{
		if (Now < GEventState.NextStatusLogTime)
			return;
		printf("[Events] Waiting: %s\n", Reason);
		GEventState.NextStatusLogTime = Now + 2.0;
	}

	bool PlaylistMatchesEvent(
		const FEvent& Event, const FEventContext& Context)
	{
		if (!Event.PlaylistPath)
			return true;
		auto Expected =
			FindObject<UFortPlaylistAthena>(Event.PlaylistPath);
		if (IsLiveEventObject(Expected) &&
			IsLiveEventObject(Context.Playlist) &&
			(Expected == Context.Playlist ||
			 Expected->Name == Context.Playlist->Name))
		{
			return true;
		}

		const bool bConfiguredLegacyPlaylist =
			Event.EventVersion <= 14.60 &&
			FConfiguration::Playlist &&
			wcscmp(FConfiguration::Playlist, Event.PlaylistPath) == 0;
		if (bConfiguredLegacyPlaylist)
		{
			SDK::DbgLog(
				"[Events] using configured legacy playlist fallback "
				"for version %.2f\n",
				Event.EventVersion);
			return true;
		}

		return false;
	}

	bool CollectConnectedEventPlayers(
		UWorld* World,
		AFortGameMode* GameMode,
		std::vector<AFortPlayerControllerAthena*>& OutPlayers)
	{
		OutPlayers.clear();
		const UClass* ControllerClass =
			AFortPlayerControllerAthena::StaticClass();
		if (!IsLiveEventObject(ControllerClass))
			return false;

		auto AddPlayer =
			[&](AFortPlayerControllerAthena* PlayerController)
			{
				if (!IsLiveEventObject(PlayerController) ||
					!PlayerController->IsA(ControllerClass) ||
					std::find(
						OutPlayers.begin(), OutPlayers.end(),
						PlayerController) != OutPlayers.end())
				{
					return;
				}
				OutPlayers.push_back(PlayerController);
			};

		auto AddConnection =
			[&](UNetConnection* Connection)
			{
				if (!IsLiveEventObject(Connection))
					return;

				AddPlayer(Connection->PlayerController);
				if (!Connection->HasChildren())
					return;

				auto& Children = Connection->Children;
				if (!IsSaneArray(
						Children.Data, Children.Num(), Children.Max(),
						sizeof(UNetConnection*), 256))
				{
					return;
				}
				for (int32 ChildIndex = 0;
					 ChildIndex < Children.Num(); ++ChildIndex)
				{
					auto ChildConnection = Children[ChildIndex];
					if (IsLiveEventObject(ChildConnection))
						AddPlayer(ChildConnection->PlayerController);
				}
			};

		if (IsLiveEventObject(World) &&
			World->HasNetDriver() &&
			IsLiveEventObject(World->NetDriver))
		{
			auto Driver = static_cast<UNetDriver*>(World->NetDriver);
			if (Driver->HasClientConnections())
			{
				auto& Connections = Driver->ClientConnections;
				if (IsSaneArray(
						Connections.Data,
						Connections.Num(),
						Connections.Max(),
						sizeof(UNetConnection*),
						256))
				{
					for (int32 ConnectionIndex = 0;
						 ConnectionIndex < Connections.Num();
						 ++ConnectionIndex)
					{
						AddConnection(Connections[ConnectionIndex]);
					}
				}
			}
		}

		if (IsLiveEventObject(GameMode) &&
			GameMode->HasAlivePlayers())
		{
			auto& AlivePlayers = GameMode->AlivePlayers;
			if (IsSaneArray(
					AlivePlayers.Data,
					AlivePlayers.Num(),
					AlivePlayers.Max(),
					sizeof(AActor*),
					256))
			{
				for (int32 PlayerIndex = 0;
					 PlayerIndex < AlivePlayers.Num(); ++PlayerIndex)
				{
					AddPlayer(static_cast<
						AFortPlayerControllerAthena*>(
							AlivePlayers[PlayerIndex]));
				}
			}
		}

		return !OutPlayers.empty();
	}

	float ResolveKiwiIslandSurfaceZ(
		AFortPlayerPawnAthena* FallbackPawn)
	{
		if (FPlatformMath::IsFinite(GEventState.KiwiIslandSurfaceZ) &&
			fabs(GEventState.KiwiIslandSurfaceZ) > 1.f)
		{
			return GEventState.KiwiIslandSurfaceZ;
		}

		auto KiwiLevelScript = FindLiveActor(
			L"/Kiwi/Levels/Kiwi_P.Kiwi_P_C");
		FTransform FarmTransform{};
		if (IsLiveEventObject(KiwiLevelScript) &&
			ReadEventProperty(
				KiwiLevelScript,
				"FarmPOITransform",
				CastClassStructProperty,
				FarmTransform) &&
			FPlatformMath::IsFinite(FarmTransform.Translation.Z) &&
			fabs(FarmTransform.Translation.Z) > 1.f)
		{
			GEventState.KiwiIslandSurfaceZ =
				FarmTransform.Translation.Z;
			return GEventState.KiwiIslandSurfaceZ;
		}

		return IsLiveEventObject(FallbackPawn)
			? FallbackPawn->K2_GetActorLocation().Z
			: 0.f;
	}

	bool TickKiwiIslandVortex(
		const FEventContext& Context, double Now)
	{
		const FEvent* Event = UsesKiwi1750Compatibility(
			GEventState.Event)
			? GEventState.Event
			: FindCurrentEvent();
		if (!UsesKiwi1750Compatibility(Event) ||
			Now < GEventState.NextKiwiVortexUpdateTime)
		{
			return false;
		}
		GEventState.NextKiwiVortexUpdateTime = Now + 0.1;

		if (!PlaylistMatchesEvent(*Event, Context) ||
			!IsLiveEventObject(Context.GameMode))
		{
			return false;
		}

		int32 KiwiPhase = GEventState.AuthoritativePhaseIndex;
		if (KiwiPhase == UnobservedPhase)
			KiwiPhase = GEventState.ObservedPhaseIndex;
		if (KiwiPhase != UnobservedPhase && KiwiPhase >= 1)
		{
			for (auto& State : GEventState.KiwiVortexPlayers)
				if (State.bActive)
					SetKiwiVortexActive(State, nullptr, false);
			return false;
		}

		std::vector<AFortPlayerControllerAthena*> Players;
		if (!CollectConnectedEventPlayers(
				Context.World, Context.GameMode, Players))
		{
			return false;
		}

		std::vector<FEventObjectIdentity> CurrentPawns;
		bool bAnyActive = false;
		for (auto PlayerController : Players)
		{
			if (!IsLiveEventObject(PlayerController) ||
				!IsLiveEventObject(PlayerController->MyFortPawn))
			{
				continue;
			}

			auto Pawn = PlayerController->MyFortPawn;
			FEventObjectIdentity PawnIdentity{};
			if (!TryGetEventObjectIdentity(Pawn, PawnIdentity))
				continue;
			CurrentPawns.push_back(PawnIdentity);

			auto State = std::find_if(
				GEventState.KiwiVortexPlayers.begin(),
				GEventState.KiwiVortexPlayers.end(),
				[&PawnIdentity](const FKiwiVortexState& Candidate)
				{
					return Candidate.PawnIdentity == PawnIdentity;
				});
			if (State == GEventState.KiwiVortexPlayers.end())
			{
				FKiwiVortexState NewState{};
				NewState.PawnIdentity = PawnIdentity;
				NewState.IslandSurfaceZ =
					ResolveKiwiIslandSurfaceZ(Pawn);
				GEventState.KiwiVortexPlayers.push_back(NewState);
				State = GEventState.KiwiVortexPlayers.end() - 1;
			}

			const FVector Location = Pawn->K2_GetActorLocation();
			if (!FPlatformMath::IsFinite(State->IslandSurfaceZ) ||
				fabs(State->IslandSurfaceZ) <= 1.f)
			{
				State->IslandSurfaceZ = Location.Z;
			}
			const bool bGrounded =
				IsLiveEventObject(Pawn->CharacterMovement) &&
				Pawn->CharacterMovement->IsMovingOnGround();
			if (!State->bActive && bGrounded &&
				FPlatformMath::IsFinite(Location.Z))
			{
				State->IslandSurfaceZ = Location.Z;
				State->LastGroundedLocation = FVector(
					Location.X, Location.Y, Location.Z);
				State->bHasGroundedLocation = true;
			}
			const bool bFalling =
				IsLiveEventObject(Pawn->CharacterMovement) &&
				Pawn->CharacterMovement->IsFalling();
			const bool bDescending =
				bFalling &&
				Pawn->CharacterMovement->Velocity.Z < -50.f;
			const double GroundDeltaX =
				Location.X - State->LastGroundedLocation.X;
			const double GroundDeltaY =
				Location.Y - State->LastGroundedLocation.Y;
			const bool bOutsideIslandFootprint =
				!State->bHasGroundedLocation ||
				GroundDeltaX * GroundDeltaX +
					GroundDeltaY * GroundDeltaY >=
					250000.0;
			const bool bBelowIsland =
				Location.Z <= State->IslandSurfaceZ - 750.f;
			const bool bReachedLandingHeight =
				State->bActive &&
				IsLiveEventObject(Pawn->CharacterMovement) &&
				Pawn->CharacterMovement->Velocity.Z > 0.f &&
				Location.Z >= State->IslandSurfaceZ - 150.f;
			const bool bLandedOnIsland =
				bGrounded &&
				Location.Z >= State->IslandSurfaceZ - 500.f;

			if (!State->bActive && bBelowIsland &&
				bDescending && bOutsideIslandFootprint)
			{
				SetKiwiVortexActive(*State, Pawn, true);
			}
			else if (State->bActive &&
				(bReachedLandingHeight || bLandedOnIsland))
				SetKiwiVortexActive(*State, Pawn, false);

			bAnyActive = bAnyActive || State->bActive;
		}

		for (auto State = GEventState.KiwiVortexPlayers.begin();
			 State != GEventState.KiwiVortexPlayers.end();)
		{
			if (HasTrackedIdentity(CurrentPawns, State->PawnIdentity))
			{
				++State;
				continue;
			}
			if (State->bActive)
				SetKiwiVortexActive(*State, nullptr, false);
			State = GEventState.KiwiVortexPlayers.erase(State);
		}
		return bAnyActive;
	}

	bool EquipKiwiBackpack(
		AFortPlayerControllerAthena* PlayerController,
		AFortPlayerPawnAthena* Pawn);

	bool TickKiwiPlayerSetup(
		const FEventContext& Context, double Now)
	{
		const FEvent* Event = UsesKiwi1750Compatibility(
			GEventState.Event)
			? GEventState.Event
			: FindCurrentEvent();
		if (!UsesKiwi1750Compatibility(Event) ||
			Now < GEventState.NextKiwiPlayerSetupTime)
		{
			return false;
		}
		GEventState.NextKiwiPlayerSetupTime = Now + 0.5;

		if (!PlaylistMatchesEvent(*Event, Context) ||
			!IsLiveEventObject(Context.GameMode))
		{
			return false;
		}

		std::vector<AFortPlayerControllerAthena*> Players;
		if (!CollectConnectedEventPlayers(
				Context.World, Context.GameMode, Players))
		{
			return false;
		}

		const UClass* ControllerComponentClass = nullptr;
		const UClass* PawnComponentClass = nullptr;
		if (!ResolveKiwiComponentClasses(
				ControllerComponentClass,
				PawnComponentClass))
		{
			if (Now >= GEventState.NextKiwiSetupLogTime)
			{
				GEventState.NextKiwiSetupLogTime = Now + 2.0;
				SDK::DbgLog(
					"[Events] Kiwi player setup waiting for native "
					"component classes controller=%p pawn=%p\n",
					static_cast<const void*>(ControllerComponentClass),
					static_cast<const void*>(PawnComponentClass));
			}
			return false;
		}

		int32 KiwiPhase = GEventState.AuthoritativePhaseIndex;
		if (KiwiPhase == UnobservedPhase)
			KiwiPhase = GEventState.ObservedPhaseIndex;
		const bool bPrisonPhase =
			KiwiPhase != UnobservedPhase && KiwiPhase >= 1;
		if (bPrisonPhase &&
			(!GEventState.KiwiLowGravityEffects.empty() ||
			 !GEventState.KiwiVortexPlayers.empty()))
		{
			CleanupKiwiLowGravity();
		}
		int32 TargetPlayers = 0;
		int32 ConfiguredPlayers = 0;
		for (size_t PlayerIndex = 0;
			 PlayerIndex < Players.size(); ++PlayerIndex)
		{
			auto PlayerController = Players[PlayerIndex];
			if (!IsLiveEventObject(PlayerController) ||
				!IsLiveEventObject(PlayerController->MyFortPawn))
			{
				continue;
			}

			++TargetPlayers;
			auto Pawn = PlayerController->MyFortPawn;
			const bool bHadControllerComponent =
				IsLiveEventObject(FindRiftTourComponent(
					PlayerController,
					ControllerComponentClass));
			const bool bHadPawnComponent =
				IsLiveEventObject(FindRiftTourComponent(
					Pawn, PawnComponentClass));
			if (!EnsureKiwiPlayerComponents(
					PlayerController,
					Pawn,
					ControllerComponentClass,
					PawnComponentClass))
			{
				continue;
			}

			++ConfiguredPlayers;
			EnsureKiwiPrisonStart(
				PlayerController,
				Pawn,
				ControllerComponentClass,
				static_cast<int32>(PlayerIndex),
				bPrisonPhase);
			if (bPrisonPhase)
			{
				EquipKiwiBackpack(PlayerController, Pawn);
			}
			else
			{
				ApplyKiwiLowGravity(PlayerController, Pawn);
			}
			if (!bHadControllerComponent || !bHadPawnComponent)
			{
				InvokeZeroed(
					Pawn, Pawn->GetFunction("ForceNetUpdate"));
				InvokeZeroed(
					PlayerController,
					PlayerController->GetFunction("ForceNetUpdate"));
				SDK::DbgLog(
					"[Events] Kiwi player components restored "
					"controller=%p pawn=%p controllerComponent=%d "
					"pawnComponent=%d\n",
					static_cast<void*>(PlayerController),
					static_cast<void*>(Pawn),
					bHadControllerComponent ? 0 : 1,
					bHadPawnComponent ? 0 : 1);
			}
		}

		return TargetPlayers > 0 &&
			ConfiguredPlayers == TargetPlayers;
	}

	bool EquipKiwiBackpack(
		AFortPlayerControllerAthena* PlayerController,
		AFortPlayerPawnAthena* Pawn)
	{
		if (!IsLiveEventObject(PlayerController) ||
			!IsLiveEventObject(Pawn))
		{
			return false;
		}

		FEventObjectIdentity PawnIdentity{};
		if (!TryGetEventObjectIdentity(Pawn, PawnIdentity))
			return false;
		if (HasTrackedIdentity(
				GEventState.KiwiBackpackPlayers,
				PawnIdentity))
		{
			return true;
		}

		auto Backpack = const_cast<UObject*>(FindObject<UObject>(
			L"/Kiwi/Gameplay/Blueprints/Backpack/"
			L"CP_Backpack_Kiwi.CP_Backpack_Kiwi"));
		if (!IsLiveEventObject(Backpack) ||
			!IsLiveEventObject(Pawn->GetFunction("ServerChoosePart")))
		{
			return false;
		}

		constexpr uint8 BackpackPartType = 3;
		Pawn->ServerChoosePart(BackpackPartType, Backpack);
		InvokeZeroed(
			Pawn,
			Pawn->GetFunction("OnCharacterPartsReinitialized"));
		if (IsLiveEventObject(PlayerController->PlayerState))
		{
			InvokeZeroed(
				PlayerController->PlayerState,
				PlayerController->PlayerState->GetFunction(
					"OnRep_CharacterParts"));
			InvokeZeroed(
				PlayerController->PlayerState,
				PlayerController->PlayerState->GetFunction(
					"ForceNetUpdate"));
		}
		InvokeZeroed(Pawn, Pawn->GetFunction("ForceNetUpdate"));
		InvokeZeroed(
			PlayerController,
			PlayerController->GetFunction("ForceNetUpdate"));
		GEventState.KiwiBackpackPlayers.push_back(PawnIdentity);
		SDK::DbgLog(
			"[Events] Kiwi event backpack equipped "
			"controller=%p pawn=%p part=%p\n",
			static_cast<void*>(PlayerController),
			static_cast<void*>(Pawn),
			static_cast<void*>(Backpack));
		return true;
	}

	void PrepareKiwiPrisonPlayers(bool bPlaceInAuthoredChamber)
	{
		if (!UsesKiwi1750Compatibility(FindCurrentEvent()) ||
			!IsLiveEventObject(GEventState.World) ||
			!IsLiveEventObject(GEventState.GameMode))
		{
			return;
		}

		std::vector<AFortPlayerControllerAthena*> Players;
		if (!CollectConnectedEventPlayers(
				GEventState.World, GEventState.GameMode, Players))
		{
			return;
		}

		const UClass* ControllerComponentClass = nullptr;
		const UClass* PawnComponentClass = nullptr;
		if (!ResolveKiwiComponentClasses(
				ControllerComponentClass,
				PawnComponentClass))
		{
			return;
		}

		CleanupKiwiLowGravity();
		for (size_t PlayerIndex = 0;
			 PlayerIndex < Players.size(); ++PlayerIndex)
		{
			auto PlayerController = Players[PlayerIndex];
			if (!IsLiveEventObject(PlayerController) ||
				!IsLiveEventObject(PlayerController->MyFortPawn))
			{
				continue;
			}

			auto Pawn = PlayerController->MyFortPawn;
			EnsureKiwiPlayerComponents(
				PlayerController,
				Pawn,
				ControllerComponentClass,
				PawnComponentClass);
			EnsureKiwiPrisonStart(
				PlayerController,
				Pawn,
				ControllerComponentClass,
				static_cast<int32>(PlayerIndex),
				bPlaceInAuthoredChamber);
			EquipKiwiBackpack(PlayerController, Pawn);
		}
	}

	bool TryInvokeLoader(const FEvent& Event)
	{
		const auto Actors = ResolveEventActors(Event);
		UObject* LoaderTarget = Actors.Loader
			? static_cast<UObject*>(Actors.Loader)
			: static_cast<UObject*>(Actors.Script);
		auto LoaderFunction = const_cast<UFunction*>(
			FindObject<UFunction>(Event.LoaderFuncPath));
		return IsLiveEventObject(LoaderTarget) &&
			InvokeContentLoader(LoaderTarget, LoaderFunction);
	}

	bool IsContentLoaderPrepared(
		const FEvent& Event, const FEventContext& Context)
	{
		return GPreparedLoaderWorld == Context.World &&
			GPreparedLoaderGameMode == Context.GameMode &&
			GPreparedLoaderGameState == Context.GameState &&
			fabs(GPreparedLoaderVersion - Event.EventVersion) < 0.001;
	}

	bool AreEventClientsStreamingReady(
		const FEventContext& Context, bool& OutHadPredicate)
	{
		OutHadPredicate = false;
		if (!IsLiveEventObject(Context.GameMode) ||
			!Context.GameMode->HasAlivePlayers())
		{
			return false;
		}

		auto& Players = Context.GameMode->AlivePlayers;
		if (!IsSaneArray(
			Players.Data, Players.Num(), Players.Max(),
			sizeof(AActor*), 256))
		{
			return false;
		}
		const UClass* ControllerClass =
			AFortPlayerControllerAthena::StaticClass();
		if (!IsLiveEventObject(ControllerClass))
			return false;
		const uint64 NativeStreamingReadyAddress =
			FindHasStreamingLevelsCompletedLoadingUnLoading();

		int32 CheckedPlayers = 0;
		for (int32 Index = 0; Index < Players.Num(); ++Index)
		{
			auto PlayerController =
				static_cast<AFortPlayerControllerAthena*>(Players[Index]);
			if (!IsLiveEventObject(PlayerController) ||
				!PlayerController->IsA(ControllerClass))
			{
				continue;
			}

			auto StreamingReadyFunction = PlayerController->GetFunction(
				"HasStreamingLevelsCompletedLoadingUnLoading");
			bool bPlayerReady = false;
			bool bInvoked = InvokeBoolNoInput(
				PlayerController,
				StreamingReadyFunction,
				bPlayerReady);
			if (!bInvoked && NativeStreamingReadyAddress)
			{
				auto NativeStreamingReady = reinterpret_cast<
					bool (*)(AFortPlayerControllerAthena*)>(
						NativeStreamingReadyAddress);
				bPlayerReady = NativeStreamingReady(PlayerController);
				bInvoked = true;
			}
			if (!bInvoked)
			{
				continue;
			}
			OutHadPredicate = true;
			++CheckedPlayers;
			if (!bPlayerReady)
				return false;
		}
		return CheckedPlayers > 0;
	}

	bool IsContentLoaderReady(
		const FEventContext& Context, double Now)
	{
		if (Now < GPreparedLoaderReadyTime)
			return false;

		bool bHadStreamingPredicate = false;
		const bool bStreamingReady = AreEventClientsStreamingReady(
			Context, bHadStreamingPredicate);
		if (bHadStreamingPredicate)
			return bStreamingReady;

		// Some legacy builds do not expose the native per-client readiness
		// function as a UFunction. Give their Blueprint stream request a
		// conservative fallback window instead of assuming two seconds was
		// enough for the Cattus level and foundations to become visible.
		return Now >= GPreparedLoaderFallbackTime;
	}

	bool ReadActivePhase(
		ASpecialEventScript* Script, int32& OutPhase)
	{
		if (!IsLiveEventObject(Script) ||
			!Script->HasReplicatedActivePhaseIndex())
		{
			return false;
		}
		auto Address = &Script->ReplicatedActivePhaseIndex;
		if (!SDK::MemReadable(Address, sizeof(*Address)))
			return false;
		OutPhase = *Address;
		return OutPhase >= -1 && OutPhase < 128;
	}

	bool ResolveDataLayerStates(
		int32& OutUnloaded, int32& OutActivated)
	{
		auto RuntimeState = FindEnum("EDataLayerRuntimeState");
		if (!IsLiveEventObject(RuntimeState))
			return false;
		const int64 Unloaded = RuntimeState->GetValue("Unloaded");
		const int64 Activated = RuntimeState->GetValue("Activated");
		if (Unloaded < 0 || Unloaded > 0xff ||
			Activated < 0 || Activated > 0xff)
		{
			return false;
		}
		OutUnloaded = static_cast<int32>(Unloaded);
		OutActivated = static_cast<int32>(Activated);
		return true;
	}

	bool ApplyPhaseDataLayers(
		ASpecialEventScript* Script,
		int32 OldPhaseIndex,
		int32 NewPhaseIndex)
	{
		if (VersionInfo.FortniteVersion < 23.0 ||
			!IsLiveEventObject(Script) ||
			!Script->HasPhaseInfoArray())
		{
			return false;
		}

		const UStruct* PhaseInfoStruct = FPhaseInfo::StaticStruct();
		const UStruct* EntryStruct = FPhaseDataLayerEntry::StaticStruct();
		if (!PhaseInfoStruct || !EntryStruct ||
			!FPhaseInfo::HasDataLayers() ||
			!FPhaseDataLayerEntry::HasDataLayerAsset() ||
			!FPhaseDataLayerEntry::HasbIsRecursive())
		{
			return false;
		}

		const int32 PhaseInfoSize =
			PhaseInfoStruct->GetPropertiesSize();
		const int32 EntrySize = EntryStruct->GetPropertiesSize();
		if (PhaseInfoSize <= 0 || PhaseInfoSize > 0x400 ||
			EntrySize <= 0 || EntrySize > 0x100)
		{
			return false;
		}

		auto& Phases = Script->PhaseInfoArray;
		if (!IsSaneArray(
			Phases.Data, Phases.Num(), Phases.Max(),
			PhaseInfoSize, 128) ||
			!Phases.IsValidIndex(NewPhaseIndex))
		{
			return false;
		}

		auto World = UWorld::GetWorld();
		if (!IsLiveEventObject(World))
			return false;
		auto DataLayerManager = World->GetDataLayerManager();
		if (!IsLiveEventObject(DataLayerManager) ||
			!IsLiveEventObject(
				DataLayerManager->GetFunction(
					"SetDataLayerRuntimeState")))
			return false;

		int32 UnloadedState = 0;
		int32 ActivatedState = 0;
		if (!ResolveDataLayerStates(
			UnloadedState, ActivatedState))
		{
			return false;
		}

		auto ApplyState = [&](int32 PhaseIndex, int32 RuntimeState)
		{
			if (!Phases.IsValidIndex(PhaseIndex))
				return true;
			auto& Phase = Phases.Get(PhaseIndex, PhaseInfoSize);
			auto& Layers = Phase.DataLayers;
			if (!IsSaneArray(
				Layers.Data, Layers.Num(), Layers.Max(),
				EntrySize, 256))
			{
				return false;
			}

			bool bApplied = true;
			for (int32 Index = 0; Index < Layers.Num(); ++Index)
			{
				auto& Entry = Layers.Get(Index, EntrySize);
				auto Asset = Entry.DataLayerAsset;
				if (!IsLiveEventObject(Asset))
				{
					bApplied = false;
					continue;
				}
				if (!DataLayerManager->SetDataLayerRuntimeState(
						Asset, RuntimeState, Entry.bIsRecursive))
				{
					bApplied = false;
				}
			}
			return bApplied;
		};

		bool bApplied = true;
		if (OldPhaseIndex != NewPhaseIndex)
			bApplied = ApplyState(OldPhaseIndex, UnloadedState);
		return ApplyState(NewPhaseIndex, ActivatedState) && bApplied;
	}

	void PrepareBattleRoyalePhase(const FEventContext& Context)
	{
		auto PhaseLogicClass =
			UFortGameStateComponent_BattleRoyaleGamePhaseLogic::
				StaticClass();
		auto PhaseLogic = PhaseLogicClass
			? UFortGameStateComponent_BattleRoyaleGamePhaseLogic::
				Get(Context.World)
			: nullptr;
		if (IsLiveEventObject(PhaseLogic))
		{
			const int32 PhaseOffset =
				PhaseLogic->GetOffset("GamePhase");
			const int32 StepOffset =
				PhaseLogic->GetOffset("GamePhaseStep");
			if (PhaseOffset >= 0 && StepOffset >= 0 &&
				SDK::MemReadable(
					(PBYTE)PhaseLogic + PhaseOffset, sizeof(uint8)) &&
				SDK::MemReadable(
					(PBYTE)PhaseLogic + StepOffset, sizeof(uint8)))
			{
				PhaseLogic->SetGamePhase(EAthenaGamePhase::SafeZones);
				PhaseLogic->SetGamePhaseStep(
					EAthenaGamePhaseStep::StormHolding);
				return;
			}
		}

		if (!IsLiveEventObject(Context.GameState) ||
			!Context.GameState->HasGamePhase() ||
			!Context.GameState->HasGamePhaseStep())
		{
			return;
		}
		Context.GameState->GamePhase =
			static_cast<uint8>(EAthenaGamePhase::SafeZones);
		Context.GameState->GamePhaseStep =
			static_cast<uint8>(EAthenaGamePhaseStep::StormHolding);
		auto OnRep = Context.GameState->GetFunction("OnRep_GamePhase");
		InvokeZeroed(Context.GameState, OnRep);
		InvokeZeroed(
			Context.GameState,
			Context.GameState->GetFunction("ForceNetUpdate"));
	}

	void ShortenValidatedContentDelay(ASpecialEventScript* Script)
	{
		if (!IsLiveEventObject(Script) ||
			!Script->HasDelayAfterConentLoad())
		{
			return;
		}
		auto Delay = &Script->DelayAfterConentLoad;
		if (!SDK::MemReadable(Delay, sizeof(*Delay)))
			return;

		// Event playlist levels have already passed the visible-level gate by
		// this point. Keep the established short transition without writing
		// through a missing reflected property on versions that lack it.
		*Delay = 0.6f;
	}

	bool WasMeshActorInitialized(
		const ASpecialEventScriptMeshActor* MeshActor)
	{
		FEventObjectIdentity Identity{};
		if (!TryGetEventObjectIdentity(MeshActor, Identity))
			return false;
		return HasTrackedIdentity(
			GEventState.InitializedMeshActors, Identity);
	}

	int32 PrepareMeshActors(
		bool bInitializeSingleRoot,
		bool bKeepClientNodeType)
	{
		const UClass* MeshClass =
			ASpecialEventScriptMeshActor::StaticClass();
		if (!IsLiveEventObject(MeshClass))
			return 0;

		TArray<ASpecialEventScriptMeshActor*> MeshActors;
		Utils::GetAll<ASpecialEventScriptMeshActor>(MeshActors);
		if (!IsSaneArray(
			MeshActors.Data, MeshActors.Num(), MeshActors.Max(),
			sizeof(ASpecialEventScriptMeshActor*), 1024))
		{
			MeshActors.Free();
			return 0;
		}

		UObject* MeshNetworkSubsystem =
			const_cast<UObject*>(
				TUObjectArray::FindFirstObject("MeshNetworkSubsystem"));
		uint8* NodeTypeAddress = nullptr;
		uint8 SavedNodeType = 0;
		if (IsLiveEventObject(MeshNetworkSubsystem))
		{
			const int32 NodeTypeOffset =
				MeshNetworkSubsystem->GetOffset("NodeType");
			const int32 ObjectSize =
				MeshNetworkSubsystem->Class->GetPropertiesSize();
			if (NodeTypeOffset >= 0 && NodeTypeOffset < ObjectSize)
			{
				auto Candidate =
					(PBYTE)MeshNetworkSubsystem + NodeTypeOffset;
				if (SDK::MemReadable(Candidate, sizeof(uint8)))
				{
					NodeTypeAddress = Candidate;
					SavedNodeType = *NodeTypeAddress;
					*NodeTypeAddress = 0;
				}
			}
		}

		std::vector<ASpecialEventScriptMeshActor*> RootInitializedActors;
		for (int32 Index = 0; Index < MeshActors.Num(); ++Index)
		{
			auto MeshActor = MeshActors[Index];
			if (!IsLiveEventObject(MeshActor) ||
				WasMeshActorInitialized(MeshActor))
				continue;
			if (InvokeZeroed(
				MeshActor,
				MeshActor->GetFunction("MeshRootStartEvent")))
			{
				RootInitializedActors.push_back(MeshActor);
				if (bInitializeSingleRoot)
					break;
			}
		}

		if (NodeTypeAddress)
			*NodeTypeAddress = 2;

		int32 InitializedActors = 0;
		for (auto MeshActor : RootInitializedActors)
		{
			if (!IsLiveEventObject(MeshActor))
				continue;
			const bool bRootReplicated = InvokeZeroed(
				MeshActor,
				MeshActor->GetFunction("OnRep_RootStartTime"));
			if (!bKeepClientNodeType)
			{
				InvokeZeroed(
					MeshActor,
					MeshActor->GetFunction("FlushNetDormancy"));
				InvokeZeroed(
					MeshActor,
					MeshActor->GetFunction("ForceNetUpdate"));
			}
			if (bRootReplicated)
			{
				FEventObjectIdentity Identity{};
				if (TryGetEventObjectIdentity(MeshActor, Identity))
				{
					GEventState.InitializedMeshActors.push_back(Identity);
					++InitializedActors;
				}
			}
		}
		if (NodeTypeAddress)
		{
			*NodeTypeAddress =
				bKeepClientNodeType && InitializedActors > 0
					? 2
					: SavedNodeType;
		}
		if (InitializedActors > 0)
		{
			printf("[Events] Initialized %d event mesh actor(s).\n",
				InitializedActors);
			SDK::DbgLog(
				"[Events] mesh initialization actors=%d node=%u->%u "
				"single=%d keepClient=%d\n",
				InitializedActors,
				static_cast<unsigned>(SavedNodeType),
				static_cast<unsigned>(
					NodeTypeAddress ? *NodeTypeAddress : SavedNodeType),
				bInitializeSingleRoot ? 1 : 0,
				bKeepClientNodeType ? 1 : 0);
		}
		MeshActors.Free();
		return InitializedActors;
	}

	bool GrantEventActivator(AFortGameMode* GameMode)
	{
		if (VersionInfo.FortniteVersion < 16.0)
			return true;
		if (!IsLiveEventObject(GameMode) ||
			!GameMode->HasAlivePlayers())
		{
			return false;
		}

		auto EventModeActivator = FindObject<UFortItemDefinition>(
			L"/EventMode/Items/WID_EventMode_Activator.WID_EventMode_Activator");
		if (!IsLiveEventObject(EventModeActivator))
			return false;

		auto& Players = GameMode->AlivePlayers;
		if (!IsSaneArray(
			Players.Data, Players.Num(), Players.Max(),
			sizeof(AActor*), 256))
		{
			return false;
		}

		const UClass* ControllerClass =
			AFortPlayerControllerAthena::StaticClass();
		const UClass* InventoryClass = AFortInventory::StaticClass();
		const UStruct* ItemEntryStruct = FFortItemEntry::StaticStruct();
		const UStruct* ItemListStruct = FFortItemList::StaticStruct();
		if (!ControllerClass || !InventoryClass || !ItemEntryStruct ||
			!ItemListStruct || !FFortItemEntry::HasItemDefinition() ||
			!FFortItemList::HasReplicatedEntries())
		{
			return false;
		}

		const int32 EntrySize = ItemEntryStruct->GetPropertiesSize();
		if (EntrySize <= 0 || EntrySize > 0x400)
			return false;

		const bool bEquipActivator =
			UsesRiftTour1730Compatibility(GEventState.Event);
		int32 TargetPlayers = 0;
		int32 GrantedPlayers = 0;
		for (int32 PlayerIndex = 0;
			 PlayerIndex < Players.Num(); ++PlayerIndex)
		{
			auto PlayerController =
				static_cast<AFortPlayerControllerAthena*>(
					Players[PlayerIndex]);
			if (!IsLiveEventObject(PlayerController) ||
				!PlayerController->IsA(ControllerClass))
			{
				continue;
			}
			FEventObjectIdentity PlayerIdentity{};
			if (!TryGetEventObjectIdentity(
					PlayerController, PlayerIdentity))
			{
				continue;
			}
			++TargetPlayers;
			const bool bGrantTracked = HasTrackedIdentity(
				GEventState.GrantedActivatorPlayers,
				PlayerIdentity);
			const bool bEquipTracked = HasTrackedIdentity(
				GEventState.EquippedActivatorPlayers,
				PlayerIdentity);

			const int32 InventoryOffset =
				PlayerController->GetOffset("WorldInventory");
			if (InventoryOffset < 0 ||
				!SDK::MemReadable(
					(PBYTE)PlayerController + InventoryOffset,
					sizeof(AFortInventory*)))
			{
				continue;
			}
			auto Inventory = *reinterpret_cast<AFortInventory**>(
				(PBYTE)PlayerController + InventoryOffset);
			if (!IsLiveEventObject(Inventory) ||
				!Inventory->IsA(InventoryClass) ||
				!Inventory->HasInventory())
			{
				continue;
			}

			auto& Entries = Inventory->Inventory.ReplicatedEntries;
			if (!IsSaneArray(
				Entries.Data, Entries.Num(), Entries.Max(),
				EntrySize, 512))
			{
				continue;
			}

			bool bAlreadyGranted = false;
			FGuid ActivatorGuid{};
			for (int32 EntryIndex = 0;
				 EntryIndex < Entries.Num(); ++EntryIndex)
			{
				auto& Entry = Entries.Get(EntryIndex, EntrySize);
				if (Entry.ItemDefinition == EventModeActivator)
				{
					bAlreadyGranted = true;
					ActivatorGuid = Entry.ItemGuid;
					break;
				}
			}
			if (!bAlreadyGranted)
			{
				if (bEquipActivator)
				{
					FGuid MeleeGuid{};
					bool bFoundMelee = false;
					for (int32 EntryIndex = 0;
						 EntryIndex < Entries.Num(); ++EntryIndex)
					{
						auto& Entry = Entries.Get(
							EntryIndex, EntrySize);
						if (Entry.ItemDefinition &&
							Entry.ItemDefinition != EventModeActivator &&
							Entry.ItemDefinition->Cast<
								UFortWeaponMeleeItemDefinition>())
						{
							MeleeGuid = Entry.ItemGuid;
							bFoundMelee = true;
							break;
						}
					}
					if (bFoundMelee)
					{
						Inventory->Remove(MeleeGuid);
						SDK::DbgLog(
							"[Events] removed Rift Tour melee before "
							"activator controller=%p\n",
							static_cast<void*>(PlayerController));
					}
				}

				auto GrantedItem = Inventory->GiveItem(
					EventModeActivator);
				if (!IsLiveEventObject(GrantedItem))
					continue;
				ActivatorGuid = GrantedItem->ItemEntry.ItemGuid;
			}

			const bool bHasActivatorGuid =
				ActivatorGuid.A || ActivatorGuid.B ||
				ActivatorGuid.C || ActivatorGuid.D;
			if (!bHasActivatorGuid)
				continue;

			if (!bGrantTracked)
			{
				GEventState.GrantedActivatorPlayers.push_back(
					PlayerIdentity);
			}

			if (bEquipActivator &&
				(!bEquipTracked || !bAlreadyGranted))
			{
				if (!IsLiveEventObject(PlayerController->MyFortPawn) ||
					!IsLiveEventObject(PlayerController->GetFunction(
						"ServerExecuteInventoryItem")) ||
					!IsLiveEventObject(PlayerController->GetFunction(
						"ClientEquipItem")))
				{
					continue;
				}
				PlayerController->ServerExecuteInventoryItem(
					ActivatorGuid);
				PlayerController->ClientEquipItem(
					ActivatorGuid, true);
				PlayerController->ForceNetUpdate();
				GEventState.EquippedActivatorPlayers.push_back(
					PlayerIdentity);
				SDK::DbgLog(
					"[Events] equipped Rift Tour activator controller=%p "
					"guid=%08x-%08x-%08x-%08x\n",
					static_cast<void*>(PlayerController),
					static_cast<unsigned>(ActivatorGuid.A),
					static_cast<unsigned>(ActivatorGuid.B),
					static_cast<unsigned>(ActivatorGuid.C),
					static_cast<unsigned>(ActivatorGuid.D));
			}
			++GrantedPlayers;
		}
		return TargetPlayers > 0 && GrantedPlayers == TargetPlayers;
	}

	bool CanInvokeEventFunction(
		UObject* Target,
		UFunction* Function,
		const wchar_t* FunctionPath,
		const FEventContext& Context)
	{
		if (!IsLiveEventObject(Target) ||
			!IsLiveEventObject(Function))
		{
			return false;
		}
		if (wcsstr(FunctionPath, L"OnReady"))
		{
			FOnReadyLayout Layout{};
			return Context.Playlist &&
				ResolveOnReadyLayout(Function, Layout);
		}
		if (wcsstr(FunctionPath, L"StartEventAtIndex"))
		{
			FStartAtIndexLayout Layout{};
			return ResolveStartAtIndexLayout(Function, Layout);
		}
		return CanInvokeWithZeroedScalarInput(Function);
	}

	bool UsesJerky1241LegacyDispatch(const FEvent& Event)
	{
		return fabs(Event.EventVersion - 12.41) < 0.001 &&
			fabs(VersionInfo.FortniteVersion - 12.41) < 0.001;
	}

	bool TryDispatchJerky1241(const FEvent& Event)
	{
		static constexpr const wchar_t* StartFunctionPath =
			L"/CycloneJerky/Gameplay/BP_Jerky_Loader."
			L"BP_Jerky_Loader_C.startevent";

		auto Loader = FindLiveActor(Event.LoaderClass);
		auto StartFunction = const_cast<UFunction*>(
			FindObject<UFunction>(StartFunctionPath));
		if (!IsLiveEventObject(Loader) ||
			!IsLiveEventObject(StartFunction))
		{
			return false;
		}

		Loader->Call<void>(StartFunction, 0.f);
		SDK::DbgLog(
			"[Events] dispatched legacy Jerky start target=%p function=%ls\n",
			static_cast<void*>(Loader), StartFunctionPath);
		return true;
	}

	bool TryDispatchEvent(
		const FEvent& Event, const FEventContext& Context)
	{
		if (UsesJerky1241LegacyDispatch(Event))
			return TryDispatchJerky1241(Event);

		const auto Actors = ResolveEventActors(Event);
		for (const auto& EventFunction : Event.EventFunctions)
		{
			auto Target = GetFunctionTarget(EventFunction, Actors);
			auto Function = const_cast<UFunction*>(
				FindObject<UFunction>(EventFunction.FunctionPath));
			if (!CanInvokeEventFunction(
				Target, Function, EventFunction.FunctionPath, Context))
			{
				return false;
			}
		}

		for (const auto& EventFunction : Event.EventFunctions)
		{
			auto Target = GetFunctionTarget(EventFunction, Actors);
			auto Function = const_cast<UFunction*>(
				FindObject<UFunction>(EventFunction.FunctionPath));
			SDK::DbgLog(
				"[Events] dispatch begin version=%.2f target=%p function=%ls\n",
				Event.EventVersion,
				static_cast<void*>(Target),
				EventFunction.FunctionPath);
			bool bInvoked = false;
			if (wcsstr(EventFunction.FunctionPath, L"OnReady"))
			{
				bInvoked = InvokeOnReady(Target, Function, Context);
			}
			else if (wcsstr(
				EventFunction.FunctionPath, L"StartEventAtIndex"))
			{
				const bool bRiftTour1730 =
					UsesRiftTour1730Compatibility(&Event);
				PrepareBattleRoyalePhase(Context);
				ShortenValidatedContentDelay(
					static_cast<ASpecialEventScript*>(Target));
				const int32 InitializedMeshActors =
					PrepareMeshActors(
						bRiftTour1730,
						bRiftTour1730);
				// Event mesh actors may arrive with later streamed phases. Keep a
				// low-frequency per-actor discovery pass active for the event rather
				// than stopping after the first actor succeeds. Rift Tour is the
				// exception: its one mesh root owns the complete phase graph, so
				// force-starting later actors preempts authored location handoffs.
				GEventState.bMeshInitializationPending = !bRiftTour1730;

				auto Script =
					static_cast<ASpecialEventScript*>(Target);
				GEventState.Script = Script;
				int32 OldPhase = UnobservedPhase;
				ReadActivePhase(Script, OldPhase);
				if (bRiftTour1730)
				{
					bInvoked = InitializedMeshActors > 0;
					if (bInvoked)
					{
						GEventState.ObservedPhaseIndex = OldPhase;
						SDK::DbgLog(
							"[Events] Rift Tour reference mesh sequence completed; "
							"phase=%d StartEventAtIndex=skipped\n",
							OldPhase);
					}
				}
				else
				{
					bInvoked = InvokeStartAtIndex(Target, Function);
				}
				if (bInvoked && !bRiftTour1730)
				{
					int32 NewPhase = UnobservedPhase;
					if (ReadActivePhase(Script, NewPhase))
					{
						// Also activate the current phase when the replicated
						// index was already zero before StartEventAtIndex. Some
						// modern scripts begin at zero but still need their data
						// layers explicitly made visible.
						const bool bLayersApplied = ApplyPhaseDataLayers(
							Script,
							OldPhase == UnobservedPhase
								? -1 : OldPhase,
							NewPhase);
						GEventState.ObservedPhaseIndex =
							bLayersApplied
								? NewPhase
								: UnobservedPhase;
					}
				}
			}
			else
			{
				bInvoked = InvokeZeroed(Target, Function);
			}

			if (!bInvoked)
			{
				SDK::DbgLog(
					"[Events] dispatch failed version=%.2f function=%ls\n",
					Event.EventVersion,
					EventFunction.FunctionPath);
				return false;
			}
			SDK::DbgLog(
				"[Events] dispatch complete version=%.2f function=%ls\n",
				Event.EventVersion,
				EventFunction.FunctionPath);
		}
		return !Event.EventFunctions.empty();
	}

	void TickModernEventPhase(double Now)
	{
		const bool bObserveRiftTour1730 =
			UsesRiftTour1730Compatibility(GEventState.Event);
		const bool bUsesModernDataLayers =
			VersionInfo.FortniteVersion >= 23.0;
		if (!GEventState.Event ||
			(!bObserveRiftTour1730 && !bUsesModernDataLayers) ||
			Now < GEventState.NextPhasePollTime)
		{
			return;
		}
		GEventState.NextPhasePollTime = Now +
			(bUsesModernDataLayers ? 0.05 : 0.25);

		auto Script = GEventState.Script;
		if (!IsLiveEventObject(Script))
		{
			Script = static_cast<ASpecialEventScript*>(
				FindLiveActor(GEventState.Event->ScriptingClass));
			GEventState.Script = Script;
		}
		int32 CurrentPhase = UnobservedPhase;
		if (!ReadActivePhase(Script, CurrentPhase))
			return;

		if (GEventState.ObservedPhaseIndex == UnobservedPhase ||
			GEventState.ObservedPhaseIndex != CurrentPhase)
		{
			if (!bUsesModernDataLayers)
			{
				SDK::DbgLog(
					"[Events] Rift Tour phase changed old=%d new=%d\n",
					GEventState.ObservedPhaseIndex,
					CurrentPhase);
				GEventState.RiftTourPhaseEnteredTime = Now;
				GEventState.NextRiftTourRepairTime = Now;
				GEventState.NextRiftTourRepairLogTime = 0.0;
				if (CurrentPhase == 2)
					GEventState.bRiftTourSlideRepairComplete = false;
				else if (CurrentPhase == 6)
				{
					GEventState.bRiftTourStarsRepairComplete = false;
					GEventState.bRiftTourStarsServerActivationInvoked = false;
					GEventState.bRiftTourStarsClientActivationInvoked = false;
					GEventState.bRiftTourStarsFloatingStarted = false;
				}
				else if (CurrentPhase == 7)
					GEventState.bRiftTourBubbleRelevancyApplied = false;
				else if (CurrentPhase == 8)
					GEventState.bRiftTourBubbleRepairComplete = false;
				else if (CurrentPhase == 10)
				{
					GEventState.bRiftTourEscherTeleportInvoked = false;
					GEventState.bRiftTourEscherRepairComplete = false;
				}
				GEventState.ObservedPhaseIndex = CurrentPhase;
			}
			else
			{
				const bool bLayersApplied = ApplyPhaseDataLayers(
					Script,
					GEventState.ObservedPhaseIndex == UnobservedPhase
						? -1
						: GEventState.ObservedPhaseIndex,
					CurrentPhase);
				if (bLayersApplied)
					GEventState.ObservedPhaseIndex = CurrentPhase;
			}
		}

		if (bObserveRiftTour1730)
			TickRiftTourPhaseRepair(Now, CurrentPhase);
	}

	void TickPendingMeshInitialization(double Now)
	{
		if (!GEventState.bMeshInitializationPending ||
			Now < GEventState.NextMeshPollTime)
		{
			return;
		}
		GEventState.NextMeshPollTime = Now + 1.0;
		PrepareMeshActors(false, false);
	}

	void TickPendingActivatorGrant(double Now)
	{
		if (!GEventState.bActivatorGrantPending ||
			Now < GEventState.NextActivatorPollTime)
		{
			return;
		}
		GEventState.NextActivatorPollTime = Now + 1.0;
		GrantEventActivator(GEventState.GameMode);
	}

	void TickUnvault(double Now)
	{
		if (!GEventState.bUnvaultPending ||
			Now < GEventState.UnvaultDueTime ||
			!GEventState.Event)
		{
			return;
		}

		auto Script = FindLiveActor(GEventState.Event->ScriptingClass);
		auto SetUnvault = const_cast<UFunction*>(FindObject<UFunction>(
			L"/Game/Athena/Prototype/Blueprints/White/BP_SnowScripting.BP_SnowScripting_C.SetUnvaultItemName"));
		auto Pillars = const_cast<UFunction*>(FindObject<UFunction>(
			L"/Game/Athena/Prototype/Blueprints/White/BP_SnowScripting.BP_SnowScripting_C.PillarsConcluded"));
		FName Name(L"DrumGun");

		if (!IsLiveEventObject(Script) ||
			!ValidateParameterBuffer(SetUnvault) ||
			!ValidateParameterBuffer(Pillars))
		{
			GEventState.UnvaultDueTime = Now + 1.0;
			return;
		}

		if (InvokeOptionalName(Script, SetUnvault, Name) &&
			InvokeOptionalName(Script, Pillars, Name))
		{
			GEventState.bUnvaultPending = false;
			printf("[Events] 8.51 unvault conclusion dispatched.\n");
		}
		else
		{
			GEventState.UnvaultDueTime = Now + 1.0;
		}
	}
}

void Events::PrepareEventContent()
{
	FEventContext Context{};
	const FEvent* Event = FindCurrentEvent();
	if (!Event || !Event->LoaderFuncPath ||
		!ResolveEventContext(Context) ||
		IsContentLoaderPrepared(*Event, Context))
	{
		return;
	}

	if (TryInvokeLoader(*Event))
	{
		GPreparedLoaderWorld = Context.World;
		GPreparedLoaderGameMode = Context.GameMode;
		GPreparedLoaderGameState = Context.GameState;
		GPreparedLoaderVersion = Event->EventVersion;
		GPreparedLoaderReadyTime =
			UGameplayStatics::GetTimeSeconds(Context.World) + 2.0;
		GPreparedLoaderFallbackTime =
			GPreparedLoaderReadyTime + 6.0;
		printf("[Events] Prepared event content during match setup.\n");
	}
	else
	{
		printf("[Events] Event content loader is not ready; start will retry it.\n");
	}
}

void Events::StartEvent()
{
	GStartRequestWorld.store(
		GActiveEventWorld.load(std::memory_order_acquire),
		std::memory_order_release);
	if (!GStartRequested.exchange(true, std::memory_order_acq_rel))
		printf("[Events] Start requested; waiting for game-thread readiness.\n");
}

bool Events::RequiresNativeStreamingReadiness()
{
	auto World = UWorld::GetWorld();
	if (!World)
		return false;

	if (GEventState.World == World &&
		(GEventState.Stage == EEventStartStage::LoadContent ||
		 GEventState.Stage == EEventStartStage::WaitForLoader))
	{
		if (GPreparedLoaderWorld != World ||
			GPreparedLoaderFallbackTime <= 0.0)
		{
			return true;
		}
	}

	if (GPreparedLoaderWorld != World)
		return false;

	return UGameplayStatics::GetTimeSeconds(World) <
		GPreparedLoaderFallbackTime;
}

bool Events::IsEventSessionActive()
{
	return GEventState.Event &&
		GEventState.Stage != EEventStartStage::Idle;
}

void Events::Tick()
{
	FEventContext Context{};
	if (!ResolveEventContext(Context))
		return;

	if (GEventState.World != Context.World ||
		GEventState.GameMode != Context.GameMode ||
		GEventState.GameState != Context.GameState)
	{
		ResetEventState(Context);
	}

	const double Now = UGameplayStatics::GetTimeSeconds(Context.World);
	TickKiwiIslandVortex(Context, Now);
	TickKiwiPlayerSetup(Context, Now);
	if (FConfiguration::bAutoStartEvent.load(
			std::memory_order_acquire) &&
		!FConfiguration::bEventStarted.load(
			std::memory_order_acquire) &&
		!GEventState.bAutoRequestIssued &&
		GEventState.Stage == EEventStartStage::Idle)
	{
		const float BaseTime =
			FConfiguration::EventStartBaseTime.load(
				std::memory_order_acquire);
		const float Delay =
			FConfiguration::EventStartTime.load(
				std::memory_order_acquire);
		if (BaseTime > 0.f && Now >= BaseTime + Delay)
		{
			GEventState.bAutoRequestIssued = true;
			GStartRequestWorld.store(
				Context.World, std::memory_order_release);
			GStartRequested.store(true, std::memory_order_release);
			printf("[Events] Auto-start requested at T=%.1f.\n", Now);
		}
	}

	if (GStartRequested.exchange(false, std::memory_order_acq_rel))
	{
		const UWorld* RequestedWorld = GStartRequestWorld.exchange(
			nullptr, std::memory_order_acq_rel);
		// A request can arrive before the first game-thread Tick publishes the
		// active world. Treat that initial null token as the current world; a
		// non-null token from an old seamless-travel world is still discarded.
		if (RequestedWorld && RequestedWorld != Context.World)
		{
			printf("[Events] Discarded a start request from an old world.\n");
		}
		else if (GEventState.Stage == EEventStartStage::Started ||
			FConfiguration::bEventStarted.load(
				std::memory_order_acquire))
		{
			printf("[Events] Event is already started.\n");
		}
		else if (GEventState.Stage != EEventStartStage::Idle)
		{
			printf("[Events] Event start is already pending.\n");
		}
		else
		{
			GEventState.Event = FindCurrentEvent();
			if (!GEventState.Event)
			{
				printf("[Events] Build does not have a configured event.\n");
			}
			else
			{
				const bool bNeedsLoader =
					GEventState.Event->LoaderFuncPath &&
					!IsContentLoaderPrepared(
						*GEventState.Event, Context);
				if (bNeedsLoader)
				{
					GEventState.Stage = EEventStartStage::LoadContent;
				}
				else if (GEventState.Event->LoaderFuncPath)
				{
					GEventState.LoaderReadyTime =
						GPreparedLoaderReadyTime;
					GEventState.Stage =
						EEventStartStage::WaitForLoader;
				}
				else
				{
					GEventState.Stage =
						EEventStartStage::WaitForTargets;
				}
				GEventState.NextAttemptTime = 0.0;
				GEventState.NextStatusLogTime = 0.0;
			}
		}
	}

	if (GEventState.Stage == EEventStartStage::Started)
	{
		TickModernEventPhase(Now);
		TickPendingMeshInitialization(Now);
		TickPendingActivatorGrant(Now);
		TickUnvault(Now);
		return;
	}
	if (!GEventState.Event ||
		GEventState.Stage == EEventStartStage::Idle ||
		Now < GEventState.NextAttemptTime)
	{
		return;
	}

	if (!UsesJerky1241LegacyDispatch(*GEventState.Event) &&
		!PlaylistMatchesEvent(*GEventState.Event, Context))
	{
		LogWaiting(Now, "the configured event playlist");
		GEventState.NextAttemptTime = Now + 0.25;
		return;
	}

	if (GEventState.Stage == EEventStartStage::LoadContent)
	{
		if (!TryInvokeLoader(*GEventState.Event))
		{
			LogWaiting(Now, "the event content loader");
			GEventState.NextAttemptTime = Now + 0.25;
			return;
		}
		GPreparedLoaderWorld = Context.World;
		GPreparedLoaderGameMode = Context.GameMode;
		GPreparedLoaderGameState = Context.GameState;
		GPreparedLoaderVersion =
			GEventState.Event->EventVersion;
		GPreparedLoaderReadyTime = Now + 2.0;
		GPreparedLoaderFallbackTime = Now + 8.0;
		GEventState.LoaderReadyTime =
			GPreparedLoaderReadyTime;
		GEventState.Stage = EEventStartStage::WaitForLoader;
		printf("[Events] Content loader dispatched on the game thread.\n");
		return;
	}

	if (GEventState.Stage == EEventStartStage::WaitForLoader)
	{
		if (!IsContentLoaderReady(Context, Now))
		{
			LogWaiting(Now, "event streaming visibility");
			return;
		}
		GEventState.Stage = EEventStartStage::WaitForTargets;
	}

	if (GEventState.Stage == EEventStartStage::WaitForTargets)
	{
		if (!TryDispatchEvent(*GEventState.Event, Context))
		{
			LogWaiting(
				Now,
				"streamed event actors and reflected function schemas");
			GEventState.NextAttemptTime = Now + 0.25;
			return;
		}

		GEventState.bActivatorGrantPending =
			VersionInfo.FortniteVersion >= 16.0;
		GrantEventActivator(Context.GameMode);
		GEventState.Stage = EEventStartStage::Started;
		FConfiguration::bEventStarted.store(
			true, std::memory_order_release);
		SDK::DbgLog(
			"[Events] started version=%.2f world=%p\n",
			GEventState.Event->EventVersion,
			static_cast<void*>(Context.World));
		if (fabs(GEventState.Event->EventVersion - 8.51) < 0.001)
		{
			GEventState.bUnvaultPending = true;
			GEventState.UnvaultDueTime = Now + 180.0;
		}
		printf("[Events] Started successfully on the game thread.\n");
	}
}

void (*ActivatePhaseAtIndexOG)(
	ASpecialEventScript* Script,
	int IndexToActivate);

void ActivatePhaseAtIndex(
	ASpecialEventScript* Script,
	int IndexToActivate)
{
	if (!IsLiveEventObject(Script))
		return;

	int32 OldPhaseIndex = UnobservedPhase;
	ReadActivePhase(Script, OldPhaseIndex);
	bool bPhaseCommitted = false;
	if (Script->HasReplicatedActivePhaseIndex())
	{
		auto PhaseAddress = &Script->ReplicatedActivePhaseIndex;
		if (SDK::MemReadable(
				PhaseAddress, sizeof(*PhaseAddress)))
		{
			*PhaseAddress = IndexToActivate;
			bPhaseCommitted = true;
		}
	}

	const bool bOnRepInvoked = bPhaseCommitted && InvokeZeroed(
		Script,
		Script->GetFunction("OnRep_ReplicatedActivePhaseIndex"));

	SDK::DbgLog(
		"[Events] Rift Tour authored phase dispatcher old=%d new=%d "
		"committed=%d onRep=%d\n",
		OldPhaseIndex,
		IndexToActivate,
		bPhaseCommitted ? 1 : 0,
		bOnRepInvoked ? 1 : 0);
	if (ActivatePhaseAtIndexOG)
		ActivatePhaseAtIndexOG(Script, IndexToActivate);
}

void (*ActivatePhaseOG)(
	ASpecialEventScript* Script,
	int IndexToActivate,
	float SequenceTimeOffset);

void ActivatePhase(
	ASpecialEventScript* Script,
	int IndexToActivate,
	float SequenceTimeOffset)
{
	if (!IsLiveEventObject(Script))
		return;

	if (GEventState.Script != Script)
	{
		GEventState.Script = Script;
		GEventState.AuthoritativePhaseIndex = UnobservedPhase;
	}

	int32 NativeOldPhaseIndex = UnobservedPhase;
	const bool bHasNativeOldPhase =
		ReadActivePhase(Script, NativeOldPhaseIndex) &&
		NativeOldPhaseIndex >= 0;
	int32 OldPhaseIndex = bHasNativeOldPhase
		? NativeOldPhaseIndex
		: GEventState.AuthoritativePhaseIndex;
	if (OldPhaseIndex == UnobservedPhase)
		OldPhaseIndex = -1;
	const bool bKiwiPrisonTransition =
		UsesKiwi1750Compatibility(FindCurrentEvent()) &&
		IndexToActivate == 1 && OldPhaseIndex != IndexToActivate;
	if (bKiwiPrisonTransition)
		PrepareKiwiPrisonPlayers(false);
	if (VersionInfo.FortniteVersion < 23.0)
	{
		SDK::DbgLog(
			"[Events] legacy phase activation old=%d new=%d offset=%.3f\n",
			OldPhaseIndex,
			IndexToActivate,
			SequenceTimeOffset);
	}
	else
	{
		const bool bLayersApplied = ApplyPhaseDataLayers(
			Script, OldPhaseIndex, IndexToActivate);
		SDK::DbgLog(
			"[Events] modern phase data layers old=%d new=%d applied=%d\n",
			OldPhaseIndex,
			IndexToActivate,
			bLayersApplied ? 1 : 0);
	}

	if (fabs(VersionInfo.FortniteVersion - 27.11) < 0.001 &&
		IndexToActivate == 5 && OldPhaseIndex != IndexToActivate)
	{
		const int32 RecipientCount = SendDurianFinalPhaseCue();
		SDK::DbgLog(
			"[Events] Durian final-phase cue sent recipients=%d\n",
			RecipientCount);
	}

	if (ActivatePhaseOG)
	{
		ActivatePhaseOG(
			Script, IndexToActivate, SequenceTimeOffset);
		if (bKiwiPrisonTransition)
			PrepareKiwiPrisonPlayers(true);
		GEventState.AuthoritativePhaseIndex = IndexToActivate;
		GEventState.ObservedPhaseIndex = IndexToActivate;

		int32 NativePhaseIndex = UnobservedPhase;
		if (ReadActivePhase(Script, NativePhaseIndex))
		{
			if (VersionInfo.FortniteVersion < 23.0 &&
				NativePhaseIndex != IndexToActivate &&
				Script->HasReplicatedActivePhaseIndex())
			{
				auto PhaseAddress =
					&Script->ReplicatedActivePhaseIndex;
				if (SDK::MemReadable(
						PhaseAddress, sizeof(*PhaseAddress)))
				{
					*PhaseAddress = IndexToActivate;
					const bool bOnRepInvoked = InvokeZeroed(
						Script,
						Script->GetFunction(
							"OnRep_ReplicatedActivePhaseIndex"));
					NativePhaseIndex = IndexToActivate;
					SDK::DbgLog(
						"[Events] legacy phase fallback committed "
						"requested=%d onRep=%d\n",
						IndexToActivate,
						bOnRepInvoked ? 1 : 0);
				}
			}
			SDK::DbgLog(
				"[Events] native phase activation completed requested=%d "
				"active=%d tracked=%d\n",
				IndexToActivate,
				NativePhaseIndex,
				GEventState.AuthoritativePhaseIndex);
		}
	}
}

void Events::Hook()
{
	InstallRiftTourMeshPredicateHooks();
	if (fabs(VersionInfo.FortniteVersion - 27.11) < 0.001)
	{
		const auto ActivatePhaseAddress = FindActivatePhase();
		if (ActivatePhaseAddress)
		{
			Utils::Hook(
				ActivatePhaseAddress,
				ActivatePhase,
				ActivatePhaseOG);
			SDK::DbgLog(
				"[Events] Durian ActivatePhase hook installed RVA=0x%llX\n",
				ActivatePhaseAddress - ImageBase);
		}
		else
		{
			SDK::DbgLog(
				"[Events] Durian ActivatePhase hook target was not found\n");
		}
		return;
	}

	if (fabs(VersionInfo.FortniteVersion - 17.30) < 0.001)
	{
		constexpr uint64 ActivatePhaseAtIndexRva = 0x3DE5CE8;
		constexpr uint8 ActivatePhaseAtIndexCode[] =
		{
			0x48, 0x89, 0x5C, 0x24, 0x08,
			0x48, 0x89, 0x74, 0x24, 0x10,
			0x57, 0x48, 0x83, 0xEC, 0x40,
		};
		const uint64 ActivatePhaseAtIndexAddress =
			ImageBase + ActivatePhaseAtIndexRva;
		if (MatchesEventCode(
				ActivatePhaseAtIndexAddress,
				ActivatePhaseAtIndexCode))
		{
			Utils::Hook(
				ActivatePhaseAtIndexAddress,
				ActivatePhaseAtIndex,
				ActivatePhaseAtIndexOG);
			SDK::DbgLog(
				"[Events] Rift Tour authored phase dispatcher hook "
				"installed RVA=0x%llX\n",
				ActivatePhaseAtIndexRva);
			return;
		}
		SDK::DbgLog(
			"[Events] Rift Tour authored phase dispatcher signature "
			"did not match; using legacy outer hook\n");
	}

	if (VersionInfo.FortniteVersion >= 17.0 &&
		VersionInfo.FortniteVersion < 23.0)
	{
		const auto ActivatePhaseAddress = FindActivatePhase();
		if (ActivatePhaseAddress)
		{
			Utils::Hook(
				ActivatePhaseAddress,
				ActivatePhase,
				ActivatePhaseOG);
			SDK::DbgLog(
				"[Events] legacy ActivatePhase hook installed RVA=0x%llX\n",
				ActivatePhaseAddress - ImageBase);
		}
		else
		{
			SDK::DbgLog(
				"[Events] legacy ActivatePhase hook target was not found\n");
		}
		return;
	}

	if (VersionInfo.FortniteVersion >= 23.0)
		printf("[Events] Modern data layers use safe game-thread phase polling.\n");
}
