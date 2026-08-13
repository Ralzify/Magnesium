#include "pch.h"
#include "../Public/GUI.h"
#include <d3d11.h>
#include "../../ImGui/imgui.h"
#include "../../ImGui/imgui_stdlib.h"
#include "../../ImGui/imgui_impl_win32.h"
#include "../../ImGui/imgui_impl_dx11.h"
#include "../Public/Calendar.h"
#include "../Public/AutoHosting.h"
#include "../Public/Configuration.h"
#include "../Public/Events.h"
#include "../Public/Misc.h"
#include "../Public/PlayerLoadout.h"
#include "../Support/Public/FaultGuard.h"
#include "../../FortniteGame/Public/BattleRoyaleGamePhaseLogic.h"
#include "../../FortniteGame/Public/FortAthenaMutator.h"
#include "../../FortniteGame/Public/BuildingSMActor.h"
#include "../../FortniteGame/Public/GameplayTagContainer.h"
#include "../../Engine/Public/NetDriver.h"
#include "../../FortniteGame/Public/FortPhysicsPawn.h"
#include "../../FortniteGame/Public/FortVehicleMods.h"
#include "../../FortniteGame/Public/FortPlayerPawnAthena.h"
#include "../../FortniteGame/Public/FortPlayerStateAthena.h"
#include "../../FortniteGame/Public/FortGameStateAthena.h"
#include "../../FortniteGame/Public/FortGameMode.h"
#include "../../Engine/Public/Texture.h"
#include <sstream>
#include <fstream>
#include <string>
#include <atomic>
#include <Windows.h>
#include <Shellapi.h>
#include <chrono>
#include <algorithm>
#include <filesystem>
#include <cmath>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma warning(push)
#pragma warning(disable: 4996) // stb_image_write uses sprintf in the HDR path
#include "stb_image_write.h"
#pragma warning(pop)
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
#include "EmbeddedImage.h"
#include "Icon.h" // ATLAS logo (ported menu branding)

#pragma comment(lib, "d3d11.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

UINT g_ResizeWidth = 0, g_ResizeHeight = 0;

static std::atomic<ULONGLONG> GServerJoinableAtMs{ 0 };
static unsigned int GPreferenceEditorGeneration = 0;

namespace TrickshotManager
{
    void GameThreadTick();
    void RegisterSpawnedActor(
        AActor* Actor,
        AFortPlayerControllerAthena* Controller,
        const std::string& CanonicalClassPath);
}

namespace
{
    SRWLOCK GPlayerNameCacheLock = SRWLOCK_INIT;
    std::atomic_bool GPlayerNameCacheDisabled{ false };
    std::atomic<ULONGLONG> GPlayerNameCacheRetryAtMs{ 0 };
    constexpr ULONGLONG kPlayerNameCacheFaultRetryMs = 5000ULL;
    UWorld* GPlayerNameCacheWorld = nullptr;
    uint64_t GPlayerNameCacheWorldIdentity = 0;
    ULONGLONG GNextPlayerNameRefreshMs = 0;
    std::unordered_map<uint64_t, std::string>
        GPlayerNamesByState;
    std::unordered_map<uint64_t, std::string>
        GPlayerNamesByConnection;

    struct FPlayerCombatStats
    {
        uint64_t WorldIdentity = 0;
        uint64_t ControllerIdentity = 0;
        uint64_t PlayerStateIdentity = 0;
        uint64_t PawnIdentity = 0;
        ULONGLONG CapturedAtMs = 0;
        float Health = 0.f;
        float Shield = 0.f;
        int Kills = 0;
    };

    constexpr ULONGLONG kPlayerCombatMaxAgeMs = 1000ULL;
    std::unordered_map<uint64_t, FPlayerCombatStats>
        GPlayerCombatStatsByState;

    uint64_t GetGuiObjectIdentityGuarded(
        const UObject* Object) noexcept;

    int CountConnectedPlayersForDisplay(UWorld* World)
    {
        if (!GetGuiObjectIdentityGuarded(World) ||
            !World->HasNetDriver() ||
            !GetGuiObjectIdentityGuarded(World->NetDriver))
        {
            return 0;
        }

        auto Driver = static_cast<UNetDriver*>(World->NetDriver);
        if (!Driver->HasClientConnections())
            return 0;

        auto& Connections = Driver->ClientConnections;
        if (Connections.Num() < 0 ||
            Connections.Max() < Connections.Num() ||
            Connections.Max() > 4096 ||
            (Connections.Num() > 0 &&
             (!Connections.Data ||
              !SDK::MemReadable(
                  Connections.Data,
                  static_cast<size_t>(Connections.Num()) *
                      sizeof(UNetConnection*)))))
        {
            return 0;
        }

        std::vector<AFortPlayerControllerAthena*> Players;
        auto AddConnection =
            [&](UNetConnection* Connection)
            {
                if (!GetGuiObjectIdentityGuarded(Connection))
                    return;

                auto PlayerController = Connection->PlayerController;
                if (GetGuiObjectIdentityGuarded(PlayerController) &&
                    std::find(
                        Players.begin(), Players.end(),
                        PlayerController) == Players.end())
                {
                    Players.push_back(PlayerController);
                }

                if (!Connection->HasChildren())
                    return;
                auto& Children = Connection->Children;
                if (Children.Num() < 0 ||
                    Children.Max() < Children.Num() ||
                    Children.Max() > 256 ||
                    (Children.Num() > 0 &&
                     (!Children.Data ||
                      !SDK::MemReadable(
                          Children.Data,
                          static_cast<size_t>(Children.Num()) *
                              sizeof(UNetConnection*)))))
                {
                    return;
                }
                for (int32 ChildIndex = 0;
                     ChildIndex < Children.Num(); ++ChildIndex)
                {
                    auto Child = Children[ChildIndex];
                    if (!GetGuiObjectIdentityGuarded(Child))
                        continue;
                    auto ChildController = Child->PlayerController;
                    if (GetGuiObjectIdentityGuarded(ChildController) &&
                        std::find(
                            Players.begin(), Players.end(),
                            ChildController) == Players.end())
                    {
                        Players.push_back(ChildController);
                    }
                }
            };

        for (int32 ConnectionIndex = 0;
             ConnectionIndex < Connections.Num(); ++ConnectionIndex)
        {
            AddConnection(Connections[ConnectionIndex]);
        }
        return static_cast<int>(Players.size());
    }

    class FPlayerNameCacheSharedLock final
    {
    public:
        FPlayerNameCacheSharedLock() noexcept
            : Acquired(
                TryAcquireSRWLockShared(
                    &GPlayerNameCacheLock) != FALSE)
        {
        }

        ~FPlayerNameCacheSharedLock()
        {
            if (Acquired)
                ReleaseSRWLockShared(&GPlayerNameCacheLock);
        }

        FPlayerNameCacheSharedLock(
            const FPlayerNameCacheSharedLock&) = delete;
        FPlayerNameCacheSharedLock& operator=(
            const FPlayerNameCacheSharedLock&) = delete;

        bool owns_lock() const noexcept
        {
            return Acquired;
        }

    private:
        bool Acquired = false;
    };

    class FPlayerNameCacheExclusiveLock final
    {
    public:
        FPlayerNameCacheExclusiveLock() noexcept
            : Acquired(
                TryAcquireSRWLockExclusive(
                    &GPlayerNameCacheLock) != FALSE)
        {
        }

        ~FPlayerNameCacheExclusiveLock()
        {
            if (Acquired)
                ReleaseSRWLockExclusive(&GPlayerNameCacheLock);
        }

        FPlayerNameCacheExclusiveLock(
            const FPlayerNameCacheExclusiveLock&) = delete;
        FPlayerNameCacheExclusiveLock& operator=(
            const FPlayerNameCacheExclusiveLock&) = delete;

        bool owns_lock() const noexcept
        {
            return Acquired;
        }

    private:
        bool Acquired = false;
    };

    void DisablePlayerNameCache() noexcept
    {
        GPlayerNameCacheRetryAtMs.store(
            GetTickCount64() + kPlayerNameCacheFaultRetryMs,
            std::memory_order_release);
        if (!GPlayerNameCacheDisabled.exchange(
                true, std::memory_order_acq_rel))
        {
            OutputDebugStringA(
                "[PlayerNames] disabled after a guarded fault\n");
        }
    }

    bool TryReenablePlayerNameCache() noexcept
    {
        if (!GPlayerNameCacheDisabled.load(
                std::memory_order_acquire))
        {
            return true;
        }

        if (GetTickCount64() < GPlayerNameCacheRetryAtMs.load(
                std::memory_order_acquire))
        {
            return false;
        }

        GPlayerNameCacheDisabled.store(
            false, std::memory_order_release);
        GPlayerNameCacheRetryAtMs.store(
            0, std::memory_order_release);
        OutputDebugStringA(
            "[PlayerNames] retrying after guarded fault\n");
        return true;
    }

    bool TryCopyFStringUtf8(
        const FString* Value,
        int32 MaxCharacters,
        std::string& OutValue)
    {
        OutValue.clear();
        if (!Value ||
            !SDK::MemReadable(Value, sizeof(FString)) ||
            !Value->Data ||
            Value->NumElements <= 0 ||
            Value->NumElements > MaxCharacters ||
            Value->MaxElements < Value->NumElements ||
            Value->MaxElements > (1 << 20))
        {
            return false;
        }

        const size_t ReadBytes =
            (size_t)Value->NumElements * sizeof(wchar_t);
        if (!SDK::MemReadable(Value->Data, ReadBytes))
            return false;

        int32 CharacterCount = 0;
        while (CharacterCount < Value->NumElements &&
            Value->Data[CharacterCount] != L'\0')
        {
            const wchar_t Character =
                Value->Data[CharacterCount];
            if (Character < L' ')
                return false;
            CharacterCount++;
        }
        if (CharacterCount <= 0)
            return false;

        const int Utf8Bytes = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            Value->Data,
            CharacterCount,
            nullptr,
            0,
            nullptr,
            nullptr);
        // A connection RequestURL can legitimately be much longer than a
        // player name because older clients append authentication/options.
        // Keep the conversion bounded by the caller's validated UTF-16 limit
        // instead of silently imposing a second 1 KiB cap that made valid
        // URLs fall through to an unreliable PlayerState placeholder.
        const int MaxUtf8Bytes = MaxCharacters * 3;
        if (Utf8Bytes <= 0 || Utf8Bytes > MaxUtf8Bytes)
            return false;

        OutValue.resize((size_t)Utf8Bytes);
        return WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            Value->Data,
            CharacterCount,
            OutValue.data(),
            Utf8Bytes,
            nullptr,
            nullptr) == Utf8Bytes;
    }

    bool TryReadReflectedPlayerName(
        AFortPlayerStateAthena* PlayerState,
        const char* PropertyName,
        std::string& OutName)
    {
        if (!PlayerState || !PropertyName ||
            !SDK::MemReadable(PlayerState, sizeof(UObject)))
        {
            return false;
        }

        auto Property = PlayerState->GetProperty(
            PropertyName, GUESS_PROP_FLAGS(FString));
        if (!Property)
            return false;

        // ElementSize is reliable on the classic reflection layouts.  FN32's
        // restricted metadata encrypts/reorders parts of FProperty, so the
        // exact-name lookup plus the fully validated FString header/data below
        // is the stronger check there.
        if (VersionInfo.FortniteVersion < 32.0 &&
            GetFromOffset<uint32>(
                Property, Offsets::ElementSize) != sizeof(FString))
        {
            return false;
        }

        const uint32 Offset = SDK::DecryptPropOffset(
            GetFromOffset<uint32>(
                Property, Offsets::Offset_Internal));
        if (Offset == UINT32_MAX || Offset >= 0x10000)
            return false;

        auto Value = reinterpret_cast<const FString*>(
            reinterpret_cast<const uint8*>(PlayerState) + Offset);
        return TryCopyFStringUtf8(Value, 512, OutName);
    }

    bool GuardedInvokePlayerNameFunction(
        AFortPlayerStateAthena* PlayerState,
        UFunction* Function,
        void* Parameters) noexcept
    {
        __try
        {
            PlayerState->ProcessEvent(Function, Parameters);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    bool TryReadPlayerNameFunction(
        AFortPlayerStateAthena* PlayerState,
        std::string& OutName)
    {
        OutName.clear();
        if (!PlayerState ||
            !SDK::MemReadable(PlayerState, sizeof(UObject)))
        {
            return false;
        }

        // This is the same displayed-name source the older menu used. Resolve
        // the function dynamically instead of using DEFINE_FUNC's one-shot
        // cache: the earliest supported builds do not expose this UFunction,
        // and a transient early miss must not permanently disable it.
        auto Function = PlayerState->GetFunction("GetPlayerName");
        if (!Function)
            return false;

        // GetPlayerName is stable semantically, but its reflected return offset
        // is not guaranteed to be byte zero across UProperty/FField layouts.
        // Resolve the sole named return and invoke a bounded parameter buffer
        // instead of relying on UObject::Call's iteration-order ABI.
        const auto Parameters = Function->GetParamsNamed();
        uint32 ReturnValueOffset = UINT32_MAX;
        uint32 ReturnValueSize = 0;
        uint64 ReturnValueFlags = 0;
        for (const auto& Parameter : Parameters.NameOffsetMap)
        {
            if (Parameter.Name == "ReturnValue")
            {
                ReturnValueOffset = Parameter.Offset;
                ReturnValueSize = Parameter.ElementSize;
                ReturnValueFlags = Parameter.PropertyFlags;
            }
        }

        constexpr uint64 CPF_Parm = 0x80;
        constexpr uint64 CPF_OutParm = 0x100;
        constexpr uint64 CPF_ReturnParm = 0x400;
        const bool bEncryptedPropertyMetadata =
            VersionInfo.FortniteVersion >= 32.0f;
        const uint32 BufferSize = bEncryptedPropertyMetadata
            ? 0x1000u : Parameters.Size;
        const bool bReturnFits =
            ReturnValueOffset != UINT32_MAX &&
            ReturnValueOffset <= BufferSize &&
            sizeof(FString) <= BufferSize - ReturnValueOffset;
        if (Parameters.NameOffsetMap.size() != 1 ||
            BufferSize == 0 || BufferSize > 0x1000 ||
            !bReturnFits ||
            (!bEncryptedPropertyMetadata &&
             (ReturnValueSize != sizeof(FString) ||
              !(ReturnValueFlags & CPF_Parm) ||
              !(ReturnValueFlags & CPF_OutParm) ||
              !(ReturnValueFlags & CPF_ReturnParm))))
        {
            return false;
        }

        void* Memory = FMemory::Malloc(BufferSize);
        if (!Memory)
            return false;
        memset(Memory, 0, BufferSize);
        const bool bInvoked = GuardedInvokePlayerNameFunction(
            PlayerState, Function, Memory);
        FString Value{};
        if (bInvoked)
        {
            memcpy(
                &Value,
                reinterpret_cast<uint8*>(Memory) + ReturnValueOffset,
                sizeof(Value));
        }
        const bool Copied =
            bInvoked && TryCopyFStringUtf8(&Value, 512, OutName);
        // ProcessEvent constructs the return FString for the caller. The SDK's
        // TArray destructor is intentionally non-owning, so release this one
        // explicitly after copying instead of leaking it every cache refresh.
        if (bInvoked && Value.Data &&
            Value.NumElements >= 0 &&
            Value.MaxElements >= Value.NumElements &&
            Value.MaxElements <= 4096 &&
            SDK::MemReadable(Value.Data, sizeof(wchar_t)))
        {
            Value.Free();
        }
        FMemory::Free(Memory);
        return Copied;
    }

    bool IsGenericPlayerNamePlaceholder(const std::string& Name);

    std::string ResolveAuthoritativePlayerName(
        AFortPlayerStateAthena* PlayerState)
    {
        std::string Name;
        std::string Provisional;
        // Use Fortnite's displayed PlayerState name before any connection URL
        // identity. FN30 can put an opaque launcher/profile alias in the join
        // URL even while GetPlayerName returns the correct in-game label.
        if (TryReadPlayerNameFunction(PlayerState, Name))
        {
            if (!IsGenericPlayerNamePlaceholder(Name))
                return Name;
            Provisional = Name;
        }
        if (TryReadReflectedPlayerName(
                PlayerState, "PlayerNamePrivate", Name))
        {
            if (!IsGenericPlayerNamePlaceholder(Name))
                return Name;
            if (Provisional.empty())
                Provisional = Name;
        }
        // PlayerNamePrivate can legitimately contain the same temporary
        // "Player N" placeholder as GetPlayerName while the public replicated
        // field has already received the canonical label. Probe both fields
        // independently instead of letting a successful placeholder read
        // short-circuit the second capability.
        if (TryReadReflectedPlayerName(
                PlayerState, "PlayerName", Name))
        {
            if (!IsGenericPlayerNamePlaceholder(Name))
                return Name;
            if (Provisional.empty())
                Provisional = Name;
        }
        return Provisional;
    }

    bool TryCopyConnectionPlayerNameUnsafe(
        UNetConnection* Connection,
        char* OutName,
        size_t OutCapacity,
        size_t* OutLength)
    {
        if (!OutName || !OutCapacity || !OutLength)
            return false;
        OutName[0] = '\0';
        *OutLength = 0;

        const std::string Name =
            GUI::GetPlayerNameFromConnection(Connection);
        // A transient engine-generated "Player N" is not an identity. Leaving
        // this refresh unresolved preserves any prior canonical cache entry and
        // lets the next replicated PlayerName update replace the UI fallback.
        if (Name.empty() || IsGenericPlayerNamePlaceholder(Name) ||
            Name.size() >= OutCapacity)
            return false;
        memcpy(OutName, Name.data(), Name.size());
        OutName[Name.size()] = '\0';
        *OutLength = Name.size();
        return true;
    }

    bool TryCopyConnectionPlayerNameGuarded(
        UNetConnection* Connection,
        char* OutName,
        size_t OutCapacity,
        size_t* OutLength) noexcept
    {
        __try
        {
            return TryCopyConnectionPlayerNameUnsafe(
                Connection, OutName, OutCapacity, OutLength);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (OutName && OutCapacity)
                OutName[0] = '\0';
            if (OutLength)
                *OutLength = 0;
            return false;
        }
    }

    bool IsGenericPlayerNamePlaceholder(const std::string& Name)
    {
        static constexpr char Prefix[] = "player";
        if (Name.size() <= std::size(Prefix) - 1)
            return false;
        for (size_t Index = 0; Index < std::size(Prefix) - 1; ++Index)
        {
            char Character = Name[Index];
            if (Character >= 'A' && Character <= 'Z')
                Character = static_cast<char>(Character - 'A' + 'a');
            if (Character != Prefix[Index])
                return false;
        }

        size_t Index = std::size(Prefix) - 1;
        while (Index < Name.size() && Name[Index] == ' ')
            ++Index;
        if (Index == Name.size())
            return false;
        for (; Index < Name.size(); ++Index)
        {
            if (Name[Index] < '0' || Name[Index] > '9')
                return false;
        }
        return true;
    }

    bool TryCopyResolvedPlayerNameUnsafe(
        AFortPlayerStateAthena* PlayerState,
        UNetConnection* Connection,
        char* OutName,
        size_t OutCapacity,
        size_t* OutLength)
    {
        if (!OutName || OutCapacity == 0 || !OutLength)
            return false;
        OutName[0] = '\0';
        *OutLength = 0;

        // Prefer the authoritative replicated PlayerState label. RequestURL is
        // only a compatibility fallback: its private connection offset is not
        // stable on UE5 and may be unavailable while profile registration is
        // still settling for a late joiner.
        std::string Name = ResolveAuthoritativePlayerName(PlayerState);
        if (Name.empty() || IsGenericPlayerNamePlaceholder(Name))
        {
            std::array<char, 1025> ConnectionName{};
            size_t ConnectionNameLength = 0;
            TryCopyConnectionPlayerNameGuarded(
                Connection,
                ConnectionName.data(),
                ConnectionName.size(),
                &ConnectionNameLength);
            if (ConnectionNameLength > 0)
            {
                std::string ConnectionLabel(
                    ConnectionName.data(), ConnectionNameLength);
                if (Name.empty() || ConnectionLabel != Name)
                    Name = std::move(ConnectionLabel);
            }
        }
        if (Name.empty() || IsGenericPlayerNamePlaceholder(Name) ||
            Name.size() >= OutCapacity)
            return false;

        memcpy(OutName, Name.data(), Name.size());
        OutName[Name.size()] = '\0';
        *OutLength = Name.size();
        return true;
    }

    bool TryCopyResolvedPlayerNameGuarded(
        AFortPlayerStateAthena* PlayerState,
        UNetConnection* Connection,
        char* OutName,
        size_t OutCapacity,
        size_t* OutLength) noexcept
    {
        __try
        {
            return TryCopyResolvedPlayerNameUnsafe(
                PlayerState,
                Connection,
                OutName,
                OutCapacity,
                OutLength);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (OutName && OutCapacity)
                OutName[0] = '\0';
            if (OutLength)
                *OutLength = 0;
            return false;
        }
    }

    int HexDigitValue(char Character)
    {
        if (Character >= '0' && Character <= '9')
            return Character - '0';
        if (Character >= 'a' && Character <= 'f')
            return Character - 'a' + 10;
        if (Character >= 'A' && Character <= 'F')
            return Character - 'A' + 10;
        return -1;
    }

    std::string DecodeURLPlayerName(const std::string& Value)
    {
        std::string Decoded;
        Decoded.reserve(Value.size());
        for (size_t Index = 0; Index < Value.size(); Index++)
        {
            if (Value[Index] == '+')
            {
                Decoded.push_back(' ');
                continue;
            }
            if (Value[Index] == '%' && Index + 2 < Value.size())
            {
                const int High = HexDigitValue(Value[Index + 1]);
                const int Low = HexDigitValue(Value[Index + 2]);
                if (High >= 0 && Low >= 0)
                {
                    Decoded.push_back(
                        (char)((High << 4) | Low));
                    Index += 2;
                    continue;
                }
            }
            Decoded.push_back(Value[Index]);
        }
        return Decoded;
    }

    bool EqualsAsciiInsensitive(
        const std::string& Value,
        size_t Offset,
        size_t Length,
        const char* Expected)
    {
        if (!Expected || strlen(Expected) != Length)
            return false;
        for (size_t Index = 0; Index < Length; ++Index)
        {
            const unsigned char Left =
                static_cast<unsigned char>(
                    Value[Offset + Index]);
            const unsigned char Right =
                static_cast<unsigned char>(Expected[Index]);
            if (std::tolower(Left) != std::tolower(Right))
                return false;
        }
        return true;
    }

    bool IsPlausiblePlayerName(const std::string& Name)
    {
        if (Name.empty() || Name.size() > 512)
            return false;
        for (const unsigned char Character : Name)
        {
            if (Character < 0x20 || Character == 0x7f)
                return false;
        }
        return MultiByteToWideChar(
                   CP_UTF8,
                   MB_ERR_INVALID_CHARS,
                   Name.data(),
                   static_cast<int>(Name.size()),
                   nullptr,
                   0) > 0;
    }

    bool TryReadExactURLNameOption(
        const std::string& URL,
        const char* Key,
        std::string& Name)
    {
        Name.clear();
        size_t SegmentStart = 0;
        while (SegmentStart <= URL.size())
        {
            const size_t SegmentEnd =
                URL.find_first_of("?&", SegmentStart);
            const size_t BoundedEnd =
                SegmentEnd == std::string::npos
                    ? URL.size()
                    : SegmentEnd;
            const size_t Equals =
                URL.find('=', SegmentStart);
            if (Equals != std::string::npos &&
                Equals < BoundedEnd &&
                EqualsAsciiInsensitive(
                    URL,
                    SegmentStart,
                    Equals - SegmentStart,
                    Key))
            {
                auto Candidate = DecodeURLPlayerName(
                    URL.substr(
                        Equals + 1,
                        BoundedEnd - Equals - 1));
                if (IsPlausiblePlayerName(Candidate))
                {
                    Name = std::move(Candidate);
                    return true;
                }
            }
            if (SegmentEnd == std::string::npos)
                break;
            SegmentStart = SegmentEnd + 1;
        }
        return false;
    }

    bool TryResetPlayerNameCache(UWorld* World)
    {
        const uint64_t WorldIdentity =
            GetGuiObjectIdentityGuarded(World);
        FPlayerNameCacheExclusiveLock Lock;
        if (!Lock.owns_lock())
            return false;

        GPlayerNameCacheWorld = World;
        GPlayerNameCacheWorldIdentity = WorldIdentity;
        GNextPlayerNameRefreshMs = 0;
        GPlayerNamesByState.clear();
        GPlayerNamesByConnection.clear();
        GPlayerCombatStatsByState.clear();
        return true;
    }

    bool TryBeginPlayerNameRefresh(
        UWorld* World,
        ULONGLONG CurrentTimeMs)
    {
        const uint64_t WorldIdentity =
            GetGuiObjectIdentityGuarded(World);
        if (!World || !WorldIdentity)
            return false;

        FPlayerNameCacheExclusiveLock Lock;
        if (!Lock.owns_lock())
            return false;

        if (GPlayerNameCacheWorld != World ||
            GPlayerNameCacheWorldIdentity != WorldIdentity)
        {
            GPlayerNameCacheWorld = World;
            GPlayerNameCacheWorldIdentity = WorldIdentity;
            GNextPlayerNameRefreshMs = 0;
            GPlayerNamesByState.clear();
            GPlayerNamesByConnection.clear();
            GPlayerCombatStatsByState.clear();
        }

        if (CurrentTimeMs < GNextPlayerNameRefreshMs)
            return false;

        GNextPlayerNameRefreshMs = CurrentTimeMs + 250ULL;
        return true;
    }

    bool TryCopyCachedPlayerNameUnsafe(
        AFortPlayerStateAthena* PlayerState,
        UNetConnection* Connection,
        char* OutName,
        size_t OutCapacity,
        size_t* OutLength)
    {
        if (!OutName || OutCapacity == 0 || !OutLength)
            return false;

        OutName[0] = '\0';
        *OutLength = 0;

        // Object-array probing is the only fault-prone live-object work in a
        // cache lookup. Complete it before taking the SRW lock so a guarded
        // failure can never strand the cache lock in the acquired state.
        const uint64_t PlayerStateIdentity =
            GetGuiObjectIdentityGuarded(PlayerState);
        const uint64_t ConnectionIdentity =
            GetGuiObjectIdentityGuarded(Connection);

        FPlayerNameCacheSharedLock Lock;
        if (!Lock.owns_lock())
            return false;

        const std::string* CachedName = nullptr;
        if (PlayerStateIdentity)
        {
            auto Found = GPlayerNamesByState.find(
                PlayerStateIdentity);
            if (Found != GPlayerNamesByState.end() &&
                !Found->second.empty())
            {
                CachedName = &Found->second;
            }
        }
        if (!CachedName && ConnectionIdentity)
        {
            auto Found = GPlayerNamesByConnection.find(
                ConnectionIdentity);
            if (Found != GPlayerNamesByConnection.end() &&
                !Found->second.empty())
            {
                CachedName = &Found->second;
            }
        }

        if (!CachedName || CachedName->size() >= OutCapacity)
            return false;

        memcpy(OutName, CachedName->data(), CachedName->size());
        OutName[CachedName->size()] = '\0';
        *OutLength = CachedName->size();
        return true;
    }

    bool TryCopyCachedPlayerNameGuarded(
        AFortPlayerStateAthena* PlayerState,
        UNetConnection* Connection,
        char* OutName,
        size_t OutCapacity,
        size_t* OutLength) noexcept
    {
        __try
        {
            return TryCopyCachedPlayerNameUnsafe(
                PlayerState,
                Connection,
                OutName,
                OutCapacity,
                OutLength);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (OutName && OutCapacity)
                OutName[0] = '\0';
            if (OutLength)
                *OutLength = 0;
            DisablePlayerNameCache();
            return false;
        }
    }

    bool TryReadPlayerCombatStatsUnsafe(
        UWorld* World,
        AFortPlayerControllerAthena* PlayerController,
        AFortPlayerStateAthena* PlayerState,
        AFortPlayerPawnAthena* PlayerPawn,
        uint64_t WorldIdentity,
        uint64_t ControllerIdentity,
        uint64_t PlayerStateIdentity,
        uint64_t PawnIdentity,
        ULONGLONG CapturedAtMs,
        FPlayerCombatStats& OutStats)
    {
        if (!World || !PlayerController ||
            !PlayerState || !PlayerPawn ||
            !WorldIdentity || !ControllerIdentity ||
            !PlayerStateIdentity || !PawnIdentity ||
            PlayerController->PlayerState != PlayerState)
        {
            return false;
        }

        const auto ControllerOwnsPawn = [&]()
        {
            return
                (PlayerController->HasMyFortPawn() &&
                 PlayerController->MyFortPawn == PlayerPawn) ||
                (PlayerController->HasPawn() &&
                 PlayerController->Pawn == PlayerPawn);
        };
        if (!ControllerOwnsPawn())
            return false;

        FPlayerCombatStats Stats{};
        Stats.Health = PlayerPawn->GetHealth();
        Stats.Shield = PlayerPawn->GetShield();
        Stats.Kills = PlayerState->HasKillScore()
            ? PlayerState->KillScore
            : (PlayerState->HasKills()
                   ? PlayerState->Kills
                   : 0);

        if (!std::isfinite(Stats.Health))
            Stats.Health = 0.f;
        if (!std::isfinite(Stats.Shield))
            Stats.Shield = 0.f;
        Stats.Health =
            (std::clamp)(Stats.Health, 0.f, 9999.f);
        Stats.Shield =
            (std::clamp)(Stats.Shield, 0.f, 9999.f);
        Stats.Kills =
            (std::clamp)(Stats.Kills, 0, 99999);

        // A pawn can change during a respawn while the values above are being
        // queried. Publish only if the complete object relationship is still
        // exactly the one that began this sample.
        if (GetGuiObjectIdentityGuarded(World) != WorldIdentity ||
            GetGuiObjectIdentityGuarded(PlayerController) !=
                ControllerIdentity ||
            GetGuiObjectIdentityGuarded(PlayerState) !=
                PlayerStateIdentity ||
            GetGuiObjectIdentityGuarded(PlayerPawn) != PawnIdentity ||
            PlayerController->PlayerState != PlayerState ||
            !ControllerOwnsPawn())
        {
            return false;
        }

        Stats.WorldIdentity = WorldIdentity;
        Stats.ControllerIdentity = ControllerIdentity;
        Stats.PlayerStateIdentity = PlayerStateIdentity;
        Stats.PawnIdentity = PawnIdentity;
        Stats.CapturedAtMs = CapturedAtMs;
        OutStats = Stats;
        return true;
    }

    bool TryReadPlayerCombatStatsGuarded(
        UWorld* World,
        AFortPlayerControllerAthena* PlayerController,
        AFortPlayerStateAthena* PlayerState,
        AFortPlayerPawnAthena* PlayerPawn,
        uint64_t WorldIdentity,
        uint64_t ControllerIdentity,
        uint64_t PlayerStateIdentity,
        uint64_t PawnIdentity,
        ULONGLONG CapturedAtMs,
        FPlayerCombatStats& OutStats) noexcept
    {
        __try
        {
            return TryReadPlayerCombatStatsUnsafe(
                World, PlayerController, PlayerState, PlayerPawn,
                WorldIdentity, ControllerIdentity,
                PlayerStateIdentity, PawnIdentity,
                CapturedAtMs, OutStats);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            OutStats = {};
            return false;
        }
    }

    bool TryCopyCachedPlayerCombatStats(
        AFortPlayerControllerAthena* PlayerController,
        AFortPlayerStateAthena* PlayerState,
        AFortPlayerPawnAthena* PlayerPawn,
        FPlayerCombatStats& OutStats)
    {
        OutStats = {};
        auto World = UWorld::GetWorld();
        const uint64_t WorldIdentity =
            GetGuiObjectIdentityGuarded(World);
        const uint64_t ControllerIdentity =
            GetGuiObjectIdentityGuarded(PlayerController);
        const uint64_t PlayerStateIdentity =
            GetGuiObjectIdentityGuarded(PlayerState);
        const uint64_t PawnIdentity =
            GetGuiObjectIdentityGuarded(PlayerPawn);
        if (!WorldIdentity || !ControllerIdentity ||
            !PlayerStateIdentity || !PawnIdentity)
        {
            return false;
        }
        const ULONGLONG CurrentTimeMs = GetTickCount64();

        FPlayerNameCacheSharedLock Lock;
        if (!Lock.owns_lock())
            return false;

        if (GPlayerNameCacheWorld != World ||
            GPlayerNameCacheWorldIdentity != WorldIdentity)
        {
            return false;
        }

        auto Found = GPlayerCombatStatsByState.find(
            PlayerStateIdentity);
        if (Found == GPlayerCombatStatsByState.end())
            return false;

        const auto& Stats = Found->second;
        if (Stats.WorldIdentity != WorldIdentity ||
            Stats.ControllerIdentity != ControllerIdentity ||
            Stats.PlayerStateIdentity != PlayerStateIdentity ||
            Stats.PawnIdentity != PawnIdentity ||
            CurrentTimeMs < Stats.CapturedAtMs ||
            CurrentTimeMs - Stats.CapturedAtMs >
                kPlayerCombatMaxAgeMs)
        {
            return false;
        }

        OutStats = Stats;
        return true;
    }

    uint64_t GetGuiObjectIdentityUnsafe(const UObject* Object)
    {
        if (!Object || !SDK::MemReadable(Object, sizeof(UObject)))
            return 0;
        const int32 ObjectIndex = Object->Index;
        if (ObjectIndex < 0 || ObjectIndex >= TUObjectArray::Num())
            return 0;
        auto Item = TUObjectArray::GetItemByIndex(ObjectIndex);
        const int32 InvalidFlags =
            Offsets::bEncryptedObjects ? 0x10200000 : 0x20;
        if (!Item || Item->GetObject() != Object ||
            (Item->GetFlags() & InvalidFlags))
        {
            return 0;
        }
        return
            (static_cast<uint64_t>(
                static_cast<uint32_t>(ObjectIndex)) << 32) |
            static_cast<uint32_t>(Item->SerialRef());
    }

    uint64_t GetGuiObjectIdentityGuarded(
        const UObject* Object) noexcept
    {
        __try
        {
            return GetGuiObjectIdentityUnsafe(Object);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return 0;
        }
    }
}

std::string GUI::GetPlayerNameFromConnection(
    UNetConnection* Connection)
{
    if (!Connection ||
        !SDK::MemReadable(Connection, sizeof(UObject)))
    {
        return {};
    }

    FString* RequestURL = GetRequestURL(Connection);
    std::string URL;
    if (!TryCopyFStringUtf8(RequestURL, 16384, URL))
        return {};

    // Parse complete option keys instead of accepting the first arbitrary
    // suffix ending in "Name". Legacy clients can send several name-shaped
    // options; these keys identify the connection's displayed player name.
    std::string Name;
    if (TryReadExactURLNameOption(URL, "PlayerName", Name) ||
        TryReadExactURLNameOption(URL, "DisplayName", Name) ||
        TryReadExactURLNameOption(URL, "Name", Name))
    {
        return Name;
    }
    return {};
}

std::string GUI::GetPlayerName(
    AFortPlayerStateAthena* PlayerState,
    UNetConnection* Connection)
{
    if (GPlayerNameCacheDisabled.load(
            std::memory_order_acquire))
    {
        return {};
    }

    std::array<char, 1025> Name{};
    size_t NameLength = 0;
    if (TryCopyCachedPlayerNameGuarded(
            PlayerState,
            Connection,
            Name.data(),
            Name.size(),
            &NameLength))
    {
        return std::string(Name.data(), NameLength);
    }

    // The menu renders on a separate thread, so a cache miss must never touch
    // live Unreal objects. PlayerNamesGameTick reads the connection identity
    // and any PlayerState fallback on the game thread.
    return {};
}

std::string GUI::GetPlayerNameGameThread(
    AFortPlayerStateAthena* PlayerState,
    UNetConnection* Connection)
{
    // Prefer the snapshot when one exists so ordinary connected players keep
    // the exact same URL-name resolution as the menu. Cheat bots do not own a
    // UNetConnection and therefore can never enter that cache; resolving their
    // PlayerState here is safe because elimination reports run on the game
    // thread.
    std::string Cached = GetPlayerName(PlayerState, Connection);
    if (!Cached.empty())
        return Cached;

    std::array<char, 1025> Resolved{};
    size_t ResolvedLength = 0;
    if (TryCopyResolvedPlayerNameGuarded(
            PlayerState,
            Connection,
            Resolved.data(),
            Resolved.size(),
            &ResolvedLength))
    {
        return std::string(Resolved.data(), ResolvedLength);
    }

    return {};
}

namespace
{
    void PlayerNamesGameTickUnsafe()
    {
        auto World = UWorld::GetWorld();
        auto Driver = World
            ? (UNetDriver*)World->NetDriver
            : nullptr;
        if (!World || !Driver)
        {
            TryResetPlayerNameCache(nullptr);
            return;
        }

        const ULONGLONG CurrentTimeMs = GetTickCount64();
        if (!TryBeginPlayerNameRefresh(
                World, CurrentTimeMs))
        {
            return;
        }

        const uint64_t WorldIdentity =
            GetGuiObjectIdentityGuarded(World);
        if (!WorldIdentity)
            return;

        std::unordered_map<uint64_t, std::string>
            NamesByState;
        std::unordered_map<uint64_t, std::string>
            NamesByConnection;
        std::unordered_map<uint64_t, FPlayerCombatStats>
            CombatStatsByState;
        auto PlayerControllerClass =
            AFortPlayerControllerAthena::StaticClass();
        auto PlayerPawnClass =
            AFortPlayerPawnAthena::StaticClass();
        auto& Connections = Driver->ClientConnections;
        if (Connections.Num() < 0 ||
            Connections.Max() < Connections.Num() ||
            Connections.Max() > 4096 ||
            (Connections.Num() > 0 &&
             (!Connections.Data ||
              !SDK::MemReadable(
                  Connections.Data,
                  static_cast<size_t>(Connections.Num()) *
                      sizeof(UNetConnection*)))))
        {
            return;
        }
        for (int32 Index = 0;
            Index < Connections.Num();
            Index++)
        {
            auto Connection = Connections[Index];
            const uint64_t ConnectionIdentity =
                GetGuiObjectIdentityGuarded(Connection);
            auto PlayerController = ConnectionIdentity
                ? Connection->PlayerController
                : nullptr;
            const uint64_t ControllerIdentity =
                GetGuiObjectIdentityGuarded(PlayerController);
            auto PlayerState = ControllerIdentity
                ? (AFortPlayerStateAthena*)
                    PlayerController->PlayerState
                : nullptr;
            const uint64_t PlayerStateIdentity =
                GetGuiObjectIdentityGuarded(PlayerState);
            if (!ConnectionIdentity && !PlayerStateIdentity)
                continue;

            AFortPlayerPawnAthena* PlayerPawn = nullptr;
            auto FortPlayerController =
                ControllerIdentity && PlayerControllerClass &&
                PlayerController->IsA(PlayerControllerClass)
                    ? (AFortPlayerControllerAthena*)PlayerController
                    : nullptr;
            uint64_t PawnIdentity = 0;
            if (FortPlayerController && PlayerPawnClass &&
                FortPlayerController->HasMyFortPawn())
            {
                auto Candidate =
                    FortPlayerController->MyFortPawn;
                const uint64_t CandidateIdentity =
                    GetGuiObjectIdentityGuarded(Candidate);
                if (CandidateIdentity &&
                    Candidate->IsA(PlayerPawnClass))
                {
                    PlayerPawn = Candidate;
                    PawnIdentity = CandidateIdentity;
                }
            }
            if (!PlayerPawn && FortPlayerController &&
                PlayerPawnClass &&
                FortPlayerController->HasPawn() &&
                FortPlayerController->Pawn)
            {
                auto Candidate = FortPlayerController->Pawn;
                const uint64_t CandidateIdentity =
                    GetGuiObjectIdentityGuarded(Candidate);
                if (CandidateIdentity &&
                    Candidate->IsA(PlayerPawnClass))
                {
                    PlayerPawn = (AFortPlayerPawnAthena*)Candidate;
                    PawnIdentity = CandidateIdentity;
                }
            }

            FPlayerCombatStats CombatStats{};
            if (ControllerIdentity && PlayerStateIdentity &&
                PawnIdentity &&
                TryReadPlayerCombatStatsGuarded(
                    World, FortPlayerController,
                    PlayerState, PlayerPawn,
                    WorldIdentity, ControllerIdentity,
                    PlayerStateIdentity, PawnIdentity,
                    CurrentTimeMs, CombatStats))
            {
                CombatStatsByState.emplace(
                    PlayerStateIdentity, CombatStats);
            }

            // Resolve the authoritative PlayerState label on the game thread;
            // the connection URL remains a guarded compatibility fallback.
            std::array<char, 1025> Resolved{};
            size_t ResolvedLength = 0;
            TryCopyResolvedPlayerNameGuarded(
                PlayerState,
                Connection,
                Resolved.data(),
                Resolved.size(),
                &ResolvedLength);
            std::string Name(
                Resolved.data(), ResolvedLength);
            if (Name.empty())
            {
                // A disconnect can transiently invalidate one reflected row.
                // Preserve its previous good label for this refresh rather than
                // blanking the entire cache or disabling names for everyone.
                std::array<char, 1025> Cached{};
                size_t CachedLength = 0;
                if (TryCopyCachedPlayerNameGuarded(
                        PlayerState,
                        Connection,
                        Cached.data(),
                        Cached.size(),
                        &CachedLength))
                {
                    Name.assign(Cached.data(), CachedLength);
                }
            }
            if (Name.empty())
                continue;

            if (PlayerStateIdentity)
            {
                NamesByState.emplace(
                    PlayerStateIdentity, Name);
            }
            if (ConnectionIdentity)
            {
                NamesByConnection.emplace(
                    ConnectionIdentity, Name);
            }
        }

        FPlayerNameCacheExclusiveLock Lock;
        if (Lock.owns_lock() &&
            GPlayerNameCacheWorld == World &&
            GPlayerNameCacheWorldIdentity == WorldIdentity)
        {
            GPlayerNamesByState.swap(NamesByState);
            GPlayerNamesByConnection.swap(NamesByConnection);
            GPlayerCombatStatsByState.swap(
                CombatStatsByState);
        }
    }
}

void GUI::PlayerNamesGameTick()
{
    if (!TryReenablePlayerNameCache())
        return;

    __try
    {
        PlayerNamesGameTickUnsafe();
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        DisablePlayerNameCache();
    }
}

int GUI::GetSelectedPlaylist()
{
    return PublishedSelectedPlaylist.load(
        std::memory_order_acquire);
}

void GUI::PublishSelectedPlaylist(int Value)
{
    PublishedSelectedPlaylist.store(
        Value, std::memory_order_release);
}

void GUI::MarkServerJoinable()
{
    if (GServerJoinableAtMs.load(std::memory_order_acquire) != 0)
    {
        if (gsStatus < Joinable)
            gsStatus = Joinable;
        return;
    }

    ULONGLONG Expected = 0;
    const ULONGLONG Now = GetTickCount64();
    GServerJoinableAtMs.compare_exchange_strong(
        Expected,
        Now,
        std::memory_order_release,
        std::memory_order_relaxed);

    if (gsStatus < Joinable)
        gsStatus = Joinable;
}

void GUI::ResetServerLifecycle()
{
    GServerJoinableAtMs.store(
        0, std::memory_order_release);
    gsStatus.store(
        NotReady, std::memory_order_release);
}

ID3D11ShaderResourceView* g_EmbedTexture = nullptr;
int EmbedWidth = 0;
int EmbedHeight = 0;

// Ported ATLAS-style menu logo
ID3D11ShaderResourceView* g_LogoTexture = nullptr;
int g_LogoW = 0, g_LogoH = 0;

// Grey/dark theme accent (highlights, active tab, checkmarks, sliders) - soft cool silver
#define ACCENT_R 0.780f
#define ACCENT_G 0.820f
#define ACCENT_B 0.910f
static ImVec4 Accent(float a = 1.f) { return ImVec4(ACCENT_R, ACCENT_G, ACCENT_B, a); }
static ImVec4 AccentDk(float a = 1.f) { return ImVec4(0.50f, 0.54f, 0.64f, a); } // darker for active

bool LoadTextureFromMemory(const unsigned char* buffer, int buffer_size, ID3D11Device* d3dDevice, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height)
{
    int image_width = 0;
    int image_height = 0;
    unsigned char* image_data = stbi_load_from_memory(buffer, buffer_size, &image_width, &image_height, NULL, 4);

    if (image_data == NULL)
        return false;

    // Full mip chain + GPU-generated mips so large source images downscale smoothly
    // to small on-screen sizes (e.g. the 1024px logo) instead of looking pixelated.
    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = image_width;
    desc.Height = image_height;
    desc.MipLevels = 0; // 0 => allocate a full mip chain
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    desc.CPUAccessFlags = 0;

    ID3D11Texture2D* pTexture = NULL;
    d3dDevice->CreateTexture2D(&desc, NULL, &pTexture);
    if (pTexture == NULL)
    {
        stbi_image_free(image_data);
        return false;
    }

    ID3D11DeviceContext* ctx = NULL;
    d3dDevice->GetImmediateContext(&ctx);
    ctx->UpdateSubresource(pTexture, 0, NULL, image_data, image_width * 4, 0);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    ZeroMemory(&srvDesc, sizeof(srvDesc));
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = (UINT)-1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    d3dDevice->CreateShaderResourceView(pTexture, &srvDesc, out_srv);
    if (*out_srv)
        ctx->GenerateMips(*out_srv);
    if (ctx)
        ctx->Release();
    pTexture->Release();

    *out_width = image_width;
    *out_height = image_height;

    stbi_image_free(image_data);

    return true;
}

// Upload an already-decoded RGBA8 buffer to a texture on our device. Sibling of
// LoadTextureFromMemory minus the stb decode; used for extracted map pixels.
bool CreateTextureFromRGBA8(const unsigned char* rgba, int width, int height, ID3D11Device* d3dDevice, ID3D11ShaderResourceView** out_srv)
{
    if (!rgba || width <= 0 || height <= 0 || !d3dDevice)
        return false;

    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 0; // full mip chain, GPU-generated (smooth downscale to ~260px)
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    desc.CPUAccessFlags = 0;

    ID3D11Texture2D* pTexture = NULL;
    d3dDevice->CreateTexture2D(&desc, NULL, &pTexture);
    if (pTexture == NULL)
        return false;

    ID3D11DeviceContext* ctx = NULL;
    d3dDevice->GetImmediateContext(&ctx);
    ctx->UpdateSubresource(pTexture, 0, NULL, rgba, width * 4, 0);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    ZeroMemory(&srvDesc, sizeof(srvDesc));
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = (UINT)-1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    d3dDevice->CreateShaderResourceView(pTexture, &srvDesc, out_srv);
    if (*out_srv)
        ctx->GenerateMips(*out_srv);
    if (ctx)
        ctx->Release();
    pTexture->Release();
    return true;
}

// ============================================================================
//  Custom Safe Zone interactive map editor support.
//  The GUI runs on its own standalone D3D11 device with no bridge into the game
//  renderer, so the in-game minimap UTexture2D can't be handed to ImGui directly.
//  We best-effort read its mip0 pixels out of CPU memory, decode to RGBA8, and
//  upload to *our* device (caching a PNG in Local AppData for reuse).
// ============================================================================
namespace SafeZoneMap
{
    static inline float Clamp(float v, float lo, float hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    struct MapTransform
    {
        float CenterX = 0.f;
        float CenterY = 0.f;
        // World-space vectors from the map center to the right and bottom
        // edges. Keeping the complete basis supports non-square captures and
        // authoritative runtime sampling without hard-coding map dimensions.
        float AxisUX = 0.f;
        float AxisUY = 135345.f;
        float AxisVX = -135345.f;
        float AxisVY = 0.f;
    };

    // MapInfo is owned by the game thread while the editor is rendered on the GUI
    // thread. A small sequence lock publishes a coherent snapshot without ever
    // handing an Unreal object to the GUI thread.
    static std::atomic<uint32_t> g_TransformSequence{ 0 };
    static std::atomic<float> g_MapCenterX{ 0.f };
    static std::atomic<float> g_MapCenterY{ 0.f };
    static std::atomic<float> g_MapAxisUX{ 0.f };
    static std::atomic<float> g_MapAxisUY{ 135345.f };
    static std::atomic<float> g_MapAxisVX{ -135345.f };
    static std::atomic<float> g_MapAxisVY{ 0.f };

    // A map click can happen in the frontend before the Athena map manager
    // exists. Keep the image-space selection as the source of truth so the
    // game thread can reproject it when the exact match transform arrives.
    static std::atomic<float> g_SelectedU{ 0.5f };
    static std::atomic<float> g_SelectedV{ 0.5f };
    static std::atomic<bool> g_HasNormalizedSelection{ false };

    static bool UsesLegacyAthenaCapture()
    {
        // The original Chapter 1 terrain uses one stable minimap capture from
        // the early releases through 10.40. Season OG has its own Rufus
        // projection and is resolved by the runtime map manager instead.
        return VersionInfo.FortniteVersion < 11.00f;
    }

    static MapTransform DefaultTransformForVersion()
    {
        // Used before AFortAthenaMapInfo is ready and as a guarded fallback.
        // Fortnite's native 10.40 world-to-map converter resolves the shared
        // Chapter 1 capture to this center and half-span. This includes the
        // image border outside the ten labeled grid cells.
        const float v = VersionInfo.FortniteVersion;
        if (UsesLegacyAthenaCapture())
            return { 32000.f, -25744.f, 0.f, 129760.4f, -129760.4f, 0.f };

        const float extent = (v >= 27.00f && v < 28.00f) ? 125000.f : 135345.f;
        // Athena's world plane uses X for map north/south and Y for east/west:
        // at zero map yaw, image-right is world +Y and image-bottom is world -X.
        // Runtime data replaces this provisional transform in-match.
        return { 0.f, 0.f, 0.f, extent, -extent, 0.f };
    }

    static inline float AxisULength(const MapTransform& map)
    {
        return sqrtf(map.AxisUX * map.AxisUX + map.AxisUY * map.AxisUY);
    }

    static inline float AxisVLength(const MapTransform& map)
    {
        return sqrtf(map.AxisVX * map.AxisVX + map.AxisVY * map.AxisVY);
    }

    static MapTransform GetTransform()
    {
        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const uint32_t before = g_TransformSequence.load(std::memory_order_acquire);
            if (before == 0)
                return DefaultTransformForVersion();
            if (before & 1)
                continue;

            MapTransform result{
                g_MapCenterX.load(std::memory_order_relaxed),
                g_MapCenterY.load(std::memory_order_relaxed),
                g_MapAxisUX.load(std::memory_order_relaxed),
                g_MapAxisUY.load(std::memory_order_relaxed),
                g_MapAxisVX.load(std::memory_order_relaxed),
                g_MapAxisVY.load(std::memory_order_relaxed)
            };
            if (before == g_TransformSequence.load(std::memory_order_acquire))
                return result;
        }
        return DefaultTransformForVersion();
    }

    static void PublishTransform(const MapTransform& value)
    {
        g_TransformSequence.fetch_add(1, std::memory_order_acq_rel); // writer active (odd)
        g_MapCenterX.store(value.CenterX, std::memory_order_relaxed);
        g_MapCenterY.store(value.CenterY, std::memory_order_relaxed);
        g_MapAxisUX.store(value.AxisUX, std::memory_order_relaxed);
        g_MapAxisUY.store(value.AxisUY, std::memory_order_relaxed);
        g_MapAxisVX.store(value.AxisVX, std::memory_order_relaxed);
        g_MapAxisVY.store(value.AxisVY, std::memory_order_relaxed);
        g_TransformSequence.fetch_add(1, std::memory_order_release); // snapshot ready (even)
    }

    static void RememberSelection(float u, float v)
    {
        g_SelectedU.store(Clamp(u, 0.f, 1.f), std::memory_order_relaxed);
        g_SelectedV.store(Clamp(v, 0.f, 1.f), std::memory_order_relaxed);
        g_HasNormalizedSelection.store(true, std::memory_order_release);
    }

    static void ForgetNormalizedSelection()
    {
        g_HasNormalizedSelection.store(false, std::memory_order_release);
    }

    // Always display the complete capture. FortWorldSettings::PvPMapWorldWidth
    // is the playable rectangle, while the cooked minimap commonly includes
    // additional capture space around it. Cropping a fixed number of texels and
    // then stretching the playable width over the result changes the scale.
    // The runtime transform below derives the complete capture size from
    // MapWorldScale and the map manager's logical layer size instead.
    static void GetImageUVs(ImVec2& uv0, ImVec2& uv1)
    {
        uv0 = ImVec2(0.f, 0.f);
        uv1 = ImVec2(1.f, 1.f);
    }

    // Canvas-local pixel (origin top-left, y down) -> UE world (cm), using the
    // full map basis supplied by WorldSettings or Fortnite's map manager.
    static inline void PixelToWorld(float lx, float ly, float side, const MapTransform& map,
                                    float& worldX, float& worldY)
    {
        const float su = 2.f * lx / side - 1.f;
        const float sv = 2.f * ly / side - 1.f;
        worldX = map.CenterX + map.AxisUX * su + map.AxisVX * sv;
        worldY = map.CenterY + map.AxisUY * su + map.AxisVY * sv;
    }

    // UE world (cm) -> canvas-local pixel (add the canvas rect-min for screen pos).
    static inline void WorldToPixel(float worldX, float worldY, float side, const MapTransform& map,
                                    float& lx, float& ly)
    {
        const float dx = worldX - map.CenterX;
        const float dy = worldY - map.CenterY;
        const float det = map.AxisUX * map.AxisVY - map.AxisVX * map.AxisUY;
        if (fabsf(det) < 1e-6f)
        {
            lx = ly = side * 0.5f;
            return;
        }
        const float su = (dx * map.AxisVY - map.AxisVX * dy) / det;
        const float sv = (map.AxisUX * dy - dx * map.AxisUY) / det;
        lx = side * 0.5f * (1.f + su);
        ly = side * 0.5f * (1.f + sv);
    }

    static inline ImVec2 RadiusToPixelAxes(float radiusCm, float side, const MapTransform& map)
    {
        const float extentU = AxisULength(map);
        const float extentV = AxisVLength(map);
        return ImVec2(radiusCm * side / (2.f * extentU),
                      radiusCm * side / (2.f * extentV));
    }

    static void ReprojectRememberedSelection(const MapTransform& map)
    {
        if (!g_HasNormalizedSelection.load(std::memory_order_acquire))
            return;

        const float u = g_SelectedU.load(std::memory_order_relaxed);
        const float v = g_SelectedV.load(std::memory_order_relaxed);
        float worldX, worldY;
        PixelToWorld(u, v, 1.f, map, worldX, worldY);
        FConfiguration::CustomSafeZoneCenter.X = worldX;
        FConfiguration::CustomSafeZoneCenter.Y = worldY;

        // Radius is an actual gameplay distance, not an image coordinate. The
        // frontend uses a provisional map span while the match is loading. If
        // the drag endpoint is reprojected with the later runtime span, a 240 m
        // circle can silently become a 550 m circle. Reproject only the center;
        // preserve the exact distance selected by the user.
        SDK::DbgLog("[SafeZoneMap] reprojected selection uv=(%.5f, %.5f) to world=(%.1f, %.1f), radius-preserved=%.1f\n",
            u, v, worldX, worldY,
            FConfiguration::CustomSafeZoneRadius.load(
                std::memory_order_relaxed));
    }

    // Shade the part of rect [rmin,rmax] outside a world-space circle. It may
    // project as an ellipse when the runtime map bounds are not perfectly square.
    // Horizontal strips avoid the anti-aliased shared edges that made the old
    // angular fan look like purple rays radiating away from the safe zone.
    static void FillOutsideEllipse(ImDrawList* dl, const ImVec2& rmin, const ImVec2& rmax,
                                   const ImVec2& c, const ImVec2& radius, ImU32 col)
    {
        if (radius.x <= 0.f || radius.y <= 0.f)
        {
            dl->AddRectFilled(rmin, rmax, col);
            return;
        }

        const ImDrawListFlags oldFlags = dl->Flags;
        dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;

        const float ellipseTop = c.y - radius.y;
        const float ellipseBottom = c.y + radius.y;
        const float clippedTop = Clamp(ellipseTop, rmin.y, rmax.y);
        const float clippedBottom = Clamp(ellipseBottom, rmin.y, rmax.y);

        if (clippedTop > rmin.y)
            dl->AddRectFilled(rmin, ImVec2(rmax.x, clippedTop), col);
        if (clippedBottom < rmax.y)
            dl->AddRectFilled(ImVec2(rmin.x, clippedBottom), rmax, col);

        const int N = 128;
        if (clippedBottom > clippedTop)
        {
            const float step = (clippedBottom - clippedTop) / (float)N;
            for (int i = 0; i < N; ++i)
            {
                const float y0 = clippedTop + step * (float)i;
                const float y1 = (i + 1 == N) ? clippedBottom : y0 + step;
                const float ny0 = (y0 - c.y) / radius.y;
                const float ny1 = (y1 - c.y) / radius.y;
                const float extent0 = radius.x * sqrtf((std::max)(0.f, 1.f - ny0 * ny0));
                const float extent1 = radius.x * sqrtf((std::max)(0.f, 1.f - ny1 * ny1));
                const float left0 = Clamp(c.x - extent0, rmin.x, rmax.x);
                const float left1 = Clamp(c.x - extent1, rmin.x, rmax.x);
                const float right0 = Clamp(c.x + extent0, rmin.x, rmax.x);
                const float right1 = Clamp(c.x + extent1, rmin.x, rmax.x);

                if (left0 > rmin.x || left1 > rmin.x)
                    dl->AddQuadFilled(ImVec2(rmin.x, y0), ImVec2(left0, y0),
                                      ImVec2(left1, y1), ImVec2(rmin.x, y1), col);
                if (right0 < rmax.x || right1 < rmax.x)
                    dl->AddQuadFilled(ImVec2(right0, y0), ImVec2(rmax.x, y0),
                                      ImVec2(rmax.x, y1), ImVec2(right1, y1), col);
            }
        }

        dl->Flags = oldFlags;
    }

    // Draw one line, omitting the portion that lies inside the safe ellipse.
    static void AddLineOutsideEllipse(ImDrawList* dl, const ImVec2& p0, const ImVec2& p1,
                                      const ImVec2& c, const ImVec2& radius,
                                      ImU32 col, float thickness)
    {
        const ImVec2 d((p1.x - p0.x) / radius.x, (p1.y - p0.y) / radius.y);
        const ImVec2 f((p0.x - c.x) / radius.x, (p0.y - c.y) / radius.y);
        const float a = d.x * d.x + d.y * d.y;
        const float b = 2.f * (f.x * d.x + f.y * d.y);
        const float cc = f.x * f.x + f.y * f.y - 1.f;
        const float discriminant = b * b - 4.f * a * cc;

        float cuts[4] = { 0.f, 1.f, 0.f, 0.f };
        int cutCount = 2;
        if (a > 0.f && discriminant > 0.f)
        {
            const float root = sqrtf(discriminant);
            const float t0 = (-b - root) / (2.f * a);
            const float t1 = (-b + root) / (2.f * a);
            if (t0 > 0.f && t0 < 1.f) cuts[cutCount++] = t0;
            if (t1 > 0.f && t1 < 1.f) cuts[cutCount++] = t1;
        }
        std::sort(cuts, cuts + cutCount);

        const ImVec2 screenD(p1.x - p0.x, p1.y - p0.y);
        for (int i = 0; i + 1 < cutCount; ++i)
        {
            const float begin = cuts[i], end = cuts[i + 1];
            const float mid = (begin + end) * 0.5f;
            const float mx = f.x + d.x * mid;
            const float my = f.y + d.y * mid;
            if (mx * mx + my * my < 1.f)
                continue;
            dl->AddLine(ImVec2(p0.x + screenD.x * begin, p0.y + screenD.y * begin),
                        ImVec2(p0.x + screenD.x * end, p0.y + screenD.y * end), col, thickness);
        }
    }

    // Fortnite's storm map uses parallel bands running from bottom-left to
    // top-right. Keep them clipped to the storm area outside the safe ellipse.
    static void DrawStormBands(ImDrawList* dl, const ImVec2& rmin, const ImVec2& rmax,
                               const ImVec2& c, const ImVec2& radius, ImU32 col)
    {
        const float width = rmax.x - rmin.x;
        const float height = rmax.y - rmin.y;
        const float spacing = (std::max)(28.f, width / 10.f);
        for (float diagonal = 0.f; diagonal <= width + height; diagonal += spacing)
        {
            ImVec2 p0, p1;
            if (diagonal <= height)
                p0 = ImVec2(rmin.x, rmin.y + diagonal);
            else
                p0 = ImVec2(rmin.x + diagonal - height, rmax.y);

            if (diagonal <= width)
                p1 = ImVec2(rmin.x + diagonal, rmin.y);
            else
                p1 = ImVec2(rmax.x, rmin.y + diagonal - width);

            AddLineOutsideEllipse(dl, p0, p1, c, radius, col, 4.f);
        }
    }

    // Manual override for tuning on a specific engine version (0 = auto-detect).
    static uint32_t g_PlatformDataOffsetOverride = 0;
    // Player loadout icons reuse this decoder but must stay quiet and small.
    // These are game-thread-local so the Safe Zone editor keeps its diagnostics
    // and full-resolution behavior on the GUI thread.
    static thread_local bool g_SuppressTextureExtractionLogs = false;
    static thread_local int32 g_MaxTextureExtractionDimension = 0;

    // EPixelFormat is append-only across the supported UE4 builds. Chapter 2
    // minimaps can be BC7; treating its 16-byte blocks as same-sized BC3 is what
    // produced the vertical multicolour corruption on 17.30.
    enum : int32
    {
        PF_B8G8R8A8 = 2,
        PF_DXT1 = 5,
        PF_DXT3 = 6,
        PF_DXT5 = 7,
        PF_R8G8B8A8 = 37,
        PF_BC7 = 56
    };

    static bool IsKnownFormat(int32 f)
    {
        return f == PF_B8G8R8A8 || f == PF_DXT1 || f == PF_DXT3 ||
            f == PF_DXT5 || f == PF_R8G8B8A8 || f == PF_BC7;
    }
    static size_t FormatBytes(int32 f, int w, int h)
    {
        const size_t px = (size_t)w * h;
        if (f == PF_B8G8R8A8 || f == PF_R8G8B8A8)
            return px * 4; // 32bpp uncompressed
        const size_t blocksX = ((size_t)w + 3) / 4;
        const size_t blocksY = ((size_t)h + 3) / 4;
        if (f == PF_DXT1) return blocksX * blocksY * 8;
        return blocksX * blocksY * 16; // BC2/BC3/BC7
    }

    // True only if every byte of [p, p+bytes) is committed and readable.
    static bool IsReadable(const void* p, size_t bytes)
    {
        if (!p || bytes == 0) return false;
        const uintptr_t start = (uintptr_t)p;
        if (start < 0x10000) return false;               // null / low reserved region
        if (start + bytes < start) return false;         // address-space wraparound => bogus ptr
        if (start > 0x7FFFFFFFFFFFull) return false;      // above x64 user address space
        MEMORY_BASIC_INFORMATION mbi{};
        const uint8_t* cur = (const uint8_t*)p;
        const uint8_t* end = cur + bytes;
        while (cur < end)
        {
            if (VirtualQuery(cur, &mbi, sizeof(mbi)) == 0) return false;
            if (mbi.State != MEM_COMMIT) return false;
            if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || mbi.Protect == 0) return false;
            cur = (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
        }
        return true;
    }

    // Bytes of the committed region from p to the end of its VirtualQuery block.
    // Restricted to MEM_PRIVATE: texture pixel buffers are FMemory::Malloc heap
    // allocations. MEM_IMAGE (the EXE) and MEM_MAPPED (pak files) regions also
    // pass a plain "readable" test and previously produced garbage textures
    // (2MB of program code decoded as DXT1 == colored static; see 10.40/27.11).
    static size_t RegionSize(const void* p)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!p || (uintptr_t)p < 0x10000 || VirtualQuery(p, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) return 0;
        if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || mbi.Protect == 0) return 0;
        if (mbi.Type != MEM_PRIVATE) return 0; // heap only: reject EXE image / mapped-file pointers
        size_t before = (const uint8_t*)p - (const uint8_t*)mbi.BaseAddress;
        return mbi.RegionSize > before ? (size_t)(mbi.RegionSize - before) : 0;
    }

    // The strict resident-icon decoder proves the mip layout, dimensions,
    // encoded byte count and payload identity before and after its bounded
    // copy. That is strong enough to admit read-only IoStore/pak mappings here
    // without weakening the legacy heuristic decoder above. Executable/image
    // mappings remain invalid icon payloads.
    static size_t StrictResidentIconPayloadRegionSize(const void* p)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!p ||
            (uintptr_t)p < 0x10000 ||
            VirtualQuery(p, &mbi, sizeof(mbi)) == 0 ||
            mbi.State != MEM_COMMIT)
        {
            return 0;
        }
        if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            mbi.Protect == 0 ||
            (mbi.Protect &
                (PAGE_EXECUTE |
                 PAGE_EXECUTE_READ |
                 PAGE_EXECUTE_READWRITE |
                 PAGE_EXECUTE_WRITECOPY)))
        {
            return 0;
        }
        if (mbi.Type != MEM_PRIVATE &&
            mbi.Type != MEM_MAPPED)
        {
            return 0;
        }
        const size_t Before =
            static_cast<const uint8_t*>(p) -
            static_cast<const uint8_t*>(mbi.BaseAddress);
        return mbi.RegionSize > Before
            ? static_cast<size_t>(mbi.RegionSize - Before)
            : 0;
    }

    static bool IsPow2Dim(int32 v) { return v >= 64 && v <= 16384 && (v & (v - 1)) == 0; }

    // Reject "pixel data" pointers whose first 8 bytes are a code/vtable pointer
    // into a loaded module - the classic false positive (async-IO handles, linker
    // objects stored inside FByteBulkData) that decodes as colored static.
    static bool StartsWithImagePointer(const uint8_t* p)
    {
        if (!IsReadable(p, 8)) return false;
        const uint8_t* q = *(const uint8_t* const*)p;
        if (!q || (uintptr_t)q < 0x10000 || ((uintptr_t)q & 7)) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(q, &mbi, sizeof(mbi)) == 0) return false;
        return mbi.Type == MEM_IMAGE;
    }

    struct FPlatformData
    {
        const uint8_t* Ptr = nullptr;
        int32 SizeX = 0, SizeY = 0, PixelFormat = 0;
        uint32_t FoundAtOffset = 0;
    };

    // Scan the UTexture2D object for the non-reflected PlatformData pointer by
    // recognising FTexturePlatformData's header: {int32 SizeX, int32 SizeY,
    // uint32 PackedData/NumSlices, EPixelFormat PixelFormat}. Reading PixelFormat
    // from +0x0C (not a "first small int" probe, which grabs PackedData) both
    // validates the candidate and gives the correct format to decode.
    static bool DetectPlatformData(const void* tex, FPlatformData& out)
    {
        if (!IsReadable(tex, 0x220)) return false;
        const uint8_t* base = (const uint8_t*)tex;

        FPlatformData fallback; bool haveFallback = false;

        auto consider = [&](const uint8_t* P, uint32_t off) -> bool
        {
            if (!IsReadable(P, 0x20) || ((uintptr_t)P & 7)) return false;
            int32 sx = *(const int32*)(P + 0x0);
            int32 sy = *(const int32*)(P + 0x4);
            if (!IsPow2Dim(sx) || !IsPow2Dim(sy)) return false;
            // PixelFormat is at +0x0C (after two dims + PackedData); a couple of
            // older layouts keep it at +0x08. Take whichever is a known format.
            const int32 pf0c =
                *reinterpret_cast<const int32*>(P + 0x0C);
            const int32 pf08 =
                *reinterpret_cast<const int32*>(P + 0x08);
            // UE4 stores PixelFormat as a byte-sized TEnumAsByte in several cooked
            // FTexturePlatformData layouts. The following padding is not
            // guaranteed to be zero, so reading four bytes can turn a valid
            // PF_DXT5 (7) into an unrecognised integer.
            const int32 pf0cByte =
                *reinterpret_cast<const uint8_t*>(P + 0x0C);
            const int32 pf08Byte =
                *reinterpret_cast<const uint8_t*>(P + 0x08);
            const int32 pf =
                IsKnownFormat(pf0c)
                    ? pf0c
                    : (IsKnownFormat(pf0cByte)
                        ? pf0cByte
                        : (IsKnownFormat(pf08)
                            ? pf08
                            : (IsKnownFormat(pf08Byte)
                                ? pf08Byte
                                : 0)));
            if (pf != 0) // strong match: real FTexturePlatformData with a known format
            {
                out.Ptr = P; out.SizeX = sx; out.SizeY = sy; out.PixelFormat = pf; out.FoundAtOffset = off;
                if (!g_SuppressTextureExtractionLogs)
                    SDK::DbgLog("[SafeZoneMap] PlatformData @0x%X ptr=%p %dx%d PixelFormat=%d\n",
                        off, (const void*)P, sx, sy, pf);
                return true;
            }
            if (!haveFallback)
            {
                fallback = { P, sx, sy, 0, off };
                haveFallback = true;
                if (!g_SuppressTextureExtractionLogs)
                    SDK::DbgLog(
                        "[SafeZoneMap] PlatformData candidate @0x%X ptr=%p %dx%d header pf08=%d pf0c=%d pf10=%d pf14=%d\n",
                        off, (const void*)P, sx, sy, pf08, pf0c,
                        *(const int32*)(P + 0x10),
                        *(const int32*)(P + 0x14));
            }
            return false;
        };

        uint32_t start = g_PlatformDataOffsetOverride ? g_PlatformDataOffsetOverride : 0x10;
        uint32_t stop  = g_PlatformDataOffsetOverride ? g_PlatformDataOffsetOverride : 0x200;
        for (uint32_t off = start; off <= stop; off += 8)
        {
            const uint8_t* P = *(const uint8_t* const*)(base + off);
            if (consider(P, off)) return true;
            if (IsReadable(P, 8)) // UE5 pointer-to-pointer (PrivatePlatformData) variant
            {
                const uint8_t* Q = *(const uint8_t* const*)P;
                if (consider(Q, off)) return true;
            }
        }
        if (haveFallback) // no known-format match; fall back to first pow2 candidate
        {
            out = fallback;
            if (!g_SuppressTextureExtractionLogs)
                SDK::DbgLog("[SafeZoneMap] PlatformData (weak) @0x%X ptr=%p %dx%d (no known format)\n",
                    out.FoundAtOffset, (const void*)out.Ptr, out.SizeX, out.SizeY);
            return true;
        }
        if (!g_SuppressTextureExtractionLogs)
            SDK::DbgLog("[SafeZoneMap] PlatformData not found in texture object\n");
        return false;
    }

    // From detected platform data, find mip0's resident pixel bytes (a pointer to
    // a committed region large enough for neededBytes).
    static bool LooksLikeMip(const uint8_t* mip, int32 sizeX, int32 sizeY)
    {
        if (!IsReadable(mip, 0xA0)) return false;

        // FTexture2DMipMap starts with FByteBulkData, whose size varies heavily
        // between UE4/UE5 builds. Validate it by finding its trailing dimensions
        // instead of assuming the dimensions are at byte zero.
        for (uint32_t off = 0; off <= 0x90; off += 4)
        {
            const int32 x = *(const int32*)(mip + off);
            const int32 y = *(const int32*)(mip + off + 4);
            if (x == sizeX && y == sizeY) return true;
        }
        // Some cooked layouts keep mip dimensions as uint16 values.
        for (uint32_t off = 0; off <= 0x94; off += 2)
        {
            const uint16_t x = *(const uint16_t*)(mip + off);
            const uint16_t y = *(const uint16_t*)(mip + off + 2);
            if (x == sizeX && y == sizeY) return true;
        }
        return false;
    }

    static const uint8_t* FindMip0Bytes(
        const FPlatformData& pd, size_t neededBytes)
    {
        for (uint32_t moff = 0x0C; moff <= 0x80; moff += 4) // the mips TArray {Data,Num,Max}
        {
            const uint8_t* arrAt = pd.Ptr + moff;
            if (!IsReadable(arrAt, 16)) continue;
            const uint8_t* data = *(const uint8_t* const*)(arrAt);
            int32 num = *(const int32*)(arrAt + 8);
            int32 max = *(const int32*)(arrAt + 12);
            if (num < 1 || num > 20 || max < num || !IsReadable(data, 8)) continue;

            // Element may be inline (FTexture2DMipMap) or a pointer (TIndirectArray).
            const uint8_t* mip0 = data;
            const uint8_t* asPtr = *(const uint8_t* const*)data;
            if (LooksLikeMip(asPtr, pd.SizeX, pd.SizeY))
                mip0 = asPtr;
            else if (!LooksLikeMip(mip0, pd.SizeX, pd.SizeY))
                continue;

            for (uint32_t boff = 0x0; boff <= 0x80; boff += 8) // the bulk-data pointer
            {
                const uint8_t* cand = *(const uint8_t* const*)(mip0 + boff);
                if (RegionSize(cand) >= neededBytes && !StartsWithImagePointer(cand))
                {
                    if (!g_SuppressTextureExtractionLogs)
                        SDK::DbgLog("[SafeZoneMap] mip0 bytes @mip+0x%X ptr=%p (need %zu, mips@pd+0x%X num=%d)\n",
                            boff, (const void*)cand, neededBytes, moff, num);
                    return cand;
                }
            }
        }
        if (!g_SuppressTextureExtractionLogs)
            SDK::DbgLog("[SafeZoneMap] mip0 resident bytes not found (need %zu)\n", neededBytes);
        return nullptr;
    }

    static bool DecodeTextureBytes(
        const uint8_t* src,
        int fmt,
        int w,
        int h,
        std::vector<unsigned char>& rgba,
        int& outW,
        int& outH)
    {
        if (!src || w <= 0 || h <= 0)
            return false;
        const size_t pixels = (size_t)w * h;
        if (pixels > SIZE_MAX / 4)
            return false;

        rgba.assign(pixels * 4, 0);
        auto decodeBlocks = [&](int blockBytes, void(*dec)(const void*, void*, int))
        {
            const int bw = (w + 3) / 4, bh = (h + 3) / 4;
            const uint8_t* bp = src;
            for (int by = 0; by < bh; ++by)
                for (int bx = 0; bx < bw; ++bx, bp += blockBytes)
                {
                    unsigned char block[64]; // 4x4 RGBA
                    dec(bp, block, 4 * 4);
                    for (int py = 0; py < 4; ++py)
                    {
                        int y = by * 4 + py; if (y >= h) break;
                        for (int px = 0; px < 4; ++px)
                        {
                            int x = bx * 4 + px; if (x >= w) break;
                            unsigned char* d = &rgba[((size_t)y * w + x) * 4];
                            unsigned char* s = &block[(py * 4 + px) * 4];
                            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
                        }
                    }
                }
        };

        if (fmt == PF_B8G8R8A8)
        {
            for (size_t i = 0; i < pixels; ++i)
            {
                const uint8_t* s = src + i * 4;
                unsigned char* d = &rgba[i * 4];
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; // BGRA -> RGBA
            }
        }
        else if (fmt == PF_R8G8B8A8)
        {
            memcpy(rgba.data(), src, pixels * 4);
        }
        else if (fmt == PF_DXT1) { decodeBlocks(BCDEC_BC1_BLOCK_SIZE, bcdec_bc1); }
        else if (fmt == PF_DXT3) { decodeBlocks(BCDEC_BC2_BLOCK_SIZE, bcdec_bc2); }
        else if (fmt == PF_DXT5) { decodeBlocks(BCDEC_BC3_BLOCK_SIZE, bcdec_bc3); }
        else if (fmt == PF_BC7)  { decodeBlocks(BCDEC_BC7_BLOCK_SIZE, bcdec_bc7); }
        else return false;

        outW = w;
        outH = h;
        return true;
    }

    // Best-effort: extract the given minimap texture into an RGBA8 buffer.
    static bool ExtractToRGBA(const void* tex, std::vector<unsigned char>& rgba, int& outW, int& outH)
    {
        FPlatformData pd;
        if (!DetectPlatformData(tex, pd)) return false;
        if (g_MaxTextureExtractionDimension > 0 &&
            (pd.SizeX > g_MaxTextureExtractionDimension ||
             pd.SizeY > g_MaxTextureExtractionDimension))
        {
            return false;
        }

        const int w = pd.SizeX, h = pd.SizeY;
        const size_t pixels = (size_t)w * h;
        int fmt = pd.PixelFormat;
        const uint8_t* src = nullptr;

        // Known format -> look for exactly the mip that size (no size-guessing,
        // which is what previously decoded BC1 bytes as raw RGBA -> garbled).
        if (IsKnownFormat(fmt))
            src = FindMip0Bytes(pd, FormatBytes(fmt, w, h));

        if (!src) // unknown format or not found: probe by descending size class
        {
            const struct { int f; size_t need; } probes[] = {
                { PF_B8G8R8A8, pixels * 4 },
                { PF_DXT5,     pixels     },
                { PF_DXT1,     FormatBytes(PF_DXT1, w, h) },
            };
            for (auto& pr : probes)
            {
                const uint8_t* p = FindMip0Bytes(pd, pr.need);
                if (p) { src = p; fmt = pr.f; break; }
            }
            if (!src) return false;
        }

        if (!DecodeTextureBytes(
                src, fmt, w, h,
                rgba, outW, outH))
            return false;
        if (!g_SuppressTextureExtractionLogs)
            SDK::DbgLog("[SafeZoneMap] extracted %dx%d fmt=%d ok\n", w, h, fmt);
        return true;
    }

    // SEH guard: chasing unknown cross-version texture layouts can dereference a
    // bad pointer (e.g. the Rufus minimaps on 27.x). Catch the access violation
    // and fall back to the numeric editor instead of crashing the GUI thread.
    static bool ExtractToRGBA_Guarded(const void* tex, std::vector<unsigned char>& rgba, int& outW, int& outH)
    {
        __try
        {
            return ExtractToRGBA(tex, rgba, outW, outH);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (!g_SuppressTextureExtractionLogs)
                SDK::DbgLog("[SafeZoneMap] extraction faulted (SEH); skipping image\n");
            return false;
        }
    }

    constexpr int32 kMaximumResidentIconMips = 20;
    constexpr int32 kMaximumResidentIconDimension = 512;
    constexpr size_t kMaximumResidentIconBytes =
        1024 * 1024;

    struct FResidentIconMipChain
    {
        const uint8_t* Header = nullptr;
        const uint8_t* Data = nullptr;
        size_t ArrayStorageBytes = 0;
        size_t MipScanBytes = 0;
        int32 Num = 0;
        int32 Max = 0;
        const uint8_t* Mips[kMaximumResidentIconMips]{};
        uint16_t DimensionOffset = 0;
        bool DimensionsAre16Bit = false;
        bool MipsAreIndirect = false;
    };

    struct FResidentIconPayload
    {
        const uint8_t* Bytes = nullptr;
        int32 MipIndex = -1;
        int32 Width = 0;
        int32 Height = 0;
        size_t ByteCount = 0;
    };

    static bool ReadResidentIconMipDimensions(
        const uint8_t* Mip,
        uint16_t Offset,
        bool Is16Bit,
        int32& Width,
        int32& Height,
        int32& Depth)
    {
        const size_t Bytes =
            Is16Bit
                ? sizeof(uint16_t) * 3
                : sizeof(uint32_t) * 3;
        if (!Mip ||
            !IsReadable(Mip + Offset, Bytes))
        {
            return false;
        }

        if (Is16Bit)
        {
            const auto Values =
                reinterpret_cast<const uint16_t*>(
                    Mip + Offset);
            Width = Values[0];
            Height = Values[1];
            Depth = Values[2];
        }
        else
        {
            const auto Values =
                reinterpret_cast<const uint32_t*>(
                    Mip + Offset);
            Width = static_cast<int32>(Values[0]);
            Height = static_cast<int32>(Values[1]);
            Depth = static_cast<int32>(Values[2]);
        }
        return true;
    }

    static bool IsResidentIconMipLayout(
        const FPlatformData& PlatformData,
        const FResidentIconMipChain& Chain,
        uint16_t DimensionOffset,
        bool DimensionsAre16Bit)
    {
        for (int32 Index = 0;
             Index < Chain.Num;
             ++Index)
        {
            int32 Width = 0;
            int32 Height = 0;
            int32 Depth = 0;
            if (!ReadResidentIconMipDimensions(
                    Chain.Mips[Index],
                    DimensionOffset,
                    DimensionsAre16Bit,
                    Width,
                    Height,
                    Depth) ||
                Width !=
                    (std::max)(
                        1,
                        PlatformData.SizeX >> Index) ||
                Height !=
                    (std::max)(
                        1,
                        PlatformData.SizeY >> Index) ||
                Depth != 1)
            {
                return false;
            }
        }
        return true;
    }

    static bool FinalizeResidentIconMipLayout(
        const FPlatformData& PlatformData,
        FResidentIconMipChain& Chain)
    {
        if (Chain.MipScanBytes <
                sizeof(uint16_t) * 3)
        {
            return false;
        }

        int LayoutCount = 0;
        uint16_t MatchedOffset = 0;
        bool Matched16Bit = false;
        const size_t Maximum32BitOffset =
            Chain.MipScanBytes >= sizeof(uint32_t) * 3
                ? Chain.MipScanBytes -
                    sizeof(uint32_t) * 3
                : 0;
        for (uint16_t Offset = 0;
             Offset <= Maximum32BitOffset;
             Offset += 4)
        {
            if (IsResidentIconMipLayout(
                    PlatformData,
                    Chain,
                    Offset,
                    false))
            {
                ++LayoutCount;
                MatchedOffset = Offset;
                Matched16Bit = false;
            }
        }
        const size_t Maximum16BitOffset =
            Chain.MipScanBytes -
            sizeof(uint16_t) * 3;
        for (uint16_t Offset = 0;
             Offset <= Maximum16BitOffset;
             Offset += 2)
        {
            if (IsResidentIconMipLayout(
                    PlatformData,
                    Chain,
                    Offset,
                    true))
            {
                ++LayoutCount;
                MatchedOffset = Offset;
                Matched16Bit = true;
            }
        }
        if (LayoutCount != 1)
            return false;

        Chain.DimensionOffset = MatchedOffset;
        Chain.DimensionsAre16Bit = Matched16Bit;
        return true;
    }

    static bool ValidateResidentIconMipHeaderUnsafe(
        const FPlatformData& PlatformData,
        uint32_t HeaderOffset,
        FResidentIconMipChain& Result)
    {
        Result = {};
        const uint8_t* Header =
            PlatformData.Ptr + HeaderOffset;
        if (!IsReadable(Header, 16))
            return false;

        const uint8_t* Data =
            *reinterpret_cast<const uint8_t* const*>(
                Header);
        const int32 Num =
            *reinterpret_cast<const int32*>(
                Header + 8);
        const int32 Max =
            *reinterpret_cast<const int32*>(
                Header + 12);
        if (!Data ||
            (reinterpret_cast<uintptr_t>(Data) & 7) ||
            Num < 1 ||
            Num > kMaximumResidentIconMips ||
            Max < Num ||
            Max > 64)
        {
            return false;
        }

        FResidentIconMipChain Matched;
        int StorageMatches = 0;
        auto TryCandidate = [&] (
            bool IsIndirect,
            size_t InlineStride)
        {
            FResidentIconMipChain Candidate;
            Candidate.Header = Header;
            Candidate.Data = Data;
            Candidate.Num = Num;
            Candidate.Max = Max;
            Candidate.MipsAreIndirect = IsIndirect;
            const size_t ElementStride = IsIndirect
                ? sizeof(const uint8_t*)
                : InlineStride;
            Candidate.ArrayStorageBytes =
                static_cast<size_t>(Max) *
                ElementStride;
            Candidate.MipScanBytes = IsIndirect
                ? 0x100
                : InlineStride;
            const size_t ConstructedBytes =
                static_cast<size_t>(Num) *
                ElementStride;
            if (!ElementStride ||
                !ConstructedBytes ||
                !IsReadable(Data, ConstructedBytes))
                return;

            const uintptr_t ArrayBegin =
                reinterpret_cast<uintptr_t>(Data);
            const uintptr_t ArrayEnd =
                ArrayBegin +
                Candidate.ArrayStorageBytes;
            if (ArrayEnd < ArrayBegin)
                return;
            for (int32 Index = 0;
                 Index < Num;
                 ++Index)
            {
                const uint8_t* Mip = IsIndirect
                    ? reinterpret_cast<
                        const uint8_t* const*>(Data)[Index]
                    : Data + InlineStride *
                        static_cast<size_t>(Index);
                if (!Mip ||
                    (reinterpret_cast<uintptr_t>(Mip) & 7) ||
                    !IsReadable(
                        Mip,
                        Candidate.MipScanBytes))
                {
                    return;
                }
                const uintptr_t MipAddress =
                    reinterpret_cast<uintptr_t>(Mip);
                if (IsIndirect &&
                    MipAddress >= ArrayBegin &&
                    MipAddress < ArrayEnd)
                {
                    return;
                }
                Candidate.Mips[Index] = Mip;
            }
            if (!FinalizeResidentIconMipLayout(
                    PlatformData, Candidate))
            {
                return;
            }
            Matched = Candidate;
            ++StorageMatches;
        };

        // UE4 uses both TIndirectArray<FTexture2DMipMap> and an inline
        // TArray<FTexture2DMipMap> across the supported releases. First try
        // the pointer-array form, then prove an inline element stride by the
        // complete halving dimension sequence. Ambiguous layouts fail closed.
        TryCandidate(true, 0);
        if (Num >= 2)
        {
            for (size_t Stride = 0x30;
                 Stride <= 0x100;
                 Stride += 8)
            {
                TryCandidate(false, Stride);
            }
        }
        if (StorageMatches != 1)
        {
            Result = {};
            return false;
        }
        Result = Matched;

        // Re-read the header and pointer array so a streaming update cannot
        // turn a partially observed chain into a valid-looking snapshot.
        if (*reinterpret_cast<
                const uint8_t* const*>(Header) != Data ||
            *reinterpret_cast<
                const int32*>(Header + 8) != Num ||
            *reinterpret_cast<
                const int32*>(Header + 12) != Max)
        {
            Result = {};
            return false;
        }
        if (Result.MipsAreIndirect)
        {
            for (int32 Index = 0;
                 Index < Num;
                 ++Index)
            {
                if (reinterpret_cast<
                        const uint8_t* const*>(
                            Data)[Index] !=
                    Result.Mips[Index])
                {
                    Result = {};
                    return false;
                }
            }
        }
        if (!IsResidentIconMipLayout(
                PlatformData,
                Result,
                Result.DimensionOffset,
                Result.DimensionsAre16Bit))
        {
            Result = {};
            return false;
        }
        return true;
    }

    static bool SnapshotResidentIconMipChainUnsafe(
        const FPlatformData& PlatformData,
        FResidentIconMipChain& Result)
    {
        Result = {};
        int Matches = 0;
        for (uint32_t Offset = 0x10;
             Offset <= 0x80;
             Offset += 8)
        {
            FResidentIconMipChain Candidate;
            if (!ValidateResidentIconMipHeaderUnsafe(
                    PlatformData,
                    Offset,
                    Candidate))
            {
                continue;
            }
            Result = Candidate;
            if (++Matches > 1)
            {
                Result = {};
                return false;
            }
        }
        return Matches == 1;
    }

    static bool TrySnapshotResidentIconMipChain(
        const FPlatformData* PlatformData,
        FResidentIconMipChain* Result)
    {
        __try
        {
            return SnapshotResidentIconMipChainUnsafe(
                *PlatformData,
                *Result);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *Result = {};
            return false;
        }
    }

    static bool TryDetectResidentIconPlatformData(
        const void* Texture,
        FPlatformData* PlatformData)
    {
        __try
        {
            return DetectPlatformData(
                Texture,
                *PlatformData);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *PlatformData = {};
            return false;
        }
    }

    static bool IsPointerInResidentIconRange(
        const uint8_t* Pointer,
        const uint8_t* Start,
        size_t Size)
    {
        if (!Pointer || !Start || !Size)
            return false;
        const uintptr_t Value =
            reinterpret_cast<uintptr_t>(Pointer);
        const uintptr_t Begin =
            reinterpret_cast<uintptr_t>(Start);
        const uintptr_t End =
            Begin + Size;
        return End >= Begin &&
            Value >= Begin &&
            Value < End;
    }

    static bool IsResidentIconMipChainCurrentUnsafe(
        const FPlatformData& PlatformData,
        const FResidentIconMipChain& Chain)
    {
        if (!Chain.Header ||
            !Chain.Data ||
            Chain.Num < 1 ||
            Chain.Num > kMaximumResidentIconMips ||
            !Chain.MipScanBytes ||
            !IsReadable(Chain.Header, 16) ||
            *reinterpret_cast<
                const uint8_t* const*>(
                    Chain.Header) != Chain.Data ||
            *reinterpret_cast<const int32*>(
                Chain.Header + 8) != Chain.Num ||
            *reinterpret_cast<const int32*>(
                Chain.Header + 12) != Chain.Max ||
            (Chain.MipsAreIndirect &&
             !IsReadable(
                 Chain.Data,
                 static_cast<size_t>(Chain.Num) *
                    sizeof(const uint8_t*))))
        {
            return false;
        }

        for (int32 Index = 0;
             Index < Chain.Num;
             ++Index)
        {
            const uint8_t* CurrentMip =
                Chain.MipsAreIndirect
                    ? reinterpret_cast<
                        const uint8_t* const*>(
                            Chain.Data)[Index]
                    : Chain.Data +
                        Chain.MipScanBytes *
                            static_cast<size_t>(Index);
            if (CurrentMip != Chain.Mips[Index] ||
                !IsReadable(
                    CurrentMip,
                    Chain.MipScanBytes))
            {
                return false;
            }
        }
        return IsResidentIconMipLayout(
            PlatformData,
            Chain,
            Chain.DimensionOffset,
            Chain.DimensionsAre16Bit);
    }

    static int ResidentIconBulkSizeScore(
        const uint8_t* Mip,
        uint16_t DimensionOffset,
        bool DimensionsAre16Bit,
        uint32_t PointerOffset,
        size_t MipScanBytes,
        size_t ExpectedBytes)
    {
        const uint32_t DimensionBytes =
            DimensionsAre16Bit
                ? sizeof(uint16_t) * 3
                : sizeof(uint32_t) * 3;
        auto Overlaps = [](
            uint32_t LeftOffset,
            uint32_t LeftSize,
            uint32_t RightOffset,
            uint32_t RightSize)
        {
            return LeftOffset <
                    RightOffset + RightSize &&
                RightOffset <
                    LeftOffset + LeftSize;
        };

        int Score = 0;
        for (uint32_t Offset = 0;
             Offset + sizeof(uint32_t) <=
                 MipScanBytes;
             Offset += 4)
        {
            if (Overlaps(
                    Offset,
                    sizeof(uint32_t),
                    DimensionOffset,
                    DimensionBytes) ||
                Overlaps(
                    Offset,
                    sizeof(uint32_t),
                    PointerOffset,
                    sizeof(const uint8_t*)))
            {
                continue;
            }
            const uint32_t Value =
                *reinterpret_cast<const uint32_t*>(
                    Mip + Offset);
            if (Value == ExpectedBytes)
            {
                Score = (std::max)(
                    Score,
                    (Offset ==
                             PointerOffset +
                                 sizeof(const uint8_t*) ||
                     Offset + sizeof(uint32_t) ==
                         PointerOffset)
                        ? 7
                        : 3);
            }
        }
        for (uint32_t Offset = 0;
             Offset + sizeof(uint64_t) <=
                 MipScanBytes;
             Offset += 8)
        {
            if (Overlaps(
                    Offset,
                    sizeof(uint64_t),
                    DimensionOffset,
                    DimensionBytes) ||
                Overlaps(
                    Offset,
                    sizeof(uint64_t),
                    PointerOffset,
                    sizeof(const uint8_t*)))
            {
                continue;
            }
            const uint64_t Value =
                *reinterpret_cast<const uint64_t*>(
                    Mip + Offset);
            if (Value == ExpectedBytes)
            {
                Score = (std::max)(
                    Score,
                    (Offset ==
                             PointerOffset +
                                 sizeof(const uint8_t*) ||
                     Offset + sizeof(uint64_t) ==
                         PointerOffset)
                        ? 8
                        : 4);
            }
        }
        return Score;
    }

    static bool FindResidentIconPayloadUnsafe(
        const FPlatformData& PlatformData,
        const FResidentIconMipChain& Chain,
        int32 MipIndex,
        FResidentIconPayload& Result)
    {
        Result = {};
        if (MipIndex < 0 ||
            MipIndex >= Chain.Num ||
            !IsResidentIconMipChainCurrentUnsafe(
                PlatformData, Chain))
        {
            return false;
        }

        int32 Width = 0;
        int32 Height = 0;
        int32 Depth = 0;
        const uint8_t* Mip =
            Chain.Mips[MipIndex];
        if (!ReadResidentIconMipDimensions(
                Mip,
                Chain.DimensionOffset,
                Chain.DimensionsAre16Bit,
                Width,
                Height,
                Depth) ||
            Width !=
                (std::max)(
                    1,
                    PlatformData.SizeX >> MipIndex) ||
            Height !=
                (std::max)(
                    1,
                    PlatformData.SizeY >> MipIndex) ||
            Depth != 1)
        {
            return false;
        }

        const size_t PixelCount =
            static_cast<size_t>(Width) *
            Height;
        const size_t ExpectedBytes =
            FormatBytes(
                PlatformData.PixelFormat,
                Width,
                Height);
        if (!ExpectedBytes ||
            ExpectedBytes >
                kMaximumResidentIconBytes ||
            PixelCount >
                kMaximumResidentIconBytes / 4)
        {
            return false;
        }

        const uint8_t* Best = nullptr;
        int BestScore = 0;
        bool Ambiguous = false;
        for (uint32_t Offset = 0;
             Offset + sizeof(const uint8_t*) <=
                 Chain.MipScanBytes;
             Offset += 8)
        {
            const uint32_t DimensionBytes =
                Chain.DimensionsAre16Bit
                    ? sizeof(uint16_t) * 3
                    : sizeof(uint32_t) * 3;
            if (Offset <
                    Chain.DimensionOffset +
                        DimensionBytes &&
                Chain.DimensionOffset <
                    Offset +
                        sizeof(const uint8_t*))
            {
                continue;
            }
            const uint8_t* Candidate =
                *reinterpret_cast<
                    const uint8_t* const*>(
                        Mip + Offset);
            if (!Candidate ||
                (reinterpret_cast<
                    uintptr_t>(Candidate) & 7) ||
                StrictResidentIconPayloadRegionSize(Candidate) <
                    ExpectedBytes ||
                StartsWithImagePointer(Candidate) ||
                IsPointerInResidentIconRange(
                    Candidate,
                    PlatformData.Ptr,
                    0x100) ||
                IsPointerInResidentIconRange(
                    Candidate,
                    Chain.Data,
                    Chain.ArrayStorageBytes))
            {
                continue;
            }

            bool PointsIntoMip = false;
            for (int32 Index = 0;
                 Index < Chain.Num;
                 ++Index)
            {
                if (IsPointerInResidentIconRange(
                        Candidate,
                        Chain.Mips[Index],
                        Chain.MipScanBytes))
                {
                    PointsIntoMip = true;
                    break;
                }
            }
            if (PointsIntoMip)
                continue;

            const int Score =
                ResidentIconBulkSizeScore(
                    Mip,
                    Chain.DimensionOffset,
                    Chain.DimensionsAre16Bit,
                    Offset,
                    Chain.MipScanBytes,
                    ExpectedBytes);
            if (!Score)
                continue;
            if (Score > BestScore)
            {
                Best = Candidate;
                BestScore = Score;
                Ambiguous = false;
            }
            else if (Score == BestScore &&
                     Best != Candidate)
            {
                Ambiguous = true;
            }
        }
        if (!Best || Ambiguous)
            return false;

        Result.Bytes = Best;
        Result.MipIndex = MipIndex;
        Result.Width = Width;
        Result.Height = Height;
        Result.ByteCount = ExpectedBytes;
        return true;
    }

    static bool TryFindResidentIconPayload(
        const FPlatformData* PlatformData,
        const FResidentIconMipChain* Chain,
        int32 MipIndex,
        FResidentIconPayload* Result)
    {
        __try
        {
            return FindResidentIconPayloadUnsafe(
                *PlatformData,
                *Chain,
                MipIndex,
                *Result);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            *Result = {};
            return false;
        }
    }

    static bool CopyResidentIconPayloadUnsafe(
        const FPlatformData& PlatformData,
        const FResidentIconMipChain& Chain,
        const FResidentIconPayload& Expected,
        uint8_t* Destination)
    {
        if (!Destination ||
            !Expected.Bytes ||
            !Expected.ByteCount)
        {
            return false;
        }

        FResidentIconPayload Before;
        if (!FindResidentIconPayloadUnsafe(
                PlatformData,
                Chain,
                Expected.MipIndex,
                Before) ||
            Before.Bytes != Expected.Bytes ||
            Before.ByteCount != Expected.ByteCount)
        {
            return false;
        }

        memcpy(
            Destination,
            Expected.Bytes,
            Expected.ByteCount);

        FResidentIconPayload After;
        if (!FindResidentIconPayloadUnsafe(
                PlatformData,
                Chain,
                Expected.MipIndex,
                After) ||
            After.Bytes != Expected.Bytes ||
            After.ByteCount != Expected.ByteCount)
        {
            return false;
        }
        return true;
    }

    static bool TryCopyResidentIconPayload(
        const FPlatformData* PlatformData,
        const FResidentIconMipChain* Chain,
        const FResidentIconPayload* Expected,
        uint8_t* Destination)
    {
        __try
        {
            return CopyResidentIconPayloadUnsafe(
                *PlatformData,
                *Chain,
                *Expected,
                Destination);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    static bool ExtractResidentIconToRGBA(
        const void* Texture,
        std::vector<unsigned char>& RGBA,
        int& Width,
        int& Height)
    {
        FPlatformData PlatformData;
        if (!TryDetectResidentIconPlatformData(
                Texture,
                &PlatformData) ||
            !IsKnownFormat(
                PlatformData.PixelFormat))
        {
            return false;
        }

        FResidentIconMipChain Chain;
        if (!TrySnapshotResidentIconMipChain(
                &PlatformData,
                &Chain))
        {
            return false;
        }

        for (int32 Index = 0;
             Index < Chain.Num;
             ++Index)
        {
            const int32 MipWidth =
                (std::max)(
                    1,
                    PlatformData.SizeX >> Index);
            const int32 MipHeight =
                (std::max)(
                    1,
                    PlatformData.SizeY >> Index);
            if (MipWidth >
                    kMaximumResidentIconDimension ||
                MipHeight >
                    kMaximumResidentIconDimension)
            {
                continue;
            }

            FResidentIconPayload Payload;
            if (!TryFindResidentIconPayload(
                    &PlatformData,
                    &Chain,
                    Index,
                    &Payload))
            {
                continue;
            }

            std::vector<unsigned char> Encoded(
                Payload.ByteCount);
            if (!TryCopyResidentIconPayload(
                    &PlatformData,
                    &Chain,
                    &Payload,
                    Encoded.data()))
            {
                continue;
            }
            return DecodeTextureBytes(
                Encoded.data(),
                PlatformData.PixelFormat,
                Payload.Width,
                Payload.Height,
                RGBA,
                Width,
                Height);
        }
        return false;
    }

    static bool ExtractResidentIconToRGBA_Guarded(
        const void* Texture,
        std::vector<unsigned char>& RGBA,
        int& Width,
        int& Height)
    {
        __try
        {
            return ExtractResidentIconToRGBA(
                Texture,
                RGBA,
                Width,
                Height);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    // ------------------------------------------------------------------------
    // Game-thread load bridge. UE's loader (StaticLoadObject) is game-thread-
    // only: calling it from the GUI thread is what faulted on e.g. 17.30/27.x.
    // The GUI thread only POSTS a request; UNetDriver::TickFlush (game thread)
    // drains it, loads + extracts the pixels, and the GUI picks them up on a
    // later Acquire retry (the editor already re-polls every ~3s).
    // ------------------------------------------------------------------------
    enum class LoadState : int { Idle = 0, Requested, Ready, Failed, Consumed };
    static std::atomic<int> g_LoadState{ (int)LoadState::Idle };
    static std::atomic<int> g_LoadAttempts{ 0 };
    static std::vector<unsigned char> g_LoadedRGBA; // written under Requested, read under Ready
    static int g_LoadedW = 0, g_LoadedH = 0;

    static const UTexture2D* StaticLoadMinimapSEH(const wchar_t* path, const UClass* texClass)
    {
        __try
        {
            return (const UTexture2D*)SDK::StaticLoadObject(path, texClass);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            SDK::DbgLog("[SafeZoneMap] StaticLoadObject(%ls) faulted (SEH)\n", path);
            return nullptr;
        }
    }

    static int MinimapPathsForVersion(const wchar_t** out, int cap); // fwd
    static const UTexture2D* FindLoadedMinimapTexture(const wchar_t** paths, int np); // fwd
    static constexpr int kMaxMinimapPaths = 12;

    struct MapPoint
    {
        double U = 0.0;
        double V = 0.0;
    };

    static bool ReadMapPoint(const uint8_t* buffer, size_t bufferSize,
                             uint32_t offset, MapPoint& out)
    {
        if (!buffer)
            return false;

        if (VersionInfo.FortniteVersion >= 20.00f)
        {
            if ((size_t)offset + sizeof(double) * 2 > bufferSize)
                return false;
            out.U = *(const double*)(buffer + offset);
            out.V = *(const double*)(buffer + offset + sizeof(double));
        }
        else
        {
            if ((size_t)offset + sizeof(float) * 2 > bufferSize)
                return false;
            out.U = *(const float*)(buffer + offset);
            out.V = *(const float*)(buffer + offset + sizeof(float));
        }
        return std::isfinite(out.U) && std::isfinite(out.V);
    }

    static bool CallWorldToMapUnsafe(const UObject* manager, UFunction* function,
                                     const FVector& worldLocation, MapPoint& out)
    {
        if (!manager || !function)
            return false;

        auto params = function->GetParamsNamed();
        size_t bufferSize = VersionInfo.FortniteVersion >= 32.00f
            ? 0x1000
            : (size_t)(params.Size > 0 ? params.Size : 0x100);
        if (bufferSize < 0x100)
            bufferSize = 0x100;
        if (bufferSize > 0x10000)
            return false;

        for (auto& param : params.NameOffsetMap)
        {
            if (param.Offset > 0x10000)
                return false;
            const size_t required = (size_t)param.Offset + 0x20;
            if (required > bufferSize)
                bufferSize = required;
        }
        if (bufferSize > 0x10000)
            return false;

        std::vector<uint8_t> buffer(bufferSize, 0);
        bool wroteInput = false;
        uint32_t returnOffset = UINT32_MAX;
        uint32_t namedOutputOffset = UINT32_MAX;
        uint32_t flaggedOutputOffset = UINT32_MAX;

        for (auto& param : params.NameOffsetMap)
        {
            const bool isWorldInput =
                param.Name == "WorldLocation" ||
                param.Name == "InWorldLocation" ||
                param.Name.find("WorldLocation") != UEAllocatedString::npos;
            if (isWorldInput && !wroteInput)
            {
                const size_t vectorSize = (size_t)FVector::Size();
                if ((size_t)param.Offset + vectorSize > buffer.size())
                    return false;
                memcpy(buffer.data() + param.Offset, &worldLocation, vectorSize);
                wroteInput = true;
                continue;
            }

            // The native helper scales its result by this caller-supplied widget
            // size. Zero produces (0,0) for every world position, which made the
            // calibration silently fall back to the approximate version bounds.
            // A size of one makes the result a normalized texture coordinate.
            if (param.Name == "InMapSize" || param.Name == "MapSize")
            {
                if ((size_t)param.Offset + sizeof(float) > buffer.size())
                    return false;
                *(float*)(buffer.data() + param.Offset) = 1.0f;
                continue;
            }

            if (param.Name == "ReturnValue")
                returnOffset = param.Offset;
            else if (param.Name.find("MapLocation") != UEAllocatedString::npos)
                namedOutputOffset = param.Offset;
            else if ((param.PropertyFlags & 0x100) != 0 ||
                     (param.PropertyFlags & 0x400) != 0)
                flaggedOutputOffset = param.Offset;
        }
        if (!wroteInput)
            return false;

        manager->ProcessEvent(function, buffer.data());

        if (returnOffset != UINT32_MAX &&
            ReadMapPoint(buffer.data(), buffer.size(), returnOffset, out))
            return true;
        if (namedOutputOffset != UINT32_MAX &&
            ReadMapPoint(buffer.data(), buffer.size(), namedOutputOffset, out))
            return true;
        return flaggedOutputOffset != UINT32_MAX &&
            ReadMapPoint(buffer.data(), buffer.size(), flaggedOutputOffset, out);
    }

    static bool CallWorldToMap(const UObject* manager, UFunction* function,
                               const FVector& worldLocation, MapPoint& out)
    {
        __try
        {
            return CallWorldToMapUnsafe(manager, function, worldLocation, out);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            SDK::DbgLog("[SafeZoneMap] BPWorldLocationToMapLocation faulted (SEH)\n");
            return false;
        }
    }

    static bool ReadMapCenterUnsafe(const AFortAthenaMapInfo* mapInfo, FVector& out)
    {
        out = mapInfo->GetMapCenter();
        return std::isfinite(out.X) && std::isfinite(out.Y) &&
            fabs(out.X) < 1000000.0 && fabs(out.Y) < 1000000.0;
    }

    static bool ReadMapCenter(const AFortAthenaMapInfo* mapInfo, FVector& out)
    {
        __try
        {
            return mapInfo && ReadMapCenterUnsafe(mapInfo, out);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            SDK::DbgLog("[SafeZoneMap] GetMapCenter faulted (SEH)\n");
            return false;
        }
    }

    static int ReadMapLayerSizeUnsafe(const UObject* preferredManager)
    {
        auto readFrom = [](const UObject* object) -> int
        {
            if (!object || !object->Class)
                return 0;
            const uint32_t offset = object->GetOffset("MapLayerSize");
            if (offset == UINT32_MAX)
                return 0;
            const int value = GetFromOffset<int>(object, offset);
            return value >= 64 && value <= 8192 ? value : 0;
        };

        int result = readFrom(preferredManager);
        if (result)
            return result;

        // Older Athena versions create this Blueprint manager lazily (and
        // dedicated servers may never create an instance). Its CDO still holds
        // the cooked logical layer size for the currently loaded game version.
        const UObject* blueprintDefault = FindObject<UObject>(
            L"/Game/UI/IngameMap/UIMapManager.Default__UIMapManager_C");
        result = readFrom(blueprintDefault);
        if (result)
            return result;

        const UClass* managerClass = FindClass("FortInGameMapManager");
        if (managerClass)
            result = readFrom(managerClass->GetDefaultObj());
        return result;
    }

    static float ReadSceneCaptureWidthFromManagerUnsafe(
        const UObject* manager, const UObject*& captureClassOut,
        const UObject*& captureComponentOut)
    {
        captureClassOut = nullptr;
        captureComponentOut = nullptr;
        if (!manager || !manager->Class)
            return 0.f;

        const UObject* captureActor = nullptr;
        const uint32_t liveCaptureOffset = manager->GetOffset("SceneCapture");
        if (liveCaptureOffset != UINT32_MAX)
        {
            const UObject* candidate =
                GetFromOffset<UObject*>(manager, liveCaptureOffset);
            if (candidate && candidate->Class)
                captureActor = candidate;
        }

        const uint32_t captureClassOffset =
            manager->GetOffset("SceneCaptureClass");
        if (captureClassOffset != UINT32_MAX)
        {
            const UClass* captureClass =
                GetFromOffset<UClass*>(manager, captureClassOffset);
            if (captureClass)
            {
                captureClassOut = captureClass;
                if (!captureActor)
                    captureActor = captureClass->GetDefaultObj();
            }
        }
        if (!captureActor || !captureActor->Class)
            return 0.f;

        const uint32_t componentOffset =
            captureActor->GetOffset("CaptureComponent2D");
        if (componentOffset == UINT32_MAX)
            return 0.f;
        const UObject* component =
            GetFromOffset<UObject*>(captureActor, componentOffset);
        if (!component || !component->Class)
            return 0.f;
        captureComponentOut = component;

        const uint32_t orthoWidthOffset = component->GetOffset("OrthoWidth");
        if (orthoWidthOffset == UINT32_MAX)
            return 0.f;
        const float width = GetFromOffset<float>(component, orthoWidthOffset);
        return std::isfinite(width) && width >= 50000.f &&
            width <= 1000000.f ? width : 0.f;
    }

    static float ReadSceneCaptureWidthUnsafe(
        const UObject* preferredManager, const UObject*& managerOut,
        const UObject*& captureClassOut, const UObject*& captureComponentOut)
    {
        managerOut = nullptr;
        captureClassOut = nullptr;
        captureComponentOut = nullptr;

        const UObject* candidates[3]{
            preferredManager,
            FindObject<UObject>(
                L"/Game/UI/IngameMap/UIMapManager.Default__UIMapManager_C"),
            nullptr
        };
        const UClass* managerClass = FindClass("FortInGameMapManager");
        if (managerClass)
            candidates[2] = managerClass->GetDefaultObj();

        for (const UObject* candidate : candidates)
        {
            if (!candidate || !candidate->Class)
                continue;
            const UObject* captureClass = nullptr;
            const UObject* captureComponent = nullptr;
            const float width = ReadSceneCaptureWidthFromManagerUnsafe(
                candidate, captureClass, captureComponent);
            if (width > 0.f)
            {
                managerOut = candidate;
                captureClassOut = captureClass;
                captureComponentOut = captureComponent;
                return width;
            }
        }
        return 0.f;
    }

    static bool ReadAthenaMapBrushSizeUnsafe(const UObject* settings,
                                             float& width, float& height,
                                             const UObject*& resource)
    {
        width = height = 0.f;
        resource = nullptr;
        if (!settings || !settings->Class)
            return false;

        const uint32_t brushOffset = settings->GetOffset("AthenaMapImage");
        const UStruct* brushStruct =
            FindObject<UStruct>(L"/Script/SlateCore.SlateBrush");
        if (brushOffset == UINT32_MAX || !brushStruct)
            return false;

        const uint32_t imageSizeOffset = brushStruct->GetOffset("ImageSize");
        if (imageSizeOffset == UINT32_MAX)
            return false;

        // Slate's image size remains a pair of floats even on UE5 versions
        // where gameplay FVector2D changed to doubles.
        const uint8_t* brush = (const uint8_t*)settings + brushOffset;
        width = *(const float*)(brush + imageSizeOffset);
        height = *(const float*)(brush + imageSizeOffset + sizeof(float));

        const uint32_t resourceOffset = brushStruct->GetOffset("ResourceObject");
        if (resourceOffset != UINT32_MAX)
            resource = *(UObject* const*)(brush + resourceOffset);

        return std::isfinite(width) && std::isfinite(height) &&
            width >= 64.f && width <= 8192.f &&
            height >= 64.f && height <= 8192.f;
    }

    static bool ReadWorldSettingsTransformUnsafe(UWorld* world,
                                                  const UObject* preferredManager,
                                                  MapTransform& out,
                                                  const UObject*& settingsOut)
    {
        settingsOut = nullptr;
        if (!world || !world->HasPersistentLevel() || !world->PersistentLevel ||
            !world->PersistentLevel->Class)
            return false;

        UObject* level = world->PersistentLevel;
        const uint32_t worldSettingsOffset = level->GetOffset("WorldSettings");
        if (worldSettingsOffset == UINT32_MAX)
            return false;

        UObject* settings = GetFromOffset<UObject*>(level, worldSettingsOffset);
        if (!settings || !settings->Class)
            return false;

        const uint32_t centerOffset = settings->GetOffset("PvPMapWorldCenter");
        if (centerOffset == UINT32_MAX)
            return false;

        const uint8_t* centerBytes = (const uint8_t*)settings + centerOffset;
        double centerX = 0.0;
        double centerY = 0.0;
        if (VersionInfo.FortniteVersion >= 20.00f)
        {
            centerX = *(const double*)(centerBytes + 0);
            centerY = *(const double*)(centerBytes + 8);
        }
        else
        {
            centerX = *(const float*)(centerBytes + 0);
            centerY = *(const float*)(centerBytes + 4);
        }
        if (!std::isfinite(centerX) || !std::isfinite(centerY) ||
            fabs(centerX) > 1000000.0 || fabs(centerY) > 1000000.0)
            return false;

        MapTransform result = DefaultTransformForVersion();
        // Original Athena reports the gameplay origin here (0,0), while the
        // 2048 minimap capture itself is offset. Keep the capture center
        // recovered from Fortnite's native converter for that map family.
        if (!UsesLegacyAthenaCapture() ||
            fabs(centerX) > 1.0 || fabs(centerY) > 1.0)
        {
            result.CenterX = (float)centerX;
            result.CenterY = (float)centerY;
        }

        float extentU = AxisULength(result);
        float extentV = AxisVLength(result);
        float width = 0.f;
        float height = 0.f;
        float mapWorldScale = 0.f;
        const uint32_t widthOffset = settings->GetOffset("PvPMapWorldWidth");
        const uint32_t heightOffset = settings->GetOffset("PvPMapWorldHeight");
        const uint32_t scaleOffset = settings->GetOffset("MapWorldScale");
        if (widthOffset != UINT32_MAX)
            width = GetFromOffset<float>(settings, widthOffset);
        if (heightOffset != UINT32_MAX)
            height = GetFromOffset<float>(settings, heightOffset);
        if (scaleOffset != UINT32_MAX)
            mapWorldScale = GetFromOffset<float>(settings, scaleOffset);
        const bool validWidth =
            std::isfinite(width) && width >= 50000.f && width <= 1000000.f;
        const bool validHeight =
            std::isfinite(height) && height >= 50000.f && height <= 1000000.f;
        if (validWidth)
            extentU = width * 0.5f;
        if (validHeight)
            extentV = height * 0.5f;

        float brushWidth = 0.f;
        float brushHeight = 0.f;
        const UObject* brushResource = nullptr;
        const bool haveBrushSize =
            ReadAthenaMapBrushSizeUnsafe(settings, brushWidth, brushHeight, brushResource);
        const int mapLayerSize = ReadMapLayerSizeUnsafe(preferredManager);
        const UObject* captureManager = nullptr;
        const UObject* captureClass = nullptr;
        const UObject* captureComponent = nullptr;
        const float sceneCaptureWidth = ReadSceneCaptureWidthUnsafe(
            preferredManager, captureManager, captureClass, captureComponent);

        // MapWorldScale is centimeters per logical map-layer unit, not per
        // source-texture pixel. Chapter 1 proves the distinction: its source
        // image is 2048 px, while Fortnite converts locations on an 896-unit
        // layer at 290 cm/unit (about 259.8 km total). Multiplying by 2048 made
        // the editor's world span 2.285x too large and sent Season 7/8 zones
        // into the ocean.
        const float absoluteScale = fabsf(mapWorldScale);
        bool usedFullCaptureScale = false;
        if (std::isfinite(absoluteScale) &&
            absoluteScale >= 0.01f && absoluteScale <= 10000.f)
        {
            const float logicalWidth = mapLayerSize
                ? (float)mapLayerSize
                : (haveBrushSize ? brushWidth : 0.f);
            const float logicalHeight = mapLayerSize
                ? (float)mapLayerSize
                : (haveBrushSize ? brushHeight : 0.f);
            const float scaledExtentU = absoluteScale * logicalWidth * 0.5f;
            const float scaledExtentV = absoluteScale * logicalHeight * 0.5f;
            if (scaledExtentU >= 25000.f && scaledExtentU <= 500000.f &&
                scaledExtentV >= 25000.f && scaledExtentV <= 500000.f)
            {
                extentU = scaledExtentU;
                extentV = scaledExtentV;
                usedFullCaptureScale = true;
            }
        }

        // Later managers may store the already-normalized world-to-map scale.
        if (!usedFullCaptureScale &&
            std::isfinite(mapWorldScale) && absoluteScale > 1e-9f)
        {
            const float scaleExtent = 0.5f / absoluteScale;
            if (scaleExtent >= 25000.f && scaleExtent <= 500000.f)
            {
                if (!validWidth)
                    extentU = scaleExtent;
                if (!validHeight)
                    extentV = scaleExtent;
            }
        }

        // The orthographic capture width is useful only when the logical
        // scale/layer pair is unavailable. A class-default capture can target a
        // different render layer, so it must not override Fortnite's own
        // MapWorldScale * MapLayerSize conversion.
        if (!usedFullCaptureScale &&
            sceneCaptureWidth >= 50000.f &&
            sceneCaptureWidth <= 1000000.f)
        {
            extentU = sceneCaptureWidth * 0.5f;
            extentV = sceneCaptureWidth * 0.5f;
        }

        // Read the capture orientation supplied by this map. At zero yaw the
        // legacy Athena basis is image-right=world +Y, image-bottom=world -X.
        float reportedMapYaw = 0.f;
        const uint32_t rotationOffset = settings->GetOffset("MapRotation");
        if (rotationOffset != UINT32_MAX)
        {
            const uint8_t* rotationBytes = (const uint8_t*)settings + rotationOffset;
            const double candidateYaw = VersionInfo.FortniteVersion >= 20.00f
                ? *(const double*)(rotationBytes + 8)
                : *(const float*)(rotationBytes + 4);
            if (std::isfinite(candidateYaw) && fabs(candidateYaw) <= 36000.0)
                reportedMapYaw = (float)candidateYaw;
        }

        // Rotate the legacy Athena image basis in the world plane. Newer
        // versions that expose BPWorldLocationToMapLocation are sampled
        // natively below, so this is only their safe fallback.
        const float yawRadians = reportedMapYaw * 0.01745329251994329577f;
        const float cosYaw = cosf(yawRadians);
        const float sinYaw = sinf(yawRadians);
        result.AxisUX = -sinYaw * extentU;
        result.AxisUY = cosYaw * extentU;
        result.AxisVX = -cosYaw * extentV;
        result.AxisVY = -sinYaw * extentV;

        static const UObject* loggedSettings = nullptr;
        static int loggedLayerSize = 0;
        static float loggedScale = 0.f;
        static float loggedSceneCaptureWidth = -1.f;
        if (loggedSettings != settings || loggedLayerSize != mapLayerSize ||
            fabsf(loggedScale - mapWorldScale) > 0.000001f ||
            fabsf(loggedSceneCaptureWidth - sceneCaptureWidth) > 1.f)
        {
            SDK::DbgLog(
                "[SafeZoneMap] WorldSettings map center=(%.1f, %.1f) capture=(%.1f, %.1f) playable=(%.1f, %.1f) layer=%d brush=(%.1f, %.1f) resource=%p sceneOrtho=%.1f captureManager=%p captureClass=%p captureComponent=%p projection=runtime-yaw yaw=%.2f scale=%.9f\n",
                result.CenterX, result.CenterY, extentU * 2.f, extentV * 2.f,
                width, height, mapLayerSize, brushWidth, brushHeight,
                (const void*)brushResource, sceneCaptureWidth,
                (const void*)captureManager, (const void*)captureClass,
                (const void*)captureComponent, reportedMapYaw, mapWorldScale);
            loggedSettings = settings;
            loggedLayerSize = mapLayerSize;
            loggedScale = mapWorldScale;
            loggedSceneCaptureWidth = sceneCaptureWidth;
        }
        out = result;
        settingsOut = settings;
        return true;
    }

    static bool ReadWorldSettingsTransform(UWorld* world,
                                           const UObject* preferredManager,
                                           MapTransform& out,
                                           const UObject*& settingsOut)
    {
        __try
        {
            return ReadWorldSettingsTransformUnsafe(
                world, preferredManager, out, settingsOut);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            SDK::DbgLog("[SafeZoneMap] WorldSettings map transform faulted (SEH)\n");
            settingsOut = nullptr;
            return false;
        }
    }

    struct PoiCalibrationAnchor
    {
        double RawU = 0.0;
        double RawV = 0.0;
        double WorldX = 0.0;
        double WorldY = 0.0;
        FName Tag;
    };

    struct PoiCalibrationPoint
    {
        double U = 0.0;
        double V = 0.0;
        double WorldX = 0.0;
        double WorldY = 0.0;
    };

    struct PoiAffineFit
    {
        double X[3]{};
        double Y[3]{};
        double Rms = 0.0;
        int Inliers = 0;
    };

    struct PoiPositionNormalization
    {
        const char* Name = nullptr;
        double OffsetU = 0.0;
        double OffsetV = 0.0;
        double ScaleU = 1.0;
        double ScaleV = 1.0;
    };

    static FName ReadGameplayTagName(const uint8_t* bytes)
    {
        FName result;
        result.ComparisonIndex = *(const int32*)bytes;
        if (VersionInfo.FortniteVersion < 20.00f)
            result.Number = *(const int32*)(bytes + sizeof(int32));
        return result;
    }

    static bool GameplayTagArrayContains(const TArray<FGameplayTag>& tags,
                                         const FName& wanted)
    {
        const int count = tags.Num();
        const int maximum = tags.Max();
        const int tagSize = FGameplayTag::Size();
        if (count <= 0 || count > 128 || maximum < count || maximum > 4096 ||
            !tags.Data || !SDK::MemReadable(tags.Data, (size_t)count * tagSize))
            return false;

        for (int i = 0; i < count; ++i)
        {
            const uint8_t* tagBytes =
                (const uint8_t*)tags.Data + (size_t)i * tagSize;
            if (ReadGameplayTagName(tagBytes) == wanted)
                return true;
        }
        return false;
    }

    static bool GameplayTagContainerContains(const FGameplayTagContainer* tags,
                                             const FName& wanted)
    {
        if (!tags || !SDK::MemReadable(tags, sizeof(FGameplayTagContainer)))
            return false;
        return GameplayTagArrayContains(tags->GameplayTags, wanted) ||
            GameplayTagArrayContains(tags->ParentTags, wanted);
    }

    static bool IsUsablePoiLocation(const FVector& location)
    {
        return std::isfinite(location.X) && std::isfinite(location.Y) &&
            fabs(location.X) <= 1000000.0 &&
            fabs(location.Y) <= 1000000.0 &&
            // A retained NullRHI POI actor can have an uninitialized root at
            // the world origin. No named Athena POI is actually centered there.
            fabs(location.X) + fabs(location.Y) > 100.0;
    }

    static bool ReadPoiVolumeLocationUnsafe(AActor* volume, FVector& out)
    {
        if (!volume || !volume->Class)
            return false;

        // FortPoiVolume's actor/root transform is commonly zero on old
        // dedicated NullRHI builds. The collision component still retains the
        // cooked level transform used to define the POI footprint.
        const uint32_t collisionOffset =
            volume->GetOffset("PoiCollisionComp");
        if (collisionOffset != UINT32_MAX)
        {
            UActorComponent* collision =
                GetFromOffset<UActorComponent*>(volume, collisionOffset);
            if (collision && collision->Class)
            {
                const FVector location =
                    collision->K2_GetComponentToWorld().GetTranslation();
                if (IsUsablePoiLocation(location))
                {
                    out = FVector(location.X, location.Y, location.Z);
                    return true;
                }
            }
        }

        UActorComponent* root = volume->RootComponent;
        if (root && root->Class)
        {
            const FVector location =
                root->K2_GetComponentToWorld().GetTranslation();
            if (IsUsablePoiLocation(location))
            {
                out = FVector(location.X, location.Y, location.Z);
                return true;
            }
        }

        const FVector actorLocation = volume->K2_GetActorLocation();
        if (IsUsablePoiLocation(actorLocation))
        {
            out = FVector(
                actorLocation.X, actorLocation.Y, actorLocation.Z);
            return true;
        }
        return false;
    }

    static bool SolvePoi3x3(double matrix[3][3], const double rhs[3],
                            double result[3])
    {
        double augmented[3][4]{
            { matrix[0][0], matrix[0][1], matrix[0][2], rhs[0] },
            { matrix[1][0], matrix[1][1], matrix[1][2], rhs[1] },
            { matrix[2][0], matrix[2][1], matrix[2][2], rhs[2] }
        };

        for (int column = 0; column < 3; ++column)
        {
            int pivot = column;
            for (int row = column + 1; row < 3; ++row)
                if (fabs(augmented[row][column]) >
                    fabs(augmented[pivot][column]))
                    pivot = row;
            if (fabs(augmented[pivot][column]) < 1e-12)
                return false;
            if (pivot != column)
                for (int cell = column; cell < 4; ++cell)
                    std::swap(augmented[pivot][cell],
                              augmented[column][cell]);

            const double divisor = augmented[column][column];
            for (int cell = column; cell < 4; ++cell)
                augmented[column][cell] /= divisor;
            for (int row = 0; row < 3; ++row)
            {
                if (row == column)
                    continue;
                const double factor = augmented[row][column];
                for (int cell = column; cell < 4; ++cell)
                    augmented[row][cell] -= factor *
                        augmented[column][cell];
            }
        }

        result[0] = augmented[0][3];
        result[1] = augmented[1][3];
        result[2] = augmented[2][3];
        return std::isfinite(result[0]) && std::isfinite(result[1]) &&
            std::isfinite(result[2]);
    }

    static bool FitPoiAffine(const std::vector<PoiCalibrationPoint>& points,
                             const std::vector<int>& indices,
                             PoiAffineFit& out)
    {
        if (indices.size() < 3)
            return false;

        double normal[3][3]{};
        double rhsX[3]{};
        double rhsY[3]{};
        for (int index : indices)
        {
            if (index < 0 || index >= (int)points.size())
                return false;
            const PoiCalibrationPoint& point = points[index];
            const double basis[3]{ 1.0, point.U, point.V };
            for (int row = 0; row < 3; ++row)
            {
                rhsX[row] += basis[row] * point.WorldX;
                rhsY[row] += basis[row] * point.WorldY;
                for (int column = 0; column < 3; ++column)
                    normal[row][column] += basis[row] * basis[column];
            }
        }

        double normalCopy[3][3];
        memcpy(normalCopy, normal, sizeof(normal));
        if (!SolvePoi3x3(normal, rhsX, out.X) ||
            !SolvePoi3x3(normalCopy, rhsY, out.Y))
            return false;

        double squaredError = 0.0;
        for (int index : indices)
        {
            const PoiCalibrationPoint& point = points[index];
            const double predictedX =
                out.X[0] + out.X[1] * point.U + out.X[2] * point.V;
            const double predictedY =
                out.Y[0] + out.Y[1] * point.U + out.Y[2] * point.V;
            const double dx = predictedX - point.WorldX;
            const double dy = predictedY - point.WorldY;
            squaredError += dx * dx + dy * dy;
        }
        out.Rms = sqrt(squaredError / (double)indices.size());
        out.Inliers = (int)indices.size();
        return std::isfinite(out.Rms);
    }

    static bool FitRobustPoiAffine(
        const std::vector<PoiCalibrationPoint>& points, PoiAffineFit& out)
    {
        const int count = (int)points.size();
        if (count < 4)
            return false;

        constexpr double kInlierDistance = 15000.0;
        constexpr double kInlierDistanceSquared =
            kInlierDistance * kInlierDistance;
        std::vector<int> bestInliers;
        double bestSquaredError = DBL_MAX;

        // A few old maps contain nested POI volumes whose actor origin is not
        // the label center. Testing every three-anchor model prevents those
        // volumes from pulling the complete map projection off target.
        for (int first = 0; first < count - 2; ++first)
        {
            for (int second = first + 1; second < count - 1; ++second)
            {
                for (int third = second + 1; third < count; ++third)
                {
                    std::vector<int> seed{ first, second, third };
                    PoiAffineFit candidate;
                    if (!FitPoiAffine(points, seed, candidate))
                        continue;

                    std::vector<int> inliers;
                    double squaredError = 0.0;
                    for (int index = 0; index < count; ++index)
                    {
                        const PoiCalibrationPoint& point = points[index];
                        const double dx =
                            candidate.X[0] + candidate.X[1] * point.U +
                            candidate.X[2] * point.V - point.WorldX;
                        const double dy =
                            candidate.Y[0] + candidate.Y[1] * point.U +
                            candidate.Y[2] * point.V - point.WorldY;
                        const double error = dx * dx + dy * dy;
                        if (error <= kInlierDistanceSquared)
                        {
                            inliers.push_back(index);
                            squaredError += error;
                        }
                    }

                    if (inliers.size() > bestInliers.size() ||
                        (inliers.size() == bestInliers.size() &&
                         squaredError < bestSquaredError))
                    {
                        bestInliers = std::move(inliers);
                        bestSquaredError = squaredError;
                    }
                }
            }
        }

        const int minimumInliers = (std::max)(4, (count * 2 + 4) / 5);
        if ((int)bestInliers.size() < minimumInliers)
            return false;
        return FitPoiAffine(points, bestInliers, out);
    }

    static bool BuildPoiTransformForNormalization(
        const std::vector<PoiCalibrationAnchor>& anchors,
        const PoiPositionNormalization& normalization,
        MapTransform& transformOut, PoiAffineFit& fitOut,
        double& coverageOut)
    {
        if (normalization.ScaleU <= 0.0 || normalization.ScaleV <= 0.0)
            return false;

        std::vector<PoiCalibrationPoint> points;
        points.reserve(anchors.size());
        int inside = 0;
        double minU = DBL_MAX, minV = DBL_MAX;
        double maxU = -DBL_MAX, maxV = -DBL_MAX;
        for (const PoiCalibrationAnchor& anchor : anchors)
        {
            PoiCalibrationPoint point;
            point.U = normalization.OffsetU +
                anchor.RawU / normalization.ScaleU;
            point.V = normalization.OffsetV +
                anchor.RawV / normalization.ScaleV;
            point.WorldX = anchor.WorldX;
            point.WorldY = anchor.WorldY;
            if (!std::isfinite(point.U) || !std::isfinite(point.V))
                return false;
            if (point.U >= -0.10 && point.U <= 1.10 &&
                point.V >= -0.10 && point.V <= 1.10)
                ++inside;
            minU = (std::min)(minU, point.U);
            minV = (std::min)(minV, point.V);
            maxU = (std::max)(maxU, point.U);
            maxV = (std::max)(maxV, point.V);
            points.push_back(point);
        }

        if (inside < (std::max)(4, (int)anchors.size() * 3 / 4))
            return false;
        const double rangeU = maxU - minU;
        const double rangeV = maxV - minV;
        coverageOut = (std::min)(rangeU, rangeV);
        if (!std::isfinite(coverageOut) || coverageOut < 0.15)
            return false;

        PoiAffineFit fit;
        if (!FitRobustPoiAffine(points, fit))
            return false;

        MapTransform transform{
            (float)(fit.X[0] + fit.X[1] * 0.5 + fit.X[2] * 0.5),
            (float)(fit.Y[0] + fit.Y[1] * 0.5 + fit.Y[2] * 0.5),
            (float)(fit.X[1] * 0.5),
            (float)(fit.Y[1] * 0.5),
            (float)(fit.X[2] * 0.5),
            (float)(fit.Y[2] * 0.5)
        };
        const double extentU = AxisULength(transform);
        const double extentV = AxisVLength(transform);
        if (!std::isfinite(transform.CenterX) ||
            !std::isfinite(transform.CenterY) ||
            fabs(transform.CenterX) > 1000000.0 ||
            fabs(transform.CenterY) > 1000000.0 ||
            !std::isfinite(extentU) || !std::isfinite(extentV) ||
            extentU < 25000.0 || extentU > 500000.0 ||
            extentV < 25000.0 || extentV > 500000.0)
            return false;

        const double aspect = extentU / extentV;
        const double orthogonality = fabs(
            transform.AxisUX * transform.AxisVX +
            transform.AxisUY * transform.AxisVY) / (extentU * extentV);
        const double maximumRms =
            (std::max)(5000.0, (std::min)(18000.0,
                (std::min)(extentU, extentV) * 0.12));
        if (aspect < 0.5 || aspect > 2.0 || orthogonality > 0.35 ||
            fit.Rms > maximumRms)
            return false;

        transformOut = transform;
        fitOut = fit;
        return true;
    }

    static bool BuildPoiCalibrationUnsafe(UWorld* world,
                                          const UObject* settings,
                                          const UObject* preferredManager,
                                          MapTransform& out,
                                          bool& stableFailure)
    {
        stableFailure = false;
        if (!world || !settings || !settings->Class)
            return false;

        const UStruct* mapLocationStruct =
            FindObject<UStruct>(L"/Script/FortniteGame.MapLocation");
        const uint32_t mapLocationsOffset = settings->GetOffset("MapLocations");
        if (!mapLocationStruct || mapLocationsOffset == UINT32_MAX)
            return false;

        const int elementSize = mapLocationStruct->GetPropertiesSize();
        const uint32_t positionOffset =
            mapLocationStruct->GetOffset("Position");
        const uint32_t locationTagOffset =
            mapLocationStruct->GetOffset("LocationTag");
        const int vector2DSize =
            VersionInfo.FortniteVersion >= 20.00f ? 16 : 8;
        if (elementSize < 16 || elementSize > 4096 ||
            positionOffset == UINT32_MAX ||
            locationTagOffset == UINT32_MAX ||
            (int)positionOffset + vector2DSize > elementSize ||
            (int)locationTagOffset + FGameplayTag::Size() > elementSize)
        {
            static const UObject* loggedInvalidLayout = nullptr;
            if (loggedInvalidLayout != settings)
            {
                SDK::DbgLog(
                    "[SafeZoneMap] POI calibration unavailable: invalid MapLocation layout element=0x%x position=0x%x tag=0x%x\n",
                    elementSize, positionOffset, locationTagOffset);
                loggedInvalidLayout = settings;
            }
            return false;
        }

        const TArray<uint8_t>* mapLocations =
            (const TArray<uint8_t>*)((const uint8_t*)settings +
                                    mapLocationsOffset);
        const int mapLocationCount = mapLocations->Num();
        static const UObject* loggedMapArraySettings = nullptr;
        static int loggedMapArrayCount = -1;
        if (loggedMapArraySettings != settings ||
            loggedMapArrayCount != mapLocationCount)
        {
            SDK::DbgLog(
                "[SafeZoneMap] POI calibration MapLocations count=%d max=%d data=%p\n",
                mapLocationCount, mapLocations->Max(),
                (const void*)mapLocations->Data);
            loggedMapArraySettings = settings;
            loggedMapArrayCount = mapLocationCount;
        }
        if (mapLocationCount < 4 || mapLocationCount > 512 ||
            mapLocations->Max() < mapLocationCount ||
            mapLocations->Max() > 4096 || !mapLocations->Data ||
            !SDK::MemReadable(mapLocations->Data,
                (size_t)mapLocationCount * elementSize))
            return false;

        const UClass* poiVolumeClass = FindClass("FortPoiVolume");
        if (!poiVolumeClass)
            return false;
        std::vector<AActor*> volumes;
        TArray<AActor*> worldVolumes =
            UGameplayStatics::GetAllActorsOfClass(world, poiVolumeClass);
        if (worldVolumes.Num() > 0 && worldVolumes.Num() <= 2048 &&
            worldVolumes.Data)
        {
            volumes.reserve(worldVolumes.Num());
            for (int index = 0; index < worldVolumes.Num(); ++index)
                if (worldVolumes[index])
                    volumes.push_back(worldVolumes[index]);
        }
        worldVolumes.Free();

        // NullRHI can omit FortPoiVolume actors from the world's actor lists
        // even though FortPoiManager retains the authoritative POI pointers.
        // Those tagged volume centers let old versions recover the exact
        // texture-to-world affine transform without any season constants.
        int managerVolumeCount = 0;
        const UObject* poiManager = nullptr;
        if (world->GameState && world->GameState->Class)
        {
            const uint32_t poiManagerOffset =
                world->GameState->GetOffset("PoiManager");
            if (poiManagerOffset != UINT32_MAX)
                poiManager =
                    GetFromOffset<UObject*>(world->GameState, poiManagerOffset);
        }
        if (poiManager && poiManager->Class)
        {
            const uint32_t allVolumesOffset =
                poiManager->GetOffset("AllPoiVolumes");
            if (allVolumesOffset != UINT32_MAX)
            {
                const TArray<AActor*>* managerVolumes =
                    (const TArray<AActor*>*)((const uint8_t*)poiManager +
                                             allVolumesOffset);
                const int count = managerVolumes->Num();
                if (count > 0 && count <= 2048 &&
                    managerVolumes->Max() >= count &&
                    managerVolumes->Max() <= 4096 &&
                    managerVolumes->Data &&
                    SDK::MemReadable(managerVolumes->Data,
                        (size_t)count * sizeof(AActor*)))
                {
                    managerVolumeCount = count;
                    volumes.reserve(volumes.size() + count);
                    for (int index = 0; index < count; ++index)
                    {
                        AActor* volume = (*managerVolumes)[index];
                        if (volume &&
                            std::find(volumes.begin(), volumes.end(), volume) ==
                                volumes.end())
                            volumes.push_back(volume);
                    }
                }
            }
        }
        const int volumeCount = (int)volumes.size();
        static const UWorld* loggedVolumeWorld = nullptr;
        static int loggedVolumeCount = -1;
        if (loggedVolumeWorld != world || loggedVolumeCount != volumeCount)
        {
            SDK::DbgLog(
                "[SafeZoneMap] POI calibration FortPoiVolume unique=%d manager=%d managerObject=%p\n",
                volumeCount, managerVolumeCount, (const void*)poiManager);
            loggedVolumeWorld = world;
            loggedVolumeCount = volumeCount;
        }
        if (volumeCount < 4 || volumeCount > 2048)
            return false;

        std::vector<PoiCalibrationAnchor> anchors;
        anchors.reserve((std::min)(mapLocationCount, 64));
        for (int mapIndex = 0;
             mapIndex < mapLocationCount && anchors.size() < 64;
             ++mapIndex)
        {
            const uint8_t* entry =
                (const uint8_t*)mapLocations->Data +
                (size_t)mapIndex * elementSize;
            MapPoint rawPosition;
            if (!ReadMapPoint(entry, elementSize, positionOffset,
                              rawPosition))
                continue;
            const FName locationTag =
                ReadGameplayTagName(entry + locationTagOffset);
            if (!locationTag.IsValid())
                continue;

            double sumX = 0.0;
            double sumY = 0.0;
            int matches = 0;
            for (int volumeIndex = 0;
                 volumeIndex < volumeCount && volumeIndex < 2048;
                 ++volumeIndex)
            {
                AActor* volume = volumes[volumeIndex];
                if (!volume || !volume->Class)
                    continue;
                const uint32_t tagsOffset =
                    volume->GetOffset("LocationTags");
                if (tagsOffset == UINT32_MAX)
                    continue;
                const FGameplayTagContainer* tags =
                    (const FGameplayTagContainer*)((const uint8_t*)volume +
                                                   tagsOffset);
                if (!GameplayTagContainerContains(tags, locationTag))
                    continue;

                FVector location;
                if (!ReadPoiVolumeLocationUnsafe(volume, location))
                    continue;
                sumX += location.X;
                sumY += location.Y;
                ++matches;
            }

            if (matches > 0)
            {
                PoiCalibrationAnchor anchor;
                anchor.RawU = rawPosition.U;
                anchor.RawV = rawPosition.V;
                anchor.WorldX = sumX / matches;
                anchor.WorldY = sumY / matches;
                anchor.Tag = locationTag;
                anchors.push_back(anchor);
            }
        }
        static const UObject* loggedSettings = nullptr;
        static int loggedAnchorCount = -1;
        if (loggedSettings != settings ||
            loggedAnchorCount != (int)anchors.size())
        {
            SDK::DbgLog(
                "[SafeZoneMap] POI calibration metadata mapLocations=%d volumes=%d matched=%zu element=0x%x position=0x%x tag=0x%x\n",
                mapLocationCount, volumeCount, anchors.size(), elementSize,
                positionOffset, locationTagOffset);
            for (size_t i = 0; i < anchors.size() && i < 32; ++i)
            {
                const UEAllocatedString tag = anchors[i].Tag.ToString();
                SDK::DbgLog(
                    "[SafeZoneMap]   POI %s raw=(%.4f, %.4f) world=(%.1f, %.1f)\n",
                    tag.c_str(), anchors[i].RawU, anchors[i].RawV,
                    anchors[i].WorldX, anchors[i].WorldY);
            }
            loggedSettings = settings;
            loggedAnchorCount = (int)anchors.size();
        }
        if (anchors.size() < 4)
        {
            // At this point both the map metadata and the world's POI volumes
            // are populated. A missing tag intersection is a version/layout
            // incompatibility, not streaming that can become ready later.
            stableFailure = true;
            return false;
        }

        float brushWidth = 0.f;
        float brushHeight = 0.f;
        const UObject* brushResource = nullptr;
        ReadAthenaMapBrushSizeUnsafe(settings, brushWidth, brushHeight,
                                     brushResource);
        const int mapLayerSize =
            ReadMapLayerSizeUnsafe(preferredManager);

        std::vector<PoiPositionNormalization> normalizations;
        normalizations.push_back(
            { "normalized", 0.0, 0.0, 1.0, 1.0 });
        normalizations.push_back(
            { "normalized-centered", 0.5, 0.5, 1.0, 1.0 });
        if (mapLayerSize > 0)
        {
            normalizations.push_back(
                { "map-layer", 0.0, 0.0,
                  (double)mapLayerSize, (double)mapLayerSize });
            normalizations.push_back(
                { "map-layer-centered", 0.5, 0.5,
                  (double)mapLayerSize, (double)mapLayerSize });
        }
        if (brushWidth >= 64.f && brushHeight >= 64.f &&
            (!mapLayerSize ||
             fabsf(brushWidth - mapLayerSize) > 1.f ||
             fabsf(brushHeight - mapLayerSize) > 1.f))
        {
            normalizations.push_back(
                { "brush-pixels", 0.0, 0.0,
                  brushWidth, brushHeight });
            normalizations.push_back(
                { "brush-pixels-centered", 0.5, 0.5,
                  brushWidth, brushHeight });
        }

        bool found = false;
        MapTransform bestTransform;
        PoiAffineFit bestFit;
        const char* bestName = nullptr;
        double bestCoverage = -1.0;
        for (const PoiPositionNormalization& normalization :
             normalizations)
        {
            MapTransform candidateTransform;
            PoiAffineFit candidateFit;
            double candidateCoverage = 0.0;
            if (!BuildPoiTransformForNormalization(
                    anchors, normalization, candidateTransform,
                    candidateFit, candidateCoverage))
                continue;

            // Equivalent unit systems have the same residual. The one whose
            // POIs cover more of [0,1] is the texture coordinate system, while
            // dividing an already-logical coordinate by a larger source
            // texture clusters every label near the middle.
            if (!found || candidateFit.Inliers > bestFit.Inliers ||
                (candidateFit.Inliers == bestFit.Inliers &&
                 candidateFit.Rms < bestFit.Rms - 1.0) ||
                (candidateFit.Inliers == bestFit.Inliers &&
                 fabs(candidateFit.Rms - bestFit.Rms) <= 1.0 &&
                 candidateCoverage > bestCoverage))
            {
                found = true;
                bestTransform = candidateTransform;
                bestFit = candidateFit;
                bestName = normalization.Name;
                bestCoverage = candidateCoverage;
            }
        }
        if (!found)
            return false;

        SDK::DbgLog(
            "[SafeZoneMap] POI-calibrated projection units=%s anchors=%d rms=%.1f coverage=%.3f center=(%.1f, %.1f) axisU=(%.1f, %.1f) axisV=(%.1f, %.1f)\n",
            bestName ? bestName : "unknown", bestFit.Inliers, bestFit.Rms,
            bestCoverage, bestTransform.CenterX, bestTransform.CenterY,
            bestTransform.AxisUX, bestTransform.AxisUY,
            bestTransform.AxisVX, bestTransform.AxisVY);
        out = bestTransform;
        return true;
    }

    static bool BuildPoiCalibration(UWorld* world,
                                    const UObject* settings,
                                    const UObject* preferredManager,
                                    MapTransform& out)
    {
        static const UWorld* cachedWorld = nullptr;
        static const UObject* cachedSettings = nullptr;
        static MapTransform cachedTransform;
        static bool haveCachedTransform = false;
        static const UWorld* unsupportedWorld = nullptr;
        static const UObject* unsupportedSettings = nullptr;
        if (haveCachedTransform && cachedWorld == world &&
            cachedSettings == settings)
        {
            out = cachedTransform;
            return true;
        }
        if (unsupportedWorld == world && unsupportedSettings == settings)
            return false;

        bool stableFailure = false;
        __try
        {
            if (!BuildPoiCalibrationUnsafe(
                    world, settings, preferredManager, out, stableFailure))
            {
                if (stableFailure)
                {
                    unsupportedWorld = world;
                    unsupportedSettings = settings;
                    SDK::DbgLog(
                        "[SafeZoneMap] POI calibration unsupported for this world; using cached fallback\n");
                }
                return false;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            SDK::DbgLog("[SafeZoneMap] POI calibration faulted (SEH)\n");
            return false;
        }

        cachedWorld = world;
        cachedSettings = settings;
        cachedTransform = out;
        haveCachedTransform = true;
        return true;
    }

    static MapTransform TransformFromMapInfoCenter(const FVector& center)
    {
        MapTransform out = DefaultTransformForVersion();
        // GetMapCenter() is the gameplay origin on old Athena, not the center
        // of the full minimap capture. Replacing the native capture offset with
        // that (usually zero) value shifts every Chapter 1 selection.
        if (!UsesLegacyAthenaCapture())
        {
            out.CenterX = (float)center.X;
            out.CenterY = (float)center.Y;
        }
        return out;
    }

    static bool FinalizeSelectionForMapInfo(AFortAthenaMapInfo* mapInfo)
    {
        if (!mapInfo || !g_HasNormalizedSelection.load(std::memory_order_acquire))
            return false;

        FVector center;
        if (!ReadMapCenter(mapInfo, center))
        {
            SDK::DbgLog("[SafeZoneMap] could not resolve MapInfo center before applying custom zone\n");
            return false;
        }

        const MapTransform map = TransformFromMapInfoCenter(center);
        PublishTransform(map);
        ReprojectRememberedSelection(map);
        SDK::DbgLog(
            "[SafeZoneMap] finalized custom zone from MapInfo center=(%.1f, %.1f) axes=(%.1f, %.1f)\n",
            map.CenterX, map.CenterY, AxisULength(map), AxisVLength(map));
        return true;
    }

    static bool ReadRuntimeTransform(MapTransform& out, const UObject*& managerOut)
    {
        managerOut = nullptr;
        UWorld* world = UWorld::GetWorld();
        if (!world || !world->HasGameState() || !world->GameState)
            return false;

        // Do not query Athena-only reflected properties on the frontend/base
        // GameState. DEFINE_PROP caches a missing property as offset -1 globally;
        // doing that here before Athena_Terrain loads poisons later MapInfo reads.
        AActor* gameStateObject = world->GameState;
        const UClass* athenaGameStateClass = AFortGameStateAthena::StaticClass();
        if (!athenaGameStateClass || !gameStateObject->Class ||
            !gameStateObject->IsA(athenaGameStateClass))
            return false;

        AFortGameStateAthena* gameState = (AFortGameStateAthena*)gameStateObject;
        if (!gameState->HasMapInfo() || !gameState->MapInfo)
            return false;

        AFortAthenaMapInfo* mapInfo = gameState->MapInfo;
        const UClass* mapInfoClass = AFortAthenaMapInfo::StaticClass();
        if (!mapInfoClass || !mapInfo->Class || !mapInfo->IsA(mapInfoClass))
            return false;

        FVector center;
        if (!ReadMapCenter(mapInfo, center))
            return false;

        MapTransform fallbackTransform = TransformFromMapInfoCenter(center);
        const UObject* fallbackSource = mapInfo;

        // FortGameStateZone owns the map manager used by this match when one is
        // created. Dedicated/NullRHI servers commonly leave it null, but reading
        // this reflected pointer is more exact than a global object search.
        const UObject* manager = nullptr;
        const uint32_t uiMapManagerOffset = gameState->GetOffset("UIMapManager");
        if (uiMapManagerOffset != UINT32_MAX)
        {
            const UObject* candidate =
                GetFromOffset<UObject*>(gameState, uiMapManagerOffset);
            if (candidate && candidate->Class && !candidate->IsDefaultObject())
                manager = candidate;
        }

        // Some old clients do not publish UIMapManager on GameState. Prefer the
        // current world's actor before reading map dimensions so MapLayerSize
        // comes from the live version-specific manager whenever it exists.
        const UClass* managerClass = FindClass("FortInGameMapManager");
        if (managerClass && !manager)
        {
            TArray<AActor*> managers =
                UGameplayStatics::GetAllActorsOfClass(world, managerClass);
            manager = managers.Num() > 0 ? managers[0] : nullptr;
            managers.Free();
        }

        MapTransform worldSettingsTransform;
        const UObject* worldSettings = nullptr;
        if (ReadWorldSettingsTransform(
                world, manager, worldSettingsTransform, worldSettings))
        {
            // MapInfo belongs to this match and can carry a non-zero island
            // origin. Original Athena is the exception: its gameplay origin is
            // not the center of the full minimap capture.
            if (!UsesLegacyAthenaCapture())
            {
                worldSettingsTransform.CenterX = (float)center.X;
                worldSettingsTransform.CenterY = (float)center.Y;
            }
            fallbackTransform = worldSettingsTransform;
            fallbackSource = worldSettings;

            // Builds before BPWorldLocationToMapLocation exposed no callable
            // world-to-map helper. Their WorldSettings map labels and POI
            // volumes still share gameplay tags, though, so use those native
            // anchor pairs to recover this season's exact texture projection.
            MapTransform poiCalibratedTransform;
            if (BuildPoiCalibration(
                    world, worldSettings, manager, poiCalibratedTransform))
            {
                fallbackTransform = poiCalibratedTransform;
                fallbackSource = worldSettings;
            }
        }
        auto useFallbackTransform = [&]() -> bool
        {
            out = fallbackTransform;
            managerOut = fallbackSource;
            return true;
        };

        // Ask the current world's map manager for the same conversion its own
        // widgets use. A global first-object lookup can return a frontend/CDO
        // or a manager retained from the previous match, causing an otherwise
        // correct selection to move later. GetAllActorsOfClass scopes the
        // optional high-confidence sample to this UWorld.
        if (!managerClass)
            return useFallbackTransform();
        if (!manager || !manager->Class)
            return useFallbackTransform();
        UFunction* function = manager->GetFunction("BPWorldLocationToMapLocation");
        if (!function)
            return useFallbackTransform();

        static UFunction* loggedFunction = nullptr;
        if (loggedFunction != function)
        {
            auto params = function->GetParamsNamed();
            SDK::DbgLog("[SafeZoneMap] BPWorldLocationToMapLocation params size=0x%x count=%zu\n",
                params.Size, params.NameOffsetMap.size());
            for (auto& param : params.NameOffsetMap)
                SDK::DbgLog("[SafeZoneMap]   param %s off=0x%x flags=0x%llx elem=0x%x\n",
                    param.Name.c_str(), param.Offset,
                    (unsigned long long)param.PropertyFlags, param.ElementSize);
            loggedFunction = function;
        }

        constexpr double step = 10000.0;
        MapPoint p0, px, py;
        if (!CallWorldToMap(manager, function, center, p0) ||
            !CallWorldToMap(manager, function,
                FVector(center.X + step, center.Y, center.Z), px) ||
            !CallWorldToMap(manager, function,
                FVector(center.X, center.Y + step, center.Z), py))
        {
            static const UObject* loggedSampleFailure = nullptr;
            if (loggedSampleFailure != manager)
            {
                SDK::DbgLog("[SafeZoneMap] authoritative map sampling failed for manager=%p\n",
                    (const void*)manager);
                loggedSampleFailure = manager;
            }
            return useFallbackTransform();
        }

        const double duDx = (px.U - p0.U) / step;
        const double duDy = (py.U - p0.U) / step;
        const double dvDx = (px.V - p0.V) / step;
        const double dvDy = (py.V - p0.V) / step;
        if (!std::isfinite(duDx) || !std::isfinite(duDy) ||
            !std::isfinite(dvDx) || !std::isfinite(dvDy))
            return useFallbackTransform();

        const double det = duDx * dvDy - duDy * dvDx;
        if (fabs(det) < 1e-15)
            return useFallbackTransform();
        const double targetU = 0.5 - p0.U;
        const double targetV = 0.5 - p0.V;
        const double centerDeltaX = (targetU * dvDy - duDy * targetV) / det;
        const double centerDeltaY = (duDx * targetV - targetU * dvDx) / det;
        const double captureCenterX = center.X + centerDeltaX;
        const double captureCenterY = center.Y + centerDeltaY;
        if (!std::isfinite(captureCenterX) || !std::isfinite(captureCenterY) ||
            fabs(captureCenterX) > 1000000.0 || fabs(captureCenterY) > 1000000.0)
            return useFallbackTransform();

        // Invert the complete sampled world->map derivative. This preserves
        // rotation, axis signs, non-square scale, and any season-specific map
        // orientation. AxisU/V reach from center to an image edge, hence 0.5.
        const double inv00 = dvDy / det;
        const double inv01 = -duDy / det;
        const double inv10 = -dvDx / det;
        const double inv11 = duDx / det;
        MapTransform candidate{
            (float)captureCenterX, (float)captureCenterY,
            (float)(0.5 * inv00), (float)(0.5 * inv10),
            (float)(0.5 * inv01), (float)(0.5 * inv11)
        };
        const float extentU = AxisULength(candidate);
        const float extentV = AxisVLength(candidate);
        if (!std::isfinite(extentU) || !std::isfinite(extentV) ||
            extentU < 25000.f || extentU > 500000.f ||
            extentV < 25000.f || extentV > 500000.f)
            return useFallbackTransform();
        const float aspect = extentU / extentV;
        if (aspect < 0.4f || aspect > 2.5f)
            return useFallbackTransform();

        out = candidate;
        managerOut = manager;
        return true;
    }

    static void RefreshRuntimeTransform()
    {
        // Once gameplay starts the map projection is immutable and the chosen
        // safe-zone location has already been applied. Continuing to probe it
        // from the server tick only risks game-thread hitches on old builds.
        if (GUI::gsStatus >= StartedMatch)
            return;

        static uint32_t ticks = 0;
        static MapTransform last{};
        static const UObject* lastManager = nullptr;
        static bool haveLast = false;
        ++ticks;
        const uint32_t interval = haveLast ? 120 : 10;
        if (ticks % interval != 1)
            return;

        MapTransform current;
        const UObject* manager = nullptr;
        if (!ReadRuntimeTransform(current, manager)) return;
        const bool changed = !haveLast || manager != lastManager ||
            fabsf(current.CenterX - last.CenterX) > 1.f ||
            fabsf(current.CenterY - last.CenterY) > 1.f ||
            fabsf(current.AxisUX - last.AxisUX) > 1.f ||
            fabsf(current.AxisUY - last.AxisUY) > 1.f ||
            fabsf(current.AxisVX - last.AxisVX) > 1.f ||
            fabsf(current.AxisVY - last.AxisVY) > 1.f;
        if (!changed) return;

        const bool fromMapInfo = manager &&
            manager->IsA(AFortAthenaMapInfo::StaticClass());
        PublishTransform(current);
        ReprojectRememberedSelection(current);
        last = current;
        lastManager = manager;
        haveLast = true;
        SDK::DbgLog("[SafeZoneMap] %s map transform center=(%.1f, %.1f) axes=(%.1f, %.1f)\n",
            fromMapInfo ? "MapInfo" : "authoritative",
            current.CenterX, current.CenterY,
            AxisULength(current), AxisVLength(current));
    }

    // Called from the pre-Start GetMaxTickRate pump and server tick hooks; a
    // single atomic read unless a request is actually pending.
    static void GameThreadTick()
    {
        const bool bLoadRequested =
            g_LoadState.load(std::memory_order_acquire) == (int)LoadState::Requested;

        // Before Start, only service an explicit GUI load. Runtime transform
        // discovery needs an Athena GameState and cannot succeed yet.
        if (!FConfiguration::bReadyToStart)
        {
            if (!bLoadRequested)
                return;
        }
        else
            RefreshRuntimeTransform();

        if (!bLoadRequested)
            return;
        g_LoadAttempts.fetch_add(1, std::memory_order_relaxed);

        const wchar_t* paths[kMaxMinimapPaths];
        const int np = MinimapPathsForVersion(paths, kMaxMinimapPaths);
        const UClass* texClass = UTexture2D::StaticClass();
        const UTexture2D* tex = nullptr;
        if (np && texClass && SDK::Offsets::StaticLoadObject)
            for (int i = 0; i < np && !tex; ++i)
            {
                tex = StaticLoadMinimapSEH(paths[i], texClass);
                SDK::DbgLog("[SafeZoneMap] game-thread StaticLoadObject(%ls) = %p\n", paths[i], (const void*)tex);
            }
        if (!tex) // the load may have registered it under a different outer/mount
            tex = FindLoadedMinimapTexture(paths, np);

        int w = 0, h = 0;
        if (tex && ExtractToRGBA_Guarded(tex, g_LoadedRGBA, w, h))
        {
            g_LoadedW = w; g_LoadedH = h;
            g_LoadState.store((int)LoadState::Ready, std::memory_order_release);
            return;
        }
        g_LoadedRGBA.clear();
        g_LoadState.store((int)LoadState::Failed, std::memory_order_release);
        SDK::DbgLog("[SafeZoneMap] game-thread minimap load failed\n");
    }

    static bool HasReadyPixels()
    {
        return g_LoadState.load(std::memory_order_acquire) == (int)LoadState::Ready;
    }

    static bool IsLoadingOrRetrying()
    {
        const int state = g_LoadState.load(std::memory_order_acquire);
        if (state == (int)LoadState::Requested || state == (int)LoadState::Ready)
            return true;
        return (state == (int)LoadState::Idle || state == (int)LoadState::Failed) &&
            g_LoadAttempts.load(std::memory_order_relaxed) < 3;
    }

    // Candidate minimap object paths for the current engine version (find-only,
    // first hit wins). Paths are the full Package.ObjectName form. Some versions
    // ship the asset under more than one mount, so we list fallbacks.
    static int MinimapPathsForVersion(const wchar_t** out, int cap)
    {
        const float v = VersionInfo.FortniteVersion;
        int n = 0;
        auto add = [&](const wchar_t* p) { if (n < cap) out[n++] = p; };

        if (v < 11.00f)
            add(L"/Game/Athena/HUD/MiniMap/MiniMapAthena.MiniMapAthena");
        else if (v >= 13.00f && v < 14.00f) // C2S3 uses a season-specific texture
        {
            add(L"/Game/Athena/Apollo/Maps/UI/Apollo_Terrain_Minimap_S13_7.Apollo_Terrain_Minimap_S13_7");
            // Base asset fallback. NOTE: on some C2 builds the base texture is a
            // blanked placeholder (season art superseded it), so this mainly serves
            // to make Maps\Apollo_Terrain_Minimap.png a usable bundled fallback.
            add(L"/Game/Athena/Apollo/Maps/UI/Apollo_Terrain_Minimap.Apollo_Terrain_Minimap");
        }
        else if (v >= 27.00f && v < 28.00f)
        {
            // Chapter 1 OG shipped several weekly captures. Loaded-object lookup
            // also accepts any Rufus index, so hotfix versions work too.
            add(L"/Game/Athena/UI/Rufus/Capture_Iteration_Discovered_Rufus_03.Capture_Iteration_Discovered_Rufus_03");
            add(L"/Rufus/Game/UI/Capture_Iteration_Discovered_Rufus_03.Capture_Iteration_Discovered_Rufus_03");
            add(L"/Game/Athena/UI/Rufus/Capture_Iteration_Discovered_Rufus_01.Capture_Iteration_Discovered_Rufus_01");
            add(L"/Rufus/Game/UI/Capture_Iteration_Discovered_Rufus_01.Capture_Iteration_Discovered_Rufus_01");
            add(L"/Game/Athena/UI/Rufus/Capture_Iteration_Discovered_Rufus_04.Capture_Iteration_Discovered_Rufus_04");
            add(L"/Rufus/Game/UI/Capture_Iteration_Discovered_Rufus_04.Capture_Iteration_Discovered_Rufus_04");
        }
        else
        {
            // Apollo is intentionally retained as the broad fallback: many later
            // island releases reuse this cooked UI asset path with updated art.
            add(L"/Game/Athena/Apollo/Maps/UI/Apollo_Terrain_Minimap.Apollo_Terrain_Minimap");
            add(L"/Game/Athena/Artemis/Maps/UI/Artemis_Terrain_Minimap.Artemis_Terrain_Minimap");
            add(L"/Game/Athena/Asteria/Maps/UI/Asteria_Terrain_Minimap.Asteria_Terrain_Minimap");
            add(L"/Game/Athena/Helios/Maps/UI/Helios_Terrain_Minimap.Helios_Terrain_Minimap");
            add(L"/Game/Athena/Hermes/Maps/UI/Hermes_Terrain_Minimap.Hermes_Terrain_Minimap");
        }

        return n;
    }

    // Directory containing Magnesium.dll (not the host exe).
    static std::wstring ModuleDirW()
    {
        wchar_t buf[MAX_PATH] = {};
        DWORD n = GetModuleFileNameW(GetModuleHandleW(L"Magnesium.dll"), buf, MAX_PATH);
        std::wstring path(buf, n);
        size_t slash = path.find_last_of(L"\\/");
        return (slash == std::wstring::npos) ? L"." : path.substr(0, slash);
    }

    static std::wstring MapStorageDirW()
    {
        static const std::wstring directory = []()
        {
            wchar_t localAppData[MAX_PATH] = {};
            std::wstring result;
            if (SUCCEEDED(SHGetFolderPathW(
                    nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT,
                    localAppData)))
            {
                result = std::wstring(localAppData) + L"\\Magnesium\\Maps";
            }
            else
            {
                // Keep map support functional if the shell folder lookup fails.
                result = ModuleDirW() + L"\\Maps";
            }

            std::error_code error;
            std::filesystem::create_directories(result, error);
            SDK::DbgLog("[SafeZoneMap] map storage -> %ls%s\n",
                result.c_str(), error ? " (directory creation failed)" : "");
            return result;
        }();
        return directory;
    }

    // Cache PNG keyed by the EXACT version: Apollo seasons reuse one object path
    // but ship different art, so a coarser key would serve the wrong season.
    static std::wstring CacheFileNameW()
    {
        wchar_t name[64];
        // v3 invalidates Chapter 2+ caches created from the similarly named
        // discoverability/fog mask. Keep Chapter 1's known-good cache intact.
        if (VersionInfo.FortniteVersion >= 11.00f)
            swprintf(name, 64, L"Magnesium_SafeZoneMap_v3_%.2f.png",
                (double)VersionInfo.FortniteVersion);
        else
            swprintf(name, 64, L"Magnesium_SafeZoneMap_%.2f.png",
                (double)VersionInfo.FortniteVersion);
        return name;
    }

    static std::wstring CachePathW()
    {
        return MapStorageDirW() + L"\\" + CacheFileNameW();
    }

    // Read caches made by older Magnesium builds once, then copy them into the
    // AppData folder. The original file is left untouched for safe migration.
    static std::wstring LegacyCachePathW()
    {
        return ModuleDirW() + L"\\" + CacheFileNameW();
    }

    static std::string WideToUtf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), len, nullptr, nullptr);
        return s;
    }

    // Scan the global object array for a loaded UTexture2D whose leaf name matches
    // one of the candidate paths' object names. Finds the texture regardless of its
    // outer/mount path (StaticFindObject needs the exact path, which varies), so it
    // succeeds where the hard-coded paths miss - as long as the texture is resident.
    // Rufus (ch1 remix) weekly map textures are matched by PREFIX: the resident
    // weekly index rarely matches the hard-coded one (e.g. 27.11 keeps Rufus_03
    // loaded, not Rufus_04), and any week's art is close enough for zone placement.
    static const UTexture2D* FindLoadedMinimapTexture(const wchar_t** paths, int np)
    {
        const UClass* texClass = UTexture2D::StaticClass();
        if (!texClass) return nullptr;

        // Exact-name candidates (fast FName compare) + prefix candidates (string).
        FName want[kMaxMinimapPaths]; int nn = 0;
        std::string prefixes[kMaxMinimapPaths]; int npre = 0;
        for (int i = 0; i < np; ++i)
        {
            const wchar_t* dot = wcsrchr(paths[i], L'.');
            std::wstring wleaf(dot ? dot + 1 : paths[i]);
            std::string nleaf(wleaf.begin(), wleaf.end()); // leaf names are ASCII

            // "..._Rufus_04" -> prefix "Capture_Iteration_Discovered_Rufus_"
            const std::string rufusTag = "_Rufus_";
            size_t rp = nleaf.rfind(rufusTag);
            if (rp != std::string::npos && npre < kMaxMinimapPaths)
            {
                std::string pre = nleaf.substr(0, rp + rufusTag.size());
                bool dup = false;
                for (int k = 0; k < npre; ++k) if (prefixes[k] == pre) { dup = true; break; }
                if (!dup) prefixes[npre++] = pre;
            }
            if (nn < kMaxMinimapPaths)
            {
                UEAllocatedString s = nleaf.c_str();
                UEAllocatedWString ws(s.begin(), s.end());
                want[nn++] = FName(ws);
            }
        }

        const UTexture2D* prefixHit = nullptr; // exact name wins over prefix match
        const UTexture2D* exactHit = nullptr;
        int exactHitIndex = kMaxMinimapPaths;
        const UTexture2D* genericHit = nullptr;
        int genericScore = 0;
        const int32 total = SDK::TUObjectArray::Num();
        for (int32 i = 0; i < total; ++i)
        {
            const UObject* obj = SDK::TUObjectArray::GetObjectByIndex(i);
            if (!obj) continue;
            int hitIndex = -1;
            for (int k = 0; k < nn; ++k) if (obj->Name == want[k]) { hitIndex = k; break; }
            if (hitIndex >= 0)
            {
                if (obj->Class && obj->IsA(texClass))
                {
                    if (hitIndex == 0)
                    {
                        SDK::DbgLog("[SafeZoneMap] object-array scan exact minimap priority=0 @%p\n", (const void*)obj);
                        return (const UTexture2D*)obj;
                    }
                    if (hitIndex < exactHitIndex)
                    {
                        exactHitIndex = hitIndex;
                        exactHit = (const UTexture2D*)obj;
                    }
                }
                continue;
            }
            if (!obj->Class || !obj->IsA(texClass)) continue; // ToString allocates; textures only
            std::string nm = obj->Name.ToString().c_str();
            std::string lowerName = nm;
            std::transform(lowerName.begin(), lowerName.end(),
                lowerName.begin(), [](unsigned char c)
                {
                    return (char)std::tolower(c);
                });
            if (!prefixHit)
            {
                for (int k = 0; k < npre; ++k)
                {
                    if (nm.compare(0, prefixes[k].size(), prefixes[k]) != 0) continue;
                    SDK::DbgLog("[SafeZoneMap] object-array scan prefix-matched '%s' @%p\n", nm.c_str(), (const void*)obj);
                    prefixHit = (const UTexture2D*)obj;
                    break;
                }
            }

            // Unknown versions still get a best-effort candidate. Score strong,
            // full-island capture names and reject masks/icons/device textures.
            int score = 0;
            if (nm.find("Terrain_Minimap") != std::string::npos) score += 100;
            if (nm.find("MiniMapAthena") != std::string::npos) score += 100;
            if (nm.find("Capture_Iteration_Discovered") != std::string::npos) score += 90;
            if (nm.find("Minimap") != std::string::npos || nm.find("MiniMap") != std::string::npos) score += 35;
            if (nm.find("Terrain") != std::string::npos) score += 20;
            // Auxiliary fog/discovery textures often retain the full
            // "Terrain_Minimap" prefix. 17.30 even misspells its mask as
            // "Discoverabilty", so reject the whole discover* family instead
            // of matching one exact suffix.
            if (lowerName.find("mask") != std::string::npos ||
                lowerName.find("icon") != std::string::npos ||
                lowerName.find("device") != std::string::npos ||
                lowerName.find("discover") != std::string::npos ||
                lowerName.find("fog") != std::string::npos)
                score = -1000;
            if (score > genericScore)
            {
                genericScore = score;
                genericHit = (const UTexture2D*)obj;
            }
        }
        if (exactHit)
        {
            SDK::DbgLog("[SafeZoneMap] object-array scan exact minimap priority=%d @%p\n",
                exactHitIndex, (const void*)exactHit);
            return exactHit;
        }
        if (prefixHit) return prefixHit;
        if (genericHit && genericScore >= 70)
        {
            const std::string name = genericHit->Name.ToString().c_str();
            SDK::DbgLog(
                "[SafeZoneMap] object-array scan generic minimap '%s' score=%d @%p\n",
                name.c_str(), genericScore, (const void*)genericHit);
            return genericHit;
        }
        return nullptr;
    }

    // One-shot diagnostic: report how many textures are resident and any with a
    // map-like name, so we can tell "wrong path" from "texture simply not loaded".
    static void DiagnosticLogTextures()
    {
        const UClass* texClass = UTexture2D::StaticClass();
        if (!texClass) { SDK::DbgLog("[SafeZoneMap] diag: no UTexture2D class\n"); return; }

        const int32 total = SDK::TUObjectArray::Num();
        int texCount = 0, logged = 0;
        for (int32 i = 0; i < total; ++i)
        {
            const UObject* obj = SDK::TUObjectArray::GetObjectByIndex(i);
            if (!obj || !obj->Class || !obj->IsA(texClass)) continue;
            ++texCount;
            if (logged >= 24) continue;
            std::string nm = obj->Name.ToString().c_str();
            if (nm.find("inimap") != std::string::npos || nm.find("errain") != std::string::npos ||
                nm.find("iscover") != std::string::npos || nm.find("apture") != std::string::npos ||
                nm.find("_Map") != std::string::npos || nm.find("Rufus") != std::string::npos)
            {
                SDK::DbgLog("[SafeZoneMap] diag: loaded texture '%s'\n", nm.c_str());
                ++logged;
            }
        }
        SDK::DbgLog("[SafeZoneMap] diag: %d UTexture2D resident, %d map-like\n", texCount, logged);
    }

    // Read a PNG file from disk and upload it to a texture.
    static bool LoadPngFile(const std::wstring& file, ID3D11Device* dev, ID3D11ShaderResourceView** outSrv, int* outW, int* outH)
    {
        std::ifstream f(file.c_str(), std::ios::binary);
        if (!f) return false;
        std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (data.empty()) return false;
        if (LoadTextureFromMemory((unsigned char*)data.data(), (int)data.size(), dev, outSrv, outW, outH))
        {
            SDK::DbgLog("[SafeZoneMap] loaded PNG %ls (%d bytes)\n", file.c_str(), (int)data.size());
            return true;
        }
        return false;
    }

    // User-supplied minimap PNGs (e.g. exported from FModel). AppData is checked
    // first, while the old DLL-adjacent Maps folder remains a compatible fallback.
    // Checked most-specific first:
    //   <Maps>\<version>.png   (e.g. 17.30.png) - exact per-version override
    //   <Maps>\<leaf>.png      (e.g. Apollo_Terrain_Minimap.png) - asset default
    static bool LoadBundledMinimap(const wchar_t** paths, int np, ID3D11Device* dev, ID3D11ShaderResourceView** outSrv, int* outW, int* outH)
    {
        const std::wstring directories[2]{
            MapStorageDirW(),
            ModuleDirW() + L"\\Maps"
        };

        for (const std::wstring& dir : directories)
        {
            wchar_t vname[64];
            swprintf(vname, 64, L"\\%.2f.png",
                (double)VersionInfo.FortniteVersion);
            if (LoadPngFile(dir + vname, dev, outSrv, outW, outH))
                return true;

            for (int i = 0; i < np; ++i)
            {
                const wchar_t* dot = wcsrchr(paths[i], L'.');
                std::wstring leaf(dot ? dot + 1 : paths[i]);
                const std::wstring file = dir + L"\\" + leaf + L".png";
                if (LoadPngFile(file, dev, outSrv, outW, outH))
                    return true;
            }
        }
        return false;
    }

    // Try live extraction (and dump a PNG), else user-bundled PNGs, else cache.
    static bool Acquire(ID3D11Device* dev, ID3D11ShaderResourceView** outSrv, int* outW, int* outH)
    {
        const wchar_t* paths[kMaxMinimapPaths];
        const int np = MinimapPathsForVersion(paths, kMaxMinimapPaths);
        const std::wstring cacheW = CachePathW();

        // 1) Live extraction from the loaded UTexture2D. Use StaticFindObject
        // (find-only): FindObject<>() falls back to StaticLoadObject when the asset
        // isn't already loaded, and that native loader faults on some versions
        // (e.g. 17.30 / 27.x). If the minimap isn't resident we just skip the image.
        const UClass* texClass = UTexture2D::StaticClass();
        const UTexture2D* tex = nullptr;
        if (texClass)
            for (int i = 0; i < np && !tex; ++i)
            {
                tex = (const UTexture2D*)SDK::StaticFindObject(paths[i], texClass);
                SDK::DbgLog("[SafeZoneMap] StaticFindObject(%ls) = %p\n", paths[i], (const void*)tex);
            }
        if (!tex) // exact path missed; scan the object array by leaf name
        {
            tex = FindLoadedMinimapTexture(paths, np);
            static bool s_Diag = false;
            if (!tex && !s_Diag) { s_Diag = true; DiagnosticLogTextures(); }
        }
        if (tex)
        {
            std::vector<unsigned char> rgba; int w = 0, h = 0;
            if (ExtractToRGBA_Guarded(tex, rgba, w, h) && CreateTextureFromRGBA8(rgba.data(), w, h, dev, outSrv))
            {
                *outW = w; *outH = h;
                const std::string cacheU8 = WideToUtf8(cacheW);
                if (stbi_write_png(cacheU8.c_str(), w, h, 4, rgba.data(), w * 4))
                    SDK::DbgLog("[SafeZoneMap] dumped cache PNG -> %s\n", cacheU8.c_str());
                return true;
            }
        }
        else
        {
            SDK::DbgLog("[SafeZoneMap] FindObject(minimap) returned null\n");
        }

        // 2) Pixels produced by the game-thread load bridge (see GameThreadTick):
        // a previous Acquire posted a request and the pre-Start/server pump
        // loaded and extracted it.
        if (g_LoadState.load(std::memory_order_acquire) == (int)LoadState::Ready)
        {
            const bool ok = CreateTextureFromRGBA8(g_LoadedRGBA.data(), g_LoadedW, g_LoadedH, dev, outSrv);
            if (ok)
            {
                *outW = g_LoadedW; *outH = g_LoadedH;
                const std::string cacheU8 = WideToUtf8(cacheW);
                if (stbi_write_png(cacheU8.c_str(), g_LoadedW, g_LoadedH, 4, g_LoadedRGBA.data(), g_LoadedW * 4))
                    SDK::DbgLog("[SafeZoneMap] dumped cache PNG -> %s\n", cacheU8.c_str());
            }
            g_LoadedRGBA.clear(); g_LoadedRGBA.shrink_to_fit();
            g_LoadState.store((int)LoadState::Consumed, std::memory_order_release);
            if (ok) return true;
        }

        // 3) User-provided PNGs in Maps\ (FModel exports) - optional per-version
        // overrides for art the live path can't produce.
        if (LoadBundledMinimap(paths, np, dev, outSrv, outW, outH))
            return true;

        // 4) A PNG previously dumped by a successful live extraction.
        if (LoadPngFile(cacheW, dev, outSrv, outW, outH))
            return true;

        // 5) Migrate a cache made by an older DLL-adjacent build.
        const std::wstring legacyCacheW = LegacyCachePathW();
        if (legacyCacheW != cacheW &&
            LoadPngFile(legacyCacheW, dev, outSrv, outW, outH))
        {
            std::error_code error;
            std::filesystem::copy_file(
                legacyCacheW, cacheW,
                std::filesystem::copy_options::skip_existing, error);
            SDK::DbgLog("[SafeZoneMap] legacy cache migration -> %ls%s\n",
                cacheW.c_str(), error ? " (copy failed)" : "");
            return true;
        }

        // 6) Nothing resident and no PNG on disk: ask the game thread to load the
        // asset properly (loading is game-thread-only; doing it here faulted on
        // 17.30/27.x). Retry a few times because MapInfo and streamed UI assets can
        // become available after the editor's first frame.
        int state = g_LoadState.load(std::memory_order_acquire);
        if (g_LoadAttempts.load(std::memory_order_relaxed) < 3 &&
            (state == (int)LoadState::Idle || state == (int)LoadState::Failed) &&
            g_LoadState.compare_exchange_strong(state, (int)LoadState::Requested, std::memory_order_acq_rel))
        {
            SDK::DbgLog("[SafeZoneMap] posted game-thread load request (attempt %d/3)\n",
                g_LoadAttempts.load(std::memory_order_relaxed) + 1);
        }

        SDK::DbgLog("[SafeZoneMap] acquire failed; numeric fallback in use\n");
        return false;
    }
}

void GUI::SafeZoneMapGameTick()
{
    Events::Tick();
    SafeZoneMap::GameThreadTick();
    PlayerLoadout::GameThreadTick();
    TrickshotManager::GameThreadTick();
    ABuildingSMActor::TickSavedTrapAttachments();
    AFortPlayerPawnAthena::TickPendingPlayerMapIcons();
}

bool GameTextureBridge::ExtractToRGBA(
    const UTexture2D* Texture,
    std::vector<unsigned char>& RGBA,
    int& Width,
    int& Height)
{
    if (!Texture)
        return false;

    // Item cards render below 128 px. Use only a structurally validated,
    // already-resident CPU mip no larger than 512 px; never request streaming
    // or decode the full preview merely to shrink it for this admin UI.
    const bool PreviousSuppress =
        SafeZoneMap::g_SuppressTextureExtractionLogs;
    const int32 PreviousMaximum =
        SafeZoneMap::g_MaxTextureExtractionDimension;
    SafeZoneMap::g_SuppressTextureExtractionLogs = true;
    bool Result =
        SafeZoneMap::ExtractResidentIconToRGBA_Guarded(
        Texture, RGBA, Width, Height);
    if (!Result)
    {
        // The strict extractor requires a uniquely identifiable cooked mip
        // layout. Its older guarded decoder recognizes additional UE4 inline
        // and indirect-array layouts; keep the same hard 512px ceiling so a
        // menu icon can never decode a full map-sized texture.
        RGBA.clear();
        Width = Height = 0;
        SafeZoneMap::g_MaxTextureExtractionDimension =
            SafeZoneMap::kMaximumResidentIconDimension;
        Result =
            SafeZoneMap::ExtractToRGBA_Guarded(
                Texture, RGBA, Width, Height);
    }
    SafeZoneMap::g_MaxTextureExtractionDimension =
        PreviousMaximum;
    SafeZoneMap::g_SuppressTextureExtractionLogs =
        PreviousSuppress;
    return Result;
}

void GUI::ResolveCustomSafeZoneForMap(AFortAthenaMapInfo* MapInfo)
{
    if (!SafeZoneMap::g_HasNormalizedSelection.load(std::memory_order_acquire))
        return;

    SafeZoneMap::MapTransform map;
    const UObject* manager = nullptr;
    if (SafeZoneMap::ReadRuntimeTransform(map, manager))
    {
        SafeZoneMap::PublishTransform(map);
        SafeZoneMap::ReprojectRememberedSelection(map);
        SDK::DbgLog(
            "[SafeZoneMap] finalized custom zone from %s center=(%.1f, %.1f) axes=(%.1f, %.1f)\n",
            manager == MapInfo ? "MapInfo" : "runtime map data",
            map.CenterX, map.CenterY,
            SafeZoneMap::AxisULength(map), SafeZoneMap::AxisVLength(map));
        return;
    }

    SafeZoneMap::FinalizeSelectionForMapInfo(MapInfo);
}

bool GUI::GetNormalizedSafeZoneSelection(
    float& U,
    float& V)
{
    U = SafeZoneMap::g_SelectedU.load(
        std::memory_order_relaxed);
    V = SafeZoneMap::g_SelectedV.load(
        std::memory_order_relaxed);
    return SafeZoneMap::g_HasNormalizedSelection.load(
        std::memory_order_acquire);
}

void GUI::RestoreNormalizedSafeZoneSelection(
    bool bHasSelection,
    float U,
    float V)
{
    SafeZoneMap::g_SelectedU.store(
        SafeZoneMap::Clamp(U, 0.f, 1.f),
        std::memory_order_relaxed);
    SafeZoneMap::g_SelectedV.store(
        SafeZoneMap::Clamp(V, 0.f, 1.f),
        std::memory_order_relaxed);
    SafeZoneMap::g_HasNormalizedSelection.store(
        bHasSelection, std::memory_order_release);
}

void Hyperlink(const char* label, const char* url)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.6f, 1.0f, 1.0f));
    ImGui::Text("%s", label);
    ImGui::PopStyleColor();

    if (ImGui::IsItemHovered())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Would you like to be redirected to this link?");
    }

    if (ImGui::IsItemClicked())
    {
        ShellExecuteA(0, "open", url, 0, 0, SW_SHOWNORMAL);
    }
}

void SmallSeparator(float Width, float Thickness = 1.0f)
{
    ImVec2 Pos = ImGui::GetCursorScreenPos();
    auto* Draw = ImGui::GetWindowDrawList();

    Draw->AddLine(Pos, ImVec2(Pos.x + Width, Pos.y), ImGui::GetColorU32(ImGuiCol_Separator), Thickness);

    ImGui::Dummy(ImVec2(Width, Thickness + 4));
}

static float ContentSectionWidth(float FallbackWidth)
{
    float Width = ImGui::GetContentRegionAvail().x;
    if (Width < FallbackWidth)
        Width = FallbackWidth;

    return Width;
}

static void SectionHeader(const char* Title, float Width)
{
    ImGui::Spacing();

    const float HeaderH = 28.f;
    const ImVec2 Pos = ImGui::GetCursorScreenPos();
    const ImVec2 End(Pos.x + Width, Pos.y + HeaderH);
    auto* Draw = ImGui::GetWindowDrawList();

    Draw->AddRectFilled(Pos, End, ImGui::GetColorU32(ImVec4(0.116f, 0.122f, 0.141f, 1.f)), 4.f);
    Draw->AddRectFilled(Pos, ImVec2(Pos.x + 3.f, End.y), ImGui::GetColorU32(Accent()), 4.f);

    ImGui::SetCursorScreenPos(ImVec2(Pos.x + 12.f, Pos.y + (HeaderH - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.82f, 0.84f, 0.90f, 1.f));
    ImGui::TextUnformatted(Title);
    ImGui::PopStyleColor();

    ImGui::SetCursorScreenPos(ImVec2(Pos.x, End.y + 8.f));
}

static void BeginSectionBody()
{
    ImGui::Indent(10.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.f, 7.f));
}

static void EndSectionBody()
{
    ImGui::PopStyleVar();
    ImGui::Unindent(10.f);
}

static bool LabeledSliderInt(
    const char* Label,
    const char* Id,
    int* Value,
    int Min,
    int Max,
    float Width,
    const char* Format = "%d")
{
    ImGui::TextUnformatted(Label);
    ImGui::SetNextItemWidth(Width);
    return ImGui::SliderInt(
        Id, Value, Min, Max, Format,
        ImGuiSliderFlags_AlwaysClamp);
}

static bool LabeledSliderFloat(const char* Label, const char* Id, float* Value, float Min, float Max, const char* Format, float Width)
{
    ImGui::TextUnformatted(Label);
    ImGui::SetNextItemWidth(Width);
    return ImGui::SliderFloat(
        Id, Value, Min, Max, Format,
        ImGuiSliderFlags_AlwaysClamp);
}

static bool AtomicCheckbox(
    const char* Label,
    std::atomic_bool& Setting)
{
    bool Value = Setting.load(std::memory_order_acquire);
    const bool bChanged = ImGui::Checkbox(Label, &Value);
    if (bChanged)
    {
        Setting.store(Value, std::memory_order_release);
    }
    return bChanged;
}

static void ApplyInitialTrickshotDefaults()
{
    FConfiguration::ResetTrickshotSettings();

    FConfiguration::bUseWinLines.store(
        true, std::memory_order_release);
    FConfiguration::bCrownSlomo.store(
        true, std::memory_order_release);

    if (FConfiguration::bLateGame.load(
        std::memory_order_acquire))
    {
        FConfiguration::RandomizeKills.store(
            true, std::memory_order_release);
    }

    FConfiguration::RandomizeLevels.store(
        true, std::memory_order_release);
    FConfiguration::bDisableJumpFatigue.store(
        true, std::memory_order_release);
    FConfiguration::bDisableSupplyDrops.store(
        true, std::memory_order_release);
    FConfiguration::bVehicleBumpLaunch.store(
        VersionInfo.FortniteVersion >= 4.30,
        std::memory_order_release);
    FConfiguration::bAutoGodMode.store(
        true, std::memory_order_release);
    FConfiguration::bAutoReloadOnWaypointTP.store(
        true, std::memory_order_release);
    FConfiguration::bRemoveIceOnWaypointTP.store(
        VersionInfo.FortniteVersion >= 6.01,
        std::memory_order_release);
    FConfiguration::bAutoPauseTODM.store(
        false, std::memory_order_release);

    if (GUI::IsArenaPlaylist() ||
        GUI::IsTournamentPlaylist())
    {
        FConfiguration::RandomizeArenaPoints.store(
            true, std::memory_order_release);
    }
}

static bool TrickshotTabCheckbox(const char* Label)
{
    bool bEnabled =
        FConfiguration::bEnableTrickshotTab.load(
            std::memory_order_acquire);

    if (!ImGui::Checkbox(Label, &bEnabled))
        return false;

    FConfiguration::SetTrickshotTabEnabled(bEnabled);
    if (bEnabled)
        ApplyInitialTrickshotDefaults();

    return true;
}

static bool AtomicLabeledSliderInt(
    const char* Label,
    const char* Id,
    std::atomic_int& Setting,
    int Min,
    int Max,
    float Width,
    const char* Format = "%d")
{
    int Value = Setting.load(std::memory_order_acquire);
    const bool bChanged =
        LabeledSliderInt(
            Label, Id, &Value, Min, Max, Width,
            Format);
    if (bChanged)
    {
        Setting.store(Value, std::memory_order_release);
    }
    return bChanged;
}

static bool AtomicLabeledSliderFloat(
    const char* Label,
    const char* Id,
    std::atomic<float>& Setting,
    float Min,
    float Max,
    const char* Format,
    float Width)
{
    float Value = Setting.load(std::memory_order_acquire);
    const bool bChanged =
        LabeledSliderFloat(
            Label, Id, &Value, Min, Max, Format,
            Width);
    if (bChanged)
    {
        Setting.store(Value, std::memory_order_release);
    }
    return bChanged;
}

static bool AtomicInputInt(
    const char* Label,
    std::atomic_int& Setting)
{
    int Value = Setting.load(std::memory_order_acquire);
    const bool bChanged = ImGui::InputInt(Label, &Value);
    if (bChanged)
    {
        Setting.store(Value, std::memory_order_release);
    }
    return bChanged;
}

static bool AtomicCombo(
    const char* Label,
    std::atomic_int& Setting,
    const char* const Items[],
    int ItemCount)
{
    int Value = Setting.load(std::memory_order_acquire);
    const bool bChanged =
        ImGui::Combo(
            Label, &Value, Items, ItemCount);
    if (bChanged)
    {
        Setting.store(Value, std::memory_order_release);
    }
    return bChanged;
}

static std::string FormatDurationSeconds(double Seconds)
{
    long long TotalSeconds = (long long)floor(Seconds);
    if (TotalSeconds < 0)
        TotalSeconds = 0;

    const long long Days = TotalSeconds / 86400;
    TotalSeconds %= 86400;
    const long long Hours = TotalSeconds / 3600;
    TotalSeconds %= 3600;
    const long long Minutes = TotalSeconds / 60;
    const long long RemainingSeconds = TotalSeconds % 60;

    std::string Result;
    auto AppendUnit = [&Result](long long Value, const char* Singular)
        {
            if (Value <= 0)
                return;

            if (!Result.empty())
                Result += " ";

            Result += std::to_string(Value);
            Result += " ";
            Result += Singular;
            if (Value != 1)
                Result += "s";
        };

    AppendUnit(Days, "Day");
    AppendUnit(Hours, "Hour");
    AppendUnit(Minutes, "Minute");

    if (Result.empty() || RemainingSeconds > 0)
        AppendUnit(RemainingSeconds, "Second");

    if (Result.empty())
        Result = "0 Seconds";

    return Result;
}

// One full-width vertical tab in the left sidebar. Sets *activeUI to uiValue on click.
static void SidebarTab(const char* label, int uiValue, float yPos, float tabH, int* activeUI)
{
    ImGui::PushID(uiValue);
    const bool active = (*activeUI == uiValue);

    ImGui::SetCursorPos(ImVec2(0.f, yPos));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, tabH);

    if (ImGui::InvisibleButton("##tab", size))
        *activeUI = uiValue;
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active)
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(Accent(0.12f)));
    else if (hovered)
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(Accent(0.06f)));

    ImVec4 textCol = active ? Accent() : ImVec4(0.60f, 0.63f, 0.69f, 1.f);
    if (!active && hovered) textCol = ImVec4(0.85f, 0.87f, 0.92f, 1.f);

    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + 26.f, p.y + (size.y - ts.y) * 0.5f), ImGui::GetColorU32(textCol), label);

    ImGui::PopID();
}

static const char* GetSelectedPlaylistModeName()
{
    switch (GUI::SelectedPlaylist)
    {
    case (int)Playlist::Solos:
        return "Solos";
    case (int)Playlist::Duos:
        return "Duos";
    case (int)Playlist::Trios:
        return "Trios";
    case (int)Playlist::Squads:
        return "Squads";
    case (int)Playlist::GetawaySolos:
        return "The Getaway Solos";
    case (int)Playlist::GetawayDuos:
        return "The Getaway Duos";
    case (int)Playlist::GetawaySquads:
        return "The Getaway Squads";
    case (int)Playlist::FoodFight:
        return "Food Fight";
    case (int)Playlist::DeepFriedSquads:
        return "Food Fight: Deep Fried";
    case (int)Playlist::ArsenalSolos:
        return "Arsenal Solos";
    case (int)Playlist::WicksBountySolo:
        return "Wick's Bounty Solo";
    case (int)Playlist::WicksBountyDuo:
        return "Wick's Bounty Duo";
    case (int)Playlist::WicksBountySquads:
        return "Wick's Bounty Squads";
    case (int)Playlist::BountySolo:
        return "Bounty Solo";
    case (int)Playlist::BountyDuo:
        return "Bounty Duo";
    case (int)Playlist::BountySquads:
        return "Bounty Squads";
    case (int)Playlist::AvengersEndgame:
        return "Avengers: Endgame";
    case (int)Playlist::DiscoDomination:
        return "Disco Domination";
    case (int)Playlist::ScoreRoyaleSolo:
        return "Score Royale Solos";
    case (int)Playlist::ScoreRoyaleDuos:
        return "Score Royale Duos";
    case (int)Playlist::ScoreRoyaleSquads:
        return "Score Royale Squads";
    case (int)Playlist::InfinityGauntletSolos:
        return "Infinity Gauntlet Solos";
    case (int)Playlist::ZBSolos:
        return "Zero Build Solos";
    case (int)Playlist::ZBDuos:
        return "Zero Build Duos";
    case (int)Playlist::ZBTrios:
        return "Zero Build Trios";
    case (int)Playlist::ZBSquads:
        return "Zero Build Squads";
    case (int)Playlist::Playground:
        return "Playground";
    case (int)Playlist::Creative:
        return "Creative";
    case (int)Playlist::OneShotSolos:
        return "One Shot Solos";
    case (int)Playlist::OneShotDuos:
        return "One Shot Duos";
    case (int)Playlist::OneShotSquads:
        return "One Shot Squads";
    case (int)Playlist::SiphonSolos:
        return "Siphon Solos";
    case (int)Playlist::SiphonDuos:
        return "Siphon Duos";
    case (int)Playlist::SiphonSquads:
        return "Siphon Squads";
    case (int)Playlist::UnvSolos:
        return "Unvaulted Solos";
    case (int)Playlist::UnvDuos:
        return "Unvaulted Duos";
    case (int)Playlist::UnvTrios:
        return "Unvaulted Trios";
    case (int)Playlist::UnvSquads:
        return "Unvaulted Squads";
    case (int)Playlist::SlideSolos:
        return "Slide Solos";
    case (int)Playlist::SlideDuos:
        return "Slide Duos";
    case (int)Playlist::FILSolos:
        return "Floor Is Lava Solos";
    case (int)Playlist::FILDuos:
        return "Floor Is Lava Duos";
    case (int)Playlist::FILSquads:
        return "Floor Is Lava Squads";
    case (int)Playlist::TournamentSolos:
        return "Tournament Solos";
    case (int)Playlist::TournamentDuos:
        return "Tournament Duos";
    case (int)Playlist::TournamentTrios:
        return "Tournament Trios";
    case (int)Playlist::TournamentSquads:
        return "Tournament Squads";
    case (int)Playlist::ArenaSolos:
        return "Arena Solos";
    case (int)Playlist::ArenaDuos:
        return "Arena Duos";
    case (int)Playlist::ArenaTrios:
        return "Arena Trios";
    case (int)Playlist::ArenaSquads:
        return "Arena Squads";
    case (int)Playlist::ArenaZBSolos:
        return "Arena Zero Build Solos";
    case (int)Playlist::ArenaZBDuos:
        return "Arena Zero Build Duos";
    case (int)Playlist::ArenaZBTrios:
        return "Arena Zero Build Trios";
    case (int)Playlist::ArenaZBSquads:
        return "Arena Zero Build Squads";
    case (int)Playlist::Gav:
        return "Gav 1v1 Map";
    case (int)Playlist::Retrac1v1:
        return "Retrac 1v1 Map";
    case (int)Playlist::RetracTurtle:
        return "Retrac Turtle Fights";
    case (int)Playlist::RetracWater:
        return "Retrac Water Map";
    case (int)Playlist::TiltedZW:
        return "Tilted FFA";
    case (int)Playlist::OnlyUp:
        return "Only Up Map";
    case (int)Playlist::Twine1v1:
        return "Twine 1v1 Map";
    case (int)Playlist::Boxfight:
        return "Boxfights";
    case (int)Playlist::Backrooms:
        return "Backrooms Map";
    case (int)Playlist::Event:
        return "Event Playlist";
    case (int)Playlist::Custom:
        return "Custom";
    default:
        return "Unknown";
    }
}

static bool IsScoreRoyalePlaylistBuild()
{
    constexpr double Tolerance = 0.001;
    return VersionInfo.FortniteVersion + Tolerance >= 7.30 &&
        VersionInfo.FortniteVersion <= 10.00 + Tolerance;
}

static bool IsSpecialMapSelection(int SelectedPlaylist)
{
    switch (static_cast<Playlist>(SelectedPlaylist))
    {
    case Playlist::Gav:
    case Playlist::Retrac1v1:
    case Playlist::RetracTurtle:
    case Playlist::RetracWater:
    case Playlist::TiltedZW:
    case Playlist::OnlyUp:
    case Playlist::Twine1v1:
    case Playlist::Boxfight:
    case Playlist::Backrooms:
        return true;
    default:
        return false;
    }
}

bool GUI::UsesDefaultMatchSettings(int SelectedPlaylist)
{
    return !IsSpecialMapSelection(SelectedPlaylist) &&
        SelectedPlaylist != static_cast<int>(Playlist::Creative) &&
        SelectedPlaylist != static_cast<int>(Playlist::Event);
}

static bool LocksLateGameForSelection(int SelectedPlaylist)
{
    if (IsSpecialMapSelection(SelectedPlaylist))
        return true;

    switch (static_cast<Playlist>(SelectedPlaylist))
    {
    case Playlist::Event:
    case Playlist::GetawaySolos:
    case Playlist::GetawayDuos:
    case Playlist::GetawaySquads:
    case Playlist::InfinityGauntletSolos:
    case Playlist::FoodFight:
    case Playlist::DeepFriedSquads:
        return true;
    default:
        return false;
    }
}

static bool EventUsesSpawnIslandBusControl()
{
    const double Version = VersionInfo.FortniteVersion;
    return Version <= 4.50 ||
        Version == 6.21 ||
        Version == 7.20 ||
        Version == 7.30 ||
        Version == 8.51 ||
        Version == 9.40 ||
        Version == 9.41 ||
        Version == 10.40;
}

static bool IsNativeLTMSelection(int SelectedPlaylist)
{
    if (IsScoreRoyalePlaylistBuild() &&
        (SelectedPlaylist ==
             static_cast<int>(Playlist::ScoreRoyaleSolo) ||
         SelectedPlaylist ==
             static_cast<int>(Playlist::ScoreRoyaleDuos) ||
         SelectedPlaylist ==
             static_cast<int>(Playlist::ScoreRoyaleSquads)))
    {
        return true;
    }

    if (SelectedPlaylist ==
            static_cast<int>(Playlist::FoodFight) &&
        FFortAthenaNativeLTMCompatibility::
            IsOriginalFoodFightSupportedBuild())
    {
        return true;
    }

    if (VersionInfo.FortniteVersion != 10.40)
        return false;

    switch (SelectedPlaylist)
    {
    case (int)Playlist::GetawaySolos:
    case (int)Playlist::GetawayDuos:
    case (int)Playlist::GetawaySquads:
    case (int)Playlist::FoodFight:
    case (int)Playlist::DeepFriedSquads:
    case (int)Playlist::ArsenalSolos:
    case (int)Playlist::WicksBountySolo:
    case (int)Playlist::WicksBountyDuo:
    case (int)Playlist::WicksBountySquads:
    case (int)Playlist::BountySolo:
    case (int)Playlist::BountyDuo:
    case (int)Playlist::BountySquads:
    case (int)Playlist::AvengersEndgame:
    case (int)Playlist::DiscoDomination:
        return true;
    default:
        return false;
    }
}

struct FCustomMapLifecyclePresetState
{
    int SelectedPlaylist = -1;
    bool bOwnsJoinInProgress = false;
    bool bOriginalJoinInProgress = false;
    bool bAppliedJoinInProgress = false;
    bool bOwnsKeepInventory = false;
    bool bOriginalKeepInventory = false;
    bool bAppliedKeepInventory = false;
    bool bOwnsForceRespawns = false;
    bool bOriginalForceRespawns = false;
    bool bAppliedForceRespawns = false;
    bool bOwnsPermanentRespawn = false;
    bool bOriginalPermanentRespawn = false;
    bool bAppliedPermanentRespawn = false;
    bool bOwnsCustomRespawnPoint = false;
    bool bOriginalHasCustomRespawnPoint = false;
    bool bAppliedHasCustomRespawnPoint = false;
    FVector OriginalCustomRespawnPoint{};
    FVector AppliedCustomRespawnPoint{};
    bool bOwnsAutoBusStart = false;
    bool bOriginalAutoBusStart = false;
    bool bAppliedAutoBusStart = false;
    bool bOwnsInfiniteAmmo = false;
    bool bOriginalInfiniteAmmo = false;
    bool bAppliedInfiniteAmmo = false;
    bool bOwnsInfiniteMats = false;
    bool bOriginalInfiniteMats = false;
    bool bAppliedInfiniteMats = false;
};

static FCustomMapLifecyclePresetState
    GCustomMapLifecyclePresetState{};
static int GLastSelectedPlaylist = -1;

void GUI::ResetPreferenceEditorState()
{
    GCustomMapLifecyclePresetState = {};
    GLastSelectedPlaylist = SelectedPlaylist;
    PublishSelectedPlaylist(SelectedPlaylist);
    ++GPreferenceEditorGeneration;
}

static bool AreRespawnPointsEqual(
    const FVector& Left,
    const FVector& Right)
{
    return Left.X == Right.X &&
        Left.Y == Right.Y &&
        Left.Z == Right.Z;
}

static void RestoreOutgoingCustomMapLifecyclePreset(
    int OutgoingPlaylist)
{
    auto& State = GCustomMapLifecyclePresetState;
    if (State.SelectedPlaylist != OutgoingPlaylist)
        return;

    // Restore only fields that still contain the value applied by the
    // outgoing preset. A setting changed after selecting that preset is a
    // user choice and must survive the transition.
    if (State.bOwnsJoinInProgress &&
        FConfiguration::bJoinInProgress ==
            State.bAppliedJoinInProgress)
    {
        FConfiguration::bJoinInProgress =
            State.bOriginalJoinInProgress;
    }
    if (State.bOwnsKeepInventory &&
        FConfiguration::bKeepInventory ==
            State.bAppliedKeepInventory)
    {
        FConfiguration::bKeepInventory =
            State.bOriginalKeepInventory;
    }
    if (State.bOwnsForceRespawns &&
        FConfiguration::bForceRespawns ==
            State.bAppliedForceRespawns)
    {
        FConfiguration::bForceRespawns =
            State.bOriginalForceRespawns;
    }
    if (State.bOwnsPermanentRespawn &&
        FConfiguration::PermanentRespawn ==
            State.bAppliedPermanentRespawn)
    {
        FConfiguration::PermanentRespawn =
            State.bOriginalPermanentRespawn;
    }
    if (State.bOwnsCustomRespawnPoint &&
        FConfiguration::HasCustomRespawnPoint ==
            State.bAppliedHasCustomRespawnPoint &&
        AreRespawnPointsEqual(
            FConfiguration::CustomRespawnPoint,
            State.AppliedCustomRespawnPoint))
    {
        FConfiguration::HasCustomRespawnPoint =
            State.bOriginalHasCustomRespawnPoint;
        FConfiguration::CustomRespawnPoint =
            State.OriginalCustomRespawnPoint;
    }
    if (State.bOwnsAutoBusStart &&
        FConfiguration::bAutoBusStart.load(
            std::memory_order_acquire) ==
            State.bAppliedAutoBusStart)
    {
        FConfiguration::bAutoBusStart.store(
            State.bOriginalAutoBusStart,
            std::memory_order_release);
    }
    if (State.bOwnsInfiniteAmmo &&
        FConfiguration::bInfiniteAmmo.load(
            std::memory_order_acquire) ==
            State.bAppliedInfiniteAmmo)
    {
        FConfiguration::bInfiniteAmmo.store(
            State.bOriginalInfiniteAmmo,
            std::memory_order_release);
    }
    if (State.bOwnsInfiniteMats &&
        FConfiguration::bInfiniteMats.load(
            std::memory_order_acquire) ==
            State.bAppliedInfiniteMats)
    {
        FConfiguration::bInfiniteMats.store(
            State.bOriginalInfiniteMats,
            std::memory_order_release);
    }

    State = {};
}

static void SanitizeNativeLTMSelection(int SelectedPlaylist)
{
    // Publish the render-thread selection through an atomic handoff before
    // the game thread evaluates early mode ownership or lifecycle policy.
    GUI::PublishSelectedPlaylist(SelectedPlaylist);

    // These modes own their map/objective phase flow. Enforce the lock every
    // frame so restored preferences and later tab visits cannot re-enable Late
    // Game after the one-time selection preset has run.
    if (LocksLateGameForSelection(SelectedPlaylist))
        FConfiguration::SetLateGameEnabled(false);

    // Apply the native-mode defaults only when the selected mode changes.
    // This function is intentionally called from the render loop, so without
    // this transition guard every checkbox click is overwritten next frame.
    if (SelectedPlaylist == GLastSelectedPlaylist)
        return;

    // Auto Hosting restores the resolved playlist and the user's explicit
    // overrides before the first GUI frame. Reapplying selection defaults here
    // would silently replace that saved profile.
    if (GLastSelectedPlaylist == -1 &&
        AutoHosting::HasRestoredPreferences())
    {
        GLastSelectedPlaylist = SelectedPlaylist;
        return;
    }

    RestoreOutgoingCustomMapLifecyclePreset(
        GLastSelectedPlaylist);
    GLastSelectedPlaylist = SelectedPlaylist;

    if (IsNativeLTMSelection(SelectedPlaylist))
    {
        // These playlists provide their own phase flow, inventory, and
        // objective mechanics. Clear only settings that would replace those
        // foundations. Ordinary gameplay options remain user-owned and must
        // survive both selecting an LTM and its subsequent native setup.
        FConfiguration::SetLateGameEnabled(false);
        FConfiguration::bIsCustomMap = false;
        FConfiguration::bCustomSafeZone = false;
        FConfiguration::HasCustomRespawnPoint = false;
        if (SelectedPlaylist ==
            static_cast<int>(
                Playlist::AvengersEndgame))
        {
            // Endgame's Chitauri weapons and building resources are finite.
            // Own these two selection defaults so the player can still turn
            // either option back on after selecting the mode, and restore the
            // prior values when they leave it.
            auto& EndgamePreset =
                GCustomMapLifecyclePresetState;
            EndgamePreset.SelectedPlaylist =
                SelectedPlaylist;
            EndgamePreset.bOwnsInfiniteAmmo = true;
            EndgamePreset.bOriginalInfiniteAmmo =
                FConfiguration::bInfiniteAmmo.load(
                    std::memory_order_acquire);
            EndgamePreset.bAppliedInfiniteAmmo = false;
            FConfiguration::bInfiniteAmmo.store(
                false, std::memory_order_release);
            EndgamePreset.bOwnsInfiniteMats = true;
            EndgamePreset.bOriginalInfiniteMats =
                FConfiguration::bInfiniteMats.load(
                    std::memory_order_acquire);
            EndgamePreset.bAppliedInfiniteMats = false;
            FConfiguration::bInfiniteMats.store(
                false, std::memory_order_release);
        }
        return;
    }

    auto& CustomPreset =
        GCustomMapLifecyclePresetState;
    CustomPreset.SelectedPlaylist =
        SelectedPlaylist;

    auto ApplyJoinInProgressPreset =
        [&]()
        {
            CustomPreset.bOwnsJoinInProgress = true;
            CustomPreset.bOriginalJoinInProgress =
                FConfiguration::bJoinInProgress;
            CustomPreset.bAppliedJoinInProgress = true;
            FConfiguration::bJoinInProgress = true;
        };

    auto ApplyKeepInventoryPreset =
        [&]()
        {
            CustomPreset.bOwnsKeepInventory = true;
            CustomPreset.bOriginalKeepInventory =
                FConfiguration::bKeepInventory;
            CustomPreset.bAppliedKeepInventory = true;
            FConfiguration::bKeepInventory = true;
        };

    auto ApplyAutoBusStartPreset =
        [&](bool Value)
        {
            CustomPreset.bOwnsAutoBusStart = true;
            CustomPreset.bOriginalAutoBusStart =
                FConfiguration::bAutoBusStart.load(
                    std::memory_order_acquire);
            CustomPreset.bAppliedAutoBusStart = Value;
            FConfiguration::bAutoBusStart.store(
                Value, std::memory_order_release);
        };

    auto ApplyArenaMapDefaults =
        [&](int SiphonAmount)
        {
            FConfiguration::bSiphon = true;
            FConfiguration::SiphonAmount =
                SiphonAmount;
            FConfiguration::bInfiniteAmmo = true;
            FConfiguration::bInfiniteMats = true;
            ApplyJoinInProgressPreset();
            ApplyKeepInventoryPreset();
            FConfiguration::MaxTickRate = 60.f;
        };

    switch ((Playlist)SelectedPlaylist)
    {
    case Playlist::Gav:
    case Playlist::Retrac1v1:
    case Playlist::RetracTurtle:
    case Playlist::RetracWater:
    case Playlist::TiltedZW:
    case Playlist::Twine1v1:
        FConfiguration::SetLateGameEnabled(false);
        ApplyArenaMapDefaults(200);
        break;
    case Playlist::OnlyUp:
        FConfiguration::SetLateGameEnabled(false);
        FConfiguration::bEnableCheats = false;
        FConfiguration::bInfiniteAmmo = false;
        FConfiguration::bInfiniteMats = false;
        ApplyJoinInProgressPreset();
        FConfiguration::MaxTickRate = 60.f;
        break;
    case Playlist::Boxfight:
        FConfiguration::SetLateGameEnabled(false);
        ApplyArenaMapDefaults(500);
        ApplyAutoBusStartPreset(false);
        break;
    case Playlist::Backrooms:
        FConfiguration::SetLateGameEnabled(false);
        if (VersionInfo.FortniteVersion == 7.40)
        {
            ApplyJoinInProgressPreset();
            CustomPreset.bOwnsForceRespawns = true;
            CustomPreset.bOriginalForceRespawns =
                FConfiguration::bForceRespawns;
            CustomPreset.bAppliedForceRespawns = true;
            FConfiguration::bForceRespawns = true;

            CustomPreset.bOwnsPermanentRespawn = true;
            CustomPreset.bOriginalPermanentRespawn =
                FConfiguration::PermanentRespawn;
            CustomPreset.bAppliedPermanentRespawn = true;
            FConfiguration::PermanentRespawn = true;

            CustomPreset.bOwnsCustomRespawnPoint = true;
            CustomPreset.bOriginalHasCustomRespawnPoint =
                FConfiguration::HasCustomRespawnPoint;
            CustomPreset.OriginalCustomRespawnPoint =
                FConfiguration::CustomRespawnPoint;
            CustomPreset.bAppliedHasCustomRespawnPoint =
                true;
            CustomPreset.AppliedCustomRespawnPoint =
                FVector(0.f, 0.f, 85.275009f);
            FConfiguration::HasCustomRespawnPoint =
                true;
            FConfiguration::CustomRespawnPoint =
                CustomPreset.AppliedCustomRespawnPoint;
        }
        break;
    case Playlist::Playground:
    case Playlist::Creative:
    case Playlist::Event:
        FConfiguration::SetLateGameEnabled(false);
        break;
    default:
        CustomPreset = {};
        break;
    }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

auto WindowWidth = 800;
auto WindowHeight = 600;

inline std::vector<std::pair<AFortPlayerControllerAthena*, UNetConnection*>> AllControllers;

void GUI::RegisterTrickshotSpawnedActor(
    AActor* Actor,
    AFortPlayerControllerAthena* Controller,
    const std::string& CanonicalClassPath)
{
    TrickshotManager::RegisterSpawnedActor(
        Actor, Controller, CanonicalClassPath);
}

namespace TrickshotManager
{
    namespace fs = std::filesystem;

    struct FTrackedSpawnedActor
    {
        TWeakObjectPtr<UWorld> World;
        TWeakObjectPtr<AActor> Actor;
        TWeakObjectPtr<AFortPlayerControllerAthena> Controller;
        std::string ClassPath;
    };

    std::vector<FTrackedSpawnedActor> GTrackedSpawnedActors;

    void ForgetTrackedSpawnedActor(AActor* Actor)
    {
        if (!Actor)
            return;
        std::erase_if(
            GTrackedSpawnedActors,
            [&](const FTrackedSpawnedActor& Entry)
            {
                return Entry.Actor.Get() == Actor;
            });
    }

    enum class EAsyncOperation : uint8
    {
        None,
        Save,
        Load
    };

    enum class EAsyncState : int
    {
        Idle,
        Publishing,
        Pending,
        Running,
        Completed
    };

    constexpr size_t kAsyncNameCapacity = 256;
    constexpr size_t kAsyncMessageCapacity = 1024;
    std::atomic<int> GAsyncState{
        static_cast<int>(EAsyncState::Idle) };
    EAsyncOperation GAsyncOperation = EAsyncOperation::None;
    EAsyncOperation GAsyncResultOperation = EAsyncOperation::None;
    bool GAsyncResultSucceeded = false;
    char GAsyncName[kAsyncNameCapacity]{};
    char GAsyncResultName[kAsyncNameCapacity]{};
    char GAsyncResultMessage[kAsyncMessageCapacity]{};

    constexpr const char* kTrickshotTireClassPath =
        "/Game/Athena/Items/Consumables/TowerGrenade/"
        "Prop_TirePile_Tower.Prop_TirePile_Tower_C";
    constexpr const wchar_t* kTrickshotTireClassPathWide =
        L"/Game/Athena/Items/Consumables/TowerGrenade/"
        L"Prop_TirePile_Tower.Prop_TirePile_Tower_C";
    constexpr size_t kMaximumTrickshotSpawnedObjects = 2048;
    constexpr size_t kMaximumTrickshotWaypoints = 256;
    constexpr size_t kMaximumTrickshotWaypointHistory = 16;
    constexpr size_t kMaximumTrickshotWaypointPhraseBytes = 128;
    constexpr double kMaximumTrickshotCoordinate = 10000000.0;

    struct FPendingTrickshotBuild
    {
        TWeakObjectPtr<UClass> Class;
        FVector Location;
        FRotator Rotation;
        int Level = 0;
        int Parent = -1;
        uint8 AttachmentType = 0;
        int AttachmentSlot = -1;
        bool Mirrored = false;
        int TrapLevel = -1;
        int OriginalTrapLevel = -1;
        bool IsTrap = false;
        bool SupportAnchor = false;
        bool HasSavedSupportAnchor = false;
        bool HasExternalParent = false;
        FVector ExternalParentLocation;
        FRotator ExternalParentRotation;
        int ExternalParentBuildingType = -1;
        std::string ClassPath;
        std::string ItemDefinition;
        std::string ExternalParentClassPath;
        std::string ExternalParentActorPath;
        TWeakObjectPtr<UFortDecoItemDefinition> ResolvedItemDefinition;
        TWeakObjectPtr<UFortDecoItemDefinition> ResolvedConcreteDefinition;
        TWeakObjectPtr<ABuildingSMActor> ResolvedExternalParent;
    };

    struct FPendingTrickshotProp
    {
        TWeakObjectPtr<UClass> Class;
        FVector Location;
        FRotator Rotation;
        FVector Scale{ 1.0, 1.0, 1.0 };
        std::string ClassPath;
    };

    enum class ELoadPhase : uint8
    {
        Cleanup,
        Structures,
        ReleaseStructuralSupport,
        StructureSettle,
        TrapPlacement
    };

    enum class ELoadPumpResult : uint8
    {
        Running,
        Succeeded,
        Failed
    };

    struct FTrickshotLoadJob
    {
        bool Active = false;
        std::string Name;
        TWeakObjectPtr<UWorld> World;
        TWeakObjectPtr<AFortPlayerControllerAthena> Controller;
        std::vector<FPendingTrickshotBuild> Pending;
        std::vector<FPendingTrickshotProp> PendingProps;
        std::vector<int> StructuralOrder;
        std::vector<TWeakObjectPtr<ABuildingSMActor>> SpawnedBuilds;
        std::vector<TWeakObjectPtr<AActor>> SpawnedProps;
        std::vector<TWeakObjectPtr<UClass>> TrackedPropClasses;
        // Exact command-spawned actor instances belonging to the current
        // shared trickshot session, regardless of which player spawned them.
        // Arbitrary class-wide cleanup would destroy natural map actors and
        // unrelated runtime gameplay actors of the same class.
        std::vector<TWeakObjectPtr<AActor>> ExistingTrackedProps;
        // Exact pre-load building identities. Deferred legacy trap recovery
        // must never adopt, mutate, or destroy natural/foreign actors that
        // already occupied an external support before this transaction.
        std::vector<TWeakObjectPtr<ABuildingSMActor>>
            BaselineBuildingActors;
        bool LegacyTireProps = false;
        std::vector<TWeakObjectPtr<UObject>> TemporaryRootedAssets;
        bool RestoreWaypoints = false;
        AFortPlayerControllerAthena::FWaypointMap PendingWaypoints;
        size_t NextStructural = 0;
        uint8 TrapPlacementPass = 0;
        ELoadPhase Phase = ELoadPhase::Cleanup;
        ULONGLONG NextAdvanceMs = 0;
        int LoadedBuilds = 0;
        int LoadedTraps = 0;
        int LoadedProps = 0;
        int Skipped = 0;
        int FailedTrapPlacements = 0;
        int FailedPropPlacements = 0;
        std::string FailureMessage;
    };

    FTrickshotLoadJob GLoadJob;
    bool GTrickshotPackageFindDisabled = false;
    bool GTrickshotPackageLoadDisabled = false;

    constexpr int32 kTrickshotRootSetFlag = 1 << 30;

    bool IsTrickshotAssetRooted(const UObject* Object)
    {
        if (!Object || !SDK::MemReadable(Object, 0x40))
            return false;
        if (Object->Index < 0 || Object->Index >= TUObjectArray::Num())
            return false;
        auto Item = TUObjectArray::GetItemByIndex(Object->Index);
        return Item && Item->GetObject() == Object &&
            (Item->GetFlags() & kTrickshotRootSetFlag) != 0;
    }

    void RemoveTrickshotAssetRoot(UObject* Object)
    {
        if (!Object || !SDK::MemReadable(Object, 0x40))
            return;
        if (Object->Index < 0 || Object->Index >= TUObjectArray::Num())
            return;
        auto Item = TUObjectArray::GetItemByIndex(Object->Index);
        if (!Item || Item->GetObject() != Object)
            return;
        if (SDK::Offsets::bEncryptedObjects)
        {
            *reinterpret_cast<int32*>(
                reinterpret_cast<uint8*>(Item) + 0x4) &=
                ~kTrickshotRootSetFlag;
        }
        else
        {
            Item->Flags &= ~kTrickshotRootSetFlag;
        }
    }

    void ReleaseTrickshotAssetRoots(
        std::vector<TWeakObjectPtr<UObject>>& Assets)
    {
        for (auto It = Assets.rbegin(); It != Assets.rend(); ++It)
        {
            if (auto Object = It->Get())
                RemoveTrickshotAssetRoot(Object);
        }
        Assets.clear();
    }

    struct FScopedTrickshotAssetRoots
    {
        std::vector<TWeakObjectPtr<UObject>> Assets;

        FScopedTrickshotAssetRoots() = default;
        FScopedTrickshotAssetRoots(
            const FScopedTrickshotAssetRoots&) = delete;
        FScopedTrickshotAssetRoots& operator=(
            const FScopedTrickshotAssetRoots&) = delete;

        ~FScopedTrickshotAssetRoots()
        {
            ReleaseTrickshotAssetRoots(Assets);
        }

        void Root(UObject* Object)
        {
            if (!Object || IsTrickshotAssetRooted(Object))
                return;
            Object->AddToRoot();
            if (IsTrickshotAssetRooted(Object))
                Assets.emplace_back(Object);
        }

        std::vector<TWeakObjectPtr<UObject>> Commit()
        {
            std::vector<TWeakObjectPtr<UObject>> Result;
            Result.swap(Assets);
            return Result;
        }
    };

    inline bool Save(const char* Name, std::string& Message);
    inline bool Load(const std::string& Name, std::string& Message);
    inline ELoadPumpResult PumpLoad(std::string& Message);

    struct FStructuralCellKey
    {
        int64 X = 0;
        int64 Y = 0;
        int64 Z = 0;
        uint8 BuildingType = 0;

        bool operator==(const FStructuralCellKey& Other) const
        {
            return X == Other.X && Y == Other.Y && Z == Other.Z &&
                BuildingType == Other.BuildingType;
        }
    };

    struct FStructuralCellKeyHash
    {
        size_t operator()(const FStructuralCellKey& Key) const
        {
            size_t Result = std::hash<int64>{}(Key.X);
            Result ^= std::hash<int64>{}(Key.Y) +
                0x9e3779b97f4a7c15ULL + (Result << 6) + (Result >> 2);
            Result ^= std::hash<int64>{}(Key.Z) +
                0x9e3779b97f4a7c15ULL + (Result << 6) + (Result >> 2);
            Result ^= std::hash<uint8>{}(Key.BuildingType) +
                0x9e3779b97f4a7c15ULL + (Result << 6) + (Result >> 2);
            return Result;
        }
    };

    bool IsTrickshotSessionPlayerBuild(
        const ABuildingSMActor* Build)
    {
        if (!Build || !Build->bPlayerPlaced || Build->bDestroyed ||
            (Build->HasbActorIsBeingDestroyed() &&
             Build->bActorIsBeingDestroyed) ||
            (Build->HasbNetStartup() && Build->bNetStartup))
        {
            return false;
        }
        // Trickshot presets represent the shared setup in this world, not one
        // player's ownership slice. bPlayerPlaced is the cross-version boundary
        // that includes every participant's builds while excluding natural and
        // map-startup structures.
        return true;
    }

    bool TryGetStructuralCellKey(
        const ABuildingSMActor* Build, FStructuralCellKey& OutKey)
    {
        if (!Build || !Build->HasBuildingType())
            return false;
        const FVector Location = Build->K2_GetActorLocation();
        if (!std::isfinite(Location.X) || !std::isfinite(Location.Y) ||
            !std::isfinite(Location.Z))
        {
            return false;
        }
        OutKey.X = static_cast<int64>(std::llround(Location.X));
        OutKey.Y = static_cast<int64>(std::llround(Location.Y));
        OutKey.Z = static_cast<int64>(std::llround(Location.Z));
        OutKey.BuildingType = Build->BuildingType;
        return true;
    }

    bool TryGetStructuralCellKey(
        const FPendingTrickshotBuild& SavedBuild,
        FStructuralCellKey& OutKey)
    {
        auto SavedClass = SavedBuild.Class.Get();
        auto DefaultBuild = SavedClass
            ? static_cast<ABuildingSMActor*>(SavedClass->GetDefaultObj())
            : nullptr;
        if (!DefaultBuild || !DefaultBuild->HasBuildingType() ||
            !std::isfinite(SavedBuild.Location.X) ||
            !std::isfinite(SavedBuild.Location.Y) ||
            !std::isfinite(SavedBuild.Location.Z))
        {
            return false;
        }
        OutKey.X = static_cast<int64>(std::llround(SavedBuild.Location.X));
        OutKey.Y = static_cast<int64>(std::llround(SavedBuild.Location.Y));
        OutKey.Z = static_cast<int64>(std::llround(SavedBuild.Location.Z));
        OutKey.BuildingType = DefaultBuild->BuildingType;
        return true;
    }

    bool IsPortableSupportAnchor(ABuildingSMActor* Build)
    {
        if (!Build)
            return false;
        if ((Build->HasbSupportedDirectly() && Build->bSupportedDirectly) ||
            (Build->HasbForciblyStructurallySupported() &&
             Build->bForciblyStructurallySupported) ||
            (Build->HasSavedDirectlySupportedStatus() &&
             Build->SavedDirectlySupportedStatus == 1))
        {
            return true;
        }
        return Build->GetFunction("IsSupportedByWorld") &&
            Build->IsSupportedByWorld();
    }

    int CanonicalizePendingStructuralCells(
        std::vector<FPendingTrickshotBuild>& Pending)
    {
        const int Count = static_cast<int>(Pending.size());
        std::vector<int> Alias(Count);
        std::vector<bool> Keep(Count, true);
        for (int Index = 0; Index < Count; ++Index)
            Alias[Index] = Index;

        std::unordered_map<FStructuralCellKey, int,
            FStructuralCellKeyHash> LatestByCell;
        int Removed = 0;
        for (int Index = 0; Index < Count; ++Index)
        {
            if (Pending[Index].IsTrap)
                continue;
            FStructuralCellKey Key;
            if (!TryGetStructuralCellKey(Pending[Index], Key))
                continue;
            auto Existing = LatestByCell.find(Key);
            if (Existing != LatestByCell.end())
            {
                const int Previous = Existing->second;
                Keep[Previous] = false;
                Alias[Previous] = Index;
                Existing->second = Index;
                ++Removed;
                SDK::DbgLog(
                    "[TrickshotLoad] duplicate structural cell old=%d new=%d oldClass=%s newClass=%s\n",
                    Previous, Index, Pending[Previous].ClassPath.c_str(),
                    Pending[Index].ClassPath.c_str());
            }
            else
            {
                LatestByCell.emplace(Key, Index);
            }
        }
        if (!Removed)
            return 0;

        auto ResolveAlias = [&](int Index)
        {
            int Guard = 0;
            while (Index >= 0 && Index < Count && Alias[Index] != Index &&
                Guard++ < Count)
            {
                Index = Alias[Index];
            }
            return Index;
        };

        std::vector<int> OldToNew(Count, -1);
        int NewCount = 0;
        for (int Index = 0; Index < Count; ++Index)
        {
            if (Keep[Index])
                OldToNew[Index] = NewCount++;
        }

        std::vector<FPendingTrickshotBuild> Canonical;
        Canonical.reserve(NewCount);
        for (int Index = 0; Index < Count; ++Index)
        {
            if (!Keep[Index])
                continue;
            auto SavedBuild = std::move(Pending[Index]);
            if (SavedBuild.IsTrap && !SavedBuild.HasExternalParent &&
                SavedBuild.Parent >= 0 && SavedBuild.Parent < Count)
            {
                const int CanonicalParent = ResolveAlias(SavedBuild.Parent);
                SavedBuild.Parent = CanonicalParent >= 0 &&
                    CanonicalParent < Count
                    ? OldToNew[CanonicalParent] : SavedBuild.Parent;
            }
            Canonical.push_back(std::move(SavedBuild));
        }
        Pending = std::move(Canonical);
        return Removed;
    }

    void EnsurePortableSupportAnchors(
        std::vector<FPendingTrickshotBuild>& Pending)
    {
        std::unordered_map<uint64, double> MinimumZByColumn;
        auto ColumnKey = [](const FVector& Location)
        {
            const auto X = static_cast<uint32>(
                static_cast<int32>(std::llround(Location.X)));
            const auto Y = static_cast<uint32>(
                static_cast<int32>(std::llround(Location.Y)));
            return (static_cast<uint64>(X) << 32) |
                static_cast<uint64>(Y);
        };
        for (const auto& SavedBuild : Pending)
        {
            if (SavedBuild.IsTrap || SavedBuild.HasSavedSupportAnchor)
                continue;
            auto [It, Inserted] = MinimumZByColumn.try_emplace(
                ColumnKey(SavedBuild.Location), SavedBuild.Location.Z);
            if (!Inserted)
                It->second = (std::min)(It->second, SavedBuild.Location.Z);
        }

        int AddedAnchors = 0;
        for (auto& SavedBuild : Pending)
        {
            if (SavedBuild.IsTrap || SavedBuild.HasSavedSupportAnchor)
                continue;
            const auto It = MinimumZByColumn.find(
                ColumnKey(SavedBuild.Location));
            if (It != MinimumZByColumn.end() &&
                std::abs(SavedBuild.Location.Z - It->second) <= 1.0)
            {
                SavedBuild.SupportAnchor = true;
                ++AddedAnchors;
            }
        }
        if (AddedAnchors > 0)
        {
            SDK::DbgLog(
                "[TrickshotLoad] retained portable support for %d legacy structural records\n",
                AddedAnchors);
        }
    }

    double RotationDelta(double Left, double Right)
    {
        double Delta = std::fmod(std::abs(Left - Right), 360.0);
        return Delta > 180.0 ? 360.0 - Delta : Delta;
    }

    ABuildingSMActor* ResolveExternalTrickshotParent(
        const FPendingTrickshotBuild& SavedBuild,
        const std::vector<ABuildingSMActor*>& Candidates)
    {
        ABuildingSMActor* ExactMatch = nullptr;
        ABuildingSMActor* FallbackMatch = nullptr;
        int FallbackMatches = 0;
        for (auto Candidate : Candidates)
        {
            if (!Candidate ||
                (Candidate->bPlayerPlaced &&
                 !(Candidate->HasbNetStartup() &&
                   Candidate->bNetStartup)) ||
                Candidate->bDestroyed ||
                (Candidate->HasbActorIsBeingDestroyed() &&
                 Candidate->bActorIsBeingDestroyed) ||
                Candidate->Cast<ABuildingTrap>())
            {
                continue;
            }

            if (SavedBuild.ExternalParentBuildingType >= 0 &&
                (!Candidate->HasBuildingType() ||
                 Candidate->BuildingType !=
                    static_cast<uint8>(
                        SavedBuild.ExternalParentBuildingType)))
            {
                continue;
            }

            const FVector Location = Candidate->K2_GetActorLocation();
            const double DX = Location.X - SavedBuild.ExternalParentLocation.X;
            const double DY = Location.Y - SavedBuild.ExternalParentLocation.Y;
            const double DZ = Location.Z - SavedBuild.ExternalParentLocation.Z;
            if (DX * DX + DY * DY + DZ * DZ > 4.0)
                continue;

            const FRotator Rotation = Candidate->K2_GetActorRotation();
            if (RotationDelta(Rotation.Pitch,
                    SavedBuild.ExternalParentRotation.Pitch) > 1.0 ||
                RotationDelta(Rotation.Yaw,
                    SavedBuild.ExternalParentRotation.Yaw) > 1.0 ||
                RotationDelta(Rotation.Roll,
                    SavedBuild.ExternalParentRotation.Roll) > 1.0)
            {
                continue;
            }

            const std::string ClassPath = FStringToStdString(
                UKismetSystemLibrary::GetPathName(Candidate->Class));
            if (ClassPath != SavedBuild.ExternalParentClassPath)
                continue;

            if (!SavedBuild.ExternalParentActorPath.empty())
            {
                const std::string ActorPath = FStringToStdString(
                    UKismetSystemLibrary::GetPathName(Candidate));
                if (ActorPath == SavedBuild.ExternalParentActorPath)
                {
                    if (ExactMatch && ExactMatch != Candidate)
                        return nullptr;
                    ExactMatch = Candidate;
                    continue;
                }
            }

            ++FallbackMatches;
            if (FallbackMatches == 1)
                FallbackMatch = Candidate;
        }
        if (ExactMatch)
            return ExactMatch;
        return FallbackMatches == 1 ? FallbackMatch : nullptr;
    }

    bool IsLiveTrickshotAsset(
        const UObject* Object, const UClass* ExpectedClass)
    {
        if (!Object || !ExpectedClass ||
            !SDK::MemReadable(Object, 0x40))
        {
            return false;
        }
        const int32 ObjectIndex = static_cast<int32>(Object->Index);
        if (ObjectIndex < 0 || ObjectIndex >= TUObjectArray::Num() ||
            TUObjectArray::GetObjectByIndex(ObjectIndex) != Object)
        {
            return false;
        }
        return Object->IsA(ExpectedClass);
    }

    bool IsLiveTrackedSpawnedActor(
        AActor* Actor, UWorld* ExpectedWorld)
    {
        if (!ExpectedWorld ||
            !IsLiveTrickshotAsset(Actor, AActor::StaticClass()) ||
            (Actor->HasbActorIsBeingDestroyed() &&
             Actor->bActorIsBeingDestroyed))
        {
            return false;
        }
        if (auto Build = Actor->Cast<ABuildingSMActor>())
        {
            if (Build->HasbDestroyed() && Build->bDestroyed)
                return false;
        }
        return true;
    }

    std::string GetCanonicalTrackedClassPath(UClass* Class)
    {
        if (!Class)
            return {};
        if (VersionInfo.FortniteVersion < 32.00)
        {
            return FStringToStdString(
                UKismetSystemLibrary::GetPathName(Class));
        }
        auto Outer = Class->Outer;
        if (!Outer)
            return {};
        const auto OuterName = Outer->Name.ToString();
        const auto ClassName = Class->Name.ToString();
        if (OuterName.empty() || ClassName.empty())
            return {};
        return std::string(OuterName.c_str()) + "." +
            std::string(ClassName.c_str());
    }

    bool IsUnsafeTrickshotLifecycleClass(UClass* Class)
    {
        auto DefaultActor = Class
            ? static_cast<AActor*>(Class->GetDefaultObj()) : nullptr;
        return !DefaultActor ||
            !DefaultActor->IsA(AActor::StaticClass()) ||
            DefaultActor->IsA(
                AFortPlayerControllerAthena::StaticClass()) ||
            DefaultActor->IsA(
                AFortPlayerPawnAthena::StaticClass()) ||
            DefaultActor->IsA(
                AFortPlayerStateAthena::StaticClass()) ||
            DefaultActor->IsA(AFortGameMode::StaticClass()) ||
            DefaultActor->IsA(AFortGameStateAthena::StaticClass());
    }

    void PruneTrackedSpawnedActors(UWorld* CurrentWorld)
    {
        std::erase_if(
            GTrackedSpawnedActors,
            [&](const FTrackedSpawnedActor& Entry)
            {
                return Entry.World.Get() != CurrentWorld ||
                    !IsLiveTrackedSpawnedActor(
                        Entry.Actor.Get(), CurrentWorld) ||
                    Entry.ClassPath.empty();
            });
    }

    bool RegisterSpawnedActorInternal(
        AActor* Actor,
        AFortPlayerControllerAthena* Controller,
        const std::string& CanonicalClassPath,
        bool RequireEnabledTab)
    {
        // Both UI switches are sampled at successful command-spawn time.
        // Turning either off later does not silently forget an existing setup.
        if ((RequireEnabledTab &&
             !FConfiguration::bEnableTrickshotTab.load(
                 std::memory_order_acquire)) ||
            (RequireEnabledTab &&
             !FConfiguration::bSaveAndTrackSpawnedObjects.load(
                 std::memory_order_acquire)) ||
            !Actor || !Controller || CanonicalClassPath.empty())
        {
            return false;
        }

        auto World = UWorld::GetWorld();
        if (!IsLiveTrackedSpawnedActor(Actor, World) ||
            !IsLiveTrickshotAsset(
                Controller,
                AFortPlayerControllerAthena::StaticClass()))
        {
            return false;
        }
        if (IsUnsafeTrickshotLifecycleClass(Actor->Class))
        {
            SDK::DbgLog(
                "[TrickshotSpawn] rejected lifecycle actor=%p class=%s\n",
                Actor, CanonicalClassPath.c_str());
            return false;
        }
        if (CanonicalClassPath.find('\0') != std::string::npos ||
            GetCanonicalTrackedClassPath(Actor->Class) !=
                CanonicalClassPath)
        {
            SDK::DbgLog(
                "[TrickshotSpawn] rejected noncanonical class actor=%p class=%s\n",
                Actor, CanonicalClassPath.c_str());
            return false;
        }

        PruneTrackedSpawnedActors(World);
        for (auto& Entry : GTrackedSpawnedActors)
        {
            if (Entry.Actor.Get() == Actor)
            {
                Entry.World = TWeakObjectPtr<UWorld>(World);
                Entry.Controller =
                    TWeakObjectPtr<AFortPlayerControllerAthena>(Controller);
                Entry.ClassPath = CanonicalClassPath;
                return true;
            }
        }
        const size_t SessionActorCount = static_cast<size_t>(std::count_if(
            GTrackedSpawnedActors.begin(), GTrackedSpawnedActors.end(),
            [&](const FTrackedSpawnedActor& Entry)
            {
                return Entry.World.Get() == World;
            }));
        if (SessionActorCount >= kMaximumTrickshotSpawnedObjects)
        {
            SDK::DbgLog(
                "[TrickshotSpawn] session tracking limit reached controller=%p limit=%zu class=%s\n",
                Controller, kMaximumTrickshotSpawnedObjects,
                CanonicalClassPath.c_str());
            return false;
        }
        GTrackedSpawnedActors.push_back({
            TWeakObjectPtr<UWorld>(World),
            TWeakObjectPtr<AActor>(Actor),
            TWeakObjectPtr<AFortPlayerControllerAthena>(Controller),
            CanonicalClassPath });
        SDK::DbgLog(
            "[TrickshotSpawn] registered actor=%p controller=%p class=%s\n",
            Actor, Controller, CanonicalClassPath.c_str());
        return true;
    }

    void RegisterSpawnedActor(
        AActor* Actor,
        AFortPlayerControllerAthena* Controller,
        const std::string& CanonicalClassPath)
    {
        (void)RegisterSpawnedActorInternal(
            Actor, Controller, CanonicalClassPath, true);
    }

    const UObject* FindTrickshotAssetGuarded(
        const wchar_t* Path, const UClass* ExpectedClass)
    {
        if (GTrickshotPackageFindDisabled || !Path || !*Path ||
            !ExpectedClass || !SDK::Offsets::StaticFindObject)
        {
            return nullptr;
        }

        ++GGuardedNativeCallDepth;
        const UObject* Result = nullptr;
        bool bFaulted = false;
        __try
        {
            Result = SDK::StaticFindObject(Path, ExpectedClass);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Result = nullptr;
            bFaulted = true;
        }
        --GGuardedNativeCallDepth;
        if (bFaulted)
        {
            GTrickshotPackageFindDisabled = true;
            SDK::DbgLog(
                "[TrickshotLoad] StaticFindObject(%ls) faulted; resident lookup disabled\n",
                Path);
        }
        return IsLiveTrickshotAsset(Result, ExpectedClass)
            ? Result : nullptr;
    }

    const UObject* LoadTrickshotAssetGuarded(
        const wchar_t* Path, const UClass* ExpectedClass)
    {
        if (GTrickshotPackageLoadDisabled || !Path || !*Path ||
            !ExpectedClass || !SDK::Offsets::StaticLoadObject)
        {
            return nullptr;
        }

        ++GGuardedNativeCallDepth;
        const UObject* Result = nullptr;
        bool bFaulted = false;
        __try
        {
            Result = SDK::StaticLoadObject(Path, ExpectedClass);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Result = nullptr;
            bFaulted = true;
        }
        --GGuardedNativeCallDepth;
        if (bFaulted)
        {
            GTrickshotPackageLoadDisabled = true;
            SDK::DbgLog(
                "[TrickshotLoad] StaticLoadObject(%ls) faulted; package fallback disabled\n",
                Path);
        }
        else if (!IsLiveTrickshotAsset(Result, ExpectedClass))
        {
            SDK::DbgLog(
                "[TrickshotLoad] StaticLoadObject returned no valid asset path=%ls expected=%p\n",
                Path, ExpectedClass);
        }
        return IsLiveTrickshotAsset(Result, ExpectedClass)
            ? Result : nullptr;
    }

    UClass* ResolveTrickshotTireClass(bool bAllowPackageLoad)
    {
        auto ClassObject = const_cast<UObject*>(
            FindTrickshotAssetGuarded(
                kTrickshotTireClassPathWide,
                UClass::StaticClass()));
        if (!ClassObject && bAllowPackageLoad)
        {
            ClassObject = const_cast<UObject*>(
                LoadTrickshotAssetGuarded(
                    kTrickshotTireClassPathWide,
                    UClass::StaticClass()));
        }
        auto Class = IsLiveTrickshotAsset(
            ClassObject, UClass::StaticClass())
            ? static_cast<UClass*>(ClassObject) : nullptr;
        auto DefaultActor = Class
            ? static_cast<AActor*>(Class->GetDefaultObj()) : nullptr;
        return DefaultActor && DefaultActor->IsA(AActor::StaticClass())
            ? Class : nullptr;
    }

    bool IsTrackedTrickshotPropClass(
        UClass* Class,
        const std::vector<TWeakObjectPtr<UClass>>& TrackedClasses)
    {
        if (!Class)
            return false;
        for (const auto& Tracked : TrackedClasses)
        {
            if (Tracked.Get() == Class)
                return true;
        }
        return false;
    }

    bool IsSessionTrickshotTire(
        AActor* Actor,
        UClass* TireClass)
    {
        if (!Actor || Actor->Class != TireClass ||
            (Actor->HasbActorIsBeingDestroyed() &&
             Actor->bActorIsBeingDestroyed) ||
            (Actor->HasbNetStartup() && Actor->bNetStartup))
        {
            return false;
        }
        if (auto Build = Actor->Cast<ABuildingSMActor>())
        {
            if (Build->HasbDestroyed() && Build->bDestroyed)
                return false;
        }

        // This exact functional class is the only legacy prop adopted by a
        // world scan. Net-startup instances were rejected above, so every
        // remaining instance belongs to the active player-created session,
        // regardless of which participant deployed it or whether they left.
        return true;
    }

    void CleanupPartialLoad()
    {
        if (!GLoadJob.Active ||
            GLoadJob.World.Get() != UWorld::GetWorld())
        {
            return;
        }

        // Free-standing spawned objects may overlap or depend on the restored
        // structure, so remove them before attached children and supports.
        for (auto It = GLoadJob.SpawnedProps.rbegin();
            It != GLoadJob.SpawnedProps.rend(); ++It)
        {
            auto Actor = It->Get();
            if (Actor &&
                !(Actor->HasbActorIsBeingDestroyed() &&
                  Actor->bActorIsBeingDestroyed))
            {
                ForgetTrackedSpawnedActor(Actor);
                Actor->K2_DestroyActor();
            }
        }

        // Traps and other attached children must die before their supports so
        // a native parent cascade cannot make this snapshot revisit a child.
        for (int Index = 0;
            Index < static_cast<int>(GLoadJob.Pending.size()); ++Index)
        {
            if (!GLoadJob.Pending[Index].IsTrap)
                continue;
            auto Actor = GLoadJob.SpawnedBuilds[Index].Get();
            if (Actor && !Actor->bDestroyed &&
                !(Actor->HasbActorIsBeingDestroyed() &&
                  Actor->bActorIsBeingDestroyed))
                Actor->SilentDie(true);
        }
        for (auto It = GLoadJob.StructuralOrder.rbegin();
            It != GLoadJob.StructuralOrder.rend(); ++It)
        {
            const int Index = *It;
            auto Actor = GLoadJob.SpawnedBuilds[Index].Get();
            if (Actor && !Actor->bDestroyed &&
                !(Actor->HasbActorIsBeingDestroyed() &&
                  Actor->bActorIsBeingDestroyed))
                Actor->SilentDie(true);
        }
    }

    void PublishResult(
        EAsyncOperation Operation,
        bool Succeeded,
        const std::string& Name,
        const std::string& Message)
    {
        GAsyncResultOperation = Operation;
        GAsyncResultSucceeded = Succeeded;
        strncpy_s(
            GAsyncResultName, Name.c_str(), _TRUNCATE);
        strncpy_s(
            GAsyncResultMessage, Message.c_str(), _TRUNCATE);
        GAsyncState.store(
            static_cast<int>(EAsyncState::Completed),
            std::memory_order_release);
    }

    void PumpActiveLoad()
    {
        const std::string Name = GLoadJob.Name;
        std::string ResultMessage;
        ELoadPumpResult Result = ELoadPumpResult::Failed;
        try
        {
            Result = PumpLoad(ResultMessage);
        }
        catch (const std::exception& Error)
        {
            ResultMessage =
                std::string("Failed to load trickshot: ") +
                Error.what();
        }
        catch (...)
        {
            ResultMessage = "Failed to load trickshot.";
        }

        if (Result == ELoadPumpResult::Running)
            return;

        const bool Succeeded =
            Result == ELoadPumpResult::Succeeded;
        if (!Succeeded)
            CleanupPartialLoad();
        ReleaseTrickshotAssetRoots(GLoadJob.TemporaryRootedAssets);
        GLoadJob = FTrickshotLoadJob{};
        PublishResult(
            EAsyncOperation::Load, Succeeded,
            Name, ResultMessage);
    }

    bool RequestOperation(
        EAsyncOperation Operation,
        const char* Name,
        std::string& ImmediateMessage)
    {
        int Expected = static_cast<int>(EAsyncState::Idle);
        if (!GAsyncState.compare_exchange_strong(
                Expected,
                static_cast<int>(EAsyncState::Publishing),
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            ImmediateMessage =
                "A trickshot save or load is already in progress.";
            return false;
        }

        GAsyncOperation = Operation;
        strncpy_s(GAsyncName, Name ? Name : "", _TRUNCATE);
        GAsyncState.store(
            static_cast<int>(EAsyncState::Pending),
            std::memory_order_release);
        ImmediateMessage = Operation == EAsyncOperation::Save
            ? "Saving trickshot..."
            : "Loading trickshot...";
        return true;
    }

    bool RequestSave(
        const char* Name,
        std::string& ImmediateMessage)
    {
        return RequestOperation(
            EAsyncOperation::Save, Name, ImmediateMessage);
    }

    bool RequestLoad(
        const std::string& Name,
        std::string& ImmediateMessage)
    {
        return RequestOperation(
            EAsyncOperation::Load, Name.c_str(), ImmediateMessage);
    }

    bool IsBusy()
    {
        return GAsyncState.load(std::memory_order_acquire) !=
            static_cast<int>(EAsyncState::Idle);
    }

    bool ConsumeResult(
        EAsyncOperation& Operation,
        bool& Succeeded,
        std::string& Name,
        std::string& Message)
    {
        if (GAsyncState.load(std::memory_order_acquire) !=
            static_cast<int>(EAsyncState::Completed))
        {
            return false;
        }

        Operation = GAsyncResultOperation;
        Succeeded = GAsyncResultSucceeded;
        Name = GAsyncResultName;
        Message = GAsyncResultMessage;
        GAsyncState.store(
            static_cast<int>(EAsyncState::Idle),
            std::memory_order_release);
        return true;
    }

    void GameThreadTick()
    {
        const int CurrentState =
            GAsyncState.load(std::memory_order_acquire);
        if (CurrentState == static_cast<int>(EAsyncState::Running))
        {
            if (GAsyncOperation == EAsyncOperation::Load &&
                GLoadJob.Active)
            {
                PumpActiveLoad();
            }
            return;
        }

        int Expected = static_cast<int>(EAsyncState::Pending);
        if (!GAsyncState.compare_exchange_strong(
                Expected,
                static_cast<int>(EAsyncState::Running),
                std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            return;
        }

        const EAsyncOperation Operation = GAsyncOperation;
        const std::string Name = GAsyncName;
        std::string ResultMessage;
        bool Succeeded = false;
        try
        {
            if (Operation == EAsyncOperation::Save)
                Succeeded = Save(Name.c_str(), ResultMessage);
            else if (Operation == EAsyncOperation::Load)
            {
                Succeeded = Load(Name, ResultMessage);
                if (Succeeded && GLoadJob.Active)
                {
                    // Cleanup is the first staged phase. Keeping the state at
                    // Running prevents the render thread from publishing a
                    // result or accepting another operation between layers.
                    PumpActiveLoad();
                    return;
                }
            }
            else
                ResultMessage = "Invalid trickshot operation.";
        }
        catch (const std::exception& Error)
        {
            ResultMessage =
                std::string("Trickshot operation failed: ") +
                Error.what();
        }
        catch (...)
        {
            ResultMessage = "Trickshot operation failed.";
        }

        PublishResult(
            Operation, Succeeded, Name, ResultMessage);
    }

    inline fs::path GetDirectory()
    {
        char Path[MAX_PATH]{};
        if (FAILED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, Path)))
            return {};

        fs::path Directory = fs::path(Path) / "Magnesium" / "trickshots";
        std::error_code Error;
        fs::create_directories(Directory, Error);
        return Error ? fs::path{} : Directory;
    }

    inline std::string SanitizeName(const char* Name)
    {
        std::string Result = Name ? Name : "";
        Result.erase(Result.begin(), std::find_if(Result.begin(), Result.end(), [](unsigned char Character) { return !std::isspace(Character); }));
        Result.erase(std::find_if(Result.rbegin(), Result.rend(), [](unsigned char Character) { return !std::isspace(Character); }).base(), Result.end());

        for (auto& Character : Result)
        {
            if (Character == '<' || Character == '>' || Character == ':' || Character == '"' || Character == '/' ||
                Character == '\\' || Character == '|' || Character == '?' || Character == '*')
                Character = '_';
        }

        return Result;
    }

    inline std::string GetCurrentMapPath()
    {
        auto World = UWorld::GetWorld();
        return World ? FStringToStdString(UKismetSystemLibrary::GetPathName(World)) : "";
    }

    inline AFortPlayerControllerAthena* GetHostController()
    {
        auto World = UWorld::GetWorld();
        auto Driver = World ? static_cast<UNetDriver*>(World->NetDriver) : nullptr;
        if (!Driver)
            return nullptr;

        for (auto Connection : Driver->ClientConnections)
        {
            if (Connection && Connection->PlayerController)
                return static_cast<AFortPlayerControllerAthena*>(Connection->PlayerController);
        }
        return nullptr;
    }

    bool IsValidSavedWaypointLocation(const FVector& Location)
    {
        return std::isfinite(Location.X) &&
            std::isfinite(Location.Y) &&
            std::isfinite(Location.Z) &&
            std::abs(Location.X) <= kMaximumTrickshotCoordinate &&
            std::abs(Location.Y) <= kMaximumTrickshotCoordinate &&
            std::abs(Location.Z) <= kMaximumTrickshotCoordinate;
    }

    bool SerializeWaypoints(
        nlohmann::json& Root,
        size_t& SavedWaypointCount,
        std::string& Error)
    {
        SavedWaypointCount = 0;
        auto Snapshot =
            AFortPlayerControllerAthena::SnapshotWaypoints();
        if (Snapshot.size() > kMaximumTrickshotWaypoints)
        {
            Error = "There are too many saved waypoints (maximum 256).";
            return false;
        }

        std::vector<std::string> Phrases;
        Phrases.reserve(Snapshot.size());
        for (const auto& [Phrase, History] : Snapshot)
            Phrases.push_back(Phrase);
        std::sort(Phrases.begin(), Phrases.end());

        nlohmann::json Saved = nlohmann::json::array();
        for (const auto& Phrase : Phrases)
        {
            const auto Existing = Snapshot.find(Phrase);
            if (Existing == Snapshot.end())
                continue;
            const auto& History = Existing->second;
            if (Phrase.empty() ||
                Phrase.size() > kMaximumTrickshotWaypointPhraseBytes ||
                Phrase.find('\0') != std::string::npos ||
                History.empty() ||
                History.size() > kMaximumTrickshotWaypointHistory)
            {
                Error = "A saved waypoint name or history is invalid.";
                return false;
            }

            nlohmann::json Locations = nlohmann::json::array();
            for (const auto& Location : History)
            {
                if (!IsValidSavedWaypointLocation(Location))
                {
                    Error = "A saved waypoint location is invalid.";
                    return false;
                }
                Locations.push_back({
                    Location.X, Location.Y, Location.Z });
            }
            Saved.push_back({
                { "phrase", Phrase },
                { "locations", std::move(Locations) }
            });
            ++SavedWaypointCount;
        }
        Root["waypoints"] = std::move(Saved);
        return true;
    }

    AFortPlayerControllerAthena::FWaypointMap ParseSavedWaypoints(
        const nlohmann::json& Saved)
    {
        if (!Saved.is_array() ||
            Saved.size() > kMaximumTrickshotWaypoints)
        {
            throw std::runtime_error("invalid trickshot waypoints");
        }

        AFortPlayerControllerAthena::FWaypointMap Result;
        Result.reserve(Saved.size());
        for (const auto& Record : Saved)
        {
            if (!Record.is_object() ||
                !Record.contains("phrase") ||
                !Record["phrase"].is_string() ||
                !Record.contains("locations") ||
                !Record["locations"].is_array())
            {
                throw std::runtime_error("invalid trickshot waypoint");
            }
            const std::string Phrase =
                Record["phrase"].get<std::string>();
            const auto& Locations = Record["locations"];
            if (Phrase.empty() ||
                Phrase.size() > kMaximumTrickshotWaypointPhraseBytes ||
                Phrase.find('\0') != std::string::npos ||
                Locations.empty() ||
                Locations.size() > kMaximumTrickshotWaypointHistory ||
                Result.contains(Phrase))
            {
                throw std::runtime_error("invalid trickshot waypoint");
            }

            AFortPlayerControllerAthena::FWaypointHistory History;
            History.reserve(Locations.size());
            for (const auto& SavedLocation : Locations)
            {
                if (!SavedLocation.is_array() ||
                    SavedLocation.size() != 3)
                {
                    throw std::runtime_error(
                        "invalid trickshot waypoint location");
                }
                FVector Location(
                    SavedLocation[0].get<double>(),
                    SavedLocation[1].get<double>(),
                    SavedLocation[2].get<double>());
                if (!IsValidSavedWaypointLocation(Location))
                {
                    throw std::runtime_error(
                        "invalid trickshot waypoint location");
                }
                History.push_back(Location);
            }
            Result.emplace(Phrase, std::move(History));
        }
        return Result;
    }

    inline uint8 GuessAttachmentType(const std::string& ClassPath)
    {
        std::string Lower = ClassPath;
        std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
        if (Lower.find("wall") != std::string::npos)
            return 1;
        if (Lower.find("ceiling") != std::string::npos)
            return 2;
        if (Lower.find("stair") != std::string::npos)
            return 8;
        return 0;
    }

    struct FScopedPresetReadGuard
    {
        HANDLE Handle = INVALID_HANDLE_VALUE;
        DWORD Error = ERROR_SUCCESS;

        explicit FScopedPresetReadGuard(
            const fs::path& Path,
            bool bAllowAtomicReplacement = false)
        {
            const DWORD ShareMode = bAllowAtomicReplacement
                ? FILE_SHARE_READ | FILE_SHARE_DELETE
                : FILE_SHARE_READ;
            Handle = CreateFileW(
                Path.c_str(), GENERIC_READ, ShareMode,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (Handle == INVALID_HANDLE_VALUE)
                Error = GetLastError();
        }

        ~FScopedPresetReadGuard()
        {
            if (Handle != INVALID_HANDLE_VALUE)
                CloseHandle(Handle);
        }

        FScopedPresetReadGuard(const FScopedPresetReadGuard&) = delete;
        FScopedPresetReadGuard& operator=(
            const FScopedPresetReadGuard&) = delete;

        explicit operator bool() const
        {
            return Handle != INVALID_HANDLE_VALUE;
        }

        bool ShouldRetryScan() const
        {
            return Error == ERROR_SHARING_VIOLATION ||
                Error == ERROR_LOCK_VIOLATION ||
                Error == ERROR_FILE_NOT_FOUND ||
                Error == ERROR_PATH_NOT_FOUND;
        }
    };

    inline std::vector<std::string> GetSavedNames(
        bool* bScanSucceeded = nullptr)
    {
        if (bScanSucceeded)
            *bScanSucceeded = false;

        std::vector<std::string> Names;
        try
        {
            auto Directory = GetDirectory();
            if (Directory.empty())
                return Names;

            std::error_code Error;
            fs::directory_iterator Iterator(Directory, Error);
            const fs::directory_iterator End;
            while (!Error && Iterator != End)
            {
                const auto& Entry = *Iterator;
                std::error_code EntryError;
                const bool bRegularFile =
                    Entry.is_regular_file(EntryError);
                if (EntryError)
                {
                    Error = EntryError;
                    break;
                }

                const auto Path = Entry.path();
                auto Extension = Path.extension().string();
                std::transform(
                    Extension.begin(), Extension.end(),
                    Extension.begin(),
                    [](unsigned char Character)
                    {
                        return static_cast<char>(
                            std::tolower(Character));
                    });
                if (bRegularFile && Extension == ".json")
                {
                    // A drag/copy can publish the destination filename before
                    // its writer closes. Opening with read-only sharing fails
                    // while any writer is still active, so the last good GUI
                    // list is retained until the copy is actually loadable.
                    FScopedPresetReadGuard ReadGuard(
                        Path, true);
                    if (!ReadGuard)
                    {
                        if (ReadGuard.ShouldRetryScan())
                        {
                            Error = std::error_code(
                                static_cast<int>(ReadGuard.Error),
                                std::system_category());
                            break;
                        }
                        Iterator.increment(Error);
                        continue;
                    }
                    Names.push_back(Path.stem().string());
                }
                Iterator.increment(Error);
            }

            if (Error)
                return {};
        }
        catch (...)
        {
            return {};
        }

        std::sort(Names.begin(), Names.end());
        Names.erase(
            std::unique(Names.begin(), Names.end()), Names.end());
        if (bScanSucceeded)
            *bScanSucceeded = true;
        return Names;
    }

    inline bool Delete(const std::string& Name, std::string& Message)
    {
        const auto Directory = GetDirectory();
        if (Name.empty() || Directory.empty())
        {
            Message = "Select a saved trickshot to delete.";
            return false;
        }

        std::error_code Error;
        const bool Removed = fs::remove(Directory / (Name + ".json"), Error);
        Message = Removed && !Error ? "Deleted " + Name + "." : "Could not delete the selected trickshot.";
        return Removed && !Error;
    }

    inline bool OpenDirectory(std::string& Message)
    {
        const auto Directory = GetDirectory();
        if (Directory.empty())
        {
            Message = "Could not open the trickshot folder.";
            return false;
        }

        const auto Result = ShellExecuteA(nullptr, "open", Directory.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        const bool Opened = reinterpret_cast<INT_PTR>(Result) > 32;
        if (!Opened)
            Message = "Could not open the trickshot folder.";
        return Opened;
    }

    inline bool Save(const char* Name, std::string& Message)
    {
        const std::string SafeName = SanitizeName(Name);
        const auto Directory = GetDirectory();
        auto Controller = GetHostController();
        if (SafeName.empty() || Directory.empty() || !UWorld::GetWorld() ||
            !Controller)
        {
            Message = SafeName.empty() ? "Enter a trickshot name." :
                !Controller ? "A connected player is required to save builds." :
                "Unable to access the trickshot folder or world.";
            return false;
        }

        nlohmann::json Root;
        Root["version"] = 7;
        Root["fortnite"] = VersionInfo.FortniteVersion;
        Root["name"] = SafeName;
        Root["map"] = GetCurrentMapPath();
        Root["builds"] = nlohmann::json::array();
        Root["props"] = nlohmann::json::array();

        // The functional Tower tire is a free-standing gameplay prop. Keep it
        // out of the structural/trap graph and persist its world transform in
        // a dedicated allowlisted collection.
        auto TireClass = ResolveTrickshotTireClass(false);

        auto CurrentWorld = UWorld::GetWorld();
        PruneTrackedSpawnedActors(CurrentWorld);
        const bool bSaveSpawnedObjects =
            FConfiguration::bSaveAndTrackSpawnedObjects.load(
                std::memory_order_acquire);

        // A Tower/Port-a-Fort deployment creates its functional tire from
        // Blueprint rather than through the cheat summon command, so it never
        // reaches RegisterTrickshotSpawnedActor. Adopt only this exact,
        // allowlisted runtime class at save time. Net-startup checks keep map
        // tires out while including tires deployed by every participant (even
        // one who has since disconnected). Later cleanup remains instance-based.
        if (bSaveSpawnedObjects &&
            FConfiguration::bEnableTrickshotTab.load(
                std::memory_order_acquire) &&
            TireClass)
        {
            TArray<AActor*> DeployedTires;
            Utils::GetAll<AActor>(TireClass, DeployedTires);
            for (auto Tire : DeployedTires)
            {
                if (!IsSessionTrickshotTire(Tire, TireClass))
                {
                    continue;
                }
                (void)RegisterSpawnedActorInternal(
                    Tire, Controller, kTrickshotTireClassPath, false);
            }
            DeployedTires.Free();
        }

        std::vector<FTrackedSpawnedActor*> TrackedProps;
        std::unordered_set<AActor*> TrackedPropActors;
        for (auto& Entry : GTrackedSpawnedActors)
        {
            auto Actor = Entry.Actor.Get();
            if (Entry.World.Get() != CurrentWorld ||
                !IsLiveTrackedSpawnedActor(Actor, CurrentWorld))
            {
                continue;
            }
            if (bSaveSpawnedObjects)
                TrackedProps.push_back(&Entry);
            TrackedPropActors.insert(Actor);
        }

        TArray<ABuildingSMActor*> Builds;
        Utils::GetAll<ABuildingSMActor>(Builds);
        std::vector<ABuildingSMActor*> SavedBuilds;
        std::unordered_map<ABuildingSMActor*, ABuildingSMActor*> AttachedParents;

        auto RememberAttachment = [&](ABuildingSMActor* Parent, ABuildingSMActor* Child, bool bAuthoritative = false)
        {
            if (!Child || Child->bDestroyed ||
                (Child->HasbActorIsBeingDestroyed() &&
                 Child->bActorIsBeingDestroyed))
                return;

            if (bAuthoritative)
            {
                if (Parent && Parent != Child)
                    AttachedParents[Child] = Parent;
                else
                    AttachedParents.erase(Child);
            }
            else if (Parent && Parent != Child)
                AttachedParents.try_emplace(Child, Parent);
        };

        // Capture parent-side arrays as a compatibility fallback. They can retain
        // stale children on some versions, so child-side links override them below.
        for (auto Build : Builds)
        {
            if (!Build)
                continue;

            if (Build->HasAttachedBuildingActors())
            {
                for (auto Attached : Build->AttachedBuildingActors)
                    RememberAttachment(Build, Attached);
            }
        }

        // ParentActorToAttachTo is the child-side structural relationship, while
        // ABuildingTrap::AttachedTo is the trap's authoritative supporting build.
        for (auto Build : Builds)
        {
            if (!Build)
                continue;

            if (Build->HasParentActorToAttachTo())
                RememberAttachment(Build->ParentActorToAttachTo, Build, true);
            if (auto Trap = Build->Cast<ABuildingTrap>())
            {
                if (Trap->HasAttachedTo())
                    RememberAttachment(Trap->AttachedTo, Trap, true);
            }
        }

        std::vector<ABuildingSMActor*> CandidateBuilds;
        std::unordered_map<FStructuralCellKey, ABuildingSMActor*,
            FStructuralCellKeyHash> CanonicalBuildByCell;
        for (auto Build : Builds)
        {
            if (!IsTrickshotSessionPlayerBuild(Build) ||
                Build->Cast<ABuildingTrap>() ||
                (TireClass && Build->Class == TireClass) ||
                TrackedPropActors.contains(Build))
                continue;

            // ParentActorToAttachTo is also used by some edited structural
            // pieces. Only exclude a child from the root list when it is an
            // actual deco/trap; otherwise the save silently drops that build.
            if (AttachedParents.contains(Build) &&
                ABuildingSMActor::GetTrapDefinition(Build))
                continue;

            CandidateBuilds.push_back(Build);
            FStructuralCellKey Key;
            if (TryGetStructuralCellKey(Build, Key))
                CanonicalBuildByCell[Key] = Build;
        }
        for (auto Build : CandidateBuilds)
        {
            FStructuralCellKey Key;
            if (TryGetStructuralCellKey(Build, Key))
            {
                auto Canonical = CanonicalBuildByCell.find(Key);
                if (Canonical != CanonicalBuildByCell.end() &&
                    Canonical->second != Build)
                {
                    SDK::DbgLog(
                        "[TrickshotSave] skipped stale duplicate actor=%p canonical=%p cell=(%lld,%lld,%lld) type=%u\n",
                        Build, Canonical->second, Key.X, Key.Y, Key.Z,
                        static_cast<unsigned>(Key.BuildingType));
                    continue;
                }
            }
            SavedBuilds.push_back(Build);
        }

        std::unordered_map<ABuildingSMActor*, int> SavedIndices;
        for (auto Build : SavedBuilds)
        {
            const FVector Location = Build->K2_GetActorLocation();
            const FRotator Rotation = Build->K2_GetActorRotation();
            const std::string ClassPath = FStringToStdString(UKismetSystemLibrary::GetPathName(Build->Class));
            const int SavedIndex = static_cast<int>(Root["builds"].size());
            SavedIndices.emplace(Build, SavedIndex);
            Root["builds"].push_back({
                { "kind", "build" },
                { "class", ClassPath },
                { "location", { Location.X, Location.Y, Location.Z } },
                { "rotation", { Rotation.Pitch, Rotation.Yaw, Rotation.Roll } },
                { "level", Build->CurrentBuildingLevel },
                { "mirrored", Build->HasbMirrored() && Build->bMirrored },
                { "supportAnchor", IsPortableSupportAnchor(Build) },
                { "parent", -1 }
            });
        }

        int SavedTraps = 0;
        bool bUnresolvedTrapDefinition = false;
        bool bUnsupportedTrapParent = false;
        std::unordered_set<ABuildingSMActor*> SerializedAttachments;
        auto SerializeTrap = [&](ABuildingSMActor* Trap, ABuildingSMActor* Parent)
        {
            if (!Trap || !Parent ||
                !IsTrickshotSessionPlayerBuild(Trap) ||
                (TireClass && Trap->Class == TireClass) ||
                TrackedPropActors.contains(Trap) ||
                Trap->bDestroyed ||
                (Trap->HasbActorIsBeingDestroyed() &&
                 Trap->bActorIsBeingDestroyed) ||
                SerializedAttachments.contains(Trap))
            {
                return;
            }

            auto ParentIndex = SavedIndices.find(Parent);
            if (ParentIndex == SavedIndices.end() &&
                IsTrickshotSessionPlayerBuild(Parent))
            {
                FStructuralCellKey ParentKey;
                if (TryGetStructuralCellKey(Parent, ParentKey))
                {
                    auto Canonical = CanonicalBuildByCell.find(ParentKey);
                    if (Canonical != CanonicalBuildByCell.end())
                        ParentIndex = SavedIndices.find(Canonical->second);
                }
            }
            if (ParentIndex == SavedIndices.end() &&
                TrackedPropActors.contains(Parent))
            {
                bUnsupportedTrapParent = true;
                SDK::DbgLog(
                    "[TrickshotSave] trap=%p has unsupported tracked-object parent=%p\n",
                    Trap, Parent);
                return;
            }
            const bool bExternalParent =
                ParentIndex == SavedIndices.end() &&
                (!Parent->bPlayerPlaced ||
                 (Parent->HasbNetStartup() && Parent->bNetStartup)) &&
                !Parent->bDestroyed &&
                !(Parent->HasbActorIsBeingDestroyed() &&
                  Parent->bActorIsBeingDestroyed);
            if (ParentIndex == SavedIndices.end() && !bExternalParent)
            {
                bUnsupportedTrapParent = true;
                SDK::DbgLog(
                    "[TrickshotSave] trap=%p has unsupported parent=%p\n",
                    Trap, Parent);
                return;
            }

            auto BuildingTrap = Trap->Cast<ABuildingTrap>();
            auto ItemDefinition =
                ABuildingSMActor::GetTrapDefinition(Trap);
            if (!BuildingTrap && !ItemDefinition)
                return;
            // Native trap placement needs the concrete definition, and its path
            // is also what lets a later process recover a non-resident class.
            if (!ItemDefinition)
            {
                bUnresolvedTrapDefinition = true;
                return;
            }

            const FVector Location = Trap->K2_GetActorLocation();
            const FRotator Rotation = Trap->K2_GetActorRotation();
            const std::string ClassPath = FStringToStdString(
                UKismetSystemLibrary::GetPathName(Trap->Class));
            const std::string ItemDefinitionPath = ItemDefinition
                ? FStringToStdString(
                    UKismetSystemLibrary::GetPathName(ItemDefinition))
                : std::string{};
            if (ItemDefinitionPath.empty())
            {
                bUnresolvedTrapDefinition = true;
                return;
            }
            const uint8 AttachmentType = Trap->HasBuildingAttachmentType()
                ? Trap->BuildingAttachmentType
                : GuessAttachmentType(ClassPath);
            const int AttachmentSlot = Trap->HasBuildingAttachmentSlot()
                ? static_cast<int>(Trap->BuildingAttachmentSlot) : -1;
            int TrapLevel = -1;
            int OriginalTrapLevel = -1;
            if (BuildingTrap)
            {
                if (BuildingTrap->HasTrapLevel())
                    TrapLevel = BuildingTrap->TrapLevel;
                else if (BuildingTrap->GetFunction("GetTrapLevel"))
                    TrapLevel = BuildingTrap->GetTrapLevel();
                if (BuildingTrap->HasOriginalTrapLevel())
                    OriginalTrapLevel = BuildingTrap->OriginalTrapLevel;
            }

            nlohmann::json SavedTrap = {
                { "kind", "trap" },
                { "class", ClassPath },
                { "location", { Location.X, Location.Y, Location.Z } },
                { "rotation", { Rotation.Pitch, Rotation.Yaw, Rotation.Roll } },
                { "level", Trap->CurrentBuildingLevel },
                { "parent", bExternalParent ? -1 : ParentIndex->second },
                { "attachmentType", AttachmentType },
                { "attachmentSlot", AttachmentSlot },
                { "itemDefinition", ItemDefinitionPath }
            };
            if (bExternalParent)
            {
                const FVector ParentLocation =
                    Parent->K2_GetActorLocation();
                const FRotator ParentRotation =
                    Parent->K2_GetActorRotation();
                SavedTrap["externalParent"] = {
                    { "actorPath", FStringToStdString(
                        UKismetSystemLibrary::GetPathName(Parent)) },
                    { "class", FStringToStdString(
                        UKismetSystemLibrary::GetPathName(Parent->Class)) },
                    { "location", {
                        ParentLocation.X, ParentLocation.Y,
                        ParentLocation.Z } },
                    { "rotation", {
                        ParentRotation.Pitch, ParentRotation.Yaw,
                        ParentRotation.Roll } },
                    { "buildingType", Parent->HasBuildingType()
                        ? static_cast<int>(Parent->BuildingType) : -1 }
                };
            }
            if (TrapLevel >= 0)
                SavedTrap["trapLevel"] = TrapLevel;
            if (OriginalTrapLevel >= 0)
                SavedTrap["originalTrapLevel"] = OriginalTrapLevel;
            Root["builds"].push_back(std::move(SavedTrap));
            SerializedAttachments.insert(Trap);
            ++SavedTraps;
        };

        for (auto Build : Builds)
        {
            auto Parent = AttachedParents.find(Build);
            if (Parent != AttachedParents.end())
                SerializeTrap(Build, Parent->second);
        }
        // A child can be present in the parent's array before it appears in the
        // global actor snapshot. Include that valid relationship as well.
        for (auto Parent : SavedBuilds)
        {
            if (!Parent->HasAttachedBuildingActors())
                continue;
            for (auto Attached : Parent->AttachedBuildingActors)
            {
                auto KnownParent = AttachedParents.find(Attached);
                if (KnownParent != AttachedParents.end() &&
                    KnownParent->second != Parent)
                {
                    continue;
                }
                SerializeTrap(Attached, Parent);
            }
        }
        Builds.Free();

        int SavedProps = 0;
        std::unordered_set<AActor*> SerializedProps;
        auto SerializeSpawnedObject = [&](
            AActor* Actor, const std::string& ClassPath)
        {
            if (!Actor || ClassPath.empty() ||
                !SerializedProps.insert(Actor).second ||
                !IsLiveTrackedSpawnedActor(Actor, CurrentWorld))
            {
                return;
            }

            const FTransform Transform = Actor->GetTransform();
            const FVector Location = Transform.Translation;
            const FRotator Rotation = Transform.Rotation.Rotator();
            const FVector Scale = Transform.Scale3D;
            if (!std::isfinite(Location.X) ||
                !std::isfinite(Location.Y) ||
                !std::isfinite(Location.Z) ||
                !std::isfinite(Rotation.Pitch) ||
                !std::isfinite(Rotation.Yaw) ||
                !std::isfinite(Rotation.Roll) ||
                !std::isfinite(Scale.X) ||
                !std::isfinite(Scale.Y) ||
                !std::isfinite(Scale.Z))
            {
                return;
            }

            Root["props"].push_back({
                { "kind", "spawnedActor" },
                { "class", ClassPath },
                { "location", {
                    Location.X, Location.Y, Location.Z } },
                { "rotation", {
                    Rotation.Pitch, Rotation.Yaw, Rotation.Roll } },
                { "scale", { Scale.X, Scale.Y, Scale.Z } }
            });
            ++SavedProps;
        };
        for (auto Tracked : TrackedProps)
        {
            auto Actor = Tracked ? Tracked->Actor.Get() : nullptr;
            SerializeSpawnedObject(Actor, Tracked->ClassPath);
        }

        if (SavedBuilds.empty() && SavedProps == 0)
        {
            Message =
                "There are no player-built structures or spawned objects to save.";
            return false;
        }
        if (SavedProps >
            static_cast<int>(kMaximumTrickshotSpawnedObjects))
        {
            Message =
                "There are too many spawned objects to save (maximum 2048).";
            return false;
        }

        if (bUnsupportedTrapParent)
        {
            Message = "Could not save because an attached trap has an unsupported player-built parent.";
            return false;
        }
        if (bUnresolvedTrapDefinition)
        {
            Message = "Could not save because an attached trap's item definition is unavailable.";
            return false;
        }

        size_t SavedWaypointCount = 0;
        if (FConfiguration::bSaveWaypoints.load(
                std::memory_order_acquire) &&
            !SerializeWaypoints(
                Root, SavedWaypointCount, Message))
        {
            return false;
        }

        const fs::path FinalPath = Directory / (SafeName + ".json");
        const fs::path TemporaryPath = Directory /
            (SafeName + ".json.tmp." +
             std::to_string(GetCurrentProcessId()) + "." +
             std::to_string(GetTickCount64()));
        const std::string SerializedPreset = Root.dump(4);
        std::ofstream File(
            TemporaryPath,
            std::ios::binary | std::ios::trunc);
        if (!File)
        {
            Message = "Could not create the trickshot file.";
            return false;
        }
        File.write(
            SerializedPreset.data(),
            static_cast<std::streamsize>(SerializedPreset.size()));
        File.flush();
        const bool WriteSucceeded = File.good();
        File.close();
        if (!WriteSucceeded || File.fail())
        {
            std::error_code RemoveError;
            fs::remove(TemporaryPath, RemoveError);
            Message =
                "Could not finish writing the trickshot file; the previous save was kept.";
            return false;
        }
        if (!MoveFileExW(
                TemporaryPath.c_str(),
                FinalPath.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            std::error_code RemoveError;
            fs::remove(TemporaryPath, RemoveError);
            Message =
                "Could not publish the trickshot file; the previous save was kept.";
            return false;
        }
        Message = "Saved " + std::to_string(SavedBuilds.size()) +
            " player builds, " + std::to_string(SavedTraps) +
            " traps, and " + std::to_string(SavedProps) +
            " spawned objects";
        if (Root.contains("waypoints"))
        {
            Message += ", plus " +
                std::to_string(SavedWaypointCount) + " waypoints";
        }
        Message += ".";
        return true;
    }

    inline bool Load(const std::string& Name, std::string& Message)
    {
        const auto Directory = GetDirectory();
        auto Controller = GetHostController();
        if (Name.empty() || Directory.empty() || !Controller)
        {
            Message = Name.empty() ? "Select a saved trickshot." : "A connected player is required to load builds.";
            return false;
        }

        const auto PresetPath = Directory / (Name + ".json");
        FScopedPresetReadGuard ReadGuard(PresetPath);
        if (!ReadGuard)
        {
            Message = ReadGuard.ShouldRetryScan()
                ? "That preset is still being copied. Try again in a moment."
                : "Could not open the selected trickshot file.";
            return false;
        }

        try
        {
            // Keep ReadGuard alive while parsing so an external writer cannot
            // replace or modify the file between the readiness check and read.
            std::ifstream File(PresetPath);
            nlohmann::json Root;
            if (!File || !(File >> Root) || !Root.contains("builds") || !Root["builds"].is_array())
            {
                Message = "The selected trickshot file is invalid.";
                return false;
            }

            if (Root.value("map", "") != GetCurrentMapPath())
            {
                Message = "This trickshot was saved on a different map.";
                return false;
            }
            if (Root.contains("props") && !Root["props"].is_array())
                throw std::runtime_error("invalid trickshot props");
            if (Root.contains("waypoints") &&
                !Root["waypoints"].is_array())
                throw std::runtime_error("invalid trickshot waypoints");
            const int SchemaVersion = Root.value("version", 1);
            if (SchemaVersion < 1 || SchemaVersion > 7)
                throw std::runtime_error("unsupported trickshot schema");
            if (SchemaVersion < 7 &&
                Root.contains("waypoints"))
            {
                throw std::runtime_error(
                    "waypoint state requires trickshot schema 7");
            }

            // Attachment enums have changed between Fortnite releases. New
            // presets record their source version so a raw attachment value is
            // never reinterpreted by a different build of the game.
            if (SchemaVersion >= 3 && !Root.contains("fortnite"))
                throw std::runtime_error("missing Fortnite version");
            if (Root.contains("fortnite"))
            {
                double SavedFortniteVersion = 0.0;
                if (Root["fortnite"].is_number())
                    SavedFortniteVersion = Root["fortnite"].get<double>();
                else if (Root["fortnite"].is_string())
                    SavedFortniteVersion = std::stod(
                        Root["fortnite"].get<std::string>());
                else
                    throw std::runtime_error("invalid Fortnite version");

                if (!std::isfinite(SavedFortniteVersion))
                    throw std::runtime_error("invalid Fortnite version");

                if (std::abs(
                        SavedFortniteVersion -
                        VersionInfo.FortniteVersion) > 0.001)
                {
                    Message = "This trickshot was saved on a different Fortnite version.";
                    return false;
                }
            }

            std::vector<FPendingTrickshotBuild> Pending;
            std::vector<FPendingTrickshotProp> PendingProps;
            bool RestoreWaypoints = false;
            AFortPlayerControllerAthena::FWaypointMap PendingWaypoints;
            if (SchemaVersion >= 7 &&
                FConfiguration::bSaveWaypoints.load(
                    std::memory_order_acquire) &&
                Root.contains("waypoints"))
            {
                PendingWaypoints =
                    ParseSavedWaypoints(Root["waypoints"]);
                RestoreWaypoints = true;
            }
            std::vector<TWeakObjectPtr<UClass>> TrackedPropClasses;
            FScopedTrickshotAssetRoots AssetRoots;
            UClass* TireClass = nullptr;
            const bool LegacyTireProps = SchemaVersion <= 5;
            if (LegacyTireProps)
            {
                TireClass = ResolveTrickshotTireClass(false);
                if (TireClass)
                {
                    AssetRoots.Root(TireClass);
                    TrackedPropClasses.emplace_back(TireClass);
                }
            }
            const auto SavedProps = Root.find("props");
            if (SavedProps != Root.end() && !SavedProps->empty())
            {
                constexpr size_t kMaximumClassPathLength = 2048;
                if (SavedProps->size() >
                    kMaximumTrickshotSpawnedObjects)
                    throw std::runtime_error(
                        "too many trickshot spawned objects");

                if (LegacyTireProps)
                {
                    if (!TireClass)
                        TireClass = ResolveTrickshotTireClass(true);
                }
                if (LegacyTireProps && !TireClass)
                {
                    Message = std::string(
                        "Required tire asset is unavailable: ") +
                        kTrickshotTireClassPath +
                        ". The current trickshot was left unchanged.";
                    return false;
                }

                PendingProps.reserve(SavedProps->size());
                for (const auto& SavedProp : *SavedProps)
                {
                    if (!SavedProp.is_object() ||
                        !SavedProp.contains("class") ||
                        !SavedProp["class"].is_string())
                    {
                        throw std::runtime_error("invalid trickshot prop");
                    }
                    const std::string ClassPath =
                        SavedProp["class"].get<std::string>();
                    if (ClassPath.empty() ||
                        ClassPath.size() > kMaximumClassPathLength ||
                        ClassPath.find('\0') != std::string::npos)
                        throw std::runtime_error(
                            "invalid trickshot prop class path");
                    if (LegacyTireProps &&
                        ClassPath != kTrickshotTireClassPath)
                        throw std::runtime_error(
                            "unsupported legacy trickshot prop class");
                    if (!LegacyTireProps &&
                        SavedProp.value("kind", "") != "spawnedActor")
                        throw std::runtime_error(
                            "invalid trickshot prop kind");

                    UClass* PropClass = TireClass;
                    if (!LegacyTireProps)
                    {
                        UEAllocatedWString WideClassPath(
                            ClassPath.begin(), ClassPath.end());
                        auto ClassObject = const_cast<UObject*>(
                            FindTrickshotAssetGuarded(
                                WideClassPath.c_str(),
                                UClass::StaticClass()));
                        if (!IsLiveTrickshotAsset(
                                ClassObject, UClass::StaticClass()))
                        {
                            ClassObject = const_cast<UObject*>(
                                LoadTrickshotAssetGuarded(
                                    WideClassPath.c_str(),
                                    UClass::StaticClass()));
                        }
                        PropClass = IsLiveTrickshotAsset(
                            ClassObject, UClass::StaticClass())
                            ? static_cast<UClass*>(ClassObject) : nullptr;
                        if (IsUnsafeTrickshotLifecycleClass(PropClass))
                        {
                            PropClass = nullptr;
                        }
                        if (PropClass &&
                            GetCanonicalTrackedClassPath(PropClass) !=
                                ClassPath)
                        {
                            PropClass = nullptr;
                        }
                    }
                    if (!PropClass)
                    {
                        Message = "Required spawned object class is unavailable: " +
                            ClassPath +
                            ". The current trickshot was left unchanged.";
                        return false;
                    }
                    AssetRoots.Root(PropClass);
                    const auto& L = SavedProp.at("location");
                    const auto& R = SavedProp.at("rotation");
                    if (!L.is_array() || L.size() != 3 ||
                        !R.is_array() || R.size() != 3)
                    {
                        throw std::runtime_error(
                            "invalid trickshot prop data");
                    }
                    FVector Scale(1.0, 1.0, 1.0);
                    if (SavedProp.contains("scale"))
                    {
                        const auto& S = SavedProp["scale"];
                        if (!S.is_array() || S.size() != 3)
                            throw std::runtime_error(
                                "invalid trickshot prop scale");
                        Scale = FVector(
                            S[0].get<double>(), S[1].get<double>(),
                            S[2].get<double>());
                    }
                    FVector Location(
                        L[0].get<double>(), L[1].get<double>(),
                        L[2].get<double>());
                    FRotator Rotation(
                        R[0].get<double>(), R[1].get<double>(),
                        R[2].get<double>());
                    constexpr double kMaximumPropCoordinate = 10000000.0;
                    constexpr double kMaximumPropRotation = 1000000.0;
                    constexpr double kMaximumPropScale = 1000.0;
                    if (!std::isfinite(Location.X) ||
                        !std::isfinite(Location.Y) ||
                        !std::isfinite(Location.Z) ||
                        !std::isfinite(Rotation.Pitch) ||
                        !std::isfinite(Rotation.Yaw) ||
                        !std::isfinite(Rotation.Roll) ||
                        !std::isfinite(Scale.X) ||
                        !std::isfinite(Scale.Y) ||
                        !std::isfinite(Scale.Z) ||
                        std::abs(Location.X) > kMaximumPropCoordinate ||
                        std::abs(Location.Y) > kMaximumPropCoordinate ||
                        std::abs(Location.Z) > kMaximumPropCoordinate ||
                        std::abs(Rotation.Pitch) > kMaximumPropRotation ||
                        std::abs(Rotation.Yaw) > kMaximumPropRotation ||
                        std::abs(Rotation.Roll) > kMaximumPropRotation ||
                        std::abs(Scale.X) > kMaximumPropScale ||
                        std::abs(Scale.Y) > kMaximumPropScale ||
                        std::abs(Scale.Z) > kMaximumPropScale)
                    {
                        throw std::runtime_error(
                            "invalid trickshot prop transform");
                    }
                    PendingProps.push_back({
                        TWeakObjectPtr<UClass>(PropClass),
                        Location, Rotation, Scale,
                        ClassPath });
                }
            }
            if (LegacyTireProps && TireClass &&
                TrackedPropClasses.empty())
            {
                AssetRoots.Root(TireClass);
                TrackedPropClasses.emplace_back(TireClass);
            }
            const auto& SavedBuildArray = Root["builds"];
            if (SavedBuildArray.empty() && PendingProps.empty())
            {
                Message = "The selected trickshot contains no builds or spawned objects; the current trickshot was left unchanged.";
                return false;
            }
            for (size_t RecordIndex = 0;
                RecordIndex < SavedBuildArray.size(); ++RecordIndex)
            {
                const auto& SavedBuild = SavedBuildArray[RecordIndex];
                if (!SavedBuild.is_object())
                    throw std::runtime_error("invalid trickshot record");
                const auto ClassPath = SavedBuild.at("class").get<std::string>();
                const int Parent = SavedBuild.value("parent", -1);
                if (Parent < -1)
                    throw std::runtime_error("invalid building parent");
                const std::string ItemDefinitionPath =
                    SavedBuild.value("itemDefinition", "");
                const bool HasExternalParent =
                    SchemaVersion >= 4 &&
                    SavedBuild.contains("externalParent");
                std::string Kind;
                if (SchemaVersion >= 3)
                {
                    if (!SavedBuild.contains("kind") ||
                        !SavedBuild["kind"].is_string())
                    {
                        throw std::runtime_error(
                            "missing trickshot record kind");
                    }
                    Kind = SavedBuild["kind"].get<std::string>();
                    const bool ValidInternalTrap =
                        Kind == "trap" && Parent >= 0 &&
                        !HasExternalParent;
                    const bool ValidExternalTrap =
                        Kind == "trap" && Parent == -1 &&
                        HasExternalParent;
                    if ((Kind == "build" &&
                         (Parent != -1 || HasExternalParent)) ||
                        (Kind == "trap" &&
                         (ItemDefinitionPath.empty() ||
                          (!ValidInternalTrap && !ValidExternalTrap))) ||
                        (Kind != "build" && Kind != "trap"))
                    {
                        throw std::runtime_error(
                            "invalid trickshot record kind");
                    }
                }
                const bool IsTrap = SchemaVersion >= 3
                    ? Kind == "trap" : Parent >= 0;
                UEAllocatedWString WideClassPath(ClassPath.begin(), ClassPath.end());
                auto ClassObject = const_cast<UObject*>(
                    FindTrickshotAssetGuarded(
                        WideClassPath.c_str(), UClass::StaticClass()));
                const bool bLoadedClassPackage =
                    !IsLiveTrickshotAsset(
                        ClassObject, UClass::StaticClass());
                if (bLoadedClassPackage)
                {
                    // An edited subclass may not be resident in a fresh match.
                    // Load only from this explicit user-triggered, game-thread
                    // operation and keep the guarded fallback out of tick-time
                    // placement and attachment repair.
                    ClassObject = const_cast<UObject*>(
                        LoadTrickshotAssetGuarded(
                            WideClassPath.c_str(), UClass::StaticClass()));
                }
                UClass* Class = IsLiveTrickshotAsset(
                    ClassObject, UClass::StaticClass())
                    ? static_cast<UClass*>(ClassObject) : nullptr;
                if (Class && bLoadedClassPackage)
                    AssetRoots.Root(Class);
                UFortDecoItemDefinition* ItemDefinition = nullptr;
                if (IsTrap && !ItemDefinitionPath.empty())
                {
                    UEAllocatedWString WideItemDefinitionPath(
                        ItemDefinitionPath.begin(),
                        ItemDefinitionPath.end());
                    auto ItemObject = const_cast<UObject*>(
                        FindTrickshotAssetGuarded(
                        WideItemDefinitionPath.c_str(),
                        UFortDecoItemDefinition::StaticClass()));
                    const bool bLoadedItemPackage =
                        !IsLiveTrickshotAsset(
                            ItemObject,
                            UFortDecoItemDefinition::StaticClass());
                    if (bLoadedItemPackage)
                    {
                        ItemObject = const_cast<UObject*>(
                            LoadTrickshotAssetGuarded(
                                WideItemDefinitionPath.c_str(),
                                UFortDecoItemDefinition::StaticClass()));
                    }
                    ItemDefinition = IsLiveTrickshotAsset(
                        ItemObject,
                        UFortDecoItemDefinition::StaticClass())
                        ? static_cast<UFortDecoItemDefinition*>(ItemObject)
                        : nullptr;
                    if (ItemDefinition)
                        AssetRoots.Root(ItemDefinition);
                }
                const auto& L = SavedBuild.at("location");
                const auto& R = SavedBuild.at("rotation");
                if (L.size() != 3 || R.size() != 3)
                    throw std::runtime_error("invalid building data");
                FVector ParsedLocation(
                    L[0].get<double>(), L[1].get<double>(),
                    L[2].get<double>());
                FRotator ParsedRotation(
                    R[0].get<double>(), R[1].get<double>(),
                    R[2].get<double>());
                constexpr double kMaximumSavedCoordinate = 10000000.0;
                constexpr double kMaximumSavedRotation = 1000000.0;
                if (!std::isfinite(ParsedLocation.X) ||
                    !std::isfinite(ParsedLocation.Y) ||
                    !std::isfinite(ParsedLocation.Z) ||
                    !std::isfinite(ParsedRotation.Pitch) ||
                    !std::isfinite(ParsedRotation.Yaw) ||
                    !std::isfinite(ParsedRotation.Roll) ||
                    std::abs(ParsedLocation.X) > kMaximumSavedCoordinate ||
                    std::abs(ParsedLocation.Y) > kMaximumSavedCoordinate ||
                    std::abs(ParsedLocation.Z) > kMaximumSavedCoordinate ||
                    std::abs(ParsedRotation.Pitch) > kMaximumSavedRotation ||
                    std::abs(ParsedRotation.Yaw) > kMaximumSavedRotation ||
                    std::abs(ParsedRotation.Roll) > kMaximumSavedRotation)
                {
                    throw std::runtime_error(
                        "invalid building transform");
                }
                const int RawAttachmentType = SavedBuild.value(
                    "attachmentType",
                    static_cast<int>(GuessAttachmentType(ClassPath)));
                const int AttachmentSlot =
                    SavedBuild.value("attachmentSlot", -1);
                const int TrapLevel =
                    SavedBuild.value("trapLevel", -1);
                const int OriginalTrapLevel =
                    SavedBuild.value("originalTrapLevel", -1);
                if (RawAttachmentType < 0 || RawAttachmentType > UINT8_MAX ||
                    AttachmentSlot < -1 || AttachmentSlot > UINT8_MAX ||
                    TrapLevel < -1 || OriginalTrapLevel < -1)
                {
                    throw std::runtime_error("invalid trap attachment data");
                }
                auto ConcreteItemDefinition = IsTrap && ItemDefinition
                    ? ABuildingSMActor::ResolveTrapDefinitionForAttachment(
                        ItemDefinition,
                        static_cast<uint8>(RawAttachmentType), Class)
                    : ItemDefinition;
                if (ConcreteItemDefinition)
                    AssetRoots.Root(ConcreteItemDefinition);
                if (!Class && ConcreteItemDefinition)
                {
                    Class = static_cast<UClass*>(
                        const_cast<UObject*>(
                            ConcreteItemDefinition->BlueprintClass.WeakPtr.Get()));
                }
                auto DefaultBuilding = Class
                    ? static_cast<ABuildingSMActor*>(Class->GetDefaultObj())
                    : nullptr;
                if (!DefaultBuilding ||
                    !DefaultBuilding->IsA(
                        ABuildingSMActor::StaticClass()))
                {
                    Class = nullptr;
                }
                else if (!IsTrap && DefaultBuilding->Cast<ABuildingTrap>())
                {
                    throw std::runtime_error(
                        "trap class cannot be restored as a structure");
                }
                if (Class)
                    AssetRoots.Root(Class);
                if (!Class)
                {
                    SDK::DbgLog(
                        "[TrickshotLoad] unavailable class record=%zu path=%s item=%s\n",
                        RecordIndex, ClassPath.c_str(),
                        ItemDefinitionPath.c_str());
                }
                if (IsTrap && !ItemDefinition)
                {
                    SDK::DbgLog(
                        "[TrickshotLoad] unavailable trap definition record=%zu class=%s item=%s\n",
                        RecordIndex, ClassPath.c_str(),
                        ItemDefinitionPath.c_str());
                }
                FPendingTrickshotBuild PendingBuild;
                PendingBuild.Class = TWeakObjectPtr<UClass>(Class);
                PendingBuild.Location = ParsedLocation;
                PendingBuild.Rotation = ParsedRotation;
                PendingBuild.Level = SavedBuild.value("level", 0);
                PendingBuild.Parent = Parent;
                PendingBuild.AttachmentType =
                    static_cast<uint8>(RawAttachmentType);
                PendingBuild.AttachmentSlot = AttachmentSlot;
                PendingBuild.Mirrored = SavedBuild.value("mirrored", false);
                PendingBuild.TrapLevel = TrapLevel;
                PendingBuild.OriginalTrapLevel = OriginalTrapLevel;
                PendingBuild.IsTrap = IsTrap;
                PendingBuild.SupportAnchor =
                    SavedBuild.value("supportAnchor", false);
                PendingBuild.HasSavedSupportAnchor =
                    SavedBuild.contains("supportAnchor");
                PendingBuild.ClassPath = ClassPath;
                PendingBuild.ItemDefinition = ItemDefinitionPath;
                PendingBuild.ResolvedItemDefinition =
                    TWeakObjectPtr<UFortDecoItemDefinition>(ItemDefinition);
                PendingBuild.ResolvedConcreteDefinition =
                    TWeakObjectPtr<UFortDecoItemDefinition>(
                        ConcreteItemDefinition);
                if (HasExternalParent)
                {
                    const auto& External = SavedBuild["externalParent"];
                    if (!External.is_object() ||
                        !External.contains("class") ||
                        !External.contains("location") ||
                        !External.contains("rotation"))
                    {
                        throw std::runtime_error(
                            "invalid external trap parent");
                    }
                    const auto& ExternalLocation = External["location"];
                    const auto& ExternalRotation = External["rotation"];
                    if (!ExternalLocation.is_array() ||
                        ExternalLocation.size() != 3 ||
                        !ExternalRotation.is_array() ||
                        ExternalRotation.size() != 3)
                    {
                        throw std::runtime_error(
                            "invalid external trap parent transform");
                    }
                    PendingBuild.HasExternalParent = true;
                    PendingBuild.ExternalParentClassPath =
                        External.at("class").get<std::string>();
                    PendingBuild.ExternalParentActorPath =
                        External.value("actorPath", "");
                    PendingBuild.ExternalParentLocation = FVector(
                        ExternalLocation[0].get<double>(),
                        ExternalLocation[1].get<double>(),
                        ExternalLocation[2].get<double>());
                    PendingBuild.ExternalParentRotation = FRotator(
                        ExternalRotation[0].get<double>(),
                        ExternalRotation[1].get<double>(),
                        ExternalRotation[2].get<double>());
                    PendingBuild.ExternalParentBuildingType =
                        External.value("buildingType", -1);
                    if (PendingBuild.ExternalParentClassPath.empty() ||
                        PendingBuild.ExternalParentBuildingType < -1 ||
                        PendingBuild.ExternalParentBuildingType > UINT8_MAX ||
                        !std::isfinite(PendingBuild.ExternalParentLocation.X) ||
                        !std::isfinite(PendingBuild.ExternalParentLocation.Y) ||
                        !std::isfinite(PendingBuild.ExternalParentLocation.Z) ||
                        !std::isfinite(PendingBuild.ExternalParentRotation.Pitch) ||
                        !std::isfinite(PendingBuild.ExternalParentRotation.Yaw) ||
                        !std::isfinite(PendingBuild.ExternalParentRotation.Roll))
                    {
                        throw std::runtime_error(
                            "invalid external trap parent data");
                    }
                }
                Pending.push_back(std::move(PendingBuild));
            }

            for (const auto& SavedBuild : Pending)
            {
                if (SavedBuild.IsTrap && !SavedBuild.HasExternalParent &&
                    SavedBuild.Parent >= static_cast<int>(Pending.size()))
                    throw std::runtime_error("invalid trap parent");
            }
            const int Canonicalized =
                CanonicalizePendingStructuralCells(Pending);
            if (Canonicalized > 0)
            {
                SDK::DbgLog(
                    "[TrickshotLoad] canonicalized %d stale structural records\n",
                    Canonicalized);
            }

            const bool HasStructuralRoot = std::any_of(
                Pending.begin(), Pending.end(),
                [](const FPendingTrickshotBuild& SavedBuild)
                {
                    return !SavedBuild.IsTrap;
                });
            if ((!Pending.empty() && !HasStructuralRoot) ||
                (Pending.empty() && PendingProps.empty()))
            {
                Message = "The selected trickshot contains no structural builds or spawned objects; the current trickshot was left unchanged.";
                return false;
            }

            if (std::any_of(
                    Pending.begin(), Pending.end(),
                    [](const FPendingTrickshotBuild& SavedBuild)
                    {
                        return SavedBuild.HasExternalParent;
                    }))
            {
                TArray<ABuildingSMActor*> WorldBuildings;
                Utils::GetAll<ABuildingSMActor>(WorldBuildings);
                std::vector<ABuildingSMActor*> ExternalCandidates;
                ExternalCandidates.reserve(WorldBuildings.Num());
                for (auto Build : WorldBuildings)
                    ExternalCandidates.push_back(Build);

                for (auto& SavedBuild : Pending)
                {
                    if (!SavedBuild.HasExternalParent)
                        continue;
                    auto ExternalParent = ResolveExternalTrickshotParent(
                        SavedBuild, ExternalCandidates);
                    if (!ExternalParent)
                    {
                        WorldBuildings.Free();
                        Message = "The natural building required by a saved trap is unavailable or ambiguous; the current trickshot was left unchanged.";
                        return false;
                    }
                    SavedBuild.ResolvedExternalParent =
                        TWeakObjectPtr<ABuildingSMActor>(ExternalParent);
                }
                WorldBuildings.Free();
            }

            // Validate the complete graph and all resident assets before the
            // destructive replacement step. A failed preflight leaves the
            // player's current trickshot untouched.
            for (int Index = 0; Index < static_cast<int>(Pending.size()); ++Index)
            {
                auto& SavedBuild = Pending[Index];
                auto SavedClass = SavedBuild.Class.Get();
                if (!SavedClass)
                {
                    Message = "Required build or trap class is unavailable: " +
                        SavedBuild.ClassPath +
                        ". The current trickshot was left unchanged.";
                    return false;
                }
                if (!SavedBuild.IsTrap)
                    continue;
                if (SavedBuild.HasExternalParent)
                {
                    if (!SavedBuild.ResolvedExternalParent.Get())
                        throw std::runtime_error(
                            "external trap parent became unavailable");
                }
                else if (
                    (SavedBuild.Parent >= static_cast<int>(Pending.size()) ||
                     SavedBuild.Parent == Index ||
                     Pending[SavedBuild.Parent].IsTrap))
                {
                    throw std::runtime_error("invalid trap parent");
                }
                auto ResolvedItemDefinition =
                    SavedBuild.ResolvedItemDefinition.Get();
                if (!ResolvedItemDefinition)
                {
                    ResolvedItemDefinition =
                        ABuildingSMActor::GetTrapDefinition(SavedClass);
                    SavedBuild.ResolvedItemDefinition =
                        TWeakObjectPtr<UFortDecoItemDefinition>(
                            ResolvedItemDefinition);
                }
                if (!ResolvedItemDefinition)
                {
                    Message = "Required trap definition is unavailable: " +
                        SavedBuild.ItemDefinition +
                        ". The current trickshot was left unchanged.";
                    return false;
                }

                auto ResolvedConcreteDefinition =
                    ABuildingSMActor::ResolveTrapDefinitionForAttachment(
                        ResolvedItemDefinition,
                        SavedBuild.AttachmentType,
                        SavedClass);
                SavedBuild.ResolvedConcreteDefinition =
                    TWeakObjectPtr<UFortDecoItemDefinition>(
                        ResolvedConcreteDefinition);
                if (!ResolvedConcreteDefinition)
                {
                    Message = "The saved trap definition does not support its attachment type; the current trickshot was left unchanged.";
                    return false;
                }
                auto DefinitionClass = static_cast<UClass*>(
                    const_cast<UObject*>(
                        ResolvedConcreteDefinition->
                            BlueprintClass.WeakPtr.Get()));
                if (DefinitionClass && DefinitionClass != SavedClass)
                    throw std::runtime_error("concrete trap definition does not match its saved class");
            }

            // The staged transaction deliberately destroys the existing setup
            // before later layers spawn. Root every dependency now so an asset
            // referenced only by the old actors cannot be collected mid-load.
            for (auto& SavedBuild : Pending)
            {
                if (auto Class = SavedBuild.Class.Get())
                    AssetRoots.Root(Class);
                if (auto Definition =
                    SavedBuild.ResolvedItemDefinition.Get())
                {
                    AssetRoots.Root(Definition);
                }
                if (auto Concrete =
                    SavedBuild.ResolvedConcreteDefinition.Get())
                {
                    AssetRoots.Root(Concrete);
                }
            }

            EnsurePortableSupportAnchors(Pending);

            std::vector<int> StructuralOrder;
            StructuralOrder.reserve(Pending.size());
            for (int Index = 0; Index < static_cast<int>(Pending.size()); ++Index)
            {
                if (!Pending[Index].IsTrap)
                    StructuralOrder.push_back(Index);
            }
            std::stable_sort(
                StructuralOrder.begin(), StructuralOrder.end(),
                [&](int Left, int Right)
                {
                    return Pending[Left].Location.Z <
                        Pending[Right].Location.Z;
                });

            FTrickshotLoadJob Job;
            Job.Active = true;
            Job.Name = Name;
            Job.World = TWeakObjectPtr<UWorld>(UWorld::GetWorld());
            Job.Controller =
                TWeakObjectPtr<AFortPlayerControllerAthena>(Controller);
            Job.Pending = std::move(Pending);
            Job.PendingProps = std::move(PendingProps);
            Job.RestoreWaypoints = RestoreWaypoints;
            Job.PendingWaypoints = std::move(PendingWaypoints);
            Job.StructuralOrder = std::move(StructuralOrder);
            Job.SpawnedBuilds.resize(Job.Pending.size());
            Job.SpawnedProps.resize(Job.PendingProps.size());
            Job.TrackedPropClasses = std::move(TrackedPropClasses);
            Job.LegacyTireProps = LegacyTireProps;
            auto CurrentWorld = UWorld::GetWorld();
            {
                TArray<ABuildingSMActor*> BaselineBuildings;
                Utils::GetAll<ABuildingSMActor>(BaselineBuildings);
                Job.BaselineBuildingActors.reserve(
                    BaselineBuildings.Num());
                for (auto Building : BaselineBuildings)
                {
                    if (Building)
                    {
                        Job.BaselineBuildingActors.emplace_back(
                            Building);
                    }
                }
                BaselineBuildings.Free();
            }
            PruneTrackedSpawnedActors(CurrentWorld);
            for (const auto& Entry : GTrackedSpawnedActors)
            {
                auto Actor = Entry.Actor.Get();
                if (Entry.World.Get() == CurrentWorld &&
                    IsLiveTrackedSpawnedActor(Actor, CurrentWorld))
                {
                    Job.ExistingTrackedProps.emplace_back(Actor);
                }
            }
            Job.TemporaryRootedAssets = AssetRoots.Commit();
            GLoadJob = std::move(Job);
            Message = "Loading trickshot...";
            return true;
        }
        catch (const std::exception& Error)
        {
            Message = std::string("Failed to load trickshot: ") + Error.what();
            return false;
        }
    }

    inline void ApplyLoadedBuildOwnership(
        ABuildingSMActor* Build,
        AFortPlayerControllerAthena* Controller)
    {
        if (!Build || !Controller)
            return;

        Build->bPlayerPlaced = true;
        if (Controller->PlayerState)
        {
            auto PlayerState = static_cast<AFortPlayerStateAthena*>(
                Controller->PlayerState);
            if (Build->HasTeam() && PlayerState->HasTeamIndex())
                Build->Team = PlayerState->TeamIndex;
            if (Build->HasTeamIndex() && PlayerState->HasTeamIndex())
                Build->TeamIndex = PlayerState->TeamIndex;
            if (Build->HasOwnerPersistentID() &&
                PlayerState->HasWorldPlayerId())
            {
                Build->OwnerPersistentID = PlayerState->WorldPlayerId;
            }
            if (PlayerState->HasTeamIndex())
                Build->SetTeam(PlayerState->TeamIndex);
        }
        Build->ForceNetUpdate();
    }

    inline bool ValidateLoadContext(
        AFortPlayerControllerAthena*& Controller,
        std::string& Message)
    {
        auto World = GLoadJob.World.Get();
        Controller = GLoadJob.Controller.Get();
        if (!World || World != UWorld::GetWorld())
        {
            Message =
                "The world changed while the trickshot was loading.";
            return false;
        }
        if (!Controller || Controller != GetHostController())
        {
            Message =
                "The host disconnected while the trickshot was loading.";
            return false;
        }
        return true;
    }

    std::string BuildLoadCompletionMessage()
    {
        std::string Message = "Loaded " +
            std::to_string(GLoadJob.LoadedBuilds) +
            " player builds, " +
            std::to_string(GLoadJob.LoadedTraps) + " traps, and " +
            std::to_string(GLoadJob.LoadedProps) + " spawned objects.";
        if (GLoadJob.RestoreWaypoints)
        {
            Message += " Restored " +
                std::to_string(
                    AFortPlayerControllerAthena::
                        SnapshotWaypoints().size()) +
                " waypoints.";
        }
        if (GLoadJob.FailedTrapPlacements > 0)
        {
            Message += " Failed " +
                std::to_string(GLoadJob.FailedTrapPlacements) +
                " trap restorations.";
        }
        if (GLoadJob.FailedPropPlacements > 0)
        {
            Message += " Failed " +
                std::to_string(GLoadJob.FailedPropPlacements) +
                " spawned-object restorations.";
        }
        if (GLoadJob.Skipped >
            GLoadJob.FailedTrapPlacements +
                GLoadJob.FailedPropPlacements)
        {
            Message += " Skipped " +
                std::to_string(
                    GLoadJob.Skipped -
                    GLoadJob.FailedTrapPlacements -
                    GLoadJob.FailedPropPlacements) +
                " other actor placements.";
        }
        return Message;
    }

    inline ELoadPumpResult PumpLoad(std::string& Message)
    {
        constexpr ULONGLONG kLoadPhaseDelayMs = 100ULL;
        constexpr double kStructuralLayerTolerance = 1.0;

        if (!GLoadJob.Active)
        {
            Message = "No trickshot load is in progress.";
            return ELoadPumpResult::Failed;
        }

        AFortPlayerControllerAthena* Controller = nullptr;
        if (!ValidateLoadContext(Controller, Message))
            return ELoadPumpResult::Failed;

        const ULONGLONG Now = GetTickCount64();
        if (Now < GLoadJob.NextAdvanceMs)
            return ELoadPumpResult::Running;

        if (GLoadJob.Phase == ELoadPhase::Cleanup)
        {
            auto IsExistingTrackedProp = [&](AActor* Candidate)
            {
                if (!Candidate)
                    return false;
                for (const auto& Existing :
                    GLoadJob.ExistingTrackedProps)
                {
                    if (Existing.Get() == Candidate)
                        return true;
                }
                return GLoadJob.LegacyTireProps &&
                    IsTrackedTrickshotPropClass(
                        Candidate->Class,
                        GLoadJob.TrackedPropClasses);
            };

            // Modern presets own exact registry instances. Never destroy every
            // actor of an arbitrary saved class: the map or another player may
            // legitimately own actors of that same class.
            for (const auto& Existing : GLoadJob.ExistingTrackedProps)
            {
                auto Prop = Existing.Get();
                if (IsLiveTrackedSpawnedActor(
                        Prop, GLoadJob.World.Get()))
                {
                    ForgetTrackedSpawnedActor(Prop);
                    Prop->K2_DestroyActor();
                }
            }

            // Schemas 1-5 predate the instance registry and only allowed the
            // Tower tire. Retain their narrow session-scoped migration cleanup.
            if (GLoadJob.LegacyTireProps)
            {
                for (const auto& TrackedClass :
                    GLoadJob.TrackedPropClasses)
                {
                    auto PropClass = TrackedClass.Get();
                    if (!PropClass)
                        continue;
                    TArray<AActor*> ExistingProps;
                    Utils::GetAll<AActor>(PropClass, ExistingProps);
                    for (auto Prop : ExistingProps)
                    {
                        if (IsSessionTrickshotTire(Prop, PropClass))
                        {
                            ForgetTrackedSpawnedActor(Prop);
                            Prop->K2_DestroyActor();
                        }
                    }
                    ExistingProps.Free();
                }
            }

            TArray<ABuildingSMActor*> ExistingBuilds;
            Utils::GetAll<ABuildingSMActor>(ExistingBuilds);
            std::unordered_set<ABuildingSMActor*>
                ExistingAttachments;
            for (auto Build : ExistingBuilds)
            {
                if (!Build || IsExistingTrackedProp(Build))
                    continue;
                if (Build->HasAttachedBuildingActors())
                {
                    for (auto Attached : Build->AttachedBuildingActors)
                    {
                        if (Attached)
                            ExistingAttachments.insert(Attached);
                    }
                }
                if (Build->HasParentActorToAttachTo() &&
                    Build->ParentActorToAttachTo)
                {
                    ExistingAttachments.insert(Build);
                }
                if (auto Trap = Build->Cast<ABuildingTrap>())
                {
                    if (Trap->HasAttachedTo() && Trap->AttachedTo)
                        ExistingAttachments.insert(Trap);
                }
            }

            // Destroy attachment children before their supports. The delay
            // after this phase gives Fortnite's structural grid a full tick to
            // remove the old setup before the first replacement layer arrives.
            for (auto Build : ExistingBuilds)
            {
                if (ExistingAttachments.contains(Build) &&
                    !IsExistingTrackedProp(Build) &&
                    IsTrickshotSessionPlayerBuild(Build))
                {
                    Build->SilentDie(true);
                }
            }
            for (auto Build : ExistingBuilds)
            {
                if (!ExistingAttachments.contains(Build) &&
                    !IsExistingTrackedProp(Build) &&
                    IsTrickshotSessionPlayerBuild(Build))
                {
                    Build->SilentDie(true);
                }
            }
            ExistingBuilds.Free();

            GLoadJob.Phase = ELoadPhase::Structures;
            GLoadJob.NextAdvanceMs =
                GetTickCount64() + kLoadPhaseDelayMs;
            SDK::DbgLog(
                "[TrickshotLoad] cleanup complete; waiting for structural grid\n");
            return ELoadPumpResult::Running;
        }

        if (GLoadJob.Phase == ELoadPhase::Structures)
        {
            if (GLoadJob.NextStructural >=
                GLoadJob.StructuralOrder.size())
            {
                GLoadJob.Phase = ELoadPhase::ReleaseStructuralSupport;
                GLoadJob.NextAdvanceMs =
                    GetTickCount64() + kLoadPhaseDelayMs;
                return ELoadPumpResult::Running;
            }

            const int FirstIndex = GLoadJob.StructuralOrder[
                GLoadJob.NextStructural];
            const double LayerZ =
                GLoadJob.Pending[FirstIndex].Location.Z;
            int LayerBuilds = 0;

            // Spawn one complete height band per timed pump. Scheduling the
            // next deadline after this work also prevents GetMaxTickRate and
            // TickFlush from advancing two bands in the same engine frame.
            while (GLoadJob.NextStructural <
                GLoadJob.StructuralOrder.size())
            {
                const int Index = GLoadJob.StructuralOrder[
                    GLoadJob.NextStructural];
                const auto& SavedBuild = GLoadJob.Pending[Index];
                if (std::abs(SavedBuild.Location.Z - LayerZ) >
                    kStructuralLayerTolerance)
                {
                    break;
                }
                ++GLoadJob.NextStructural;

                auto SavedClass = SavedBuild.Class.Get();
                if (!SavedClass)
                {
                    ++GLoadJob.Skipped;
                    GLoadJob.FailureMessage =
                        "A structural build asset became unavailable; later layers were not loaded.";
                    GLoadJob.Phase = ELoadPhase::StructureSettle;
                    GLoadJob.NextStructural =
                        GLoadJob.StructuralOrder.size();
                    break;
                }

                auto Build =
                    UWorld::SpawnActorUnfinished<ABuildingSMActor>(
                        SavedClass, SavedBuild.Location,
                        SavedBuild.Rotation, Controller);
                if (Build)
                {
                    // Hold incomplete height bands in the grid until every
                    // saved neighbor exists. This is instance state only; it
                    // does not mutate the shared building class asset.
                    if (Build->HasbForciblyStructurallySupported())
                        Build->bForciblyStructurallySupported = true;
                    Build->InitializeKismetSpawnedBuildingActor(
                        Build, Controller, true, nullptr, false);
                    UWorld::FinishSpawnActor(
                        Build, SavedBuild.Location,
                        SavedBuild.Rotation);
                }
                if (!Build)
                {
                    ++GLoadJob.Skipped;
                    GLoadJob.FailureMessage =
                        "A structural build failed to spawn; later layers were not loaded.";
                    GLoadJob.Phase = ELoadPhase::StructureSettle;
                    GLoadJob.NextStructural =
                        GLoadJob.StructuralOrder.size();
                    break;
                }

                int BuildingLevel = SavedBuild.Level;
                Build->CurrentBuildingLevel = BuildingLevel;
                Build->OnRep_CurrentBuildingLevel();
                Build->SetMirrored(SavedBuild.Mirrored);
                ApplyLoadedBuildOwnership(Build, Controller);
                GLoadJob.SpawnedBuilds[Index] =
                    TWeakObjectPtr<ABuildingSMActor>(Build);
                ++GLoadJob.LoadedBuilds;
                ++LayerBuilds;
            }

            if (GLoadJob.NextStructural >=
                GLoadJob.StructuralOrder.size())
            {
                GLoadJob.Phase = ELoadPhase::ReleaseStructuralSupport;
            }
            GLoadJob.NextAdvanceMs =
                GetTickCount64() + kLoadPhaseDelayMs;
            SDK::DbgLog(
                "[TrickshotLoad] spawned structural layer z=%.2f actors=%d\n",
                LayerZ, LayerBuilds);
            return ELoadPumpResult::Running;
        }

        if (GLoadJob.Phase == ELoadPhase::ReleaseStructuralSupport)
        {
            // Restore normal structural behavior after the complete graph is
            // present. Pieces that were directly world-supported when saved
            // remain portable anchors; every other piece is checked through
            // Fortnite's own finished structural graph.
            for (int Index : GLoadJob.StructuralOrder)
            {
                auto Build = GLoadJob.SpawnedBuilds[Index].Get();
                if (!Build || Build->bDestroyed ||
                    (Build->HasbActorIsBeingDestroyed() &&
                     Build->bActorIsBeingDestroyed))
                {
                    const auto& SavedBuild = GLoadJob.Pending[Index];
                    SDK::DbgLog(
                        "[TrickshotLoad] structure vanished before support release index=%d class=%s loc=(%.2f,%.2f,%.2f)\n",
                        Index, SavedBuild.ClassPath.c_str(),
                        SavedBuild.Location.X, SavedBuild.Location.Y,
                        SavedBuild.Location.Z);
                    Message =
                        "A restored structural layer did not remain connected.";
                    return ELoadPumpResult::Failed;
                }
                const auto& SavedBuild = GLoadJob.Pending[Index];
                if (Build->HasbForciblyStructurallySupported())
                {
                    Build->bForciblyStructurallySupported =
                        SavedBuild.SupportAnchor;
                }
            }

            // Clear every temporary hold before asking native integrity code
            // to traverse the graph. An early traversal must not see a later
            // non-anchor piece as still forcibly supported.
            for (int Index : GLoadJob.StructuralOrder)
            {
                auto Build = GLoadJob.SpawnedBuilds[Index].Get();
                if (!Build || Build->bDestroyed ||
                    (Build->HasbActorIsBeingDestroyed() &&
                     Build->bActorIsBeingDestroyed))
                {
                    continue;
                }
                if (Build->GetFunction(
                        "MarkConnectedBuildingsForStructuralIntegrityCheck"))
                {
                    Build->MarkConnectedBuildingsForStructuralIntegrityCheck();
                }
                Build->ForceNetUpdate();
            }
            GLoadJob.Phase = ELoadPhase::StructureSettle;
            GLoadJob.NextAdvanceMs =
                GetTickCount64() + kLoadPhaseDelayMs;
            SDK::DbgLog(
                "[TrickshotLoad] released temporary support; waiting for integrity check\n");
            return ELoadPumpResult::Running;
        }

        if (GLoadJob.Phase == ELoadPhase::StructureSettle)
        {
            // The final support layer has now had a complete settle interval.
            // Only after that may native trap placement see the final graph.
            for (int Index : GLoadJob.StructuralOrder)
            {
                auto Build = GLoadJob.SpawnedBuilds[Index].Get();
                if (!Build || Build->bDestroyed ||
                    (Build->HasbActorIsBeingDestroyed() &&
                     Build->bActorIsBeingDestroyed))
                {
                    const auto& SavedBuild = GLoadJob.Pending[Index];
                    SDK::DbgLog(
                        "[TrickshotLoad] structure failed settle index=%d class=%s loc=(%.2f,%.2f,%.2f) anchor=%d actor=%p destroyed=%d destroying=%d\n",
                        Index, SavedBuild.ClassPath.c_str(),
                        SavedBuild.Location.X, SavedBuild.Location.Y,
                        SavedBuild.Location.Z,
                        SavedBuild.SupportAnchor ? 1 : 0, Build,
                        Build && Build->bDestroyed ? 1 : 0,
                        Build && Build->HasbActorIsBeingDestroyed() &&
                            Build->bActorIsBeingDestroyed ? 1 : 0);
                    if (GLoadJob.FailureMessage.empty())
                    {
                        GLoadJob.FailureMessage =
                            "A restored structural layer did not remain connected; traps were not loaded.";
                    }
                    Message = GLoadJob.FailureMessage;
                    return ELoadPumpResult::Failed;
                }
            }

            if (!GLoadJob.FailureMessage.empty())
            {
                Message = GLoadJob.FailureMessage;
                return ELoadPumpResult::Failed;
            }
            GLoadJob.Phase = ELoadPhase::TrapPlacement;
            GLoadJob.TrapPlacementPass = 0;
        }

        if (GLoadJob.Phase != ELoadPhase::TrapPlacement)
        {
            Message = "The trickshot loader entered an invalid trap phase.";
            return ELoadPumpResult::Failed;
        }

        // Legacy ServerSpawnDeco can finish on a later game tick. Retry only
        // unresolved traps on wall-clock deadlines; this never blocks the game
        // thread and lets deferred children be adopted before another native
        // spawn is attempted. Four native passes are followed by one final
        // observation/cleanup sweep; the bounded worst-case wait is 2 seconds.
        constexpr uint8 kNativeTrapPlacementPasses = 4;
        constexpr uint8 kMaximumTrapPlacementPasses =
            kNativeTrapPlacementPasses + 1;
        constexpr ULONGLONG kTrapRetryDelaysMs[
            kMaximumTrapPlacementPasses - 1] = {
                250ULL, 500ULL, 1000ULL, 250ULL };
        int UnresolvedTraps = 0;
        for (int Index = 0;
            Index < static_cast<int>(GLoadJob.Pending.size());
            ++Index)
        {
            const auto& SavedBuild = GLoadJob.Pending[Index];
            if (!SavedBuild.IsTrap)
                continue;

            auto ExistingTrap = GLoadJob.SpawnedBuilds[Index].Get();
            if (ExistingTrap && !ExistingTrap->bDestroyed &&
                !(ExistingTrap->HasbActorIsBeingDestroyed() &&
                  ExistingTrap->bActorIsBeingDestroyed))
            {
                continue;
            }
            GLoadJob.SpawnedBuilds[Index] = {};

            auto SavedClass = SavedBuild.Class.Get();
            ABuildingSMActor* Parent = nullptr;
            if (SavedBuild.HasExternalParent)
                Parent = SavedBuild.ResolvedExternalParent.Get();
            else if (SavedBuild.Parent >= 0 && SavedBuild.Parent <
                static_cast<int>(GLoadJob.SpawnedBuilds.size()))
            {
                Parent =
                    GLoadJob.SpawnedBuilds[SavedBuild.Parent].Get();
            }
            // A support disappearing is structural corruption, not a
            // transient placement failure. Keep this fatal on every pass.
            if (!SavedClass || !Parent || Parent->bDestroyed ||
                (Parent->HasbActorIsBeingDestroyed() &&
                 Parent->bActorIsBeingDestroyed))
            {
                SDK::DbgLog(
                    "[TrickshotLoad] trap parent unavailable index=%d pass=%u external=%d parent=%p\n",
                    Index,
                    static_cast<unsigned>(
                        GLoadJob.TrapPlacementPass + 1),
                    SavedBuild.HasExternalParent ? 1 : 0,
                    Parent);
                Message = SavedBuild.HasExternalParent
                    ? "A required natural trap support became unavailable during loading."
                    : "A restored trap support became unavailable during loading.";
                return ELoadPumpResult::Failed;
            }

            UEAllocatedWString ItemDefinitionPath(
                SavedBuild.ItemDefinition.begin(),
                SavedBuild.ItemDefinition.end());
            auto Trap = ABuildingSMActor::SpawnSavedTrap(
                SavedClass, SavedBuild.Location,
                SavedBuild.Rotation, Parent,
                SavedBuild.AttachmentType, Controller,
                SavedBuild.ItemDefinition.empty()
                    ? nullptr : ItemDefinitionPath.c_str(),
                SavedBuild.AttachmentSlot,
                SavedBuild.TrapLevel,
                SavedBuild.OriginalTrapLevel,
                SavedBuild.ResolvedItemDefinition.Get(),
                GLoadJob.TrapPlacementPass > 0,
                GLoadJob.TrapPlacementPass >=
                    kNativeTrapPlacementPasses,
                &GLoadJob.SpawnedBuilds,
                &GLoadJob.BaselineBuildingActors);
            if (!Trap)
            {
                SDK::DbgLog(
                    "[TrickshotLoad] trap unresolved index=%d pass=%u class=%s parent=%p attachment=%u slot=%d\n",
                    Index,
                    static_cast<unsigned>(
                        GLoadJob.TrapPlacementPass + 1),
                    SavedBuild.ClassPath.c_str(), Parent,
                    static_cast<unsigned>(
                        SavedBuild.AttachmentType),
                    SavedBuild.AttachmentSlot);
                ++UnresolvedTraps;
                continue;
            }

            int TrapBuildingLevel = SavedBuild.Level;
            Trap->CurrentBuildingLevel = TrapBuildingLevel;
            Trap->OnRep_CurrentBuildingLevel();
            ApplyLoadedBuildOwnership(Trap, Controller);
            GLoadJob.SpawnedBuilds[Index] =
                TWeakObjectPtr<ABuildingSMActor>(Trap);
        }

        if (UnresolvedTraps > 0 &&
            GLoadJob.TrapPlacementPass + 1 <
                kMaximumTrapPlacementPasses)
        {
            const ULONGLONG RetryDelay =
                kTrapRetryDelaysMs[GLoadJob.TrapPlacementPass];
            SDK::DbgLog(
                "[TrickshotLoad] trap pass=%u/%u unresolved=%d retryInMs=%llu\n",
                static_cast<unsigned>(
                    GLoadJob.TrapPlacementPass + 1),
                static_cast<unsigned>(kMaximumTrapPlacementPasses),
                UnresolvedTraps,
                static_cast<unsigned long long>(RetryDelay));
            ++GLoadJob.TrapPlacementPass;
            GLoadJob.NextAdvanceMs = Now + RetryDelay;
            return ELoadPumpResult::Running;
        }
        if (UnresolvedTraps > 0)
        {
            GLoadJob.Skipped += UnresolvedTraps;
            GLoadJob.FailedTrapPlacements += UnresolvedTraps;
        }
        GLoadJob.LoadedTraps = 0;
        for (int Index = 0;
            Index < static_cast<int>(GLoadJob.Pending.size());
            ++Index)
        {
            if (!GLoadJob.Pending[Index].IsTrap)
                continue;
            auto Trap = GLoadJob.SpawnedBuilds[Index].Get();
            if (Trap && !Trap->bDestroyed &&
                !(Trap->HasbActorIsBeingDestroyed() &&
                  Trap->bActorIsBeingDestroyed))
            {
                ++GLoadJob.LoadedTraps;
            }
        }
        SDK::DbgLog(
            "[TrickshotLoad] trap phase complete passes=%u loaded=%d unresolved=%d\n",
            static_cast<unsigned>(GLoadJob.TrapPlacementPass + 1),
            GLoadJob.LoadedTraps, UnresolvedTraps);

        // Trap retries deliberately span real time. Revalidate the entire
        // structural graph after that window—not just unresolved traps'
        // parents—so a delayed second-layer collapse cannot be hidden by a
        // successful prop spawn and scene commit.
        for (int Index : GLoadJob.StructuralOrder)
        {
            auto Build = GLoadJob.SpawnedBuilds[Index].Get();
            if (!Build || Build->bDestroyed ||
                (Build->HasbActorIsBeingDestroyed() &&
                 Build->bActorIsBeingDestroyed))
            {
                const auto& SavedBuild = GLoadJob.Pending[Index];
                SDK::DbgLog(
                    "[TrickshotLoad] structure vanished during trap retries index=%d class=%s loc=(%.2f,%.2f,%.2f)\n",
                    Index, SavedBuild.ClassPath.c_str(),
                    SavedBuild.Location.X, SavedBuild.Location.Y,
                    SavedBuild.Location.Z);
                Message =
                    "A restored structural layer collapsed while traps were settling; the partial load was removed.";
                return ELoadPumpResult::Failed;
            }
        }

        // Command-spawned objects go last so their construction and collision
        // see the final structure/trap scene. AlwaysSpawn preserves the exact
        // saved transform, including custom summon scale.
        for (size_t Index = 0;
            Index < GLoadJob.PendingProps.size(); ++Index)
        {
            const auto& SavedProp = GLoadJob.PendingProps[Index];
            auto SavedClass = SavedProp.Class.Get();
            if (!SavedClass)
            {
                ++GLoadJob.Skipped;
                ++GLoadJob.FailedPropPlacements;
                continue;
            }

            FTransform SpawnTransform(
                SavedProp.Location,
                SavedProp.Rotation.Quaternion(),
                SavedProp.Scale);
            AActor* SpawnOwner =
                SavedProp.ClassPath == kTrickshotTireClassPath
                ? static_cast<AActor*>(Controller) : nullptr;
            auto Prop = UWorld::SpawnActor(
                SavedClass, SpawnTransform, SpawnOwner, 1);
            if (!Prop)
            {
                ++GLoadJob.Skipped;
                ++GLoadJob.FailedPropPlacements;
                continue;
            }
            if (Prop->Class != SavedClass ||
                !IsLiveTrackedSpawnedActor(
                    Prop, GLoadJob.World.Get()))
            {
                if (IsLiveTrackedSpawnedActor(
                        Prop, GLoadJob.World.Get()))
                    Prop->K2_DestroyActor();
                ++GLoadJob.Skipped;
                ++GLoadJob.FailedPropPlacements;
                continue;
            }
            Prop->SetActorScale3D(SavedProp.Scale);
            if (auto Car = Prop->Cast<AFortDagwoodVehicle>())
            {
                FortVehicleMods::RegisterSpawnedVehicle(Car);
                Car->SetFuel(100.f);
            }
            Prop->ForceNetUpdate();
            if (!IsLiveTrackedSpawnedActor(
                    Prop, GLoadJob.World.Get()) ||
                !RegisterSpawnedActorInternal(
                    Prop, Controller, SavedProp.ClassPath, false))
            {
                if (IsLiveTrackedSpawnedActor(
                        Prop, GLoadJob.World.Get()))
                    Prop->K2_DestroyActor();
                ++GLoadJob.Skipped;
                ++GLoadJob.FailedPropPlacements;
                continue;
            }
            GLoadJob.SpawnedProps[Index] =
                TWeakObjectPtr<AActor>(Prop);
            ++GLoadJob.LoadedProps;
        }
        if (GLoadJob.FailedPropPlacements > 0)
        {
            Message = "One or more saved objects failed to restore; the partial load was removed.";
            return ELoadPumpResult::Failed;
        }

        // Waypoints are process-session state rather than actors. Publish the
        // fully validated replacement only after every destructive scene phase
        // has succeeded, so a failed preset never partially rewrites them.
        if (GLoadJob.RestoreWaypoints)
        {
            AFortPlayerControllerAthena::ReplaceWaypoints(
                std::move(GLoadJob.PendingWaypoints));
        }
        Message = BuildLoadCompletionMessage();
        SDK::DbgLog(
            "[TrickshotLoad] complete builds=%d traps=%d objects=%d skipped=%d failedTraps=%d failedObjects=%d\n",
            GLoadJob.LoadedBuilds, GLoadJob.LoadedTraps,
            GLoadJob.LoadedProps, GLoadJob.Skipped,
            GLoadJob.FailedTrapPlacements,
            GLoadJob.FailedPropPlacements);
        return ELoadPumpResult::Succeeded;
    }
}

void GUI::Init()
{
    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASS wc{};
    wc.lpszClassName = L"ErbiumWC";
    wc.lpfnWndProc = WndProc;
    RegisterClass(&wc);

    wchar_t buffer[67];
    swprintf_s(buffer, VersionInfo.EngineVersion >= 5.0 ? L"Magnesium (FN %.2f, UE %.1f)" : (VersionInfo.FortniteVersion >= 5.00 || VersionInfo.FortniteVersion < 1.2 ? L"Magnesium (FN %.2f, UE %.2f)" : L"Magnesium (FN %.1f, UE %.2f)"), VersionInfo.FortniteVersion, VersionInfo.EngineVersion);
    auto hWnd = CreateWindow(wc.lpszClassName, buffer, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME, 100, 100, (int)(WindowWidth * main_scale), (int)(WindowHeight * main_scale), nullptr, nullptr, nullptr, nullptr);

    IDXGISwapChain* g_pSwapChain = nullptr;
    ID3D11Device* g_pd3dDevice = nullptr;
    ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;

    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    //createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return;

    LoadTextureFromMemory(embedded_image, sizeof(embedded_image), g_pd3dDevice, &g_EmbedTexture, &EmbedWidth, &EmbedHeight);
    LoadTextureFromMemory(Icon, sizeof(Icon), g_pd3dDevice, &g_LogoTexture, &g_LogoW, &g_LogoH);

    ID3D11RenderTargetView* g_mainRenderTargetView;

    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();

    ShowWindow(hWnd, SW_SHOWDEFAULT);
    UpdateWindow(hWnd);
    DWORD dwMyID = ::GetCurrentThreadId();
    DWORD dwCurID = ::GetWindowThreadProcessId(hWnd, NULL);
    AttachThreadInput(dwCurID, dwMyID, TRUE);
    SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
    SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE);
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);
    SetActiveWindow(hWnd);
    AttachThreadInput(dwCurID, dwMyID, FALSE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.IniFilename = NULL;
    //io.DisplaySize = ImGui::GetMainViewport()->Size;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImFontConfig FontConfig;
    FontConfig.FontDataOwnedByAtlas = false;
    ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
        (void*)font, sizeof(font), 17.f, &FontConfig);

    auto& mStyle = ImGui::GetStyle();
    mStyle.WindowRounding = 0.f;
    mStyle.ItemSpacing = ImVec2(20, 6);
    mStyle.ItemInnerSpacing = ImVec2(8, 4);
    mStyle.FrameRounding = 4.5f;
    mStyle.GrabMinSize = 14.0f;
    mStyle.GrabRounding = 16.0f;
    mStyle.ScrollbarSize = 18.0f;
    mStyle.ScrollbarRounding = 16.0f;

    ImGuiStyle& style = mStyle;
    auto C = [](float r, float g, float b, float a = 1.f) { return ImVec4(r, g, b, a); };
    ImVec4* col = style.Colors;

    // Neutral graphite dark/grey theme (slight cool tint), ATLAS-style spread.
    col[ImGuiCol_WindowBg]             = C(0.090f, 0.094f, 0.106f, 1.00f); // graphite background
    col[ImGuiCol_ChildBg]              = C(0.090f, 0.094f, 0.106f, 1.00f);
    col[ImGuiCol_PopupBg]              = C(0.063f, 0.067f, 0.078f, 0.98f); // bar
    col[ImGuiCol_Border]               = C(0.196f, 0.204f, 0.227f, 1.00f);
    col[ImGuiCol_BorderShadow]         = C(0.f, 0.f, 0.f, 0.f);
    col[ImGuiCol_Text]                 = C(0.882f, 0.894f, 0.918f, 1.00f);
    col[ImGuiCol_TextDisabled]         = C(0.435f, 0.451f, 0.490f, 1.00f);
    col[ImGuiCol_TextSelectedBg]       = Accent(0.28f);
    col[ImGuiCol_FrameBg]              = C(0.137f, 0.145f, 0.165f, 1.00f);
    col[ImGuiCol_FrameBgHovered]       = C(0.176f, 0.184f, 0.208f, 1.00f);
    col[ImGuiCol_FrameBgActive]        = C(0.216f, 0.227f, 0.255f, 1.00f);
    col[ImGuiCol_TitleBg]              = C(0.063f, 0.067f, 0.078f, 1.00f);
    col[ImGuiCol_TitleBgActive]        = C(0.063f, 0.067f, 0.078f, 1.00f);
    col[ImGuiCol_TitleBgCollapsed]     = C(0.063f, 0.067f, 0.078f, 0.90f);
    col[ImGuiCol_MenuBarBg]            = C(0.063f, 0.067f, 0.078f, 1.00f);
    col[ImGuiCol_CheckMark]            = Accent();
    col[ImGuiCol_SliderGrab]           = Accent();
    col[ImGuiCol_SliderGrabActive]     = AccentDk();
    col[ImGuiCol_Button]               = C(0.157f, 0.165f, 0.188f, 1.00f);
    col[ImGuiCol_ButtonHovered]        = C(0.216f, 0.227f, 0.255f, 1.00f);
    col[ImGuiCol_ButtonActive]         = C(0.255f, 0.267f, 0.298f, 1.00f);
    col[ImGuiCol_Header]               = Accent(0.14f);
    col[ImGuiCol_HeaderHovered]        = Accent(0.22f);
    col[ImGuiCol_HeaderActive]         = Accent(0.30f);
    col[ImGuiCol_Separator]            = C(0.196f, 0.204f, 0.227f, 1.00f);
    col[ImGuiCol_SeparatorHovered]     = Accent(0.40f);
    col[ImGuiCol_SeparatorActive]      = Accent(0.70f);
    col[ImGuiCol_ScrollbarBg]          = C(0.063f, 0.067f, 0.078f, 0.40f);
    col[ImGuiCol_ScrollbarGrab]        = C(0.196f, 0.204f, 0.227f, 1.00f);
    col[ImGuiCol_ScrollbarGrabHovered] = C(0.255f, 0.267f, 0.298f, 1.00f);
    col[ImGuiCol_ScrollbarGrabActive]  = C(0.318f, 0.333f, 0.369f, 1.00f);
    col[ImGuiCol_Tab]                  = C(0.063f, 0.067f, 0.078f, 1.00f);
    col[ImGuiCol_TabHovered]           = Accent(0.18f);
    col[ImGuiCol_TabSelected]          = C(0.090f, 0.094f, 0.106f, 1.00f);
    col[ImGuiCol_PlotLines]            = Accent();
    col[ImGuiCol_PlotHistogram]        = Accent();
    //ImGui::StyleColorsDark();

    //ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    ImGui_ImplWin32_Init(hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    bool done = false;
    bool g_SwapChainOccluded = false;

    // Start a restored Auto Host countdown only after the launcher and its
    // countdown control are ready to render. Auto Host does not require a
    // saved preference snapshot; with Save Settings off it uses clean defaults.
    if (FConfiguration::bAutoHost.load(
            std::memory_order_acquire))
    {
        AutoHosting::ArmCountdown();
    }

    while (!done)
    {
        MSG msg;

        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }

        if (done)
            break;

        // Keep the countdown independent of rendering. In particular, a
        // minimized or occluded launcher must not pause Auto Hosting.
        AutoHosting::TickCountdown();
        AutoHosting::TickPostMatchShutdown();
        const bool bAutoHostCountdownThisFrame =
            AutoHosting::IsCountdownActive();
        const int AutoHostCountdownSecondsThisFrame =
            bAutoHostCountdownThisFrame
                ? AutoHosting::GetRemainingSeconds()
                : 0;

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            Sleep(10);
            continue;
        }

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            g_mainRenderTargetView->Release();

            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;

            ID3D11Texture2D* pBackBuffer;
            g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
            g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
            pBackBuffer->Release();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        SanitizeNativeLTMSelection(SelectedPlaylist);

        // Match the original one-shot UI behavior: once requested, keep the
        // button hidden for the rest of this joinable phase. A later server
        // lifecycle resets it automatically before the next joinable phase.
        static bool bStartBusEarlyDismissed = false;
        if (gsStatus != Joinable)
            bStartBusEarlyDismissed = false;

        main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(WindowWidth * main_scale, WindowHeight * main_scale), ImGuiCond_Always);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::Begin("Magnesium", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();

        const float W = ImGui::GetWindowWidth();
        const float H = ImGui::GetWindowHeight();
        const ImVec2 wp = ImGui::GetWindowPos();
        const float TopBarH = 48.f;
        const float SidebarW = 150.f;

        int SelectedUI = 0;
        int hasEvent = 0;
        if (hasEvent == 0)
        {
            hasEvent = 1;
            for (auto& Event : Events::EventsArray)
            {
                if (Event.EventVersion != VersionInfo.FortniteVersion)
                    continue;
                hasEvent = 2;
            }
        }

        // ---- Top bar (#284a2c): logo + branding ----
        ImGui::GetWindowDrawList()->AddRectFilled(wp, ImVec2(wp.x + W, wp.y + TopBarH),
            ImGui::GetColorU32(ImVec4(0.063f, 0.067f, 0.078f, 1.f)));
        {
            const float LogoSize = 32.f;
            const float PadL = 14.f;
            ImGui::SetCursorPos(ImVec2(PadL, (TopBarH - LogoSize) * 0.5f));
            if (g_LogoTexture)
                ImGui::Image((void*)g_LogoTexture, ImVec2(LogoSize, LogoSize));
            else
                ImGui::Dummy(ImVec2(LogoSize, LogoSize));

            ImGui::SameLine(PadL + LogoSize + 10.f);
            const float TitleY = (TopBarH - ImGui::GetTextLineHeight()) * 0.5f;
            ImGui::SetCursorPosY(TitleY);
            ImGui::PushStyleColor(ImGuiCol_Text, Accent());
            ImGui::Text("MAGNESIUM");
            ImGui::PopStyleColor();
            ImGui::SameLine(0.f, 7.f);
            ImGui::SetCursorPosY(TitleY);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.62f, 0.68f, 1.f));
            ImGui::Text("| Gameserver");
            ImGui::PopStyleColor();
            ImGui::SameLine(0.f, 8.f);
            ImGui::SetCursorPosY(TitleY);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.46f, 0.48f, 0.54f, 1.f));
            ImGui::TextUnformatted("v2.5.0");
            ImGui::PopStyleColor();

            // FN / UE versions on the right, aligned to the visible viewport so they
            // never run off the window edge.
            char ver[48];
            snprintf(ver, sizeof(ver), "FN %.2f  \xC2\xB7  UE %.2f", VersionInfo.FortniteVersion, VersionInfo.EngineVersion);
            const float verW = ImGui::CalcTextSize(ver).x;
            const float rightEdge = ImGui::GetIO().DisplaySize.x;
            ImGui::SetCursorPos(ImVec2(rightEdge - verW - 18.f, TitleY));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.54f, 0.56f, 0.62f, 0.92f));
            ImGui::TextUnformatted(ver);
            ImGui::PopStyleColor();
        }

        // Keep the top-bar rule in the Magnesium window layer. The old
        // foreground draw-list placement rendered this chrome over modal
        // windows, including the loadout picker.
        {
            ImDrawList* fdl = ImGui::GetWindowDrawList();
            const ImU32 lineCol = IM_COL32(50, 52, 58, 255);
            fdl->AddLine(
                ImVec2(wp.x, wp.y + TopBarH - 1.f),
                ImVec2(wp.x + W, wp.y + TopBarH - 1.f),
                lineCol, 1.f);
        }

        // ---- Sidebar (#284a2c): vertical tabs replacing the old tab bar ----
        static int s_ActiveUI = 0;
        ImGui::SetCursorPos(ImVec2(0.f, TopBarH));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.063f, 0.067f, 0.078f, 1.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::BeginChild("##sidebar", ImVec2(SidebarW, H - TopBarH), false, ImGuiWindowFlags_NoScrollbar);
        {
            const float TabH = 38.f;
            const float TabsTop = 12.f;
            float yy = TabsTop;
            float activeY = -1.f;
            const bool inMatch = !FConfiguration::bReadyToStart;

            struct TabDef { const char* label; int ui; bool show; };
            TabDef tabs[] = {
                { "Match",      0, true },
                { "Lategame",   3, inMatch },
                { "Playlist",   1, inMatch },
                { "Creative",   5, inMatch && SelectedPlaylist == static_cast<int>(Playlist::Creative) },
                { "Custom Map", 6, inMatch && FConfiguration::bIsCustomMap },
                { "Player Bot", 4, inMatch },
                { "Players",    2, gsStatus >= Joinable },
                { "Calendar",   9, Calendar::HasSnowControls() },
                { "Trickshot",  7, FConfiguration::bEnableTrickshotTab },
                { "Credits",    8, true },
            };

            for (auto& t : tabs)
            {
                if (!t.show) continue;
                SidebarTab(t.label, t.ui, yy, TabH, &s_ActiveUI);
                if (s_ActiveUI == t.ui) activeY = yy + TabH * 0.5f;
                yy += TabH;
            }

            if (activeY < 0.f) { s_ActiveUI = 0; activeY = TabsTop + TabH * 0.5f; }

            static float s_IndY = -1.f;
            if (s_IndY < 0.f) s_IndY = activeY;
            float lerp = ImGui::GetIO().DeltaTime * 16.f; if (lerp > 1.f) lerp = 1.f;
            s_IndY += (activeY - s_IndY) * lerp;
            const ImVec2 sbPos = ImGui::GetWindowPos();
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(sbPos.x, sbPos.y + s_IndY - 9.f), ImVec2(sbPos.x + 3.f, sbPos.y + s_IndY + 9.f),
                ImGui::GetColorU32(Accent()), 2.f);

            // Primary CTA pinned to the bottom of the sidebar: start the server.
            if (gsStatus == NotReady && !FConfiguration::bReadyToStart)
            {
                const float bMargin = 12.f;
                const float bH = 40.f;
                const float bW = SidebarW - bMargin * 2.f;
                const bool bAutoHostCountdown =
                    bAutoHostCountdownThisFrame;
                // Anchor to the visible viewport height (the window is taller than the
                // actual client area, so using H would push this off-screen).
                const float visH = ImGui::GetIO().DisplaySize.y;
                ImGui::SetCursorPos(ImVec2(bMargin, (visH - TopBarH) - bH - bMargin));
                const ImVec2 bp = ImGui::GetCursorScreenPos();
                if (bAutoHostCountdown)
                {
                    // The normal CTA becomes a display-only countdown while
                    // Auto Hosting owns startup.
                    ImGui::Dummy(ImVec2(bW, bH));
                }
                else if (ImGui::InvisibleButton(
                             "##startserver",
                             ImVec2(bW, bH)))
                {
                    AutoHosting::CancelCountdown();
                    AutoHosting::SaveNow(true);
                    FConfiguration::bReadyToStart.store(
                        true, std::memory_order_release);
                }
                const bool bHov =
                    !bAutoHostCountdown &&
                    ImGui::IsItemHovered();
                const bool bAct =
                    !bAutoHostCountdown &&
                    ImGui::IsItemActive();
                ImDrawList* bdl = ImGui::GetWindowDrawList();
                const ImVec4 fillC =
                    bAutoHostCountdown
                        ? ImVec4(0.28f, 0.31f, 0.38f, 1.f)
                        : (bAct
                               ? ImVec4(
                                     0.62f, 0.66f,
                                     0.78f, 1.f)
                               : (bHov
                                      ? ImVec4(
                                            0.88f, 0.91f,
                                            0.97f, 1.f)
                                      : Accent()));
                bdl->AddRectFilled(bp, ImVec2(bp.x + bW, bp.y + bH), ImGui::GetColorU32(fillC), 6.f);

                char CountdownLabel[48]{};
                const char* bLbl = "START SERVER";
                if (bAutoHostCountdown)
                {
                    snprintf(
                        CountdownLabel,
                        sizeof(CountdownLabel),
                        "STARTING IN %ds",
                        AutoHostCountdownSecondsThisFrame);
                    bLbl = CountdownLabel;
                }
                const ImVec2 bts = ImGui::CalcTextSize(bLbl);
                const ImVec2 tpos(bp.x + (bW - bts.x) * 0.5f, bp.y + (bH - bts.y) * 0.5f);
                const ImU32 tcol =
                    bAutoHostCountdown
                        ? ImGui::GetColorU32(Accent())
                        : IM_COL32(16, 18, 22, 255);
                bdl->AddText(tpos, tcol, bLbl);
                bdl->AddText(ImVec2(tpos.x + 1.f, tpos.y), tcol, bLbl); // faux-bold
            }
        }
        {
            // Draw the sidebar edge in the sidebar's own draw list so it sits
            // above sidebar contents but below later popup/modal windows.
            const ImVec2 SidebarPos = ImGui::GetWindowPos();
            const float SidebarWidth = ImGui::GetWindowWidth();
            const float SidebarHeight = ImGui::GetWindowHeight();
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(
                    SidebarPos.x + SidebarWidth - 1.f,
                    SidebarPos.y),
                ImVec2(
                    SidebarPos.x + SidebarWidth - 1.f,
                    SidebarPos.y + SidebarHeight),
                IM_COL32(50, 52, 58, 255),
                1.f);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        SelectedUI = s_ActiveUI;

        // ---- Content panel (inset; transparent so the #32703b background shows) ----
        const float ContentPadX = 22.f;
        ImGui::SetCursorPos(ImVec2(SidebarW + ContentPadX, TopBarH + 14.f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::BeginChild("##content", ImVec2((W - SidebarW) - ContentPadX * 2.f, (H - TopBarH) - 26.f), false);
        float Width = 260.0f;
        float Height = 0.0f;
        const float SectionWidth = ContentSectionWidth(Width);

        static char commandBuffer[1024] = { 0 };
        static char playlistBuffer[1024] = { 0 };
        static unsigned int PlaylistBufferResetGeneration =
            GPreferenceEditorGeneration;
        if (PlaylistBufferResetGeneration !=
            GPreferenceEditorGeneration)
        {
            playlistBuffer[0] = '\0';
            PlaylistBufferResetGeneration =
                GPreferenceEditorGeneration;
        }
        switch (SelectedUI)
        {
        case 0:
        {
            SectionHeader("Match Information", SectionWidth);
            BeginSectionBody();

            ImGui::Text("- Status: ");
            ImGui::SameLine(0.0f, 0.0f);

            ImVec4 Color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // default white

            if (!FConfiguration::bReadyToStart)
            {
                Color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f); // gray
                ImGui::TextColored(Color, "Configuring...");
            }
            else if (gsStatus == NotReady)
            {
                Color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); // yellow
                ImGui::TextColored(Color, "Setting up the server...");
            }
            else if (gsStatus == Joinable)
            {
                Color = ImVec4(0.0f, 1.0f, 0.0f, 1.0f); // green
                ImGui::TextColored(Color, "Joinable!");
            }
            else if (gsStatus == StartedMatch)
            {
                Color = ImVec4(1.0f, 0.65f, 0.0f, 1.0f); // orange
                ImGui::TextColored(Color, "Match Started.");
            }
            else if (gsStatus == Ended)
            {
                Color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f); // red
                ImGui::TextColored(Color, "Match Ended.");
            }
            else
            {
                ImGui::TextColored(Color, "N/A");
            }

            ImGui::Text("- Mode: %s", GetSelectedPlaylistModeName());
            if (FConfiguration::bInfiniteRender)
            {
                ImGui::TextUnformatted("- Infinite Render: ");
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Enabled");
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextUnformatted(" (only works on the last player to join!)");
            }
            ImGui::Text(
                "- Server Port: %d",
                FConfiguration::Port.load(std::memory_order_relaxed));
            ImGui::Text(
                "- Server Tick Rate: %.0f",
                FConfiguration::MaxTickRate.load(
                    std::memory_order_relaxed));

            const ULONGLONG JoinableAtMs =
                GServerJoinableAtMs.load(std::memory_order_acquire);

            if (gsStatus == Joinable && JoinableAtMs != 0)
            {
                const ULONGLONG Now = GetTickCount64();
                const double UptimeSeconds =
                    Now >= JoinableAtMs
                        ? static_cast<double>(Now - JoinableAtMs) / 1000.0
                        : 0.0;
                const std::string Uptime =
                    FormatDurationSeconds(UptimeSeconds);
                ImGui::Text("- Uptime: %s", Uptime.c_str());
            }

            if (gsStatus >= Joinable)
            {
                AFortGameMode* GameMode = nullptr;
                auto World = UWorld::GetWorld();
                auto AuthorityGameMode = World ? World->AuthorityGameMode : nullptr;
                auto AthenaGameModeClass = AFortGameModeAthena::StaticClass();

                // The frontend game mode derives from the generic FortGameMode
                // but does not expose Athena's AlivePlayers array. Resolve that
                // reflected property only on an actual Athena game mode.
                if (AuthorityGameMode && AthenaGameModeClass &&
                    AuthorityGameMode->IsA(AthenaGameModeClass))
                {
                    auto Candidate = (AFortGameMode*)AuthorityGameMode;
                    if (Candidate->HasAlivePlayers())
                        GameMode = Candidate;
                }

                int AliveCount = 0;

                if (GameMode)
                    AliveCount = GameMode->AlivePlayers.Num();

                AliveCount = (std::max)(
                    AliveCount,
                    CountConnectedPlayersForDisplay(World));

                ImGui::Text("- Players: %d", AliveCount);

                static std::string LastElimStatusMessage;
                static std::chrono::high_resolution_clock::time_point AddMessageTime;

                if (!FConfiguration::ElimStatusMessage.empty() && FConfiguration::ElimStatusMessage != LastElimStatusMessage)
                {
                    LastElimStatusMessage = FConfiguration::ElimStatusMessage;
                    AddMessageTime = std::chrono::high_resolution_clock::now();
                }

                if (!LastElimStatusMessage.empty() && duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now() - AddMessageTime).count() < 120)
                {
                    ImVec4 KillerColor = ImVec4(0x4e / 255.f, 0x86 / 255.f, 0xa5 / 255.f, 1.0f);
                    ImVec4 EliminatedColor = ImVec4(0xa5 / 255.f, 0x56 / 255.f, 0x4c / 255.f, 1.0f);
                    ImVec4 WeaponColor = ImVec4(1.0f, 0.84f, 0.0f, 1.0f);

                    ImGui::TextUnformatted("- ");
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextColored(KillerColor, "%s", FConfiguration::ElimKillerName.c_str());
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextUnformatted(" eliminated ");
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextColored(EliminatedColor, "%s", FConfiguration::ElimEliminatedName.c_str());
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextUnformatted(" from ");
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::Text("%sm!", FConfiguration::ElimDistance.c_str());
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextUnformatted(" (");
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextColored(WeaponColor, "%s", FConfiguration::ElimWeaponName.c_str());
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextUnformatted(")");

                    static bool bHasLogged = false;

                    if (!bHasLogged)
                    {
                        printf("- %s eliminated %s from %sm! (%s)\n", FConfiguration::ElimKillerName.c_str(), FConfiguration::ElimEliminatedName.c_str(), FConfiguration::ElimDistance.c_str(), FConfiguration::ElimWeaponName.c_str());
                        bHasLogged = true;
                    }
                }
                else
                {
                    LastElimStatusMessage.clear();
                    FConfiguration::ElimStatusMessage.clear();
                }
            }

            EndSectionBody();

            const bool bIsOnlyUp =
                SelectedPlaylist ==
                    static_cast<int>(Playlist::OnlyUp);
            const bool bIsEventPlaylist =
                SelectedPlaylist ==
                    static_cast<int>(Playlist::Event);
            const bool bShowsOnlyUpPreGameConfig =
                bIsOnlyUp && gsStatus < Joinable;
            const bool bShowsDefaultPreGameConfig =
                UsesDefaultMatchSettings(SelectedPlaylist);
            const bool bShowsEventBusControl =
                bIsEventPlaylist &&
                EventUsesSpawnIslandBusControl() &&
                gsStatus == Joinable;
            const bool bShowsDefaultMatchSettings =
                bShowsDefaultPreGameConfig;

            if (gsStatus <= Joinable &&
                (bShowsOnlyUpPreGameConfig ||
                 bShowsDefaultPreGameConfig ||
                 bShowsEventBusControl) &&
                !(gsStatus == Joinable &&
                  bStartBusEarlyDismissed))
            {
                SectionHeader(
                    "Pre-Game Configuration", SectionWidth);
                BeginSectionBody();

                if (bShowsOnlyUpPreGameConfig)
                {
                    AtomicCheckbox(
                        "Disable Jump Fatigue",
                        FConfiguration::bDisableJumpFatigue);
                    AtomicCheckbox(
                        "Player Has Pickaxe",
                        FConfiguration::bHasPickaxe);
                }
                else if (bShowsDefaultPreGameConfig &&
                         gsStatus < Joinable)
                {
                    if (AtomicCheckbox(
                            "Auto Bus Start",
                            FConfiguration::bAutoBusStart))
                    {
                        FConfiguration::bBusSettingsUserOverride.store(
                            true, std::memory_order_release);
                    }

                    static bool bInitializedZone = false;

                    if (!bInitializedZone)
                    {
                        if (!AutoHosting::
                                HasRestoredPreferences())
                        {
                            FConfiguration::LateGameZone =
                                FConfiguration::IsS27()
                                    ? 1
                                    : 4;
                        }
                        bInitializedZone = true;
                    }

                    static bool bAutoDumpDefaultInitialized =
                        false;
                    if (!bAutoDumpDefaultInitialized)
                    {
                        if (!AutoHosting::
                                HasRestoredPreferences() &&
                            VersionInfo.FortniteVersion == 19.20)
                        {
                            FConfiguration::bAutoDump = false;
                        }
                        bAutoDumpDefaultInitialized = true;
                    }

                    AtomicCheckbox(
                        "Auto Dump Text",
                        FConfiguration::bAutoDump);
                    AtomicCheckbox(
                        "Use Custom Map",
                        FConfiguration::bIsCustomMap);

                    if (!FConfiguration::bReadyToStart)
                    {
                        TrickshotTabCheckbox(
                            "Enable Trickshot Tab");
                    }

                    if (FConfiguration::bAutoBusStart &&
                        AtomicLabeledSliderFloat(
                            "Bus Start Delay",
                            "##bus-start-delay",
                            FConfiguration::BusStartDelay,
                            0.0f, 300.0f,
                            "%.0f sec", Width))
                    {
                        FConfiguration::bBusSettingsUserOverride.store(
                            true, std::memory_order_release);
                    }

                    if (AtomicLabeledSliderFloat(
                            "Max Tick Rate",
                            "##max-tick-rate",
                            FConfiguration::MaxTickRate,
                            FConfiguration::MinimumMaxTickRate,
                            FConfiguration::MaximumMaxTickRate,
                            "%.0f Hz", Width))
                    {
                        FConfiguration::bMaxTickRateUserOverride.store(
                            true, std::memory_order_release);
                    }
                }

                if (gsStatus == Joinable &&
                    (bShowsDefaultPreGameConfig ||
                     bShowsEventBusControl) &&
                    !bStartBusEarlyDismissed)
                {
                    ImGui::Spacing();

                    if (ImGui::Button(
                            "Start Bus Early",
                            ImVec2(Width, Height)))
                    {
                        bStartBusEarlyDismissed = true;
                        FConfiguration::bStartBusRequested.store(
                            true, std::memory_order_release);
                    }
                }

                EndSectionBody();
            }

            if (gsStatus == StartedMatch)
            {
                SectionHeader("Storm Control", SectionWidth);
                BeginSectionBody();

                if (!UFortGameStateComponent_BattleRoyaleGamePhaseLogic::
                        GetSafeZonePausedSnapshot())
                {
                    if (ImGui::Button("Pause Safe Zone", ImVec2(Width, Height)))
                        UFortGameStateComponent_BattleRoyaleGamePhaseLogic::
                            RequestSafeZonePaused(true);
                }
                else
                {
                    if (ImGui::Button("Resume Safe Zone", ImVec2(Width, Height)))
                        UFortGameStateComponent_BattleRoyaleGamePhaseLogic::
                            RequestSafeZonePaused(false);
                }

                if (ImGui::Button("Skip Safe Zone", ImVec2(Width, Height)))
                {
                    auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;

                    if (GameMode->HasSafeZoneIndicator())
                    {
                        if (GameMode->SafeZoneIndicator)
                        {
                            GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
                            GameMode->SafeZoneIndicator->SafeZoneFinishShrinkTime = GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime + 0.05f;
                        }
                    }
                    else
                    {
                        auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(UWorld::GetWorld());

                        if (GamePhaseLogic->SafeZoneIndicator)
                        {
                            GamePhaseLogic->SafeZoneIndicator->SafeZoneStartShrinkTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
                            GamePhaseLogic->SafeZoneIndicator->SafeZoneFinishShrinkTime = GamePhaseLogic->SafeZoneIndicator->SafeZoneStartShrinkTime + 0.05f;
                        }
                    }

                    // UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"skipsafezone"), nullptr);
                }

                if (ImGui::Button("Start Shrinking Safe Zone", ImVec2(Width, Height)))
                {
                    auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;

                    if (GameMode->HasSafeZoneIndicator())
                    {
                        if (GameMode->SafeZoneIndicator)
                            GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
                    }
                    else
                    {
                        auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(UWorld::GetWorld());

                        if (GamePhaseLogic->SafeZoneIndicator)
                            GamePhaseLogic->SafeZoneIndicator->SafeZoneStartShrinkTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
                    }

                    // UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"startshrinksafezone"), nullptr);
                }

                EndSectionBody();
            }

            if (gsStatus >= Joinable)
            {
                SectionHeader("World Actions", SectionWidth);
                BeginSectionBody();

                if (ImGui::Button("Reset Player Builds", ImVec2(Width, Height)))
                {
                    TArray<ABuildingSMActor*> Builds;
                    Utils::GetAll<ABuildingSMActor>(Builds);

                    for (auto& Build : Builds)
                    {
                        if (Build && Build->bPlayerPlaced)
                            Build->SilentDie(true);
                    }

                    Builds.Free();
                }

                if (ImGui::Button("Destroy Floor Loot", ImVec2(Width, Height)))
                {
                    TArray<AActor*> Pickups;
                    Utils::GetAll<AActor>(AFortPickupAthena::StaticClass(), Pickups);

                    for (auto& Pickup : Pickups)
                    {
                        if (Pickup)
                            Pickup->K2_DestroyActor();
                    }

                    Pickups.Free();
                }

                EndSectionBody();

                if (SelectedPlaylist == (int)Playlist::Event && hasEvent == 2)
                {
                    bool bHasMatchingEvent = false;

                    for (auto& Event : Events::EventsArray)
                    {
                        const bool bMatchesEventPlaylist = !Event.PlaylistPath || wcscmp(FConfiguration::Playlist, Event.PlaylistPath) == 0;

                        if (Event.EventVersion == VersionInfo.FortniteVersion && bMatchesEventPlaylist)
                        {
                            bHasMatchingEvent = true;
                            break;
                        }
                    }

                    if (bHasMatchingEvent)
                    {
                        SectionHeader("Event", SectionWidth);
                        BeginSectionBody();

                        if (ImGui::Button("Start Event", ImVec2(Width, Height)))
                            Events::StartEvent();

                        EndSectionBody();
                    }
                }
            }

            const bool bCanShowGliderRedeploy =
                bShowsDefaultMatchSettings &&
                gsStatus >= Joinable &&
                gsStatus < Ended &&
                FConfiguration::
                    IsGliderRedeploySupportedBuild();
            const bool bPlaylistHidesRespawnSection =
                !bShowsDefaultMatchSettings ||
                SelectedPlaylist ==
                    static_cast<int>(Playlist::FoodFight) ||
                SelectedPlaylist ==
                    static_cast<int>(Playlist::DeepFriedSquads) ||
                SelectedPlaylist ==
                    static_cast<int>(Playlist::ArsenalSolos);
            const bool bCanShowRespawns =
                !bPlaylistHidesRespawnSection &&
                (VersionInfo.FortniteVersion >= 8.00 ||
                    gsStatus < Joinable);

            if (bCanShowRespawns)
            {
                SectionHeader("Respawns", SectionWidth);
                BeginSectionBody();

                // The render thread owns only the configuration value. The
                // authoritative playlist/GameState policy is applied from the
                // server tick so native LTM mutators cannot overwrite it after
                // this click (and UObject state is never mutated from ImGui).
                const bool bRespawnsWereEnabled =
                    FConfiguration::bForceRespawns;
                if (AtomicCheckbox(
                        "Infinite Respawns",
                        FConfiguration::bForceRespawns) &&
                    bRespawnsWereEnabled &&
                    !FConfiguration::bForceRespawns)
                {
                    // These controls are children of Infinite Respawns. Do
                    // not leave an invisible stale child affecting later
                    // permanent deaths after its parent is switched off.
                    FConfiguration::PermanentRespawn =
                        false;
                    FConfiguration::bKeepInventory =
                        false;
                    FConfiguration::bMidZoneRespawning =
                        false;
                }

                if (FConfiguration::bForceRespawns)
                {
                    ImGui::Indent(12.f);

                    AtomicCheckbox(
                        "Storm Respawns",
                        FConfiguration::PermanentRespawn);

                    AtomicCheckbox(
                        "Keep Inventory on Respawn",
                        FConfiguration::bKeepInventory);
                    AtomicCheckbox(
                        "Midzone Respawns",
                        FConfiguration::bMidZoneRespawning);

                    AtomicLabeledSliderInt(
                        "Respawn Time", "##respawn-time",
                        FConfiguration::RespawnTime,
                        1, 10, Width);

                    AtomicLabeledSliderInt(
                        "Respawn Height", "##respawn-height",
                        FConfiguration::RespawnHeight,
                        1000, 50000, Width);

                    ImGui::Unindent(12.f);
                }

                EndSectionBody();
            }

            const int PublishedPlaylist = GetSelectedPlaylist();
            const bool bConfiguredFoodFightPlaylist =
                FConfiguration::Playlist &&
                (wcscmp(
                     FConfiguration::Playlist,
                     L"/Game/Athena/Playlists/Barrier/"
                     L"Playlist_Barrier.Playlist_Barrier") == 0 ||
                 wcscmp(
                     FConfiguration::Playlist,
                     L"/Game/Athena/Playlists/Barrier/"
                     L"Playlist_Barrier_16_B_Lava."
                     L"Playlist_Barrier_16_B_Lava") == 0);
            const bool bFoodFightConfiguration =
                SelectedPlaylist ==
                    static_cast<int>(Playlist::FoodFight) ||
                SelectedPlaylist ==
                    static_cast<int>(Playlist::DeepFriedSquads) ||
                PublishedPlaylist ==
                    static_cast<int>(Playlist::FoodFight) ||
                PublishedPlaylist ==
                    static_cast<int>(Playlist::DeepFriedSquads) ||
                bConfiguredFoodFightPlaylist;
            if (bFoodFightConfiguration && gsStatus < Ended)
            {
                SectionHeader("LTM Configuration", SectionWidth);
                BeginSectionBody();

                const int MaximumObjectiveHealth =
                    FConfiguration::
                        GetFoodFightObjectiveHealthMaximum();
                const int StoredObjectiveHealth =
                    FConfiguration::FoodFightObjectiveHealth.load(
                        std::memory_order_acquire);
                int DisplayObjectiveHealth =
                    StoredObjectiveHealth ==
                            FConfiguration::
                                FoodFightObjectiveHealthAuthored
                        ? MaximumObjectiveHealth
                        : std::clamp(
                              StoredObjectiveHealth,
                              FConfiguration::
                                  FoodFightObjectiveHealthMinimum,
                              MaximumObjectiveHealth);

                ImGui::BeginDisabled(gsStatus >= StartedMatch);
                if (LabeledSliderInt(
                        "Objective Health",
                        "##food-fight-objective-health",
                        &DisplayObjectiveHealth,
                        FConfiguration::
                            FoodFightObjectiveHealthMinimum,
                        MaximumObjectiveHealth,
                        Width,
                        "%d HP"))
                {
                    FConfiguration::FoodFightObjectiveHealth.store(
                        DisplayObjectiveHealth,
                        std::memory_order_release);
                }
                ImGui::EndDisabled();

                ImGui::BeginDisabled(gsStatus != StartedMatch);
                if (ImGui::Button(
                        "Drop Wall",
                        ImVec2(Width, Height)))
                {
                    FFortAthenaNativeLTMCompatibility::
                        RequestFoodFightWallDrop();
                }
                ImGui::EndDisabled();

                EndSectionBody();
            }

            if (gsStatus >= Joinable)
            {
                SectionHeader("Gameplay Toggles", SectionWidth);
                BeginSectionBody();

                if (bCanShowGliderRedeploy)
                {
                    AtomicCheckbox(
                        "Glider Redeploy",
                        FConfiguration::bGliderRedeploy);
                }

                AtomicCheckbox(
                    "Infinite Materials",
                    FConfiguration::bInfiniteMats);
                AtomicCheckbox(
                    "Infinite Ammo",
                    FConfiguration::bInfiniteAmmo);
                AtomicCheckbox(
                    "Toggle Cheat Commands",
                    FConfiguration::bEnableCheats);
                TrickshotTabCheckbox(
                    "Show Trickshot Tab");
                AtomicCheckbox(
                    "Siphon",
                    FConfiguration::bSiphon);

                if (FConfiguration::bSiphon)
                {
                    ImGui::Indent(12.f);

                    ImGui::TextUnformatted("Siphon Amount");
                    ImGui::SetNextItemWidth(Width);
                    AtomicInputInt(
                        "##siphon-amount",
                        FConfiguration::SiphonAmount);

					struct FSiphonAnimationOption
					{
						int Type;
						const char* Label;
					};
					std::vector<FSiphonAnimationOption> SiphonAnimations = {
						{ 0, "Default" }
					};

					if (VersionInfo.FortniteVersion >= 11.00)
					{
						SiphonAnimations.push_back({ 1, "Slurp" });
						SiphonAnimations.push_back(
							{ 2, "Bandage Bazooka" });
					}

					if (VersionInfo.FortniteVersion >= 12.50)
					{
						SiphonAnimations.push_back(
							{ 3, "Orange Paint" });
						SiphonAnimations.push_back(
							{ 4, "Purple Paint" });
					}

					SiphonAnimations.push_back({ 5, "Health Siphon" });

					if (VersionInfo.FortniteVersion >= 19.00)
					{
						SiphonAnimations.push_back({ 6, "Med Mist" });
					}

					if (VersionInfo.FortniteVersion >= 11.40)
					{
						SiphonAnimations.push_back(
							{ 7, "Upgrade Weapon" });
					}

					if (VersionInfo.FortniteVersion == 12.41)
					{
						SiphonAnimations.push_back(
							{ 8, "Astronomical Event Glow" });
					}

					if (VersionInfo.FortniteVersion == 17.30)
					{
						SiphonAnimations.push_back(
							{ 9, "Rift Tour Golden Glow" });
						SiphonAnimations.push_back(
							{ 10, "Rift Tour Rift" });
						SiphonAnimations.push_back(
							{ 11, "Rift Tour Paint Boost" });
					}

					std::vector<const char*> SiphonAnimationLabels;
					SiphonAnimationLabels.reserve(
						SiphonAnimations.size());
					int SelectedSiphonAnimation = 0;
					const int ConfiguredSiphonAnimation =
						FConfiguration::SiphonAnimType.load(
							std::memory_order_acquire);
					bool bFoundConfiguredSiphonAnimation = false;
					for (int OptionIndex = 0;
						 OptionIndex < (int)SiphonAnimations.size();
						 ++OptionIndex)
					{
						const auto& Option = SiphonAnimations[OptionIndex];
						SiphonAnimationLabels.push_back(Option.Label);
						if (Option.Type == ConfiguredSiphonAnimation)
						{
							SelectedSiphonAnimation = OptionIndex;
							bFoundConfiguredSiphonAnimation = true;
						}
					}
					if (!bFoundConfiguredSiphonAnimation)
					{
						FConfiguration::SiphonAnimType.store(
							0, std::memory_order_release);
					}

					ImGui::TextUnformatted("Siphon Animation");
					ImGui::SetNextItemWidth(Width);
					if (ImGui::Combo(
						"##siphon-animation",
						&SelectedSiphonAnimation,
						SiphonAnimationLabels.data(),
						(int)SiphonAnimationLabels.size()))
					{
						FConfiguration::SiphonAnimType.store(
							SiphonAnimations[
								SelectedSiphonAnimation].Type,
							std::memory_order_release);
					}

                    ImGui::Unindent(12.f);
                }

                EndSectionBody();
            }

            if (gsStatus >= Joinable)
            {
                SectionHeader("Server Console", SectionWidth);
                BeginSectionBody();

                ImGui::SetNextItemWidth(Width);
                ImGui::InputText("##server-command", commandBuffer, IM_ARRAYSIZE(commandBuffer));

                if (ImGui::Button("Execute Console Command", ImVec2(Width, Height)))
                {
                    std::string str = commandBuffer;
                    auto wstr = std::wstring(str.begin(), str.end());

                    UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(wstr.c_str()), nullptr);
                }

                EndSectionBody();
            }

            if (!FConfiguration::bReadyToStart.load(
                    std::memory_order_acquire))
            {
                SectionHeader("Auto Hosting", SectionWidth);
                BeginSectionBody();

                if (AtomicCheckbox(
                        "Toggle Auto Host",
                        FConfiguration::bAutoHost))
                {
                    const bool bAutoHostEnabled =
                        FConfiguration::bAutoHost.load(
                            std::memory_order_acquire);
                    if (bAutoHostEnabled &&
                        !FConfiguration::bReadyToStart.load(
                            std::memory_order_acquire))
                    {
                        // Auto-hosted servers default cheat commands closed to
                        // guests. The verified host override remains available,
                        // and the user can explicitly opt back in afterward.
                        FConfiguration::bEnableCheats.store(
                            false, std::memory_order_release);
                        AutoHosting::ArmCountdown();
                        AutoHosting::SaveNow(true);
                    }
                    else
                    {
                        AutoHosting::CancelCountdown();
                        AutoHosting::SaveNow(false);
                    }
                }

                if (FConfiguration::bAutoHost.load(
                        std::memory_order_acquire))
                {
                    ImGui::Indent(12.f);
                    if (AtomicCheckbox(
                            "Save Settings",
                            FConfiguration::bSaveAutoHostSettings))
                    {
                        AutoHosting::SaveNow(
                            FConfiguration::bSaveAutoHostSettings.load(
                                std::memory_order_acquire));
                    }
                    ImGui::Unindent(12.f);
                }

                bool bCountdownActive =
                    AutoHosting::IsCountdownActive();
                if (bCountdownActive)
                {
                    int RemainingSeconds = (std::max)(
                        1,
                        (std::min)(
                            bAutoHostCountdownThisFrame
                                ? AutoHostCountdownSecondsThisFrame
                                : AutoHosting::
                                      GetRemainingSeconds(),
                            60));
                    ImGui::SetNextItemWidth(Width);
                    const bool bDelayChanged =
                        ImGui::SliderInt(
                            "##auto-host-delay-countdown",
                            &RemainingSeconds,
                            1, 60,
                            "%d sec",
                            ImGuiSliderFlags_AlwaysClamp);
                    if (bDelayChanged)
                    {
                        // Treat an edit as a new countdown duration. The display
                        // continues ticking down, while the chosen value remains
                        // the saved delay used on the next launcher start.
                        FConfiguration::AutoHostDelaySeconds.store(
                            RemainingSeconds,
                            std::memory_order_release);
                        AutoHosting::ArmCountdown();
                    }
                    if (ImGui::IsItemDeactivatedAfterEdit())
                    {
                        // Commit once the drag or text edit finishes instead of
                        // performing synchronous disk writes on every mouse move.
                        AutoHosting::SaveNow(false);
                    }
                }
                else
                {
                    int DelaySeconds =
                        FConfiguration::AutoHostDelaySeconds.load(
                            std::memory_order_acquire);
                    ImGui::SetNextItemWidth(Width);
                    if (ImGui::SliderInt(
                            "##auto-host-delay",
                            &DelaySeconds,
                            1, 60,
                            "%d sec",
                            ImGuiSliderFlags_AlwaysClamp))
                    {
                        FConfiguration::AutoHostDelaySeconds.store(
                            DelaySeconds,
                            std::memory_order_release);
                        if (FConfiguration::bAutoHost.load(
                                std::memory_order_acquire) &&
                            !FConfiguration::bReadyToStart.load(
                                std::memory_order_acquire))
                        {
                            // Restart from the newly selected duration so the
                            // visible countdown never jumps to an unexpected start.
                            AutoHosting::ArmCountdown();
                        }
                        AutoHosting::SaveNow(false);
                    }
                }

                if (ImGui::Button(
                        "Reset Preferences",
                        ImVec2(Width, Height)))
                {
                    AutoHosting::ResetPreferences();
                    bCountdownActive = false;
                }

                if (bCountdownActive)
                {
                    ImGui::TextDisabled(
                        "Adjust the timer, or disable Auto Host to cancel.");
                }
                else if (FConfiguration::bAutoHost.load(
                             std::memory_order_acquire))
                {
                    ImGui::TextDisabled(
                        "Saved for the next launcher start.");
                }

                EndSectionBody();
            }

            // "Start Server" button moved to the sidebar (bottom) for easy access.

            break;
        }
        case 1:
        {
            if (VersionInfo.FortniteVersion == 7.40 || VersionInfo.FortniteVersion == 14.40 || VersionInfo.FortniteVersion == 27.11 || VersionInfo.FortniteVersion == 30.00)
            {
                SectionHeader("Custom Playlists", SectionWidth);
                BeginSectionBody();

                if (VersionInfo.FortniteVersion == 7.40)
                {
                    ImGui::RadioButton("Backrooms Map", &SelectedPlaylist, (int)Playlist::Backrooms);
                }

                if (VersionInfo.FortniteVersion == 27.11)
                {
                    ImGui::RadioButton("Gav 1v1 Map", &SelectedPlaylist, (int)Playlist::Gav);
                    ImGui::RadioButton("Only Up Map", &SelectedPlaylist, (int)Playlist::OnlyUp);
                    ImGui::RadioButton("Tilted FFA", &SelectedPlaylist, (int)Playlist::TiltedZW);
                }

                if (VersionInfo.FortniteVersion == 14.40)
                {
                    ImGui::RadioButton("Retrac 1v1 Map", &SelectedPlaylist, (int)Playlist::Retrac1v1);
                    ImGui::RadioButton("Retrac Turtle Fights", &SelectedPlaylist, (int)Playlist::RetracTurtle);
                    //ImGui::RadioButton("Retrac Water Map", &SelectedPlaylist, (int)Playlist::RetracWater);
                    //ImGui::RadioButton("Twine 1v1 Map", &SelectedPlaylist, (int)Playlist::Twine1v1);
                }

                if (VersionInfo.FortniteVersion == 30.00)
                {
                    ImGui::RadioButton("Boxfights", &SelectedPlaylist, (int)Playlist::Boxfight);
                }

                EndSectionBody();
            }

            SectionHeader("Playlists", SectionWidth);
            BeginSectionBody();

            ImGui::RadioButton("Solos", &SelectedPlaylist, (int)Playlist::Solos);
            ImGui::RadioButton("Duos", &SelectedPlaylist, (int)Playlist::Duos);

            if (VersionInfo.FortniteVersion >= 7.40) // 7.30 content update idfk
            {
                ImGui::RadioButton("Trios", &SelectedPlaylist, (int)Playlist::Trios);
            }

            ImGui::RadioButton("Squads", &SelectedPlaylist, (int)Playlist::Squads);

            const bool bGetawayAvailable =
                VersionInfo.FortniteVersion == 10.40 ||
                FFortAthenaHeistCompatibility::IsSupportedBuild();
            const bool bNative1040LTMsAvailable =
                VersionInfo.FortniteVersion == 10.40;
            const bool bFoodFightAvailable =
                bNative1040LTMsAvailable ||
                FFortAthenaNativeLTMCompatibility::
                    IsOriginalFoodFightSupportedBuild();
            const bool bScoreRoyaleAvailable =
                IsScoreRoyalePlaylistBuild();

            const bool bInfinityGauntletAvailable =
                VersionInfo.FortniteVersion == 4.20;
            if (bInfinityGauntletAvailable &&
                ImGui::RadioButton(
                    "Infinity Gauntlet Solos",
                    &SelectedPlaylist,
                    (int)Playlist::InfinityGauntletSolos))
            {
                FConfiguration::SetLateGameEnabled(false);
            }

            if (VersionInfo.FortniteVersion >= 20.00)
            {
                ImGui::RadioButton("Zero Build Solos", &SelectedPlaylist, (int)Playlist::ZBSolos);
                ImGui::RadioButton("Zero Build Duos", &SelectedPlaylist, (int)Playlist::ZBDuos);
                ImGui::RadioButton("Zero Build Trios", &SelectedPlaylist, (int)Playlist::ZBTrios);
                ImGui::RadioButton("Zero Build Squads", &SelectedPlaylist, (int)Playlist::ZBSquads);
            }

            if (VersionInfo.FortniteVersion >= 8.20)
            {
                ImGui::RadioButton("Arena Solos", &SelectedPlaylist, (int)Playlist::ArenaSolos);
                ImGui::RadioButton("Arena Duos", &SelectedPlaylist, (int)Playlist::ArenaDuos);
                ImGui::RadioButton("Arena Trios", &SelectedPlaylist, (int)Playlist::ArenaTrios);
                ImGui::RadioButton("Arena Squads", &SelectedPlaylist, (int)Playlist::ArenaSquads);

                if (VersionInfo.FortniteVersion >= 20.00)
                {
                    ImGui::RadioButton("Arena Zero Build Solos", &SelectedPlaylist, (int)Playlist::ArenaZBSolos);
                    ImGui::RadioButton("Arena Zero Build Duos", &SelectedPlaylist, (int)Playlist::ArenaZBDuos);
                    ImGui::RadioButton("Arena Zero Build Trios", &SelectedPlaylist, (int)Playlist::ArenaZBTrios);
                    ImGui::RadioButton("Arena Zero Build Squads", &SelectedPlaylist, (int)Playlist::ArenaZBSquads);
                }
            }

            if (VersionInfo.FortniteVersion >= 6.10)
            {
                ImGui::RadioButton("Tournament Solos", &SelectedPlaylist, (int)Playlist::TournamentSolos);
                ImGui::RadioButton("Tournament Duos", &SelectedPlaylist, (int)Playlist::TournamentDuos);
                ImGui::RadioButton("Tournament Trios", &SelectedPlaylist, (int)Playlist::TournamentTrios);
                ImGui::RadioButton("Tournament Squads", &SelectedPlaylist, (int)Playlist::TournamentSquads);
            }

            if (VersionInfo.FortniteVersion >= 7.10)
            {
                ImGui::RadioButton("One Shot Solos", &SelectedPlaylist, (int)Playlist::OneShotSolos);
                ImGui::RadioButton("One Shot Duos", &SelectedPlaylist, (int)Playlist::OneShotDuos);
                ImGui::RadioButton("One Shot Squads", &SelectedPlaylist, (int)Playlist::OneShotSquads);
            }

            if (bGetawayAvailable &&
                ImGui::RadioButton(
                    "The Getaway Solos",
                    &SelectedPlaylist,
                    (int)Playlist::GetawaySolos))
            {
                SanitizeNativeLTMSelection(SelectedPlaylist);
            }

            if (bGetawayAvailable &&
                ImGui::RadioButton(
                    "The Getaway Duos",
                    &SelectedPlaylist,
                    (int)Playlist::GetawayDuos))
            {
                SanitizeNativeLTMSelection(SelectedPlaylist);
            }

            if (bGetawayAvailable &&
                ImGui::RadioButton(
                    "The Getaway Squads",
                    &SelectedPlaylist,
                    (int)Playlist::GetawaySquads))
            {
                SanitizeNativeLTMSelection(SelectedPlaylist);
            }

            if (bScoreRoyaleAvailable &&
                ImGui::RadioButton(
                    "Score Royale Solos",
                    &SelectedPlaylist,
                    (int)Playlist::ScoreRoyaleSolo))
            {
                SanitizeNativeLTMSelection(SelectedPlaylist);
            }

            if (bScoreRoyaleAvailable &&
                ImGui::RadioButton(
                    "Score Royale Duos",
                    &SelectedPlaylist,
                    (int)Playlist::ScoreRoyaleDuos))
            {
                SanitizeNativeLTMSelection(SelectedPlaylist);
            }

            if (bScoreRoyaleAvailable &&
                ImGui::RadioButton(
                    "Score Royale Squads",
                    &SelectedPlaylist,
                    (int)Playlist::ScoreRoyaleSquads))
            {
                SanitizeNativeLTMSelection(SelectedPlaylist);
            }

            if (VersionInfo.FortniteVersion >= 7.10)
            {
                ImGui::RadioButton("Siphon Solos", &SelectedPlaylist, (int)Playlist::SiphonSolos);
                ImGui::RadioButton("Siphon Duos", &SelectedPlaylist, (int)Playlist::SiphonDuos);
                ImGui::RadioButton("Siphon Squads", &SelectedPlaylist, (int)Playlist::SiphonSquads);
                ImGui::RadioButton("Unvaulted Solos", &SelectedPlaylist, (int)Playlist::UnvSolos);
                ImGui::RadioButton("Unvaulted Duos", &SelectedPlaylist, (int)Playlist::UnvDuos);
                ImGui::RadioButton("Unvaulted Trios", &SelectedPlaylist, (int)Playlist::UnvTrios);
                ImGui::RadioButton("Unvaulted Squads", &SelectedPlaylist, (int)Playlist::UnvSquads);
                ImGui::RadioButton("Slide Solos", &SelectedPlaylist, (int)Playlist::SlideSolos);
                ImGui::RadioButton("Slide Duos", &SelectedPlaylist, (int)Playlist::SlideDuos);
            }

            if ((VersionInfo.FortniteVersion >= 8.20 && VersionInfo.FortniteVersion <= 10.40) || VersionInfo.FortniteVersion >= 15.20)
            {
                ImGui::RadioButton("Floor Is Lava Solos", &SelectedPlaylist, (int)Playlist::FILSolos);
                ImGui::RadioButton("Floor Is Lava Duos", &SelectedPlaylist, (int)Playlist::FILDuos);
                ImGui::RadioButton("Floor Is Lava Squads", &SelectedPlaylist, (int)Playlist::FILSquads);
            }

            if (bNative1040LTMsAvailable &&
                ImGui::RadioButton(
                    "Food Fight: Deep Fried",
                    &SelectedPlaylist,
                    (int)Playlist::DeepFriedSquads))
            {
                SanitizeNativeLTMSelection(SelectedPlaylist);
            }

            if (bFoodFightAvailable &&
                ImGui::RadioButton(
                    "Food Fight",
                    &SelectedPlaylist,
                    (int)Playlist::FoodFight))
            {
                SanitizeNativeLTMSelection(SelectedPlaylist);
            }

            if (bNative1040LTMsAvailable &&
                ImGui::RadioButton(
                    "Arsenal Solos",
                    &SelectedPlaylist,
                    (int)Playlist::ArsenalSolos))
            {
                SanitizeNativeLTMSelection(SelectedPlaylist);
            }

            if (bNative1040LTMsAvailable &&
                ImGui::RadioButton(
                    "Wick's Bounty Solo",
                    &SelectedPlaylist,
                    (int)Playlist::WicksBountySolo))
            {
                SanitizeNativeLTMSelection(SelectedPlaylist);
            }

            if (bNative1040LTMsAvailable &&
                ImGui::RadioButton(
                    "Wick's Bounty Duo",
                    &SelectedPlaylist,
                    (int)Playlist::WicksBountyDuo))
            {
                SanitizeNativeLTMSelection(SelectedPlaylist);
            }

            if (bNative1040LTMsAvailable &&
                ImGui::RadioButton(
                    "Wick's Bounty Squads",
                    &SelectedPlaylist,
                    (int)Playlist::WicksBountySquads))
            {
                SanitizeNativeLTMSelection(SelectedPlaylist);
            }

            if (bNative1040LTMsAvailable &&
                ImGui::RadioButton(
                    "Bounty Solo",
                    &SelectedPlaylist,
                    (int)Playlist::BountySolo))
            {
                SanitizeNativeLTMSelection(SelectedPlaylist);
            }

            if (bNative1040LTMsAvailable &&
                ImGui::RadioButton(
                    "Bounty Duo",
                    &SelectedPlaylist,
                    (int)Playlist::BountyDuo))
            {
                SanitizeNativeLTMSelection(SelectedPlaylist);
            }

            if (bNative1040LTMsAvailable &&
                ImGui::RadioButton(
                    "Bounty Squads",
                    &SelectedPlaylist,
                    (int)Playlist::BountySquads))
            {
                SanitizeNativeLTMSelection(SelectedPlaylist);
            }

            if (bNative1040LTMsAvailable &&
                ImGui::RadioButton(
                    "Avengers: Endgame",
                    &SelectedPlaylist,
                    (int)Playlist::AvengersEndgame))
            {
                SanitizeNativeLTMSelection(SelectedPlaylist);
            }

            if (bNative1040LTMsAvailable &&
                ImGui::RadioButton(
                    "Disco Domination",
                    &SelectedPlaylist,
                    (int)Playlist::DiscoDomination))
            {
                SanitizeNativeLTMSelection(SelectedPlaylist);
            }

            if (VersionInfo.FortniteVersion >= 4.5 && VersionInfo.FortniteVersion < 11.31)
            {
                ImGui::RadioButton("Playground", &SelectedPlaylist, (int)Playlist::Playground);
            }

            if (VersionInfo.FortniteVersion >= 7.00)
            {
                ImGui::RadioButton("Creative ", &SelectedPlaylist, (int)Playlist::Creative);
            }

            for (auto& Event : Events::EventsArray)
            {
                if (Event.EventVersion == VersionInfo.FortniteVersion)
                {
                    ImGui::RadioButton("Event Playlist", &SelectedPlaylist, (int)Playlist::Event);
                }
            }

            ImGui::RadioButton("Custom", &SelectedPlaylist, (int)Playlist::Custom);

            EndSectionBody();

            // Handle every radio-button transition in the frame it occurs.
            // This is especially important when leaving a custom-map preset:
            // its hidden respawn values must be restored before Start Server
            // can consume the newly selected ordinary playlist.
            SanitizeNativeLTMSelection(SelectedPlaylist);

            switch (SelectedPlaylist)
            {
            case (int)Playlist::Solos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo";
                break;
            }
            case (int)Playlist::Duos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Playlist_DefaultDuo.Playlist_DefaultDuo";
                break;
            }
            case (int)Playlist::Trios:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Trios/Playlist_Trios.Playlist_Trios";
                break;
            }
            case (int)Playlist::Squads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Playlist_DefaultSquad.Playlist_DefaultSquad";
                break;
            }
            case (int)Playlist::GetawaySolos:
            {
                if (bGetawayAvailable)
                {
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Bling/Playlist_Bling_Solo.Playlist_Bling_Solo";
                }
                else
                {
                    SelectedPlaylist = (int)Playlist::Solos;
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo";
                }
                break;
            }
            case (int)Playlist::GetawayDuos:
            {
                if (bGetawayAvailable)
                {
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Bling/Playlist_Bling_Duos.Playlist_Bling_Duos";
                }
                else
                {
                    SelectedPlaylist = (int)Playlist::Duos;
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Playlist_DefaultDuo.Playlist_DefaultDuo";
                }
                break;
            }
            case (int)Playlist::GetawaySquads:
            {
                if (bGetawayAvailable)
                {
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Bling/Playlist_Bling_Squads.Playlist_Bling_Squads";
                }
                else
                {
                    SelectedPlaylist = (int)Playlist::Squads;
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Playlist_DefaultSquad.Playlist_DefaultSquad";
                }
                break;
            }
            case (int)Playlist::ScoreRoyaleSolo:
            {
                if (bScoreRoyaleAvailable)
                {
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Score/"
                        L"Playlist_Score_Solo."
                        L"Playlist_Score_Solo";
                    SanitizeNativeLTMSelection(SelectedPlaylist);
                }
                else
                {
                    SelectedPlaylist = (int)Playlist::Solos;
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/"
                        L"Playlist_DefaultSolo."
                        L"Playlist_DefaultSolo";
                }
                break;
            }
            case (int)Playlist::ScoreRoyaleDuos:
            {
                if (bScoreRoyaleAvailable)
                {
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Score/"
                        L"Playlist_Score_Duos."
                        L"Playlist_Score_Duos";
                    SanitizeNativeLTMSelection(SelectedPlaylist);
                }
                else
                {
                    SelectedPlaylist = (int)Playlist::Duos;
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/"
                        L"Playlist_DefaultDuo."
                        L"Playlist_DefaultDuo";
                }
                break;
            }
            case (int)Playlist::ScoreRoyaleSquads:
            {
                if (bScoreRoyaleAvailable)
                {
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Score/"
                        L"Playlist_Score_Squads."
                        L"Playlist_Score_Squads";
                    SanitizeNativeLTMSelection(SelectedPlaylist);
                }
                else
                {
                    SelectedPlaylist = (int)Playlist::Squads;
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/"
                        L"Playlist_DefaultSquad."
                        L"Playlist_DefaultSquad";
                }
                break;
            }
            case (int)Playlist::FoodFight:
            {
                if (bFoodFightAvailable)
                {
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Barrier/"
                        L"Playlist_Barrier.Playlist_Barrier";
                    SanitizeNativeLTMSelection(SelectedPlaylist);
                }
                else
                {
                    SelectedPlaylist = (int)Playlist::Squads;
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/"
                        L"Playlist_DefaultSquad."
                        L"Playlist_DefaultSquad";
                }
                break;
            }
            case (int)Playlist::DeepFriedSquads:
            {
                if (bNative1040LTMsAvailable)
                {
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Barrier/Playlist_Barrier_16_B_Lava.Playlist_Barrier_16_B_Lava";
                    SanitizeNativeLTMSelection(SelectedPlaylist);
                }
                else
                {
                    SelectedPlaylist = (int)Playlist::Squads;
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Playlist_DefaultSquad.Playlist_DefaultSquad";
                }
                break;
            }
            case (int)Playlist::ArsenalSolos:
            {
                if (bNative1040LTMsAvailable)
                {
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/gg/Playlist_Gg_Reverse.Playlist_Gg_Reverse";
                    SanitizeNativeLTMSelection(SelectedPlaylist);
                }
                else
                {
                    SelectedPlaylist = (int)Playlist::Solos;
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo";
                }
                break;
            }
            case (int)Playlist::WicksBountySolo:
            {
                if (bNative1040LTMsAvailable)
                {
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Wax/"
                        L"Playlist_Wax_Solo.Playlist_Wax_Solo";
                    SanitizeNativeLTMSelection(SelectedPlaylist);
                }
                else
                {
                    SelectedPlaylist = (int)Playlist::Solos;
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/"
                        L"Playlist_DefaultSolo."
                        L"Playlist_DefaultSolo";
                }
                break;
            }
            case (int)Playlist::WicksBountyDuo:
            {
                if (bNative1040LTMsAvailable)
                {
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Wax/"
                        L"Playlist_Wax_Duos."
                        L"Playlist_Wax_Duos";
                    SanitizeNativeLTMSelection(SelectedPlaylist);
                }
                else
                {
                    SelectedPlaylist = (int)Playlist::Duos;
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/"
                        L"Playlist_DefaultDuo."
                        L"Playlist_DefaultDuo";
                }
                break;
            }
            case (int)Playlist::WicksBountySquads:
            {
                if (bNative1040LTMsAvailable)
                {
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Wax/"
                        L"Playlist_Wax_Squads."
                        L"Playlist_Wax_Squads";
                    SanitizeNativeLTMSelection(SelectedPlaylist);
                }
                else
                {
                    SelectedPlaylist = (int)Playlist::Squads;
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/"
                        L"Playlist_DefaultSquad."
                        L"Playlist_DefaultSquad";
                }
                break;
            }
            case (int)Playlist::BountySolo:
            {
                if (bNative1040LTMsAvailable)
                {
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Wax/"
                        L"Playlist_Bounty_Solo."
                        L"Playlist_Bounty_Solo";
                    SanitizeNativeLTMSelection(SelectedPlaylist);
                }
                else
                {
                    SelectedPlaylist = (int)Playlist::Solos;
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/"
                        L"Playlist_DefaultSolo."
                        L"Playlist_DefaultSolo";
                }
                break;
            }
            case (int)Playlist::BountyDuo:
            {
                if (bNative1040LTMsAvailable)
                {
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Wax/"
                        L"Playlist_Bounty_Duos."
                        L"Playlist_Bounty_Duos";
                    SanitizeNativeLTMSelection(SelectedPlaylist);
                }
                else
                {
                    SelectedPlaylist = (int)Playlist::Duos;
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/"
                        L"Playlist_DefaultDuo."
                        L"Playlist_DefaultDuo";
                }
                break;
            }
            case (int)Playlist::BountySquads:
            {
                if (bNative1040LTMsAvailable)
                {
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Wax/"
                        L"Playlist_Bounty_Squads."
                        L"Playlist_Bounty_Squads";
                    SanitizeNativeLTMSelection(SelectedPlaylist);
                }
                else
                {
                    SelectedPlaylist = (int)Playlist::Squads;
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/"
                        L"Playlist_DefaultSquad."
                        L"Playlist_DefaultSquad";
                }
                break;
            }
            case (int)Playlist::AvengersEndgame:
            {
                if (bNative1040LTMsAvailable)
                {
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Ashton/"
                        L"Playlist_Ashton_Lg.Playlist_Ashton_Lg";
                    SanitizeNativeLTMSelection(SelectedPlaylist);
                }
                else
                {
                    SelectedPlaylist = (int)Playlist::Squads;
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/"
                        L"Playlist_DefaultSquad."
                        L"Playlist_DefaultSquad";
                }
                break;
            }
            case (int)Playlist::DiscoDomination:
            {
                if (bNative1040LTMsAvailable)
                {
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/50v50/Disco/"
                        L"Playlist_Disco_32.Playlist_Disco_32";
                    SanitizeNativeLTMSelection(SelectedPlaylist);
                }
                else
                {
                    SelectedPlaylist = (int)Playlist::Squads;
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/"
                        L"Playlist_DefaultSquad."
                        L"Playlist_DefaultSquad";
                }
                break;
            }
            case (int)Playlist::InfinityGauntletSolos:
            {
                if (bInfinityGauntletAvailable)
                {
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Carmine/Playlist_Carmine.Playlist_Carmine";
                }
                else
                {
                    SelectedPlaylist = (int)Playlist::Solos;
                    FConfiguration::Playlist =
                        L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo";
                }
                break;
            }
            case (int)Playlist::ZBSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Playlist_NoBuildBR_Solo.Playlist_NoBuildBR_Solo";
                break;
            }
            case (int)Playlist::ZBDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Playlist_NoBuildBR_Duo.Playlist_NoBuildBR_Duo";
                break;
            }
            case (int)Playlist::ZBTrios:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Playlist_NoBuildBR_Trio.Playlist_NoBuildBR_Trio";
                break;
            }
            case (int)Playlist::ZBSquads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Playlist_NoBuildBR_Squad.Playlist_NoBuildBR_Squad";
                break;
            }
            case (int)Playlist::ArenaZBSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Competitive/Playlist_ShowdownAlt_NoBuildBR_Solo.Playlist_ShowdownAlt_NoBuildBR_Solo";
                break;
            }
            case (int)Playlist::ArenaZBDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Competitive/Playlist_ShowdownAlt_NoBuildBR_Duos.Playlist_ShowdownAlt_NoBuildBR_Duos";
                break;
            }
            case (int)Playlist::ArenaZBTrios:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Competitive/Playlist_ShowdownAlt_NoBuildBR_Trios.Playlist_ShowdownAlt_NoBuildBR_Trios";
                break;
            }
            case (int)Playlist::ArenaZBSquads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Competitive/Playlist_ShowdownAlt_NoBuildBR_Squads.Playlist_ShowdownAlt_NoBuildBR_Squads";
                break;
            }
            case (int)Playlist::OneShotSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Low/Playlist_Low_Solo.Playlist_Low_Solo";
                break;
            }
            case (int)Playlist::OneShotDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Low/Playlist_Low_Duos.Playlist_Low_Duos";
                break;
            }
            case (int)Playlist::OneShotSquads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Low/Playlist_Low_Squads.Playlist_Low_Squads";
                break;
            }
            case (int)Playlist::SiphonSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Vamp/Playlist_Vamp_Solo.Playlist_Vamp_Solo";
                break;
            }
            case (int)Playlist::SiphonDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Vamp/Playlist_Vamp_Duos.Playlist_Vamp_Duos";
                break;
            }
            case (int)Playlist::SiphonSquads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Vamp/Playlist_Vamp_Squad.Playlist_Vamp_Squad";
                break;
            }
            case (int)Playlist::UnvSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Unvaulted/Playlist_Unvaulted_Solo.Playlist_Unvaulted_Solo";
                break;
            }
            case (int)Playlist::UnvDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Unvaulted/Playlist_Unvaulted_Duos.Playlist_Unvaulted_Duos";
                break;
            }
            case (int)Playlist::UnvTrios:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Unvaulted/Playlist_Unvaulted_Trios.Playlist_Unvaulted_Trios";
                break;
            }
            case (int)Playlist::UnvSquads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Unvaulted/Playlist_Unvaulted_Squads.Playlist_Unvaulted_Squads";
                break;
            }
            case (int)Playlist::SlideSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Slide/Playlist_Slide_Solo.Playlist_Slide_Solo";
                break;
            }
            case (int)Playlist::SlideDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Slide/Playlist_Slide_Duos.Playlist_Slide_Duos";
                break;
            }
            case (int)Playlist::TournamentSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_Showdown_Solo.Playlist_Showdown_Solo";
                break;
            }
            case (int)Playlist::TournamentDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_Showdown_Duos.Playlist_Showdown_Duos";
                break;
            }
            case (int)Playlist::TournamentTrios:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_Showdown_Trios.Playlist_Showdown_Trios";
                break;
            }
            case (int)Playlist::TournamentSquads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_Showdown_Squads.Playlist_Showdown_Squads";
                break;
            }
            case (int)Playlist::ArenaSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Solo.Playlist_ShowdownAlt_Solo";
                break;
            }
            case (int)Playlist::ArenaDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Duos.Playlist_ShowdownAlt_Duos";
                break;
            }
            case (int)Playlist::ArenaTrios:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Trios.Playlist_ShowdownAlt_Trios";
                break;
            }
            case (int)Playlist::ArenaSquads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Squads.Playlist_ShowdownAlt_Squads";
                break;
            }
            case (int)Playlist::FILSolos:
            {
                if (VersionInfo.FortniteVersion <= 10.40)
                    FConfiguration::Playlist = L"/Game/Athena/Playlists/Fill/Playlist_Fill_Solo.Playlist_Fill_Solo";
                else
                    FConfiguration::Playlist = L"/Melt/Playlists/Playlist_Melt_Solo.Playlist_Melt_Solo";
                break;
            }
            case (int)Playlist::FILDuos:
            {
                if (VersionInfo.FortniteVersion <= 10.40)
                    FConfiguration::Playlist = L"/Game/Athena/Playlists/Fill/Playlist_Fill_Duos.Playlist_Fill_Duos";
                else
                    FConfiguration::Playlist = L"/Melt/Playlists/Playlist_Melt_Duos.Playlist_Melt_Duos";
                break;
            }
            case (int)Playlist::FILSquads:
            {
                if (VersionInfo.FortniteVersion <= 10.40)
                    FConfiguration::Playlist = L"/Game/Athena/Playlists/Fill/Playlist_Fill_Squads.Playlist_Fill_Squads";
                else
                    FConfiguration::Playlist = L"/Melt/Playlists/Playlist_Melt_Squads.Playlist_Melt_Squads";
                break;
            }
            case (int)Playlist::Playground:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Playground/Playlist_Playground.Playlist_Playground";
                break;
            }
            case (int)Playlist::Creative:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Creative/Playlist_PlaygroundV2.Playlist_PlaygroundV2";
                break;
            }
            case (int)Playlist::Gav:
            {
                FConfiguration::Playlist = L"/Game/Gav/Levels/GM_1v1/Playlist_Arena_DefaultSolo_Respawn.Playlist_Arena_DefaultSolo_Respawn";
                break;
            }
            case (int)Playlist::Retrac1v1:
            {
				FConfiguration::Playlist = L"/Buddy/Playlist/Playlist_Retrac_1v1.Playlist_Retrac_1v1";
                break;
            }
            case (int)Playlist::RetracTurtle:
            {
				FConfiguration::Playlist = L"/Buddy/Playlist/Playlist_Retrac_Turtle.Playlist_Retrac_Turtle";
                break;
            }
            case (int)Playlist::RetracWater:
            {
				FConfiguration::Playlist = L"/Game/Retrac/Playlists/Playlist_ShowdownAlt_Solo_Retrac.Playlist_ShowdownAlt_Solo_Retrac";
                break;
            }
            case (int)Playlist::TiltedZW:
            {
                FConfiguration::Playlist = L"/Game/Jett/TiltedZW/Playlist_TiltedZW_Jett.Playlist_TiltedZW_Jett";
                break;
			}
            case (int)Playlist::OnlyUp:
            {
                FConfiguration::Playlist = L"/Game/Jett/Playlist_OnlyUp_Jett.Playlist_OnlyUp_Jett";
                break;
			}
            case (int)Playlist::Twine1v1:
            {
                FConfiguration::Playlist = L"/Buddy/Playlists/Playlist_1v1Twine.Playlist_1v1Twine";
                break;
			}
            case (int)Playlist::Boxfight:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Respawn/Playlist_Respawn_Solo.Playlist_Respawn_Solo";
                break;
			}
            case (int)Playlist::Backrooms:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo";
                break;
            }
            case (int)Playlist::Event:
            {
                for (auto& Event : Events::EventsArray)
                {
                    if (Event.EventVersion == VersionInfo.FortniteVersion)
                    {
                        if (Event.PlaylistPath != nullptr)
                            FConfiguration::Playlist = Event.PlaylistPath;

                        break;
                    }
                }
                break;
            }
            case (int)Playlist::Custom:
            {
                break;
            }
            default:
            {
                break;
            }
            }

            // The mapping switch can fall back to a normal playlist when a
            // selected mode is unavailable on this version. Publish that
            // corrected value in the same frame.
            GUI::PublishSelectedPlaylist(
                SelectedPlaylist);

            if (SelectedPlaylist == (int)Playlist::Custom)
            {
                SectionHeader("Custom Playlist", SectionWidth);
                BeginSectionBody();

                ImGui::SetNextItemWidth(Width);
                ImGui::InputText("##custom-playlist", playlistBuffer, IM_ARRAYSIZE(playlistBuffer));

                if (ImGui::Button("Set Playlist", ImVec2(Width, Height)))
                {
                    std::string str = playlistBuffer;
                    static std::wstring persistentWstr;
                    persistentWstr = std::wstring(str.begin(), str.end());

                    FConfiguration::Playlist = persistentWstr.c_str();
                }

                EndSectionBody();
            }

            if (SelectedPlaylist == (int)Playlist::Event)
            {
                SectionHeader("Event Settings", SectionWidth);
                BeginSectionBody();

                AtomicCheckbox(
                    "Auto Start Event",
                    FConfiguration::bAutoStartEvent);

                if (FConfiguration::bAutoStartEvent)
                {
                    AtomicLabeledSliderFloat(
                        "Event Start Time",
                        "##event-start-time",
                        FConfiguration::EventStartTime,
                        30.0f, 300.0f,
                        "%.1f seconds", Width);
                }

                EndSectionBody();
            }

            break;
        }
        case 2:
        {
            auto World = UWorld::GetWorld();

            if (!World) 
                break;

            static int InspectedPlayerIdx = -1;
            static bool bIsInspecting = false;
			static AFortPlayerControllerAthena*
				InspectedPlayerController = nullptr;
			static UNetConnection*
				InspectedPlayerConnection = nullptr;
			static uint64_t InspectedControllerIdentity = 0;
			static uint64_t InspectedConnectionIdentity = 0;

			const auto ClearInspectedPlayer = [&]()
			{
				InspectedPlayerIdx = -1;
				bIsInspecting = false;
				InspectedPlayerController = nullptr;
				InspectedPlayerConnection = nullptr;
				InspectedControllerIdentity = 0;
				InspectedConnectionIdentity = 0;
			};

            UObject* NetDriver = World->NetDriver;

            if (!NetDriver) 
                break;

            UNetDriver* Driver = static_cast<UNetDriver*>(NetDriver);

            auto& ClientConnections = Driver->ClientConnections;
            AllControllers.clear();

            for (int i = 0; i < ClientConnections.Num(); i++)
            {
                auto Connection = ClientConnections[i];

                if (!Connection || !Connection->PlayerController) 
                    continue;

                AllControllers.push_back(std::make_pair((AFortPlayerControllerAthena*)Connection->PlayerController, Connection));
            }

            if (!bIsInspecting)
            {
                const std::string Header = "Players Connected: " + std::to_string(AllControllers.size());
                SectionHeader(Header.c_str(), SectionWidth);
                BeginSectionBody();

                for (int i = 0; i < AllControllers.size(); i++)
                {
                    auto& CurrentPair = AllControllers[i];
                    auto CurrentPlayerState = CurrentPair.first->PlayerState;

                    if (!CurrentPlayerState)
                    {
                        printf("PlayerState is null!\n");
                        continue;
                    }

                    auto Connection = CurrentPair.second;

                    std::string ButtonLabel = GUI::GetPlayerName(CurrentPlayerState, Connection);

                    if (ButtonLabel.empty())
                        ButtonLabel = std::string("Player ") + std::to_string(i + 1);

                    // Names are display text, not stable widget identifiers;
                    // two players may legitimately share one.  Key the row to
                    // its connection so each button remains independently usable.
                    ImGui::PushID(Connection);
                    const bool InspectClicked = ImGui::Button(
                        ButtonLabel.c_str(), ImVec2(Width, Height));
                    ImGui::PopID();
                    if (InspectClicked)
                    {
						const uint64_t ControllerIdentity =
							GetGuiObjectIdentityGuarded(
								CurrentPair.first);
						const uint64_t ConnectionIdentity =
							GetGuiObjectIdentityGuarded(Connection);
						if (ControllerIdentity && ConnectionIdentity)
						{
							InspectedPlayerIdx = i;
							bIsInspecting = true;
							InspectedPlayerController =
								CurrentPair.first;
							InspectedPlayerConnection = Connection;
							InspectedControllerIdentity =
								ControllerIdentity;
							InspectedConnectionIdentity =
								ConnectionIdentity;
						}
                    }
                }

                EndSectionBody();
            }
            else
            {
				// Connections can reorder or disappear between frames.  Re-find the
				// exact UObject generations selected by the user; never let a stale
				// numeric row silently retarget actions to another player.
				InspectedPlayerIdx = -1;
				for (int i = 0;
					 i < static_cast<int>(AllControllers.size());
					 ++i)
				{
					const auto& Pair = AllControllers[i];
					if (Pair.first != InspectedPlayerController ||
						Pair.second != InspectedPlayerConnection)
					{
						continue;
					}
					if (GetGuiObjectIdentityGuarded(Pair.first) ==
							InspectedControllerIdentity &&
						GetGuiObjectIdentityGuarded(Pair.second) ==
							InspectedConnectionIdentity)
					{
						InspectedPlayerIdx = i;
					}
					break;
				}
				if (InspectedPlayerIdx < 0)
                {
					ClearInspectedPlayer();
                    break;
                }

                auto TargetPC = AllControllers[InspectedPlayerIdx].first;
                if (!TargetPC)
                {
					ClearInspectedPlayer();
                    break;
                }

                auto TargetPS = TargetPC->PlayerState;
                AFortPlayerPawnAthena* TargetPawn = nullptr;
                auto PlayerPawnClass =
                    AFortPlayerPawnAthena::StaticClass();
                if (PlayerPawnClass &&
                    TargetPC->HasMyFortPawn() &&
                    TargetPC->MyFortPawn &&
                    TargetPC->MyFortPawn->IsA(
                        PlayerPawnClass))
                {
                    TargetPawn = TargetPC->MyFortPawn;
                }
                else if (PlayerPawnClass &&
                    TargetPC->HasPawn() &&
                    TargetPC->Pawn &&
                    TargetPC->Pawn->IsA(PlayerPawnClass))
                {
                    TargetPawn = TargetPC->Pawn;
                }

                if (!TargetPawn || !TargetPS)
                {
					ClearInspectedPlayer();
                    break;
                }

                if (ImGui::Button("Back", ImVec2(Width, Height)))
                {
					ClearInspectedPlayer();
                    break;
                }

                SectionHeader("Player Information", SectionWidth);
                BeginSectionBody();

                auto InspectedPlayerState = (AFortPlayerStateAthena*)AllControllers[InspectedPlayerIdx].first->PlayerState;
                auto InspectedConnection = AllControllers[InspectedPlayerIdx].second;

                std::string DisplayName = GUI::GetPlayerName(InspectedPlayerState, InspectedConnection);

                if (DisplayName.empty())
                    DisplayName = std::string("Player ") + std::to_string(InspectedPlayerIdx + 1);

                ImGui::TextUnformatted("Inspecting Player: ");
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(ImVec4(1.f, 1.f, 0.f, 1.0f), DisplayName.c_str());

				ImGui::Text("Join Order: #%d", InspectedPlayerIdx + 1);

                //ImGui::Text("Ping: %f ms", TargetPS->GetPingInMilliseconds()); // ig it js doesn't exist on some versions

                FPlayerCombatStats PlayerStats{};
                const bool HasPlayerStats =
                    TryCopyCachedPlayerCombatStats(
                        TargetPC,
                        (AFortPlayerStateAthena*)TargetPS,
                        TargetPawn,
                        PlayerStats);

                if (HasPlayerStats)
                    ImGui::Text("Kills: %d", PlayerStats.Kills);
                else
                    ImGui::TextUnformatted("Kills: --");

                ImGui::TextUnformatted("Health: ");
                ImGui::SameLine(0.0f, 0.0f);
                if (HasPlayerStats)
                {
                    ImGui::TextColored(
                        ImVec4(0.372f, 0.792f, 0.255f, 1.0f),
                        "%.0f", PlayerStats.Health);
                }
                else
                {
                    ImGui::TextUnformatted("--");
                }

                ImGui::TextUnformatted("Shield: ");
                ImGui::SameLine(0.0f, 0.0f);
                if (HasPlayerStats)
                {
                    ImGui::TextColored(
                        ImVec4(0.278f, 0.612f, 0.945f, 1.0f),
                        "%.0f", PlayerStats.Shield);
                }
                else
                {
                    ImGui::TextUnformatted("--");
                }

                EndSectionBody();

                SectionHeader("Player Loadout", SectionWidth);
                BeginSectionBody();

                PlayerLoadout::Render(
                    TargetPC,
                    SectionWidth - 20.f,
                    g_pd3dDevice);

                EndSectionBody();

                SectionHeader("Player Actions", SectionWidth);
                BeginSectionBody();

                if (ImGui::Button("Copy Player's Location", ImVec2(Width, Height)))
                {
                    auto Location = TargetPawn->K2_GetActorLocation();

                    Memcury::Util::CopyToClipboard(std::to_string(Location.X) + " " + std::to_string(Location.Y) + " " + std::to_string(Location.Z));
				}

                if (ImGui::Button("Teleport All Players", ImVec2(Width, Height)))
                    AFortPlayerControllerAthena::TeleportAllPlayersTo(TargetPC);

                if (ImGui::Button("Regenerate Health & Shield", ImVec2(Width, Height)))
                {
                    TargetPawn->SetHealth(TargetPawn->GetMaxHealth());
                    TargetPawn->SetShield(TargetPawn->GetMaxShield());

                    auto Handle = TargetPS->AbilitySystemComponent->MakeEffectContext();
                    FGameplayTag Tag;
                    static auto Cue = FName(L"GameplayCue.Shield.PotionConsumed");
                    Tag.TagName = Cue;
                    auto PredictionKey = (FPredictionKey*)malloc(FPredictionKey::Size());
                    memset((PBYTE)PredictionKey, 0, FPredictionKey::Size());
                    TargetPS->AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded(Tag, *PredictionKey, Handle);
                    TargetPS->AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted(Tag, *PredictionKey, Handle);
                    free(PredictionKey);
                }

                if (VersionInfo.FortniteVersion >= 5.00 &&
                    ImGui::Button(
                        "Rift Player", ImVec2(Width, Height)))
                {
					auto Loc = TargetPawn->K2_GetActorLocation();

                    static auto RiftClass = FindObject<UClass>(L"/Game/Athena/Items/Consumables/RiftItem/BGA_RiftPortal_Item_Athena.BGA_RiftPortal_Item_Athena_C");

                    if (RiftClass)
                    {
                        auto Actor = UWorld::SpawnActor<AActor>(
                            RiftClass, Loc, {});
                        if (Actor)
                        {
                            Actor->ForceNetUpdate();
                            Actor->K2_DestroyActor();
                        }
                    }
				}

                const bool bIsMinimumGodded =
                    AFortPlayerPawnAthena::
                        HasMinimumHealthGodMode(TargetPC);
                const bool bIsGodded =
                    bIsMinimumGodded ||
                    AFortPlayerPawnAthena::
                        HasFullHealthGodMode(TargetPC);

                if (bIsGodded)
                {
                    if (ImGui::Button("Ungod Player", ImVec2(Width, Height)))
                    {
                        AFortPlayerPawnAthena::DisableGodModes(
                            TargetPC, TargetPawn);
                    }
                }
                else
                {
                    if (ImGui::Button("God Player", ImVec2(Width, Height)))
                    {
                        const bool bAppliedGod =
                            AFortPlayerPawnAthena::
                                SetFullHealthGodMode(
                                    TargetPC, TargetPawn, true);

                        if (bAppliedGod)
                        {
                            float MaxHealth = TargetPawn->GetMaxHealth();
                            float MaxShield = TargetPawn->GetMaxShield();

                            TargetPawn->SetHealth(MaxHealth);
                            TargetPawn->SetShield(MaxShield);
                            TargetPawn->ForceNetUpdate();

                            if (TargetPS &&
                                TargetPS->HasAbilitySystemComponent() &&
                                TargetPS->AbilitySystemComponent)
                            {
                                auto AbilitySystem =
                                    TargetPS->AbilitySystemComponent;
                                auto Handle =
                                    AbilitySystem->MakeEffectContext();
                                FGameplayTag Tag{};
                                static auto Cue = FName(
                                    L"GameplayCue.Shield.PotionConsumed");
                                Tag.TagName = Cue;
                                auto PredictionKey =
                                    (FPredictionKey*)malloc(
                                        FPredictionKey::Size());
                                if (PredictionKey)
                                {
                                    memset(
                                        (PBYTE)PredictionKey, 0,
                                        FPredictionKey::Size());
                                    AbilitySystem
                                        ->NetMulticast_InvokeGameplayCueAdded(
                                            Tag, *PredictionKey, Handle);
                                    AbilitySystem
                                        ->NetMulticast_InvokeGameplayCueExecuted(
                                            Tag, *PredictionKey, Handle);
                                    free(PredictionKey);
                                }
                            }
                        }
                    }
                }

                if (ImGui::Button("Eliminate Player", ImVec2(Width, Height)))
                {
                    AFortPlayerControllerAthena::TryEliminatePlayer(
                        TargetPC);
                    bIsInspecting = false;
                }

                if (ImGui::Button("Kick Player", ImVec2(Width, Height)))
                {
                    TargetPC->ServerReturnToMainMenu("You have been kicked from the game by the host.");
					bIsInspecting = false;
                }

                ImGui::Spacing();
                ImGui::Spacing();

                static float LaunchX = 0.f;
                static float LaunchY = 0.f;
                static float LaunchZ = 0.f;

                ImGui::SetNextItemWidth(Width);
                ImGui::InputFloat("Launch X", &LaunchX);

                ImGui::SetNextItemWidth(Width);
                ImGui::InputFloat("Launch Y", &LaunchY);

                ImGui::SetNextItemWidth(Width);
                ImGui::InputFloat("Launch Z", &LaunchZ);

                if (ImGui::Button("Launch Player", ImVec2(Width, Height)))
                {
                    FVector LaunchVelocity = FVector(LaunchX, LaunchY, LaunchZ);
                    TargetPawn->LaunchCharacterJump(LaunchVelocity, false, nullptr, true);
                }

                ImGui::Spacing();
                ImGui::Spacing();

                static char WID[256] = {};
				static int Amount = 1.f;

                ImGui::SetNextItemWidth(Width);
                ImGui::InputText("Item To Give", WID, IM_ARRAYSIZE(WID));

                ImGui::SetNextItemWidth(Width);
                ImGui::InputInt("Amount To Give", &Amount);

                if (ImGui::Button("Give Item To Player", ImVec2(Width, Height)))
                {
                    if (WID[0] != '\0')
                    {
                        std::string ItemID = WID;
                        auto ItemDefinition = FindObject<UFortItemDefinition>(UEAllocatedWString(ItemID.begin(), ItemID.end()));

                        if (!ItemDefinition)
                            ItemDefinition = TUObjectArray::FindObject<UFortItemDefinition>(ItemID.c_str());

                        int32 Count = Amount;

                        if (Count <= 0)
							Count = ItemDefinition->GetMaxStackSize();

                        FVector FinalLoc = TargetPawn ? TargetPawn->K2_GetActorLocation() : FVector();

                        FVector ForwardVector = TargetPawn ? TargetPawn->GetActorForwardVector() : FVector();
                        ForwardVector.Z = 0.0f;
                        ForwardVector.Normalize();

                        const float RandomAngleVariation = ((float)rand() * 0.00109866634f) - 18.f;
                        const float FinalAngle = RandomAngleVariation * 0.017453292519943295f;

                        FinalLoc.X += cos(FinalAngle) * 100.f;
                        FinalLoc.Y += sin(FinalAngle) * 100.f;

                        auto Pickup = AFortInventory::SpawnPickup(FinalLoc, ItemDefinition, Count, -1, EFortPickupSourceTypeFlag::GetOther(), EFortPickupSpawnSource::GetUnset(), TargetPawn);

                        if (TargetPawn && Pickup)
                            TargetPawn->ServerHandlePickup(Pickup, Pickup->PickupLocationData.FlyTime, FVector(), true);
                    }
				}

                ImGui::Spacing();
                ImGui::Spacing();

                static std::string nameStr;
                ImGui::SetNextItemWidth(Width);
                ImGui::InputText("New Name", &nameStr);

                if (ImGui::Button("Change Player's Name", ImVec2(Width, Height)))
                {
                    if (!nameStr.empty())
                    {
                        std::wstring nameW(nameStr.begin(), nameStr.end());
                        FString NewName = FString(nameW.c_str());

                        if (!TargetPC)
                            return;

                        TargetPC->ServerChangeName(NewName);
                        TargetPS->OnRep_PlayerName();
                        //nameStr.clear();
                    }
                }

                EndSectionBody();
            }

            break;
        }
        case 3:
        {
            SectionHeader("Lategame Options", SectionWidth);
            BeginSectionBody();

            if (gsStatus < StartedMatch)
            {
                // Special maps and native objective modes require their normal
                // phase flow.
                const bool bLockLateGame =
                    LocksLateGameForSelection(
                        SelectedPlaylist);
                if (bLockLateGame)
                    FConfiguration::SetLateGameEnabled(false);

                ImGui::BeginDisabled(bLockLateGame);
                bool bLateGameEnabled = FConfiguration::bLateGame;
                if (ImGui::Checkbox("Late Game", &bLateGameEnabled))
                    FConfiguration::SetLateGameEnabled(bLateGameEnabled);
                ImGui::EndDisabled();

                if (FConfiguration::bLateGame)
                {
                    if (VersionInfo.FortniteVersion > 2.50)
                        AtomicCheckbox(
                            "Use Moving Bus",
                            FConfiguration::bMovingBus);

                    AtomicCheckbox(
                        "Use Long Zone",
                        FConfiguration::bLateGameLongZone);
                    if (AtomicCheckbox(
                            "Use Versionized Lategame Loadouts",
                            FConfiguration::
                                bUseVersionizedLoadout) &&
                        FConfiguration::
                            bUseVersionizedLoadout)
                    {
                        FConfiguration::
                            bUseCustomLoadout = false;
                    }
                    if (AtomicCheckbox(
                            "Use Custom Lategame Loadout",
                            FConfiguration::
                                bUseCustomLoadout) &&
                        FConfiguration::
                            bUseCustomLoadout)
                    {
                        FConfiguration::
                            bUseVersionizedLoadout = false;
                    }
                    AtomicCheckbox(
                        "Custom Safe Zone",
                        FConfiguration::bCustomSafeZone);

                    if (!FConfiguration::bCustomSafeZone)
                        AtomicLabeledSliderInt(
                            "Starting Zone",
                            "##starting-zone",
                            FConfiguration::LateGameZone,
                            1, 7, Width);
                    else
                    {
                        // Interactive minimap: click to place the zone center, drag
                        // out to set the radius. The numeric inputs below stay live as
                        // a fine-tune and as a fallback if the map image is unavailable.
                        static ID3D11ShaderResourceView* s_MapSRV = nullptr;
                        static int s_MapW = 0, s_MapH = 0;
                        static int s_RetryIn = 0;
                        static float s_MapZoom = 1.f;
                        static ImVec2 s_MapPan(0.f, 0.f);
                        static bool s_MapPanning = false;
                        static bool s_MapPanningMiddle = false;
                        if (!s_MapSRV) // retry until the minimap texture becomes resident
                        {
                            // Pixels produced by TickFlush should be uploaded on the
                            // next GUI frame, not after the normal three-second poll.
                            if (s_RetryIn <= 0 || SafeZoneMap::HasReadyPixels())
                            {
                                SafeZoneMap::Acquire(g_pd3dDevice, &s_MapSRV, &s_MapW, &s_MapH);
                                s_RetryIn = 180; // ~3s between attempts
                            }
                            else --s_RetryIn;
                        }

                        const SafeZoneMap::MapTransform map = SafeZoneMap::GetTransform();

                        if (s_MapSRV)
                        {
                            const float S = Width * 1.8f; // square canvas
                            ImVec2 uv0, uv1;
                            SafeZoneMap::GetImageUVs(uv0, uv1);
                            ImGui::InvisibleButton("##mapcanvas", ImVec2(S, S));
                            const ImVec2 r0 = ImGui::GetItemRectMin();
                            const ImVec2 r1(r0.x + S, r0.y + S);
                            ImGuiIO& io = ImGui::GetIO();

                            auto ClampMapPan = [&]()
                            {
                                const float minPan = S * (1.f - s_MapZoom);
                                s_MapPan.x = SafeZoneMap::Clamp(s_MapPan.x, minPan, 0.f);
                                s_MapPan.y = SafeZoneMap::Clamp(s_MapPan.y, minPan, 0.f);
                            };
                            ClampMapPan();

                            // Ctrl + wheel zooms toward the mouse so the point under
                            // the cursor remains stationary. The canvas itself stays
                            // fixed-size and clips the enlarged map and its overlays.
                            if (ImGui::IsItemHovered() && io.KeyCtrl)
                            {
                                ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
                                if (io.MouseWheel != 0.f)
                                {
                                    const float oldZoom = s_MapZoom;
                                    const float newZoom = SafeZoneMap::Clamp(
                                        oldZoom * powf(1.2f, io.MouseWheel), 1.f, 5.f);
                                    const ImVec2 mouseLocal(io.MousePos.x - r0.x, io.MousePos.y - r0.y);
                                    const ImVec2 imagePoint((mouseLocal.x - s_MapPan.x) / oldZoom,
                                                            (mouseLocal.y - s_MapPan.y) / oldZoom);
                                    s_MapZoom = newZoom;
                                    s_MapPan.x = mouseLocal.x - imagePoint.x * newZoom;
                                    s_MapPan.y = mouseLocal.y - imagePoint.y * newZoom;
                                    ClampMapPan();
                                }
                            }

                            // Ctrl + left drag pans the zoomed map. The gesture is latched on
                            // press so letting go of Ctrl mid-drag does not turn the rest of
                            // the drag into a zone edit. At 100% zoom ClampMapPan pins the
                            // offset to zero, so this is a no-op until the user zooms in.
                            if (ImGui::IsItemActivated() && io.KeyCtrl)
                                s_MapPanning = true;
                            if (!ImGui::IsItemActive())
                                s_MapPanning = false;

                            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
                                s_MapPanningMiddle = true;
                            if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle))
                                s_MapPanningMiddle = false;

                            if (s_MapPanning || s_MapPanningMiddle ||
                                (ImGui::IsItemHovered() && io.KeyCtrl && s_MapZoom > 1.f))
                                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

                            if (s_MapPanningMiddle && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
                            {
                                s_MapPan.x += io.MouseDelta.x;
                                s_MapPan.y += io.MouseDelta.y;
                                ClampMapPan();
                            }

                            if (s_MapPanning)
                            {
                                if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                                {
                                    s_MapPan.x += io.MouseDelta.x;
                                    s_MapPan.y += io.MouseDelta.y;
                                    ClampMapPan();
                                }
                            }
                            // Press => set center; drag => set radius from that center.
                            else
                            {
                                if (ImGui::IsItemActivated())
                                {
                                    const ImVec2 m = io.MousePos;
                                    const float u = SafeZoneMap::Clamp(
                                        (m.x - r0.x - s_MapPan.x) / (S * s_MapZoom), 0.f, 1.f);
                                    const float v = SafeZoneMap::Clamp(
                                        (m.y - r0.y - s_MapPan.y) / (S * s_MapZoom), 0.f, 1.f);
                                    SafeZoneMap::RememberSelection(u, v);
                                    float wx, wy;
                                    SafeZoneMap::PixelToWorld(u, v, 1.f, map, wx, wy);
                                    FConfiguration::CustomSafeZoneCenter.X = wx;
                                    FConfiguration::CustomSafeZoneCenter.Y = wy;
                                }
                                if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                                {
                                    const ImVec2 m = io.MousePos;
                                    const float u = SafeZoneMap::Clamp(
                                        (m.x - r0.x - s_MapPan.x) / (S * s_MapZoom), 0.f, 1.f);
                                    const float v = SafeZoneMap::Clamp(
                                        (m.y - r0.y - s_MapPan.y) / (S * s_MapZoom), 0.f, 1.f);
                                    float mouseX, mouseY;
                                    SafeZoneMap::PixelToWorld(u, v, 1.f, map, mouseX, mouseY);
                                    const float dx = mouseX - (float)FConfiguration::CustomSafeZoneCenter.X;
                                    const float dy = mouseY - (float)FConfiguration::CustomSafeZoneCenter.Y;
                                    FConfiguration::CustomSafeZoneRadius =
                                        SafeZoneMap::Clamp(sqrtf(dx * dx + dy * dy), 500.f, 100000.f);
                                }
                            }

                            // Overlay the stored center + radius every frame (also when idle).
                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            dl->PushClipRect(r0, r1, true);
                            const ImVec2 imageMin(r0.x + s_MapPan.x, r0.y + s_MapPan.y);
                            const ImVec2 imageMax(imageMin.x + S * s_MapZoom, imageMin.y + S * s_MapZoom);
                            dl->AddImage((void*)s_MapSRV, imageMin, imageMax, uv0, uv1);
                            float lx, ly;
                            SafeZoneMap::WorldToPixel((float)FConfiguration::CustomSafeZoneCenter.X,
                                                      (float)FConfiguration::CustomSafeZoneCenter.Y, S, map, lx, ly);
                            const ImVec2 c(r0.x + s_MapPan.x + lx * s_MapZoom,
                                           r0.y + s_MapPan.y + ly * s_MapZoom);
                            ImVec2 rpx = SafeZoneMap::RadiusToPixelAxes(
                                FConfiguration::CustomSafeZoneRadius, S, map);
                            rpx.x *= s_MapZoom;
                            rpx.y *= s_MapZoom;
                            // Storm shading: everything outside the safe circle is purple (~0.5 alpha).
                            SafeZoneMap::FillOutsideEllipse(dl, r0, r1, c, rpx, IM_COL32(140, 40, 200, 128));
                            SafeZoneMap::DrawStormBands(dl, r0, r1, c, rpx, IM_COL32(205, 80, 235, 105));
                            dl->AddEllipseFilled(c, rpx, IM_COL32(90, 160, 255, 40), 0.f, 96);
                            dl->AddEllipse(c, rpx, IM_COL32(130, 200, 255, 230), 0.f, 96, 2.f);
                            // Center marker (dot).
                            dl->AddCircleFilled(c, 4.f, IM_COL32(255, 255, 255, 235));
                            dl->AddCircle(c, 4.f, IM_COL32(20, 30, 60, 200), 0, 1.5f);
                            dl->PopClipRect();

                            ImGui::TextDisabled("Ctrl + wheel to zoom  |  Ctrl + drag to pan  |  %.0f%%", s_MapZoom * 100.f);
                            ImGui::Spacing();
                        }
                        else
                        {
                            if (SafeZoneMap::IsLoadingOrRetrying())
                                ImGui::TextDisabled("Loading map...");
                            else
                                ImGui::TextDisabled("Map image unavailable - set coordinates manually.");
                        }

                        // Numeric readout + fine-tune (always available).
                        float cx = (float)FConfiguration::CustomSafeZoneCenter.X;
                        float cy = (float)FConfiguration::CustomSafeZoneCenter.Y;

                        ImGui::SetNextItemWidth(Width);
                        if (ImGui::InputFloat("Center X", &cx))
                        {
                            SafeZoneMap::ForgetNormalizedSelection();
                            FConfiguration::CustomSafeZoneCenter.X = cx;
                        }
                        ImGui::SetNextItemWidth(Width);
                        if (ImGui::InputFloat("Center Y", &cy))
                        {
                            SafeZoneMap::ForgetNormalizedSelection();
                            FConfiguration::CustomSafeZoneCenter.Y = cy;
                        }

                        float RadiusMeters = FConfiguration::CustomSafeZoneRadius / 100.f;
                        if (LabeledSliderFloat("Radius", "##custom-sz-radius", &RadiusMeters, 5.f, 1000.f, "%.0f m", Width))
                        {
                            FConfiguration::CustomSafeZoneRadius = RadiusMeters * 100.f;
                        }
                    }
                }
            }

            EndSectionBody();

            if (gsStatus < StartedMatch && FConfiguration::bLateGame)
            {
            static char PrimaryWeaponBuffer[256] = { 0 };
            static char SecondaryWeaponBuffer[256] = { 0 };
            static char TertiaryWeaponBuffer[256] = { 0 };
            static char QuaternaryWeaponBuffer[256] = { 0 };
            static char QuinaryWeaponBuffer[256] = { 0 };
            static char TrapsBuffer[256] = { 0 };

            static int PrimaryAmountBuffer = 1;
            static int SecondaryAmountBuffer = 1;
            static int TertiaryAmountBuffer = 1;
            static int QuaternaryAmountBuffer = 1;
            static int QuinaryAmountBuffer = 1;
            static int TrapsAmountBuffer = 6;

            static bool bBuffersInitialized = false;
            static unsigned int LoadoutBufferResetGeneration =
                GPreferenceEditorGeneration;
            static std::string LoadoutStatusMessage;
            static std::chrono::high_resolution_clock::time_point StatusMessageTime;
            static std::string ApplyLoadoutStatusMessage;
            static std::chrono::high_resolution_clock::time_point ApplyStatusMessageTime;

            if (LoadoutBufferResetGeneration !=
                GPreferenceEditorGeneration)
            {
                PrimaryWeaponBuffer[0] = '\0';
                SecondaryWeaponBuffer[0] = '\0';
                TertiaryWeaponBuffer[0] = '\0';
                QuaternaryWeaponBuffer[0] = '\0';
                QuinaryWeaponBuffer[0] = '\0';
                TrapsBuffer[0] = '\0';
                bBuffersInitialized = false;
                LoadoutBufferResetGeneration =
                    GPreferenceEditorGeneration;
            }

            if (FConfiguration::bUseCustomLoadout)
            {
                if (!bBuffersInitialized)
                {
                    strcpy_s(PrimaryWeaponBuffer, TCHAR_TO_UTF8(*FConfiguration::Primary));
                    strcpy_s(SecondaryWeaponBuffer, TCHAR_TO_UTF8(*FConfiguration::Secondary));
                    strcpy_s(TertiaryWeaponBuffer, TCHAR_TO_UTF8(*FConfiguration::Tertiary));
                    strcpy_s(QuaternaryWeaponBuffer, TCHAR_TO_UTF8(*FConfiguration::Quaternary));
                    strcpy_s(QuinaryWeaponBuffer, TCHAR_TO_UTF8(*FConfiguration::Quinary));
                    strcpy_s(TrapsBuffer, TCHAR_TO_UTF8(*FConfiguration::Traps));

                    PrimaryAmountBuffer = FConfiguration::PrimaryAmount;
                    SecondaryAmountBuffer = FConfiguration::SecondaryAmount;
                    TertiaryAmountBuffer = FConfiguration::TertiaryAmount;
                    QuaternaryAmountBuffer = FConfiguration::QuaternaryAmount;
                    QuinaryAmountBuffer = FConfiguration::QuinaryAmount;
                    TrapsAmountBuffer = FConfiguration::TrapsAmount;

                    bBuffersInitialized = true;
                }

                SectionHeader("Custom Loadout Slots", SectionWidth);
                BeginSectionBody();

                ImGui::PushItemWidth(Width);
                ImGui::InputText("Slot 1", PrimaryWeaponBuffer, sizeof(PrimaryWeaponBuffer));
                ImGui::InputInt("Slot 1 Amount", &PrimaryAmountBuffer);

                ImGui::InputText("Slot 2", SecondaryWeaponBuffer, sizeof(SecondaryWeaponBuffer));
                ImGui::InputInt("Slot 2 Amount", &SecondaryAmountBuffer);

                ImGui::InputText("Slot 3", TertiaryWeaponBuffer, sizeof(TertiaryWeaponBuffer));
                ImGui::InputInt("Slot 3 Amount", &TertiaryAmountBuffer);

                ImGui::InputText("Slot 4", QuaternaryWeaponBuffer, sizeof(QuaternaryWeaponBuffer));
                ImGui::InputInt("Slot 4 Amount", &QuaternaryAmountBuffer);

                ImGui::InputText("Slot 5", QuinaryWeaponBuffer, sizeof(QuinaryWeaponBuffer));
                ImGui::InputInt("Slot 5 Amount", &QuinaryAmountBuffer);

                ImGui::InputText("Trap", TrapsBuffer, sizeof(TrapsBuffer));
                ImGui::InputInt("Trap Amount", &TrapsAmountBuffer);
                ImGui::PopItemWidth();

                if (ImGui::Button("Apply Loadout", ImVec2(Width, Height)))
                {
                    FConfiguration::Primary = FString(PrimaryWeaponBuffer);
                    FConfiguration::Secondary = FString(SecondaryWeaponBuffer);
                    FConfiguration::Tertiary = FString(TertiaryWeaponBuffer);
                    FConfiguration::Quaternary = FString(QuaternaryWeaponBuffer);
                    FConfiguration::Quinary = FString(QuinaryWeaponBuffer);
                    FConfiguration::Traps = FString(TrapsBuffer);

                    FConfiguration::PrimaryAmount = PrimaryAmountBuffer;
                    FConfiguration::SecondaryAmount = SecondaryAmountBuffer;
                    FConfiguration::TertiaryAmount = TertiaryAmountBuffer;
                    FConfiguration::QuaternaryAmount = QuaternaryAmountBuffer;
                    FConfiguration::QuinaryAmount = QuinaryAmountBuffer;
                    FConfiguration::TrapsAmount = TrapsAmountBuffer;

                    printf("Saved current loadout.\n");
                    ApplyLoadoutStatusMessage = "Loadout saved successfully!";
                    ApplyStatusMessageTime = std::chrono::high_resolution_clock::now();

                    if (!ApplyLoadoutStatusMessage.empty())
                    {
                        auto Elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now() - StatusMessageTime).count();

                        if (Elapsed < 5)
                        {
                            ImVec4 StatusColor = (ApplyLoadoutStatusMessage.find("Failed.") != std::string::npos) ? ImVec4(1.0f, 0.0f, 0.0f, 1.0f) : ImVec4(0.0f, 1.0f, 0.0f, 1.0f);

                            ImGui::TextColored(StatusColor, "%s", LoadoutStatusMessage.c_str());
                        }
                        else
                        {
                            ApplyLoadoutStatusMessage.clear();
                        }
                    }
                }

                EndSectionBody();

                SectionHeader("Save/Load Loadout", SectionWidth);
                BeginSectionBody();

                if (ImGui::Button("Save Loadout to File", ImVec2(Width, Height)))
                {
                    if (LoadoutManager::SaveLoadout(PrimaryWeaponBuffer, PrimaryAmountBuffer, SecondaryWeaponBuffer, SecondaryAmountBuffer, TertiaryWeaponBuffer, TertiaryAmountBuffer, QuaternaryWeaponBuffer, QuaternaryAmountBuffer, QuinaryWeaponBuffer, QuinaryAmountBuffer, TrapsBuffer, TrapsAmountBuffer))
                    {
                        LoadoutStatusMessage = "Loadout saved successfully!";
                        printf("Loadout saved to: %s\n", LoadoutManager::GetLoadoutFilePath().c_str());
                    }
                    else
                    {
                        LoadoutStatusMessage = "Failed to save loadout!";
                    }

                    StatusMessageTime = std::chrono::high_resolution_clock::now();
                }

                if (ImGui::Button("Load Loadout from File", ImVec2(Width, Height)))
                {
                    if (LoadoutManager::LoadLoadout(PrimaryWeaponBuffer, PrimaryAmountBuffer, SecondaryWeaponBuffer, SecondaryAmountBuffer, TertiaryWeaponBuffer, TertiaryAmountBuffer, QuaternaryWeaponBuffer, QuaternaryAmountBuffer, QuinaryWeaponBuffer, QuinaryAmountBuffer, TrapsBuffer, TrapsAmountBuffer))
                    {
                        LoadoutStatusMessage = "Loadout loaded successfully!";
                        printf("Loadout loaded from: %s\n", LoadoutManager::GetLoadoutFilePath().c_str());
                    }
                    else
                    {
                        LoadoutStatusMessage = "Failed to load loadout! File may not exist.";
                    }

                    StatusMessageTime = std::chrono::high_resolution_clock::now();
                }

                if (!LoadoutStatusMessage.empty())
                {
                    auto Elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now() - StatusMessageTime).count();

                    if (Elapsed < 5)
                    {
                        ImVec4 StatusColor = (LoadoutStatusMessage.find("Failed") != std::string::npos) ? ImVec4(1.0f, 0.0f, 0.0f, 1.0f) : ImVec4(0.0f, 1.0f, 0.0f, 1.0f);

                        ImGui::TextColored(StatusColor, "%s", LoadoutStatusMessage.c_str());
                    }
                    else
                    {
                        LoadoutStatusMessage.clear();
                    }
                }

                EndSectionBody();
            }
            } // gsStatus < StartedMatch

            break;
        }
        case 4:
        {
            SectionHeader("Bot Stats", SectionWidth);
            BeginSectionBody();

            ImGui::PushItemWidth(Width);
            AtomicInputInt(
                "Bot Health",
                FConfiguration::BotHealth);
            AtomicInputInt(
                "Bot Shield",
                FConfiguration::BotShield);
            ImGui::PopItemWidth();

            EndSectionBody();

            SectionHeader("Bot Names", SectionWidth);
            BeginSectionBody();

            AtomicCheckbox(
                "Use Custom Bot Names",
                FConfiguration::UseCustomBotNames);

            if (FConfiguration::UseCustomBotNames)
            {
                static char BotNameBuffer[64] = {};
                static unsigned int BotNameBufferResetGeneration =
                    GPreferenceEditorGeneration;

                if (BotNameBufferResetGeneration !=
                    GPreferenceEditorGeneration)
                {
                    BotNameBuffer[0] = '\0';
                    BotNameBufferResetGeneration =
                        GPreferenceEditorGeneration;
                }

                if (BotNameBuffer[0] == '\0' &&
                    !FConfiguration::BotName.empty())
                    strncpy_s(BotNameBuffer, sizeof(BotNameBuffer), FConfiguration::BotName.c_str(), _TRUNCATE);

                ImGui::PushItemWidth(Width);
                ImGui::InputText("Bot Name", BotNameBuffer, sizeof(BotNameBuffer));
                ImGui::PopItemWidth();

                if (ImGui::Button("Apply Bot Name", ImVec2(Width, Height)))
                {
                    FConfiguration::BotName = BotNameBuffer;
                }
            }

            EndSectionBody();

            break;
        }
        case 5:
        {
            SectionHeader("Creative Plot", SectionWidth);
            BeginSectionBody();

            ImGui::RadioButton("Temperate Island", &SelectedPlot, (int)Plot::Temperate);
            ImGui::RadioButton("Meadow Island", &SelectedPlot, (int)Plot::Meadow);
            ImGui::RadioButton("Arctic Island", &SelectedPlot, (int)Plot::Arctic);
            ImGui::RadioButton("Frosty Fortress", &SelectedPlot, (int)Plot::Fortress);
            ImGui::RadioButton("Ice Lake Island", &SelectedPlot, (int)Plot::IceLake);
            ImGui::RadioButton("Canyon Island", &SelectedPlot, (int)Plot::Canyon);
            ImGui::RadioButton("Arid Island", &SelectedPlot, (int)Plot::Arid);
            ImGui::RadioButton("Wasteland Island", &SelectedPlot, (int)Plot::Wasteland);
            ImGui::RadioButton("Tropical Island", &SelectedPlot, (int)Plot::Tropical);
            ImGui::RadioButton("River Edge Island", &SelectedPlot, (int)Plot::RiverEdge);
            ImGui::RadioButton("Volcano Island", &SelectedPlot, (int)Plot::Volcano);
            ImGui::RadioButton("Sandbar Island", &SelectedPlot, (int)Plot::Sandbar);
            ImGui::RadioButton("Caldera Island", &SelectedPlot, (int)Plot::Caldera);
            ImGui::RadioButton("Kevin Floating Islands", &SelectedPlot, (int)Plot::Kevin);
            ImGui::RadioButton("Black Glass Island", &SelectedPlot, (int)Plot::BlackGlass);
            ImGui::RadioButton("Grid Island", &SelectedPlot, (int)Plot::Grid);
            ImGui::RadioButton("The Block", &SelectedPlot, (int)Plot::Block);
            ImGui::RadioButton("Grassy Hill Island", &SelectedPlot, (int)Plot::GrassyHill);
            ImGui::RadioButton("Shoreline Island", &SelectedPlot, (int)Plot::Shoreline);
            ImGui::RadioButton("Archipelago Island", &SelectedPlot, (int)Plot::Archipelago);
            ImGui::RadioButton("Horseshoe Island", &SelectedPlot, (int)Plot::Horseshoe);
            ImGui::RadioButton("The Shark", &SelectedPlot, (int)Plot::Shark);
            ImGui::RadioButton("Floating Island Hub", &SelectedPlot, (int)Plot::FloatingHub);
            ImGui::RadioButton("Fortilla Island", &SelectedPlot, (int)Plot::Fortilla);
            ImGui::RadioButton("Debris Island", &SelectedPlot, (int)Plot::Debris);
            ImGui::RadioButton("Custom", &SelectedPlot, (int)Plot::Custom);

            EndSectionBody();

            switch (SelectedPlot)
            {
            case (int)Plot::Temperate:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Temperate_Medium.Temperate_Medium";
                break;
            }
            case (int)Plot::Meadow:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/FlatGrass_Large.FlatGrass_Large";
                break;
            }
            case (int)Plot::Arctic:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Arctic_Medium.Arctic_Medium";
                break;
            }
            case (int)Plot::Fortress:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Arctic_Competitive_Medium1.Arctic_Competitive_Medium1";
                break;
            }
            case (int)Plot::IceLake:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/IceLake_Large.IceLake_Large";
                break;
            }
            case (int)Plot::Canyon:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Desert_Large.Desert_Large";
                break;
            }
            case (int)Plot::Arid:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Pandora_Large.Pandora_Large";
                break;
            }
            case (int)Plot::Wasteland:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Desert_Large_02.Desert_Large_02";
                break;
            }
            case (int)Plot::Tropical:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Desert_Large_02.Desert_Large_02";
                break;
            }
            case (int)Plot::RiverEdge:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Military_Medium.Military_Medium";
                break;
            }
            case (int)Plot::Volcano:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Volcano_Large.Volcano_Large";
                break;
            }
            case (int)Plot::Sandbar:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Sandbar_Large.Sandbar_Large";
                break;
            }
            case (int)Plot::Caldera:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Volcano_Large_02.Volcano_Large_02";
                break;
            }
            case (int)Plot::Kevin:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Kevin_Large.Kevin_Large";
                break;
            }
            case (int)Plot::BlackGlass:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/BlackGlass_Medium.BlackGlass_Medium";
                break;
            }
            case (int)Plot::Grid:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/FlatGrid_Large.FlatGrid_Large";
                break;
            }
            case (int)Plot::Block:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/TheBlock_Season7.TheBlock_Season7";
                break;
            }
            case (int)Plot::GrassyHill:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/TheHub_01.TheHub_01";
                break;
            }
            case (int)Plot::Shoreline:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/FlatGrass_LargeV2.FlatGrass_LargeV2";
                break;
            }
            case (int)Plot::Archipelago:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/SandBar_LargeV2.SandBar_LargeV2";
                break;
            }
            case (int)Plot::Horseshoe:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Escape_Large.Escape_Large";
                break;
            }
            case (int)Plot::Shark:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Shark_Large.Shark_Large";
                break;
            }
            case (int)Plot::Fortilla:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Arena_Large_01.Arena_Large_01";
                break;
            }
            case (int)Plot::Debris:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Arena_Large_02.Arena_Large_02";
                break;
            }
            case (int)Plot::Custom:
            {
                break;
            }
            default:
            {
                break;
            }
            }

            break;
        }
        case 6:
        {
            SectionHeader("Custom Map Configuration", SectionWidth);
            BeginSectionBody();

			AtomicCheckbox(
                "One Kill Ends Game",
                FConfiguration::AutoEndGame);

            EndSectionBody();

            SectionHeader("Custom Map", SectionWidth);
            BeginSectionBody();

            ImGui::RadioButton("Athena Faceoff", &SelectedMap, (int)Map::Faceoff);
            ImGui::RadioButton("Papaya (Party Royale)", &SelectedMap, (int)Map::Papaya);
            ImGui::RadioButton("The Combine", &SelectedMap, (int)Map::Crucible);
            ImGui::RadioButton("Flat Grid", &SelectedMap, (int)Map::FlatGrid);
            ImGui::RadioButton("Prop Hunt", &SelectedMap, (int)Map::PropHunt);

            EndSectionBody();

            switch (SelectedMap)
            {
            case (int)Map::Papaya:
            {
                FConfiguration::CustomMap = L"open /Game/Athena/Apollo/Maps/Special/Papaya/Apollo_Papaya";
                break;
            }
            case (int)Map::Crucible:
            {
                FConfiguration::CustomMap = L"open /Game/Athena/Maps/Crucible/Athena_Crucible";
                break;
            }
            case (int)Map::TutorialMap:
            {
                FConfiguration::CustomMap = L"open /Game/Athena/Maps/Tutorial/Athena_Tutorial_Map_A";
                break;
            }
            case (int)Map::EmptyTest:
            {
                FConfiguration::CustomMap = L"open /Game/Athena/Maps/Athena_EmptyTest";
                break;
            }
            case (int)Map::Faceoff:
            {
                FConfiguration::CustomMap = L"open /Game/Athena/Maps/Athena_Faceoff";
                break;
            }
            case (int)Map::Playground:
            {
                FConfiguration::CustomMap = L"open /Game/Athena/Maps/Athena_Playground";
                break;
            }
            case (int)Map::DADBRO:
            {
                FConfiguration::CustomMap = L"open /Game/Athena/Playlists/DADBRO/Athena_DADBRO_Apollo_Island";
                break;
            }
            case (int)Map::Kevin:
            {
                FConfiguration::CustomMap = L"open /Game/Creative/Maps/Islands/105x105/Kevin/Kevin_Floating_Island_105x105_Mesh";
                break;
            }
            case (int)Map::FlatGrid:
            {
                FConfiguration::CustomMap = L"open /Game/Creative/Maps/Islands/105x105/FlatGrid/FlatGrid_Island_105x105";
                break;
            }
            case (int)Map::PropHunt:
            {
                FConfiguration::CustomMap = L"open /Game/Creative/Maps/Islands/105x105/Loki/Loki_Island_105x105_M";
                break;
            }
            default:
            {
                break;
            }
            }

            break;
        }
        case 7:
        {
            SectionHeader("Trickshot Customization", SectionWidth);
            BeginSectionBody();

            const bool bBeforeJoinable =
                gsStatus.load(std::memory_order_acquire) < Joinable;

            AtomicCheckbox(
                "Toggle Swag Lines",
                FConfiguration::bUseWinLines);

            if (VersionInfo.FortniteVersion <= 23.50)
                AtomicCheckbox(
                    "Toggle Infinite Render",
                    FConfiguration::bInfiniteRender);

            if (bBeforeJoinable)
            {
                if ((IsArenaPlaylist() ||
                        IsTournamentPlaylist()) &&
                    FConfiguration::bLateGame &&
                    !FConfiguration::bForceRespawns)
                    AtomicCheckbox(
                        "Randomize Arena Points",
                        FConfiguration::RandomizeArenaPoints);

                AtomicCheckbox(
                    "Player Map Icons",
                    FConfiguration::bPlayerMapIcons);
            }

            AtomicCheckbox(
                "Auto Reload on Waypoint TP",
                FConfiguration::bAutoReloadOnWaypointTP);

            if (VersionInfo.FortniteVersion >= 6.01)
            {
                AtomicCheckbox(
                    "Remove Ice on Waypoint TP",
                    FConfiguration::bRemoveIceOnWaypointTP);
            }

            AtomicCheckbox(
                "Auto God Mode",
                FConfiguration::bAutoGodMode);

            if (FConfiguration::bAutoGodMode)
            {
                ImGui::Indent(12.f);

                static const char* const AutoGodModes[] = {
                    "Maximum",
                    "Minimum"
                };

                ImGui::TextUnformatted("God Mode Type");
                ImGui::SetNextItemWidth(Width);
                AtomicCombo(
                    "##auto-god-mode",
                    FConfiguration::AutoGodModeType,
                    AutoGodModes,
                    IM_ARRAYSIZE(AutoGodModes));

                // The trickshotters join first and the player they are all
                // aiming at joins last, so the newest arrival is the one
                // who has to stay killable.
                if (FConfiguration::bInfiniteRender)
                    AtomicCheckbox(
                        "Exclude Last Player",
                        FConfiguration::
                            bAutoGodModeExcludeLastPlayer);

                ImGui::Unindent(12.f);
            }

            if (bBeforeJoinable)
            {
                if (FConfiguration::bLateGame)
                    AtomicCheckbox(
                        "Randomize Kills",
                        FConfiguration::RandomizeKills);

                AtomicCheckbox(
                    "Randomize Levels",
                    FConfiguration::RandomizeLevels);
            }

            AtomicCheckbox(
                "Disable Jump Fatigue",
                FConfiguration::bDisableJumpFatigue);

            if (bBeforeJoinable &&
                !FConfiguration::bReadyToStart)
            {
                AtomicCheckbox(
                    "Disable Supply Drops",
                    FConfiguration::bDisableSupplyDrops);
            }

            //ImGui::Checkbox("Make Projectiles Rideable (WIP)", &FConfiguration::bRideableProjectiles);

            if (VersionInfo.FortniteVersion >= 19.00)
                AtomicCheckbox(
                    "Toggle Crown Slomo",
                    FConfiguration::bCrownSlomo);

            if (VersionInfo.FortniteVersion >= 23.20 && VersionInfo.FortniteVersion < 25.20)
                AtomicCheckbox(
                    "Negate Velocity on Win",
                    FConfiguration::bCancelVelocityOnWin);

            //ImGui::Checkbox("Down But Not Out (DBNO)", &FConfiguration::bEnableDBNO);

            if (VersionInfo.FortniteVersion >= 8.00)
            {
                AtomicCheckbox(
                    "Cannon Launch Animations",
                    FConfiguration::bCannonLaunchAnimations);

                // The multipliers scale a launch this code applies itself, so
                // they have nothing to act on while native owns the shot.
                if (!FConfiguration::bCannonLaunchAnimations)
                {
                    ImGui::Indent(12.f);

                    AtomicLabeledSliderFloat(
                        "Cannon Launch X",
                        "##cannon-launch-x",
                        FConfiguration::CannonLaunchXMultiplier,
                        0.0f, 5.0f, "%.2fx", Width);
                    AtomicLabeledSliderFloat(
                        "Cannon Launch Y",
                        "##cannon-launch-y",
                        FConfiguration::CannonLaunchYMultiplier,
                        0.0f, 5.0f, "%.2fx", Width);
                    AtomicLabeledSliderFloat(
                        "Cannon Launch Z",
                        "##cannon-launch-z",
                        FConfiguration::CannonLaunchZMultiplier,
                        0.0f, 5.0f, "%.2fx", Width);

                    ImGui::Unindent(12.f);
                }
            }

            if (VersionInfo.FortniteVersion >= 4.30)
            {
                AtomicCheckbox(
                    "Vehicle Bump Launch",
                    FConfiguration::bVehicleBumpLaunch);

                if (FConfiguration::bVehicleBumpLaunch)
                {
                    ImGui::Indent(12.f);

                    AtomicCheckbox(
                        "Bump Damage",
                        FConfiguration::bVehicleBumpDamage);

                    AtomicLabeledSliderFloat(
                        "Bump Minimum Speed",
                        "##vehicle-bump-min-speed",
                        FConfiguration::VehicleBumpMinSpeedKmh,
                        0.0f, 120.0f, "%.0f km/h", Width);

                    AtomicLabeledSliderFloat(
                        "Bump Force Multiplier",
                        "##vehicle-bump-force-multiplier",
                        FConfiguration::VehicleBumpForceMultiplier,
                        0.0f, 10.0f, "%.1fx", Width);

                    ImGui::Unindent(12.f);
                }
            }

            AtomicCheckbox(
                "Auto Pause TODM",
                FConfiguration::bAutoPauseTODM);

            if (FConfiguration::bAutoPauseTODM)
            {
                AtomicLabeledSliderFloat(
                    "Time Of Day",
                    "##time-of-day",
                    FConfiguration::TODMTime,
                    0.f, 24.f, "%.1f h", Width);
            }

            if (FConfiguration::bRideableProjectiles)
            {
                static char ProjClassBuffer[512] = {};

                ImGui::InputText("Class", ProjClassBuffer, IM_ARRAYSIZE(ProjClassBuffer));

                if (ImGui::Button("Apply", ImVec2(Width, Height)))
                {
                    if (ProjClassBuffer[0] != '\0')
                    {
                        std::string ProjClass = ProjClassBuffer;
                        UClass* ProjectileClass = (UClass*)SDK::StaticLoadObject(UEAllocatedWString(ProjClass.begin(), ProjClass.end()).c_str(), SDK::UClass::StaticClass());

                        if (ProjectileClass)
                        {
                            static auto ProjectileBaseClass = FindClass("FortProjectileBase");

                            if (ProjectileClass->IsA(ProjectileBaseClass))
                            {
                                auto DefaultObject = ProjectileClass->GetDefaultObj();

                                if (DefaultObject)
                                {
                                    static auto CapsuleComponentOffset = ProjectileClass->GetOffset("CapsuleComponent");

                                    if (CapsuleComponentOffset != -1)
                                    {
                                        auto CapsuleComponent = GetFromOffset<UPrimitiveComponent*>(DefaultObject, CapsuleComponentOffset);

                                        if (CapsuleComponent)
                                        {
                                            static auto CanCharacterStepUpOnOffset = CapsuleComponent->GetOffset("bCanCharacterStepUpOn");

                                            if (CanCharacterStepUpOnOffset != -1)
                                            {
                                                GetFromOffset<bool>(CapsuleComponent, CanCharacterStepUpOnOffset) = true;
                                            }

                                            static auto WalkableSlopeOverrideOffset = CapsuleComponent->GetOffset("WalkableSlopeOverride");

                                            if (WalkableSlopeOverrideOffset != -1)
                                            {
                                                auto& SlopeOverride = GetFromOffset<FWalkableSlopeOverride>(CapsuleComponent, WalkableSlopeOverrideOffset);
                                                SlopeOverride.WalkableSlopeBehavior = EWalkableSlopeBehavior::WalkableSlope_Increase;
                                                SlopeOverride.WalkableSlopeAngle = 90.0f;
                                            }

                                            static auto CollisionEnabledOffset = CapsuleComponent->GetOffset("CollisionEnabled");

                                            if (CollisionEnabledOffset != -1)
                                            {
                                                GetFromOffset<ECollisionEnabled>(CapsuleComponent, CollisionEnabledOffset) = ECollisionEnabled::QueryOnly;
                                            }

                                            static auto SetCollisionEnabledFunc = CapsuleComponent->Class->GetFunction("SetCollisionEnabled");

                                            if (SetCollisionEnabledFunc)
                                            {
                                                struct { ECollisionEnabled NewType; } Params;
                                                Params.NewType = ECollisionEnabled::QueryOnly;
                                                CapsuleComponent->ProcessEvent(SetCollisionEnabledFunc, &Params);
                                            }

                                            static auto SetCollisionResponseToChannelFunc = CapsuleComponent->Class->GetFunction("SetCollisionResponseToChannel");

                                            if (SetCollisionResponseToChannelFunc)
                                            {
                                                struct { uint8 Channel; ECollisionResponse NewResponse; } Params;
                                                Params.Channel = 1;
                                                Params.NewResponse = ECollisionResponse::ECR_Block;
                                                CapsuleComponent->ProcessEvent(SetCollisionResponseToChannelFunc, &Params);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            EndSectionBody();

            SectionHeader("Trickshot Presets", SectionWidth);
            BeginSectionBody();

            static char TrickshotName[128]{};
            static std::vector<std::string> SavedTrickshots;
            static int SelectedTrickshot = -1;
            static std::string TrickshotMessage;
            static bool InitializedTrickshotList = false;
            static ULONGLONG NextTrickshotListRefreshAtMs = 0;

            const ULONGLONG NowMs = GetTickCount64();
            if (!InitializedTrickshotList ||
                NowMs >= NextTrickshotListRefreshAtMs)
            {
                const std::string SelectedName =
                    SelectedTrickshot >= 0 &&
                    SelectedTrickshot < SavedTrickshots.size()
                        ? SavedTrickshots[SelectedTrickshot]
                        : "";
                bool bRefreshSucceeded = false;
                auto RefreshedTrickshots =
                    TrickshotManager::GetSavedNames(
                        &bRefreshSucceeded);
                if (bRefreshSucceeded &&
                    RefreshedTrickshots != SavedTrickshots)
                {
                    SavedTrickshots =
                        std::move(RefreshedTrickshots);
                    auto Selected = std::find(
                        SavedTrickshots.begin(),
                        SavedTrickshots.end(), SelectedName);
                    SelectedTrickshot =
                        SelectedName.empty() ||
                        Selected == SavedTrickshots.end()
                            ? -1
                            : static_cast<int>(std::distance(
                                SavedTrickshots.begin(), Selected));
                }
                InitializedTrickshotList = true;
                NextTrickshotListRefreshAtMs = NowMs + 500ULL;
            }

            TrickshotManager::EAsyncOperation CompletedOperation =
                TrickshotManager::EAsyncOperation::None;
            bool CompletedSuccessfully = false;
            std::string CompletedName;
            std::string CompletedMessage;
            if (TrickshotManager::ConsumeResult(
                    CompletedOperation,
                    CompletedSuccessfully,
                    CompletedName,
                    CompletedMessage))
            {
                TrickshotMessage = CompletedMessage;
                if (CompletedSuccessfully &&
                    CompletedOperation ==
                        TrickshotManager::EAsyncOperation::Save)
                {
                    const std::string SavedName =
                        TrickshotManager::SanitizeName(
                            CompletedName.c_str());
                    bool bRefreshSucceeded = false;
                    auto RefreshedTrickshots =
                        TrickshotManager::GetSavedNames(
                            &bRefreshSucceeded);
                    if (bRefreshSucceeded)
                        SavedTrickshots =
                            std::move(RefreshedTrickshots);
                    if (std::find(
                            SavedTrickshots.begin(),
                            SavedTrickshots.end(), SavedName) ==
                        SavedTrickshots.end())
                    {
                        SavedTrickshots.push_back(SavedName);
                        std::sort(
                            SavedTrickshots.begin(),
                            SavedTrickshots.end());
                    }
                    auto It = std::find(
                        SavedTrickshots.begin(),
                        SavedTrickshots.end(), SavedName);
                    SelectedTrickshot =
                        It == SavedTrickshots.end()
                            ? -1
                            : static_cast<int>(std::distance(
                                SavedTrickshots.begin(), It));
                }
            }

            ImGui::SetNextItemWidth(Width);
            ImGui::InputTextWithHint("##trickshot-name", "Trickshot Name", TrickshotName, IM_ARRAYSIZE(TrickshotName));

            ImGui::SetNextItemWidth(Width);
            const char* Preview = SelectedTrickshot >= 0 && SelectedTrickshot < SavedTrickshots.size()
                ? SavedTrickshots[SelectedTrickshot].c_str() : "Select Saved Trickshot";
            if (ImGui::BeginCombo("##saved-trickshots", Preview))
            {
                for (int Index = 0; Index < SavedTrickshots.size(); ++Index)
                {
                    const bool Selected = Index == SelectedTrickshot;
                    if (ImGui::Selectable(SavedTrickshots[Index].c_str(), Selected))
                        SelectedTrickshot = Index;
                    if (Selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            const float ButtonWidth = (Width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (ImGui::Button("Save", ImVec2(ButtonWidth, Height)))
            {
                TrickshotManager::RequestSave(
                    TrickshotName, TrickshotMessage);
            }
            ImGui::SameLine();
            if (ImGui::Button("Load", ImVec2(ButtonWidth, Height)))
            {
                TrickshotManager::RequestLoad(
                    SelectedTrickshot >= 0 &&
                        SelectedTrickshot < SavedTrickshots.size()
                        ? SavedTrickshots[SelectedTrickshot]
                        : "",
                    TrickshotMessage);
            }

            if (ImGui::Button("Delete", ImVec2(ButtonWidth, Height)))
            {
                if (TrickshotManager::IsBusy())
                {
                    TrickshotMessage =
                        "Wait for the current trickshot operation to finish.";
                }
                else
                {
                    const std::string SelectedName =
                        SelectedTrickshot >= 0 &&
                            SelectedTrickshot < SavedTrickshots.size()
                            ? SavedTrickshots[SelectedTrickshot]
                            : "";
                    if (TrickshotManager::Delete(
                            SelectedName, TrickshotMessage))
                    {
                        SavedTrickshots.erase(
                            std::remove(
                                SavedTrickshots.begin(),
                                SavedTrickshots.end(), SelectedName),
                            SavedTrickshots.end());
                        bool bRefreshSucceeded = false;
                        auto RefreshedTrickshots =
                            TrickshotManager::GetSavedNames(
                                &bRefreshSucceeded);
                        if (bRefreshSucceeded)
                            SavedTrickshots =
                                std::move(RefreshedTrickshots);
                        SelectedTrickshot = SavedTrickshots.empty()
                            ? -1
                            : (std::min)(
                                SelectedTrickshot,
                                static_cast<int>(
                                    SavedTrickshots.size()) - 1);
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Open Folder", ImVec2(ButtonWidth, Height)))
                TrickshotManager::OpenDirectory(TrickshotMessage);

            if (!TrickshotMessage.empty())
                ImGui::TextWrapped("%s", TrickshotMessage.c_str());

            AtomicCheckbox(
                "Save and Track Spawned Objects",
                FConfiguration::bSaveAndTrackSpawnedObjects);
            AtomicCheckbox(
                "Save Waypoints",
                FConfiguration::bSaveWaypoints);

            EndSectionBody();

            break;
        }
        case 8:
        {
            auto rule = []()
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float w = ImGui::GetContentRegionAvail().x;
                dl->AddLine(ImVec2(p.x, p.y + 1.f), ImVec2(p.x + w, p.y + 1.f), ImGui::GetColorU32(Accent(0.30f)), 1.f);
                ImGui::Dummy(ImVec2(0.f, 9.f));
            };
            auto credit = [](const char* name, const char* role, const char* url)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, Accent(0.95f));
                ImGui::TextUnformatted(name);
                ImGui::PopStyleColor();
                if (url && url[0])
                {
                    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (ImGui::IsItemClicked()) ShellExecuteA(0, "open", url, 0, 0, SW_SHOWNORMAL);
                }
                ImGui::SameLine(0.f, 8.f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.60f, 0.66f, 1.f));
                ImGui::Text("- %s", role);
                ImGui::PopStyleColor();
                ImGui::Spacing();
            };

            // Header: logo + title
            if (g_LogoTexture)
            {
                ImGui::Image((void*)g_LogoTexture, ImVec2(44.f, 44.f));
                ImGui::SameLine(0.f, 12.f);
            }
            ImGui::BeginGroup();
            ImGui::PushStyleColor(ImGuiCol_Text, Accent());
            ImGui::TextUnformatted("MAGNESIUM");
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.54f, 0.56f, 0.62f, 1.f));
            ImGui::TextUnformatted("Gameserver  -  v2.5.0");
            ImGui::PopStyleColor();
            ImGui::EndGroup();

            ImGui::Dummy(ImVec2(0.f, 14.f));

            ImGui::PushStyleColor(ImGuiCol_Text, Accent(0.85f));
            ImGui::TextUnformatted("CREDITS");
            ImGui::PopStyleColor();
            rule();

            // Core attribution is part of Magnesium itself, so keep it
            // independent of every optional feature, playlist and runtime
            // integration used to decide which contributor credits apply.
            struct CoreCredit
            {
                const char* Name;
                const char* Role;
                const char* Url;
            };
            static constexpr CoreCredit CoreCredits[] = {
                {
                    "Erbium",
                    "Base of the project",
                    "https://github.com/plooshi/Erbium"
                },
                {
                    "Core",
                    "Feature inspiration and references",
                    "https://github.com/PongooDev/Core"
                },
            };
            for (const CoreCredit& Entry : CoreCredits)
                credit(Entry.Name, Entry.Role, Entry.Url);

            const bool anyContrib = FConfiguration::bInfiniteRender
                || SelectedPlaylist == static_cast<int>(Playlist::Gav)
                || SelectedPlaylist == static_cast<int>(Playlist::Retrac1v1)
                || SelectedPlaylist == static_cast<int>(Playlist::RetracTurtle)
                || SelectedPlaylist == static_cast<int>(Playlist::OnlyUp)
                || SelectedPlaylist == static_cast<int>(Playlist::TiltedZW);

            if (anyContrib)
            {
                ImGui::Dummy(ImVec2(0.f, 10.f));
                ImGui::PushStyleColor(ImGuiCol_Text, Accent(0.85f));
                ImGui::TextUnformatted("CONTRIBUTORS");
                ImGui::PopStyleColor();
                rule();

                if (FConfiguration::bInfiniteRender)
                    credit("Sweefy / Milxnor", "Infinite Render research", "https://x.com/Sweefyyy");
                if (SelectedPlaylist == static_cast<int>(Playlist::Gav))
                    credit("Gav", "Maker of the 27.11 1v1 map", "https://github.com/gavbowersdomain/27.11-Mods/tree/main/Mods/1v1");
                if (SelectedPlaylist == static_cast<int>(Playlist::Retrac1v1) || SelectedPlaylist == static_cast<int>(Playlist::RetracTurtle))
                    credit("Retrac", "Creator of the 1v1 & Turtle Fight maps", "https://discord.gg/retrac");
                if (SelectedPlaylist == static_cast<int>(Playlist::OnlyUp) || SelectedPlaylist == static_cast<int>(Playlist::TiltedZW))
                    credit("Jett", "Maker of the Only Up & Tilted FFA maps", "https://discord.com/channels/1469866169635962884/1473850399994806362/1473850399994806362");
            }

            ImGui::Dummy(ImVec2(0.f, 18.f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.44f, 0.50f, 1.f));
            ImGui::TextUnformatted("Thank you for using Magnesium.");
            ImGui::PopStyleColor();

            break;
        }
        case 9:
        {
            const Calendar::FSnowVersionModel SnowModel =
                Calendar::GetSnowVersionModel();
            const bool bSnowPhases =
                SnowModel.Model == Calendar::ESnowValueModel::Phase;

            SectionHeader("Snow", SectionWidth);
            BeginSectionBody();

            if (SnowModel.Model == Calendar::ESnowValueModel::Unsupported)
            {
                ImGui::TextWrapped(
                    "This build has no snow setup to drive.");
            }
            else
            {
                ImGui::PushStyleColor(
                    ImGuiCol_Text,
                    ImVec4(0.58f, 0.60f, 0.66f, 1.f));
                ImGui::TextWrapped("%s", SnowModel.Note);
                ImGui::PopStyleColor();
                ImGui::Spacing();

                float SnowValue =
                    FConfiguration::SnowValue.load(
                        std::memory_order_acquire);

                if (bSnowPhases)
                {
                    int SnowPhase = (int)SnowValue;
                    if (LabeledSliderInt(
                            "Snow Phase",
                            "##snow-value",
                            &SnowPhase,
                            (int)SnowModel.Min,
                            (int)SnowModel.Max,
                            Width))
                    {
                        FConfiguration::SnowValue.store(
                            (float)SnowPhase,
                            std::memory_order_release);
                    }
                }
                else if (LabeledSliderFloat(
                             "Snow Coverage",
                             "##snow-value",
                             &SnowValue,
                             SnowModel.Min,
                             SnowModel.Max,
                             "%.3f",
                             Width))
                {
                    FConfiguration::SnowValue.store(
                        SnowValue, std::memory_order_release);
                }

                const float PresetWidth =
                    (Width -
                        ImGui::GetStyle().ItemSpacing.x *
                            (SnowModel.PresetCount - 1)) /
                    SnowModel.PresetCount;

                for (int Index = 0;
                    Index < SnowModel.PresetCount;
                    ++Index)
                {
                    const Calendar::FSnowPreset& Preset =
                        SnowModel.Presets[Index];

                    if (Index > 0)
                        ImGui::SameLine();

                    ImGui::PushID(Index);
                    if (ImGui::Button(
                            Preset.Label,
                            ImVec2(PresetWidth, Height)))
                    {
                        FConfiguration::SnowValue.store(
                            Preset.Value,
                            std::memory_order_release);
                    }
                    ImGui::PopID();
                }

                AtomicCheckbox(
                    "Apply On Bus Start",
                    FConfiguration::bSnowOnMatchStart);

                if (gsStatus >= Joinable)
                {
                    if (ImGui::Button(
                            "Apply Now", ImVec2(Width, Height)))
                    {
                        Calendar::RequestSnow(
                            FConfiguration::SnowValue.load(
                                std::memory_order_acquire));
                    }
                }
                else
                {
                    ImGui::TextWrapped(
                        "Start the server to change snow live.");
                }

                switch (Calendar::GetSnowStatus())
                {
                case Calendar::ESnowStatus::Pending:
                {
                    ImGui::TextUnformatted("- Applying...");
                    break;
                }
                case Calendar::ESnowStatus::Applied:
                {
                    if (bSnowPhases)
                        ImGui::Text(
                            "- Applied phase %d.",
                            (int)Calendar::GetAppliedSnowValue());
                    else
                        ImGui::Text(
                            "- Applied %.3f.",
                            Calendar::GetAppliedSnowValue());
                    break;
                }
                case Calendar::ESnowStatus::SetupMissing:
                {
                    ImGui::TextWrapped(
                        "- No snow setup was found in this map.");
                    break;
                }
                default:
                {
                    break;
                }
                }
            }

            EndSectionBody();

            break;
        }
        default:
        {
            break;
        }
        }

        // Render the loadout modal every frame, even when the inspected pawn
        // vanished or another tab was selected, so ImGui cannot retain an
        // invisible modal that blocks the rest of the interface.
        PlayerLoadout::RenderPicker();

        AutoHosting::SaveIfChanged();

        ImGui::Dummy(ImVec2(0.0f, 40.0f));
        ImGui::EndChild();      // content panel
        ImGui::PopStyleVar();   // content WindowPadding
        ImGui::PopStyleColor(); // content ChildBg
        ImGui::End();           // window


        ImGui::Render();
        const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    // GUI close ends the entire host process below, so there is no later
    // destructor/atexit opportunity to flush the toggle or delay.
    AutoHosting::SaveNow(false);

    PlayerLoadout::ShutdownRenderer();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (g_EmbedTexture)
        g_EmbedTexture->Release();

    g_pSwapChain->Release();
    g_pd3dDeviceContext->Release();
    g_pd3dDevice->Release();
    DestroyWindow(hWnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);
    TerminateProcess(GetCurrentProcess(), 0);
}
