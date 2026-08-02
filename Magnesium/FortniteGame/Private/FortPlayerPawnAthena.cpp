#include "pch.h"
#include "../Public/FortPlayerPawnAthena.h"
#include "../Public/FortInventory.h"
#include "../Public/FortGameMode.h"
#include "../Public/FortAthenaMutator.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortWeapon.h"
#include "../Public/FortPhysicsPawn.h"
#include "../../Engine/Public/NetDriver.h"
#include "../../Erbium/Public/Configuration.h"
#include "../../Erbium/Public/GUI.h"

#include <array>

struct _Pad_0xC
{
	uint8_t Padding[0xC];
};

struct _Pad_0x18
{
	uint8_t Padding[0x18];
};

namespace
{
	constexpr float HealthStateEpsilon = 0.01f;
	constexpr size_t MaxTrackedHealthStates = 256;
	constexpr size_t MaxMinimumHealthGodStates = 256;

	struct FTrackedHealthState
	{
		TWeakObjectPtr<AFortPlayerPawnAthena> Pawn;
		TWeakObjectPtr<AFortPlayerControllerAthena> Controller;
		ULONGLONG LastForceKillAttemptMs = 0;
		uint8 ConsecutiveUnresolvedZeroFlushes = 0;
		uint8 ForceKillAttempts = 0;
		bool bObservedAlive = false;
		bool bZeroStateLogged = false;
	};

	struct FMinimumHealthGodState
	{
		TWeakObjectPtr<AFortPlayerControllerAthena> Controller;
		TWeakObjectPtr<AFortPlayerPawnAthena> AppliedPawn;
		TWeakObjectPtr<UFortHealthSet> AppliedHealthSet;
		float PreviousMinimum = 0.f;
		bool bCapturedPreviousMinimum = false;
	};

	std::array<FTrackedHealthState, MaxTrackedHealthStates>
		GTrackedHealthStates{};
	std::array<FMinimumHealthGodState, MaxMinimumHealthGodStates>
		GMinimumHealthGodStates{};
	size_t GTrackedHealthStateCursor = 0;
	size_t GMinimumHealthGodStateCursor = 0;
	TWeakObjectPtr<UWorld> GTrackedHealthStateWorld;
	TWeakObjectPtr<UWorld> GMinimumHealthGodStateWorld;
	uint32 GShieldRepairLogCount = 0;
	thread_local uint32 GReviveCompatDepth = 0;
	constexpr uint32
		ServerReviveFromDBNOImplementationSlot15_30 = 487;
	using FServerReviveFromDBNOImplementation15_30 =
		void (*)(
			AFortPlayerPawnAthena*,
			AController*);
	FServerReviveFromDBNOImplementation15_30
		GServerReviveFromDBNOImplementation15_30OG =
			nullptr;

	void ServerReviveFromDBNOImplementation15_30(
		AFortPlayerPawnAthena* Pawn,
		AController* EventInstigator);

	class FScopedReviveCompatCall final
	{
	public:
		FScopedReviveCompatCall()
		{
			++GReviveCompatDepth;
		}

		~FScopedReviveCompatCall()
		{
			--GReviveCompatDepth;
		}

		FScopedReviveCompatCall(
			const FScopedReviveCompatCall&) = delete;
		FScopedReviveCompatCall& operator=(
			const FScopedReviveCompatCall&) = delete;
	};

	void ServerReviveFromDBNOImplementation15_30(
		AFortPlayerPawnAthena* Pawn,
		AController* EventInstigator)
	{
		// 15.30's normal teammate interaction calls the virtual
		// ServerReviveFromDBNO_Implementation directly. It never enters the
		// reflected ExecFunction hook used by script/cheat revives.
		SDK::DbgLog(
			"[Revive] 15.30 native implementation received "
			"pawn=%p instigator=%p slot=%u depth=%u\n",
			(void*)Pawn,
			(void*)EventInstigator,
			ServerReviveFromDBNOImplementationSlot15_30,
			GReviveCompatDepth);
		if (GReviveCompatDepth > 0)
		{
			SDK::DbgLog(
				"[Revive] 15.30 suppressed recursive native "
				"implementation pawn=%p instigator=%p\n",
				(void*)Pawn,
				(void*)EventInstigator);
			return;
		}

		FScopedReviveCompatCall ScopedCompatCall;
		const bool bSucceeded =
			AFortPlayerPawnAthena::ReviveFromDBNOCompat(
				Pawn,
				EventInstigator);
		if (!bSucceeded)
		{
			SDK::DbgLog(
				"[Revive] 15.30 native implementation "
				"transition failed pawn=%p instigator=%p\n",
				(void*)Pawn,
				(void*)EventInstigator);
		}
	}

	bool IsLiveHealthStateObject(const UObject* Object)
	{
		if (!Object || !SDK::MemReadable(Object, 0x40))
			return false;

		const int32 ObjectIndex = Object->Index;
		if (ObjectIndex < 0 || ObjectIndex >= TUObjectArray::Num())
			return false;

		auto Item = TUObjectArray::GetItemByIndex(ObjectIndex);
		const int32 InvalidObjectFlags =
			Offsets::bEncryptedObjects ? 0x10200000 : 0x20;
		return Item &&
			Item->GetObject() == Object &&
			!(Item->GetFlags() & InvalidObjectFlags) &&
			Object->Class &&
			SDK::MemReadable(Object->Class, 0x40);
	}

	struct FPlayerMapIconLinearColor
	{
		float R;
		float G;
		float B;
		float A;
	};

	constexpr float PlayerMapIconViewableDistance = 500000.f;
	bool GWarnedMissingPlayerMapIconClass = false;
	bool GWarnedMissingPlayerMapIconTexture = false;
	bool GWarnedPlayerMapIconCreation = false;
	bool GWarnedPlayerMapIconSetup = false;
	bool GWarnedPlayerMapIconException = false;
	bool GPlayerMapIconSetupDisabled = false;

	UObject* ResolvePlayerMapIconPreview(
		const UObject* Definition,
		const UClass* TextureClass)
	{
		if (!IsLiveHealthStateObject(Definition) || !TextureClass)
			return nullptr;

		constexpr const char* PreviewProperties[] = {
			"LargePreviewImage",
			"SmallPreviewImage"
		};

		for (const char* PropertyName : PreviewProperties)
		{
			const uint32 PropertyOffset =
				Definition->GetOffset(PropertyName);
			if (PropertyOffset == UINT32_MAX ||
				PropertyOffset > 0x10000)
			{
				continue;
			}

			auto SoftTexture = reinterpret_cast<FSoftObjectPtr*>(
				reinterpret_cast<uint8_t*>(
					const_cast<UObject*>(Definition)) +
				PropertyOffset);
			if (!SDK::MemReadable(
					SoftTexture,
					FSoftObjectPtr::Size()))
			{
				continue;
			}

			// A cosmetic preview is optional, so only reuse one that is already
			// resident. InternalGet may synchronously load each bot's soft path
			// during spawn and can stall the game thread on large bot fills.
			auto Texture = const_cast<UObject*>(SoftTexture->Get());
			if (IsLiveHealthStateObject(Texture) &&
				Texture->IsA(TextureClass))
			{
				return Texture;
			}
		}

		return nullptr;
	}

	UObject* ResolvePlayerMapIconTexture(
		AFortPlayerControllerAthena* Controller,
		AFortPlayerPawnAthena* Pawn)
	{
		static const UClass* TextureClass = FindClass("Texture2D");
		if (!TextureClass)
			return nullptr;

		AFortPlayerStateAthena* PlayerState = nullptr;
		if (Pawn && Pawn->HasPlayerState() && Pawn->PlayerState)
		{
			PlayerState =
				Pawn->PlayerState->Cast<AFortPlayerStateAthena>();
		}
		if (!PlayerState && Controller &&
			Controller->HasPlayerState() && Controller->PlayerState)
		{
			PlayerState = Controller->PlayerState;
		}

		if (IsLiveHealthStateObject(PlayerState) &&
			PlayerState->HasHeroType())
		{
			if (auto Texture = ResolvePlayerMapIconPreview(
					PlayerState->HeroType,
					TextureClass))
			{
				return Texture;
			}
		}

		UAthenaCharacterItemDefinition* Character = nullptr;
		if (IsLiveHealthStateObject(Controller))
		{
			if (Controller->HasCosmeticLoadoutPC())
				Character = Controller->CosmeticLoadoutPC.Character;
			if (!Character && Controller->HasCustomizationLoadout())
				Character = Controller->CustomizationLoadout.Character;
		}

		if (IsLiveHealthStateObject(Character))
		{
			if (Character->HasHeroDefinition())
			{
				if (auto Texture = ResolvePlayerMapIconPreview(
						Character->HeroDefinition,
						TextureClass))
				{
					return Texture;
				}
			}

			if (auto Texture = ResolvePlayerMapIconPreview(
					Character,
					TextureClass))
			{
				return Texture;
			}
		}

		static UObject* FallbackTexture = nullptr;
		static bool bFallbackTextureResolved = false;
		if (!bFallbackTextureResolved)
		{
			bFallbackTextureResolved = true;
			FallbackTexture = const_cast<UObject*>(SDK::FindObject(
				L"/Game/UI/Foundation/Textures/Icons/Heroes/Athena/Soldier/"
				L"T-Soldier-HID-001-Athena-Commando-F-L."
				L"T-Soldier-HID-001-Athena-Commando-F-L",
				TextureClass));
		}

		return IsLiveHealthStateObject(FallbackTexture) &&
			FallbackTexture->IsA(TextureClass)
			? FallbackTexture
			: nullptr;
	}

	void ApplyPlayerMapIconPawnBrush(
		AFortPlayerPawnAthena* Pawn,
		UObject* IconTexture)
	{
		if (!Pawn || !IconTexture)
			return;

		static const UStruct* SlateBrushStruct =
			SDK::Offsets::StaticFindObject
				? static_cast<const UStruct*>(
					SDK::StaticFindObject(
						L"/Script/SlateCore.SlateBrush",
						nullptr))
				: nullptr;
		if (!SlateBrushStruct)
			return;

		const uint32 BrushOffset =
			Pawn->GetOffset("MiniMapIconBrush");
		const uint32 ResourceOffset =
			SlateBrushStruct->GetOffset("ResourceObject");
		if (BrushOffset == UINT32_MAX ||
			BrushOffset > 0x10000 ||
			ResourceOffset == UINT32_MAX ||
			ResourceOffset > 0x400)
		{
			return;
		}

		auto ResourceAddress =
			reinterpret_cast<uint8_t*>(Pawn) +
				BrushOffset + ResourceOffset;
		if (!SDK::MemReadable(
				ResourceAddress,
				sizeof(IconTexture)))
		{
			return;
		}

		memcpy(
			ResourceAddress,
			&IconTexture,
			sizeof(IconTexture));
	}

	constexpr size_t PlayerMapIconParameterBufferSize = 0x1000;

	bool IsPlayerMapIconParameterRangeValid(
		uint32 Offset,
		size_t Size)
	{
		return Offset != UINT32_MAX &&
			Offset < PlayerMapIconParameterBufferSize &&
			Size <= PlayerMapIconParameterBufferSize - Offset;
	}

	bool WritePlayerMapIconParameter(
		uint8_t* Parameters,
		UFunction* Function,
		const char* Name,
		const void* Value,
		size_t Size)
	{
		if (!Parameters || !Function || !Name || !Value || !Size)
			return false;

		const uint32 Offset = Function->GetOffset(Name);
		if (!IsPlayerMapIconParameterRangeValid(Offset, Size))
			return false;

		memcpy(Parameters + Offset, Value, Size);
		return true;
	}

	UObject* ReadPlayerMapIconObjectReturn(
		const uint8_t* Parameters,
		UFunction* Function)
	{
		if (!Parameters || !Function)
			return nullptr;

		const uint32 ReturnOffset =
			Function->GetOffset("ReturnValue");
		if (!IsPlayerMapIconParameterRangeValid(
				ReturnOffset,
				sizeof(UObject*)))
		{
			return nullptr;
		}

		UObject* Result = nullptr;
		memcpy(
			&Result,
			Parameters + ReturnOffset,
			sizeof(Result));
		return Result;
	}

	bool InvokePlayerMapIconObjectEventGuarded(
		const UObject* Context,
		UFunction* Function,
		void* Parameters,
		UObject*& Result)
	{
		Result = nullptr;
		bool bSucceeded = false;
		__try
		{
			Context->ProcessEvent(Function, Parameters);
			Result = ReadPlayerMapIconObjectReturn(
				reinterpret_cast<const uint8_t*>(Parameters),
				Function);
			bSucceeded = true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}

		if (!bSucceeded)
		{
			GPlayerMapIconSetupDisabled = true;
			if (!GWarnedPlayerMapIconException)
			{
				GWarnedPlayerMapIconException = true;
				SDK::DbgLog(
					"[PlayerMapIcons] reflected setup faulted and was "
					"disabled for object=%p on FN %.2f\n",
					(void*)Context,
					VersionInfo.FortniteVersion);
			}
		}

		return bSucceeded;
	}

	UObject* InvokeGetPlayerMapIconComponent(
		AFortPlayerPawnAthena* Pawn,
		UFunction* Function,
		const UClass* ComponentClass)
	{
		if (!Pawn || !Function || !ComponentClass)
			return nullptr;

		alignas(16) uint8 Parameters[
			PlayerMapIconParameterBufferSize]{};
		if (!WritePlayerMapIconParameter(
				Parameters,
				Function,
				"ComponentClass",
				&ComponentClass,
				sizeof(ComponentClass)))
		{
			return nullptr;
		}

		Pawn->ProcessEvent(Function, Parameters);
		return ReadPlayerMapIconObjectReturn(
			Parameters,
			Function);
	}

	UObject* FindPlayerMapIconComponent(
		AFortPlayerPawnAthena* Pawn,
		const UClass* ComponentClass)
	{
		if (!IsLiveHealthStateObject(Pawn) || !ComponentClass)
			return nullptr;

		static UFunction* GetComponentFunction =
			Pawn->GetFunction("GetComponentByClass");
		auto Component = InvokeGetPlayerMapIconComponent(
			Pawn,
			GetComponentFunction,
			ComponentClass);
		return IsLiveHealthStateObject(Component) &&
			Component->IsA(ComponentClass)
			? Component
			: nullptr;
	}

	UObject* InvokeAddPlayerMapIconComponent(
		AFortPlayerPawnAthena* Pawn,
		UFunction* Function,
		const UClass* ComponentClass)
	{
		if (!Pawn || !Function || !ComponentClass)
			return nullptr;

		alignas(16) uint8 Parameters[
			PlayerMapIconParameterBufferSize]{};
		const char* ClassParameterName =
			Function->GetProperty("Class")
				? "Class"
				: "ComponentClass";
		bool bManualAttachment = false;
		bool bDeferredFinish = false;
		FTransform RelativeTransform{};
		if (!WritePlayerMapIconParameter(
				Parameters,
				Function,
				ClassParameterName,
				&ComponentClass,
				sizeof(ComponentClass)) ||
			!WritePlayerMapIconParameter(
				Parameters,
				Function,
				"bManualAttachment",
				&bManualAttachment,
				sizeof(bManualAttachment)) ||
			!WritePlayerMapIconParameter(
				Parameters,
				Function,
				"RelativeTransform",
				&RelativeTransform,
				FTransform::Size()) ||
			!WritePlayerMapIconParameter(
				Parameters,
				Function,
				"bDeferredFinish",
				&bDeferredFinish,
				sizeof(bDeferredFinish)))
		{
			return nullptr;
		}

		Pawn->ProcessEvent(Function, Parameters);
		return ReadPlayerMapIconObjectReturn(
			Parameters,
			Function);
	}

	UObject* InvokeSpawnPlayerMapIconComponent(
		UObject* GameplayStaticsDefault,
		UFunction* Function,
		const UClass* ComponentClass,
		AFortPlayerPawnAthena* Pawn)
	{
		if (!GameplayStaticsDefault || !Function ||
			!ComponentClass || !Pawn)
		{
			return nullptr;
		}

		alignas(16) uint8 Parameters[
			PlayerMapIconParameterBufferSize]{};
		UObject* Outer = Pawn;
		if (!WritePlayerMapIconParameter(
				Parameters,
				Function,
				"ObjectClass",
				&ComponentClass,
				sizeof(ComponentClass)) ||
			!WritePlayerMapIconParameter(
				Parameters,
				Function,
				"Outer",
				&Outer,
				sizeof(Outer)))
		{
			return nullptr;
		}

		GameplayStaticsDefault->ProcessEvent(
			Function,
			Parameters);
		return ReadPlayerMapIconObjectReturn(
			Parameters,
			Function);
	}

	struct FPlayerMapIconFunctions
	{
		UFunction* Setup = nullptr;
		UFunction* SetReplicated = nullptr;
		UFunction* Activate = nullptr;
		UFunction* SetViewableDistance = nullptr;
		UFunction* SetVisible = nullptr;
		UFunction* SetVisibleOnMap = nullptr;
		UFunction* SetVisibleOnMiniMap = nullptr;
		UFunction* RepNotify = nullptr;
	};

	const FPlayerMapIconFunctions& GetPlayerMapIconFunctions(
		UObject* Component)
	{
		static const FPlayerMapIconFunctions Functions = [Component]
		{
			FPlayerMapIconFunctions Result{};
			if (!Component)
				return Result;

			Result.Setup = Component->GetFunction("SetupMiniMapComponent");
			Result.SetReplicated = Component->GetFunction("SetIsReplicated");
			Result.Activate = Component->GetFunction("Activate");
			Result.SetViewableDistance =
				Component->GetFunction("SetMiniMapViewableDistance");
			if (!Result.SetViewableDistance)
			{
				Result.SetViewableDistance =
					Component->GetFunction("SetMinimapViewableDistance");
			}
			Result.SetVisible = Component->GetFunction(
				"SetMiniMapIndicatorIsVisible");
			Result.SetVisibleOnMap = Component->GetFunction(
				"SetMiniMapIndicatorIsVisibleOnMap");
			Result.SetVisibleOnMiniMap = Component->GetFunction(
				"SetMiniMapIndicatorIsVisibleOnMiniMap");
			Result.RepNotify = Component->GetFunction("OnRep_MiniMapData");
			return Result;
		}();
		return Functions;
	}

	bool IsPlayerMapIconTemplateArrayValid(
		const TArray<UActorComponent*>* Templates)
	{
		if (!Templates ||
			!SDK::MemReadable(Templates, sizeof(*Templates)))
		{
			return false;
		}

		const int32 Count = Templates->Num();
		const int32 Capacity = Templates->Max();
		constexpr int32 MaximumTemplateCount = 65536;
		if (Count < 0 || Capacity < Count ||
			Capacity > MaximumTemplateCount)
		{
			return false;
		}

		return Count == 0 ||
			(Templates->GetData() &&
				SDK::MemReadable(
					Templates->GetData(),
					static_cast<size_t>(Count) *
						sizeof(UActorComponent*)));
	}

	bool ResolveLegacyPlayerMapIconTemplate(
		AFortPlayerPawnAthena* Pawn,
		const UClass* ComponentClass,
		FName& TemplateName,
		TArray<UActorComponent*>*& ModifiedTemplates,
		int32& AddedTemplateIndex)
	{
		ModifiedTemplates = nullptr;
		AddedTemplateIndex = -1;
		if (!Pawn || !Pawn->Class || !ComponentClass)
			return false;

		const uint32 TemplatesOffset =
			Pawn->Class->GetOffset("ComponentTemplates");
		if (TemplatesOffset == UINT32_MAX ||
			TemplatesOffset > 0x10000)
		{
			return false;
		}

		auto Templates = reinterpret_cast<
			TArray<UActorComponent*>*>(
				reinterpret_cast<uint8_t*>(Pawn->Class) +
				TemplatesOffset);
		if (!IsPlayerMapIconTemplateArrayValid(Templates))
			return false;

		for (int32 Index = 0; Index < Templates->Num(); ++Index)
		{
			auto Candidate = (*Templates)[Index];
			if (IsLiveHealthStateObject(Candidate) &&
				Candidate->IsA(ComponentClass))
			{
				TemplateName = Candidate->Name;
				return TemplateName.IsValid();
			}
		}

		auto ComponentTemplate = ComponentClass->GetDefaultObj();
		if (!IsLiveHealthStateObject(ComponentTemplate) ||
			!ComponentTemplate->IsA(ComponentClass) ||
			!ComponentTemplate->Name.IsValid())
		{
			return false;
		}

		// Legacy AActor::AddComponent looks up a template by name in this
		// reflected array. A component CDO is a valid duplication template;
		// exposing it here lets the engine own, initialize, and register the
		// new component through its normal version-specific implementation.
		auto TypedTemplate =
			static_cast<UActorComponent*>(ComponentTemplate);
		const int32 PreviousCount = Templates->Num();
		auto& AddedTemplate = Templates->Add(TypedTemplate);
		ModifiedTemplates = Templates;
		AddedTemplateIndex = PreviousCount;
		TemplateName = ComponentTemplate->Name;
		return AddedTemplate == TypedTemplate;
	}

	bool InvokeLegacyAddPlayerMapIconComponent(
		AFortPlayerPawnAthena* Pawn,
		UFunction* Function,
		const FName& TemplateName,
		UObject*& Component)
	{
		Component = nullptr;
		if (!Pawn || !Function || !TemplateName.IsValid())
			return false;

		alignas(16) uint8 Parameters[
			PlayerMapIconParameterBufferSize]{};
		bool bManualAttachment = false;
		FTransform RelativeTransform{};
		const UObject* ComponentTemplateContext = Pawn;
		if (!WritePlayerMapIconParameter(
				Parameters,
				Function,
				"TemplateName",
				&TemplateName,
				sizeof(TemplateName)) ||
			!WritePlayerMapIconParameter(
				Parameters,
				Function,
				"bManualAttachment",
				&bManualAttachment,
				sizeof(bManualAttachment)) ||
			!WritePlayerMapIconParameter(
				Parameters,
				Function,
				"RelativeTransform",
				&RelativeTransform,
				FTransform::Size()) ||
			!WritePlayerMapIconParameter(
				Parameters,
				Function,
				"ComponentTemplateContext",
				&ComponentTemplateContext,
				sizeof(ComponentTemplateContext)))
		{
			return false;
		}

		return InvokePlayerMapIconObjectEventGuarded(
			Pawn,
			Function,
			Parameters,
			Component);
	}

	UObject* CreatePlayerMapIconComponent(
		AFortPlayerPawnAthena* Pawn,
		const UClass* ComponentClass,
		bool& bUsedLegacyFallback)
	{
		bUsedLegacyFallback = false;
		if (!Pawn || !ComponentClass)
			return nullptr;

		static UFunction* AddComponentFunction =
			Pawn->GetFunction("AddComponentByClass");
		if (AddComponentFunction)
		{
			auto Component = InvokeAddPlayerMapIconComponent(
				Pawn,
				AddComponentFunction,
				ComponentClass);
			if (!IsLiveHealthStateObject(Component) ||
				!Component->IsA(ComponentClass))
			{
				Component = FindPlayerMapIconComponent(
					Pawn,
					ComponentClass);
			}
			if (Component)
				return Component;
		}

		// Before AddComponentByClass, AddComponent accepted a named blueprint
		// component template. Expose the component CDO as such a template and let
		// the legacy engine duplicate and register it normally.
		static UFunction* LegacyAddComponentFunction =
			Pawn->GetFunction("AddComponent");
		FName TemplateName{};
		TArray<UActorComponent*>* ModifiedTemplates = nullptr;
		int32 AddedTemplateIndex = -1;
		if (LegacyAddComponentFunction)
		{
			const bool bTemplateResolved =
				ResolveLegacyPlayerMapIconTemplate(
				Pawn,
				ComponentClass,
				TemplateName,
				ModifiedTemplates,
				AddedTemplateIndex);
			UObject* Component = nullptr;
			const bool bInvoked = bTemplateResolved &&
				InvokeLegacyAddPlayerMapIconComponent(
					Pawn,
					LegacyAddComponentFunction,
					TemplateName,
					Component);
			if (ModifiedTemplates &&
				IsPlayerMapIconTemplateArrayValid(
					ModifiedTemplates) &&
				ModifiedTemplates->IsValidIndex(
					AddedTemplateIndex) &&
				(*ModifiedTemplates)[AddedTemplateIndex] ==
					static_cast<UActorComponent*>(
						ComponentClass->GetDefaultObj()))
			{
				ModifiedTemplates->Remove(AddedTemplateIndex);
			}
			if (!bInvoked && GPlayerMapIconSetupDisabled)
				return nullptr;

			if (!IsLiveHealthStateObject(Component) ||
				!Component->IsA(ComponentClass))
			{
				Component = FindPlayerMapIconComponent(
					Pawn,
					ComponentClass);
			}
			if (Component)
			{
				bUsedLegacyFallback = true;
				return Component;
			}
		}

		// Native pawn classes have no ComponentTemplates array. SpawnObject still
		// creates an owned actor component in cooked builds; SetIsReplicated below
		// puts it in the actor's dynamic subobject list, and the receiving actor
		// channel registers the client component before applying replicated data.
		// UE 4.16 explicitly rejects component classes in SpawnObject, so its
		// registered template path above is mandatory on those earliest builds.
		if (VersionInfo.EngineVersion <= 4.16)
			return nullptr;

		auto GameplayStaticsClass = UGameplayStatics::StaticClass();
		auto GameplayStaticsDefault = GameplayStaticsClass
			? GameplayStaticsClass->GetDefaultObj()
			: nullptr;
		static UFunction* SpawnObjectFunction = GameplayStaticsDefault
			? GameplayStaticsDefault->GetFunction("SpawnObject")
			: nullptr;
		if (!SpawnObjectFunction)
			return nullptr;

		auto Component = InvokeSpawnPlayerMapIconComponent(
			GameplayStaticsDefault,
			SpawnObjectFunction,
			ComponentClass,
			Pawn);
		if (!IsLiveHealthStateObject(Component) ||
			!Component->IsA(ComponentClass))
			return nullptr;
		if (FindPlayerMapIconComponent(
				Pawn,
				ComponentClass) != Component)
		{
			return nullptr;
		}

		bUsedLegacyFallback = true;
		return Component;
	}

	bool EnsurePlayerMapIconUnsafe(
		AFortPlayerControllerAthena* Controller,
		AFortPlayerPawnAthena* Pawn)
	{
		if (!IsLiveHealthStateObject(Pawn))
			return false;

		static const UClass* ComponentClass =
			FindClass("FortMiniMapComponent");
		if (!ComponentClass)
		{
			if (!GWarnedMissingPlayerMapIconClass)
			{
				GWarnedMissingPlayerMapIconClass = true;
				SDK::DbgLog(
					"[PlayerMapIcons] FortMiniMapComponent is unavailable "
					"on FN %.2f\n",
					VersionInfo.FortniteVersion);
			}
			return false;
		}

		auto IconTexture = ResolvePlayerMapIconTexture(
			Controller,
			Pawn);
		if (!IconTexture)
		{
			if (!GWarnedMissingPlayerMapIconTexture)
			{
				GWarnedMissingPlayerMapIconTexture = true;
				SDK::DbgLog(
					"[PlayerMapIcons] no character or fallback icon texture "
					"was available on FN %.2f\n",
					VersionInfo.FortniteVersion);
			}
			return false;
		}

		auto Component = FindPlayerMapIconComponent(
			Pawn,
			ComponentClass);
		bool bUsedLegacyFallback = false;
		const bool bCreated = Component == nullptr;
		if (!Component)
		{
			Component = CreatePlayerMapIconComponent(
				Pawn,
				ComponentClass,
				bUsedLegacyFallback);
		}
		if (!Component)
		{
			if (!GWarnedPlayerMapIconCreation)
			{
				GWarnedPlayerMapIconCreation = true;
				SDK::DbgLog(
					"[PlayerMapIcons] component creation failed on FN %.2f\n",
					VersionInfo.FortniteVersion);
			}
			return false;
		}

		const auto& Functions = GetPlayerMapIconFunctions(Component);
		if (!Functions.Setup || !Functions.SetReplicated)
		{
			if (!GWarnedPlayerMapIconSetup)
			{
				GWarnedPlayerMapIconSetup = true;
				SDK::DbgLog(
					"[PlayerMapIcons] required component API is unavailable "
					"on FN %.2f\n",
					VersionInfo.FortniteVersion);
			}
			return false;
		}

		bool bReplicated = true;
		Component->Call<void>(
			Functions.SetReplicated,
			bReplicated);

		if (Functions.Activate)
		{
			bool bReset = true;
			Component->Call<void>(Functions.Activate, bReset);
		}

		const FPlayerMapIconLinearColor White{
			1.f, 1.f, 1.f, 1.f
		};
		float ColorPulsesPerSecond = 0.f;
		float SizePulsesPerSecond = 0.f;
		Component->Call<void>(
			Functions.Setup,
			IconTexture,
			White,
			White,
			ColorPulsesPerSecond,
			SizePulsesPerSecond);

		if (Functions.SetViewableDistance)
		{
			float Distance = PlayerMapIconViewableDistance;
			Component->Call<void>(
				Functions.SetViewableDistance,
				Distance);
		}

		bool bVisible = true;
		UFunction* VisibilityFunctions[] = {
			Functions.SetVisible,
			Functions.SetVisibleOnMap,
			Functions.SetVisibleOnMiniMap
		};
		for (auto VisibilityFunction : VisibilityFunctions)
		{
			if (VisibilityFunction)
			{
				Component->Call<void>(
					VisibilityFunction,
					bVisible);
			}
		}

		ApplyPlayerMapIconPawnBrush(Pawn, IconTexture);

		if (Functions.RepNotify)
		{
			Component->Call<void>(Functions.RepNotify);
		}

		Pawn->ForceNetUpdate();
		if (bCreated)
		{
			SDK::DbgLog(
				"[PlayerMapIcons] configured pawn=%p component=%p icon=%p "
				"legacyFallback=%d FN=%.2f\n",
				(void*)Pawn,
				(void*)Component,
				(void*)IconTexture,
				bUsedLegacyFallback ? 1 : 0,
				VersionInfo.FortniteVersion);
		}

		return true;
	}

	void ResetMinimumHealthGodStatesForWorld(UWorld* World)
	{
		if (GMinimumHealthGodStateWorld.Get() == World)
			return;

		GMinimumHealthGodStates = {};
		GMinimumHealthGodStateCursor = 0;
		GMinimumHealthGodStateWorld =
			TWeakObjectPtr<UWorld>(World);
	}

	bool ResolveMinimumHealthGodAttribute(
		AFortPlayerPawnAthena* Pawn,
		UFortHealthSet*& HealthSet,
		FFortGameplayAttributeData*& Health)
	{
		HealthSet = nullptr;
		Health = nullptr;
		if (!IsLiveHealthStateObject(Pawn) ||
			!Pawn->HasHealthSet())
		{
			return false;
		}

		auto CandidateHealthSet = Pawn->HealthSet;
		if (!IsLiveHealthStateObject(CandidateHealthSet) ||
			!CandidateHealthSet->HasHealth() ||
			!FFortGameplayAttributeData::StaticStruct() ||
			!FFortGameplayAttributeData::HasMinimum())
		{
			return false;
		}

		HealthSet = CandidateHealthSet;
		Health = &CandidateHealthSet->Health;
		return true;
	}

	FMinimumHealthGodState* FindMinimumHealthGodState(
		const AFortPlayerControllerAthena* Controller)
	{
		if (!Controller)
			return nullptr;

		for (auto& State : GMinimumHealthGodStates)
		{
			if (State.Controller.Get() == Controller)
				return &State;
		}
		return nullptr;
	}

	void RestoreMinimumHealthGodState(
		FMinimumHealthGodState& State)
	{
		auto Pawn = State.AppliedPawn.Get();
		auto HealthSet = State.AppliedHealthSet.Get();
		if (State.bCapturedPreviousMinimum &&
			IsLiveHealthStateObject(Pawn) &&
			IsLiveHealthStateObject(HealthSet) &&
			HealthSet->HasHealth() &&
			FFortGameplayAttributeData::StaticStruct() &&
			FFortGameplayAttributeData::HasMinimum())
		{
			auto& Health = HealthSet->Health;
			// Do not overwrite a newer external policy that replaced our
			// one-health floor while the mode was active.
			if (FPlatformMath::IsFinite(Health.Minimum) &&
				std::abs(Health.Minimum - 1.f) <=
					HealthStateEpsilon)
			{
				Health.Minimum = State.PreviousMinimum;
				Pawn->ForceNetUpdate();
			}
		}

		State.AppliedPawn = {};
		State.AppliedHealthSet = {};
		State.PreviousMinimum = 0.f;
		State.bCapturedPreviousMinimum = false;
	}

	bool ApplyMinimumHealthGodState(
		FMinimumHealthGodState& State,
		AFortPlayerPawnAthena* Pawn)
	{
		UFortHealthSet* HealthSet = nullptr;
		FFortGameplayAttributeData* Health = nullptr;
		if (!ResolveMinimumHealthGodAttribute(
				Pawn, HealthSet, Health))
		{
			return false;
		}

		if (State.AppliedPawn.Get() != Pawn ||
			State.AppliedHealthSet.Get() != HealthSet)
		{
			RestoreMinimumHealthGodState(State);
			State.AppliedPawn =
				TWeakObjectPtr<AFortPlayerPawnAthena>(Pawn);
			State.AppliedHealthSet =
				TWeakObjectPtr<UFortHealthSet>(HealthSet);
			State.PreviousMinimum =
				FPlatformMath::IsFinite(Health->Minimum)
					? Health->Minimum
					: 0.f;
			State.bCapturedPreviousMinimum = true;
		}

		if (!FPlatformMath::IsFinite(Health->Minimum) ||
			std::abs(Health->Minimum - 1.f) >
				HealthStateEpsilon)
		{
			// Minimum is an enforcement field, not the health value itself.
			// A raw write avoids the GAS base-value delta caused by calling
			// OnRep_Health without a real pre-mutation attribute snapshot.
			Health->Minimum = 1.f;
			Pawn->ForceNetUpdate();
		}
		return true;
	}

	FMinimumHealthGodState& AddMinimumHealthGodState(
		AFortPlayerControllerAthena* Controller)
	{
		size_t EmptyIndex = MaxMinimumHealthGodStates;
		for (size_t Index = 0;
			Index < GMinimumHealthGodStates.size();
			++Index)
		{
			auto ExistingController =
				GMinimumHealthGodStates[Index].Controller.Get();
			if (ExistingController == Controller)
				return GMinimumHealthGodStates[Index];
			if (!ExistingController &&
				EmptyIndex == MaxMinimumHealthGodStates)
			{
				EmptyIndex = Index;
			}
		}

		const size_t TargetIndex =
			EmptyIndex < MaxMinimumHealthGodStates
				? EmptyIndex
				: GMinimumHealthGodStateCursor++ %
					MaxMinimumHealthGodStates;
		auto& State = GMinimumHealthGodStates[TargetIndex];
		RestoreMinimumHealthGodState(State);
		State = {};
		State.Controller =
			TWeakObjectPtr<AFortPlayerControllerAthena>(
				Controller);
		return State;
	}

	AFortPlayerPawnAthena* GetMinimumHealthGodPawn(
		AFortPlayerControllerAthena* Controller)
	{
		if (!IsLiveHealthStateObject(Controller))
			return nullptr;

		AFortPlayerPawnAthena* Pawn = nullptr;
		if (Controller->HasMyFortPawn() &&
			IsLiveHealthStateObject(Controller->MyFortPawn))
		{
			Pawn = Controller->MyFortPawn;
		}
		else if (Controller->HasPawn() &&
			IsLiveHealthStateObject(Controller->Pawn))
		{
			Pawn = Controller->Pawn;
		}

		return Pawn &&
			Pawn->IsA(AFortPlayerPawnAthena::StaticClass())
				? Pawn
				: nullptr;
	}

	bool IsWritableReviveMemory(void* Address, size_t Size)
	{
		if (!Address || !Size)
			return false;

		MEMORY_BASIC_INFORMATION MemoryInfo{};
		if (VirtualQuery(
				Address,
				&MemoryInfo,
				sizeof(MemoryInfo)) != sizeof(MemoryInfo) ||
			MemoryInfo.State != MEM_COMMIT ||
			(MemoryInfo.Protect &
				(PAGE_GUARD | PAGE_NOACCESS)))
		{
			return false;
		}

		const DWORD Protection =
			MemoryInfo.Protect & 0xFF;
		const bool bWritable =
			Protection == PAGE_READWRITE ||
			Protection == PAGE_WRITECOPY ||
			Protection == PAGE_EXECUTE_READWRITE ||
			Protection == PAGE_EXECUTE_WRITECOPY;
		const auto Begin =
			reinterpret_cast<uintptr_t>(Address);
		const auto End = Begin + Size;
		const auto RegionEnd =
			reinterpret_cast<uintptr_t>(
				MemoryInfo.BaseAddress) +
			MemoryInfo.RegionSize;
		return bWritable &&
			End >= Begin &&
			End <= RegionEnd;
	}

	bool IsHealthRepairMatchActive(UWorld* World)
	{
		if (GUI::gsStatus == StartedMatch)
			return true;
		if (!World ||
			!IsLiveHealthStateObject(World->AuthorityGameMode))
		{
			return false;
		}

		auto GameMode = static_cast<AFortGameMode*>(
			World->AuthorityGameMode);
		if (!GameMode->HasMatchState())
			return false;

		static const FName InProgressName(L"InProgress");
		return GameMode->MatchState == InProgressName;
	}

	bool IsDBNOAuthoritativelyDisabled(UWorld* World)
	{
		if (!FConfiguration::bEnableDBNO.load(
				std::memory_order_acquire))
		{
			return true;
		}
		if (!World ||
			!IsLiveHealthStateObject(World->AuthorityGameMode))
		{
			return false;
		}

		auto GameMode = static_cast<AFortGameMode*>(
			World->AuthorityGameMode);
		bool bHasAuthoritativeState = false;
		bool bAnyStateEnabled = false;
		if (GameMode->HasbEnableDBNO())
		{
			bHasAuthoritativeState = true;
			bAnyStateEnabled =
				bAnyStateEnabled || GameMode->bEnableDBNO;
		}
		if (GameMode->HasbDBNOEnabled())
		{
			bHasAuthoritativeState = true;
			bAnyStateEnabled =
				bAnyStateEnabled || GameMode->bDBNOEnabled;
		}

		auto GameState = GameMode->GameState
			? static_cast<AFortGameStateAthena*>(
				GameMode->GameState)
			: nullptr;
		if (IsLiveHealthStateObject(GameState) &&
			GameState->HasbDBNOEnabledForGameMode())
		{
			bHasAuthoritativeState = true;
			bAnyStateEnabled = bAnyStateEnabled ||
				GameState->bDBNOEnabledForGameMode;
		}

		return bHasAuthoritativeState && !bAnyStateEnabled;
	}

	FTrackedHealthState& FindTrackedHealthState(
		AFortPlayerPawnAthena* Pawn,
		AFortPlayerControllerAthena* Controller)
	{
		size_t EmptyIndex = MaxTrackedHealthStates;
		for (size_t Index = 0;
			Index < GTrackedHealthStates.size();
			++Index)
		{
			auto ExistingPawn =
				GTrackedHealthStates[Index].Pawn.Get();
			if (ExistingPawn == Pawn)
			{
				if (GTrackedHealthStates[Index]
						.Controller.Get() != Controller)
				{
					GTrackedHealthStates[Index] = {};
					GTrackedHealthStates[Index].Pawn =
						TWeakObjectPtr<
							AFortPlayerPawnAthena>(Pawn);
					GTrackedHealthStates[Index].Controller =
						TWeakObjectPtr<
							AFortPlayerControllerAthena>(
								Controller);
				}
				return GTrackedHealthStates[Index];
			}
			if (!ExistingPawn &&
				EmptyIndex == MaxTrackedHealthStates)
			{
				EmptyIndex = Index;
			}
		}

		const size_t TargetIndex =
			EmptyIndex < MaxTrackedHealthStates
			? EmptyIndex
			: GTrackedHealthStateCursor++ %
				MaxTrackedHealthStates;
		auto& State = GTrackedHealthStates[TargetIndex];
		State = {};
		State.Pawn =
			TWeakObjectPtr<AFortPlayerPawnAthena>(Pawn);
		State.Controller =
			TWeakObjectPtr<AFortPlayerControllerAthena>(
				Controller);
		return State;
	}

	bool IsPawnInNativeDeathOrDBNO(
		AFortPlayerPawnAthena* Pawn)
	{
		if (!Pawn)
			return true;

		if ((Pawn->HasbActorIsBeingDestroyed() &&
				Pawn->bActorIsBeingDestroyed) ||
			(Pawn->HasbIsHiddenForDeath() &&
				Pawn->bIsHiddenForDeath) ||
			(Pawn->HasbIsDying() && Pawn->bIsDying) ||
			(Pawn->HasbPlayedDying() &&
				Pawn->bPlayedDying) ||
			(Pawn->HasbIsDBNO() && Pawn->bIsDBNO))
		{
			return true;
		}

		auto IsDBNOFunction = Pawn->GetFunction("IsDBNO");
		return IsDBNOFunction &&
			Pawn->Call<bool>(IsDBNOFunction);
	}

	AFortPlayerControllerAthena*
		GetNativeLastDamagerController(
			AFortPlayerControllerAthena* VictimController)
	{
		if (!VictimController ||
			!VictimController->HasLastDamager() ||
			!IsLiveHealthStateObject(
				VictimController->LastDamager))
		{
			return nullptr;
		}

		auto LastDamager =
			VictimController->LastDamager->Cast<
				AFortPlayerControllerAthena>();
		return LastDamager &&
			LastDamager != VictimController
			? LastDamager
			: nullptr;
	}

	AActor* GetLiveControlledPawn(
		AFortPlayerControllerAthena* Controller)
	{
		if (!Controller)
			return nullptr;

		AActor* ControlledActor = nullptr;
		if (Controller->HasMyFortPawn() &&
			IsLiveHealthStateObject(Controller->MyFortPawn))
		{
			ControlledActor =
				reinterpret_cast<AActor*>(
					Controller->MyFortPawn);
		}
		else if (Controller->HasPawn())
		{
			ControlledActor =
				reinterpret_cast<AActor*>(Controller->Pawn);
		}
		return IsLiveHealthStateObject(ControlledActor)
			? ControlledActor
			: nullptr;
	}

	void RepairPossessedPawnHealthState(
		AFortPlayerControllerAthena* PlayerController,
		AFortPlayerPawnAthena* Pawn,
		ULONGLONG CurrentTimeMs)
	{
		if (!IsLiveHealthStateObject(PlayerController) ||
			!IsLiveHealthStateObject(Pawn) ||
			Pawn->IsDefaultObject() ||
			!Pawn->HasAuthority())
		{
			return;
		}

		auto& State = FindTrackedHealthState(
			Pawn, PlayerController);
		float Health = Pawn->GetHealth();
		const float MaxHealth = Pawn->GetMaxHealth();
		const float Shield = Pawn->GetShield();
		const float MaxShield = Pawn->GetMaxShield();
		const bool bHasLastDamagedTime =
			Pawn->HasLastDamagedTime();
		const float LastDamagedTime = bHasLastDamagedTime
			? Pawn->LastDamagedTime
			: 0.f;
		const bool bValidLastDamagedTime =
			bHasLastDamagedTime &&
			FPlatformMath::IsFinite(LastDamagedTime);

		// Native GAS can bypass AFortPlayerPawnAthena::SetShield. Normalize a
		// bad result before another hit treats the negative amount as a real
		// absorption layer. Do not infer health overflow from a polled negative
		// value: without the exact per-hit callback it could be a same-frame GE
		// compensation/recompute, and replaying it could double damage.
		if (!FPlatformMath::IsFinite(Shield) || Shield < 0.f)
		{
			const float InvalidShieldValue = Shield;
			Pawn->SetShield(0.f);
			if (!IsLiveHealthStateObject(Pawn))
				return;
			Pawn->ForceNetUpdate();
			if (GShieldRepairLogCount++ < 32)
			{
				SDK::DbgLog(
					"[HealthRepair] normalized invalid shield "
					"pawn=%p value=%.2f health=%.2f "
					"maxHealth=%.2f maxShield=%.2f "
					"damagers=%d version=%.2f\n",
					(void*)Pawn,
					InvalidShieldValue,
					Health,
					MaxHealth,
					MaxShield,
					Pawn->HasDamagers()
						? Pawn->Damagers.Num()
						: -1,
					VersionInfo.FortniteVersion);
			}
		}
		if (!IsLiveHealthStateObject(Pawn))
			return;

		Health = Pawn->GetHealth();
		const bool bDeathAlreadyNotified =
			PlayerController->HasbClientNotifiedOfPawnDied() &&
			PlayerController->bClientNotifiedOfPawnDied;
		if (FindMinimumHealthGodState(PlayerController) &&
			FPlatformMath::IsFinite(Health) &&
			Health < 1.f &&
			FPlatformMath::IsFinite(MaxHealth) &&
			MaxHealth > HealthStateEpsilon &&
			!bDeathAlreadyNotified &&
			!IsPawnInNativeDeathOrDBNO(Pawn))
		{
			// Health.Minimum should stop the native hit before this point.
			// This is a pre-replication backstop for builds whose aggregator
			// briefly publishes zero without beginning DBNO/death.
			Pawn->SetHealth(1.f);
			if (!IsLiveHealthStateObject(Pawn))
				return;
			Pawn->ForceNetUpdate();
			State.bObservedAlive = true;
			State.LastForceKillAttemptMs = 0;
			State.ConsecutiveUnresolvedZeroFlushes = 0;
			State.ForceKillAttempts = 0;
			State.bZeroStateLogged = false;
			return;
		}

		if (FPlatformMath::IsFinite(Health) &&
			Health > HealthStateEpsilon &&
			FPlatformMath::IsFinite(MaxHealth) &&
			MaxHealth > HealthStateEpsilon)
		{
			State.bObservedAlive = true;
			State.LastForceKillAttemptMs = 0;
			State.ConsecutiveUnresolvedZeroFlushes = 0;
			State.ForceKillAttempts = 0;
			State.bZeroStateLogged = false;
			return;
		}

		// A newly spawned pawn can report zero until its health set and default
		// gameplay effects finish initializing. Only a pawn observed alive in
		// this world is eligible for lethal recovery.
		if (!State.bObservedAlive)
			return;

		// A non-finite aggregator or a temporary zero-capacity form is not a
		// trustworthy lethal result. Leave it to native GAS instead of
		// converting uncertain state into an elimination.
		if (!FPlatformMath::IsFinite(Health) ||
			!FPlatformMath::IsFinite(MaxHealth) ||
			MaxHealth <= HealthStateEpsilon)
		{
			State.LastForceKillAttemptMs = 0;
			State.ConsecutiveUnresolvedZeroFlushes = 0;
			State.ForceKillAttempts = 0;
			State.bZeroStateLogged = false;
			return;
		}

		if (!State.bZeroStateLogged)
		{
			State.bZeroStateLogged = true;
			const bool bNativeIsDBNO =
				Pawn->GetFunction("IsDBNO") &&
				Pawn->Call<bool>(Pawn->GetFunction("IsDBNO"));
			SDK::DbgLog(
				"[HealthRepair] observed zero-health pawn=%p "
				"controller=%p health=%.2f maxHealth=%.2f "
				"shield=%.2f lastDamage=%.3f gsStatus=%d "
				"dbno=%d/%d dying=%d played=%d hidden=%d "
				"destroying=%d notified=%d version=%.2f\n",
				(void*)Pawn,
				(void*)PlayerController,
				Health,
				MaxHealth,
				Shield,
				bValidLastDamagedTime ? LastDamagedTime : -1.f,
				static_cast<int32>(GUI::gsStatus),
				Pawn->HasbIsDBNO() && Pawn->bIsDBNO ? 1 : 0,
				bNativeIsDBNO ? 1 : 0,
				Pawn->HasbIsDying() && Pawn->bIsDying ? 1 : 0,
				Pawn->HasbPlayedDying() && Pawn->bPlayedDying
					? 1 : 0,
				Pawn->HasbIsHiddenForDeath() &&
					Pawn->bIsHiddenForDeath ? 1 : 0,
				Pawn->HasbActorIsBeingDestroyed() &&
					Pawn->bActorIsBeingDestroyed ? 1 : 0,
				PlayerController->HasbClientNotifiedOfPawnDied() &&
					PlayerController->bClientNotifiedOfPawnDied
					? 1 : 0,
				VersionInfo.FortniteVersion);
		}

		// TickHealthStateRepair runs immediately before each server replication
		// send. Never replace a native knock/death transition with ForceKill.
		if ((PlayerController->HasbClientNotifiedOfPawnDied() &&
				PlayerController->bClientNotifiedOfPawnDied) ||
			IsPawnInNativeDeathOrDBNO(Pawn))
		{
			State.LastForceKillAttemptMs = 0;
			State.ConsecutiveUnresolvedZeroFlushes = 0;
			State.ForceKillAttempts = 0;
			return;
		}

		// This is the broken state: the exact possessed pawn was previously
		// alive, now has a finite zero health value and positive capacity, but
		// native damage produced neither DBNO nor death. When DBNO is explicitly
		// disabled by the authoritative match settings, there is no next-frame
		// knock transition to preserve, so finalize before the first bad zero can
		// replicate. Otherwise retain one completed-flush grace period for builds
		// that publish DBNO on the following frame. LastDamagedTime is diagnostic
		// only because several high-damage GAS paths do not advance it.
		const bool bDBNOKnownDisabled =
			IsDBNOAuthoritativelyDisabled(UWorld::GetWorld());
		const uint8 RequiredZeroFlushes =
			bDBNOKnownDisabled ? 1 : 2;
		if (State.ConsecutiveUnresolvedZeroFlushes <
			RequiredZeroFlushes)
			++State.ConsecutiveUnresolvedZeroFlushes;
		if (State.ConsecutiveUnresolvedZeroFlushes <
			RequiredZeroFlushes)
			return;

		if (State.ForceKillAttempts >= 2)
			return;
		if (State.ForceKillAttempts > 0 &&
			(CurrentTimeMs < State.LastForceKillAttemptMs ||
				CurrentTimeMs - State.LastForceKillAttemptMs <
					1000ULL))
		{
			return;
		}

		auto KillerController =
			GetNativeLastDamagerController(PlayerController);
		// Never combine LastDamager with an arbitrary Damagers[] entry: that
		// array is an assist ledger, not a chronological per-hit record. A
		// controller plus its own live pawn (or null/null for environmental
		// damage) is one coherent, version-stable attribution pair.
		auto KillerActor =
			GetLiveControlledPawn(KillerController);
		auto ForceKillFunction =
			Pawn->GetFunction("ForceKill");
		if (!ForceKillFunction)
		{
			State.ForceKillAttempts = 2;
			SDK::DbgLog(
				"[HealthRepair] zero-health pawn has no "
				"ForceKill capability pawn=%p version=%.2f\n",
				(void*)Pawn,
				VersionInfo.FortniteVersion);
			return;
		}

		++State.ForceKillAttempts;
		State.LastForceKillAttemptMs = CurrentTimeMs;
		SDK::DbgLog(
			"[HealthRepair] finalizing stuck lethal state before replication "
			"pawn=%p controller=%p killer=%p causer=%p "
			"health=%.2f attempt=%u dbnoKnownDisabled=%d "
			"zeroFlush=%u/%u version=%.2f\n",
			(void*)Pawn,
			(void*)PlayerController,
			(void*)KillerController,
			(void*)KillerActor,
			Health,
			static_cast<unsigned>(State.ForceKillAttempts),
			bDBNOKnownDisabled ? 1 : 0,
			static_cast<unsigned>(
				State.ConsecutiveUnresolvedZeroFlushes),
			static_cast<unsigned>(RequiredZeroFlushes),
			VersionInfo.FortniteVersion);

		FGameplayTag DeathReason{};
		Pawn->Call<void>(
			ForceKillFunction,
			DeathReason,
			KillerController,
			KillerActor);
		if (IsLiveHealthStateObject(Pawn))
			Pawn->ForceNetUpdate();
	}

	bool IsDBNOAbility(const UFortGameplayAbility* Ability)
	{
		if (!Ability)
			return false;

		auto DBNOAbilityClass =
			UGAB_AthenaDBNO_C::StaticClass();
		if (DBNOAbilityClass)
			return Ability->IsA(DBNOAbilityClass);

		return Ability->Name.ToString().find("DBNO") !=
			std::string::npos;
	}

	bool IsDBNOEffect(const UGameplayEffect* Effect)
	{
		if (!Effect)
			return false;

		const auto Name = Effect->Name.ToString();
		return Name.find("DBNO") != std::string::npos ||
			Name.find("Downed") != std::string::npos;
	}

	bool ClearReviveDeathInfo15_30(
		AFortPlayerStateAthena* PlayerState)
	{
		if (!PlayerState || !PlayerState->HasDeathInfo())
			return false;

		auto& DeathInfo = PlayerState->DeathInfo;
		bool bChanged = false;
		if (FDeathInfo::HasFinisherOrDowner() &&
			DeathInfo.FinisherOrDowner)
		{
			DeathInfo.FinisherOrDowner = nullptr;
			bChanged = true;
		}
		if (FDeathInfo::HasDowner() && DeathInfo.Downer)
		{
			DeathInfo.Downer = nullptr;
			bChanged = true;
		}
		if (FDeathInfo::HasbDBNO() && DeathInfo.bDBNO)
		{
			DeathInfo.bDBNO = false;
			bChanged = true;
		}
		if (FDeathInfo::HasDeathCause() &&
			DeathInfo.DeathCause != 0)
		{
			DeathInfo.DeathCause = 0;
			bChanged = true;
		}
		if (FDeathInfo::HasDeathClassSlot() &&
			DeathInfo.DeathClassSlot != static_cast<uint8>(-1))
		{
			DeathInfo.DeathClassSlot =
				static_cast<uint8>(-1);
			bChanged = true;
		}
		if (FDeathInfo::HasDistance() &&
			DeathInfo.Distance != 0.f)
		{
			DeathInfo.Distance = 0.f;
			bChanged = true;
		}
		if (FDeathInfo::HasDeathLocation())
		{
			auto& DeathLocation = DeathInfo.DeathLocation;
			if (DeathLocation.X != 0.0 ||
				DeathLocation.Y != 0.0 ||
				DeathLocation.Z != 0.0)
			{
				DeathLocation = FVector{};
				bChanged = true;
			}
		}

		auto ClearTags =
			[&bChanged](FGameplayTagContainer& Tags)
			{
				if (Tags.GameplayTags.Num() != 0)
				{
					Tags.GameplayTags.ResetNum();
					bChanged = true;
				}
				if (Tags.ParentTags.Num() != 0)
				{
					Tags.ParentTags.ResetNum();
					bChanged = true;
				}
			};
		if (FDeathInfo::HasDeathTags())
			ClearTags(DeathInfo.DeathTags);
		if (FDeathInfo::HasFinisherOrDownerTags())
			ClearTags(DeathInfo.FinisherOrDownerTags);
		if (FDeathInfo::HasVictimTags())
			ClearTags(DeathInfo.VictimTags);

		if (FDeathInfo::HasbInitialized() &&
			DeathInfo.bInitialized)
		{
			DeathInfo.bInitialized = false;
			bChanged = true;
		}
		if (bChanged)
			PlayerState->OnRep_DeathInfo();
		return bChanged;
	}

	bool SendReviveGameplayEvent15_30(
		AFortPlayerPawnAthena* Pawn,
		UAbilitySystemComponent* AbilitySystemComponent,
		AController* EventInstigator)
	{
		auto Fail =
			[Pawn](const char* Reason)
			{
				SDK::DbgLog(
					"[Revive] compatibility revive event "
					"unavailable pawn=%p reason=%s "
					"version=%.2f\n",
					(void*)Pawn,
					Reason,
					VersionInfo.FortniteVersion);
				return false;
			};

		if (!Pawn || !AbilitySystemComponent ||
			!EventInstigator)
			return Fail("invalid actor");

		const uint32 ReviveTagOffset =
			Pawn->GetOffset("EventReviveTag");
		const uint32 ReviveTagSize =
			static_cast<uint32>(FGameplayTag::Size());
		if (ReviveTagOffset == uint32(-1) ||
			ReviveTagOffset > 0x10000 ||
			!SDK::MemReadable(
				reinterpret_cast<const uint8*>(Pawn) +
					ReviveTagOffset,
				ReviveTagSize))
		{
			return Fail("EventReviveTag property");
		}

		FGameplayTag ReviveTag{};
		memcpy(
			&ReviveTag,
			reinterpret_cast<const uint8*>(Pawn) +
				ReviveTagOffset,
			ReviveTagSize);
		if (!ReviveTag.TagName.IsValid())
			return Fail("empty EventReviveTag");

		auto EventDataStruct =
			FindStruct("GameplayEventData");
		if (!EventDataStruct)
			return Fail("GameplayEventData struct");

		const int32 EventDataSize =
			EventDataStruct->GetPropertiesSize();
		if (EventDataSize <= 0 || EventDataSize > 0x200)
			return Fail("GameplayEventData size");

		std::vector<uint8> EventData(
			static_cast<size_t>(EventDataSize),
			0);
		auto WriteEventField =
			[&](const char* Name,
				const void* Source,
				uint32 SourceSize)
			{
				auto Property =
					EventDataStruct->GetProperty(Name);
				if (!Property)
					return false;

				const uint32 Offset =
					EventDataStruct->GetOffset(Name);
				const uint32 ElementSize =
					GetFromOffset<uint32>(
						Property,
						Offsets::ElementSize);
				if (Offset == uint32(-1) ||
					ElementSize != SourceSize ||
					Offset >
						static_cast<uint32>(EventDataSize) ||
					SourceSize >
						static_cast<uint32>(EventDataSize) -
							Offset)
				{
					return false;
				}

				memcpy(
					EventData.data() + Offset,
					Source,
					SourceSize);
				return true;
			};

		AActor* InstigatorActor =
			reinterpret_cast<AActor*>(EventInstigator);
		AActor* TargetActor =
			reinterpret_cast<AActor*>(Pawn);
		if (!WriteEventField(
				"EventTag",
				&ReviveTag,
				ReviveTagSize) ||
			!WriteEventField(
				"Instigator",
				&InstigatorActor,
				sizeof(InstigatorActor)) ||
			!WriteEventField(
				"Target",
				&TargetActor,
				sizeof(TargetActor)))
		{
			return Fail("GameplayEventData fields");
		}

		auto MakeContextFunction =
			AbilitySystemComponent->GetFunction(
				"MakeEffectContext");
		if (!MakeContextFunction)
			return Fail("MakeEffectContext function");
		auto ContextHandleProperty =
			EventDataStruct->GetProperty("ContextHandle");
		const uint32 ContextHandleSize =
			ContextHandleProperty
				? GetFromOffset<uint32>(
					ContextHandleProperty,
					Offsets::ElementSize)
				: 0;
		const auto MakeContextParams =
			MakeContextFunction->GetParamsNamed();
		const UFunction::ParamNamed*
			MakeContextReturnParam = nullptr;
		for (const auto& Param :
			MakeContextParams.NameOffsetMap)
		{
			if (Param.Name == "ReturnValue")
				MakeContextReturnParam = &Param;
		}
		constexpr uint64 CPF_Parm = 0x80;
		constexpr uint64 CPF_ReturnParm = 0x400;
		if (ContextHandleSize == 0 ||
			ContextHandleSize >
				sizeof(FGameplayEffectContextHandle) ||
			MakeContextFunction->GetPropertiesSize() !=
				ContextHandleSize ||
			MakeContextParams.Size != ContextHandleSize ||
			MakeContextParams.NameOffsetMap.size() != 1 ||
			!MakeContextReturnParam ||
			MakeContextReturnParam->Offset != 0 ||
			MakeContextReturnParam->ElementSize !=
				ContextHandleSize ||
			!(MakeContextReturnParam->PropertyFlags &
				CPF_Parm) ||
			!(MakeContextReturnParam->PropertyFlags &
				CPF_ReturnParm))
		{
			return Fail("MakeEffectContext schema");
		}
		auto ContextHandle =
			AbilitySystemComponent->
				Call<FGameplayEffectContextHandle>(
					MakeContextFunction);
		if (!WriteEventField(
				"ContextHandle",
				&ContextHandle,
				ContextHandleSize))
		{
			return Fail("GameplayEventData context");
		}

		FGameplayTagContainer EmptyTags{};
		const FGameplayTagContainer* TargetTags =
			Pawn->HasGameplayTags()
				? &Pawn->GameplayTags
				: &EmptyTags;
		const FGameplayTagContainer* InstigatorTags =
			&EmptyTags;
		auto InstigatorController =
			EventInstigator->Cast<
				AFortPlayerControllerAthena>();
		auto InstigatorPawn =
			InstigatorController &&
				InstigatorController->HasPawn()
				? InstigatorController->Pawn
				: nullptr;
		if (InstigatorPawn &&
			InstigatorPawn->HasGameplayTags())
		{
			InstigatorTags =
				&InstigatorPawn->GameplayTags;
		}
		if (!WriteEventField(
				"InstigatorTags",
				InstigatorTags,
				sizeof(FGameplayTagContainer)) ||
			!WriteEventField(
				"TargetTags",
				TargetTags,
				sizeof(FGameplayTagContainer)))
		{
			return Fail("GameplayEventData tags");
		}

		auto LibraryClass =
			FindClass("AbilitySystemBlueprintLibrary");
		auto LibraryDefault =
			LibraryClass
				? LibraryClass->GetDefaultObj()
				: nullptr;
		if (!LibraryDefault)
			return Fail("AbilitySystemBlueprintLibrary");

		auto TargetDataFunction =
			LibraryDefault->GetFunction(
				"AbilityTargetDataFromActor");
		if (!TargetDataFunction)
			return Fail("AbilityTargetDataFromActor function");
		const auto TargetDataParams =
			TargetDataFunction->GetParamsNamed();
		if (TargetDataParams.Size == 0 ||
			TargetDataParams.Size > 0x100 ||
			TargetDataParams.NameOffsetMap.size() != 2)
		{
			return Fail("AbilityTargetDataFromActor schema");
		}

		const UFunction::ParamNamed*
			TargetDataActorParam = nullptr;
		const UFunction::ParamNamed*
			TargetDataReturnParam = nullptr;
		for (const auto& Param :
			TargetDataParams.NameOffsetMap)
		{
			if (Param.Name == "Actor")
				TargetDataActorParam = &Param;
			else if (Param.Name == "ReturnValue")
				TargetDataReturnParam = &Param;
		}
		auto TargetDataProperty =
			EventDataStruct->GetProperty("TargetData");
		const uint32 TargetDataHandleSize =
			TargetDataProperty
				? GetFromOffset<uint32>(
					TargetDataProperty,
					Offsets::ElementSize)
				: 0;
		if (TargetDataHandleSize == 0 ||
			TargetDataHandleSize > 0x100)
		{
			return Fail("GameplayEventData target data size");
		}
		auto IsValidTargetDataParam =
			[&TargetDataParams](
				const UFunction::ParamNamed* Param,
				uint32 ExpectedSize)
			{
				return Param &&
					(Param->PropertyFlags & CPF_Parm) &&
					Param->ElementSize == ExpectedSize &&
					Param->Offset <= TargetDataParams.Size &&
					ExpectedSize <=
						TargetDataParams.Size - Param->Offset;
			};
		if (!IsValidTargetDataParam(
				TargetDataActorParam,
				sizeof(TargetActor)) ||
			(TargetDataActorParam->PropertyFlags &
				CPF_ReturnParm) ||
			!IsValidTargetDataParam(
				TargetDataReturnParam,
				TargetDataHandleSize) ||
			!(TargetDataReturnParam->PropertyFlags &
				CPF_ReturnParm))
		{
			return Fail(
				"AbilityTargetDataFromActor parameters");
		}

		void* TargetDataMemory =
			FMemory::Malloc(TargetDataParams.Size);
		if (!TargetDataMemory)
			return Fail("target-data allocation");
		memset(
			TargetDataMemory,
			0,
			TargetDataParams.Size);
		memcpy(
			reinterpret_cast<uint8*>(TargetDataMemory) +
				TargetDataActorParam->Offset,
			&TargetActor,
			sizeof(TargetActor));
		LibraryDefault->ProcessEvent(
			TargetDataFunction,
			TargetDataMemory);

		std::vector<uint8> TargetData(
			TargetDataHandleSize,
			0);
		memcpy(
			TargetData.data(),
			reinterpret_cast<uint8*>(TargetDataMemory) +
				TargetDataReturnParam->Offset,
			TargetDataHandleSize);
		FMemory::Free(TargetDataMemory);
		if (!WriteEventField(
				"TargetData",
				TargetData.data(),
				TargetDataHandleSize))
		{
			return Fail("GameplayEventData target data");
		}

		auto SendEventFunction =
			LibraryDefault->GetFunction(
				"SendGameplayEventToActor");
		if (!SendEventFunction)
			return Fail("SendGameplayEventToActor function");

		const auto Params =
			SendEventFunction->GetParamsNamed();
		if (Params.Size == 0 || Params.Size > 0x400 ||
			Params.NameOffsetMap.size() != 3)
		{
			return Fail("SendGameplayEventToActor schema");
		}

		const UFunction::ParamNamed* ActorParam = nullptr;
		const UFunction::ParamNamed* EventTagParam = nullptr;
		const UFunction::ParamNamed* PayloadParam = nullptr;
		for (const auto& Param : Params.NameOffsetMap)
		{
			if (Param.Name == "Actor")
				ActorParam = &Param;
			else if (Param.Name == "EventTag")
				EventTagParam = &Param;
			else if (Param.Name == "Payload")
				PayloadParam = &Param;
		}

		auto IsValidParam =
			[&Params](
				const UFunction::ParamNamed* Param,
				uint32 ExpectedSize)
			{
				return Param &&
					(Param->PropertyFlags & CPF_Parm) &&
					!(Param->PropertyFlags & CPF_ReturnParm) &&
					Param->ElementSize == ExpectedSize &&
					Param->Offset <= Params.Size &&
					ExpectedSize <=
						Params.Size - Param->Offset;
			};
		if (!IsValidParam(
				ActorParam,
				sizeof(TargetActor)) ||
			!IsValidParam(
				EventTagParam,
				ReviveTagSize) ||
			!IsValidParam(
				PayloadParam,
				static_cast<uint32>(EventDataSize)))
		{
			return Fail("SendGameplayEventToActor parameters");
		}

		void* ParamMemory = FMemory::Malloc(Params.Size);
		if (!ParamMemory)
			return Fail("parameter allocation");
		memset(ParamMemory, 0, Params.Size);
		memcpy(
			reinterpret_cast<uint8*>(ParamMemory) +
				ActorParam->Offset,
			&TargetActor,
			sizeof(TargetActor));
		memcpy(
			reinterpret_cast<uint8*>(ParamMemory) +
				EventTagParam->Offset,
			&ReviveTag,
			ReviveTagSize);
		memcpy(
			reinterpret_cast<uint8*>(ParamMemory) +
				PayloadParam->Offset,
			EventData.data(),
			static_cast<size_t>(EventDataSize));

		LibraryDefault->ProcessEvent(
			SendEventFunction,
			ParamMemory);
		FMemory::Free(ParamMemory);

		const auto ReviveTagName =
			ReviveTag.TagName.ToString();
		SDK::DbgLog(
			"[Revive] compatibility revive event dispatched "
			"pawn=%p tag=%s eventSize=0x%X params=0x%X "
			"version=%.2f\n",
			(void*)Pawn,
			ReviveTagName.c_str(),
			EventDataSize,
			Params.Size,
			VersionInfo.FortniteVersion);
		return true;
	}

	UFunction* FindValidatedCancelDBNOAbilitiesFunction(
		UAbilitySystemComponent* AbilitySystemComponent,
		const char*& FailureReason)
	{
		FailureReason = nullptr;
		if (!AbilitySystemComponent)
		{
			FailureReason = "invalid ASC";
			return nullptr;
		}

		auto CancelFunction =
			AbilitySystemComponent->GetFunction(
				"BP_CancelAbilitiesWithTags");
		if (!CancelFunction)
		{
			FailureReason = "function";
			return nullptr;
		}

		const auto Params =
			CancelFunction->GetParamsNamed();
		const UFunction::ParamNamed* TagsParam = nullptr;
		for (const auto& Param : Params.NameOffsetMap)
		{
			if (Param.Name == "Tags")
				TagsParam = &Param;
		}
		constexpr uint64 CPF_Parm = 0x80;
		constexpr uint64 CPF_ReturnParm = 0x400;
		if (Params.Size != sizeof(FGameplayTagContainer) ||
			Params.NameOffsetMap.size() != 1 ||
			!TagsParam ||
			TagsParam->Offset != 0 ||
			TagsParam->ElementSize !=
				sizeof(FGameplayTagContainer) ||
			!(TagsParam->PropertyFlags & CPF_Parm) ||
			(TagsParam->PropertyFlags & CPF_ReturnParm))
		{
			FailureReason = "parameter schema";
			return nullptr;
		}
		return CancelFunction;
	}

	bool CancelDBNOAbilitiesByTag15_30(
		UAbilitySystemComponent* AbilitySystemComponent)
	{
		const char* FailureReason = nullptr;
		auto CancelFunction =
			FindValidatedCancelDBNOAbilitiesFunction(
				AbilitySystemComponent,
				FailureReason);
		if (!CancelFunction)
		{
			SDK::DbgLog(
				"[Revive] compatibility DBNO tag cancel "
				"unavailable asc=%p reason=%s version=%.2f\n",
				(void*)AbilitySystemComponent,
				FailureReason
					? FailureReason
					: "unknown",
				VersionInfo.FortniteVersion);
			return false;
		}

		FGameplayTagContainer DBNOTags{};
		FGameplayTag DBNOTag{};
		DBNOTag.TagName =
			FName(L"Gameplay.Action.Player.DBNO");
		DBNOTags.GameplayTags.Add(
			DBNOTag,
			FGameplayTag::Size());
		FGameplayTag AthenaDBNOTag{};
		AthenaDBNOTag.TagName =
			FName(L"Gameplay.Action.Player.DBNOAthena");
		DBNOTags.GameplayTags.Add(
			AthenaDBNOTag,
			FGameplayTag::Size());

		AbilitySystemComponent->ProcessEvent(
			CancelFunction,
			&DBNOTags);
		DBNOTags.GameplayTags.Free();
		DBNOTags.ParentTags.Free();
		return true;
	}

	bool HasStableManualReviveOwnership27_11(
		AFortPlayerPawnAthena* Pawn,
		AFortPlayerControllerAthena* DeadController,
		AFortPlayerStateAthena* DeadPlayerState,
		UAbilitySystemComponent* AbilitySystemComponent,
		AController* EventInstigator)
	{
		if (VersionInfo.FortniteVersion != 27.11 ||
			!IsLiveHealthStateObject(Pawn) ||
			!IsLiveHealthStateObject(DeadController) ||
			!IsLiveHealthStateObject(DeadPlayerState) ||
			!IsLiveHealthStateObject(
				AbilitySystemComponent) ||
			!IsLiveHealthStateObject(EventInstigator) ||
			!Pawn->HasAuthority() ||
			!DeadController->HasAuthority() ||
			!EventInstigator->HasAuthority() ||
			Pawn->Controller != DeadController ||
			DeadController->PlayerState != DeadPlayerState ||
			DeadPlayerState->AbilitySystemComponent !=
				AbilitySystemComponent)
		{
			return false;
		}

		if (DeadController->HasPawn() &&
			DeadController->Pawn != Pawn)
		{
			return false;
		}
		if (DeadController->HasMyFortPawn() &&
			DeadController->MyFortPawn != Pawn)
		{
			return false;
		}
		return true;
	}

	bool QueryMatchingGameplayTag27_11(
		UAbilitySystemComponent* AbilitySystemComponent,
		const wchar_t* TagName,
		bool& HasTag)
	{
		HasTag = false;
		if (VersionInfo.FortniteVersion != 27.11 ||
			!IsLiveHealthStateObject(
				AbilitySystemComponent) ||
			!TagName)
		{
			return false;
		}

		auto Function =
			AbilitySystemComponent->GetFunction(
				"HasMatchingGameplayTag");
		if (!Function)
			return false;

		const auto Params = Function->GetParamsNamed();
		const UFunction::ParamNamed* TagParam = nullptr;
		const UFunction::ParamNamed* ReturnParam = nullptr;
		for (const auto& Param : Params.NameOffsetMap)
		{
			if (Param.Name == "TagToCheck")
				TagParam = &Param;
			else if (Param.Name == "ReturnValue")
				ReturnParam = &Param;
		}
		constexpr uint64 CPF_Parm = 0x80;
		constexpr uint64 CPF_ReturnParm = 0x400;
		const uint32 TagSize =
			static_cast<uint32>(FGameplayTag::Size());
		const bool bSchemaValid =
			Params.Size > 0 &&
			Params.Size <= 0x40 &&
			Function->GetPropertiesSize() ==
				Params.Size &&
			Params.NameOffsetMap.size() == 2 &&
			TagParam &&
			TagParam->ElementSize == TagSize &&
			TagParam->Offset <= Params.Size &&
			TagSize <= Params.Size - TagParam->Offset &&
			(TagParam->PropertyFlags & CPF_Parm) &&
			!(TagParam->PropertyFlags & CPF_ReturnParm) &&
			ReturnParam &&
			ReturnParam->ElementSize == sizeof(bool) &&
			ReturnParam->Offset <= Params.Size &&
			sizeof(bool) <=
				Params.Size - ReturnParam->Offset &&
			(ReturnParam->PropertyFlags & CPF_Parm) &&
			(ReturnParam->PropertyFlags & CPF_ReturnParm);
		if (!bSchemaValid)
			return false;

		FGameplayTag Tag{};
		Tag.TagName = FName(TagName);
		if (!Tag.TagName.IsValid())
			return false;

		std::vector<uint8> ParamMemory(
			Params.Size,
			0);
		memcpy(
			ParamMemory.data() + TagParam->Offset,
			&Tag,
			TagSize);
		AbilitySystemComponent->ProcessEvent(
			Function,
			ParamMemory.data());
		memcpy(
			&HasTag,
			ParamMemory.data() + ReturnParam->Offset,
			sizeof(HasTag));
		return true;
	}

	bool AreDBNOOwnedTagsCleared27_11(
		UAbilitySystemComponent* AbilitySystemComponent,
		bool& QueryAvailable)
	{
		QueryAvailable = false;
		bool HasDBNOTag = false;
		bool HasAthenaDBNOTag = false;
		if (!QueryMatchingGameplayTag27_11(
				AbilitySystemComponent,
				L"Gameplay.Action.Player.DBNO",
				HasDBNOTag) ||
			!IsLiveHealthStateObject(
				AbilitySystemComponent) ||
			!QueryMatchingGameplayTag27_11(
				AbilitySystemComponent,
				L"Gameplay.Action.Player.DBNOAthena",
				HasAthenaDBNOTag) ||
			!IsLiveHealthStateObject(
				AbilitySystemComponent))
		{
			return false;
		}
		QueryAvailable = true;
		return !HasDBNOTag && !HasAthenaDBNOTag;
	}

	bool ReadDBNOState27_11(
		AFortPlayerPawnAthena* Pawn,
		bool& StateAvailable)
	{
		StateAvailable = false;
		if (!IsLiveHealthStateObject(Pawn))
			return true;

		bool IsDBNO = false;
		if (Pawn->HasbIsDBNO())
		{
			StateAvailable = true;
			IsDBNO = Pawn->bIsDBNO;
		}

		auto Function = Pawn->GetFunction("IsDBNO");
		if (!Function)
			return IsDBNO;

		const auto Params = Function->GetParamsNamed();
		const auto* ReturnParam =
			Params.NameOffsetMap.size() == 1
				? &Params.NameOffsetMap[0]
				: nullptr;
		constexpr uint64 CPF_Parm = 0x80;
		constexpr uint64 CPF_ReturnParm = 0x400;
		const bool bSchemaValid =
			Function->GetPropertiesSize() ==
				sizeof(bool) &&
			Params.Size == sizeof(bool) &&
			ReturnParam &&
			ReturnParam->Name == "ReturnValue" &&
			ReturnParam->Offset == 0 &&
			ReturnParam->ElementSize == sizeof(bool) &&
			(ReturnParam->PropertyFlags & CPF_Parm) &&
			(ReturnParam->PropertyFlags & CPF_ReturnParm);
		if (!bSchemaValid)
			return IsDBNO;

		StateAvailable = true;
		return IsDBNO ||
			Pawn->Call<bool>(Function);
	}

	bool PerformManualDBNORevive27_11(
		AFortPlayerPawnAthena* Pawn,
		AFortPlayerControllerAthena* DeadController,
		AController* EventInstigator)
	{
		if (VersionInfo.FortniteVersion != 27.11 ||
			!IsLiveHealthStateObject(Pawn) ||
			!IsLiveHealthStateObject(DeadController) ||
			!IsLiveHealthStateObject(EventInstigator))
		{
			return false;
		}

		auto DeadPlayerState =
			DeadController->PlayerState
				? DeadController->PlayerState->Cast<
					AFortPlayerStateAthena>()
				: nullptr;
		auto AbilitySystemComponent =
			DeadPlayerState
				? DeadPlayerState->AbilitySystemComponent
				: nullptr;
		if (!HasStableManualReviveOwnership27_11(
				Pawn,
				DeadController,
				DeadPlayerState,
				AbilitySystemComponent,
				EventInstigator))
		{
			SDK::DbgLog(
				"[Revive] 27.11 manual fallback rejected "
				"unstable ownership pawn=%p controller=%p "
				"playerState=%p asc=%p\n",
				(void*)Pawn,
				(void*)DeadController,
				(void*)DeadPlayerState,
				(void*)AbilitySystemComponent);
			return false;
		}

		auto OnRepIsDBNOFunction =
			Pawn->GetFunction("OnRep_IsDBNO");
		auto ClientOnPawnRevivedFunction =
			DeadController->GetFunction(
				"ClientOnPawnRevived");
		const auto OnRepParams =
			OnRepIsDBNOFunction
				? OnRepIsDBNOFunction->GetParamsNamed()
				: UFunction::ParamsNamed{};
		const auto ClientRevivedParams =
			ClientOnPawnRevivedFunction
				? ClientOnPawnRevivedFunction->
					GetParamsNamed()
				: UFunction::ParamsNamed{};
		constexpr uint64 CPF_Parm = 0x80;
		constexpr uint64 CPF_ReturnParm = 0x400;
		const auto* ClientRevivedParam =
			ClientRevivedParams.NameOffsetMap.size() == 1
				? &ClientRevivedParams.NameOffsetMap[0]
				: nullptr;
		constexpr uint64 CASTCLASS_FObjectProperty =
			0x10000;
		auto ClientEventInstigatorProperty =
			ClientOnPawnRevivedFunction
				? ClientOnPawnRevivedFunction->
					GetProperty(
						"EventInstigator",
						CASTCLASS_FObjectProperty)
				: nullptr;
		const bool bNotificationSchemaValid =
			OnRepIsDBNOFunction &&
			OnRepIsDBNOFunction->GetPropertiesSize() == 0 &&
			OnRepParams.Size == 0 &&
			OnRepParams.NameOffsetMap.empty() &&
			ClientOnPawnRevivedFunction &&
			ClientOnPawnRevivedFunction->
				GetPropertiesSize() ==
					sizeof(EventInstigator) &&
			ClientRevivedParams.Size ==
				sizeof(EventInstigator) &&
			ClientRevivedParam &&
			ClientRevivedParam->Name ==
				"EventInstigator" &&
			ClientRevivedParam->Offset == 0 &&
			ClientRevivedParam->ElementSize ==
				sizeof(EventInstigator) &&
			(ClientRevivedParam->PropertyFlags &
				CPF_Parm) &&
			!(ClientRevivedParam->PropertyFlags &
				CPF_ReturnParm) &&
			ClientEventInstigatorProperty;
		if (!bNotificationSchemaValid)
		{
			SDK::DbgLog(
				"[Revive] 27.11 manual fallback rejected "
				"notification schema onRep=%p onRepSize=0x%X "
				"onRepFields=%d client=%p clientSize=0x%X "
				"clientFields=%d\n",
				(void*)OnRepIsDBNOFunction,
				OnRepParams.Size,
				(int)OnRepParams.NameOffsetMap.size(),
				(void*)ClientOnPawnRevivedFunction,
				ClientRevivedParams.Size,
				(int)ClientRevivedParams.
					NameOffsetMap.size());
			return false;
		}

		const char* CancelPreflightFailure = nullptr;
		if (!FindValidatedCancelDBNOAbilitiesFunction(
				AbilitySystemComponent,
				CancelPreflightFailure))
		{
			SDK::DbgLog(
				"[Revive] 27.11 manual fallback rejected "
				"cancel schema asc=%p reason=%s\n",
				(void*)AbilitySystemComponent,
				CancelPreflightFailure
					? CancelPreflightFailure
					: "unknown");
			return false;
		}

		// This event is the version-owned signal that releases the owning
		// client's DBNO ability, crawl/input restrictions, and gameplay cues.
		// Its payload and function layouts are reflected and checked before
		// dispatch by the helper; fail closed instead of producing a pawn that
		// only looks revived on the server.
		const bool bReviveEventDispatched =
			SendReviveGameplayEvent15_30(
				Pawn,
				AbilitySystemComponent,
				EventInstigator);
		if (!bReviveEventDispatched)
		{
			SDK::DbgLog(
				"[Revive] 27.11 manual fallback aborted "
				"before state clear pawn=%p controller=%p "
				"instigator=%p\n",
				(void*)Pawn,
				(void*)DeadController,
				(void*)EventInstigator);
			return false;
		}

		if (!HasStableManualReviveOwnership27_11(
				Pawn,
				DeadController,
				DeadPlayerState,
				AbilitySystemComponent,
				EventInstigator))
		{
			SDK::DbgLog(
				"[Revive] 27.11 manual fallback aborted "
				"after revive event ownership changed "
				"pawn=%p controller=%p\n",
				(void*)Pawn,
				(void*)DeadController);
			return false;
		}

		const bool bTagCancelDispatched =
			CancelDBNOAbilitiesByTag15_30(
				AbilitySystemComponent);
		if (!bTagCancelDispatched ||
			!HasStableManualReviveOwnership27_11(
				Pawn,
				DeadController,
				DeadPlayerState,
				AbilitySystemComponent,
				EventInstigator))
		{
			SDK::DbgLog(
				"[Revive] 27.11 manual fallback aborted "
				"after tag cancel dispatched=%d pawn=%p "
				"controller=%p\n",
				(int)bTagCancelDispatched,
				(void*)Pawn,
				(void*)DeadController);
			return false;
		}

		bool bTagQueryAvailableBeforeState = false;
		const bool bTagsClearedBeforeState =
			AreDBNOOwnedTagsCleared27_11(
				AbilitySystemComponent,
				bTagQueryAvailableBeforeState);
		if (!HasStableManualReviveOwnership27_11(
				Pawn,
				DeadController,
				DeadPlayerState,
				AbilitySystemComponent,
				EventInstigator) ||
			(bTagQueryAvailableBeforeState &&
				!bTagsClearedBeforeState))
		{
			SDK::DbgLog(
				"[Revive] 27.11 manual fallback aborted "
				"DBNO tags remain query=%d clear=%d "
				"pawn=%p asc=%p\n",
				(int)bTagQueryAvailableBeforeState,
				(int)bTagsClearedBeforeState,
				(void*)Pawn,
				(void*)AbilitySystemComponent);
			return false;
		}

		const bool bDeathInfoChanged =
			ClearReviveDeathInfo15_30(
				DeadPlayerState);
		if (!HasStableManualReviveOwnership27_11(
				Pawn,
				DeadController,
				DeadPlayerState,
				AbilitySystemComponent,
				EventInstigator))
		{
			SDK::DbgLog(
				"[Revive] 27.11 manual fallback aborted "
				"after death-info notification pawn=%p "
				"controller=%p\n",
				(void*)Pawn,
				(void*)DeadController);
			return false;
		}

		if (Pawn->HasbIsDBNO())
			Pawn->bIsDBNO = false;
		if (Pawn->HasbWasDBNOOnDeath())
			Pawn->bWasDBNOOnDeath = false;
		if (Pawn->HasbPlayedDying())
			Pawn->bPlayedDying = false;
		if (Pawn->HasbIsDying())
			Pawn->bIsDying = false;
		if (Pawn->HasbIsHiddenForDeath())
			Pawn->bIsHiddenForDeath = false;

		bool bRevivalStackCleared = false;
		const int32 PawnPropertiesSize =
			Pawn->Class
				? Pawn->Class->GetPropertiesSize()
				: 0;
		constexpr uint64 CASTCLASS_FByteProperty =
			0x40;
		for (const char* PropertyName :
			{ "DBNORevivalStacking",
				"DBNORevivingActorsCount" })
		{
			auto Property =
				Pawn->GetProperty(
					PropertyName,
					CASTCLASS_FByteProperty);
			if (!Property ||
				!SDK::MemReadable(
					Property,
					static_cast<size_t>((std::max)(
						Offsets::ElementSize,
						Offsets::Offset_Internal)) +
						sizeof(uint32)))
			{
				continue;
			}

			const uint32 PropertyOffset =
				DecryptPropOffset(
					GetFromOffset<uint32>(
						Property,
						Offsets::Offset_Internal));
			const uint32 PropertySize =
				GetFromOffset<uint32>(
					Property,
					Offsets::ElementSize);
			if (PropertyOffset == uint32(-1) ||
				PropertySize != sizeof(uint8) ||
				PawnPropertiesSize <= 0 ||
				PropertyOffset >
					static_cast<uint32>(
						PawnPropertiesSize) ||
				sizeof(uint8) >
					static_cast<uint32>(
						PawnPropertiesSize) -
						PropertyOffset)
			{
				continue;
			}

			auto Address =
				reinterpret_cast<uint8*>(Pawn) +
				PropertyOffset;
			if (!IsWritableReviveMemory(
					Address,
					sizeof(uint8)))
			{
				continue;
			}
			auto& Value = *Address;
			bRevivalStackCleared =
				bRevivalStackCleared || Value != 0;
			Value = 0;
		}

		const float MaxHealth = Pawn->GetMaxHealth();
		const float ReviveHealth =
			FPlatformMath::IsFinite(MaxHealth) &&
				MaxHealth > 0.f
				? (MaxHealth < 30.f
					? MaxHealth
					: 30.f)
				: 30.f;
		Pawn->SetHealth(ReviveHealth);
		if (!HasStableManualReviveOwnership27_11(
				Pawn,
				DeadController,
				DeadPlayerState,
				AbilitySystemComponent,
				EventInstigator))
		{
			return false;
		}
		Pawn->ProcessEvent(
			OnRepIsDBNOFunction,
			nullptr);
		if (!HasStableManualReviveOwnership27_11(
				Pawn,
				DeadController,
				DeadPlayerState,
				AbilitySystemComponent,
				EventInstigator))
		{
			SDK::DbgLog(
				"[Revive] 27.11 manual fallback aborted "
				"after DBNO notification pawn=%p "
				"controller=%p\n",
				(void*)Pawn,
				(void*)DeadController);
			return false;
		}

		if (DeadController->HasbMarkedAlive())
			DeadController->bMarkedAlive = true;
		if (DeadController->
				HasbClientNotifiedOfPawnDied())
		{
			DeadController->
				bClientNotifiedOfPawnDied = false;
		}

		DeadController->ProcessEvent(
			ClientOnPawnRevivedFunction,
			&EventInstigator);
		if (!HasStableManualReviveOwnership27_11(
				Pawn,
				DeadController,
				DeadPlayerState,
				AbilitySystemComponent,
				EventInstigator))
		{
			SDK::DbgLog(
				"[Revive] 27.11 manual fallback aborted "
				"after client notification pawn=%p "
				"controller=%p\n",
				(void*)Pawn,
				(void*)DeadController);
			return false;
		}
		Pawn->ForceNetUpdate();
		DeadPlayerState->ForceNetUpdate();
		DeadController->ForceNetUpdate();

		bool bDBNOStateAvailable = false;
		const bool bStillDBNO =
			ReadDBNOState27_11(
				Pawn,
				bDBNOStateAvailable);
		const bool bStableAfterDBNORead =
			HasStableManualReviveOwnership27_11(
				Pawn,
				DeadController,
				DeadPlayerState,
				AbilitySystemComponent,
				EventInstigator);
		bool bTagQueryAvailableAfterState = false;
		const bool bTagsClearedAfterState =
			bStableAfterDBNORead &&
			AreDBNOOwnedTagsCleared27_11(
				AbilitySystemComponent,
				bTagQueryAvailableAfterState);
		const bool bStablePostState =
			bStableAfterDBNORead &&
			HasStableManualReviveOwnership27_11(
				Pawn,
				DeadController,
				DeadPlayerState,
				AbilitySystemComponent,
				EventInstigator);
		const float FinalHealth =
			bStablePostState
				? Pawn->GetHealth()
				: 0.f;
		const bool bSucceeded =
			bStablePostState &&
			bDBNOStateAvailable &&
			!bStillDBNO &&
			FPlatformMath::IsFinite(FinalHealth) &&
			FinalHealth > 0.f &&
			(!bTagQueryAvailableAfterState ||
				bTagsClearedAfterState);
		SDK::DbgLog(
			"[Revive] 27.11 manual fallback result "
			"pawn=%p controller=%p instigator=%p "
			"event=%d tags=%d deathInfo=%d stack=%d "
			"tagQuery=%d/%d dbnoState=%d "
			"stillDBNO=%d stable=%d health=%.2f "
			"success=%d\n",
			(void*)Pawn,
			(void*)DeadController,
			(void*)EventInstigator,
			(int)bReviveEventDispatched,
			(int)bTagCancelDispatched,
			(int)bDeathInfoChanged,
			(int)bRevivalStackCleared,
			(int)bTagQueryAvailableAfterState,
			(int)bTagsClearedAfterState,
			(int)bDBNOStateAvailable,
			(int)bStillDBNO,
			(int)bStablePostState,
			FinalHealth,
			(int)bSucceeded);
		return bSucceeded;
	}

	bool PerformManualDBNORevive15_30(
		AFortPlayerPawnAthena* Pawn,
		AFortPlayerControllerAthena* DeadController,
		AController* EventInstigator)
	{
		auto DeadPlayerState =
			DeadController && DeadController->PlayerState
				? DeadController->PlayerState->Cast<
					AFortPlayerStateAthena>()
				: nullptr;
		auto AbilitySystemComponent =
			DeadPlayerState
				? DeadPlayerState->AbilitySystemComponent
				: nullptr;
		if (!DeadPlayerState || !AbilitySystemComponent)
		{
			SDK::DbgLog(
				"[Revive] 15.30 manual transition missing "
				"player state/ASC pawn=%p controller=%p "
				"playerState=%p asc=%p\n",
				(void*)Pawn,
				(void*)DeadController,
				(void*)DeadPlayerState,
				(void*)AbilitySystemComponent);
			return false;
		}

		const int32 ActivationInfoSize =
			FGameplayAbilityActivationInfo::Size();
		if (ActivationInfoSize <= 0 ||
			ActivationInfoSize > 0x100)
		{
			SDK::DbgLog(
				"[Revive] 15.30 invalid activation-info "
				"size=%d pawn=%p\n",
				ActivationInfoSize,
				(void*)Pawn);
			return false;
		}

		// The DBNO ability waits for this tag to release its client-owned
		// crawl/input state. Clearing bIsDBNO alone makes the server look
		// revived while the owning client remains functionally downed.
		const bool bReviveEventDispatched =
			SendReviveGameplayEvent15_30(
				Pawn,
				AbilitySystemComponent,
				EventInstigator);
		if (!bReviveEventDispatched)
		{
			SDK::DbgLog(
				"[Revive] 15.30 manual transition aborted "
				"before state clear pawn=%p controller=%p "
				"instigator=%p\n",
				(void*)Pawn,
				(void*)DeadController,
				(void*)EventInstigator);
			return false;
		}

		const bool bDeathInfoChanged =
			ClearReviveDeathInfo15_30(
				DeadPlayerState);
		const bool bTagCancelDispatched =
			CancelDBNOAbilitiesByTag15_30(
				AbilitySystemComponent);

		// The event and tag cancellation are the native revive path. Snapshot
		// only abilities that remain active afterward, so the fallback never
		// reuses activation state invalidated by either synchronous call.
		struct FPendingDBNOAbilityEnd
		{
			FGameplayAbilitySpecHandle Handle{};
			std::vector<uint8_t> ActivationInfoBytes;
		};
		std::vector<FPendingDBNOAbilityEnd>
			AbilitiesToEnd;
		auto& AbilityItems =
			AbilitySystemComponent->
				ActivatableAbilities.Items;
		for (int32 Index = 0;
			Index < AbilityItems.Num();
			++Index)
		{
			auto& Spec = AbilityItems.Get(
				Index,
				FGameplayAbilitySpec::Size());
			if (!IsDBNOAbility(Spec.Ability) ||
				(Spec.HasActiveCount() &&
					Spec.ActiveCount == 0))
			{
				continue;
			}

			FPendingDBNOAbilityEnd Pending{};
			Pending.Handle = Spec.Handle;
			Pending.ActivationInfoBytes.resize(
				ActivationInfoSize);
			memcpy(
				Pending.ActivationInfoBytes.data(),
				&Spec.ActivationInfo,
				ActivationInfoSize);
			AbilitiesToEnd.push_back(
				std::move(Pending));
		}

		for (auto& Pending : AbilitiesToEnd)
		{
			auto& ActivationInfo =
				*reinterpret_cast<
					FGameplayAbilityActivationInfo*>(
						Pending.ActivationInfoBytes.data());

			// This is a last-resort cleanup for a DBNO ability that ignored the
			// native event/tag path. Notify the predicted client copy and end
			// the still-active authoritative instance with its fresh key.
			AbilitySystemComponent->ClientCancelAbility(
				Pending.Handle,
				ActivationInfo);
			AbilitySystemComponent->ClientEndAbility(
				Pending.Handle,
				ActivationInfo);
			FPredictionKey EmptyPredictionKey{};
			auto PredictionKey =
				FGameplayAbilityActivationInfo::
					HasPredictionKeyWhenActivated()
					? &ActivationInfo.
						PredictionKeyWhenActivated
					: &EmptyPredictionKey;
			AbilitySystemComponent->ServerEndAbility(
				Pending.Handle,
				ActivationInfo,
				*PredictionKey);
		}

		std::vector<FActiveGameplayEffectHandle>
			DBNOEffectsToRemove;
		auto& ActiveEffects =
			AbilitySystemComponent->ActiveGameplayEffects.
				GameplayEffects_Internal;
		for (int32 Index = 0;
			Index < ActiveEffects.Num();
			++Index)
		{
			auto& ActiveEffect = ActiveEffects.Get(
				Index,
				FActiveGameplayEffect::Size());
			if (!IsDBNOEffect(ActiveEffect.Spec.Def))
				continue;

			FActiveGameplayEffectHandle Handle{};
			memcpy(
				&Handle,
				reinterpret_cast<const uint8*>(
					&ActiveEffect) + 0xC,
				sizeof(Handle));
			if (Handle.Handle > 0)
				DBNOEffectsToRemove.push_back(Handle);
		}

		int32 RemovedEffectCount = 0;
		auto RemoveActiveEffectFunction =
			AbilitySystemComponent->GetFunction(
				"RemoveActiveGameplayEffect");
		if (RemoveActiveEffectFunction)
		{
			for (auto& Handle : DBNOEffectsToRemove)
			{
				if (AbilitySystemComponent->Call<bool>(
						RemoveActiveEffectFunction,
						Handle,
						-1))
				{
					++RemovedEffectCount;
				}
			}
		}

		int32 RemainingActiveDBNOAbilities = 0;
		for (int32 Index = 0;
			Index < AbilityItems.Num();
			++Index)
		{
			auto& Spec = AbilityItems.Get(
				Index,
				FGameplayAbilitySpec::Size());
			if (IsDBNOAbility(Spec.Ability) &&
				(!Spec.HasActiveCount() ||
					Spec.ActiveCount > 0))
			{
				++RemainingActiveDBNOAbilities;
			}
		}
		int32 RemainingDBNOEffects = 0;
		for (int32 Index = 0;
			Index < ActiveEffects.Num();
			++Index)
		{
			auto& ActiveEffect = ActiveEffects.Get(
				Index,
				FActiveGameplayEffect::Size());
			if (IsDBNOEffect(ActiveEffect.Spec.Def))
				++RemainingDBNOEffects;
		}

		if (Pawn->HasbIsDBNO())
			Pawn->bIsDBNO = false;
		if (Pawn->HasbWasDBNOOnDeath())
			Pawn->bWasDBNOOnDeath = false;
		if (Pawn->HasbPlayedDying())
			Pawn->bPlayedDying = false;
		if (Pawn->HasbIsDying())
			Pawn->bIsDying = false;
		if (Pawn->HasbIsHiddenForDeath())
			Pawn->bIsHiddenForDeath = false;

		bool bRevivalStackCleared = false;
		for (const char* PropertyName :
			{ "DBNORevivalStacking",
				"DBNORevivingActorsCount" })
		{
			const uint32 PropertyOffset =
				Pawn->GetOffset(PropertyName);
			if (PropertyOffset == uint32(-1) ||
				PropertyOffset > 0x10000 ||
				!SDK::MemReadable(
					reinterpret_cast<uint8*>(Pawn) +
						PropertyOffset,
					sizeof(uint8)))
			{
				continue;
			}

			auto& Value = *reinterpret_cast<uint8*>(
				reinterpret_cast<uint8*>(Pawn) +
					PropertyOffset);
			bRevivalStackCleared =
				bRevivalStackCleared || Value != 0;
			Value = 0;
		}

		const float MaxHealth = Pawn->GetMaxHealth();
		const float ReviveHealth =
			FPlatformMath::IsFinite(MaxHealth) &&
				MaxHealth > 0.f
				? (MaxHealth < 30.f
					? MaxHealth
					: 30.f)
				: 30.f;
		Pawn->SetHealth(ReviveHealth);
		Pawn->OnRep_IsDBNO();

		if (DeadController->HasbMarkedAlive())
			DeadController->bMarkedAlive = true;
		if (DeadController->
				HasbClientNotifiedOfPawnDied())
		{
			DeadController->
				bClientNotifiedOfPawnDied = false;
		}

		DeadController->ClientOnPawnRevived(
			EventInstigator);
		Pawn->ForceNetUpdate();
		DeadPlayerState->ForceNetUpdate();
		DeadController->ForceNetUpdate();

		bool bStillDBNO =
			Pawn->HasbIsDBNO() && Pawn->bIsDBNO;
		if (auto IsDBNOFunction =
				Pawn->GetFunction("IsDBNO"))
		{
			bStillDBNO =
				bStillDBNO ||
				Pawn->Call<bool>(IsDBNOFunction);
		}
		const float FinalHealth = Pawn->GetHealth();
		const bool bSucceeded =
			bReviveEventDispatched &&
			!bStillDBNO &&
			RemainingActiveDBNOAbilities == 0 &&
			RemainingDBNOEffects == 0 &&
			FPlatformMath::IsFinite(FinalHealth) &&
			FinalHealth > 0.f;
		SDK::DbgLog(
			"[Revive] 15.30 manual transition pawn=%p "
			"controller=%p instigator=%p abilities=%d "
			"active=%d effects=%d/%d remaining=%d "
			"event=%d tags=%d deathInfo=%d stack=%d "
			"stillDBNO=%d health=%.2f success=%d\n",
			(void*)Pawn,
			(void*)DeadController,
			(void*)EventInstigator,
			(int)AbilitiesToEnd.size(),
			RemainingActiveDBNOAbilities,
			RemovedEffectCount,
			(int)DBNOEffectsToRemove.size(),
			RemainingDBNOEffects,
			(int)bReviveEventDispatched,
			(int)bTagCancelDispatched,
			(int)bDeathInfoChanged,
			(int)bRevivalStackCleared,
			(int)bStillDBNO,
			FinalHealth,
			(int)bSucceeded);
		return bSucceeded;
	}
}

bool AFortPlayerPawnAthena::EnsurePlayerMapIcon(
	AFortPlayerControllerAthena* Controller,
	AFortPlayerPawnAthena* Pawn)
{
	if (!FConfiguration::bPlayerMapIcons.load(
			std::memory_order_acquire) ||
		GPlayerMapIconSetupDisabled)
	{
		return false;
	}

	bool bConfigured = false;
	__try
	{
		bConfigured = EnsurePlayerMapIconUnsafe(
			Controller,
			Pawn);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		GPlayerMapIconSetupDisabled = true;
		if (!GWarnedPlayerMapIconException)
		{
			GWarnedPlayerMapIconException = true;
			SDK::DbgLog(
				"[PlayerMapIcons] reflected setup faulted and was disabled "
				"for pawn=%p on FN %.2f\n",
				(void*)Pawn,
				VersionInfo.FortniteVersion);
		}
	}

	return bConfigured;
}

bool AFortPlayerPawnAthena::SetMinimumHealthGodMode(
	AFortPlayerControllerAthena* Controller,
	bool bEnabled)
{
	auto World = UWorld::GetWorld();
	ResetMinimumHealthGodStatesForWorld(World);
	if (!World || !IsLiveHealthStateObject(Controller))
		return false;

	auto ExistingState =
		FindMinimumHealthGodState(Controller);
	if (!bEnabled)
	{
		if (!ExistingState)
			return true;

		RestoreMinimumHealthGodState(*ExistingState);
		*ExistingState = {};
		return true;
	}

	auto& State = ExistingState
		? *ExistingState
		: AddMinimumHealthGodState(Controller);
	auto Pawn = GetMinimumHealthGodPawn(Controller);
	return Pawn
		? ApplyMinimumHealthGodState(State, Pawn)
		: false;
}

bool AFortPlayerPawnAthena::HasMinimumHealthGodMode(
	const AFortPlayerControllerAthena* Controller)
{
	ResetMinimumHealthGodStatesForWorld(UWorld::GetWorld());
	return FindMinimumHealthGodState(Controller) != nullptr;
}

bool AFortPlayerPawnAthena::HasMinimumHealthGodMode(
	const AFortPlayerPawnAthena* Pawn)
{
	ResetMinimumHealthGodStatesForWorld(UWorld::GetWorld());
	if (!Pawn)
		return false;

	AActor* PawnController =
		Pawn->HasController() ? Pawn->Controller : nullptr;
	for (auto& State : GMinimumHealthGodStates)
	{
		if (State.AppliedPawn.Get() == Pawn ||
			(PawnController &&
				reinterpret_cast<const UObject*>(
					State.Controller.Get()) ==
				reinterpret_cast<const UObject*>(
					PawnController)))
		{
			return true;
		}
	}
	return false;
}

bool AFortPlayerPawnAthena::HasFullHealthGodMode(
	const AFortPlayerPawnAthena* Pawn)
{
	if (!Pawn)
		return false;
	if (Pawn->HasbCanBeDamaged() &&
		!Pawn->bCanBeDamaged)
	{
		return true;
	}

	if (!Pawn->HasHealthSet() ||
		!Pawn->HealthSet ||
		!Pawn->HealthSet->HasHealth() ||
		!FFortGameplayAttributeData::StaticStruct() ||
		!FFortGameplayAttributeData::HasMinimum())
	{
		return false;
	}

	const float MaxHealth = Pawn->GetMaxHealth();
	const float Minimum =
		Pawn->HealthSet->Health.Minimum;
	return FPlatformMath::IsFinite(MaxHealth) &&
		MaxHealth > 1.f &&
		FPlatformMath::IsFinite(Minimum) &&
		std::abs(Minimum - MaxHealth) <=
			HealthStateEpsilon;
}

void AFortPlayerPawnAthena::TickHealthStateRepair(
	UNetDriver* Driver)
{
	auto World = UWorld::GetWorld();
	if (!Driver || !World || Driver != World->NetDriver)
		return;

	if (GTrackedHealthStateWorld.Get() != World)
	{
		GTrackedHealthStates = {};
		GTrackedHealthStateCursor = 0;
		GShieldRepairLogCount = 0;
		GTrackedHealthStateWorld = TWeakObjectPtr<UWorld>(World);
	}
	ResetMinimumHealthGodStatesForWorld(World);

	// Possession can legitimately report zero while the match is starting or
	// after it has ended. Restrict lethal finalization to active gameplay; the
	// invalid-shield pass is still reached for every live pawn below.
	const bool bCanFinalizeLethalState =
		IsHealthRepairMatchActive(World);
	const ULONGLONG CurrentTimeMs = GetTickCount64();
	std::array<
		AFortPlayerControllerAthena*,
		MaxTrackedHealthStates> ProcessedControllers{};
	size_t ProcessedControllerCount = 0;

	auto TickPlayerController =
		[&](AFortPlayerControllerAthena* PlayerController)
	{
		if (!IsLiveHealthStateObject(PlayerController) ||
			!PlayerController->IsA(
				AFortPlayerControllerAthena::StaticClass()))
		{
			return;
		}

		for (size_t Index = 0;
			Index < ProcessedControllerCount;
			++Index)
		{
			if (ProcessedControllers[Index] ==
				PlayerController)
			{
				return;
			}
		}
		if (ProcessedControllerCount <
			ProcessedControllers.size())
		{
			ProcessedControllers[
				ProcessedControllerCount++] =
				PlayerController;
		}

		AFortPlayerPawnAthena* Pawn = nullptr;
		if (PlayerController->HasPawn())
		{
			auto ControlledActor =
				reinterpret_cast<AActor*>(
					PlayerController->Pawn);
			// Guided missiles and vehicles temporarily replace the controlled
			// pawn while MyFortPawn still remembers the character. Never treat
			// that unpossessed character as a stuck lethal player.
			if (!IsLiveHealthStateObject(ControlledActor) ||
				!ControlledActor->IsA(
					AFortPlayerPawnAthena::StaticClass()))
			{
				return;
			}
			Pawn = static_cast<AFortPlayerPawnAthena*>(
				ControlledActor);
		}
		else if (PlayerController->HasMyFortPawn())
		{
			auto RememberedPawn =
				PlayerController->MyFortPawn;
			if (!IsLiveHealthStateObject(RememberedPawn) ||
				!RememberedPawn->IsA(
					AFortPlayerPawnAthena::StaticClass()))
			{
				return;
			}
			Pawn = RememberedPawn;
		}
		if (!IsLiveHealthStateObject(Pawn))
			return;
		if (PlayerController->HasMyFortPawn() &&
			IsLiveHealthStateObject(
				PlayerController->MyFortPawn) &&
			PlayerController->MyFortPawn != Pawn)
		{
			return;
		}

		auto AthenaController =
			static_cast<AFortPlayerControllerAthena*>(
				PlayerController);
		if (auto MinimumGodState =
				FindMinimumHealthGodState(AthenaController))
		{
			ApplyMinimumHealthGodState(
				*MinimumGodState, Pawn);
		}

		if (!bCanFinalizeLethalState)
		{
			// Let the helper observe/normalize positive and shield state, but
			// do not retain lethal-recovery state outside live gameplay.
			auto& State = FindTrackedHealthState(
				Pawn, PlayerController);
			const float Health = Pawn->GetHealth();
			const float MaxHealth = Pawn->GetMaxHealth();
			const float Shield = Pawn->GetShield();
			if (!FPlatformMath::IsFinite(Shield) ||
				Shield < 0.f)
			{
				Pawn->SetShield(0.f);
				if (!IsLiveHealthStateObject(Pawn))
					return;
				Pawn->ForceNetUpdate();
			}
			if (FPlatformMath::IsFinite(Health) &&
				Health > HealthStateEpsilon &&
				FPlatformMath::IsFinite(MaxHealth) &&
				MaxHealth > HealthStateEpsilon)
			{
				State.bObservedAlive = true;
			}
			else
			{
				State.bObservedAlive = false;
			}
			State.LastForceKillAttemptMs = 0;
			State.ConsecutiveUnresolvedZeroFlushes = 0;
			State.ForceKillAttempts = 0;
			State.bZeroStateLogged = false;
			return;
		}

		RepairPossessedPawnHealthState(
			PlayerController,
			Pawn,
			CurrentTimeMs);
	};

	auto TickConnection =
		[&](UNetConnection* Connection)
	{
		if (!Connection ||
			!SDK::MemReadable(Connection, 0x40))
		{
			return;
		}

		TickPlayerController(
			Connection->PlayerController);
	};

	for (auto Connection : Driver->ClientConnections)
	{
		TickConnection(Connection);
		if (!Connection)
			continue;

		for (auto ChildConnection : Connection->Children)
			TickConnection(ChildConnection);
	}

	// A listen-server host has no server-side client connection. AlivePlayers
	// is the authoritative match list, so run the same pre-replication repair
	// for any controller not already reached above.
	if (IsLiveHealthStateObject(World->AuthorityGameMode) &&
		World->AuthorityGameMode->IsA(
			AFortGameMode::StaticClass()))
	{
		auto GameMode = static_cast<AFortGameMode*>(
			World->AuthorityGameMode);
		if (GameMode->HasAlivePlayers())
		{
			for (auto PlayerActor : GameMode->AlivePlayers)
			{
				if (IsLiveHealthStateObject(PlayerActor) &&
					PlayerActor->IsA(
						AFortPlayerControllerAthena::
							StaticClass()))
				{
					TickPlayerController(
						static_cast<
							AFortPlayerControllerAthena*>(
								PlayerActor));
				}
			}
		}
	}
}

struct FFortPickupRequestInfo final
{
public:
	struct FGuid                                  SwapWithItem;                                      // 0x0000(0x0010)(ZeroConstructor, IsPlainOldData, NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic)
	float                                         FlyTime;                                           // 0x0010(0x0004)(ZeroConstructor, IsPlainOldData, NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic)
	struct _Pad_0xC                               Direction;                                         // 0x0014(0x000C)(ZeroConstructor, IsPlainOldData, NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic)
	uint8                                         bPlayPickupSound : 1;                              // 0x0020(0x0001)(BitIndex: 0x00, PropSize: 0x0001 (NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic))
	uint8                                         bIsAutoPickup : 1;                                 // 0x0020(0x0001)(BitIndex: 0x01, PropSize: 0x0001 (NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic))
	uint8                                         bUseRequestedSwap : 1;                             // 0x0020(0x0001)(BitIndex: 0x02, PropSize: 0x0001 (NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic))
	uint8                                         bTrySwapWithWeapon : 1;                            // 0x0020(0x0001)(BitIndex: 0x03, PropSize: 0x0001 (NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic))
	uint8                                         Pad_21[0x3];                                       // 0x0021(0x0003)(Fixing Struct Size After Last Property [ Dumper-7 ])
};

struct alignas(0x8) FFortPickupRequestInfoNew final
{
public:
	struct FGuid                                  SwapWithItem;                                      // 0x0000(0x0010)(ZeroConstructor, IsPlainOldData, NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic)
	float                                         FlyTime;                                           // 0x0010(0x0004)(ZeroConstructor, IsPlainOldData, NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic)
	uint8 Pad_1[0x4];
	struct _Pad_0x18                              Direction;                                        // 0x0014(0x000C)(ZeroConstructor, IsPlainOldData, NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic)
	uint8                                         bPlayPickupSound : 1;                              // 0x0020(0x0001)(BitIndex: 0x00, PropSize: 0x0001 (NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic))
	uint8                                         bIsAutoPickup : 1;                                 // 0x0020(0x0001)(BitIndex: 0x01, PropSize: 0x0001 (NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic))
	uint8                                         bUseRequestedSwap : 1;                             // 0x0020(0x0001)(BitIndex: 0x02, PropSize: 0x0001 (NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic))
	uint8                                         bTrySwapWithWeapon : 1;                            // 0x0020(0x0001)(BitIndex: 0x03, PropSize: 0x0001 (NoDestructor, HasGetValueTypeHash, NativeAccessSpecifierPublic))
	uint8                                         Pad_2[0x7];                                       // 0x0021(0x0003)(Fixing Struct Size After Last Property [ Dumper-7 ])
};

uint64_t SetPickupTarget_ = 0;

static bool StagePickupTargetManually(
	AFortPlayerPawnAthena* Pawn,
	AFortPickupAthena* Pickup,
	float RequestedFlyTime,
	FVector StartDirection,
	bool bPlayPickupSound)
{
	if (!Pawn || !Pickup ||
		!Pickup->HasPickupLocationData() ||
		!Pickup->HasbPickedUp())
	{
		return false;
	}

	float PickupSpeed = Pawn->HasPickupSpeedMultiplier()
		? Pawn->PickupSpeedMultiplier
		: 1.f;
	if (!FPlatformMath::IsFinite(PickupSpeed) ||
		PickupSpeed <= 0.f)
	{
		PickupSpeed = 1.f;
	}

	float FlyTime = RequestedFlyTime;
	if (!FPlatformMath::IsFinite(FlyTime) || FlyTime <= 0.f)
		FlyTime = 0.4f;
	FlyTime /= PickupSpeed;

	// SetPickupTarget is absent on a few early builds. This is their original
	// replicated pickup setup. The spline hook completes inventory when it is
	// available; otherwise the caller completes inventory immediately while
	// leaving this actor alive long enough to finish the client animation.
	auto& LocationData = Pickup->PickupLocationData;
	if (!LocationData.HasPickupTarget())
		return false;
	Pickup->SetLifeSpan(5.f);
	if (FFortPickupLocationData::HasbPlayPickupSound())
		LocationData.bPlayPickupSound = bPlayPickupSound;
	if (FFortPickupLocationData::HasFlyTime())
		LocationData.FlyTime = FlyTime;
	if (LocationData.HasItemOwner())
		LocationData.ItemOwner = Pawn;
	if (FFortPickupLocationData::HasPickupGuid())
		LocationData.PickupGuid =
			Pickup->PrimaryPickupItemEntry.ItemGuid;
	LocationData.PickupTarget = Pawn;
	if (FFortPickupLocationData::HasStartDirection())
		LocationData.StartDirection = StartDirection;
	if (Pawn->HasIncomingPickups())
		Pawn->IncomingPickups.Add(Pickup);
	Pickup->OnRep_PickupLocationData();

	Pickup->bPickedUp = true;
	Pickup->OnRep_bPickedUp();
	Pickup->ForceNetUpdate();

	SDK::DbgLog(
		"[Pickup] manual animated target staging "
		"pickup=%p pawn=%p flyTime=%.3f FN=%.2f\n",
		(void*)Pickup,
		(void*)Pawn,
		FlyTime,
		VersionInfo.FortniteVersion);
	return true;
}

static bool IsWaxGameModePickup(const AActor* Actor)
{
	static const UClass* WaxPickupClass = nullptr;
	if (!WaxPickupClass)
		WaxPickupClass = FindClass("FortGameModePickup_Wax");

	return Actor && WaxPickupClass && Actor->IsA(WaxPickupClass);
}

static bool ShouldRejectAshtonWorldPickup(
	AFortPlayerPawnAthena* Pawn,
	AFortPickupAthena* Pickup)
{
	return Pickup &&
		FFortAthenaNativeLTMCompatibility::
			ShouldRejectAshtonPickup(
				Pawn,
				Pickup->PrimaryPickupItemEntry
					.ItemDefinition);
}

static bool ShouldBlockAshtonGenericWorldPickup(
	AFortPlayerPawnAthena* Pawn,
	AFortPickupAthena* Pickup)
{
	return Pickup &&
		FFortAthenaNativeLTMCompatibility::
			ShouldBlockAshtonGenericPickup(
				Pawn,
				Pickup->PrimaryPickupItemEntry
					.ItemDefinition);
}

static bool CompletePickupWithoutSpline(
	AFortPlayerPawnAthena* Pawn,
	AFortPickupAthena* Pickup,
	bool bPreserveAnimatedActor = false)
{
	if (FFortAthenaNativeLTMCompatibility::
			TryCollectWaxPickup(Pawn, Pickup))
	{
		return true;
	}

	if (!Pawn || !Pickup ||
		!Pickup->PrimaryPickupItemEntry.ItemDefinition)
		return false;

	auto PlayerController = Pawn->Controller
		? Pawn->Controller->Cast<AFortPlayerControllerAthena>()
		: nullptr;
	if (!PlayerController || !PlayerController->WorldInventory)
		return false;
	if (ShouldRejectAshtonWorldPickup(Pawn, Pickup))
		return true;
	if (FFortAthenaNativeLTMCompatibility::
			TryCompleteAshtonStonePickup(
				Pawn,
				Pickup,
				Pickup->PrimaryPickupItemEntry
					.ItemDefinition,
				"pickup-without-spline"))
	{
		return true;
	}
	if (FFortAthenaNativeLTMCompatibility::
			ShouldBlockAshtonGenericPickup(
				Pawn,
				Pickup->PrimaryPickupItemEntry
					.ItemDefinition))
	{
		return true;
	}

	// Animation initiation and authoritative inventory completion are separate.
	// If a native or reflected target was staged, leave its actor alive so the
	// client can finish the fly-to-player animation. Otherwise retain the
	// instant retirement fallback for layouts that expose neither path.
	if (PlayerController->HasbTryPickupSwap())
		PlayerController->bTryPickupSwap = false;
	if (!bPreserveAnimatedActor)
	{
		Pickup->bPickedUp = true;
		Pickup->OnRep_bPickedUp();
	}
	PlayerController->InternalPickup(&Pickup->PrimaryPickupItemEntry);
	Pickup->SetLifeSpan(
		bPreserveAnimatedActor ? 5.f : 0.01f);

	SDK::DbgLog("[Pickup] completion fallback item=%p count=%d animated=%d FN=%.2f\n",
		(void*)Pickup->PrimaryPickupItemEntry.ItemDefinition,
		Pickup->PrimaryPickupItemEntry.Count,
		(int)bPreserveAnimatedActor,
		VersionInfo.FortniteVersion);
	return true;
}

// Makes a shield value written straight into the attribute actually absorb
// damage on S4+ builds. The raw write only works on the earliest builds (1.7.2);
// by S4 the shield lives in the ability-system aggregator that native damage
// reads, and a struct write never reaches it. The game's own BR shield GE does
// reach it, so we apply that GE at level 0 - it activates the aggregator on the
// amount we already wrote and adds nothing. On 1.7.2 there is no such GE and the
// raw write already absorbs, so finding nothing here is the correct no-op.
// The BR shield-grant GE, cached. GE_Athena_Shields (Ch1) / GE_Athena_*_Shields
// (Ch2). Null on 1.7.2, where the raw write already absorbs. Skips the
// damage/tag/default-object variants.
static UClass* FindShieldAbsorbGE()
{
	static UClass* ShieldGE = nullptr;
	static bool bSearched = false;

	if (!bSearched)
	{
		bSearched = true;

		int total = TUObjectArray::Num();
		for (int i = 0; i < total; i++)
		{
			auto obj = TUObjectArray::GetObjectByIndex(i);
			if (!obj)
				continue;

			std::string nm = obj->Name.ToString().c_str();

			if (nm.rfind("GE_Athena", 0) == 0 &&
				nm.find("Shield") != std::string::npos &&
				!nm.empty() && nm.back() == 'C' &&
				nm.find("Damage") == std::string::npos &&
				nm.find("Default__") == std::string::npos)
			{
				ShieldGE = (UClass*)obj;
				break;
			}
		}
	}

	return ShieldGE;
}

// True when SetShield will route through the GE. The GE grants a flat +1 shield
// when applied, so SetShield pre-subtracts 1 from its raw write to land on the
// exact requested value.
bool AFortPlayerPawnAthena::ShieldAbsorbUsesGE() const
{
	return FindShieldAbsorbGE() != nullptr;
}

void AFortPlayerPawnAthena::ActivateShieldAbsorb() const
{
	auto ShieldGE = FindShieldAbsorbGE();
	if (!ShieldGE)
		return;

	auto PlayerStateAthena = (AFortPlayerStateAthena*)this->PlayerState;
	if (!PlayerStateAthena)
		return;

	auto ASC = PlayerStateAthena->AbilitySystemComponent;
	if (!ASC)
		return;

	auto Context = ASC->MakeEffectContext();
	Context.Instigator = (AActor*)this->Controller;
	Context.Causer = (AActor*)this;
	Context.AddSourceObject((AActor*)this);
	ASC->BP_ApplyGameplayEffectToSelf(ShieldGE, 0.f, Context);
}

void AFortPlayerPawnAthena::ServerHandlePickup_(UObject* Context, FFrame& Stack)
{
	AFortPickupAthena* Pickup;
	float InFlyTime;
	FVector InStartDirection;
	bool bPlayPickupSound;
	Stack.StepCompiledIn(&Pickup);
	Stack.StepCompiledIn(&InFlyTime);
	Stack.StepCompiledIn(&InStartDirection);
	Stack.StepCompiledIn(&bPlayPickupSound);
	Stack.IncrementCode();
	auto Pawn = (AFortPlayerPawnAthena*)Context;
	if (FFortAthenaNativeLTMCompatibility::
			TryCollectWaxPickup(Pawn, Pickup))
	{
		return;
	}
	if (ShouldRejectAshtonWorldPickup(Pawn, Pickup))
		return;
	// A validated villain stone still uses the normal server world-pickup
	// stage so SetPickupTarget can drive its authored spline. The stone is
	// intercepted at spline completion and never reaches InternalPickup.
	if (!Pawn || !Pickup || Pickup->bPickedUp)
		return;

	if (!SetPickupTarget_)
	{
		const bool bAnimationStaged =
			StagePickupTargetManually(
				Pawn,
				Pickup,
				InFlyTime,
				InStartDirection,
				bPlayPickupSound);
		if (!bAnimationStaged ||
			!FinishedTargetSplineOG)
		{
			CompletePickupWithoutSpline(
				Pawn,
				Pickup,
				bAnimationStaged);
		}
		return;
	}

	/*Pickup->SetLifeSpan(5.f);
	if (FFortPickupLocationData::HasbPlayPickupSound())
		Pickup->PickupLocationData.bPlayPickupSound = bPlayPickupSound;
	Pickup->PickupLocationData.PickupTarget = Pawn;
	Pickup->PickupLocationData.StartDirection = InStartDirection;
	Pickup->PickupLocationData.FlyTime /= Pawn->PickupSpeedMultiplier;
	Pickup->OnRep_PickupLocationData();

	Pickup->bPickedUp = true;
	Pickup->OnRep_bPickedUp();


	Pawn->IncomingPickups.Add(Pickup);*/
	auto SetPickupTarget = (void(*&)(AFortPickupAthena*, AFortPlayerPawnAthena*, float, FVector, bool))SetPickupTarget_;

	SetPickupTarget(Pickup, Pawn, InFlyTime / (Pawn->HasPickupSpeedMultiplier() ? Pawn->PickupSpeedMultiplier : 1), InStartDirection, bPlayPickupSound);
	if (!FinishedTargetSplineOG)
		CompletePickupWithoutSpline(Pawn, Pickup, true);
}

void AFortPlayerPawnAthena::ServerHandlePickupInfo(UObject* Context, FFrame& Stack)
{
	bool bTrySwapWithWeapon;
	bool bUseRequestedSwap;
	bool bPlayPickupSound;
	FGuid SwapWithItem;
	float FlyTime;
	FVector Direction;

	AFortPickupAthena* Pickup;
	Stack.StepCompiledIn(&Pickup);
	if (VersionInfo.FortniteVersion >= 20.00)
	{
		FFortPickupRequestInfoNew Params;
		Stack.StepCompiledIn(&Params);
		bTrySwapWithWeapon = Params.bTrySwapWithWeapon;
		bUseRequestedSwap = Params.bUseRequestedSwap;
		bPlayPickupSound = Params.bPlayPickupSound;
		SwapWithItem = Params.SwapWithItem;
		FlyTime = Params.FlyTime;
		Direction = *(FVector*)&Params.Direction;
	}
	else
	{
		FFortPickupRequestInfo Params;
		Stack.StepCompiledIn(&Params);
		bTrySwapWithWeapon = Params.bTrySwapWithWeapon;
		bUseRequestedSwap = Params.bUseRequestedSwap;
		bPlayPickupSound = Params.bPlayPickupSound;
		SwapWithItem = Params.SwapWithItem;
		FlyTime = Params.FlyTime;
		Direction = *(FVector*)&Params.Direction;
	}
	Stack.IncrementCode();
	auto Pawn = (AFortPlayerPawnAthena*)Context;

	if (FFortAthenaNativeLTMCompatibility::
			TryCollectWaxPickup(Pawn, Pickup))
	{
		return;
	}
	if (ShouldRejectAshtonWorldPickup(Pawn, Pickup))
		return;
	// Preserve FortGameModePickup's world-target stage for an eligible
	// Chitauri; FinishedTargetSpline owns objective completion.
	if (!Pawn || !Pickup || Pickup->bPickedUp)
		return;

	auto PlayerController = Pawn->Controller
		? Pawn->Controller->Cast<AFortPlayerControllerAthena>()
		: nullptr;
	if (FinishedTargetSplineOG &&
		PlayerController &&
		bUseRequestedSwap &&
		Pawn->CurrentWeapon &&
		AFortInventory::IsPrimaryQuickbar(
			((AFortWeapon*)Pawn->CurrentWeapon)->WeaponData) &&
		AFortInventory::IsPrimaryQuickbar(
			Pickup->PrimaryPickupItemEntry.ItemDefinition))
	{
		/*auto SwapEntry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
			{ return entry.ItemGuid == SwapWithItem; }, FFortItemEntry::Size());
		PlayerController->SwappingItemDefinition = SwapEntry; // proper af*/
		PlayerController->bTryPickupSwap = true;
	}

	if (!SetPickupTarget_)
	{
		const bool bAnimationStaged =
			StagePickupTargetManually(
				Pawn,
				Pickup,
				FlyTime,
				Direction,
				bPlayPickupSound);
		if (!bAnimationStaged ||
			!FinishedTargetSplineOG)
		{
			CompletePickupWithoutSpline(
				Pawn,
				Pickup,
				bAnimationStaged);
		}
		return;
	}

	auto SetPickupTarget = (void(*&)(AFortPickupAthena*, AFortPlayerPawnAthena*, float, FVector&, bool))SetPickupTarget_;

	SetPickupTarget(Pickup, Pawn, FlyTime / (Pawn->HasPickupSpeedMultiplier() ? Pawn->PickupSpeedMultiplier : 1), Direction, bPlayPickupSound);
	if (!FinishedTargetSplineOG)
		CompletePickupWithoutSpline(Pawn, Pickup, true);
	/*Pickup->SetLifeSpan(5.f);
	Pickup->PickupLocationData.bPlayPickupSound = bPlayPickupSound;
	Pickup->PickupLocationData.PickupGuid = Pickup->PrimaryPickupItemEntry.ItemGuid;
	Pickup->PickupLocationData.PickupTarget = Pawn;
	Pickup->PickupLocationData.FlyTime /= Pawn->PickupSpeedMultiplier;
	//Pickup->PickupLocationData.StartDirection = Params.Direction.QuantizeNormal();
	Pickup->OnRep_PickupLocationData();

	Pickup->bPickedUp = true;
	Pickup->OnRep_bPickedUp();


	Pawn->IncomingPickups.Add(Pickup);*/
}


void AFortPlayerPawnAthena::ServerHandlePickupWithRequestedSwap(UObject* Context, FFrame& Stack)
{
	AFortPickupAthena* Pickup;
	FGuid Swap;
	float InFlyTime;
	FVector InStartDirection;
	bool bPlayPickupSound;
	Stack.StepCompiledIn(&Pickup);
	Stack.StepCompiledIn(&Swap);
	Stack.StepCompiledIn(&InFlyTime);
	Stack.StepCompiledIn(&InStartDirection);
	Stack.StepCompiledIn(&bPlayPickupSound);
	Stack.IncrementCode();

	auto Pawn = (AFortPlayerPawnAthena*)Context;

	if (FFortAthenaNativeLTMCompatibility::
			TryCollectWaxPickup(Pawn, Pickup))
	{
		return;
	}
	if (ShouldRejectAshtonWorldPickup(Pawn, Pickup))
		return;
	// Requested-swap input cannot inventory an Ashton stone. It may only
	// advance the same server-authored target spline as the normal handler.
	if (!Pawn || !Pickup || Pickup->bPickedUp)
		return;

	auto PlayerController = (AFortPlayerControllerAthena*)Pawn->Controller;

	if (FinishedTargetSplineOG && PlayerController)
		PlayerController->bTryPickupSwap = true;

	if (!SetPickupTarget_)
	{
		const bool bAnimationStaged =
			StagePickupTargetManually(
				Pawn,
				Pickup,
				InFlyTime,
				InStartDirection,
				bPlayPickupSound);
		if (!bAnimationStaged ||
			!FinishedTargetSplineOG)
		{
			CompletePickupWithoutSpline(
				Pawn,
				Pickup,
				bAnimationStaged);
		}
		return;
	}

	auto SetPickupTarget = (void(*&)(AFortPickupAthena*, AFortPlayerPawnAthena*, float, FVector&, bool))SetPickupTarget_;

	SetPickupTarget(Pickup, Pawn, InFlyTime / (Pawn->HasPickupSpeedMultiplier() ? Pawn->PickupSpeedMultiplier : 1), InStartDirection, bPlayPickupSound);
	if (!FinishedTargetSplineOG)
		CompletePickupWithoutSpline(Pawn, Pickup, true);
	/*Pickup->SetLifeSpan(5.f);
	Pickup->PickupLocationData.bPlayPickupSound = bPlayPickupSound;
	Pickup->PickupLocationData.PickupGuid = Pickup->PrimaryPickupItemEntry.ItemGuid;
	Pickup->PickupLocationData.PickupTarget = Pawn;
	Pickup->PickupLocationData.FlyTime /= Pawn->PickupSpeedMultiplier;
	//Pickup->PickupLocationData.StartDirection = Params.Direction.QuantizeNormal();
	Pickup->OnRep_PickupLocationData();

	Pickup->bPickedUp = true;
	Pickup->OnRep_bPickedUp();


	Pawn->IncomingPickups.Add(Pickup);*/
}


bool AFortPlayerPawnAthena::FinishedTargetSpline(void* _Pickup)
{
	auto Pickup = (AFortPickupAthena*)_Pickup;
	auto Pawn = Pickup
		? (AFortPlayerPawnAthena*)
			Pickup->PickupLocationData.PickupTarget
		: nullptr;
	if (FFortAthenaNativeLTMCompatibility::
			TryCollectWaxPickup(Pawn, Pickup))
	{
		return true;
	}
	if (ShouldRejectAshtonWorldPickup(Pawn, Pickup))
	{
		Pickup->bPickedUp = false;
		Pickup->OnRep_bPickedUp();
		return false;
	}
	if (Pawn &&
		FFortAthenaNativeLTMCompatibility::
			TryCompleteAshtonStonePickup(
				Pawn,
				Pickup,
				Pickup->PrimaryPickupItemEntry
					.ItemDefinition,
				"finished-target-spline"))
	{
		return true;
	}
	if (ShouldBlockAshtonGenericWorldPickup(Pawn, Pickup))
	{
		if (Pickup->HasbPickedUp() &&
			Pickup->bPickedUp)
		{
			Pickup->bPickedUp = false;
			Pickup->OnRep_bPickedUp();
		}
		return false;
	}
	if (!Pawn)
		return FinishedTargetSplineOG(Pickup);

	auto PlayerController = (AFortPlayerControllerAthena*)Pawn->Controller;
	if (!PlayerController)
		return FinishedTargetSplineOG(Pickup);
	//if (auto entry = PlayerController->HasSwappingItemDefinition() ? (FFortItemEntry*)PlayerController->SwappingItemDefinition : nullptr)
	if (PlayerController->HasbTryPickupSwap() ? PlayerController->bTryPickupSwap : false)
	{
		FVector FinalLoc = Pawn->K2_GetActorLocation();

		FVector ForwardVector = Pawn->GetActorForwardVector();
		ForwardVector.Z = 0.0f;
		ForwardVector.Normalize();

		FinalLoc = FinalLoc + ForwardVector * 450.f;
		FinalLoc.Z += 50.f;

		const float RandomAngleVariation = ((float)rand() * 0.00109866634f) - 18.f;
		const float FinalAngle = RandomAngleVariation * 0.017453292519943295f;

		FinalLoc.X += cos(FinalAngle) * 100.f;
		FinalLoc.Y += sin(FinalAngle) * 100.f;

		if (AFortInventory::IsPrimaryQuickbar(((AFortWeapon*)Pawn->CurrentWeapon)->WeaponData) && AFortInventory::IsPrimaryQuickbar(Pickup->PrimaryPickupItemEntry.ItemDefinition))
		{
			PlayerController->bTryPickupSwap = false;

			auto entry = PlayerController->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
				{ return entry.ItemGuid == ((AFortWeapon*)PlayerController->Pawn->CurrentWeapon)->ItemEntryGuid; }, FFortItemEntry::Size());

			AFortInventory::SpawnPickup(PlayerController->GetViewTarget()->K2_GetActorLocation(), *entry, EFortPickupSourceTypeFlag::GetPlayer(), EFortPickupSpawnSource::GetUnset(), PlayerController->MyFortPawn, -1, true, true, true, nullptr, FinalLoc);
			// SwapEntry(PC, *entry, Pickup->PrimaryPickupItemEntry);
			PlayerController->WorldInventory->Remove(entry->ItemGuid);
			auto Item = PlayerController->WorldInventory->GiveItem(Pickup->PrimaryPickupItemEntry);
			PlayerController->ServerExecuteInventoryItem(Item->ItemEntry.ItemGuid);
			/*if (VersionInfo.FortniteVersion < 3)
			{
				auto& QuickBar = (AFortInventory::IsPrimaryQuickbar(Item->ItemEntry.ItemDefinition) || Item->ItemEntry.ItemDefinition->ItemType == EFortItemType::GetWeaponHarvest()) ? PlayerController->QuickBars->PrimaryQuickBar : PlayerController->QuickBars->SecondaryQuickBar;
				int i = 0;
				for (i = 0; i < QuickBar.Slots.Num(); i++)
				{
					auto& Slot = QuickBar.Slots.Get(i, FQuickBarSlot::Size());

					for (auto& SlotItem : Slot.Items)
						if (SlotItem == Item->ItemEntry.ItemGuid)
						{
							PlayerController->QuickBars->ServerActivateSlotInternal(!(AFortInventory::IsPrimaryQuickbar(Item->ItemEntry.ItemDefinition) || Item->ItemEntry.ItemDefinition->ItemType == EFortItemType::GetWeaponHarvest()), i, 0.f, true);
							break;
						}
				}
			}
			else
				PlayerController->ClientEquipItem(Item->ItemEntry.ItemGuid, true);*/
		}
		else
			AFortInventory::SpawnPickup(PlayerController->GetViewTarget()->K2_GetActorLocation(), Pickup->PrimaryPickupItemEntry, EFortPickupSourceTypeFlag::GetPlayer(), EFortPickupSpawnSource::GetUnset(), PlayerController->MyFortPawn, -1, true, true, true, nullptr, FinalLoc);

	}
	else
		PlayerController->InternalPickup(&Pickup->PrimaryPickupItemEntry);

	return FinishedTargetSplineOG(Pickup);
}


uint64_t OnRep_ZiplineState = 0;
void AFortPlayerPawnAthena::ServerSendZiplineState(UObject* Context, FFrame& Stack)
{
	FZiplinePawnState State;

	Stack.StepCompiledIn(&State);
	Stack.IncrementCode();

	auto Pawn = (AFortPlayerPawnAthena*)Context;

	if (!Pawn)
		return;

	auto Zipline = Pawn->GetActiveZipline();

	auto PreviousState = Pawn->ZiplineState;

	memcpy((PBYTE)&Pawn->ZiplineState, (const PBYTE)&State, FZiplinePawnState::Size());

	if (OnRep_ZiplineState)
		((void (*)(AFortPlayerPawnAthena*)) OnRep_ZiplineState)(Pawn);

	if (State.bJumped)
	{
		auto Velocity = Pawn->CharacterMovement->Velocity;
		auto VelocityX = Velocity.X * -0.5f;
		auto VelocityY = Velocity.Y * -0.5f;
		Pawn->LaunchCharacterJump(FVector{ VelocityX >= -750 ? min(VelocityX, 750) : -750, VelocityY >= -750 ? min(VelocityY, 750) : -750, 1200 }, false, false, true, true);
	}

	auto NewZipline = Pawn->GetActiveZipline();

	static auto ZipLineClass = FindObject<UClass>(L"/Ascender/Gameplay/Ascender/B_Athena_Zipline_Ascender.B_Athena_Zipline_Ascender_C");
	if (auto Ascender = Zipline->Cast<AFortAscenderZipline>(ZipLineClass))
	{
		Ascender->PawnUsingHandle = nullptr;
		Ascender->PreviousPawnUsingHandle = Pawn;
		Ascender->OnRep_PawnUsingHandle();
	}
	else if (auto Ascender = NewZipline->Cast<AFortAscenderZipline>(ZipLineClass))
	{
		Ascender->PawnUsingHandle = Pawn;
		Ascender->PreviousPawnUsingHandle = nullptr;
		Ascender->OnRep_PawnUsingHandle();
	}
}


void AFortPlayerPawnAthena::OnCapsuleBeginOverlap_(UObject* Context, FFrame& Stack)
{
	UObject* OverlappedComp;
	AActor* OtherActor;
	UObject* OtherComp;
	int32 OtherBodyIndex;
	bool bFromSweep;
	struct { uint8_t Padding[0x100]; } SweepResult;
	Stack.StepCompiledIn(&OverlappedComp);
	Stack.StepCompiledIn(&OtherActor);
	Stack.StepCompiledIn(&OtherComp);
	Stack.StepCompiledIn(&OtherBodyIndex);
	Stack.StepCompiledIn(&bFromSweep);
	Stack.StepCompiledIn(&SweepResult);
	Stack.IncrementCode();

	auto Pawn = (AFortPlayerPawnAthena*)Context;

	if (IsWaxGameModePickup(OtherActor))
	{
		return callOG(Pawn, Stack.GetCurrentNativeFunction(), OnCapsuleBeginOverlap, OverlappedComp, OtherActor, OtherComp, OtherBodyIndex, bFromSweep, SweepResult);
	}

	static auto FortPCClass = FindClass("FortPlayerController");

	if (!Pawn || !Pawn->Controller || !Pawn->Controller->IsA(FortPCClass))
		return callOG(Pawn, Stack.GetCurrentNativeFunction(), OnCapsuleBeginOverlap, OverlappedComp, OtherActor, OtherComp, OtherBodyIndex, bFromSweep, SweepResult);

	auto Pickup = OtherActor->Cast<AFortPickupAthena>();
	if (!Pickup || !Pickup->PrimaryPickupItemEntry.ItemDefinition || !((AFortPlayerControllerAthena*)Pawn->Controller)->WorldInventory)
		return callOG(Pawn, Stack.GetCurrentNativeFunction(), OnCapsuleBeginOverlap, OverlappedComp, OtherActor, OtherComp, OtherBodyIndex, bFromSweep, SweepResult);

	auto MaxStack = Pickup->PrimaryPickupItemEntry.ItemDefinition->GetMaxStackSize();
	auto itemEntry = ((AFortPlayerControllerAthena*)Pawn->Controller)->WorldInventory->Inventory.ReplicatedEntries.Search([&](FFortItemEntry& entry)
		{ return entry.ItemDefinition == Pickup->PrimaryPickupItemEntry.ItemDefinition && entry.Count <= MaxStack; }, FFortItemEntry::Size());

	if (Pickup && Pickup->PawnWhoDroppedPickup != Pawn)
	{
		if ((!itemEntry && ((Pickup->PrimaryPickupItemEntry.ItemDefinition->HasbForceAutoPickup() && (Pickup->PrimaryPickupItemEntry.ItemDefinition->HasbForceAutoPickup() ? Pickup->PrimaryPickupItemEntry.ItemDefinition->bForceAutoPickup : (Pickup->PrimaryPickupItemEntry.ItemDefinition->GetPickupComponent() ? Pickup->PrimaryPickupItemEntry.ItemDefinition->GetPickupComponent()->bForceAutoPickup : false))) || !AFortInventory::IsPrimaryQuickbar(Pickup->PrimaryPickupItemEntry.ItemDefinition))) || (itemEntry && itemEntry->Count < MaxStack))
			Pawn->ServerHandlePickup(Pickup, Pickup->PickupLocationData.FlyTime, FVector(), true);
	}

	if (OtherActor && OtherActor->Name.ToString().contains("Launch_Pad"))
	{
		Pawn->LaunchCharacterJump(FVector(0.f, 0.f, 0.f), false, nullptr, true);
	}

	return callOG(Pawn, Stack.GetCurrentNativeFunction(), OnCapsuleBeginOverlap, OverlappedComp, OtherActor, OtherComp, OtherBodyIndex, bFromSweep, SweepResult);
}


void AFortPlayerPawnAthena::MovingEmoteStopped(UObject* Context, FFrame& Stack)
{
	Stack.IncrementCode();
	auto Pawn = (AFortPlayerPawnAthena*)Context;

	if (Pawn->HasbIsPlayingEmote() && Pawn->bIsPlayingEmote)
		return;

	static auto HasbMovingEmote = Pawn->HasbMovingEmote();
	if (HasbMovingEmote)
		Pawn->bMovingEmote = false;

	static auto HasbMovingEmoteForwardOnly = Pawn->HasbMovingEmoteForwardOnly();
	if (HasbMovingEmoteForwardOnly)
		Pawn->bMovingEmoteForwardOnly = false;

	static auto HasbMovingEmoteFollowingOnly = Pawn->HasbMovingEmoteFollowingOnly();
	if (HasbMovingEmoteFollowingOnly)
		Pawn->bMovingEmoteFollowingOnly = false;

	if (Pawn->HasLastReplicatedEmoteExecuted())
	{
		auto OldEmote = Pawn->LastReplicatedEmoteExecuted;
		Pawn->LastReplicatedEmoteExecuted = nullptr;
		Pawn->OnRep_LastReplicatedEmoteExecuted(OldEmote);
	}
}

class UGA_Athena_MedConsumable_Parent_C : public UObject
{
public:
	UCLASS_COMMON_MEMBERS(UGA_Athena_MedConsumable_Parent_C);

	DEFINE_PROP(PlayerPawn, AFortPlayerPawnAthena*);
	DEFINE_PROP(HealsShields, bool);
	DEFINE_PROP(HealsHealth, bool);
	DEFINE_PROP(HealthHealAmount, float);
};

void AFortPlayerPawnAthena::Athena_MedConsumable_Triggered(UObject* Context, FFrame& Stack)
{
	UGA_Athena_MedConsumable_Parent_C* Consumable = (UGA_Athena_MedConsumable_Parent_C*)Context;

	printf("Called yo\n");
	if (!Consumable || (!Consumable->HealsShields && !Consumable->HealsHealth) || !Consumable->PlayerPawn)
		return Athena_MedConsumable_TriggeredOG(Context, Stack);

	auto PlayerState = (AFortPlayerStateAthena*)Consumable->PlayerPawn->PlayerState;
	static auto ShieldCue = FName(L"GameplayCue.Shield.PotionConsumed");
	static auto HealthCue = FName(L"GameplayCue.Athena.Health.HealUsed");

	auto Handle = PlayerState->AbilitySystemComponent->MakeEffectContext();
	FGameplayTag Tag{};
	FName CueName = Consumable->HealsShields ? ShieldCue : HealthCue;

	if (Consumable->HealsHealth && Consumable->HealsShields)
	{
		static auto HealthHealAmountOffset = Consumable->GetOffset("HealthHealAmount");
		auto HealthHealAmount = Consumable->HasHealthHealAmount() ? *(float*)(__int64(Consumable) + HealthHealAmountOffset) : *(double*)(__int64(Consumable) + HealthHealAmountOffset);
		if (Consumable->PlayerPawn->GetHealth() + HealthHealAmount <= 100)
			CueName = HealthCue;
	}
	Tag.TagName = CueName;

	auto PredictionKey = (FPredictionKey*)malloc(FPredictionKey::Size());
	memset((PBYTE)PredictionKey, 0, FPredictionKey::Size());

	PlayerState->AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded(Tag, *PredictionKey, Handle);
	PlayerState->AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted(Tag, *PredictionKey, Handle);

	free(PredictionKey);

	return Athena_MedConsumable_TriggeredOG(Context, Stack);
}

void AFortPlayerPawnAthena::ServerOnExitVehicle_(
	UObject* Context, FFrame& Stack, AActor** Ret)
{
	struct FVehicleExitData { uint8_t Pad[0x30]; };

	FVehicleExitData VehicleExitData{};
	uint8_t ExitForceBehavior = 0;
	bool bDestroyVehicleWhenForced = false;
	bool bHasDestroyVehicleWhenForced = false;
	bool bHasReturnValue = false;

	auto ExitFunction = Stack.GetCurrentNativeFunction();
	if (!ExitFunction)
	{
		if (Ret)
			*Ret = nullptr;
		return;
	}

	{
		auto Params = ExitFunction->GetParamsNamed();
		for (const auto& Param : Params.NameOffsetMap)
		{
			if (Param.Name == "bDestroyVehicleWhenForced")
				bHasDestroyVehicleWhenForced = true;
			if (Param.Name == "ReturnValue" ||
				(Param.PropertyFlags & 0x400) != 0)
			{
				bHasReturnValue = true;
			}
		}
	}

	if (VersionInfo.FortniteVersion >= 29)
		Stack.StepCompiledIn(&VehicleExitData);
	else
	{
		Stack.StepCompiledIn(&ExitForceBehavior);
		if (bHasDestroyVehicleWhenForced)
			Stack.StepCompiledIn(&bDestroyVehicleWhenForced);
	}

	Stack.IncrementCode();

	auto Pawn = (AFortPlayerPawnAthena*)Context;

	if (!Pawn)
	{
		if (Ret)
			*Ret = nullptr;
		return;
	}

	auto PlayerController = (AFortPlayerControllerAthena*)Pawn->Controller;
	AActor* ExitedVehicle = nullptr;

	if (VersionInfo.FortniteVersion >= 29)
	{
		ExitedVehicle = callOGWithRet(
			Pawn, ExitFunction, ServerOnExitVehicle,
			VehicleExitData);
	}
	else if (bHasDestroyVehicleWhenForced)
	{
		ExitedVehicle = callOGWithRet(
			Pawn, ExitFunction, ServerOnExitVehicle,
			ExitForceBehavior, bDestroyVehicleWhenForced);
	}
	else
	{
		// FN10.40 has one legacy input plus an object ReturnValue. Stepping or
		// forwarding a second bool consumes bytecode that belongs to the caller
		// and discards native's authoritative exit-success result.
		ExitedVehicle = callOGWithRet(
			Pawn, ExitFunction, ServerOnExitVehicle,
			ExitForceBehavior);
	}

	if (Ret)
		*Ret = ExitedVehicle;

	// Native can reject this RPC (notably while the FN10.40 B.R.U.T.E. swaps
	// driver/gunner possession). When this build exposes the returned vehicle,
	// null is an explicit rejection. The controller helper then verifies that
	// the rider's authoritative slot was actually cleared.
	if (!bHasReturnValue || ExitedVehicle)
	{
		AFortPlayerControllerAthena::RestoreVehicleLoadoutAfterExit(
			PlayerController);
	}
}

void AFortPlayerPawnAthena::EmoteStopped_(UObject* Context, FFrame& Stack)
{
	UObject* MontageItemDef;

	Stack.StepCompiledIn(&MontageItemDef);
	Stack.IncrementCode();
	auto Pawn = (AFortPlayerPawnAthena*)Context;

	if (Pawn->HasLastReplicatedEmoteExecuted() && Pawn->LastReplicatedEmoteExecuted == MontageItemDef)
	{
		auto OldEmote = Pawn->LastReplicatedEmoteExecuted;
		Pawn->LastReplicatedEmoteExecuted = nullptr;
		Pawn->OnRep_LastReplicatedEmoteExecuted(OldEmote);
	}

	return callOG(Pawn, Stack.GetCurrentNativeFunction(), EmoteStopped, MontageItemDef);
}

void AFortPlayerPawnAthena::EndSkydiving(AFortPlayerPawnAthena* Pawn)
{
	auto PlayerControllerClass =
		AFortPlayerControllerAthena::StaticClass();
	auto ResolveCurrentPlayerController =
		[PlayerControllerClass](AFortPlayerPawnAthena* CurrentPawn)
			-> AFortPlayerControllerAthena*
	{
		if (!IsLiveHealthStateObject(CurrentPawn) ||
			!PlayerControllerClass)
		{
			return nullptr;
		}

		auto Controller = CurrentPawn->Controller;
		if (!IsLiveHealthStateObject(Controller) ||
			!Controller->IsA(PlayerControllerClass))
		{
			return nullptr;
		}

		auto PlayerController =
			(AFortPlayerControllerAthena*)Controller;
		auto CurrentFortPawn = PlayerController->MyFortPawn;
		if (CurrentFortPawn != CurrentPawn &&
			IsLiveHealthStateObject(CurrentFortPawn))
		{
			return nullptr;
		}

		return CurrentFortPawn == CurrentPawn ||
			(!IsLiveHealthStateObject(CurrentFortPawn) &&
				PlayerController->Pawn == CurrentPawn)
			? PlayerController
			: nullptr;
	};

	auto PlayerController = ResolveCurrentPlayerController(Pawn);
	AFortPlayerControllerAthena::CaptureLandingItemBeforeNativeEnd(
		PlayerController, Pawn);

	EndSkydivingOG(Pawn);

	// Native may unpossess or replace the pawn. Resolve ownership again instead
	// of using a pre-native controller pointer for restore and quest work.
	PlayerController = ResolveCurrentPlayerController(Pawn);
	AFortPlayerControllerAthena::FinalizeRespawnAfterLanding(PlayerController, Pawn);

	if (PlayerController && Pawn->bIsSkydiving)
		PlayerController->GetQuestManager(1)->SendStatEvent(PlayerController, EFortQuestObjectiveStatEvent::GetLand(), 1, Pawn);

	if (PlayerController && Pawn && Pawn->bIsSkydivingFromBus)
	{
		PlayerController->GetQuestManager(1)->SendStatEvent(PlayerController, EFortQuestObjectiveStatEvent::GetVisit(), 1, Pawn);
	}
}

bool AFortPlayerPawnAthena::ReviveFromDBNOCompat(
	AFortPlayerPawnAthena* Pawn,
	AController* EventInstigator)
{
	if (!IsLiveHealthStateObject(Pawn) ||
		!IsLiveHealthStateObject(EventInstigator) ||
		Pawn->IsDefaultObject() ||
		!Pawn->HasAuthority() ||
		!Pawn->HasController())
	{
		SDK::DbgLog(
			"[Revive] rejected invalid transition pawn=%p "
			"instigator=%p version=%.2f\n",
			(void*)Pawn,
			(void*)EventInstigator,
			VersionInfo.FortniteVersion);
		return false;
	}

	auto ControllerClass = FindClass("Controller");
	if (!ControllerClass ||
		!EventInstigator->IsA(ControllerClass))
	{
		SDK::DbgLog(
			"[Revive] rejected non-controller instigator "
			"pawn=%p instigator=%p version=%.2f\n",
			(void*)Pawn,
			(void*)EventInstigator,
			VersionInfo.FortniteVersion);
		return false;
	}

	auto DeadController =
		Pawn->Controller
			? Pawn->Controller->Cast<
				AFortPlayerControllerAthena>()
			: nullptr;
	if (!IsLiveHealthStateObject(DeadController) ||
		(DeadController->HasPawn() &&
			DeadController->Pawn &&
			DeadController->Pawn != Pawn) ||
		(DeadController->HasMyFortPawn() &&
			DeadController->MyFortPawn &&
			DeadController->MyFortPawn != Pawn))
	{
		SDK::DbgLog(
			"[Revive] rejected stale pawn ownership pawn=%p "
			"controller=%p controllerPawn=%p fortPawn=%p "
			"version=%.2f\n",
			(void*)Pawn,
			(void*)DeadController,
			(void*)(DeadController &&
				DeadController->HasPawn()
					? DeadController->Pawn
					: nullptr),
			(void*)(DeadController &&
				DeadController->HasMyFortPawn()
					? DeadController->MyFortPawn
					: nullptr),
			VersionInfo.FortniteVersion);
		return false;
	}

	const bool bWasDBNO =
		Pawn->HasbIsDBNO()
			? Pawn->bIsDBNO
			: (Pawn->GetFunction("IsDBNO") &&
				Pawn->Call<bool>(
					Pawn->GetFunction("IsDBNO")));
	if (!bWasDBNO)
	{
		SDK::DbgLog(
			"[Revive] ignored non-DBNO pawn=%p controller=%p "
			"version=%.2f\n",
			(void*)Pawn,
			(void*)DeadController,
			VersionInfo.FortniteVersion);
		return false;
	}

	// 15.30's reflected ReviveFromDBNO is only a wrapper back into the hooked
	// server RPC. Forwarding that nested RPC to its saved exec prevents the
	// stack overflow but performs no state transition on this build. Use the
	// native-equivalent, same-pawn cleanup here and never enter respawn.
	if (VersionInfo.FortniteVersion == 15.30)
	{
		return PerformManualDBNORevive15_30(
			Pawn,
			DeadController,
			EventInstigator);
	}
	if (VersionInfo.FortniteVersion == 27.11)
	{
		return PerformManualDBNORevive27_11(
			Pawn,
			DeadController,
			EventInstigator);
	}

	// ReviveFromDBNO is inherited from FortPlayerPawn and keeps this exact pawn
	// possessed. It also owns the version-correct teammate/self revive gameplay
	// effect, set-by-caller health, DBNO ability cleanup, cues, and client
	// notification. Calling any death-respawn API after it creates a second pawn.
	auto NativeReviveFunction =
		Pawn->GetFunction("ReviveFromDBNO");
	if (!NativeReviveFunction)
	{
		SDK::DbgLog(
			"[Revive] native ReviveFromDBNO capability missing "
			"pawn=%p; checking force capability version=%.2f\n",
			(void*)Pawn,
			VersionInfo.FortniteVersion);
	}

	// ReviveFromDBNO is a lower-level transition on some builds, but on 15.30
	// its reflected path dispatches ServerReviveFromDBNO again. Mark this
	// synchronous call so the nested server exec can use its original handler
	// instead of recursing through this compatibility helper.
	const bool bNativeLowerInvoked =
		NativeReviveFunction != nullptr;
	if (NativeReviveFunction)
	{
		FScopedReviveCompatCall ScopedCompatCall;
		Pawn->Call<void>(
			NativeReviveFunction,
			EventInstigator);
	}

	if (!IsLiveHealthStateObject(Pawn))
		return true;

	auto ReadDBNOState =
		[Pawn]()
		{
			bool bIsDBNO =
				Pawn->HasbIsDBNO() && Pawn->bIsDBNO;
			if (auto IsDBNOFunction =
					Pawn->GetFunction("IsDBNO"))
			{
				bIsDBNO =
					bIsDBNO ||
					Pawn->Call<bool>(IsDBNOFunction);
			}
			return bIsDBNO;
		};

	bool bStillDBNO = ReadDBNOState();
	float FinalHealth = Pawn->GetHealth();
	bool bSucceeded =
		!bStillDBNO &&
		FPlatformMath::IsFinite(FinalHealth) &&
		FinalHealth > 0.f;
	bool bForceReviveInvoked = false;

	// Some builds expose a complete authority-side fallback separately from
	// ReviveFromDBNO. ForceReviveFromDBNO can take no parameters on older
	// builds and one EventInstigator controller on newer builds such as 32.11;
	// validate either reflected schema before invoking it instead of rebuilding
	// version-sensitive GAS/effect layouts.
	if (!bSucceeded && bStillDBNO)
	{
		auto ForceReviveFunction =
			Pawn->GetFunction("ForceReviveFromDBNO");
		if (ForceReviveFunction)
		{
			const auto ForceParams =
				ForceReviveFunction->GetParamsNamed();
			constexpr uint64 CPF_Parm = 0x80;
			constexpr uint64 CPF_ReturnParm = 0x400;
			const bool bZeroParameterSchema =
				ForceReviveFunction->GetPropertiesSize() == 0 &&
				ForceParams.Size == 0 &&
				ForceParams.NameOffsetMap.empty();
			const UFunction::ParamNamed*
				ForceInstigatorParam = nullptr;
			for (const auto& Param :
				ForceParams.NameOffsetMap)
			{
				if (Param.Name == "EventInstigator")
					ForceInstigatorParam = &Param;
			}
			// FN32 encrypts PropertiesSize/ElementSize/PropertyFlags, but keeps
			// reflected parameter names and offsets usable.
			const bool bEncryptedParameterMetadata =
				VersionInfo.FortniteVersion >= 32.00;
			const bool bControllerParameterSchema =
				ForceParams.NameOffsetMap.size() == 1 &&
				ForceInstigatorParam &&
				ForceInstigatorParam->Offset == 0 &&
				(bEncryptedParameterMetadata ||
					(ForceReviveFunction->GetPropertiesSize() ==
							sizeof(EventInstigator) &&
						ForceParams.Size ==
							sizeof(EventInstigator) &&
						ForceInstigatorParam->ElementSize ==
							sizeof(EventInstigator) &&
						(ForceInstigatorParam->PropertyFlags &
							CPF_Parm) &&
						!(ForceInstigatorParam->PropertyFlags &
							CPF_ReturnParm)));
			if (bZeroParameterSchema ||
				bControllerParameterSchema)
			{
				if (bControllerParameterSchema)
				{
					Pawn->ProcessEvent(
						ForceReviveFunction,
						&EventInstigator);
				}
				else
				{
					Pawn->ProcessEvent(
						ForceReviveFunction,
						nullptr);
				}
				bForceReviveInvoked = true;
				if (!IsLiveHealthStateObject(Pawn))
					return true;

				bStillDBNO = ReadDBNOState();
				FinalHealth = Pawn->GetHealth();
				bSucceeded =
					!bStillDBNO &&
					FPlatformMath::IsFinite(FinalHealth) &&
					FinalHealth > 0.f;
			}
			else
			{
				SDK::DbgLog(
					"[Revive] ForceReviveFromDBNO schema "
					"rejected pawn=%p size=0x%X fields=%d "
					"version=%.2f\n",
					(void*)Pawn,
					ForceParams.Size,
					(int)ForceParams.NameOffsetMap.size(),
					VersionInfo.FortniteVersion);
			}
		}
	}
	Pawn->ForceNetUpdate();
	SDK::DbgLog(
		"[Revive] native same-pawn transition pawn=%p "
		"controller=%p instigator=%p stillDBNO=%d "
		"health=%.2f lower=%d forced=%d success=%d "
		"version=%.2f\n",
		(void*)Pawn,
		(void*)DeadController,
		(void*)EventInstigator,
		(int)bStillDBNO,
		FinalHealth,
		(int)bNativeLowerInvoked,
		(int)bForceReviveInvoked,
		(int)bSucceeded,
		VersionInfo.FortniteVersion);
	return bSucceeded;
}

void AFortPlayerPawnAthena::ServerReviveFromDBNO_(UObject* Context, FFrame& Stack)
{
	// Keep the frame untouched: the original exec still needs to deserialize
	// EventInstigator. This path is reached when 15.30's reflected
	// ReviveFromDBNO wrapper dispatches the server RPC synchronously.
	if (GReviveCompatDepth > 0)
	{
		if (ServerReviveFromDBNO_OG &&
			ServerReviveFromDBNO_OG !=
				ServerReviveFromDBNO_)
		{
			return ServerReviveFromDBNO_OG(
				Context,
				Stack);
		}

		SDK::DbgLog(
			"[Revive] recursive server exec has no original "
			"handler context=%p version=%.2f\n",
			(void*)Context,
			VersionInfo.FortniteVersion);
		return;
	}

	auto Pawn = Context
		? Context->Cast<AFortPlayerPawnAthena>()
		: nullptr;
	// On a build without the lower-level capability, preserve its original RPC
	// implementation with the untouched stack instead of guessing a native
	// address or rebuilding version-sensitive GAS parameter layouts.
	if (!Pawn ||
		!Pawn->GetFunction("ReviveFromDBNO"))
	{
		SDK::DbgLog(
			"[Revive] passing RPC to native exec; compatible "
			"lower function unavailable pawn=%p version=%.2f\n",
			(void*)Pawn,
			VersionInfo.FortniteVersion);
		if (ServerReviveFromDBNO_OG &&
			ServerReviveFromDBNO_OG !=
				ServerReviveFromDBNO_)
			return ServerReviveFromDBNO_OG(Context, Stack);
		return;
	}

	AController* EventInstigator = nullptr;

	Stack.StepCompiledIn(&EventInstigator);
	Stack.IncrementCode();

	SDK::DbgLog(
		"[Revive] server exec received pawn=%p instigator=%p "
		"version=%.2f\n",
		(void*)Pawn,
		(void*)EventInstigator,
		VersionInfo.FortniteVersion);
	ReviveFromDBNOCompat(
		Pawn,
		EventInstigator);
}

void AFortPlayerPawnAthena::ServerThrowCarriedPlayer_(UObject* Context, FFrame& Stack)
{
	Stack.IncrementCode();
	auto Pawn = (AFortPlayerPawnAthena*)Context;

	callOG(Pawn, Stack.GetCurrentNativeFunction(), ServerThrowCarriedPlayer);
	Pawn->LocalThrowCarriedPlayer();
}

void AFortPlayerPawnAthena::ServerInterrogateDBNOPlayer_(UObject* Context, FFrame& Stack)
{
	AFortPlayerPawnAthena* InDBNOHoistee = nullptr;

	Stack.StepCompiledIn(&InDBNOHoistee);
	Stack.IncrementCode();
	auto Pawn = (AFortPlayerPawnAthena*)Context;

	// Preserve native interrogation behavior (flags/animation/cue) before doing the reveal.
	callOG(Pawn, Stack.GetCurrentNativeFunction(), ServerInterrogateDBNOPlayer, InDBNOHoistee);

	if (!Pawn || !InDBNOHoistee)
		return;

	auto Interrogator = Pawn->Controller ? Pawn->Controller->Cast<AFortPlayerControllerAthena>() : nullptr;
	if (!Interrogator)
		return;

	AFortPlayerControllerAthena::RevealInterrogatedTeam(Interrogator, (AActor*)InDBNOHoistee);
}

void AFortPlayerPawnAthena::PostLoadHook()
{
	SDK::DbgLog("  [PPA] 0 pre-finds\n");
	OnRep_ZiplineState = FindOnRep_ZiplineState();
	SetPickupTarget_ = FindSetPickupTarget();
	SDK::DbgLog("  [PPA] 1 finds done (ZS=%p SPT=%p)\n", (void*)OnRep_ZiplineState, (void*)SetPickupTarget_);

	auto ServerHandlePickupInfoFn = GetDefaultObj()->GetFunction("ServerHandlePickupInfo");

	if (ServerHandlePickupInfoFn)
		Utils::ExecHook(
			ServerHandlePickupInfoFn,
			ServerHandlePickupInfo,
			ServerHandlePickupInfoOG);
	else
	{
		Utils::ExecHook(
			GetDefaultObj()->GetFunction("ServerHandlePickup"),
			ServerHandlePickup_,
			ServerHandlePickup_OG);
		Utils::ExecHook(
			GetDefaultObj()->GetFunction(
				"ServerHandlePickupWithRequestedSwap"),
			ServerHandlePickupWithRequestedSwap,
			ServerHandlePickupWithRequestedSwapOG);
	}

	SDK::DbgLog("  [PPA] 1b pickup-info exechooks done, pre-FindFinishedTargetSpline\n");
	auto _fts = FindFinishedTargetSpline();
	SDK::DbgLog("  [PPA] 2 FindFinishedTargetSpline=%p\n", (void*)_fts);
	if (_fts)
		Utils::Hook(_fts, FinishedTargetSpline, FinishedTargetSplineOG);
	if (FinishedTargetSplineOG)
		SDK::DbgLog("  [PPA] 2b spline hook done\n");
	else
		SDK::DbgLog("  [PPA] 2b spline unavailable; immediate pickup completion enabled\n");
	Utils::ExecHook(GetDefaultObj()->GetFunction("OnCapsuleBeginOverlap"), OnCapsuleBeginOverlap_, OnCapsuleBeginOverlap_OG);
	SDK::DbgLog("  [PPA] 3 spline+overlap hooks done\n");

	Utils::ExecHook(GetDefaultObj()->GetFunction("ServerSendZiplineState"), ServerSendZiplineState);
	Utils::ExecHook(GetDefaultObj()->GetFunction("MovingEmoteStopped"), MovingEmoteStopped);

	Utils::ExecHook(GetDefaultObj()->GetFunction("ServerOnExitVehicle"), ServerOnExitVehicle_, ServerOnExitVehicle_OG);

	Utils::ExecHook(GetDefaultObj()->GetFunction("EmoteStopped"), EmoteStopped_, EmoteStopped_OG);

	SDK::DbgLog("  [PPA] 4 exec hooks done, pre-EndSkydiving\n");
	auto EndSkydivingFn = GetDefaultObj()->GetFunction("EndSkydiving");

	// 2.50 reflects EndSkydiving, but its reported legacy vtable slot does not
	// dispatch this hook in practice. The controller's observed-state landing
	// poll handles that build and avoids patching an unverified slot.
	if (EndSkydivingFn && VersionInfo.FortniteVersion != 2.50)
		Utils::Hook<AFortPlayerPawnAthena>(EndSkydivingFn->GetVTableIndex(), EndSkydiving, EndSkydivingOG);
	SDK::DbgLog("  [PPA] 5 EndSkydiving done fn=%p hooked=%d\n",
		(void*)EndSkydivingFn,
		(int)(EndSkydivingFn && VersionInfo.FortniteVersion != 2.50));

	auto ReviveFn = GetDefaultObj()->GetFunction("ServerReviveFromDBNO");

	if (ReviveFn)
	{
		// ServerReviveFromDBNO's parameters and implementation path move
		// between seasons. The native RPC is the only version-correct handler
		// for normal teammate interactions, so preserve it on every build
		// except the one confirmed exception below. Intercepting later builds
		// (27.11 in particular) and calling their lower ReviveFromDBNO wrapper
		// leaves the pawn downed and makes the client retry the RPC.
		bool bExecHooked = false;
		if (VersionInfo.FortniteVersion == 15.30 &&
			ReviveFn->ExecFunction !=
				reinterpret_cast<void*>(
					ServerReviveFromDBNO_))
		{
			Utils::ExecHook(
				ReviveFn,
				ServerReviveFromDBNO_,
				ServerReviveFromDBNO_OG);
			bExecHooked =
				ReviveFn->ExecFunction ==
					reinterpret_cast<void*>(
						ServerReviveFromDBNO_);
		}

		bool bNativeImplementationHooked = false;
		void* NativeImplementation = nullptr;
		uint32 ResolvedNativeSlot = uint32(-1);
		if (VersionInfo.FortniteVersion == 15.30)
		{
			// 15.30 exec thunk RVA 0x3402450 calls _Validate through
			// vtable byte offset 0xF30 (slot 486, bool), then calls the
			// void _Implementation through 0xF38 (slot 487). Normal
			// teammate interaction invokes slot 487 directly, bypassing
			// ExecFunction. Hook the typed implementation slot and do not
			// replay its recursive native body.
			auto DefaultPawn = GetDefaultObj();
			ResolvedNativeSlot =
				ReviveFn->GetVTableIndex();
			if (ResolvedNativeSlot ==
					ServerReviveFromDBNOImplementationSlot15_30 &&
				DefaultPawn && DefaultPawn->Vft)
			{
				NativeImplementation =
					DefaultPawn->Vft[
						ServerReviveFromDBNOImplementationSlot15_30];
				if (NativeImplementation !=
					reinterpret_cast<void*>(
						ServerReviveFromDBNOImplementation15_30))
				{
					Utils::Hook<AFortPlayerPawnAthena>(
						ServerReviveFromDBNOImplementationSlot15_30,
						ServerReviveFromDBNOImplementation15_30,
						GServerReviveFromDBNOImplementation15_30OG);
				}
				bNativeImplementationHooked =
					DefaultPawn->Vft[
						ServerReviveFromDBNOImplementationSlot15_30] ==
					reinterpret_cast<void*>(
						ServerReviveFromDBNOImplementation15_30);
			}
		}
		SDK::DbgLog(
			"  [PPA] revive dispatch execHooked=%d "
			"nativePreserved=%d native15.30=%d slot=%u "
			"resolved=%u original=%p\n",
			(int)bExecHooked,
			(int)(VersionInfo.FortniteVersion != 15.30),
			(int)bNativeImplementationHooked,
			ServerReviveFromDBNOImplementationSlot15_30,
			ResolvedNativeSlot,
			NativeImplementation);
	}
	else
		SDK::DbgLog("  [PPA] revive: ServerReviveFromDBNO not found on this version\n");
	Utils::ExecHook(GetDefaultObj()->GetFunction("ServerThrowCarriedPlayer"), ServerThrowCarriedPlayer_, ServerThrowCarriedPlayer_OG);

	// Shakedown - only present on builds that shipped DBNO interrogation, so guard the lookup.
	if (auto ServerInterrogateFn = GetDefaultObj()->GetFunction("ServerInterrogateDBNOPlayer"))
		Utils::ExecHook(ServerInterrogateFn, ServerInterrogateDBNOPlayer_, ServerInterrogateDBNOPlayer_OG);
	SDK::DbgLog("  [PPA] 6 PostLoadHook complete\n");
}
