#include "pch.h"

#include "../Public/PlayerLoadout.h"
#include "../../Engine/Public/Texture.h"
#include "../../Engine/Public/NetDriver.h"
#include "../../FortniteGame/Public/FortInventory.h"
#include "../../FortniteGame/Public/FortPlayerControllerAthena.h"
#include "../../FortniteGame/Public/FortWeapon.h"
#include "../../ImGui/imgui.h"

#include <d3d11.h>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <deque>
#include <limits>
#include <unordered_map>
#include <unordered_set>

// Optional ATLAS clients report only their five quickbar GUIDs through an
// exact, fixed-width ServerCheat payload.  This bridge is deliberately local to
// the loadout subsystem: unsupported clients never negotiate, ordinary cheat
// commands are ignored here, and no network address or player identity leaves
// the process.
namespace PlayerLoadoutBridgeServer
{
    constexpr size_t kSlotCount = 5;

    struct FGuidValue
    {
        uint32_t A = 0;
        uint32_t B = 0;
        uint32_t C = 0;
        uint32_t D = 0;

        bool IsZero() const noexcept
        {
            return A == 0 && B == 0 && C == 0 && D == 0;
        }
    };

    struct FSnapshot
    {
        uintptr_t ControllerToken = 0;
        uint64_t ControllerIdentity = 0;
        uint64_t WorldIdentity = 0;
        uint64_t Session = 0;
        uint64_t Generation = 0;
        ULONGLONG ReceivedAt = 0;
        uint32_t Sequence = 0;
        std::array<FGuidValue, kSlotCount> Slots{};
    };

    namespace
    {
        constexpr char kProtocolPrefix[] = "mgloadout:v1:";
        constexpr char kHelloPrefix[] = "mgloadout:v1:h:";
        constexpr char kReportPrefix[] = "mgloadout:v1:r:";
        constexpr wchar_t kAckPrefix[] = L"mgloadout:v1:a:";
        constexpr size_t kProtocolPrefixLength = 13;
        constexpr size_t kMessagePrefixLength = 15;
        constexpr size_t kSessionLength = 16;
        constexpr size_t kSequenceLength = 8;
        constexpr size_t kGuidLength = 32;
        constexpr size_t kHelloLength =
            kMessagePrefixLength + kSessionLength;
        constexpr size_t kReportLength =
            kMessagePrefixLength + kSessionLength + 1 +
            kSequenceLength +
            kSlotCount * (1 + kGuidLength);
        // ATLAS emits no more than one report every 100 ms. Keep the receiver
        // below that boundary so ordinary frame/network jitter cannot discard
        // a changed report until the one-second heartbeat.
        constexpr ULONGLONG kMinimumReportIntervalMs = 50;
        constexpr ULONGLONG kMinimumHelloAckIntervalMs = 1000;
        constexpr ULONGLONG kDisconnectedRetentionMs =
            30 * 60 * 1000;
        constexpr size_t kMaximumControllers = 256;

        static_assert(kHelloLength == 31);
        static_assert(kReportLength == 205);

        struct FRecord
        {
            FSnapshot Snapshot{};
            uintptr_t ControllerToken = 0;
            uint64_t ControllerIdentity = 0;
            uint64_t WorldIdentity = 0;
            ULONGLONG LastActivityAt = 0;
            ULONGLONG NextAcceptedAt = 0;
            ULONGLONG NextHelloAckAt = 0;
            uint64_t PendingReplacementSession = 0;
            ULONGLONG PendingReplacementAt = 0;
            uint32_t PendingReplacementSequence = 0;
        };

        SRWLOCK GBridgeLock = SRWLOCK_INIT;
        std::unordered_map<uint64_t, FRecord> GRecords;
        uint64_t GNextGeneration = 1;

        class FTryExclusiveBridgeLock
        {
        public:
            FTryExclusiveBridgeLock() noexcept
                : Acquired(
                    TryAcquireSRWLockExclusive(
                        &GBridgeLock) != FALSE)
            {
            }

            ~FTryExclusiveBridgeLock() noexcept
            {
                if (Acquired)
                    ReleaseSRWLockExclusive(&GBridgeLock);
            }

            explicit operator bool() const noexcept
            {
                return Acquired;
            }

        private:
            bool Acquired = false;
        };

        class FTrySharedBridgeLock
        {
        public:
            FTrySharedBridgeLock() noexcept
                : Acquired(
                    TryAcquireSRWLockShared(
                        &GBridgeLock) != FALSE)
            {
            }

            ~FTrySharedBridgeLock() noexcept
            {
                if (Acquired)
                    ReleaseSRWLockShared(&GBridgeLock);
            }

            explicit operator bool() const noexcept
            {
                return Acquired;
            }

        private:
            bool Acquired = false;
        };

        static uint64_t GetObjectIdentityUnsafe(
            const UObject* Object)
        {
            if (!Object ||
                !SDK::MemReadable(
                    Object,
                    sizeof(UObject)))
            {
                return 0;
            }

            const int32 Index = Object->Index;
            if (Index < 0 || Index >= TUObjectArray::Num())
                return 0;
            auto Item = TUObjectArray::GetItemByIndex(Index);
            const int32 InvalidFlags = 0x20;
            if (!Item ||
                Item->GetObject() != Object ||
                (Item->GetFlags() & InvalidFlags) ||
                !Object->Class ||
                !SDK::MemReadable(Object->Class, sizeof(UClass)))
            {
                return 0;
            }
            return
                (static_cast<uint64_t>(
                    static_cast<uint32_t>(Index)) << 32) |
                static_cast<uint32_t>(Item->SerialRef());
        }

        static uint64_t GetObjectIdentity(
            const UObject* Object) noexcept
        {
            __try
            {
                return GetObjectIdentityUnsafe(Object);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return 0;
            }
        }

        static uint64_t GetControllerIdentity(
            const AFortPlayerControllerAthena* PlayerController) noexcept
        {
            return GetObjectIdentity(PlayerController);
        }

        static int HexValue(char Character) noexcept
        {
            if (Character >= '0' && Character <= '9')
                return Character - '0';
            if (Character >= 'a' && Character <= 'f')
                return 10 + Character - 'a';
            if (Character >= 'A' && Character <= 'F')
                return 10 + Character - 'A';
            return -1;
        }

        static bool ParseHex(
            const char* Text,
            size_t Length,
            uint64_t& Value) noexcept
        {
            Value = 0;
            if (!Text || !Length || Length > 16)
                return false;
            for (size_t Index = 0; Index < Length; ++Index)
            {
                const int Digit = HexValue(Text[Index]);
                if (Digit < 0)
                    return false;
                Value = (Value << 4) |
                    static_cast<uint64_t>(Digit);
            }
            return true;
        }

        static bool ParseGuid(
            const char* Text,
            FGuidValue& Guid) noexcept
        {
            uint64_t Part = 0;
            if (!ParseHex(Text, 8, Part))
                return false;
            Guid.A = static_cast<uint32_t>(Part);
            if (!ParseHex(Text + 8, 8, Part))
                return false;
            Guid.B = static_cast<uint32_t>(Part);
            if (!ParseHex(Text + 16, 8, Part))
                return false;
            Guid.C = static_cast<uint32_t>(Part);
            if (!ParseHex(Text + 24, 8, Part))
                return false;
            Guid.D = static_cast<uint32_t>(Part);
            return true;
        }

        static bool GuidEquals(
            const FGuidValue& Left,
            const FGuidValue& Right) noexcept
        {
            return Left.A == Right.A &&
                Left.B == Right.B &&
                Left.C == Right.C &&
                Left.D == Right.D;
        }

        static bool ValidateUniqueGuids(
            const std::array<FGuidValue, kSlotCount>& Slots) noexcept
        {
            for (size_t Left = 0; Left < Slots.size(); ++Left)
            {
                if (Slots[Left].IsZero())
                    continue;
                for (size_t Right = Left + 1;
                     Right < Slots.size(); ++Right)
                {
                    if (GuidEquals(Slots[Left], Slots[Right]))
                        return false;
                }
            }
            return true;
        }

        static void BuildAck(
            uint64_t Session,
            std::array<wchar_t, kHelloLength + 1>& Ack) noexcept
        {
            static constexpr wchar_t Hex[] = L"0123456789abcdef";
            memcpy(
                Ack.data(),
                kAckPrefix,
                kMessagePrefixLength * sizeof(wchar_t));
            wchar_t* Write = Ack.data() + kMessagePrefixLength;
            for (size_t Index = 0; Index < kSessionLength; ++Index)
            {
                const size_t Shift =
                    (kSessionLength - Index - 1) * 4;
                *Write++ = Hex[(Session >> Shift) & 0xf];
            }
            *Write = L'\0';
        }

        static bool IsNewerSequence(
            uint32_t Candidate,
            uint32_t Previous) noexcept
        {
            const uint32_t Distance = Candidate - Previous;
            return Distance != 0 && Distance < 0x80000000u;
        }

        static bool IsSameLifetime(
            const FRecord& Record,
            uintptr_t ControllerToken,
            uint64_t ControllerIdentity,
            uint64_t WorldIdentity) noexcept
        {
            return
                Record.ControllerToken == ControllerToken &&
                Record.ControllerIdentity == ControllerIdentity &&
                Record.WorldIdentity == WorldIdentity;
        }

        static void PruneForActivity(
            uint64_t CurrentWorldIdentity,
            ULONGLONG Now,
            bool NeedsCapacity)
        {
            const bool AtCapacity =
                NeedsCapacity &&
                GRecords.size() >= kMaximumControllers;
            for (auto It = GRecords.begin();
                 It != GRecords.end();)
            {
                const auto& Record = It->second;
                const bool WrongWorld =
                    Record.WorldIdentity != CurrentWorldIdentity;
                const bool DisconnectedTooLong =
                    AtCapacity &&
                    Now >= Record.LastActivityAt &&
                    Now - Record.LastActivityAt >
                        kDisconnectedRetentionMs;
                if (WrongWorld || DisconnectedTooLong)
                    It = GRecords.erase(It);
                else
                    ++It;
            }

            if (!NeedsCapacity ||
                GRecords.size() < kMaximumControllers)
            {
                return;
            }

            auto Oldest = GRecords.end();
            for (auto It = GRecords.begin();
                 It != GRecords.end(); ++It)
            {
                if (Oldest == GRecords.end() ||
                    It->second.LastActivityAt <
                        Oldest->second.LastActivityAt)
                {
                    Oldest = It;
                }
            }
            if (Oldest != GRecords.end())
                GRecords.erase(Oldest);
        }

        static bool AcceptHelloRateLimit(
            AFortPlayerControllerAthena* PlayerController)
        {
            const uint64_t ControllerIdentity =
                GetControllerIdentity(PlayerController);
            const uint64_t WorldIdentity =
                GetObjectIdentity(UWorld::GetWorld());
            if (!ControllerIdentity || !WorldIdentity)
                return false;

            const uintptr_t ControllerToken =
                reinterpret_cast<uintptr_t>(PlayerController);
            const ULONGLONG Now = GetTickCount64();
            FTryExclusiveBridgeLock Lock;
            if (!Lock)
                return false;

            const bool NeedsCapacity =
                GRecords.find(ControllerIdentity) ==
                    GRecords.end();
            PruneForActivity(
                WorldIdentity, Now, NeedsCapacity);

            auto Existing = GRecords.find(ControllerIdentity);
            if (Existing == GRecords.end())
            {
                if (GRecords.size() >= kMaximumControllers)
                    return false;
                Existing = GRecords.emplace(
                    ControllerIdentity, FRecord{}).first;
            }

            auto& Record = Existing->second;
            const bool SameLifetime = IsSameLifetime(
                Record,
                ControllerToken,
                ControllerIdentity,
                WorldIdentity);
            if (Record.ControllerIdentity && !SameLifetime)
                Record = {};
            else if (SameLifetime &&
                     Now < Record.NextHelloAckAt)
                return false;

            Record.ControllerToken = ControllerToken;
            Record.ControllerIdentity = ControllerIdentity;
            Record.WorldIdentity = WorldIdentity;
            Record.LastActivityAt = Now;
            Record.NextHelloAckAt =
                Now + kMinimumHelloAckIntervalMs;
            return true;
        }

        static bool PublishReport(
            AFortPlayerControllerAthena* PlayerController,
            uint64_t Session,
            uint32_t Sequence,
            const std::array<FGuidValue, kSlotCount>& Slots)
        {
            const uint64_t ControllerIdentity =
                GetControllerIdentity(PlayerController);
            const uint64_t WorldIdentity =
                GetObjectIdentity(UWorld::GetWorld());
            if (!ControllerIdentity || !WorldIdentity)
                return false;

            const uintptr_t ControllerToken =
                reinterpret_cast<uintptr_t>(PlayerController);
            const ULONGLONG Now = GetTickCount64();
            FTryExclusiveBridgeLock Lock;
            if (!Lock)
                return false;

            const bool NeedsCapacity =
                GRecords.find(ControllerIdentity) ==
                    GRecords.end();
            PruneForActivity(
                WorldIdentity, Now, NeedsCapacity);

            auto Existing = GRecords.find(ControllerIdentity);
            if (Existing == GRecords.end())
            {
                if (GRecords.size() >= kMaximumControllers)
                    return false;
                Existing = GRecords.emplace(
                    ControllerIdentity, FRecord{}).first;
            }

            auto& Record = Existing->second;
            const bool SameLifetime = IsSameLifetime(
                Record,
                ControllerToken,
                ControllerIdentity,
                WorldIdentity);
            if (Record.ControllerIdentity && !SameLifetime)
                Record = {};

            auto& Snapshot = Record.Snapshot;
            if (SameLifetime && Snapshot.Generation)
            {
                if (Now < Record.NextAcceptedAt)
                {
                    // Remember a companion reload without weakening the
                    // controller-bound limiter. Its next sequence can then
                    // continue after this short admission window.
                    if (Snapshot.Session != Session && Sequence == 1)
                    {
                        Record.PendingReplacementSession = Session;
                        Record.PendingReplacementSequence = Sequence;
                        Record.PendingReplacementAt = Now;
                    }
                    return false;
                }

                if (Snapshot.Session == Session)
                {
                    if (!IsNewerSequence(
                            Sequence, Snapshot.Sequence))
                    {
                        return false;
                    }
                }
                else
                {
                    const bool FreshSession = Sequence == 1;
                    const bool ContinuesPendingSession =
                        Record.PendingReplacementSession == Session &&
                        Now >= Record.PendingReplacementAt &&
                        Now - Record.PendingReplacementAt <= 60 * 1000 &&
                        IsNewerSequence(
                            Sequence,
                            Record.PendingReplacementSequence);
                    if (!FreshSession && !ContinuesPendingSession)
                        return false;
                }
            }

            Snapshot.ControllerToken = ControllerToken;
            Snapshot.ControllerIdentity = ControllerIdentity;
            Snapshot.WorldIdentity = WorldIdentity;
            Snapshot.Session = Session;
            Snapshot.Sequence = Sequence;
            Snapshot.ReceivedAt = Now;
            Snapshot.Slots = Slots;
            Snapshot.Generation = GNextGeneration++;
            if (!GNextGeneration)
                GNextGeneration = 1;

            Record.ControllerToken = ControllerToken;
            Record.ControllerIdentity = ControllerIdentity;
            Record.WorldIdentity = WorldIdentity;
            Record.LastActivityAt = Now;
            Record.NextAcceptedAt =
                Now + kMinimumReportIntervalMs;
            Record.PendingReplacementSession = 0;
            Record.PendingReplacementSequence = 0;
            Record.PendingReplacementAt = 0;
            return true;
        }
    }

    static bool TryGetLatestSnapshot(
        AFortPlayerControllerAthena* PlayerController,
        FSnapshot& Snapshot,
        uint64_t MaximumAgeMilliseconds) noexcept
    {
        Snapshot = {};
        const uint64_t ControllerIdentity =
            GetControllerIdentity(PlayerController);
        const uint64_t WorldIdentity =
            GetObjectIdentity(UWorld::GetWorld());
        if (!ControllerIdentity || !WorldIdentity)
            return false;

        try
        {
            FTrySharedBridgeLock Lock;
            if (!Lock)
                return false;
            const auto Existing =
                GRecords.find(ControllerIdentity);
            if (Existing == GRecords.end())
                return false;

            const auto& Current = Existing->second.Snapshot;
            const ULONGLONG Now = GetTickCount64();
            if (!Current.Session ||
                !Current.Generation ||
                !Current.ReceivedAt ||
                Current.ControllerToken !=
                    reinterpret_cast<uintptr_t>(PlayerController) ||
                Current.ControllerIdentity != ControllerIdentity ||
                Current.WorldIdentity != WorldIdentity ||
                Now < Current.ReceivedAt ||
                (MaximumAgeMilliseconds &&
                 Now - Current.ReceivedAt >
                    MaximumAgeMilliseconds))
            {
                return false;
            }
            Snapshot = Current;
            return true;
        }
        catch (...)
        {
            Snapshot = {};
            return false;
        }
    }

    static bool TryHandleMessage(
        AFortPlayerControllerAthena* PlayerController,
        const std::string& Message) noexcept
    {
        if (Message.size() < kProtocolPrefixLength ||
            Message.compare(
                0,
                kProtocolPrefixLength,
                kProtocolPrefix) != 0)
        {
            return false;
        }

        const bool IsHello =
            Message.size() == kHelloLength &&
            Message.compare(
                0,
                kMessagePrefixLength,
                kHelloPrefix) == 0;
        const bool IsReport =
            Message.size() == kReportLength &&
            Message.compare(
                0,
                kMessagePrefixLength,
                kReportPrefix) == 0;
        if (!IsHello && !IsReport)
            return true;

        uint64_t Session = 0;
        if (!ParseHex(
                Message.data() + kMessagePrefixLength,
                kSessionLength,
                Session) ||
            !Session)
        {
            return true;
        }

        try
        {
            if (IsHello)
            {
                if (AcceptHelloRateLimit(PlayerController))
                {
                    std::array<wchar_t, kHelloLength + 1> Ack{};
                    BuildAck(Session, Ack);
                    PlayerController->ClientMessage(
                        FString(Ack.data()), FName(), 1.f);
                }
                return true;
            }

            if (Message[kMessagePrefixLength +
                        kSessionLength] != ':')
            {
                return true;
            }
            uint64_t ParsedSequence = 0;
            const size_t SequenceOffset =
                kMessagePrefixLength + kSessionLength + 1;
            if (!ParseHex(
                    Message.data() + SequenceOffset,
                    kSequenceLength,
                    ParsedSequence) ||
                !ParsedSequence ||
                ParsedSequence > UINT32_MAX)
            {
                return true;
            }

            std::array<FGuidValue, kSlotCount> Slots{};
            size_t Offset = SequenceOffset + kSequenceLength;
            for (size_t Slot = 0; Slot < kSlotCount; ++Slot)
            {
                if (Message[Offset] != ':' ||
                    !ParseGuid(
                        Message.data() + Offset + 1,
                        Slots[Slot]))
                {
                    return true;
                }
                Offset += 1 + kGuidLength;
            }
            if (Offset != Message.size() ||
                !ValidateUniqueGuids(Slots))
            {
                return true;
            }

            const uint32_t Sequence =
                static_cast<uint32_t>(ParsedSequence);
            PublishReport(
                PlayerController,
                Session,
                Sequence,
                Slots);
            return true;
        }
        catch (...)
        {
            return true;
        }
    }
}

namespace PlayerLoadout
{
bool HandleBridgeMessage(
    AFortPlayerControllerAthena* PlayerController,
    const std::string& Message) noexcept
{
    bool IsLoadoutProtocol = false;
    __try
    {
        constexpr size_t ProtocolPrefixLength = 13;
        IsLoadoutProtocol =
            Message.size() >= ProtocolPrefixLength &&
            Message.compare(
                0,
                ProtocolPrefixLength,
                "mgloadout:v1:") == 0;
        return PlayerLoadoutBridgeServer::
            TryHandleMessage(
                PlayerController,
                Message);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Optional telemetry is consumed on a guarded fault only when its
        // exact protocol prefix was already validated. Ordinary ServerCheat
        // traffic remains available to the rest of the command pipeline.
        return IsLoadoutProtocol;
    }
}

namespace
{
    constexpr int kSlotCount = 5;
    constexpr int kCatalogForegroundObjectsPerTick = 1024;
    constexpr int kCatalogBackgroundObjectsPerTick = 256;
    constexpr int kCatalogForegroundItemsPerTick = 24;
    constexpr int kCatalogBackgroundItemsPerTick = 4;
    constexpr auto kCatalogForegroundTimeBudget =
        std::chrono::microseconds(600);
    constexpr auto kCatalogBackgroundTimeBudget =
        std::chrono::microseconds(200);
    constexpr size_t kCatalogPublishItemsPerTick = 24;
    constexpr size_t kCatalogReserveItems = 8192;
    constexpr size_t kMetadataCacheMaxItems = 16384;
    constexpr ULONGLONG kInspectLeaseMs = 900;
    constexpr ULONGLONG kSnapshotIntervalMs = 125;
    constexpr ULONGLONG kBridgeSnapshotPollMs = 25;
    constexpr ULONGLONG kCatalogLeaseMs = 900;
    constexpr ULONGLONG kCatalogForegroundTickIntervalMs = 50;
    constexpr ULONGLONG kCatalogBackgroundTickIntervalMs = 100;
    constexpr ULONGLONG kPickerIconIntervalMs = 60;
    constexpr ULONGLONG kLiveSlotIconIntervalMs = 30;
    constexpr size_t kMaxIconRequests = 32;
    constexpr size_t kMaxIconResults = 16;
    constexpr size_t kMaxIconUploadsPerFrame = 1;
    constexpr size_t kMaxGpuIcons = 256;
    constexpr size_t kMaxAsyncIconLoads = 2;
    constexpr size_t kMaxAsyncPickerIconLoads = 1;
    constexpr ULONGLONG kAsyncIconLoadRequestIntervalMs = 200;
    constexpr ULONGLONG kAsyncIconLoadTimeoutMs = 12000;
    constexpr ULONGLONG kAsyncIconLiveRetryMs = 10000;
    constexpr ULONGLONG kAsyncIconPickerRetryMs = 30000;
    constexpr int kReportedQuickbarCapacity = 10;
    constexpr ULONGLONG kReportedMutationPollMs = 100;
    constexpr ULONGLONG kReportedMutationAckTimeoutMs = 6000;
    constexpr ULONGLONG kReportedMutationRecoveryTimeoutMs = 4000;
    // ATLAS sends a stable heartbeat every second. Five seconds tolerates
    // transient packet/tick stalls without allowing a disconnected client's
    // slot permutation to remain authoritative for most of a minute.
    constexpr uint64_t kBridgeSnapshotMaxAgeMs = 5000;
    constexpr uint64_t kBridgeMutationMaxAgeMs = 1500;
	constexpr ULONGLONG kReflectionSchemaRetryMs = 2000;

    constexpr uint64_t kCpfParm = 0x80;
    constexpr uint64_t kCpfOutParm = 0x100;
    constexpr uint64_t kCpfReturnParm = 0x400;
    constexpr uint64_t kCastClassStructProperty =
        0x100000;
    constexpr uint64_t kCastClassIntProperty = 0x80;
    constexpr uint64_t kCastClassObjectProperty = 0x10000;
    constexpr uint64_t kCastClassNameProperty = 0x2000;
    constexpr uint64_t kCastClassDelegateProperty = 0x800000;
    constexpr uint64_t kCastClassSoftObjectProperty =
        0x20000000;

    enum class ELoadoutAvailability
    {
        Loading,
        Ready,
        Unavailable
    };

    enum class EItemMode : uint8
    {
        BattleRoyale,
        SaveTheWorld,
        Unknown
    };

    struct FGuidValue
    {
        int32 A = 0;
        int32 B = 0;
        int32 C = 0;
        int32 D = 0;

        bool IsZero() const
        {
            return A == 0 && B == 0 && C == 0 && D == 0;
        }
    };

    struct FItemMetadata
    {
        uintptr_t Token = 0;
        uint64_t ObjectIdentity = 0;
        std::string Id;
        std::string Name;
        std::string SearchText;
        int Rarity = 0;
        EItemMode Mode = EItemMode::Unknown;
    };

    struct FSlotView
    {
        bool Occupied = false;
        uintptr_t ItemToken = 0;
        uint64_t ItemIdentity = 0;
        FGuidValue Guid;
        std::string Id;
        std::string Name;
        int Rarity = 0;
        EItemMode Mode = EItemMode::Unknown;
    };

    struct FLoadoutSnapshot
    {
        uintptr_t TargetToken = 0;
        uint64_t WorldGeneration = 0;
        uint64_t TargetIdentity = 0;
        uint64_t Generation = 0;
        ELoadoutAvailability Availability =
            ELoadoutAvailability::Loading;
        bool HasExactSlotOrder = false;
        bool CanEditSlots = false;
        bool UsesGuidOnlyMutation = false;
        bool UsesBridgeSlots = false;
        uint64_t BridgeSession = 0;
        uint64_t BridgeGeneration = 0;
        std::array<FSlotView, kSlotCount> Slots{};
        FGuidValue EquippedGuid;
        std::string Message;
    };

    struct FCatalogSnapshot
    {
        uint64_t Generation = 0;
        int ScannedObjects = 0;
        int TotalObjects = 0;
        bool Complete = false;
        std::vector<FItemMetadata> Items;
    };

    struct FActionRequest
    {
        uint64_t Id = 0;
        uintptr_t TargetToken = 0;
        uint64_t WorldGeneration = 0;
        uint64_t TargetIdentity = 0;
        int Slot = -1;
        bool Clear = false;
        bool InventoryOnly = false;
        uintptr_t ItemToken = 0;
        uint64_t ItemIdentity = 0;
        std::string ExpectedItemId;
        FGuidValue ExpectedGuid;
    };

    struct FActionResult
    {
        uint64_t Id = 0;
        uintptr_t TargetToken = 0;
        bool HasResult = false;
        bool Success = false;
        std::string Message;
    };

    struct FIconPixels
    {
        uintptr_t ItemToken = 0;
        uint64_t ItemIdentity = 0;
        bool PickerOnly = false;
        bool Success = false;
        ULONGLONG DeferredUntil = 0;
        bool DeferredWorkPerformed = false;
        int Width = 0;
        int Height = 0;
        ULONGLONG RetryAfterMs = 30000;
        std::vector<unsigned char> Pixels;
    };

    struct FIconRequest
    {
        uintptr_t ItemToken = 0;
        uint64_t ItemIdentity = 0;
        bool PickerOnly = false;
        ULONGLONG NotBeforeAt = 0;
    };

    struct FIconFailure
    {
        uint64_t ItemIdentity = 0;
        ULONGLONG RetryAt = 0;
        bool PickerOnly = false;
    };

    struct FTextureDecodeFailure
    {
        uint64_t TextureIdentity = 0;
        ULONGLONG RetryAt = 0;
        unsigned int FailureCount = 0;
        bool PickerOnly = false;
    };

    struct FReflectedPropertyView
    {
        uint32 Offset = UINT32_MAX;
        uint32 ElementSize = 0;
        int32 ArrayDimension = 0;
        uint64_t PropertyFlags = 0;
    };

    struct FPreviewSoftReference
    {
        std::array<uint8_t, 0x40> Bytes{};
        uint32 Size = 0;
        UEAllocatedWString Path;
        bool Valid = false;
    };

    struct FAsyncIconLoad
    {
        uintptr_t ItemToken = 0;
        uint64_t ItemIdentity = 0;
        uint64_t WorldIdentity = 0;
        const UClass* ExpectedClass = nullptr;
        bool PickerOnly = false;
        ULONGLONG StartedAt = 0;
        std::wstring Path;
    };

    struct FAsyncAssetLoadFailure
    {
        uint64_t WorldIdentity = 0;
        ULONGLONG RetryAt = 0;
        bool PickerOnly = false;
    };

    struct FAsyncIconLoadSchema
    {
        const UClass* LibraryClass = nullptr;
        UObject* DefaultObject = nullptr;
        UFunction* Function = nullptr;
        bool Initialized = false;
        bool Readable = false;
        ULONGLONG NextRetryAt = 0;
        uint32 BufferSize = 0;
        FReflectedPropertyView WorldContext;
        FReflectedPropertyView Asset;
        FReflectedPropertyView OnLoaded;
        FReflectedPropertyView LatentInfo;
        uint32 LatentLinkageOffset = UINT32_MAX;
        uint32 LatentUuidOffset = UINT32_MAX;
        uint32 LatentCallbackTargetOffset = UINT32_MAX;
    };

    struct FPublishedState
    {
        FLoadoutSnapshot Loadout;
        FCatalogSnapshot Catalog;
        FActionResult ActionResult;
        bool ActionPending = false;
        uintptr_t PendingTarget = 0;
        int PendingSlot = -1;
    };

    struct FResolvedSlot
    {
        bool Occupied = false;
        FGuid Guid{};
        const UFortItemDefinition* Definition = nullptr;
        int32 Count = 0;
    };

    // QuickBarEquippedItemGuids is stable from the first builds that expose
    // PrimaryQuickBarSlotItemGuids through the current releases. Keep this as
    // a copied POD snapshot; never retain a pointer into controller memory.
    struct FReportedQuickbarGuidState
    {
        FGuid EquippedItemGuids[
            kReportedQuickbarCapacity]{};
        int32 NumEnabledSlots = 0;
    };
    static_assert(
        sizeof(FReportedQuickbarGuidState) == 0xA4);

    enum class EActionTransactionPhase : uint8
    {
        None,
        Granting,
        Staged,
        Assigned,
        WaitingForReportedAck,
        WaitingForReportedRecovery,
        ClearingOld,
        RemovingOld,
        Committed
    };

    struct FActionTransaction
    {
        bool HasStagedItem = false;
        bool RecoveryFailed = false;
        uintptr_t TargetToken = 0;
        uintptr_t InventoryToken = 0;
        uint64_t InventoryIdentity = 0;
        int Slot = -1;
        FGuid NewGuid{};
        uint64_t NewInstanceIdentity = 0;
        FResolvedSlot Previous;
        bool UsesGuidOnlyMutation = false;
        bool VerifyPreviousOrderIndex = false;
        bool HasModernLedgerSnapshot = false;
        uint64_t ModernLedgerControllerIdentity = 0;
        uint64_t ModernLedgerInventoryIdentity = 0;
        std::array<FGuidValue, kSlotCount>
            ModernLedgerSlots{};
        std::array<FGuidValue, kSlotCount>
            BaselineSlots{};
        EActionTransactionPhase Phase =
            EActionTransactionPhase::None;
        std::vector<FGuidValue> BaselineGuids;
    };

    struct FResolvedLoadout
    {
        AFortPlayerControllerAthena* PlayerController = nullptr;
        AFortInventory* Inventory = nullptr;
        std::array<FResolvedSlot, kSlotCount> Slots{};
        bool HasLegacyQuickbar = false;
        bool HasOrderIndex = false;
        bool HasClientQuickbarPlacement = false;
        bool UsesReportedQuickbarSlots = false;
        bool UsesBridgeQuickbarSlots = false;
        bool UsesClientQuickbarSlots = false;
        bool UsesOrderIndexSlots = false;
        bool UsesFallbackSlots = false;
        bool HasResolvedSlots = false;
        bool HasAuthoritativeSlots = false;
        bool CanMutateSlots = false;
        bool UsesGuidOnlyMutation = false;
        bool HasStrictReportedQuickbarSchema = false;
        int ReportedQuickbarRawBase = -1;
        int ReportedQuickbarEnabledSlots = 0;
        uint64_t BridgeSession = 0;
        uint64_t BridgeGeneration = 0;
    };

    enum class ESlotResolvePolicy : uint8
    {
        Normal,
        PostMutationVerification
    };

    struct FModernSlotLedger
    {
        uint64_t InventoryIdentity = 0;
        ULONGLONG LastExactClientAt = 0;
        std::array<FGuidValue, kSlotCount> Slots{};
    };

    enum class EReportedMutationPhase : uint8
    {
        None,
        WaitingForAck,
        WaitingForRecovery
    };

    struct FReportedMutation
    {
        bool Active = false;
        bool Clear = false;
        bool UsesBridgeSnapshot = false;
        FActionRequest Request;
        FActionTransaction Transaction;
        EReportedMutationPhase Phase =
            EReportedMutationPhase::None;
        int RawBase = -1;
        int EnabledSlots = 0;
        uint64_t BridgeSession = 0;
        uint64_t BridgeBaselineGeneration = 0;
        uint64_t BridgeRecoveryBaselineGeneration = 0;
        bool HasRecoveryStartSlots = false;
        bool RecoveryObservedTransition = false;
        std::array<FGuidValue, kSlotCount>
            RecoveryStartSlots{};
        FGuid NewGuid{};
        uint64_t NewInstanceIdentity = 0;
        ULONGLONG NextPollAt = 0;
        ULONGLONG Deadline = 0;
        std::string FailureMessage;
    };

    std::atomic<bool> GProcessDisabled{ false };
    std::atomic_flag GGameTickActive =
        ATOMIC_FLAG_INIT;
    SRWLOCK GSharedLock = SRWLOCK_INIT;

    class FTrySharedStateLock
    {
    public:
        FTrySharedStateLock() noexcept
        {
            if (!GProcessDisabled.load(
                    std::memory_order_acquire))
            {
                Acquired =
                    TryAcquireSRWLockExclusive(
                        &GSharedLock) != FALSE;
            }
        }

        ~FTrySharedStateLock() noexcept
        {
            if (Acquired)
                ReleaseSRWLockExclusive(
                    &GSharedLock);
        }

        FTrySharedStateLock(
            const FTrySharedStateLock&) = delete;
        FTrySharedStateLock& operator=(
            const FTrySharedStateLock&) = delete;

        bool owns_lock() const noexcept
        {
            return Acquired;
        }

    private:
        bool Acquired = false;
    };

    FPublishedState GPublished;
    std::deque<FActionRequest> GActions;
    std::deque<FIconRequest> GIconRequests;
    std::deque<FIconPixels> GIconResults;
    std::unordered_set<uintptr_t> GQueuedIcons;

    std::atomic<uintptr_t> GRequestedTarget{ 0 };
    std::atomic<ULONGLONG> GInspectLeaseUntil{ 0 };
    std::atomic<ULONGLONG> GCatalogLeaseUntil{ 0 };
    std::atomic<ULONGLONG> GPickerIconLeaseUntil{ 0 };
    std::atomic<uintptr_t> GDisabledWorld{ 0 };
    std::atomic<uint64_t> GWorldGeneration{ 0 };
    std::atomic<uint64_t> GCacheEpoch{ 0 };
    std::atomic<uint64_t> GIconQueueEpoch{ 0 };
    std::atomic<bool> GCatalogResetRequested{ false };

    uint64_t GLoadoutGeneration = 0;
    uint64_t GCatalogGeneration = 0;
    uint64_t GNextActionId = 1;
    uintptr_t GLastSnapshotTarget = 0;
    ULONGLONG GLastSnapshotAt = 0;
    uintptr_t GSnapshotFaultTarget = 0;
    unsigned int GSnapshotFaultCount = 0;
    ULONGLONG GSnapshotRetryAt = 0;
    ULONGLONG GLastIconAt = 0;
    ULONGLONG GLastCatalogTickAt = 0;
    ULONGLONG GLastBridgeSnapshotPollAt = 0;
    ULONGLONG GLastAsyncIconLoadAt = 0;
    int32 GNextAsyncIconLoadUuid = 0x4D470000;

    int32 GCatalogScanIndex = 0;
    int32 GCatalogScanLimit = 0;
    int32 GCatalogScanFront = 0;
    int32 GCatalogScanBack = -1;
    bool GCatalogScanNewestNext = true;
    bool GCatalogComplete = false;
    std::deque<FItemMetadata> GPendingCatalogItems;
    std::unordered_set<uintptr_t> GCatalogTokens;
    std::unordered_map<uintptr_t, FItemMetadata>
        GMetadataCache;
    std::unordered_map<uintptr_t, FIconFailure>
        GIconFailures;
    std::unordered_map<uintptr_t, FTextureDecodeFailure>
        GTextureDecodeFailures;
    std::vector<FAsyncIconLoad> GAsyncIconLoads;
    std::unordered_map<std::wstring, FAsyncAssetLoadFailure>
        GAsyncIconLoadFailures;
    std::unordered_set<std::wstring> GAsyncIconAttemptedPaths;
    FAsyncIconLoadSchema GAsyncIconLoadSchema;
    bool GAsyncIconLoadingDisabled = false;
    std::unordered_set<uint64_t> GFailedControllerIdentities;
    std::unordered_map<uint64_t, FModernSlotLedger>
        GModernSlotLedgers;
    std::deque<FIconPixels> GPendingIconResults;
    std::deque<FIconRequest> GPendingDeferredIconRequests;
    bool GHasPendingActionResult = false;
    bool GRecoveryFaultRequested = false;
    FReportedMutation GReportedMutation;
    FActionRequest GPendingActionRequest;
    bool GPendingActionSuccess = false;
    std::string GPendingActionMessage;
    uint64_t GObservedWorldIdentity =
        (std::numeric_limits<uint64_t>::max)();
    uint64_t GPendingWorldIdentity = 0;
    bool GWorldResetPending = false;
    std::atomic<bool> GFaultPublicationPending{ false };
    FLoadoutSnapshot GGameThreadSnapshot;

    static bool AreGuidsEqual(const FGuid& Left, const FGuid& Right)
    {
        return Left.A == Right.A &&
            Left.B == Right.B &&
            Left.C == Right.C &&
            Left.D == Right.D;
    }

    static bool AreGuidsEqual(
        const FGuid& Left,
        const FGuidValue& Right)
    {
        return Left.A == Right.A &&
            Left.B == Right.B &&
            Left.C == Right.C &&
            Left.D == Right.D;
    }

    static FGuidValue ToGuidValue(const FGuid& Guid)
    {
        return { Guid.A, Guid.B, Guid.C, Guid.D };
    }

    static bool AreGuidValuesEqual(
        const FGuidValue& Left,
        const FGuidValue& Right)
    {
        return Left.A == Right.A &&
            Left.B == Right.B &&
            Left.C == Right.C &&
            Left.D == Right.D;
    }

    static std::string Lowercase(std::string Value)
    {
        std::transform(
            Value.begin(), Value.end(), Value.begin(),
            [](unsigned char Character)
            {
                return static_cast<char>(
                    std::tolower(Character));
            });
        return Value;
    }

    static bool IsLiveObject(const UObject* Object)
    {
        if (!Object || !SDK::MemReadable(Object, sizeof(UObject)))
            return false;

        const int32 ObjectIndex = Object->Index;
        if (ObjectIndex < 0 || ObjectIndex >= TUObjectArray::Num())
            return false;

        auto Item = TUObjectArray::GetItemByIndex(ObjectIndex);
        const int32 InvalidFlags = 0x20;
        return Item &&
            Item->GetObject() == Object &&
            !(Item->GetFlags() & InvalidFlags) &&
            Object->Class &&
            SDK::MemReadable(Object->Class, sizeof(UClass));
    }

    static uint64_t GetLiveObjectIdentity(
        const UObject* Object)
    {
        if (!IsLiveObject(Object))
            return 0;

        auto Item =
            TUObjectArray::GetItemByIndex(Object->Index);
        if (!Item)
            return 0;

        // Match TWeakObjectPtr construction before publishing an identity.
        // Some synthetic/runtime objects enter the array with serial zero;
        // allowing a later weak-pointer construction to assign that serial
        // would make an in-flight request look like address reuse.
        FWeakObjectPtr StableWeakIdentity(Object);
        const int32 Serial = StableWeakIdentity.ObjectSerialNumber;
        if (!Serial || Item->SerialRef() != Serial)
            return 0;

        return
            (static_cast<uint64_t>(
                static_cast<uint32_t>(Object->Index)) << 32) |
            static_cast<uint32_t>(Serial);
    }

    static uint64_t GetTargetIdentity(
        uintptr_t TargetToken)
    {
        return GetLiveObjectIdentity(
            reinterpret_cast<const UObject*>(
                TargetToken));
    }

    static void MarkTargetFailed(
        uintptr_t TargetToken)
    {
        const uint64_t Identity =
            GetTargetIdentity(TargetToken);
        if (Identity)
            GFailedControllerIdentities.insert(Identity);
    }

    static bool IsTargetFailed(
        uintptr_t TargetToken)
    {
        const uint64_t Identity =
            GetTargetIdentity(TargetToken);
        return Identity &&
            GFailedControllerIdentities.contains(Identity);
    }

    template <typename ElementType>
    static bool IsSafeArray(
        const TArray<ElementType>& Array,
        int32 ElementSize,
        int32 MaximumCount)
    {
        if (Array.NumElements < 0 ||
            Array.MaxElements < Array.NumElements ||
            Array.MaxElements > MaximumCount ||
            ElementSize <= 0 ||
            ElementSize > 0x4000)
        {
            return false;
        }

        if (Array.NumElements == 0)
            return true;
        if (!Array.Data)
            return false;

        const size_t Count =
            static_cast<size_t>(Array.NumElements);
        const size_t Size =
            static_cast<size_t>(ElementSize);
        if (Count > (std::numeric_limits<size_t>::max)() / Size)
            return false;

        return SDK::MemReadable(Array.Data, Count * Size);
    }

    static bool TryReadReflectedPropertyViewUnsafe(
        const UField* Property,
        FReflectedPropertyView& Out)
    {
        Out = {};
        if (!Property ||
            !Offsets::Offset_Internal ||
            Offsets::ElementSize < sizeof(int32) ||
            !Offsets::PropertyFlags)
        {
            return false;
        }

        size_t MetadataBytes =
            static_cast<size_t>(Offsets::Offset_Internal) +
            sizeof(uint32);
        MetadataBytes = (std::max)(
            MetadataBytes,
            static_cast<size_t>(Offsets::ElementSize) +
                sizeof(uint32));
        MetadataBytes = (std::max)(
            MetadataBytes,
            static_cast<size_t>(Offsets::PropertyFlags) +
                sizeof(uint64_t));
        if (MetadataBytes > 0x400 ||
            !SDK::MemReadable(Property, MetadataBytes))
        {
            return false;
        }

        Out.Offset = SDK::ReadPropertyOffset(
            GetFromOffset<uint32>(
                Property, Offsets::Offset_Internal));
        Out.ElementSize = GetFromOffset<uint32>(
            Property, Offsets::ElementSize);
        Out.ArrayDimension = GetFromOffset<int32>(
            Property,
            Offsets::ElementSize - sizeof(int32));
        Out.PropertyFlags = GetFromOffset<uint64_t>(
            Property, Offsets::PropertyFlags);
        return Out.Offset != UINT32_MAX &&
            Out.Offset <= 0x20000 &&
            Out.ElementSize > 0 &&
            Out.ElementSize <= 0x10000 &&
            Out.ArrayDimension > 0 &&
            Out.ArrayDimension <= 4096;
    }

    static bool TryReadReflectedPropertyView(
        const UField* Property,
        FReflectedPropertyView* Out)
    {
        __try
        {
            return TryReadReflectedPropertyViewUnsafe(
                Property, *Out);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *Out = {};
            return false;
        }
    }

    static AFortPlayerControllerAthena* ResolveRequestedController(
        uintptr_t TargetToken)
    {
        if (!TargetToken)
            return nullptr;

        auto Candidate =
            reinterpret_cast<AFortPlayerControllerAthena*>(TargetToken);
        if (!IsLiveObject(Candidate))
            return nullptr;

        auto ControllerClass =
            AFortPlayerControllerAthena::StaticClass();
        if (!ControllerClass || !Candidate->IsA(ControllerClass))
            return nullptr;

        auto World = UWorld::GetWorld();
        if (!IsLiveObject(World) ||
            !World->HasNetDriver() ||
            !World->NetDriver)
        {
            return nullptr;
        }

        auto Driver = static_cast<UNetDriver*>(World->NetDriver);
        if (!IsLiveObject(Driver) ||
            !Driver->HasClientConnections())
        {
            return nullptr;
        }

        auto& Connections = Driver->GetClientConnections();
        if (!IsSafeArray(
                Connections,
                sizeof(UNetConnection*),
                1024))
        {
            return nullptr;
        }

        for (int32 Index = 0;
             Index < Connections.Num();
             ++Index)
        {
            auto Connection = Connections[Index];
            if (!IsLiveObject(Connection) ||
                !Connection->HasPlayerController())
            {
                continue;
            }

            if (Connection->GetPlayerController() == Candidate)
                return Candidate;
        }

        return nullptr;
    }

    static AFortInventory* ResolveWorldInventory(
        AFortPlayerControllerAthena* PlayerController)
    {
        if (!IsLiveObject(PlayerController))
            return nullptr;

        auto Property =
            PlayerController->GetProperty("WorldInventory");
        if (!Property)
            return nullptr;

        const uint32 Offset = SDK::ReadPropertyOffset(
            GetFromOffset<uint32>(
                Property, Offsets::Offset_Internal));
        if (Offset == UINT32_MAX || Offset > 0x10000)
            return nullptr;

        auto Inventory =
            GetFromOffset<AFortInventory*>(
                PlayerController, Offset);
        if (!IsLiveObject(Inventory))
            return nullptr;

        auto InventoryClass = AFortInventory::StaticClass();
        if (!InventoryClass ||
            !Inventory->IsA(InventoryClass) ||
            !Inventory->HasInventory() ||
            (Inventory->HasOwner() &&
             Inventory->GetOwner() != PlayerController))
        {
            return nullptr;
        }

        return Inventory;
    }

    static bool HasSafeInventoryEntries(
        AFortInventory* Inventory,
        int32& EntrySize)
    {
        EntrySize = 0;
        if (!IsLiveObject(Inventory) ||
            !FFortItemList::StaticStruct() ||
            !FFortItemList::HasReplicatedEntries() ||
            !FFortItemEntry::StaticStruct())
        {
            return false;
        }

        EntrySize = FFortItemEntry::Size();
        if (EntrySize < 0x20 || EntrySize > 0x2000)
            return false;

        return IsSafeArray(
            Inventory->GetInventory()
                .GetReplicatedEntries(),
            EntrySize,
            4096);
    }

    static bool HasSafeMutationInventory(
        AFortInventory* Inventory,
        int32& EntrySize)
    {
        if (!HasSafeInventoryEntries(
                Inventory, EntrySize) ||
            !FFortItemList::HasItemInstances() ||
            Inventory->GetInventory()
                    .GetReplicatedEntries()
                    .Num() > 256)
        {
            return false;
        }

        auto& Instances =
            Inventory->GetInventory()
                .GetItemInstances();
        if (!IsSafeArray(
                Instances,
                sizeof(UFortWorldItem*),
                256))
        {
            return false;
        }
        for (int32 Index = 0;
             Index < Instances.Num();
             ++Index)
        {
            auto Instance = Instances[Index];
            if (Instance &&
                (!IsLiveObject(Instance) ||
                 !Instance->HasItemEntry()))
            {
                return false;
            }
        }
        return true;
    }

    enum class EExistingItemPlacement
    {
        Invalid,
        NonPrimary,
        Primary
    };

    static EExistingItemPlacement ClassifyExistingItemDefinition(
        const UFortItemDefinition* Definition)
    {
        if (!IsLiveObject(Definition) ||
            Definition->IsDefaultObject())
        {
            return EExistingItemPlacement::Invalid;
        }

        auto WorldItemClass =
            UFortWorldItemDefinition::StaticClass();
        if (!WorldItemClass ||
            !Definition->IsA(WorldItemClass))
        {
            return EExistingItemPlacement::Invalid;
        }

        if (Definition->HasItemType())
        {
            return AFortInventory::IsPrimaryQuickbar(Definition)
                ? EExistingItemPlacement::Primary
                : EExistingItemPlacement::NonPrimary;
        }

        // Very old or forked builds may omit ItemType. Only accept definitions
        // that explicitly advertise quickbar focus or are known weapon/gadget
        // classes; everything else fails closed.
        if (Definition->HasbForceIntoOverflow() &&
            Definition->bForceIntoOverflow)
        {
            return EExistingItemPlacement::NonPrimary;
        }
        if (Definition->HasbSupportsQuickbarFocus() &&
            Definition->bSupportsQuickbarFocus)
        {
            return EExistingItemPlacement::Primary;
        }

        auto WeaponClass = UFortWeaponItemDefinition::StaticClass();
        auto GadgetClass = UFortGadgetItemDefinition::StaticClass();
        return (
            (WeaponClass && Definition->IsA(WeaponClass)) ||
            (GadgetClass && Definition->IsA(GadgetClass)))
                ? EExistingItemPlacement::Primary
                : EExistingItemPlacement::NonPrimary;
    }

    static bool IsSafePrimaryItemDefinition(
        const UFortItemDefinition* Definition)
    {
        if (ClassifyExistingItemDefinition(Definition) !=
            EExistingItemPlacement::Primary)
        {
            return false;
        }
        if (Definition->HasbForceFocusWhenAdded() &&
            Definition->bForceFocusWhenAdded)
        {
            return false;
        }

        // IsPrimaryQuickbar deliberately accepts every item type it does not
        // recognise as a resource/ammo/building row. The loaded object array
        // also contains quest tokens, account-only world items and internal
        // backing definitions, so require the same focus contract used by a
        // real carried item whenever that contract exists on the build.
        if (Definition->HasbSupportsQuickbarFocus() &&
            !Definition->bSupportsQuickbarFocus)
        {
            return false;
        }

        auto ResourceClass =
            UFortResourceItemDefinition::StaticClass();
        auto AmmoClass =
            UFortAmmoItemDefinition::StaticClass();
        auto DecoClass =
            UFortDecoItemDefinition::StaticClass();
        auto BuildingClass =
            UFortBuildingItemDefinition::StaticClass();
        auto EditToolClass =
            UFortEditToolItemDefinition::StaticClass();
        if ((ResourceClass &&
             Definition->IsA(ResourceClass)) ||
            (AmmoClass &&
             Definition->IsA(AmmoClass)) ||
            (DecoClass &&
             Definition->IsA(DecoClass)) ||
            (BuildingClass &&
             Definition->IsA(BuildingClass)) ||
            (EditToolClass &&
             Definition->IsA(EditToolClass)))
        {
            return false;
        }

        if (auto Gadget =
                Definition->Cast<UFortGadgetItemDefinition>())
        {
            // Gadget grants can install abilities, backing rows, timers, or
            // force-focus behavior before placement is verified. Those effects
            // have no generic cross-version rollback API, so keep existing
            // gadgets visible but never offer them as admin-grant candidates.
            return false;
        }
        return true;
    }

    static const FFortItemEntry* FindEntryByGuid(
        AFortInventory* Inventory,
        int32 EntrySize,
        const FGuid& Guid)
    {
        auto& Entries =
            Inventory->GetInventory()
                .GetReplicatedEntries();
        for (int32 Index = 0;
             Index < Entries.Num();
             ++Index)
        {
            auto& Entry = Entries.Get(Index, EntrySize);
            if (AreGuidsEqual(Entry.ItemGuid, Guid))
                return &Entry;
        }
        return nullptr;
    }

    static FFortItemEntry* FindMutableEntryByGuid(
        AFortInventory* Inventory,
        int32 EntrySize,
        const FGuid& Guid)
    {
        return const_cast<FFortItemEntry*>(
            FindEntryByGuid(Inventory, EntrySize, Guid));
    }

    static bool HasOtherEntryForDefinition(
        AFortInventory* Inventory,
        int32 EntrySize,
        const UFortItemDefinition* Definition,
        const FGuid* ExcludedGuidA = nullptr,
        const FGuid* ExcludedGuidB = nullptr)
    {
        auto& Entries =
            Inventory->GetInventory()
                .GetReplicatedEntries();
        for (int32 Index = 0;
             Index < Entries.Num();
             ++Index)
        {
            auto& Entry =
                Entries.Get(Index, EntrySize);
            if (Entry.ItemDefinition != Definition)
                continue;
            if (ExcludedGuidA &&
                AreGuidsEqual(
                    Entry.ItemGuid, *ExcludedGuidA))
            {
                continue;
            }
            if (ExcludedGuidB &&
                AreGuidsEqual(
                    Entry.ItemGuid, *ExcludedGuidB))
            {
                continue;
            }
            return true;
        }
        return false;
    }

    static UFortWorldItem* FindItemInstanceByGuid(
        AFortInventory* Inventory,
        const FGuid& Guid)
    {
        auto& Instances =
            Inventory->GetInventory()
                .GetItemInstances();
        for (int32 Index = 0;
             Index < Instances.Num();
             ++Index)
        {
            auto Instance = Instances[Index];
            if (Instance &&
                AreGuidsEqual(
                    Instance->GetItemEntry()
                        .ItemGuid,
                    Guid))
            {
                return Instance;
            }
        }
        return nullptr;
    }

    static bool IsGuidInSlots(
        const FGuid& Guid,
        const std::array<FResolvedSlot, kSlotCount>& Slots);

    static void ResolveLegacySlots(
        AFortPlayerControllerAthena* PlayerController,
        AFortInventory* Inventory,
        int32 EntrySize,
        FResolvedLoadout& Out,
        const FGuid* IgnoredGuid)
    {
        if (VersionInfo.FortniteVersion >= 7.40f ||
            !PlayerController->HasQuickBars() ||
            !PlayerController->GetQuickBars() ||
            !IsLiveObject(
                PlayerController->GetQuickBars()) ||
            !FQuickBar::StaticStruct() ||
            !FQuickBar::HasSlots() ||
            !FQuickBarSlot::StaticStruct() ||
            !FQuickBarSlot::HasItems())
        {
            return;
        }

        auto QuickBars =
            PlayerController->GetQuickBars();
        if (QuickBars->HasOwner() &&
            QuickBars->GetOwner() != PlayerController)
        {
            return;
        }
        if (!QuickBars->HasPrimaryQuickBar() ||
            !QuickBars->GetFunction("EmptySlot") ||
            !QuickBars->GetFunction(
                "ServerRemoveItemInternal") ||
            !QuickBars->GetFunction(
                "ServerAddItemInternal"))
            return;

        auto& Slots =
            QuickBars->GetPrimaryQuickBar().GetSlots();
        const int32 SlotSize = FQuickBarSlot::Size();
        if (!IsSafeArray(Slots, SlotSize, 64) ||
            Slots.Num() < kSlotCount + 1)
        {
            return;
        }

        std::array<FResolvedSlot, kSlotCount> ResolvedSlots{};
        for (int LogicalSlot = 0;
             LogicalSlot < kSlotCount;
             ++LogicalSlot)
        {
            const int RawSlot = LogicalSlot + 1;
            if (RawSlot >= Slots.Num())
                break;

            auto& QuickbarSlot = Slots.Get(RawSlot, SlotSize);
            if (!IsSafeArray(
                    QuickbarSlot.GetItems(),
                    sizeof(FGuid),
                    16))
            {
                return;
            }
            FGuid Guid{};
            int RemainingItems = 0;
            for (const auto& CandidateGuid :
                 QuickbarSlot.GetItems())
            {
                if (IgnoredGuid &&
                    AreGuidsEqual(
                        CandidateGuid,
                        *IgnoredGuid))
                {
                    continue;
                }
                Guid = CandidateGuid;
                if (++RemainingItems > 1)
                    return;
            }
            if (RemainingItems == 0)
            {
                continue;
            }
            auto Entry =
                FindEntryByGuid(Inventory, EntrySize, Guid);
            if (!Entry ||
                ClassifyExistingItemDefinition(
                    Entry->ItemDefinition) !=
                    EExistingItemPlacement::Primary ||
                IsGuidInSlots(
                    Entry->ItemGuid,
                    ResolvedSlots))
            {
                return;
            }

            ResolvedSlots[LogicalSlot] = {
                true,
                Entry->ItemGuid,
                Entry->ItemDefinition,
                Entry->Count
            };
        }

        Out.Slots = ResolvedSlots;
        Out.HasLegacyQuickbar = true;
        Out.HasResolvedSlots = true;
        Out.HasAuthoritativeSlots = true;
        Out.CanMutateSlots = true;
    }

    struct FModernPrimaryRow
    {
        FResolvedSlot Slot;
        int Order = -1;
    };

    static bool IsGuidInRows(
        const FGuidValue& Guid,
        const std::vector<FModernPrimaryRow>& Rows)
    {
        if (Guid.IsZero())
            return false;
        for (const auto& Row : Rows)
        {
            if (AreGuidsEqual(Row.Slot.Guid, Guid))
                return true;
        }
        return false;
    }

    static bool IsGuidInSlots(
        const FGuid& Guid,
        const std::array<FResolvedSlot, kSlotCount>& Slots)
    {
        for (const auto& Slot : Slots)
        {
            if (Slot.Occupied &&
                AreGuidsEqual(Slot.Guid, Guid))
            {
                return true;
            }
        }
        return false;
    }

    static bool AreSlotMapsEqual(
        const std::array<FResolvedSlot, kSlotCount>& Left,
        const std::array<FResolvedSlot, kSlotCount>& Right)
    {
        for (int Slot = 0; Slot < kSlotCount; ++Slot)
        {
            if (Left[Slot].Occupied != Right[Slot].Occupied)
                return false;
            if (Left[Slot].Occupied &&
                !AreGuidsEqual(
                    Left[Slot].Guid,
                    Right[Slot].Guid))
            {
                return false;
            }
        }
        return true;
    }

    static bool TryResolveGuidWindow(
        const FGuid* RawGuids,
        int RawGuidCount,
        int RawBase,
        const std::vector<FModernPrimaryRow>& Rows,
        const FGuid* IgnoredGuid,
        std::array<FResolvedSlot, kSlotCount>& Result)
    {
        Result = {};
        if (!RawGuids ||
            RawGuidCount < 0 ||
            RawGuidCount > kReportedQuickbarCapacity ||
            RawBase < 0 ||
            RawBase + kSlotCount > RawGuidCount)
        {
            return false;
        }

        int ResolvedCount = 0;
        for (int Slot = 0; Slot < kSlotCount; ++Slot)
        {
            const auto& Guid = RawGuids[RawBase + Slot];
            if (ToGuidValue(Guid).IsZero() ||
                (IgnoredGuid &&
                 AreGuidsEqual(Guid, *IgnoredGuid)))
            {
                continue;
            }

            const FModernPrimaryRow* Match = nullptr;
            for (const auto& Row : Rows)
            {
                if (AreGuidsEqual(Row.Slot.Guid, Guid))
                {
                    Match = &Row;
                    break;
                }
            }
            if (!Match ||
                IsGuidInSlots(Match->Slot.Guid, Result))
            {
                return false;
            }

            Result[Slot] = Match->Slot;
            ++ResolvedCount;
        }

        if (Rows.size() <= kSlotCount)
        {
            if (ResolvedCount !=
                static_cast<int>(Rows.size()))
            {
                return false;
            }
            for (const auto& Row : Rows)
            {
                if (!IsGuidInSlots(
                        Row.Slot.Guid, Result))
                {
                    return false;
                }
            }
            return true;
        }

        // With overflow rows, every visible cell must resolve. Otherwise an
        // overflow item and a genuine player-visible hole are indistinguishable.
        return ResolvedCount == kSlotCount;
    }

    static bool SyncModernSlotLedger(
        const FResolvedLoadout& Loadout)
    {
        const uint64_t ControllerIdentity =
            GetLiveObjectIdentity(
                Loadout.PlayerController);
        const uint64_t InventoryIdentity =
            GetLiveObjectIdentity(
                Loadout.Inventory);
        if (!ControllerIdentity ||
            !InventoryIdentity)
        {
            return false;
        }

        auto& Ledger =
            GModernSlotLedgers[ControllerIdentity];
        Ledger = {};
        Ledger.InventoryIdentity =
            InventoryIdentity;
        if (Loadout.UsesReportedQuickbarSlots ||
            Loadout.UsesBridgeQuickbarSlots ||
            Loadout.UsesClientQuickbarSlots)
        {
            Ledger.LastExactClientAt =
                GetTickCount64();
        }
        for (int Slot = 0; Slot < kSlotCount; ++Slot)
        {
            if (Loadout.Slots[Slot].Occupied)
            {
                Ledger.Slots[Slot] =
                    ToGuidValue(
                        Loadout.Slots[Slot].Guid);
            }
        }
        return true;
    }

    struct FReportedQuickbarStateSchemaCache
    {
        const UClass* ControllerClass = nullptr;
        bool Initialized = false;
        bool Readable = false;
        bool Strict = false;
        uint32 Offset = UINT32_MAX;
		ULONGLONG NextRetryAt = 0;
    };

    static FReportedQuickbarStateSchemaCache
        GReportedQuickbarStateSchema;

    // Resolving the state offset walks reflection metadata. Cache the fully
    // validated result per controller class so an outstanding network
    // transaction can poll only the small POD state rather than reflecting on
    // every game frame.
    static bool EnsureReportedQuickbarStateSchemaUnsafe(
        AFortPlayerControllerAthena* PlayerController)
    {
        if (!PlayerController || !PlayerController->Class)
            return false;
        if (GReportedQuickbarStateSchema.ControllerClass ==
                PlayerController->Class &&
            GReportedQuickbarStateSchema.Readable)
        {
			return true;
        }
		const ULONGLONG Now = GetTickCount64();
		if (GReportedQuickbarStateSchema.ControllerClass ==
				PlayerController->Class &&
			GReportedQuickbarStateSchema.Initialized &&
			Now < GReportedQuickbarStateSchema.NextRetryAt)
		{
			return false;
		}

        GReportedQuickbarStateSchema = {};
        GReportedQuickbarStateSchema.ControllerClass =
            PlayerController->Class;
        GReportedQuickbarStateSchema.Initialized = true;
		GReportedQuickbarStateSchema.NextRetryAt =
			Now + kReflectionSchemaRetryMs;

        auto StateProperty = PlayerController->GetProperty(
            "PrimaryQuickBarSlotItemGuids",
            kCastClassStructProperty);
        if (!StateProperty)
            return false;

        auto StateStruct = SDK::FindStruct(
            "QuickBarEquippedItemGuids");
        if (StateStruct &&
            (!StateStruct->Class ||
             StateStruct->Class->Name.ToUtf8() !=
                 "ScriptStruct"))
        {
            StateStruct = nullptr;
        }

        uint32 StateOffset = UINT32_MAX;
        if (StateStruct)
        {
            auto GuidsProperty = StateStruct->GetProperty(
                "EquippedItemGuids");
            auto CountProperty = StateStruct->GetProperty(
                "NumEnabledSlots");
            FReflectedPropertyView StateView;
            FReflectedPropertyView GuidsView;
            FReflectedPropertyView CountView;
            const bool SchemaMatches =
                GuidsProperty &&
                CountProperty &&
                TryReadReflectedPropertyViewUnsafe(
                    StateProperty, StateView) &&
                TryReadReflectedPropertyViewUnsafe(
                    GuidsProperty, GuidsView) &&
                TryReadReflectedPropertyViewUnsafe(
                    CountProperty, CountView) &&
                StateView.ElementSize ==
                    sizeof(FReportedQuickbarGuidState) &&
                StateView.ArrayDimension == 1 &&
                GuidsView.Offset == 0 &&
                GuidsView.ElementSize == sizeof(FGuid) &&
                GuidsView.ArrayDimension ==
                    kReportedQuickbarCapacity &&
                CountView.Offset ==
                    offsetof(
                        FReportedQuickbarGuidState,
                        NumEnabledSlots) &&
                CountView.ElementSize == sizeof(int32) &&
                CountView.ArrayDimension == 1 &&
                StateStruct->GetPropertiesSize() ==
                    sizeof(FReportedQuickbarGuidState);
            if (SchemaMatches)
            {
                const int32 ControllerSize =
                    PlayerController->Class
                        ->GetPropertiesSize();
                if (ControllerSize > 0 &&
                    ControllerSize <= 0x20000 &&
                    StateView.Offset <=
                        static_cast<uint32>(
                            ControllerSize) &&
                    sizeof(FReportedQuickbarGuidState) <=
                        static_cast<size_t>(
                            ControllerSize) -
                            StateView.Offset)
                {
                    StateOffset = StateView.Offset;
                    GReportedQuickbarStateSchema.Strict = true;
                }
            }
        }

        if (StateOffset == UINT32_MAX)
            return false;

        GReportedQuickbarStateSchema.Offset = StateOffset;
        GReportedQuickbarStateSchema.Readable = true;
		GReportedQuickbarStateSchema.NextRetryAt = 0;
        return true;
    }

    static bool TryReadReportedQuickbarStateUnsafe(
        AFortPlayerControllerAthena* PlayerController,
        FReportedQuickbarGuidState& State,
        bool& StrictSchemaVerified)
    {
        State = {};
        StrictSchemaVerified = false;
        if (!EnsureReportedQuickbarStateSchemaUnsafe(
                PlayerController))
        {
            return false;
        }

        const auto StateAddress =
            reinterpret_cast<const uint8_t*>(
                PlayerController) +
            GReportedQuickbarStateSchema.Offset;
        if (!SDK::MemReadable(
                StateAddress,
                sizeof(FReportedQuickbarGuidState)))
        {
            return false;
        }
        memcpy(&State, StateAddress, sizeof(State));
        StrictSchemaVerified =
            GReportedQuickbarStateSchema.Strict;
        if (State.NumEnabledSlots < kSlotCount ||
            State.NumEnabledSlots >
                kReportedQuickbarCapacity)
        {
            return false;
        }
        for (int Left = 0;
             Left < State.NumEnabledSlots;
             ++Left)
        {
            if (ToGuidValue(
                    State.EquippedItemGuids[Left])
                    .IsZero())
            {
                continue;
            }
            for (int Right = Left + 1;
                 Right < State.NumEnabledSlots;
                 ++Right)
            {
                if (AreGuidsEqual(
                        State.EquippedItemGuids[Left],
                        State.EquippedItemGuids[Right]))
                {
                    return false;
                }
            }
        }
        return true;
    }

    static bool HasSafeReportedQuickbarWritePath(
        AFortPlayerControllerAthena* PlayerController);

    static bool TryResolveReportedQuickbarSlotsUnsafe(
        AFortPlayerControllerAthena* PlayerController,
        const std::vector<FModernPrimaryRow>& Rows,
        const FGuid* IgnoredGuid,
        std::array<FResolvedSlot, kSlotCount>& Result,
        bool& StrictSchemaVerified,
        int& RawBase,
        int& EnabledSlots)
    {
        Result = {};
        StrictSchemaVerified = false;
        RawBase = -1;
        EnabledSlots = 0;
        FReportedQuickbarGuidState State{};
        if (!TryReadReportedQuickbarStateUnsafe(
                PlayerController,
                State,
                StrictSchemaVerified) ||
            (!StrictSchemaVerified && Rows.empty()))
        {
            return false;
        }
        EnabledSlots = State.NumEnabledSlots;

        if (State.NumEnabledSlots >= kSlotCount + 1)
        {
            // The normal shape includes the pickaxe at raw index zero and the
            // five combat cells at 1..5.
            const bool Resolved = TryResolveGuidWindow(
                State.EquippedItemGuids,
                kReportedQuickbarCapacity,
                1,
                Rows,
                IgnoredGuid,
                Result);
            if (Resolved)
                RawBase = 1;
            return Resolved;
        }

        // Some forks report five compact combat cells at 0..4, while stock
        // quickbar APIs on other builds report a count of five but retain the
        // canonical 1..5 indexing. Validate both against live inventory rows
        // and accept a layout only when that evidence is unambiguous.
        std::array<FResolvedSlot, kSlotCount>
            Compatibility{};
        std::array<FResolvedSlot, kSlotCount>
            Canonical{};
        const bool CompatibilityValid =
            TryResolveGuidWindow(
                State.EquippedItemGuids,
                kReportedQuickbarCapacity,
                0,
                Rows,
                IgnoredGuid,
                Compatibility);
        const bool CanonicalValid =
            TryResolveGuidWindow(
                State.EquippedItemGuids,
                kReportedQuickbarCapacity,
                1,
                Rows,
                IgnoredGuid,
                Canonical);
        if (CompatibilityValid && CanonicalValid &&
            !AreSlotMapsEqual(
                Compatibility, Canonical))
        {
            // An edge hole can make both overlapping windows contain the same
            // GUID set at different positions. Without a build-specific shape
            // signal, claiming either order would be worse than a read-only
            // fallback.
            return false;
        }
        if (CompatibilityValid)
        {
            Result = Compatibility;
            // If both overlapping layouts validate to the same visible map,
            // rendering is harmless but the raw write target is ambiguous.
            // Keep it read-only by withholding a fixed base.
            if (!CanonicalValid)
                RawBase = 0;
            return true;
        }
        if (CanonicalValid)
        {
            Result = Canonical;
            RawBase = 1;
            return true;
        }
        return false;
    }

    static bool TryResolveReportedQuickbarSlots(
        AFortPlayerControllerAthena* PlayerController,
        const std::vector<FModernPrimaryRow>& Rows,
        const FGuid* IgnoredGuid,
        FResolvedLoadout& Out)
    {
        std::array<FResolvedSlot, kSlotCount> Slots{};
        bool Resolved = false;
        bool StrictSchemaVerified = false;
        int RawBase = -1;
        int EnabledSlots = 0;
        __try
        {
            Resolved = TryResolveReportedQuickbarSlotsUnsafe(
                PlayerController,
                Rows,
                IgnoredGuid,
                Slots,
                StrictSchemaVerified,
                RawBase,
                EnabledSlots);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Resolved = false;
            Slots = {};
            StrictSchemaVerified = false;
            RawBase = -1;
            EnabledSlots = 0;
        }
        if (!Resolved)
            return false;

        Out.Slots = Slots;
        Out.UsesReportedQuickbarSlots = true;
        Out.HasResolvedSlots = true;
        Out.HasAuthoritativeSlots = true;
        Out.HasStrictReportedQuickbarSchema =
            StrictSchemaVerified;
        Out.ReportedQuickbarRawBase = RawBase;
        Out.ReportedQuickbarEnabledSlots =
            EnabledSlots;
        // A reported map becomes editable only when its complete POD layout,
        // fixed raw window, and both write functions have been independently
        // validated. Completion is still asynchronous and is handled by the
        // transaction poller; this flag is capability, never an acknowledgement.
        Out.CanMutateSlots =
            StrictSchemaVerified &&
            // The validated writer ABI addresses stock raw cells 1..5.  A
            // compact 0..4 compatibility map remains exact display data but
            // must not be written with Slot+1 semantics.
            RawBase == 1 &&
            HasSafeReportedQuickbarWritePath(
                PlayerController);
        SyncModernSlotLedger(Out);
        return true;
    }

    static bool TryResolveBridgeQuickbarSlots(
        AFortPlayerControllerAthena* PlayerController,
        const std::vector<FModernPrimaryRow>& Rows,
        const FGuid* IgnoredGuid,
        FResolvedLoadout& Out)
    {
        PlayerLoadoutBridgeServer::FSnapshot Snapshot{};
        if (!PlayerLoadoutBridgeServer::TryGetLatestSnapshot(
                PlayerController,
                Snapshot,
                kBridgeSnapshotMaxAgeMs) ||
            !Snapshot.Session ||
            !Snapshot.Generation)
        {
            return false;
        }

        std::array<FResolvedSlot, kSlotCount> Resolved{};
        int MappedRows = 0;
        for (int Slot = 0; Slot < kSlotCount; ++Slot)
        {
            const auto& WireGuid = Snapshot.Slots[Slot];
            const FGuidValue Guid{
                static_cast<int32>(WireGuid.A),
                static_cast<int32>(WireGuid.B),
                static_cast<int32>(WireGuid.C),
                static_cast<int32>(WireGuid.D)
            };
            if (Guid.IsZero() ||
                (IgnoredGuid && AreGuidsEqual(*IgnoredGuid, Guid)))
            {
                continue;
            }

            const FModernPrimaryRow* Match = nullptr;
            for (const auto& Row : Rows)
            {
                if (AreGuidsEqual(Row.Slot.Guid, Guid))
                {
                    Match = &Row;
                    break;
                }
            }
            if (!Match || IsGuidInSlots(Match->Slot.Guid, Resolved))
                return false;

            Resolved[Slot] = Match->Slot;
            ++MappedRows;
        }

        if (Rows.size() <= kSlotCount)
        {
            if (MappedRows != static_cast<int>(Rows.size()))
                return false;
            for (const auto& Row : Rows)
            {
                if (!IsGuidInSlots(Row.Slot.Guid, Resolved))
                    return false;
            }
        }

        Out.Slots = Resolved;
        Out.UsesBridgeQuickbarSlots = true;
        Out.HasResolvedSlots = true;
        Out.HasAuthoritativeSlots = true;
        Out.BridgeSession = Snapshot.Session;
        Out.BridgeGeneration = Snapshot.Generation;
        Out.CanMutateSlots =
            HasSafeReportedQuickbarWritePath(PlayerController);
        SyncModernSlotLedger(Out);
        return true;
    }

    static bool ResolveRecentClientSlotLedger(
        const std::vector<FModernPrimaryRow>& Rows,
        FResolvedLoadout& Out)
    {
        const uint64_t ControllerIdentity =
            GetLiveObjectIdentity(
                Out.PlayerController);
        const uint64_t InventoryIdentity =
            GetLiveObjectIdentity(
                Out.Inventory);
        const auto Existing =
            GModernSlotLedgers.find(
                ControllerIdentity);
        const ULONGLONG Now = GetTickCount64();
        if (!ControllerIdentity ||
            !InventoryIdentity ||
            Existing == GModernSlotLedgers.end() ||
            Existing->second.InventoryIdentity !=
                InventoryIdentity ||
            !Existing->second.LastExactClientAt ||
            Now - Existing->second.LastExactClientAt >
                900)
        {
            return false;
        }

        std::array<FResolvedSlot, kSlotCount>
            StableSlots{};
        int MappedRows = 0;
        for (int Slot = 0;
             Slot < kSlotCount;
             ++Slot)
        {
            const auto& Guid =
                Existing->second.Slots[Slot];
            if (Guid.IsZero())
                continue;
            bool Found = false;
            for (const auto& Row : Rows)
            {
                if (AreGuidsEqual(
                        Row.Slot.Guid,
                        Guid))
                {
                    StableSlots[Slot] =
                        Row.Slot;
                    ++MappedRows;
                    Found = true;
                    break;
                }
            }
            if (!Found)
                return false;
        }
        if ((Rows.size() <= kSlotCount &&
             MappedRows !=
                 static_cast<int>(Rows.size())) ||
            (Rows.size() > kSlotCount &&
             MappedRows != kSlotCount))
        {
            return false;
        }

        Out.Slots = StableSlots;
        Out.UsesFallbackSlots = true;
        Out.HasResolvedSlots = true;
        Out.HasAuthoritativeSlots = false;
        return true;
    }

    static bool TryResolveDirectClientQuickbarSlotsUnsafe(
        AFortQuickBars* ClientQuickbars,
        const std::vector<FModernPrimaryRow>& Rows,
        const FGuid* IgnoredGuid,
        std::array<FResolvedSlot, kSlotCount>& Result)
    {
        Result = {};
        if (!ClientQuickbars ||
            !FQuickBar::StaticStruct() ||
            !FQuickBar::HasSlots() ||
            !FQuickBarSlot::StaticStruct() ||
            !FQuickBarSlot::HasItems() ||
            !ClientQuickbars->HasPrimaryQuickBar())
        {
            return false;
        }

        auto& RawSlots =
            ClientQuickbars->GetPrimaryQuickBar()
                .GetSlots();
        const int32 SlotSize = FQuickBarSlot::Size();
        if (!IsSafeArray(RawSlots, SlotSize, 64))
            return false;

        const auto TryWindow =
            [&](int RawBase,
                std::array<FResolvedSlot, kSlotCount>&
                    Candidate)
            {
                Candidate = {};
                if (RawBase < 0 ||
                    RawBase + kSlotCount >
                        RawSlots.Num())
                {
                    return false;
                }

                int ResolvedCount = 0;
                for (int LogicalSlot = 0;
                     LogicalSlot < kSlotCount;
                     ++LogicalSlot)
                {
                    auto& RawSlot = RawSlots.Get(
                        RawBase + LogicalSlot,
                        SlotSize);
                    if (!IsSafeArray(
                            RawSlot.GetItems(),
                            sizeof(FGuid),
                            16))
                    {
                        return false;
                    }

                    const FModernPrimaryRow* Match =
                        nullptr;
                    for (const auto& Guid :
                         RawSlot.GetItems())
                    {
                        if (IgnoredGuid &&
                            AreGuidsEqual(
                                Guid,
                                *IgnoredGuid))
                        {
                            continue;
                        }
                        if (Match)
                        {
                            // Multiple surviving inventory rows in one combat
                            // cell are not an exact player-visible mapping.
                            return false;
                        }
                        for (const auto& Row : Rows)
                        {
                            if (AreGuidsEqual(
                                    Row.Slot.Guid,
                                    Guid))
                            {
                                Match = &Row;
                                break;
                            }
                        }
                        if (!Match)
                            return false;
                    }

                    if (!Match)
                        continue;
                    if (IsGuidInSlots(
                            Match->Slot.Guid,
                            Candidate))
                    {
                        return false;
                    }
                    Candidate[LogicalSlot] =
                        Match->Slot;
                    ++ResolvedCount;
                }

                if (Rows.size() <= kSlotCount)
                {
                    if (ResolvedCount !=
                        static_cast<int>(Rows.size()))
                    {
                        return false;
                    }
                    for (const auto& Row : Rows)
                    {
                        if (!IsGuidInSlots(
                                Row.Slot.Guid,
                                Candidate))
                        {
                            return false;
                        }
                    }
                    return true;
                }

                // With overflow rows, require all five visible cells. Otherwise
                // an overflow item and a genuine hotbar hole are ambiguous.
                return ResolvedCount == kSlotCount;
            };

        std::array<FResolvedSlot, kSlotCount> Candidate{};
        // The canonical modern layout keeps the harvesting tool in raw cell
        // zero and the five combat cells in 1..5.
        if (TryWindow(1, Candidate))
        {
            Result = Candidate;
            return true;
        }

        // A few forks expose combat cells as 0..4. Accept that compatibility
        // window only when raw cell zero itself resolves to a primary row.
        if (TryWindow(0, Candidate) &&
            Candidate[0].Occupied)
        {
            Result = Candidate;
            return true;
        }
        return false;
    }

    static bool TryResolveDirectClientQuickbarSlots(
        AFortQuickBars* ClientQuickbars,
        const std::vector<FModernPrimaryRow>& Rows,
        const FGuid* IgnoredGuid,
        std::array<FResolvedSlot, kSlotCount>* Result)
    {
        __try
        {
            return TryResolveDirectClientQuickbarSlotsUnsafe(
                ClientQuickbars,
                Rows,
                IgnoredGuid,
                *Result);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *Result = {};
            return false;
        }
    }

    struct FQuickbarGetterSchema
    {
        UFunction* Function = nullptr;
        uint32 BufferSize = 0;
        bool HasSlotIndex = false;
        FReflectedPropertyView QuickbarType;
        FReflectedPropertyView SlotIndex;
        FReflectedPropertyView ReturnValue;
    };

    enum class EClientQuickbarProbeMode : uint8
    {
        DirectOnly,
        GettersOnly
    };

    static bool PropertyRangesOverlap(
        const FReflectedPropertyView& Left,
        const FReflectedPropertyView& Right)
    {
        const uint64_t LeftEnd =
            static_cast<uint64_t>(Left.Offset) +
            Left.ElementSize;
        const uint64_t RightEnd =
            static_cast<uint64_t>(Right.Offset) +
            Right.ElementSize;
        return Left.Offset < RightEnd &&
            Right.Offset < LeftEnd;
    }

    static bool HasExpectedFunctionParameterCountUnsafe(
        UFunction* Function,
        int ExpectedCount)
    {
        int Count = 0;
        int Guard = 0;
        const UField* Property =
            VersionInfo.FortniteVersion >= 12.10
                ? Function->GetChildProperties()
                : Function->GetChildren();
        while (Property && Guard++ < 32)
        {
            FReflectedPropertyView View;
            if (!TryReadReflectedPropertyViewUnsafe(
                    Property, View))
            {
                return false;
            }
            if ((View.PropertyFlags & kCpfParm) != 0)
                ++Count;
            Property =
                VersionInfo.FortniteVersion >= 12.10
                    ? Property->FField_GetNext()
                    : Property->GetNext();
        }
        return !Property && Count == ExpectedCount;
    }

    static bool TryBuildQuickbarGetterSchemaUnsafe(
        UFunction* Function,
        bool HasSlotIndex,
        uint32 ReturnSize,
        FQuickbarGetterSchema& Out)
    {
        Out = {};
        if (!Function)
            return false;

        const int32 BufferSize =
            Function->GetPropertiesSize();
        if (BufferSize <= 0 || BufferSize > 0x100)
            return false;

        auto QuickbarProperty =
            Function->GetProperty("QuickBarType");
        if (!QuickbarProperty)
        {
            QuickbarProperty =
                Function->GetProperty("QuickbarType");
        }
        auto SlotProperty = HasSlotIndex
            ? Function->GetProperty("SlotIndex")
            : nullptr;
        auto ReturnProperty =
            Function->GetProperty(
                "ReturnValue",
                kCastClassSoftObjectProperty);
        if (!QuickbarProperty ||
            (HasSlotIndex && !SlotProperty) ||
            !ReturnProperty)
        {
            return false;
        }

        FReflectedPropertyView QuickbarView;
        FReflectedPropertyView SlotView;
        FReflectedPropertyView ReturnView;
        if (!TryReadReflectedPropertyViewUnsafe(
                QuickbarProperty, QuickbarView) ||
            (HasSlotIndex &&
             !TryReadReflectedPropertyViewUnsafe(
                 SlotProperty, SlotView)) ||
            !TryReadReflectedPropertyViewUnsafe(
                ReturnProperty, ReturnView))
        {
            return false;
        }

        const auto IsInput = [](const auto& View)
        {
            return View.ArrayDimension == 1 &&
                (View.PropertyFlags & kCpfParm) != 0 &&
                (View.PropertyFlags &
                    (kCpfOutParm | kCpfReturnParm)) == 0;
        };
        const bool ReturnValid =
            ReturnView.ArrayDimension == 1 &&
            ReturnView.ElementSize == ReturnSize &&
            (ReturnView.PropertyFlags & kCpfParm) != 0 &&
            (ReturnView.PropertyFlags &
                kCpfReturnParm) != 0;
        if (!IsInput(QuickbarView) ||
            (QuickbarView.ElementSize != sizeof(uint8) &&
             QuickbarView.ElementSize != sizeof(int32)) ||
            (HasSlotIndex &&
             (!IsInput(SlotView) ||
              SlotView.ElementSize != sizeof(int32))) ||
            !ReturnValid)
        {
            return false;
        }

        const auto Fits = [BufferSize](const auto& View)
        {
            return View.Offset <
                    static_cast<uint32>(BufferSize) &&
                View.ElementSize <=
                    static_cast<uint32>(BufferSize) -
                        View.Offset;
        };
        if (!Fits(QuickbarView) ||
            (HasSlotIndex && !Fits(SlotView)) ||
            !Fits(ReturnView) ||
            PropertyRangesOverlap(
                QuickbarView, ReturnView) ||
            (HasSlotIndex &&
             (PropertyRangesOverlap(
                  QuickbarView, SlotView) ||
              PropertyRangesOverlap(
                  SlotView, ReturnView))) ||
            !HasExpectedFunctionParameterCountUnsafe(
                Function,
                HasSlotIndex ? 3 : 2))
        {
            return false;
        }

        Out.Function = Function;
        Out.BufferSize =
            static_cast<uint32>(BufferSize);
        Out.HasSlotIndex = HasSlotIndex;
        Out.QuickbarType = QuickbarView;
        Out.SlotIndex = SlotView;
        Out.ReturnValue = ReturnView;
        return true;
    }

    static bool InvokeQuickbarCountUnsafe(
        AFortPlayerControllerAthena* PlayerController,
        const FQuickbarGetterSchema& Schema,
        int32& Result)
    {
        alignas(16) uint8 Params[0x100]{};
        PlayerController->ProcessEvent(
            Schema.Function, Params);
        memcpy(
            &Result,
            Params + Schema.ReturnValue.Offset,
            sizeof(Result));
        return true;
    }

    static bool TryInvokeQuickbarCount(
        AFortPlayerControllerAthena* PlayerController,
        const FQuickbarGetterSchema& Schema,
        int32* Result)
    {
        __try
        {
            return InvokeQuickbarCountUnsafe(
                PlayerController, Schema, *Result);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *Result = 0;
            return false;
        }
    }

    static bool InvokeQuickbarItemUnsafe(
        AFortPlayerControllerAthena* PlayerController,
        const FQuickbarGetterSchema& Schema,
        int32 RawSlot,
        UObject*& Result)
    {
        alignas(16) uint8 Params[0x100]{};
        memcpy(
            Params + Schema.SlotIndex.Offset,
            &RawSlot,
            sizeof(RawSlot));
        PlayerController->ProcessEvent(
            Schema.Function, Params);
        memcpy(
            &Result,
            Params + Schema.ReturnValue.Offset,
            sizeof(Result));
        return true;
    }

    static bool TryInvokeQuickbarItem(
        AFortPlayerControllerAthena* PlayerController,
        const FQuickbarGetterSchema& Schema,
        int32 RawSlot,
        UObject** Result)
    {
        __try
        {
            return InvokeQuickbarItemUnsafe(
                PlayerController,
                Schema,
                RawSlot,
                *Result);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *Result = nullptr;
            return false;
        }
    }

    struct FReportedQuickbarWriterSchema
    {
        const UClass* ControllerClass = nullptr;
        bool Initialized = false;
        bool Valid = false;
        UFunction* AddFunction = nullptr;
        UFunction* RemoveFunction = nullptr;
        uint32 AddBufferSize = 0;
        uint32 RemoveBufferSize = 0;
        uint32 AddDefinitionOffset = UINT32_MAX;
        uint32 AddQuickbarOffset = UINT32_MAX;
        uint32 AddSlotOffset = UINT32_MAX;
        uint32 RemoveDefinitionOffset = UINT32_MAX;
		ULONGLONG NextRetryAt = 0;
    };

    static FReportedQuickbarWriterSchema
        GReportedQuickbarWriterSchema;

    static bool HasExactNamedFunctionPropertiesUnsafe(
        UFunction* Function,
        const char* const* ExpectedNames,
        int ExpectedCount)
    {
        if (!Function || !ExpectedNames ||
            ExpectedCount <= 0 || ExpectedCount > 8)
        {
            return false;
        }

        std::array<bool, 8> Found{};
        int Count = 0;
        int Guard = 0;
        const UField* Property =
            VersionInfo.FortniteVersion >= 12.10
                ? Function->GetChildProperties()
                : Function->GetChildren();
        while (Property && Guard++ < 16)
        {
            const auto ReflectedName =
                VersionInfo.FortniteVersion >= 12.10
                    ? Property->FField_GetName()
                          .ToSDKString()
                    : Property->GetName()
                          .ToSDKString();
            const std::string Name(
                ReflectedName.c_str());
            int Match = -1;
            for (int Index = 0;
                 Index < ExpectedCount;
                 ++Index)
            {
                if (!Found[Index] &&
                    Name == ExpectedNames[Index])
                {
                    Match = Index;
                    break;
                }
            }
            if (Match < 0)
                return false;
            Found[Match] = true;
            ++Count;
            Property =
                VersionInfo.FortniteVersion >= 12.10
                    ? Property->FField_GetNext()
                    : Property->GetNext();
        }
        if (Property || Count != ExpectedCount)
            return false;
        for (int Index = 0;
             Index < ExpectedCount;
             ++Index)
        {
            if (!Found[Index])
                return false;
        }
        return true;
    }

    static bool EnsureReportedQuickbarWriterSchemaUnsafe(
        AFortPlayerControllerAthena* PlayerController)
    {
        if (!PlayerController || !PlayerController->Class)
            return false;
        if (GReportedQuickbarWriterSchema.ControllerClass ==
                PlayerController->Class &&
            GReportedQuickbarWriterSchema.Valid)
        {
			return true;
        }
		const ULONGLONG Now = GetTickCount64();
		if (GReportedQuickbarWriterSchema.ControllerClass ==
				PlayerController->Class &&
			GReportedQuickbarWriterSchema.Initialized &&
			Now < GReportedQuickbarWriterSchema.NextRetryAt)
		{
			return false;
		}

        GReportedQuickbarWriterSchema = {};
        auto& Schema = GReportedQuickbarWriterSchema;
        Schema.ControllerClass = PlayerController->Class;
        Schema.Initialized = true;
		Schema.NextRetryAt = Now + kReflectionSchemaRetryMs;
        Schema.AddFunction =
            PlayerController->GetFunction(
                "AddItemToQuickBars");
        Schema.RemoveFunction =
            PlayerController->GetFunction(
                "RemoveItemFromQuickBars");
        if (!Schema.AddFunction ||
            !Schema.RemoveFunction)
        {
            return false;
        }

        auto AddDefinition =
            Schema.AddFunction->GetProperty(
                "ItemDefinition");
        auto AddQuickbar =
            Schema.AddFunction->GetProperty(
                "QuickBarType");
        if (!AddQuickbar)
        {
            AddQuickbar =
                Schema.AddFunction->GetProperty(
                    "QuickbarType");
        }
        auto AddSlot =
            Schema.AddFunction->GetProperty(
                "SlotIndex");
        auto RemoveDefinition =
            Schema.RemoveFunction->GetProperty(
                "ItemDefinition");
        if (!AddDefinition || !AddQuickbar ||
            !AddSlot || !RemoveDefinition)
        {
            return false;
        }

        const char* AddNames[] = {
            "ItemDefinition", "QuickBarType", "SlotIndex"
        };
        const char* AddCompatibilityNames[] = {
            "ItemDefinition", "QuickbarType", "SlotIndex"
        };
        const char* RemoveNames[] = {
            "ItemDefinition"
        };
        if ((!HasExactNamedFunctionPropertiesUnsafe(
                 Schema.AddFunction,
                 AddNames,
                 3) &&
             !HasExactNamedFunctionPropertiesUnsafe(
                 Schema.AddFunction,
                 AddCompatibilityNames,
                 3)) ||
            !HasExactNamedFunctionPropertiesUnsafe(
                Schema.RemoveFunction,
                RemoveNames,
                1))
        {
            return false;
        }

        FReflectedPropertyView AddDefinitionView;
        FReflectedPropertyView AddQuickbarView;
        FReflectedPropertyView AddSlotView;
        FReflectedPropertyView RemoveDefinitionView;
        if (!TryReadReflectedPropertyViewUnsafe(
                AddDefinition, AddDefinitionView) ||
            !TryReadReflectedPropertyViewUnsafe(
                AddQuickbar, AddQuickbarView) ||
            !TryReadReflectedPropertyViewUnsafe(
                AddSlot, AddSlotView) ||
            !TryReadReflectedPropertyViewUnsafe(
                RemoveDefinition,
                RemoveDefinitionView))
        {
            return false;
        }
        const auto IsInput = [](const auto& View)
        {
            return View.ArrayDimension == 1 &&
                (View.PropertyFlags & kCpfParm) != 0 &&
                (View.PropertyFlags &
                    (kCpfOutParm | kCpfReturnParm)) == 0;
        };
        if (!IsInput(AddDefinitionView) ||
            AddDefinitionView.ElementSize !=
                sizeof(UFortItemDefinition*) ||
            !IsInput(AddQuickbarView) ||
            (AddQuickbarView.ElementSize != sizeof(uint8) &&
             AddQuickbarView.ElementSize != sizeof(int32)) ||
            !IsInput(AddSlotView) ||
            AddSlotView.ElementSize != sizeof(int32) ||
            !IsInput(RemoveDefinitionView) ||
            RemoveDefinitionView.ElementSize !=
                sizeof(UFortItemDefinition*))
        {
            return false;
        }

        const int32 AddBufferSize =
            Schema.AddFunction->GetPropertiesSize();
        const int32 RemoveBufferSize =
            Schema.RemoveFunction->GetPropertiesSize();
        const auto Fits = [](int32 BufferSize,
                             const auto& View)
        {
            return BufferSize > 0 &&
                BufferSize <= 0x100 &&
                View.Offset <
                    static_cast<uint32>(BufferSize) &&
                View.ElementSize <=
                    static_cast<uint32>(BufferSize) -
                        View.Offset;
        };
        if (!Fits(AddBufferSize, AddDefinitionView) ||
            !Fits(AddBufferSize, AddQuickbarView) ||
            !Fits(AddBufferSize, AddSlotView) ||
            !Fits(RemoveBufferSize,
                  RemoveDefinitionView) ||
            PropertyRangesOverlap(
                AddDefinitionView, AddQuickbarView) ||
            PropertyRangesOverlap(
                AddDefinitionView, AddSlotView) ||
            PropertyRangesOverlap(
                AddQuickbarView, AddSlotView))
        {
            return false;
        }

        Schema.AddBufferSize =
            static_cast<uint32>(AddBufferSize);
        Schema.RemoveBufferSize =
            static_cast<uint32>(RemoveBufferSize);
        Schema.AddDefinitionOffset =
            AddDefinitionView.Offset;
        Schema.AddQuickbarOffset =
            AddQuickbarView.Offset;
        Schema.AddSlotOffset = AddSlotView.Offset;
        Schema.RemoveDefinitionOffset =
            RemoveDefinitionView.Offset;
        Schema.Valid = true;
		Schema.NextRetryAt = 0;
        return true;
    }

    static bool HasSafeReportedQuickbarWritePath(
        AFortPlayerControllerAthena* PlayerController)
    {
        __try
        {
            return EnsureReportedQuickbarWriterSchemaUnsafe(
                PlayerController);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool IssueReportedQuickbarWriteUnsafe(
        AFortPlayerControllerAthena* PlayerController,
        const UFortItemDefinition* Definition,
        int Slot,
        bool Clear,
        bool& DispatchMayHaveStarted)
    {
        DispatchMayHaveStarted = false;
        if (!IsLiveObject(PlayerController) ||
            !IsLiveObject(Definition) ||
            Slot < 0 || Slot >= kSlotCount ||
            !EnsureReportedQuickbarWriterSchemaUnsafe(
                PlayerController))
        {
            return false;
        }

        auto& Schema = GReportedQuickbarWriterSchema;
        alignas(16) uint8 Params[0x100]{};
        auto MutableDefinition =
            const_cast<UFortItemDefinition*>(Definition);
        if (Clear)
        {
            if (Schema.RemoveBufferSize > sizeof(Params))
                return false;
            memcpy(
                Params + Schema.RemoveDefinitionOffset,
                &MutableDefinition,
                sizeof(MutableDefinition));
            DispatchMayHaveStarted = true;
            PlayerController->ProcessEvent(
                Schema.RemoveFunction, Params);
        }
        else
        {
            if (Schema.AddBufferSize > sizeof(Params))
                return false;
            const uint8 PrimaryQuickbar = 0;
            const int32 RawSlot = Slot + 1;
            memcpy(
                Params + Schema.AddDefinitionOffset,
                &MutableDefinition,
                sizeof(MutableDefinition));
            memcpy(
                Params + Schema.AddQuickbarOffset,
                &PrimaryQuickbar,
                sizeof(PrimaryQuickbar));
            memcpy(
                Params + Schema.AddSlotOffset,
                &RawSlot,
                sizeof(RawSlot));
            DispatchMayHaveStarted = true;
            PlayerController->ProcessEvent(
                Schema.AddFunction, Params);
        }
        PlayerController->ForceNetUpdate();
        return true;
    }

    static bool TryIssueReportedQuickbarWrite(
        AFortPlayerControllerAthena* PlayerController,
        const UFortItemDefinition* Definition,
        int Slot,
        bool Clear,
        bool* DispatchMayHaveStarted)
    {
        if (!DispatchMayHaveStarted)
            return false;
        *DispatchMayHaveStarted = false;
        __try
        {
            return IssueReportedQuickbarWriteUnsafe(
                PlayerController,
                Definition,
                Slot,
                Clear,
                *DispatchMayHaveStarted);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool TryResolveClientQuickbarSlotsUnsafe(
        AFortPlayerControllerAthena* PlayerController,
        AFortInventory* Inventory,
        int32 EntrySize,
        const std::vector<FModernPrimaryRow>& Rows,
        const FGuid* IgnoredGuid,
        EClientQuickbarProbeMode ProbeMode,
        FResolvedLoadout& Out)
    {
        bool HasValidatedClientObject = false;
        auto ClientQuickbarProperty =
            PlayerController->GetProperty(
                "ClientQuickBars");
        if (ClientQuickbarProperty &&
            Offsets::Offset_Internal &&
            SDK::MemReadable(
                reinterpret_cast<const uint8_t*>(
                    ClientQuickbarProperty) +
                    Offsets::Offset_Internal,
                sizeof(uint32)))
        {
            const uint32 ClientQuickbarOffset =
                SDK::ReadPropertyOffset(
                    GetFromOffset<uint32>(
                        ClientQuickbarProperty,
                        Offsets::Offset_Internal));
            if (ClientQuickbarOffset != UINT32_MAX &&
                ClientQuickbarOffset <= 0x10000 &&
                SDK::MemReadable(
                    reinterpret_cast<const uint8_t*>(
                        PlayerController) +
                        ClientQuickbarOffset,
                    sizeof(AFortQuickBars*)))
            {
                auto ClientQuickbars =
                    GetFromOffset<AFortQuickBars*>(
                        PlayerController,
                        ClientQuickbarOffset);
                auto QuickbarsClass =
                    AFortQuickBars::StaticClass();
                AActor* QuickbarOwner = nullptr;
                if (IsLiveObject(ClientQuickbars) &&
                    QuickbarsClass &&
                    ClientQuickbars->IsA(
                        QuickbarsClass))
                {
                    QuickbarOwner =
                        ClientQuickbars->HasOwner()
                            ? ClientQuickbars->GetOwner()
                            : nullptr;
                    HasValidatedClientObject =
                        !QuickbarOwner ||
                        QuickbarOwner == PlayerController;
                }

                if (HasValidatedClientObject &&
                    ProbeMode ==
                        EClientQuickbarProbeMode::DirectOnly)
                {
                    std::array<
                        FResolvedSlot,
                        kSlotCount> DirectSlots{};
                    if (TryResolveDirectClientQuickbarSlots(
                            ClientQuickbars,
                            Rows,
                            IgnoredGuid,
                            &DirectSlots))
                    {
                        Out.Slots = DirectSlots;
                        Out.UsesClientQuickbarSlots = true;
                        Out.HasResolvedSlots = true;
                        Out.HasAuthoritativeSlots = true;
                        // Modern controller writes complete asynchronously.
                        // Only an owning-client bridge/native reported map can
                        // acknowledge the resulting permutation safely.
                        Out.CanMutateSlots = false;
                        SyncModernSlotLedger(Out);
                        return true;
                    }
                }
            }
        }

        if (ProbeMode ==
            EClientQuickbarProbeMode::DirectOnly)
        {
            return false;
        }

        auto ItemFunction =
            PlayerController->GetFunction(
                "GetItemInQuickbarSlot");
        auto CountFunction =
            PlayerController->GetFunction(
                "GetNumQuickbarSlots");
        auto WorldItemClass =
            UFortWorldItem::StaticClass();
        FQuickbarGetterSchema ItemSchema;
        if (!ItemFunction || !WorldItemClass ||
            !TryBuildQuickbarGetterSchemaUnsafe(
                ItemFunction,
                true,
                sizeof(UObject*),
                ItemSchema))
        {
            return false;
        }
        bool HasUsableCount = false;
        int32 RawSlotCount = 0;
        if (CountFunction)
        {
            FQuickbarGetterSchema CountSchema;
            HasUsableCount =
                TryBuildQuickbarGetterSchemaUnsafe(
                    CountFunction,
                    false,
                    sizeof(int32),
                    CountSchema) &&
                TryInvokeQuickbarCount(
                    PlayerController,
                    CountSchema,
                    &RawSlotCount) &&
                // Some client models count only the five combat cells even
                // though their raw indices remain 1..5 (slot zero is the
                // separate harvesting tool).
                RawSlotCount >= kSlotCount &&
                RawSlotCount <= 64;
        }
        if (!HasValidatedClientObject &&
            !HasUsableCount)
        {
            // Without a validated local quickbar object, a count getter is the
            // only bounded proof that these local native accessors have state.
            return false;
        }

        std::array<FResolvedSlot, kSlotCount>
            CandidateSlots[2]{};
        bool CandidateValid[2]{};
        constexpr int RawSlotBases[2] = { 1, 0 };
        const bool AmbiguousGetterShape =
            HasUsableCount;
        for (int CandidateIndex = 0;
             CandidateIndex < 2 &&
                 (AmbiguousGetterShape ||
                  !CandidateValid[0]);
             ++CandidateIndex)
        {
            auto& ResolvedSlots =
                CandidateSlots[CandidateIndex];
            int ResolvedCount = 0;
            bool Invalid = false;
            for (int Slot = 0;
                 Slot < kSlotCount;
                 ++Slot)
            {
                UObject* Returned = nullptr;
                if (!TryInvokeQuickbarItem(
                        PlayerController,
                        ItemSchema,
                        Slot +
                            RawSlotBases[
                                CandidateIndex],
                        &Returned))
                {
                    Invalid = true;
                    break;
                }
                if (!Returned)
                    continue;
                if (!IsLiveObject(Returned) ||
                    !Returned->IsA(WorldItemClass))
                {
                    Invalid = true;
                    break;
                }

                auto Instance =
                    static_cast<UFortWorldItem*>(
                        Returned);
                if (!Instance->HasItemEntry())
                {
                    Invalid = true;
                    break;
                }
                const auto& ItemEntry =
                    Instance->GetItemEntry();
                if (IgnoredGuid &&
                    AreGuidsEqual(
                        ItemEntry.ItemGuid,
                        *IgnoredGuid))
                {
                    continue;
                }
                auto Entry =
                    FindEntryByGuid(
                        Inventory,
                        EntrySize,
                        ItemEntry.ItemGuid);
                if (!Entry ||
                    FindItemInstanceByGuid(
                        Inventory,
                        ItemEntry.ItemGuid) !=
                        Instance ||
                    Entry->ItemDefinition !=
                        ItemEntry.ItemDefinition ||
                    ClassifyExistingItemDefinition(
                        Entry->ItemDefinition) !=
                        EExistingItemPlacement::Primary ||
                    IsGuidInSlots(
                        Entry->ItemGuid,
                        ResolvedSlots))
                {
                    // A delayed client action can briefly return a row that
                    // the authoritative inventory has already removed.
                    Invalid = true;
                    break;
                }

                ResolvedSlots[Slot] = {
                    true,
                    Entry->ItemGuid,
                    Entry->ItemDefinition,
                    Entry->Count
                };
                ++ResolvedCount;
            }

            if (Invalid)
                continue;

            if (Rows.size() <= kSlotCount &&
                ResolvedCount !=
                    static_cast<int>(Rows.size()))
            {
                continue;
            }
            bool IncludesAllRows = true;
            for (const auto& Row : Rows)
            {
                if (Rows.size() <= kSlotCount &&
                    !IsGuidInSlots(
                        Row.Slot.Guid,
                        ResolvedSlots))
                {
                    IncludesAllRows = false;
                    break;
                }
            }
            if (!IncludesAllRows)
                continue;
            if (Rows.size() > kSlotCount &&
                ResolvedCount != kSlotCount)
            {
                // Without all five cells, an overflow row and a genuine
                // quickbar hole cannot be distinguished safely.
                continue;
            }
            CandidateValid[CandidateIndex] = true;
        }

        // Getter counts are fork-dependent: some include reserved cells while
        // others count only combat cells. Whenever a validated count exists,
        // probe both windows and fail closed when both validate to different
        // permutations; otherwise a leading compact hole can make the
        // canonical window look complete while shifting every slot left.
        if (AmbiguousGetterShape &&
            CandidateValid[0] &&
            CandidateValid[1] &&
            !AreSlotMapsEqual(
                CandidateSlots[0],
                CandidateSlots[1]))
        {
            return false;
        }

        // Outside that explicitly ambiguous shape, preserve the canonical
        // preference and require evidence in raw cell zero before accepting a
        // compatibility-only getter layout.
        if (!CandidateValid[0] &&
            (!CandidateValid[1] ||
             (!AmbiguousGetterShape &&
              !CandidateSlots[1][0].Occupied)))
        {
            return false;
        }

        Out.Slots =
            CandidateValid[0]
                ? CandidateSlots[0]
                : CandidateSlots[1];
        Out.UsesClientQuickbarSlots = true;
        Out.HasResolvedSlots = true;
        Out.HasAuthoritativeSlots = true;
        Out.CanMutateSlots = false;
        SyncModernSlotLedger(Out);
        return true;
    }

    static bool TryResolveClientQuickbarSlots(
        AFortPlayerControllerAthena* PlayerController,
        AFortInventory* Inventory,
        int32 EntrySize,
        const std::vector<FModernPrimaryRow>& Rows,
        const FGuid* IgnoredGuid,
        EClientQuickbarProbeMode ProbeMode,
        FResolvedLoadout& Out)
    {
        __try
        {
            return TryResolveClientQuickbarSlotsUnsafe(
                PlayerController,
                Inventory,
                EntrySize,
                Rows,
                IgnoredGuid,
                ProbeMode,
                Out);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool ResolveModernFallbackSlots(
        const std::vector<FModernPrimaryRow>& Rows,
        FResolvedLoadout& Out)
    {
        const uint64_t ControllerIdentity =
            GetLiveObjectIdentity(
                Out.PlayerController);
        const uint64_t InventoryIdentity =
            GetLiveObjectIdentity(
                Out.Inventory);
        if (!ControllerIdentity ||
            !InventoryIdentity)
        {
            return false;
        }

        auto& Ledger =
            GModernSlotLedgers[ControllerIdentity];
        if (Ledger.InventoryIdentity !=
                InventoryIdentity)
        {
            Ledger = {};
            Ledger.InventoryIdentity =
                InventoryIdentity;
        }

        // First discard GUIDs that are no longer in this inventory.
        for (auto& Guid : Ledger.Slots)
        {
            if (!Guid.IsZero() &&
                !IsGuidInRows(Guid, Rows))
            {
                Guid = {};
            }
        }

        // New rows take the first stable empty slot. This mirrors the client
        // quickbar's normal pickup behavior and, unlike rebuilding from array
        // indices each tick, preserves holes created by a clear operation.
        for (const auto& Row : Rows)
        {
            bool Assigned = false;
            for (const auto& Guid : Ledger.Slots)
            {
                if (!Guid.IsZero() &&
                    AreGuidsEqual(
                        Row.Slot.Guid,
                        Guid))
                {
                    Assigned = true;
                    break;
                }
            }
            if (Assigned)
                continue;

            for (auto& Guid : Ledger.Slots)
            {
                if (Guid.IsZero())
                {
                    Guid =
                        ToGuidValue(
                            Row.Slot.Guid);
                    Assigned = true;
                    break;
                }
            }
            // More than five primary rows can legitimately represent overflow.
            // They remain in the inventory but are not combat quickbar slots.
        }

        int MappedRows = 0;
        for (int Slot = 0;
             Slot < kSlotCount;
             ++Slot)
        {
            const auto& Guid =
                Ledger.Slots[Slot];
            if (Guid.IsZero())
                continue;
            for (const auto& Row : Rows)
            {
                if (AreGuidsEqual(
                        Row.Slot.Guid,
                        Guid))
                {
                    Out.Slots[Slot] =
                        Row.Slot;
                    ++MappedRows;
                    break;
                }
            }
        }
        if (Rows.size() <= kSlotCount &&
            MappedRows !=
                static_cast<int>(Rows.size()))
        {
            return false;
        }

        Out.UsesFallbackSlots = true;
        Out.HasResolvedSlots = true;
        // A GUID-membership ledger preserves a stable estimate across add and
        // remove operations, but it cannot observe a client-side permutation.
        // It can still support GUID-bound inventory edits without claiming the
        // selected card is an authoritative client-side slot number.
        Out.HasAuthoritativeSlots = false;
        return true;
    }

    static bool SetModernLedgerSlot(
        const FResolvedLoadout& Loadout,
        int Slot,
        const FGuid* Guid)
    {
        if (Loadout.HasLegacyQuickbar ||
            Slot < 0 ||
            Slot >= kSlotCount ||
            !SyncModernSlotLedger(Loadout))
        {
            return Loadout.HasLegacyQuickbar;
        }

        const uint64_t ControllerIdentity =
            GetLiveObjectIdentity(
                Loadout.PlayerController);
        auto Existing =
            GModernSlotLedgers.find(
                ControllerIdentity);
        if (Existing ==
            GModernSlotLedgers.end())
        {
            return false;
        }

        if (Guid)
        {
            for (auto& Other :
                 Existing->second.Slots)
            {
                if (AreGuidsEqual(
                        *Guid,
                        Other))
                {
                    Other = {};
                }
            }
            Existing->second.Slots[Slot] =
                ToGuidValue(*Guid);
        }
        else
        {
            Existing->second.Slots[Slot] = {};
        }
        return true;
    }

    static bool CaptureModernLedger(
        const FResolvedLoadout& Loadout,
        FActionTransaction& Transaction)
    {
        if (Loadout.HasLegacyQuickbar)
            return true;
        if (!SyncModernSlotLedger(Loadout))
            return false;

        const uint64_t ControllerIdentity =
            GetLiveObjectIdentity(
                Loadout.PlayerController);
        const auto Existing =
            GModernSlotLedgers.find(
                ControllerIdentity);
        if (!ControllerIdentity ||
            Existing == GModernSlotLedgers.end())
        {
            return false;
        }

        Transaction.HasModernLedgerSnapshot = true;
        Transaction.ModernLedgerControllerIdentity =
            ControllerIdentity;
        Transaction.ModernLedgerInventoryIdentity =
            Existing->second.InventoryIdentity;
        Transaction.ModernLedgerSlots =
            Existing->second.Slots;
        return true;
    }

    static bool RestoreModernLedger(
        const FActionTransaction& Transaction,
        AFortPlayerControllerAthena* PlayerController,
        AFortInventory* Inventory)
    {
        if (!Transaction.HasModernLedgerSnapshot)
            return true;
        if (GetLiveObjectIdentity(
                PlayerController) !=
                Transaction
                    .ModernLedgerControllerIdentity ||
            GetLiveObjectIdentity(
                Inventory) !=
                Transaction
                    .ModernLedgerInventoryIdentity)
        {
            return false;
        }

        auto& Ledger =
            GModernSlotLedgers[
                Transaction
                    .ModernLedgerControllerIdentity];
        Ledger.InventoryIdentity =
            Transaction
                .ModernLedgerInventoryIdentity;
        Ledger.Slots =
            Transaction.ModernLedgerSlots;
        return true;
    }

    static void FinalizeModernMutationCapability(
        AFortInventory* Inventory,
        int32 EntrySize,
        FResolvedLoadout& Loadout)
    {
        if (!Loadout.HasResolvedSlots)
            return;

        int32 MutationEntrySize = 0;
        const bool HasMutableInventory =
            HasSafeMutationInventory(
                Inventory, MutationEntrySize) &&
            MutationEntrySize == EntrySize;
        if (!HasMutableInventory)
        {
            Loadout.CanMutateSlots = false;
            Loadout.UsesGuidOnlyMutation = false;
            return;
        }

        // Exact bridge/reported writers keep their acknowledged positional
        // transaction. Every other modern source is still actionable, but its
        // contract is the exact GUID attached to the clicked card—not an
        // unverified client slot number.
        // Keep CanMutateSlots positional-only. Reported mutation and
        // verification code relies on that distinction.
        Loadout.UsesGuidOnlyMutation =
            !Loadout.CanMutateSlots;
    }

    static void ResolveModernSlots(
        AFortPlayerControllerAthena* PlayerController,
        AFortInventory* Inventory,
        int32 EntrySize,
        FResolvedLoadout& Out,
        const FGuid* IgnoredGuid,
        ESlotResolvePolicy Policy)
    {
        auto& Entries =
            Inventory->GetInventory()
                .GetReplicatedEntries();

        Out.HasOrderIndex =
            FFortItemEntry::StaticStruct() &&
            FFortItemEntry::HasOrderIndex();
        Out.HasClientQuickbarPlacement =
            PlayerController->GetFunction(
                "AddItemToQuickBars") != nullptr;

        std::vector<FModernPrimaryRow> Rows;
        Rows.reserve(kSlotCount + 2);
        for (int32 Index = 0;
             Index < Entries.Num();
             ++Index)
        {
            auto& Entry = Entries.Get(Index, EntrySize);
            if (IgnoredGuid &&
                AreGuidsEqual(
                    Entry.ItemGuid,
                    *IgnoredGuid))
            {
                continue;
            }

            const auto Placement =
                ClassifyExistingItemDefinition(
                    Entry.ItemDefinition);
            if (Placement ==
                EExistingItemPlacement::Invalid)
            {
                return;
            }
            if (Placement ==
                EExistingItemPlacement::NonPrimary)
            {
                continue;
            }
            if (ToGuidValue(
                    Entry.ItemGuid).IsZero())
            {
                return;
            }
            for (const auto& Existing : Rows)
            {
                if (AreGuidsEqual(
                        Existing.Slot.Guid,
                        Entry.ItemGuid))
                {
                    return;
                }
            }

            FModernPrimaryRow Row;
            Row.Slot = {
                true,
                Entry.ItemGuid,
                Entry.ItemDefinition,
                Entry.Count
            };
            if (Out.HasOrderIndex)
            {
                Row.Order =
                    static_cast<int>(
                        Entry.OrderIndex);
            }
            Rows.push_back(Row);
            if (Rows.size() > 64)
                return;
        }

        // Prefer the owning-client bridge on every modern release.  It is the
        // only source here that observes the player's true local permutation
        // and can acknowledge an asynchronous slot write without guessing.
        if (TryResolveBridgeQuickbarSlots(
                PlayerController,
                Rows,
                IgnoredGuid,
                Out))
        {
            FinalizeModernMutationCapability(
                Inventory, EntrySize, Out);
            return;
        }

        // Remote modern controllers report an exact fixed GUID snapshot to the
        // server. Prefer it before local-only quickbar actors so a validated
        // asynchronous writer can retain true positional editing.
        if (TryResolveReportedQuickbarSlots(
                PlayerController,
                Rows,
                IgnoredGuid,
                Out))
        {
            FinalizeModernMutationCapability(
                Inventory, EntrySize, Out);
            return;
        }

        // A local/forked quickbar object remains useful for exact display. If
        // it has no acknowledged writer, FinalizeModernMutationCapability
        // routes edits through the GUID-bound inventory transaction.
        if (TryResolveClientQuickbarSlots(
                PlayerController,
                Inventory,
                EntrySize,
                Rows,
                IgnoredGuid,
                EClientQuickbarProbeMode::DirectOnly,
                Out))
        {
            FinalizeModernMutationCapability(
                Inventory, EntrySize, Out);
            return;
        }

        // Finally recover local/forked layouts that expose controller getters
        // without a usable ClientQuickBars property.
        if (TryResolveClientQuickbarSlots(
                PlayerController,
                Inventory,
                EntrySize,
                Rows,
                IgnoredGuid,
                EClientQuickbarProbeMode::GettersOnly,
                Out))
        {
            FinalizeModernMutationCapability(
                Inventory, EntrySize, Out);
            return;
        }

        // A drag operation can make the client quickbar briefly incomplete.
        // Preserve the last exact view as read-only for under one second rather
        // than snapping to stale server metadata during that transition.
        if (Policy == ESlotResolvePolicy::Normal &&
            ResolveRecentClientSlotLedger(
                Rows, Out))
        {
            FinalizeModernMutationCapability(
                Inventory, EntrySize, Out);
            return;
        }

        if (Out.HasOrderIndex)
        {
            std::array<FResolvedSlot, kSlotCount>
                OrderedSlots{};
            std::array<bool, kSlotCount> Used{};
            int OrderedRows = 0;
            bool DuplicateOrder = false;
            bool InvalidOrder = false;
            for (const auto& Row : Rows)
            {
                if (Row.Order < 0)
                {
                    continue;
                }
                if (Row.Order >= kSlotCount)
                {
                    InvalidOrder = true;
                    break;
                }
                if (Used[Row.Order])
                {
                    DuplicateOrder = true;
                    break;
                }
                Used[Row.Order] = true;
                OrderedSlots[Row.Order] =
                    Row.Slot;
                ++OrderedRows;
            }

            const bool CompleteNormalInventory =
                Rows.size() <= kSlotCount &&
                OrderedRows ==
                    static_cast<int>(Rows.size());
            const bool UsefulOverflowMapping =
                Rows.size() > kSlotCount &&
                OrderedRows == kSlotCount;
            if (!InvalidOrder &&
                !DuplicateOrder &&
                (CompleteNormalInventory ||
                 UsefulOverflowMapping))
            {
                Out.Slots = OrderedSlots;
                Out.UsesOrderIndexSlots = true;
                Out.UsesFallbackSlots = true;
                Out.HasResolvedSlots = true;
                // OrderIndex is transient on modern builds and can remain a
                // complete, unique but stale permutation after a remote drag.
                // It is useful for a read-only estimate, never as proof for a
                // slot-specific mutation.
                Out.HasAuthoritativeSlots = false;
                FinalizeModernMutationCapability(
                    Inventory, EntrySize, Out);
                return;
            }
        }

        // FN 7.40+ moved the persistent quickbar to the client. Several builds
        // leave the reflected transient OrderIndex unset on the server and do
        // not instantiate ClientQuickBars for remote controllers. Keep a
        // validated GUID ledger seeded from replicated inventory order so the
        // UI remains usable while every action still targets an exact live row.
        // Verification also needs the same stable GUID ledger. Restricting the
        // fallback to normal rendering made a successful GUID edit look like a
        // failed transaction on seasons without a reported quickbar schema.
        (void)Policy;
        ResolveModernFallbackSlots(Rows, Out);
        FinalizeModernMutationCapability(
            Inventory, EntrySize, Out);
    }

    static bool ResolveLoadoutUnsafe(
        uintptr_t TargetToken,
        FResolvedLoadout& Out,
        std::string& Error,
        const FGuid* IgnoredGuid = nullptr,
        ESlotResolvePolicy Policy =
            ESlotResolvePolicy::Normal)
    {
        Out = {};
        auto PlayerController =
            ResolveRequestedController(TargetToken);
        if (!PlayerController)
        {
            Error = "Player is no longer connected.";
            return false;
        }

        auto Inventory =
            ResolveWorldInventory(PlayerController);
        int32 EntrySize = 0;
        if (!Inventory ||
            !HasSafeInventoryEntries(
                Inventory, EntrySize))
        {
            Error = "Inventory is not available on this player yet.";
            return false;
        }

        Out.PlayerController = PlayerController;
        Out.Inventory = Inventory;
        if (VersionInfo.FortniteVersion < 7.40f)
        {
            ResolveLegacySlots(
                PlayerController,
                Inventory,
                EntrySize,
                Out,
                IgnoredGuid);
        }
        else
        {
            ResolveModernSlots(
                PlayerController,
                Inventory,
                EntrySize,
                Out,
                IgnoredGuid,
                Policy);
        }
        if (!Out.HasResolvedSlots)
        {
            Error =
                "This build does not expose a safe quickbar layout.";
            return false;
        }
        return true;
    }

    static std::string BuildObjectLineage(
        const UObject* Object)
    {
        std::array<std::string, 12> Parts;
        int PartCount = 0;
        const UObject* Current = Object;
        for (int Depth = 0;
             Current && Depth < 12;
             ++Depth)
        {
            if (!IsLiveObject(Current))
                break;
            const std::string Part =
                Current->Name.ToUtf8();
            if (Part.empty() ||
                Part.size() > 1024)
            {
                break;
            }
            Parts[PartCount++] =
                Lowercase(Part);
            Current = Current->Outer;
        }

        // Reconstruct a path-like identity even on builds where the package
        // hierarchy is exposed as separate Outer components. A UPackage FName
        // may already contain /Game/...; retaining it is intentional.
        std::string Result;
        for (int Index = PartCount - 1;
             Index >= 0;
             --Index)
        {
            if (Result.size() +
                    Parts[Index].size() + 1 >
                4096)
            {
                break;
            }
            if (Result.empty() &&
                !Parts[Index].empty() &&
                Parts[Index].front() != '/')
            {
                Result.push_back('/');
            }
            else if (!Result.empty() &&
                     Result.back() != '/')
            {
                Result.push_back('/');
            }
            Result += Parts[Index];
        }
        return Result;
    }

    static std::string BuildClassLineage(
        const UObject* Object)
    {
        std::string Result;
        const UStruct* Current =
            Object && IsLiveObject(Object)
                ? static_cast<const UStruct*>(
                    Object->Class)
                : nullptr;
        for (int Depth = 0;
             Current && Depth < 16;
             ++Depth)
        {
            if (!IsLiveObject(Current))
                break;
            const std::string Part =
                Lowercase(Current->Name.ToUtf8());
            if (Part.empty() ||
                Part.size() > 256 ||
                Result.size() + Part.size() + 1 >
                    4096)
            {
                break;
            }
            if (!Result.empty())
                Result.push_back('\n');
            Result += Part;
            Current = Current->GetSuper();
        }
        return Result;
    }

    static bool StartsWith(
        const std::string& Value,
        const char* Prefix)
    {
        if (!Prefix)
            return false;
        const size_t Length = strlen(Prefix);
        return Value.size() >= Length &&
            Value.compare(0, Length, Prefix) == 0;
    }

    static bool EndsWith(
        const std::string& Value,
        const char* Suffix)
    {
        if (!Suffix)
            return false;
        const size_t Length = strlen(Suffix);
        return Value.size() >= Length &&
            Value.compare(
                Value.size() - Length,
                Length,
                Suffix) == 0;
    }

    static bool ClassLineHasModeStem(
        const std::string& Lineage,
        const char* Stem)
    {
        if (!Stem || !*Stem)
            return false;

        std::string FortStem = "fort";
        FortStem += Stem;
        size_t Start = 0;
        while (Start <= Lineage.size())
        {
            size_t End = Lineage.find('\n', Start);
            if (End == std::string::npos)
                End = Lineage.size();
            const std::string Component =
                Lineage.substr(Start, End - Start);
            if (StartsWith(Component, Stem) ||
                StartsWith(Component, FortStem.c_str()))
            {
                return true;
            }
            if (End == Lineage.size())
                break;
            Start = End + 1;
        }
        return false;
    }

    static EItemMode ClassifyItemModeUnsafe(
        const UFortItemDefinition* Definition,
        const std::string& Id)
    {
        const std::string ObjectPath =
            BuildObjectLineage(Definition);
        const std::string ClassPath =
            BuildClassLineage(Definition);
        const std::string ItemId =
            Lowercase(Id);
        const auto PathContains =
            [&ObjectPath](const char* Marker)
            {
                return ObjectPath.find(Marker) !=
                    std::string::npos;
            };
        const auto IdContains =
            [&ItemId](const char* Marker)
            {
                return ItemId.find(Marker) !=
                    std::string::npos;
            };

        // Explicit package roots and class ancestry are the strongest signals
        // available consistently across the supported generations. Generic
        // /Game/Items content is deliberately not classified: both modes use it.
        const bool StrongBattleRoyale =
            PathContains("/game/athena/") ||
            PathContains("/athena/") ||
            PathContains("/game/creative/") ||
            PathContains("/battle royale/") ||
            PathContains("/battleroyale/") ||
            ClassLineHasModeStem(ClassPath, "athena");
        const bool StrongSaveTheWorld =
            PathContains("/savetheworld/") ||
            PathContains("/save_the_world/") ||
            PathContains("/game/campaign/") ||
            StartsWith(ObjectPath, "/campaign/") ||
            ClassLineHasModeStem(
                ClassPath, "savetheworld") ||
            ClassLineHasModeStem(
                ClassPath, "save_the_world") ||
            ClassLineHasModeStem(
                ClassPath, "campaign") ||
            ClassLineHasModeStem(
                ClassPath, "homebase") ||
            ClassLineHasModeStem(
                ClassPath, "fortschematicitemdefinition");
        if (StrongBattleRoyale != StrongSaveTheWorld)
        {
            return StrongBattleRoyale
                ? EItemMode::BattleRoyale
                : EItemMode::SaveTheWorld;
        }
        if (StrongBattleRoyale && StrongSaveTheWorld)
            return EItemMode::Unknown;

        // Asset-ID and chapter-root fallbacks are kept conservative. Unknown
        // is preferable to silently placing shared/test/plugin content in STW.
        const bool BattleRoyaleFallback =
            StartsWith(ItemId, "athena_") ||
            StartsWith(ItemId, "br_") ||
            IdContains("_athena_") ||
            EndsWith(ItemId, "_athena") ||
            IdContains("_br_") ||
            EndsWith(ItemId, "_br") ||
            PathContains("/apollo/") ||
            PathContains("/artemis/") ||
            PathContains("/asteria/") ||
            PathContains("/helios/");
        const bool SaveTheWorldFallback =
            StartsWith(ItemId, "stw_") ||
            IdContains("_stw_") ||
            EndsWith(ItemId, "_stw") ||
            StartsWith(ItemId, "campaign_") ||
            IdContains("_campaign_") ||
            IdContains("_homebase_") ||
            IdContains("_malachite_") ||
            IdContains("_shadowshard_") ||
            IdContains("_obsidian_") ||
            IdContains("_sunbeam_") ||
            IdContains("_brightcore_");
        if (BattleRoyaleFallback !=
            SaveTheWorldFallback)
        {
            return BattleRoyaleFallback
                ? EItemMode::BattleRoyale
                : EItemMode::SaveTheWorld;
        }
        return EItemMode::Unknown;
    }

    static EItemMode ClassifyItemMode(
        const UFortItemDefinition* Definition,
        const std::string& Id)
    {
        EItemMode Result = EItemMode::Unknown;
        __try
        {
            Result =
                ClassifyItemModeUnsafe(Definition, Id);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Result = EItemMode::Unknown;
        }
        return Result;
    }

    static FItemMetadata BuildMetadata(
        const UFortItemDefinition* Definition)
    {
        FItemMetadata Result;
        Result.Token =
            reinterpret_cast<uintptr_t>(Definition);
        Result.ObjectIdentity =
            GetLiveObjectIdentity(Definition);
        Result.Id = Definition->Name.ToUtf8();
        Result.Name = Result.Id;

        if (Definition->HasDisplayName())
        {
            auto DisplayName =
                Definition->DisplayName.ToString();
            if (!DisplayName.empty())
                Result.Name.assign(
                    DisplayName.begin(), DisplayName.end());
        }
        else if (Definition->HasItemName())
        {
            auto ItemName =
                Definition->ItemName.ToString();
            if (!ItemName.empty())
                Result.Name.assign(
                    ItemName.begin(), ItemName.end());
        }

        if (Definition->HasRarity())
            Result.Rarity =
                (std::clamp)(
                    static_cast<int>(Definition->Rarity),
                    0, 9);

        Result.Mode =
            ClassifyItemMode(Definition, Result.Id);
        Result.SearchText =
            Lowercase(Result.Name + "\n" + Result.Id);
        return Result;
    }

    static FItemMetadata BuildMetadataCached(
        const UFortItemDefinition* Definition)
    {
        const uintptr_t Token =
            reinterpret_cast<uintptr_t>(Definition);
        const uint64_t Identity =
            GetLiveObjectIdentity(Definition);
        const auto Existing =
            GMetadataCache.find(Token);
        if (Identity &&
            Existing != GMetadataCache.end() &&
            Existing->second.ObjectIdentity == Identity)
        {
            return Existing->second;
        }

        FItemMetadata Metadata =
            BuildMetadata(Definition);
        if (!Metadata.ObjectIdentity ||
            Metadata.Id.empty())
        {
            if (Existing != GMetadataCache.end())
                GMetadataCache.erase(Existing);
            return Metadata;
        }

        // Metadata contains only copied strings and scalar values. Keeping it
        // across map travel avoids repeatedly walking FText and class lineage
        // for the same persistent definition. Object identity is checked on
        // every reuse, so an index/address recycle cannot reuse stale data.
        if (Existing == GMetadataCache.end() &&
            GMetadataCache.size() >=
                kMetadataCacheMaxItems)
        {
            GMetadataCache.erase(
                GMetadataCache.begin());
        }
        GMetadataCache[Token] = Metadata;
        return Metadata;
    }

    static FSlotView BuildSlotView(
        const FResolvedSlot& Resolved)
    {
        FSlotView Result;
        if (!Resolved.Occupied ||
            ClassifyExistingItemDefinition(
                Resolved.Definition) !=
                EExistingItemPlacement::Primary)
        {
            return Result;
        }

        const auto Metadata =
            BuildMetadataCached(Resolved.Definition);
        Result.Occupied = true;
        Result.ItemToken = Metadata.Token;
        Result.ItemIdentity =
            Metadata.ObjectIdentity;
        Result.Guid = ToGuidValue(Resolved.Guid);
        Result.Id = Metadata.Id;
        Result.Name = Metadata.Name;
        Result.Rarity = Metadata.Rarity;
        Result.Mode = Metadata.Mode;
        return Result;
    }

    static bool BuildLoadoutSnapshotUnsafe(
        uintptr_t TargetToken,
        FLoadoutSnapshot& Snapshot)
    {
        Snapshot = {};
        Snapshot.TargetToken = TargetToken;
        Snapshot.WorldGeneration =
            GWorldGeneration.load(
                std::memory_order_acquire);
        Snapshot.TargetIdentity =
            GetTargetIdentity(TargetToken);
        Snapshot.Generation = ++GLoadoutGeneration;

        FResolvedLoadout Resolved;
        std::string Error;
        if (!ResolveLoadoutUnsafe(
                TargetToken, Resolved, Error))
        {
            Snapshot.Availability =
                ELoadoutAvailability::Unavailable;
            Snapshot.Message = std::move(Error);
            return true;
        }

        Snapshot.Availability =
            ELoadoutAvailability::Ready;
        Snapshot.HasExactSlotOrder =
            Resolved.HasAuthoritativeSlots;
        Snapshot.CanEditSlots =
            Resolved.CanMutateSlots ||
            Resolved.UsesGuidOnlyMutation;
        Snapshot.UsesGuidOnlyMutation =
            Resolved.UsesGuidOnlyMutation;
        Snapshot.UsesBridgeSlots =
            Resolved.UsesBridgeQuickbarSlots;
        Snapshot.BridgeSession =
            Resolved.BridgeSession;
        Snapshot.BridgeGeneration =
            Resolved.BridgeGeneration;
        for (int Slot = 0; Slot < kSlotCount; ++Slot)
            Snapshot.Slots[Slot] =
                BuildSlotView(Resolved.Slots[Slot]);

        auto Pawn =
            Resolved.PlayerController->HasMyFortPawn()
                ? Resolved.PlayerController
                      ->GetMyFortPawn()
                : nullptr;
        if (IsLiveObject(Pawn) &&
            Pawn->HasCurrentWeapon())
        {
            auto CurrentWeapon =
                Pawn->GetCurrentWeapon();
            auto WeaponClass =
                AFortWeapon::StaticClass();
            if (IsLiveObject(CurrentWeapon) &&
                WeaponClass &&
                CurrentWeapon->IsA(
                    WeaponClass))
            {
                auto Weapon =
                    static_cast<AFortWeapon*>(
                        CurrentWeapon);
                if (Weapon->HasItemEntryGuid())
                {
                    Snapshot.EquippedGuid =
                        ToGuidValue(
                            Weapon->GetItemEntryGuid());
                }
            }
        }
        return true;
    }

    static bool TryBuildLoadoutSnapshot(
        uintptr_t TargetToken,
        FLoadoutSnapshot* Snapshot)
    {
        __try
        {
            return BuildLoadoutSnapshotUnsafe(
                TargetToken, *Snapshot);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool RefreshBridgePermutationUnsafe(
        const FLoadoutSnapshot& Previous,
        FLoadoutSnapshot& Refreshed,
        bool& Changed,
        bool& MembershipChanged)
    {
        Changed = false;
        MembershipChanged = false;
        if (!Previous.UsesBridgeSlots ||
            !Previous.BridgeSession ||
            !Previous.BridgeGeneration ||
            !Previous.TargetToken ||
            Previous.WorldGeneration !=
                GWorldGeneration.load(
                    std::memory_order_acquire))
        {
            return false;
        }

        auto PlayerController =
            reinterpret_cast<
                AFortPlayerControllerAthena*>(
                    Previous.TargetToken);
        auto ControllerClass =
            AFortPlayerControllerAthena::StaticClass();
        if (!IsLiveObject(PlayerController) ||
            !ControllerClass ||
            !PlayerController->IsA(
                ControllerClass) ||
            GetLiveObjectIdentity(
                PlayerController) !=
                Previous.TargetIdentity)
        {
            return false;
        }

        PlayerLoadoutBridgeServer::FSnapshot Wire{};
        if (!PlayerLoadoutBridgeServer::TryGetLatestSnapshot(
                PlayerController,
                Wire,
                kBridgeSnapshotMaxAgeMs) ||
            Wire.Session != Previous.BridgeSession ||
            Wire.Generation <
                Previous.BridgeGeneration)
        {
            return false;
        }
        if (Wire.Generation ==
            Previous.BridgeGeneration)
        {
            return true;
        }

        std::array<FSlotView, kSlotCount> Slots{};
        std::array<bool, kSlotCount> Used{};
        for (int Slot = 0; Slot < kSlotCount; ++Slot)
        {
            const auto& Source = Wire.Slots[Slot];
            const FGuidValue Guid{
                static_cast<int32>(Source.A),
                static_cast<int32>(Source.B),
                static_cast<int32>(Source.C),
                static_cast<int32>(Source.D)
            };
            if (Guid.IsZero())
                continue;

            int Match = -1;
            for (int ExistingSlot = 0;
                 ExistingSlot < kSlotCount;
                 ++ExistingSlot)
            {
                if (!Used[ExistingSlot] &&
                    Previous.Slots[ExistingSlot]
                        .Occupied &&
                    AreGuidValuesEqual(
                        Previous.Slots[ExistingSlot]
                            .Guid,
                        Guid))
                {
                    Match = ExistingSlot;
                    break;
                }
            }
            if (Match < 0)
            {
                // The inventory membership changed. Let the normal bounded
                // inventory resolver rebuild metadata; this fast lane handles
                // only a permutation of the already validated five GUIDs.
                MembershipChanged = true;
                return false;
            }
            Used[Match] = true;
            Slots[Slot] = Previous.Slots[Match];
        }
        for (int ExistingSlot = 0;
             ExistingSlot < kSlotCount;
             ++ExistingSlot)
        {
            if (Previous.Slots[ExistingSlot].Occupied &&
                !Used[ExistingSlot])
            {
                MembershipChanged = true;
                return false;
            }
        }

        Refreshed = Previous;
        Refreshed.Generation =
            ++GLoadoutGeneration;
        Refreshed.BridgeGeneration =
            Wire.Generation;
        Refreshed.Slots = std::move(Slots);
        for (int Slot = 0; Slot < kSlotCount; ++Slot)
        {
            if (!AreGuidValuesEqual(
                    Refreshed.Slots[Slot].Guid,
                    Previous.Slots[Slot].Guid))
            {
                Changed = true;
                break;
            }
        }
        return true;
    }

    static bool TryRefreshBridgePermutation(
        const FLoadoutSnapshot& Previous,
        FLoadoutSnapshot* Refreshed,
        bool* Changed,
        bool* MembershipChanged)
    {
        __try
        {
            return RefreshBridgePermutationUnsafe(
                Previous,
                *Refreshed,
                *Changed,
                *MembershipChanged);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *Changed = false;
            *MembershipChanged = false;
            return false;
        }
    }

    static bool IsCatalogObjectUnsafe(
        int32 ObjectIndex,
        FItemMetadata& Metadata)
    {
        if (ObjectIndex < 0 ||
            ObjectIndex >= TUObjectArray::Num())
        {
            return false;
        }

        auto Item =
            TUObjectArray::GetItemByIndex(ObjectIndex);
        const int32 InvalidFlags = 0x20;
        if (!Item || (Item->GetFlags() & InvalidFlags))
            return false;

        auto Object = Item->GetObject();
        if (!IsSafePrimaryItemDefinition(
                reinterpret_cast<
                    const UFortItemDefinition*>(Object)))
        {
            return false;
        }

        auto Definition =
            static_cast<const UFortItemDefinition*>(Object);
        Metadata = BuildMetadataCached(Definition);
        return Metadata.ObjectIdentity &&
            !Metadata.Id.empty();
    }

    static bool TryBuildCatalogItem(
        int32 ObjectIndex,
        FItemMetadata* Metadata)
    {
        __try
        {
            return IsCatalogObjectUnsafe(
                ObjectIndex, *Metadata);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void SeedCatalogFromLoadout(
        const FLoadoutSnapshot& Snapshot)
    {
        // Insert in reverse with push_front so slot 1 remains first while the
        // five live definitions bypass any small scan batch awaiting publish.
        // This makes the picker immediately useful without increasing global
        // object-scan work or its game-thread budget.
        for (int SlotIndex = kSlotCount - 1;
             SlotIndex >= 0;
             --SlotIndex)
        {
            const auto& Slot =
                Snapshot.Slots[SlotIndex];
            if (!Slot.Occupied ||
                !Slot.ItemToken ||
                !Slot.ItemIdentity ||
                !GCatalogTokens.insert(
                    Slot.ItemToken).second)
            {
                continue;
            }

            FItemMetadata Metadata;
            Metadata.Token = Slot.ItemToken;
            Metadata.ObjectIdentity =
                Slot.ItemIdentity;
            Metadata.Id = Slot.Id;
            Metadata.Name = Slot.Name;
            Metadata.Rarity = Slot.Rarity;
            // This panel is reached through an Athena controller. Shared
            // /Game/Items definitions often carry no BR/STW path marker, but
            // a definition actively present in this Athena quickbar is a
            // strong runtime BR signal. Keep global unloaded definitions
            // conservative; contextualize only these five live seeds so the
            // default BR filter is useful immediately.
            Metadata.Mode =
                Slot.Mode == EItemMode::Unknown
                    ? EItemMode::BattleRoyale
                    : Slot.Mode;
            Metadata.SearchText =
                Lowercase(
                    Metadata.Name + "\n" +
                    Metadata.Id);
            GPendingCatalogItems.push_front(
                std::move(Metadata));
        }
    }

    static void PublishCatalogBatch(
        std::vector<FItemMetadata>& Batch)
    {
        for (auto& Item : Batch)
            GPendingCatalogItems.push_back(
                std::move(Item));

        FTrySharedStateLock Lock;
        if (!Lock.owns_lock())
            return;

        size_t PublishedItems = 0;
        while (!GPendingCatalogItems.empty() &&
               PublishedItems <
                   kCatalogPublishItemsPerTick)
        {
            GPublished.Catalog.Items.push_back(
                std::move(
                    GPendingCatalogItems.front()));
            GPendingCatalogItems.pop_front();
            ++PublishedItems;
        }

        if (PublishedItems)
        {
            GPublished.Catalog.Generation =
                ++GCatalogGeneration;
        }
        GPublished.Catalog.ScannedObjects =
            GCatalogScanIndex;
        GPublished.Catalog.TotalObjects =
            GCatalogScanLimit;
        GPublished.Catalog.Complete =
            GCatalogComplete &&
            GPendingCatalogItems.empty();
    }

    static void ResetCatalogStateLocked()
    {
        GCatalogScanIndex = 0;
        GCatalogScanLimit = 0;
        GCatalogScanFront = 0;
        GCatalogScanBack = -1;
        GCatalogScanNewestNext = true;
        GCatalogComplete = true;
        GLastCatalogTickAt = 0;
        GPendingCatalogItems.clear();
        GCatalogTokens.clear();
        if (GMetadataCache.empty())
        {
            GMetadataCache.reserve(
                64);
        }
        GIconFailures.clear();
        GTextureDecodeFailures.clear();
        GPendingIconResults.clear();
        GIconRequests.clear();
        GIconResults.clear();
        GQueuedIcons.clear();

        GPublished.Catalog = {};
        GPublished.Catalog.Complete = true;
        GPublished.Catalog.Generation =
            ++GCatalogGeneration;
        GCacheEpoch.fetch_add(
            1, std::memory_order_acq_rel);
    }

    static bool ResetCatalogIfRequested()
    {
        if (!GCatalogResetRequested.load(
                std::memory_order_acquire))
        {
            return true;
        }

        FTrySharedStateLock Lock;
        if (!Lock.owns_lock())
            return false;

        ResetCatalogStateLocked();
        GCatalogResetRequested.store(
            false, std::memory_order_release);
        return true;
    }

    static bool TickCatalog()
    {
        if (!ResetCatalogIfRequested())
            return false;

        if (!GPendingCatalogItems.empty())
        {
            std::vector<FItemMetadata> Empty;
            PublishCatalogBatch(Empty);
            // Drain at most one bounded publication batch per game tick.
            // Scanning resumes on a later tick after any contended backlog.
            return true;
        }

        const ULONGLONG Now = GetTickCount64();
        const bool Foreground =
            GCatalogLeaseUntil.load(
                std::memory_order_acquire) >= Now;
        // Warm the copied metadata cache quietly once a world exists. This is
        // deliberately slower than the visible-page lane, but it means the
        // picker normally has useful results before it is opened.
        if (!Foreground && !GObservedWorldIdentity)
            return false;
        const ULONGLONG TickInterval =
            Foreground
                ? kCatalogForegroundTickIntervalMs
                : kCatalogBackgroundTickIntervalMs;
        if (Now - GLastCatalogTickAt < TickInterval)
        {
            return false;
        }
        GLastCatalogTickAt = Now;

        const int32 CurrentObjectCount =
            TUObjectArray::Num();
        if (CurrentObjectCount <= 0)
            return false;

        if (GCatalogScanLimit <= 0)
        {
            GCatalogScanLimit = CurrentObjectCount;
            GCatalogScanFront = 0;
            GCatalogScanBack =
                CurrentObjectCount - 1;
            GCatalogScanNewestNext = true;
            GCatalogComplete = false;
        }
        else if (GCatalogScanFront >
                 GCatalogScanBack)
        {
            GCatalogScanIndex =
                GCatalogScanLimit;
            if (CurrentObjectCount >
                GCatalogScanLimit)
            {
                // UObject storage only appends during a live world. Scan the
                // newly appended range without remapping or repeating the
                // already completed prefix.
                GCatalogScanFront =
                    GCatalogScanLimit;
                GCatalogScanBack =
                    CurrentObjectCount - 1;
                GCatalogScanLimit =
                    CurrentObjectCount;
                GCatalogScanNewestNext = true;
                GCatalogComplete = false;
            }
            else
            {
                GCatalogComplete = true;
                std::vector<FItemMetadata> Empty;
                PublishCatalogBatch(Empty);
                return false;
            }
        }

        const auto Started =
            std::chrono::steady_clock::now();
        std::vector<FItemMetadata> Batch;
        const int ObjectLimit =
            Foreground
                ? kCatalogForegroundObjectsPerTick
                : kCatalogBackgroundObjectsPerTick;
        const int ItemLimit =
            Foreground
                ? kCatalogForegroundItemsPerTick
                : kCatalogBackgroundItemsPerTick;
        const auto TimeBudget =
            Foreground
                ? kCatalogForegroundTimeBudget
                : kCatalogBackgroundTimeBudget;
        Batch.reserve(ItemLimit);

        int Processed = 0;
        while (GCatalogScanFront <=
                   GCatalogScanBack &&
               Processed < ObjectLimit)
        {
            int32 ObjectIndex = 0;
            if (GCatalogScanNewestNext)
            {
                ObjectIndex =
                    GCatalogScanBack--;
            }
            else
            {
                ObjectIndex =
                    GCatalogScanFront++;
            }
            GCatalogScanNewestNext =
                !GCatalogScanNewestNext;
            ++GCatalogScanIndex;
            ++Processed;

            FItemMetadata Metadata;
            bool AddedItem = false;
            if (TryBuildCatalogItem(
                    ObjectIndex, &Metadata) &&
                GCatalogTokens.insert(
                    Metadata.Token).second)
            {
                Batch.push_back(std::move(Metadata));
                AddedItem = true;
            }

            if (static_cast<int>(Batch.size()) >=
                    ItemLimit ||
                (((Processed & 15) == 0 ||
                  AddedItem) &&
                std::chrono::steady_clock::now() -
                    Started >= TimeBudget))
            {
                break;
            }
        }

        GCatalogComplete =
            GCatalogScanFront >
                GCatalogScanBack &&
            CurrentObjectCount <=
                GCatalogScanLimit;
        if (GCatalogComplete)
            GCatalogScanIndex =
                GCatalogScanLimit;
        PublishCatalogBatch(Batch);
        return true;
    }

    static bool IsSafeSoftSubPath(
        const FString& SubPath)
    {
        if (SubPath.NumElements < 0 ||
            SubPath.MaxElements <
                SubPath.NumElements ||
            SubPath.MaxElements > 1024)
        {
            return false;
        }
        if (SubPath.NumElements == 0)
            return true;
        if (!SubPath.Data ||
            !SDK::MemReadable(
                SubPath.Data,
                static_cast<size_t>(
                    SubPath.NumElements) *
                    sizeof(wchar_t)))
        {
            return false;
        }
        return SubPath.Data[
            SubPath.NumElements - 1] == L'\0';
    }

    static bool CopySafeSoftSubPath(
        const FString& Source,
        UEAllocatedWString& Destination)
    {
        if (!IsSafeSoftSubPath(Source))
            return false;
        if (Source.NumElements <= 1)
        {
            Destination.clear();
            return true;
        }
        Destination.assign(
            Source.Data,
            Source.Data +
                Source.NumElements - 1);
        return true;
    }

    static uint32 ResidentSoftReferenceSize()
    {
        if (VersionInfo.EngineVersion <= 4.16)
        {
            return static_cast<uint32>(
                offsetof(
                    FSoftObjectPtr,
                    ObjectID) +
                sizeof(FString));
        }
        if (VersionInfo.FortniteVersion >= 23)
        {
            const uint32 SubPathOffset =
                VersionInfo.EngineVersion < 5.3
                    ? 0x18
                    : 0x10;
            return SubPathOffset +
                sizeof(FString);
        }
        return static_cast<uint32>(
            offsetof(
                FSoftObjectPtr,
                ObjectID) +
            sizeof(FSoftObjectPath));
    }

    static const UObject* ResolveResidentSoftReference(
        FSoftObjectPtr* SoftObject,
        const UClass* ObjectClass,
        UEAllocatedWString* SafePath = nullptr)
    {
        if (SafePath)
            SafePath->clear();
        if (!SoftObject || !ObjectClass)
            return nullptr;

        auto Weak = SoftObject->WeakPtr.Get();
        const UObject* WeakResident =
            IsLiveObject(Weak) &&
            Weak->IsA(ObjectClass)
                ? Weak
                : nullptr;
        if (WeakResident && !SafePath)
            return WeakResident;

        UEAllocatedWString Path;
        if (VersionInfo.EngineVersion <= 4.16)
        {
            const auto& LegacyPath =
                *reinterpret_cast<const FString*>(
                    reinterpret_cast<const uint8_t*>(
                        SoftObject) +
                    offsetof(
                        FSoftObjectPtr,
                        ObjectID));
            if (!IsSafeSoftSubPath(LegacyPath) ||
                LegacyPath.NumElements <= 1)
            {
                return nullptr;
            }
            if (!CopySafeSoftSubPath(
                    LegacyPath,
                    Path))
            {
                return nullptr;
            }
        }
        else if (VersionInfo.FortniteVersion >= 23)
        {
            const uint8_t* Value =
                reinterpret_cast<const uint8_t*>(
                    SoftObject);
            const uint32_t PackageOffset =
                VersionInfo.EngineVersion < 5.3
                    ? 0x10
                    : 0x08;
            const uint32_t AssetOffset =
                VersionInfo.EngineVersion < 5.3
                    ? 0x14
                    : 0x0C;
            const uint32_t SubPathOffset =
                VersionInfo.EngineVersion < 5.3
                    ? 0x18
                    : 0x10;
            if (!SDK::MemReadable(
                    Value + PackageOffset,
                    sizeof(FName)) ||
                !SDK::MemReadable(
                    Value + AssetOffset,
                    sizeof(FName)) ||
                !SDK::MemReadable(
                    Value + SubPathOffset,
                    sizeof(FString)))
            {
                return nullptr;
            }

            const auto& PackageName =
                *reinterpret_cast<const FName*>(
                    Value + PackageOffset);
            const auto& AssetName =
                *reinterpret_cast<const FName*>(
                    Value + AssetOffset);
            const auto& SubPath =
                *reinterpret_cast<const FString*>(
                    Value + SubPathOffset);
            if (!PackageName.IsValid() ||
                !IsSafeSoftSubPath(SubPath))
            {
                return nullptr;
            }
            Path = PackageName.ToWString();
            if (AssetName.IsValid())
            {
                Path += L".";
                Path += AssetName.ToWString();
            }
            if (SubPath.NumElements > 1)
            {
                UEAllocatedWString SafeSubPath;
                if (!CopySafeSoftSubPath(
                        SubPath,
                        SafeSubPath))
                {
                    return nullptr;
                }
                Path += L":";
                Path += SafeSubPath;
            }
        }
        else
        {
            const auto& ObjectPath =
                SoftObject->ObjectID;
            if (!ObjectPath.AssetPathName.IsValid() ||
                !IsSafeSoftSubPath(
                    ObjectPath.SubPathString))
            {
                return nullptr;
            }
            Path =
                ObjectPath.AssetPathName.ToWString();
            if (ObjectPath.SubPathString
                    .NumElements > 1)
            {
                UEAllocatedWString SafeSubPath;
                if (!CopySafeSoftSubPath(
                        ObjectPath.SubPathString,
                        SafeSubPath))
                {
                    return nullptr;
                }
                Path += L":";
                Path += SafeSubPath;
            }
        }

        if (Path.empty() ||
            Path.size() > 2048 ||
            Path[0] != L'/')
        {
            return nullptr;
        }

        if (SafePath)
            *SafePath = Path;
        if (WeakResident)
            return WeakResident;
        if (!SDK::Offsets::StaticFindObject)
            return nullptr;
        auto Resident =
            SDK::StaticFindObject(
                Path.c_str(),
                ObjectClass);
        return IsLiveObject(Resident) &&
            Resident->IsA(ObjectClass)
                ? Resident
                : nullptr;
    }

    // ProcessEvent constructs a returned soft object path inside the caller's
    // parameter buffer. Unlike reflected properties owned by an item
    // definition, that temporary FString belongs to us and must be released
    // after its path has been copied/resolved. Keep the cleanup guarded because
    // this compatibility path deliberately accepts several historical layouts.
    static void FreeReturnedSoftReferencePath(
        FSoftObjectPtr* SoftObject)
    {
        if (!SoftObject)
            return;

        FString* OwnedPath = nullptr;
        if (VersionInfo.EngineVersion <= 4.16)
        {
            OwnedPath = reinterpret_cast<FString*>(
                reinterpret_cast<uint8_t*>(SoftObject) +
                offsetof(FSoftObjectPtr, ObjectID));
        }
        else if (VersionInfo.FortniteVersion >= 23)
        {
            OwnedPath = reinterpret_cast<FString*>(
                reinterpret_cast<uint8_t*>(SoftObject) +
                (VersionInfo.EngineVersion < 5.3
                    ? 0x18
                    : 0x10));
        }
        else
        {
            OwnedPath = &SoftObject->ObjectID.SubPathString;
        }

        __try
        {
            if (OwnedPath &&
                OwnedPath->Data &&
                IsSafeSoftSubPath(*OwnedPath))
            {
                OwnedPath->Free();
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // A malformed optional preview getter must not take down the game
            // server. The outer icon retry will simply keep its placeholder.
        }
    }

    static const UTexture2D* ResolveBrushTexture(
        const UFortItemDefinition* Definition,
        const char* PropertyName,
        UEAllocatedWString* SoftPath,
        FPreviewSoftReference* PendingSoftReference = nullptr)
    {
        if (SoftPath)
            SoftPath->clear();
        if (PendingSoftReference)
            *PendingSoftReference = {};
        auto Property =
            Definition->GetProperty(PropertyName);
        if (!Property)
            return nullptr;

        const uint32 PropertyOffset =
            SDK::ReadPropertyOffset(
                GetFromOffset<uint32>(
                    Property,
                    Offsets::Offset_Internal));
        if (PropertyOffset == UINT32_MAX ||
            PropertyOffset > 0x10000)
        {
            return nullptr;
        }

        const uint32 ElementSize =
            Offsets::ElementSize &&
            SDK::MemReadable(
                reinterpret_cast<const uint8_t*>(Property) +
                    Offsets::ElementSize,
                sizeof(uint32))
                ? GetFromOffset<uint32>(
                    Property, Offsets::ElementSize)
                : 0;
        const uint8_t* Value =
            reinterpret_cast<const uint8_t*>(Definition) +
            PropertyOffset;
        auto TextureClass = UTexture2D::StaticClass();
        if (!TextureClass)
            return nullptr;

        if (ElementSize == sizeof(UObject*) &&
            SDK::MemReadable(Value, sizeof(UObject*)))
        {
            auto Direct =
                *reinterpret_cast<UObject* const*>(Value);
            if (IsLiveObject(Direct) &&
                Direct->IsA(TextureClass))
            {
                return static_cast<const UTexture2D*>(
                    Direct);
            }
        }

        const uint32 SoftReferenceSize =
            ResidentSoftReferenceSize();
        if (ElementSize == SoftReferenceSize &&
            SDK::MemReadable(Value, ElementSize))
        {
            auto SoftObject =
                reinterpret_cast<FSoftObjectPtr*>(
                    const_cast<uint8_t*>(Value));
            UEAllocatedWString LocalPath;
            auto PathOutput = SoftPath
                ? SoftPath
                : (PendingSoftReference ? &LocalPath : nullptr);
            // The HUD can stream a texture through a copy of this soft pointer,
            // leaving this definition's weak cache empty. Resolve its path with
            // StaticFindObject only; never call the synchronous package loader
            // from this optional game-thread work.
            auto Resolved =
                ResolveResidentSoftReference(
                    SoftObject,
                    TextureClass,
                    PathOutput);
            if (Resolved)
            {
                return static_cast<const UTexture2D*>(
                    Resolved);
            }
            const UEAllocatedWString* CandidatePath =
                SoftPath ? SoftPath : &LocalPath;
            if (PendingSoftReference && CandidatePath &&
                !CandidatePath->empty() &&
                ElementSize <= PendingSoftReference->Bytes.size())
            {
                memcpy(
                    PendingSoftReference->Bytes.data(),
                    Value,
                    ElementSize);
                PendingSoftReference->Size = ElementSize;
                PendingSoftReference->Path = *CandidatePath;
                PendingSoftReference->Valid = true;
            }
            // A known soft-object property must never be reinterpreted as a
            // Slate brush when its resident texture is temporarily absent.
            return nullptr;
        }

        if (ElementSize < 0x48 ||
            ElementSize > 0x200)
        {
            return nullptr;
        }
        auto BrushStruct =
            SDK::Offsets::StaticFindObject
                ? static_cast<const UStruct*>(
                    SDK::StaticFindObject(
                        L"/Script/SlateCore.SlateBrush",
                        nullptr))
                : nullptr;
        if (!BrushStruct)
            return nullptr;
        const uint32 ResourceOffset =
            BrushStruct->GetOffset("ResourceObject");
        if (ResourceOffset == UINT32_MAX ||
            ResourceOffset > 0x400 ||
            (ElementSize &&
             ElementSize <
                 ResourceOffset + sizeof(UObject*)))
        {
            return nullptr;
        }

        const uint8_t* Brush = Value;
        if (!SDK::MemReadable(
                Brush + ResourceOffset,
                sizeof(UObject*)))
        {
            return nullptr;
        }

        auto Resource =
            *reinterpret_cast<UObject* const*>(
                Brush + ResourceOffset);
        if (TextureClass &&
            IsLiveObject(Resource) &&
            Resource->IsA(TextureClass))
        {
            return static_cast<const UTexture2D*>(Resource);
        }

        // Some builds leave ResourceObject empty but retain a path in
        // ResourceName. A resident lookup is acceptable; never stream/load an
        // icon asset from this optional admin UI.
        const uint32 ResourceNameOffset =
            BrushStruct->GetOffset("ResourceName");
        if (ResourceNameOffset == UINT32_MAX ||
            ResourceNameOffset > 0x400 ||
            ResourceNameOffset + sizeof(FName) >
                ElementSize ||
            !SDK::MemReadable(
                Brush + ResourceNameOffset,
                sizeof(FName)))
        {
            return nullptr;
        }

        const auto& ResourceName =
            *reinterpret_cast<const FName*>(
                Brush + ResourceNameOffset);
        if (!ResourceName.IsValid())
            return nullptr;

        auto Path = ResourceName.ToWString();
        if (Path.empty() || Path[0] != L'/')
        {
            return nullptr;
        }
        if (SoftPath)
            *SoftPath = Path;
        if (!SDK::Offsets::StaticFindObject)
            return nullptr;

        auto Loaded = static_cast<const UTexture2D*>(
            SDK::StaticFindObject(
                Path.c_str(), TextureClass));
        return IsLiveObject(Loaded) &&
            Loaded->IsA(TextureClass)
                ? Loaded
                : nullptr;
    }

    static bool IsExecutableAddress(const void* Address)
    {
        MEMORY_BASIC_INFORMATION Information{};
        if (!Address ||
            !VirtualQuery(Address, &Information, sizeof(Information)) ||
            Information.State != MEM_COMMIT ||
            (Information.Protect & PAGE_GUARD) != 0)
            return false;
        const DWORD Protection = Information.Protect & 0xFF;
        return Protection == PAGE_EXECUTE ||
            Protection == PAGE_EXECUTE_READ ||
            Protection == PAGE_EXECUTE_READWRITE ||
            Protection == PAGE_EXECUTE_WRITECOPY;
    }

    static bool IsExactInputParameter(
        const FReflectedPropertyView& View,
        uint32 ExpectedSize,
        uint32 BufferSize)
    {
        return View.ArrayDimension == 1 &&
            View.ElementSize == ExpectedSize &&
            (View.PropertyFlags & kCpfParm) != 0 &&
            (View.PropertyFlags &
                (kCpfOutParm | kCpfReturnParm)) == 0 &&
            View.Offset <= BufferSize &&
            View.ElementSize <= BufferSize - View.Offset;
    }

    static bool IsExactStructMember(
        const FReflectedPropertyView& View,
        uint32 ExpectedSize,
        uint32 StructSize)
    {
        return View.ArrayDimension == 1 &&
            View.ElementSize == ExpectedSize &&
            View.Offset <= StructSize &&
            View.ElementSize <= StructSize - View.Offset;
    }

    static bool BuildAsyncIconLoadSchemaUnsafe()
    {
        if (GAsyncIconLoadingDisabled ||
            !SDK::Offsets::StaticFindObject)
            return false;

        const ULONGLONG Now = GetTickCount64();
        if (GAsyncIconLoadSchema.Readable &&
            IsLiveObject(GAsyncIconLoadSchema.LibraryClass) &&
            IsLiveObject(GAsyncIconLoadSchema.DefaultObject) &&
            IsLiveObject(GAsyncIconLoadSchema.Function))
            return true;
        if (GAsyncIconLoadSchema.Initialized &&
            Now < GAsyncIconLoadSchema.NextRetryAt)
            return false;

        GAsyncIconLoadSchema = {};
        GAsyncIconLoadSchema.Initialized = true;
        GAsyncIconLoadSchema.NextRetryAt =
            Now + kReflectionSchemaRetryMs;
        auto ClassClass = UClass::StaticClass();
        auto LibraryClass = static_cast<const UClass*>(
            SDK::StaticFindObject(
                L"/Script/Engine.KismetSystemLibrary",
                ClassClass));
        if (!ClassClass || !IsLiveObject(LibraryClass) ||
            !LibraryClass->IsA(ClassClass))
            return false;
        auto DefaultObject = LibraryClass->GetDefaultObj();
        auto Function = DefaultObject
            ? DefaultObject->GetFunction("LoadAsset")
            : nullptr;
        if (!IsLiveObject(DefaultObject) ||
            !DefaultObject->IsA(LibraryClass) ||
            !IsLiveObject(Function) ||
            !Function->GetNativeFunc() ||
            !IsExecutableAddress(Function->GetNativeFunc()))
            return false;

        const int32 RawBufferSize = Function->GetPropertiesSize();
        if (RawBufferSize <= 0 || RawBufferSize > 0x100)
            return false;
        const uint32 BufferSize = static_cast<uint32>(RawBufferSize);
        auto WorldProperty = Function->GetProperty(
            "WorldContextObject", kCastClassObjectProperty);
        auto AssetProperty = Function->GetProperty(
            "Asset", kCastClassSoftObjectProperty);
        auto OnLoadedProperty = Function->GetProperty(
            "OnLoaded", kCastClassDelegateProperty);
        auto LatentProperty = Function->GetProperty(
            "LatentInfo", kCastClassStructProperty);
        FReflectedPropertyView WorldView;
        FReflectedPropertyView AssetView;
        FReflectedPropertyView OnLoadedView;
        FReflectedPropertyView LatentView;
        if (!WorldProperty || !AssetProperty ||
            !OnLoadedProperty || !LatentProperty ||
            !TryReadReflectedPropertyViewUnsafe(WorldProperty, WorldView) ||
            !TryReadReflectedPropertyViewUnsafe(AssetProperty, AssetView) ||
            !TryReadReflectedPropertyViewUnsafe(OnLoadedProperty, OnLoadedView) ||
            !TryReadReflectedPropertyViewUnsafe(LatentProperty, LatentView) ||
            !IsExactInputParameter(WorldView, sizeof(UObject*), BufferSize) ||
            !IsExactInputParameter(
                AssetView, ResidentSoftReferenceSize(), BufferSize) ||
            !IsExactInputParameter(
                OnLoadedView,
                static_cast<uint32>(FScriptDelegate::Size()),
                BufferSize) ||
            !IsExactInputParameter(LatentView, 0x18, BufferSize) ||
            !HasExpectedFunctionParameterCountUnsafe(Function, 4) ||
            PropertyRangesOverlap(WorldView, AssetView) ||
            PropertyRangesOverlap(WorldView, OnLoadedView) ||
            PropertyRangesOverlap(WorldView, LatentView) ||
            PropertyRangesOverlap(AssetView, OnLoadedView) ||
            PropertyRangesOverlap(AssetView, LatentView) ||
            PropertyRangesOverlap(OnLoadedView, LatentView))
            return false;

        auto LatentStruct = static_cast<const UStruct*>(
            SDK::StaticFindObject(
                L"/Script/Engine.LatentActionInfo", nullptr));
        if (!IsLiveObject(LatentStruct) || !LatentStruct->Class ||
            LatentStruct->Class->Name.ToUtf8() != "ScriptStruct" ||
            LatentStruct->Name.ToUtf8() != "LatentActionInfo" ||
            LatentStruct->GetPropertiesSize() !=
                static_cast<int32>(LatentView.ElementSize))
            return false;
        auto LinkageProperty = LatentStruct->GetProperty(
            "Linkage", kCastClassIntProperty);
        auto UuidProperty = LatentStruct->GetProperty(
            "UUID", kCastClassIntProperty);
        auto ExecutionFunctionProperty = LatentStruct->GetProperty(
            "ExecutionFunction", kCastClassNameProperty);
        auto CallbackTargetProperty = LatentStruct->GetProperty(
            "CallbackTarget", kCastClassObjectProperty);
        FReflectedPropertyView LinkageView;
        FReflectedPropertyView UuidView;
        FReflectedPropertyView ExecutionFunctionView;
        FReflectedPropertyView CallbackTargetView;
        const uint32 RuntimeNameSize =
            VersionInfo.FortniteVersion >= 20.0
                ? sizeof(int32)
                : sizeof(FName);
        if (!LinkageProperty || !UuidProperty ||
            !ExecutionFunctionProperty || !CallbackTargetProperty ||
            !TryReadReflectedPropertyViewUnsafe(LinkageProperty, LinkageView) ||
            !TryReadReflectedPropertyViewUnsafe(UuidProperty, UuidView) ||
            !TryReadReflectedPropertyViewUnsafe(
                ExecutionFunctionProperty, ExecutionFunctionView) ||
            !TryReadReflectedPropertyViewUnsafe(
                CallbackTargetProperty, CallbackTargetView) ||
            !IsExactStructMember(
                LinkageView, sizeof(int32), LatentView.ElementSize) ||
            !IsExactStructMember(
                UuidView, sizeof(int32), LatentView.ElementSize) ||
            !IsExactStructMember(
                ExecutionFunctionView, RuntimeNameSize, LatentView.ElementSize) ||
            !IsExactStructMember(
                CallbackTargetView, sizeof(UObject*), LatentView.ElementSize) ||
            PropertyRangesOverlap(LinkageView, UuidView) ||
            PropertyRangesOverlap(LinkageView, ExecutionFunctionView) ||
            PropertyRangesOverlap(LinkageView, CallbackTargetView) ||
            PropertyRangesOverlap(UuidView, ExecutionFunctionView) ||
            PropertyRangesOverlap(UuidView, CallbackTargetView) ||
            PropertyRangesOverlap(ExecutionFunctionView, CallbackTargetView))
            return false;

        GAsyncIconLoadSchema.LibraryClass = LibraryClass;
        GAsyncIconLoadSchema.DefaultObject = DefaultObject;
        GAsyncIconLoadSchema.Function = Function;
        GAsyncIconLoadSchema.BufferSize = BufferSize;
        GAsyncIconLoadSchema.WorldContext = WorldView;
        GAsyncIconLoadSchema.Asset = AssetView;
        GAsyncIconLoadSchema.OnLoaded = OnLoadedView;
        GAsyncIconLoadSchema.LatentInfo = LatentView;
        GAsyncIconLoadSchema.LatentLinkageOffset = LinkageView.Offset;
        GAsyncIconLoadSchema.LatentUuidOffset = UuidView.Offset;
        GAsyncIconLoadSchema.LatentCallbackTargetOffset =
            CallbackTargetView.Offset;
        GAsyncIconLoadSchema.Readable = true;
        return true;
    }

    static bool EnsureAsyncIconLoadSchema()
    {
        __try { return BuildAsyncIconLoadSchemaUnsafe(); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            GAsyncIconLoadSchema = {};
            GAsyncIconLoadingDisabled = true;
            return false;
        }
    }

    static bool InvokeAsyncIconLoad(const uint8* Parameters)
    {
        __try
        {
            GAsyncIconLoadSchema.DefaultObject->ProcessEvent(
                GAsyncIconLoadSchema.Function,
                const_cast<uint8*>(Parameters));
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            GAsyncIconLoadingDisabled = true;
            return false;
        }
    }

    static void PruneAsyncIconLoads(ULONGLONG Now)
    {
        for (auto It = GAsyncIconLoads.begin();
             It != GAsyncIconLoads.end();)
        {
            const bool WorldChanged =
                GObservedWorldIdentity !=
                    (std::numeric_limits<uint64_t>::max)() &&
                It->WorldIdentity != GObservedWorldIdentity;
            const bool TimedOut =
                Now - It->StartedAt >= kAsyncIconLoadTimeoutMs;
            const UObject* Resident = nullptr;
            if (!WorldChanged && SDK::Offsets::StaticFindObject &&
                IsLiveObject(It->ExpectedClass))
            {
                Resident = SDK::StaticFindObject(
                    It->Path.c_str(), It->ExpectedClass);
                if (!IsLiveObject(Resident) ||
                    !Resident->IsA(It->ExpectedClass))
                {
                    Resident = nullptr;
                }
            }
            if (!WorldChanged && !TimedOut && !Resident)
            {
                ++It;
                continue;
            }
            if (Resident)
                GAsyncIconLoadFailures.erase(It->Path);
            if (TimedOut && !WorldChanged && !Resident)
            {
                GAsyncIconLoadFailures[It->Path] = {
                    It->WorldIdentity,
                    Now + (It->PickerOnly
                        ? kAsyncIconPickerRetryMs
                        : kAsyncIconLiveRetryMs),
                    It->PickerOnly
                };
                // AttemptedPaths is the permanent guard used after a native
                // request has completed or a synchronous fallback has
                // faulted. A latent request that genuinely timed out is
                // different: once its bounded backoff expires, the caller
                // must be allowed to issue a fresh load for the same CID
                // portrait. Leaving this token set made FN30's map-icon retry
                // queue permanently return the temporary resident fallback.
                GAsyncIconAttemptedPaths.erase(It->Path);
            }
            It = GAsyncIconLoads.erase(It);
        }
    }

    static void CompleteAsyncIconLoad(
        uintptr_t ItemToken,
        uint64_t ItemIdentity,
        const wchar_t* ResidentPath = nullptr)
    {
        const bool HasPath = ResidentPath && ResidentPath[0] == L'/';
        for (auto It = GAsyncIconLoads.begin();
             It != GAsyncIconLoads.end();)
        {
            const bool SameItem =
                It->ItemToken == ItemToken &&
                It->ItemIdentity == ItemIdentity;
            const bool SamePath = HasPath && It->Path == ResidentPath;
            // A stable request owner can serialize several different assets.
            // When the completed path is known, never discard bookkeeping for
            // another latent load that happens to share that owner.
            if (SamePath || (!HasPath && SameItem))
                It = GAsyncIconLoads.erase(It);
            else
                ++It;
        }
        if (HasPath)
            GAsyncIconLoadFailures.erase(
                std::wstring(ResidentPath));
    }

    static bool RequestAsyncIconLoad(
        const FPreviewSoftReference& Reference,
        uintptr_t ItemToken,
        uint64_t ItemIdentity,
        const UClass* ExpectedClass,
        bool PickerOnly,
        ULONGLONG* DeferredUntil,
        bool* StartedLoad)
    {
        *DeferredUntil = 0;
        *StartedLoad = false;
        if (GAsyncIconLoadingDisabled ||
            !Reference.Valid || !ItemToken || !ItemIdentity ||
            !IsLiveObject(ExpectedClass) ||
            Reference.Path.empty() || Reference.Path.size() > 2048 ||
            Reference.Path[0] != L'/' || Reference.Size == 0 ||
            Reference.Size > Reference.Bytes.size())
            return false;

        const ULONGLONG Now = GetTickCount64();
        const std::wstring Path(Reference.Path.c_str());
        PruneAsyncIconLoads(Now);
        for (auto& Load : GAsyncIconLoads)
        {
            if (Load.Path == Path)
            {
                if (!PickerOnly)
                    Load.PickerOnly = false;
                *DeferredUntil = Now + (PickerOnly ? 250 : 100);
                return true;
            }
        }

        auto Failure = GAsyncIconLoadFailures.find(Path);
        if (Failure != GAsyncIconLoadFailures.end())
        {
            if (Failure->second.WorldIdentity ==
                    GObservedWorldIdentity &&
                Failure->second.RetryAt > Now &&
                (PickerOnly || !Failure->second.PickerOnly))
                return false;
            GAsyncIconLoadFailures.erase(Failure);
        }
        if (GAsyncIconAttemptedPaths.find(Path) !=
                GAsyncIconAttemptedPaths.end())
            return false;

        size_t PickerLoads = 0;
        for (const auto& Load : GAsyncIconLoads)
        {
            if (Load.PickerOnly)
                ++PickerLoads;
        }
        if (GAsyncIconLoads.size() >= kMaxAsyncIconLoads ||
            (PickerOnly && PickerLoads >= kMaxAsyncPickerIconLoads))
        {
            *DeferredUntil = Now + (PickerOnly ? 500 : 200);
            return true;
        }
        if (Now - GLastAsyncIconLoadAt < kAsyncIconLoadRequestIntervalMs)
        {
            *DeferredUntil =
                GLastAsyncIconLoadAt + kAsyncIconLoadRequestIntervalMs;
            return true;
        }
        if (!EnsureAsyncIconLoadSchema())
        {
            // Reflection can be temporarily unavailable while the early world
            // is still initializing. Keep the same requested CID queued until
            // the schema's bounded retry instead of burning through unrelated
            // catalog candidates as if their packages were invalid.
            if (!GAsyncIconLoadingDisabled &&
                GAsyncIconLoadSchema.Initialized &&
                GAsyncIconLoadSchema.NextRetryAt > Now)
            {
                *DeferredUntil =
                    GAsyncIconLoadSchema.NextRetryAt;
                return true;
            }
            return false;
        }
        if (Reference.Size !=
            GAsyncIconLoadSchema.Asset.ElementSize)
        {
            return false;
        }

        auto World = UWorld::GetWorld();
        const uint64_t WorldIdentity = GetLiveObjectIdentity(World);
        if (!IsLiveObject(World) || !WorldIdentity ||
            !IsLiveObject(GAsyncIconLoadSchema.DefaultObject) ||
            !IsLiveObject(GAsyncIconLoadSchema.Function))
            return false;

        alignas(16) uint8 Parameters[0x100]{};
        auto WorldContext = static_cast<UObject*>(World);
        memcpy(
            Parameters + GAsyncIconLoadSchema.WorldContext.Offset,
            &WorldContext,
            sizeof(WorldContext));
        memcpy(
            Parameters + GAsyncIconLoadSchema.Asset.Offset,
            Reference.Bytes.data(),
            Reference.Size);
        uint8* Latent =
            Parameters + GAsyncIconLoadSchema.LatentInfo.Offset;
        const int32 Linkage = -1;
        if (GNextAsyncIconLoadUuid >= 0x6D47FFFE ||
            GNextAsyncIconLoadUuid <= 0)
            GNextAsyncIconLoadUuid = 0x4D470000;
        const int32 Uuid = GNextAsyncIconLoadUuid++;
        UObject* CallbackTarget = GAsyncIconLoadSchema.DefaultObject;
        memcpy(
            Latent + GAsyncIconLoadSchema.LatentLinkageOffset,
            &Linkage,
            sizeof(Linkage));
        memcpy(
            Latent + GAsyncIconLoadSchema.LatentUuidOffset,
            &Uuid,
            sizeof(Uuid));
        memcpy(
            Latent + GAsyncIconLoadSchema.LatentCallbackTargetOffset,
            &CallbackTarget,
            sizeof(CallbackTarget));
        if (!InvokeAsyncIconLoad(Parameters))
            return false;

        GLastAsyncIconLoadAt = Now;
        GAsyncIconAttemptedPaths.insert(Path);
        GAsyncIconLoads.push_back({
            ItemToken, ItemIdentity, WorldIdentity,
            ExpectedClass, PickerOnly, Now, Path
        });
        *DeferredUntil = Now + (PickerOnly ? 250 : 100);
        *StartedLoad = true;
        return true;
    }

    static FSoftObjectLoadResult
        ResolveOrRequestSoftObjectUnsafe(
            const void* Owner,
            const void* SoftReference,
            uint32 SoftReferenceSize,
            const UClass* ExpectedClass)
    {
        FSoftObjectLoadResult Result{
            nullptr,
            EPreviewTextureLoadState::Unavailable,
            0
        };
        if (!Owner || !SoftReference || !ExpectedClass ||
            SoftReferenceSize != ResidentSoftReferenceSize() ||
            !SDK::MemReadable(
                SoftReference,
                SoftReferenceSize))
        {
            return Result;
        }

        auto OwnerObject =
            reinterpret_cast<const UObject*>(Owner);
        const uint64_t OwnerIdentity =
            GetLiveObjectIdentity(OwnerObject);
        if (!OwnerIdentity || !IsLiveObject(ExpectedClass))
            return Result;

        auto SoftObject = reinterpret_cast<FSoftObjectPtr*>(
            const_cast<void*>(SoftReference));
        UEAllocatedWString Path;
        auto Resident = ResolveResidentSoftReference(
            SoftObject,
            ExpectedClass,
            &Path);
        if (Resident)
        {
            CompleteAsyncIconLoad(
                reinterpret_cast<uintptr_t>(OwnerObject),
                OwnerIdentity,
                Path.empty() ? nullptr : Path.c_str());
            Result.Object = Resident;
            Result.State = EPreviewTextureLoadState::Resident;
            return Result;
        }
        if (Path.empty() || Path.size() > 2048 || Path[0] != L'/')
            return Result;

        FPreviewSoftReference Reference;
        if (SoftReferenceSize > Reference.Bytes.size())
            return Result;
        memcpy(
            Reference.Bytes.data(),
            SoftReference,
            SoftReferenceSize);
        Reference.Size = SoftReferenceSize;
        Reference.Path = Path;
        Reference.Valid = true;

        const uintptr_t OwnerToken =
            reinterpret_cast<uintptr_t>(OwnerObject);
        const ULONGLONG Now = GetTickCount64();
        ULONGLONG DeferredUntil = 0;
        bool StartedLoad = false;
        if (RequestAsyncIconLoad(
                Reference,
                OwnerToken,
                OwnerIdentity,
                ExpectedClass,
                false,
                &DeferredUntil,
                &StartedLoad) &&
            DeferredUntil > Now)
        {
            Result.State = EPreviewTextureLoadState::Pending;
            Result.RetryAfterMs = DeferredUntil - Now;
        }
        return Result;
    }

    static FPreviewTextureLoadResult
        ResolveOrRequestPreviewTextureUnsafe(
            const void* Owner,
            const void* SoftReference,
            uint32 SoftReferenceSize)
    {
        auto TextureClass = UTexture2D::StaticClass();
        auto Generic = ResolveOrRequestSoftObjectUnsafe(
            Owner,
            SoftReference,
            SoftReferenceSize,
            TextureClass);
        return {
            static_cast<const UTexture2D*>(Generic.Object),
            Generic.State,
            Generic.RetryAfterMs
        };
    }

    constexpr const char* kPreviewProperties[] = {
        "SmallPreviewImage",
        "LargePreviewImage",
        "WidePreviewImage",
        "DetailsPreviewImage"
    };

    constexpr const char* kPreviewGetterFunctions[] = {
        "GetSmallPreviewImage",
        "GetLargePreviewImage",
        "GetWidePreviewImage"
    };

    // Keep this helper POD-only so MSVC permits __finally. If an optional
    // reflected getter faults while its returned path is being inspected, the
    // UE-allocated FString is still released before the outer SEH guard turns
    // the icon attempt into a harmless retry.
    static const UTexture2D*
        ResolveReturnedPreviewTextureAndFree(
            FSoftObjectPtr* SoftObject,
            const UClass* TextureClass)
    {
        const UObject* Resolved = nullptr;
        __try
        {
            Resolved = ResolveResidentSoftReference(
                SoftObject, TextureClass, nullptr);
        }
        __finally
        {
            FreeReturnedSoftReferencePath(SoftObject);
        }
        return Resolved
            ? static_cast<const UTexture2D*>(Resolved)
            : nullptr;
    }

    static const UTexture2D* ResolvePreviewGetterTexture(
        const UFortItemDefinition* Definition,
        const char* FunctionName)
    {
        if (!Definition || !FunctionName)
            return nullptr;

        auto Function =
            Definition->GetFunction(FunctionName);
        if (!Function)
            return nullptr;

        const int32 BufferSize =
            Function->GetPropertiesSize();
        if (BufferSize <= 0 || BufferSize > 0x80)
            return nullptr;

        auto TextureClass = UTexture2D::StaticClass();
        if (!TextureClass)
            return nullptr;

        auto ReturnProperty =
            Function->GetProperty(
                "ReturnValue",
                kCastClassSoftObjectProperty);
        FReflectedPropertyView ReturnView;
        if (!ReturnProperty ||
            !TryReadReflectedPropertyViewUnsafe(
                ReturnProperty, ReturnView) ||
            ReturnView.ArrayDimension != 1 ||
            ReturnView.ElementSize !=
                ResidentSoftReferenceSize() ||
            (ReturnView.PropertyFlags & kCpfParm) == 0 ||
            (ReturnView.PropertyFlags &
                kCpfReturnParm) == 0 ||
            ReturnView.Offset >=
                static_cast<uint32>(BufferSize) ||
            ReturnView.ElementSize >
                static_cast<uint32>(BufferSize) -
                    ReturnView.Offset ||
            !HasExpectedFunctionParameterCountUnsafe(
                Function, 1))
        {
            return nullptr;
        }

        alignas(16) uint8 Params[0x80]{};
        Definition->ProcessEvent(Function, Params);
        auto SoftObject =
            reinterpret_cast<FSoftObjectPtr*>(
                Params + ReturnView.Offset);
        return ResolveReturnedPreviewTextureAndFree(
            SoftObject, TextureClass);
    }

    static void DownsampleIcon(
        FIconPixels& Icon,
        int MaximumDimension)
    {
        if (!Icon.Success ||
            Icon.Width <= 0 ||
            Icon.Height <= 0 ||
            Icon.Pixels.empty() ||
            (Icon.Width <= MaximumDimension &&
             Icon.Height <= MaximumDimension))
        {
            return;
        }

        const float Scale =
            static_cast<float>(MaximumDimension) /
            static_cast<float>(
                (std::max)(Icon.Width, Icon.Height));
        const int NewWidth =
            (std::max)(
                1,
                static_cast<int>(
                    Icon.Width * Scale + 0.5f));
        const int NewHeight =
            (std::max)(
                1,
                static_cast<int>(
                    Icon.Height * Scale + 0.5f));
        std::vector<unsigned char> Reduced(
            static_cast<size_t>(NewWidth) *
                NewHeight * 4);

        for (int Y = 0; Y < NewHeight; ++Y)
        {
            const int SourceY =
                (std::min)(
                    Icon.Height - 1,
                    static_cast<int>(Y / Scale));
            for (int X = 0; X < NewWidth; ++X)
            {
                const int SourceX =
                    (std::min)(
                        Icon.Width - 1,
                        static_cast<int>(X / Scale));
                const size_t Source =
                    (static_cast<size_t>(SourceY) *
                         Icon.Width +
                     SourceX) *
                    4;
                const size_t Destination =
                    (static_cast<size_t>(Y) *
                         NewWidth +
                     X) *
                    4;
                memcpy(
                    Reduced.data() + Destination,
                    Icon.Pixels.data() + Source,
                    4);
            }
        }

        Icon.Pixels = std::move(Reduced);
        Icon.Width = NewWidth;
        Icon.Height = NewHeight;
    }

    static bool BuildIconUnsafe(
        uintptr_t ItemToken,
        uint64_t ItemIdentity,
        bool PickerOnly,
        FIconPixels& Result)
    {
        Result = {};
        Result.ItemToken = ItemToken;
        Result.ItemIdentity = ItemIdentity;
        Result.PickerOnly = PickerOnly;
        auto Definition =
            reinterpret_cast<const UFortItemDefinition*>(
                ItemToken);
        if (!ItemIdentity ||
            GetLiveObjectIdentity(Definition) !=
                ItemIdentity ||
            ClassifyExistingItemDefinition(
                Definition) !=
                EExistingItemPlacement::Primary)
        {
            Result.RetryAfterMs = 30000;
            return true;
        }

        const ULONGLONG Now = GetTickCount64();
        bool FoundResidentTexture = false;
        FPreviewSoftReference PendingSoftReference;
        std::array<
            const UTexture2D*,
            sizeof(kPreviewProperties) /
                sizeof(kPreviewProperties[0]) +
                sizeof(kPreviewGetterFunctions) /
                sizeof(kPreviewGetterFunctions[0])>
            TriedResidentTextures{};
        size_t TriedResidentCount = 0;
        for (const char* Property :
             kPreviewProperties)
        {
            FPreviewSoftReference Candidate;
            UEAllocatedWString CandidatePath;
            auto Texture =
                ResolveBrushTexture(
                    Definition,
                    Property,
                    &CandidatePath,
                    &Candidate);
            if (!PendingSoftReference.Valid && Candidate.Valid)
                PendingSoftReference = std::move(Candidate);
            if (!Texture)
                continue;

            CompleteAsyncIconLoad(
                ItemToken,
                ItemIdentity,
                CandidatePath.empty()
                    ? nullptr
                    : CandidatePath.c_str());

            bool AlreadyTried = false;
            for (size_t Index = 0;
                 Index < TriedResidentCount;
                 ++Index)
            {
                if (TriedResidentTextures[Index] ==
                    Texture)
                {
                    AlreadyTried = true;
                    break;
                }
            }
            if (AlreadyTried)
                continue;
            TriedResidentTextures[
                TriedResidentCount++] = Texture;

            const uintptr_t TextureToken =
                reinterpret_cast<uintptr_t>(Texture);
            const uint64_t TextureIdentity =
                GetLiveObjectIdentity(Texture);
            if (!TextureIdentity)
                continue;
            FoundResidentTexture = true;

            auto DecodeFailure =
                GTextureDecodeFailures.find(
                    TextureToken);
            if (DecodeFailure !=
                    GTextureDecodeFailures.end() &&
                DecodeFailure->second.TextureIdentity ==
                    TextureIdentity &&
                DecodeFailure->second.RetryAt > Now &&
                (PickerOnly ||
                 !DecodeFailure->second.PickerOnly))
            {
                continue;
            }
            if (DecodeFailure !=
                    GTextureDecodeFailures.end() &&
                (DecodeFailure->second.TextureIdentity !=
                     TextureIdentity ||
                 (!PickerOnly &&
                  DecodeFailure->second.PickerOnly)))
            {
                GTextureDecodeFailures.erase(
                    DecodeFailure);
                DecodeFailure =
                    GTextureDecodeFailures.end();
            }

            Result.Pixels.clear();
            Result.Width = 0;
            Result.Height = 0;
            Result.Success =
                GameTextureBridge::ExtractToRGBA(
                    Texture,
                    Result.Pixels,
                    Result.Width,
                    Result.Height);
            if (Result.Success)
            {
                if (DecodeFailure !=
                    GTextureDecodeFailures.end())
                {
                    GTextureDecodeFailures.erase(
                        DecodeFailure);
                }
                Result.RetryAfterMs = 0;
                DownsampleIcon(Result, 128);
                return true;
            }

            // Never decode more than one resident texture in a game tick.
            // Back off unreadable mip identities exponentially, but retry them
            // later because Fortnite can stream CPU mip data into the same
            // live texture object without changing its UObject identity.
            const unsigned int FailureCount =
                DecodeFailure !=
                        GTextureDecodeFailures.end()
                    ? (std::min)(
                        DecodeFailure->second
                                .FailureCount +
                            1u,
                        16u)
                    : 1u;
            const unsigned int Shift =
                (std::min)(
                    FailureCount - 1u, 3u);
            const ULONGLONG BaseDelay =
                PickerOnly ? 3000 : 750;
            const ULONGLONG MaximumDelay =
                PickerOnly ? 24000 : 6000;
            const ULONGLONG RetryDelay =
                (std::min)(
                    MaximumDelay,
                    BaseDelay << Shift);
            GTextureDecodeFailures[
                TextureToken] = {
                    TextureIdentity,
                    Now + RetryDelay,
                    FailureCount,
                    PickerOnly
                };
            Result.RetryAfterMs = 250;
            return true;
        }

        // Newer item-definition layouts can synthesize or override the preview
        // through BlueprintPure getters while the backing reflected property is
        // absent on the concrete class. Invoke only validated, zero-input
        // soft-reference getters and still perform a find-only resident lookup;
        // this never streams a package or blocks on asset IO.
        for (const char* FunctionName :
             kPreviewGetterFunctions)
        {
            auto Texture = ResolvePreviewGetterTexture(
                Definition, FunctionName);
            if (!Texture)
                continue;

            CompleteAsyncIconLoad(ItemToken, ItemIdentity);

            bool AlreadyTried = false;
            for (size_t Index = 0;
                 Index < TriedResidentCount;
                 ++Index)
            {
                if (TriedResidentTextures[Index] ==
                    Texture)
                {
                    AlreadyTried = true;
                    break;
                }
            }
            if (AlreadyTried ||
                TriedResidentCount >=
                    TriedResidentTextures.size())
            {
                continue;
            }
            TriedResidentTextures[
                TriedResidentCount++] = Texture;

            const uintptr_t TextureToken =
                reinterpret_cast<uintptr_t>(Texture);
            const uint64_t TextureIdentity =
                GetLiveObjectIdentity(Texture);
            if (!TextureIdentity)
                continue;
            FoundResidentTexture = true;

            auto DecodeFailure =
                GTextureDecodeFailures.find(
                    TextureToken);
            if (DecodeFailure !=
                    GTextureDecodeFailures.end() &&
                DecodeFailure->second.TextureIdentity ==
                    TextureIdentity &&
                DecodeFailure->second.RetryAt > Now &&
                (PickerOnly ||
                 !DecodeFailure->second.PickerOnly))
            {
                continue;
            }
            if (DecodeFailure !=
                    GTextureDecodeFailures.end() &&
                (DecodeFailure->second.TextureIdentity !=
                     TextureIdentity ||
                 (!PickerOnly &&
                  DecodeFailure->second.PickerOnly)))
            {
                GTextureDecodeFailures.erase(
                    DecodeFailure);
                DecodeFailure =
                    GTextureDecodeFailures.end();
            }

            Result.Pixels.clear();
            Result.Width = 0;
            Result.Height = 0;
            Result.Success =
                GameTextureBridge::ExtractToRGBA(
                    Texture,
                    Result.Pixels,
                    Result.Width,
                    Result.Height);
            if (Result.Success)
            {
                if (DecodeFailure !=
                    GTextureDecodeFailures.end())
                {
                    GTextureDecodeFailures.erase(
                        DecodeFailure);
                }
                Result.RetryAfterMs = 0;
                DownsampleIcon(Result, 128);
                return true;
            }

            const unsigned int FailureCount =
                DecodeFailure !=
                        GTextureDecodeFailures.end()
                    ? (std::min)(
                        DecodeFailure->second
                                .FailureCount +
                            1u,
                        16u)
                    : 1u;
            const unsigned int Shift =
                (std::min)(FailureCount - 1u, 3u);
            const ULONGLONG BaseDelay =
                PickerOnly ? 3000 : 750;
            const ULONGLONG MaximumDelay =
                PickerOnly ? 24000 : 6000;
            const ULONGLONG RetryDelay =
                (std::min)(
                    MaximumDelay,
                    BaseDelay << Shift);
            GTextureDecodeFailures[
                TextureToken] = {
                    TextureIdentity,
                    Now + RetryDelay,
                    FailureCount,
                    PickerOnly
                };
            Result.RetryAfterMs =
                PickerOnly ? 750 : 250;
            return true;
        }

        ULONGLONG DeferredUntil = 0;
        bool StartedAsyncLoad = false;
        if (!FoundResidentTexture &&
            PendingSoftReference.Valid &&
            RequestAsyncIconLoad(
                PendingSoftReference,
                ItemToken,
                ItemIdentity,
                UTexture2D::StaticClass(),
                PickerOnly,
                &DeferredUntil,
                &StartedAsyncLoad) &&
            DeferredUntil > Now)
        {
            Result.DeferredUntil = DeferredUntil;
            Result.DeferredWorkPerformed = StartedAsyncLoad;
            Result.RetryAfterMs = DeferredUntil - Now;
            return true;
        }

        // Never synchronously load an optional icon package. If the validated
        // async path is unavailable, Fortnite may still stream it for its HUD;
        // a throttled retry will then decode the already-resident texture.
        Result.RetryAfterMs =
            FoundResidentTexture
                ? (PickerOnly ? 3000 : 750)
                : (PickerOnly ? 5000 : 1000);
        return true;
    }

    static bool TryBuildIcon(
        uintptr_t ItemToken,
        uint64_t ItemIdentity,
        bool PickerOnly,
        FIconPixels* Result)
    {
        __try
        {
            return BuildIconUnsafe(
                ItemToken,
                ItemIdentity,
                PickerOnly,
                *Result);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static void InsertIconRequestByPriorityLocked(
        const FIconRequest& Request)
    {
        if (Request.PickerOnly)
        {
            GIconRequests.push_back(Request);
            return;
        }
        const auto FirstPicker =
            std::find_if(
                GIconRequests.begin(),
                GIconRequests.end(),
                [](const FIconRequest& Pending)
                {
                    return Pending.PickerOnly;
                });
        GIconRequests.insert(
            FirstPicker, Request);
    }

    static bool TickIconRequests()
    {
        if (!GPendingDeferredIconRequests.empty())
        {
            FTrySharedStateLock Lock;
            if (!Lock.owns_lock())
                return false;
            while (!GPendingDeferredIconRequests.empty())
            {
                InsertIconRequestByPriorityLocked(
                    GPendingDeferredIconRequests.front());
                GPendingDeferredIconRequests.pop_front();
            }
        }
        {
            FTrySharedStateLock Lock;
            if (Lock.owns_lock())
            {
                while (!GPendingIconResults.empty() &&
                       GIconResults.size() <
                           kMaxIconResults)
                {
                    const uintptr_t Token =
                        GPendingIconResults.front()
                            .ItemToken;
                    GQueuedIcons.erase(Token);
                    GIconResults.push_back(
                        std::move(
                            GPendingIconResults.front()));
                    GPendingIconResults.pop_front();
                }
            }
        }

        const ULONGLONG Now = GetTickCount64();
        PruneAsyncIconLoads(Now);
        if (GPickerIconLeaseUntil.load(
                std::memory_order_acquire) < Now)
        {
            FTrySharedStateLock Lock;
            if (Lock.owns_lock())
            {
                bool PrunedPickerRequest = false;
                for (auto It = GIconRequests.begin();
                     It != GIconRequests.end();)
                {
                    if (!It->PickerOnly)
                    {
                        ++It;
                        continue;
                    }
                    GQueuedIcons.erase(It->ItemToken);
                    It = GIconRequests.erase(It);
                    PrunedPickerRequest = true;
                }
                if (PrunedPickerRequest)
                    GIconQueueEpoch.fetch_add(
                        1, std::memory_order_acq_rel);
            }
        }
        if (GInspectLeaseUntil.load(
                std::memory_order_acquire) < Now)
        {
            FTrySharedStateLock Lock;
            if (Lock.owns_lock() &&
                (!GIconRequests.empty() ||
                 !GIconResults.empty()))
            {
                GIconRequests.clear();
                GIconResults.clear();
                GQueuedIcons.clear();
                GPendingIconResults.clear();
                GPendingDeferredIconRequests.clear();
                GIconQueueEpoch.fetch_add(
                    1, std::memory_order_acq_rel);
            }
            return false;
        }
        if (!GPendingIconResults.empty())
            return false;

        FIconRequest Request;
        {
            FTrySharedStateLock Lock;
            if (!Lock.owns_lock() ||
                GIconRequests.empty() ||
                GIconResults.size() >= kMaxIconResults)
            {
                return false;
            }
            // Live slots retain priority, but delayed async polls must not
            // prevent a ready picker tile from using an otherwise idle tick.
            auto Next = std::find_if(
                GIconRequests.begin(),
                GIconRequests.end(),
                [Now](const FIconRequest& Pending)
                {
                    return !Pending.PickerOnly &&
                        Pending.NotBeforeAt <= Now;
                });
            if (Next == GIconRequests.end())
            {
                Next = std::find_if(
                    GIconRequests.begin(),
                    GIconRequests.end(),
                    [Now](const FIconRequest& Pending)
                    {
                        return Pending.PickerOnly &&
                            Pending.NotBeforeAt <= Now;
                    });
            }
            if (Next == GIconRequests.end())
                return false;
            const ULONGLONG RequestInterval =
                Next->PickerOnly
                    ? kPickerIconIntervalMs
                    : kLiveSlotIconIntervalMs;
            if (Now - GLastIconAt <
                    RequestInterval)
            {
                return false;
            }
            Request = *Next;
            GIconRequests.erase(Next);
        }
        const uintptr_t ItemToken =
            Request.ItemToken;

        GLastIconAt = Now;
        FIconPixels Result;
        auto Failure = GIconFailures.find(ItemToken);
        const bool HasActiveFailure =
            Failure != GIconFailures.end() &&
            Failure->second.ItemIdentity ==
                Request.ItemIdentity &&
            Failure->second.RetryAt > Now &&
            (Request.PickerOnly ||
             !Failure->second.PickerOnly);
        if (HasActiveFailure)
        {
            Result = {};
            Result.ItemToken = ItemToken;
            Result.ItemIdentity =
                Request.ItemIdentity;
            Result.PickerOnly =
                Failure->second.PickerOnly;
            Result.RetryAfterMs =
                Failure->second.RetryAt - Now;
        }
        else
        {
            const bool Completed =
                TryBuildIcon(
                    ItemToken,
                    Request.ItemIdentity,
                    Request.PickerOnly,
                    &Result);
            if (!Completed)
            {
                Result = {};
                Result.ItemToken = ItemToken;
                Result.ItemIdentity =
                    Request.ItemIdentity;
                Result.PickerOnly =
                    Request.PickerOnly;
                Result.RetryAfterMs = 30000;
            }
        }
        if (Result.DeferredUntil)
        {
            Request.NotBeforeAt = Result.DeferredUntil;
            GPendingDeferredIconRequests.push_back(Request);
            return Result.DeferredWorkPerformed;
        }
        if (!Result.Success)
        {
            const ULONGLONG RetryAfter =
                (std::clamp)(
                    Result.RetryAfterMs,
                    static_cast<ULONGLONG>(250),
                    static_cast<ULONGLONG>(30000));
            GIconFailures[ItemToken] = {
                Request.ItemIdentity,
                Now + RetryAfter,
                Result.PickerOnly
            };
        }
        else
        {
            GIconFailures.erase(ItemToken);
        }

        // Publish the completed pixels on the same tick when the renderer is
        // not holding the short shared-state lock. The old unconditional
        // staging path added a full server tick to every thumbnail, so five
        // otherwise-ready hotbar icons could still visibly staircase in.
        {
            FTrySharedStateLock Lock;
            if (Lock.owns_lock() &&
                GIconResults.size() < kMaxIconResults)
            {
                GQueuedIcons.erase(ItemToken);
                GIconResults.push_back(
                    std::move(Result));
                return true;
            }
        }
        GPendingIconResults.push_back(
            std::move(Result));
        return true;
    }

    static int32 SafeStackCount(
        const UFortItemDefinition* Definition)
    {
        int32 Count = Definition->GetMaxStackSize();
        if (Count <= 0 || Count > 999)
            Count = 1;
        return Count;
    }

    static void ResolveLoadedAmmo(
        const UFortItemDefinition* Definition,
        int32& LoadedAmmo,
        int32& PhantomReserveAmmo)
    {
        LoadedAmmo = 0;
        PhantomReserveAmmo = 0;

        const UFortWeaponItemDefinition* Weapon =
            Definition->Cast<UFortWeaponItemDefinition>();
        if (!Weapon)
        {
            if (auto Gadget =
                    Definition->Cast<UFortGadgetItemDefinition>())
            {
                if (Gadget->GetFunction(
                        "GetWeaponItemDefinition"))
                {
                    Weapon =
                        Gadget->GetWeaponItemDefinition();
                }
            }
        }
        if (!IsLiveObject(Weapon))
            return;

        auto Stats = AFortInventory::GetStats(Weapon);
        if (!Stats ||
            !SDK::MemReadable(
                Stats,
                sizeof(FFortRangedWeaponStats)))
        {
            return;
        }

        if (Stats->ClipSize >= 0 &&
            Stats->ClipSize <= 100000)
        {
            LoadedAmmo = Stats->ClipSize;
        }
        if (Weapon->HasbUsesPhantomReserveAmmo() &&
            Weapon->bUsesPhantomReserveAmmo &&
            Stats->InitialClips > 1 &&
            Stats->InitialClips < 1000)
        {
            PhantomReserveAmmo =
                (Stats->InitialClips - 1) *
                LoadedAmmo;
        }
    }

    static bool AssignNewItemToSlot(
        FResolvedLoadout& Loadout,
        const FGuid& NewGuid,
        const UFortItemDefinition* Definition,
        UFortWorldItem* ExpectedInstance,
        uint64_t ExpectedInstanceIdentity,
        const FResolvedSlot& Existing,
        int Slot,
        bool UsesGuidOnlyMutation,
        std::string& Error)
    {
        if ((!UsesGuidOnlyMutation &&
             !Loadout.CanMutateSlots) ||
            (UsesGuidOnlyMutation &&
             Loadout.HasLegacyQuickbar))
        {
            Error =
                "The quickbar source changed before the item could be assigned.";
            return false;
        }
        auto Inventory = Loadout.Inventory;
        auto PlayerController =
            Loadout.PlayerController;
        int32 EntrySize = 0;
        if (!HasSafeMutationInventory(
                Inventory, EntrySize))
        {
            Error = "Inventory changed while assigning the item.";
            return false;
        }

        auto Entry =
            FindMutableEntryByGuid(
                Inventory, EntrySize, NewGuid);
        if (!Entry)
        {
            Error = "The new item was not created.";
            return false;
        }
        auto Instance =
            FindItemInstanceByGuid(
                Inventory, NewGuid);
        if (!ExpectedInstanceIdentity ||
            Instance != ExpectedInstance ||
            GetLiveObjectIdentity(Instance) !=
                ExpectedInstanceIdentity ||
            Instance->GetItemEntry().ItemDefinition !=
                Definition)
        {
            Error =
                "The new item's inventory instance changed before assignment.";
            return false;
        }

        if (!UsesGuidOnlyMutation &&
            Loadout.HasOrderIndex)
        {
            Entry->OrderIndex =
                static_cast<int16>(Slot);
            if (!Instance->GetItemEntry()
                    .HasOrderIndex())
            {
                Error =
                    "The new item's quickbar order is unavailable.";
                return false;
            }
            Instance->GetItemEntry().OrderIndex =
                static_cast<int16>(Slot);
            Instance->GetItemEntry().bIsDirty = true;
        }

        if (!UsesGuidOnlyMutation &&
            Loadout.HasLegacyQuickbar)
        {
            auto QuickBars =
                PlayerController->GetQuickBars();
            if (!IsLiveObject(QuickBars) ||
                (QuickBars->HasOwner() &&
                 QuickBars->GetOwner() !=
                     PlayerController))
            {
                Error = "The player's quickbar disappeared.";
                return false;
            }
            QuickBars->ServerRemoveItemInternal(
                NewGuid, false, true);
            if (Existing.Occupied)
            {
                // Keep the old inventory row alive until the replacement is
                // known-good, but vacate its legacy quickbar cell so Remove()
                // cannot later EmptySlot() and erase the newly added GUID too.
                QuickBars->EmptySlot(
                    false, Slot + 1);
            }
            QuickBars->ServerAddItemInternal(
                NewGuid, false, Slot + 1);
        }
        else if (!UsesGuidOnlyMutation &&
                 Loadout.HasClientQuickbarPlacement)
        {
            uint8 PrimaryQuickbar = 0;
            PlayerController->AddItemToQuickBars(
                const_cast<UFortItemDefinition*>(
                    Definition),
                PrimaryQuickbar,
                Slot + 1);
            if (PlayerController->GetFunction(
                    "ClientForceUpdateQuickbar"))
            {
                PlayerController->ClientForceUpdateQuickbar(
                    PrimaryQuickbar);
            }
        }

        // ProcessEvent calls above may reallocate the replicated fast array.
        // Never dereference the pointer captured before those calls.
        if (!HasSafeMutationInventory(
                Inventory, EntrySize))
        {
            Error =
                "Inventory changed while verifying the quickbar slot.";
            return false;
        }
        Entry = FindMutableEntryByGuid(
            Inventory, EntrySize, NewGuid);
        if (!Entry ||
            Entry->ItemDefinition != Definition)
        {
            Error =
                "The staged item changed while assigning its quickbar slot.";
            return false;
        }
        Instance =
            FindItemInstanceByGuid(
                Inventory, NewGuid);
        if (Instance != ExpectedInstance ||
            GetLiveObjectIdentity(Instance) !=
                ExpectedInstanceIdentity ||
            Instance->GetItemEntry().ItemDefinition !=
                Definition)
        {
            Error =
                "The staged item instance changed while assigning its quickbar slot.";
            return false;
        }
        if (!UsesGuidOnlyMutation &&
            Loadout.HasOrderIndex &&
            (Entry->OrderIndex !=
                 static_cast<int16>(Slot) ||
             !Instance->GetItemEntry()
                  .HasOrderIndex() ||
             Instance->GetItemEntry().OrderIndex !=
                 static_cast<int16>(Slot)))
        {
            Error =
                "The game did not retain the requested quickbar order.";
            return false;
        }
        if (!UsesGuidOnlyMutation &&
            Loadout.HasLegacyQuickbar)
        {
            auto QuickBars = PlayerController->GetQuickBars();
            if (!QuickBars ||
                !QuickBars->HasPrimaryQuickBar())
            {
                Error =
                    "The player's legacy quickbar disappeared.";
                return false;
            }
            auto& Slots =
                QuickBars->GetPrimaryQuickBar().GetSlots();
            const int32 SlotSize = FQuickBarSlot::Size();
            if (!IsSafeArray(Slots, SlotSize, 64) ||
                Slot + 1 >= Slots.Num())
            {
                Error =
                    "The legacy quickbar could not be verified.";
                return false;
            }
            auto& Items =
                Slots.Get(Slot + 1, SlotSize).GetItems();
            if (!IsSafeArray(
                    Items, sizeof(FGuid), 16) ||
                Items.Num() != 1 ||
                !AreGuidsEqual(Items[0], NewGuid))
            {
                Error =
                    "The game did not accept the requested quickbar slot.";
                return false;
            }
        }
        else if (!SetModernLedgerSlot(
                     Loadout,
                     Slot,
                     &NewGuid))
        {
            Error =
                "The modern quickbar slot could not be tracked safely.";
            return false;
        }

        Inventory->UpdateEntry(*Entry);
        Inventory->ForceNetUpdate();
        PlayerController->ForceNetUpdate();
        return true;
    }

    static bool ArmActionTransaction(
        FResolvedLoadout& Loadout,
        const FActionRequest& Request,
        const FResolvedSlot& Existing,
        FActionTransaction& Transaction,
        std::string& Error)
    {
        int32 EntrySize = 0;
        if (!HasSafeMutationInventory(
                Loadout.Inventory, EntrySize))
        {
            Error =
                "Inventory changed before the item could be staged.";
            return false;
        }

        auto& Entries =
            Loadout.Inventory->GetInventory()
                .GetReplicatedEntries();
        Transaction = {};
        Transaction.TargetToken = Request.TargetToken;
        Transaction.InventoryToken =
            reinterpret_cast<uintptr_t>(
                Loadout.Inventory);
        Transaction.InventoryIdentity =
            GetLiveObjectIdentity(
                Loadout.Inventory);
        if (!Transaction.InventoryIdentity)
        {
            Transaction = {};
            Error =
                "The inventory identity could not be validated.";
            return false;
        }
        Transaction.Slot = Request.Slot;
        Transaction.Previous = Existing;
        Transaction.UsesGuidOnlyMutation =
            Loadout.UsesGuidOnlyMutation;
        for (int Slot = 0; Slot < kSlotCount; ++Slot)
        {
            if (Loadout.Slots[Slot].Occupied)
            {
                Transaction.BaselineSlots[Slot] =
                    ToGuidValue(
                        Loadout.Slots[Slot].Guid);
            }
        }
        Transaction.VerifyPreviousOrderIndex =
            Loadout.UsesOrderIndexSlots &&
            !Transaction.UsesGuidOnlyMutation;
        if (!CaptureModernLedger(
                Loadout,
                Transaction))
        {
            Transaction = {};
            Error =
                "The current quickbar mapping could not be snapshotted safely.";
            return false;
        }
        Transaction.BaselineGuids.reserve(
            static_cast<size_t>(Entries.Num()));
        for (int32 Index = 0;
             Index < Entries.Num();
             ++Index)
        {
            const FGuid Guid =
                Entries.Get(Index, EntrySize)
                    .ItemGuid;
            const FGuidValue GuidValue =
                ToGuidValue(Guid);
            if (GuidValue.IsZero())
            {
                Transaction = {};
                Error =
                    "Inventory contains an invalid zero item GUID.";
                return false;
            }
            for (const auto& ExistingGuid :
                 Transaction.BaselineGuids)
            {
                if (AreGuidsEqual(
                        Guid, ExistingGuid))
                {
                    Transaction = {};
                    Error =
                        "Inventory contains duplicate item GUIDs.";
                    return false;
                }
            }
            Transaction.BaselineGuids.push_back(
                GuidValue);
        }
        Transaction.Phase =
            EActionTransactionPhase::Granting;
        Transaction.HasStagedItem = true;
        return true;
    }

    static bool UntouchedSlotsMatch(
        const FResolvedLoadout& Loadout,
        const FActionTransaction& Transaction)
    {
        if (Transaction.Slot < 0 ||
            Transaction.Slot >= kSlotCount)
        {
            return false;
        }
        for (int Slot = 0; Slot < kSlotCount; ++Slot)
        {
            if (Slot == Transaction.Slot)
                continue;
            const auto& Expected =
                Transaction.BaselineSlots[Slot];
            const auto& Current = Loadout.Slots[Slot];
            if (Expected.IsZero())
            {
                if (Current.Occupied)
                    return false;
                continue;
            }
            if (!Current.Occupied ||
                !AreGuidsEqual(Current.Guid, Expected))
            {
                return false;
            }
        }
        return true;
    }

    static bool ResolveTransactionInventory(
        const FActionTransaction& Transaction,
        AFortPlayerControllerAthena*& PlayerController,
        AFortInventory*& Inventory,
        int32& EntrySize)
    {
        PlayerController =
            ResolveRequestedController(
                Transaction.TargetToken);
        Inventory =
            ResolveWorldInventory(PlayerController);
        return PlayerController &&
            Inventory &&
            reinterpret_cast<uintptr_t>(Inventory) ==
                Transaction.InventoryToken &&
            GetLiveObjectIdentity(Inventory) ==
                Transaction.InventoryIdentity &&
            HasSafeMutationInventory(
                Inventory, EntrySize);
    }

    static bool GuidOnlyLedgerMatches(
        const FActionTransaction& Transaction,
        AFortPlayerControllerAthena* PlayerController,
        AFortInventory* Inventory,
        const FGuidValue& TargetGuid)
    {
        if (!Transaction.UsesGuidOnlyMutation ||
            !Transaction.HasModernLedgerSnapshot ||
            Transaction.Slot < 0 ||
            Transaction.Slot >= kSlotCount ||
            GetLiveObjectIdentity(PlayerController) !=
                Transaction.ModernLedgerControllerIdentity ||
            GetLiveObjectIdentity(Inventory) !=
                Transaction.ModernLedgerInventoryIdentity)
        {
            return false;
        }

        const auto Existing =
            GModernSlotLedgers.find(
                Transaction
                    .ModernLedgerControllerIdentity);
        if (Existing == GModernSlotLedgers.end() ||
            Existing->second.InventoryIdentity !=
                Transaction
                    .ModernLedgerInventoryIdentity)
        {
            return false;
        }

        for (int Slot = 0; Slot < kSlotCount; ++Slot)
        {
            const auto& Expected =
                Slot == Transaction.Slot
                    ? TargetGuid
                    : Transaction.ModernLedgerSlots[Slot];
            if (!AreGuidValuesEqual(
                    Existing->second.Slots[Slot],
                    Expected))
            {
                return false;
            }
        }
        return true;
    }

    static bool InventoryGuidSetMatches(
        AFortInventory* Inventory,
        int32 EntrySize,
        const FActionTransaction& Transaction,
        const FGuid* AddedGuid,
        bool RemovePrevious)
    {
        if (!Inventory ||
            EntrySize < 0x20 ||
            EntrySize > 0x2000)
        {
            return false;
        }

        bool PreviousWasInBaseline =
            !RemovePrevious ||
            !Transaction.Previous.Occupied;
        for (const auto& BaselineGuid :
             Transaction.BaselineGuids)
        {
            if (RemovePrevious &&
                Transaction.Previous.Occupied &&
                AreGuidsEqual(
                    Transaction.Previous.Guid,
                    BaselineGuid))
            {
                PreviousWasInBaseline = true;
                break;
            }
        }
        if (!PreviousWasInBaseline)
            return false;

        int ExpectedCount = static_cast<int>(
            Transaction.BaselineGuids.size());
        if (RemovePrevious &&
            Transaction.Previous.Occupied)
        {
            --ExpectedCount;
        }
        if (AddedGuid)
            ++ExpectedCount;

        auto& Entries =
            Inventory->GetInventory()
                .GetReplicatedEntries();
        if (ExpectedCount < 0 ||
            Entries.Num() != ExpectedCount)
        {
            return false;
        }

        for (int32 Index = 0;
             Index < Entries.Num();
             ++Index)
        {
            const FGuid Guid =
                Entries.Get(Index, EntrySize)
                    .ItemGuid;
            if (ToGuidValue(Guid).IsZero())
                return false;

            for (int32 PreviousIndex = 0;
                 PreviousIndex < Index;
                 ++PreviousIndex)
            {
                if (AreGuidsEqual(
                        Guid,
                        Entries.Get(
                            PreviousIndex,
                            EntrySize).ItemGuid))
                {
                    return false;
                }
            }

            bool Expected =
                AddedGuid &&
                AreGuidsEqual(Guid, *AddedGuid);
            if (!Expected)
            {
                for (const auto& BaselineGuid :
                     Transaction.BaselineGuids)
                {
                    if (!AreGuidsEqual(
                            Guid, BaselineGuid))
                    {
                        continue;
                    }
                    Expected =
                        !(RemovePrevious &&
                          Transaction.Previous.Occupied &&
                          AreGuidsEqual(
                              Guid,
                              Transaction.Previous.Guid));
                    break;
                }
            }
            if (!Expected)
                return false;
        }
        return true;
    }

    static bool VerifyGuidOnlyReplacement(
        const FGuid& NewGuid,
        const UFortItemDefinition* GrantedDefinition,
        const FActionTransaction& Transaction)
    {
        AFortPlayerControllerAthena* PlayerController =
            nullptr;
        AFortInventory* Inventory = nullptr;
        int32 EntrySize = 0;
        if (!Transaction.NewInstanceIdentity ||
            !ResolveTransactionInventory(
                Transaction,
                PlayerController,
                Inventory,
                EntrySize) ||
            !InventoryGuidSetMatches(
                Inventory,
                EntrySize,
                Transaction,
                &NewGuid,
                Transaction.Previous.Occupied) ||
            !GuidOnlyLedgerMatches(
                Transaction,
                PlayerController,
                Inventory,
                ToGuidValue(NewGuid)))
        {
            return false;
        }

        const auto NewEntry =
            FindEntryByGuid(
                Inventory, EntrySize, NewGuid);
        const auto NewInstance =
            FindItemInstanceByGuid(
                Inventory, NewGuid);
        if (!NewEntry ||
            NewEntry->ItemDefinition !=
                GrantedDefinition ||
            !NewInstance ||
            !Transaction.NewInstanceIdentity ||
            GetLiveObjectIdentity(NewInstance) !=
                Transaction.NewInstanceIdentity ||
            NewInstance->GetItemEntry()
                    .ItemDefinition !=
                GrantedDefinition)
        {
            return false;
        }

        return !Transaction.Previous.Occupied ||
            (!FindEntryByGuid(
                 Inventory,
                 EntrySize,
                 Transaction.Previous.Guid) &&
             !FindItemInstanceByGuid(
                 Inventory,
                 Transaction.Previous.Guid));
    }

    static bool VerifyGuidOnlyClear(
        const FActionTransaction& Transaction)
    {
        AFortPlayerControllerAthena* PlayerController =
            nullptr;
        AFortInventory* Inventory = nullptr;
        int32 EntrySize = 0;
        const FGuidValue EmptyGuid{};
        return ResolveTransactionInventory(
                   Transaction,
                   PlayerController,
                   Inventory,
                   EntrySize) &&
            InventoryGuidSetMatches(
                Inventory,
                EntrySize,
                Transaction,
                nullptr,
                true) &&
            !FindEntryByGuid(
                Inventory,
                EntrySize,
                Transaction.Previous.Guid) &&
            !FindItemInstanceByGuid(
                Inventory,
                Transaction.Previous.Guid) &&
            GuidOnlyLedgerMatches(
                Transaction,
                PlayerController,
                Inventory,
                EmptyGuid);
    }

    static bool VerifyGuidOnlyStagedState(
        const FGuid& NewGuid,
        const UFortItemDefinition* GrantedDefinition,
        UFortWorldItem* ExpectedInstance,
        uint64_t ExpectedInstanceIdentity,
        const FActionTransaction& Transaction)
    {
        AFortPlayerControllerAthena* PlayerController =
            nullptr;
        AFortInventory* Inventory = nullptr;
        int32 EntrySize = 0;
        if (!ExpectedInstanceIdentity ||
            ExpectedInstanceIdentity !=
                Transaction.NewInstanceIdentity ||
            !ResolveTransactionInventory(
                Transaction,
                PlayerController,
                Inventory,
                EntrySize) ||
            !InventoryGuidSetMatches(
                Inventory,
                EntrySize,
                Transaction,
                &NewGuid,
                false) ||
            !GuidOnlyLedgerMatches(
                Transaction,
                PlayerController,
                Inventory,
                Transaction.ModernLedgerSlots[
                    Transaction.Slot]))
        {
            return false;
        }

        const auto NewEntry =
            FindEntryByGuid(
                Inventory, EntrySize, NewGuid);
        const auto NewInstance =
            FindItemInstanceByGuid(
                Inventory, NewGuid);
        if (!NewEntry ||
            NewEntry->ItemDefinition !=
                GrantedDefinition ||
            !NewInstance ||
            NewInstance != ExpectedInstance ||
            GetLiveObjectIdentity(NewInstance) !=
                ExpectedInstanceIdentity ||
            NewInstance->GetItemEntry()
                    .ItemDefinition !=
                GrantedDefinition)
        {
            return false;
        }

        if (!Transaction.Previous.Occupied)
            return true;
        const auto PreviousEntry =
            FindEntryByGuid(
                Inventory,
                EntrySize,
                Transaction.Previous.Guid);
        const auto PreviousInstance =
            FindItemInstanceByGuid(
                Inventory,
                Transaction.Previous.Guid);
        return PreviousEntry &&
            PreviousEntry->ItemDefinition ==
                Transaction.Previous.Definition &&
            PreviousInstance &&
            PreviousInstance->GetItemEntry()
                    .ItemDefinition ==
                Transaction.Previous.Definition;
    }

    static bool VerifyCommittedReplacement(
        const FActionRequest& Request,
        const FGuid& NewGuid,
        const UFortItemDefinition* GrantedDefinition,
        const FActionTransaction& Transaction)
    {
        if (Transaction.UsesGuidOnlyMutation)
        {
            return VerifyGuidOnlyReplacement(
                NewGuid,
                GrantedDefinition,
                Transaction);
        }

        FResolvedLoadout Verified;
        std::string Error;
        if (!ResolveLoadoutUnsafe(
                Request.TargetToken,
                Verified,
                Error,
                nullptr,
                ESlotResolvePolicy::
                    PostMutationVerification) ||
            !Verified.HasAuthoritativeSlots ||
            !Verified.CanMutateSlots)
        {
            return false;
        }
        int32 EntrySize = 0;
        if (!HasSafeMutationInventory(
                Verified.Inventory,
                EntrySize))
        {
            return false;
        }

        auto NewEntry =
            FindEntryByGuid(
                Verified.Inventory,
                EntrySize,
                NewGuid);
        auto NewInstance =
            FindItemInstanceByGuid(
                Verified.Inventory,
                NewGuid);
        if (!NewEntry ||
            NewEntry->ItemDefinition !=
                GrantedDefinition ||
            !NewInstance ||
            NewInstance->GetItemEntry()
                    .ItemDefinition !=
                GrantedDefinition)
        {
            return false;
        }
        if (Transaction.Previous.Occupied &&
            (FindEntryByGuid(
                 Verified.Inventory,
                 EntrySize,
                 Transaction.Previous.Guid) ||
             FindItemInstanceByGuid(
                 Verified.Inventory,
                 Transaction.Previous.Guid)))
        {
            return false;
        }

        if (!UntouchedSlotsMatch(
                Verified, Transaction))
        {
            return false;
        }

        const auto& VerifiedSlot =
            Verified.Slots[Request.Slot];
        return VerifiedSlot.Occupied &&
            AreGuidsEqual(
                VerifiedSlot.Guid, NewGuid) &&
            VerifiedSlot.Definition ==
                GrantedDefinition;
    }

    static bool TryAssignNewItemToSlot(
        FResolvedLoadout* Loadout,
        const FGuid* NewGuid,
        const UFortItemDefinition* Definition,
        UFortWorldItem* ExpectedInstance,
        uint64_t ExpectedInstanceIdentity,
        const FResolvedSlot* Existing,
        int Slot,
        bool UsesGuidOnlyMutation,
        std::string* Error,
        bool* Assigned)
    {
        __try
        {
            *Assigned = AssignNewItemToSlot(
                *Loadout,
                *NewGuid,
                Definition,
                ExpectedInstance,
                ExpectedInstanceIdentity,
                *Existing,
                Slot,
                UsesGuidOnlyMutation,
                *Error);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *Assigned = false;
            return false;
        }
    }

    static bool IsTransactionBaselineGuid(
        const FActionTransaction& Transaction,
        const FGuid& Guid)
    {
        for (const auto& Existing :
             Transaction.BaselineGuids)
        {
            if (AreGuidsEqual(Guid, Existing))
                return true;
        }
        return false;
    }

    static bool RollbackStagedItemUnsafe(
        const FActionTransaction& Transaction)
    {
        auto PlayerController =
            ResolveRequestedController(
                Transaction.TargetToken);
        auto Inventory =
            ResolveWorldInventory(PlayerController);
        int32 EntrySize = 0;
        if (!PlayerController || !Inventory ||
            reinterpret_cast<uintptr_t>(
                Inventory) !=
                Transaction.InventoryToken ||
            GetLiveObjectIdentity(Inventory) !=
                Transaction.InventoryIdentity ||
            !HasSafeMutationInventory(
                Inventory, EntrySize))
        {
            return false;
        }

        if (Transaction.Phase ==
            EActionTransactionPhase::ClearingOld)
        {
            if (!Transaction.Previous.Occupied ||
                Transaction.Slot < 0 ||
                Transaction.Slot >= kSlotCount)
            {
                return false;
            }
            if (Transaction.UsesGuidOnlyMutation)
            {
                auto PreviousEntry =
                    FindEntryByGuid(
                        Inventory,
                        EntrySize,
                        Transaction.Previous.Guid);
                auto PreviousInstance =
                    FindItemInstanceByGuid(
                        Inventory,
                        Transaction.Previous.Guid);
                if (!!PreviousEntry != !!PreviousInstance)
                    return false;

                if (!RestoreModernLedger(
                        Transaction,
                        PlayerController,
                        Inventory))
                {
                    return false;
                }

                if (PreviousEntry)
                {
                    if (PreviousEntry->ItemDefinition !=
                            Transaction.Previous.Definition ||
                        PreviousInstance->GetItemEntry()
                                .ItemDefinition !=
                            Transaction.Previous.Definition ||
                        !InventoryGuidSetMatches(
                            Inventory,
                            EntrySize,
                            Transaction,
                            nullptr,
                            false) ||
                        !GuidOnlyLedgerMatches(
                            Transaction,
                            PlayerController,
                            Inventory,
                            Transaction.ModernLedgerSlots[
                                Transaction.Slot]))
                    {
                        return false;
                    }
                }
                else
                {
                    // Remove() completed before a later verification fault.
                    // Preserve that clear instead of restoring a dangling GUID.
                    auto ExistingLedger =
                        GModernSlotLedgers.find(
                            Transaction
                                .ModernLedgerControllerIdentity);
                    if (ExistingLedger ==
                        GModernSlotLedgers.end())
                    {
                        return false;
                    }
                    ExistingLedger->second.Slots[
                        Transaction.Slot] = {};
                    const FGuidValue EmptyGuid{};
                    if (!InventoryGuidSetMatches(
                            Inventory,
                            EntrySize,
                            Transaction,
                            nullptr,
                            true) ||
                        !GuidOnlyLedgerMatches(
                            Transaction,
                            PlayerController,
                            Inventory,
                            EmptyGuid))
                    {
                        return false;
                    }
                }
                Inventory->ForceNetUpdate();
                PlayerController->ForceNetUpdate();
                return true;
            }
            if (!RestoreModernLedger(
                    Transaction,
                    PlayerController,
                    Inventory))
            {
                return false;
            }

            FResolvedLoadout Verified;
            std::string ResolveError;
            if (!ResolveLoadoutUnsafe(
                    Transaction.TargetToken,
                    Verified,
                    ResolveError,
                    nullptr,
                    ESlotResolvePolicy::
                        PostMutationVerification) ||
                !Verified.CanMutateSlots ||
                reinterpret_cast<uintptr_t>(
                    Verified.Inventory) !=
                    Transaction.InventoryToken ||
                GetLiveObjectIdentity(
                    Verified.Inventory) !=
                    Transaction.InventoryIdentity)
            {
                return false;
            }

            auto PreviousEntry =
                FindEntryByGuid(
                    Inventory,
                    EntrySize,
                    Transaction.Previous.Guid);
            auto PreviousInstance =
                FindItemInstanceByGuid(
                    Inventory,
                    Transaction.Previous.Guid);
            if (!!PreviousEntry != !!PreviousInstance)
                return false;

            const auto& VerifiedSlot =
                Verified.Slots[Transaction.Slot];
            if (PreviousEntry)
            {
                return VerifiedSlot.Occupied &&
                    AreGuidsEqual(
                        VerifiedSlot.Guid,
                        Transaction.Previous.Guid) &&
                    VerifiedSlot.Definition ==
                        Transaction.Previous.Definition;
            }
            return !VerifiedSlot.Occupied;
        }

        std::vector<FGuid> AddedGuids;
        auto& Entries =
            Inventory->GetInventory()
                .GetReplicatedEntries();
        AddedGuids.reserve(
            static_cast<size_t>(Entries.Num()));
        for (int32 Index = 0;
             Index < Entries.Num();
             ++Index)
        {
            const FGuid Guid =
                Entries.Get(Index, EntrySize)
                    .ItemGuid;
            if (!IsTransactionBaselineGuid(
                    Transaction, Guid))
            {
                if (Transaction.UsesGuidOnlyMutation &&
                    (ToGuidValue(
                         Transaction.NewGuid).IsZero() ||
                     !AreGuidsEqual(
                         Guid,
                         Transaction.NewGuid)))
                {
                    // Never reinterpret an unrelated concurrent pickup as the
                    // item staged by this GUID-bound transaction.
                    return false;
                }
                const FGuidValue GuidValue =
                    ToGuidValue(Guid);
                if (GuidValue.IsZero())
                    return false;
                for (const auto& AddedGuid :
                     AddedGuids)
                {
                    if (AreGuidsEqual(
                            AddedGuid, Guid))
                    {
                        return false;
                    }
                }
                AddedGuids.push_back(Guid);
                if (AddedGuids.size() > 1)
                {
                    // Safe grant candidates create exactly one row. Never turn
                    // a corrupt diff into an unbounded game-thread removal loop.
                    return false;
                }
            }
        }

        auto PreviousEntry =
            Transaction.Previous.Occupied
                ? FindEntryByGuid(
                    Inventory,
                    EntrySize,
                    Transaction.Previous.Guid)
                : nullptr;
        const UFortItemDefinition* PreviousDefinition =
            PreviousEntry
                ? PreviousEntry->ItemDefinition
                : nullptr;
        auto PreviousInstance =
            Transaction.Previous.Occupied
                ? FindItemInstanceByGuid(
                    Inventory,
                    Transaction.Previous.Guid)
                : nullptr;
        for (const auto& AddedGuid : AddedGuids)
        {
            if (!FindItemInstanceByGuid(
                    Inventory, AddedGuid))
            {
                return false;
            }
        }
        if (Transaction.Previous.Occupied &&
            (!PreviousEntry ||
             !PreviousInstance))
        {
            // RemovingOld is handled below: the old row/instance may already
            // be gone, in which case preserving the replacement is safer.
            if (Transaction.Phase !=
                EActionTransactionPhase::RemovingOld)
            {
                return false;
            }
        }

        // If removal of the original GUID faulted after actually deleting it,
        // preserve the already-assigned replacement. Deleting both is the only
        // unrecoverable outcome and is never an acceptable rollback policy.
        if (Transaction.Phase ==
                EActionTransactionPhase::RemovingOld &&
            Transaction.Previous.Occupied &&
            (!PreviousEntry || !PreviousInstance))
        {
            const auto ReplacementEntry =
                FindEntryByGuid(
                    Inventory,
                    EntrySize,
                    Transaction.NewGuid);
            const auto ReplacementInstance =
                FindItemInstanceByGuid(
                    Inventory,
                    Transaction.NewGuid);
            if (!ReplacementEntry ||
                !ReplacementInstance ||
                ClassifyExistingItemDefinition(
                    ReplacementEntry->ItemDefinition) !=
                    EExistingItemPlacement::Primary ||
                ReplacementInstance->GetItemEntry()
                        .ItemDefinition !=
                    ReplacementEntry->ItemDefinition ||
                FindEntryByGuid(
                    Inventory,
                    EntrySize,
                    Transaction.Previous.Guid) ||
                FindItemInstanceByGuid(
                    Inventory,
                    Transaction.Previous.Guid))
            {
                return false;
            }
            if (Transaction.UsesGuidOnlyMutation)
            {
                if (!VerifyGuidOnlyReplacement(
                        Transaction.NewGuid,
                        ReplacementEntry->ItemDefinition,
                        Transaction))
                {
                    return false;
                }
                Inventory->ForceNetUpdate();
                PlayerController->ForceNetUpdate();
                return true;
            }
            if (VersionInfo.FortniteVersion >= 7.40f &&
                FFortItemEntry::HasOrderIndex() &&
                ReplacementEntry->OrderIndex !=
                    static_cast<int16>(
                        Transaction.Slot))
            {
                return false;
            }
            if (VersionInfo.FortniteVersion < 7.40f)
            {
                auto QuickBars =
                    PlayerController->GetQuickBars();
                if (!IsLiveObject(QuickBars) ||
                    !QuickBars->HasPrimaryQuickBar())
                {
                    return false;
                }
                auto& Slots =
                    QuickBars->GetPrimaryQuickBar()
                        .GetSlots();
                const int32 SlotSize =
                    FQuickBarSlot::Size();
                if (!IsSafeArray(
                        Slots, SlotSize, 64) ||
                    Transaction.Slot + 1 >=
                        Slots.Num())
                {
                    return false;
                }
                auto& Items =
                    Slots.Get(
                        Transaction.Slot + 1,
                        SlotSize).GetItems();
                if (!IsSafeArray(
                        Items, sizeof(FGuid), 16) ||
                    Items.Num() != 1 ||
                    !AreGuidsEqual(
                        Items[0],
                        Transaction.NewGuid))
                {
                    return false;
                }
            }
            Inventory->ForceNetUpdate();
            PlayerController->ForceNetUpdate();
            return true;
        }

        if (VersionInfo.FortniteVersion < 7.40f &&
            PlayerController->HasQuickBars())
        {
            auto QuickBars =
                PlayerController->GetQuickBars();
            if (!IsLiveObject(QuickBars) ||
                (QuickBars->HasOwner() &&
                 QuickBars->GetOwner() !=
                     PlayerController) ||
                !QuickBars->GetFunction("EmptySlot") ||
                !QuickBars->GetFunction(
                    "ServerRemoveItemInternal") ||
                !QuickBars->GetFunction(
                    "ServerAddItemInternal"))
            {
                return false;
            }
            for (const auto& AddedGuid : AddedGuids)
            {
                QuickBars->ServerRemoveItemInternal(
                    AddedGuid, false, true);
            }
            QuickBars->EmptySlot(
                false, Transaction.Slot + 1);
            if (PreviousEntry &&
                ClassifyExistingItemDefinition(
                    PreviousDefinition) ==
                    EExistingItemPlacement::Primary)
            {
                QuickBars->ServerAddItemInternal(
                    Transaction.Previous.Guid,
                    false,
                    Transaction.Slot + 1);
            }
        }

        for (const auto& AddedGuid : AddedGuids)
            Inventory->Remove(AddedGuid);

        if (!Transaction.UsesGuidOnlyMutation &&
            VersionInfo.FortniteVersion >= 7.40f &&
            (Transaction.Phase ==
                 EActionTransactionPhase::Assigned ||
             Transaction.Phase ==
                 EActionTransactionPhase::RemovingOld) &&
            PreviousEntry &&
            ClassifyExistingItemDefinition(
                PreviousDefinition) ==
                EExistingItemPlacement::Primary &&
            PlayerController->GetFunction(
                "AddItemToQuickBars"))
        {
            uint8 PrimaryQuickbar = 0;
            PlayerController->AddItemToQuickBars(
                const_cast<UFortItemDefinition*>(
                    PreviousDefinition),
                PrimaryQuickbar,
                Transaction.Slot + 1);
            if (PlayerController->GetFunction(
                    "ClientForceUpdateQuickbar"))
            {
                PlayerController->ClientForceUpdateQuickbar(
                    PrimaryQuickbar);
            }
        }

        if (!RestoreModernLedger(
                Transaction,
                PlayerController,
                Inventory))
        {
            return false;
        }
        Inventory->ForceNetUpdate();
        PlayerController->ForceNetUpdate();

        if (!HasSafeMutationInventory(
                Inventory, EntrySize))
        {
            return false;
        }
        for (const auto& AddedGuid : AddedGuids)
        {
            if (FindEntryByGuid(
                    Inventory, EntrySize, AddedGuid) ||
                FindItemInstanceByGuid(
                    Inventory, AddedGuid))
            {
                return false;
            }
        }
        PreviousEntry =
            Transaction.Previous.Occupied
                ? FindEntryByGuid(
                    Inventory,
                    EntrySize,
                    Transaction.Previous.Guid)
                : nullptr;
        if (Transaction.Previous.Occupied &&
            (!PreviousEntry ||
             !FindItemInstanceByGuid(
                 Inventory,
                 Transaction.Previous.Guid)))
        {
            return false;
        }
        if (Transaction.VerifyPreviousOrderIndex &&
            VersionInfo.FortniteVersion >= 7.40f &&
            Transaction.Previous.Occupied &&
            FFortItemEntry::HasOrderIndex() &&
            PreviousEntry->OrderIndex !=
                static_cast<int16>(
                    Transaction.Slot))
        {
            return false;
        }

        if (Transaction.UsesGuidOnlyMutation &&
            (!InventoryGuidSetMatches(
                 Inventory,
                 EntrySize,
                 Transaction,
                 nullptr,
                 false) ||
             !GuidOnlyLedgerMatches(
                 Transaction,
                 PlayerController,
                 Inventory,
                 Transaction.ModernLedgerSlots[
                     Transaction.Slot])))
        {
            return false;
        }

        if (VersionInfo.FortniteVersion < 7.40f)
        {
            auto QuickBars =
                PlayerController->GetQuickBars();
            auto& Slots =
                QuickBars->GetPrimaryQuickBar()
                    .GetSlots();
            const int32 SlotSize = FQuickBarSlot::Size();
            if (!IsSafeArray(Slots, SlotSize, 64) ||
                Transaction.Slot + 1 >= Slots.Num())
            {
                return false;
            }
            auto& Items =
                Slots.Get(
                    Transaction.Slot + 1,
                    SlotSize).GetItems();
            if (!IsSafeArray(
                    Items, sizeof(FGuid), 16))
            {
                return false;
            }
            if (Transaction.Previous.Occupied)
            {
                if (Items.Num() != 1 ||
                    !AreGuidsEqual(
                        Items[0],
                        Transaction.Previous.Guid))
                {
                    return false;
                }
            }
            else if (Items.Num() != 0)
            {
                return false;
            }
        }
        return true;
    }

    static bool TryRollbackStagedItem(
        const FActionTransaction* Transaction)
    {
        __try
        {
            return RollbackStagedItemUnsafe(
                *Transaction);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool RecoverTransaction(
        FActionTransaction& Transaction)
    {
        const bool Verified =
            TryRollbackStagedItem(&Transaction);
        Transaction.HasStagedItem = false;
        if (!Verified)
            Transaction.RecoveryFailed = true;
        return Verified;
    }

    enum class EReportedMutationBeginResult : uint8
    {
        NotReported,
        Immediate,
        Pending
    };

    static bool TryReadStrictReportedRawSlotsUnsafe(
        AFortPlayerControllerAthena* PlayerController,
        AFortInventory* Inventory,
        int RawBase,
        int EnabledSlots,
        std::array<FGuidValue, kSlotCount>& Slots)
    {
        Slots = {};
        if (RawBase < 0 ||
            RawBase + kSlotCount >
                kReportedQuickbarCapacity ||
            EnabledSlots < kSlotCount ||
            EnabledSlots > kReportedQuickbarCapacity)
        {
            return false;
        }

        FReportedQuickbarGuidState State{};
        bool StrictSchema = false;
        int32 EntrySize = 0;
        if (!TryReadReportedQuickbarStateUnsafe(
                PlayerController,
                State,
                StrictSchema) ||
            !StrictSchema ||
            State.NumEnabledSlots != EnabledSlots ||
            !HasSafeMutationInventory(
                Inventory, EntrySize))
        {
            return false;
        }

        for (int Slot = 0; Slot < kSlotCount; ++Slot)
        {
            const auto& Guid =
                State.EquippedItemGuids[
                    RawBase + Slot];
            const auto GuidValue = ToGuidValue(Guid);
            if (GuidValue.IsZero())
                continue;
            auto Entry = FindEntryByGuid(
                Inventory, EntrySize, Guid);
            auto Instance = FindItemInstanceByGuid(
                Inventory, Guid);
            if (!Entry || !Instance ||
                Entry->ItemDefinition !=
                    Instance->GetItemEntry()
                        .ItemDefinition ||
                ClassifyExistingItemDefinition(
                    Entry->ItemDefinition) !=
                    EExistingItemPlacement::Primary)
            {
                return false;
            }
            for (int Previous = 0;
                 Previous < Slot;
                 ++Previous)
            {
                if (!Slots[Previous].IsZero() &&
                    AreGuidsEqual(
                        Guid,
                        Slots[Previous]))
                {
                    return false;
                }
            }
            Slots[Slot] = GuidValue;
        }
        return true;
    }

    static bool TryReadStrictReportedRawSlots(
        AFortPlayerControllerAthena* PlayerController,
        AFortInventory* Inventory,
        int RawBase,
        int EnabledSlots,
        std::array<FGuidValue, kSlotCount>* Slots)
    {
        __try
        {
            return TryReadStrictReportedRawSlotsUnsafe(
                PlayerController,
                Inventory,
                RawBase,
                EnabledSlots,
                *Slots);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *Slots = {};
            return false;
        }
    }

    static bool TryReadStrictBridgeRawSlotsUnsafe(
        AFortPlayerControllerAthena* PlayerController,
        AFortInventory* Inventory,
        uint64_t Session,
        uint64_t BaselineGeneration,
        uint64_t MaximumAgeMilliseconds,
        std::array<FGuidValue, kSlotCount>& Slots,
        uint64_t& Generation)
    {
        Slots = {};
        Generation = 0;
        PlayerLoadoutBridgeServer::FSnapshot Snapshot{};
        int32 EntrySize = 0;
        if (!Session ||
            !PlayerLoadoutBridgeServer::TryGetLatestSnapshot(
                PlayerController,
                Snapshot,
                MaximumAgeMilliseconds) ||
            Snapshot.Session != Session ||
            Snapshot.Generation < BaselineGeneration ||
            !HasSafeMutationInventory(Inventory, EntrySize))
        {
            return false;
        }

        for (int Slot = 0; Slot < kSlotCount; ++Slot)
        {
            const auto& WireGuid = Snapshot.Slots[Slot];
            const FGuid Guid{
                static_cast<int32>(WireGuid.A),
                static_cast<int32>(WireGuid.B),
                static_cast<int32>(WireGuid.C),
                static_cast<int32>(WireGuid.D)
            };
            const FGuidValue GuidValue = ToGuidValue(Guid);
            if (GuidValue.IsZero())
                continue;

            auto Entry = FindEntryByGuid(Inventory, EntrySize, Guid);
            auto Instance = FindItemInstanceByGuid(Inventory, Guid);
            if (!Entry || !Instance ||
                Entry->ItemDefinition !=
                    Instance->GetItemEntry().ItemDefinition ||
                ClassifyExistingItemDefinition(Entry->ItemDefinition) !=
                    EExistingItemPlacement::Primary)
            {
                return false;
            }
            for (int Previous = 0; Previous < Slot; ++Previous)
            {
                if (!Slots[Previous].IsZero() &&
                    AreGuidValuesEqual(Slots[Previous], GuidValue))
                {
                    return false;
                }
            }
            Slots[Slot] = GuidValue;
        }
        Generation = Snapshot.Generation;
        return true;
    }

    static bool TryReadStrictBridgeRawSlots(
        AFortPlayerControllerAthena* PlayerController,
        AFortInventory* Inventory,
        uint64_t Session,
        uint64_t BaselineGeneration,
        uint64_t MaximumAgeMilliseconds,
        std::array<FGuidValue, kSlotCount>* Slots,
        uint64_t* Generation)
    {
        __try
        {
            return TryReadStrictBridgeRawSlotsUnsafe(
                PlayerController,
                Inventory,
                Session,
                BaselineGeneration,
                MaximumAgeMilliseconds,
                *Slots,
                *Generation);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *Slots = {};
            *Generation = 0;
            return false;
        }
    }

    static bool RawUntouchedSlotsMatch(
        const std::array<FGuidValue, kSlotCount>& Current,
        const FActionTransaction& Transaction)
    {
        if (Transaction.Slot < 0 ||
            Transaction.Slot >= kSlotCount)
        {
            return false;
        }
        for (int Slot = 0; Slot < kSlotCount; ++Slot)
        {
            if (Slot == Transaction.Slot)
                continue;
            if (!AreGuidValuesEqual(
                    Current[Slot],
                    Transaction.BaselineSlots[Slot]))
            {
                return false;
            }
        }
        return true;
    }

    static bool RawSlotMapsEqual(
        const std::array<FGuidValue, kSlotCount>& Left,
        const std::array<FGuidValue, kSlotCount>& Right)
    {
        for (int Slot = 0; Slot < kSlotCount; ++Slot)
        {
            if (!AreGuidValuesEqual(
                    Left[Slot], Right[Slot]))
            {
                return false;
            }
        }
        return true;
    }

    static bool RawMapContainsGuid(
        const std::array<FGuidValue, kSlotCount>& Slots,
        const FGuid& Guid)
    {
        for (const auto& Slot : Slots)
        {
            if (AreGuidsEqual(Guid, Slot))
                return true;
        }
        return false;
    }

    static bool RawSlotsMatchResolvedLoadout(
        const std::array<FGuidValue, kSlotCount>& RawSlots,
        const FResolvedLoadout& Loadout)
    {
        for (int Slot = 0; Slot < kSlotCount; ++Slot)
        {
            const auto& Resolved = Loadout.Slots[Slot];
            if (!Resolved.Occupied)
            {
                if (!RawSlots[Slot].IsZero())
                    return false;
                continue;
            }
            if (!AreGuidsEqual(
                    Resolved.Guid,
                    RawSlots[Slot]))
            {
                return false;
            }
        }
        return true;
    }

    static void InitializeReportedMutation(
        FReportedMutation& Pending,
        const FActionRequest& Request,
        FActionTransaction&& Transaction,
        bool Clear,
        bool UsesBridgeSnapshot,
        int RawBase,
        int EnabledSlots,
        uint64_t BridgeSession,
        uint64_t BridgeBaselineGeneration,
        const FGuid& NewGuid,
        uint64_t NewInstanceIdentity)
    {
        Pending = {};
        Pending.Active = true;
        Pending.Clear = Clear;
        Pending.UsesBridgeSnapshot = UsesBridgeSnapshot;
        Pending.Request = Request;
        Pending.Transaction =
            std::move(Transaction);
        Pending.Phase =
            EReportedMutationPhase::WaitingForAck;
        Pending.RawBase = RawBase;
        Pending.EnabledSlots = EnabledSlots;
        Pending.BridgeSession = BridgeSession;
        Pending.BridgeBaselineGeneration =
            BridgeBaselineGeneration;
        Pending.NewGuid = NewGuid;
        Pending.NewInstanceIdentity =
            NewInstanceIdentity;
        const ULONGLONG Now = GetTickCount64();
        Pending.NextPollAt =
            Now + kReportedMutationPollMs;
        Pending.Deadline =
            Now + kReportedMutationAckTimeoutMs;
    }

    static EReportedMutationBeginResult
        BeginReportedMutationUnsafe(
            const FActionRequest& Request,
            std::string& Message,
            bool& ImmediateSuccess,
            FReportedMutation& Pending,
            FActionTransaction& GuardTransaction)
    {
        ImmediateSuccess = false;
        Pending = {};
        GuardTransaction = {};
        if (Request.WorldGeneration !=
                GWorldGeneration.load(
                    std::memory_order_acquire) ||
            !Request.TargetIdentity ||
            Request.TargetIdentity !=
                GetTargetIdentity(
                    Request.TargetToken))
        {
            Message =
                "The player or match changed; reopen the loadout menu.";
            return EReportedMutationBeginResult::Immediate;
        }
        if (Request.Slot < 0 ||
            Request.Slot >= kSlotCount)
        {
            Message = "Invalid loadout slot.";
            return EReportedMutationBeginResult::Immediate;
        }

        FResolvedLoadout Loadout;
        if (!ResolveLoadoutUnsafe(
                Request.TargetToken,
                Loadout,
                Message))
        {
            return EReportedMutationBeginResult::Immediate;
        }
        if (!Loadout.UsesReportedQuickbarSlots &&
            !Loadout.UsesBridgeQuickbarSlots)
            return EReportedMutationBeginResult::NotReported;
        if (Loadout.UsesGuidOnlyMutation)
        {
            // The source can display exact GUIDs but cannot acknowledge a
            // positional write. Route it through the inventory-only action.
            return EReportedMutationBeginResult::NotReported;
        }
        if (!Loadout.HasAuthoritativeSlots ||
            !Loadout.CanMutateSlots ||
            (Loadout.UsesReportedQuickbarSlots &&
             (!Loadout.HasStrictReportedQuickbarSchema ||
              Loadout.ReportedQuickbarRawBase < 0 ||
              Loadout.ReportedQuickbarEnabledSlots <
                  kSlotCount)) ||
            (Loadout.UsesBridgeQuickbarSlots &&
             (!Loadout.BridgeSession ||
              !Loadout.BridgeGeneration)))
        {
            Message =
                "This quickbar source does not expose a fully validated asynchronous write path.";
            return EReportedMutationBeginResult::Immediate;
        }

        if (Loadout.UsesBridgeQuickbarSlots)
        {
            std::array<FGuidValue, kSlotCount>
                FreshBridgeSlots{};
            uint64_t FreshBridgeGeneration = 0;
            if (!TryReadStrictBridgeRawSlots(
                    Loadout.PlayerController,
                    Loadout.Inventory,
                    Loadout.BridgeSession,
                    Loadout.BridgeGeneration,
                    kBridgeMutationMaxAgeMs,
                    &FreshBridgeSlots,
                    &FreshBridgeGeneration) ||
                !RawSlotsMatchResolvedLoadout(
                    FreshBridgeSlots,
                    Loadout))
            {
                Message =
                    "The client's exact slot report is no longer fresh; wait a moment for it to update and try again.";
                return EReportedMutationBeginResult::Immediate;
            }
            // Every later acknowledgement must advance beyond the freshest
            // complete map that still matches what the operator clicked.
            Loadout.BridgeGeneration =
                FreshBridgeGeneration;
        }

        int32 EntrySize = 0;
        if (!HasSafeMutationInventory(
                Loadout.Inventory, EntrySize))
        {
            Message =
                "This build does not expose a safe mutable inventory layout.";
            return EReportedMutationBeginResult::Immediate;
        }

        const auto& Existing =
            Loadout.Slots[Request.Slot];
        if (Existing.Occupied)
        {
            auto ExistingInstance =
                FindItemInstanceByGuid(
                    Loadout.Inventory,
                    Existing.Guid);
            if (!ExistingInstance ||
                ExistingInstance->GetItemEntry()
                        .ItemDefinition !=
                    Existing.Definition)
            {
                Message =
                    "The slot's inventory instance could not be validated.";
                return EReportedMutationBeginResult::Immediate;
            }
        }
        if ((Existing.Occupied &&
             !AreGuidsEqual(
                 Existing.Guid,
                 Request.ExpectedGuid)) ||
            (!Existing.Occupied &&
             !Request.ExpectedGuid.IsZero()))
        {
            Message =
                "Inventory changed; reopen the item menu and try again.";
            return EReportedMutationBeginResult::Immediate;
        }

        // The modern quickbar RPC identifies removals by definition, not by
        // GUID. If another row shares the target definition, clearing this
        // slot (or later removing its old row after replacement) can disturb a
        // different slot and cannot be recovered unambiguously.
        if (Existing.Occupied &&
            HasOtherEntryForDefinition(
                Loadout.Inventory,
                EntrySize,
                Existing.Definition,
                &Existing.Guid))
        {
            Message =
                "This slot shares an item definition with another inventory row and cannot be edited unambiguously.";
            return EReportedMutationBeginResult::Immediate;
        }

        FActionTransaction& Transaction =
            GuardTransaction;
        if (Request.Clear)
        {
            if (!Existing.Occupied)
            {
                ImmediateSuccess = true;
                Message = "Slot is already empty.";
                return EReportedMutationBeginResult::Immediate;
            }
            if (!ArmActionTransaction(
                    Loadout,
                    Request,
                    Existing,
                    Transaction,
                    Message))
            {
                return EReportedMutationBeginResult::Immediate;
            }

            Transaction.Phase =
                EActionTransactionPhase::
                    WaitingForReportedAck;
            bool DispatchMayHaveStarted = false;
            if (!TryIssueReportedQuickbarWrite(
                    Loadout.PlayerController,
                    Existing.Definition,
                    Request.Slot,
                    true,
                    &DispatchMayHaveStarted))
            {
                Transaction.HasStagedItem = false;
                if (DispatchMayHaveStarted)
                {
                    // ProcessEvent crossed an unknown completion boundary.
                    // Retain the old row so a late client reference remains
                    // valid, and quarantine only this player's editor.
                    MarkTargetFailed(Request.TargetToken);
                    Message =
                        "The client quickbar clear faulted after dispatch may have started; the inventory row was retained and editing was disabled for this player.";
                }
                else
                {
                    Message =
                        "The client quickbar writer was unavailable before dispatch; no inventory change was made. Try again.";
                }
                return EReportedMutationBeginResult::Immediate;
            }
            InitializeReportedMutation(
                Pending,
                Request,
                std::move(Transaction),
                true,
                Loadout.UsesBridgeQuickbarSlots,
                Loadout.ReportedQuickbarRawBase,
                Loadout.ReportedQuickbarEnabledSlots,
                Loadout.BridgeSession,
                Loadout.BridgeGeneration,
                FGuid{},
                0);
            return EReportedMutationBeginResult::Pending;
        }

        auto Definition =
            reinterpret_cast<const UFortItemDefinition*>(
                Request.ItemToken);
        if (!Request.ItemIdentity ||
            Request.ItemIdentity !=
                GetLiveObjectIdentity(Definition) ||
            !IsSafePrimaryItemDefinition(Definition))
        {
            Message =
                "That item is unavailable or cannot occupy a loadout slot.";
            return EReportedMutationBeginResult::Immediate;
        }
        if (Definition->Name.ToUtf8() !=
            Request.ExpectedItemId)
        {
            Message =
                "That item changed or unloaded; reopen the item menu.";
            return EReportedMutationBeginResult::Immediate;
        }
        if (Existing.Occupied &&
            Existing.Definition == Definition)
        {
            ImmediateSuccess = true;
            Message =
                "Loadout slot " +
                std::to_string(Request.Slot + 1) +
                " already contains that item.";
            return EReportedMutationBeginResult::Immediate;
        }
        if (HasOtherEntryForDefinition(
                Loadout.Inventory,
                EntrySize,
                Definition,
                Existing.Occupied
                    ? &Existing.Guid
                    : nullptr))
        {
            Message =
                "That item definition already exists in another slot, so it cannot be placed unambiguously.";
            return EReportedMutationBeginResult::Immediate;
        }

        if (!ArmActionTransaction(
                Loadout,
                Request,
                Existing,
                Transaction,
                Message))
        {
            return EReportedMutationBeginResult::Immediate;
        }

        const int32 Count = SafeStackCount(Definition);
        int32 LoadedAmmo = 0;
        int32 PhantomReserveAmmo = 0;
        ResolveLoadedAmmo(
            Definition,
            LoadedAmmo,
            PhantomReserveAmmo);
        auto NewItem = Loadout.Inventory->GiveItem(
            Definition,
            Count,
            LoadedAmmo,
            0,
            false,
            false,
            PhantomReserveAmmo,
            TArray<FFortItemEntryStateValue>{},
            true);
        if (!IsLiveObject(NewItem))
        {
            const bool Restored =
                RecoverTransaction(Transaction);
            if (!Restored)
                MarkTargetFailed(Request.TargetToken);
            Message = Restored
                ? "The game rejected that item; the old slot was kept."
                : "The game rejected that item and recovery could not be verified; editing was disabled for this player.";
            return EReportedMutationBeginResult::Immediate;
        }

        const uint64_t NewItemIdentity =
            GetLiveObjectIdentity(NewItem);
        const FGuid NewGuid =
            NewItem->GetItemEntry().ItemGuid;
        Transaction.NewGuid = NewGuid;
        Transaction.NewInstanceIdentity =
            NewItemIdentity;
        Transaction.Phase =
            EActionTransactionPhase::Staged;
        const UFortItemDefinition* GrantedDefinition =
            NewItem->GetItemEntry().ItemDefinition;
        const FFortItemEntry* GrantedEntry = nullptr;
        UFortWorldItem* GrantedInstance = nullptr;
        if (ToGuidValue(NewGuid).IsZero())
        {
            const bool Restored =
                RecoverTransaction(Transaction);
            if (!Restored)
                MarkTargetFailed(Request.TargetToken);
            Message = Restored
                ? "The game produced an invalid item GUID; the original slot was kept."
                : "The game produced an invalid item GUID and recovery could not be verified; editing was disabled for this player.";
            return EReportedMutationBeginResult::Immediate;
        }
        if (IsTransactionBaselineGuid(
                Transaction, NewGuid))
        {
            // A duplicate GUID makes it impossible to identify a newly added
            // row without risking deletion of a pre-existing item.
            Transaction.HasStagedItem = false;
            MarkTargetFailed(Request.TargetToken);
            Message =
                "The game produced a duplicate item GUID; no cleanup was attempted and editing was disabled for this player.";
            return EReportedMutationBeginResult::Immediate;
        }
        if (!IsSafePrimaryItemDefinition(
                GrantedDefinition) ||
            GrantedDefinition != Definition ||
            !NewItemIdentity ||
            !HasSafeMutationInventory(
                Loadout.Inventory, EntrySize) ||
            !(GrantedEntry = FindEntryByGuid(
                Loadout.Inventory,
                EntrySize,
                NewGuid)) ||
            GrantedEntry->ItemDefinition !=
                GrantedDefinition ||
            !(GrantedInstance =
                FindItemInstanceByGuid(
                    Loadout.Inventory,
                    NewGuid)) ||
            GrantedInstance != NewItem ||
            GetLiveObjectIdentity(GrantedInstance) !=
                NewItemIdentity ||
            GrantedInstance->GetItemEntry()
                    .ItemDefinition !=
                GrantedDefinition)
        {
            const bool Restored =
                RecoverTransaction(Transaction);
            if (!Restored)
                MarkTargetFailed(Request.TargetToken);
            Message = Restored
                ? "The staged item could not be validated; the old slot was kept."
                : "The staged item could not be validated or rolled back; editing was disabled for this player.";
            return EReportedMutationBeginResult::Immediate;
        }
        if (HasOtherEntryForDefinition(
                Loadout.Inventory,
                EntrySize,
                GrantedDefinition,
                &NewGuid,
                Existing.Occupied
                    ? &Existing.Guid
                    : nullptr))
        {
            const bool Restored =
                RecoverTransaction(Transaction);
            if (!Restored)
                MarkTargetFailed(Request.TargetToken);
            Message = Restored
                ? "The granted item matches another inventory row and could not be placed unambiguously."
                : "An ambiguous grant could not be rolled back; editing was disabled for this player.";
            return EReportedMutationBeginResult::Immediate;
        }

        FResolvedLoadout CurrentLoadout;
        std::string CurrentError;
        if (!ResolveLoadoutUnsafe(
                Request.TargetToken,
                CurrentLoadout,
                CurrentError,
                &NewGuid) ||
            CurrentLoadout.UsesReportedQuickbarSlots !=
                Loadout.UsesReportedQuickbarSlots ||
            CurrentLoadout.UsesBridgeQuickbarSlots !=
                Loadout.UsesBridgeQuickbarSlots ||
            !CurrentLoadout.CanMutateSlots ||
            (Loadout.UsesReportedQuickbarSlots &&
             (!CurrentLoadout.HasStrictReportedQuickbarSchema ||
              CurrentLoadout.ReportedQuickbarRawBase !=
                  Loadout.ReportedQuickbarRawBase ||
              CurrentLoadout.ReportedQuickbarEnabledSlots !=
                  Loadout.ReportedQuickbarEnabledSlots)) ||
            (Loadout.UsesBridgeQuickbarSlots &&
             (CurrentLoadout.BridgeSession !=
                  Loadout.BridgeSession ||
              CurrentLoadout.BridgeGeneration <
                  Loadout.BridgeGeneration)) ||
            reinterpret_cast<uintptr_t>(
                CurrentLoadout.Inventory) !=
                Transaction.InventoryToken ||
            GetLiveObjectIdentity(
                CurrentLoadout.Inventory) !=
                Transaction.InventoryIdentity ||
            !UntouchedSlotsMatch(
                CurrentLoadout,
                Transaction) ||
            (Existing.Occupied &&
             (!CurrentLoadout.Slots[
                    Request.Slot].Occupied ||
              !AreGuidsEqual(
                  CurrentLoadout.Slots[
                      Request.Slot].Guid,
                  Existing.Guid))) ||
            (!Existing.Occupied &&
             CurrentLoadout.Slots[
                 Request.Slot].Occupied))
        {
            const bool Restored =
                RecoverTransaction(Transaction);
            if (!Restored)
                MarkTargetFailed(Request.TargetToken);
            Message = Restored
                ? "Inventory changed during the grant; the original slot was kept."
                : "Inventory changed during the grant and recovery could not be verified; editing was disabled for this player.";
            return EReportedMutationBeginResult::Immediate;
        }

        if (Loadout.UsesBridgeQuickbarSlots)
        {
            std::array<FGuidValue, kSlotCount>
                FreshBridgeSlots{};
            uint64_t FreshBridgeGeneration = 0;
            if (!TryReadStrictBridgeRawSlots(
                    CurrentLoadout.PlayerController,
                    CurrentLoadout.Inventory,
                    CurrentLoadout.BridgeSession,
                    Loadout.BridgeGeneration,
                    kBridgeMutationMaxAgeMs,
                    &FreshBridgeSlots,
                    &FreshBridgeGeneration) ||
                !RawSlotMapsEqual(
                    FreshBridgeSlots,
                    Transaction.BaselineSlots))
            {
                const bool Restored =
                    RecoverTransaction(Transaction);
                if (!Restored)
                    MarkTargetFailed(Request.TargetToken);
                Message = Restored
                    ? "The client's exact slot report went stale during the grant; the staged item was removed. Wait for the slots to refresh and try again."
                    : "The client's exact slot report went stale during the grant and recovery could not be verified; editing was disabled for this player.";
                return EReportedMutationBeginResult::Immediate;
            }
            CurrentLoadout.BridgeGeneration =
                FreshBridgeGeneration;
        }

        Transaction.Phase =
            EActionTransactionPhase::
                WaitingForReportedAck;
        bool DispatchMayHaveStarted = false;
        if (!TryIssueReportedQuickbarWrite(
                CurrentLoadout.PlayerController,
                GrantedDefinition,
                Request.Slot,
                false,
                &DispatchMayHaveStarted))
        {
            if (DispatchMayHaveStarted)
            {
                // The call may have crossed ProcessEvent before faulting.
                // Retain both rows so a client slot that accepted the staged
                // GUID can never be left dangling.
                Transaction.HasStagedItem = false;
                MarkTargetFailed(Request.TargetToken);
                Message =
                    "The client quickbar update faulted after dispatch may have started; both inventory rows were retained and editing was disabled for this player.";
            }
            else
            {
                const bool Restored =
                    RecoverTransaction(Transaction);
                if (!Restored)
                    MarkTargetFailed(Request.TargetToken);
                Message = Restored
                    ? "The client quickbar writer was unavailable before dispatch; the staged item was removed. Try again."
                    : "The client quickbar writer was unavailable and staged-item recovery could not be verified; editing was disabled for this player.";
            }
            return EReportedMutationBeginResult::Immediate;
        }

        InitializeReportedMutation(
            Pending,
            Request,
            std::move(Transaction),
            false,
            CurrentLoadout.UsesBridgeQuickbarSlots,
            CurrentLoadout.ReportedQuickbarRawBase,
            CurrentLoadout.ReportedQuickbarEnabledSlots,
            CurrentLoadout.BridgeSession,
            CurrentLoadout.BridgeGeneration,
            NewGuid,
            NewItemIdentity);
        return EReportedMutationBeginResult::Pending;
    }

    static EReportedMutationBeginResult
        TryBeginReportedMutation(
            const FActionRequest* Request,
            std::string* Message,
            bool* ImmediateSuccess,
            FReportedMutation* Pending,
            FActionTransaction* GuardTransaction,
            bool* Completed)
    {
        __try
        {
            *Completed = true;
            return BeginReportedMutationUnsafe(
                *Request,
                *Message,
                *ImmediateSuccess,
                *Pending,
                *GuardTransaction);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *Completed = false;
            *ImmediateSuccess = false;
            return EReportedMutationBeginResult::Immediate;
        }
    }

    static void PublishActionResult(
        const FActionRequest& Request,
        bool Success,
        std::string Message);

    static void FinishReportedMutation(
        bool Success,
        std::string Message)
    {
        const FActionRequest Request =
            GReportedMutation.Request;
        GReportedMutation.Transaction.HasStagedItem =
            false;
        GReportedMutation = {};
        PublishActionResult(
            Request,
            Success,
            std::move(Message));
        GLastSnapshotAt = 0;
    }

    static bool BeginReportedRecoveryUnsafe(
        std::string FailureMessage,
        const std::array<FGuidValue, kSlotCount>*
            ObservedSlots = nullptr,
        uint64_t ObservedBridgeGeneration = 0)
    {
        auto& Pending = GReportedMutation;
        auto PlayerController =
            ResolveRequestedController(
                Pending.Request.TargetToken);
        auto Inventory =
            ResolveWorldInventory(PlayerController);
        int32 EntrySize = 0;
        if (!PlayerController || !Inventory ||
            reinterpret_cast<uintptr_t>(Inventory) !=
                Pending.Transaction.InventoryToken ||
            GetLiveObjectIdentity(Inventory) !=
                Pending.Transaction.InventoryIdentity ||
            !HasSafeMutationInventory(
                Inventory, EntrySize))
        {
            return false;
        }

        const UFortItemDefinition* RestoreDefinition = nullptr;
        bool ClearTarget = false;
        if (Pending.Transaction.Previous.Occupied)
        {
            auto PreviousEntry = FindEntryByGuid(
                Inventory,
                EntrySize,
                Pending.Transaction.Previous.Guid);
            auto PreviousInstance =
                FindItemInstanceByGuid(
                    Inventory,
                    Pending.Transaction.Previous.Guid);
            if (!PreviousEntry || !PreviousInstance ||
                PreviousEntry->ItemDefinition !=
                    Pending.Transaction.Previous.Definition ||
                PreviousInstance->GetItemEntry()
                        .ItemDefinition !=
                    Pending.Transaction.Previous.Definition)
            {
                return false;
            }
            RestoreDefinition =
                Pending.Transaction.Previous.Definition;
        }
        else
        {
            if (Pending.Clear)
                return false;
            auto NewEntry = FindEntryByGuid(
                Inventory,
                EntrySize,
                Pending.NewGuid);
            auto NewInstance = FindItemInstanceByGuid(
                Inventory,
                Pending.NewGuid);
            if (!NewEntry || !NewInstance ||
                NewEntry->ItemDefinition !=
                    NewInstance->GetItemEntry()
                        .ItemDefinition)
            {
                return false;
            }
            RestoreDefinition = NewEntry->ItemDefinition;
            ClearTarget = true;
        }

        if (ObservedSlots)
        {
            Pending.HasRecoveryStartSlots = true;
            Pending.RecoveryStartSlots = *ObservedSlots;
        }

        // A bridge recovery must be acknowledged by a report newer than every
        // same-session report visible before the recovery write. Otherwise a
        // cached pre-edit baseline could falsely authorize deletion of a new
        // inventory row that the client still references.
        Pending.BridgeRecoveryBaselineGeneration =
            Pending.BridgeBaselineGeneration;
        if (Pending.UsesBridgeSnapshot)
        {
            Pending.BridgeRecoveryBaselineGeneration =
                (std::max)(
                    Pending.BridgeRecoveryBaselineGeneration,
                    ObservedBridgeGeneration);
            std::array<FGuidValue, kSlotCount>
                LatestSlots{};
            uint64_t LatestGeneration = 0;
            if (TryReadStrictBridgeRawSlots(
                    PlayerController,
                    Inventory,
                    Pending.BridgeSession,
                    Pending.BridgeBaselineGeneration,
                    kBridgeMutationMaxAgeMs,
                    &LatestSlots,
                    &LatestGeneration))
            {
                Pending.BridgeRecoveryBaselineGeneration =
                    (std::max)(
                        Pending.BridgeRecoveryBaselineGeneration,
                        LatestGeneration);
            }
        }

        Pending.Transaction.Phase =
            EActionTransactionPhase::
                WaitingForReportedRecovery;
        bool DispatchMayHaveStarted = false;
        if (!TryIssueReportedQuickbarWrite(
                PlayerController,
                RestoreDefinition,
                Pending.Request.Slot,
                ClearTarget,
                &DispatchMayHaveStarted))
        {
            return false;
        }
        Pending.Phase =
            EReportedMutationPhase::
                WaitingForRecovery;
        Pending.FailureMessage =
            std::move(FailureMessage);
        const ULONGLONG Now = GetTickCount64();
        Pending.NextPollAt =
            Now + kReportedMutationPollMs;
        Pending.Deadline =
            Now +
            kReportedMutationRecoveryTimeoutMs;
        return true;
    }

    static void FailReportedMutationClosed(
        std::string Message)
    {
        MarkTargetFailed(
            GReportedMutation.Request.TargetToken);
        FinishReportedMutation(
            false,
            std::move(Message));
    }

    static bool RemoveReportedStagedItemAfterRecoveryUnsafe(
        AFortInventory* Inventory)
    {
        auto& Pending = GReportedMutation;
        if (Pending.Clear)
            return true;
        int32 EntrySize = 0;
        if (!HasSafeMutationInventory(
                Inventory, EntrySize))
        {
            return false;
        }
        auto Entry = FindEntryByGuid(
            Inventory,
            EntrySize,
            Pending.NewGuid);
        auto Instance = FindItemInstanceByGuid(
            Inventory,
            Pending.NewGuid);
        if (!Entry || !Instance ||
            GetLiveObjectIdentity(Instance) !=
                Pending.NewInstanceIdentity ||
            Entry->ItemDefinition !=
                Instance->GetItemEntry()
                    .ItemDefinition)
        {
            return false;
        }
        Inventory->Remove(Pending.NewGuid);
        if (!HasSafeMutationInventory(
                Inventory, EntrySize))
        {
            return false;
        }
        return !FindEntryByGuid(
                    Inventory,
                    EntrySize,
                    Pending.NewGuid) &&
            !FindItemInstanceByGuid(
                Inventory,
                Pending.NewGuid);
    }

    static bool FinalizeReportedMutationUnsafe(
        AFortPlayerControllerAthena* PlayerController,
        AFortInventory* Inventory,
        std::string& Message)
    {
        auto& Pending = GReportedMutation;
        int32 EntrySize = 0;
        if (!HasSafeMutationInventory(
                Inventory, EntrySize))
        {
            Message =
                "Inventory changed after the client acknowledgement; no deletion was attempted.";
            return false;
        }

        if (Pending.Clear)
        {
            auto PreviousEntry = FindEntryByGuid(
                Inventory,
                EntrySize,
                Pending.Transaction.Previous.Guid);
            auto PreviousInstance =
                FindItemInstanceByGuid(
                    Inventory,
                    Pending.Transaction.Previous.Guid);
            if (!PreviousEntry || !PreviousInstance ||
                PreviousEntry->ItemDefinition !=
                    Pending.Transaction.Previous.Definition ||
                PreviousInstance->GetItemEntry()
                        .ItemDefinition !=
                    Pending.Transaction.Previous.Definition)
            {
                Message =
                    "The original inventory row changed after the clear acknowledgement.";
                return false;
            }
            Pending.Transaction.Phase =
                EActionTransactionPhase::RemovingOld;
            Inventory->Remove(
                Pending.Transaction.Previous.Guid);
        }
        else if (Pending.Transaction.Previous.Occupied)
        {
            auto PreviousEntry = FindEntryByGuid(
                Inventory,
                EntrySize,
                Pending.Transaction.Previous.Guid);
            auto PreviousInstance =
                FindItemInstanceByGuid(
                    Inventory,
                    Pending.Transaction.Previous.Guid);
            if (!PreviousEntry || !PreviousInstance ||
                PreviousEntry->ItemDefinition !=
                    Pending.Transaction.Previous.Definition ||
                PreviousInstance->GetItemEntry()
                        .ItemDefinition !=
                    Pending.Transaction.Previous.Definition)
            {
                Message =
                    "The original inventory row changed after the placement acknowledgement.";
                return false;
            }
            Pending.Transaction.Phase =
                EActionTransactionPhase::RemovingOld;
            Inventory->Remove(
                Pending.Transaction.Previous.Guid);
        }

        Inventory->ForceNetUpdate();
        PlayerController->ForceNetUpdate();
        if (!HasSafeMutationInventory(
                Inventory, EntrySize) ||
            (Pending.Transaction.Previous.Occupied &&
             (FindEntryByGuid(
                  Inventory,
                  EntrySize,
                  Pending.Transaction.Previous.Guid) ||
              FindItemInstanceByGuid(
                  Inventory,
                  Pending.Transaction.Previous.Guid))))
        {
            Message =
                "The acknowledged quickbar change could not be verified after inventory replication.";
            return false;
        }

        FResolvedLoadout Verified;
        std::string VerifyError;
        if (!ResolveLoadoutUnsafe(
                Pending.Request.TargetToken,
                Verified,
                VerifyError,
                nullptr,
                ESlotResolvePolicy::
                    PostMutationVerification) ||
            Verified.UsesReportedQuickbarSlots !=
                !Pending.UsesBridgeSnapshot ||
            Verified.UsesBridgeQuickbarSlots !=
                Pending.UsesBridgeSnapshot ||
            !Verified.HasAuthoritativeSlots ||
            !Verified.CanMutateSlots ||
            (!Pending.UsesBridgeSnapshot &&
             (Verified.ReportedQuickbarRawBase !=
                  Pending.RawBase ||
              Verified.ReportedQuickbarEnabledSlots !=
                  Pending.EnabledSlots)) ||
            (Pending.UsesBridgeSnapshot &&
             (Verified.BridgeSession !=
                  Pending.BridgeSession ||
              Verified.BridgeGeneration <
                  Pending.BridgeBaselineGeneration)) ||
            reinterpret_cast<uintptr_t>(
                Verified.Inventory) !=
                Pending.Transaction.InventoryToken ||
            GetLiveObjectIdentity(
                Verified.Inventory) !=
                Pending.Transaction.InventoryIdentity ||
            !UntouchedSlotsMatch(
                Verified,
                Pending.Transaction))
        {
            Message =
                "The acknowledged quickbar map changed during final verification.";
            return false;
        }

        const auto& VerifiedSlot =
            Verified.Slots[Pending.Request.Slot];
        if (Pending.Clear)
        {
            if (VerifiedSlot.Occupied)
            {
                Message =
                    "The cleared slot changed during final verification.";
                return false;
            }
            Message =
                "Cleared loadout slot " +
                std::to_string(
                    Pending.Request.Slot + 1) +
                ".";
        }
        else
        {
            auto NewEntry = FindEntryByGuid(
                Inventory,
                EntrySize,
                Pending.NewGuid);
            auto NewInstance = FindItemInstanceByGuid(
                Inventory,
                Pending.NewGuid);
            if (!NewEntry || !NewInstance ||
                GetLiveObjectIdentity(NewInstance) !=
                    Pending.NewInstanceIdentity ||
                !VerifiedSlot.Occupied ||
                !AreGuidsEqual(
                    VerifiedSlot.Guid,
                    Pending.NewGuid) ||
                VerifiedSlot.Definition !=
                    NewEntry->ItemDefinition ||
                NewInstance->GetItemEntry()
                        .ItemDefinition !=
                    NewEntry->ItemDefinition)
            {
                Message =
                    "The replacement changed during final verification.";
                return false;
            }
            Message =
                "Updated loadout slot " +
                std::to_string(
                    Pending.Request.Slot + 1) +
                " to " +
                Pending.Request.ExpectedItemId +
                ".";
        }
        Pending.Transaction.Phase =
            EActionTransactionPhase::Committed;
        return true;
    }

    static void TickReportedMutation()
    {
        auto& Pending = GReportedMutation;
        if (!Pending.Active)
            return;
        const ULONGLONG Now = GetTickCount64();
        if (Now < Pending.NextPollAt)
            return;
        Pending.NextPollAt =
            Now + kReportedMutationPollMs;

        if (Pending.Request.WorldGeneration !=
                GWorldGeneration.load(
                    std::memory_order_acquire) ||
            Pending.Request.TargetIdentity !=
                GetTargetIdentity(
                    Pending.Request.TargetToken))
        {
            FailReportedMutationClosed(
                "The player or match changed while waiting for the client; no unacknowledged inventory row was deleted.");
            return;
        }

        auto PlayerController =
            ResolveRequestedController(
                Pending.Request.TargetToken);
        auto Inventory =
            ResolveWorldInventory(PlayerController);
        if (!PlayerController || !Inventory ||
            reinterpret_cast<uintptr_t>(Inventory) !=
                Pending.Transaction.InventoryToken ||
            GetLiveObjectIdentity(Inventory) !=
                Pending.Transaction.InventoryIdentity)
        {
            FailReportedMutationClosed(
                "The player's inventory identity changed while waiting for the client; no unacknowledged inventory row was deleted.");
            return;
        }

        std::array<FGuidValue, kSlotCount>
            CurrentSlots{};
        uint64_t CurrentBridgeGeneration = 0;
        const uint64_t BridgeReadBaseline =
            Pending.Phase ==
                    EReportedMutationPhase::WaitingForRecovery &&
                Pending.BridgeRecoveryBaselineGeneration
                ? Pending.BridgeRecoveryBaselineGeneration
                : Pending.BridgeBaselineGeneration;
        const bool ReadCurrentSlots =
            Pending.UsesBridgeSnapshot
                ? TryReadStrictBridgeRawSlots(
                      PlayerController,
                      Inventory,
                      Pending.BridgeSession,
                      BridgeReadBaseline,
                      kBridgeMutationMaxAgeMs,
                      &CurrentSlots,
                      &CurrentBridgeGeneration)
                : TryReadStrictReportedRawSlots(
                      PlayerController,
                      Inventory,
                      Pending.RawBase,
                      Pending.EnabledSlots,
                      &CurrentSlots);
        if (!ReadCurrentSlots)
        {
            if (Now < Pending.Deadline)
                return;
            FailReportedMutationClosed(
                "The client quickbar could not be read at the timeout; no recovery write was sent, all possibly referenced inventory rows were retained, and editing was disabled for this player.");
            return;
        }

        const int Slot = Pending.Request.Slot;
        const auto& BaselineTarget =
            Pending.Transaction.BaselineSlots[Slot];
        if (Pending.Phase ==
            EReportedMutationPhase::WaitingForAck)
        {
            const bool UntouchedMatch =
                RawUntouchedSlotsMatch(
                    CurrentSlots,
                    Pending.Transaction);
            const bool TargetAcknowledged =
                (!Pending.UsesBridgeSnapshot ||
                 CurrentBridgeGeneration >
                     Pending.BridgeBaselineGeneration) &&
                (Pending.Clear
                     ? CurrentSlots[Slot].IsZero()
                     : AreGuidsEqual(
                           Pending.NewGuid,
                           CurrentSlots[Slot]));
            const bool TargetStillBaseline =
                AreGuidValuesEqual(
                    CurrentSlots[Slot],
                    BaselineTarget);

            // Swaps among the four non-target cells are harmless. Preserve
            // their freshly reported order instead of permanently disabling
            // editing for an active player. A target GUID appearing in any
            // other cell is different: deleting or recovering either row
            // could then affect the wrong slot, so that remains fail-closed.
            bool TargetGuidMovedElsewhere = false;
            for (int OtherSlot = 0;
                 OtherSlot < kSlotCount;
                 ++OtherSlot)
            {
                if (OtherSlot == Slot)
                    continue;
                if ((!BaselineTarget.IsZero() &&
                     AreGuidValuesEqual(
                         CurrentSlots[OtherSlot],
                         BaselineTarget)) ||
                    (!Pending.Clear &&
                     AreGuidsEqual(
                         Pending.NewGuid,
                         CurrentSlots[OtherSlot])))
                {
                    TargetGuidMovedElsewhere = true;
                    break;
                }
            }

            if (TargetGuidMovedElsewhere ||
                (!TargetAcknowledged &&
                 !TargetStillBaseline))
            {
                FailReportedMutationClosed(
                    "The player rearranged the quickbar during the edit; no recovery write was sent, all possibly referenced inventory rows were retained, and editing was disabled for this player.");
                return;
            }
            if (!UntouchedMatch)
            {
                for (int OtherSlot = 0;
                     OtherSlot < kSlotCount;
                     ++OtherSlot)
                {
                    if (OtherSlot != Slot)
                    {
                        Pending.Transaction.BaselineSlots[
                            OtherSlot] =
                            CurrentSlots[OtherSlot];
                    }
                }
            }
            if (!TargetAcknowledged)
            {
                if (Now >= Pending.Deadline)
                {
                    if (!BeginReportedRecoveryUnsafe(
                            "The client did not acknowledge the quickbar change before the timeout; the original target slot was restored.",
                            &CurrentSlots,
                            CurrentBridgeGeneration))
                    {
                        FailReportedMutationClosed(
                            "The client did not acknowledge or recover the quickbar change; all unacknowledged inventory rows were retained.");
                    }
                }
                return;
            }

            std::string Message;
            const bool Success =
                FinalizeReportedMutationUnsafe(
                    PlayerController,
                    Inventory,
                    Message);
            if (!Success)
            {
                // The destructive step is reached only after the strong raw
                // acknowledgement. If final verification then fails, retain
                // whichever acknowledged row remains and disable further edits.
                MarkTargetFailed(
                    Pending.Request.TargetToken);
            }
            FinishReportedMutation(
                Success,
                std::move(Message));
            return;
        }

        if (!Pending.UsesBridgeSnapshot &&
            !Pending.HasRecoveryStartSlots)
        {
            Pending.HasRecoveryStartSlots = true;
            Pending.RecoveryStartSlots = CurrentSlots;
        }
        else if (!Pending.UsesBridgeSnapshot &&
                 !RawSlotMapsEqual(
                     CurrentSlots,
                     Pending.RecoveryStartSlots))
        {
            Pending.RecoveryObservedTransition = true;
        }

        const bool TargetRestored =
            AreGuidValuesEqual(
                CurrentSlots[Slot],
                BaselineTarget);
        const bool NewGuidAbsent =
            Pending.Clear ||
            !RawMapContainsGuid(
                CurrentSlots,
                Pending.NewGuid);
        const bool RecoveryAcknowledged =
            Pending.UsesBridgeSnapshot
                ? CurrentBridgeGeneration >
                    Pending.BridgeRecoveryBaselineGeneration
                : Pending.RecoveryObservedTransition;
        if (RecoveryAcknowledged &&
            TargetRestored && NewGuidAbsent)
        {
            if (!RemoveReportedStagedItemAfterRecoveryUnsafe(
                    Inventory))
            {
                FailReportedMutationClosed(
                    "The original quickbar slot was restored, but staged-item cleanup could not be verified; editing was disabled for this player.");
                return;
            }
            Inventory->ForceNetUpdate();
            PlayerController->ForceNetUpdate();
            const std::string Failure =
                Pending.FailureMessage.empty()
                    ? "The quickbar edit was cancelled and the original slot was restored."
                    : Pending.FailureMessage;
            FinishReportedMutation(
                false,
                Failure);
            return;
        }
        if (Now >= Pending.Deadline)
        {
            FailReportedMutationClosed(
                "The client did not confirm recovery; all possibly referenced inventory rows were retained and editing was disabled for this player.");
        }
    }

    static bool NormalizeManualItemId(
        const std::string& Input,
        std::string& ObjectPath,
        std::string& ObjectName)
    {
        ObjectPath.clear();
        ObjectName.clear();
        size_t Start = 0;
        size_t End = Input.size();
        while (Start < End &&
               (Input[Start] == ' ' || Input[Start] == '\t' ||
                Input[Start] == '\r' || Input[Start] == '\n'))
        {
            ++Start;
        }
        while (End > Start &&
               (Input[End - 1] == ' ' || Input[End - 1] == '\t' ||
                Input[End - 1] == '\r' || Input[End - 1] == '\n'))
        {
            --End;
        }
        if (Start == End || End - Start > 511)
            return false;

        ObjectPath = Input.substr(Start, End - Start);
        const size_t FirstQuote = ObjectPath.find('\'');
        const size_t LastQuote = ObjectPath.rfind('\'');
        if (FirstQuote != std::string::npos &&
            LastQuote != std::string::npos &&
            LastQuote > FirstQuote + 1)
        {
            ObjectPath = ObjectPath.substr(
                FirstQuote + 1,
                LastQuote - FirstQuote - 1);
        }
        if (ObjectPath.empty() || ObjectPath.size() > 511)
            return false;
        for (const unsigned char Character : ObjectPath)
        {
            if (Character < 0x21 || Character > 0x7e)
                return false;
        }

        const size_t LastSlash = ObjectPath.find_last_of('/');
        const size_t LastDot = ObjectPath.find_last_of('.');
        const size_t Separator =
            LastDot != std::string::npos &&
                    (LastSlash == std::string::npos || LastDot > LastSlash)
                ? LastDot
                : LastSlash;
        ObjectName = Separator == std::string::npos
            ? ObjectPath
            : ObjectPath.substr(Separator + 1);
        return !ObjectName.empty() && ObjectName.size() <= 255;
    }

    static const UFortItemDefinition*
        ResolveResidentManualItemDefinition(
            const std::string& Input,
            std::string& Error)
    {
        std::string ObjectPath;
        std::string ObjectName;
        if (!NormalizeManualItemId(
                Input, ObjectPath, ObjectName))
        {
            Error =
                "Enter a valid loaded item ID or full asset path.";
            return nullptr;
        }

        const UFortItemDefinition* Definition = nullptr;
        auto ItemClass = UFortItemDefinition::StaticClass();
        if (!ItemClass)
        {
            Error = "Item definitions are unavailable on this build.";
            return nullptr;
        }

        // StaticFindObject never loads a package. Try the exact path first;
        // short IDs fall back to one resident-object name lookup only when the
        // operator presses Replace. The continuous all-item catalog is gone.
        if (SDK::Offsets::StaticFindObject)
        {
            const UEAllocatedWString WidePath(
                ObjectPath.begin(), ObjectPath.end());
            Definition =
                static_cast<const UFortItemDefinition*>(
                    SDK::StaticFindObject(
                        WidePath.c_str(), ItemClass));
        }
        const bool IsShortId =
            ObjectPath.find('/') == std::string::npos &&
            ObjectPath.find('.') == std::string::npos;
        if (!Definition && IsShortId)
        {
            Definition =
                TUObjectArray::FindObject<UFortItemDefinition>(
                    ObjectName.c_str());
        }
        if (!Definition || !IsLiveObject(Definition))
        {
            Error =
                "No loaded item matched that ID. Use its exact WID or full asset path.";
            return nullptr;
        }
        if (Lowercase(Definition->Name.ToUtf8()) !=
                Lowercase(ObjectName) ||
            !IsSafePrimaryItemDefinition(Definition))
        {
            Error =
                "That object is not a safe carryable loadout item.";
            return nullptr;
        }
        return Definition;
    }

    static bool ApplyActionUnsafe(
        const FActionRequest& Request,
        std::string& Message,
        FActionTransaction& Transaction)
    {
        if (Request.WorldGeneration !=
                GWorldGeneration.load(
                    std::memory_order_acquire) ||
            !Request.TargetIdentity ||
            Request.TargetIdentity !=
                GetTargetIdentity(
                    Request.TargetToken))
        {
            Message =
                "The player or match changed; reopen the loadout menu.";
            return false;
        }
        if (Request.Slot < 0 ||
            Request.Slot >= kSlotCount)
        {
            Message = "Invalid loadout slot.";
            return false;
        }

        FResolvedLoadout Loadout;
        if (!ResolveLoadoutUnsafe(
                Request.TargetToken,
                Loadout,
                Message))
        {
            return false;
        }
        if (Request.InventoryOnly &&
            !Loadout.HasLegacyQuickbar)
        {
            // The simplified editor targets the exact displayed inventory GUID
            // and deliberately makes no client-position promise. Never enter a
            // reported/bridge writer or wait for a client acknowledgement.
            Loadout.CanMutateSlots = false;
            Loadout.UsesGuidOnlyMutation = true;
        }
        if (!Loadout.UsesGuidOnlyMutation &&
            (Loadout.UsesReportedQuickbarSlots ||
             Loadout.UsesBridgeQuickbarSlots))
        {
            // TickActions must route acknowledged positional sources through
            // BeginReportedMutationUnsafe. A source that appeared between the
            // two guarded resolutions is retried instead of written without an
            // acknowledgement state machine.
            Message =
                "The quickbar source changed; retry the loadout edit.";
            return false;
        }
        if (!Loadout.HasAuthoritativeSlots &&
            !Loadout.UsesGuidOnlyMutation)
        {
            Message =
                "The player's exact quickbar order is not observable on this build.";
            return false;
        }
        if (!Loadout.CanMutateSlots &&
            !Loadout.UsesGuidOnlyMutation)
        {
            Message =
                "The player's exact reported slots are visible, but this build does not expose a synchronous quickbar write path that can be verified safely.";
            return false;
        }
        int32 MutationEntrySize = 0;
        if (!HasSafeMutationInventory(
                Loadout.Inventory,
                MutationEntrySize))
        {
            Message =
                "This build does not expose a safe mutable inventory layout.";
            return false;
        }

        const auto& Existing =
            Loadout.Slots[Request.Slot];
        if (Existing.Occupied)
        {
            auto ExistingInstance =
                FindItemInstanceByGuid(
                    Loadout.Inventory,
                    Existing.Guid);
            if (!ExistingInstance ||
                ExistingInstance->GetItemEntry()
                        .ItemDefinition !=
                    Existing.Definition)
            {
                Message =
                    "The slot's inventory instance could not be validated.";
                return false;
            }
        }
        if ((Existing.Occupied &&
             !AreGuidsEqual(
                 Existing.Guid,
                 Request.ExpectedGuid)) ||
            (!Existing.Occupied &&
             !Request.ExpectedGuid.IsZero()))
        {
            Message =
                "Inventory changed; reopen the item menu and try again.";
            return false;
        }

        if (Request.Clear)
        {
            if (!Existing.Occupied)
            {
                Message = "Slot is already empty.";
                return true;
            }
            if (!ArmActionTransaction(
                    Loadout,
                    Request,
                    Existing,
                    Transaction,
                    Message))
            {
                return false;
            }
            if (!Loadout.HasLegacyQuickbar &&
                !SetModernLedgerSlot(
                    Loadout,
                    Request.Slot,
                    nullptr))
            {
                const bool Restored =
                    RestoreModernLedger(
                        Transaction,
                        Loadout.PlayerController,
                        Loadout.Inventory);
                Transaction.HasStagedItem = false;
                Transaction.RecoveryFailed =
                    !Restored;
                Message = Restored
                    ? "The modern quickbar slot could not be tracked safely."
                    : "The modern quickbar mapping could not be tracked or restored safely.";
                return false;
            }
            Transaction.Phase =
                EActionTransactionPhase::ClearingOld;
            Loadout.Inventory->Remove(Existing.Guid);
            bool ClearVerified = false;
            if (Transaction.UsesGuidOnlyMutation)
            {
                ClearVerified =
                    VerifyGuidOnlyClear(Transaction);
            }
            else
            {
                int32 VerifyEntrySize = 0;
                FResolvedLoadout VerifiedLoadout;
                std::string VerifyError;
                ClearVerified =
                    HasSafeMutationInventory(
                        Loadout.Inventory,
                        VerifyEntrySize) &&
                    !FindEntryByGuid(
                        Loadout.Inventory,
                        VerifyEntrySize,
                        Existing.Guid) &&
                    !FindItemInstanceByGuid(
                        Loadout.Inventory,
                        Existing.Guid) &&
                    ResolveLoadoutUnsafe(
                        Request.TargetToken,
                        VerifiedLoadout,
                        VerifyError,
                        nullptr,
                        ESlotResolvePolicy::
                            PostMutationVerification) &&
                    VerifiedLoadout.HasAuthoritativeSlots &&
                    VerifiedLoadout.CanMutateSlots &&
                    reinterpret_cast<uintptr_t>(
                        VerifiedLoadout.Inventory) ==
                        Transaction.InventoryToken &&
                    GetLiveObjectIdentity(
                        VerifiedLoadout.Inventory) ==
                        Transaction.InventoryIdentity &&
                    UntouchedSlotsMatch(
                        VerifiedLoadout,
                        Transaction) &&
                    !VerifiedLoadout.Slots[
                        Request.Slot].Occupied;
            }
            if (!ClearVerified)
            {
                const bool Recovered =
                    RecoverTransaction(Transaction);
                if (!Recovered)
                    MarkTargetFailed(Request.TargetToken);
                Message =
                    Recovered
                        ? "The clear operation could not be confirmed; a safe inventory state was retained."
                        : "The clear operation and recovery could not be verified; loadout editing was disabled for this player.";
                return false;
            }
            Transaction.Phase =
                EActionTransactionPhase::Committed;
            Transaction.HasStagedItem = false;
            Message =
                "Cleared loadout slot " +
                std::to_string(Request.Slot + 1) + ".";
            return true;
        }

        if (!Loadout.HasAuthoritativeSlots &&
            !Loadout.UsesGuidOnlyMutation)
        {
            Message =
                "Exact loadout placement is not supported on this build.";
            return false;
        }

        const UFortItemDefinition* Definition = nullptr;
        if (Request.InventoryOnly)
        {
            Definition =
                ResolveResidentManualItemDefinition(
                    Request.ExpectedItemId, Message);
        }
        else
        {
            Definition =
                reinterpret_cast<const UFortItemDefinition*>(
                    Request.ItemToken);
        }
        if (!Definition ||
            (!Request.InventoryOnly &&
             (!Request.ItemIdentity ||
              Request.ItemIdentity !=
                  GetLiveObjectIdentity(Definition))) ||
            !IsSafePrimaryItemDefinition(Definition))
        {
            if (Message.empty())
            {
                Message =
                    "That item is unavailable or cannot occupy a loadout slot.";
            }
            return false;
        }
        if (!Request.InventoryOnly &&
            Definition->Name.ToUtf8() !=
            Request.ExpectedItemId)
        {
            Message =
                "That item changed or unloaded; reopen the item menu.";
            return false;
        }
        if (Existing.Occupied &&
            Existing.Definition == Definition)
        {
            Message =
                "Loadout slot " +
                std::to_string(Request.Slot + 1) +
                " already contains that item.";
            return true;
        }
        if (!Loadout.HasLegacyQuickbar &&
            !Loadout.UsesGuidOnlyMutation &&
            HasOtherEntryForDefinition(
                Loadout.Inventory,
                MutationEntrySize,
                Definition,
                Existing.Occupied
                    ? &Existing.Guid
                    : nullptr))
        {
            Message =
                "That item definition already exists in another slot, so this build cannot place it unambiguously.";
            return false;
        }

        const int32 Count = SafeStackCount(Definition);
        int32 LoadedAmmo = 0;
        int32 PhantomReserveAmmo = 0;
        ResolveLoadedAmmo(
            Definition,
            LoadedAmmo,
            PhantomReserveAmmo);

        if (!ArmActionTransaction(
                Loadout,
                Request,
                Existing,
                Transaction,
                Message))
        {
            return false;
        }

        auto NewItem = Loadout.Inventory->GiveItem(
            Definition,
            Count,
            LoadedAmmo,
            0,
            false,
            false,
            PhantomReserveAmmo,
            TArray<FFortItemEntryStateValue>{},
            true);
        if (!IsLiveObject(NewItem))
        {
            const bool Restored =
                RecoverTransaction(Transaction);
            if (!Restored)
                MarkTargetFailed(Request.TargetToken);
            Message =
                Restored
                    ? "The game rejected that item; the old slot was kept."
                    : "The game rejected that item and recovery could not be verified; editing was disabled for this player.";
            return false;
        }

        const uint64_t NewItemIdentity =
            GetLiveObjectIdentity(NewItem);
        const FGuid NewGuid =
            NewItem->GetItemEntry().ItemGuid;
        Transaction.NewGuid = NewGuid;
        Transaction.NewInstanceIdentity =
            NewItemIdentity;
        Transaction.Phase =
            EActionTransactionPhase::Staged;
        if (ToGuidValue(NewGuid).IsZero())
        {
            const bool Restored =
                RecoverTransaction(Transaction);
            if (!Restored)
                MarkTargetFailed(Request.TargetToken);
            Message =
                Restored
                    ? "The game produced an invalid item GUID; the original slot was kept."
                    : "The game produced an invalid item GUID and recovery could not be verified; editing was disabled for this player.";
            return false;
        }
        if (IsTransactionBaselineGuid(
                Transaction, NewGuid))
        {
            Transaction.HasStagedItem = false;
            Transaction.RecoveryFailed = true;
            MarkTargetFailed(Request.TargetToken);
            Message =
                "The game produced a duplicate item GUID; loadout editing was disabled for this player.";
            return false;
        }

        const UFortItemDefinition* GrantedDefinition =
            NewItem->GetItemEntry().ItemDefinition;
        int32 EntrySize = 0;
        const FFortItemEntry* GrantedEntry = nullptr;
        UFortWorldItem* GrantedInstance = nullptr;
        if (!IsSafePrimaryItemDefinition(
                GrantedDefinition) ||
            !NewItemIdentity ||
            !HasSafeMutationInventory(
                Loadout.Inventory, EntrySize) ||
            !(GrantedEntry = FindEntryByGuid(
                Loadout.Inventory,
                EntrySize,
                NewGuid)) ||
            GrantedEntry->ItemDefinition !=
                GrantedDefinition ||
            !(GrantedInstance =
                FindItemInstanceByGuid(
                    Loadout.Inventory,
                    NewGuid)) ||
            GrantedInstance != NewItem ||
            GetLiveObjectIdentity(
                GrantedInstance) !=
                NewItemIdentity ||
            GrantedInstance->GetItemEntry()
                    .ItemDefinition !=
                GrantedDefinition)
        {
            const bool Restored =
                RecoverTransaction(Transaction);
            if (!Restored)
                MarkTargetFailed(Request.TargetToken);
            Message =
                Restored
                    ? "The new item could not be verified; the old slot was kept."
                    : "The staged item could not be verified or rolled back; editing was disabled for this player.";
            return false;
        }
        if (Existing.Occupied &&
            Existing.Definition == GrantedDefinition)
        {
            const bool Restored =
                RecoverTransaction(Transaction);
            if (!Restored)
                MarkTargetFailed(Request.TargetToken);
            Message =
                Restored
                    ? "Loadout slot " +
                        std::to_string(
                            Request.Slot + 1) +
                        " already contains that item."
                    : "The duplicate grant could not be rolled back; editing was disabled for this player.";
            return Restored;
        }
        if (!Loadout.HasLegacyQuickbar &&
            !Transaction.UsesGuidOnlyMutation &&
            HasOtherEntryForDefinition(
                Loadout.Inventory,
                EntrySize,
                GrantedDefinition,
                &NewGuid,
                Existing.Occupied
                    ? &Existing.Guid
                    : nullptr))
        {
            const bool Restored =
                RecoverTransaction(Transaction);
            if (!Restored)
                MarkTargetFailed(Request.TargetToken);
            Message =
                Restored
                    ? "The granted item matches another inventory row and could not be placed unambiguously."
                    : "An ambiguous grant could not be rolled back; editing was disabled for this player.";
            return false;
        }

        bool StagedStateStable = false;
        FResolvedLoadout CurrentLoadout;
        if (Transaction.UsesGuidOnlyMutation)
        {
            StagedStateStable =
                VerifyGuidOnlyStagedState(
                    NewGuid,
                    GrantedDefinition,
                    NewItem,
                    NewItemIdentity,
                    Transaction);
        }
        else
        {
            std::string CurrentError;
            StagedStateStable =
                ResolveLoadoutUnsafe(
                    Request.TargetToken,
                    CurrentLoadout,
                    CurrentError,
                    &NewGuid) &&
                CurrentLoadout.HasAuthoritativeSlots &&
                CurrentLoadout.CanMutateSlots &&
                reinterpret_cast<uintptr_t>(
                    CurrentLoadout.Inventory) ==
                    Transaction.InventoryToken &&
                GetLiveObjectIdentity(
                    CurrentLoadout.Inventory) ==
                    Transaction.InventoryIdentity &&
                UntouchedSlotsMatch(
                    CurrentLoadout,
                    Transaction) &&
                (!Existing.Occupied ||
                 (CurrentLoadout.Slots[
                      Request.Slot].Occupied &&
                  AreGuidsEqual(
                      CurrentLoadout.Slots[
                          Request.Slot].Guid,
                      Existing.Guid))) &&
                (Existing.Occupied ||
                 !CurrentLoadout.Slots[
                      Request.Slot].Occupied);
        }
        if (!StagedStateStable)
        {
            const bool Restored =
                RecoverTransaction(Transaction);
            if (!Restored)
                MarkTargetFailed(Request.TargetToken);
            Message =
                Restored
                    ? "Inventory changed during the grant; the original slot was kept."
                    : "Inventory changed during the grant and recovery could not be verified; editing was disabled for this player.";
            return false;
        }
        if (!Transaction.UsesGuidOnlyMutation)
            Loadout = CurrentLoadout;

        // Fully place and replicate the staged item before deleting the old
        // GUID. If any version-specific quickbar step fails, cleanup can still
        // remove only the staged item and leave the player's original slot
        // untouched.
        bool Assigned = false;
        const bool AssignmentCompleted =
            TryAssignNewItemToSlot(
                &Loadout,
                &NewGuid,
                GrantedDefinition,
                NewItem,
                NewItemIdentity,
                &Existing,
                Request.Slot,
                Transaction.UsesGuidOnlyMutation,
                &Message,
                &Assigned);
        if (!AssignmentCompleted || !Assigned)
        {
            const bool Restored =
                RecoverTransaction(Transaction);
            if (!Restored)
                MarkTargetFailed(Request.TargetToken);
            if (!AssignmentCompleted)
            {
                Message =
                    Restored
                        ? "The quickbar change faulted; the original slot was restored."
                        : "The quickbar change faulted and recovery could not be verified; editing was disabled for this player.";
            }
            return false;
        }
        Transaction.Phase =
            EActionTransactionPhase::Assigned;

        if (Existing.Occupied &&
            !AreGuidsEqual(Existing.Guid, NewGuid))
        {
            Transaction.Phase =
                EActionTransactionPhase::RemovingOld;
            Loadout.Inventory->Remove(Existing.Guid);
        }
        if (!VerifyCommittedReplacement(
                Request,
                NewGuid,
                GrantedDefinition,
                Transaction))
        {
            const bool Recovered =
                RecoverTransaction(Transaction);
            if (!Recovered)
                MarkTargetFailed(Request.TargetToken);
            Message =
                Recovered
                    ? "The game did not confirm the replacement; a safe inventory state was retained."
                    : "The replacement and recovery could not be verified; editing was disabled for this player.";
            return false;
        }
        Transaction.Phase =
            EActionTransactionPhase::Committed;
        Transaction.HasStagedItem = false;

        Message =
            "Updated loadout slot " +
            std::to_string(Request.Slot + 1) +
            " to " +
            Request.ExpectedItemId + ".";
        return true;
    }

    static bool TryApplyAction(
        const FActionRequest* Request,
        std::string* Message,
        bool* Result,
        FActionTransaction* Transaction)
    {
        bool Completed = false;
        __try
        {
            *Result =
                ApplyActionUnsafe(
                    *Request,
                    *Message,
                    *Transaction);
            Completed = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Completed = false;
        }
        if (!Completed &&
            Transaction->HasStagedItem)
        {
            RecoverTransaction(*Transaction);
        }
        return Completed;
    }

    static void PublishActionResult(
        const FActionRequest& Request,
        bool Success,
        std::string Message)
    {
        GPendingActionRequest = Request;
        GPendingActionSuccess = Success;
        GPendingActionMessage = std::move(Message);
        GHasPendingActionResult = true;

        FTrySharedStateLock Lock;
        if (!Lock.owns_lock())
            return;

        GPublished.ActionPending = false;
        GPublished.PendingTarget = 0;
        GPublished.PendingSlot = -1;
        GPublished.ActionResult = {
            GPendingActionRequest.Id,
            GPendingActionRequest.TargetToken,
            true,
            GPendingActionSuccess,
            std::move(GPendingActionMessage)
        };
        GHasPendingActionResult = false;
    }

    static void TickActions()
    {
        if (GHasPendingActionResult)
        {
            PublishActionResult(
                GPendingActionRequest,
                GPendingActionSuccess,
                std::move(GPendingActionMessage));
            if (GHasPendingActionResult)
                return;
        }

        if (GReportedMutation.Active)
        {
            TickReportedMutation();
            return;
        }

        FActionRequest Request;
        {
            FTrySharedStateLock Lock;
            if (!Lock.owns_lock() || GActions.empty())
                return;
            Request = GActions.front();
            GActions.pop_front();
        }

        std::string Message;
        bool Success = false;
        bool BeginCompleted = Request.InventoryOnly;
        FReportedMutation ReportedPending;
        FActionTransaction ReportedGuardTransaction;
        auto ReportedBegin =
            EReportedMutationBeginResult::NotReported;
        if (!Request.InventoryOnly)
        {
            ReportedBegin =
                TryBeginReportedMutation(
                    &Request,
                    &Message,
                    &Success,
                    &ReportedPending,
                    &ReportedGuardTransaction,
                    &BeginCompleted);
        }
        if (ReportedBegin ==
            EReportedMutationBeginResult::Pending)
        {
            GReportedMutation =
                std::move(ReportedPending);
            GLastSnapshotAt = 0;
            return;
        }
        if (ReportedBegin ==
            EReportedMutationBeginResult::Immediate)
        {
            if (!BeginCompleted)
            {
                Success = false;
                auto& FaultTransaction =
                    ReportedPending.Transaction.HasStagedItem
                        ? ReportedPending.Transaction
                        : ReportedGuardTransaction;
                bool Recovered = true;
                if (FaultTransaction.HasStagedItem)
                {
                    const bool UnknownWriteBoundary =
                        FaultTransaction.Phase ==
                            EActionTransactionPhase::
                                WaitingForReportedAck ||
                        FaultTransaction.Phase ==
                            EActionTransactionPhase::
                                WaitingForReportedRecovery;
                    if (UnknownWriteBoundary)
                    {
                        // A client ProcessEvent may already have crossed its
                        // completion boundary. Retain every possibly referenced
                        // row; deleting a staged GUID here could invalidate the
                        // client's accepted quickbar update.
                        FaultTransaction.HasStagedItem = false;
                        FaultTransaction.RecoveryFailed = true;
                        Recovered = false;
                    }
                    else
                    {
                        Recovered =
                            RecoverTransaction(
                                FaultTransaction);
                    }
                }
                if (!Recovered ||
                    FaultTransaction.RecoveryFailed)
                {
                    MarkTargetFailed(
                        Request.TargetToken);
                    Message =
                        "The reported quickbar edit faulted at an unknown completion boundary; all possibly referenced inventory rows were retained and editing was disabled for this player.";
                }
                else
                {
                    Message =
                        "The quickbar preflight faulted, but the original inventory state was verified. Try the edit again.";
                }
            }
            PublishActionResult(
                Request,
                Success,
                std::move(Message));
            GLastSnapshotAt = 0;
            return;
        }

        FActionTransaction Transaction;
        const bool Completed =
            TryApplyAction(
                &Request,
                &Message,
                &Success,
                &Transaction);
        if (!Completed)
        {
            Success = false;
        }
        if (Transaction.RecoveryFailed)
        {
            MarkTargetFailed(Request.TargetToken);
            Success = false;
            Message =
                "The item change faulted and inventory recovery could not be verified; loadout editing was disabled for this match.";
            GRecoveryFaultRequested = true;
        }
        else if (!Completed)
        {
            Message =
                "The item change faulted, but the original inventory state was verified.";
        }

        PublishActionResult(
            Request, Success, std::move(Message));

        // Force a fresh snapshot after any attempted mutation.
        GLastSnapshotAt = 0;
    }

    static void PublishUnavailableSnapshot(
        uintptr_t TargetToken,
        const char* Message)
    {
        FLoadoutSnapshot Snapshot;
        Snapshot.TargetToken = TargetToken;
        Snapshot.WorldGeneration =
            GWorldGeneration.load(
                std::memory_order_acquire);
        Snapshot.TargetIdentity =
            GetTargetIdentity(TargetToken);
        Snapshot.Generation = ++GLoadoutGeneration;
        Snapshot.Availability =
            ELoadoutAvailability::Unavailable;
        Snapshot.Message = Message ? Message : "";

        FTrySharedStateLock Lock;
        if (Lock.owns_lock())
            GPublished.Loadout = std::move(Snapshot);
    }

    static void TickLoadoutSnapshot()
    {
        const ULONGLONG Now = GetTickCount64();
        const uintptr_t TargetToken =
            GRequestedTarget.load(
                std::memory_order_acquire);
        if (!TargetToken ||
            GInspectLeaseUntil.load(
                std::memory_order_acquire) < Now)
        {
            return;
        }

        if (IsTargetFailed(TargetToken))
        {
            PublishUnavailableSnapshot(
                TargetToken,
                "Loadout editing was disabled for this player after a safe recovery.");
            return;
        }

        if (TargetToken != GLastSnapshotTarget)
        {
            GLastSnapshotTarget = TargetToken;
            GLastSnapshotAt = 0;
            GLastBridgeSnapshotPollAt = 0;
            GSnapshotFaultTarget = TargetToken;
            GSnapshotFaultCount = 0;
            GSnapshotRetryAt = 0;
            GGameThreadSnapshot = {};
        }

        // A disconnect, replication update, or reflected-object replacement can
        // race a guarded read for one tick. Keep the editor recoverable and use
        // a bounded backoff instead of permanently quarantining the player after
        // one transient snapshot exception. Mutation faults still retain their
        // stricter fail-closed quarantine and recovery path.
        if (GSnapshotFaultTarget == TargetToken &&
            Now < GSnapshotRetryAt)
        {
            return;
        }

        // The client bridge already supplies a validated five-GUID POD map.
        // For ordinary drag swaps, remap only those five GUIDs against the
        // previous validated cards. This avoids a full inventory/reflection
        // walk while still falling back whenever membership or identity
        // changes. Mutation authorization continues to perform its own fresh
        // full resolution and acknowledgement checks.
        bool BridgeMembershipChanged = false;
        if (GGameThreadSnapshot.TargetToken ==
                TargetToken &&
            GGameThreadSnapshot.UsesBridgeSlots &&
            Now - GLastBridgeSnapshotPollAt >=
                kBridgeSnapshotPollMs)
        {
            GLastBridgeSnapshotPollAt = Now;
            FLoadoutSnapshot Refreshed;
            bool Changed = false;
            if (TryRefreshBridgePermutation(
                    GGameThreadSnapshot,
                    &Refreshed,
                    &Changed,
                    &BridgeMembershipChanged))
            {
                if (Changed)
                {
                    FTrySharedStateLock Lock;
                    if (Lock.owns_lock())
                    {
                        GGameThreadSnapshot =
                            Refreshed;
                        GPublished.Loadout =
                            std::move(Refreshed);
                    }
                }
                else if (Refreshed.BridgeGeneration >
                         GGameThreadSnapshot
                             .BridgeGeneration)
                {
                    // A heartbeat with the same permutation is useful for the
                    // next cheap comparison, but does not need a GUI publish.
                    GGameThreadSnapshot.BridgeGeneration =
                        Refreshed.BridgeGeneration;
                }
            }
            else if (BridgeMembershipChanged)
            {
                // A client-side add/remove can precede the corresponding
                // replicated inventory row by a tick. Re-resolve immediately,
                // but retain the last exact bridge view if the row has not
                // arrived yet instead of publishing a stale server estimate.
                GLastSnapshotAt = 0;
            }
        }
        if (Now - GLastSnapshotAt <
            kSnapshotIntervalMs)
        {
            return;
        }
        GLastSnapshotAt = Now;

        FLoadoutSnapshot Snapshot;
        if (!TryBuildLoadoutSnapshot(
                TargetToken, &Snapshot))
        {
            static constexpr std::array<ULONGLONG, 6>
                RetryDelaysMs{
                    250, 500, 1000, 2000, 4000, 5000
                };
            GSnapshotFaultTarget = TargetToken;
            const size_t RetryIndex =
                (std::min)(
                    static_cast<size_t>(GSnapshotFaultCount),
                    RetryDelaysMs.size() - 1);
            if (GSnapshotFaultCount <
                static_cast<unsigned int>(
                    RetryDelaysMs.size() - 1))
            {
                ++GSnapshotFaultCount;
            }
            GSnapshotRetryAt =
                Now + RetryDelaysMs[RetryIndex];
            PublishUnavailableSnapshot(
                TargetToken,
                "Inventory changed during the guarded refresh; retrying automatically.");
            return;
        }

        GSnapshotFaultTarget = TargetToken;
        GSnapshotFaultCount = 0;
        GSnapshotRetryAt = 0;

        if (BridgeMembershipChanged &&
            GGameThreadSnapshot.UsesBridgeSlots &&
            !Snapshot.UsesBridgeSlots)
        {
            GLastSnapshotAt = 0;
            return;
        }

        GGameThreadSnapshot = Snapshot;
        FTrySharedStateLock Lock;
        if (Lock.owns_lock())
            GPublished.Loadout = std::move(Snapshot);
    }

    static void GameThreadTickUnsafe()
    {
        TickActions();
        if (GRecoveryFaultRequested)
            return;
        TickLoadoutSnapshot();
        // Only the five visible received items can enqueue icon work. The old
        // all-definition catalog scan is intentionally never ticked.
        TickIconRequests();
    }

    static bool TryReadWorldContext(
        uintptr_t* WorldToken,
        uint64_t* WorldIdentity)
    {
        __try
        {
            auto World = UWorld::GetWorld();
            *WorldToken =
                reinterpret_cast<uintptr_t>(World);
            *WorldIdentity =
                GetLiveObjectIdentity(World);
            if (World && !*WorldIdentity)
            {
                // Still distinguish a readable non-null world from no world;
                // any later unsafe access remains inside the outer guard.
                *WorldIdentity =
                    static_cast<uint64_t>(*WorldToken);
            }
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *WorldToken = 0;
            *WorldIdentity = 0;
            return false;
        }
    }

    static bool TryResetForWorld(
        uint64_t WorldIdentity)
    {
        if (WorldIdentity != GObservedWorldIdentity)
        {
            GPendingWorldIdentity = WorldIdentity;
            GWorldResetPending = true;
        }
        if (!GWorldResetPending)
            return true;

        FTrySharedStateLock Lock;
        if (!Lock.owns_lock())
            return false;

        GPublished = {};
        GActions.clear();
        GIconRequests.clear();
        GIconResults.clear();
        GQueuedIcons.clear();
        GPendingIconResults.clear();
        GPendingDeferredIconRequests.clear();
        GPendingCatalogItems.clear();
        GHasPendingActionResult = false;
        GRecoveryFaultRequested = false;
        GReportedMutation = {};
        GPendingActionMessage.clear();
        GFailedControllerIdentities.clear();
        GModernSlotLedgers.clear();
        GLastSnapshotTarget = 0;
        GLastSnapshotAt = 0;
        GLastBridgeSnapshotPollAt = 0;
        GSnapshotFaultTarget = 0;
        GSnapshotFaultCount = 0;
        GSnapshotRetryAt = 0;
        GGameThreadSnapshot = {};
        GLastIconAt = 0;
        GAsyncIconLoads.clear();
        GAsyncIconLoadFailures.clear();
        GAsyncIconAttemptedPaths.clear();
        GAsyncIconLoadSchema = {};
        GLastAsyncIconLoadAt = 0;
        GNextAsyncIconLoadUuid = 0x4D470000;
        GRequestedTarget.store(
            0, std::memory_order_release);
        GInspectLeaseUntil.store(
            0, std::memory_order_release);
        GCatalogLeaseUntil.store(
            0, std::memory_order_release);
        GPickerIconLeaseUntil.store(
            0, std::memory_order_release);
        GCatalogResetRequested.store(
            false, std::memory_order_release);
        GDisabledWorld.store(
            0, std::memory_order_release);
        GFaultPublicationPending.store(
            false, std::memory_order_release);
        ResetCatalogStateLocked();

        GObservedWorldIdentity =
            GPendingWorldIdentity;
        GWorldResetPending = false;
        GWorldGeneration.fetch_add(
            1, std::memory_order_acq_rel);
        return true;
    }

    static bool TryPublishSubsystemFault()
    {
        if (GProcessDisabled.load(
                std::memory_order_acquire))
        {
            return false;
        }
        if (!GFaultPublicationPending.load(
                std::memory_order_acquire))
        {
            return true;
        }

        // Never block here: an AV-style SEH may have interrupted code that
        // owned the shared lock without running its C++ destructor.
        FTrySharedStateLock Lock;
        if (!Lock.owns_lock())
            return false;

        GPendingIconResults.clear();
        GHasPendingActionResult = false;
        GReportedMutation = {};
        GPendingActionMessage.clear();
        GModernSlotLedgers.clear();
        GActions.clear();
        GIconRequests.clear();
        GIconResults.clear();
        GQueuedIcons.clear();
        GPublished.ActionPending = false;
        GPublished.Loadout = {};
        GPublished.Loadout.TargetToken =
            GRequestedTarget.load(
                std::memory_order_acquire);
        GPublished.Loadout.Generation =
            ++GLoadoutGeneration;
        GPublished.Loadout.Availability =
            ELoadoutAvailability::Unavailable;
        GPublished.Loadout.Message =
            "Loadout editing was disabled for this match after a game-data fault; no further loadout changes will be attempted.";
        GGameThreadSnapshot = {};
        GLastBridgeSnapshotPollAt = 0;
        ResetCatalogStateLocked();
        GFaultPublicationPending.store(
            false, std::memory_order_release);
        return true;
    }

    static void PublishSubsystemFault(
        uintptr_t WorldToken)
    {
        if (GProcessDisabled.load(
                std::memory_order_acquire))
        {
            return;
        }
        GDisabledWorld.store(
            WorldToken ? WorldToken : 1,
            std::memory_order_release);
        GFaultPublicationPending.store(
            true, std::memory_order_release);
        TryPublishSubsystemFault();
    }

    static bool QueueIconRequest(
        uintptr_t ItemToken,
        uint64_t ItemIdentity,
        bool PickerOnly)
    {
        if (!ItemToken || !ItemIdentity)
            return false;

        FTrySharedStateLock Lock;
        if (!Lock.owns_lock())
        {
            return false;
        }
        if (GQueuedIcons.contains(ItemToken))
        {
            for (auto It = GIconRequests.begin();
                 It != GIconRequests.end();
                 ++It)
            {
                if (It->ItemToken != ItemToken)
                    continue;
                const bool IdentityChanged =
                    It->ItemIdentity !=
                    ItemIdentity;
                It->ItemIdentity =
                    ItemIdentity;
                const bool Promoted =
                    !PickerOnly &&
                    It->PickerOnly;
                if (Promoted)
                {
                    FIconRequest PromotedRequest =
                        *It;
                    PromotedRequest.PickerOnly =
                        false;
                    GIconRequests.erase(It);
                    InsertIconRequestByPriorityLocked(
                        PromotedRequest);
                }
                return IdentityChanged || Promoted;
            }
            return false;
        }
        const size_t AdmissionLimit =
            PickerOnly
                ? kMaxIconRequests - kSlotCount
                : kMaxIconRequests;
        if (GQueuedIcons.size() >= AdmissionLimit)
            return false;

        GQueuedIcons.insert(ItemToken);
        const FIconRequest Request{
            ItemToken, ItemIdentity, PickerOnly
        };
        InsertIconRequestByPriorityLocked(Request);
        return true;
    }

    static void ConsumeIconResults(
        std::vector<FIconPixels>& Results,
        size_t MaxResults)
    {
        FTrySharedStateLock Lock;
        if (!Lock.owns_lock())
            return;
        while (!GIconResults.empty() &&
               Results.size() < MaxResults)
        {
            Results.push_back(
                std::move(GIconResults.front()));
            GIconResults.pop_front();
        }
    }

    static bool ReadLoadoutSnapshot(
        FLoadoutSnapshot& Snapshot,
        FActionResult& ActionResult,
        bool& ActionPending,
        uintptr_t& PendingTarget,
        int& PendingSlot)
    {
        FTrySharedStateLock Lock;
        if (!Lock.owns_lock())
            return false;
        Snapshot = GPublished.Loadout;
        ActionResult = GPublished.ActionResult;
        ActionPending = GPublished.ActionPending;
        PendingTarget = GPublished.PendingTarget;
        PendingSlot = GPublished.PendingSlot;
        return true;
    }

    static bool ReadCatalogSnapshot(
        uint64_t KnownGeneration,
        FCatalogSnapshot& Snapshot)
    {
        FTrySharedStateLock Lock;
        if (!Lock.owns_lock())
            return false;
        if (KnownGeneration ==
            GPublished.Catalog.Generation)
        {
            Snapshot.ScannedObjects =
                GPublished.Catalog.ScannedObjects;
            Snapshot.TotalObjects =
                GPublished.Catalog.TotalObjects;
            Snapshot.Complete =
                GPublished.Catalog.Complete;
            return false;
        }
        const size_t ExistingItems =
            Snapshot.Items.size();
        const size_t PublishedItems =
            GPublished.Catalog.Items.size();
        if (ExistingItems > PublishedItems)
        {
            Snapshot = GPublished.Catalog;
            return true;
        }

        Snapshot.Generation =
            GPublished.Catalog.Generation;
        Snapshot.ScannedObjects =
            GPublished.Catalog.ScannedObjects;
        Snapshot.TotalObjects =
            GPublished.Catalog.TotalObjects;
        Snapshot.Complete =
            GPublished.Catalog.Complete;
        if (Snapshot.Items.capacity() <
            kCatalogReserveItems)
        {
            Snapshot.Items.reserve(
                kCatalogReserveItems);
        }
        Snapshot.Items.insert(
            Snapshot.Items.end(),
            GPublished.Catalog.Items.begin() +
                ExistingItems,
            GPublished.Catalog.Items.end());
        return true;
    }

    static bool SubmitAction(
        uintptr_t TargetToken,
        uint64_t WorldGeneration,
        uint64_t TargetIdentity,
        int Slot,
        bool Clear,
        uintptr_t ItemToken,
        uint64_t ItemIdentity,
        std::string ExpectedItemId,
        const FGuidValue& ExpectedGuid,
        bool InventoryOnly = false)
    {
        if (GProcessDisabled.load(
                std::memory_order_acquire) ||
            GDisabledWorld.load(
                std::memory_order_acquire) ||
            GFaultPublicationPending.load(
                std::memory_order_acquire))
        {
            return false;
        }
        FTrySharedStateLock Lock;
        if (!Lock.owns_lock())
            return false;
        if (GProcessDisabled.load(
                std::memory_order_acquire) ||
            GDisabledWorld.load(
                std::memory_order_acquire) ||
            GFaultPublicationPending.load(
                std::memory_order_acquire) ||
            GPublished.ActionPending ||
            GActions.size() >= 1)
        {
            return false;
        }

        FActionRequest Request;
        Request.Id = GNextActionId++;
        Request.TargetToken = TargetToken;
        Request.WorldGeneration = WorldGeneration;
        Request.TargetIdentity = TargetIdentity;
        Request.Slot = Slot;
        Request.Clear = Clear;
        Request.InventoryOnly = InventoryOnly;
        Request.ItemToken = ItemToken;
        Request.ItemIdentity = ItemIdentity;
        Request.ExpectedItemId =
            std::move(ExpectedItemId);
        Request.ExpectedGuid = ExpectedGuid;
        GActions.push_back(Request);

        GPublished.ActionPending = true;
        GPublished.PendingTarget = TargetToken;
        GPublished.PendingSlot = Slot;
        GPublished.ActionResult = {};
        return true;
    }

    static ImVec4 RarityColor(int Rarity)
    {
        switch (Rarity)
        {
        case 1:
            return ImVec4(0.27f, 0.64f, 0.04f, 1.f);
        case 2:
            return ImVec4(0.03f, 0.60f, 0.83f, 1.f);
        case 3:
            return ImVec4(0.61f, 0.22f, 0.84f, 1.f);
        case 4:
            return ImVec4(0.90f, 0.46f, 0.06f, 1.f);
        case 5:
            return ImVec4(0.83f, 0.60f, 0.13f, 1.f);
        case 6:
            return ImVec4(0.47f, 0.73f, 0.91f, 1.f);
        case 7:
            return ImVec4(0.85f, 0.18f, 0.23f, 1.f);
        case 8:
            return ImVec4(0.15f, 0.69f, 0.73f, 1.f);
        case 9:
            return ImVec4(0.80f, 0.25f, 0.63f, 1.f);
        default:
            return ImVec4(0.55f, 0.57f, 0.60f, 1.f);
        }
    }

    static const char* RarityName(int Rarity)
    {
        switch (Rarity)
        {
        case 0: return "Common";
        case 1: return "Uncommon";
        case 2: return "Rare";
        case 3: return "Epic";
        case 4: return "Legendary";
        case 5: return "Mythic";
        case 6: return "Transcendent";
        case 7: return "Unattainable";
        case 8: return "Exotic";
        default: return "Special";
        }
    }

    static const char* ModeName(EItemMode Mode)
    {
        switch (Mode)
        {
        case EItemMode::BattleRoyale:
            return "Battle Royale";
        case EItemMode::SaveTheWorld:
            return "Save the World";
        default:
            return "Other / Unknown";
        }
    }

    static int ModeSortRank(EItemMode Mode)
    {
        switch (Mode)
        {
        case EItemMode::BattleRoyale:
            return 0;
        case EItemMode::SaveTheWorld:
            return 1;
        default:
            return 2;
        }
    }

    struct FGpuIcon
    {
        ID3D11ShaderResourceView* View = nullptr;
        uint64_t ItemIdentity = 0;
        int Width = 0;
        int Height = 0;
        uint64_t LastUsedFrame = 0;
    };

    struct FRendererState
    {
        ID3D11Device* Device = nullptr;
        std::unordered_map<uintptr_t, FGpuIcon> Icons;
        std::unordered_map<uintptr_t, uint64_t> RequestedIcons;
        std::unordered_set<uintptr_t> SlotIconRequests;
        std::unordered_map<uintptr_t, uint64_t>
            PinnedSlotIcons;
        std::unordered_map<uintptr_t, FIconFailure>
            MissingUntil;
        FCatalogSnapshot Catalog;
        std::vector<size_t> FilteredItems;
        uint64_t CatalogGeneration = 0;
        std::string LastQuery;
        char Search[512]{};
        int ModeFilter = 1;
        int RarityFilter = -1;
        int LastModeFilter = -1;
        int LastRarityFilter = -2;
        int SelectedSlot = -1;
        uintptr_t SelectedItemToken = 0;
        FGuidValue ExpectedGuid;
        uintptr_t PopupTarget = 0;
        uint64_t PopupWorldGeneration = 0;
        uint64_t PopupTargetIdentity = 0;
        uint64_t PopupCacheEpoch = 0;
        uintptr_t RenderedTarget = 0;
        int RenderedFrame = -1;
        bool RenderedExactSlotOrder = false;
        std::array<FGuidValue, kSlotCount>
            RenderedSlotGuids{};
        FLoadoutSnapshot LastSnapshot;
        FActionResult LastActionResult;
        bool LastActionPending = false;
        uintptr_t LastPendingTarget = 0;
        int LastPendingSlot = -1;
        ULONGLONG LastCatalogRefreshAt = 0;
        ULONGLONG ResultShownAt = 0;
        uint64_t LastResultId = 0;
        uint64_t CacheEpoch = 0;
        uint64_t IconQueueEpoch = 0;
        int IconUploadFrame = -1;
    };

    FRendererState GRenderer;

    static void ReleaseGpuIcons()
    {
        for (auto& Pair : GRenderer.Icons)
        {
            if (Pair.second.View)
                Pair.second.View->Release();
        }
        GRenderer.Icons.clear();
        GRenderer.RequestedIcons.clear();
        GRenderer.SlotIconRequests.clear();
        GRenderer.PinnedSlotIcons.clear();
        GRenderer.MissingUntil.clear();
        GRenderer.IconUploadFrame = -1;
    }

    static void SyncRendererCacheEpoch()
    {
        const uint64_t Epoch =
            GCacheEpoch.load(std::memory_order_acquire);
        const uint64_t QueueEpoch =
            GIconQueueEpoch.load(
                std::memory_order_acquire);
        if (GRenderer.IconQueueEpoch !=
            QueueEpoch)
        {
            // A closed picker or expired inspection lease cancels queued work
            // only. Preserve the catalog, failure backoff and all uploaded
            // textures so reopening cannot trigger a copy/sort or icon flash.
            GRenderer.RequestedIcons.clear();
            GRenderer.SlotIconRequests.clear();
            GRenderer.IconQueueEpoch =
                QueueEpoch;
        }
        if (GRenderer.CacheEpoch != Epoch)
        {
            // A true world/catalog reset invalidates published snapshots and
            // queued UObject work. GPU icons and identity-aware failure entries
            // remain safe LRU data and keep already-loaded images stable.
            GRenderer.RequestedIcons.clear();
            GRenderer.SlotIconRequests.clear();
            GRenderer.IconUploadFrame = -1;
            GRenderer.Catalog = {};
            GRenderer.FilteredItems.clear();
            GRenderer.CatalogGeneration = 0;
            GRenderer.LastQuery = "\1";
            GRenderer.LastModeFilter = -1;
            GRenderer.LastRarityFilter = -2;
            GRenderer.LastCatalogRefreshAt = 0;
            GRenderer.LastSnapshot = {};
            GRenderer.LastActionResult = {};
            GRenderer.LastActionPending = false;
            GRenderer.LastPendingTarget = 0;
            GRenderer.LastPendingSlot = -1;
            GRenderer.RenderedExactSlotOrder =
                false;
            GRenderer.RenderedSlotGuids = {};
            GRenderer.CacheEpoch = Epoch;
        }
    }

    static bool UploadIcon(
        ID3D11Device* Device,
        const FIconPixels& Source,
        ID3D11ShaderResourceView** OutView)
    {
        *OutView = nullptr;
        if (!Device ||
            !Source.Success ||
            Source.Width <= 0 ||
            Source.Height <= 0 ||
            Source.Pixels.size() !=
                static_cast<size_t>(Source.Width) *
                    Source.Height * 4)
        {
            return false;
        }

        D3D11_TEXTURE2D_DESC Description{};
        Description.Width = Source.Width;
        Description.Height = Source.Height;
        Description.MipLevels = 1;
        Description.ArraySize = 1;
        Description.Format =
            DXGI_FORMAT_R8G8B8A8_UNORM;
        Description.SampleDesc.Count = 1;
        Description.Usage = D3D11_USAGE_IMMUTABLE;
        Description.BindFlags =
            D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA Data{};
        Data.pSysMem = Source.Pixels.data();
        Data.SysMemPitch = Source.Width * 4;

        ID3D11Texture2D* Texture = nullptr;
        if (FAILED(Device->CreateTexture2D(
                &Description, &Data, &Texture)) ||
            !Texture)
        {
            return false;
        }

        const HRESULT Result =
            Device->CreateShaderResourceView(
                Texture, nullptr, OutView);
        Texture->Release();
        return SUCCEEDED(Result) && *OutView;
    }

    static void EvictGpuIconIfNeeded()
    {
        if (GRenderer.Icons.size() < kMaxGpuIcons)
            return;

        auto Oldest = GRenderer.Icons.end();
        const uint64_t CurrentFrame =
            static_cast<uint64_t>(
                ImGui::GetFrameCount());
        for (auto It = GRenderer.Icons.begin();
             It != GRenderer.Icons.end();
             ++It)
        {
            const auto Pinned =
                GRenderer.PinnedSlotIcons.find(
                    It->first);
            if (It->second.LastUsedFrame ==
                    CurrentFrame ||
                (Pinned !=
                     GRenderer.PinnedSlotIcons.end() &&
                 Pinned->second ==
                     It->second.ItemIdentity))
            {
                continue;
            }
            if (Oldest == GRenderer.Icons.end() ||
                It->second.LastUsedFrame <
                    Oldest->second.LastUsedFrame)
            {
                Oldest = It;
            }
        }
        if (Oldest != GRenderer.Icons.end())
        {
            if (Oldest->second.View)
                Oldest->second.View->Release();
            GRenderer.Icons.erase(Oldest);
        }
    }

    static void ConsumeAndUploadIcons(
        ID3D11Device* Device)
    {
        const int CurrentFrame =
            ImGui::GetFrameCount();
        if (GRenderer.Device == Device &&
            GRenderer.IconUploadFrame ==
                CurrentFrame)
        {
            return;
        }
        if (GRenderer.Device != Device)
        {
            ReleaseGpuIcons();
            GRenderer.Device = Device;
        }
        GRenderer.IconUploadFrame = CurrentFrame;

        std::vector<FIconPixels> Results;
        ConsumeIconResults(
            Results, kMaxIconUploadsPerFrame);
        for (auto& Result : Results)
        {
            auto Requested =
                GRenderer.RequestedIcons.find(
                    Result.ItemToken);
            const auto Pinned =
                GRenderer.PinnedSlotIcons.find(
                    Result.ItemToken);
            if (Requested !=
                    GRenderer.RequestedIcons.end() &&
                Requested->second !=
                    Result.ItemIdentity)
            {
                // This UObject address was reused or superseded while its
                // earlier decode was pending. Never let the stale completion
                // replace the current identity's cache/request state.
                continue;
            }
            if (Pinned !=
                    GRenderer.PinnedSlotIcons.end() &&
                Pinned->second !=
                    Result.ItemIdentity)
            {
                // The result is stale relative to a live slot. Retire only a
                // matching old request marker; a marker for a newer identity
                // must remain intact so its completion is still expected.
                if (Requested !=
                        GRenderer.RequestedIcons.end() &&
                    Requested->second ==
                        Result.ItemIdentity)
                {
                    GRenderer.RequestedIcons.erase(
                        Requested);
                    GRenderer.SlotIconRequests.erase(
                        Result.ItemToken);
                }
                continue;
            }
            if (Requested !=
                    GRenderer.RequestedIcons.end() &&
                Requested->second ==
                    Result.ItemIdentity)
            {
                GRenderer.RequestedIcons.erase(
                    Requested);
            }
            GRenderer.SlotIconRequests.erase(
                Result.ItemToken);
            if (!Result.Success)
            {
                const ULONGLONG RetryAfter =
                    (std::clamp)(
                        Result.RetryAfterMs,
                        static_cast<ULONGLONG>(250),
                        static_cast<ULONGLONG>(30000));
                GRenderer.MissingUntil[
                    Result.ItemToken] =
                    {
                        Result.ItemIdentity,
                        GetTickCount64() +
                            RetryAfter,
                        Result.PickerOnly
                    };
                continue;
            }

            ID3D11ShaderResourceView* View = nullptr;
            if (!UploadIcon(Device, Result, &View))
            {
                GRenderer.MissingUntil[
                    Result.ItemToken] =
                    {
                        Result.ItemIdentity,
                        GetTickCount64() + 30000,
                        Result.PickerOnly
                    };
                continue;
            }
            auto Existing =
                GRenderer.Icons.find(
                    Result.ItemToken);
            if (Existing != GRenderer.Icons.end())
            {
                if (Existing->second.View)
                    Existing->second.View->Release();
                GRenderer.Icons.erase(Existing);
            }
            else
            {
                EvictGpuIconIfNeeded();
            }
            GRenderer.Icons.emplace(
                Result.ItemToken,
                FGpuIcon{
                    View,
                    Result.ItemIdentity,
                    Result.Width,
                    Result.Height,
                    static_cast<uint64_t>(
                        ImGui::GetFrameCount())
                });
            auto Missing =
                GRenderer.MissingUntil.find(
                    Result.ItemToken);
            if (Missing !=
                    GRenderer.MissingUntil.end() &&
                Missing->second.ItemIdentity ==
                    Result.ItemIdentity)
            {
                GRenderer.MissingUntil.erase(
                    Missing);
            }
        }
    }

    static ID3D11ShaderResourceView* GetOrRequestIcon(
        uintptr_t ItemToken,
        uint64_t ItemIdentity,
        int& Width,
        int& Height,
        bool PickerOnly)
    {
        Width = Height = 0;
        if (!ItemToken || !ItemIdentity)
            return nullptr;

        auto Existing =
            GRenderer.Icons.find(ItemToken);
        if (Existing != GRenderer.Icons.end())
        {
            if (Existing->second.ItemIdentity !=
                ItemIdentity)
            {
                // The old SRV may already be referenced by an ImGui draw
                // command emitted earlier in this frame. Keep it alive as a
                // cache miss; the matching upload replaces it at the start of
                // a later frame, before any new draw commands are recorded.
            }
            else
            {
                Existing->second.LastUsedFrame =
                    ImGui::GetFrameCount();
                Width = Existing->second.Width;
                Height = Existing->second.Height;
                return Existing->second.View;
            }
        }

        const bool NeedsSlotPromotion =
            !PickerOnly &&
            !GRenderer.SlotIconRequests.contains(
                ItemToken);
        const ULONGLONG Now = GetTickCount64();
        auto Missing =
            GRenderer.MissingUntil.find(ItemToken);
        if (Missing != GRenderer.MissingUntil.end())
        {
            if (Missing->second.ItemIdentity ==
                    ItemIdentity &&
                Missing->second.RetryAt > Now &&
                (PickerOnly ||
                 !Missing->second.PickerOnly))
            {
                return nullptr;
            }
            GRenderer.MissingUntil.erase(Missing);
        }

        const auto Requested =
            GRenderer.RequestedIcons.find(ItemToken);
        const bool NeedsIdentityRequest =
            Requested ==
                GRenderer.RequestedIcons.end() ||
            Requested->second != ItemIdentity;
        if ((NeedsIdentityRequest ||
             NeedsSlotPromotion) &&
            QueueIconRequest(
                ItemToken,
                ItemIdentity,
                PickerOnly))
        {
            GRenderer.RequestedIcons[
                ItemToken] = ItemIdentity;
            if (!PickerOnly)
            {
                GRenderer.SlotIconRequests.insert(
                    ItemToken);
            }
        }
        return nullptr;
    }

    static void DrawPlaceholderIcon(
        ImDrawList* Draw,
        const ImVec2& Center,
        float Size,
        ImU32 Color)
    {
        const float Half = Size * 0.34f;
        Draw->AddRect(
            ImVec2(Center.x - Half, Center.y - Half),
            ImVec2(Center.x + Half, Center.y + Half),
            Color, 3.f, 0, 1.5f);
        Draw->AddLine(
            ImVec2(Center.x - Half * 0.65f, Center.y + Half * 0.35f),
            ImVec2(Center.x - Half * 0.10f, Center.y - Half * 0.15f),
            Color, 1.5f);
        Draw->AddLine(
            ImVec2(Center.x - Half * 0.10f, Center.y - Half * 0.15f),
            ImVec2(Center.x + Half * 0.65f, Center.y + Half * 0.45f),
            Color, 1.5f);
        Draw->AddCircleFilled(
            ImVec2(Center.x + Half * 0.38f, Center.y - Half * 0.38f),
            (std::max)(1.5f, Size * 0.045f),
            Color);
    }

    static bool DrawItemCard(
        const char* WidgetId,
        const FItemMetadata* Item,
        const ImVec2& Size,
        bool Empty,
        bool Pending,
        bool Selected,
        bool Enabled,
        bool PickerItem)
    {
        ImGui::PushID(WidgetId);
        const ImVec2 Position =
            ImGui::GetCursorScreenPos();
        const bool Clicked =
            ImGui::InvisibleButton(
                "##card", Size);
        const bool Hovered = ImGui::IsItemHovered();
        auto Draw = ImGui::GetWindowDrawList();

        const ImVec2 End(
            Position.x + Size.x,
            Position.y + Size.y);
        const float CornerRadius =
            PickerItem ? 7.f : 4.f;
        Draw->AddRectFilled(
            ImVec2(Position.x + 2.f, Position.y + 2.f),
            ImVec2(End.x + 2.f, End.y + 2.f),
            IM_COL32(0, 0, 0, 100),
            CornerRadius);

        const ImVec4 Base = Empty
            ? ImVec4(0.09f, 0.10f, 0.12f, 1.f)
            : RarityColor(Item ? Item->Rarity : 0);
        Draw->AddRectFilled(
            Position, End,
            ImGui::GetColorU32(Base),
            CornerRadius);

        const auto Tone =
            [](const ImVec4& Color,
               float Scale,
               float Lift)
            {
                return ImVec4(
                    (std::clamp)(
                        Color.x * Scale + Lift,
                        0.f, 1.f),
                    (std::clamp)(
                        Color.y * Scale + Lift,
                        0.f, 1.f),
                    (std::clamp)(
                        Color.z * Scale + Lift,
                        0.f, 1.f),
                    1.f);
            };
        const ImVec4 Top =
            Empty
                ? Tone(Base, 1.18f, 0.015f)
                : Tone(Base, 1.13f, 0.055f);
        const ImVec4 Bottom =
            Tone(Base, Empty ? 0.72f : 0.53f, 0.f);
        Draw->AddRectFilledMultiColor(
            ImVec2(Position.x + 1.f, Position.y + 1.f),
            ImVec2(End.x - 1.f, End.y - 1.f),
            ImGui::GetColorU32(Top),
            ImGui::GetColorU32(Top),
            ImGui::GetColorU32(Bottom),
            ImGui::GetColorU32(Bottom));

        if (Hovered && Enabled)
        {
            Draw->AddRectFilled(
                Position,
                End,
                IM_COL32(255, 255, 255, 22),
                CornerRadius);
        }

        if (Selected)
        {
            Draw->AddRect(
                Position,
                End,
                IM_COL32(17, 18, 20, 255),
                CornerRadius,
                0,
                5.f);
            Draw->AddRect(
                ImVec2(Position.x + 1.f, Position.y + 1.f),
                ImVec2(End.x - 1.f, End.y - 1.f),
                IM_COL32(247, 239, 35, 255),
                CornerRadius,
                0,
                3.f);
        }
        else
        {
            ImVec4 BorderColor = Empty
                ? ImVec4(0.19f, 0.21f, 0.24f, 1.f)
                : ImVec4(
                    (std::min)(1.f, Base.x + 0.18f),
                    (std::min)(1.f, Base.y + 0.18f),
                    (std::min)(1.f, Base.z + 0.18f),
                    1.f);
            if (Hovered && Enabled)
                BorderColor =
                    ImVec4(0.85f, 0.88f, 0.93f, 1.f);
            Draw->AddRect(
                Position,
                End,
                ImGui::GetColorU32(BorderColor),
                CornerRadius,
                0,
                Hovered && Enabled ? 2.f : 1.5f);
        }

        const ImVec2 Center(
            (Position.x + End.x) * 0.5f,
            Empty
                ? (Position.y + End.y) * 0.5f
                : Position.y + Size.y * 0.47f);
        if (Empty)
        {
            const float Radius =
                (std::min)(Size.x, Size.y) * 0.105f;
            const ImU32 PlusColor =
                ImGui::GetColorU32(
                    Hovered && Enabled
                        ? ImVec4(0.68f, 0.72f, 0.79f, 1.f)
                        : ImVec4(0.37f, 0.40f, 0.45f, 1.f));
            Draw->AddLine(
                ImVec2(Center.x - Radius, Center.y),
                ImVec2(Center.x + Radius, Center.y),
                PlusColor, 2.f);
            Draw->AddLine(
                ImVec2(Center.x, Center.y - Radius),
                ImVec2(Center.x, Center.y + Radius),
                PlusColor, 2.f);
        }
        else if (Item)
        {
            int TextureWidth = 0;
            int TextureHeight = 0;
            auto Texture = GetOrRequestIcon(
                Item->Token,
                Item->ObjectIdentity,
                TextureWidth,
                TextureHeight,
                PickerItem);
            if (Texture)
            {
                const float MaximumWidth =
                    Size.x * 0.86f;
                const float MaximumHeight =
                    Size.y * 0.73f;
                const float Scale =
                    TextureWidth > 0 &&
                    TextureHeight > 0
                        ? (std::min)(
                            MaximumWidth /
                                TextureWidth,
                            MaximumHeight /
                                TextureHeight)
                        : 1.f;
                const ImVec2 ImageSize(
                    TextureWidth > 0
                        ? TextureWidth * Scale
                        : MaximumWidth,
                    TextureHeight > 0
                        ? TextureHeight * Scale
                        : MaximumHeight);
                const ImVec2 ImageMin(
                    Center.x - ImageSize.x * 0.5f,
                    Center.y - ImageSize.y * 0.5f);
                Draw->AddImage(
                    reinterpret_cast<ImTextureID>(Texture),
                    ImageMin,
                    ImVec2(
                        ImageMin.x + ImageSize.x,
                        ImageMin.y + ImageSize.y));
            }
            else
            {
                DrawPlaceholderIcon(
                    Draw,
                    Center,
                    (std::min)(Size.x, Size.y) * 0.48f,
                    IM_COL32(255, 255, 255, 105));
            }
        }

        if (Pending || !Enabled)
        {
            Draw->AddRectFilled(
                Position, End,
                IM_COL32(8, 9, 12, 145),
                CornerRadius);
            if (Pending)
            {
                Draw->AddCircle(
                    Center,
                    (std::min)(Size.x, Size.y) * 0.12f,
                    IM_COL32(220, 225, 238, 230),
                    24, 2.f);
            }
        }

        if (Hovered)
        {
            if (Empty)
            {
                ImGui::SetTooltip(
                    Enabled
                        ? "Empty slot\nClick to choose an item"
                        : "Loadout is not ready");
            }
            else if (Item)
            {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(
                    Item->Name.c_str());
                ImGui::PushStyleColor(
                    ImGuiCol_Text,
                    ImVec4(0.58f, 0.61f, 0.68f, 1.f));
                ImGui::TextUnformatted(
                    Item->Id.c_str());
                ImGui::Text(
                    "%s  |  %s",
                    ModeName(Item->Mode),
                    RarityName(Item->Rarity));
                ImGui::PopStyleColor();
                ImGui::EndTooltip();
            }
        }

        ImGui::PopID();
        return Clicked && !Pending && Enabled;
    }

    static bool DrawClearCard(
        const ImVec2& Size)
    {
        ImGui::PushID("clear-item");
        const ImVec2 Position =
            ImGui::GetCursorScreenPos();
        const bool Clicked =
            ImGui::InvisibleButton(
                "##card", Size);
        const bool Hovered = ImGui::IsItemHovered();
        auto Draw = ImGui::GetWindowDrawList();
        const ImVec2 End(
            Position.x + Size.x,
            Position.y + Size.y);
        const float CornerRadius = 7.f;
        Draw->AddRectFilled(
            ImVec2(Position.x + 2.f, Position.y + 2.f),
            ImVec2(End.x + 2.f, End.y + 2.f),
            IM_COL32(0, 0, 0, 100),
            CornerRadius);
        Draw->AddRectFilled(
            Position, End,
            Hovered
                ? IM_COL32(78, 43, 50, 255)
                : IM_COL32(23, 25, 30, 255),
            CornerRadius);
        Draw->AddRectFilledMultiColor(
            ImVec2(Position.x + 1.f, Position.y + 1.f),
            ImVec2(End.x - 1.f, End.y - 1.f),
            Hovered
                ? IM_COL32(102, 53, 61, 255)
                : IM_COL32(37, 40, 48, 255),
            Hovered
                ? IM_COL32(102, 53, 61, 255)
                : IM_COL32(37, 40, 48, 255),
            Hovered
                ? IM_COL32(50, 25, 31, 255)
                : IM_COL32(18, 20, 25, 255),
            Hovered
                ? IM_COL32(50, 25, 31, 255)
                : IM_COL32(18, 20, 25, 255));
        Draw->AddRect(
            Position, End,
            Hovered
                ? IM_COL32(230, 112, 122, 255)
                : IM_COL32(47, 51, 60, 255),
            CornerRadius,
            0,
            Hovered ? 2.f : 1.5f);

        const ImVec2 Center(
            (Position.x + End.x) * 0.5f,
            (Position.y + End.y) * 0.5f);
        const float Unit =
            (std::min)(Size.x, Size.y) * 0.10f;
        const ImU32 Color =
            Hovered
                ? IM_COL32(255, 196, 201, 255)
                : IM_COL32(202, 205, 215, 230);
        Draw->AddRect(
            ImVec2(Center.x - Unit, Center.y - Unit * 0.65f),
            ImVec2(Center.x + Unit, Center.y + Unit * 1.25f),
            Color, 2.f, 0, 2.f);
        Draw->AddLine(
            ImVec2(Center.x - Unit * 1.25f, Center.y - Unit),
            ImVec2(Center.x + Unit * 1.25f, Center.y - Unit),
            Color, 2.f);
        Draw->AddLine(
            ImVec2(Center.x - Unit * 0.45f, Center.y - Unit * 1.35f),
            ImVec2(Center.x + Unit * 0.45f, Center.y - Unit * 1.35f),
            Color, 2.f);

        if (Hovered)
            ImGui::SetTooltip("Clear this loadout slot");
        ImGui::PopID();
        return Clicked;
    }

    static void RebuildFilter()
    {
        GRenderer.FilteredItems.clear();
        const std::string Query =
            Lowercase(GRenderer.Search);
        GRenderer.LastQuery = Query;
        GRenderer.LastModeFilter =
            GRenderer.ModeFilter;
        GRenderer.LastRarityFilter =
            GRenderer.RarityFilter;

        for (size_t Index = 0;
             Index < GRenderer.Catalog.Items.size();
             ++Index)
        {
            const auto& Item =
                GRenderer.Catalog.Items[Index];
            const bool MatchesMode =
                GRenderer.ModeFilter == 0 ||
                (GRenderer.ModeFilter == 1 &&
                 Item.Mode ==
                      EItemMode::BattleRoyale) ||
                (GRenderer.ModeFilter == 2 &&
                 Item.Mode ==
                     EItemMode::SaveTheWorld) ||
                (GRenderer.ModeFilter == 3 &&
                 Item.Mode ==
                     EItemMode::Unknown);
            const bool MatchesRarity =
                GRenderer.RarityFilter < 0 ||
                Item.Rarity ==
                    GRenderer.RarityFilter;
            const bool MatchesSearch =
                Query.empty() ||
                Item.SearchText.find(Query) !=
                    std::string::npos;
            if (MatchesMode &&
                MatchesRarity &&
                MatchesSearch)
            {
                GRenderer.FilteredItems.push_back(Index);
            }
        }
    }

    static void RefreshCatalogCache()
    {
        const ULONGLONG Now = GetTickCount64();
        const ULONGLONG RefreshInterval =
            GRenderer.Catalog.Complete ? 750 : 200;
        if (Now - GRenderer.LastCatalogRefreshAt <
            RefreshInterval)
        {
            const std::string Query =
                Lowercase(GRenderer.Search);
            if (Query != GRenderer.LastQuery ||
                GRenderer.ModeFilter !=
                    GRenderer.LastModeFilter ||
                GRenderer.RarityFilter !=
                    GRenderer.LastRarityFilter)
                RebuildFilter();
            return;
        }
        GRenderer.LastCatalogRefreshAt = Now;

        const size_t PreviousItemCount =
            GRenderer.Catalog.Items.size();
        const bool Changed =
            ReadCatalogSnapshot(
                GRenderer.CatalogGeneration,
                GRenderer.Catalog);
        if (Changed)
        {
            GRenderer.CatalogGeneration =
                GRenderer.Catalog.Generation;
            const auto ItemLess =
                [](const FItemMetadata& Left,
                   const FItemMetadata& Right)
                {
                    if (Left.Mode != Right.Mode)
                    {
                        return ModeSortRank(
                                   Left.Mode) <
                            ModeSortRank(
                                Right.Mode);
                    }
                    if (Left.Rarity != Right.Rarity)
                        return Left.Rarity > Right.Rarity;
                    if (Left.Name != Right.Name)
                        return Left.Name < Right.Name;
                    return Left.Id < Right.Id;
                };
            if (PreviousItemCount <=
                GRenderer.Catalog.Items.size())
            {
                auto Middle =
                    GRenderer.Catalog.Items.begin() +
                    PreviousItemCount;
                std::sort(
                    Middle,
                    GRenderer.Catalog.Items.end(),
                    ItemLess);
                if (PreviousItemCount)
                {
                    std::inplace_merge(
                        GRenderer.Catalog.Items.begin(),
                        Middle,
                        GRenderer.Catalog.Items.end(),
                        ItemLess);
                }
            }
            else
            {
                std::sort(
                    GRenderer.Catalog.Items.begin(),
                    GRenderer.Catalog.Items.end(),
                    ItemLess);
            }
            RebuildFilter();
        }

        const std::string Query =
            Lowercase(GRenderer.Search);
        if (Query != GRenderer.LastQuery ||
            GRenderer.ModeFilter !=
                GRenderer.LastModeFilter ||
            GRenderer.RarityFilter !=
                GRenderer.LastRarityFilter)
            RebuildFilter();
    }

    static void RenderItemPicker(
        uintptr_t CurrentTarget)
    {
        const ImVec2 DisplaySize =
            ImGui::GetIO().DisplaySize;
        const float PickerWidth =
            (std::max)(
                1.f,
                (std::min)(
                    760.f,
                    DisplaySize.x - 28.f));
        const float PickerHeight =
            (std::max)(
                1.f,
                (std::min)(
                    620.f,
                    DisplaySize.y - 28.f));
        ImGui::SetNextWindowPos(
            ImVec2(
                DisplaySize.x * 0.5f,
                DisplaySize.y * 0.5f),
            ImGuiCond_Always,
            ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(
            ImVec2(PickerWidth, PickerHeight),
            ImGuiCond_Always);
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowPadding,
            ImVec2(0.f, 0.f));
        ImGui::PushStyleVar(
            ImGuiStyleVar_PopupRounding,
            10.f);
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowBorderSize,
            1.f);
        ImGui::PushStyleColor(
            ImGuiCol_PopupBg,
            ImVec4(0.063f, 0.067f, 0.078f, 1.f));
        ImGui::PushStyleColor(
            ImGuiCol_ModalWindowDimBg,
            ImVec4(0.015f, 0.017f, 0.022f, 0.82f));
        ImGui::PushStyleColor(
            ImGuiCol_Border,
            ImVec4(0.20f, 0.22f, 0.27f, 1.f));
        bool Open = true;
        if (!ImGui::BeginPopupModal(
                "Choose Loadout Item",
                &Open,
                ImGuiWindowFlags_NoTitleBar |
                    ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoScrollbar |
                    ImGuiWindowFlags_NoScrollWithMouse))
        {
            GPickerIconLeaseUntil.store(
                0, std::memory_order_release);
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(3);
            return;
        }
        const auto EndPicker =
            []()
            {
                ImGui::EndPopup();
                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar(3);
            };
        if (!CurrentTarget ||
            GRenderer.PopupTarget != CurrentTarget ||
            GProcessDisabled.load(
                std::memory_order_acquire) ||
            GDisabledWorld.load(
                std::memory_order_acquire) ||
            GFaultPublicationPending.load(
                std::memory_order_acquire) ||
            GRenderer.PopupWorldGeneration !=
                GWorldGeneration.load(
                    std::memory_order_acquire) ||
            GRenderer.PopupCacheEpoch !=
                GCacheEpoch.load(
                    std::memory_order_acquire) ||
            GRenderer.LastSnapshot.TargetToken !=
                CurrentTarget ||
            GRenderer.LastSnapshot.TargetIdentity !=
                GRenderer.PopupTargetIdentity ||
            GRenderer.SelectedSlot < 0 ||
            GRenderer.SelectedSlot >= kSlotCount ||
            !GRenderer.RenderedExactSlotOrder ||
            !AreGuidValuesEqual(
                GRenderer.RenderedSlotGuids[
                    GRenderer.SelectedSlot],
                GRenderer.ExpectedGuid))
        {
            GPickerIconLeaseUntil.store(
                0, std::memory_order_release);
            ImGui::CloseCurrentPopup();
            EndPicker();
            return;
        }

        GCatalogLeaseUntil.store(
            GetTickCount64() + kCatalogLeaseMs,
            std::memory_order_release);
        GPickerIconLeaseUntil.store(
            GetTickCount64() + kCatalogLeaseMs,
            std::memory_order_release);
        const bool CatalogReadyForPopup =
            !GCatalogResetRequested.load(
                std::memory_order_acquire);
        if (CatalogReadyForPopup)
        {
            RefreshCatalogCache();
        }
        else
        {
            GRenderer.Catalog = {};
            GRenderer.FilteredItems.clear();
            GRenderer.CatalogGeneration = 0;
        }

        const ImVec2 WindowPos =
            ImGui::GetWindowPos();
        const ImVec2 WindowSize =
            ImGui::GetWindowSize();
        ImDrawList* WindowDraw =
            ImGui::GetWindowDrawList();
        const float HeaderHeight = 58.f;
        const float BodyPadding = 16.f;
        const ImU32 HeaderColor =
            IM_COL32(22, 24, 30, 255);
        const ImU32 AccentColor =
            IM_COL32(191, 207, 255, 255);
        WindowDraw->AddRectFilled(
            WindowPos,
            ImVec2(
                WindowPos.x + WindowSize.x,
                WindowPos.y + HeaderHeight),
            HeaderColor,
            10.f);
        WindowDraw->AddRectFilled(
            ImVec2(
                WindowPos.x,
                WindowPos.y + HeaderHeight - 12.f),
            ImVec2(
                WindowPos.x + WindowSize.x,
                WindowPos.y + HeaderHeight),
            HeaderColor);
        WindowDraw->AddRectFilled(
            WindowPos,
            ImVec2(
                WindowPos.x + 3.f,
                WindowPos.y + HeaderHeight),
            AccentColor,
            10.f);
        WindowDraw->AddLine(
            ImVec2(
                WindowPos.x,
                WindowPos.y + HeaderHeight),
            ImVec2(
                WindowPos.x + WindowSize.x,
                WindowPos.y + HeaderHeight),
            IM_COL32(49, 52, 61, 255),
            1.f);

        ImGui::SetCursorPos(
            ImVec2(18.f, 10.f));
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            ImVec4(0.75f, 0.82f, 1.f, 1.f));
        ImGui::TextUnformatted(
            "PLAYER LOADOUT");
        ImGui::PopStyleColor();
        ImGui::SetCursorPos(
            ImVec2(18.f, 31.f));
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            ImVec4(0.52f, 0.55f, 0.62f, 1.f));
        ImGui::Text(
            "Choose an item for slot %d",
            GRenderer.SelectedSlot + 1);
        ImGui::PopStyleColor();

        ImGui::SetCursorPos(
            ImVec2(WindowSize.x - 45.f, 13.f));
        ImGui::PushStyleVar(
            ImGuiStyleVar_FrameRounding,
            6.f);
        ImGui::PushStyleColor(
            ImGuiCol_Button,
            ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonHovered,
            ImVec4(0.22f, 0.24f, 0.29f, 1.f));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonActive,
            ImVec4(0.28f, 0.30f, 0.36f, 1.f));
        const bool ClosePicker =
            ImGui::Button(
                "X##close-loadout-picker",
                ImVec2(30.f, 30.f));
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        if (ClosePicker)
        {
            GPickerIconLeaseUntil.store(
                0, std::memory_order_release);
            ImGui::CloseCurrentPopup();
            EndPicker();
            return;
        }

        const float ContentWidth =
            (std::max)(
                1.f,
                WindowSize.x - BodyPadding * 2.f);
        ImGui::SetCursorPos(
            ImVec2(
                BodyPadding,
                HeaderHeight + 14.f));
        ImGui::PushStyleVar(
            ImGuiStyleVar_FrameRounding,
            7.f);
        ImGui::PushStyleVar(
            ImGuiStyleVar_FramePadding,
            ImVec2(10.f, 9.f));
        ImGui::PushStyleColor(
            ImGuiCol_FrameBg,
            ImVec4(0.087f, 0.092f, 0.106f, 1.f));
        ImGui::PushStyleColor(
            ImGuiCol_FrameBgHovered,
            ImVec4(0.105f, 0.112f, 0.130f, 1.f));
        ImGui::PushStyleColor(
            ImGuiCol_FrameBgActive,
            ImVec4(0.116f, 0.124f, 0.145f, 1.f));
        ImGui::SetNextItemWidth(ContentWidth);
        ImGui::InputTextWithHint(
            "##loadout-search",
            "Search items by name or ID...",
            GRenderer.Search,
            sizeof(GRenderer.Search));
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);

        constexpr const char* ModeLabels[] = {
            "All##mode-all",
            "BR##mode-br",
            "STW##mode-stw",
            "Other##mode-other"
        };
        constexpr const char* ModeHints[] = {
            "Show every classified and unclassified item",
            "Battle Royale items",
            "Save the World items",
            "Shared or unclassified items"
        };
        constexpr float ModeWidths[] = {
            48.f, 46.f, 50.f, 64.f
        };
        ImGui::SetCursorPosX(BodyPadding);
        ImGui::Dummy(ImVec2(0.f, 2.f));
        ImGui::SetCursorPosX(BodyPadding);
        ImGui::PushStyleVar(
            ImGuiStyleVar_FrameRounding,
            6.f);
        ImGui::PushStyleVar(
            ImGuiStyleVar_FramePadding,
            ImVec2(8.f, 6.f));
        for (int Mode = 0; Mode < 4; ++Mode)
        {
            if (Mode > 0)
                ImGui::SameLine(0.f, 6.f);
            const bool Selected =
                GRenderer.ModeFilter == Mode;
            ImGui::PushStyleColor(
                ImGuiCol_Button,
                Selected
                    ? ImVec4(
                        0.32f, 0.37f, 0.50f, 1.f)
                    : ImVec4(
                        0.095f, 0.101f, 0.116f, 1.f));
            ImGui::PushStyleColor(
                ImGuiCol_ButtonHovered,
                Selected
                    ? ImVec4(
                        0.38f, 0.43f, 0.57f, 1.f)
                    : ImVec4(
                        0.14f, 0.15f, 0.18f, 1.f));
            ImGui::PushStyleColor(
                ImGuiCol_ButtonActive,
                ImVec4(
                    0.42f, 0.47f, 0.61f, 1.f));
            ImGui::PushStyleColor(
                ImGuiCol_Text,
                Selected
                    ? ImVec4(
                        0.88f, 0.91f, 1.f, 1.f)
                    : ImVec4(
                        0.68f, 0.70f, 0.76f, 1.f));
            if (ImGui::Button(
                    ModeLabels[Mode],
                    ImVec2(ModeWidths[Mode], 30.f)))
            {
                GRenderer.ModeFilter = Mode;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "%s", ModeHints[Mode]);
            ImGui::PopStyleColor(4);
        }
        ImGui::PopStyleVar(2);

        const char* RarityPreview =
            GRenderer.RarityFilter < 0
                ? "Any rarity"
                : RarityName(
                    GRenderer.RarityFilter);
        const float RarityWidth = 148.f;
        ImGui::SameLine();
        ImGui::SetCursorPosX(
            BodyPadding + ContentWidth -
                RarityWidth);
        ImGui::SetNextItemWidth(
            RarityWidth);
        ImGui::PushStyleVar(
            ImGuiStyleVar_FrameRounding,
            6.f);
        if (ImGui::BeginCombo(
                "##loadout-rarity",
                RarityPreview))
        {
            if (ImGui::Selectable(
                    "All rarities",
                    GRenderer.RarityFilter < 0))
            {
                GRenderer.RarityFilter = -1;
            }
            for (int Rarity = 0;
                 Rarity <= 9;
                 ++Rarity)
            {
                ImGui::PushID(Rarity);
                if (ImGui::Selectable(
                        RarityName(Rarity),
                        GRenderer.RarityFilter ==
                            Rarity))
                {
                    GRenderer.RarityFilter =
                        Rarity;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar();

        const int Scanned =
            GRenderer.Catalog.ScannedObjects;
        const int Total =
            GRenderer.Catalog.TotalObjects;
        if (!GRenderer.Catalog.Complete)
        {
            const float Fraction =
                Total > 0
                    ? (std::clamp)(
                        static_cast<float>(Scanned) /
                            Total,
                        0.f, 1.f)
                    : 0.f;
            char Overlay[96];
            snprintf(
                Overlay, sizeof(Overlay),
                "Indexing carryable items  |  %d%%",
                static_cast<int>(Fraction * 100.f));
            ImGui::SetCursorPosX(
                BodyPadding);
            ImGui::PushStyleColor(
                ImGuiCol_Text,
                ImVec4(0.48f, 0.51f, 0.58f, 1.f));
            ImGui::TextUnformatted(Overlay);
            ImGui::PopStyleColor();
            ImGui::SetCursorPosX(
                BodyPadding);
            ImGui::PushStyleColor(
                ImGuiCol_PlotHistogram,
                ImVec4(0.55f, 0.62f, 0.82f, 1.f));
            ImGui::PushStyleColor(
                ImGuiCol_FrameBg,
                ImVec4(0.10f, 0.105f, 0.12f, 1.f));
            ImGui::ProgressBar(
                Fraction,
                ImVec2(ContentWidth, 4.f),
                "");
            ImGui::PopStyleColor(2);
        }
        else
        {
            ImGui::SetCursorPosX(
                BodyPadding);
            ImGui::PushStyleColor(
                ImGuiCol_Text,
                ImVec4(0.48f, 0.51f, 0.58f, 1.f));
            ImGui::TextDisabled(
                "%zu results  |  %zu carryable items indexed",
                GRenderer.FilteredItems.size(),
                GRenderer.Catalog.Items.size());
            ImGui::PopStyleColor();
        }

        const float FooterHeight =
            ImGui::GetTextLineHeight() + 14.f;
        const float GridHeight =
            (std::max)(
                1.f,
                ImGui::GetContentRegionAvail().y -
                    FooterHeight - 6.f);
        ImGui::SetCursorPosX(
            BodyPadding);
        ImGui::PushStyleColor(
            ImGuiCol_ChildBg,
            ImVec4(0.047f, 0.050f, 0.059f, 1.f));
        ImGui::PushStyleVar(
            ImGuiStyleVar_ChildRounding,
            8.f);
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowPadding,
            ImVec2(6.f, 8.f));
        ImGui::BeginChild(
            "##loadout-item-grid",
            ImVec2(ContentWidth, GridHeight),
            false,
            ImGuiWindowFlags_NoNavFocus);
        {
            const ImVec2 GridPos =
                ImGui::GetWindowPos();
            const ImVec2 GridSize =
                ImGui::GetWindowSize();
            ImGui::GetWindowDrawList()->AddRect(
                ImVec2(
                    GridPos.x + 0.5f,
                    GridPos.y + 0.5f),
                ImVec2(
                    GridPos.x + GridSize.x - 0.5f,
                    GridPos.y + GridSize.y - 0.5f),
                IM_COL32(51, 55, 65, 255),
                8.f,
                0,
                1.f);
        }

        const float Spacing = 8.f;
        const float Available =
            ImGui::GetContentRegionAvail().x;
        const int Columns =
            (std::clamp)(
                static_cast<int>(
                    (Available + Spacing) /
                    (96.f + Spacing)),
                1,
                5);
        const float RawCardSide =
            (Available -
             Spacing * (Columns - 1)) /
            Columns;
        const float CardSide =
            (std::max)(1.f, RawCardSide);
        const float GridWidth =
            CardSide * Columns +
            Spacing * (Columns - 1);
        const int TotalCards =
            (CatalogReadyForPopup
                ? static_cast<int>(
                    GRenderer.FilteredItems.size())
                : 0) +
            1;
        const int Rows =
            (TotalCards + Columns - 1) / Columns;
        const float RowHeight =
            CardSide + Spacing;
        const float StartX =
            ImGui::GetCursorPosX();
        const float StartY =
            ImGui::GetCursorPosY();

        ImGuiListClipper Clipper;
        Clipper.Begin(Rows, RowHeight);
        while (Clipper.Step())
        {
            for (int Row = Clipper.DisplayStart;
                 Row < Clipper.DisplayEnd;
                 ++Row)
            {
                for (int Column = 0;
                     Column < Columns;
                     ++Column)
                {
                    const int CardIndex =
                        Row * Columns + Column;
                    if (CardIndex >= TotalCards)
                        break;
                    ImGui::SetCursorPos(
                        ImVec2(
                            StartX +
                                Column *
                                    (CardSide + Spacing),
                            StartY +
                                Row * RowHeight));

                    bool Selected = false;
                    uintptr_t ItemToken = 0;
                    uint64_t ItemIdentity = 0;
                    if (CardIndex == 0)
                    {
                        Selected = DrawClearCard(
                            ImVec2(
                                CardSide,
                                CardSide));
                    }
                    else
                    {
                        const size_t CatalogIndex =
                            GRenderer.FilteredItems[
                                CardIndex - 1];
                        const auto& Item =
                            GRenderer.Catalog.Items[
                                CatalogIndex];
                        ItemToken = Item.Token;
                        ItemIdentity =
                            Item.ObjectIdentity;
                        char Id[48];
                        snprintf(
                            Id, sizeof(Id),
                            "catalog-%p",
                            reinterpret_cast<void*>(
                                Item.Token));
                        Selected = DrawItemCard(
                            Id,
                            &Item,
                            ImVec2(
                                CardSide,
                                CardSide),
                            false,
                            false,
                            Item.Token ==
                                GRenderer.SelectedItemToken,
                            true,
                            true);
                    }

                    if (Selected)
                    {
                        const bool Clear =
                            CardIndex == 0;
                        if (SubmitAction(
                                GRenderer.PopupTarget,
                                GRenderer.PopupWorldGeneration,
                                GRenderer.PopupTargetIdentity,
                                GRenderer.SelectedSlot,
                                Clear,
                                ItemToken,
                                ItemIdentity,
                                Clear
                                    ? std::string()
                                    : GRenderer.Catalog.Items[
                                        GRenderer.FilteredItems[
                                            CardIndex - 1]]
                                        .Id,
                                GRenderer.ExpectedGuid))
                        {
                            GPickerIconLeaseUntil.store(
                                0,
                                std::memory_order_release);
                            ImGui::CloseCurrentPopup();
                        }
                    }
                }
            }
        }
        Clipper.End();
        ImGui::SetCursorPos(
            ImVec2(
                StartX,
                StartY + Rows * RowHeight));
        ImGui::Dummy(
            ImVec2(GridWidth, 1.f));
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();

        ImGui::SetCursorPos(
            ImVec2(
                BodyPadding,
                WindowSize.y -
                    FooterHeight + 4.f));
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            ImVec4(0.45f, 0.48f, 0.55f, 1.f));
        ImGui::TextUnformatted(
            "Hover for details  |  Click to equip");
        ImGui::PopStyleColor();

        EndPicker();
    }

    static void RenderManualItemEditor(
        uintptr_t CurrentTarget)
    {
        const ImVec2 DisplaySize =
            ImGui::GetIO().DisplaySize;
        const float ModalWidth =
            (std::max)(
                1.f,
                (std::min)(440.f, DisplaySize.x - 28.f));
        ImGui::SetNextWindowPos(
            ImVec2(
                DisplaySize.x * 0.5f,
                DisplaySize.y * 0.5f),
            ImGuiCond_Always,
            ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(
            ImVec2(ModalWidth, 158.f),
            ImGuiCond_Always);
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowPadding,
            ImVec2(0.f, 0.f));
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowRounding, 14.f);
        ImGui::PushStyleVar(
            ImGuiStyleVar_WindowBorderSize, 1.f);
        ImGui::PushStyleColor(
            ImGuiCol_PopupBg,
            ImVec4(0.063f, 0.067f, 0.078f, 1.f));
        ImGui::PushStyleColor(
            ImGuiCol_ModalWindowDimBg,
            ImVec4(0.015f, 0.017f, 0.022f, 0.80f));
        ImGui::PushStyleColor(
            ImGuiCol_Border,
            ImVec4(0.20f, 0.22f, 0.27f, 1.f));

        if (!ImGui::BeginPopupModal(
                "Loadout Item",
                nullptr,
                ImGuiWindowFlags_NoTitleBar |
                    ImGuiWindowFlags_NoResize |
                    ImGuiWindowFlags_NoSavedSettings |
                    ImGuiWindowFlags_NoScrollbar |
                    ImGuiWindowFlags_NoScrollWithMouse))
        {
            ImGui::PopStyleColor(3);
            ImGui::PopStyleVar(3);
            return;
        }

        const auto EndEditor =
            []()
            {
                ImGui::EndPopup();
                ImGui::PopStyleColor(3);
                ImGui::PopStyleVar(3);
            };
        const bool ValidSlot =
            GRenderer.SelectedSlot >= 0 &&
            GRenderer.SelectedSlot < kSlotCount;
        bool SelectionMatches = false;
        if (ValidSlot)
        {
            const auto& CurrentSlot =
                GRenderer.LastSnapshot.Slots[
                    GRenderer.SelectedSlot];
            const bool ExpectedOccupied =
                !GRenderer.ExpectedGuid.IsZero();
            SelectionMatches =
                CurrentSlot.Occupied == ExpectedOccupied &&
                AreGuidValuesEqual(
                    GRenderer.RenderedSlotGuids[
                        GRenderer.SelectedSlot],
                    GRenderer.ExpectedGuid);
        }
        const bool ValidSelection =
            CurrentTarget &&
            GRenderer.PopupTarget == CurrentTarget &&
            !GProcessDisabled.load(
                std::memory_order_acquire) &&
            !GDisabledWorld.load(
                std::memory_order_acquire) &&
            !GFaultPublicationPending.load(
                std::memory_order_acquire) &&
            GRenderer.PopupWorldGeneration ==
                GWorldGeneration.load(
                    std::memory_order_acquire) &&
            GRenderer.PopupCacheEpoch ==
                GCacheEpoch.load(
                    std::memory_order_acquire) &&
            GRenderer.LastSnapshot.TargetToken ==
                CurrentTarget &&
            GRenderer.LastSnapshot.TargetIdentity ==
                GRenderer.PopupTargetIdentity &&
            ValidSlot &&
            GRenderer.RenderedExactSlotOrder &&
            SelectionMatches;
        if (!ValidSelection)
        {
            GRenderer.PopupTarget = 0;
            ImGui::CloseCurrentPopup();
            EndEditor();
            return;
        }

        const auto& Selected =
            GRenderer.LastSnapshot.Slots[
                GRenderer.SelectedSlot];
        const bool Occupied = Selected.Occupied;
        const ImVec2 WindowSize =
            ImGui::GetWindowSize();
        const float BodyPadding = 16.f;

        ImGui::SetCursorPos(
            ImVec2(BodyPadding, 14.f));
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            ImVec4(0.78f, 0.82f, 0.91f, 1.f));
        ImGui::Text(
            "Slot %d", GRenderer.SelectedSlot + 1);
        ImGui::PopStyleColor();

        ImGui::SetCursorPos(
            ImVec2(WindowSize.x - 42.f, 8.f));
        ImGui::PushStyleVar(
            ImGuiStyleVar_FrameRounding, 8.f);
        ImGui::PushStyleColor(
            ImGuiCol_Button,
            ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonHovered,
            ImVec4(0.18f, 0.19f, 0.23f, 1.f));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonActive,
            ImVec4(0.24f, 0.25f, 0.30f, 1.f));
        const bool CloseEditor =
            ImGui::Button(
                "X##close-loadout-item",
                ImVec2(30.f, 30.f));
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        if (CloseEditor)
        {
            GRenderer.PopupTarget = 0;
            ImGui::CloseCurrentPopup();
            EndEditor();
            return;
        }

        ImGui::SetCursorPos(
            ImVec2(BodyPadding, 51.f));
        ImGui::PushStyleVar(
            ImGuiStyleVar_FrameRounding, 8.f);
        ImGui::PushStyleVar(
            ImGuiStyleVar_FramePadding,
            ImVec2(10.f, 8.f));
        ImGui::PushStyleColor(
            ImGuiCol_FrameBg,
            ImVec4(0.087f, 0.092f, 0.106f, 1.f));
        ImGui::PushStyleColor(
            ImGuiCol_FrameBgHovered,
            ImVec4(0.11f, 0.12f, 0.14f, 1.f));
        ImGui::PushStyleColor(
            ImGuiCol_FrameBgActive,
            ImVec4(0.13f, 0.14f, 0.17f, 1.f));
        ImGui::SetNextItemWidth(
            WindowSize.x - BodyPadding * 2.f);
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        const bool SubmitFromEnter =
            ImGui::InputTextWithHint(
            "##manual-loadout-item-id",
            "Item ID or asset path",
            GRenderer.Search,
            IM_ARRAYSIZE(GRenderer.Search),
            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();

        ImGui::SetCursorPos(
            ImVec2(BodyPadding, 105.f));
        const float Gap = 8.f;
        const float ContentWidth =
            (std::max)(
                1.f,
                WindowSize.x - BodyPadding * 2.f);
        const float PrimaryWidth = Occupied
            ? (std::max)(
                  1.f, (ContentWidth - Gap) * 0.5f)
            : ContentWidth;
        const bool Pending =
            GRenderer.LastActionPending;
        const bool CanSubmit =
            !Pending && GRenderer.Search[0] != '\0';
        ImGui::BeginDisabled(!CanSubmit);
        ImGui::PushStyleColor(
            ImGuiCol_Button,
            ImVec4(0.29f, 0.36f, 0.57f, 1.f));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonHovered,
            ImVec4(0.36f, 0.44f, 0.68f, 1.f));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonActive,
            ImVec4(0.25f, 0.31f, 0.50f, 1.f));
        const bool PrimaryClicked =
            ImGui::Button(
                Occupied ? "Replace" : "Add",
                ImVec2(PrimaryWidth, 34.f));
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        bool DeleteClicked = false;
        if (Occupied)
        {
            ImGui::SameLine(0.f, Gap);
            ImGui::BeginDisabled(Pending);
            ImGui::PushStyleColor(
                ImGuiCol_Button,
                ImVec4(0.46f, 0.16f, 0.18f, 1.f));
            ImGui::PushStyleColor(
                ImGuiCol_ButtonHovered,
                ImVec4(0.60f, 0.20f, 0.23f, 1.f));
            ImGui::PushStyleColor(
                ImGuiCol_ButtonActive,
                ImVec4(0.39f, 0.13f, 0.15f, 1.f));
            DeleteClicked =
                ImGui::Button(
                    "Delete",
                    ImVec2(PrimaryWidth, 34.f));
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();
        }
        ImGui::PopStyleVar();

        bool Submitted = false;
        if ((PrimaryClicked || SubmitFromEnter) &&
            CanSubmit)
        {
            Submitted = SubmitAction(
                GRenderer.PopupTarget,
                GRenderer.PopupWorldGeneration,
                GRenderer.PopupTargetIdentity,
                GRenderer.SelectedSlot,
                false,
                0,
                0,
                std::string(GRenderer.Search),
                GRenderer.ExpectedGuid,
                true);
        }
        else if (Occupied && DeleteClicked && !Pending)
        {
            Submitted = SubmitAction(
                GRenderer.PopupTarget,
                GRenderer.PopupWorldGeneration,
                GRenderer.PopupTargetIdentity,
                GRenderer.SelectedSlot,
                true,
                0,
                0,
                {},
                GRenderer.ExpectedGuid,
                true);
        }
        if (Submitted)
        {
            GRenderer.PopupTarget = 0;
            ImGui::CloseCurrentPopup();
        }
        EndEditor();
    }

    static void GameThreadTickPipelineUnsafe()
    {
        uintptr_t WorldToken = 0;
        uint64_t WorldIdentity = 0;
        if (!TryReadWorldContext(
                &WorldToken, &WorldIdentity))
        {
            // Do not touch shared STL state after a world-context AV. The
            // renderer observes this atomic latch and shows a disabled card.
            GProcessDisabled.store(
                true, std::memory_order_release);
            return;
        }

        // Compare the stable object index+serial identity before consulting
        // the raw disabled pointer, so a recycled UWorld address cannot inherit
        // a previous world's quarantine.
        if (!TryResetForWorld(WorldIdentity))
            return;

        if (!TryPublishSubsystemFault())
            return;

        const uintptr_t DisabledWorld =
            GDisabledWorld.load(
                std::memory_order_acquire);
        if (DisabledWorld)
        {
            if (DisabledWorld == WorldToken ||
                (!WorldToken && DisabledWorld == 1))
            {
                return;
            }

            GDisabledWorld.store(
                0, std::memory_order_release);
        }

        GRecoveryFaultRequested = false;
        GameThreadTickUnsafe();
        if (GRecoveryFaultRequested)
            PublishSubsystemFault(WorldToken);
    }
}

FPreviewTextureLoadResult ResolveOrRequestPreviewTexture(
    const void* Owner,
    const void* SoftReference,
    std::uint32_t SoftReferenceSize) noexcept
{
    FPreviewTextureLoadResult Result{
        nullptr,
        EPreviewTextureLoadState::Unavailable,
        0
    };
    if (GProcessDisabled.load(std::memory_order_acquire))
        return Result;
    if (GGameTickActive.test_and_set(std::memory_order_acquire))
    {
        Result.State = EPreviewTextureLoadState::Pending;
        Result.RetryAfterMs = 16;
        return Result;
    }

    bool Completed = false;
    __try
    {
        uintptr_t WorldToken = 0;
        uint64_t WorldIdentity = 0;
        if (TryReadWorldContext(
                &WorldToken,
                &WorldIdentity) &&
            TryResetForWorld(WorldIdentity))
        {
            Result = ResolveOrRequestPreviewTextureUnsafe(
                Owner,
                SoftReference,
                SoftReferenceSize);
        }
        else
        {
            Result.State = EPreviewTextureLoadState::Pending;
            Result.RetryAfterMs = 16;
        }
        Completed = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Completed = false;
    }

    if (!Completed)
    {
        // This API mutates the same request containers as GameThreadTick(). If
        // an SEH interrupted that mutation, quarantine the optional subsystem
        // rather than touching potentially inconsistent STL state again.
        GProcessDisabled.store(true, std::memory_order_release);
        GDisabledWorld.store(1, std::memory_order_release);
        GFaultPublicationPending.store(true, std::memory_order_release);
        GAsyncIconLoadingDisabled = true;
        Result = {
            nullptr,
            EPreviewTextureLoadState::Unavailable,
            0
        };
    }
    GGameTickActive.clear(std::memory_order_release);
    return Result;
}

FSoftObjectLoadResult ResolveOrRequestSoftObject(
    const void* Owner,
    const void* SoftReference,
    std::uint32_t SoftReferenceSize,
    const UClass* ExpectedClass) noexcept
{
    FSoftObjectLoadResult Result{
        nullptr,
        EPreviewTextureLoadState::Unavailable,
        0
    };
    if (GProcessDisabled.load(std::memory_order_acquire))
        return Result;
    if (GGameTickActive.test_and_set(std::memory_order_acquire))
    {
        Result.State = EPreviewTextureLoadState::Pending;
        Result.RetryAfterMs = 16;
        return Result;
    }

    bool Completed = false;
    __try
    {
        uintptr_t WorldToken = 0;
        uint64_t WorldIdentity = 0;
        if (TryReadWorldContext(
                &WorldToken,
                &WorldIdentity) &&
            TryResetForWorld(WorldIdentity))
        {
            Result = ResolveOrRequestSoftObjectUnsafe(
                Owner,
                SoftReference,
                SoftReferenceSize,
                ExpectedClass);
        }
        else
        {
            Result.State = EPreviewTextureLoadState::Pending;
            Result.RetryAfterMs = 16;
        }
        Completed = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Completed = false;
    }

    if (!Completed)
    {
        GProcessDisabled.store(true, std::memory_order_release);
        GDisabledWorld.store(1, std::memory_order_release);
        GFaultPublicationPending.store(true, std::memory_order_release);
        GAsyncIconLoadingDisabled = true;
        Result = {
            nullptr,
            EPreviewTextureLoadState::Unavailable,
            0
        };
    }
    GGameTickActive.clear(std::memory_order_release);
    return Result;
}

void GameThreadTick()
{
    if (GProcessDisabled.load(
            std::memory_order_acquire))
    {
        return;
    }
    if (GGameTickActive.test_and_set(
            std::memory_order_acquire))
    {
        return;
    }

    bool Completed = false;
    __try
    {
        GameThreadTickPipelineUnsafe();
        Completed = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Completed = false;
    }

    if (!Completed)
    {
        // An SEH can bypass C++ destructors and leave a try-lock owned or an STL
        // mutation incomplete. Permanently stop only this optional subsystem;
        // never touch its shared containers again in this process.
        GProcessDisabled.store(
            true, std::memory_order_release);
        GDisabledWorld.store(
            1, std::memory_order_release);
        GFaultPublicationPending.store(
            true, std::memory_order_release);
    }
    GGameTickActive.clear(
        std::memory_order_release);
}

void Render(
    AFortPlayerControllerAthena* PlayerController,
    float Width,
    ID3D11Device* Device)
{
    SyncRendererCacheEpoch();
    const uintptr_t TargetToken =
        reinterpret_cast<uintptr_t>(
            PlayerController);
    GRenderer.RenderedTarget = TargetToken;
    GRenderer.RenderedFrame = ImGui::GetFrameCount();
    const ULONGLONG Now = GetTickCount64();
    GRequestedTarget.store(
        TargetToken, std::memory_order_release);
    GInspectLeaseUntil.store(
        Now + kInspectLeaseMs,
        std::memory_order_release);

    FLoadoutSnapshot Snapshot;
    FActionResult ActionResult;
    bool ActionPending = false;
    uintptr_t PendingTarget = 0;
    int PendingSlot = -1;
    const bool SnapshotRead =
        ReadLoadoutSnapshot(
            Snapshot,
            ActionResult,
            ActionPending,
            PendingTarget,
            PendingSlot);
    if (SnapshotRead)
    {
        GRenderer.LastSnapshot = Snapshot;
        GRenderer.LastActionResult = ActionResult;
        GRenderer.LastActionPending =
            ActionPending;
        GRenderer.LastPendingTarget =
            PendingTarget;
        GRenderer.LastPendingSlot = PendingSlot;
    }
    else
    {
        // The game thread and renderer deliberately use non-blocking locks.
        // Reuse the last complete render snapshot on a transient miss so slot
        // cards and an open picker never flash empty or close spuriously.
        Snapshot = GRenderer.LastSnapshot;
        ActionResult =
            GRenderer.LastActionResult;
        ActionPending =
            GRenderer.LastActionPending;
        PendingTarget =
            GRenderer.LastPendingTarget;
        PendingSlot =
            GRenderer.LastPendingSlot;
    }

    if (ActionResult.HasResult &&
        ActionResult.Id != GRenderer.LastResultId)
    {
        GRenderer.LastResultId =
            ActionResult.Id;
        GRenderer.ResultShownAt = Now;
    }

    const bool SubsystemDisabled =
        GProcessDisabled.load(
            std::memory_order_acquire) ||
        GDisabledWorld.load(
            std::memory_order_acquire) ||
        GFaultPublicationPending.load(
            std::memory_order_acquire);
    const bool Ready =
        !SubsystemDisabled &&
        Snapshot.TargetToken == TargetToken &&
        Snapshot.Availability ==
            ELoadoutAvailability::Ready;
    const bool Editable =
        Ready && Snapshot.CanEditSlots;
    if (SnapshotRead)
    {
        GRenderer.RenderedExactSlotOrder =
            Editable;
        GRenderer.RenderedSlotGuids = {};
        if (Editable)
        {
            for (int Slot = 0;
                 Slot < kSlotCount;
                 ++Slot)
            {
                GRenderer.RenderedSlotGuids[Slot] =
                    Snapshot.Slots[Slot].Guid;
            }
        }
    }
    if (SnapshotRead)
    {
        GRenderer.PinnedSlotIcons.clear();
        if (Ready)
        {
            for (int Slot = 0;
                 Slot < kSlotCount;
                 ++Slot)
            {
                if (Snapshot.Slots[Slot].Occupied)
                {
                    GRenderer.PinnedSlotIcons[
                        Snapshot.Slots[Slot]
                            .ItemToken] =
                        Snapshot.Slots[Slot]
                            .ItemIdentity;
                }
            }
        }
    }

    // Upload only after the current five slots have been pinned. Catalog
    // thumbnails can never evict a loaded icon that is still in the hotbar.
    ConsumeAndUploadIcons(Device);

    if (Ready)
    {
        // Queue only the five visible slots from the player page. Catalog
        // thumbnails are requested lazily by the clipped picker rows, so they
        // cannot displace or delay the live hotbar lane.
        for (int Slot = 0;
             Slot < kSlotCount;
             ++Slot)
        {
            if (!Snapshot.Slots[Slot].Occupied)
                continue;
            int IconWidth = 0;
            int IconHeight = 0;
            GetOrRequestIcon(
                Snapshot.Slots[Slot].ItemToken,
                Snapshot.Slots[Slot].ItemIdentity,
                IconWidth,
                IconHeight,
                false);
        }
    }
    const float Spacing = 7.f;
    const float Available =
        (std::min)(
            Width,
            ImGui::GetContentRegionAvail().x);
    const float RawCardSide =
        (Available -
         Spacing * (kSlotCount - 1)) /
        kSlotCount;
        const float CardSide =
            (std::min)(
                112.f,
                (std::max)(
                    1.f,
                    static_cast<float>(
                        std::floor(RawCardSide))));
    const float RowWidth =
        CardSide * kSlotCount +
        Spacing * (kSlotCount - 1);
    ImGui::SetCursorPosX(
        ImGui::GetCursorPosX() +
        (std::max)(
            0.f,
            (Available - RowWidth) * 0.5f));

    for (int Slot = 0;
         Slot < kSlotCount;
         ++Slot)
    {
        if (Slot > 0)
            ImGui::SameLine(0.f, Spacing);

        FItemMetadata Item;
        const bool Occupied =
            Ready &&
            Snapshot.Slots[Slot].Occupied;
        if (Occupied)
        {
            Item.Token =
                Snapshot.Slots[Slot].ItemToken;
            Item.ObjectIdentity =
                Snapshot.Slots[Slot].ItemIdentity;
            Item.Id =
                Snapshot.Slots[Slot].Id;
            Item.Name =
                Snapshot.Slots[Slot].Name;
            Item.Rarity =
                Snapshot.Slots[Slot].Rarity;
            Item.Mode =
                Snapshot.Slots[Slot].Mode;
        }

        char WidgetId[32];
        snprintf(
            WidgetId, sizeof(WidgetId),
            "slot-%d", Slot);
        const bool Pending =
            ActionPending &&
            PendingTarget == TargetToken &&
            PendingSlot == Slot;
        const bool Equipped =
            Occupied &&
            !Snapshot.EquippedGuid.IsZero() &&
            AreGuidValuesEqual(
                Snapshot.Slots[Slot].Guid,
                Snapshot.EquippedGuid);
        if (DrawItemCard(
                WidgetId,
                Occupied ? &Item : nullptr,
                ImVec2(CardSide, CardSide),
                !Occupied,
                Pending,
                Equipped,
                Editable,
                false) &&
            Editable)
        {
            GRenderer.SelectedSlot = Slot;
            GRenderer.SelectedItemToken =
                Occupied
                    ? Snapshot.Slots[Slot].ItemToken
                    : 0;
            GRenderer.ExpectedGuid =
                Occupied
                    ? Snapshot.Slots[Slot].Guid
                    : FGuidValue{};
            GRenderer.PopupTarget = TargetToken;
            GRenderer.PopupWorldGeneration =
                Snapshot.WorldGeneration;
            GRenderer.PopupTargetIdentity =
                Snapshot.TargetIdentity;
            GRenderer.PopupCacheEpoch =
                GCacheEpoch.load(
                    std::memory_order_acquire);
            GRenderer.Search[0] = '\0';
            ImGui::OpenPopup(
                "Loadout Item");
        }
    }

    if (SubsystemDisabled)
    {
        ImGui::TextDisabled(
            GProcessDisabled.load(
                std::memory_order_acquire)
                ? "Loadout editing is disabled for this server session after an internal fault."
                : "Loadout editing is disabled for this match after a safe recovery.");
    }
    else if (Snapshot.TargetToken != TargetToken)
    {
        ImGui::TextDisabled(
            "Loading inventory...");
    }
    else if (Snapshot.Availability ==
             ELoadoutAvailability::Unavailable)
    {
        ImGui::TextDisabled(
            "%s", Snapshot.Message.c_str());
    }
    else if (ActionPending &&
             PendingTarget == TargetToken)
    {
        ImGui::TextDisabled(
            "Applying loadout change...");
    }
    else if (ActionResult.HasResult &&
             ActionResult.TargetToken == TargetToken &&
             Now - GRenderer.ResultShownAt < 5000)
    {
        ImGui::PushStyleColor(
            ImGuiCol_Text,
            ActionResult.Success
                ? ImVec4(0.42f, 0.82f, 0.43f, 1.f)
                : ImVec4(0.94f, 0.43f, 0.45f, 1.f));
        ImGui::TextWrapped(
            "%s", ActionResult.Message.c_str());
        ImGui::PopStyleColor();
    }
    else if (Ready &&
             !Snapshot.HasExactSlotOrder)
    {
        ImGui::TextDisabled(
            Snapshot.CanEditSlots
                ? "Click a slot to add, replace, or delete. Client controls final ordering."
                : "Showing received carryable items; its mutable inventory is not available yet.");
    }
    else if (Ready &&
             !Snapshot.CanEditSlots)
    {
        ImGui::TextDisabled(
            "Showing the player's exact reported slots; its mutable inventory is not available yet.");
    }
    else if (Ready)
    {
        ImGui::TextDisabled(
            "Click a slot to add, replace, or delete.");
    }

}

void RenderPicker()
{
    const uintptr_t CurrentTarget =
        GRenderer.RenderedFrame == ImGui::GetFrameCount()
            ? GRenderer.RenderedTarget
            : 0;
    RenderManualItemEditor(CurrentTarget);
}

void ShutdownRenderer()
{
    ReleaseGpuIcons();
    GRenderer.Device = nullptr;
}
}
