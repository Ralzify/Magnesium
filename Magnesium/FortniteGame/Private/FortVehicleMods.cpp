#include "pch.h"
#include "../Public/FortVehicleMods.h"
#include "../Public/FortPhysicsPawn.h"
#include "../Public/GameplayTagContainer.h"

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
    constexpr uint64 CPF_ConstParm = 0x0000000000000002;
    constexpr uint64 CPF_Parm = 0x0000000000000080;
    constexpr uint64 CPF_OutParm = 0x0000000000000100;
    constexpr uint64 CPF_ReturnParm = 0x0000000000000400;
    constexpr uint64 CPF_ReferenceParm = 0x0000000008000000;

    using FQueueCollectModDataForVehicle =
        void(*)(UObject*, const IInterface*);
    using FResolveVehicleModConfig =
        UObject*(*)(const IInterface*, const FGameplayTag*);
    using FApplyVehicleModConfig =
        void(*)(UObject*, const IInterface*);
    using FRemoveVehicleModConfig =
        void(*)(UObject*, const IInterface*);
    using FGetVehicleModComponent =
        UObject*(*)(const IInterface*, const FGameplayTag*);
    using FApplyTireModImplementation =
        void(*)(const IInterface*, const FGameplayTag*, UObject*);

    struct FExpectedParam
    {
        const char* Name;
        uint32 Offset;
        uint32 Size;
        uint64 RequiredFlags;
        uint64 ForbiddenFlags;
    };

    struct FVehicleModSchema
    {
        const UClass* VehicleClass = nullptr;
        const UClass* VehicleInterfaceClass = nullptr;
        const UClass* VehicleSpawnerClass = nullptr;
        const UClass* VehicleModSubsystemClass = nullptr;
        const UClass* SubsystemBlueprintLibraryClass = nullptr;
        UObject* SubsystemBlueprintLibrary = nullptr;
        FQueueCollectModDataForVehicle
            QueueCollectModDataForVehicle = nullptr;
        FResolveVehicleModConfig
            ResolveVehicleModConfig = nullptr;
        FApplyVehicleModConfig
            ApplyVehicleModConfig = nullptr;
        FRemoveVehicleModConfig
            RemoveVehicleModConfig = nullptr;
        UFunction* GetGameInstanceSubsystem = nullptr;
        UFunction* SpawnVehicleWithConstruction = nullptr;
        UFunction* TryConstructWithModServer = nullptr;
        UFunction* CanApplyMod = nullptr;
        UFunction* ApplyVehicleMod = nullptr;
        UFunction* ApplyTireMod = nullptr;
        uint32 CollectedVehicleModDataOffset = 0;
        uint32 AppliedVehicleModTagsOffset = 0;
        bool CoreValid = false;
        bool Complete = false;
    };

    struct FVehicleModConstructionState
    {
        TWeakObjectPtr<AFortAthenaVehicle> Vehicle;
        TWeakObjectPtr<UObject> SpawnSource;
        const IInterface* VehicleInterface = nullptr;
        std::vector<uint8> ForcedGameplayTags;
        std::vector<uint8> ForcedParentTags;
        int32 ForcedGameplayTagCount = 0;
        int32 ForcedParentTagCount = 0;
        uint8 Attempts = 0;
        bool HasForcedMods = false;
        bool Requested = false;
        bool InProgress = false;
        bool Dispatched = false;
        bool Ready = false;
        bool Failed = false;
        TWeakObjectPtr<UFortVehicleFuelComponent> FuelComponent;
        FVector LastFuelSampleLocation{};
        float LastFuelSample = 0.f;
        ULONGLONG LastFuelSampleTimeMs = 0;
        uint8 UnchangedMovingFuelSamples = 0;
        bool FuelInitializationAttempted = false;
        bool FuelSampleInitialized = false;
        bool NativeFuelConsumptionObserved = false;
        bool FuelFallbackLogged = false;
        ULONGLONG NextDamageablePartsAttemptTimeMs = 0;
        uint8 DamageablePartsAttempts = 0;
        bool DamageablePartsReady = false;
    };

    struct FVehiclePushModelApi
    {
        UObject* Helpers = nullptr;
        UFunction* MarkPropertyDirty = nullptr;
        ULONGLONG NextResolveTimeMs = 0;
        uint32 ResolveLogCount = 0;
        uint32 MarkFailureLogCount = 0;
    };

    using FVoidExec = void(*)(UObject*, FFrame&);
    using FBoolExec = void(*)(UObject*, FFrame&, bool*);

    FVehicleModSchema GVehicleModSchema{};
    FVehiclePushModelApi GVehiclePushModelApi{};
    std::vector<FVehicleModConstructionState>
        GVehicleModConstructionStates;
    TWeakObjectPtr<UWorld> GConstructionWorld;
    FBoolExec GCanApplyModOriginal = nullptr;
    FVoidExec GApplyVehicleModOriginal = nullptr;
    FVoidExec GApplyTireModOriginal = nullptr;
    bool GCanApplyModHookInstalled = false;
    bool GApplyVehicleModHookInstalled = false;
    bool GApplyTireModHookInstalled = false;
    bool GHookInstallInProgress = false;
    bool GSchemaResolveAttempted = false;
    ULONGLONG GNextSchemaResolveTimeMs = 0;
    ULONGLONG GNextHookInstallAttemptTimeMs = 0;
    uint8 GLastHookInstallStatus = 0xFF;
    uint32 GConstructionPendingLogCount = 0;
    uint32 GConstructionSkipLogCount = 0;
    uint32 GUnresolvedHookLogCount = 0;
    uint32 GCanApplyResultLogCount = 0;
    uint32 GApplyResultLogCount = 0;
    uint32 GNativeSpawnLogCount = 0;
    uint32 GTireLifecycleLogCount = 0;
    uint32 GSeatLifecycleLogCount = 0;
    uint32 GFuelLifecycleLogCount = 0;
    uint32 GFuelFallbackLogCount = 0;
    uint32 GDamageablePartsLogCount = 0;
    ULONGLONG GNextQueuedConstructionTimeMs = 0;
    ULONGLONG GNextFuelMonitorTimeMs = 0;
    bool GConstructionCircuitOpen = false;
    bool GHasPendingConstructionRequest = false;
    bool GNativeVehicleModFunctionsResolved = false;
    FQueueCollectModDataForVehicle
        GResolvedQueueCollectModDataForVehicle = nullptr;
    FResolveVehicleModConfig
        GResolvedResolveVehicleModConfig = nullptr;
    FApplyVehicleModConfig
        GResolvedApplyVehicleModConfig = nullptr;
    FRemoveVehicleModConfig
        GResolvedRemoveVehicleModConfig = nullptr;
    bool GNativeConfigApplyInProgress = false;

    constexpr uint8 MaxConstructionAttempts = 1;
    constexpr uint32 MaxFrameSnapshotSize = 0x100;
    constexpr uint32 VehicleModConfigApplyVirtualOffset = 0x2B8;
    constexpr uint32 VehicleModConfigRemoveVirtualOffset = 0x2C0;
    constexpr uint32 VehicleGetModByTagVirtualOffset = 0x1C0;
    constexpr uint32 VehicleGetModBySlotVirtualOffset = 0x1C8;
    constexpr uint32 VehicleApplyTireModVirtualOffset = 0x1E8;
    constexpr uint32 VehicleModComponentConfigOffset = 0xA8;
    constexpr uint32 VehicleModComponentEnabledOffset = 0x1C8;
    constexpr uint32 VehicleModConfigTagOffset = 0x28;
    constexpr uint32 VehicleModConfigSlotOffset = 0x2C;
    constexpr uint32 RF_BeginDestroyed = 0x00008000;
    constexpr uint32 RF_FinishDestroyed = 0x00010000;
    constexpr uint32 RF_Garbage = 0x40000000;
    constexpr std::array<uint8, 17> DagwoodApplyTireModThunk =
    {
        0x48, 0x81, 0xC1, 0xF0, 0xFA, 0xFF, 0xFF,
        0x48, 0x8B, 0x01,
        0x48, 0xFF, 0xA0, 0xA0, 0x08, 0x00, 0x00
    };

    bool IsSeason30()
    {
        return VersionInfo.FortniteVersion >= 30.00 &&
            VersionInfo.FortniteVersion < 31.00;
    }

    void SyncConstructionWorld()
    {
        auto* World = UWorld::GetWorld();
        if (!World || GConstructionWorld.Get() == World)
            return;

        GVehicleModConstructionStates.clear();
        GConstructionWorld = TWeakObjectPtr<UWorld>(World);
        GNextQueuedConstructionTimeMs = 0;
        GNextFuelMonitorTimeMs = 0;
        GConstructionCircuitOpen = false;
        GHasPendingConstructionRequest = false;
        GConstructionPendingLogCount = 0;
        GConstructionSkipLogCount = 0;
        GUnresolvedHookLogCount = 0;
        GCanApplyResultLogCount = 0;
        GApplyResultLogCount = 0;
        GNativeSpawnLogCount = 0;
        GTireLifecycleLogCount = 0;
        GSeatLifecycleLogCount = 0;
        GFuelLifecycleLogCount = 0;
        GFuelFallbackLogCount = 0;
        GDamageablePartsLogCount = 0;
    }

    template <size_t ParamCount>
    bool ValidateFunction(
        UFunction* Function,
        uint32 ExpectedSize,
        const FExpectedParam(&Expected)[ParamCount])
    {
        if (!Function ||
            !Function->ExecFunction ||
            !SDK::MemReadable(Function->ExecFunction, 1))
        {
            return false;
        }

        const auto Params = Function->GetParamsNamed();
        if (Params.Size != ExpectedSize ||
            Params.NameOffsetMap.size() != ParamCount)
        {
            return false;
        }

        for (const auto& ExpectedParam : Expected)
        {
            const UFunction::ParamNamed* Match = nullptr;
            for (const auto& Param : Params.NameOffsetMap)
            {
                if (Param.Name == ExpectedParam.Name)
                {
                    if (Match)
                        return false;

                    Match = &Param;
                }
            }

            if (!Match ||
                Match->Offset != ExpectedParam.Offset ||
                Match->ElementSize != ExpectedParam.Size ||
                (Match->PropertyFlags & ExpectedParam.RequiredFlags) !=
                    ExpectedParam.RequiredFlags ||
                (Match->PropertyFlags & ExpectedParam.ForbiddenFlags) != 0 ||
                Match->Offset > Params.Size ||
                Match->ElementSize > Params.Size - Match->Offset)
            {
                return false;
            }
        }

        return true;
    }

    bool ResolveVehiclePushModelApi()
    {
        constexpr FExpectedParam Params[] =
        {
            {
                "Object",
                0x0,
                uint32(sizeof(UObject*)),
                CPF_Parm,
                CPF_OutParm | CPF_ReturnParm | CPF_ReferenceParm
            },
            {
                "PropertyName",
                0x8,
                uint32(sizeof(int32)),
                CPF_Parm,
                CPF_OutParm | CPF_ReturnParm | CPF_ReferenceParm
            }
        };

        if (GVehiclePushModelApi.Helpers &&
            SDK::MemReadable(
                GVehiclePushModelApi.Helpers,
                sizeof(UObject)) &&
            ValidateFunction(
                GVehiclePushModelApi.MarkPropertyDirty,
                0x10,
                Params))
        {
            return true;
        }

        const ULONGLONG CurrentTimeMs = GetTickCount64();
        if (CurrentTimeMs <
            GVehiclePushModelApi.NextResolveTimeMs)
        {
            return false;
        }
        GVehiclePushModelApi.NextResolveTimeMs =
            CurrentTimeMs + 5000ULL;

        auto* HelpersClass = FindObject<UClass>(
            L"/Script/Engine.NetPushModelHelpers");
        if (!HelpersClass)
            HelpersClass = SDK::FindClass("NetPushModelHelpers");

        auto* Helpers = HelpersClass
            ? HelpersClass->GetDefaultObj()
            : nullptr;
        UFunction* MarkPropertyDirty = Helpers
            ? Helpers->GetFunction("MarkPropertyDirty")
            : nullptr;
        if (!Helpers ||
            !SDK::MemReadable(Helpers, sizeof(UObject)) ||
            !ValidateFunction(
                MarkPropertyDirty,
                0x10,
                Params))
        {
            GVehiclePushModelApi.Helpers = nullptr;
            GVehiclePushModelApi.MarkPropertyDirty = nullptr;
            if (GVehiclePushModelApi.ResolveLogCount++ < 4)
            {
                SDK::DbgLog(
                    "[VehicleMods] "
                    "NetPushModelHelpers.MarkPropertyDirty "
                    "unavailable or has an unexpected schema\n");
            }
            return false;
        }

        GVehiclePushModelApi.Helpers = Helpers;
        GVehiclePushModelApi.MarkPropertyDirty =
            MarkPropertyDirty;
        GVehiclePushModelApi.NextResolveTimeMs = 0;
        return true;
    }

    bool MarkReplicatedPropertyDirty(
        UObject* Object,
        const wchar_t* PropertyNameText)
    {
        if (!Object ||
            !PropertyNameText ||
            !ResolveVehiclePushModelApi())
        {
            return false;
        }

        bool Succeeded = false;
        __try
        {
            FName PropertyName(PropertyNameText);
            GVehiclePushModelApi.Helpers->Call<void>(
                GVehiclePushModelApi.MarkPropertyDirty,
                Object,
                PropertyName);
            Succeeded = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            GVehiclePushModelApi.Helpers = nullptr;
            GVehiclePushModelApi.MarkPropertyDirty = nullptr;
            GVehiclePushModelApi.NextResolveTimeMs =
                GetTickCount64() + 5000ULL;
        }

        if (!Succeeded &&
            GVehiclePushModelApi.MarkFailureLogCount++ < 4)
        {
            SDK::DbgLog(
                "[VehicleMods] Failed to mark replicated "
                "property dirty object=%p property=%ls\n",
                Object,
                PropertyNameText);
        }
        return Succeeded;
    }

    bool MarkAppliedVehicleModTagsDirty(
        AFortAthenaVehicle* Vehicle)
    {
        return MarkReplicatedPropertyDirty(
            static_cast<UObject*>(Vehicle),
            L"AppliedVehicleModTags");
    }

    bool ValidateTryConstructFunction(UFunction* Function)
    {
        constexpr FExpectedParam Params[] =
        {
            {
                "ForceMods",
                0x0,
                0x20,
                CPF_ConstParm | CPF_Parm | CPF_OutParm |
                    CPF_ReferenceParm,
                CPF_ReturnParm
            }
        };
        return ValidateFunction(Function, 0x20, Params);
    }

    bool ValidateSpawnVehicleWithConstructionFunction(
        UFunction* Function)
    {
        constexpr FExpectedParam Params[] =
        {
            {
                "Class",
                0x0,
                0x8,
                CPF_Parm,
                CPF_OutParm | CPF_ReturnParm | CPF_ReferenceParm
            },
            {
                "Transform",
                0x10,
                0x60,
                CPF_ConstParm | CPF_Parm | CPF_OutParm |
                    CPF_ReferenceParm,
                CPF_ReturnParm
            },
            {
                "ReturnValue",
                0x70,
                0x8,
                CPF_Parm | CPF_OutParm | CPF_ReturnParm,
                CPF_ReferenceParm
            }
        };
        return ValidateFunction(Function, 0x80, Params);
    }

    bool ValidateGetGameInstanceSubsystemFunction(
        UFunction* Function)
    {
        constexpr FExpectedParam Params[] =
        {
            {
                "ContextObject",
                0x0,
                0x8,
                CPF_Parm,
                CPF_OutParm | CPF_ReturnParm | CPF_ReferenceParm
            },
            {
                "Class",
                0x8,
                0x8,
                CPF_Parm,
                CPF_OutParm | CPF_ReturnParm | CPF_ReferenceParm
            },
            {
                "ReturnValue",
                0x10,
                0x8,
                CPF_Parm | CPF_OutParm | CPF_ReturnParm,
                CPF_ReferenceParm
            }
        };
        return ValidateFunction(Function, 0x18, Params);
    }

    void ResolveNativeVehicleModFunctions()
    {
        if (GNativeVehicleModFunctionsResolved)
            return;

        GNativeVehicleModFunctionsResolved = true;
        const auto QueueCollectionAddress =
            Memcury::Scanner::FindPattern(
                "48 85 D2 0F 84 ? ? ? ? 48 89 5C 24 18 55 56 "
                "57 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC "
                "E0 01 00 00 48 8B 05 ? ? ? ? 48 33 C4 48 89 "
                "85 D0 00 00 00 48 8B 02 4C 8B F1 48 8B CA 33 "
                "FF FF 90 E8 05 00 00").Get();

        if (QueueCollectionAddress &&
            SDK::MemReadable(
                reinterpret_cast<const void*>(
                    QueueCollectionAddress),
                0xAA))
        {
            const auto* QueueBytes =
                reinterpret_cast<const uint8*>(
                    QueueCollectionAddress);
            constexpr uint8 PendingSetBytes[] =
                { 0x49, 0x8D, 0x8E, 0x80, 0x00, 0x00, 0x00 };
            constexpr uint8 ScheduledCallbackBytes[] =
                { 0x49, 0x39, 0xBE, 0xD0, 0x00, 0x00, 0x00 };
            if (std::memcmp(
                    QueueBytes + 0x97,
                    PendingSetBytes,
                    sizeof(PendingSetBytes)) == 0 &&
                std::memcmp(
                    QueueBytes + 0xA3,
                    ScheduledCallbackBytes,
                    sizeof(ScheduledCallbackBytes)) == 0)
            {
                GResolvedQueueCollectModDataForVehicle =
                    reinterpret_cast<
                        FQueueCollectModDataForVehicle>(
                        QueueCollectionAddress);
            }
        }

        // FN30's public ApplyVehicleMod virtual is a stripped, one-byte
        // return stub in Shipping. This retained helper resolves the correct
        // UFortVehicleModConfig CDO by incoming mod tag and the vehicle's
        // classification tags.
        const auto ResolveConfigAddress =
            Memcury::Scanner::FindPattern(
                "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 "
                "48 89 78 20 41 56 48 83 EC 20 48 8B 01 4C 8B "
                "F2 48 8B D9 FF 90 F0 05 00 00 48 8B C8 E8 ? "
                "? ? ? 48 8B E8 48 85 C0 74 ? 4C 8B 03 48 8B "
                "CB 41 FF 50 48").Get();
        if (ResolveConfigAddress &&
            SDK::MemReadable(
                reinterpret_cast<const void*>(ResolveConfigAddress),
                0x89))
        {
            GResolvedResolveVehicleModConfig =
                reinterpret_cast<FResolveVehicleModConfig>(
                    ResolveConfigAddress);
        }

        // Anchor + 0x35 is the first config/vehicle-interface use inside
        // UFortVehicleModConfig::ApplyToVehicleInternal. Resolve the entry
        // from that stable body sequence and later require the config CDO's
        // own virtual slot to point back to this exact function.
        const auto ApplyConfigAnchor =
            Memcury::Scanner::FindPattern(
                "48 8B 02 4C 8B F1 48 89 4C 24 58 48 8B F2 "
                "48 8B CA 48 89 55 A0 FF 90 E8 05 00 00").Get();
        if (ApplyConfigAnchor > 0x35)
        {
            const auto ApplyConfigAddress =
                ApplyConfigAnchor - 0x35;
            if (SDK::MemReadable(
                    reinterpret_cast<const void*>(
                        ApplyConfigAddress),
                    0x50))
            {
                GResolvedApplyVehicleModConfig =
                    reinterpret_cast<FApplyVehicleModConfig>(
                        ApplyConfigAddress);
            }
        }

        const auto RemoveConfigAddress =
            Memcury::Scanner::FindPattern(
                "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 "
                "57 41 54 41 55 41 56 41 57 48 83 EC 20 48 8B "
                "02 4C 8B E9").Get();
        if (RemoveConfigAddress &&
            SDK::MemReadable(
                reinterpret_cast<const void*>(RemoveConfigAddress),
                0x40))
        {
            GResolvedRemoveVehicleModConfig =
                reinterpret_cast<FRemoveVehicleModConfig>(
                    RemoveConfigAddress);
        }
    }

    bool ReadPropertyMetadata(
        const UField* Property,
        uint32 OwnerSize,
        uint32 ExpectedElementSize,
        uint32& OutOffset)
    {
        if (!Property ||
            !Offsets::Offset_Internal ||
            !Offsets::ElementSize)
        {
            return false;
        }

        const uint32 MetadataEnd = (std::max)(
            Offsets::Offset_Internal + (uint32)sizeof(uint32),
            Offsets::ElementSize + (uint32)sizeof(uint32));
        if (!SDK::MemReadable(Property, MetadataEnd))
            return false;

        const uint32 ElementSize =
            GetFromOffset<uint32>(Property, Offsets::ElementSize);
        const uint32 Offset = SDK::ReadPropertyOffset(
            GetFromOffset<uint32>(Property, Offsets::Offset_Internal));
        if (ElementSize != ExpectedElementSize ||
            Offset > OwnerSize ||
            ElementSize > OwnerSize - Offset)
        {
            return false;
        }

        OutOffset = Offset;
        return true;
    }

    UFunction* FindFunction(const wchar_t* Path)
    {
        return const_cast<UFunction*>(FindObject<UFunction>(Path));
    }

    FVehicleModSchema& ResolveVehicleModSchema()
    {
        if (GVehicleModSchema.Complete)
            return GVehicleModSchema;

        if (!IsSeason30())
            return GVehicleModSchema;

        const ULONGLONG CurrentTimeMs = GetTickCount64();
        if (GSchemaResolveAttempted &&
            CurrentTimeMs < GNextSchemaResolveTimeMs)
        {
            return GVehicleModSchema;
        }
        GSchemaResolveAttempted = true;
        GNextSchemaResolveTimeMs =
            CurrentTimeMs + 5000ULL;

        // Do not use the SDK StaticClass helpers here. They cache a first
        // lookup miss forever, while this hook can be installed before all
        // Fortnite classes have entered the object array.
        FVehicleModSchema Candidate{};
        Candidate.VehicleClass =
            SDK::FindClass("FortAthenaVehicle");
        Candidate.VehicleInterfaceClass =
            SDK::FindClass("FortVehicleInterface");
        Candidate.VehicleSpawnerClass =
            SDK::FindClass("FortAthenaVehicleSpawner");
        Candidate.VehicleModSubsystemClass =
            SDK::FindClass("FortVehicleModSubsystem");
        Candidate.SubsystemBlueprintLibraryClass =
            SDK::FindClass("SubsystemBlueprintLibrary");
        Candidate.SubsystemBlueprintLibrary =
            Candidate.SubsystemBlueprintLibraryClass
                ? Candidate.SubsystemBlueprintLibraryClass
                    ->GetDefaultObj()
                : nullptr;
        ResolveNativeVehicleModFunctions();
        Candidate.QueueCollectModDataForVehicle =
            GResolvedQueueCollectModDataForVehicle;
        Candidate.ResolveVehicleModConfig =
            GResolvedResolveVehicleModConfig;
        Candidate.ApplyVehicleModConfig =
            GResolvedApplyVehicleModConfig;
        Candidate.RemoveVehicleModConfig =
            GResolvedRemoveVehicleModConfig;

        // This is also checked against the reflected parameter size below.
        static_assert(
            sizeof(FGameplayTagContainer) == 0x20,
            "FN30 vehicle mod construction expects a 0x20 tag container");
        static_assert(
            sizeof(FTransform) == 0x60,
            "FN30 vehicle spawning expects a 0x60 transform");

        constexpr FExpectedParam CanApplyParams[] =
        {
            {
                "InTag",
                0x0,
                0x4,
                CPF_ConstParm | CPF_Parm | CPF_OutParm |
                    CPF_ReferenceParm,
                CPF_ReturnParm
            },
            {
                "ReturnValue",
                0x4,
                0x1,
                CPF_Parm | CPF_OutParm | CPF_ReturnParm,
                CPF_ReferenceParm
            }
        };
        constexpr FExpectedParam ApplyVehicleParams[] =
        {
            {
                "ModTag",
                0x0,
                0x4,
                CPF_ConstParm | CPF_Parm | CPF_OutParm |
                    CPF_ReferenceParm,
                CPF_ReturnParm
            },
            {
                "EventInstigator",
                0x8,
                0x8,
                CPF_ConstParm | CPF_Parm,
                CPF_OutParm | CPF_ReturnParm | CPF_ReferenceParm
            }
        };
        constexpr FExpectedParam ApplyTireParams[] =
        {
            {
                "NewTireModTag",
                0x0,
                0x4,
                CPF_ConstParm | CPF_Parm | CPF_OutParm |
                    CPF_ReferenceParm,
                CPF_ReturnParm
            },
            {
                "EventInstigator",
                0x8,
                0x8,
                CPF_ConstParm | CPF_Parm,
                CPF_OutParm | CPF_ReturnParm | CPF_ReferenceParm
            }
        };

        auto* SpawnWithConstruction = FindFunction(
            L"/Script/FortniteGame.FortAthenaVehicleSpawner."
            L"SpawnVehicleWithConstruction");
        auto* GetGameInstanceSubsystem = FindFunction(
            L"/Script/Engine.SubsystemBlueprintLibrary."
            L"GetGameInstanceSubsystem");
        auto* TryConstruct = FindFunction(
            L"/Script/FortniteGame.FortVehicleInterface."
            L"TryConstructWithModServer");
        auto* CanApply = FindFunction(
            L"/Script/FortniteGame.FortVehicleInterface.CanApplyMod");
        auto* ApplyVehicle = FindFunction(
            L"/Script/FortniteGame.FortVehicleInterface.ApplyVehicleMod");
        auto* ApplyTire = FindFunction(
            L"/Script/FortniteGame.FortVehicleInterface.ApplyTireMod");

        if (ValidateSpawnVehicleWithConstructionFunction(
                SpawnWithConstruction))
        {
            Candidate.SpawnVehicleWithConstruction =
                SpawnWithConstruction;
        }
        if (ValidateGetGameInstanceSubsystemFunction(
                GetGameInstanceSubsystem))
        {
            Candidate.GetGameInstanceSubsystem =
                GetGameInstanceSubsystem;
        }
        if (ValidateTryConstructFunction(TryConstruct))
            Candidate.TryConstructWithModServer = TryConstruct;
        if (ValidateFunction(CanApply, 0x8, CanApplyParams))
            Candidate.CanApplyMod = CanApply;
        if (ValidateFunction(
                ApplyVehicle,
                0x10,
                ApplyVehicleParams))
        {
            Candidate.ApplyVehicleMod = ApplyVehicle;
        }
        if (ValidateFunction(ApplyTire, 0x10, ApplyTireParams))
            Candidate.ApplyTireMod = ApplyTire;

        uint32 CollectedDataOffset = 0;
        const UField* CollectedDataProperty = Candidate.VehicleClass
            ? Candidate.VehicleClass->GetProperty(
                "CollectedVehicleModData",
                0x10000)
            : nullptr;
        uint32 AppliedTagsOffset = 0;
        const UField* AppliedTagsProperty = Candidate.VehicleClass
            ? Candidate.VehicleClass->GetProperty(
                "AppliedVehicleModTags")
            : nullptr;
        const uint32 VehicleSize =
            Candidate.VehicleClass
                ? Candidate.VehicleClass->GetPropertiesSize()
                : 0;
        if (ReadPropertyMetadata(
                CollectedDataProperty,
                VehicleSize,
                (uint32)sizeof(UObject*),
                CollectedDataOffset))
        {
            Candidate.CollectedVehicleModDataOffset =
                CollectedDataOffset;
        }
        if (ReadPropertyMetadata(
                AppliedTagsProperty,
                VehicleSize,
                (uint32)sizeof(FGameplayTagContainer),
                AppliedTagsOffset))
        {
            Candidate.AppliedVehicleModTagsOffset =
                AppliedTagsOffset;
        }

        Candidate.CoreValid =
            Candidate.VehicleClass &&
            Candidate.VehicleInterfaceClass &&
            Candidate.VehicleModSubsystemClass &&
            Candidate.SubsystemBlueprintLibraryClass &&
            Candidate.SubsystemBlueprintLibrary &&
            SDK::MemReadable(
                Candidate.SubsystemBlueprintLibrary,
                sizeof(UObject)) &&
            Candidate.SubsystemBlueprintLibrary->Class &&
            Candidate.SubsystemBlueprintLibrary->IsDefaultObject() &&
            Candidate.SubsystemBlueprintLibrary->IsA(
                Candidate.SubsystemBlueprintLibraryClass) &&
            Offsets::GetInterfaceAddress &&
            Candidate.QueueCollectModDataForVehicle &&
            Candidate.GetGameInstanceSubsystem &&
            Candidate.TryConstructWithModServer &&
            Candidate.CollectedVehicleModDataOffset != 0;
        Candidate.Complete =
            Candidate.CoreValid &&
            Candidate.VehicleSpawnerClass &&
            Candidate.SpawnVehicleWithConstruction &&
            Candidate.CanApplyMod &&
            Candidate.ApplyVehicleMod &&
            Candidate.ApplyTireMod &&
            Candidate.ResolveVehicleModConfig &&
            Candidate.ApplyVehicleModConfig &&
            Candidate.RemoveVehicleModConfig &&
            Candidate.AppliedVehicleModTagsOffset != 0;

        const bool SchemaChanged =
            Candidate.VehicleClass !=
                GVehicleModSchema.VehicleClass ||
            Candidate.VehicleInterfaceClass !=
                GVehicleModSchema.VehicleInterfaceClass ||
            Candidate.VehicleSpawnerClass !=
                GVehicleModSchema.VehicleSpawnerClass ||
            Candidate.VehicleModSubsystemClass !=
                GVehicleModSchema.VehicleModSubsystemClass ||
            Candidate.SubsystemBlueprintLibraryClass !=
                GVehicleModSchema.SubsystemBlueprintLibraryClass ||
            Candidate.SubsystemBlueprintLibrary !=
                GVehicleModSchema.SubsystemBlueprintLibrary ||
            Candidate.QueueCollectModDataForVehicle !=
                GVehicleModSchema.QueueCollectModDataForVehicle ||
            Candidate.ResolveVehicleModConfig !=
                GVehicleModSchema.ResolveVehicleModConfig ||
            Candidate.ApplyVehicleModConfig !=
                GVehicleModSchema.ApplyVehicleModConfig ||
            Candidate.RemoveVehicleModConfig !=
                GVehicleModSchema.RemoveVehicleModConfig ||
            Candidate.GetGameInstanceSubsystem !=
                GVehicleModSchema.GetGameInstanceSubsystem ||
            Candidate.SpawnVehicleWithConstruction !=
                GVehicleModSchema.SpawnVehicleWithConstruction ||
            Candidate.TryConstructWithModServer !=
                GVehicleModSchema.TryConstructWithModServer ||
            Candidate.CanApplyMod !=
                GVehicleModSchema.CanApplyMod ||
            Candidate.ApplyVehicleMod !=
                GVehicleModSchema.ApplyVehicleMod ||
            Candidate.ApplyTireMod !=
                GVehicleModSchema.ApplyTireMod ||
            Candidate.CollectedVehicleModDataOffset !=
                GVehicleModSchema.CollectedVehicleModDataOffset ||
            Candidate.AppliedVehicleModTagsOffset !=
                GVehicleModSchema.AppliedVehicleModTagsOffset ||
            Candidate.CoreValid != GVehicleModSchema.CoreValid ||
            Candidate.Complete != GVehicleModSchema.Complete;

        GVehicleModSchema = Candidate;
        if (GVehicleModSchema.Complete)
            GNextSchemaResolveTimeMs = 0;

        if (SchemaChanged)
        {
            SDK::DbgLog(
                "[VehicleMods] FN30 schema core=%s construct=%s "
                "native-spawn=%s native-collection=%s "
                "native-config-apply=%s "
                "collected-data=%s "
                "applied-tags=%s interface=%s "
                "can-apply=%s apply=%s tire=%s\n",
                GVehicleModSchema.CoreValid ? "valid" : "invalid",
                GVehicleModSchema.TryConstructWithModServer
                    ? "valid"
                    : "invalid",
                GVehicleModSchema.SpawnVehicleWithConstruction &&
                        GVehicleModSchema.VehicleSpawnerClass
                    ? "valid"
                    : "invalid",
                GVehicleModSchema.VehicleModSubsystemClass &&
                        GVehicleModSchema.SubsystemBlueprintLibrary &&
                        GVehicleModSchema.GetGameInstanceSubsystem &&
                        GVehicleModSchema
                            .QueueCollectModDataForVehicle
                    ? "valid"
                    : "invalid",
                GVehicleModSchema.ResolveVehicleModConfig &&
                        GVehicleModSchema.ApplyVehicleModConfig &&
                        GVehicleModSchema.RemoveVehicleModConfig
                    ? "valid"
                    : "invalid",
                GVehicleModSchema.CollectedVehicleModDataOffset
                    ? "valid"
                    : "invalid",
                GVehicleModSchema.AppliedVehicleModTagsOffset
                    ? "valid"
                    : "invalid",
                Offsets::GetInterfaceAddress ? "valid" : "invalid",
                GVehicleModSchema.CanApplyMod ? "valid" : "invalid",
                GVehicleModSchema.ApplyVehicleMod
                    ? "valid"
                    : "invalid",
                GVehicleModSchema.ApplyTireMod
                    ? "valid"
                    : "invalid");
        }

        return GVehicleModSchema;
    }

    bool IsSaneTagArray(const TArray<FGameplayTag>& Tags)
    {
        if (Tags.NumElements < 0 ||
            Tags.MaxElements < Tags.NumElements ||
            Tags.MaxElements > 4096)
        {
            return false;
        }

        if (Tags.NumElements == 0)
            return true;

        const size_t ByteCount =
            (size_t)Tags.NumElements * (size_t)FGameplayTag::Size();
        return Tags.Data && SDK::MemReadable(Tags.Data, ByteCount);
    }

    const FGameplayTagContainer* GetForcedMods(
        const UObject* SpawnSource)
    {
        if (!SpawnSource ||
            !SDK::MemReadable(SpawnSource, sizeof(UObject)) ||
            !SpawnSource->Class ||
            !SDK::MemReadable(SpawnSource->Class, 0x40))
        {
            return nullptr;
        }

        const UField* ForceModsProperty =
            SpawnSource->GetProperty("ForceMods");
        const uint32 OwnerSize =
            SpawnSource->Class->GetPropertiesSize();
        uint32 ForceModsOffset = 0;
        if (!ReadPropertyMetadata(
                ForceModsProperty,
                OwnerSize,
                (uint32)sizeof(FGameplayTagContainer),
                ForceModsOffset))
        {
            return nullptr;
        }

        auto* ForceMods = reinterpret_cast<const FGameplayTagContainer*>(
            reinterpret_cast<const uint8*>(SpawnSource) +
            ForceModsOffset);
        if (!SDK::MemReadable(
                ForceMods,
                sizeof(FGameplayTagContainer)) ||
            !IsSaneTagArray(ForceMods->GameplayTags) ||
            !IsSaneTagArray(ForceMods->ParentTags))
        {
            return nullptr;
        }

        return ForceMods;
    }

    void CaptureForcedMods(
        FVehicleModConstructionState& State,
        const UObject* SpawnSource)
    {
        const auto* ForceMods = GetForcedMods(SpawnSource);
        if (!ForceMods)
            return;

        const int32 TagSize = FGameplayTag::Size();
        const auto CaptureArray =
            [TagSize](
                const TArray<FGameplayTag>& Source,
                std::vector<uint8>& Destination,
                int32& DestinationCount)
            {
                DestinationCount = Source.NumElements;
                const size_t ByteCount =
                    static_cast<size_t>(DestinationCount) *
                    static_cast<size_t>(TagSize);
                Destination.resize(ByteCount);
                if (ByteCount)
                {
                    std::memcpy(
                        Destination.data(),
                        Source.Data,
                        ByteCount);
                }
            };

        CaptureArray(
            ForceMods->GameplayTags,
            State.ForcedGameplayTags,
            State.ForcedGameplayTagCount);
        CaptureArray(
            ForceMods->ParentTags,
            State.ForcedParentTags,
            State.ForcedParentTagCount);
        State.HasForcedMods = true;
    }

    void WriteForcedModsParams(
        const FVehicleModConstructionState& State,
        std::array<uint8, 0x20>& Params)
    {
        if (!State.HasForcedMods)
            return;

        FGameplayTagContainer ForceMods{};
        ForceMods.GameplayTags.Data =
            reinterpret_cast<FGameplayTag*>(
                const_cast<uint8*>(State.ForcedGameplayTags.data()));
        ForceMods.GameplayTags.NumElements =
            State.ForcedGameplayTagCount;
        ForceMods.GameplayTags.MaxElements =
            State.ForcedGameplayTagCount;
        ForceMods.ParentTags.Data =
            reinterpret_cast<FGameplayTag*>(
                const_cast<uint8*>(State.ForcedParentTags.data()));
        ForceMods.ParentTags.NumElements =
            State.ForcedParentTagCount;
        ForceMods.ParentTags.MaxElements =
            State.ForcedParentTagCount;
        std::memcpy(
            Params.data(),
            &ForceMods,
            sizeof(ForceMods));
    }

    FVehicleModConstructionState* FindConstructionState(
        AFortAthenaVehicle* Vehicle,
        bool Create)
    {
        auto Iterator = GVehicleModConstructionStates.begin();
        while (Iterator != GVehicleModConstructionStates.end())
        {
            AFortAthenaVehicle* Existing = Iterator->Vehicle.Get();
            if (!Existing)
            {
                Iterator = GVehicleModConstructionStates.erase(Iterator);
                continue;
            }

            if (Existing == Vehicle)
                return &*Iterator;

            ++Iterator;
        }

        if (!Create)
            return nullptr;

        FVehicleModConstructionState State{};
        State.Vehicle = TWeakObjectPtr<AFortAthenaVehicle>(Vehicle);
        GVehicleModConstructionStates.emplace_back(State);
        return &GVehicleModConstructionStates.back();
    }

    bool ReadCollectedVehicleModData(
        const AFortAthenaVehicle* Vehicle,
        const FVehicleModSchema& Schema,
        UObject*& OutCollectedData)
    {
        OutCollectedData = nullptr;
        if (!Vehicle ||
            !Schema.CollectedVehicleModDataOffset)
        {
            return false;
        }

        auto* Address =
            reinterpret_cast<const uint8*>(Vehicle) +
            Schema.CollectedVehicleModDataOffset;
        if (!SDK::MemReadable(Address, sizeof(UObject*)))
            return false;

        std::memcpy(
            &OutCollectedData,
            Address,
            sizeof(OutCollectedData));
        return true;
    }

    const FGameplayTagContainer* ReadAppliedVehicleModTags(
        const AFortAthenaVehicle* Vehicle,
        const FVehicleModSchema& Schema)
    {
        if (!Vehicle || !Schema.AppliedVehicleModTagsOffset)
            return nullptr;

        auto* Tags = reinterpret_cast<const FGameplayTagContainer*>(
            reinterpret_cast<const uint8*>(Vehicle) +
            Schema.AppliedVehicleModTagsOffset);
        if (!SDK::MemReadable(
                Tags,
                sizeof(FGameplayTagContainer)) ||
            !IsSaneTagArray(Tags->GameplayTags) ||
            !IsSaneTagArray(Tags->ParentTags))
        {
            return nullptr;
        }

        return Tags;
    }

    int32 ReadAppliedVehicleModCount(
        const AFortAthenaVehicle* Vehicle,
        const FVehicleModSchema& Schema)
    {
        const auto* Tags =
            ReadAppliedVehicleModTags(Vehicle, Schema);
        return Tags ? Tags->GameplayTags.NumElements : -1;
    }

    bool HasAppliedVehicleModTag(
        const AFortAthenaVehicle* Vehicle,
        const FVehicleModSchema& Schema,
        const FGameplayTag& Tag)
    {
        const auto* Tags =
            ReadAppliedVehicleModTags(Vehicle, Schema);
        return Tags &&
            Tags->GameplayTags.Contains(
                Tag,
                FGameplayTag::Size());
    }

    bool ReadGameplayTagIndex(
        const FGameplayTag& Tag,
        int32& OutComparisonIndex)
    {
        OutComparisonIndex = 0;
        static_assert(
            sizeof(FGameplayTag) >= sizeof(OutComparisonIndex),
            "FGameplayTag must contain its four-byte FN30 name index");
        std::memcpy(
            &OutComparisonIndex,
            &Tag,
            sizeof(OutComparisonIndex));
        return OutComparisonIndex != 0;
    }

    bool AddAppliedVehicleModTag(
        AFortAthenaVehicle* Vehicle,
        const FVehicleModSchema& Schema,
        const FGameplayTag& Tag)
    {
        auto* Tags = const_cast<FGameplayTagContainer*>(
            ReadAppliedVehicleModTags(Vehicle, Schema));
        if (!Tags)
            return false;

        bool Added = false;
        if (!Tags->GameplayTags.Contains(
                Tag,
                FGameplayTag::Size()))
        {
            Tags->GameplayTags.Add(
                Tag,
                FGameplayTag::Size());
            Added = true;
        }
        const bool Present = Tags->GameplayTags.Contains(
            Tag,
            FGameplayTag::Size());
        if (Added && Present)
            MarkAppliedVehicleModTagsDirty(Vehicle);
        return Present;
    }

    bool RemoveAppliedVehicleModTag(
        AFortAthenaVehicle* Vehicle,
        const FVehicleModSchema& Schema,
        const FGameplayTag& Tag)
    {
        auto* Tags = const_cast<FGameplayTagContainer*>(
            ReadAppliedVehicleModTags(Vehicle, Schema));
        if (!Tags)
            return false;

        bool Removed = false;
        for (int32 Index =
                Tags->GameplayTags.NumElements - 1;
            Index >= 0;
            --Index)
        {
            const auto& Existing =
                Tags->GameplayTags.Get(
                    Index,
                    FGameplayTag::Size());
            if (std::memcmp(
                    &Existing,
                    &Tag,
                    FGameplayTag::Size()) == 0)
            {
                Tags->GameplayTags.Remove(
                    Index,
                    FGameplayTag::Size());
                Removed = true;
            }
        }
        if (Removed)
            MarkAppliedVehicleModTagsDirty(Vehicle);
        return Removed;
    }

    bool SnapshotFrameForDecode(
        const FFrame& Source,
        std::array<uint8, MaxFrameSnapshotSize>& Storage,
        FFrame*& OutFrame)
    {
        OutFrame = nullptr;
        if (Offsets::FFrame_PropertyChainForCompiledIn >
                MaxFrameSnapshotSize - sizeof(void*) ||
            Offsets::FFrame_CurrentNativeFunction >
                MaxFrameSnapshotSize - sizeof(void*))
        {
            return false;
        }

        uint32 RequiredSize =
            static_cast<uint32>(sizeof(FFrame));
        RequiredSize = (std::max)(
            RequiredSize,
            Offsets::FFrame_PropertyChainForCompiledIn +
                static_cast<uint32>(sizeof(void*)));
        RequiredSize = (std::max)(
            RequiredSize,
            Offsets::FFrame_CurrentNativeFunction +
                static_cast<uint32>(sizeof(void*)));
        if (RequiredSize > Storage.size() ||
            !SDK::MemReadable(&Source, RequiredSize))
        {
            return false;
        }

        Storage.fill(0);
        std::memcpy(
            Storage.data(),
            &Source,
            RequiredSize);
        OutFrame = reinterpret_cast<FFrame*>(Storage.data());
        return true;
    }

    bool DecodeApplyModTagImpl(
        const FFrame& Source,
        FGameplayTag& OutTag,
        int32& OutComparisonIndex)
    {
        OutTag = {};
        OutComparisonIndex = 0;

        std::array<uint8, MaxFrameSnapshotSize> Storage{};
        FFrame* Frame = nullptr;
        if (!SnapshotFrameForDecode(Source, Storage, Frame))
            return false;

        if (Frame->Code)
        {
            if (!Offsets::Step)
                return false;
        }
        else
        {
            if (!Offsets::StepExplicitProperty)
                return false;

            const auto ChainAddress =
                reinterpret_cast<const uint8*>(Frame) +
                Offsets::FFrame_PropertyChainForCompiledIn;
            const UField* PropertyChain = nullptr;
            if (!SDK::MemReadable(
                    ChainAddress,
                    sizeof(PropertyChain)))
            {
                return false;
            }
            std::memcpy(
                &PropertyChain,
                ChainAddress,
                sizeof(PropertyChain));
            if (!PropertyChain ||
                !SDK::MemReadable(
                    reinterpret_cast<const uint8*>(
                        PropertyChain) +
                        Offsets::FFrame_Next,
                    sizeof(void*)))
            {
                return false;
            }
        }

        void* TagSource =
            Frame->StepCompiledInRefInternal(&OutTag);
        if (!TagSource ||
            !SDK::MemReadable(
                TagSource,
                FGameplayTag::Size()))
        {
            return false;
        }
        if (TagSource != &OutTag)
        {
            std::memcpy(
                &OutTag,
                TagSource,
                FGameplayTag::Size());
        }
        return ReadGameplayTagIndex(
            OutTag,
            OutComparisonIndex);
    }

    bool DecodeApplyModTag(
        const FFrame& Source,
        FGameplayTag& OutTag,
        int32& OutComparisonIndex)
    {
        bool Succeeded = false;
        __try
        {
            Succeeded = DecodeApplyModTagImpl(
                Source,
                OutTag,
                OutComparisonIndex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            OutTag = {};
            OutComparisonIndex = 0;
            Succeeded = false;
        }
        return Succeeded;
    }

    bool IsLiveVehicle(
        AFortAthenaVehicle* Vehicle,
        const FVehicleModSchema& Schema)
    {
        if (!Vehicle ||
            !SDK::MemReadable(Vehicle, sizeof(UObject)) ||
            !Vehicle->Class ||
            !SDK::MemReadable(Vehicle->Class, 0x40) ||
            !Schema.VehicleClass ||
            !SDK::MemReadable(Schema.VehicleClass, 0x40) ||
            !Vehicle->IsA(Schema.VehicleClass) ||
            Vehicle->IsDefaultObject())
        {
            return false;
        }

        const uint32 ObjectFlags =
            static_cast<uint32>(static_cast<int32>(
                Vehicle->ObjectFlags));
        if ((ObjectFlags &
                (RF_BeginDestroyed |
                    RF_FinishDestroyed |
                    RF_Garbage)) != 0)
        {
            return false;
        }

        if (Vehicle->HasbActorIsBeingDestroyed() &&
            Vehicle->bActorIsBeingDestroyed)
        {
            return false;
        }

        auto* World = UWorld::GetWorld();
        if (!World ||
            !SDK::MemReadable(World, sizeof(UObject)))
        {
            return false;
        }

        // A live actor is ultimately outered to its current UWorld (normally
        // through a ULevel). This rejects class archetypes and stale actors
        // left over from a previous world without relying on a tick scan.
        const UObject* Outer = Vehicle->Outer;
        for (uint8 Depth = 0; Outer && Depth < 8; ++Depth)
        {
            if (Outer == World)
                return true;

            if (!SDK::MemReadable(Outer, sizeof(UObject)) ||
                !Outer->Class)
            {
                return false;
            }
            Outer = Outer->Outer;
        }

        return false;
    }

    AFortAthenaVehicle* GetVehicle(
        UObject* InterfaceContext,
        FFrame& Stack)
    {
        auto& Schema = ResolveVehicleModSchema();
        if (!Schema.VehicleClass)
            return nullptr;

        // Blueprint interface callers can leave their own actor in
        // FFrame::Object. Prefer a real UObject candidate when present, then
        // map the adjusted interface context back to one of the vehicles we
        // registered at spawn. InterfaceContext itself must never be
        // dereferenced or cast to UObject.
        UObject* Candidate = Stack.Object;
        if (Candidate &&
            SDK::MemReadable(Candidate, sizeof(UObject)) &&
            Candidate->Class &&
            SDK::MemReadable(Candidate->Class, 0x40) &&
            SDK::MemReadable(Schema.VehicleClass, 0x40) &&
            Candidate->IsA(Schema.VehicleClass))
        {
            auto* Vehicle =
                static_cast<AFortAthenaVehicle*>(Candidate);
            if (IsLiveVehicle(Vehicle, Schema))
                return Vehicle;
        }

        const void* RawContext =
            reinterpret_cast<const void*>(InterfaceContext);
        const void* RawStackObject =
            reinterpret_cast<const void*>(Stack.Object);

        // Registration caches the adjusted interface pointer. The common path
        // is therefore only pointer comparisons plus one liveness check for
        // the matching vehicle, even when a map has hundreds of cars.
        for (auto& State : GVehicleModConstructionStates)
        {
            auto* Vehicle = State.Vehicle.Get();
            if (!Vehicle)
                continue;

            if (reinterpret_cast<const void*>(Vehicle) ==
                    RawContext ||
                reinterpret_cast<const void*>(Vehicle) ==
                    RawStackObject ||
                (State.VehicleInterface &&
                    (reinterpret_cast<const void*>(
                            State.VehicleInterface) ==
                            RawContext ||
                        reinterpret_cast<const void*>(
                            State.VehicleInterface) ==
                            RawStackObject)))
            {
                if (IsLiveVehicle(Vehicle, Schema))
                    return Vehicle;
            }
        }

        // If registration happened before the interface schema streamed in,
        // populate the missing cache lazily once. Never dereference Context.
        for (auto& State : GVehicleModConstructionStates)
        {
            if (State.VehicleInterface)
                continue;

            auto* Vehicle = State.Vehicle.Get();
            if (!IsLiveVehicle(Vehicle, Schema))
                continue;

            State.VehicleInterface = Vehicle->GetInterface(
                    Schema.VehicleInterfaceClass);
            if (State.VehicleInterface &&
                (reinterpret_cast<const void*>(
                        State.VehicleInterface) == RawContext ||
                    reinterpret_cast<const void*>(
                        State.VehicleInterface) == RawStackObject))
            {
                return Vehicle;
            }
        }

        return nullptr;
    }

    void LogConstructionSkip(
        const char* Reason,
        const AFortAthenaVehicle* Vehicle)
    {
        if (GConstructionSkipLogCount++ >= 4)
            return;

        SDK::DbgLog(
            "[VehicleMods] Native construction skipped "
            "reason=%s vehicle=%p\n",
            Reason,
            Vehicle);
    }

    bool QueueNativeVehicleModCollection(
        AFortAthenaVehicle* Vehicle,
        FVehicleModConstructionState& State,
        const FVehicleModSchema& Schema)
    {
        if (!Vehicle ||
            !Schema.VehicleInterfaceClass ||
            !Schema.VehicleModSubsystemClass ||
            !Schema.SubsystemBlueprintLibrary ||
            !Schema.GetGameInstanceSubsystem ||
            !Schema.QueueCollectModDataForVehicle ||
            !SDK::MemReadable(
                Schema.SubsystemBlueprintLibrary,
                sizeof(UObject)))
        {
            return false;
        }

        bool Queued = false;
        __try
        {
            // Resolve the subsystem through the current vehicle's game
            // instance every time. Subsystems are world-lifetime objects and
            // must never be cached across matches.
            alignas(8) std::array<uint8, 0x18> Params{};
            UObject* ContextObject = Vehicle;
            const UClass* SubsystemClass =
                Schema.VehicleModSubsystemClass;
            std::memcpy(
                Params.data(),
                &ContextObject,
                sizeof(ContextObject));
            std::memcpy(
                Params.data() + 0x8,
                &SubsystemClass,
                sizeof(SubsystemClass));
            Schema.SubsystemBlueprintLibrary->ProcessEvent(
                Schema.GetGameInstanceSubsystem,
                Params.data());

            UObject* Subsystem = nullptr;
            std::memcpy(
                &Subsystem,
                Params.data() + 0x10,
                sizeof(Subsystem));
            if (!Subsystem ||
                !SDK::MemReadable(
                    Subsystem,
                    sizeof(UObject)) ||
                !Subsystem->Class ||
                !SDK::MemReadable(
                    Subsystem->Class,
                    0x40) ||
                !Subsystem->IsA(
                    Schema.VehicleModSubsystemClass) ||
                Subsystem->IsDefaultObject())
            {
                return false;
            }

            auto* World = UWorld::GetWorld();
            if (!World ||
                !SDK::MemReadable(World, sizeof(UObject)) ||
                !World->HasOwningGameInstance())
            {
                return false;
            }

            auto* GameInstance = World->OwningGameInstance;
            if (!GameInstance ||
                !SDK::MemReadable(
                    GameInstance,
                    sizeof(UObject)))
            {
                return false;
            }

            bool OwnedByCurrentGameInstance = false;
            const UObject* Outer = Subsystem;
            for (uint8 Depth = 0;
                Outer && Depth < 8;
                ++Depth)
            {
                if (Outer == GameInstance)
                {
                    OwnedByCurrentGameInstance = true;
                    break;
                }
                if (!SDK::MemReadable(
                        Outer,
                        sizeof(UObject)) ||
                    !Outer->Class)
                {
                    break;
                }
                Outer = Outer->Outer;
            }
            if (!OwnedByCurrentGameInstance)
                return false;

            // The interface address is an adjusted pointer whose lifetime is
            // tied to this exact vehicle. Refresh it immediately before the
            // native call instead of reusing a prior-world or pre-construction
            // cache.
            State.VehicleInterface = Vehicle->GetInterface(
                Schema.VehicleInterfaceClass);
            if (!State.VehicleInterface ||
                !SDK::MemReadable(
                    State.VehicleInterface,
                    sizeof(void*)))
            {
                return false;
            }

            // QueueCollectModDataForVehicle takes the adjusted
            // IFortVehicleInterface pointer, not the primary UObject. Native
            // code inserts its weak owner into VehiclesPendingModCollection
            // and schedules the subsystem's normal asynchronous collector.
            Schema.QueueCollectModDataForVehicle(
                Subsystem,
                State.VehicleInterface);
            Queued = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Queued = false;
        }
        return Queued;
    }

    bool IsUsableObject(
        const UObject* Object,
        bool RequireDefaultObject)
    {
        if (!Object ||
            !SDK::MemReadable(Object, sizeof(UObject)) ||
            !Object->Class ||
            !SDK::MemReadable(Object->Class, 0x40))
        {
            return false;
        }
        return !RequireDefaultObject ||
            Object->IsDefaultObject();
    }

    bool ReadVirtualFunction(
        const void* Object,
        uint32 VirtualOffset,
        uintptr_t& OutFunction)
    {
        OutFunction = 0;
        if (!Object ||
            !SDK::MemReadable(Object, sizeof(void*)))
        {
            return false;
        }

        const void* VTable = nullptr;
        std::memcpy(
            &VTable,
            Object,
            sizeof(VTable));
        if (!VTable ||
            !SDK::MemReadable(
                reinterpret_cast<const uint8*>(VTable) +
                    VirtualOffset,
                sizeof(void*)))
        {
            return false;
        }

        std::memcpy(
            &OutFunction,
            reinterpret_cast<const uint8*>(VTable) +
                VirtualOffset,
            sizeof(OutFunction));
        return OutFunction &&
            SDK::MemReadable(
                reinterpret_cast<const void*>(OutFunction),
                1) &&
            *reinterpret_cast<const uint8*>(OutFunction) != 0xC3;
    }

    bool RefreshNativeTireMod(
        const IInterface* VehicleInterface,
        const FGameplayTag& CanonicalTag)
    {
        uintptr_t FunctionAddress = 0;
        if (!ReadVirtualFunction(
                VehicleInterface,
                VehicleApplyTireModVirtualOffset,
                FunctionAddress) ||
            !SDK::MemReadable(
                reinterpret_cast<const void*>(FunctionAddress),
                DagwoodApplyTireModThunk.size()) ||
            std::memcmp(
                reinterpret_cast<const void*>(FunctionAddress),
                DagwoodApplyTireModThunk.data(),
                DagwoodApplyTireModThunk.size()) != 0)
        {
            return false;
        }

        bool Succeeded = false;
        __try
        {
            reinterpret_cast<FApplyTireModImplementation>(
                FunctionAddress)(
                    VehicleInterface,
                    &CanonicalTag,
                    nullptr);
            Succeeded = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Succeeded = false;
        }
        return Succeeded;
    }

    bool TryInvokeZeroParameterFunction(
        UObject* Object,
        UFunction* Function)
    {
        bool Succeeded = false;
        __try
        {
            Object->ProcessEvent(Function, nullptr);
            Succeeded = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Succeeded = false;
        }
        return Succeeded;
    }

    bool InvokeZeroParameterFunction(
        UObject* Object,
        const char* FunctionName)
    {
        if (!IsUsableObject(Object, false) || !FunctionName)
            return false;

        UFunction* Function = Object->GetFunction(FunctionName);
        if (!Function ||
            !Function->ExecFunction ||
            !SDK::MemReadable(Function->ExecFunction, 1))
        {
            return false;
        }

        const auto Params = Function->GetParamsNamed();
        if (Params.Size != 0 || !Params.NameOffsetMap.empty())
            return false;

        return TryInvokeZeroParameterFunction(Object, Function);
    }

    int32 ReadReflectedArrayCount(
        const UObject* Object,
        const char* PropertyName)
    {
        if (!IsUsableObject(Object, false) || !PropertyName)
            return -1;

        const uint32 Offset = Object->GetOffset(PropertyName);
        if (Offset == UINT32_MAX ||
            Offset > 0x100000 ||
            !SDK::MemReadable(
                reinterpret_cast<const uint8*>(Object) + Offset,
                0x10))
        {
            return -1;
        }

        struct FArrayHeader
        {
            const void* Data = nullptr;
            int32 Num = 0;
            int32 Max = 0;
        };
        FArrayHeader Header{};
        std::memcpy(
            &Header,
            reinterpret_cast<const uint8*>(Object) + Offset,
            sizeof(Header));
        if (Header.Num < 0 ||
            Header.Max < Header.Num ||
            Header.Max > 4096 ||
            (Header.Num > 0 &&
                (!Header.Data ||
                    !SDK::MemReadable(Header.Data, 1))))
        {
            return -1;
        }
        return Header.Num;
    }

    UObject* ReadReflectedObject(
        const UObject* Object,
        const char* PropertyName)
    {
        if (!IsUsableObject(Object, false) || !PropertyName)
            return nullptr;

        const uint32 Offset = Object->GetOffset(PropertyName);
        if (Offset == UINT32_MAX ||
            Offset > 0x100000 ||
            !SDK::MemReadable(
                reinterpret_cast<const uint8*>(Object) + Offset,
                sizeof(UObject*)))
        {
            return nullptr;
        }

        UObject* Value = nullptr;
        std::memcpy(
            &Value,
            reinterpret_cast<const uint8*>(Object) + Offset,
            sizeof(Value));
        return IsUsableObject(Value, false) ? Value : nullptr;
    }

    void EnsureVehicleDamageablePartsInitialized(
        AFortAthenaVehicle* Vehicle,
        FVehicleModConstructionState& State,
        ULONGLONG CurrentTimeMs)
    {
        if (State.DamageablePartsReady ||
            State.DamageablePartsAttempts >= 6 ||
            CurrentTimeMs <
                State.NextDamageablePartsAttemptTimeMs ||
            !IsUsableObject(Vehicle, false) ||
            !Vehicle->HasRole() ||
            !Vehicle->HasAuthority())
        {
            return;
        }

        const int32 RuntimePartCount =
            ReadReflectedArrayCount(
                Vehicle,
                "DamageableParts");
        const int32 TireStateCount =
            ReadReflectedArrayCount(
                Vehicle,
                "TireStates");
        // A partially initialized raw-spawned vehicle can already have its
        // damageable-part records while the replicated tire-state array is
        // still empty. Treating that state as ready permanently skips the
        // native refresh and leaves otherwise configured tires unable to
        // transition to popped. FN30's wheeled Valet configs initialize both
        // arrays together; vehicles with no configured breakable parts are
        // handled by the explicit zero-config case below.
        if (RuntimePartCount > 0 && TireStateCount > 0)
        {
            State.DamageablePartsReady = true;
            if (GDamageablePartsLogCount++ < 24)
            {
                SDK::DbgLog(
                    "[VehicleParts] ready vehicle=%s(%p) "
                    "runtime=%d tires=%d attempts=%u\n",
                    Vehicle->Name.ToString().c_str(),
                    Vehicle,
                    RuntimePartCount,
                    TireStateCount,
                    State.DamageablePartsAttempts);
            }
            return;
        }

        UObject* VehicleConfigs =
            ReadReflectedObject(
                Vehicle,
                "FortPhysicsVehicleConfigs");
        const int32 ConfigPartCount =
            ReadReflectedArrayCount(
                VehicleConfigs,
                "DamageableParts");
        if (VehicleConfigs && ConfigPartCount == 0)
        {
            // Motorcycles and other vehicles can legitimately have no
            // configured breakable parts.
            State.DamageablePartsReady = true;
            return;
        }

        ++State.DamageablePartsAttempts;
        State.NextDamageablePartsAttemptTimeMs =
            CurrentTimeMs + 1000ULL;
        bool Refreshed = false;
        if (VehicleConfigs && ConfigPartCount > 0)
        {
            Refreshed =
                InvokeZeroParameterFunction(
                    Vehicle,
                    "RefreshDamageableParts");
        }

        const int32 RefreshedPartCount =
            ReadReflectedArrayCount(
                Vehicle,
                "DamageableParts");
        const int32 RefreshedTireCount =
            ReadReflectedArrayCount(
                Vehicle,
                "TireStates");
        State.DamageablePartsReady =
            RefreshedPartCount > 0 &&
            RefreshedTireCount > 0;
        if (State.DamageablePartsReady)
        {
            Vehicle->FlushNetDormancy();
            Vehicle->ForceNetUpdate();
        }

        if (GDamageablePartsLogCount++ < 24)
        {
            SDK::DbgLog(
                "[VehicleParts] initialize vehicle=%s(%p) "
                "configs=%p configured=%d runtime=%d->%d "
                "tires=%d->%d refreshed=%d attempt=%u ready=%d\n",
                Vehicle->Name.ToString().c_str(),
                Vehicle,
                VehicleConfigs,
                ConfigPartCount,
                RuntimePartCount,
                RefreshedPartCount,
                TireStateCount,
                RefreshedTireCount,
                Refreshed ? 1 : 0,
                State.DamageablePartsAttempts,
                State.DamageablePartsReady ? 1 : 0);
        }
    }

    bool TryHandlePawnEnteredVehicleModSeat(
        UObject* ModComponent,
        UFunction* Function,
        const TScriptInterface<IFortVehicleInterface>& VehicleInterface,
        AFortPlayerPawnAthena* PlayerPawn,
        int32 SeatIndex)
    {
        struct alignas(8) FHandlePawnEnteredSeatParams
        {
            TScriptInterface<IFortVehicleInterface> VehicleInterface{};
            AFortPlayerPawnAthena* PlayerPawn = nullptr;
            int32 SeatIndex = -1;
            uint8 Pad[4]{};
        } Params;
        static_assert(
            sizeof(FHandlePawnEnteredSeatParams) == 0x20,
            "FN30 HandlePawnEnteredSeat ABI changed");

        Params.VehicleInterface = VehicleInterface;
        Params.PlayerPawn = PlayerPawn;
        Params.SeatIndex = SeatIndex;

        bool Succeeded = false;
        __try
        {
            ModComponent->ProcessEvent(Function, &Params);
            Succeeded = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Succeeded = false;
        }
        return Succeeded;
    }

    bool SynchronizeOccupiedVehicleWeaponSeat(
        AFortAthenaVehicle* Vehicle,
        UObject* ExactModComponent,
        const IInterface* NativeVehicleInterface)
    {
        if (!IsUsableObject(Vehicle, false) ||
            !IsUsableObject(ExactModComponent, false) ||
            !NativeVehicleInterface)
        {
            return false;
        }

        auto* WeaponSeatClass =
            UFortVehicleSeatWeaponComponent::StaticClass();
        auto* SeatClass =
            UFortVehicleSeatComponent::StaticClass();
        if (!WeaponSeatClass || !SeatClass)
            return false;

        auto* SeatComponent =
            reinterpret_cast<UFortVehicleSeatComponent*>(
                Vehicle->GetComponentByClass(
                    SeatClass));
        if (!IsUsableObject(SeatComponent, false) ||
            SeatComponent->PlayerSlots.Num() < 0 ||
            SeatComponent->PlayerSlots.Num() > 16 ||
            SeatComponent->PlayerSlots.Max() <
                SeatComponent->PlayerSlots.Num() ||
            (SeatComponent->PlayerSlots.Num() > 0 &&
                (!SeatComponent->PlayerSlots.GetData() ||
                    !SDK::MemReadable(
                        SeatComponent->PlayerSlots.GetData(),
                        static_cast<size_t>(
                            SeatComponent->PlayerSlots.Num()) *
                            FAthenaCarPlayerSlot::Size()))))
        {
            return false;
        }

        uint32 DynamicComponentsOffset = 0;
        const UField* DynamicComponentsProperty =
            ExactModComponent->GetProperty(
                "DynamicSeatWeaponComponents");
        if (!ReadPropertyMetadata(
                DynamicComponentsProperty,
                ExactModComponent->Class->GetPropertiesSize(),
                static_cast<uint32>(
                    sizeof(TArray<
                        UFortVehicleSeatWeaponComponent*>)),
                DynamicComponentsOffset))
        {
            return false;
        }

        auto& DynamicComponents =
            GetFromOffset<
                TArray<UFortVehicleSeatWeaponComponent*>>(
                    ExactModComponent,
                    DynamicComponentsOffset);
        if (DynamicComponents.Num() < 0 ||
            DynamicComponents.Num() > 16 ||
            DynamicComponents.Max() < DynamicComponents.Num() ||
            (DynamicComponents.Num() > 0 &&
                (!DynamicComponents.GetData() ||
                    !SDK::MemReadable(
                        DynamicComponents.GetData(),
                        static_cast<size_t>(
                            DynamicComponents.Num()) *
                            sizeof(
                                UFortVehicleSeatWeaponComponent*)))))
        {
            return false;
        }

        constexpr FExpectedParam HandleSeatParams[] =
        {
            {
                "VehicleInterface",
                0x0,
                uint32(sizeof(
                    TScriptInterface<IFortVehicleInterface>)),
                CPF_ConstParm | CPF_Parm | CPF_OutParm |
                    CPF_ReferenceParm,
                CPF_ReturnParm
            },
            {
                "PlayerPawn",
                0x10,
                uint32(sizeof(AFortPlayerPawnAthena*)),
                CPF_Parm,
                CPF_OutParm | CPF_ReturnParm | CPF_ReferenceParm
            },
            {
                "SeatIndex",
                0x18,
                uint32(sizeof(int32)),
                CPF_Parm,
                CPF_OutParm | CPF_ReturnParm | CPF_ReferenceParm
            }
        };
        auto* HandlePawnEnteredSeat =
            ExactModComponent->GetFunction(
                "HandlePawnEnteredSeat");
        if (!ValidateFunction(
                HandlePawnEnteredSeat,
                0x20,
                HandleSeatParams))
        {
            return false;
        }

        TScriptInterface<IFortVehicleInterface>
            VehicleInterface{};
        VehicleInterface.ObjectPointer = Vehicle;
        VehicleInterface.InterfacePointer =
            NativeVehicleInterface;
        std::array<int32, 16> HandledSeats{};
        HandledSeats.fill(-1);
        int32 HandledSeatCount = 0;

        for (int32 ComponentIndex = 0;
            ComponentIndex < DynamicComponents.Num();
            ++ComponentIndex)
        {
            auto* WeaponSeatComponent =
                DynamicComponents[ComponentIndex];
            if (!IsUsableObject(
                    WeaponSeatComponent, false) ||
                !WeaponSeatComponent->IsA(
                    WeaponSeatClass) ||
                !WeaponSeatComponent->HasActiveSeatIdx())
            {
                continue;
            }

            auto& Definitions =
                WeaponSeatComponent
                    ->WeaponSeatDefinitions;
            const int32 DefinitionSize =
                FWeaponSeatDefinition::Size();
            if (DefinitionSize <= 0 ||
                DefinitionSize > 0x100 ||
                Definitions.Num() < 0 ||
                Definitions.Num() > 16 ||
                Definitions.Max() < Definitions.Num() ||
                (Definitions.Num() > 0 &&
                    (!Definitions.GetData() ||
                        !SDK::MemReadable(
                            Definitions.GetData(),
                            static_cast<size_t>(
                                Definitions.Num()) *
                                DefinitionSize))))
            {
                continue;
            }

            for (int32 DefinitionIndex = 0;
                DefinitionIndex < Definitions.Num();
                ++DefinitionIndex)
            {
                auto& Definition = Definitions.Get(
                    DefinitionIndex, DefinitionSize);
                int32 SeatIndex =
                    Definition.SeatIndex;
                if (SeatIndex < 0 ||
                    SeatIndex >=
                        SeatComponent->PlayerSlots.Num() ||
                    !Definition.VehicleWeapon)
                {
                    continue;
                }

                auto& PlayerSlot =
                    SeatComponent->PlayerSlots.Get(
                        SeatIndex,
                        FAthenaCarPlayerSlot::Size());
                if (!IsUsableObject(
                        PlayerSlot.Player, false))
                {
                    continue;
                }

                WeaponSeatComponent->ActiveSeatIdx =
                    SeatIndex;
                bool AlreadyHandled = false;
                for (int32 HandledIndex = 0;
                    HandledIndex < HandledSeatCount;
                    ++HandledIndex)
                {
                    if (HandledSeats[HandledIndex] ==
                        SeatIndex)
                    {
                        AlreadyHandled = true;
                        break;
                    }
                }
                if (AlreadyHandled)
                    continue;

                if (TryHandlePawnEnteredVehicleModSeat(
                        ExactModComponent,
                        HandlePawnEnteredSeat,
                        VehicleInterface,
                        PlayerSlot.Player,
                        SeatIndex))
                {
                    HandledSeats[HandledSeatCount++] =
                        SeatIndex;
                }
            }
        }
        if (HandledSeatCount > 0 &&
            GSeatLifecycleLogCount++ < 16)
        {
            SDK::DbgLog(
                "[VehicleMods] replayed occupied-seat lifecycle "
                "vehicle=%p component=%p dynamic-components=%d "
                "handled-seats=%d\n",
                Vehicle,
                ExactModComponent,
                DynamicComponents.Num(),
                HandledSeatCount);
        }
        return HandledSeatCount > 0;
    }

    bool ReadVehicleModConfigTag(
        const UObject* Config,
        uint32 Offset,
        FGameplayTag& OutTag)
    {
        OutTag = {};
        if (!IsUsableObject(Config, true) ||
            !SDK::MemReadable(
                reinterpret_cast<const uint8*>(Config) + Offset,
                FGameplayTag::Size()))
        {
            return false;
        }

        std::memcpy(
            &OutTag,
            reinterpret_cast<const uint8*>(Config) + Offset,
            FGameplayTag::Size());
        int32 ComparisonIndex = 0;
        return ReadGameplayTagIndex(
            OutTag,
            ComparisonIndex);
    }

    UObject* ResolveCompatibleVehicleModConfig(
        const FVehicleModSchema& Schema,
        const IInterface* VehicleInterface,
        const FGameplayTag& IncomingTag,
        bool& OutCallSucceeded)
    {
        OutCallSucceeded = false;
        UObject* Config = nullptr;
        if (!Schema.ResolveVehicleModConfig ||
            !VehicleInterface)
        {
            return nullptr;
        }

        __try
        {
            Config = Schema.ResolveVehicleModConfig(
                VehicleInterface,
                &IncomingTag);
            OutCallSucceeded = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Config = nullptr;
            OutCallSucceeded = false;
        }
        return Config;
    }

    UObject* GetVehicleModComponent(
        const IInterface* VehicleInterface,
        const FGameplayTag& Tag,
        uint32 VirtualOffset,
        bool& OutCallSucceeded)
    {
        OutCallSucceeded = false;
        uintptr_t FunctionAddress = 0;
        if (!ReadVirtualFunction(
                VehicleInterface,
                VirtualOffset,
                FunctionAddress))
        {
            return nullptr;
        }

        UObject* Component = nullptr;
        const auto Function =
            reinterpret_cast<FGetVehicleModComponent>(
                FunctionAddress);
        __try
        {
            Component = Function(
                VehicleInterface,
                &Tag);
            OutCallSucceeded = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Component = nullptr;
            OutCallSucceeded = false;
        }
        return Component;
    }

    UObject* ReadVehicleModComponentConfig(
        const UObject* Component)
    {
        if (!IsUsableObject(Component, false) ||
            !SDK::MemReadable(
                reinterpret_cast<const uint8*>(Component) +
                    VehicleModComponentConfigOffset,
                sizeof(UObject*)))
        {
            return nullptr;
        }

        UObject* Config = nullptr;
        std::memcpy(
            &Config,
            reinterpret_cast<const uint8*>(Component) +
                VehicleModComponentConfigOffset,
            sizeof(Config));
        return IsUsableObject(Config, true)
            ? Config
            : nullptr;
    }

    bool IsVehicleModComponentEnabled(
        const UObject* Component)
    {
        if (!IsUsableObject(Component, false) ||
            !SDK::MemReadable(
                reinterpret_cast<const uint8*>(Component) +
                    VehicleModComponentEnabledOffset,
                sizeof(uint8)))
        {
            return false;
        }

        uint8 Enabled = 0;
        std::memcpy(
            &Enabled,
            reinterpret_cast<const uint8*>(Component) +
                VehicleModComponentEnabledOffset,
            sizeof(Enabled));
        return Enabled != 0;
    }

    bool InvokeVehicleModConfigAction(
        const UObject* Config,
        const IInterface* VehicleInterface,
        uintptr_t ExpectedFunction,
        uint32 VirtualOffset,
        FApplyVehicleModConfig Function)
    {
        uintptr_t VirtualFunction = 0;
        if (!Config ||
            !VehicleInterface ||
            !Function ||
            !ReadVirtualFunction(
                Config,
                VirtualOffset,
                VirtualFunction) ||
            VirtualFunction != ExpectedFunction)
        {
            return false;
        }

        bool Succeeded = false;
        __try
        {
            Function(
                const_cast<UObject*>(Config),
                VehicleInterface);
            Succeeded = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Succeeded = false;
        }
        return Succeeded;
    }

    bool RollBackNewVehicleModConfig(
        AFortAthenaVehicle* Vehicle,
        const FVehicleModSchema& Schema,
        const IInterface* VehicleInterface,
        const UObject* Config,
        const FGameplayTag& CanonicalTag)
    {
        // ApplyInternal can fail after registering a component. Ask the
        // retained teardown helper to remove any partial component while the
        // canonical tag is still visible to its HasAppliedMod lookup.
        InvokeVehicleModConfigAction(
            Config,
            VehicleInterface,
            reinterpret_cast<uintptr_t>(
                Schema.RemoveVehicleModConfig),
            VehicleModConfigRemoveVirtualOffset,
            reinterpret_cast<FApplyVehicleModConfig>(
                Schema.RemoveVehicleModConfig));

        bool LookupSucceeded = false;
        UObject* RemainingComponent =
            GetVehicleModComponent(
                VehicleInterface,
                CanonicalTag,
                VehicleGetModByTagVirtualOffset,
                LookupSucceeded);
        if (!LookupSucceeded ||
            ReadVehicleModComponentConfig(
                RemainingComponent) == Config)
        {
            // Keep the tag paired with a component that native teardown could
            // not remove; this avoids creating duplicate replicated state.
            return false;
        }

        return RemoveAppliedVehicleModTag(
            Vehicle,
            Schema,
            CanonicalTag);
    }

    bool ApplyNativeVehicleModConfigImpl(
        AFortAthenaVehicle* Vehicle,
        const FGameplayTag& IncomingTag,
        bool& OutConfigResolved,
        bool& OutReplacedExisting,
        bool& OutAlreadyApplied,
        uint8& OutStage)
    {
        OutConfigResolved = false;
        OutReplacedExisting = false;
        OutAlreadyApplied = false;
        OutStage = 0;

        auto& Schema = ResolveVehicleModSchema();
        if (!IsLiveVehicle(Vehicle, Schema) ||
            !Vehicle->HasRole() ||
            !Vehicle->HasAuthority() ||
            !Schema.ResolveVehicleModConfig ||
            !Schema.ApplyVehicleModConfig ||
            !Schema.RemoveVehicleModConfig ||
            !Schema.AppliedVehicleModTagsOffset)
        {
            return false;
        }
        OutStage = 1;

        auto* State = FindConstructionState(Vehicle, true);
        if (!State)
            return false;

        State->VehicleInterface =
            Vehicle->GetInterface(
                Schema.VehicleInterfaceClass);
        const IInterface* VehicleInterface =
            State->VehicleInterface;
        if (!VehicleInterface ||
            !SDK::MemReadable(
                VehicleInterface,
                sizeof(void*)))
        {
            return false;
        }
        OutStage = 2;

        bool ResolverCallSucceeded = false;
        UObject* Config =
            ResolveCompatibleVehicleModConfig(
                Schema,
                VehicleInterface,
                IncomingTag,
                ResolverCallSucceeded);
        if (!ResolverCallSucceeded ||
            !IsUsableObject(Config, true))
        {
            return false;
        }
        OutConfigResolved = true;
        OutStage = 3;

        uintptr_t ApplyVirtual = 0;
        if (!ReadVirtualFunction(
                Config,
                VehicleModConfigApplyVirtualOffset,
                ApplyVirtual) ||
            ApplyVirtual != reinterpret_cast<uintptr_t>(
                Schema.ApplyVehicleModConfig))
        {
            return false;
        }
        OutStage = 4;

        FGameplayTag CanonicalTag{};
        if (!ReadVehicleModConfigTag(
                Config,
                VehicleModConfigTagOffset,
                CanonicalTag))
        {
            return false;
        }
        OutStage = 5;

        bool ComponentLookupSucceeded = false;
        UObject* ExactComponent =
            GetVehicleModComponent(
                VehicleInterface,
                CanonicalTag,
                VehicleGetModByTagVirtualOffset,
                ComponentLookupSucceeded);
        if (ComponentLookupSucceeded &&
            IsUsableObject(ExactComponent, false) &&
            ReadVehicleModComponentConfig(ExactComponent) == Config)
        {
            OutAlreadyApplied = true;
        }

        FGameplayTag SlotTag{};
        bool IsTireModConfig = false;
        if (ReadVehicleModConfigTag(
                Config,
                VehicleModConfigSlotOffset,
                SlotTag))
        {
            // FN30 identifies the tire family by the config's slot tag. Do
            // not infer this from a display/canonical name: passing a bumper
            // or turret config through Dagwood's tire transition can replace
            // or corrupt an unrelated tire state.
            IsTireModConfig =
                SlotTag.TagName.ToString() ==
                    "Vehicle.Mod.Slot.Tire";

            bool SlotLookupSucceeded = false;
            UObject* ExistingComponent =
                GetVehicleModComponent(
                    VehicleInterface,
                    SlotTag,
                    VehicleGetModBySlotVirtualOffset,
                    SlotLookupSucceeded);
            UObject* ExistingConfig =
                SlotLookupSucceeded
                    ? ReadVehicleModComponentConfig(
                        ExistingComponent)
                    : nullptr;
            if (ExistingConfig == Config &&
                IsUsableObject(ExistingComponent, false))
            {
                OutAlreadyApplied = true;
            }

            if (ExistingConfig &&
                ExistingConfig != Config)
            {
                FGameplayTag ExistingCanonicalTag{};
                uintptr_t RemoveVirtual = 0;
                if (!ReadVehicleModConfigTag(
                        ExistingConfig,
                        VehicleModConfigTagOffset,
                        ExistingCanonicalTag) ||
                    !ReadVirtualFunction(
                        ExistingConfig,
                        VehicleModConfigRemoveVirtualOffset,
                        RemoveVirtual) ||
                    RemoveVirtual !=
                        reinterpret_cast<uintptr_t>(
                            Schema.RemoveVehicleModConfig))
                {
                    return false;
                }

                // RemoveFromVehicleInternal locates the old component through
                // HasAppliedMod, so repair a missing bookkeeping tag before
                // invoking it. Remove the old tag only after native teardown.
                if (!AddAppliedVehicleModTag(
                        Vehicle,
                        Schema,
                        ExistingCanonicalTag) ||
                    !InvokeVehicleModConfigAction(
                        ExistingConfig,
                        VehicleInterface,
                        reinterpret_cast<uintptr_t>(
                            Schema.RemoveVehicleModConfig),
                        VehicleModConfigRemoveVirtualOffset,
                        reinterpret_cast<
                            FApplyVehicleModConfig>(
                            Schema.RemoveVehicleModConfig)))
                {
                    return false;
                }

                bool RemovalLookupSucceeded = false;
                UObject* RemainingComponent =
                    GetVehicleModComponent(
                        VehicleInterface,
                        ExistingCanonicalTag,
                        VehicleGetModByTagVirtualOffset,
                        RemovalLookupSucceeded);
                if (!RemovalLookupSucceeded ||
                    ReadVehicleModComponentConfig(
                        RemainingComponent) ==
                        ExistingConfig)
                {
                    return false;
                }

                if (!RemoveAppliedVehicleModTag(
                        Vehicle,
                        Schema,
                        ExistingCanonicalTag))
                {
                    return false;
                }
                OutReplacedExisting = true;
            }
        }
        OutStage = 6;

        // Native registration, BeginPlay, effects, and OnModApplied callbacks
        // can query HasAppliedMod while ApplyToVehicleInternal is still on the
        // stack. Mirror the server path by publishing the canonical tag first,
        // then roll it back unless native component creation is verified.
        const bool HadCanonicalTag =
            HasAppliedVehicleModTag(
                Vehicle,
                Schema,
                CanonicalTag);
        if (!AddAppliedVehicleModTag(
                Vehicle,
                Schema,
                CanonicalTag))
        {
            return false;
        }
        OutStage = 7;

        if (!InvokeVehicleModConfigAction(
                Config,
                VehicleInterface,
                reinterpret_cast<uintptr_t>(
                    Schema.ApplyVehicleModConfig),
                VehicleModConfigApplyVirtualOffset,
                Schema.ApplyVehicleModConfig))
        {
            if (!HadCanonicalTag)
            {
                RollBackNewVehicleModConfig(
                    Vehicle,
                    Schema,
                    VehicleInterface,
                    Config,
                    CanonicalTag);
            }
            return false;
        }
        OutStage = 8;

        ComponentLookupSucceeded = false;
        ExactComponent = GetVehicleModComponent(
            VehicleInterface,
            CanonicalTag,
            VehicleGetModByTagVirtualOffset,
            ComponentLookupSucceeded);
        if (!ComponentLookupSucceeded ||
            !IsUsableObject(ExactComponent, false) ||
            ReadVehicleModComponentConfig(ExactComponent) != Config ||
            !IsVehicleModComponentEnabled(ExactComponent))
        {
            if (!HadCanonicalTag)
            {
                RollBackNewVehicleModConfig(
                    Vehicle,
                    Schema,
                    VehicleInterface,
                    Config,
                    CanonicalTag);
            }
            return false;
        }
        OutStage = 9;

        // FN30's cooked tire configs use Vehicle.Mod.Slot.Tire (their
        // canonical tags are Vehicle.Mod.Tire.*). Mod boxes enter through
        // ApplyVehicleMod, so explicitly run the native tire transition
        // after the config/component has been created and verified.
        bool NativeTireTransitionSucceeded = false;
        bool TireReplicationQueued = false;
        bool TireConfigReplicationQueued = false;
        bool TireEnabledReplicationQueued = false;
        if (IsTireModConfig)
        {
            NativeTireTransitionSucceeded = RefreshNativeTireMod(
                VehicleInterface,
                CanonicalTag);
            // AppliedVehicleModTags is a push-model replicated property in
            // FN30. Directly editing its FGameplayTagContainer followed only
            // by ForceNetUpdate does not enqueue that property for clients.
            // The dynamically created mod component's ModConfig and
            // bModEnabled fields are RepNotify properties which drive
            // BP_OnModConfigReplicated/BP_OnModEnabled on each client.
            // Enqueue all three after component setup so the receiving
            // client can build the tire meshes in one actor update.
            TireReplicationQueued =
                MarkAppliedVehicleModTagsDirty(Vehicle);
            TireConfigReplicationQueued =
                MarkReplicatedPropertyDirty(
                    ExactComponent,
                    L"ModConfig");
            TireEnabledReplicationQueued =
                MarkReplicatedPropertyDirty(
                    ExactComponent,
                    L"bModEnabled");
        }

        // A turret mod may be attached while a passenger is already seated,
        // before its newly-created weapon-seat component can receive a seat
        // entry callback. Config-specific BP components expose this guarded
        // no-parameter refresh to reconcile the current occupants.
        InvokeZeroParameterFunction(
            ExactComponent,
            "RefreshSeatTimer");
        SynchronizeOccupiedVehicleWeaponSeat(
            Vehicle,
            ExactComponent,
            VehicleInterface);

        Vehicle->FlushNetDormancy();
        Vehicle->ForceNetUpdate();
        if (IsTireModConfig &&
            GTireLifecycleLogCount++ < 24)
        {
            SDK::DbgLog(
                "[VehicleMods] Tire lifecycle vehicle=%p "
                "config=%p component=%p "
                "native-transition=%d vehicle-dirty=%d "
                "config-dirty=%d enabled-dirty=%d "
                "applied-tags=%d\n",
                Vehicle,
                Config,
                ExactComponent,
                NativeTireTransitionSucceeded ? 1 : 0,
                TireReplicationQueued ? 1 : 0,
                TireConfigReplicationQueued ? 1 : 0,
                TireEnabledReplicationQueued ? 1 : 0,
                ReadAppliedVehicleModCount(
                    Vehicle,
                    Schema));
        }
        OutStage = 10;
        return true;
    }

    bool ApplyNativeVehicleModConfig(
        AFortAthenaVehicle* Vehicle,
        const FGameplayTag& IncomingTag,
        bool& OutConfigResolved,
        bool& OutReplacedExisting,
        bool& OutAlreadyApplied,
        uint8& OutStage)
    {
        OutConfigResolved = false;
        OutReplacedExisting = false;
        OutAlreadyApplied = false;
        OutStage = 0;
        if (GNativeConfigApplyInProgress)
            return false;

        GNativeConfigApplyInProgress = true;
        bool Result = false;
        __try
        {
            Result = ApplyNativeVehicleModConfigImpl(
                Vehicle,
                IncomingTag,
                OutConfigResolved,
                OutReplacedExisting,
                OutAlreadyApplied,
                OutStage);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Result = false;
        }
        GNativeConfigApplyInProgress = false;
        return Result;
    }

    void ConsumeCanApplyModParams(FFrame& Stack)
    {
        FGameplayTag IgnoredTag{};
        Stack.StepCompiledInRefInternal(&IgnoredTag);
        Stack.IncrementCode();
    }

    void ConsumeApplyModParams(FFrame& Stack)
    {
        FGameplayTag IgnoredTag{};
        UObject* IgnoredInstigator = nullptr;
        Stack.StepCompiledInRefInternal(&IgnoredTag);
        Stack.StepCompiledIn(&IgnoredInstigator);
        Stack.IncrementCode();
    }

    void CanApplyModHook(
        UObject* Context,
        FFrame& Stack,
        bool* Result)
    {
        // Construction normally dispatches shortly after spawn. Vehicles from
        // an untracked spawn path get one guarded on-demand attempt here. The
        // nested ProcessEvent owns a separate FFrame, so the original interface
        // frame remains untouched and must always be forwarded.
        AFortAthenaVehicle* Vehicle =
            GetVehicle(Context, Stack);
        TWeakObjectPtr<AFortAthenaVehicle> VehicleRef(Vehicle);
        FGameplayTag ModTag{};
        int32 ModTagIndex = 0;
        const bool ModTagKnown =
            DecodeApplyModTag(
                Stack,
                ModTag,
                ModTagIndex);
        if (Vehicle)
            FortVehicleMods::InitializeSpawnedVehicle(Vehicle);
        else if (GUnresolvedHookLogCount++ < 8)
            SDK::DbgLog(
                "[VehicleMods] CanApply owner unresolved "
                "context=%p stack-object=%p\n",
                Context,
                Stack.Object);

        if (GCanApplyModOriginal)
        {
            GCanApplyModOriginal(Context, Stack, Result);
        }
        else
        {
            ConsumeCanApplyModParams(Stack);
            if (Result)
                *Result = false;
        }

        Vehicle = VehicleRef.Get();
        if (Vehicle &&
            IsLiveVehicle(Vehicle, GVehicleModSchema) &&
            Result)
        {
            auto* State =
                FindConstructionState(Vehicle, false);
            if ((!State || !State->InProgress) &&
                GCanApplyResultLogCount++ < 16)
            {
                UObject* CollectedData = nullptr;
                ReadCollectedVehicleModData(
                    Vehicle,
                    GVehicleModSchema,
                    CollectedData);
                if (State)
                    State->Ready = CollectedData != nullptr;
                SDK::DbgLog(
                    "[VehicleMods] CanApply forwarded "
                    "vehicle=%s(%p) tag-index=0x%08X "
                    "tag-known=%d result=%d "
                    "dispatched=%d collected-ready=%d "
                    "applied-tags=%d\n",
                    Vehicle->Name.ToString().c_str(),
                    Vehicle,
                    static_cast<uint32>(ModTagIndex),
                    ModTagKnown,
                    *Result,
                    State && State->Dispatched,
                    CollectedData != nullptr,
                    ReadAppliedVehicleModCount(
                        Vehicle,
                        GVehicleModSchema));
            }
        }
    }

    void ApplyVehicleModHook(UObject* Context, FFrame& Stack)
    {
        AFortAthenaVehicle* Vehicle =
            GetVehicle(Context, Stack);
        TWeakObjectPtr<AFortAthenaVehicle> VehicleRef(Vehicle);
        FGameplayTag ModTag{};
        int32 ModTagIndex = 0;
        const bool ModTagKnown =
            DecodeApplyModTag(
                Stack,
                ModTag,
                ModTagIndex);
        const int32 AppliedBefore = Vehicle
            ? ReadAppliedVehicleModCount(
                Vehicle,
                GVehicleModSchema)
            : -1;
        if (Vehicle)
            FortVehicleMods::InitializeSpawnedVehicle(Vehicle);

        if (GApplyVehicleModOriginal)
            GApplyVehicleModOriginal(Context, Stack);
        else
            ConsumeApplyModParams(Stack);

        Vehicle = VehicleRef.Get();
        bool ConfigResolved = false;
        bool ReplacedExisting = false;
        bool AlreadyApplied = false;
        uint8 NativeConfigStage = 0;
        const bool NativeConfigApplied =
            Vehicle &&
            ModTagKnown &&
            ApplyNativeVehicleModConfig(
                Vehicle,
                ModTag,
                ConfigResolved,
                ReplacedExisting,
                AlreadyApplied,
                NativeConfigStage);
        if (Vehicle &&
            IsLiveVehicle(Vehicle, GVehicleModSchema))
        {
            UObject* CollectedData = nullptr;
            ReadCollectedVehicleModData(
                Vehicle,
                GVehicleModSchema,
                CollectedData);
            if (auto* State =
                    FindConstructionState(Vehicle, false))
            {
                State->Ready = CollectedData != nullptr;
            }

            if (Vehicle->HasRole() &&
                Vehicle->HasAuthority())
            {
                Vehicle->FlushNetDormancy();
                Vehicle->ForceNetUpdate();
            }

            if (GApplyResultLogCount++ < 16)
            {
                SDK::DbgLog(
                    "[VehicleMods] Apply entrypoint=vehicle "
                    "vehicle=%s(%p) tag-index=0x%08X "
                    "tag-known=%d "
                    "native-config=%d config-resolved=%d "
                    "stage=%u replaced=%d already-applied=%d "
                    "applied-tags=%d->%d collected-ready=%d\n",
                    Vehicle->Name.ToString().c_str(),
                    Vehicle,
                    static_cast<uint32>(ModTagIndex),
                    ModTagKnown,
                    NativeConfigApplied,
                    ConfigResolved,
                    NativeConfigStage,
                    ReplacedExisting,
                    AlreadyApplied,
                    AppliedBefore,
                    ReadAppliedVehicleModCount(
                        Vehicle,
                        GVehicleModSchema),
                    CollectedData != nullptr);
            }
        }
        else if (!Vehicle && GUnresolvedHookLogCount++ < 8)
        {
            SDK::DbgLog(
                "[VehicleMods] Apply vehicle owner unresolved "
                "context=%p stack-object=%p\n",
                Context,
                Stack.Object);
        }
    }

    void ApplyTireModHook(UObject* Context, FFrame& Stack)
    {
        AFortAthenaVehicle* Vehicle =
            GetVehicle(Context, Stack);
        TWeakObjectPtr<AFortAthenaVehicle> VehicleRef(Vehicle);
        FGameplayTag ModTag{};
        int32 ModTagIndex = 0;
        const bool ModTagKnown =
            DecodeApplyModTag(
                Stack,
                ModTag,
                ModTagIndex);
        const int32 AppliedBefore = Vehicle
            ? ReadAppliedVehicleModCount(
                Vehicle,
                GVehicleModSchema)
            : -1;
        if (Vehicle)
            FortVehicleMods::InitializeSpawnedVehicle(Vehicle);

        if (GApplyTireModOriginal)
            GApplyTireModOriginal(Context, Stack);
        else
            ConsumeApplyModParams(Stack);

        Vehicle = VehicleRef.Get();
        if (Vehicle &&
            IsLiveVehicle(Vehicle, GVehicleModSchema))
        {
            UObject* CollectedData = nullptr;
            ReadCollectedVehicleModData(
                Vehicle,
                GVehicleModSchema,
                CollectedData);
            if (auto* State =
                    FindConstructionState(Vehicle, false))
            {
                State->Ready = CollectedData != nullptr;
            }

            if (Vehicle->HasRole() &&
                Vehicle->HasAuthority())
            {
                Vehicle->FlushNetDormancy();
                Vehicle->ForceNetUpdate();
            }

            if (GApplyResultLogCount++ < 16)
            {
                SDK::DbgLog(
                    "[VehicleMods] Apply entrypoint=tires "
                    "vehicle=%s(%p) tag-index=0x%08X "
                    "tag-known=%d "
                    "applied-tags=%d->%d collected-ready=%d\n",
                    Vehicle->Name.ToString().c_str(),
                    Vehicle,
                    static_cast<uint32>(ModTagIndex),
                    ModTagKnown,
                    AppliedBefore,
                    ReadAppliedVehicleModCount(
                        Vehicle,
                        GVehicleModSchema),
                    CollectedData != nullptr);
            }
        }
        else if (!Vehicle && GUnresolvedHookLogCount++ < 8)
        {
            SDK::DbgLog(
                "[VehicleMods] Apply tire owner unresolved "
                "context=%p stack-object=%p\n",
                Context,
                Stack.Object);
        }
    }
}

AFortAthenaVehicle* FortVehicleMods::SpawnVehicleWithConstruction(
    const UObject* VehicleSpawner,
    const UClass* VehicleClass,
    const FTransform& Transform)
{
    if (!IsSeason30() ||
        !VehicleSpawner ||
        !VehicleClass)
    {
        return nullptr;
    }

    SyncConstructionWorld();
    auto& Schema = ResolveVehicleModSchema();
    if (!Schema.VehicleSpawnerClass ||
        !Schema.SpawnVehicleWithConstruction ||
        !SDK::MemReadable(VehicleSpawner, sizeof(UObject)) ||
        !VehicleSpawner->Class ||
        !SDK::MemReadable(VehicleSpawner->Class, 0x40) ||
        !VehicleSpawner->IsA(Schema.VehicleSpawnerClass))
    {
        if (GNativeSpawnLogCount++ < 4)
        {
            SDK::DbgLog(
                "[VehicleMods] Native vehicle spawn unavailable "
                "spawner=%p class=%p function=%p\n",
                VehicleSpawner,
                VehicleClass,
                Schema.SpawnVehicleWithConstruction);
        }
        return nullptr;
    }

    alignas(16) std::array<uint8, 0x80> Params{};
    auto* MutableVehicleClass =
        const_cast<UClass*>(VehicleClass);
    std::memcpy(
        Params.data(),
        &MutableVehicleClass,
        sizeof(MutableVehicleClass));
    std::memcpy(
        Params.data() + 0x10,
        &Transform,
        sizeof(Transform));

    VehicleSpawner->ProcessEvent(
        Schema.SpawnVehicleWithConstruction,
        Params.data());

    AFortAthenaVehicle* Vehicle = nullptr;
    std::memcpy(
        &Vehicle,
        Params.data() + 0x70,
        sizeof(Vehicle));
    if (!IsLiveVehicle(Vehicle, Schema) ||
        !Vehicle->IsA(VehicleClass))
    {
        if (GNativeSpawnLogCount++ < 4)
        {
            SDK::DbgLog(
                "[VehicleMods] Native vehicle spawn returned "
                "invalid vehicle=%p requested-class=%p\n",
                Vehicle,
                VehicleClass);
        }
        return nullptr;
    }

    if (GNativeSpawnLogCount++ < 4)
    {
        SDK::DbgLog(
            "[VehicleMods] Native vehicle spawn completed "
            "vehicle=%s(%p)\n",
            Vehicle->Name.ToString().c_str(),
            Vehicle);
    }
    return Vehicle;
}

bool FortVehicleMods::InitializeFiniteFuel(
    AFortAthenaVehicle* Vehicle)
{
    if (!IsSeason30() ||
        !IsUsableObject(Vehicle, false) ||
        !Vehicle->HasRole() ||
        !Vehicle->HasAuthority())
    {
        return false;
    }

    UFortVehicleFuelComponent* FuelComponent = nullptr;
    if (auto* GetFuelComponent =
            Vehicle->GetFunction("GetVehicleFuelComponent"))
    {
        FuelComponent =
            Vehicle->Call<UFortVehicleFuelComponent*>(
                GetFuelComponent);
    }

    // Raw-spawned blueprint vehicles can reach this before the vehicle's
    // transient CachedFuelComponent is populated. Resolve the owned component
    // directly in that case.
    auto* FuelComponentClass =
        FindClass("FortVehicleFuelComponent");
    if ((!IsUsableObject(FuelComponent, false) ||
            (FuelComponentClass &&
                !FuelComponent->IsA(FuelComponentClass))) &&
        FuelComponentClass)
    {
        FuelComponent =
            static_cast<UFortVehicleFuelComponent*>(
                Vehicle->GetComponentByClass(
                    FuelComponentClass));
    }
    if (!IsUsableObject(FuelComponent, false) ||
        !FuelComponentClass ||
        !FuelComponent->IsA(FuelComponentClass))
    {
        return false;
    }

    // Creative/server defaults can leave raw-spawned cars on the infinite-fuel
    // curve. Make the per-instance component finite without mutating its CDO.
    if (FuelComponent->HasUsesFuelSystem())
    {
        FuelComponent->UsesFuelSystem.Value = 1.f;
        FuelComponent->UsesFuelSystem.Curve = {};
    }
    if (FuelComponent->HasInfiniteFuel())
    {
        FuelComponent->InfiniteFuel.Value = 0.f;
        FuelComponent->InfiniteFuel.Curve = {};
    }

    if (auto* SetForceInfiniteFuel =
            Vehicle->GetFunction("SetForceInfiniteFuel"))
    {
        Vehicle->Call<void>(
            SetForceInfiniteFuel, false);
    }

    if (auto* Reinitialize =
            FuelComponent->GetFunction(
                "ReinitializeFuelUsageInfo"))
    {
        FuelComponent->Call<void>(Reinitialize);
    }

    FRuntimeFuelUsageInfo Usage{};
    if (auto* GetUsage =
            FuelComponent->GetFunction("GetFuelUsageInfo"))
    {
        Usage =
            FuelComponent->Call<FRuntimeFuelUsageInfo>(
                GetUsage);
    }

    auto ResolveConfiguredRate =
        [](FScalableFloat& Configured, float Fallback)
        {
            const float Value = Configured.Evaluate();
            return std::isfinite(Value) && Value > 0.f
                ? Value : Fallback;
        };
    bool UsageChanged = false;
    if (!std::isfinite(Usage.FuelPerSecondIdle) ||
        Usage.FuelPerSecondIdle < 0.f)
    {
        Usage.FuelPerSecondIdle = 0.05f;
        UsageChanged = true;
    }
    if (!std::isfinite(Usage.FuelPerSecondDriving) ||
        Usage.FuelPerSecondDriving <= 0.f)
    {
        Usage.FuelPerSecondDriving =
            FuelComponent->HasFuelPerSecondDriving()
            ? ResolveConfiguredRate(
                FuelComponent->FuelPerSecondDriving,
                0.35f)
            : 0.35f;
        UsageChanged = true;
    }
    if (!std::isfinite(Usage.FuelPerSecondBoosting) ||
        Usage.FuelPerSecondBoosting <= 0.f)
    {
        Usage.FuelPerSecondBoosting =
            FuelComponent->HasFuelPerSecondBoosting()
            ? ResolveConfiguredRate(
                FuelComponent->FuelPerSecondBoosting,
                0.75f)
            : 0.75f;
        UsageChanged = true;
    }
    if (UsageChanged)
    {
        if (auto* SetUsage =
                FuelComponent->GetFunction(
                    "SetFuelUsageInfo"))
        {
            FuelComponent->Call<void>(
                SetUsage, Usage);
        }
    }

    // Fuel consumption is component-tick driven. A raw spawn can leave this
    // component inactive even though its replicated fuel value is valid.
    if (auto* Activate =
            FuelComponent->GetFunction("Activate"))
    {
        FuelComponent->Call<void>(Activate, true);
    }
    if (auto* EnableTick =
            FuelComponent->GetFunction(
                "SetComponentTickEnabled"))
    {
        FuelComponent->Call<void>(EnableTick, true);
    }

    if (auto* State =
            FindConstructionState(Vehicle, false))
    {
        State->FuelComponent =
            TWeakObjectPtr<UFortVehicleFuelComponent>(
                FuelComponent);
        State->FuelSampleInitialized = false;
        State->NativeFuelConsumptionObserved = false;
        State->UnchangedMovingFuelSamples = 0;
    }

    Vehicle->FlushNetDormancy();
    Vehicle->ForceNetUpdate();
    if (GFuelLifecycleLogCount++ < 12)
    {
        const float Capacity =
            FuelComponent->GetFunction("GetFuelCapacity")
            ? FuelComponent->Call<float>(
                FuelComponent->GetFunction(
                    "GetFuelCapacity"))
            : 0.f;
        const bool Infinite =
            Vehicle->GetFunction("HasInfiniteFuel")
            ? Vehicle->Call<bool>(
                Vehicle->GetFunction(
                    "HasInfiniteFuel"))
            : false;
        SDK::DbgLog(
            "[VehicleFuel] initialized finite fuel "
            "vehicle=%s(%p) component=%p capacity=%.2f "
            "fuel=%.2f rates=%.3f/%.3f/%.3f infinite=%d\n",
            Vehicle->Name.ToString().c_str(),
            Vehicle,
            FuelComponent,
            Capacity,
            FuelComponent->HasServerFuel()
                ? FuelComponent->ServerFuel : -1.f,
            Usage.FuelPerSecondIdle,
            Usage.FuelPerSecondDriving,
            Usage.FuelPerSecondBoosting,
            Infinite);
    }
    return true;
}

void FortVehicleMods::RegisterSpawnedVehicle(
    AFortAthenaVehicle* Vehicle,
    const UObject* SpawnSource)
{
    if (!IsSeason30() || !Vehicle)
        return;

    SyncConstructionWorld();

    // Post-load normally installs these hooks. Registration is also a safe,
    // event-driven retry point if the FN30 interface streamed in later.
    InstallHooks();

    auto* State = FindConstructionState(Vehicle, true);
    if (!State)
        return;

    EnsureVehicleDamageablePartsInitialized(
        Vehicle,
        *State,
        GetTickCount64());
    State->FuelInitializationAttempted =
        InitializeFiniteFuel(Vehicle);

    if (SpawnSource &&
        SDK::MemReadable(SpawnSource, sizeof(UObject)) &&
        SpawnSource->Class)
    {
        State->SpawnSource = TWeakObjectPtr<UObject>(
            const_cast<UObject*>(SpawnSource));
        CaptureForcedMods(*State, SpawnSource);
    }

    auto& Schema = ResolveVehicleModSchema();
    if (!State->VehicleInterface &&
        Schema.VehicleInterfaceClass &&
        IsLiveVehicle(Vehicle, Schema))
    {
        State->VehicleInterface = Vehicle->GetInterface(
            Schema.VehicleInterfaceClass);
    }

    UObject* CollectedData = nullptr;
    if (Schema.CoreValid &&
        IsLiveVehicle(Vehicle, Schema) &&
        Vehicle->HasRole() &&
        Vehicle->HasAuthority() &&
        ReadCollectedVehicleModData(
            Vehicle, Schema, CollectedData) &&
        CollectedData)
    {
        State->Dispatched = true;
        State->Ready = true;
        State->Requested = false;
        State->Failed = false;
        return;
    }

    // Fortnite's normal vehicle lifecycle initializes mod state during its
    // construction path. Magnesium can still raw-spawn some vehicles, so
    // restore the missing phase gradually after world initialization instead
    // of synchronously constructing every vehicle in this spawn loop.
    if (!State->Dispatched &&
        !State->InProgress &&
        !State->Failed &&
        State->Attempts < MaxConstructionAttempts)
    {
        State->Requested = true;
        if (!GHasPendingConstructionRequest)
        {
            // Let streamed game-feature subsystems settle after the raw spawn
            // loop before beginning the amortized construction pass.
            GNextQueuedConstructionTimeMs =
                GetTickCount64() + 1000ULL;
        }
        GHasPendingConstructionRequest = true;
    }
}

bool FortVehicleMods::InitializeSpawnedVehicle(
    AFortAthenaVehicle* Vehicle,
    const UObject* SpawnSource)
{
    if (!IsSeason30() ||
        !Vehicle)
    {
        return false;
    }

    SyncConstructionWorld();

    // InstallHooks is deliberately retryable. Calling it from actual vehicle
    // activity recovers if the initial startup-time lookup ran too early.
    InstallHooks();

    auto& Schema = ResolveVehicleModSchema();
    if (!Schema.CoreValid)
    {
        LogConstructionSkip("schema-invalid", Vehicle);
        return false;
    }

    UObject* CollectedData = nullptr;
    if (ReadCollectedVehicleModData(
            Vehicle,
            Schema,
            CollectedData) &&
        CollectedData)
    {
        if (auto* State =
                FindConstructionState(Vehicle, false))
        {
            State->Dispatched = true;
            State->Ready = true;
            State->Requested = false;
            State->Failed = false;
        }
        return true;
    }

    auto* State = FindConstructionState(Vehicle, false);
    if (State &&
        (State->Dispatched || State->InProgress))
    {
        // Dispatched is intentionally distinct from Ready. Fortnite's
        // vehicle-mod subsystem may keep CollectedVehicleModData null while
        // the vehicle sits in VehiclesPendingModCollection.
        State->Ready = false;
        return true;
    }

    if (GConstructionCircuitOpen)
    {
        LogConstructionSkip("circuit-open", Vehicle);
        return false;
    }
    if (!IsLiveVehicle(Vehicle, Schema))
    {
        LogConstructionSkip("not-live", Vehicle);
        return false;
    }
    if (!Vehicle->HasRole())
    {
        LogConstructionSkip("role-unavailable", Vehicle);
        return false;
    }
    if (!Vehicle->HasAuthority())
    {
        LogConstructionSkip("not-authority", Vehicle);
        return false;
    }

    if (!State)
        State = FindConstructionState(Vehicle, true);
    if (!State)
        return false;

    if (Schema.VehicleInterfaceClass)
    {
        State->VehicleInterface = Vehicle->GetInterface(
            Schema.VehicleInterfaceClass);
    }

    if (State->Failed ||
        State->Attempts >= MaxConstructionAttempts)
    {
        return false;
    }

    if (SpawnSource &&
        SDK::MemReadable(SpawnSource, sizeof(UObject)) &&
        SpawnSource->Class)
    {
        State->SpawnSource = TWeakObjectPtr<UObject>(
            const_cast<UObject*>(SpawnSource));
        CaptureForcedMods(*State, SpawnSource);
    }

    State->InProgress = true;
    State->Requested = false;
    ++State->Attempts;

    alignas(16) std::array<uint8, 0x20> Params{};
    if (!State->HasForcedMods)
        CaptureForcedMods(*State, State->SpawnSource.Get());
    WriteForcedModsParams(*State, Params);

    const ULONGLONG ConstructionStartMs =
        GetTickCount64();
    // UE's generated Execute_ path resolves interface implementations by
    // name on the concrete UObject. Passing the interface-declared UFunction
    // directly bypasses that lookup and reaches FN30's no-op base thunk.
    UFunction* ConcreteTryConstruct =
        Vehicle->GetFunction("TryConstructWithModServer");
    const bool HasConcreteTryConstruct =
        ConcreteTryConstruct !=
            Schema.TryConstructWithModServer &&
        ValidateTryConstructFunction(ConcreteTryConstruct);
    if (HasConcreteTryConstruct)
    {
        Vehicle->ProcessEvent(
            ConcreteTryConstruct,
            Params.data());
    }

    // Native construction can synchronously trigger another vehicle's mod
    // event and grow the vector, so never retain an element pointer across
    // ProcessEvent.
    State = FindConstructionState(Vehicle, false);
    if (!State)
        return false;

    if (!IsLiveVehicle(Vehicle, Schema))
    {
        State->InProgress = false;
        State->Failed = true;
        return false;
    }
    if (!State->VehicleInterface &&
        Schema.VehicleInterfaceClass)
    {
        State->VehicleInterface = Vehicle->GetInterface(
            Schema.VehicleInterfaceClass);
    }

    const bool CollectionQueued =
        QueueNativeVehicleModCollection(
            Vehicle,
            *State,
            Schema);
    const ULONGLONG ConstructionDurationMs =
        GetTickCount64() - ConstructionStartMs;
    State->InProgress = false;
    if (!HasConcreteTryConstruct &&
        !CollectionQueued)
    {
        State->Failed = true;
        LogConstructionSkip(
            "concrete-and-collection-unavailable",
            Vehicle);
        return false;
    }

    CollectedData = nullptr;
    const bool CollectedReady =
        ReadCollectedVehicleModData(
            Vehicle,
            Schema,
            CollectedData) &&
        CollectedData;
    State->Dispatched = true;
    State->Ready = CollectedReady;
    State->Failed = false;

    // A single unexpectedly expensive native constructor is enough evidence
    // to stop dispatching new constructors for the rest of the match. Already
    // dispatched vehicles and all original interface calls remain functional.
    if (ConstructionDurationMs > 250ULL)
    {
        GConstructionCircuitOpen = true;
        GHasPendingConstructionRequest = false;
    }

    Vehicle->FlushNetDormancy();
    Vehicle->ForceNetUpdate();

    if (GConstructionPendingLogCount++ < 4)
    {
        SDK::DbgLog(
            "[VehicleMods] Native construction dispatched "
            "vehicle=%s concrete=%d collection-queued=%d "
            "function=%p interface=%p "
            "collected-ready=%d duration=%llums circuit=%d\n",
            Vehicle->Name.ToString().c_str(),
            HasConcreteTryConstruct,
            CollectionQueued,
            ConcreteTryConstruct,
            Schema.TryConstructWithModServer,
            CollectedReady,
            static_cast<unsigned long long>(
                ConstructionDurationMs),
            GConstructionCircuitOpen);
    }
    return true;
}

void FortVehicleMods::TickPendingConstruction()
{
    if (!IsSeason30())
        return;

    SyncConstructionWorld();
    const ULONGLONG CurrentTimeMs = GetTickCount64();

    // Raw-spawned cars are moved by Magnesium's compatibility ServerMove
    // path. On FN30 that can leave the native fuel component unable to observe
    // a driving input even though the car is moving. Sample once per second;
    // native consumption always wins, and this fallback starts only after
    // three moving samples with an unchanged fuel value.
    if (CurrentTimeMs >= GNextFuelMonitorTimeMs)
    {
        GNextFuelMonitorTimeMs =
            CurrentTimeMs + 1000ULL;
        for (auto& State : GVehicleModConstructionStates)
        {
            auto* Vehicle = State.Vehicle.Get();
            if (!Vehicle)
                continue;

            EnsureVehicleDamageablePartsInitialized(
                Vehicle,
                State,
                CurrentTimeMs);

            if (!State.FuelInitializationAttempted)
            {
                State.FuelInitializationAttempted =
                    InitializeFiniteFuel(Vehicle);
            }

            auto* FuelComponent =
                State.FuelComponent.Get();
            if (!IsUsableObject(FuelComponent, false) ||
                !FuelComponent->HasServerFuel())
            {
                State.FuelInitializationAttempted = false;
                State.FuelSampleInitialized = false;
                continue;
            }

            // Some creative rules reapply infinite fuel after vehicle spawn.
            // Keep this per-instance override finite without touching the
            // component CDO or any non-FN30 playlist state.
            if (FuelComponent->HasInfiniteFuel())
            {
                FuelComponent->InfiniteFuel.Value = 0.f;
                FuelComponent->InfiniteFuel.Curve = {};
            }
            auto* HasInfiniteFuel =
                Vehicle->GetFunction("HasInfiniteFuel");
            if (HasInfiniteFuel &&
                Vehicle->Call<bool>(HasInfiniteFuel))
            {
                if (auto* SetForceInfiniteFuel =
                        Vehicle->GetFunction(
                            "SetForceInfiniteFuel"))
                {
                    Vehicle->Call<void>(
                        SetForceInfiniteFuel, false);
                }
            }

            const float CurrentFuel =
                FuelComponent->ServerFuel;
            FVector CurrentLocation =
                Vehicle->K2_GetActorLocation();
            if (!std::isfinite(CurrentFuel) ||
                CurrentFuel < 0.f)
            {
                State.FuelSampleInitialized = false;
                continue;
            }

            if (!State.FuelSampleInitialized)
            {
                State.LastFuelSample = CurrentFuel;
                State.LastFuelSampleLocation =
                    CurrentLocation;
                State.LastFuelSampleTimeMs =
                    CurrentTimeMs;
                State.FuelSampleInitialized = true;
                continue;
            }

            const ULONGLONG ElapsedMs =
                CurrentTimeMs -
                State.LastFuelSampleTimeMs;
            const float ElapsedSeconds =
                std::clamp(
                    static_cast<float>(ElapsedMs) *
                        0.001f,
                    0.25f,
                    2.5f);
            const double DeltaX =
                CurrentLocation.X -
                State.LastFuelSampleLocation.X;
            const double DeltaY =
                CurrentLocation.Y -
                State.LastFuelSampleLocation.Y;
            const double DeltaZ =
                CurrentLocation.Z -
                State.LastFuelSampleLocation.Z;
            const double DistanceSquared =
                DeltaX * DeltaX +
                DeltaY * DeltaY +
                DeltaZ * DeltaZ;
            const bool Moving =
                DistanceSquared >= 10000.0;

            // Once native consumption is observed, never supplement it.
            if (CurrentFuel <
                State.LastFuelSample - 0.01f)
            {
                State.NativeFuelConsumptionObserved =
                    true;
                State.UnchangedMovingFuelSamples = 0;
            }
            else if (Moving &&
                CurrentFuel > 0.f &&
                !State.NativeFuelConsumptionObserved)
            {
                if (State.UnchangedMovingFuelSamples <
                    UINT8_MAX)
                {
                    ++State.UnchangedMovingFuelSamples;
                }

                if (State.UnchangedMovingFuelSamples >= 3)
                {
                    FRuntimeFuelUsageInfo Usage{};
                    if (auto* GetUsage =
                            FuelComponent->GetFunction(
                                "GetFuelUsageInfo"))
                    {
                        Usage =
                            FuelComponent->Call<
                                FRuntimeFuelUsageInfo>(
                                    GetUsage);
                    }
                    const float DrivingRate =
                        std::isfinite(
                            Usage.FuelPerSecondDriving) &&
                        Usage.FuelPerSecondDriving > 0.f
                        ? Usage.FuelPerSecondDriving
                        : 0.35f;
                    const float NewFuel =
                        (std::max)(
                            0.f,
                            CurrentFuel -
                                DrivingRate *
                                    ElapsedSeconds);
                    if (auto* SetFuel =
                            FuelComponent->GetFunction(
                                "SetFuel"))
                    {
                        FuelComponent->Call<void>(
                            SetFuel, NewFuel);
                        State.LastFuelSample = NewFuel;
                        Vehicle->FlushNetDormancy();
                        Vehicle->ForceNetUpdate();
                        if (!State.FuelFallbackLogged &&
                            GFuelFallbackLogCount++ < 12)
                        {
                            State.FuelFallbackLogged = true;
                            SDK::DbgLog(
                                "[VehicleFuel] compatibility drain "
                                "enabled vehicle=%s(%p) "
                                "rate=%.3f fuel=%.2f\n",
                                Vehicle->Name.ToString().c_str(),
                                Vehicle,
                                DrivingRate,
                                NewFuel);
                        }
                    }
                }
            }
            else
            {
                State.UnchangedMovingFuelSamples = 0;
            }

            if (State.NativeFuelConsumptionObserved ||
                State.UnchangedMovingFuelSamples < 3)
            {
                State.LastFuelSample = CurrentFuel;
            }
            State.LastFuelSampleLocation =
                CurrentLocation;
            State.LastFuelSampleTimeMs =
                CurrentTimeMs;
        }
    }

    if (GConstructionCircuitOpen ||
        !GHasPendingConstructionRequest)
    {
        return;
    }

    if (CurrentTimeMs < GNextQueuedConstructionTimeMs)
        return;

    if (!ResolveVehicleModSchema().CoreValid)
    {
        GNextQueuedConstructionTimeMs =
            CurrentTimeMs + 1000ULL;
        return;
    }

    for (size_t Index = 0;
        Index < GVehicleModConstructionStates.size();)
    {
        auto Vehicle =
            GVehicleModConstructionStates[Index].Vehicle.Get();
        if (!Vehicle)
        {
            GVehicleModConstructionStates.erase(
                GVehicleModConstructionStates.begin() + Index);
            continue;
        }

        if (!GVehicleModConstructionStates[Index].Requested)
        {
            ++Index;
            continue;
        }

        GVehicleModConstructionStates[Index].Requested = false;
        GHasPendingConstructionRequest = false;
        for (size_t PendingIndex = Index + 1;
            PendingIndex < GVehicleModConstructionStates.size();
            ++PendingIndex)
        {
            if (GVehicleModConstructionStates[PendingIndex].Requested &&
                GVehicleModConstructionStates[PendingIndex].Vehicle.Get())
            {
                GHasPendingConstructionRequest = true;
                break;
            }
        }
        GNextQueuedConstructionTimeMs =
            CurrentTimeMs + 100ULL;
        InitializeSpawnedVehicle(Vehicle);
        return;
    }

    GHasPendingConstructionRequest = false;
}

void FortVehicleMods::InstallHooks()
{
    if (!IsSeason30() ||
        GHookInstallInProgress ||
        (GCanApplyModHookInstalled &&
            GApplyVehicleModHookInstalled &&
            GApplyTireModHookInstalled))
    {
        return;
    }

    const ULONGLONG CurrentTimeMs = GetTickCount64();
    if (CurrentTimeMs < GNextHookInstallAttemptTimeMs)
        return;
    GNextHookInstallAttemptTimeMs =
        CurrentTimeMs + 2000ULL;

    GHookInstallInProgress = true;
    auto& Schema = ResolveVehicleModSchema();
    if (!Schema.CoreValid)
    {
        GHookInstallInProgress = false;
        return;
    }

    if (Schema.CanApplyMod &&
        !GCanApplyModHookInstalled)
    {
        Utils::ExecHook(
            Schema.CanApplyMod,
            CanApplyModHook,
            GCanApplyModOriginal);
        GCanApplyModHookInstalled =
            GCanApplyModOriginal != nullptr;
    }
    if (Schema.ApplyVehicleMod &&
        !GApplyVehicleModHookInstalled)
    {
        Utils::ExecHook(
            Schema.ApplyVehicleMod,
            ApplyVehicleModHook,
            GApplyVehicleModOriginal);
        GApplyVehicleModHookInstalled =
            GApplyVehicleModOriginal != nullptr;
    }
    if (Schema.ApplyTireMod &&
        !GApplyTireModHookInstalled)
    {
        Utils::ExecHook(
            Schema.ApplyTireMod,
            ApplyTireModHook,
            GApplyTireModOriginal);
        GApplyTireModHookInstalled =
            GApplyTireModOriginal != nullptr;
    }

    GHookInstallInProgress = false;
    const uint8 HookStatus =
        (GCanApplyModHookInstalled ? 1 : 0) |
        (GApplyVehicleModHookInstalled ? 2 : 0) |
        (GApplyTireModHookInstalled ? 4 : 0);
    if (HookStatus != GLastHookInstallStatus)
    {
        GLastHookInstallStatus = HookStatus;
        SDK::DbgLog(
            "[VehicleMods] FN30 event hooks can-apply=%s apply=%s tire=%s\n",
            GCanApplyModHookInstalled ? "installed" : "pending",
            GApplyVehicleModHookInstalled ? "installed" : "pending",
            GApplyTireModHookInstalled ? "installed" : "pending");
    }
    if (HookStatus == 0x7)
        GNextHookInstallAttemptTimeMs = 0;
}
