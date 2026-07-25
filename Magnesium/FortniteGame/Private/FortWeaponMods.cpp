#include "pch.h"
#include "../Public/FortWeaponMods.h"
#include "../Public/FortInventory.h"
#include "../Public/FortPlayerControllerAthena.h"
#include "../Public/FortPlayerPawnAthena.h"
#include "../Public/FortWeapon.h"

namespace
{
    constexpr int32 MaxWeaponModSlots = 16;

    struct FWeaponModSlotLayout
    {
        int32 Size = 0;
        uint32 WeaponModOffset = uint32(-1);
        uint32 DynamicOffset = uint32(-1);
    };

    struct FEquippedWeaponModSlotLayout
    {
        int32 Size = 0;
        uint32 EquippedWeaponModOffset = uint32(-1);
    };

    FWeaponModSlotLayout SlotLayout;
    FEquippedWeaponModSlotLayout EquippedSlotLayout;
    ULONGLONG NextSlotLayoutResolveTimeMs = 0;
    ULONGLONG NextEquippedSlotLayoutResolveTimeMs = 0;
    uint32 WeaponSlotsOffset = uint32(-1);
    uint32 EquippedWeaponSlotsOffset = uint32(-1);

    struct FWeaponModNativeApi
    {
        bool bAttempted = false;
        ULONGLONG NextAttemptTimeMs = 0;
        uint8 Attempts = 0;
        const UObject* Library = nullptr;
        UFunction* GetRandomWeaponModSetData = nullptr;
        UFunction* ApplyModSetToPickup = nullptr;
        UFunction* AssignPlayerNameToWeapon = nullptr;
        UFunction* AssignPlayerNameToItemEntry = nullptr;
    };

    FWeaponModNativeApi NativeApi;

    struct FPushModelApi
    {
        const UObject* Helpers = nullptr;
        UFunction* MarkPropertyDirty = nullptr;
        ULONGLONG NextResolveTimeMs = 0;
        uint32 ResolveLogCount = 0;
    };

    FPushModelApi PushModelApi;

    struct FWeaponModRpcLayout
    {
        UFunction* Function = nullptr;
        uint32 ParamsSize = 0;
        uint32 WeaponModOffset = uint32(-1);
        uint32 WeaponOffset = uint32(-1);
    };

    FWeaponModRpcLayout PurchaseRpcLayout;
    FWeaponModRpcLayout RemoveRpcLayout;
    bool bPurchaseHookInstalled = false;
    bool bRemoveHookInstalled = false;
    ULONGLONG NextBenchResolveTimeMs = 0;
    uint32 BenchResolveLogCount = 0;
    uint32 BenchRejectLogCount = 0;
    uint32 BenchUnchangedLogCount = 0;
    uint32 BenchSyncFailureLogCount = 0;

    struct FExpectedParam
    {
        const char* Name;
        uint32 Size;
    };

    bool ValidateFunctionSchema(
        UFunction* Function,
        std::initializer_list<FExpectedParam> Inputs,
        uint32 ReturnSize = 0)
    {
        if (!Function)
            return false;

        const auto Params = Function->GetParamsNamed();
        if (Params.Size == 0 || Params.Size > 0x4000)
            return false;

        size_t InputIndex = 0;
        bool bFoundReturn = ReturnSize == 0;
        for (const auto& Param : Params.NameOffsetMap)
        {
            if (Param.Name == "ReturnValue")
            {
                if (bFoundReturn || Param.ElementSize != ReturnSize ||
                    Param.Offset + ReturnSize > Params.Size)
                {
                    return false;
                }

                bFoundReturn = true;
                continue;
            }

            if (InputIndex >= Inputs.size())
                return false;

            const auto& Expected = *(Inputs.begin() + InputIndex);
            if (Param.Name != Expected.Name ||
                Param.ElementSize != Expected.Size ||
                Param.Offset + Expected.Size > Params.Size)
            {
                return false;
            }

            ++InputIndex;
        }

        return InputIndex == Inputs.size() && bFoundReturn;
    }

    UFunction* ResolveValidatedFunction(
        const UObject* Object,
        const char* FunctionName,
        std::initializer_list<FExpectedParam> Inputs,
        uint32 ReturnSize = 0)
    {
        auto Function = Object
            ? Object->GetFunction(FunctionName)
            : nullptr;
        return ValidateFunctionSchema(Function, Inputs, ReturnSize)
            ? Function
            : nullptr;
    }

    bool ResolvePushModelApi()
    {
        if (PushModelApi.Helpers &&
            PushModelApi.MarkPropertyDirty)
        {
            return true;
        }

        const ULONGLONG CurrentTimeMs = GetTickCount64();
        if (CurrentTimeMs < PushModelApi.NextResolveTimeMs)
            return false;
        PushModelApi.NextResolveTimeMs = CurrentTimeMs + 5000ULL;

        auto HelpersClass = FindObject<UClass>(
            L"/Script/Engine.NetPushModelHelpers");
        if (!HelpersClass)
            HelpersClass = SDK::FindClass("NetPushModelHelpers");

        const UObject* Helpers = HelpersClass
            ? HelpersClass->GetDefaultObj()
            : nullptr;
        UFunction* MarkPropertyDirty = ResolveValidatedFunction(
            Helpers,
            "MarkPropertyDirty",
            {
                { "Object", uint32(sizeof(UObject*)) },
                // Modern cooked FNameProperty parameters serialize only the
                // live comparison index (4 bytes), even though this SDK's
                // convenience FName wrapper also stores a Number field.
                { "PropertyName", uint32(sizeof(int32)) }
            });
        if (!Helpers || !MarkPropertyDirty)
        {
            PushModelApi.Helpers = nullptr;
            PushModelApi.MarkPropertyDirty = nullptr;
            if (PushModelApi.ResolveLogCount++ < 4)
            {
                SDK::DbgLog(
                    "[WeaponMods] NetPushModelHelpers.MarkPropertyDirty unavailable or has an unexpected schema\n");
            }
            return false;
        }

        PushModelApi.Helpers = Helpers;
        PushModelApi.MarkPropertyDirty = MarkPropertyDirty;
        PushModelApi.NextResolveTimeMs = 0;
        return true;
    }

    bool MarkWeaponModSlotsDirty(AFortWeapon* Weapon)
    {
        if (!Weapon || !ResolvePushModelApi())
            return false;

        FName PropertyName(L"WeaponModSlots");
        PushModelApi.Helpers->Call<void>(
            PushModelApi.MarkPropertyDirty,
            static_cast<UObject*>(Weapon),
            PropertyName);
        return true;
    }

    bool ResolveNativeApi()
    {
        if (NativeApi.Library)
            return true;

        const ULONGLONG CurrentTimeMs = GetTickCount64();
        if (NativeApi.bAttempted &&
            CurrentTimeMs < NativeApi.NextAttemptTimeMs)
        {
            return false;
        }

        NativeApi.bAttempted = true;
        NativeApi.Attempts++;
        NativeApi.Library =
            SDK::DefaultObjImpl("FortWeaponModFunctionLibrary");
        if (!NativeApi.Library)
        {
            const ULONGLONG RetryDelayMs =
                (std::min)(
                    30000ULL,
                    2000ULL *
                        static_cast<ULONGLONG>(
                            NativeApi.Attempts));
            NativeApi.NextAttemptTimeMs =
                CurrentTimeMs + RetryDelayMs;
            return false;
        }
        NativeApi.NextAttemptTimeMs = 0;

        const uint32 PointerSize = uint32(sizeof(UObject*));
        NativeApi.GetRandomWeaponModSetData = ResolveValidatedFunction(
            NativeApi.Library,
            "GetRandomWeaponModSetData",
            {
                { "WorldContext", PointerSize },
                { "WeaponItemDefinition", PointerSize }
            },
            PointerSize);
        NativeApi.ApplyModSetToPickup = ResolveValidatedFunction(
            NativeApi.Library,
            "ApplyModSetToPickup",
            {
                { "ModSet", PointerSize },
                { "WeaponPickup", PointerSize }
            });
        NativeApi.AssignPlayerNameToWeapon = ResolveValidatedFunction(
            NativeApi.Library,
            "AssignPlayerNameToWeapon",
            {
                { "PlayerState", PointerSize },
                { "Weapon", PointerSize }
            });
        NativeApi.AssignPlayerNameToItemEntry = ResolveValidatedFunction(
            NativeApi.Library,
            "AssignPlayerNameToItemEntry",
            {
                { "PlayerState", PointerSize },
                { "ItemEntry", uint32(FFortItemEntry::Size()) }
            });

        SDK::DbgLog(
            "[WeaponMods] Native helper API FN %.2f: random=%d apply=%d nameWeapon=%d nameEntry=%d\n",
            VersionInfo.FortniteVersion,
            NativeApi.GetRandomWeaponModSetData != nullptr,
            NativeApi.ApplyModSetToPickup != nullptr,
            NativeApi.AssignPlayerNameToWeapon != nullptr,
            NativeApi.AssignPlayerNameToItemEntry != nullptr);

        return true;
    }

    bool ResolveSlotLayout()
    {
        if (SlotLayout.Size > 0)
            return true;

        const ULONGLONG CurrentTimeMs = GetTickCount64();
        if (CurrentTimeMs < NextSlotLayoutResolveTimeMs)
            return false;
        NextSlotLayoutResolveTimeMs =
            CurrentTimeMs + 5000ULL;

        const auto SlotStruct = SDK::FindStruct("FortWeaponModSlot");
        if (!SlotStruct)
            return false;

        const int32 Size = SlotStruct->GetPropertiesSize();
        const uint32 WeaponModOffset = SlotStruct->GetOffset("WeaponMod");
        const uint32 DynamicOffset = SlotStruct->GetOffset("bIsDynamic");
        if (Size < 9 || Size > 0x40 ||
            WeaponModOffset == uint32(-1) || WeaponModOffset + sizeof(UObject*) > uint32(Size) ||
            DynamicOffset == uint32(-1) || DynamicOffset >= uint32(Size))
        {
            SDK::DbgLog(
                "[WeaponMods] Invalid FortWeaponModSlot layout: Size=0x%X Mod=0x%X Dynamic=0x%X\n",
                Size, WeaponModOffset, DynamicOffset);
            return false;
        }

        SlotLayout.Size = Size;
        SlotLayout.WeaponModOffset = WeaponModOffset;
        SlotLayout.DynamicOffset = DynamicOffset;
        NextSlotLayoutResolveTimeMs = 0;
        return true;
    }

    bool ResolveEquippedSlotLayout()
    {
        if (EquippedSlotLayout.Size > 0)
            return true;

        const ULONGLONG CurrentTimeMs = GetTickCount64();
        if (CurrentTimeMs <
            NextEquippedSlotLayoutResolveTimeMs)
        {
            return false;
        }
        NextEquippedSlotLayoutResolveTimeMs =
            CurrentTimeMs + 5000ULL;

        const auto SlotStruct = SDK::FindStruct(
            "FortEquippedWeaponModSlot");
        if (!SlotStruct)
            return false;

        const int32 Size = SlotStruct->GetPropertiesSize();
        const uint32 EquippedWeaponModOffset =
            SlotStruct->GetOffset("EquippedWeaponMod");
        if (Size < int32(sizeof(UObject*)) || Size > 0x100 ||
            EquippedWeaponModOffset == uint32(-1) ||
            EquippedWeaponModOffset + sizeof(UObject*) >
                uint32(Size))
        {
            SDK::DbgLog(
                "[WeaponMods] Invalid FortEquippedWeaponModSlot layout: Size=0x%X Mod=0x%X\n",
                Size,
                EquippedWeaponModOffset);
            return false;
        }

        EquippedSlotLayout.Size = Size;
        EquippedSlotLayout.EquippedWeaponModOffset =
            EquippedWeaponModOffset;
        NextEquippedSlotLayoutResolveTimeMs = 0;
        return true;
    }

    bool IsArrayHeaderSane(const TArray<void*>& Slots, bool bRequireReadableElements)
    {
        if (Slots.Num() < 0 || Slots.Num() > MaxWeaponModSlots ||
            Slots.Max() < Slots.Num() || Slots.Max() > 64)
        {
            return false;
        }

        if (Slots.Num() == 0)
            return true;

        if (!Slots.Data || !ResolveSlotLayout())
            return false;

        return !bRequireReadableElements ||
            SDK::MemReadable(Slots.Data, size_t(SlotLayout.Size) * size_t(Slots.Num()));
    }

    const TArray<void*>* GetDefinitionSlots(const UFortItemDefinition* ItemDefinition)
    {
        auto WeaponDefinition = ItemDefinition
            ? ItemDefinition->Cast<UFortWeaponItemDefinition>()
            : nullptr;
        if (!WeaponDefinition || !WeaponDefinition->HasWeaponModSlots())
            return nullptr;

        return &WeaponDefinition->WeaponModSlots;
    }

    const void* GetDefinitionSlotData(const UFortItemDefinition* ItemDefinition)
    {
        const auto Slots = GetDefinitionSlots(ItemDefinition);
        return Slots ? Slots->Data : nullptr;
    }

    bool DeepAssignSlots(
        const TArray<void*>& Source,
        TArray<void*>& Destination,
        const void* ProtectedDataA = nullptr,
        const void* ProtectedDataB = nullptr,
        const void* ProtectedDataC = nullptr)
    {
        const int32 Count = Source.Num();
        if (Count < 0 ||
            Count > MaxWeaponModSlots ||
            Source.Max() < Count ||
            Source.Max() > 64)
        {
            return false;
        }

        // Empty attachment arrays are overwhelmingly ammo, resources,
        // consumables, and older weapons. Avoid a global reflected-struct
        // lookup for the common zero-to-zero copy.
        if (Count == 0 && Destination.Num() == 0)
            return true;

        if (!ResolveSlotLayout() || !IsArrayHeaderSane(Source, true))
            return false;

        void* NewData = nullptr;
        if (Count > 0)
        {
            const size_t Bytes = size_t(SlotLayout.Size) * size_t(Count);
            NewData = FMemory::Malloc(Bytes);
            if (!NewData)
                return false;

            memcpy(NewData, Source.Data, Bytes);
        }

        // Array assignment in the local SDK only copies Data/Num/Max. Release
        // an old buffer only when it is demonstrably independent; definition
        // and sibling-entry aliases must be detached without freeing.
        void* OldData = Destination.Data;
        const bool bCanReleaseOld =
            OldData &&
            OldData != Source.Data &&
            OldData != ProtectedDataA &&
            OldData != ProtectedDataB &&
            OldData != ProtectedDataC &&
            IsArrayHeaderSane(Destination, false);

        Destination.Data = reinterpret_cast<void**>(NewData);
        Destination.NumElements = Count;
        Destination.MaxElements = Count;

        if (bCanReleaseOld)
            FMemory::Free(OldData);

        return true;
    }

    UObject* GetSlotMod(const TArray<void*>& Slots, int32 Index)
    {
        if (!ResolveSlotLayout() || !IsArrayHeaderSane(Slots, true) ||
            Index < 0 || Index >= Slots.Num())
        {
            return nullptr;
        }

        UObject* Result = nullptr;
        const uint8* Slot = reinterpret_cast<const uint8*>(Slots.Data) +
            size_t(Index) * size_t(SlotLayout.Size);
        memcpy(&Result, Slot + SlotLayout.WeaponModOffset, sizeof(Result));
        return Result;
    }

    bool IsSlotDynamic(const TArray<void*>& Slots, int32 Index)
    {
        if (!ResolveSlotLayout() || !IsArrayHeaderSane(Slots, true) ||
            Index < 0 || Index >= Slots.Num())
        {
            return false;
        }

        const uint8* Slot = reinterpret_cast<const uint8*>(Slots.Data) +
            size_t(Index) * size_t(SlotLayout.Size);
        return *(Slot + SlotLayout.DynamicOffset) != 0;
    }

    bool HasDynamicSlots(const TArray<void*>& Slots)
    {
        if (!IsArrayHeaderSane(Slots, true))
            return false;

        for (int32 Index = 0; Index < Slots.Num(); ++Index)
            if (IsSlotDynamic(Slots, Index))
                return true;

        return false;
    }

    bool SlotsEqual(const TArray<void*>& Left, const TArray<void*>& Right)
    {
        if (!ResolveSlotLayout() ||
            !IsArrayHeaderSane(Left, true) ||
            !IsArrayHeaderSane(Right, true) ||
            Left.Num() != Right.Num())
        {
            return false;
        }

        for (int32 Index = 0; Index < Left.Num(); ++Index)
        {
            const uint8* LeftSlot =
                reinterpret_cast<const uint8*>(Left.Data) +
                size_t(Index) * size_t(SlotLayout.Size);
            const uint8* RightSlot =
                reinterpret_cast<const uint8*>(Right.Data) +
                size_t(Index) * size_t(SlotLayout.Size);
            UObject* LeftMod = nullptr;
            UObject* RightMod = nullptr;
            memcpy(
                &LeftMod,
                LeftSlot + SlotLayout.WeaponModOffset,
                sizeof(LeftMod));
            memcpy(
                &RightMod,
                RightSlot + SlotLayout.WeaponModOffset,
                sizeof(RightMod));
            if (LeftMod != RightMod ||
                *(LeftSlot + SlotLayout.DynamicOffset) !=
                    *(RightSlot + SlotLayout.DynamicOffset))
            {
                return false;
            }
        }

        return true;
    }

    TArray<void*>* GetWeaponSlots(AFortWeapon* Weapon)
    {
        if (!Weapon)
            return nullptr;

        if (WeaponSlotsOffset == uint32(-1))
            WeaponSlotsOffset = Weapon->GetOffset("WeaponModSlots");
        if (WeaponSlotsOffset == uint32(-1))
            return nullptr;

        return reinterpret_cast<TArray<void*>*>(
            reinterpret_cast<uint8*>(Weapon) + WeaponSlotsOffset);
    }

    TArray<void*>* GetEquippedWeaponSlots(AFortWeapon* Weapon)
    {
        if (!Weapon)
            return nullptr;

        if (EquippedWeaponSlotsOffset == uint32(-1))
        {
            EquippedWeaponSlotsOffset =
                Weapon->GetOffset("EquippedWeaponModSlots");
        }
        if (EquippedWeaponSlotsOffset == uint32(-1))
            return nullptr;

        return reinterpret_cast<TArray<void*>*>(
            reinterpret_cast<uint8*>(Weapon) +
            EquippedWeaponSlotsOffset);
    }

    bool IsEquippedArrayHeaderSane(
        const TArray<void*>& EquippedSlots)
    {
        if (!ResolveEquippedSlotLayout() ||
            EquippedSlots.Num() < 0 ||
            EquippedSlots.Num() > MaxWeaponModSlots ||
            EquippedSlots.Max() < EquippedSlots.Num() ||
            EquippedSlots.Max() > 64 ||
            (EquippedSlots.Num() > 0 &&
                (!EquippedSlots.Data ||
                    !SDK::MemReadable(
                        EquippedSlots.Data,
                        size_t(EquippedSlotLayout.Size) *
                            size_t(EquippedSlots.Num())))))
        {
            return false;
        }

        return true;
    }

    UObject* GetEquippedSlotMod(
        const TArray<void*>& EquippedSlots,
        int32 Index)
    {
        if (!IsEquippedArrayHeaderSane(EquippedSlots) ||
            Index < 0 || Index >= EquippedSlots.Num())
        {
            return nullptr;
        }

        UObject* EquippedMod = nullptr;
        const uint8* EquippedSlot =
            reinterpret_cast<const uint8*>(EquippedSlots.Data) +
            size_t(Index) *
                size_t(EquippedSlotLayout.Size);
        memcpy(
            &EquippedMod,
            EquippedSlot +
                EquippedSlotLayout.EquippedWeaponModOffset,
            sizeof(EquippedMod));
        return EquippedMod;
    }

    bool ContainsEquippedMod(
        const TArray<void*>& EquippedSlots,
        UObject* WeaponMod)
    {
        if (!WeaponMod)
            return false;

        for (int32 Index = 0;
            Index < EquippedSlots.Num();
            ++Index)
        {
            if (GetEquippedSlotMod(EquippedSlots, Index) ==
                WeaponMod)
            {
                return true;
            }
        }

        return false;
    }

    bool EquippedSlotsMatch(
        const TArray<void*>& EquippedSlots,
        const TArray<void*>& ReplicatedSlots)
    {
        if (!IsEquippedArrayHeaderSane(EquippedSlots) ||
            !IsArrayHeaderSane(ReplicatedSlots, true))
        {
            return false;
        }

        int32 DesiredCount = 0;
        for (int32 Index = 0;
            Index < ReplicatedSlots.Num();
            ++Index)
        {
            if (GetSlotMod(ReplicatedSlots, Index))
                ++DesiredCount;
        }
        if (DesiredCount != EquippedSlots.Num())
            return false;

        for (int32 DesiredIndex = 0;
            DesiredIndex < ReplicatedSlots.Num();
            ++DesiredIndex)
        {
            auto DesiredMod =
                GetSlotMod(ReplicatedSlots, DesiredIndex);
            if (!DesiredMod)
                continue;

            bool bFound = false;
            bFound = ContainsEquippedMod(
                EquippedSlots, DesiredMod);

            if (!bFound)
                return false;
        }

        return true;
    }

    bool EquippedSlotsAreSubsetOf(
        const TArray<void*>& EquippedSlots,
        const TArray<void*>& ReplicatedSlots)
    {
        if (!IsEquippedArrayHeaderSane(EquippedSlots) ||
            !IsArrayHeaderSane(ReplicatedSlots, true))
        {
            return false;
        }

        for (int32 EquippedIndex = 0;
            EquippedIndex < EquippedSlots.Num();
            ++EquippedIndex)
        {
            auto EquippedMod =
                GetEquippedSlotMod(EquippedSlots, EquippedIndex);
            if (!EquippedMod)
                return false;

            bool bFound = false;
            for (int32 ReplicatedIndex = 0;
                ReplicatedIndex < ReplicatedSlots.Num();
                ++ReplicatedIndex)
            {
                if (GetSlotMod(
                    ReplicatedSlots, ReplicatedIndex) ==
                    EquippedMod)
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

    bool GuidEquals(const FGuid& Left, const FGuid& Right)
    {
        return Left.A == Right.A && Left.B == Right.B &&
            Left.C == Right.C && Left.D == Right.D;
    }

    FFortItemEntry* FindReplicatedEntry(
        AFortInventory* Inventory,
        const FGuid& Guid)
    {
        if (!Inventory)
            return nullptr;

        return Inventory->Inventory.ReplicatedEntries.Search(
            [&](FFortItemEntry& Entry)
            {
                return GuidEquals(Entry.ItemGuid, Guid);
            },
            FFortItemEntry::Size());
    }

    UFortWorldItem* FindItemInstance(
        AFortInventory* Inventory,
        const FGuid& Guid)
    {
        if (!Inventory)
            return nullptr;

        auto Item = Inventory->Inventory.ItemInstances.Search(
            [&](UFortWorldItem* Candidate)
            {
                return Candidate && GuidEquals(Candidate->ItemEntry.ItemGuid, Guid);
            });
        return Item ? *Item : nullptr;
    }

    bool IsOwnedInventoryWeapon(
        AFortPlayerControllerAthena* PlayerController,
        AFortWeapon* Weapon)
    {
        if (!PlayerController || !Weapon || !PlayerController->WorldInventory ||
            !PlayerController->MyFortPawn ||
            Weapon->Instigator != PlayerController->MyFortPawn)
        {
            return false;
        }

        return FindReplicatedEntry(
            PlayerController->WorldInventory,
            Weapon->ItemEntryGuid) != nullptr;
    }

    const UObject* GetWeaponModLibrary()
    {
        return ResolveNativeApi() ? NativeApi.Library : nullptr;
    }

    void AssignPlayerNameToModifiedWeapon(
        AFortPlayerControllerAthena* PlayerController,
        AFortWeapon* Weapon,
        FFortItemEntry& Entry)
    {
        const auto Library = GetWeaponModLibrary();
        auto PlayerState = PlayerController
            ? PlayerController->PlayerState
            : nullptr;
        if (!Library || !PlayerState)
            return;

        if (NativeApi.AssignPlayerNameToWeapon)
        {
            Library->Call<void>(
                NativeApi.AssignPlayerNameToWeapon,
                PlayerState,
                Weapon);
        }

        auto AssignEntry = NativeApi.AssignPlayerNameToItemEntry;
        if (!AssignEntry)
            return;

        const auto Params = AssignEntry->GetParamsNamed();
        uint32 PlayerStateOffset = uint32(-1);
        uint32 EntryOffset = uint32(-1);
        uint32 EntryElementSize = 0;
        for (const auto& Param : Params.NameOffsetMap)
        {
            if (Param.Name == "PlayerState")
                PlayerStateOffset = Param.Offset;
            else if (Param.Name == "ItemEntry")
            {
                EntryOffset = Param.Offset;
                EntryElementSize = Param.ElementSize;
            }
        }

        const uint32 EntrySize = uint32(FFortItemEntry::Size());
        if (Params.Size == 0 || Params.Size > 0x4000 ||
            PlayerStateOffset == uint32(-1) ||
            PlayerStateOffset + sizeof(UObject*) > Params.Size ||
            EntryOffset == uint32(-1) ||
            EntryElementSize != EntrySize ||
            EntryOffset + EntrySize > Params.Size)
        {
            return;
        }

        auto Memory = FMemory::Malloc(Params.Size);
        if (!Memory)
            return;

        memset(Memory, 0, Params.Size);
        memcpy(
            reinterpret_cast<uint8*>(Memory) + PlayerStateOffset,
            &PlayerState,
            sizeof(PlayerState));
        memcpy(
            reinterpret_cast<uint8*>(Memory) + EntryOffset,
            &Entry,
            EntrySize);

        Library->ProcessEvent(AssignEntry, Memory);

        // The native function mutates the reflected by-reference entry stored
        // in the params buffer. Copy its headers back before releasing only the
        // outer params allocation; nested Unreal arrays remain engine-owned.
        memcpy(
            &Entry,
            reinterpret_cast<uint8*>(Memory) + EntryOffset,
            EntrySize);
        FMemory::Free(Memory);
    }

    bool ResolvePurchaseRpcLayout(
        UFunction* Function,
        FWeaponModRpcLayout& OutLayout)
    {
        OutLayout = {};
        if (!ValidateFunctionSchema(
            Function,
            {
                { "WeaponMod", uint32(sizeof(UObject*)) },
                { "Weapon", uint32(sizeof(UObject*)) }
            }))
        {
            return false;
        }

        const auto Params = Function->GetParamsNamed();
        uint32 WeaponModOffset = uint32(-1);
        uint32 WeaponOffset = uint32(-1);
        for (const auto& Param : Params.NameOffsetMap)
        {
            if (Param.Name == "WeaponMod")
                WeaponModOffset = Param.Offset;
            else if (Param.Name == "Weapon")
                WeaponOffset = Param.Offset;
        }

        if (Params.Size == 0 || Params.Size > 0x100 ||
            WeaponModOffset == uint32(-1) ||
            WeaponOffset == uint32(-1) ||
            WeaponModOffset + sizeof(UObject*) > Params.Size ||
            WeaponOffset + sizeof(AFortWeapon*) > Params.Size)
        {
            return false;
        }

        OutLayout.Function = Function;
        OutLayout.ParamsSize = Params.Size;
        OutLayout.WeaponModOffset = WeaponModOffset;
        OutLayout.WeaponOffset = WeaponOffset;
        return true;
    }

    bool DecodePurchaseRpc(
        const FWeaponModRpcLayout& Layout,
        const FFrame& Stack,
        UObject*& OutWeaponMod,
        AFortWeapon*& OutWeapon)
    {
        OutWeaponMod = nullptr;
        OutWeapon = nullptr;

        // These server RPCs arrive through ProcessEvent with a compiled-in
        // parameter frame. Reading Locals by the validated reflected offsets
        // leaves the original FFrame entirely untouched for Epic's native exec.
        if (!Layout.Function || Stack.Code || !Stack.Locals ||
            !SDK::MemReadable(Stack.Locals, Layout.ParamsSize))
        {
            return false;
        }

        memcpy(
            &OutWeaponMod,
            Stack.Locals + Layout.WeaponModOffset,
            sizeof(OutWeaponMod));
        memcpy(
            &OutWeapon,
            Stack.Locals + Layout.WeaponOffset,
            sizeof(OutWeapon));
        return OutWeaponMod && OutWeapon;
    }

    AFortPlayerControllerAthena* GetComponentOwnerController(UObject* Context)
    {
        auto Component = Context ? Context->Cast<UActorComponent>() : nullptr;
        auto Owner = Component ? Component->GetOwner() : nullptr;
        return Owner ? Owner->Cast<AFortPlayerControllerAthena>() : nullptr;
    }

    bool ValidateModRequest(
        UObject* Context,
        UObject* WeaponMod,
        AFortWeapon* Weapon,
        AFortPlayerControllerAthena*& OutPlayerController)
    {
        OutPlayerController = GetComponentOwnerController(Context);
        const auto ModClass = SDK::FindClass("FortWeaponModItemDefinition");
        return OutPlayerController && WeaponMod && ModClass &&
            WeaponMod->IsA(ModClass) &&
            IsOwnedInventoryWeapon(OutPlayerController, Weapon);
    }

    bool ApplySlotsToWeaponActor(
        AFortWeapon* Weapon,
        const TArray<void*>& SourceSlots)
    {
        if (!Weapon || !IsArrayHeaderSane(SourceSlots, true))
            return false;

        auto WeaponSlots = GetWeaponSlots(Weapon);
        if (!WeaponSlots || !IsArrayHeaderSane(*WeaponSlots, true))
            return false;

        const bool bReplicatedSlotsMatch =
            SlotsEqual(*WeaponSlots, SourceSlots);
        auto EquippedSlots = GetEquippedWeaponSlots(Weapon);
        if (!EquippedSlots ||
            !IsEquippedArrayHeaderSane(*EquippedSlots))
        {
            return false;
        }
        if (bReplicatedSlotsMatch &&
            EquippedSlotsMatch(*EquippedSlots, SourceSlots))
        {
            return true;
        }

        auto OnRep = Weapon->GetFunction(
            "OnRep_ReplicatedWeaponModSlots");
        if (!OnRep)
            OnRep = Weapon->GetFunction("OnRep_WeaponModSlots");
        if (!ValidateFunctionSchema(
            OnRep,
            {
                {
                    "PreviousModSlots",
                    uint32(sizeof(TArray<void*>))
                }
            }))
        {
            return false;
        }

        // Push-model builds do not discover a raw reflected-array write from
        // ForceNetUpdate alone. Resolve the exact helper before touching the
        // actor so a late-loaded or incompatible helper fails without leaving
        // local and observer state divergent.
        if (!bReplicatedSlotsMatch && !ResolvePushModelApi())
            return false;

        TArray<void*> PreviousSlots{};
        if (bReplicatedSlotsMatch)
        {
            // A freshly spawned weapon can already contain the replicated
            // slots while its runtime ability/effect handles are still empty.
            // Represent the actually equipped subset as the previous raw
            // state so OnRep adds only missing mods and cannot double-grant
            // a mod that native equip already initialized.
            if (!EquippedSlotsAreSubsetOf(
                *EquippedSlots, SourceSlots) ||
                !DeepAssignSlots(SourceSlots, PreviousSlots))
            {
                return false;
            }

            for (int32 Index = 0;
                Index < PreviousSlots.Num();
                ++Index)
            {
                auto Mod = GetSlotMod(PreviousSlots, Index);
                if (!Mod ||
                    ContainsEquippedMod(*EquippedSlots, Mod))
                {
                    continue;
                }

                uint8* Slot =
                    reinterpret_cast<uint8*>(
                        PreviousSlots.Data) +
                    size_t(Index) * size_t(SlotLayout.Size);
                UObject* EmptyMod = nullptr;
                memcpy(
                    Slot + SlotLayout.WeaponModOffset,
                    &EmptyMod,
                    sizeof(EmptyMod));
            }
        }
        else if (!DeepAssignSlots(
            *WeaponSlots, PreviousSlots))
        {
            return false;
        }

        if (!bReplicatedSlotsMatch)
        {
            const void* DefinitionData = Weapon->WeaponData &&
                Weapon->WeaponData->HasWeaponModSlots()
                ? Weapon->WeaponData->WeaponModSlots.Data
                : nullptr;
            if (!DeepAssignSlots(
                SourceSlots,
                *WeaponSlots,
                DefinitionData,
                SourceSlots.Data,
                EquippedSlots ? EquippedSlots->Data : nullptr))
            {
                PreviousSlots.Free();
                return false;
            }

            MarkWeaponModSlotsDirty(Weapon);
        }

        Weapon->Call<void>(OnRep, PreviousSlots);
        PreviousSlots.Free();
        Weapon->ForceNetUpdate();
        return true;
    }

    void CommitOwnedSlots(
        TArray<void*>& OwnedSlots,
        TArray<void*>& Destination,
        const void* ProtectedDataA = nullptr,
        const void* ProtectedDataB = nullptr,
        const void* ProtectedDataC = nullptr)
    {
        void* OldData = Destination.Data;
        const bool bCanReleaseOld =
            OldData &&
            OldData != ProtectedDataA &&
            OldData != ProtectedDataB &&
            OldData != ProtectedDataC &&
            IsArrayHeaderSane(Destination, false);

        Destination = OwnedSlots;
        OwnedSlots.Data = nullptr;
        OwnedSlots.NumElements = 0;
        OwnedSlots.MaxElements = 0;

        if (bCanReleaseOld)
            FMemory::Free(OldData);
    }
}

bool FFortWeaponMods::IsSupported()
{
    return VersionInfo.FortniteVersion >= 28.0 &&
        VersionInfo.FortniteVersion < 32.0;
}

bool FFortWeaponMods::HasEntrySlots(
    const FFortItemEntry& Entry)
{
    if (!IsSupported() ||
        !FFortItemEntry::HasWeaponModSlots() ||
        !Entry.ItemDefinition)
    {
        return false;
    }

    auto WeaponDefinition =
        Entry.ItemDefinition->Cast<
            UFortWeaponItemDefinition>();
    const auto& Slots = Entry.WeaponModSlots;
    return WeaponDefinition &&
        WeaponDefinition->HasWeaponModSlots() &&
        Slots.Num() > 0 &&
        Slots.Num() <= MaxWeaponModSlots &&
        Slots.Max() >= Slots.Num() &&
        Slots.Max() <= 64 &&
        Slots.Data;
}

bool FFortWeaponMods::CopyDefinitionSlotsToEntry(
    const UFortWeaponItemDefinition* WeaponDefinition,
    FFortItemEntry& Destination)
{
    if (!IsSupported() || !WeaponDefinition ||
        !WeaponDefinition->HasWeaponModSlots() ||
        !FFortItemEntry::HasWeaponModSlots())
    {
        return false;
    }

    return DeepAssignSlots(
        WeaponDefinition->WeaponModSlots,
        Destination.WeaponModSlots,
        WeaponDefinition->WeaponModSlots.Data);
}

bool FFortWeaponMods::CopyEntrySlots(
    const FFortItemEntry& Source,
    FFortItemEntry& Destination)
{
    if (!IsSupported() || !FFortItemEntry::HasWeaponModSlots())
        return false;

    if (&Source == &Destination)
        return true;

    if (Source.WeaponModSlots.Num() == 0 &&
        Destination.WeaponModSlots.Num() == 0)
    {
        return true;
    }

    if (Source.WeaponModSlots.Data !=
            Destination.WeaponModSlots.Data &&
        SlotsEqual(
            Source.WeaponModSlots,
            Destination.WeaponModSlots))
    {
        return true;
    }

    return DeepAssignSlots(
        Source.WeaponModSlots,
        Destination.WeaponModSlots,
        GetDefinitionSlotData(Destination.ItemDefinition));
}

void FFortWeaponMods::FreeEntrySlots(FFortItemEntry& Entry)
{
    if (!IsSupported() || !FFortItemEntry::HasWeaponModSlots())
        return;

    auto& Slots = Entry.WeaponModSlots;
    const void* DefinitionData = GetDefinitionSlotData(Entry.ItemDefinition);
    if (Slots.Data && Slots.Data != DefinitionData &&
        IsArrayHeaderSane(Slots, false))
    {
        FMemory::Free(Slots.Data);
    }

    Slots.Data = nullptr;
    Slots.NumElements = 0;
    Slots.MaxElements = 0;
}

void FFortWeaponMods::InitializePickup(
    AFortPickupAthena* Pickup,
    const FFortItemEntry& SourceEntry,
    bool bAllowRandomMods)
{
    if (!Pickup || !HasEntrySlots(SourceEntry))
        return;

    CopyEntrySlots(SourceEntry, Pickup->PrimaryPickupItemEntry);

    auto WeaponDefinition = SourceEntry.ItemDefinition
        ? SourceEntry.ItemDefinition->Cast<UFortWeaponItemDefinition>()
        : nullptr;
    if (!bAllowRandomMods || !WeaponDefinition ||
        HasDynamicSlots(SourceEntry.WeaponModSlots))
    {
        return;
    }

    const auto Library = GetWeaponModLibrary();
    if (!Library ||
        !NativeApi.GetRandomWeaponModSetData ||
        !NativeApi.ApplyModSetToPickup)
    {
        return;
    }

    const UObject* ModSet = Library->Call<const UObject*>(
        NativeApi.GetRandomWeaponModSetData,
        UWorld::GetWorld(),
        WeaponDefinition);
    if (ModSet)
    {
        Library->Call<void>(
            NativeApi.ApplyModSetToPickup,
            ModSet,
            Pickup);
    }
}

bool FFortWeaponMods::ApplyEntrySlotsAfterEquip(
    AFortWeapon* Weapon,
    const FFortItemEntry& Entry)
{
    if (!Weapon || !HasEntrySlots(Entry))
        return false;

    return ApplySlotsToWeaponActor(
        Weapon, Entry.WeaponModSlots);
}

bool FFortWeaponMods::SyncWeaponSlotsToInventory(
    AFortPlayerControllerAthena* PlayerController,
    AFortWeapon* Weapon)
{
    if (!IsSupported() || !IsOwnedInventoryWeapon(PlayerController, Weapon))
        return false;

    auto Inventory = PlayerController->WorldInventory;
    auto ReplicatedEntry = FindReplicatedEntry(
        Inventory, Weapon->ItemEntryGuid);
    auto ItemInstance = FindItemInstance(
        Inventory, Weapon->ItemEntryGuid);
    auto WeaponSlots = GetWeaponSlots(Weapon);
    if (!ReplicatedEntry || !WeaponSlots ||
        !IsArrayHeaderSane(*WeaponSlots, true))
    {
        return false;
    }

    if (!FFortItemEntry::HasWeaponModSlots() ||
        !IsArrayHeaderSane(
            ReplicatedEntry->WeaponModSlots, false) ||
        (ItemInstance &&
            !IsArrayHeaderSane(
                ItemInstance->ItemEntry.WeaponModSlots, false)))
    {
        return false;
    }

    // Allocate both durable copies before replacing either owner. A failed
    // allocation therefore cannot leave the replicated entry and item
    // instance reporting different attachment sets.
    TArray<void*> NewReplicatedSlots{};
    TArray<void*> NewInstanceSlots{};
    if (!DeepAssignSlots(*WeaponSlots, NewReplicatedSlots) ||
        (ItemInstance &&
            !DeepAssignSlots(*WeaponSlots, NewInstanceSlots)))
    {
        NewReplicatedSlots.Free();
        NewInstanceSlots.Free();
        return false;
    }

    const void* OldItemData = ItemInstance
        ? ItemInstance->ItemEntry.WeaponModSlots.Data
        : nullptr;
    CommitOwnedSlots(
        NewReplicatedSlots,
        ReplicatedEntry->WeaponModSlots,
        GetDefinitionSlotData(ReplicatedEntry->ItemDefinition),
        OldItemData,
        WeaponSlots->Data);

    if (ItemInstance)
    {
        CommitOwnedSlots(
            NewInstanceSlots,
            ItemInstance->ItemEntry.WeaponModSlots,
            GetDefinitionSlotData(ItemInstance->ItemEntry.ItemDefinition),
            ReplicatedEntry->WeaponModSlots.Data,
            WeaponSlots->Data);
    }

    AssignPlayerNameToModifiedWeapon(
        PlayerController, Weapon, *ReplicatedEntry);
    Inventory->Update(ReplicatedEntry);

    // Native UpdateItemInstances can shallow-copy the replicated array header;
    // it can also move the fast-array storage. Reacquire both objects before
    // dereferencing either entry again.
    ReplicatedEntry = FindReplicatedEntry(
        Inventory, Weapon->ItemEntryGuid);
    ItemInstance = FindItemInstance(Inventory, Weapon->ItemEntryGuid);
    if (!ReplicatedEntry)
        return false;

    if (ItemInstance)
    {
        DeepAssignSlots(
            *WeaponSlots,
            ItemInstance->ItemEntry.WeaponModSlots,
            GetDefinitionSlotData(ItemInstance->ItemEntry.ItemDefinition),
            ReplicatedEntry->WeaponModSlots.Data,
            WeaponSlots->Data);
    }

    Inventory->ForceNetUpdate();
    Weapon->ForceNetUpdate();
    return true;
}

namespace
{
    void ProcessNativeBenchRpc(
        const char* Operation,
        UObject* Context,
        FFrame& Stack,
        const FWeaponModRpcLayout& Layout,
        void (*Original)(UObject*, FFrame&))
    {
        if (!Original)
        {
            if (BenchResolveLogCount++ < 4)
            {
                SDK::DbgLog(
                    "[WeaponMods] %s rejected: native RPC exec unavailable\n",
                    Operation);
            }
            return;
        }

        UObject* WeaponMod = nullptr;
        AFortWeapon* Weapon = nullptr;
        AFortPlayerControllerAthena* PlayerController = nullptr;
        if (!FFortWeaponMods::IsSupported() ||
            !DecodePurchaseRpc(
                Layout, Stack, WeaponMod, Weapon) ||
            !ValidateModRequest(
                Context,
                WeaponMod,
                Weapon,
                PlayerController))
        {
            if (BenchRejectLogCount++ < 8)
            {
                SDK::DbgLog(
                    "[WeaponMods] %s rejected: invalid frame, mod, or owned weapon\n",
                    Operation);
            }
            return;
        }

        auto WeaponSlots = GetWeaponSlots(Weapon);
        TArray<void*> PreviousSlots{};
        if (!WeaponSlots ||
            !IsArrayHeaderSane(*WeaponSlots, true) ||
            !DeepAssignSlots(*WeaponSlots, PreviousSlots))
        {
            if (BenchRejectLogCount++ < 8)
            {
                SDK::DbgLog(
                    "[WeaponMods] %s rejected: actor slot snapshot unavailable\n",
                    Operation);
            }
            return;
        }

        // Epic's component remains authoritative for the offered attachment,
        // station state, price, currency spend, replacement/default behavior,
        // and the actual actor mutation. The original frame has not been
        // stepped or otherwise changed by our inspection above.
        Original(Context, Stack);

        WeaponSlots = GetWeaponSlots(Weapon);
        if (!WeaponSlots ||
            !IsArrayHeaderSane(*WeaponSlots, true) ||
            SlotsEqual(PreviousSlots, *WeaponSlots))
        {
            if (BenchUnchangedLogCount++ < 8)
            {
                SDK::DbgLog(
                    "[WeaponMods] %s native RPC left weapon slots unchanged; inventory sync skipped\n",
                    Operation);
            }
            PreviousSlots.Free();
            return;
        }

        if (!FFortWeaponMods::SyncWeaponSlotsToInventory(
            PlayerController, Weapon))
        {
            const bool bRolledBack =
                ApplySlotsToWeaponActor(Weapon, PreviousSlots);
            if (BenchSyncFailureLogCount++ < 8)
            {
                SDK::DbgLog(
                    "[WeaponMods] %s inventory sync failed; actor rollback=%d\n",
                    Operation,
                    bRolledBack);
            }
        }

        PreviousSlots.Free();
    }
}

void FFortWeaponMods::ServerPurchaseWeaponModForWeapon_(
    UObject* Context,
    FFrame& Stack)
{
    ProcessNativeBenchRpc(
        "purchase",
        Context,
        Stack,
        PurchaseRpcLayout,
        ServerPurchaseWeaponModForWeapon_OG);
}

void FFortWeaponMods::ServerPurchaseRemoveMod_(
    UObject* Context,
    FFrame& Stack)
{
    ProcessNativeBenchRpc(
        "remove",
        Context,
        Stack,
        RemoveRpcLayout,
        ServerPurchaseRemoveMod_OG);
}

void FFortWeaponMods::EnsureBenchHooks()
{
    if (!IsSupported() ||
        (bPurchaseHookInstalled && bRemoveHookInstalled))
    {
        return;
    }

    const ULONGLONG CurrentTimeMs = GetTickCount64();
    if (CurrentTimeMs < NextBenchResolveTimeMs)
        return;
    NextBenchResolveTimeMs = CurrentTimeMs + 2000ULL;

    if (!ResolveSlotLayout())
    {
        if (BenchResolveLogCount++ < 4)
        {
            SDK::DbgLog(
                "[WeaponMods] Bench hook deferred: slot layout unavailable\n");
        }
        return;
    }

    const auto ComponentClass = SDK::FindClass(
        "FortWeaponModStationComponent");
    auto Component = ComponentClass
        ? ComponentClass->GetDefaultObj()
        : nullptr;
    if (!Component)
    {
        if (BenchResolveLogCount++ < 4)
        {
            SDK::DbgLog(
                "[WeaponMods] Bench hook deferred: component unavailable on FN %.2f\n",
                VersionInfo.FortniteVersion);
        }
        return;
    }

    if (!bPurchaseHookInstalled)
    {
        auto Purchase = Component->GetFunction(
            "ServerPurchaseWeaponModForWeapon");
        FWeaponModRpcLayout Layout{};
        if (ResolvePurchaseRpcLayout(Purchase, Layout) &&
            Purchase->ExecFunction)
        {
            const void* Detour =
                reinterpret_cast<void*>(
                    ServerPurchaseWeaponModForWeapon_);
            if (Purchase->ExecFunction != Detour)
            {
                PurchaseRpcLayout = Layout;
                Utils::ExecHook(
                    Purchase,
                    ServerPurchaseWeaponModForWeapon_,
                    ServerPurchaseWeaponModForWeapon_OG);
            }

            bPurchaseHookInstalled =
                Purchase->ExecFunction == Detour &&
                ServerPurchaseWeaponModForWeapon_OG != nullptr;
        }
        else if (BenchResolveLogCount++ < 4)
        {
            SDK::DbgLog(
                "[WeaponMods] Bench purchase hook deferred: native exec/schema unavailable\n");
        }
    }

    if (!bRemoveHookInstalled)
    {
        auto Remove = Component->GetFunction(
            "ServerPurchaseRemoveMod");
        FWeaponModRpcLayout Layout{};
        if (ResolvePurchaseRpcLayout(Remove, Layout) &&
            Remove->ExecFunction)
        {
            const void* Detour =
                reinterpret_cast<void*>(
                    ServerPurchaseRemoveMod_);
            if (Remove->ExecFunction != Detour)
            {
                RemoveRpcLayout = Layout;
                Utils::ExecHook(
                    Remove,
                    ServerPurchaseRemoveMod_,
                    ServerPurchaseRemoveMod_OG);
            }

            bRemoveHookInstalled =
                Remove->ExecFunction == Detour &&
                ServerPurchaseRemoveMod_OG != nullptr;
        }
        else if (BenchResolveLogCount++ < 4)
        {
            SDK::DbgLog(
                "[WeaponMods] Bench remove hook deferred: native exec/schema unavailable\n");
        }
    }

    if (bPurchaseHookInstalled && bRemoveHookInstalled)
    {
        NextBenchResolveTimeMs = 0;
        SDK::DbgLog(
            "[WeaponMods] Native bench RPCs hooked; Epic pricing and offer validation preserved\n");
    }
}

void FFortWeaponMods::PostLoadHook()
{
    EnsureBenchHooks();
}
