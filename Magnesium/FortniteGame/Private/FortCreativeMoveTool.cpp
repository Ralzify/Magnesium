#include "pch.h"
#include "../Public/FortCreativeMoveTool.h"
#include "../Public/FortAthenaCreativePortal.h"
#include "../Public/FortGameStateAthena.h"
#include "../Public/BuildingFoundation.h"
#include "../Public/BuildingSMActor.h"
#include "../Public/FortLootPackage.h"
#include "../../Erbium/Support/Public/VersionFeatureAdapter.h"

extern uint64_t CantBuild_;

namespace
{
    constexpr int32 MaxCreativeSelection = 100;
    constexpr uint32 InvalidOffset = static_cast<uint32>(-1);

    bool UsesLegacyCreativePhoneLifecycle()
    {
        return VersionInfo.FortniteVersion > 0.0 && VersionInfo.FortniteVersion <= 10.40;
    }

    using FExecHandler = void (*)(UObject*, FFrame&);
    FExecHandler PhoneTargetStartInteractingOG = nullptr;
    FExecHandler PhoneTargetDuplicateStartInteractingOG = nullptr;
    FExecHandler PhoneTargetSpawnActorWithTransformOG = nullptr;
    UFunction* LegacySpawnSelectedFunction = nullptr;
    UFunction* ModernSpawnSelectedFunction = nullptr;
    FExecHandler LegacySpawnSelectedOG = nullptr;
    FExecHandler ModernSpawnSelectedOG = nullptr;
    UFunction* LegacyPlaceSelectionFunction = nullptr;
    UFunction* ModernPlaceSelectionFunction = nullptr;
    FExecHandler LegacyPlaceSelectionOG = nullptr;
    FExecHandler ModernPlaceSelectionOG = nullptr;
    UFunction* LegacyClearMovementFunction = nullptr;
    UFunction* ModernClearMovementFunction = nullptr;
    FExecHandler LegacyClearMovementOG = nullptr;
    FExecHandler ModernClearMovementOG = nullptr;
    UFunction* ObjectClearMovementFunction = nullptr;
    FExecHandler ObjectClearMovementOG = nullptr;
    UFunction* LegacyMoveSelectionFunction = nullptr;
    UFunction* ModernMoveSelectionFunction = nullptr;
    FExecHandler LegacyMoveSelectionOG = nullptr;
    FExecHandler ModernMoveSelectionOG = nullptr;

    struct FPendingStructuralDuplicate
    {
        UObject* InteractionOwner = nullptr;
        AActor* SpawnedActor = nullptr;
        FTransform OriginalTransform{};
    };
    std::vector<FPendingStructuralDuplicate> PendingStructuralDuplicates;

    struct FTrackedPropMobility
    {
        UObject* InteractionOwner = nullptr;
        AActor* Actor = nullptr;
        UObject* RootComponent = nullptr;
        uint8 OriginalMobility = 0;
    };
    std::vector<FTrackedPropMobility> TrackedPropMobilities;

    struct FCreativeMoveSchema
    {
        const UStruct* SelectedActorInfo = nullptr;
        const UStruct* SpawnPair = nullptr;

        int32 SelectedActorSize = 0;
        int32 SpawnPairSize = 0;

        uint32 SelectedActorActor = InvalidOffset;
        uint32 ActorToSelectionAtDragStart = InvalidOffset;
        bool bActorToSelectionIsUnscaled = false;
        uint32 ScaleAtDragStart = InvalidOffset;
        uint32 DragStartGridSnapPoint = InvalidOffset;
        uint32 OriginalRelevancyDistance = InvalidOffset;
        uint32 bWasCollisionEnabled = InvalidOffset;
        uint32 bWasDormant = InvalidOffset;
        uint32 bSpawnedFromSaveRecord = InvalidOffset;
        uint32 LogicalConnectionChainIndex = InvalidOffset;

        uint32 PairOriginalActor = InvalidOffset;
        uint32 PairSpawnedActor = InvalidOffset;
        uint32 bSpawnedActorIsForPreview = InvalidOffset;

        bool IsStartUsable() const
        {
            return SelectedActorInfo && SelectedActorSize >= 0x18 && SelectedActorSize <= 0x200 &&
                static_cast<uint32>(SelectedActorSize) >= static_cast<uint32>(FTransform::Size()) &&
                SelectedActorActor != InvalidOffset &&
                ActorToSelectionAtDragStart != InvalidOffset && SelectedActorActor <=
                    static_cast<uint32>(SelectedActorSize) - static_cast<uint32>(sizeof(AActor*)) &&
                ActorToSelectionAtDragStart <= static_cast<uint32>(SelectedActorSize) -
                        static_cast<uint32>(FTransform::Size());
        }

        bool IsSpawnPairUsable() const
        {
            return SpawnPair && SpawnPairSize >= 0x10 && SpawnPairSize <= 0x80 &&
                PairOriginalActor != InvalidOffset && PairSpawnedActor != InvalidOffset &&
                PairOriginalActor <= static_cast<uint32>(SpawnPairSize) -
                        static_cast<uint32>(sizeof(AActor*)) && PairSpawnedActor <=
                    static_cast<uint32>(SpawnPairSize) - static_cast<uint32>(sizeof(AActor*)) &&
                (bSpawnedActorIsForPreview == InvalidOffset || bSpawnedActorIsForPreview <
                    static_cast<uint32>(SpawnPairSize));
        }
    };

    bool IsLiveObject(const UObject* Object)
    {
        return Object && SDK::MemReadable(Object, sizeof(UObject)) &&
            Object->Class && SDK::MemReadable(Object->Class, sizeof(UObject));
    }

    bool ReadReflectedBool(UObject* Object, const char* PropertyName, bool DefaultValue = false)
    {
        if (!IsLiveObject(Object))
            return DefaultValue;
        auto Property = Object->GetProperty(PropertyName, 0x20000);
        const uint32 Offset = Object->GetOffset(PropertyName, 0x20000);
        if (!Property || Offset == InvalidOffset || Offset > 0x10000 || !SDK::MemReadable(
                reinterpret_cast<uint8*>(Object) + Offset, 1))
        {
            return DefaultValue;
        }
        uint8 Mask = Property->GetFieldMask();
        if (!Mask)
            Mask = 1;
        return (reinterpret_cast<uint8*>(Object)[Offset] & Mask) != 0;
    }

    bool WriteReflectedBool(UObject* Object, const char* PropertyName, bool Value)
    {
        if (!IsLiveObject(Object))
            return false;
        auto Property = Object->GetProperty(PropertyName, 0x20000);
        const uint32 Offset = Object->GetOffset(PropertyName, 0x20000);
        if (!Property || Offset == InvalidOffset || Offset > 0x10000 || !SDK::MemReadable(
                reinterpret_cast<uint8*>(Object) + Offset, 1))
        {
            return false;
        }
        uint8 Mask = Property->GetFieldMask();
        if (!Mask)
            Mask = 1;
        auto& Byte = reinterpret_cast<uint8*>(Object)[Offset];
        if (Value)
            Byte |= Mask;
        else
            Byte &= ~Mask;
        return true;
    }

    bool CopyReflectedScalar(UObject* Destination, UObject* Source, const char* PropertyName,
        size_t MaximumSize)
    {
        if (!IsLiveObject(Destination) || !IsLiveObject(Source) || !MaximumSize)
            return false;
        auto Property = Source->GetProperty(PropertyName);
        const uint32 SourceOffset = Source->GetOffset(PropertyName);
        const uint32 DestinationOffset = Destination->GetOffset(PropertyName);
        if (!Property || SourceOffset == InvalidOffset || DestinationOffset == InvalidOffset ||
            SourceOffset > 0x10000 || DestinationOffset > 0x10000)
        {
            return false;
        }
        size_t Size = 0;
        if (Offsets::ElementSize)
        {
            Size = GetFromOffset<uint32>(Property, Offsets::ElementSize);
        }
        if (!Size || Size > MaximumSize)
        {
            // OwnerPersistentID is int32 through 12.41 and int16 from 14.30 onward.
            Size = VersionInfo.FortniteVersion >= 14.30 ? 2 : 4;
        }
        auto SourceBytes = reinterpret_cast<uint8*>(Source) + SourceOffset;
        auto DestinationBytes = reinterpret_cast<uint8*>(Destination) + DestinationOffset;
        if (!SDK::MemReadable(SourceBytes, Size) || !SDK::MemReadable(DestinationBytes, Size))
            return false;
        memcpy(DestinationBytes, SourceBytes, Size);
        return true;
    }

    uint32 GetOffset(const UStruct* Struct, const char* PropertyName)
    {
        return Struct ? Struct->GetOffset(PropertyName) : InvalidOffset;
    }

    int32 AlignTo(uint32 Value, uint32 Alignment)
    {
        return static_cast<int32>((Value + Alignment - 1u) & ~(Alignment - 1u));
    }

    int32 InferSelectedActorSize(const FCreativeMoveSchema& Schema)
    {
        uint32 End = 0;
        auto Include = [&](uint32 Offset, uint32 Size)
        {
            if (Offset != InvalidOffset && Offset < 0x200)
                End = (std::max)(End, Offset + Size);
        };

        Include(Schema.SelectedActorActor, sizeof(AActor*));
        Include(Schema.ActorToSelectionAtDragStart, static_cast<uint32>(FTransform::Size()));
        Include(Schema.ScaleAtDragStart, static_cast<uint32>(FVector::Size()));
        Include(Schema.DragStartGridSnapPoint, static_cast<uint32>(FVector::Size()));
        Include(Schema.OriginalRelevancyDistance, sizeof(float));
        Include(Schema.bWasCollisionEnabled, sizeof(bool));
        Include(Schema.bWasDormant, sizeof(bool));
        Include(Schema.bSpawnedFromSaveRecord, sizeof(bool));
        Include(Schema.LogicalConnectionChainIndex, sizeof(int32));
        if (!End)
            return 0;

        // The native selected-actor entry is alignas(0x10), so keep its TArray stride including the trailing padding.
        return AlignTo(End, 16u);
    }

    int32 InferSpawnPairSize(const FCreativeMoveSchema& Schema)
    {
        uint32 End = 0;
        if (Schema.PairOriginalActor != InvalidOffset && Schema.PairOriginalActor < 0x80)
        {
            End = Schema.PairOriginalActor + sizeof(AActor*);
        }
        if (Schema.PairSpawnedActor != InvalidOffset && Schema.PairSpawnedActor < 0x80)
        {
            End = (std::max)(End, Schema.PairSpawnedActor + static_cast<uint32>(sizeof(AActor*)));
        }
        if (Schema.bSpawnedActorIsForPreview != InvalidOffset &&
            Schema.bSpawnedActorIsForPreview < 0x80)
        {
            End = (std::max)(End, Schema.bSpawnedActorIsForPreview + 1u);
        }
        return End ? static_cast<int32>((End + 7u) & ~7u) : 0;
    }

    FCreativeMoveSchema ResolveSchema()
    {
        FCreativeMoveSchema Schema;
        Schema.SelectedActorInfo = SDK::FindStruct("CreativeSelectedActorInfo");
        Schema.SpawnPair = SDK::FindStruct("OriginalAndSpawnedPair");

        Schema.SelectedActorActor = GetOffset(Schema.SelectedActorInfo, "Actor");
        Schema.ActorToSelectionAtDragStart = GetOffset(Schema.SelectedActorInfo,
            "ActorToSelectionAtDragStart");
        if (Schema.ActorToSelectionAtDragStart == InvalidOffset)
        {
            Schema.ActorToSelectionAtDragStart = GetOffset(Schema.SelectedActorInfo,
                "UnscaledActorToSelectionAtDragStart");
            Schema.bActorToSelectionIsUnscaled =
                Schema.ActorToSelectionAtDragStart != InvalidOffset;
        }
        if (Schema.ActorToSelectionAtDragStart == InvalidOffset)
        {
            Schema.ActorToSelectionAtDragStart = GetOffset(Schema.SelectedActorInfo,
                "UnscaledObjectToSelectionAtDragStart");
            Schema.bActorToSelectionIsUnscaled =
                Schema.ActorToSelectionAtDragStart != InvalidOffset;
        }
        Schema.ScaleAtDragStart = GetOffset(Schema.SelectedActorInfo, "ScaleAtDragStart");
        Schema.DragStartGridSnapPoint = GetOffset(Schema.SelectedActorInfo,
            "DragStartGridSnapPoint");
        Schema.OriginalRelevancyDistance = GetOffset(Schema.SelectedActorInfo,
            "OriginalRelevancyDistance");
        Schema.bWasCollisionEnabled = GetOffset(Schema.SelectedActorInfo, "bWasCollisionEnabled");
        Schema.bWasDormant = GetOffset(Schema.SelectedActorInfo, "bWasDormant");
        Schema.bSpawnedFromSaveRecord = GetOffset(Schema.SelectedActorInfo,
            "bSpawnedFromSaveRecord");
        Schema.LogicalConnectionChainIndex = GetOffset(Schema.SelectedActorInfo,
            "LogicalConnectionChainIndex");

        Schema.PairOriginalActor = GetOffset(Schema.SpawnPair, "OriginalActor");
        Schema.PairSpawnedActor = GetOffset(Schema.SpawnPair, "SpawnedActor");
        Schema.bSpawnedActorIsForPreview = GetOffset(Schema.SpawnPair, "bSpawnedActorIsForPreview");

        if (Schema.SelectedActorInfo)
        {
            const int32 ReflectedSize = Schema.SelectedActorInfo->GetPropertiesSize();
            if (ReflectedSize >= 0x18 && ReflectedSize <= 0x200)
                Schema.SelectedActorSize = ReflectedSize;
        }
        if (!Schema.SelectedActorSize)
            Schema.SelectedActorSize = InferSelectedActorSize(Schema);
        if (Schema.SelectedActorSize > 0 && Schema.SelectedActorActor >= 0xA0 &&
            Schema.ActorToSelectionAtDragStart == 0)
        {
            Schema.SelectedActorSize = AlignTo(static_cast<uint32>(Schema.SelectedActorSize), 16u);
        }
        if (Schema.SpawnPair)
        {
            const int32 ReflectedSize = Schema.SpawnPair->GetPropertiesSize();
            if (ReflectedSize >= 0x10 && ReflectedSize <= 0x80)
                Schema.SpawnPairSize = ReflectedSize;
        }
        if (!Schema.SpawnPairSize)
            Schema.SpawnPairSize = InferSpawnPairSize(Schema);

        return Schema;
    }

    const FCreativeMoveSchema& GetSchema()
    {
        static const FCreativeMoveSchema Schema = ResolveSchema();
        return Schema;
    }

    template <typename T> T* GetObjectProperty(UObject* Object, const char* PropertyName)
    {
        if (!IsLiveObject(Object))
            return nullptr;

        const uint32 Offset = Object->GetOffset(PropertyName);
        if (Offset == InvalidOffset || Offset > 0x10000 || !SDK::MemReadable(
                reinterpret_cast<uint8*>(Object) + Offset, sizeof(T)))
        {
            return nullptr;
        }

        return reinterpret_cast<T*>(reinterpret_cast<uint8*>(Object) + Offset);
    }

    TArray<uint8>* GetRawArray(UObject* Object, const char* PropertyName)
    {
        auto Array = GetObjectProperty<TArray<uint8>>(Object, PropertyName);
        if (!Array || Array->Num() < 0 || Array->Num() > 0x10000 ||
            Array->MaxElements < Array->Num() || Array->MaxElements > 0x100000)
        {
            return nullptr;
        }
        return Array;
    }

    void SetStructBool(uint8* Buffer, int32 BufferSize, const UStruct* Struct, uint32 Offset,
        const char* PropertyName, bool Value)
    {
        if (!Buffer || !Struct || Offset == InvalidOffset ||
            Offset >= static_cast<uint32>(BufferSize))
        {
            return;
        }

        uint8 Mask = 1;
        if (auto Property = Struct->GetProperty(PropertyName))
        {
            const uint8 ReflectedMask = Property->GetFieldMask();
            if (ReflectedMask)
                Mask = ReflectedMask;
        }
        if (Value)
            Buffer[Offset] |= Mask;
        else
            Buffer[Offset] &= ~Mask;
    }

    template <typename TResult> bool CallWithNamedResult(UObject* Target, UFunction* Function,
        const char* InputName, const void* InputValue, size_t InputSize, TResult& OutResult,
        size_t ResultSize = sizeof(TResult))
    {
        if (!IsLiveObject(Target) || !Function || !ResultSize || ResultSize > sizeof(TResult) ||
            (InputName && (!InputValue || !InputSize)))
        {
            return false;
        }

        const uint32 ReturnOffset = Function->GetOffset("ReturnValue");
        const uint32 InputOffset = InputName ? Function->GetOffset(InputName) : 0;
        if (ReturnOffset == InvalidOffset || ReturnOffset + ResultSize > 0x1000 ||
            (InputName && (InputOffset == InvalidOffset || InputOffset + InputSize > 0x1000)))
        {
            return false;
        }

        const int32 ReflectedSize = Function->GetPropertiesSize();
        const size_t BufferSize = ReflectedSize > 0 && ReflectedSize <= 0x1000
            ? static_cast<size_t>(ReflectedSize) : 0x1000;
        if (ReturnOffset + ResultSize > BufferSize ||
            (InputName && InputOffset + InputSize > BufferSize))
        {
            return false;
        }

        std::vector<uint8> Params(BufferSize, 0);
        if (InputName)
        {
            memcpy(Params.data() + InputOffset, InputValue, InputSize);
        }
        Target->ProcessEvent(Function, Params.data());
        memcpy(&OutResult, Params.data() + ReturnOffset, ResultSize);
        return true;
    }

    template <typename TResult> bool CallMathWithNamedResult(const char* FunctionName,
        const char* FirstName, const void* FirstValue, size_t FirstSize, const char* SecondName,
        const void* SecondValue, size_t SecondSize, TResult& OutResult, size_t ResultSize)
    {
        auto Library = const_cast<UKismetMathLibrary*>(UKismetMathLibrary::GetDefaultObj());
        auto Function = Library ? Library->GetFunction(FunctionName) : nullptr;
        if (!Function || !FirstValue || !SecondValue || !ResultSize || ResultSize > sizeof(TResult))
        {
            return false;
        }

        const uint32 FirstOffset = Function->GetOffset(FirstName);
        const uint32 SecondOffset = Function->GetOffset(SecondName);
        const uint32 ReturnOffset = Function->GetOffset("ReturnValue");
        if (FirstOffset == InvalidOffset || SecondOffset == InvalidOffset ||
            ReturnOffset == InvalidOffset || FirstOffset + FirstSize > 0x1000 ||
            SecondOffset + SecondSize > 0x1000 || ReturnOffset + ResultSize > 0x1000)
        {
            return false;
        }

        const int32 ReflectedSize = Function->GetPropertiesSize();
        const size_t BufferSize = ReflectedSize > 0 && ReflectedSize <= 0x1000
            ? static_cast<size_t>(ReflectedSize) : 0x1000;
        if (FirstOffset + FirstSize > BufferSize || SecondOffset + SecondSize > BufferSize ||
            ReturnOffset + ResultSize > BufferSize)
        {
            return false;
        }

        std::vector<uint8> Params(BufferSize, 0);
        memcpy(Params.data() + FirstOffset, FirstValue, FirstSize);
        memcpy(Params.data() + SecondOffset, SecondValue, SecondSize);
        Library->ProcessEvent(Function, Params.data());
        memcpy(&OutResult, Params.data() + ReturnOffset, ResultSize);
        return true;
    }

    FTransform MakeRelativeTransformCompat(const FTransform& Transform,
        const FTransform& RelativeTo, bool& bSucceeded)
    {
        FTransform Result{};
        if (CallMathWithNamedResult("MakeRelativeTransform", "A", &Transform, FTransform::Size(),
                "RelativeTo", &RelativeTo, FTransform::Size(), Result, FTransform::Size()))
        {
            bSucceeded = true;
            return Result;
        }
        bSucceeded = false;
        return Transform;
    }

    FVector InverseTransformLocationCompat(const FTransform& Transform, const FVector& Location,
        bool& bSucceeded)
    {
        FVector Result{};
        if (CallMathWithNamedResult("InverseTransformLocation", "T", &Transform, FTransform::Size(),
                "Location", &Location, FVector::Size(), Result, FVector::Size()))
        {
            bSucceeded = true;
            return Result;
        }
        bSucceeded = false;
        return Location;
    }

    FTransform ComposeTransformsCompat(const FTransform& RelativeTransform,
        const FTransform& SelectionToWorld, bool& bSucceeded)
    {
        FTransform Result{};
        if (CallMathWithNamedResult("ComposeTransforms",
                "A", &RelativeTransform, FTransform::Size(),
                "B", &SelectionToWorld, FTransform::Size(), Result, FTransform::Size()))
        {
            bSucceeded = true;
            return Result;
        }
        bSucceeded = false;
        return RelativeTransform;
    }

    bool BuildSelectedActorInfo(AActor* Actor,
        const FTransform& DragStart, std::vector<uint8>& OutInfo)
    {
        const auto& Schema = GetSchema();
        if (!IsLiveObject(Actor) || !Schema.IsStartUsable())
            return false;

        OutInfo.assign(static_cast<size_t>(Schema.SelectedActorSize), 0);
        auto Write = [&](uint32 Offset, const void* Value, size_t Size)
        {
            if (Offset == InvalidOffset || !Value || !Size || Offset >= OutInfo.size() ||
                Size > OutInfo.size() - Offset)
            {
                return false;
            }
            memcpy(OutInfo.data() + Offset, Value, Size);
            return true;
        };

        const FTransform ActorTransform = Actor->GetTransform();
        bool bMadeRelative = false;
        FTransform ActorToSelection = MakeRelativeTransformCompat(
                ActorTransform, DragStart, bMadeRelative);
        if (!bMadeRelative)
            return false;
        if (Schema.bActorToSelectionIsUnscaled && Schema.ScaleAtDragStart != InvalidOffset)
        {
            FVector UnitScale(1, 1, 1);
            ActorToSelection.Scale3D = UnitScale;
        }
        if (!Write(Schema.SelectedActorActor, &Actor, sizeof(Actor)) ||
            !Write(Schema.ActorToSelectionAtDragStart, &ActorToSelection, FTransform::Size()))
        {
            return false;
        }

        Write(Schema.ScaleAtDragStart, &ActorTransform.Scale3D, FVector::Size());
        Write(Schema.DragStartGridSnapPoint, &DragStart.Translation, FVector::Size());

        float RelevancyDistance = 0;
        if (Actor->HasNetCullDistanceSquared() && std::isfinite(Actor->NetCullDistanceSquared) &&
            Actor->NetCullDistanceSquared > 0)
        {
            RelevancyDistance = static_cast<float>(std::sqrt(Actor->NetCullDistanceSquared));
        }
        Write(Schema.OriginalRelevancyDistance, &RelevancyDistance, sizeof(RelevancyDistance));

        const bool bCollisionEnabled = Actor->GetProperty("bActorEnableCollision", 0x20000)
                ? ReadReflectedBool(Actor, "bActorEnableCollision", true) : true;
        const bool bWasDormant = Actor->HasNetDormancy() && Actor->NetDormancy >= 2;
        SetStructBool(OutInfo.data(), Schema.SelectedActorSize, Schema.SelectedActorInfo,
            Schema.bWasCollisionEnabled, "bWasCollisionEnabled", bCollisionEnabled);
        SetStructBool(OutInfo.data(), Schema.SelectedActorSize, Schema.SelectedActorInfo,
            Schema.bWasDormant, "bWasDormant", bWasDormant);
        SetStructBool(OutInfo.data(), Schema.SelectedActorSize, Schema.SelectedActorInfo,
            Schema.bSpawnedFromSaveRecord, "bSpawnedFromSaveRecord", false);
        const int32 NoLogicalConnection = -1;
        Write(Schema.LogicalConnectionChainIndex,
            &NoLogicalConnection, sizeof(NoLogicalConnection));
        return true;
    }

    AFortPlayerControllerAthena* AsCreativeController(UObject* Object)
    {
        if (!IsLiveObject(Object))
            return nullptr;

        auto ControllerClass = AFortPlayerControllerAthena::StaticClass();
        if (ControllerClass && Object->IsA(ControllerClass))
            return (AFortPlayerControllerAthena*)Object;

        auto PawnClass = AFortPlayerPawnAthena::StaticClass();
        if (PawnClass && Object->IsA(PawnClass))
        {
            auto Pawn = (AFortPlayerPawnAthena*)Object;
            if (Pawn->HasController() && IsLiveObject(Pawn->Controller) && ControllerClass &&
                Pawn->Controller->IsA(ControllerClass))
            {
                return (AFortPlayerControllerAthena*)Pawn->Controller;
            }
        }

        return nullptr;
    }

    AFortPlayerControllerAthena* ResolveOwningController(UObject* InteractionOwner)
    {
        if (!IsLiveObject(InteractionOwner))
            return nullptr;

        UObject* Pending[16]{};
        int32 PendingCount = 0;
        int32 Next = 0;
        auto Queue = [&](UObject* Candidate)
        {
            if (!IsLiveObject(Candidate) || PendingCount >= 16)
                return;
            for (int32 Index = 0; Index < PendingCount; ++Index)
            {
                if (Pending[Index] == Candidate)
                    return;
            }
            Pending[PendingCount++] = Candidate;
        };
        Queue(InteractionOwner);

        while (Next < PendingCount)
        {
            auto Candidate = Pending[Next++];
            if (auto Controller = AsCreativeController(Candidate))
                return Controller;

            if (auto GetController = Candidate->GetFunction("GetFortPlayerController"))
            {
                AFortPlayerControllerAthena* Controller = nullptr;
                CallWithNamedResult(Candidate, GetController, nullptr, nullptr, 0, Controller);
                if (auto ValidController = AsCreativeController(Controller))
                {
                    return ValidController;
                }
            }

            auto ActorClass = AActor::StaticClass();
            if (ActorClass && Candidate->IsA(ActorClass))
            {
                auto Actor = (AActor*)Candidate;
                if (Actor->HasOwner())
                    Queue(Actor->Owner);
                if (Actor->HasInstigator())
                    Queue(Actor->Instigator);
            }

            for (const char* PropertyName : {
                    "OwningWeapon", "Weapon", "CreativeMoveTool", "InteractionOwner",
                    "OwningInteractionActor", "OwningPlayerController",
                    "FortPlayerController", "PlayerController", "ActiveBoundBehavior",
                    "ActiveBoundBehaviorReplicateToRemoteClients",
                    "BoundManipulateInteractBehavior" })
            {
                if (auto Property = GetObjectProperty<UObject*>(Candidate, PropertyName))
                {
                    Queue(*Property);
                }
            }
        }

        return nullptr;
    }

    AFortPlayerPawnAthena* GetControlledPawn(AFortPlayerControllerAthena* PlayerController)
    {
        if (!IsLiveObject(PlayerController))
            return nullptr;

        if (PlayerController->HasMyFortPawn() && IsLiveObject(PlayerController->MyFortPawn))
        {
            return PlayerController->MyFortPawn;
        }
        if (PlayerController->HasPawn() && IsLiveObject(PlayerController->Pawn))
        {
            return PlayerController->Pawn->Cast<AFortPlayerPawnAthena>();
        }
        return nullptr;
    }

    bool IsObjectInCurrentWorld(UObject* Object);

    AFortCreativeMoveTool* GetEquippedCreativePhone(AFortPlayerControllerAthena* PlayerController)
    {
        auto Pawn = GetControlledPawn(PlayerController);
        static const UClass* MoveToolClass = nullptr;
        if (!IsLiveObject(MoveToolClass))
            MoveToolClass = SDK::FindClass("FortCreativeMoveTool");
        if (!Pawn || !MoveToolClass || !Pawn->HasCurrentWeapon() ||
            !IsLiveObject(Pawn->CurrentWeapon) || !Pawn->CurrentWeapon->IsA(MoveToolClass) ||
            !IsObjectInCurrentWorld(Pawn) || !IsObjectInCurrentWorld(Pawn->CurrentWeapon))
        {
            return nullptr;
        }
        if (!Pawn->HasController() || !IsLiveObject(Pawn->Controller) ||
            Pawn->Controller != PlayerController)
        {
            return nullptr;
        }
        auto Phone = (AFortCreativeMoveTool*)Pawn->CurrentWeapon;
        if (Phone->HasWeaponData())
        {
            if (!IsLiveObject(Phone->WeaponData))
            {
                return nullptr;
            }
            static const UObject* CreativePhoneDefinition = nullptr;
            if (!IsLiveObject(CreativePhoneDefinition))
            {
                CreativePhoneDefinition = FindObject<UObject>(
                    L"/Game/Athena/Items/Weapons/Prototype/WID_CreativeTool.WID_CreativeTool");
                if (!CreativePhoneDefinition)
                {
                    CreativePhoneDefinition = TUObjectArray::FindObject<UObject>(
                            "WID_CreativeTool");
                }
            }
            if (CreativePhoneDefinition)
            {
                if (Phone->WeaponData != CreativePhoneDefinition)
                    return nullptr;
            }
            else if (Phone->WeaponData->Name.ToString() != "WID_CreativeTool")
            {
                return nullptr;
            }
        }
        return Phone;
    }

    bool IsInteractionOwnedByPhone(UObject* InteractionOwner, AFortCreativeMoveTool* Phone)
    {
        if (!IsLiveObject(InteractionOwner) || !IsLiveObject(Phone))
            return false;
        if (InteractionOwner == Phone)
            return true;

        bool bHasLiveActiveModeReference = false;
        for (const char* Name : {
                "ActiveObjectInteractionMode", "InteractionModeReplicateToRemoteClients" })
        {
            if (!Phone->GetProperty(Name))
                continue;
            if (auto Property = GetObjectProperty<UObject*>(Phone, Name);
                Property && IsLiveObject(*Property))
            {
                bHasLiveActiveModeReference = true;
                if (*Property == InteractionOwner)
                    return true;
            }
        }
        if (bHasLiveActiveModeReference)
            return false;

        UObject* Pending[20]{};
        int32 PendingCount = 0;
        int32 Next = 0;
        auto Queue = [&](UObject* Candidate)
        {
            if (!IsLiveObject(Candidate) || PendingCount >= 20)
                return;
            for (int32 Index = 0; Index < PendingCount; ++Index)
            {
                if (Pending[Index] == Candidate)
                    return;
            }
            Pending[PendingCount++] = Candidate;
        };
        Queue(InteractionOwner);
        while (Next < PendingCount)
        {
            auto Candidate = Pending[Next++];
            if (Candidate == Phone)
                return true;

            auto ActorClass = AActor::StaticClass();
            if (ActorClass && Candidate->IsA(ActorClass))
            {
                auto Actor = (AActor*)Candidate;
                if (Actor->HasOwner())
                    Queue(Actor->Owner);
            }
            for (const char* Name : {
                    "OwningWeapon", "Weapon", "CreativeMoveTool",
                    "InteractionOwner", "OwningInteractionActor" })
            {
                if (auto Property = GetObjectProperty<UObject*>(Candidate, Name))
                {
                    Queue(*Property);
                }
            }
        }

        for (const char* Name : {
                "ActiveObjectInteractionMode", "InteractionModeReplicateToRemoteClients",
                "PhoneToolActorTargetMode" })
        {
            if (auto Property = GetObjectProperty<UObject*>(Phone, Name);
                Property && *Property == InteractionOwner)
            {
                return true;
            }
        }
        if (auto QueueProperty = GetObjectProperty<TArray<UObject*>>(
                Phone, "ObjectInteractionModeQueue");
            QueueProperty && QueueProperty->Num() > 0 && QueueProperty->Num() <= 0x100 &&
            SDK::MemReadable(QueueProperty->Data, static_cast<size_t>(QueueProperty->Num()) *
                    sizeof(UObject*)))
        {
            for (auto Mode : *QueueProperty)
            {
                if (Mode == InteractionOwner)
                    return true;
            }
        }
        return false;
    }

    bool HasEquippedPhoneAuthority(UObject* InteractionOwner,
        AFortPlayerControllerAthena* PlayerController)
    {
        auto Phone = GetEquippedCreativePhone(PlayerController);
        return Phone && IsInteractionOwnedByPhone(InteractionOwner, Phone);
    }

    bool IsObjectInCurrentWorld(UObject* Object)
    {
        auto World = UWorld::GetWorld();
        if (!World || !IsLiveObject(Object))
            return false;
        UObject* Outer = Object;
        auto LevelClass = ULevel::StaticClass();
        for (int32 Depth = 0; Outer && Depth < 16; ++Depth)
        {
            if (Outer == World)
                return true;
            if (!SDK::MemReadable(Outer, sizeof(UObject)))
                return false;
            if (LevelClass && Outer->IsA(LevelClass))
            {
                auto Level = (ULevel*)Outer;
                if (Level->HasOwningWorld() && Level->OwningWorld == World)
                {
                    return true;
                }
                if (World->HasPersistentLevel() && World->PersistentLevel == Level)
                {
                    return true;
                }
            }
            Outer = Outer->Outer;
        }
        return false;
    }

    double GetCreativePhoneRange(AFortCreativeMoveTool* Phone)
    {
        constexpr double FallbackRange = 20000.0;
        constexpr double MaximumServerRange = 50000.0;
        if (!IsLiveObject(Phone))
            return 0;
        if (auto MaxRange = GetObjectProperty<float>(Phone, "MaxRange");
            MaxRange && std::isfinite(*MaxRange) && *MaxRange >= 100.0f)
        {
            return (std::min)(static_cast<double>(*MaxRange), MaximumServerRange);
        }
        return FallbackRange;
    }

    double GetEquippedPhoneRange(AFortPlayerControllerAthena* PlayerController)
    {
        return GetCreativePhoneRange(GetEquippedCreativePhone(PlayerController));
    }

    bool IsPointWithinPhoneRange(const FVector& PhoneLocation, double Range, const FVector& Point)
    {
        if (Range <= 0 || !std::isfinite(Point.X) ||
            !std::isfinite(Point.Y) || !std::isfinite(Point.Z))
        {
            return false;
        }
        const FVector Delta = Point - PhoneLocation;
        return Delta.SizeSquared() <= Range * Range;
    }

    bool IsPointInEquippedPhoneRange(AFortPlayerControllerAthena* PlayerController,
        const FVector& Point)
    {
        auto Pawn = GetControlledPawn(PlayerController);
        const double Range = GetEquippedPhoneRange(PlayerController);
        return Pawn && IsPointWithinPhoneRange(Pawn->K2_GetActorLocation(), Range, Point);
    }

    bool HasCreativeEditAuthority(UObject* InteractionOwner,
        AFortPlayerControllerAthena*& OutController)
    {
        OutController = ResolveOwningController(InteractionOwner);
        if (!OutController)
            return false;

        AFortAthenaCreativePortal::PrepareLinkedVolumeForEditing(OutController);

        if (auto GetCurrentVolume = InteractionOwner->GetFunction("GetCurrentVolume"))
        {
            AFortVolume* CurrentVolume = nullptr;
            if (GetCurrentVolume->GetOffset("bMustHavePermissions") != InvalidOffset)
            {
                const bool bMustHavePermissions = true;
                CallWithNamedResult(InteractionOwner, GetCurrentVolume, "bMustHavePermissions",
                    &bMustHavePermissions, sizeof(bMustHavePermissions), CurrentVolume);
            }
            else
            {
                CallWithNamedResult(InteractionOwner, GetCurrentVolume, nullptr, nullptr, 0,
                    CurrentVolume);
            }
            if (CurrentVolume)
                return true;
        }

        if (auto CanCreate = OutController->GetFunction("IsPlayerInAVolumeTheyCanCreateIn"))
        {
            bool bCanCreate = false;
            if (CallWithNamedResult(OutController, CanCreate, nullptr, nullptr, 0, bCanCreate) &&
                bCanCreate)
                return true;
        }

        if (!OutController->HasOwnedPortal() || !OutController->HasCreativePlotLinkedVolume() ||
            !IsLiveObject(OutController->OwnedPortal) ||
            !IsLiveObject(OutController->CreativePlotLinkedVolume) ||
            !OutController->OwnedPortal->IsA(AFortAthenaCreativePortal::StaticClass()))
        {
            return HasEquippedPhoneAuthority(InteractionOwner, OutController);
        }

        auto Portal = (AFortAthenaCreativePortal*)
            OutController->OwnedPortal;
        if (Portal->HasLinkedVolume() && Portal->LinkedVolume ==
                OutController->CreativePlotLinkedVolume)
        {
            return true;
        }

        return HasEquippedPhoneAuthority(InteractionOwner, OutController);
    }

    bool IsSelectableActor(AActor* Actor)
    {
        if (!IsLiveObject(Actor) || Actor->IsDefaultObject())
        {
            return false;
        }
        if (Actor->GetProperty("bActorIsBeingDestroyed", 0x20000) &&
            ReadReflectedBool(Actor, "bActorIsBeingDestroyed"))
        {
            return false;
        }

        auto BuildingClass = ABuildingActor::StaticClass();
        return BuildingClass && Actor->IsA(BuildingClass);
    }

    bool IsActorA(AActor* Actor, const char* ClassName)
    {
        auto Class = SDK::FindClass(ClassName);
        return IsLiveObject(Actor) && Class && Actor->IsA(Class);
    }

    bool IsStructuralBuildingActor(AActor* Actor)
    {
        auto Building = IsLiveObject(Actor) ? Actor->Cast<ABuildingSMActor>() : nullptr;
        if (!Building)
            return false;

        if (IsActorA(Actor, "BuildingProp") || IsActorA(Actor, "BuildingPropCorner"))
            return false;

        if (Building->HasBuildingType())
        {
            switch (Building->BuildingType)
            {
            case 3: // Deco
            case 4: // Prop
            case 8: // SpawnedItem
            case 9: // Container
            case 10: // Trap
            case 11: // GenericCenterCellActor
                return false;
            default:
                break;
            }
        }

        for (const char* ClassName : {
                "BuildingWall", "BuildingFloor", "BuildingStairs",
                "BuildingRoof", "BuildingCorner", "BuildingPillar" })
        {
            if (IsActorA(Actor, ClassName))
                return true;
        }

        if (auto WillRegister = Actor->GetFunction("WillRegisterWithStructuralGrid"))
        {
            bool bWillRegister = false;
            if (CallWithNamedResult(Actor, WillRegister, nullptr, nullptr, 0, bWillRegister) &&
                bWillRegister)
                return true;
        }
        if (ReadReflectedBool(Building, "bRegisterWithStructuralGrid"))
        {
            return true;
        }

        if (Building->HasBuildingType())
        {
            switch (Building->BuildingType)
            {
            case 0: // Wall
            case 1: // Floor
            case 2: // Corner
            case 5: // Stairs
            case 6: // Roof
            case 7: // Pillar
                return true;
            case 12:
            case 13:
                return false;
            default:
                break;
            }
        }
        return false;
    }

    bool SetComponentMobility(UObject* Component, uint8 Mobility)
    {
        if (!IsLiveObject(Component))
            return false;
        auto StoredMobility = GetObjectProperty<uint8>(Component, "Mobility");
        if (!StoredMobility)
            return false;
        if (*StoredMobility == Mobility)
            return true;

        auto SetMobility = Component->GetFunction("SetMobility");
        if (!SetMobility || SetMobility->GetOffset("NewMobility") == InvalidOffset)
        {
            return false;
        }
        Component->Call<void>(SetMobility, Mobility);
        return *StoredMobility == Mobility;
    }

    void PruneTrackedPropMobilities()
    {
        TrackedPropMobilities.erase(std::remove_if(TrackedPropMobilities.begin(),
                TrackedPropMobilities.end(), [](const FTrackedPropMobility& Entry)
                {
                    return !IsLiveObject(Entry.InteractionOwner) || !IsLiveObject(Entry.Actor) ||
                        !IsLiveObject(Entry.RootComponent);
                }), TrackedPropMobilities.end());
    }

    bool EnsureMovableProp(UObject* InteractionOwner, AActor* Actor)
    {
        if (!IsLiveObject(InteractionOwner) || !IsSelectableActor(Actor))
            return false;
        if (IsStructuralBuildingActor(Actor))
            return true;

        auto RootProperty = GetObjectProperty<UObject*>(Actor, "RootComponent");
        auto Root = RootProperty ? *RootProperty : nullptr;
        auto Mobility = GetObjectProperty<uint8>(Root, "Mobility");
        if (!IsLiveObject(Root) || !Mobility)
            return true;
        constexpr uint8 MovableMobility = 2;
        if (*Mobility == MovableMobility)
            return true;

        PruneTrackedPropMobilities();
        auto Existing = std::find_if(TrackedPropMobilities.begin(), TrackedPropMobilities.end(),
            [InteractionOwner, Actor](const FTrackedPropMobility& Entry)
            {
                return Entry.InteractionOwner == InteractionOwner && Entry.Actor == Actor;
            });
        if (Existing == TrackedPropMobilities.end())
        {
            TrackedPropMobilities.push_back({
                InteractionOwner, Actor, Root, *Mobility });
        }

        if (SetComponentMobility(Root, MovableMobility))
            return true;

        if (Existing == TrackedPropMobilities.end())
            TrackedPropMobilities.pop_back();
        return false;
    }

    void RestoreTrackedPropMobilityForActor(UObject* InteractionOwner, AActor* Actor)
    {
        for (size_t Index = TrackedPropMobilities.size();
             Index > 0; --Index)
        {
            const auto Entry = TrackedPropMobilities[Index - 1];
            if (Entry.InteractionOwner != InteractionOwner || Entry.Actor != Actor)
            {
                continue;
            }
            if (IsLiveObject(Entry.RootComponent))
            {
                SetComponentMobility(Entry.RootComponent, Entry.OriginalMobility);
            }
            TrackedPropMobilities.erase(TrackedPropMobilities.begin() + (Index - 1));
        }
    }

    void RestoreTrackedPropMobilities(UObject* InteractionOwner)
    {
        for (size_t Index = TrackedPropMobilities.size();
             Index > 0; --Index)
        {
            const auto Entry = TrackedPropMobilities[Index - 1];
            if (Entry.InteractionOwner != InteractionOwner)
                continue;
            if (IsLiveObject(Entry.RootComponent))
            {
                SetComponentMobility(Entry.RootComponent, Entry.OriginalMobility);
            }
            TrackedPropMobilities.erase(TrackedPropMobilities.begin() + (Index - 1));
        }
    }

    bool HasTrackedPropMobility(UObject* InteractionOwner)
    {
        PruneTrackedPropMobilities();
        return std::find_if(TrackedPropMobilities.begin(), TrackedPropMobilities.end(),
            [InteractionOwner](const FTrackedPropMobility& Entry)
            {
                return Entry.InteractionOwner == InteractionOwner;
            }) != TrackedPropMobilities.end();
    }

    bool GetActorBounds(AActor* Actor, FVector& OutMin, FVector& OutMax);

    bool IsPointInsideActorBounds(AActor* BoundsActor, const FVector& Point)
    {
        FVector BoundsMin{};
        FVector BoundsMax{};
        if (!GetActorBounds(BoundsActor, BoundsMin, BoundsMax))
            return false;

        return Point.X >= BoundsMin.X && Point.X <= BoundsMax.X &&
            Point.Y >= BoundsMin.Y && Point.Y <= BoundsMax.Y &&
            Point.Z >= BoundsMin.Z && Point.Z <= BoundsMax.Z;
    }

    AFortVolumeManager* GetCreativeVolumeManager()
    {
        auto World = UWorld::GetWorld();
        if (!World || !IsLiveObject(World->GameState) ||
            !World->GameState->IsA(AFortGameStateAthena::StaticClass()))
        {
            return nullptr;
        }

        auto GameState = (AFortGameStateAthena*)World->GameState;
        return GameState->HasVolumeManager() && IsLiveObject(GameState->VolumeManager)
            ? GameState->VolumeManager : nullptr;
    }

    AFortVolume* GetAuthorizedVolume(AFortPlayerControllerAthena* PlayerController)
    {
        if (!IsLiveObject(PlayerController) || !PlayerController->HasCreativePlotLinkedVolume() ||
            !IsLiveObject(PlayerController->CreativePlotLinkedVolume))
        {
            return nullptr;
        }
        return PlayerController->CreativePlotLinkedVolume;
    }

    bool IsActorInAuthorizedVolume(AActor* Actor, AFortPlayerControllerAthena* PlayerController)
    {
        auto AuthorizedVolume = GetAuthorizedVolume(PlayerController);
        if (!IsSelectableActor(Actor))
            return false;
        if (!AuthorizedVolume)
        {
            return GetEquippedCreativePhone(PlayerController) && IsObjectInCurrentWorld(Actor) &&
                IsPointInEquippedPhoneRange(PlayerController, Actor->K2_GetActorLocation());
        }

        auto Manager = GetCreativeVolumeManager();
        auto GetVolume = Manager ? Manager->GetFunction("GetVolumeForActor") : nullptr;
        if (GetVolume)
        {
            AFortVolume* ActorVolume = nullptr;
            CallWithNamedResult(Manager, GetVolume, "Actor", &Actor, sizeof(Actor), ActorVolume);
            if (IsLiveObject(ActorVolume))
                return ActorVolume == AuthorizedVolume;
        }

        if (auto IsOverlapping = AuthorizedVolume->GetFunction("IsOverlappingActor"))
        {
            bool bIsOverlapping = false;
            if (CallWithNamedResult(AuthorizedVolume, IsOverlapping, "Other", &Actor,
                    sizeof(Actor), bIsOverlapping) && bIsOverlapping)
                return true;
        }

        return IsPointInsideActorBounds(AuthorizedVolume, Actor->K2_GetActorLocation());
    }

    bool IsTransformInAuthorizedVolume(const FTransform& Transform,
        AFortPlayerControllerAthena* PlayerController)
    {
        auto AuthorizedVolume = GetAuthorizedVolume(PlayerController);
        if (!AuthorizedVolume)
        {
            return GetEquippedCreativePhone(PlayerController) && IsPointInEquippedPhoneRange(
                    PlayerController, Transform.Translation);
        }

        auto Manager = GetCreativeVolumeManager();
        auto GetVolume = Manager ? Manager->GetFunction("GetVolumeForLocation") : nullptr;
        if (GetVolume)
        {
            AFortVolume* TargetVolume = nullptr;
            CallWithNamedResult(Manager, GetVolume, "Location", &Transform.Translation,
                FVector::Size(), TargetVolume);
            if (IsLiveObject(TargetVolume))
                return TargetVolume == AuthorizedVolume;
        }

        return IsPointInsideActorBounds(AuthorizedVolume, Transform.Translation);
    }

    bool InvokeBehaviorClassPredicate(UObject* Behavior,
        UFunction* Function, UClass* Class, bool& OutResult)
    {
        if (!IsLiveObject(Behavior) || !Function || !Class)
            return false;

        uint32 ClassOffset = InvalidOffset;
        constexpr const char* ClassParameterNames[] = {
            "Class", "Class_0", "ActorClass", "ObjectClass", "InClass", "TargetClass"
        };
        for (const char* Name : ClassParameterNames)
        {
            ClassOffset = Function->GetOffset(Name);
            if (ClassOffset != InvalidOffset)
                break;
        }
        const uint32 ReturnOffset = Function->GetOffset("ReturnValue");
        if (ClassOffset == InvalidOffset || ReturnOffset == InvalidOffset ||
            ClassOffset + sizeof(Class) > 0x1000 || ReturnOffset + sizeof(uint8) > 0x1000)
        {
            return false;
        }

        const int32 ReflectedSize = Function->GetPropertiesSize();
        const size_t BufferSize = ReflectedSize > 0 && ReflectedSize <= 0x1000
            ? static_cast<size_t>(ReflectedSize) : 0x1000;
        if (ClassOffset + sizeof(Class) > BufferSize || ReturnOffset + sizeof(uint8) > BufferSize)
        {
            return false;
        }

        std::vector<uint8> Params(BufferSize, 0);
        memcpy(Params.data() + ClassOffset, &Class, sizeof(Class));
        Behavior->ProcessEvent(Function, Params.data());
        OutResult = Params[ReturnOffset] != 0;
        return true;
    }

    bool BehaviorAcceptsActors(UObject* Behavior, const TArray<AActor*>& Actors)
    {
        if (!IsLiveObject(Behavior) || Actors.Num() <= 0)
            return false;

        UFunction* IsAllowed = nullptr;
        UFunction* IsForbidden = nullptr;
        IsAllowed = Behavior->GetFunction("IsActorClassAllowed");
        if (!IsAllowed)
            IsAllowed = Behavior->GetFunction("IsObjectClassAllowed");
        IsForbidden = Behavior->GetFunction("IsActorClassForbidden");
        if (!IsForbidden)
            IsForbidden = Behavior->GetFunction("IsObjectClassForbidden");

        auto SoftClassArrayContains = [](UObject* Owner,
            const char* PropertyName, AActor* Actor, bool& bAvailable)
        {
            bAvailable = false;
            auto Array = GetRawArray(Owner, PropertyName);
            if (!Array || Array->Num() < 0 || Array->Num() > 0x100)
                return false;
            bAvailable = true;
            const size_t Stride = FSoftObjectPtr::Size();
            if (Array->Num() > 0 && !SDK::MemReadable(Array->Data,
                    static_cast<size_t>(Array->Num()) * Stride))
            {
                bAvailable = false;
                return false;
            }
            for (int32 Index = 0; Index < Array->Num(); ++Index)
            {
                auto SoftClass = reinterpret_cast<FSoftObjectPtr*>(
                    Array->Data + static_cast<size_t>(Index) * Stride);
                auto Class = (UClass*)SoftClass->InternalGet(UClass::StaticClass());
                if (Class && Actor->IsA(Class))
                    return true;
            }
            return false;
        };

        for (auto Actor : Actors)
        {
            if (!IsSelectableActor(Actor))
                return false;
            if (IsForbidden)
            {
                bool bForbidden = false;
                if (!InvokeBehaviorClassPredicate(Behavior,
                        IsForbidden, Actor->Class, bForbidden) || bForbidden)
                {
                    return false;
                }
            }
            if (IsAllowed)
            {
                bool bAllowed = false;
                if (!InvokeBehaviorClassPredicate(Behavior, IsAllowed, Actor->Class, bAllowed) ||
                    !bAllowed)
                {
                    return false;
                }
            }
            if (!IsAllowed && !IsForbidden)
            {
                bool bHasForbiddenArray = false;
                if (SoftClassArrayContains(Behavior, "ValidForbiddenClasses", Actor,
                        bHasForbiddenArray))
                    return false;

                bool bHasAllowedArray = false;
                const bool bAllowed = SoftClassArrayContains(Behavior, "ValidAllowedClasses", Actor,
                    bHasAllowedArray);
                auto AllowedArray = GetRawArray(Behavior, "ValidAllowedClasses");
                if (bHasAllowedArray && AllowedArray && AllowedArray->Num() > 0 && !bAllowed)
                    return false;
            }
        }
        return true;
    }

    UObject* SelectMovementMode(UObject* InteractionOwner, const TArray<AActor*>& Actors)
    {
        if (!IsLiveObject(InteractionOwner) || Actors.Num() <= 0)
            return nullptr;

        bool bAllStructural = true;
        bool bAnyStructural = false;
        for (auto Actor : Actors)
        {
            const bool bStructural = IsStructuralBuildingActor(Actor);
            bAnyStructural |= bStructural;
            if (!bStructural)
            {
                bAllStructural = false;
            }
        }
        const bool bMixedSelection = bAnyStructural && !bAllStructural;
        if (bMixedSelection && !UsesLegacyCreativePhoneLifecycle())
            return nullptr;
        const bool bUseGridBehavior = bAllStructural ||
            (UsesLegacyCreativePhoneLifecycle() && bAnyStructural);

        auto IsPreferred = [bUseGridBehavior](UObject* Behavior)
        {
            if (!IsLiveObject(Behavior))
                return false;
            const auto Name = Behavior->Class->Name.ToString();
            if (bUseGridBehavior)
                return Name.find("MoveBuildingsOnGrid") != std::string::npos;
            return Name.find("MoveObjectsFreely") != std::string::npos ||
                Name.find("MoveActorsFreely") != std::string::npos;
        };
        auto IsManipulationBehavior = [](UObject* Behavior)
        {
            if (!IsLiveObject(Behavior))
                return false;
            const auto Name = Behavior->Class->Name.ToString();
            return Name.find("Delete") == std::string::npos &&
                Name.find("Possess") == std::string::npos &&
                Name.find("Playset") == std::string::npos;
        };
        auto PriorityOf = [](UObject* Behavior)
        {
            if (auto Priority = GetObjectProperty<int32>(Behavior, "Priority"))
                return *Priority;
            return 0;
        };

        UObject* BestPreferred = nullptr;
        int32 BestPreferredPriority = INT_MIN;
        auto Consider = [&](UObject* Behavior)
        {
            if (!IsManipulationBehavior(Behavior))
                return;
            const auto BehaviorName = Behavior->Class->Name.ToString();
            const bool bKnownGrid = BehaviorName.find("MoveBuildingsOnGrid") != std::string::npos;
            const bool bKnownFree = BehaviorName.find("MoveObjectsFreely") != std::string::npos ||
                BehaviorName.find("MoveActorsFreely") != std::string::npos;
            if ((bUseGridBehavior && bKnownFree) || (!bUseGridBehavior && bKnownGrid))
            {
                return;
            }
            const bool bLegacyMixedGrid = UsesLegacyCreativePhoneLifecycle() &&
                bMixedSelection && bKnownGrid;
            if (!BehaviorAcceptsActors(Behavior, Actors) && !bLegacyMixedGrid)
            {
                return;
            }
            const int32 Priority = PriorityOf(Behavior);
            if (IsPreferred(Behavior) && (!BestPreferred || Priority > BestPreferredPriority))
            {
                BestPreferred = Behavior;
                BestPreferredPriority = Priority;
            }
        };

        auto Behaviors = GetObjectProperty<TArray<UObject*>>(
            InteractionOwner, "InteractionBehaviors");
        if (Behaviors && Behaviors->Num() > 0 && Behaviors->Num() <= 0x100 &&
            SDK::MemReadable(Behaviors->Data, static_cast<size_t>(Behaviors->Num()) *
                    sizeof(UObject*)))
        {
            for (auto Behavior : *Behaviors)
                Consider(Behavior);
        }

        for (const char* Name : {
                "BoundManipulateInteractBehavior", "ActiveBoundBehavior", "ActiveMovementMode",
                // FN 39.40+ target modes expose the two manipulation behaviours directly instead of filling InteractionBehaviors.
                "MoveObjectsFreelyInteractionBehavior", "MoveActorsFreelyInteractionBehavior",
                "MoveBuildingOnGridInteractionBehavior", "MoveBuildingsOnGridInteractionBehavior" })
        {
            if (auto Property = GetObjectProperty<UObject*>(InteractionOwner, Name))
            {
                Consider(*Property);
            }
        }

        return BestPreferred;
    }

    bool IsFreeMovementBehavior(UObject* Behavior)
    {
        if (!IsLiveObject(Behavior))
            return false;
        const auto Name = Behavior->Class->Name.ToString();
        return Name.find("MoveObjectsFreely") != std::string::npos ||
            Name.find("MoveActorsFreely") != std::string::npos;
    }

    bool GetActorBounds(AActor* Actor, FVector& OutMin, FVector& OutMax)
    {
        if (!IsLiveObject(Actor))
            return false;

        auto Function = Actor->GetFunction("GetActorBounds");
        if (!Function)
            return false;

        const int32 ReflectedSize = Function->GetPropertiesSize();
        const size_t BufferSize = ReflectedSize > 0 && ReflectedSize <= 0x1000
            ? static_cast<size_t>(ReflectedSize) : 0x1000;
        std::vector<uint8> Params(BufferSize, 0);

        const uint32 OriginOffset = Function->GetOffset("Origin");
        const uint32 ExtentOffset = Function->GetOffset("BoxExtent");
        if (OriginOffset == InvalidOffset || ExtentOffset == InvalidOffset ||
            OriginOffset + FVector::Size() > BufferSize ||
            ExtentOffset + FVector::Size() > BufferSize)
        {
            return false;
        }

        Actor->ProcessEvent(Function, Params.data());
        FVector Origin{};
        FVector Extent{};
        memcpy(&Origin, Params.data() + OriginOffset, FVector::Size());
        memcpy(&Extent, Params.data() + ExtentOffset, FVector::Size());
        if (!std::isfinite(Origin.X) || !std::isfinite(Origin.Y) || !std::isfinite(Origin.Z) ||
            !std::isfinite(Extent.X) || !std::isfinite(Extent.Y) || !std::isfinite(Extent.Z) ||
            Extent.X < 0 || Extent.Y < 0 || Extent.Z < 0)
        {
            return false;
        }
        OutMin = Origin - Extent;
        OutMax = Origin + Extent;
        return true;
    }

    std::vector<uint8> BuildSelectionBounds(const TArray<AActor*>& Actors,
        const FTransform& SelectionToWorld)
    {
        auto BoxStruct = FBox::StaticStruct();
        if (!BoxStruct)
            return {};

        const uint32 MinOffset = BoxStruct->GetOffset("Min");
        const uint32 MaxOffset = BoxStruct->GetOffset("Max");
        const uint32 ValidOffset = BoxStruct->GetOffset("IsValid");
        if (MinOffset == InvalidOffset || MaxOffset == InvalidOffset)
            return {};

        uint32 Required = (std::max)(MinOffset + static_cast<uint32>(FVector::Size()),
            MaxOffset + static_cast<uint32>(FVector::Size()));
        if (ValidOffset != InvalidOffset)
            Required = (std::max)(Required, ValidOffset + 1u);

        const int32 ReflectedSize = BoxStruct->GetPropertiesSize();
        size_t Size = ReflectedSize >= static_cast<int32>(Required) && ReflectedSize <= 0x100
            ? static_cast<size_t>(ReflectedSize) : static_cast<size_t>((Required + 7u) & ~7u);
        if (Size == 0 || Size > 0x100)
            return {};

        FVector BoundsMin{};
        FVector BoundsMax{};
        bool bHasBounds = false;
        for (auto Actor : Actors)
        {
            FVector WorldMin{};
            FVector WorldMax{};
            if (!GetActorBounds(Actor, WorldMin, WorldMax))
                continue;

            for (int32 CornerIndex = 0;
                 CornerIndex < 8; ++CornerIndex)
            {
                const FVector WorldCorner((CornerIndex & 1) ? WorldMax.X : WorldMin.X,
                    (CornerIndex & 2) ? WorldMax.Y : WorldMin.Y,
                    (CornerIndex & 4) ? WorldMax.Z : WorldMin.Z);
                bool bTransformedCorner = false;
                FVector SelectionCorner = InverseTransformLocationCompat(
                        SelectionToWorld, WorldCorner, bTransformedCorner);
                if (!bTransformedCorner)
                    return {};

                if (!bHasBounds)
                {
                    BoundsMin = SelectionCorner;
                    BoundsMax = SelectionCorner;
                    bHasBounds = true;
                    continue;
                }

                BoundsMin.X = (std::min)(BoundsMin.X, SelectionCorner.X);
                BoundsMin.Y = (std::min)(BoundsMin.Y, SelectionCorner.Y);
                BoundsMin.Z = (std::min)(BoundsMin.Z, SelectionCorner.Z);
                BoundsMax.X = (std::max)(BoundsMax.X, SelectionCorner.X);
                BoundsMax.Y = (std::max)(BoundsMax.Y, SelectionCorner.Y);
                BoundsMax.Z = (std::max)(BoundsMax.Z, SelectionCorner.Z);
            }
        }

        std::vector<uint8> Bytes(Size, 0);
        if (bHasBounds)
        {
            memcpy(Bytes.data() + MinOffset, &BoundsMin, FVector::Size());
            memcpy(Bytes.data() + MaxOffset, &BoundsMax, FVector::Size());
            if (ValidOffset != InvalidOffset && ValidOffset < Size)
                Bytes[ValidOffset] = 1;
        }
        return Bytes;
    }

    bool WriteFunctionParam(UFunction* Function, uint8* Buffer, size_t BufferSize, const char* Name,
        const void* Value, size_t ValueSize)
    {
        if (!Function || !Buffer || !Value || !ValueSize)
            return false;

        const uint32 Offset = Function->GetOffset(Name);
        if (Offset == InvalidOffset || Offset >= BufferSize || ValueSize > BufferSize - Offset)
        {
            return false;
        }

        memcpy(Buffer + Offset, Value, ValueSize);
        return true;
    }

    FQuat IdentityQuat();
    bool IsFiniteTransform(const FTransform& Transform);

    bool SendClientStartInteracting(AFortCreativeMoveTool* MoveTool,
        UObject* MovementMode, TArray<uint8>& SelectedActors, const FTransform& SelectionToWorld,
        const std::vector<uint8>& Bounds)
    {
        auto Function = MoveTool->GetFunction("ClientStartInteracting");
        if (!Function || !MovementMode || Bounds.empty())
            return false;

        const int32 ReflectedSize = Function->GetPropertiesSize();
        const size_t BufferSize = ReflectedSize > 0 && ReflectedSize <= 0x1000
            ? static_cast<size_t>(ReflectedSize) : 0x1000;
        std::vector<uint8> Params(BufferSize, 0);

        bool bComplete = WriteFunctionParam(Function, Params.data(), BufferSize,
            "NewActiveMovementMode", &MovementMode, sizeof(MovementMode));
        if (!bComplete)
        {
            bComplete = WriteFunctionParam(Function, Params.data(), BufferSize,
                "NewActiveBoundBehavior", &MovementMode, sizeof(MovementMode));
        }
        bool bWroteSelection = WriteFunctionParam(Function, Params.data(), BufferSize,
            "NewSelectedActors", &SelectedActors, sizeof(SelectedActors));
        if (!bWroteSelection)
        {
            bWroteSelection = WriteFunctionParam(Function, Params.data(), BufferSize,
                "NewPlacementActors", &SelectedActors, sizeof(SelectedActors));
        }
        bComplete &= bWroteSelection;
        bComplete &= WriteFunctionParam(Function, Params.data(), BufferSize,
            "NewSelectionToWorld", &SelectionToWorld, FTransform::Size());
        bComplete &= WriteFunctionParam(Function, Params.data(), BufferSize,
            "NewSelectionSpaceActorBounds", Bounds.data(), Bounds.size());
        auto InitialRotation = IdentityQuat();
        WriteFunctionParam(Function, Params.data(), BufferSize,
            "InitialRotationOffset", &InitialRotation, FQuat::Size());
        if (!bComplete)
            return false;

        MoveTool->ProcessEvent(Function, Params.data());
        return true;
    }

    bool StartInteractionFallback(AFortCreativeMoveTool* MoveTool,
        const TArray<AActor*>& RequestedActors, const FTransform& DragStart,
        AFortPlayerControllerAthena* PlayerController)
    {
        const auto& Schema = GetSchema();
        if (!Schema.IsStartUsable())
            return false;

        auto SelectedActors = GetRawArray(MoveTool, "SelectedActors");
        if (!SelectedActors)
            return false;

        TArray<AActor*> Actors;
        for (auto Actor : RequestedActors)
        {
            if (Actors.Contains(Actor))
                continue;
            if (!IsActorInAuthorizedVolume(Actor, PlayerController))
            {
                Actors.Free();
                return false;
            }
            Actors.Add(Actor);
            if (Actors.Num() >= MaxCreativeSelection)
                break;
        }
        if (Actors.Num() <= 0)
        {
            Actors.Free();
            return false;
        }

        // FN 10.40 uses the first selected actor as SelectionToWorld; DragStart is an input transform, not a group pivot.
        FTransform SelectionToWorld = DragStart;
        if (UsesLegacyCreativePhoneLifecycle())
            SelectionToWorld = Actors[0]->GetTransform();
        if (!IsFiniteTransform(SelectionToWorld))
        {
            Actors.Free();
            return false;
        }

        std::vector<std::vector<uint8>> ActorInfos;
        ActorInfos.reserve(static_cast<size_t>(Actors.Num()));
        for (auto Actor : Actors)
        {
            std::vector<uint8> Info;
            if (!BuildSelectedActorInfo(Actor, SelectionToWorld, Info))
            {
                Actors.Free();
                return false;
            }
            ActorInfos.emplace_back(std::move(Info));
        }

        SDK::DbgLog("[CreativePhone] start fallback prepared requested=%d selected=%d\n",
            RequestedActors.Num(), Actors.Num());

        SelectedActors->ResetNum();
        auto RemoteSelectedActors = GetRawArray(MoveTool, "SelectedActorsReplicateToRemoteClients");
        if (RemoteSelectedActors == SelectedActors)
            RemoteSelectedActors = nullptr;
        if (RemoteSelectedActors)
            RemoteSelectedActors->ResetNum();
        if (auto NewlyPlaced = GetRawArray(MoveTool, "NewlyPlacedActors"))
        {
            NewlyPlaced->ResetNum();
        }
        if (auto SpawnHelper = GetRawArray(MoveTool, "SpawnHelperNewlyPlacedActors"))
        {
            SpawnHelper->ResetNum();
        }
        for (auto& Info : ActorInfos)
        {
            SelectedActors->Add(*Info.data(), Schema.SelectedActorSize);
            if (RemoteSelectedActors)
            {
                RemoteSelectedActors->Add(*Info.data(), Schema.SelectedActorSize);
            }
        }
        if (SelectedActors->Num() != Actors.Num() || RemoteSelectedActors &&
                RemoteSelectedActors->Num() != Actors.Num())
        {
            if (RemoteSelectedActors)
                RemoteSelectedActors->ResetNum();
            Actors.Free();
            return false;
        }

        auto MovementMode = SelectMovementMode(MoveTool, Actors);
        if (!MovementMode)
        {
            Actors.Free();
            SelectedActors->ResetNum();
            if (RemoteSelectedActors)
                RemoteSelectedActors->ResetNum();
            return false;
        }

        if (auto Active = GetObjectProperty<UObject*>(MoveTool, "ActiveMovementMode"))
        {
            *Active = MovementMode;
        }
        if (auto ActiveRemote = GetObjectProperty<UObject*>(MoveTool,
                "ActiveMovementModeReplicateToRemoteClients"))
        {
            *ActiveRemote = MovementMode;
        }

        if (auto StoredTransform = GetObjectProperty<FTransform>(MoveTool, "SelectionToWorld"))
        {
            memcpy(StoredTransform, &SelectionToWorld, FTransform::Size());
        }
        if (auto DragStartTransform = GetObjectProperty<FTransform>(
                MoveTool, "SelectionToWorldAtDragStart"))
        {
            memcpy(DragStartTransform, &SelectionToWorld, FTransform::Size());
        }

        auto Bounds = BuildSelectionBounds(Actors, SelectionToWorld);
        if (!Bounds.empty())
        {
            const uint32 BoundsOffset = MoveTool->GetOffset("SelectionSpaceActorsBounds");
            if (BoundsOffset != InvalidOffset && BoundsOffset <= 0x10000 && SDK::MemReadable(
                    reinterpret_cast<uint8*>(MoveTool) + BoundsOffset, Bounds.size()))
            {
                memcpy(reinterpret_cast<uint8*>(MoveTool) + BoundsOffset,
                    Bounds.data(), Bounds.size());
            }
        }

        if (!UsesLegacyCreativePhoneLifecycle() && IsFreeMovementBehavior(MovementMode))
        {
            for (auto Actor : Actors)
            {
                if (EnsureMovableProp(MoveTool, Actor))
                    continue;
                RestoreTrackedPropMobilities(MoveTool);
                SelectedActors->ResetNum();
                if (RemoteSelectedActors)
                    RemoteSelectedActors->ResetNum();
                Actors.Free();
                return false;
            }
        }

        const bool bClientStarted = SendClientStartInteracting(
            MoveTool, MovementMode, *SelectedActors, SelectionToWorld, Bounds);
        SDK::DbgLog("[CreativePhone] start fallback handoff selected=%d client=%d\n",
            SelectedActors->Num(), bClientStarted ? 1 : 0);
        if (!bClientStarted)
        {
            SelectedActors->ResetNum();
            if (RemoteSelectedActors)
                RemoteSelectedActors->ResetNum();
            RestoreTrackedPropMobilities(MoveTool);
            Actors.Free();
            return false;
        }
        if (auto StartServer = MovementMode->GetFunction("StartCreativeInteractionOnServer"))
        {
            MovementMode->Call<void>(StartServer);
        }

        if (auto RemoteUpdate = MoveTool->GetFunction("RemoteClientsUpdateSelectedActors"))
        {
            MoveTool->Call<void>(RemoteUpdate, *SelectedActors, MovementMode);
        }

        VersionFeatureAdapter::MarkReplicatedPropertyDirty(
            MoveTool, L"SelectedActorsReplicateToRemoteClients");
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(MoveTool,
            L"ActiveMovementModeReplicateToRemoteClients");

        MoveTool->ForceNetUpdate();
        Actors.Free();
        return true;
    }

    bool IsFiniteTransform(const FTransform& Transform)
    {
        const auto& Location = Transform.Translation;
        const auto& Scale = Transform.Scale3D;
        const auto& Rotation = Transform.Rotation;
        return std::isfinite(Location.X) && std::isfinite(Location.Y) &&
            std::isfinite(Location.Z) && std::isfinite(Scale.X) && std::isfinite(Scale.Y) &&
            std::isfinite(Scale.Z) && std::isfinite(Rotation.X) && std::isfinite(Rotation.Y) &&
            std::isfinite(Rotation.Z) && std::isfinite(Rotation.W) &&
            std::abs(Location.X) < 100000000.0 && std::abs(Location.Y) < 100000000.0 &&
            std::abs(Location.Z) < 100000000.0 && std::abs(Scale.X) < 10000.0 &&
            std::abs(Scale.Y) < 10000.0 && std::abs(Scale.Z) < 10000.0;
    }

    std::vector<AActor*> UniqueActors(const TArray<AActor*>& Actors);
    const char* GetSelectionArrayName(UObject* InteractionOwner);
    bool ResolveSelectedActorTransform(const uint8* Entry, const FTransform& SelectionToWorld,
        FTransform& OutTransform);
    bool TryNativeStartSingleFreeProp(AFortCreativeMoveTool* MoveTool, UFunction* Function,
        FExecHandler Original, FExecHandler Hook, const TArray<AActor*>& Actors,
        const FTransform& DragStart);

    UObject* GetStructuralSupportSystem()
    {
        auto World = UWorld::GetWorld();
        if (!World || !IsLiveObject(World->GameState) ||
            !World->GameState->IsA(AFortGameStateAthena::StaticClass()))
        {
            return nullptr;
        }

        auto GameState = (AFortGameStateAthena*)World->GameState;
        return GameState->HasStructuralSupportSystem() &&
            IsLiveObject(GameState->StructuralSupportSystem)
            ? GameState->StructuralSupportSystem : nullptr;
    }

    bool QueryStructuralPlacement(AActor* Actor, const FTransform& Transform,
        bool bIgnoreActorBeingMoved = false)
    {
        if (!IsStructuralBuildingActor(Actor))
            return true;

        auto Building = Actor->Cast<ABuildingSMActor>();
        auto System = GetStructuralSupportSystem();
        auto Function = System && bIgnoreActorBeingMoved
            ? System->GetFunction("K2_CanAddBuildingActorToGrid") : nullptr;
        bool bUsesClass = false;
        if (!Function && System && !bIgnoreActorBeingMoved)
        {
            Function = System->GetFunction("CanAddBuildingActorClassToGrid");
            bUsesClass = Function != nullptr;
        }
        if (System && Function && Building)
        {
            const int32 ReflectedSize = Function->GetPropertiesSize();
            const size_t BufferSize = ReflectedSize > 0 && ReflectedSize <= 0x1000
                ? static_cast<size_t>(ReflectedSize) : 0x1000;
            std::vector<uint8> Params(BufferSize, 0);
            auto World = UWorld::GetWorld();
            FRotator Rotation = Transform.Rotation.Rotator();
            bool bMirrored = ReadReflectedBool(Building, "bMirrored");
            const bool bAllowStaticOverlap = false;

            bool bComplete = WriteFunctionParam(Function,
                Params.data(), BufferSize, "WorldContextObject", &World, sizeof(World));
            if (bUsesClass)
            {
                auto BuildingClass = Actor->Class;
                bComplete &= WriteFunctionParam(Function, Params.data(), BufferSize,
                    "BuildingSMActorClassToCheck", &BuildingClass, sizeof(BuildingClass));
            }
            else
            {
                bComplete &= WriteFunctionParam(Function, Params.data(), BufferSize, "ActorToCheck",
                    &Building, sizeof(Building));
            }
            bComplete &= WriteFunctionParam(Function, Params.data(), BufferSize, "Location",
                &Transform.Translation, FVector::Size());
            bComplete &= WriteFunctionParam(Function, Params.data(), BufferSize, "Rotation",
                &Rotation, FRotator::Size());
            bComplete &= WriteFunctionParam(Function, Params.data(), BufferSize, "bMirrored",
                &bMirrored, sizeof(bMirrored));
            bComplete &= WriteFunctionParam(Function,
                Params.data(), BufferSize, "bAllowStaticOverlap",
                &bAllowStaticOverlap, sizeof(bAllowStaticOverlap));

            const uint32 ExistingOffset = Function->GetOffset("ExistingBuildings");
            const uint32 ReturnOffset = Function->GetOffset("ReturnValue");
            if (bComplete && ExistingOffset != InvalidOffset && ReturnOffset != InvalidOffset &&
                ExistingOffset + sizeof(TArray<AActor*>) <= BufferSize && ReturnOffset < BufferSize)
            {
                Params[ReturnOffset] = 0xFF;
                System->ProcessEvent(Function, Params.data());
                auto Existing = reinterpret_cast<TArray<AActor*>*>(Params.data() + ExistingOffset);
                const bool bExistingArrayValid = Existing->Num() >= 0 &&
                    Existing->Num() <= 0x1000 && Existing->MaxElements >= Existing->Num() &&
                    Existing->MaxElements <= 0x10000 && (Existing->Num() == 0 || SDK::MemReadable(
                        Existing->Data, static_cast<size_t>(Existing->Num()) * sizeof(AActor*)));
                const uint8 Result = Params[ReturnOffset];
                const bool bCanPlace = bExistingArrayValid && Result == 0 && Existing->Num() == 0;
                if (bExistingArrayValid)
                    Existing->Free();
                return bCanPlace;
            }
        }

        if (!bIgnoreActorBeingMoved && CantBuild_ && Building)
        {
            struct FPad0xC { uint8 Bytes[0xC]; };
            struct FPad0x18 { uint8 Bytes[0x18]; };
            TArray<ABuildingSMActor*> Existing;
            char UnknownOut = 0;
            const UClass* BuildingClass = Actor->Class;
            const FRotator Rotation = Transform.Rotation.Rotator();
            bool bCantBuild = true;
            if (VersionInfo.FortniteVersion >= 27.0)
            {
                TSubclassOf<AActor> Class(Actor->Class);
                auto CantBuild = (__int64 (*)(UWorld*, TSubclassOf<AActor>&, FPad0x18, FPad0x18,
                    bool, TArray<ABuildingSMActor*>*, char*))
                    CantBuild_;
                bCantBuild = CantBuild(UWorld::GetWorld(), Class,
                    *reinterpret_cast<const FPad0x18*>(&Transform.Translation),
                    *reinterpret_cast<const FPad0x18*>(&Rotation),
                    ReadReflectedBool(Building, "bMirrored"), &Existing, &UnknownOut) != 0;
            }
            else if (VersionInfo.FortniteVersion >= 20.0)
            {
                auto CantBuild = (__int64 (*)(UWorld*, const UClass*, FPad0x18, FPad0x18, bool,
                    TArray<ABuildingSMActor*>*, char*))CantBuild_;
                bCantBuild = CantBuild(UWorld::GetWorld(), BuildingClass,
                    *reinterpret_cast<const FPad0x18*>(&Transform.Translation),
                    *reinterpret_cast<const FPad0x18*>(&Rotation),
                    ReadReflectedBool(Building, "bMirrored"), &Existing, &UnknownOut) != 0;
            }
            else
            {
                auto CantBuild = (__int64 (*)(UWorld*, const UClass*, FPad0xC, FPad0xC, bool,
                    TArray<ABuildingSMActor*>*, char*))CantBuild_;
                bCantBuild = CantBuild(UWorld::GetWorld(), BuildingClass,
                    *reinterpret_cast<const FPad0xC*>(&Transform.Translation),
                    *reinterpret_cast<const FPad0xC*>(&Rotation),
                    ReadReflectedBool(Building, "bMirrored"), &Existing, &UnknownOut) != 0;
            }
            const bool bHasExisting = Existing.Num() > 0;
            Existing.Free();
            return !bCantBuild && !bHasExisting;
        }

        return false;
    }

    void PrunePendingStructuralDuplicates()
    {
        PendingStructuralDuplicates.erase(std::remove_if(PendingStructuralDuplicates.begin(),
                PendingStructuralDuplicates.end(), [](const FPendingStructuralDuplicate& Pending)
                {
                    return !IsLiveObject(Pending.InteractionOwner) ||
                        !IsSelectableActor(Pending.SpawnedActor);
                }), PendingStructuralDuplicates.end());
    }

    void TrackStructuralDuplicate(UObject* InteractionOwner,
        AActor* SpawnedActor, const FTransform& OriginalTransform)
    {
        if (!IsLiveObject(InteractionOwner) || !IsSelectableActor(SpawnedActor))
        {
            return;
        }
        PrunePendingStructuralDuplicates();
        auto Existing = std::find_if(PendingStructuralDuplicates.begin(),
            PendingStructuralDuplicates.end(), [InteractionOwner, SpawnedActor](
                const FPendingStructuralDuplicate& Pending)
            {
                return Pending.InteractionOwner == InteractionOwner &&
                    Pending.SpawnedActor == SpawnedActor;
            });
        if (Existing != PendingStructuralDuplicates.end())
        {
            return;
        }
        PendingStructuralDuplicates.push_back({
            InteractionOwner, SpawnedActor, OriginalTransform });
    }

    std::vector<FPendingStructuralDuplicate>
        GetTrackedStructuralDuplicates(UObject* InteractionOwner)
    {
        PrunePendingStructuralDuplicates();
        std::vector<FPendingStructuralDuplicate> Result;
        for (const auto& Pending : PendingStructuralDuplicates)
        {
            if (Pending.InteractionOwner == InteractionOwner)
                Result.push_back(Pending);
        }
        return Result;
    }

    void RemoveTrackedStructuralDuplicates(UObject* InteractionOwner)
    {
        PendingStructuralDuplicates.erase(std::remove_if(PendingStructuralDuplicates.begin(),
                PendingStructuralDuplicates.end(), [InteractionOwner](
                    const FPendingStructuralDuplicate& Pending)
                {
                    return Pending.InteractionOwner == InteractionOwner;
                }), PendingStructuralDuplicates.end());
    }

    void DestroyTrackedDuplicates(UObject* InteractionOwner)
    {
        const auto Pending = GetTrackedStructuralDuplicates(InteractionOwner);
        RemoveTrackedStructuralDuplicates(InteractionOwner);
        for (const auto& Entry : Pending)
        {
            if (IsLiveObject(Entry.SpawnedActor))
                Entry.SpawnedActor->K2_DestroyActor();
        }
    }

    UObject* ResolvePlacementValidationBehavior(UObject* InteractionOwner)
    {
        if (!IsLiveObject(InteractionOwner))
            return nullptr;
        for (const char* Name : {
                "BoundManipulateInteractBehavior", "ActiveMovementMode", "ActiveBoundBehavior" })
        {
            auto Property = GetObjectProperty<UObject*>(InteractionOwner, Name);
            if (!Property || !IsLiveObject(*Property))
                continue;
            if ((*Property)->GetFunction("IsSelectionSetInValidPosition"))
            {
                return *Property;
            }
        }
        return nullptr;
    }

    bool ValidateTrackedStructuralDuplicates(UObject* InteractionOwner,
        const FTransform& TargetSelectionToWorld, AFortPlayerControllerAthena* PlayerController)
    {
        if (!IsTransformInAuthorizedVolume(TargetSelectionToWorld, PlayerController))
        {
            return false;
        }

        const char* ArrayName = GetSelectionArrayName(InteractionOwner);
        auto Selected = ArrayName ? GetRawArray(InteractionOwner, ArrayName) : nullptr;
        const auto& Schema = GetSchema();
        if (!Selected || !Schema.IsStartUsable() || Selected->Num() <= 0 ||
            Selected->Num() > MaxCreativeSelection)
            return false;

        struct FStructuralTarget
        {
            AActor* Actor;
            FTransform Transform;
        };
        std::vector<FStructuralTarget> StructuralTargets;
        for (int32 Index = 0; Index < Selected->Num(); ++Index)
        {
            auto SelectedEntry = Selected->Data + static_cast<size_t>(Index) *
                    Schema.SelectedActorSize;
            if (!SDK::MemReadable(SelectedEntry, Schema.SelectedActorSize))
                return false;
            auto SelectedActor = *reinterpret_cast<AActor**>(
                SelectedEntry + Schema.SelectedActorActor);
            if (!IsActorInAuthorizedVolume(SelectedActor, PlayerController))
            {
                return false;
            }
            FTransform Target{};
            if (!ResolveSelectedActorTransform(SelectedEntry, TargetSelectionToWorld, Target) ||
                !IsTransformInAuthorizedVolume(Target, PlayerController))
            {
                return false;
            }
            if (!IsStructuralBuildingActor(SelectedActor))
                continue;
            StructuralTargets.push_back({ SelectedActor, Target });
        }

        if (StructuralTargets.empty())
            return true;

        if (StructuralTargets.size() == 1)
        {
            const auto& Target = StructuralTargets.front();
            if (!QueryStructuralPlacement(Target.Actor, Target.Transform, true))
            {
                return false;
            }
        }

        auto Behavior = ResolvePlacementValidationBehavior(InteractionOwner);
        auto IsValid = Behavior ? Behavior->GetFunction("IsSelectionSetInValidPosition") : nullptr;
        bool bSelectionValid = false;
        if (IsValid && CallWithNamedResult(Behavior, IsValid,
                nullptr, nullptr, 0, bSelectionValid, sizeof(bool)))
        {
            return bSelectionValid;
        }

        return StructuralTargets.size() == 1;
    }

    bool ResolveSelectedActorTransform(const uint8* Entry, const FTransform& SelectionToWorld,
        FTransform& OutTransform)
    {
        const auto& Schema = GetSchema();
        if (!Entry || !Schema.IsStartUsable() ||
            !SDK::MemReadable(Entry, Schema.SelectedActorSize) ||
            Schema.ActorToSelectionAtDragStart + FTransform::Size() >
                static_cast<uint32>(Schema.SelectedActorSize))
        {
            return false;
        }

        FTransform Relative{};
        memcpy(&Relative, Entry + Schema.ActorToSelectionAtDragStart, FTransform::Size());
        bool bComposed = false;
        OutTransform = ComposeTransformsCompat(Relative, SelectionToWorld, bComposed);
        if (!bComposed)
            return false;

        if (Schema.bActorToSelectionIsUnscaled && Schema.ScaleAtDragStart != InvalidOffset &&
            Schema.ScaleAtDragStart + FVector::Size() <=
                static_cast<uint32>(Schema.SelectedActorSize))
        {
            FVector OriginalScale{};
            memcpy(&OriginalScale, Entry + Schema.ScaleAtDragStart, FVector::Size());
            OutTransform.Scale3D.X *= OriginalScale.X;
            OutTransform.Scale3D.Y *= OriginalScale.Y;
            OutTransform.Scale3D.Z *= OriginalScale.Z;
        }
        return IsFiniteTransform(OutTransform);
    }

    struct FTrackedPlacementTarget
    {
        AActor* Actor;
        FTransform Transform;
    };

    bool ApplySelectedActorTransform(AActor* Actor, const FTransform& Transform)
    {
        if (!IsSelectableActor(Actor) || !IsFiniteTransform(Transform))
        {
            return false;
        }

        auto Function = Actor->GetFunction("K2_SetActorTransform");
        if (!Function)
            return false;

        const int32 ReflectedSize = Function->GetPropertiesSize();
        const size_t BufferSize = ReflectedSize > 0 && ReflectedSize <= 0x1000
            ? static_cast<size_t>(ReflectedSize) : 0x1000;
        std::vector<uint8> Params(BufferSize, 0);
        const uint32 ReturnOffset = Function->GetOffset("ReturnValue");
        if (ReturnOffset == InvalidOffset || ReturnOffset >= BufferSize)
        {
            return false;
        }
        if (!WriteFunctionParam(Function, Params.data(), BufferSize,
                "NewTransform", &Transform, FTransform::Size()))
        {
            return false;
        }

        const bool bSweep = false;
        const bool bTeleport = true;
        WriteFunctionParam(Function, Params.data(), BufferSize, "bSweep", &bSweep, sizeof(bSweep));
        WriteFunctionParam(Function, Params.data(), BufferSize,
            "bTeleport", &bTeleport, sizeof(bTeleport));
        Actor->ProcessEvent(Function, Params.data());
        return Params[ReturnOffset] != 0;
    }

    void StoreSelectionToWorld(UObject* InteractionOwner,
        AFortPlayerControllerAthena* PlayerController, const FTransform& Transform)
    {
        auto Store = [&](UObject* Object)
        {
            if (auto Property = GetObjectProperty<FTransform>(Object, "SelectionToWorld"))
            {
                memcpy(Property, &Transform, FTransform::Size());
            }
        };
        Store(InteractionOwner);

        if (auto Phone = GetEquippedCreativePhone(PlayerController);
            Phone && Phone != InteractionOwner)
        {
            Store(Phone);
        }
    }

    bool SendSelectionTransformUpdate(UObject* InteractionOwner, UObject* MovementBehavior,
        const FTransform& SelectionToWorld, bool bUpdateOwningClient,
        const std::vector<FTrackedPlacementTarget>& Targets)
    {
        if (!IsLiveObject(InteractionOwner))
            return false;

        const char* MulticastName = bUpdateOwningClient ? "MulticastUpdateSelectionSetExceptServer"
            : "MulticastUpdateSelectionSetExceptServerAndOwningClient";
        auto Multicast = InteractionOwner->GetFunction(MulticastName);
        if (Multicast)
        {
            const int32 ReflectedSize = Multicast->GetPropertiesSize();
            const size_t BufferSize = ReflectedSize > 0 && ReflectedSize <= 0x1000
                ? static_cast<size_t>(ReflectedSize) : 0x1000;
            std::vector<uint8> Params(BufferSize, 0);
            bool bWroteTransform = WriteFunctionParam(Multicast, Params.data(), BufferSize,
                "NewTransformToWorld", &SelectionToWorld, FTransform::Size());
            if (!bWroteTransform)
            {
                bWroteTransform = WriteFunctionParam(Multicast, Params.data(), BufferSize,
                    "NewSelectionToWorld", &SelectionToWorld, FTransform::Size());
            }
            if (Multicast->GetOffset("BehaviorHandlingMove") != InvalidOffset &&
                !WriteFunctionParam(Multicast, Params.data(), BufferSize, "BehaviorHandlingMove",
                    &MovementBehavior, sizeof(MovementBehavior)))
            {
                return false;
            }
            if (bWroteTransform)
            {
                InteractionOwner->ProcessEvent(Multicast, Params.data());
                return true;
            }
        }

        const char* ForceName = bUpdateOwningClient ? "MulticastForceMoveActor"
            : "MulticastForceMoveActorExceptOwningClient";
        auto ForceMove = InteractionOwner->GetFunction(ForceName);
        if (!ForceMove)
            return false;
        for (const auto& Target : Targets)
        {
            const int32 ReflectedSize = ForceMove->GetPropertiesSize();
            const size_t BufferSize = ReflectedSize > 0 && ReflectedSize <= 0x1000
                ? static_cast<size_t>(ReflectedSize) : 0x1000;
            std::vector<uint8> Params(BufferSize, 0);
            const bool bWroteActor = WriteFunctionParam(ForceMove, Params.data(), BufferSize,
                "ActorToMove", &Target.Actor, sizeof(Target.Actor));
            bool bWroteTransform = WriteFunctionParam(ForceMove, Params.data(), BufferSize,
                "NewTransform", &Target.Transform, FTransform::Size());
            if (!bWroteTransform)
            {
                bWroteTransform = WriteFunctionParam(ForceMove, Params.data(), BufferSize,
                    "NewActorTransform", &Target.Transform, FTransform::Size());
            }
            if (!bWroteActor || !bWroteTransform)
                return false;
            InteractionOwner->ProcessEvent(ForceMove, Params.data());
        }
        return true;
    }

    void MoveSelectionSetFallback(UObject* Context, FFrame& Stack)
    {
        UObject* RequestedBehavior = nullptr;
        FTransform NewSelectionToWorld{};
        bool bShouldUpdateOwningClient = false;
        auto Function = Stack.GetCurrentNativeFunction();
        if (Function && Function->GetOffset("BehaviorHandlingMove") != InvalidOffset)
        {
            Stack.StepCompiledIn(&RequestedBehavior);
        }
        Stack.StepCompiledIn(&NewSelectionToWorld);
        if (Function && Function->GetOffset("bShouldUpdateOwningClient") != InvalidOffset)
        {
            Stack.StepCompiledIn(&bShouldUpdateOwningClient);
        }
        Stack.IncrementCode();

        if (!IsLiveObject(Context) || !IsFiniteTransform(NewSelectionToWorld))
        {
            return;
        }

        AFortPlayerControllerAthena* PlayerController = nullptr;
        if (!HasCreativeEditAuthority(Context, PlayerController))
            return;

        const char* ArrayName = GetSelectionArrayName(Context);
        auto Selected = ArrayName ? GetRawArray(Context, ArrayName) : nullptr;
        const auto& Schema = GetSchema();
        if (!Selected || !Schema.IsStartUsable() || Selected->Num() <= 0 ||
            Selected->Num() > MaxCreativeSelection)
        {
            return;
        }

        auto MovementBehavior = ResolvePlacementValidationBehavior(Context);
        if (!IsLiveObject(MovementBehavior))
            return;
        if (RequestedBehavior && RequestedBehavior != MovementBehavior)
        {
            RequestedBehavior = MovementBehavior;
        }

        const bool bUseCreativeVolume = GetAuthorizedVolume(PlayerController) != nullptr;
        AFortCreativeMoveTool* EquippedPhone = nullptr;
        FVector PhoneLocation{};
        double PhoneRange = 0;
        if (!bUseCreativeVolume)
        {
            EquippedPhone = GetEquippedCreativePhone(PlayerController);
            auto Pawn = GetControlledPawn(PlayerController);
            if (!EquippedPhone || !Pawn)
                return;
            PhoneLocation = Pawn->K2_GetActorLocation();
            PhoneRange = GetCreativePhoneRange(EquippedPhone);
            if (PhoneRange <= 0)
                return;
        }

        std::vector<FTrackedPlacementTarget> Targets;
        Targets.reserve(static_cast<size_t>(Selected->Num()));
        for (int32 Index = 0; Index < Selected->Num(); ++Index)
        {
            auto Entry = Selected->Data + static_cast<size_t>(Index) * Schema.SelectedActorSize;
            if (!SDK::MemReadable(Entry, Schema.SelectedActorSize))
                return;
            auto Actor = *reinterpret_cast<AActor**>(Entry + Schema.SelectedActorActor);
            FTransform Target{};
            const bool bActorAuthorized = bUseCreativeVolume
                ? IsActorInAuthorizedVolume(Actor, PlayerController) : IsSelectableActor(Actor) &&
                    IsObjectInCurrentWorld(Actor) &&
                    IsPointWithinPhoneRange(PhoneLocation, PhoneRange,
                        Actor->K2_GetActorLocation());
            if (!bActorAuthorized || !ResolveSelectedActorTransform(
                    Entry, NewSelectionToWorld, Target))
            {
                return;
            }
            const bool bTargetAuthorized = bUseCreativeVolume ? IsTransformInAuthorizedVolume(
                    Target, PlayerController) : IsPointWithinPhoneRange(
                    PhoneLocation, PhoneRange, Target.Translation);
            if (!bTargetAuthorized)
            {
                return;
            }
            for (const auto& Existing : Targets)
            {
                if (Existing.Actor == Actor)
                    return;
            }
            Targets.push_back({ Actor, Target });
        }
        if (Targets.size() != static_cast<size_t>(Selected->Num()))
        {
            return;
        }

        std::vector<FTrackedPlacementTarget> OriginalTargets;
        OriginalTargets.reserve(Targets.size());
        for (const auto& Target : Targets)
        {
            const FTransform Original = Target.Actor->GetTransform();
            if (!IsFiniteTransform(Original))
                return;
            OriginalTargets.push_back({ Target.Actor, Original });
        }

        size_t AppliedCount = 0;
        for (; AppliedCount < Targets.size(); ++AppliedCount)
        {
            const auto& Target = Targets[AppliedCount];
            if (!ApplySelectedActorTransform(Target.Actor, Target.Transform))
            {
                for (size_t Rollback = AppliedCount + 1;
                    Rollback > 0; --Rollback)
                {
                    ApplySelectedActorTransform(OriginalTargets[Rollback - 1].Actor,
                        OriginalTargets[Rollback - 1].Transform);
                    OriginalTargets[Rollback - 1].Actor->ForceNetUpdate();
                }
                SDK::DbgLog("[CreativePhone] failed to apply group movement actor=%p\n",
                    Target.Actor);
                return;
            }
        }

        if (!SendSelectionTransformUpdate(Context, MovementBehavior,
                NewSelectionToWorld, bShouldUpdateOwningClient, Targets))
        {
            for (size_t Rollback = OriginalTargets.size();
                Rollback > 0; --Rollback)
            {
                ApplySelectedActorTransform(OriginalTargets[Rollback - 1].Actor,
                    OriginalTargets[Rollback - 1].Transform);
                OriginalTargets[Rollback - 1].Actor->ForceNetUpdate();
            }
            SDK::DbgLog("[CreativePhone] failed to replicate group movement\n");
            return;
        }

        StoreSelectionToWorld(Context, PlayerController, NewSelectionToWorld);
    }

    bool CaptureTrackedPlacementTargets(UObject* InteractionOwner,
        const FTransform& SelectionToWorld, std::vector<FTrackedPlacementTarget>& OutTargets)
    {
        OutTargets.clear();
        const auto Pending = GetTrackedStructuralDuplicates(InteractionOwner);
        if (Pending.empty())
            return true;

        const char* ArrayName = GetSelectionArrayName(InteractionOwner);
        auto Selected = ArrayName ? GetRawArray(InteractionOwner, ArrayName) : nullptr;
        const auto& Schema = GetSchema();
        if (!Selected || !Schema.IsStartUsable() || Selected->Num() <= 0 ||
            Selected->Num() > MaxCreativeSelection)
        {
            return false;
        }

        for (const auto& PendingEntry : Pending)
        {
            bool bFound = false;
            for (int32 Index = 0; Index < Selected->Num(); ++Index)
            {
                auto Entry = Selected->Data + static_cast<size_t>(Index) * Schema.SelectedActorSize;
                if (!SDK::MemReadable(Entry, Schema.SelectedActorSize))
                {
                    return false;
                }
                auto Actor = *reinterpret_cast<AActor**>(Entry + Schema.SelectedActorActor);
                if (Actor != PendingEntry.SpawnedActor)
                    continue;
                FTransform Target{};
                if (!ResolveSelectedActorTransform(Entry, SelectionToWorld, Target))
                {
                    return false;
                }
                OutTargets.push_back({ Actor, Target });
                bFound = true;
                break;
            }
            if (!bFound)
                return false;
        }
        return OutTargets.size() == Pending.size();
    }

    bool IsTransformNear(const FTransform& Actual, const FTransform& Expected)
    {
        constexpr double LocationTolerance = 2.0;
        constexpr double ScaleTolerance = 0.01;
        const double DX = Actual.Translation.X - Expected.Translation.X;
        const double DY = Actual.Translation.Y - Expected.Translation.Y;
        const double DZ = Actual.Translation.Z - Expected.Translation.Z;
        if (DX * DX + DY * DY + DZ * DZ > LocationTolerance * LocationTolerance ||
            std::abs(Actual.Scale3D.X - Expected.Scale3D.X) > ScaleTolerance ||
            std::abs(Actual.Scale3D.Y - Expected.Scale3D.Y) > ScaleTolerance ||
            std::abs(Actual.Scale3D.Z - Expected.Scale3D.Z) > ScaleTolerance)
        {
            return false;
        }
        const double RotationDot = Actual.Rotation.X * Expected.Rotation.X +
            Actual.Rotation.Y * Expected.Rotation.Y + Actual.Rotation.Z * Expected.Rotation.Z +
            Actual.Rotation.W * Expected.Rotation.W;
        const double ActualNorm = Actual.Rotation.X * Actual.Rotation.X +
            Actual.Rotation.Y * Actual.Rotation.Y + Actual.Rotation.Z * Actual.Rotation.Z +
            Actual.Rotation.W * Actual.Rotation.W;
        const double ExpectedNorm = Expected.Rotation.X * Expected.Rotation.X +
            Expected.Rotation.Y * Expected.Rotation.Y + Expected.Rotation.Z * Expected.Rotation.Z +
            Expected.Rotation.W * Expected.Rotation.W;
        const double Denominator = std::sqrt(ActualNorm * ExpectedNorm);
        return Denominator > 0.000001 && std::abs(std::abs(RotationDot / Denominator) - 1.0) <=
                0.01;
    }

    bool DidTrackedPlacementComplete(const std::vector<FTrackedPlacementTarget>& Targets)
    {
        for (const auto& Target : Targets)
        {
            if (!IsSelectableActor(Target.Actor) || !IsTransformNear(
                    Target.Actor->GetTransform(), Target.Transform))
            {
                return false;
            }
            if (IsStructuralBuildingActor(Target.Actor) && !QueryStructuralPlacement(Target.Actor,
                    Target.Actor->GetTransform(), true))
            {
                return false;
            }
        }
        return true;
    }

    void PlaceSelectionValidated(UObject* Context, FFrame& Stack)
    {
        UObject* BehaviorHandlingMove = nullptr;
        FTransform TargetTransform{};
        auto Function = Stack.GetCurrentNativeFunction();
        if (Function && Function->GetOffset("BehaviorHandlingMove") != InvalidOffset)
        {
            Stack.StepCompiledIn(&BehaviorHandlingMove);
        }
        Stack.StepCompiledIn(&TargetTransform);
        Stack.IncrementCode();

        if (!IsLiveObject(Context) || !IsFiniteTransform(TargetTransform))
            return;

        AFortPlayerControllerAthena* PlayerController = nullptr;
        if (!HasCreativeEditAuthority(Context, PlayerController))
        {
            return;
        }

        if (Function && Function->GetOffset("BehaviorHandlingMove") != InvalidOffset)
        {
            auto AuthoritativeBehavior = ResolvePlacementValidationBehavior(Context);
            if (!AuthoritativeBehavior)
                return;
            BehaviorHandlingMove = AuthoritativeBehavior;
        }

        if (!ValidateTrackedStructuralDuplicates(Context, TargetTransform, PlayerController))
        {
            SDK::DbgLog(
                "[CreativePhone] rejected unsupported or overlapping structural duplicate placement\n");
            return;
        }

        std::vector<FTrackedPlacementTarget> TrackedTargets;
        if (!CaptureTrackedPlacementTargets(Context, TargetTransform, TrackedTargets))
        {
            return;
        }

        FExecHandler Original = nullptr;
        if (Function == LegacyPlaceSelectionFunction)
            Original = LegacyPlaceSelectionOG;
        else if (Function == ModernPlaceSelectionFunction)
            Original = ModernPlaceSelectionOG;
        if (!Original || Original == PlaceSelectionValidated)
            return;

        std::vector<AActor*> FinalizingPreviewActors;
        for (const auto& Target : TrackedTargets)
        {
            if (!IsStructuralBuildingActor(Target.Actor) || !ReadReflectedBool(
                    Target.Actor, "bIsForPreviewing"))
            {
                continue;
            }
            if (WriteReflectedBool(Target.Actor, "bIsForPreviewing", false))
            {
                FinalizingPreviewActors.push_back(Target.Actor);
            }
        }

        Function->ExecFunction = reinterpret_cast<void*>(Original);
        if (Function->GetOffset("BehaviorHandlingMove") != InvalidOffset)
        {
            Context->Call<void>(Function, BehaviorHandlingMove, TargetTransform);
        }
        else
        {
            Context->Call<void>(Function, TargetTransform);
        }
        Function->ExecFunction = reinterpret_cast<void*>(PlaceSelectionValidated);
        const char* SelectionName = GetSelectionArrayName(Context);
        auto RemainingSelection = SelectionName ? GetRawArray(Context, SelectionName) : nullptr;
        if (RemainingSelection && RemainingSelection->Num() == 0)
        {
            for (auto Actor : FinalizingPreviewActors)
            {
                if (IsLiveObject(Actor))
                    Actor->ForceNetUpdate();
            }
            RestoreTrackedPropMobilities(Context);
            if (DidTrackedPlacementComplete(TrackedTargets))
                RemoveTrackedStructuralDuplicates(Context);
            else
                DestroyTrackedDuplicates(Context);
        }
        else
        {
            for (auto Actor : FinalizingPreviewActors)
            {
                if (WriteReflectedBool(Actor, "bIsForPreviewing", true))
                {
                    Actor->ForceNetUpdate();
                }
            }
        }
    }

    void ResetTrackedInteractionContext(UObject* InteractionOwner)
    {
        if (!IsLiveObject(InteractionOwner))
            return;
        RestoreTrackedPropMobilities(InteractionOwner);
        DestroyTrackedDuplicates(InteractionOwner);
        if (const char* SelectionName = GetSelectionArrayName(InteractionOwner))
        {
            if (auto Selected = GetRawArray(InteractionOwner, SelectionName))
            {
                Selected->ResetNum();
            }
        }
        for (const char* Name : {
                "SelectedActorsReplicateToRemoteClients",
                "PlacementModeActorsReplicateToRemoteClients" })
        {
            if (auto Selected = GetRawArray(InteractionOwner, Name))
                Selected->ResetNum();
        }
        for (const char* Name : {
                "ActiveMovementMode", "ActiveBoundBehavior",
                "ActiveMovementModeReplicateToRemoteClients",
                "ActiveBoundBehaviorReplicateToRemoteClients" })
        {
            if (auto Active = GetObjectProperty<UObject*>(InteractionOwner, Name))
            {
                *Active = nullptr;
            }
        }
        if (auto ClientStop = InteractionOwner->GetFunction("ClientStopInteracting"))
        {
            InteractionOwner->Call<void>(ClientStop);
        }
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(InteractionOwner,
            L"SelectedActorsReplicateToRemoteClients");
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(InteractionOwner,
            L"PlacementModeActorsReplicateToRemoteClients");
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(InteractionOwner,
            L"ActiveBoundBehaviorReplicateToRemoteClients");
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(InteractionOwner,
            L"ActiveMovementModeReplicateToRemoteClients");
        ((AActor*)InteractionOwner)->ForceNetUpdate();
    }

    std::vector<UObject*> GetTrackedOwnersForController(UObject* ClearOwner)
    {
        std::vector<UObject*> Result;
        auto AddIfTracked = [&](UObject* Candidate)
        {
            if (!IsLiveObject(Candidate) || std::find(Result.begin(), Result.end(), Candidate) !=
                    Result.end())
            {
                return;
            }
            const bool bTracked = std::find_if(PendingStructuralDuplicates.begin(),
                PendingStructuralDuplicates.end(),
                [Candidate](const FPendingStructuralDuplicate& Pending)
                {
                    return Pending.InteractionOwner == Candidate;
                }) != PendingStructuralDuplicates.end() || HasTrackedPropMobility(Candidate);
            if (bTracked)
                Result.push_back(Candidate);
        };

        PrunePendingStructuralDuplicates();
        for (const char* Name : {
                "ActiveObjectInteractionMode", "PhoneToolActorTargetMode" })
        {
            if (auto Mode = GetObjectProperty<UObject*>(ClearOwner, Name))
                AddIfTracked(*Mode);
        }

        auto Controller = ResolveOwningController(ClearOwner);
        if (!Controller)
            return Result;
        for (const auto& Pending : PendingStructuralDuplicates)
        {
            auto Owner = Pending.InteractionOwner;
            if (ResolveOwningController(Owner) != Controller ||
                std::find(Result.begin(), Result.end(), Owner) != Result.end())
            {
                continue;
            }
            Result.push_back(Owner);
        }
        PruneTrackedPropMobilities();
        for (const auto& Entry : TrackedPropMobilities)
        {
            auto Owner = Entry.InteractionOwner;
            if (ResolveOwningController(Owner) != Controller ||
                std::find(Result.begin(), Result.end(), Owner) != Result.end())
            {
                continue;
            }
            Result.push_back(Owner);
        }
        return Result;
    }

    void ClearMovementModeTracked(UObject* Context, FFrame& Stack)
    {
        bool bExited = false;
        auto Function = Stack.GetCurrentNativeFunction();
        if (Function && Function->GetOffset("bExited") != InvalidOffset)
            Stack.StepCompiledIn(&bExited);
        Stack.IncrementCode();

        if (!IsLiveObject(Context) || !Function)
            return;

        std::vector<UObject*> OwnersToClean;
        if (Function == ObjectClearMovementFunction)
            OwnersToClean = GetTrackedOwnersForController(Context);
        else if (!GetTrackedStructuralDuplicates(Context).empty() ||
            HasTrackedPropMobility(Context))
            OwnersToClean.push_back(Context);

        FExecHandler Original = nullptr;
        if (Function == LegacyClearMovementFunction)
            Original = LegacyClearMovementOG;
        else if (Function == ModernClearMovementFunction)
            Original = ModernClearMovementOG;
        else if (Function == ObjectClearMovementFunction)
            Original = ObjectClearMovementOG;
        if (Original && Original != ClearMovementModeTracked)
        {
            Function->ExecFunction = reinterpret_cast<void*>(Original);
            if (Function->GetOffset("bExited") != InvalidOffset)
                Context->Call<void>(Function, bExited);
            else
                Context->Call<void>(Function);
            Function->ExecFunction = reinterpret_cast<void*>(ClearMovementModeTracked);
        }

        for (auto Owner : OwnersToClean)
            ResetTrackedInteractionContext(Owner);
    }

    void HookPlaceSelection(UObject* DefaultOwner, bool bModern)
    {
        if (!IsLiveObject(DefaultOwner))
            return;
        auto Function = DefaultOwner->GetFunction("ServerPlaceActorsAndClearMovementMode");
        if (!Function || Function->GetOffset("TargetTransformForBuildings") == InvalidOffset)
        {
            return;
        }

        if (bModern)
        {
            if (Function == LegacyPlaceSelectionFunction)
                return;
            ModernPlaceSelectionFunction = Function;
            Utils::ExecHook(Function, PlaceSelectionValidated, ModernPlaceSelectionOG);
        }
        else
        {
            LegacyPlaceSelectionFunction = Function;
            Utils::ExecHook(Function, PlaceSelectionValidated, LegacyPlaceSelectionOG);
        }
    }

    void HookMoveSelection(UObject* DefaultOwner, bool bModern)
    {
        if (!IsLiveObject(DefaultOwner))
            return;
        auto Function = DefaultOwner->GetFunction("ServerMoveSelectionSet");
        if (!Function || Function->GetOffset("NewSelectionToWorld") == InvalidOffset)
        {
            return;
        }

        if (bModern)
        {
            if (Function == LegacyMoveSelectionFunction)
                return;
            ModernMoveSelectionFunction = Function;
            Utils::ExecHook(Function, MoveSelectionSetFallback, ModernMoveSelectionOG);
        }
        else
        {
            if (Function == ModernMoveSelectionFunction)
                return;
            LegacyMoveSelectionFunction = Function;
            Utils::ExecHook(Function, MoveSelectionSetFallback, LegacyMoveSelectionOG);
        }
    }

    void HookClearMovement(UObject* DefaultOwner, bool bModern)
    {
        if (!IsLiveObject(DefaultOwner))
            return;
        auto Function = DefaultOwner->GetFunction("ServerClearMovementMode");
        if (!Function)
            return;

        if (bModern)
        {
            if (Function == LegacyClearMovementFunction)
                return;
            ModernClearMovementFunction = Function;
            Utils::ExecHook(Function, ClearMovementModeTracked, ModernClearMovementOG);
        }
        else
        {
            LegacyClearMovementFunction = Function;
            Utils::ExecHook(Function, ClearMovementModeTracked, LegacyClearMovementOG);
        }
    }

    void HookClearObjectInteractionModes(UObject* DefaultMoveTool)
    {
        if (!IsLiveObject(DefaultMoveTool))
            return;
        auto Function = DefaultMoveTool->GetFunction("ServerClearObjectInteractionModes");
        if (!Function || Function == LegacyClearMovementFunction)
            return;
        ObjectClearMovementFunction = Function;
        Utils::ExecHook(Function, ClearMovementModeTracked, ObjectClearMovementOG);
    }

    AActor* BeginDeferredCreativeActor(UClass* ActorClass,
        const FTransform& Transform, AActor* Owner)
    {
        auto Statics = UGameplayStatics::GetDefaultObj();
        auto Function = Statics ? Statics->GetFunction(
            "BeginDeferredActorSpawnFromClass") : nullptr;
        auto World = UWorld::GetWorld();
        if (!Statics || !Function || !World || !ActorClass)
            return nullptr;

        const int32 ReflectedSize = Function->GetPropertiesSize();
        const size_t BufferSize = ReflectedSize > 0 && ReflectedSize <= 0x1000
            ? static_cast<size_t>(ReflectedSize) : 0x1000;
        std::vector<uint8> Params(BufferSize, 0);
        const uint8 CollisionHandlingOverride = 2;
        bool bComplete = WriteFunctionParam(Function,
            Params.data(), BufferSize, "WorldContextObject", &World, sizeof(World));
        bComplete &= WriteFunctionParam(Function, Params.data(), BufferSize, "ActorClass",
            &ActorClass, sizeof(ActorClass));
        bComplete &= WriteFunctionParam(Function, Params.data(), BufferSize, "SpawnTransform",
            &Transform, FTransform::Size());
        bComplete &= WriteFunctionParam(Function, Params.data(), BufferSize,
            "CollisionHandlingOverride", &CollisionHandlingOverride,
            sizeof(CollisionHandlingOverride));
        bComplete &= WriteFunctionParam(Function, Params.data(), BufferSize, "Owner",
            &Owner, sizeof(Owner));
        const uint32 ReturnOffset = Function->GetOffset("ReturnValue");
        if (!bComplete || ReturnOffset == InvalidOffset ||
            ReturnOffset > BufferSize - sizeof(AActor*))
        {
            return nullptr;
        }
        Statics->ProcessEvent(Function, Params.data());
        return *reinterpret_cast<AActor**>(Params.data() + ReturnOffset);
    }

    AActor* FinishDeferredCreativeActor(AActor* Actor, const FTransform& Transform)
    {
        auto Statics = UGameplayStatics::GetDefaultObj();
        auto Function = Statics ? Statics->GetFunction("FinishSpawningActor") : nullptr;
        if (!Statics || !Function || !IsLiveObject(Actor))
            return nullptr;

        const int32 ReflectedSize = Function->GetPropertiesSize();
        const size_t BufferSize = ReflectedSize > 0 && ReflectedSize <= 0x1000
            ? static_cast<size_t>(ReflectedSize) : 0x1000;
        std::vector<uint8> Params(BufferSize, 0);
        bool bComplete = WriteFunctionParam(Function, Params.data(), BufferSize, "Actor",
            &Actor, sizeof(Actor));
        bComplete &= WriteFunctionParam(Function, Params.data(), BufferSize, "SpawnTransform",
            &Transform, FTransform::Size());
        const uint32 ReturnOffset = Function->GetOffset("ReturnValue");
        if (!bComplete || ReturnOffset == InvalidOffset ||
            ReturnOffset > BufferSize - sizeof(AActor*))
        {
            return nullptr;
        }
        Statics->ProcessEvent(Function, Params.data());
        return *reinterpret_cast<AActor**>(Params.data() + ReturnOffset);
    }

    bool InitializeDeferredBuildingActor(AActor* Spawned)
    {
        auto Function = IsLiveObject(Spawned) ? Spawned->GetFunction(
                "InitializeKismetSpawnedBuildingActor") : nullptr;
        if (!Function)
            return false;

        const int32 ReflectedSize = Function->GetPropertiesSize();
        const size_t BufferSize = ReflectedSize > 0 && ReflectedSize <= 0x1000
            ? static_cast<size_t>(ReflectedSize) : 0x1000;
        std::vector<uint8> Params(BufferSize, 0);
        AFortPlayerControllerAthena* SpawningController = nullptr;
        AActor* ReplacedBuilding = nullptr;
        const bool bUseAnimations = false;
        const bool bFinishSpawning = false;
        bool bComplete = WriteFunctionParam(Function, Params.data(), BufferSize, "BuildingOwner",
            &Spawned, sizeof(Spawned));
        bComplete &= WriteFunctionParam(Function, Params.data(), BufferSize, "SpawningController",
            &SpawningController, sizeof(SpawningController));

        bool bWroteAnimations = WriteFunctionParam(Function, Params.data(), BufferSize,
            "bUsePlayerBuildAnimations", &bUseAnimations, sizeof(bUseAnimations));
        if (!bWroteAnimations)
        {
            bWroteAnimations = WriteFunctionParam(Function,
                Params.data(), BufferSize, "bUseAnimations",
                &bUseAnimations, sizeof(bUseAnimations));
        }
        bComplete &= bWroteAnimations;

        if (Function->GetOffset("ReplacedBuilding") != InvalidOffset)
        {
            bComplete &= WriteFunctionParam(Function, Params.data(), BufferSize, "ReplacedBuilding",
                &ReplacedBuilding, sizeof(ReplacedBuilding));
        }
        if (Function->GetOffset("bFinishSpawning") != InvalidOffset)
        {
            bComplete &= WriteFunctionParam(Function, Params.data(), BufferSize, "bFinishSpawning",
                &bFinishSpawning, sizeof(bFinishSpawning));
        }
        if (!bComplete)
            return false;
        Spawned->ProcessEvent(Function, Params.data());
        return true;
    }

    AActor* DuplicateBuildingActor(AActor* Source, const FTransform& Transform,
        AFortPlayerControllerAthena* PlayerController, bool bValidateStructuralPlacement = false,
        bool bForInteractivePreview = false)
    {
        if (!IsSelectableActor(Source) || !PlayerController || !IsFiniteTransform(Transform) ||
            !IsActorInAuthorizedVolume(Source, PlayerController) || !IsTransformInAuthorizedVolume(
                Transform, PlayerController) || bValidateStructuralPlacement &&
                !QueryStructuralPlacement(Source, Transform))
            return nullptr;

        auto Spawned = BeginDeferredCreativeActor(Source->Class, Transform, PlayerController);
        if (!Spawned)
            return nullptr;

        const bool bStructuralPreview = bForInteractivePreview && IsStructuralBuildingActor(Source);
        bool bWrotePreviewFlag = false;
        if (bStructuralPreview)
        {
            bWrotePreviewFlag = WriteReflectedBool(Spawned, "bIsForPreviewing", true);
        }

        if (!InitializeDeferredBuildingActor(Spawned))
        {
            Spawned->K2_DestroyActor();
            return nullptr;
        }
        auto Finished = FinishDeferredCreativeActor(Spawned, Transform);
        if (!Finished)
        {
            if (IsLiveObject(Spawned))
                Spawned->K2_DestroyActor();
            return nullptr;
        }
        Spawned = Finished;

        auto SourceBuilding = Source->Cast<ABuildingSMActor>();
        auto SpawnedBuilding = Spawned->Cast<ABuildingSMActor>();
        if (SourceBuilding && SpawnedBuilding &&
            SourceBuilding->GetProperty("bPlayerPlaced", 0x20000) &&
            SpawnedBuilding->GetProperty("bPlayerPlaced", 0x20000))
        {
            WriteReflectedBool(SpawnedBuilding, "bPlayerPlaced", ReadReflectedBool(SourceBuilding,
                    "bPlayerPlaced"));
        }

        if (SourceBuilding && SpawnedBuilding)
        {
            if (SourceBuilding->HasTeam() && SpawnedBuilding->HasTeam())
                SpawnedBuilding->Team = SourceBuilding->Team;
            if (SourceBuilding->HasTeamIndex() && SpawnedBuilding->HasTeamIndex())
                SpawnedBuilding->TeamIndex = SourceBuilding->TeamIndex;
            if (SourceBuilding->HasOwnerPersistentID() && SpawnedBuilding->HasOwnerPersistentID())
            {
                CopyReflectedScalar(SpawnedBuilding, SourceBuilding, "OwnerPersistentID", 4);
            }
        }

        Spawned->ForceNetUpdate();
        SDK::DbgLog(
            "[CreativePhone] duplicate source=%p spawned=%p live=%d structural=%d preview_requested=%d preview_written=%d preview=%d register=%d\n",
            Source, Spawned, IsSelectableActor(Spawned) ? 1 : 0,
            IsStructuralBuildingActor(Source) ? 1 : 0, bStructuralPreview ? 1 : 0,
            bWrotePreviewFlag ? 1 : 0, ReadReflectedBool(Spawned, "bIsForPreviewing") ? 1 : 0,
            ReadReflectedBool(Spawned, "bRegisterWithStructuralGrid") ? 1 : 0);
        return Spawned;
    }

    bool DuplicateInteractionFallback(AFortCreativeMoveTool* MoveTool,
        const TArray<AActor*>& Actors, const FTransform& DragStart,
        AFortPlayerControllerAthena* PlayerController)
    {
        TArray<AActor*> SpawnedActors;
        const auto ExpectedActors = UniqueActors(Actors);
        SDK::DbgLog("[CreativePhone] duplicate fallback requested=%d unique=%d\n",
            Actors.Num(), static_cast<int32>(ExpectedActors.size()));
        for (auto Actor : ExpectedActors)
        {
            auto Transform = Actor->GetTransform();
            if (auto Spawned = DuplicateBuildingActor(Actor, Transform, PlayerController,
                    false, true))
            {
                SpawnedActors.Add(Spawned);
                continue;
            }

            for (auto Spawned : SpawnedActors)
            {
                if (IsLiveObject(Spawned))
                    Spawned->K2_DestroyActor();
            }
            SpawnedActors.Free();
            return false;
        }

        bool bStarted = false;
        const bool bCompleteDuplicate = !ExpectedActors.empty() && SpawnedActors.Num() ==
                static_cast<int32>(ExpectedActors.size());
        if (!UsesLegacyCreativePhoneLifecycle() && bCompleteDuplicate && SpawnedActors.Num() == 1 &&
            !IsStructuralBuildingActor(SpawnedActors[0]))
        {
            auto StartFunction = MoveTool->GetFunction("ServerStartInteracting");
            bStarted = TryNativeStartSingleFreeProp(MoveTool, StartFunction,
                AFortCreativeMoveTool::ServerStartInteracting_OG,
                AFortCreativeMoveTool::ServerStartInteracting_, SpawnedActors, DragStart);
        }
        if (bCompleteDuplicate && !bStarted)
        {
            bStarted = StartInteractionFallback(MoveTool, SpawnedActors, DragStart,
                PlayerController);
        }
        SDK::DbgLog("[CreativePhone] duplicate fallback spawned=%d started=%d\n",
            SpawnedActors.Num(), bStarted ? 1 : 0);
        if (!bStarted)
        {
            for (auto Spawned : SpawnedActors)
            {
                if (IsLiveObject(Spawned))
                    Spawned->K2_DestroyActor();
            }
        }
        else if (!UsesLegacyCreativePhoneLifecycle())
        {
            for (auto Spawned : SpawnedActors)
            {
                TrackStructuralDuplicate(MoveTool, Spawned, Spawned->GetTransform());
            }
        }
        SpawnedActors.Free();
        return bStarted;
    }

    enum class EAddNewlyPlacedPairResult
    {
        Added, AlreadyExists, Unavailable
    };

    bool HasNewlyPlacedOriginal(UObject* InteractionOwner, AActor* Original)
    {
        const auto& Schema = GetSchema();
        if (!Schema.IsSpawnPairUsable())
            return false;

        for (const char* ArrayName : {
                "NewlyPlacedActors", "SpawnHelperNewlyPlacedActors" })
        {
            auto NewlyPlaced = GetRawArray(InteractionOwner, ArrayName);
            if (!NewlyPlaced)
                continue;
            for (int32 Index = 0;
                 Index < NewlyPlaced->Num(); ++Index)
            {
                auto Entry = NewlyPlaced->Data + static_cast<size_t>(Index) * Schema.SpawnPairSize;
                if (!SDK::MemReadable(Entry, Schema.SpawnPairSize))
                    return false;
                if (*(AActor**)(Entry + Schema.PairOriginalActor) == Original)
                {
                    return true;
                }
            }
        }
        return false;
    }

    EAddNewlyPlacedPairResult AddNewlyPlacedPair(UObject* InteractionOwner,
        AActor* Original, AActor* Spawned, bool bForPreviewing)
    {
        const auto& Schema = GetSchema();
        if (!Schema.IsSpawnPairUsable())
            return EAddNewlyPlacedPairResult::Unavailable;

        auto NewlyPlaced = GetRawArray(InteractionOwner, "NewlyPlacedActors");
        if (!NewlyPlaced)
            NewlyPlaced = GetRawArray(InteractionOwner, "SpawnHelperNewlyPlacedActors");
        if (!NewlyPlaced)
            return EAddNewlyPlacedPairResult::Unavailable;

        for (int32 Index = 0; Index < NewlyPlaced->Num(); ++Index)
        {
            auto Entry = NewlyPlaced->Data + static_cast<size_t>(Index) * Schema.SpawnPairSize;
            if (!SDK::MemReadable(Entry, Schema.SpawnPairSize))
                return EAddNewlyPlacedPairResult::Unavailable;

            auto ExistingOriginal = *(AActor**)(Entry + Schema.PairOriginalActor);
            if (ExistingOriginal == Original)
                return EAddNewlyPlacedPairResult::AlreadyExists;
        }

        std::vector<uint8> Pair(static_cast<size_t>(Schema.SpawnPairSize), 0);
        memcpy(Pair.data() + Schema.PairOriginalActor, &Original, sizeof(Original));
        memcpy(Pair.data() + Schema.PairSpawnedActor, &Spawned, sizeof(Spawned));
        SetStructBool(Pair.data(), Schema.SpawnPairSize, Schema.SpawnPair,
            Schema.bSpawnedActorIsForPreview, "bSpawnedActorIsForPreview", bForPreviewing);
        NewlyPlaced->Add(*Pair.data(), Schema.SpawnPairSize);

        if (auto ClientNeeds = GetObjectProperty<bool>(InteractionOwner,
                "bClientNeedsToProcessNewlyPlacedActors"))
        {
            *ClientNeeds = true;
        }
        if (WriteReflectedBool(InteractionOwner, "bHasSpawnedAnActor", true))
        {
            VersionFeatureAdapter::MarkReplicatedPropertyDirty(
                InteractionOwner, L"bHasSpawnedAnActor");
        }
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(InteractionOwner, L"NewlyPlacedActors");
        ((AActor*)InteractionOwner)->ForceNetUpdate();
        return EAddNewlyPlacedPairResult::Added;
    }

    bool IsLegacyActorArrayPlacement(UObject* InteractionOwner, UFunction* SpawnFunction)
    {
        return IsLiveObject(InteractionOwner) && SpawnFunction &&
            InteractionOwner->GetProperty("NewlyPlacedActors") &&
            SpawnFunction->GetOffset("bIgnoreStructuralIssues") == InvalidOffset &&
            SpawnFunction->GetOffset("bForPreviewing") == InvalidOffset;
    }

    EAddNewlyPlacedPairResult AddLegacyNewlyPlacedActor(UObject* InteractionOwner, AActor* Spawned)
    {
        if (!IsLiveObject(InteractionOwner) || !IsLiveObject(Spawned))
            return EAddNewlyPlacedPairResult::Unavailable;

        auto NewlyPlaced = GetRawArray(InteractionOwner, "NewlyPlacedActors");
        if (!NewlyPlaced)
            return EAddNewlyPlacedPairResult::Unavailable;

        for (int32 Index = 0; Index < NewlyPlaced->Num(); ++Index)
        {
            auto Entry = NewlyPlaced->Data + static_cast<size_t>(Index) * sizeof(AActor*);
            if (!SDK::MemReadable(Entry, sizeof(AActor*)))
                return EAddNewlyPlacedPairResult::Unavailable;
            if (*(AActor**)Entry == Spawned)
                return EAddNewlyPlacedPairResult::AlreadyExists;
        }

        NewlyPlaced->Add(*reinterpret_cast<uint8*>(&Spawned), sizeof(AActor*));
        if (auto ClientNeeds = GetObjectProperty<bool>(InteractionOwner,
                "bClientNeedsToProcessNewlyPlacedActors"))
        {
            *ClientNeeds = true;
        }
        if (WriteReflectedBool(InteractionOwner, "bHasSpawnedAnActor", true))
        {
            VersionFeatureAdapter::MarkReplicatedPropertyDirty(
                InteractionOwner, L"bHasSpawnedAnActor");
        }
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(InteractionOwner, L"NewlyPlacedActors");
        ((AActor*)InteractionOwner)->ForceNetUpdate();
        return EAddNewlyPlacedPairResult::Added;
    }

    int32 GetRawArrayCount(UObject* InteractionOwner,
        const char* PrimaryName, const char* AlternateName = nullptr)
    {
        auto Array = GetRawArray(InteractionOwner, PrimaryName);
        if (!Array && AlternateName)
            Array = GetRawArray(InteractionOwner, AlternateName);
        return Array ? Array->Num() : -1;
    }

    struct FPlacedArrayCounts
    {
        int32 NewlyPlaced = -1;
        int32 SpawnHelper = -1;
    };

    FPlacedArrayCounts GetPlacedArrayCounts(UObject* InteractionOwner)
    {
        return {
            GetRawArrayCount(InteractionOwner, "NewlyPlacedActors"), GetRawArrayCount(
                InteractionOwner, "SpawnHelperNewlyPlacedActors")
        };
    }

    struct FPairReconcileResult
    {
        std::vector<AActor*> SatisfiedSources;

        bool Contains(AActor* Source) const
        {
            return std::find(SatisfiedSources.begin(), SatisfiedSources.end(), Source) !=
                SatisfiedSources.end();
        }
    };

    bool IsExpectedSource(const std::vector<AActor*>& Sources, AActor* Source)
    {
        return std::find(Sources.begin(), Sources.end(), Source) != Sources.end();
    }

    FPairReconcileResult ReconcilePlacedPairTails(UObject* InteractionOwner,
        const FPlacedArrayCounts& Before, const std::vector<AActor*>& ExpectedSources)
    {
        FPairReconcileResult Result;
        const auto& Schema = GetSchema();
        if (!IsLiveObject(InteractionOwner) || !Schema.IsSpawnPairUsable())
        {
            return Result;
        }

        struct FArrayJournal
        {
            const char* Name;
            int32 Before;
        };
        const FArrayJournal Journals[] = {
            { "NewlyPlacedActors", Before.NewlyPlaced },
            { "SpawnHelperNewlyPlacedActors", Before.SpawnHelper }
        };

        struct FAcceptedPair
        {
            AActor* Original;
            AActor* Spawned;
        };
        std::vector<FAcceptedPair> AcceptedPairs;
        bool bChanged = false;
        for (const auto& Journal : Journals)
        {
            auto Array = GetRawArray(InteractionOwner, Journal.Name);
            if (!Array || Journal.Before < 0 || Journal.Before > Array->Num())
            {
                continue;
            }

            std::vector<AActor*> JournalSources;
            std::vector<AActor*> JournalSpawnedActors;
            int32 Index = Journal.Before;
            while (Index < Array->Num())
            {
                auto Entry = Array->Data + static_cast<size_t>(Index) * Schema.SpawnPairSize;
                if (!SDK::MemReadable(Entry, Schema.SpawnPairSize))
                {
                    break;
                }
                auto Original = *reinterpret_cast<AActor**>(Entry + Schema.PairOriginalActor);
                auto Spawned = *reinterpret_cast<AActor**>(Entry + Schema.PairSpawnedActor);
                auto AcceptedForSource = std::find_if(AcceptedPairs.begin(), AcceptedPairs.end(),
                    [Original](const FAcceptedPair& Pair)
                    {
                        return Pair.Original == Original;
                    });
                const bool bMatchesMirroredPair = AcceptedForSource == AcceptedPairs.end() ||
                    AcceptedForSource->Spawned == Spawned;
                const bool bSpawnedUsedForDifferentSource = std::find_if(AcceptedPairs.begin(),
                        AcceptedPairs.end(), [Original, Spawned](const FAcceptedPair& Pair)
                        {
                            return Pair.Spawned == Spawned && Pair.Original != Original;
                        }) != AcceptedPairs.end();
                const bool bValid = IsExpectedSource(ExpectedSources, Original) &&
                    IsLiveObject(Spawned) && Spawned != Original && IsLiveObject(Original) &&
                    Spawned->Class == Original->Class && bMatchesMirroredPair &&
                    !bSpawnedUsedForDifferentSource && std::find(JournalSpawnedActors.begin(),
                        JournalSpawnedActors.end(), Spawned) == JournalSpawnedActors.end() &&
                    std::find(JournalSources.begin(), JournalSources.end(), Original) ==
                        JournalSources.end();

                if (bValid)
                {
                    if (!Result.Contains(Original))
                    {
                        Result.SatisfiedSources.push_back(Original);
                    }
                    if (AcceptedForSource == AcceptedPairs.end())
                        AcceptedPairs.push_back({ Original, Spawned });
                    JournalSources.push_back(Original);
                    JournalSpawnedActors.push_back(Spawned);
                    ++Index;
                    continue;
                }

                Array->Remove(Index, Schema.SpawnPairSize);
                bChanged = true;
            }
        }

        if (bChanged)
        {
            VersionFeatureAdapter::MarkReplicatedPropertyDirty(
                InteractionOwner, L"NewlyPlacedActors");
            VersionFeatureAdapter::MarkReplicatedPropertyDirty(
                InteractionOwner, L"SpawnHelperNewlyPlacedActors");
            ((AActor*)InteractionOwner)->ForceNetUpdate();
        }
        return Result;
    }

    const char* GetSelectionArrayName(UObject* InteractionOwner)
    {
        if (!IsLiveObject(InteractionOwner))
            return nullptr;
        if (InteractionOwner->GetProperty("SelectedActors"))
            return "SelectedActors";
        if (InteractionOwner->GetProperty("PlacementModeActors"))
            return "PlacementModeActors";
        return nullptr;
    }

    bool SelectionContainsExactly(UObject* InteractionOwner,
        const std::vector<AActor*>& ExpectedActors)
    {
        const char* ArrayName = GetSelectionArrayName(InteractionOwner);
        auto Selected = ArrayName ? GetRawArray(InteractionOwner, ArrayName) : nullptr;
        const auto& Schema = GetSchema();
        if (!Selected || !Schema.IsStartUsable() || Selected->Num() != static_cast<int32>(
                ExpectedActors.size()))
        {
            return false;
        }

        for (auto Expected : ExpectedActors)
        {
            bool bFound = false;
            for (int32 Index = 0; Index < Selected->Num(); ++Index)
            {
                auto Entry = Selected->Data + static_cast<size_t>(Index) * Schema.SelectedActorSize;
                if (!SDK::MemReadable(Entry, Schema.SelectedActorSize))
                    return false;
                if (*reinterpret_cast<AActor**>(Entry + Schema.SelectedActorActor) == Expected)
                {
                    bFound = true;
                    break;
                }
            }
            if (!bFound)
                return false;
        }
        return true;
    }

    std::vector<AActor*> UniqueActors(const TArray<AActor*>& Actors)
    {
        std::vector<AActor*> Result;
        Result.reserve(static_cast<size_t>((std::max)(0, Actors.Num())));
        for (auto Actor : Actors)
        {
            if (!IsSelectableActor(Actor) || std::find(Result.begin(), Result.end(), Actor) !=
                    Result.end())
            {
                continue;
            }
            Result.push_back(Actor);
        }
        return Result;
    }

    std::vector<AActor*> UniqueActorTargets(const TArray<UObject*>& Targets)
    {
        std::vector<AActor*> Result;
        Result.reserve(static_cast<size_t>((std::max)(0, Targets.Num())));
        for (auto Target : Targets)
        {
            auto Actor = IsLiveObject(Target) ? Target->Cast<AActor>() : nullptr;
            if (!IsSelectableActor(Actor) || std::find(Result.begin(), Result.end(), Actor) !=
                    Result.end())
            {
                continue;
            }
            Result.push_back(Actor);
        }
        return Result;
    }

    bool ValidateRequestedActors(const std::vector<AActor*>& Actors,
        AFortPlayerControllerAthena* PlayerController, int32 RawCount = -1)
    {
        if (Actors.empty() || Actors.size() > MaxCreativeSelection || RawCount >= 0 &&
                static_cast<int32>(Actors.size()) != RawCount)
            return false;
        for (auto Actor : Actors)
        {
            if (!IsActorInAuthorizedVolume(Actor, PlayerController))
                return false;
        }
        return true;
    }

    bool GetValidatedSelectedSources(UObject* InteractionOwner,
        AFortPlayerControllerAthena* PlayerController, std::vector<AActor*>& OutSources)
    {
        OutSources.clear();
        const char* ArrayName = GetSelectionArrayName(InteractionOwner);
        auto Selected = ArrayName ? GetRawArray(InteractionOwner, ArrayName) : nullptr;
        const auto& Schema = GetSchema();
        if (!Selected || !Schema.IsStartUsable() || Selected->Num() <= 0 ||
            Selected->Num() > MaxCreativeSelection)
        {
            return false;
        }

        OutSources.reserve(static_cast<size_t>(Selected->Num()));
        for (int32 Index = 0; Index < Selected->Num(); ++Index)
        {
            auto Entry = Selected->Data + static_cast<size_t>(Index) * Schema.SelectedActorSize;
            if (!SDK::MemReadable(Entry, Schema.SelectedActorSize))
            {
                OutSources.clear();
                return false;
            }
            auto Source = *reinterpret_cast<AActor**>(Entry + Schema.SelectedActorActor);
            if (!IsActorInAuthorizedVolume(Source, PlayerController) ||
                std::find(OutSources.begin(), OutSources.end(), Source) != OutSources.end())
            {
                OutSources.clear();
                return false;
            }
            OutSources.push_back(Source);
        }
        return OutSources.size() == static_cast<size_t>(Selected->Num());
    }

    void DiscardPartialDuplicateSelection(UObject* InteractionOwner,
        const std::vector<AActor*>& OriginalActors)
    {
        const char* ArrayName = GetSelectionArrayName(InteractionOwner);
        auto Selected = ArrayName ? GetRawArray(InteractionOwner, ArrayName) : nullptr;
        const auto& Schema = GetSchema();
        if (!Selected || !Schema.IsStartUsable())
            return;

        Selected->ResetNum();
        if (auto Remote = GetRawArray(InteractionOwner, "SelectedActorsReplicateToRemoteClients"))
            Remote->ResetNum();
        if (auto Remote = GetRawArray(InteractionOwner,
                "PlacementModeActorsReplicateToRemoteClients"))
            Remote->ResetNum();
    }

    FQuat IdentityQuat()
    {
        FQuat Result{};
        Result.X = 0;
        Result.Y = 0;
        Result.Z = 0;
        Result.W = 1;
        return Result;
    }

    bool IsValidTargetArray(const TArray<UObject*>& Targets)
    {
        return Targets.Num() > 0 && Targets.Num() <= MaxCreativeSelection &&
            SDK::MemReadable(Targets.Data, static_cast<size_t>(Targets.Num()) * sizeof(UObject*));
    }

    void InvokeOriginalObjectStart(UObject* Context, UFunction* Function, FExecHandler Original,
        const TArray<UObject*>& Targets, const FTransform& DragStart, FExecHandler Hook)
    {
        if (!IsLiveObject(Context) || !Function || !Original || Original == Hook)
        {
            return;
        }

        Function->ExecFunction = (void*)Original;
        Context->Call<void>(Function, Targets, DragStart);
        Function->ExecFunction = (void*)Hook;
    }

    bool StartTargetInteractionFallback(UObject* Context,
        const TArray<UObject*>& Targets, const FTransform& DragStart,
        AFortPlayerControllerAthena* PlayerController)
    {
        if (!IsLiveObject(Context) || !IsValidTargetArray(Targets))
            return false;

        const auto& Schema = GetSchema();
        const char* ArrayName = GetSelectionArrayName(Context);
        auto PlacementActors = ArrayName ? GetRawArray(Context, ArrayName) : nullptr;
        if (!Schema.IsStartUsable() || !PlacementActors)
            return false;

        TArray<AActor*> Actors;
        PlacementActors->ResetNum();
        auto RemotePlacementActors = GetRawArray(Context,
            "PlacementModeActorsReplicateToRemoteClients");
        if (RemotePlacementActors == PlacementActors)
            RemotePlacementActors = nullptr;
        if (RemotePlacementActors)
            RemotePlacementActors->ResetNum();
        if (auto NewlyPlaced = GetRawArray(Context, "NewlyPlacedActors"))
        {
            NewlyPlaced->ResetNum();
        }
        if (auto SpawnHelper = GetRawArray(Context, "SpawnHelperNewlyPlacedActors"))
        {
            SpawnHelper->ResetNum();
        }
        for (auto Target : Targets)
        {
            auto Actor = IsLiveObject(Target) ? Target->Cast<AActor>() : nullptr;
            if (Actors.Contains(Actor))
                continue;
            if (!IsActorInAuthorizedVolume(Actor, PlayerController))
            {
                PlacementActors->ResetNum();
                if (RemotePlacementActors)
                    RemotePlacementActors->ResetNum();
                Actors.Free();
                return false;
            }

            std::vector<uint8> Info;
            if (!BuildSelectedActorInfo(Actor, DragStart, Info))
            {
                PlacementActors->ResetNum();
                if (RemotePlacementActors)
                    RemotePlacementActors->ResetNum();
                Actors.Free();
                return false;
            }
            PlacementActors->Add(*Info.data(), Schema.SelectedActorSize);
            if (RemotePlacementActors)
            {
                RemotePlacementActors->Add(*Info.data(), Schema.SelectedActorSize);
            }
            Actors.Add(Actor);
        }
        if (Actors.Num() <= 0)
        {
            Actors.Free();
            return false;
        }

        UObject* MovementMode = SelectMovementMode(Context, Actors);
        if (!MovementMode)
        {
            Actors.Free();
            PlacementActors->ResetNum();
            if (RemotePlacementActors)
                RemotePlacementActors->ResetNum();
            return false;
        }

        for (const char* Name : {
                "ActiveBoundBehavior", "ActiveBoundBehaviorReplicateToRemoteClients",
                "ActiveMovementMode" })
        {
            if (auto Property = GetObjectProperty<UObject*>(Context, Name))
            {
                *Property = MovementMode;
            }
        }
        if (auto SetBound = Context->GetFunction("SetBoundManipulateInteractBehavior"))
        {
            Context->Call<void>(SetBound, MovementMode);
        }
        else if (auto Bound = GetObjectProperty<UObject*>(
                Context, "BoundManipulateInteractBehavior"))
        {
            *Bound = MovementMode;
        }

        auto ClientStart = Context->GetFunction("ClientStartInteracting");
        FTransform SelectionToWorld = DragStart;
        auto Bounds = BuildSelectionBounds(Actors, SelectionToWorld);
        if (!ClientStart || Bounds.empty())
        {
            Actors.Free();
            PlacementActors->ResetNum();
            if (RemotePlacementActors)
                RemotePlacementActors->ResetNum();
            return false;
        }

        if (auto StoredTransform = GetObjectProperty<FTransform>(Context, "SelectionToWorld"))
        {
            memcpy(StoredTransform, &SelectionToWorld, FTransform::Size());
        }
        if (auto DragStartTransform = GetObjectProperty<FTransform>(
                Context, "SelectionToWorldAtDragStart"))
        {
            memcpy(DragStartTransform, &SelectionToWorld, FTransform::Size());
        }
        if (auto Phone = GetEquippedCreativePhone(PlayerController))
        {
            if (auto StoredTransform = GetObjectProperty<FTransform>(Phone, "SelectionToWorld"))
            {
                memcpy(StoredTransform, &SelectionToWorld, FTransform::Size());
            }
            if (auto DragStartTransform = GetObjectProperty<FTransform>(
                        Phone, "SelectionToWorldAtDragStart"))
            {
                memcpy(DragStartTransform, &SelectionToWorld, FTransform::Size());
            }
        }
        const uint32 StoredBoundsOffset = Context->GetOffset("SelectionSpaceActorsBounds");
        if (StoredBoundsOffset != InvalidOffset && StoredBoundsOffset <= 0x10000 &&
            SDK::MemReadable(reinterpret_cast<uint8*>(Context) + StoredBoundsOffset, Bounds.size()))
        {
            memcpy(reinterpret_cast<uint8*>(Context) + StoredBoundsOffset,
                Bounds.data(), Bounds.size());
        }

        const int32 ReflectedSize = ClientStart->GetPropertiesSize();
        const size_t BufferSize = ReflectedSize > 0 && ReflectedSize <= 0x1000
            ? static_cast<size_t>(ReflectedSize) : 0x1000;
        std::vector<uint8> Params(BufferSize, 0);
        bool bWroteMode = WriteFunctionParam(ClientStart, Params.data(), BufferSize,
            "NewActiveBoundBehavior", &MovementMode, sizeof(MovementMode));
        if (!bWroteMode)
        {
            bWroteMode = WriteFunctionParam(ClientStart, Params.data(), BufferSize,
                "NewActiveMovementMode", &MovementMode, sizeof(MovementMode));
        }
        bool bWroteActors = WriteFunctionParam(ClientStart, Params.data(), BufferSize,
            "NewPlacementActors", PlacementActors, sizeof(*PlacementActors));
        if (!bWroteActors)
        {
            bWroteActors = WriteFunctionParam(ClientStart, Params.data(), BufferSize,
                "NewSelectedActors", PlacementActors, sizeof(*PlacementActors));
        }

        const bool bWroteTransform = WriteFunctionParam(ClientStart, Params.data(), BufferSize,
            "NewSelectionToWorld", &SelectionToWorld, FTransform::Size());
        const bool bWroteBounds = WriteFunctionParam(ClientStart, Params.data(), BufferSize,
            "NewSelectionSpaceActorBounds", Bounds.data(), Bounds.size());
        if (!bWroteMode || !bWroteActors || !bWroteTransform || !bWroteBounds)
        {
            Actors.Free();
            PlacementActors->ResetNum();
            if (RemotePlacementActors)
                RemotePlacementActors->ResetNum();
            return false;
        }

        auto InitialRotation = IdentityQuat();
        WriteFunctionParam(ClientStart, Params.data(), BufferSize,
            "InitialRotationOffset", &InitialRotation, FQuat::Size());
        if (IsFreeMovementBehavior(MovementMode))
        {
            for (auto Actor : Actors)
            {
                if (EnsureMovableProp(Context, Actor))
                    continue;
                RestoreTrackedPropMobilities(Context);
                Actors.Free();
                PlacementActors->ResetNum();
                if (RemotePlacementActors)
                    RemotePlacementActors->ResetNum();
                return false;
            }
        }
        Context->ProcessEvent(ClientStart, Params.data());

        if (auto StartServer = MovementMode->GetFunction("StartCreativeInteractionOnServer"))
        {
            MovementMode->Call<void>(StartServer);
        }
        if (auto RemoteUpdate = Context->GetFunction("RemoteClientsUpdateSelectedActors"))
        {
            Context->Call<void>(RemoteUpdate, *PlacementActors, MovementMode);
        }
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(
            Context, L"PlacementModeActorsReplicateToRemoteClients");
        VersionFeatureAdapter::MarkReplicatedPropertyDirty(
            Context, L"ActiveBoundBehaviorReplicateToRemoteClients");
        ((AActor*)Context)->ForceNetUpdate();
        Actors.Free();
        return true;
    }

    void PhoneTargetStartInteracting(UObject* Context, FFrame& Stack)
    {
        TArray<UObject*> Targets;
        FTransform DragStart{};
        Stack.StepCompiledIn(&Targets);
        Stack.StepCompiledIn(&DragStart);
        Stack.IncrementCode();

        if (!IsLiveObject(Context) || !IsValidTargetArray(Targets) || !IsFiniteTransform(DragStart))
            return;

        AFortPlayerControllerAthena* PlayerController = nullptr;
        if (!HasCreativeEditAuthority(Context, PlayerController))
            return;

        const auto ExpectedActors = UniqueActorTargets(Targets);
        if (!ValidateRequestedActors(ExpectedActors, PlayerController, Targets.Num()))
            return;

        if (!ExpectedActors.empty())
        {
            if (!StartTargetInteractionFallback(Context, Targets, DragStart, PlayerController))
            {
                SDK::DbgLog("[CreativePhone] modern multi-selection fallback unavailable\n");
            }
            return;
        }

        const char* ArrayName = GetSelectionArrayName(Context);
        auto PlacementActors = ArrayName ? GetRawArray(Context, ArrayName) : nullptr;
        if (PlacementActors)
            PlacementActors->ResetNum();

        auto Function = Stack.GetCurrentNativeFunction();
        InvokeOriginalObjectStart(Context, Function,
            PhoneTargetStartInteractingOG, Targets, DragStart, PhoneTargetStartInteracting);
        if (SelectionContainsExactly(Context, ExpectedActors))
            return;

        if (!StartTargetInteractionFallback(Context, Targets, DragStart, PlayerController))
        {
            SDK::DbgLog(
                "[CreativePhone] modern selection fallback unavailable for this reflected layout\n");
        }
    }

    void PhoneTargetDuplicateStartInteracting(UObject* Context, FFrame& Stack)
    {
        TArray<UObject*> Targets;
        FTransform DragStart{};
        Stack.StepCompiledIn(&Targets);
        Stack.StepCompiledIn(&DragStart);
        Stack.IncrementCode();

        if (!IsLiveObject(Context) || !IsValidTargetArray(Targets) || !IsFiniteTransform(DragStart))
            return;

        AFortPlayerControllerAthena* PlayerController = nullptr;
        if (!HasCreativeEditAuthority(Context, PlayerController))
            return;

        const auto ExpectedActors = UniqueActorTargets(Targets);
        if (!ValidateRequestedActors(ExpectedActors, PlayerController, Targets.Num()))
            return;

        if (!ExpectedActors.empty())
        {
            TArray<UObject*> SpawnedTargets;
            for (auto Source : ExpectedActors)
            {
                auto Spawned = DuplicateBuildingActor(
                    Source, Source->GetTransform(), PlayerController, false, true);
                if (!Spawned)
                {
                    for (auto SpawnedTarget : SpawnedTargets)
                    {
                        if (auto Actor = IsLiveObject(SpawnedTarget)
                                ? SpawnedTarget->Cast<AActor>() : nullptr)
                            Actor->K2_DestroyActor();
                    }
                    SpawnedTargets.Free();
                    return;
                }
                SpawnedTargets.Add(Spawned);
            }
            const bool bStarted = StartTargetInteractionFallback(
                Context, SpawnedTargets, DragStart, PlayerController);
            if (!bStarted)
            {
                for (auto SpawnedTarget : SpawnedTargets)
                {
                    if (auto Actor = IsLiveObject(SpawnedTarget)
                            ? SpawnedTarget->Cast<AActor>() : nullptr)
                        Actor->K2_DestroyActor();
                }
            }
            else
            {
                for (auto SpawnedTarget : SpawnedTargets)
                {
                    if (auto Actor = IsLiveObject(SpawnedTarget)
                            ? SpawnedTarget->Cast<AActor>() : nullptr)
                    {
                        TrackStructuralDuplicate(Context, Actor, Actor->GetTransform());
                    }
                }
            }
            SpawnedTargets.Free();
            return;
        }

        const char* ArrayName = GetSelectionArrayName(Context);
        auto PlacementActors = ArrayName ? GetRawArray(Context, ArrayName) : nullptr;
        if (PlacementActors)
            PlacementActors->ResetNum();

        auto Function = Stack.GetCurrentNativeFunction();
        InvokeOriginalObjectStart(Context, Function, PhoneTargetDuplicateStartInteractingOG,
            Targets, DragStart, PhoneTargetDuplicateStartInteracting);
        if (PlacementActors && PlacementActors->Num() == static_cast<int32>(ExpectedActors.size()))
            return;
        DiscardPartialDuplicateSelection(Context, ExpectedActors);

        TArray<UObject*> SpawnedTargets;
        for (auto Source : ExpectedActors)
        {
            auto Spawned = DuplicateBuildingActor(Source, Source->GetTransform(), PlayerController,
                false, true);
            if (Spawned)
            {
                SpawnedTargets.Add(Spawned);
                continue;
            }

            for (auto Target : SpawnedTargets)
            {
                if (auto Actor = IsLiveObject(Target) ? Target->Cast<AActor>() : nullptr)
                    Actor->K2_DestroyActor();
            }
            SpawnedTargets.Free();
            return;
        }

        const bool bStarted = !ExpectedActors.empty() && SpawnedTargets.Num() ==
                static_cast<int32>(ExpectedActors.size()) && StartTargetInteractionFallback(
                Context, SpawnedTargets, DragStart, PlayerController);
        if (!bStarted)
        {
            for (auto Target : SpawnedTargets)
            {
                if (auto Actor = IsLiveObject(Target) ? Target->Cast<AActor>() : nullptr)
                {
                    Actor->K2_DestroyActor();
                }
            }
        }
        else
        {
            for (auto SpawnedTarget : SpawnedTargets)
            {
                if (auto Actor = IsLiveObject(SpawnedTarget)
                        ? SpawnedTarget->Cast<AActor>() : nullptr)
                {
                    TrackStructuralDuplicate(Context, Actor, Actor->GetTransform());
                }
            }
        }
        SpawnedTargets.Free();
    }

    void InvokeOriginalStart(AFortCreativeMoveTool* MoveTool, UFunction* Function, void* Original,
        const TArray<AActor*>& Actors, const FTransform& DragStart, void* Hook)
    {
        if (!Function || !Original || Original == Hook)
            return;
        Function->ExecFunction = Original;
        MoveTool->Call<void>(Function, Actors, DragStart);
        Function->ExecFunction = Hook;
    }

    bool TryNativeStartSingleFreeProp(AFortCreativeMoveTool* MoveTool, UFunction* Function,
        FExecHandler Original, FExecHandler Hook, const TArray<AActor*>& Actors,
        const FTransform& DragStart)
    {
        const auto ExpectedActors = UniqueActors(Actors);
        if (!IsLiveObject(MoveTool) || !Function || !Original ||
            Original == Hook || ExpectedActors.size() != 1 ||
            IsStructuralBuildingActor(ExpectedActors.front()))
        {
            return false;
        }
        if (!UsesLegacyCreativePhoneLifecycle() && !EnsureMovableProp(
                MoveTool, ExpectedActors.front()))
        {
            return false;
        }

        if (auto SelectedActors = GetRawArray(MoveTool, "SelectedActors"))
        {
            SelectedActors->ResetNum();
        }
        InvokeOriginalStart(MoveTool, Function, (void*)Original, Actors, DragStart, (void*)Hook);

        const bool bStarted = SelectionContainsExactly(MoveTool, ExpectedActors) &&
            IsFreeMovementBehavior(ResolvePlacementValidationBehavior(MoveTool));
        if (!bStarted)
        {
            RestoreTrackedPropMobilityForActor(MoveTool, ExpectedActors.front());
        }
        return bStarted;
    }

    void InvokeOriginalSpawn(UObject* InteractionOwner,
        UFunction* Function, void* Original, UObject* TargetToSpawn,
        const FTransform& TargetTransform, bool bAllowOverlap,
        bool bAllowGravity, bool bIgnoreStructuralIssues,
        bool bForPreviewing, bool bNotifyLiveEdit, void* Hook)
    {
        if (!IsLiveObject(InteractionOwner) || !Function || !Original || Original == Hook)
        {
            return;
        }
        Function->ExecFunction = Original;
        InteractionOwner->Call<void>(Function, TargetToSpawn, TargetTransform,
            bAllowOverlap, bAllowGravity, bIgnoreStructuralIssues, bForPreviewing, bNotifyLiveEdit);
        Function->ExecFunction = Hook;
    }

    void InvokeOriginalSpawnSelected(UObject* InteractionOwner,
        UFunction* Function, FExecHandler Original, bool bAllowOverlap, bool bAllowGravity,
        bool bIgnoreStructuralIssues, bool bForPreviewing, bool bNotifyLiveEdit, FExecHandler Hook)
    {
        if (!IsLiveObject(InteractionOwner) || !Function || !Original || Original == Hook)
        {
            return;
        }

        Function->ExecFunction = (void*)Original;
        InteractionOwner->Call<void>(Function,
            bAllowOverlap, bAllowGravity, bIgnoreStructuralIssues, bForPreviewing, bNotifyLiveEdit);
        Function->ExecFunction = (void*)Hook;
    }

    void SpawnSelectedActorsFallback(UObject* Context, FFrame& Stack)
    {
        bool bAllowOverlap = false;
        bool bAllowGravity = false;
        bool bIgnoreStructuralIssues = false;
        bool bForPreviewing = false;
        bool bNotifyLiveEdit = false;
        bool bNotifyOwnerOnFailure = false;
        Stack.StepCompiledIn(&bAllowOverlap);
        Stack.StepCompiledIn(&bAllowGravity);
        Stack.StepCompiledIn(&bIgnoreStructuralIssues);
        Stack.StepCompiledIn(&bForPreviewing);
        auto Function = Stack.GetCurrentNativeFunction();
        if (Function && Function->GetOffset("bNotifyLiveEdit") != InvalidOffset)
        {
            Stack.StepCompiledIn(&bNotifyLiveEdit);
        }
        else if (Function && Function->GetOffset("bNotifyOwnerOnFailure") != InvalidOffset)
        {
            Stack.StepCompiledIn(&bNotifyOwnerOnFailure);
        }
        Stack.IncrementCode();

        if (!IsLiveObject(Context))
            return;

        AFortPlayerControllerAthena* PlayerController = nullptr;
        if (!HasCreativeEditAuthority(Context, PlayerController))
            return;

        std::vector<AActor*> Sources;
        if (!GetValidatedSelectedSources(Context, PlayerController, Sources))
            return;

        const bool bAnyStructural = std::any_of(Sources.begin(), Sources.end(), [](AActor* Source)
            {
                return IsStructuralBuildingActor(Source);
            });
        const bool bStrictStructuralFinal = !bForPreviewing && bAnyStructural;
        const bool EffectiveAllowOverlap = bStrictStructuralFinal ? false : bAllowOverlap;
        const bool EffectiveIgnoreStructuralIssues = bStrictStructuralFinal ? false :
                bIgnoreStructuralIssues;

        const auto BeforePlaced = GetPlacedArrayCounts(Context);
        FExecHandler Original = nullptr;
        if (Function == LegacySpawnSelectedFunction)
            Original = LegacySpawnSelectedOG;
        else if (Function == ModernSpawnSelectedFunction)
            Original = ModernSpawnSelectedOG;
        const auto& Schema = GetSchema();
        auto SpawnOne = Context->GetFunction("ServerSpawnActorWithTransform");
        if (!SpawnOne)
        {
            return;
        }

        if (!Schema.IsSpawnPairUsable())
        {
            for (auto Source : Sources)
            {
                const bool bStrictThis = !bForPreviewing && IsStructuralBuildingActor(Source);
                const FTransform Transform = Source->GetTransform();
                Context->Call<void>(SpawnOne, Source, Transform,
                    bStrictThis ? false : bAllowOverlap, bAllowGravity,
                    bStrictThis ? false : bIgnoreStructuralIssues, bForPreviewing, bNotifyLiveEdit);
            }
            return;
        }

        InvokeOriginalSpawnSelected(Context, Function, Original,
            EffectiveAllowOverlap, bAllowGravity, EffectiveIgnoreStructuralIssues, bForPreviewing,
            bNotifyLiveEdit || bNotifyOwnerOnFailure, SpawnSelectedActorsFallback);
        auto Reconciled = ReconcilePlacedPairTails(Context, BeforePlaced, Sources);

        for (auto Source : Sources)
        {
            if (Reconciled.Contains(Source))
                continue;
            const bool bStrictThis = !bForPreviewing && IsStructuralBuildingActor(Source);
            const FTransform Transform = Source->GetTransform();
            Context->Call<void>(SpawnOne, Source, Transform, bStrictThis ? false : bAllowOverlap,
                bAllowGravity, bStrictThis ? false : bIgnoreStructuralIssues,
                bForPreviewing, bNotifyLiveEdit);
        }
    }

    void ModernSpawnActorWithTransform(UObject* Context, FFrame& Stack)
    {
        UObject* TargetToSpawn = nullptr;
        FTransform TargetTransform{};
        bool bAllowOverlap = false;
        bool bAllowGravity = false;
        bool bIgnoreStructuralIssues = false;
        bool bForPreviewing = false;
        bool bNotifyLiveEdit = false;
        Stack.StepCompiledIn(&TargetToSpawn);
        Stack.StepCompiledIn(&TargetTransform);
        Stack.StepCompiledIn(&bAllowOverlap);
        Stack.StepCompiledIn(&bAllowGravity);
        Stack.StepCompiledIn(&bIgnoreStructuralIssues);
        Stack.StepCompiledIn(&bForPreviewing);
        auto Function = Stack.GetCurrentNativeFunction();
        if (Function && Function->GetOffset("bNotifyLiveEdit") != InvalidOffset)
        {
            Stack.StepCompiledIn(&bNotifyLiveEdit);
        }
        Stack.IncrementCode();

        if (!IsLiveObject(Context) || !IsLiveObject(TargetToSpawn) ||
            !IsFiniteTransform(TargetTransform))
        {
            return;
        }

        AFortPlayerControllerAthena* PlayerController = nullptr;
        if (!HasCreativeEditAuthority(Context, PlayerController))
            return;

        auto Source = TargetToSpawn->Cast<AActor>();
        if (!IsSelectableActor(Source) || !IsActorInAuthorizedVolume(Source, PlayerController) ||
            !IsTransformInAuthorizedVolume(TargetTransform, PlayerController))
        {
            return;
        }

        const bool bStrictStructuralFinal = !bForPreviewing && IsStructuralBuildingActor(Source);
        const bool EffectiveAllowOverlap = bStrictStructuralFinal ? false : bAllowOverlap;
        const bool EffectiveIgnoreStructuralIssues = bStrictStructuralFinal ? false :
                bIgnoreStructuralIssues;
        if (bStrictStructuralFinal)
        {
            if (!QueryStructuralPlacement(Source, TargetTransform))
            {
                return;
            }
        }
        const auto BeforePlaced = GetPlacedArrayCounts(Context);
        InvokeOriginalSpawn(Context, Function, (void*)PhoneTargetSpawnActorWithTransformOG,
            TargetToSpawn, TargetTransform, EffectiveAllowOverlap,
            bAllowGravity, EffectiveIgnoreStructuralIssues, bForPreviewing, bNotifyLiveEdit,
            (void*)ModernSpawnActorWithTransform);
        if (ReconcilePlacedPairTails(Context, BeforePlaced, { Source }).Contains(Source))
            return;

        if (bForPreviewing && IsStructuralBuildingActor(Source))
            return;

        if (HasNewlyPlacedOriginal(Context, Source))
            return;

        auto Spawned = DuplicateBuildingActor(Source, TargetTransform, PlayerController,
            !bForPreviewing);
        if (!Spawned)
            return;

        if (AddNewlyPlacedPair(Context, Source, Spawned, bForPreviewing) !=
            EAddNewlyPlacedPairResult::Added)
        {
            Spawned->K2_DestroyActor();
        }
    }

    void HookSpawnSelected(UObject* DefaultOwner, bool bModern)
    {
        if (!IsLiveObject(DefaultOwner))
            return;

        auto SpawnSelected = DefaultOwner->GetFunction("ServerSpawnSelectedActorsWithTransform");
        if (!SpawnSelected || SpawnSelected->GetOffset("bAllowOverlap") == InvalidOffset ||
            SpawnSelected->GetOffset("bForPreviewing") == InvalidOffset)
        {
            return;
        }

        if (bModern)
        {
            if (SpawnSelected == LegacySpawnSelectedFunction)
                return;
            ModernSpawnSelectedFunction = SpawnSelected;
            Utils::ExecHook(SpawnSelected, SpawnSelectedActorsFallback, ModernSpawnSelectedOG);
        }
        else
        {
            LegacySpawnSelectedFunction = SpawnSelected;
            Utils::ExecHook(SpawnSelected, SpawnSelectedActorsFallback, LegacySpawnSelectedOG);
        }
    }

    void HookModernPhoneTargetMode()
    {
        auto DefaultMode = const_cast<UObject*>(DefaultObjImpl("PhoneToolActorTargetMode"));
        if (!DefaultMode || !DefaultMode->GetProperty("PlacementModeActors"))
        {
            return;
        }

        bool bHookedSelection = false;
        auto Start = DefaultMode->GetFunction("ServerStartInteracting");
        if (GetSchema().IsStartUsable() && Start && Start->GetOffset("Targets") != InvalidOffset &&
            Start->GetOffset("DragStart") != InvalidOffset)
        {
            Utils::ExecHook(Start, PhoneTargetStartInteracting, PhoneTargetStartInteractingOG);
            bHookedSelection = true;
        }

        auto Duplicate = DefaultMode->GetFunction("ServerDuplicateStartInteracting");
        if (GetSchema().IsStartUsable() && Duplicate &&
            Duplicate->GetOffset("Targets") != InvalidOffset &&
            Duplicate->GetOffset("DragStart") != InvalidOffset)
        {
            Utils::ExecHook(Duplicate, PhoneTargetDuplicateStartInteracting,
                PhoneTargetDuplicateStartInteractingOG);
            bHookedSelection = true;
        }

        if (GetSchema().IsStartUsable())
        {
            HookSpawnSelected(DefaultMode, true);
            HookMoveSelection(DefaultMode, true);
            HookPlaceSelection(DefaultMode, true);
            HookClearMovement(DefaultMode, true);
        }

        auto Spawn = DefaultMode->GetFunction("ServerSpawnActorWithTransform");
        if (Spawn && GetSchema().IsSpawnPairUsable() &&
            (Spawn->GetOffset("TargetToSpawn") != InvalidOffset ||
             Spawn->GetOffset("ActorToSpawn") != InvalidOffset) &&
            Spawn->GetOffset("TargetTransform") != InvalidOffset)
        {
            Utils::ExecHook(Spawn, ModernSpawnActorWithTransform,
                PhoneTargetSpawnActorWithTransformOG);
        }

        if (bHookedSelection)
        {
            SDK::DbgLog(
                "[CreativePhone] modern target-mode selection and duplicate fallbacks ready\n");
        }
    }
}

void AFortCreativeMoveTool::ServerStartInteracting_(UObject* Context, FFrame& Stack)
{
    TArray<AActor*> Actors;
    FTransform DragStart{};
    Stack.StepCompiledIn(&Actors);
    Stack.StepCompiledIn(&DragStart);
    Stack.IncrementCode();

    auto MoveTool = (AFortCreativeMoveTool*)Context;
    if (!IsLiveObject(MoveTool) || Actors.Num() <= 0 || Actors.Num() > MaxCreativeSelection ||
        !SDK::MemReadable(Actors.Data, static_cast<size_t>(Actors.Num()) * sizeof(AActor*)) ||
        !IsFiniteTransform(DragStart))
    {
        return;
    }

    AFortPlayerControllerAthena* PlayerController = nullptr;
    if (!HasCreativeEditAuthority(MoveTool, PlayerController))
    {
        SDK::DbgLog(
            "[CreativePhone] rejected ServerStartInteracting: no creative edit authority\n");
        return;
    }

    const auto ExpectedActors = UniqueActors(Actors);
    SDK::DbgLog("[CreativePhone] start rpc actors=%d unique=%d\n",
        Actors.Num(), static_cast<int32>(ExpectedActors.size()));
    if (!ValidateRequestedActors(ExpectedActors, PlayerController, Actors.Num()))
        return;

    auto Function = Stack.GetCurrentNativeFunction();
    if (!UsesLegacyCreativePhoneLifecycle() && TryNativeStartSingleFreeProp(
            MoveTool, Function, ServerStartInteracting_OG,
            ServerStartInteracting_, Actors, DragStart))
    {
        return;
    }

    if (!ExpectedActors.empty())
    {
        if (!StartInteractionFallback(MoveTool, Actors, DragStart, PlayerController))
        {
            SDK::DbgLog("[CreativePhone] legacy multi-selection fallback unavailable\n");
        }
        return;
    }

    if (auto SelectedActors = GetRawArray(MoveTool, "SelectedActors"))
    {
        SelectedActors->ResetNum();
    }
    InvokeOriginalStart(MoveTool, Function, (void*)ServerStartInteracting_OG,
        Actors, DragStart, (void*)ServerStartInteracting_);

    if (SelectionContainsExactly(MoveTool, ExpectedActors))
        return;

    if (!StartInteractionFallback(MoveTool, Actors, DragStart, PlayerController))
    {
        SDK::DbgLog(
            "[CreativePhone] ServerStartInteracting fallback unavailable for this reflected layout\n");
    }
}

void AFortCreativeMoveTool::ServerDuplicateStartInteracting_(UObject* Context, FFrame& Stack)
{
    TArray<AActor*> Actors;
    FTransform DragStart{};
    Stack.StepCompiledIn(&Actors);
    Stack.StepCompiledIn(&DragStart);
    Stack.IncrementCode();

    auto MoveTool = (AFortCreativeMoveTool*)Context;
    if (!IsLiveObject(MoveTool) || Actors.Num() <= 0 || Actors.Num() > MaxCreativeSelection ||
        !SDK::MemReadable(Actors.Data, static_cast<size_t>(Actors.Num()) * sizeof(AActor*)) ||
        !IsFiniteTransform(DragStart))
    {
        return;
    }

    AFortPlayerControllerAthena* PlayerController = nullptr;
    if (!HasCreativeEditAuthority(MoveTool, PlayerController))
        return;

    const auto ExpectedActors = UniqueActors(Actors);
    SDK::DbgLog("[CreativePhone] duplicate rpc actors=%d unique=%d\n",
        Actors.Num(), static_cast<int32>(ExpectedActors.size()));
    if (!ValidateRequestedActors(ExpectedActors, PlayerController, Actors.Num()))
        return;
    if (!ExpectedActors.empty())
    {
        if (!DuplicateInteractionFallback(MoveTool, Actors, DragStart, PlayerController))
        {
            SDK::DbgLog("[CreativePhone] legacy multi-duplicate fallback failed\n");
        }
        return;
    }

    if (auto SelectedActors = GetRawArray(MoveTool, "SelectedActors"))
    {
        SelectedActors->ResetNum();
    }
    auto Function = Stack.GetCurrentNativeFunction();
    InvokeOriginalStart(MoveTool, Function, (void*)ServerDuplicateStartInteracting_OG,
        Actors, DragStart, (void*)ServerDuplicateStartInteracting_);
    if (GetRawArrayCount(MoveTool, "SelectedActors") == static_cast<int32>(ExpectedActors.size()))
        return;
    DiscardPartialDuplicateSelection(MoveTool, ExpectedActors);

    if (!DuplicateInteractionFallback(MoveTool, Actors, DragStart, PlayerController))
    {
        SDK::DbgLog(
            "[CreativePhone] ServerDuplicateStartInteracting fallback could not duplicate the selection\n");
    }
}

void AFortCreativeMoveTool::ServerSpawnActorWithTransform_(UObject* Context, FFrame& Stack)
{
    UObject* TargetToSpawn = nullptr;
    FTransform TargetTransform{};
    bool bAllowOverlap = false;
    bool bAllowGravity = false;
    bool bIgnoreStructuralIssues = false;
    bool bForPreviewing = false;
    bool bNotifyLiveEdit = false;
    auto Function = Stack.GetCurrentNativeFunction();
    Stack.StepCompiledIn(&TargetToSpawn);
    Stack.StepCompiledIn(&TargetTransform);
    if (Function && Function->GetOffset("bAllowOverlap") != InvalidOffset)
    {
        Stack.StepCompiledIn(&bAllowOverlap);
    }
    if (Function && Function->GetOffset("bAllowGravity") != InvalidOffset)
    {
        Stack.StepCompiledIn(&bAllowGravity);
    }
    if (Function && Function->GetOffset("bIgnoreStructuralIssues") != InvalidOffset)
    {
        Stack.StepCompiledIn(&bIgnoreStructuralIssues);
    }
    if (Function && Function->GetOffset("bForPreviewing") != InvalidOffset)
    {
        Stack.StepCompiledIn(&bForPreviewing);
    }
    if (Function && Function->GetOffset("bNotifyLiveEdit") != InvalidOffset)
    {
        Stack.StepCompiledIn(&bNotifyLiveEdit);
    }
    Stack.IncrementCode();

    auto MoveTool = (AFortCreativeMoveTool*)Context;
    AFortPlayerControllerAthena* PlayerController = nullptr;
    if (!IsLiveObject(MoveTool) || !IsLiveObject(TargetToSpawn) ||
        !IsFiniteTransform(TargetTransform) ||
        !HasCreativeEditAuthority(MoveTool, PlayerController))
    {
        return;
    }

    auto ActorToSpawn = TargetToSpawn->Cast<AActor>();
    if (!IsSelectableActor(ActorToSpawn) || !IsActorInAuthorizedVolume(
            ActorToSpawn, PlayerController) || !IsTransformInAuthorizedVolume(
            TargetTransform, PlayerController))
    {
        return;
    }

    const bool bStrictStructuralFinal = !bForPreviewing && IsStructuralBuildingActor(ActorToSpawn);
    const bool EffectiveAllowOverlap = bStrictStructuralFinal ? false : bAllowOverlap;
    const bool EffectiveIgnoreStructuralIssues = bStrictStructuralFinal ? false :
            bIgnoreStructuralIssues;
    const bool bLegacyActorArray = IsLegacyActorArrayPlacement(MoveTool, Function);
    if (bStrictStructuralFinal)
    {
        if (!QueryStructuralPlacement(ActorToSpawn, TargetTransform))
        {
            return;
        }
    }
    const auto BeforePlaced = GetPlacedArrayCounts(MoveTool);
    // Early Creative stores only spawned actor pointers, so a partial native result cannot be reconciled to its source.
    if (!bLegacyActorArray)
    {
        InvokeOriginalSpawn(MoveTool, Function, (void*)ServerSpawnActorWithTransform_OG,
            TargetToSpawn, TargetTransform, EffectiveAllowOverlap,
            bAllowGravity, EffectiveIgnoreStructuralIssues, bForPreviewing, bNotifyLiveEdit,
            (void*)ServerSpawnActorWithTransform_);
    }
    if (!bLegacyActorArray && ReconcilePlacedPairTails(MoveTool, BeforePlaced,
            { ActorToSpawn }).Contains(ActorToSpawn))
    {
        return;
    }

    if (bForPreviewing && IsStructuralBuildingActor(ActorToSpawn))
        return;

    if (HasNewlyPlacedOriginal(MoveTool, ActorToSpawn))
    {
        return;
    }

    auto Spawned = DuplicateBuildingActor(ActorToSpawn, TargetTransform, PlayerController,
        !bForPreviewing);
    if (!Spawned)
        return;

    const auto PairResult = bLegacyActorArray ? AddLegacyNewlyPlacedActor(MoveTool, Spawned)
        : AddNewlyPlacedPair(MoveTool, ActorToSpawn, Spawned, bForPreviewing);
    if (PairResult != EAddNewlyPlacedPairResult::Added)
    {
        Spawned->K2_DestroyActor();
    }
}

void AFortCreativeMoveTool::Hook()
{
    auto DefaultMoveTool = (AFortCreativeMoveTool*)
        DefaultObjImpl("FortCreativeMoveTool");
    if (!DefaultMoveTool)
    {
        HookModernPhoneTargetMode();
        return;
    }

    const bool bLegacyNativeLifecycle = UsesLegacyCreativePhoneLifecycle();
    if (!bLegacyNativeLifecycle)
    {
        HookSpawnSelected(DefaultMoveTool, false);
        HookClearObjectInteractionModes(DefaultMoveTool);
        if (GetSchema().IsStartUsable())
        {
            HookMoveSelection(DefaultMoveTool, false);
            HookPlaceSelection(DefaultMoveTool, false);
            HookClearMovement(DefaultMoveTool, false);
        }
    }
    else
    {
        SDK::DbgLog(
            "[CreativePhone] <=10.40 native ServerMoveSelectionSet/ServerPlaceActorsAndClearMovementMode/ServerClearMovementMode lifecycle preserved\n");
    }

    auto Start = DefaultMoveTool->GetFunction("ServerStartInteracting");
    if (GetSchema().IsStartUsable() && Start && (Start->GetOffset("Actors") != InvalidOffset ||
         Start->GetOffset("Targets") != InvalidOffset) &&
        Start->GetOffset("DragStart") != InvalidOffset)
    {
        Utils::ExecHook(Start, ServerStartInteracting_, ServerStartInteracting_OG);
    }

    auto Duplicate = DefaultMoveTool->GetFunction("ServerDuplicateStartInteracting");
    if (GetSchema().IsStartUsable() && Duplicate &&
        (Duplicate->GetOffset("Actors") != InvalidOffset ||
         Duplicate->GetOffset("Targets") != InvalidOffset) &&
        Duplicate->GetOffset("DragStart") != InvalidOffset)
    {
        Utils::ExecHook(Duplicate, ServerDuplicateStartInteracting_,
            ServerDuplicateStartInteracting_OG);
    }

    auto Spawn = DefaultMoveTool->GetFunction("ServerSpawnActorWithTransform");
    if (Spawn && (GetSchema().IsSpawnPairUsable() ||
         IsLegacyActorArrayPlacement(DefaultMoveTool, Spawn)) &&
        Spawn->GetOffset("ActorToSpawn") != InvalidOffset &&
        Spawn->GetOffset("TargetTransform") != InvalidOffset)
    {
        Utils::ExecHook(Spawn, ServerSpawnActorWithTransform_, ServerSpawnActorWithTransform_OG);
    }

    SDK::DbgLog(
        "[CreativePhone] reflected move-tool compatibility hooks ready (selected=%d pair=%d legacy_native_lifecycle=%d)\n",
        GetSchema().SelectedActorSize, GetSchema().SpawnPairSize, bLegacyNativeLifecycle ? 1 : 0);

    if (!bLegacyNativeLifecycle)
        HookModernPhoneTargetMode();
}

void APhoneToolActorTargetMode::Hook()
{
}
