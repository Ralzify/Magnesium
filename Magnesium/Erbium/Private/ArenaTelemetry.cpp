#include "pch.h"
#include "../Public/ArenaTelemetry.h"
#include "ArenaTelemetryPolicy.h"
#include "ArenaTelemetryWire.h"

#include "../Public/GUI.h"
#include "../../Engine/Public/NetDriver.h"
#include "../../FortniteGame/Public/FortGameMode.h"
#include "../../FortniteGame/Public/FortGameStateAthena.h"
#include "../../FortniteGame/Public/FortPlayerControllerAthena.h"
#include "../../FortniteGame/Public/FortPlayerStateAthena.h"
#include "../../FortniteGame/Public/FortPlaylistAthena.h"
#include "../../json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ArenaTelemetry
{
    namespace
    {
        constexpr int kSchemaVersion = 1;
        constexpr int kEliminationPoints = 20;
        constexpr size_t kQueueCapacity = 2048;
        constexpr size_t kMaximumPendingMessages = 8192;
        constexpr size_t kMaximumPendingAbortMessages = 256;
        constexpr size_t kMaximumParticipants = 256;
        constexpr size_t kMaximumBatchEvents = 64;
        constexpr size_t kMaximumPendingPresentationEvents = 64;
        // The SPSC ring keeps one slot empty.  Leave room for a full 256-player
        // session without stranding the last participant as "queued".
        constexpr size_t kTournamentNotificationQueueCapacity = 512;
        constexpr DWORD kIdleWorkerSleepMs = 100;
        constexpr DWORD kBusyWorkerSleepMs = 25;
        constexpr ULONGLONG kIdentityRetryIntervalMs = 250;
        constexpr ULONGLONG kCapturePollIntervalMs = 1000;
        constexpr ULONGLONG kMaximumRetryDelayMs = 10000;
        constexpr uint64_t kCastClassStrProperty = 0x4000;
        constexpr uint64_t kCastClassNameProperty = 0x2000;
        constexpr uint64_t kCastClassIntProperty = 0x80;
        constexpr uint64_t kCastClassBoolProperty = 0x20000;
        constexpr uint64_t kCastClassStructProperty = 0x100000;
        constexpr char kDefaultBackendUrl[] = "http://127.0.0.1:3551";

        // Release globally elides SDK::DbgLog because that logger opens and
        // flushes a file at more than a thousand call sites. Arena visual
        // diagnosis needs only a tiny number of lines, so write those directly
        // to the process stdout handle that ATLAS Link already captures (or
        // Fortnite's stdout.log after its normal redirect). The lifetime cap
        // prevents an unexpected event loop from adding game-thread I/O.
        constexpr uint32_t kMaximumArenaVisualDiagnostics = 64;
        std::atomic<uint32_t> GArenaVisualDiagnosticCount{ 0 };

        void WriteArenaVisualDiagnostic(
            const char* Format, ...) noexcept
        {
            if (!Format ||
                GArenaVisualDiagnosticCount.fetch_add(
                    1, std::memory_order_relaxed) >=
                    kMaximumArenaVisualDiagnostics)
            {
                return;
            }

            char Buffer[1024]{};
            va_list Arguments;
            va_start(Arguments, Format);
            _vsnprintf_s(
                Buffer, sizeof(Buffer), _TRUNCATE,
                Format, Arguments);
            va_end(Arguments);

            size_t Length = strnlen_s(Buffer, sizeof(Buffer));
            if (Length == 0)
                return;
            if (Buffer[Length - 1] != '\n' &&
                Length + 1 < sizeof(Buffer))
            {
                Buffer[Length++] = '\n';
                Buffer[Length] = '\0';
            }

            bool Written = false;
            const HANDLE Stdout = GetStdHandle(STD_OUTPUT_HANDLE);
            if (Stdout && Stdout != INVALID_HANDLE_VALUE)
            {
                DWORD BytesWritten = 0;
                Written = WriteFile(
                    Stdout, Buffer, static_cast<DWORD>(Length),
                    &BytesWritten, nullptr) != FALSE &&
                    BytesWritten == Length;
            }
            if (!Written && stdout)
            {
                fwrite(Buffer, 1, Length, stdout);
                fflush(stdout);
            }
            OutputDebugStringA(Buffer);
        }

        enum class EMessageKind : uint8_t
        {
            Start,
            Event,
            End
        };

        struct FWireEvent
        {
            char Id[192]{};
            char Type[32]{};
            uint64_t Sequence = 0;
            uint64_t OccurredAtMs = 0;
            uint64_t CaptureRevision = 0;

            char AccountId[256]{};
            char DisplayName[256]{};
            char KillerAccountId[256]{};
            char VictimAccountId[256]{};

            int TeamIndex = 0;
            int Kills = 0;
            int Placement = 0;
            int PlayersRemaining = 0;
            int PointsDelta = 0;

            bool HasAccount = false;
            bool HasTeamIndex = false;
            bool HasKillerAccount = false;
            bool HasVictimAccount = false;
            bool HasKills = false;
            bool HasPlacement = false;
            bool HasPlayersRemaining = false;
            bool HasPointsDelta = false;
        };

        struct FQueuedMessage
        {
            EMessageKind Kind = EMessageKind::Event;
            char ServerInstanceId[64]{};
            char SessionId[64]{};
            char Playlist[192]{};
            double FortniteVersion = 0.0;
            FWireEvent Event{};
        };

        struct FPendingArenaPresentationEvent
        {
            ArenaTelemetryWire::EArenaPresentationEventKind Kind =
                ArenaTelemetryWire::EArenaPresentationEventKind::Elimination;
            int32 PointsDelta = 0;
            int32 Placement = 0;
            bool NativeGraceExpected = false;
        };

        struct FParticipant
        {
            AFortPlayerControllerAthena* Controller = nullptr;
            AFortPlayerStateAthena* PlayerState = nullptr;
            char AccountId[256]{};
            char DisplayName[256]{};
            int TeamIndex = 0;
            bool TeamResolved = false;
            ULONGLONG NextIdentityRetryAtMs = 0;
            ULONGLONG NextTournamentLookupAtMs = 0;
            ULONGLONG NextTournamentNotificationAtMs = 0;
            ULONGLONG TournamentNotificationWaitStartedAtMs = 0;
            bool JoinQueued = false;
            bool TournamentLookupQueued = false;
            bool TournamentNotificationSent = false;
            bool TournamentPresentationFallbackOwned = false;
            bool TournamentIdentityCached = false;
            bool TournamentReadinessObserved = false;
            uint64_t TournamentLookupRequestId = 0;
            uint64_t TournamentCaptureGeneration = 0;
            ULONGLONG TournamentReadinessStableSinceMs = 0;
            int32 TournamentSavedHype = 0;
            int32 TournamentEntryFee = 0;
            int32 TournamentLocalHype = 0;
            bool TournamentEntryFeeKnown = false;
            bool TournamentLocalStateInitialized = false;
            bool TournamentBootstrapSent = false;
            bool TournamentEndSent = false;
            bool TournamentSavingEnabled = false;
            // The aircraft callback is only evidence that a native fare may
            // have presented. Correct local Hype ordering never depends on it.
            bool TournamentEntryFeeVisualRequested = false;
            // Set only after the private fare/control event was dispatched.
            // Zero-fare sessions still send B:0 at the aircraft boundary so
            // the client can resolve native-versus-fallback HUD ownership.
            bool TournamentEntryFeeStageComplete = false;
            ArenaTelemetryPolicy::FTournamentIdentity TournamentIdentity{};
            std::array<FPendingArenaPresentationEvent,
                kMaximumPendingPresentationEvents>
                PendingPresentationEvents{};
            size_t PendingPresentationEventCount = 0;
        };

        // These records cross the existing game-thread/worker-thread boundary
        // only.  They contain identity and tournament display data, never
        // score mutations or match facts.
        struct FTournamentLookup
        {
            AFortPlayerControllerAthena* Controller = nullptr;
            AFortPlayerStateAthena* PlayerState = nullptr;
            uint64_t SessionOrdinal = 0;
            uint64_t RequestId = 0;
            uint64_t CaptureGeneration = 0;
            double FortniteVersion = 0.0;
            bool SavedProgressionEnabled = false;
            char AccountId[256]{};
        };

        struct FTournamentDelivery
        {
            AFortPlayerControllerAthena* Controller = nullptr;
            AFortPlayerStateAthena* PlayerState = nullptr;
            uint64_t SessionOrdinal = 0;
            uint64_t RequestId = 0;
            uint64_t CaptureGeneration = 0;
            bool Resolved = false;
            int32 SavedHype = 0;
            int32 ArenaEntryFee = 0;
            bool HasArenaEntryFee = false;
            ArenaTelemetryPolicy::FTournamentIdentity Identity{};
        };

        struct FGameThreadSession
        {
            AFortGameMode* GameMode = nullptr;
            AFortGameStateAthena* GameState = nullptr;
            char SessionId[64]{};
            char Playlist[192]{};
            uint64_t NextSequence = 0;
            uint64_t NextPresentationSequence = 0;
            uint64_t Ordinal = 0;
            ArenaTelemetryPolicy::FPlacementProgressionState
                PlacementProgression{};
            std::unordered_set<std::uint64_t> CreditedVictimLives;
            std::vector<FParticipant> Participants;
            bool Active = false;
            bool StartQueued = false;
            bool Ending = false;
            bool Corrupted = false;
            bool TournamentNotificationUnavailable = false;
            bool TournamentCaptureStateInitialized = false;
            bool TournamentCaptureEnabled = false;
            uint64_t TournamentCaptureGeneration = 0;
        };

        struct FPendingMatchStart
        {
            AFortGameMode* GameMode = nullptr;
            AFortGameStateAthena* GameState = nullptr;
            bool Pending = false;
        };

        std::array<FQueuedMessage, kQueueCapacity> GQueue{};
        std::atomic<uint32_t> GQueueWrite{ 0 };
        std::atomic<uint32_t> GQueueRead{ 0 };
        std::array<FTournamentLookup, kTournamentNotificationQueueCapacity>
            GTournamentLookupQueue{};
        std::atomic<uint32_t> GTournamentLookupWrite{ 0 };
        std::atomic<uint32_t> GTournamentLookupRead{ 0 };
        std::array<FTournamentDelivery, kTournamentNotificationQueueCapacity>
            GTournamentDeliveryQueue{};
        std::atomic<uint32_t> GTournamentDeliveryWrite{ 0 };
        std::atomic<uint32_t> GTournamentDeliveryRead{ 0 };
        std::atomic_bool GWorkerStarted{ false };
        std::atomic_bool GCaptureEnabled{ false };
        std::atomic<uint64_t> GCaptureRevision{ 0 };
        std::atomic<uint64_t> GCaptureStateGeneration{ 0 };
        std::atomic<uint64_t> GLastDroppedTournamentDeliveryRequestId{ 0 };
        std::atomic<uint32_t> GDroppedMessages{ 0 };

        FGameThreadSession GSession{};
        FPendingMatchStart GPendingMatchStart{};
        char GServerInstanceId[64]{};
        DWORD GGameThreadId = 0;
        uint64_t GNextSessionOrdinal = 0;
        uint64_t GNextTournamentLookupRequestId = 0;
        uint64_t GObservedDroppedTournamentDeliveryRequestId = 0;

        void ServiceArenaPresentation(
            FParticipant& Participant,
            bool AllowEnding = false) noexcept;

        template <size_t Size>
        void CopyText(char (&Destination)[Size], const char* Source) noexcept
        {
            Destination[0] = '\0';
            if (!Source || !*Source)
                return;
            strncpy_s(Destination, Size, Source, _TRUNCATE);
        }

        template <size_t Size>
        void CopyText(
            char (&Destination)[Size],
            const std::string& Source) noexcept
        {
            CopyText(Destination, Source.c_str());
        }

        uint64_t UnixTimeMilliseconds() noexcept
        {
            FILETIME Time{};
            GetSystemTimeAsFileTime(&Time);
            ULARGE_INTEGER Value{};
            Value.LowPart = Time.dwLowDateTime;
            Value.HighPart = Time.dwHighDateTime;
            constexpr uint64_t WindowsToUnixEpoch =
                116444736000000000ULL;
            return Value.QuadPart > WindowsToUnixEpoch
                ? (Value.QuadPart - WindowsToUnixEpoch) / 10000ULL
                : 0ULL;
        }

        uint64_t ProcessCreationTicks() noexcept
        {
            FILETIME Creation{}, Exit{}, Kernel{}, User{};
            if (!GetProcessTimes(
                    GetCurrentProcess(),
                    &Creation, &Exit, &Kernel, &User))
            {
                return UnixTimeMilliseconds();
            }
            ULARGE_INTEGER Value{};
            Value.LowPart = Creation.dwLowDateTime;
            Value.HighPart = Creation.dwHighDateTime;
            return Value.QuadPart;
        }

        void EnsureServerInstanceId() noexcept
        {
            if (GServerInstanceId[0])
                return;
            sprintf_s(
                GServerInstanceId,
                "mg-%08lx-%016llx",
                static_cast<unsigned long>(GetCurrentProcessId()),
                static_cast<unsigned long long>(ProcessCreationTicks()));
        }

        bool IsGameThreadCall() noexcept
        {
            const DWORD Current = GetCurrentThreadId();
            if (!GGameThreadId)
                GGameThreadId = Current;
            if (GGameThreadId == Current)
                return true;

            SDK::DbgLog(
                "[ArenaTelemetry] rejected non-game-thread producer call current=%lu expected=%lu\n",
                Current, GGameThreadId);
            return false;
        }

        void NoteDroppedMessage(const char* Type) noexcept
        {
            const uint32_t Dropped =
                GDroppedMessages.fetch_add(
                    1, std::memory_order_relaxed) + 1;
            if (Dropped == 1 || (Dropped % 64) == 0)
            {
                SDK::DbgLog(
                    "[ArenaTelemetry] bounded queue full; dropped type=%s total=%u\n",
                    Type ? Type : "unknown", Dropped);
            }
        }

        bool TryPushMessage(const FQueuedMessage& Message) noexcept
        {
            const uint32_t Write =
                GQueueWrite.load(std::memory_order_relaxed);
            const uint32_t Next =
                (Write + 1) % static_cast<uint32_t>(kQueueCapacity);
            if (Next == GQueueRead.load(std::memory_order_acquire))
                return false;

            GQueue[Write] = Message;
            GQueueWrite.store(Next, std::memory_order_release);
            return true;
        }

        bool TryPopMessage(FQueuedMessage& Message) noexcept
        {
            const uint32_t Read =
                GQueueRead.load(std::memory_order_relaxed);
            if (Read == GQueueWrite.load(std::memory_order_acquire))
                return false;

            Message = GQueue[Read];
            GQueueRead.store(
                (Read + 1) % static_cast<uint32_t>(kQueueCapacity),
                std::memory_order_release);
            return true;
        }

        template <typename T, size_t Capacity>
        bool TryPushSpsc(
            std::array<T, Capacity>& Queue,
            std::atomic<uint32_t>& WriteIndex,
            std::atomic<uint32_t>& ReadIndex,
            const T& Value) noexcept
        {
            const uint32_t Write =
                WriteIndex.load(std::memory_order_relaxed);
            const uint32_t Next =
                (Write + 1) % static_cast<uint32_t>(Capacity);
            if (Next == ReadIndex.load(std::memory_order_acquire))
                return false;
            Queue[Write] = Value;
            WriteIndex.store(Next, std::memory_order_release);
            return true;
        }

        template <typename T, size_t Capacity>
        bool TryPopSpsc(
            std::array<T, Capacity>& Queue,
            std::atomic<uint32_t>& WriteIndex,
            std::atomic<uint32_t>& ReadIndex,
            T& Value) noexcept
        {
            const uint32_t Read =
                ReadIndex.load(std::memory_order_relaxed);
            if (Read == WriteIndex.load(std::memory_order_acquire))
                return false;
            Value = Queue[Read];
            ReadIndex.store(
                (Read + 1) % static_cast<uint32_t>(Capacity),
                std::memory_order_release);
            return true;
        }

        std::string ReadEnvironment(
            const char* Name,
            const char* Fallback = nullptr)
        {
            const DWORD Required =
                GetEnvironmentVariableA(Name, nullptr, 0);
            if (!Required || Required > 32768)
                return Fallback ? Fallback : "";

            std::vector<char> Value(Required);
            const DWORD Written = GetEnvironmentVariableA(
                Name, Value.data(), Required);
            if (!Written || Written >= Required)
                return Fallback ? Fallback : "";
            return std::string(Value.data(), Written);
        }

        void Trim(std::string& Value)
        {
            const auto IsSpace = [](unsigned char Character)
            {
                return Character == ' ' || Character == '\t' ||
                    Character == '\r' || Character == '\n';
            };
            while (!Value.empty() &&
                   IsSpace(static_cast<unsigned char>(Value.front())))
            {
                Value.erase(Value.begin());
            }
            while (!Value.empty() &&
                   IsSpace(static_cast<unsigned char>(Value.back())))
            {
                Value.pop_back();
            }
        }

        std::string ResolveBackendUrl()
        {
            std::string Url = ReadEnvironment(
                "ATLAS_BACKEND_URL", kDefaultBackendUrl);
            Trim(Url);
            const bool HasSupportedScheme =
                Url.rfind("http://", 0) == 0 ||
                Url.rfind("https://", 0) == 0;
            if (!HasSupportedScheme || Url.size() > 4096)
            {
                SDK::DbgLog(
                    "[ArenaTelemetry] invalid ATLAS_BACKEND_URL; using loopback fallback\n");
                Url = kDefaultBackendUrl;
            }
            while (!Url.empty() && Url.back() == '/')
                Url.pop_back();
            return Url;
        }

        size_t CaptureHttpResponse(
            char* Data,
            size_t ElementSize,
            size_t ElementCount,
            void* UserData)
        {
            const size_t Bytes = ElementSize * ElementCount;
            if (!UserData || !Data || !Bytes)
                return Bytes;
            auto& Response = *static_cast<std::string*>(UserData);
            if (Response.size() + Bytes > 1024 * 1024)
                return 0;
            Response.append(Data, Bytes);
            return Bytes;
        }

        struct FHttpResponse
        {
            bool Success = false;
            long Status = 0;
            std::string Body;
        };

        FHttpResponse SendHttp(
            const std::string& BaseUrl,
            const std::string& Token,
            const char* Path,
            const char* Method,
            const std::string& Body,
            const char* UserAgent = "Magnesium-Arena/1")
        {
            FHttpResponse Result{};
            CURL* Curl = curl_easy_init();
            if (!Curl)
                return Result;

            const std::string Url = BaseUrl + Path;
            curl_slist* Headers = nullptr;
            Headers = curl_slist_append(
                Headers, "Accept: application/json");
            if (strcmp(Method, "POST") == 0)
            {
                Headers = curl_slist_append(
                    Headers, "Content-Type: application/json");
            }
            std::string TokenHeader;
            if (!Token.empty())
            {
                TokenHeader = "X-ATLAS-Arena-Token: " + Token;
                Headers = curl_slist_append(
                    Headers, TokenHeader.c_str());
            }

            curl_easy_setopt(Curl, CURLOPT_URL, Url.c_str());
            curl_easy_setopt(Curl, CURLOPT_HTTPHEADER, Headers);
            curl_easy_setopt(Curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(Curl, CURLOPT_CONNECTTIMEOUT_MS, 750L);
            curl_easy_setopt(Curl, CURLOPT_TIMEOUT_MS, 2000L);
            curl_easy_setopt(Curl, CURLOPT_WRITEFUNCTION, CaptureHttpResponse);
            curl_easy_setopt(Curl, CURLOPT_WRITEDATA, &Result.Body);
            curl_easy_setopt(Curl, CURLOPT_USERAGENT, UserAgent);
            curl_easy_setopt(Curl, CURLOPT_TCP_KEEPALIVE, 1L);
            curl_easy_setopt(Curl, CURLOPT_FOLLOWLOCATION, 0L);

            if (BaseUrl.rfind("http://127.0.0.1", 0) == 0 ||
                BaseUrl.rfind("http://localhost", 0) == 0 ||
                BaseUrl.rfind("http://[::1]", 0) == 0 ||
                BaseUrl.rfind("https://127.0.0.1", 0) == 0 ||
                BaseUrl.rfind("https://localhost", 0) == 0 ||
                BaseUrl.rfind("https://[::1]", 0) == 0)
            {
                curl_easy_setopt(
                    Curl, CURLOPT_NOPROXY, "127.0.0.1,localhost,::1");
            }

            if (strcmp(Method, "POST") == 0)
            {
                curl_easy_setopt(Curl, CURLOPT_POST, 1L);
                curl_easy_setopt(Curl, CURLOPT_POSTFIELDS, Body.data());
                curl_easy_setopt(
                    Curl,
                    CURLOPT_POSTFIELDSIZE_LARGE,
                    static_cast<curl_off_t>(Body.size()));
            }

            const CURLcode Code = curl_easy_perform(Curl);
            if (Code == CURLE_OK)
            {
                curl_easy_getinfo(
                    Curl, CURLINFO_RESPONSE_CODE, &Result.Status);
                Result.Success =
                    Result.Status >= 200 && Result.Status < 300;
            }

            curl_slist_free_all(Headers);
            curl_easy_cleanup(Curl);
            return Result;
        }

        std::string EscapePathComponent(const char* Source)
        {
            static constexpr char Hex[] = "0123456789ABCDEF";
            std::string Result;
            if (!Source)
                return Result;
            for (const unsigned char* It =
                     reinterpret_cast<const unsigned char*>(Source);
                 *It;
                 ++It)
            {
                const unsigned char Character = *It;
                if ((Character >= 'A' && Character <= 'Z') ||
                    (Character >= 'a' && Character <= 'z') ||
                    (Character >= '0' && Character <= '9') ||
                    Character == '-' || Character == '_' ||
                    Character == '.')
                {
                    Result.push_back(static_cast<char>(Character));
                }
                else
                {
                    Result.push_back('%');
                    Result.push_back(Hex[(Character >> 4) & 0xF]);
                    Result.push_back(Hex[Character & 0xF]);
                }
            }
            return Result;
        }

        bool ResolveTournamentIdentity(
            const std::string& Body,
            double FortniteVersion,
            ArenaTelemetryPolicy::FTournamentIdentity& OutIdentity,
            int32& OutSavedHype,
            int32& OutArenaEntryFee,
            bool& OutHasArenaEntryFee)
        {
            return ArenaTelemetryWire::
                ResolveArenaTournamentIdentityFromPlayerResponse(
                    Body, FortniteVersion, &OutIdentity,
                    &OutSavedHype, &OutArenaEntryFee,
                    &OutHasArenaEntryFee);
        }

        void ResolveTournamentLookupOnWorker(
            const FTournamentLookup& Lookup,
            const std::string& BackendUrl,
            const std::string& Token)
        {
            FTournamentDelivery Delivery{};
            Delivery.Controller = Lookup.Controller;
            Delivery.PlayerState = Lookup.PlayerState;
            Delivery.SessionOrdinal = Lookup.SessionOrdinal;
            Delivery.RequestId = Lookup.RequestId;
            Delivery.CaptureGeneration = Lookup.CaptureGeneration;

            if (!Lookup.SavedProgressionEnabled)
            {
                // Pausing saved progression is not an Arena-off switch. Give
                // the client a clean Division 1 match-local tournament state
                // without reading or exposing the stored backend total.
                Delivery.Resolved =
                    ArenaTelemetryPolicy::BuildArenaTournamentIdentity(
                        Lookup.FortniteVersion, 1, &Delivery.Identity);
                Delivery.SavedHype = 0;
                Delivery.ArenaEntryFee = 0;
                Delivery.HasArenaEntryFee = Delivery.Resolved;
            }
            else
            {
                const std::string EncodedAccount =
                    EscapePathComponent(Lookup.AccountId);
                if (!EncodedAccount.empty())
                {
                    char UserAgent[128]{};
                    sprintf_s(
                        UserAgent,
                        "Fortnite/++Fortnite+Release-%.2f-CL-0 Windows/10.0",
                        Lookup.FortniteVersion);
                    const auto Response = SendHttp(
                        BackendUrl,
                        Token,
                        ("/api/v1/players/Fortnite/" + EncodedAccount).c_str(),
                        "GET",
                        {},
                        UserAgent);
                    if (Response.Success)
                    {
                        Delivery.Resolved = ResolveTournamentIdentity(
                            Response.Body,
                            Lookup.FortniteVersion,
                            Delivery.Identity,
                            Delivery.SavedHype,
                            Delivery.ArenaEntryFee,
                            Delivery.HasArenaEntryFee);
                    }
                }
            }
            if (Delivery.Resolved)
            {
                SDK::DbgLog(
                    "[ArenaTelemetry] tournament lookup resolved account=%s saving=%d savedHype=%d entryFee=%d feeKnown=%d event=%s window=%s\n",
                    Lookup.AccountId,
                    Lookup.SavedProgressionEnabled ? 1 : 0,
                    Delivery.SavedHype,
                    Delivery.ArenaEntryFee,
                    Delivery.HasArenaEntryFee ? 1 : 0,
                    Delivery.Identity.EventId,
                    Delivery.Identity.EventWindowId);
            }

            // A full delivery queue must not block scoring/reporting. Publish
            // a monotonic drop watermark so the game thread can release every
            // possibly affected participant and issue a fresh request. Some
            // lower request ids may be retried unnecessarily, but request-id
            // validation makes their eventual stale deliveries harmless.
            if (!TryPushSpsc(
                    GTournamentDeliveryQueue,
                    GTournamentDeliveryWrite,
                    GTournamentDeliveryRead,
                    Delivery))
            {
                uint64_t Observed =
                    GLastDroppedTournamentDeliveryRequestId.load(
                        std::memory_order_relaxed);
                while (Observed < Lookup.RequestId &&
                       !GLastDroppedTournamentDeliveryRequestId.
                            compare_exchange_weak(
                                Observed,
                                Lookup.RequestId,
                                std::memory_order_release,
                                std::memory_order_relaxed))
                {
                }
                SDK::DbgLog(
                    "[ArenaTelemetry] tournament delivery queue full; request=%llu will retry\n",
                    static_cast<unsigned long long>(Lookup.RequestId));
            }
        }

        bool ApplyCaptureResponse(
            const std::string& Body,
            uint64_t* ResponseRevision = nullptr)
        {
            if (ResponseRevision)
                *ResponseRevision = 0;
            const auto Document = nlohmann::json::parse(
                Body, nullptr, false);
            if (Document.is_discarded() || !Document.is_object())
                return false;

            const bool PreviousEnabled =
                GCaptureEnabled.load(std::memory_order_acquire);
            const uint64_t PreviousRevision =
                GCaptureRevision.load(std::memory_order_acquire);

            if (Document.contains("captureEnabled") &&
                Document["captureEnabled"].is_boolean())
            {
                GCaptureEnabled.store(
                    Document["captureEnabled"].get<bool>(),
                    std::memory_order_release);
            }
            if (Document.contains("captureRevision") &&
                (Document["captureRevision"].is_number_unsigned() ||
                 Document["captureRevision"].is_number_integer()))
            {
                const int64_t Revision =
                    Document["captureRevision"].get<int64_t>();
                if (Revision >= 0)
                {
                    if (ResponseRevision)
                    {
                        *ResponseRevision =
                            static_cast<uint64_t>(Revision);
                    }
                    GCaptureRevision.store(
                        static_cast<uint64_t>(Revision),
                        std::memory_order_release);
                }
            }
            const bool CurrentEnabled =
                GCaptureEnabled.load(std::memory_order_acquire);
            const uint64_t CurrentRevision =
                GCaptureRevision.load(std::memory_order_acquire);
            if (CurrentEnabled != PreviousEnabled ||
                CurrentRevision != PreviousRevision)
            {
                GCaptureStateGeneration.fetch_add(
                    1, std::memory_order_acq_rel);
            }
            return !Document.contains("ok") ||
                (Document["ok"].is_boolean() &&
                 Document["ok"].get<bool>());
        }

        nlohmann::json SerializeEvent(const FWireEvent& Event)
        {
            nlohmann::json Json = {
                { "id", Event.Id },
                { "sequence", Event.Sequence },
                { "type", Event.Type },
                { "occurredAtMs", Event.OccurredAtMs },
                { "captureRevision", Event.CaptureRevision }
            };
            if (Event.HasAccount)
            {
                Json["accountId"] = Event.AccountId;
                Json["displayName"] = Event.DisplayName;
            }
            if (Event.HasTeamIndex)
                Json["teamIndex"] = Event.TeamIndex;
            if (Event.HasKillerAccount)
                Json["killerAccountId"] = Event.KillerAccountId;
            if (Event.HasVictimAccount)
                Json["victimAccountId"] = Event.VictimAccountId;
            if (Event.HasKills)
                Json["kills"] = Event.Kills;
            if (Event.HasPlacement)
                Json["placement"] = Event.Placement;
            if (Event.HasPlayersRemaining)
                Json["playersRemaining"] = Event.PlayersRemaining;
            if (Event.HasPointsDelta)
                Json["pointsDelta"] = Event.PointsDelta;
            return Json;
        }

        nlohmann::json BuildEnvelope(
            const std::deque<FQueuedMessage>& Pending,
            size_t Count)
        {
            const auto& First = Pending.front();
            nlohmann::json Envelope =
                ArenaTelemetryWire::BuildSessionEnvelope(
                    First.ServerInstanceId,
                    First.SessionId,
                    First.FortniteVersion,
                    First.Playlist,
                    kSchemaVersion);
            // A start request is the capture-revision handshake.  Its events
            // must be empty so every score-bearing fact can use the exact
            // revision acknowledged by that response.
            if (First.Kind == EMessageKind::Start)
                return Envelope;

            for (size_t Index = 0; Index < Count; ++Index)
            {
                Envelope["events"].push_back(
                    SerializeEvent(Pending[Index].Event));
            }
            return Envelope;
        }

        size_t AdjacentRequestCount(
            const std::deque<FQueuedMessage>& Pending)
        {
            if (Pending.empty() ||
                Pending.front().Kind != EMessageKind::Event)
            {
                return Pending.empty() ? 0 : 1;
            }

            const auto& First = Pending.front();
            size_t Count = 0;
            for (const auto& Message : Pending)
            {
                if (Count >= kMaximumBatchEvents ||
                    Message.Kind != EMessageKind::Event ||
                    strcmp(Message.ServerInstanceId,
                           First.ServerInstanceId) != 0 ||
                    strcmp(Message.SessionId, First.SessionId) != 0)
                {
                    break;
                }
                ++Count;
            }
            return Count;
        }

        const char* EndpointFor(EMessageKind Kind) noexcept
        {
            switch (Kind)
            {
            case EMessageKind::Start:
                return "/atlas/arena/session/start";
            case EMessageKind::End:
                return "/atlas/arena/session/end";
            default:
                return "/atlas/arena/session/events";
            }
        }

        ULONGLONG RetryDelay(unsigned FailureCount) noexcept
        {
            const unsigned Shift = FailureCount > 5
                ? 5
                : FailureCount;
            const ULONGLONG Delay = 250ULL << Shift;
            return Delay > kMaximumRetryDelayMs
                ? kMaximumRetryDelayMs
                : Delay;
        }

        std::string SessionKey(
            const FQueuedMessage& Message)
        {
            std::string Key(Message.ServerInstanceId);
            Key.push_back('\n');
            Key.append(Message.SessionId);
            return Key;
        }

        bool IsSessionAbort(
            const FQueuedMessage& Message) noexcept
        {
            return Message.Kind == EMessageKind::Event &&
                strcmp(Message.Event.Type, "session_abort") == 0;
        }

        FQueuedMessage BuildAbortFrom(
            const FQueuedMessage& Source,
            uint64_t Sequence) noexcept
        {
            FQueuedMessage Abort = Source;
            Abort.Kind = EMessageKind::Event;
            Abort.Event = {};
            CopyText(Abort.Event.Type, "session_abort");
            Abort.Event.Sequence = Sequence;
            Abort.Event.OccurredAtMs = UnixTimeMilliseconds();
            Abort.Event.CaptureRevision =
                GCaptureRevision.load(std::memory_order_acquire);
            sprintf_s(
                Abort.Event.Id,
                "%s:%s:abort",
                Abort.ServerInstanceId,
                Abort.SessionId);
            return Abort;
        }

        void QueuePendingAbort(
            const FQueuedMessage& Abort,
            std::deque<FQueuedMessage>& PendingAborts,
            std::unordered_set<std::string>& PendingAbortKeys)
        {
            const std::string Key = SessionKey(Abort);
            if (!PendingAbortKeys.insert(Key).second)
                return;

            PendingAborts.push_back(Abort);
            while (PendingAborts.size() >
                   kMaximumPendingAbortMessages)
            {
                const std::string DroppedKey =
                    SessionKey(PendingAborts.front());
                PendingAbortKeys.erase(DroppedKey);
                PendingAborts.pop_front();
                SDK::DbgLog(
                    "[ArenaTelemetry] abort backlog full; dropped best-effort invalidation while keeping end unsent key=%s\n",
                    DroppedKey.c_str());
            }
        }

        void DropOldestPendingSession(
            std::deque<FQueuedMessage>& Pending,
            std::deque<FQueuedMessage>& PendingAborts,
            std::unordered_set<std::string>&
                PendingAbortKeys,
            std::unordered_set<std::string>&
                SuppressedSessions,
            std::unordered_map<std::string, uint64_t>&
                SessionCaptureRevisions)
        {
            if (Pending.empty())
                return;

            const FQueuedMessage Representative = Pending.front();
            const std::string Key = SessionKey(Representative);
            bool RemovedEnd = false;
            size_t Removed = 0;
            uint64_t HighestSequence = 0;
            Pending.erase(
                std::remove_if(
                    Pending.begin(),
                    Pending.end(),
                    [&](const FQueuedMessage& Candidate)
                    {
                        if (SessionKey(Candidate) != Key)
                            return false;
                        RemovedEnd = RemovedEnd ||
                            Candidate.Kind == EMessageKind::End;
                        HighestSequence = (std::max)(
                            HighestSequence,
                            Candidate.Event.Sequence);
                        ++Removed;
                        return true;
                    }),
                Pending.end());

            // If the producer's end has not reached the worker yet, suppress
            // the rest of this session through that marker. With no end POST,
            // the backend can retain raw pending facts but can never commit a
            // partial total.
            if (!RemovedEnd)
                SuppressedSessions.insert(Key);

            // Replace an arbitrarily large compromised session with one
            // bounded invalidation marker. The worker performs an idempotent
            // start handshake before this marker if the original start had
            // not reached ATLAS yet, so the abort always carries an accepted
            // capture revision after connectivity returns.
            QueuePendingAbort(
                BuildAbortFrom(Representative, HighestSequence + 1),
                PendingAborts,
                PendingAbortKeys);

            const size_t Separator = Key.find('\n');
            if (Separator != std::string::npos)
            {
                SessionCaptureRevisions.erase(
                    Key.substr(Separator + 1));
            }

            SDK::DbgLog(
                "[ArenaTelemetry] outage backlog evicted whole uncommitted session key=%s messages=%zu\n",
                Key.c_str(),
                Removed);
        }

        DWORD WINAPI WorkerMain(LPVOID)
        {
            curl_global_init(CURL_GLOBAL_ALL);
            const std::string BackendUrl = ResolveBackendUrl();
            const std::string Token =
                ReadEnvironment("ATLAS_ARENA_TOKEN");
            SDK::DbgLog(
                "[ArenaTelemetry] async reporter started endpoint=%s token=%s\n",
                BackendUrl.c_str(), Token.empty() ? "none" : "configured");

            std::deque<FQueuedMessage> Pending;
            std::deque<FQueuedMessage> PendingAborts;
            std::unordered_map<std::string, uint64_t>
                SessionCaptureRevisions;
            std::unordered_set<std::string>
                SuppressedSessions;
            std::unordered_set<std::string>
                PendingAbortKeys;
            ULONGLONG NextCapturePoll = 0;
            ULONGLONG NextSendAttempt = 0;
            unsigned CaptureFailures = 0;
            unsigned SendFailures = 0;
            FTournamentLookup DeferredTournamentLookup{};
            bool HasDeferredTournamentLookup = false;

            for (;;)
            {
                FQueuedMessage Message{};
                while (TryPopMessage(Message))
                {
                    const std::string Key = SessionKey(Message);
                    const auto Suppressed =
                        SuppressedSessions.find(Key);
                    if (Suppressed != SuppressedSessions.end())
                    {
                        if (IsSessionAbort(Message))
                        {
                            QueuePendingAbort(
                                Message,
                                PendingAborts,
                                PendingAbortKeys);
                        }
                        if (Message.Kind == EMessageKind::End)
                            SuppressedSessions.erase(Suppressed);
                        continue;
                    }

                    Pending.push_back(Message);
                    while (Pending.size() >
                           kMaximumPendingMessages)
                    {
                        DropOldestPendingSession(
                            Pending,
                            PendingAborts,
                            PendingAbortKeys,
                            SuppressedSessions,
                            SessionCaptureRevisions);
                    }
                }

                const ULONGLONG Now = GetTickCount64();
                bool DidWork = false;

                std::deque<FQueuedMessage>* Outbound =
                    !PendingAborts.empty()
                        ? &PendingAborts
                        : &Pending;
                if (!Outbound->empty() && Now >= NextSendAttempt)
                {
                    const size_t Count =
                        AdjacentRequestCount(*Outbound);
                    try
                    {
                        const EMessageKind RequestKind =
                            Outbound->front().Kind;
                        const bool RequestIsAbort =
                            IsSessionAbort(Outbound->front());
                        const std::string RequestSessionId =
                            Outbound->front().SessionId;
                        const std::string RequestSessionKey =
                            SessionKey(Outbound->front());

                        // A compacted/producer abort can outlive its original
                        // queued start. Re-establish that idempotent handshake
                        // first so the invalidation event always uses the
                        // backend's exact capture revision.
                        if (RequestIsAbort &&
                            SessionCaptureRevisions.find(
                                RequestSessionId) ==
                                SessionCaptureRevisions.end())
                        {
                            const auto& Abort = Outbound->front();
                            const std::string StartBody =
                                ArenaTelemetryWire::BuildSessionEnvelope(
                                    Abort.ServerInstanceId,
                                    Abort.SessionId,
                                    Abort.FortniteVersion,
                                    Abort.Playlist,
                                    kSchemaVersion).dump();
                            const auto StartResponse = SendHttp(
                                BackendUrl,
                                Token,
                                EndpointFor(EMessageKind::Start),
                                "POST",
                                StartBody);
                            uint64_t StartRevision = 0;
                            if (!StartResponse.Success ||
                                !ApplyCaptureResponse(
                                    StartResponse.Body,
                                    &StartRevision) ||
                                StartRevision < 1)
                            {
                                ++SendFailures;
                                NextSendAttempt =
                                    Now + RetryDelay(SendFailures);
                                DidWork = true;
                                Sleep(kBusyWorkerSleepMs);
                                continue;
                            }
                            SessionCaptureRevisions[
                                RequestSessionId] = StartRevision;
                        }

                        // Start is strictly ahead of this session's events in
                        // the queue. Upgrade anything snapshotted while that
                        // request was in flight to the revision it returned.
                        if (RequestKind != EMessageKind::Start)
                        {
                            const auto Revision =
                                SessionCaptureRevisions.find(
                                    RequestSessionId);
                            if (Revision !=
                                SessionCaptureRevisions.end())
                            {
                                for (size_t Index = 0;
                                     Index < Count;
                                     ++Index)
                                {
                                    if ((*Outbound)[Index].Event.
                                            CaptureRevision <
                                        Revision->second)
                                    {
                                        (*Outbound)[Index].Event.
                                            CaptureRevision =
                                            Revision->second;
                                    }
                                }
                            }
                        }

                        const std::string Body =
                            BuildEnvelope(*Outbound, Count).dump();
                        std::vector<std::string> RequestEventIds;
                        if (RequestKind == EMessageKind::Event &&
                            !RequestIsAbort)
                        {
                            RequestEventIds.reserve(Count);
                            for (size_t Index = 0;
                                 Index < Count;
                                 ++Index)
                            {
                                RequestEventIds.emplace_back(
                                    (*Outbound)[Index].Event.Id);
                            }
                        }
                        const auto Response = SendHttp(
                            BackendUrl,
                            Token,
                            EndpointFor(Outbound->front().Kind),
                            "POST",
                            Body);
                        uint64_t ResponseRevision = 0;
                        const bool CaptureResponseApplied =
                            Response.Success &&
                            ApplyCaptureResponse(
                                Response.Body,
                                &ResponseRevision);
                        const bool CompleteEventAcknowledgement =
                            RequestEventIds.empty() ||
                            ArenaTelemetryWire::AcknowledgesEveryEvent(
                                Response.Body,
                                RequestEventIds);
                        if (CaptureResponseApplied &&
                            CompleteEventAcknowledgement &&
                            (RequestKind != EMessageKind::Start ||
                             ResponseRevision >= 1))
                        {
                            if (RequestKind == EMessageKind::Start)
                            {
                                SessionCaptureRevisions[
                                    RequestSessionId] =
                                    ResponseRevision;
                            }
                            for (size_t Index = 0;
                                 Index < Count;
                                 ++Index)
                            {
                                Outbound->pop_front();
                            }
                            SendFailures = 0;
                            NextSendAttempt = 0;
                            if (RequestKind == EMessageKind::End)
                            {
                                SessionCaptureRevisions.erase(
                                    RequestSessionId);
                            }
                            if (RequestIsAbort)
                                PendingAbortKeys.erase(
                                    RequestSessionKey);
                            if (RequestKind == EMessageKind::End ||
                                RequestIsAbort)
                            {
                                SessionCaptureRevisions.erase(
                                    RequestSessionId);
                                SuppressedSessions.erase(
                                    RequestSessionKey);
                            }
                        }
                        else if (CaptureResponseApplied &&
                                 !CompleteEventAcknowledgement &&
                                 RequestKind == EMessageKind::Event &&
                                 !RequestIsAbort)
                        {
                            // Some siblings may already be pending in ATLAS.
                            // Drop every unsent fact/end for this session and
                            // replace them with an idempotent abort so those
                            // accepted siblings can never be committed.
                            DropOldestPendingSession(
                                Pending,
                                PendingAborts,
                                PendingAbortKeys,
                                SuppressedSessions,
                                SessionCaptureRevisions);
                            SendFailures = 0;
                            NextSendAttempt = 0;
                            SDK::DbgLog(
                                "[ArenaTelemetry] incomplete batch acknowledgement; invalidating whole session key=%s\n",
                                RequestSessionKey.c_str());
                        }
                        else
                        {
                            ++SendFailures;
                            NextSendAttempt =
                                Now + RetryDelay(SendFailures);
                            if (SendFailures == 1 ||
                                (SendFailures % 8) == 0)
                            {
                                SDK::DbgLog(
                                    "[ArenaTelemetry] delivery retry status=%ld pending=%zu failures=%u\n",
                                    Response.Status,
                                    Pending.size() +
                                        PendingAborts.size(),
                                    SendFailures);
                            }
                        }
                    }
                    catch (const std::exception& Error)
                    {
                        (void)Error;
                        ++SendFailures;
                        NextSendAttempt =
                            Now + RetryDelay(SendFailures);
                        SDK::DbgLog(
                            "[ArenaTelemetry] serialization failed: %s\n",
                            Error.what());
                    }
                    DidWork = true;
                }
                else if (Now >= NextCapturePoll)
                {
                    const auto Response = SendHttp(
                        BackendUrl,
                        Token,
                        "/atlas/arena/capture",
                        "GET",
                        {});
                    if (Response.Success &&
                        ApplyCaptureResponse(Response.Body))
                    {
                        CaptureFailures = 0;
                        NextCapturePoll =
                            Now + kCapturePollIntervalMs;
                    }
                    else
                    {
                        ++CaptureFailures;
                        NextCapturePoll =
                            Now + RetryDelay(CaptureFailures);
                    }
                    DidWork = true;
                }

                // A new match can be queued immediately after the previous
                // match's End. Fetching restored Hype ahead of that ordered
                // End acknowledgement would return the old total and the
                // one-shot client notification would keep it for the whole
                // match. Only resolve display state after all score-bearing
                // reporter work has drained successfully.
                if (!DidWork &&
                    Pending.empty() && PendingAborts.empty())
                {
                    if (!HasDeferredTournamentLookup)
                    {
                        HasDeferredTournamentLookup = TryPopSpsc(
                            GTournamentLookupQueue,
                            GTournamentLookupWrite,
                            GTournamentLookupRead,
                            DeferredTournamentLookup);
                    }

                    // Pop/acquire the lookup first, then recheck the reporter
                    // producer ring. The producer queues the previous End
                    // before the next match's lookup, so this ordering makes
                    // that prior release visible and closes the cross-queue
                    // drain race.
                    const bool ReporterQueueEmpty =
                        GQueueRead.load(std::memory_order_acquire) ==
                        GQueueWrite.load(std::memory_order_acquire);
                    if (HasDeferredTournamentLookup &&
                        ArenaTelemetryPolicy::CanResolveTournamentLookup(
                            Pending.size(), PendingAborts.size(),
                            ReporterQueueEmpty))
                    {
                        ResolveTournamentLookupOnWorker(
                            DeferredTournamentLookup,
                            BackendUrl, Token);
                        DeferredTournamentLookup = {};
                        HasDeferredTournamentLookup = false;
                        DidWork = true;
                    }
                }

                Sleep(DidWork
                    ? kBusyWorkerSleepMs
                    : kIdleWorkerSleepMs);
            }
        }

        void EnsureWorkerStarted() noexcept
        {
            bool Expected = false;
            if (!GWorkerStarted.compare_exchange_strong(
                    Expected, true,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                return;
            }

            HANDLE Thread = CreateThread(
                nullptr, 0, &WorkerMain, nullptr, 0, nullptr);
            if (!Thread)
            {
                GWorkerStarted.store(false, std::memory_order_release);
                SDK::DbgLog(
                    "[ArenaTelemetry] failed to create reporter worker error=%lu\n",
                    GetLastError());
                return;
            }
            CloseHandle(Thread);
        }

        FQueuedMessage BuildSessionMessage(
            EMessageKind Kind,
            const char* EventType) noexcept
        {
            FQueuedMessage Message{};
            Message.Kind = Kind;
            CopyText(
                Message.ServerInstanceId,
                GServerInstanceId);
            CopyText(Message.SessionId, GSession.SessionId);
            CopyText(Message.Playlist, GSession.Playlist);
            Message.FortniteVersion =
                VersionInfo.FortniteVersion;
            if (Kind == EMessageKind::Start)
                return Message;

            CopyText(Message.Event.Type, EventType);
            Message.Event.Sequence =
                GSession.NextSequence + 1;
            Message.Event.OccurredAtMs =
                UnixTimeMilliseconds();
            Message.Event.CaptureRevision =
                GCaptureRevision.load(
                    std::memory_order_acquire);
            sprintf_s(
                Message.Event.Id,
                "%s:%s:%llu",
                GServerInstanceId,
                GSession.SessionId,
                static_cast<unsigned long long>(
                    Message.Event.Sequence));
            return Message;
        }

        void MarkSessionCorrupted(const char* Reason) noexcept
        {
            if (GSession.Corrupted)
                return;
            GSession.Corrupted = true;
            SDK::DbgLog(
                "[ArenaTelemetry] session marked uncommittable id=%s reason=%s\n",
                GSession.SessionId,
                Reason ? Reason : "unknown");
        }

        bool QueueSessionMessage(
            FQueuedMessage& Message,
            bool ScoreCritical = false) noexcept
        {
            if (!TryPushMessage(Message))
            {
                NoteDroppedMessage(Message.Event.Type);
                if (ScoreCritical)
                {
                    // Backend totals are staged until end. Once any scoring
                    // fact is missing, suppressing that end is the fail-safe
                    // that prevents a partial match from ever committing.
                    MarkSessionCorrupted(
                        Message.Event.Type[0]
                            ? Message.Event.Type
                            : "score_queue_full");
                }
                return false;
            }
            if (Message.Event.Sequence)
                GSession.NextSequence = Message.Event.Sequence;
            return true;
        }

        UNetConnection* FindConnection(
            AFortPlayerControllerAthena* Controller,
            AFortPlayerStateAthena* PlayerState) noexcept
        {
            auto World = UWorld::GetWorld();
            auto Driver = World
                ? static_cast<UNetDriver*>(World->NetDriver)
                : nullptr;
            if (!Driver)
                return nullptr;

            for (int Index = 0;
                 Index < Driver->ClientConnections.Num();
                 ++Index)
            {
                auto Connection = Driver->ClientConnections[Index];
                if (!Connection || !Connection->PlayerController)
                    continue;
                if (Connection->PlayerController == Controller ||
                    (PlayerState &&
                     Connection->PlayerController->PlayerState ==
                        PlayerState))
                {
                    return Connection;
                }
            }
            return nullptr;
        }

        size_t FindParticipant(
            AFortPlayerControllerAthena* Controller,
            AFortPlayerStateAthena* PlayerState) noexcept
        {
            for (size_t Index = 0;
                 Index < GSession.Participants.size();
                 ++Index)
            {
                const auto& Participant =
                    GSession.Participants[Index];
                if ((PlayerState &&
                     Participant.PlayerState == PlayerState) ||
                    (Controller &&
                     Participant.Controller == Controller))
                {
                    return Index;
                }
            }
            return (std::numeric_limits<size_t>::max)();
        }

        void PopulateParticipantIdentity(
            FParticipant& Participant,
            bool ForceRetry = false) noexcept
        {
            auto Controller = Participant.Controller;
            auto PlayerState = Participant.PlayerState;
            if (!PlayerState && Controller &&
                Controller->HasPlayerState())
            {
                PlayerState =
                    static_cast<AFortPlayerStateAthena*>(
                        Controller->PlayerState);
                Participant.PlayerState = PlayerState;
            }
            if (!PlayerState)
                return;

            if (PlayerState->HasTeamIndex())
            {
                Participant.TeamIndex = PlayerState->TeamIndex;
                Participant.TeamResolved =
                    ArenaTelemetryPolicy::IsValidHumanTeamIndex(
                        Participant.TeamIndex);
            }
            if (Participant.AccountId[0])
                return;

            const ULONGLONG Now = GetTickCount64();
            if (!ForceRetry &&
                Now < Participant.NextIdentityRetryAtMs)
            {
                return;
            }
            Participant.NextIdentityRetryAtMs =
                Now + kIdentityRetryIntervalMs;

            auto Connection = FindConnection(
                Controller, PlayerState);
            const std::string Name =
                GUI::GetPlayerNameGameThread(
                    PlayerState, Connection);
            if (Name.empty())
                return;

            CopyText(Participant.AccountId, Name);
            CopyText(Participant.DisplayName, Name);
        }

        bool QueueParticipantJoin(FParticipant& Participant) noexcept
        {
            if (Participant.JoinQueued ||
                !GSession.Active ||
                !GSession.StartQueued ||
                GSession.Ending)
            {
                return Participant.JoinQueued;
            }

            PopulateParticipantIdentity(Participant);
            if (!Participant.AccountId[0] ||
                !Participant.TeamResolved)
                return false;

            auto Message = BuildSessionMessage(
                EMessageKind::Event, "player_join");
            Message.Event.HasAccount = true;
            CopyText(
                Message.Event.AccountId,
                Participant.AccountId);
            CopyText(
                Message.Event.DisplayName,
                Participant.DisplayName);
            Message.Event.HasTeamIndex = true;
            Message.Event.TeamIndex =
                Participant.TeamIndex;
            if (!QueueSessionMessage(Message))
                return false;

            Participant.JoinQueued = true;
            return true;
        }

        size_t EnsureParticipant(
            AFortPlayerControllerAthena* Controller,
            AFortPlayerStateAthena* PlayerState) noexcept
        {
            if (!GSession.Active)
                return (std::numeric_limits<size_t>::max)();

            if (!PlayerState && Controller &&
                Controller->HasPlayerState())
            {
                PlayerState =
                    static_cast<AFortPlayerStateAthena*>(
                        Controller->PlayerState);
            }
            if (!PlayerState ||
                (PlayerState->HasbIsABot() &&
                 PlayerState->bIsABot))
            {
                return (std::numeric_limits<size_t>::max)();
            }

            size_t Index = FindParticipant(
                Controller, PlayerState);
            if (Index == (std::numeric_limits<size_t>::max)())
            {
                if (GSession.Participants.size() >=
                    kMaximumParticipants)
                {
                    return Index;
                }
                FParticipant Participant{};
                Participant.Controller = Controller;
                Participant.PlayerState = PlayerState;
                GSession.Participants.push_back(Participant);
                Index = GSession.Participants.size() - 1;
            }

            auto& Participant = GSession.Participants[Index];
            if (Controller)
                Participant.Controller = Controller;
            if (PlayerState)
                Participant.PlayerState = PlayerState;
            PopulateParticipantIdentity(Participant);
            QueueParticipantJoin(Participant);
            return Index;
        }

        AFortPlayerControllerAthena* FindController(
            AFortPlayerStateAthena* PlayerState) noexcept
        {
            if (!PlayerState)
                return nullptr;

            const size_t Existing = FindParticipant(
                nullptr, PlayerState);
            if (Existing != (std::numeric_limits<size_t>::max)())
            {
                return GSession.Participants[Existing].Controller;
            }

            auto Connection = FindConnection(nullptr, PlayerState);
            return Connection
                ? static_cast<AFortPlayerControllerAthena*>(
                    Connection->PlayerController)
                : nullptr;
        }

        enum class ETournamentNotificationResult : uint8_t
        {
            Sent,
            NotReady,
            Unsupported
        };

        bool HasReadyMatchEntryChannel(
            const FParticipant& Participant) noexcept
        {
            if (!Participant.Controller || !Participant.PlayerState)
                return false;

            __try
            {
                // ClientNotifyMatchEntered is owned by PlayerState in every
                // audited release. Wait until that exact state is assigned to
                // the owning controller and has an actor channel on the same
                // live connection. ProcessEvent can otherwise return normally
                // while the client RPC is silently discarded.
                if (!Participant.Controller->HasPlayerState() ||
                    Participant.Controller->PlayerState !=
                        Participant.PlayerState)
                {
                    return false;
                }

                auto* Connection = FindConnection(
                    Participant.Controller, Participant.PlayerState);
                if (!Connection ||
                    Connection->PlayerController != Participant.Controller)
                {
                    return false;
                }

                const int ChannelCount = Connection->OpenChannels.Num();
                if (ChannelCount <= 0 || ChannelCount > 65536)
                    return false;

                const UClass* ActorChannelClass =
                    UActorChannel::StaticClass();
                if (!ActorChannelClass)
                    return false;

                for (int Index = 0; Index < ChannelCount; ++Index)
                {
                    auto* Channel = Connection->OpenChannels[Index];
                    if (!Channel || !Channel->IsA(ActorChannelClass))
                        continue;
                    if (Channel->Actor == Participant.PlayerState)
                        return true;
                }
                return false;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool HasLoadedAcknowledgedMatchEntryPawn(
            const FParticipant& Participant) noexcept
        {
            if (!Participant.Controller)
                return false;

            __try
            {
                // An actor channel only proves that PlayerState replication
                // began.  On 17.30 the client can remain in frontend tournament
                // mode for several seconds after that point.  These properties
                // are reflected throughout the supported Arena range and are
                // set only after the client has loaded and acknowledged its
                // current match pawn.
                if (!Participant.Controller->HasPawn() ||
                    !Participant.Controller->Pawn ||
                    !Participant.Controller->HasAcknowledgedPawn() ||
                    Participant.Controller->AcknowledgedPawn !=
                        Participant.Controller->Pawn)
                {
                    return false;
                }

                // Newer builds do not all expose bClientPawnIsLoaded through
                // the same reflected owner. When it exists it remains the
                // strongest readiness signal; otherwise an acknowledged pawn
                // plus the live PlayerState actor channel above is sufficient.
                if (Participant.Controller->HasbClientPawnIsLoaded() &&
                    !Participant.Controller->bClientPawnIsLoaded)
                {
                    return false;
                }
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool HasReadyArenaPresentationChannel(
            const FParticipant& Participant,
            bool AllowPostMatchDelivery = false) noexcept
        {
            if (!Participant.Controller)
                return false;

            __try
            {
                auto* Connection = FindConnection(
                    Participant.Controller, Participant.PlayerState);
                const bool HasOwningConnection = Connection &&
                    Connection->PlayerController == Participant.Controller;
                if (AllowPostMatchDelivery)
                {
                    // The acknowledged pawn is the gameplay-readiness proof
                    // used before bootstrap. At match end it may already have
                    // been destroyed; the still-owning connection is enough
                    // for reliable ClientMessage delivery and its retries.
                    return HasOwningConnection;
                }
                const bool HasPawn =
                    Participant.Controller->HasPawn() &&
                    Participant.Controller->Pawn;
                const bool AcknowledgedPawnMatches = HasPawn &&
                    Participant.Controller->HasAcknowledgedPawn() &&
                    Participant.Controller->AcknowledgedPawn ==
                        Participant.Controller->Pawn;
                return ArenaTelemetryPolicy::
                    CanUsePrivateArenaPresentationChannel(
                        HasOwningConnection,
                        HasPawn,
                        AcknowledgedPawnMatches);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool IsCurrentCanonicalArenaSession(
            bool AllowEnding = false) noexcept
        {
            if (!GSession.Active || (!AllowEnding && GSession.Ending) ||
                !GSession.GameMode || !GSession.GameState ||
                !GSession.Playlist[0])
            {
                return false;
            }

            const auto* Playlist =
                AFortGameMode::GetActivePlaylist(GSession.GameState);
            if (!Playlist)
                return false;
            const std::string ObjectName =
                Playlist->Name.ToString().c_str();
            const char* CanonicalPath = nullptr;
            return ArenaTelemetryPolicy::ResolveCanonicalPlaylist(
                       ObjectName.c_str(),
                       VersionInfo.FortniteVersion,
                       &CanonicalPath) &&
                CanonicalPath &&
                strcmp(CanonicalPath, GSession.Playlist) == 0;
        }

        bool InvokeArenaPrivateClientMessage(
            AFortPlayerControllerAthena* Controller,
            FString& Message) noexcept
        {
            __try
            {
                Controller->ClientMessage(Message, FName(), 0.0f);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool SendArenaPrivateClientMessage(
            AFortPlayerControllerAthena* Controller,
            const wchar_t* Text) noexcept
        {
            if (!Controller || !Text || !*Text)
                return false;

            FString Message(Text);
            const bool Sent =
                InvokeArenaPrivateClientMessage(Controller, Message);
            Message.Free();
            return Sent;
        }

        bool TrySendArenaPresentationEventNow(
            FParticipant& Participant,
            const FPendingArenaPresentationEvent& Event) noexcept
        {
            const bool IsEntryFee =
                Event.Kind == ArenaTelemetryWire::
                    EArenaPresentationEventKind::BusFare;
            if (!Participant.TournamentLocalStateInitialized ||
                !Participant.TournamentBootstrapSent ||
                (!IsEntryFee &&
                 !ArenaTelemetryPolicy::CanFlushArenaPresentationAwards(
                     Participant.TournamentBootstrapSent,
                     Participant.TournamentEntryFeeStageComplete)) ||
                !Participant.Controller ||
                !IsCurrentCanonicalArenaSession(GSession.Ending))
            {
                return false;
            }

            const int32 ResultingHype = static_cast<int32>(
                ArenaTelemetryPolicy::ApplySessionLocalHypeDelta(
                    Participant.TournamentLocalHype,
                    Event.PointsDelta));
            const uint64_t Sequence =
                GSession.NextPresentationSequence + 1;
            wchar_t Buffer[128]{};
            if (!ArenaTelemetryWire::FormatArenaPresentationEventV1(
                    GSession.Ordinal,
                    Sequence,
                    Event.Kind,
                    Event.PointsDelta,
                    ResultingHype,
                    Event.Placement,
                    Event.NativeGraceExpected,
                    Buffer,
                    _countof(Buffer)) ||
                !SendArenaPrivateClientMessage(
                    Participant.Controller, Buffer))
            {
                return false;
            }

            GSession.NextPresentationSequence = Sequence;
            Participant.TournamentLocalHype = ResultingHype;
            return true;
        }

        void FlushPendingArenaPresentationEvents(
            FParticipant& Participant) noexcept
        {
            if (!ArenaTelemetryPolicy::CanFlushArenaPresentationAwards(
                    Participant.TournamentBootstrapSent,
                    Participant.TournamentEntryFeeStageComplete))
            {
                return;
            }
            while (Participant.PendingPresentationEventCount > 0)
            {
                if (!TrySendArenaPresentationEventNow(
                        Participant,
                        Participant.PendingPresentationEvents[0]))
                {
                    return;
                }

                for (size_t Index = 1;
                     Index < Participant.PendingPresentationEventCount;
                     ++Index)
                {
                    Participant.PendingPresentationEvents[Index - 1] =
                        Participant.PendingPresentationEvents[Index];
                }
                --Participant.PendingPresentationEventCount;
            }
        }

        bool TrySendArenaBootstrap(FParticipant& Participant) noexcept
        {
            if (Participant.TournamentBootstrapSent)
                return true;
            if (!Participant.TournamentLocalStateInitialized ||
                !Participant.TournamentEntryFeeKnown ||
                !Participant.Controller ||
                !IsCurrentCanonicalArenaSession(GSession.Ending) ||
                !HasReadyArenaPresentationChannel(
                    Participant, GSession.Ending))
            {
                return false;
            }

            wchar_t Buffer[96]{};
            if (!ArenaTelemetryWire::FormatArenaPresentationBootstrapV1(
                    GSession.Ordinal,
                    Participant.TournamentSavingEnabled,
                    Participant.TournamentLocalHype,
                    Participant.TournamentEntryFee,
                    Buffer,
                    _countof(Buffer)) ||
                !SendArenaPrivateClientMessage(
                    Participant.Controller, Buffer))
            {
                return false;
            }

            Participant.TournamentBootstrapSent = true;
            WriteArenaVisualDiagnostic(
                "[ArenaVisual] bootstrap sent account=%s session=%llu saving=%d startingHype=%d entryFee=%d\n",
                Participant.AccountId,
                static_cast<unsigned long long>(GSession.Ordinal),
                Participant.TournamentSavingEnabled ? 1 : 0,
                Participant.TournamentLocalHype,
                Participant.TournamentEntryFee);
            return true;
        }

        bool QueueOrSendArenaPresentationEvent(
            FParticipant& Participant,
            ArenaTelemetryWire::EArenaPresentationEventKind Kind,
            int32 PointsDelta,
            int32 Placement,
            bool NativeGraceExpected) noexcept
        {
            FPendingArenaPresentationEvent Event{};
            Event.Kind = Kind;
            Event.PointsDelta = PointsDelta;
            Event.Placement = Placement;
            Event.NativeGraceExpected = NativeGraceExpected;
            if (!ArenaTelemetryWire::IsArenaPresentationEventShapeValid(
                    Kind, PointsDelta, Placement) ||
                Kind == ArenaTelemetryWire::
                    EArenaPresentationEventKind::BusFare ||
                !IsCurrentCanonicalArenaSession())
            {
                return false;
            }

            if (!Participant.TournamentEntryFeeStageComplete &&
                !Participant.TournamentEntryFeeVisualRequested)
            {
                // Some releases do not expose the aircraft callback. A real
                // score award proves gameplay has begun, so it is the bounded
                // universal fallback trigger for the still-pending fare. The
                // central ownership policy still gives a genuinely in-flight
                // native candidate its short grace period before taking over.
                Participant.TournamentEntryFeeVisualRequested = true;
                Participant.TournamentNotificationWaitStartedAtMs =
                    GetTickCount64();
                WriteArenaVisualDiagnostic(
                    "[ArenaVisual] score award forced pending bus fare account=%s kind=%lc\n",
                    Participant.AccountId,
                    static_cast<wchar_t>(Kind));
                // Releases without an aircraft callback use the first real
                // award as the same idempotent presentation trigger. Drive the
                // cached native path, fare stage, and lookup immediately rather
                // than waiting for an unrelated later Tick.
                ServiceArenaPresentation(Participant);
            }

            if (TrySendArenaPresentationEventNow(Participant, Event))
                return true;
            if (Participant.PendingPresentationEventCount >=
                Participant.PendingPresentationEvents.size())
            {
                WriteArenaVisualDiagnostic(
                    "[ArenaVisual] presentation queue full account=%s kind=%lc\n",
                    Participant.AccountId,
                    static_cast<wchar_t>(Kind));
                return false;
            }

            Participant.PendingPresentationEvents[
                Participant.PendingPresentationEventCount++] = Event;
            return true;
        }

        UFunction* ResolveMatchEntryFunction(
            const UObject* Target) noexcept
        {
            if (!Target)
                return nullptr;
            __try
            {
                // UFunctions remain UField children on modern UE4; using
                // GetProperty() here would instead search ChildProperties
                // and can reinterpret an FProperty as a UFunction.
                if (auto* Function = Target->GetFunction(
                        "ClientNotifyMatchEntered"))
                {
                    return Function;
                }

                // A concrete script path is a final fallback for SDK builds
                // whose inherited UField walk is incomplete.  Keep it bound
                // to the target's known runtime class, never to an arbitrary
                // reflected property.
                if (Target->Cast<AFortPlayerStateAthena>())
                {
                    return const_cast<UFunction*>(FindObject<UFunction>(
                        L"/Script/FortniteGame.FortPlayerStateAthena.ClientNotifyMatchEntered"));
                }
                return nullptr;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return nullptr;
            }
        }

        bool MatchesMatchEntryPropertyUnsafe(
            const UStruct* Owner,
            const char* Name,
            uint64_t CastFlags,
            uint32_t ExpectedOffset,
            uint32_t ExpectedElementSize) noexcept
        {
            if (!Owner || !Name || !*Name || !CastFlags ||
                Offsets::ElementSize < sizeof(int32))
            {
                return false;
            }

            // Comparing both lookups prevents a typed search from skipping a
            // wrong-type field and accidentally accepting an inherited field
            // with the same name.
            const UField* NamedProperty = Owner->GetProperty(Name);
            const UField* TypedProperty = Owner->GetProperty(
                Name, CastFlags);
            if (!NamedProperty || NamedProperty != TypedProperty)
                return false;

            const size_t RequiredMetadataBytes =
                static_cast<size_t>((std::max)(
                    Offsets::Offset_Internal,
                    Offsets::ElementSize)) + sizeof(uint32);
            if (RequiredMetadataBytes > 0x400 ||
                !SDK::MemReadable(
                    NamedProperty, RequiredMetadataBytes))
            {
                return false;
            }

            return SDK::ReadPropertyOffset(GetFromOffset<uint32>(
                       NamedProperty, Offsets::Offset_Internal)) ==
                       ExpectedOffset &&
                GetFromOffset<uint32>(
                    NamedProperty, Offsets::ElementSize) ==
                    ExpectedElementSize &&
                GetFromOffset<int32>(
                    NamedProperty,
                    Offsets::ElementSize - sizeof(int32)) == 1;
        }

        bool MatchEntryPropertyReferencesStructUnsafe(
            const UStruct* Owner,
            const char* Name,
            const UStruct* ExpectedStruct) noexcept
        {
            if (!Owner || !Name || !*Name || !ExpectedStruct)
                return false;

            const UField* Property = Owner->GetProperty(
                Name, kCastClassStructProperty);
            if (!Property)
                return false;

            // FStructProperty::Struct moved as the UProperty/FProperty base
            // changed across the supported UE4 builds. Avoid guessing that
            // version-dependent offset: require the exact canonical
            // UScriptStruct pointer to occur once in the reflected property's
            // bounded metadata region. A missing or ambiguous pointee fails
            // closed before a parameter buffer is ever constructed.
            constexpr size_t kMaximumStructPropertyMetadataBytes = 0x100;
            if (!SDK::MemReadable(
                    Property, kMaximumStructPropertyMetadataBytes))
            {
                return false;
            }

            size_t Matches = 0;
            for (size_t Offset = 0;
                 Offset + sizeof(const UStruct*) <=
                     kMaximumStructPropertyMetadataBytes;
                 Offset += alignof(const UStruct*))
            {
                const auto Candidate =
                    GetFromOffset<const UStruct*>(
                        Property, static_cast<uint32>(Offset));
                if (Candidate == ExpectedStruct)
                    ++Matches;
            }
            return Matches == 1;
        }

        bool ReadTournamentStatPropertyUnsafe(
            const UStruct* Owner,
            const char* Name,
            uint64_t CastFlags,
            ArenaTelemetryPolicy::FTournamentStatFieldSchema*
                OutSchema) noexcept
        {
            if (OutSchema)
                *OutSchema = {};
            if (!Owner || !Name || !*Name || !CastFlags || !OutSchema ||
                Offsets::ElementSize < sizeof(int32))
            {
                return false;
            }

            const UField* NamedProperty = Owner->GetProperty(Name);
            const UField* TypedProperty = Owner->GetProperty(
                Name, CastFlags);
            if (!NamedProperty || NamedProperty != TypedProperty)
                return false;

            const size_t RequiredMetadataBytes =
                static_cast<size_t>((std::max)(
                    Offsets::Offset_Internal,
                    Offsets::ElementSize)) + sizeof(uint32);
            if (RequiredMetadataBytes > 0x400 ||
                !SDK::MemReadable(
                    NamedProperty, RequiredMetadataBytes) ||
                GetFromOffset<int32>(
                    NamedProperty,
                    Offsets::ElementSize - sizeof(int32)) != 1)
            {
                return false;
            }

            OutSchema->Name = Name;
            OutSchema->Offset = SDK::ReadPropertyOffset(
                GetFromOffset<uint32>(
                    NamedProperty, Offsets::Offset_Internal));
            OutSchema->ElementSize = GetFromOffset<uint32>(
                NamedProperty, Offsets::ElementSize);
            return true;
        }

        const UStruct* ResolveTournamentStatInfoStructUnsafe() noexcept
        {
            const UStruct* Struct = nullptr;
            if (Offsets::StaticFindObject)
            {
                Struct = reinterpret_cast<const UStruct*>(
                    SDK::StaticFindObject(
                        L"/Script/FortniteGame.FortTournamentStatInfo",
                        UObject::StaticClass()));
            }
            if (!Struct)
                Struct = SDK::FindStruct("FortTournamentStatInfo");
            if (!Struct)
                Struct = SDK::FindStruct("FFortTournamentStatInfo");
            return Struct;
        }

        UFunction* ResolveTournamentStatFunctionUnsafe(
            const AFortPlayerStateAthena* Target) noexcept
        {
            if (!Target)
                return nullptr;
            if (auto* Function = Target->GetFunction(
                    "ClientReportTournamentStatUpdate"))
            {
                return Function;
            }
            return const_cast<UFunction*>(FindObject<UFunction>(
                L"/Script/FortniteGame.FortPlayerStateAthena.ClientReportTournamentStatUpdate"));
        }

        using ETournamentStatVisualResult =
            ArenaTelemetryPolicy::ETournamentVisualDispatchOutcome;

        enum class ETournamentStatDispatchState : LONG
        {
            NotAttempted = 0,
            Attempting = 1,
            Sent = 2,
            Failed = 3
        };

        bool GuardedTournamentStatProcessEvent(
            const UObject* Target,
            UFunction* Function,
            void* Parameters) noexcept
        {
            if (!Target || !Function || !Parameters)
                return false;
            __try
            {
                Target->ProcessEvent(Function, Parameters);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        ETournamentStatVisualResult
            TryNotifyTournamentStatVisualUnsafe(
                AFortPlayerStateAthena* Target,
                const ArenaTelemetryPolicy::FTournamentStatVisual&
                    Visual,
                bool AllowLegacyZeroParameter,
                volatile LONG* DispatchState) noexcept
        {
            if (!Target || !Visual.StatName || !*Visual.StatName ||
                !Visual.DisplayName || !*Visual.DisplayName)
            {
                return ETournamentStatVisualResult::SchemaUnsupported;
            }

            UFunction* Function =
                ResolveTournamentStatFunctionUnsafe(Target);
            if (!Function)
                return ETournamentStatVisualResult::SchemaUnsupported;

            const auto FunctionSchema = Function->GetParamsNamed();
            if (FunctionSchema.Size == 0 &&
                FunctionSchema.NameOffsetMap.empty())
            {
                const auto ResolvedSchema =
                    ArenaTelemetryPolicy::ResolveTournamentStatSchema(
                        0, nullptr, 0, 0, nullptr, 0);
                if (!AllowLegacyZeroParameter ||
                    ResolvedSchema != ArenaTelemetryPolicy::
                        ETournamentStatSchema::LegacyZeroParameter)
                {
                    return ETournamentStatVisualResult::SchemaUnsupported;
                }

                // ProcessEvent still receives stable storage even though the
                // reflected payload is empty. This mirrors generated wrappers
                // while avoiding a version-selected native call.
                uint8 EmptyParameters = 0;
                if (DispatchState)
                {
                    *DispatchState = static_cast<LONG>(
                        ETournamentStatDispatchState::Attempting);
                }
                const bool Called = GuardedTournamentStatProcessEvent(
                    Target, Function, &EmptyParameters);
                if (DispatchState)
                {
                    *DispatchState = static_cast<LONG>(
                        Called
                            ? ETournamentStatDispatchState::Sent
                            : ETournamentStatDispatchState::Failed);
                }
                static uint32 LegacyDispatchLogs = 0;
                if (LegacyDispatchLogs++ < 16)
                {
                    WriteArenaVisualDiagnostic(
                        "[ArenaVisual] stat legacy-zero-parameter version=%.2f result=%s\n",
                        VersionInfo.FortniteVersion,
                        Called ? "sent" : "ProcessEvent-failed");
                }
                return Called
                    ? ETournamentStatVisualResult::Sent
                    : ETournamentStatVisualResult::ProcessEventFailed;
            }

            if (FunctionSchema.NameOffsetMap.size() != 1)
                return ETournamentStatVisualResult::SchemaUnsupported;
            const UStruct* StatInfo =
                ResolveTournamentStatInfoStructUnsafe();
            if (!StatInfo)
                return ETournamentStatVisualResult::SchemaUnsupported;
            const auto& ReflectedParameter =
                FunctionSchema.NameOffsetMap[0];
            ArenaTelemetryPolicy::FTournamentStatParameterSchema
                ParameterSchema{
                    ReflectedParameter.Name.c_str(),
                    ReflectedParameter.Offset,
                    ReflectedParameter.ElementSize,
                    ReflectedParameter.PropertyFlags
                };

            const int32 ReflectedStatInfoSize =
                StatInfo->GetPropertiesSize();
            if (ReflectedStatInfoSize <= 0)
                return ETournamentStatVisualResult::SchemaUnsupported;

            if (!MatchesMatchEntryPropertyUnsafe(
                    Function,
                    ParameterSchema.Name,
                    kCastClassStructProperty,
                    0,
                    static_cast<uint32>(ReflectedStatInfoSize)) ||
                !MatchEntryPropertyReferencesStructUnsafe(
                    Function,
                    ParameterSchema.Name,
                    StatInfo))
            {
                return ETournamentStatVisualResult::SchemaUnsupported;
            }

            ArenaTelemetryPolicy::FTournamentStatFieldSchema
                Fields[4]{};
            size_t FieldCount = 0;
            if (!ReadTournamentStatPropertyUnsafe(
                    StatInfo, "StatName", kCastClassStrProperty,
                    &Fields[FieldCount++]) ||
                !ReadTournamentStatPropertyUnsafe(
                    StatInfo, "StatDisplayName",
                    kCastClassNameProperty,
                    &Fields[FieldCount++]) ||
                !ReadTournamentStatPropertyUnsafe(
                    StatInfo, "StatValue", kCastClassIntProperty,
                    &Fields[FieldCount++]))
            {
                return ETournamentStatVisualResult::SchemaUnsupported;
            }

            const UField* AnyDeltaProperty =
                StatInfo->GetProperty("bIsDeltaCount");
            if (AnyDeltaProperty)
            {
                if (!ReadTournamentStatPropertyUnsafe(
                        StatInfo, "bIsDeltaCount",
                        kCastClassBoolProperty,
                        &Fields[FieldCount++]))
                {
                    return ETournamentStatVisualResult::SchemaUnsupported;
                }
            }

            const auto ResolvedSchema =
                ArenaTelemetryPolicy::ResolveTournamentStatSchema(
                    FunctionSchema.Size,
                    &ParameterSchema,
                    1,
                    static_cast<uint32>(ReflectedStatInfoSize),
                    Fields,
                    FieldCount);
            if (ResolvedSchema ==
                ArenaTelemetryPolicy::ETournamentStatSchema::Unsupported)
            {
                static uint32 SchemaRejectionLogs = 0;
                if (SchemaRejectionLogs++ < 4)
                {
                    WriteArenaVisualDiagnostic(
                        "[ArenaVisual] reflected tournament stat schema rejected version=%.2f functionSize=%d parameterFlags=0x%llx statInfoSize=%d fields=%zu\n",
                        VersionInfo.FortniteVersion,
                        FunctionSchema.Size,
                        static_cast<unsigned long long>(
                            ParameterSchema.PropertyFlags),
                        StatInfo->GetPropertiesSize(),
                        FieldCount);
                }
                return ETournamentStatVisualResult::SchemaUnsupported;
            }

            const uint32 ParameterSize =
                ArenaTelemetryPolicy::TournamentStatParameterSize(
                    ResolvedSchema);
            const uint32 ValueOffset =
                ArenaTelemetryPolicy::TournamentStatValueOffset(
                    ResolvedSchema);
            if (!ParameterSize || !ValueOffset ||
                ValueOffset + sizeof(int32) > ParameterSize)
            {
                return ETournamentStatVisualResult::SchemaUnsupported;
            }
            void* Parameters = FMemory::Malloc(ParameterSize);
            if (!Parameters)
                return ETournamentStatVisualResult::SchemaUnsupported;
            memset(Parameters, 0, ParameterSize);

            auto* ParameterBytes = static_cast<uint8*>(Parameters);
            auto* StatName = new (ParameterBytes) FString(
                Visual.StatName);
            const FName DisplayName(Visual.DisplayName);
            if (!DisplayName.IsValid())
            {
                if (StatName->Data)
                    FMemory::Free(StatName->Data);
                FMemory::Free(Parameters);
                return ETournamentStatVisualResult::SchemaUnsupported;
            }

            const uint32 NameSize =
                ArenaTelemetryPolicy::TournamentStatNamePropertySize(
                    ResolvedSchema);
            memcpy(ParameterBytes + 0x10, &DisplayName, NameSize);
            const auto Payload =
                ArenaTelemetryPolicy::ResolveTournamentStatPayload(
                    ResolvedSchema, Visual);
            if (ArenaTelemetryPolicy::TournamentStatSchemaHasDelta(
                    ResolvedSchema))
            {
                ParameterBytes[0x14] = Payload.IsDelta ? 1 : 0;
            }
            memcpy(ParameterBytes + ValueOffset,
                &Payload.Value, sizeof(Payload.Value));

            if (DispatchState)
            {
                *DispatchState = static_cast<LONG>(
                    ETournamentStatDispatchState::Attempting);
            }
            const bool Called = GuardedTournamentStatProcessEvent(
                Target, Function, Parameters);
            if (DispatchState)
            {
                *DispatchState = static_cast<LONG>(
                    Called
                        ? ETournamentStatDispatchState::Sent
                        : ETournamentStatDispatchState::Failed);
            }
            if (StatName->Data)
                FMemory::Free(StatName->Data);
            FMemory::Free(Parameters);
            static uint32 DispatchLogs = 0;
            if (DispatchLogs++ < 16)
            {
                WriteArenaVisualDiagnostic(
                    "[ArenaVisual] stat display=%ls value=%d delta=%d schema=%d version=%.2f result=%s\n",
                    Visual.DisplayName,
                    Payload.Value,
                    Payload.IsDelta ? 1 : 0,
                    static_cast<int>(ResolvedSchema),
                    VersionInfo.FortniteVersion,
                    Called ? "sent" : "ProcessEvent-failed");
            }
            return Called
                ? ETournamentStatVisualResult::Sent
                : ETournamentStatVisualResult::ProcessEventFailed;
        }

        ETournamentStatVisualResult TryNotifyTournamentStatVisual(
            AFortPlayerStateAthena* Target,
            const ArenaTelemetryPolicy::FTournamentStatVisual&
                Visual,
            bool AllowLegacyZeroParameter) noexcept
        {
            volatile LONG DispatchState = static_cast<LONG>(
                ETournamentStatDispatchState::NotAttempted);
            __try
            {
                return TryNotifyTournamentStatVisualUnsafe(
                    Target, Visual, AllowLegacyZeroParameter,
                    &DispatchState);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                // If cleanup faults after ProcessEvent returned, preserve Sent
                // so no caller can issue a second one-way presentation RPC.
                if (DispatchState == static_cast<LONG>(
                        ETournamentStatDispatchState::Sent))
                {
                    return ETournamentStatVisualResult::Sent;
                }
                if (DispatchState == static_cast<LONG>(
                        ETournamentStatDispatchState::Attempting) ||
                    DispatchState == static_cast<LONG>(
                        ETournamentStatDispatchState::Failed))
                {
                    return ETournamentStatVisualResult::ProcessEventFailed;
                }
                return ETournamentStatVisualResult::SchemaUnsupported;
            }
        }

        UFunction* ResolveTournamentPlacementFunctionUnsafe(
            const AFortPlayerControllerAthena* Target) noexcept
        {
            if (!Target)
                return nullptr;
            if (auto* Function = Target->GetFunction(
                    "ClientReportTournamentPlacementPointsScored"))
            {
                return Function;
            }
            return const_cast<UFunction*>(FindObject<UFunction>(
                L"/Script/FortniteGame.FortPlayerControllerAthena.ClientReportTournamentPlacementPointsScored"));
        }

        ETournamentStatVisualResult
            TryNotifyTournamentPlacementVisualUnsafe(
                AFortPlayerControllerAthena* Target,
                int Placement,
                int PointsEarned,
                volatile LONG* DispatchState) noexcept
        {
            if (!Target || Placement < 0 ||
                !ArenaTelemetryPolicy::IsValidPointsDelta(PointsEarned))
            {
                return ETournamentStatVisualResult::SchemaUnsupported;
            }

            UFunction* Function =
                ResolveTournamentPlacementFunctionUnsafe(Target);
            if (!Function)
                return ETournamentStatVisualResult::SchemaUnsupported;

            const auto FunctionSchema = Function->GetParamsNamed();
            if (FunctionSchema.NameOffsetMap.size() != 2)
                return ETournamentStatVisualResult::SchemaUnsupported;

            ArenaTelemetryPolicy::FTournamentStatParameterSchema
                Parameters[2]{};
            for (size_t Index = 0; Index < 2; ++Index)
            {
                const auto& Reflected =
                    FunctionSchema.NameOffsetMap[Index];
                Parameters[Index] = {
                    Reflected.Name.c_str(),
                    Reflected.Offset,
                    Reflected.ElementSize,
                    Reflected.PropertyFlags
                };
            }
            if (!ArenaTelemetryPolicy::
                    ResolveTournamentPlacementPointsSchema(
                        FunctionSchema.Size,
                        Parameters,
                        2) ||
                !MatchesMatchEntryPropertyUnsafe(
                    Function, "Placement", kCastClassIntProperty,
                    0, sizeof(int32)) ||
                !MatchesMatchEntryPropertyUnsafe(
                    Function, "PointsEarned", kCastClassIntProperty,
                    sizeof(int32), sizeof(int32)))
            {
                return ETournamentStatVisualResult::SchemaUnsupported;
            }

            std::array<uint8, sizeof(int32) * 2> Buffer{};
            memcpy(Buffer.data(), &Placement, sizeof(Placement));
            memcpy(Buffer.data() + sizeof(int32),
                &PointsEarned, sizeof(PointsEarned));
            if (DispatchState)
            {
                *DispatchState = static_cast<LONG>(
                    ETournamentStatDispatchState::Attempting);
            }
            const bool Called = GuardedTournamentStatProcessEvent(
                Target, Function, Buffer.data());
            if (DispatchState)
            {
                *DispatchState = static_cast<LONG>(
                    Called
                        ? ETournamentStatDispatchState::Sent
                        : ETournamentStatDispatchState::Failed);
            }
            static uint32 DispatchLogs = 0;
            if (DispatchLogs++ < 16)
            {
                WriteArenaVisualDiagnostic(
                    "[ArenaVisual] placement placement=%d points=%d version=%.2f result=%s\n",
                    Placement,
                    PointsEarned,
                    VersionInfo.FortniteVersion,
                    Called ? "sent" : "ProcessEvent-failed");
            }
            return Called
                ? ETournamentStatVisualResult::Sent
                : ETournamentStatVisualResult::ProcessEventFailed;
        }

        ETournamentStatVisualResult
            TryNotifyTournamentPlacementVisual(
                AFortPlayerControllerAthena* Target,
                int Placement,
                int PointsEarned) noexcept
        {
            volatile LONG DispatchState = static_cast<LONG>(
                ETournamentStatDispatchState::NotAttempted);
            __try
            {
                return TryNotifyTournamentPlacementVisualUnsafe(
                    Target, Placement, PointsEarned, &DispatchState);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                // Once ProcessEvent has begun, fail closed. The one-way RPC
                // may already have been queued, so a typed-stat fallback could
                // double-present the same placement award.
                if (DispatchState == static_cast<LONG>(
                        ETournamentStatDispatchState::Sent))
                {
                    return ETournamentStatVisualResult::Sent;
                }
                if (DispatchState == static_cast<LONG>(
                        ETournamentStatDispatchState::Attempting) ||
                    DispatchState == static_cast<LONG>(
                        ETournamentStatDispatchState::Failed))
                {
                    return ETournamentStatVisualResult::ProcessEventFailed;
                }
                return ETournamentStatVisualResult::SchemaUnsupported;
            }
        }

        bool ValidateMatchEntryTypesUnsafe(
            UFunction* Function,
            ArenaTelemetryPolicy::EMatchEntrySchema Schema) noexcept
        {
            const uint32_t StringSize = sizeof(FString);
            switch (Schema)
            {
            case ArenaTelemetryPolicy::EMatchEntrySchema::EventWindowOnly:
                return MatchesMatchEntryPropertyUnsafe(
                    Function, "EventWindowId", kCastClassStrProperty,
                    0, StringSize);

            case ArenaTelemetryPolicy::EMatchEntrySchema::EventAndWindow:
                return MatchesMatchEntryPropertyUnsafe(
                           Function, "EventId", kCastClassStrProperty,
                           0, StringSize) &&
                    MatchesMatchEntryPropertyUnsafe(
                           Function, "EventWindowId",
                           kCastClassStrProperty,
                           StringSize, StringSize);

            case ArenaTelemetryPolicy::EMatchEntrySchema::EventWindowAndGroup:
                return MatchesMatchEntryPropertyUnsafe(
                           Function, "EventId", kCastClassStrProperty,
                           0, StringSize) &&
                    MatchesMatchEntryPropertyUnsafe(
                           Function, "EventWindowId",
                           kCastClassStrProperty,
                           StringSize, StringSize) &&
                    MatchesMatchEntryPropertyUnsafe(
                           Function, "EventGroupId",
                           kCastClassStrProperty,
                           StringSize * 2, StringSize);

            case ArenaTelemetryPolicy::EMatchEntrySchema::TournamentIds:
                break;

            default:
                return false;
            }

            if (!MatchesMatchEntryPropertyUnsafe(
                    Function, "EventIds", kCastClassStructProperty,
                    0, StringSize * 4))
            {
                return false;
            }

            // 14.30+ uses FEventTournamentIds. The SDK does not expose the
            // FStructProperty pointee portably, so validate both the top-level
            // StructProperty and the canonical script struct's exact layout.
            const UStruct* TournamentIds = nullptr;
            if (Offsets::StaticFindObject)
            {
                TournamentIds = reinterpret_cast<const UStruct*>(
                    SDK::StaticFindObject(
                        L"/Script/FortniteGame.EventTournamentIds",
                        UObject::StaticClass()));
            }
            if (!TournamentIds)
                TournamentIds = SDK::FindStruct("EventTournamentIds");
            if (!TournamentIds ||
                TournamentIds->GetPropertiesSize() !=
                    static_cast<int32>(StringSize * 4))
            {
                return false;
            }
            if (!MatchEntryPropertyReferencesStructUnsafe(
                    Function, "EventIds", TournamentIds))
            {
                return false;
            }

            const bool HasSubGroup =
                MatchesMatchEntryPropertyUnsafe(
                    TournamentIds, "SubGroupId",
                    kCastClassStrProperty,
                    StringSize * 3, StringSize) ||
                MatchesMatchEntryPropertyUnsafe(
                    TournamentIds, "SubGroupID",
                    kCastClassStrProperty,
                    StringSize * 3, StringSize);
            return MatchesMatchEntryPropertyUnsafe(
                       TournamentIds, "EventId",
                       kCastClassStrProperty,
                       0, StringSize) &&
                MatchesMatchEntryPropertyUnsafe(
                       TournamentIds, "WindowId",
                       kCastClassStrProperty,
                       StringSize, StringSize) &&
                MatchesMatchEntryPropertyUnsafe(
                       TournamentIds, "GroupId",
                       kCastClassStrProperty,
                       StringSize * 2, StringSize) &&
                HasSubGroup;
        }

        bool ValidateMatchEntryTypes(
            UFunction* Function,
            ArenaTelemetryPolicy::EMatchEntrySchema Schema) noexcept
        {
            __try
            {
                return ValidateMatchEntryTypesUnsafe(Function, Schema);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool GuardedMatchEntryProcessEvent(
            const UObject* Target,
            UFunction* Function,
            void* Parameters) noexcept
        {
            if (!Target || !Function || !Parameters)
                return false;
            __try
            {
                Target->ProcessEvent(Function, Parameters);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        ETournamentNotificationResult TryNotifyMatchEnteredOnTargetUnsafe(
            UObject* Target,
            const ArenaTelemetryPolicy::FTournamentIdentity& Identity) noexcept
        {
            if (!Target || !Identity.EventId[0] ||
                !Identity.EventWindowId[0])
            {
                return ETournamentNotificationResult::Unsupported;
            }

            // RPC functions remain UField children. Resolve them through the
            // function path so a modern FProperty is never treated as a
            // UFunction.
            UFunction* Function = ResolveMatchEntryFunction(Target);
            if (!Function)
                return ETournamentNotificationResult::Unsupported;

            const auto Schema = Function->GetParamsNamed();

            // Accept only the SDK-audited shapes.  In particular, 14.30+
            // supplies a reflected FEventTournamentIds parameter rather than
            // two top-level FStrings. Its property kinds and nested layout are
            // validated below before any FString is constructed.
            if (Schema.NameOffsetMap.size() > 3)
                return ETournamentNotificationResult::Unsupported;

            ArenaTelemetryPolicy::FMatchEntryParameterSchema
                PolicySchema[3]{};
            size_t PolicyIndex = 0;
            for (const auto& Parameter : Schema.NameOffsetMap)
            {
                PolicySchema[PolicyIndex++] = {
                    Parameter.Name.c_str(),
                    Parameter.Offset,
                    Parameter.ElementSize,
                    Parameter.PropertyFlags
                };
            }
            const auto EntrySchema =
                ArenaTelemetryPolicy::ResolveMatchEntrySchema(
                    Schema.Size,
                    sizeof(FString),
                    PolicySchema,
                    PolicyIndex);
            if (EntrySchema == ArenaTelemetryPolicy::EMatchEntrySchema::Unsupported)
            {
                return ETournamentNotificationResult::Unsupported;
            }
            if (!ValidateMatchEntryTypes(Function, EntrySchema))
                return ETournamentNotificationResult::Unsupported;

            std::array<uint32, 4> StringOffsets{};
            std::array<const char*, 4> StringValues{
                Identity.EventId,
                Identity.EventWindowId,
                Identity.EventGroupId,
                Identity.EventSubGroupId
            };
            size_t StringCount = 0;
            switch (EntrySchema)
            {
            case ArenaTelemetryPolicy::EMatchEntrySchema::EventWindowOnly:
                StringOffsets[0] = 0;
                StringValues[0] = Identity.EventWindowId;
                StringCount = 1;
                break;
            case ArenaTelemetryPolicy::EMatchEntrySchema::EventAndWindow:
                StringOffsets[0] = 0;
                StringOffsets[1] = sizeof(FString);
                StringCount = 2;
                break;
            case ArenaTelemetryPolicy::EMatchEntrySchema::EventWindowAndGroup:
                StringOffsets[0] = 0;
                StringOffsets[1] = sizeof(FString);
                StringOffsets[2] = sizeof(FString) * 2;
                StringCount = 3;
                break;
            case ArenaTelemetryPolicy::EMatchEntrySchema::TournamentIds:
                StringOffsets[0] = 0;
                StringOffsets[1] = sizeof(FString);
                StringOffsets[2] = sizeof(FString) * 2;
                StringOffsets[3] = sizeof(FString) * 3;
                StringCount = 4;
                break;
            default:
                return ETournamentNotificationResult::Unsupported;
            }

            void* Parameters = FMemory::Malloc(Schema.Size);
            if (!Parameters)
                return ETournamentNotificationResult::Unsupported;
            memset(Parameters, 0, Schema.Size);

            for (size_t Index = 0; Index < StringCount; ++Index)
            {
                if (StringOffsets[Index] > Schema.Size - sizeof(FString))
                {
                    FMemory::Free(Parameters);
                    return ETournamentNotificationResult::Unsupported;
                }
                const auto* Value = StringValues[Index] ? StringValues[Index] : "";
                const std::wstring ValueWide(Value, Value + strlen(Value));
                new (static_cast<uint8_t*>(Parameters) + StringOffsets[Index])
                    FString(ValueWide.c_str());
            }
            const bool Called = GuardedMatchEntryProcessEvent(
                Target, Function, Parameters);
            for (size_t Index = 0; Index < StringCount; ++Index)
            {
                auto* Value = reinterpret_cast<FString*>(
                    static_cast<uint8_t*>(Parameters) + StringOffsets[Index]);
                if (Value->Data)
                    FMemory::Free(Value->Data);
            }
            FMemory::Free(Parameters);
            return Called
                ? ETournamentNotificationResult::Sent
                : ETournamentNotificationResult::Unsupported;
        }

        ETournamentNotificationResult TryNotifyMatchEnteredOnTarget(
            UObject* Target,
            const ArenaTelemetryPolicy::FTournamentIdentity& Identity) noexcept
        {
            // GetParamsNamed() walks raw, version-dependent UProperty/FField
            // metadata. Keep the entire reflection/invocation path behind an
            // SEH boundary so an unsupported build fails closed instead of
            // taking down the game server before the narrower validators run.
            __try
            {
                return TryNotifyMatchEnteredOnTargetUnsafe(Target, Identity);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return ETournamentNotificationResult::Unsupported;
            }
        }

        ETournamentNotificationResult TryNotifyMatchEntered(
            FParticipant& Participant,
            const ArenaTelemetryPolicy::FTournamentIdentity& Identity) noexcept
        {
            if (!HasReadyMatchEntryChannel(Participant))
                return ETournamentNotificationResult::NotReady;

            return TryNotifyMatchEnteredOnTarget(
                Participant.PlayerState, Identity);
        }

        void InvalidatePendingTournamentIdentity(
            FParticipant& Participant) noexcept
        {
            Participant.TournamentLookupQueued = false;
            Participant.TournamentIdentityCached = false;
            Participant.TournamentLookupRequestId = 0;
            Participant.TournamentCaptureGeneration = 0;
            Participant.NextTournamentLookupAtMs = 0;
            Participant.NextTournamentNotificationAtMs = 0;
            Participant.TournamentReadinessObserved = false;
            Participant.TournamentReadinessStableSinceMs = 0;
            Participant.TournamentSavedHype = 0;
            Participant.TournamentEntryFee = 0;
            Participant.TournamentEntryFeeKnown = false;
            Participant.TournamentIdentity = {};
            if (!Participant.TournamentBootstrapSent)
            {
                Participant.TournamentLocalHype = 0;
                Participant.TournamentLocalStateInitialized = false;
                Participant.TournamentSavingEnabled = false;
                Participant.TournamentEntryFeeStageComplete = false;
                Participant.TournamentNotificationWaitStartedAtMs = 0;
                Participant.TournamentPresentationFallbackOwned = false;
            }
        }

        void SynchronizeTournamentCaptureState() noexcept
        {
            if (!GSession.Active)
                return;

            const uint64_t Generation =
                GCaptureStateGeneration.load(std::memory_order_acquire);
            const bool Enabled =
                GCaptureEnabled.load(std::memory_order_acquire);
            if (GSession.TournamentCaptureStateInitialized &&
                GSession.TournamentCaptureGeneration == Generation &&
                GSession.TournamentCaptureEnabled == Enabled)
            {
                return;
            }

            GSession.TournamentCaptureStateInitialized = true;
            GSession.TournamentCaptureGeneration = Generation;
            GSession.TournamentCaptureEnabled = Enabled;
            for (auto& Participant : GSession.Participants)
            {
                // A notification already sent while capture was enabled is
                // not replayed or revoked mid-match. Only pending lookup data
                // is invalidated; a later re-enable obtains a fresh identity.
                if (!Participant.TournamentNotificationSent &&
                    !Participant.TournamentPresentationFallbackOwned)
                    InvalidatePendingTournamentIdentity(Participant);
            }
        }

        void RecoverDroppedTournamentDeliveries() noexcept
        {
            const uint64_t Watermark =
                GLastDroppedTournamentDeliveryRequestId.load(
                    std::memory_order_acquire);
            if (Watermark <=
                GObservedDroppedTournamentDeliveryRequestId)
            {
                return;
            }
            GObservedDroppedTournamentDeliveryRequestId = Watermark;
            if (!GSession.Active)
                return;

            const ULONGLONG RetryAt = GetTickCount64() + 250;
            for (auto& Participant : GSession.Participants)
            {
                if (Participant.TournamentLookupQueued &&
                    Participant.TournamentLookupRequestId <= Watermark)
                {
                    InvalidatePendingTournamentIdentity(Participant);
                    Participant.NextTournamentLookupAtMs = RetryAt;
                }
            }
        }

        bool TryAdvanceArenaEntryFeeStage(
            FParticipant& Participant) noexcept
        {
            const auto Action = ArenaTelemetryPolicy::
                ResolveArenaEntryFeeStageAction(
                    Participant.TournamentBootstrapSent,
                    Participant.TournamentEntryFeeKnown,
                    Participant.TournamentEntryFeeStageComplete,
                    Participant.TournamentEntryFee,
                    Participant.TournamentEntryFeeVisualRequested);
            if (Action == ArenaTelemetryPolicy::
                    EArenaEntryFeeStageAction::Wait)
            {
                return Participant.TournamentEntryFeeStageComplete;
            }
            if (!Participant.TournamentLocalStateInitialized ||
                !ArenaTelemetryPolicy::IsTournamentRuntimeEffectEnabled(
                    ArenaTelemetryPolicy::ETournamentRuntimeEffect::
                        SessionLocalPresentation,
                    GCaptureEnabled.load(std::memory_order_acquire)))
            {
                return false;
            }
            const bool MatchEntryEligible =
                ArenaTelemetryPolicy::CanNotifyMatchEntryForPlaylist(
                    GSession.Playlist);
            const ULONGLONG Now = GetTickCount64();
            const bool NativeCandidateInFlight =
                MatchEntryEligible &&
                !Participant.TournamentNotificationSent &&
                !GSession.TournamentNotificationUnavailable &&
                !Participant.TournamentPresentationFallbackOwned &&
                ArenaTelemetryPolicy::HasInFlightMatchEntryCandidate(
                    Participant.TournamentLookupQueued,
                    Participant.TournamentIdentityCached);
            if (MatchEntryEligible &&
                !Participant.TournamentNotificationSent &&
                !GSession.TournamentNotificationUnavailable &&
                !Participant.TournamentPresentationFallbackOwned &&
                !Participant.TournamentNotificationWaitStartedAtMs)
            {
                Participant.TournamentNotificationWaitStartedAtMs = Now;
            }
            const auto Ownership = ArenaTelemetryPolicy::
                ResolveMatchEntryPresentationOwnership(
                    MatchEntryEligible,
                    Participant.TournamentNotificationSent,
                    GSession.TournamentNotificationUnavailable,
                    Participant.TournamentPresentationFallbackOwned,
                    NativeCandidateInFlight,
                    Now,
                    Participant.TournamentNotificationWaitStartedAtMs);
            if (Ownership == ArenaTelemetryPolicy::
                    EMatchEntryPresentationOwnership::AwaitNative)
            {
                return false;
            }
            if (Ownership == ArenaTelemetryPolicy::
                    EMatchEntryPresentationOwnership::DirectBridge &&
                MatchEntryEligible &&
                !Participant.TournamentNotificationSent &&
                !Participant.TournamentPresentationFallbackOwned)
            {
                // Once the ordered private bridge takes over, never issue a
                // late ClientNotifyMatchEntered for this participant. That
                // would replay the bus fare after B has already been sent.
                Participant.TournamentPresentationFallbackOwned = true;
                Participant.TournamentLookupQueued = false;
                Participant.TournamentLookupRequestId = 0;
                Participant.TournamentCaptureGeneration = 0;
                Participant.TournamentIdentityCached = false;
                Participant.TournamentIdentity = {};
                Participant.NextTournamentNotificationAtMs = 0;
                Participant.TournamentReadinessObserved = false;
                Participant.TournamentReadinessStableSinceMs = 0;
                const ULONGLONG WaitMs =
                    Participant.TournamentNotificationWaitStartedAtMs &&
                    Now >= Participant.TournamentNotificationWaitStartedAtMs
                        ? Now - Participant.
                            TournamentNotificationWaitStartedAtMs
                        : 0;
                WriteArenaVisualDiagnostic(
                    "[ArenaVisual] direct bridge owns presentation account=%s candidate=%d waitMs=%llu\n",
                    Participant.AccountId,
                    NativeCandidateInFlight ? 1 : 0,
                    static_cast<unsigned long long>(WaitMs));
            }

            // Do not mirror MatchEntryFee through a second reflected RPC.
            // ClientNotifyMatchEntered already presents the authored fare on
            // releases whose native tournament path works. The private event
            // gives ATLAS Client an exact, sequenced repair only after its
            // native-observation grace period, avoiding the historic double
            // fare while still supporting releases with no native consumer.
            // A zero-fare B frame is control-only: it changes no score and the
            // client never turns it into a badge.
            FPendingArenaPresentationEvent Event{};
            Event.Kind = ArenaTelemetryWire::
                EArenaPresentationEventKind::BusFare;
            Event.PointsDelta = Participant.TournamentEntryFee;
            Event.NativeGraceExpected =
                Participant.TournamentNotificationSent;
            if (!TrySendArenaPresentationEventNow(Participant, Event))
            {
                return false;
            }

            Participant.TournamentEntryFeeStageComplete = true;
            WriteArenaVisualDiagnostic(
                "[ArenaVisual] bus fare event sent fee=%d account=%s nativeGrace=%d\n",
                Participant.TournamentEntryFee,
                Participant.AccountId,
                Participant.TournamentNotificationSent ? 1 : 0);
            return true;
        }

        void TryAdvanceArenaPresentation(
            FParticipant& Participant) noexcept
        {
            // This is the only path that releases queued positive awards:
            // pre-fare bootstrap, exactly-once fare stage, then E/P/V.
            if (!TrySendArenaBootstrap(Participant) ||
                !TryAdvanceArenaEntryFeeStage(Participant))
            {
                return;
            }
            FlushPendingArenaPresentationEvents(Participant);
        }

        bool TrySendArenaPresentationEnd(
            FParticipant& Participant) noexcept
        {
            // END must follow every presentation frame already staged for this
            // participant. ClientMessage is reliable and ordered, so a client
            // that accepts this marker has received the complete visual stream.
            const bool ChannelReady =
                HasReadyArenaPresentationChannel(Participant, true);
            const auto Action = ArenaTelemetryPolicy::
                ResolveArenaPresentationEndAction(
                    Participant.TournamentEndSent,
                    ChannelReady,
                    Participant.TournamentBootstrapSent,
                    Participant.TournamentEntryFeeStageComplete,
                    Participant.PendingPresentationEventCount);
            if (Action == ArenaTelemetryPolicy::
                    EArenaPresentationEndAction::Complete)
            {
                return true;
            }
            if (Action != ArenaTelemetryPolicy::
                    EArenaPresentationEndAction::Send)
            {
                return false;
            }

            wchar_t Buffer[64]{};
            if (!ArenaTelemetryWire::FormatArenaPresentationEndV1(
                    GSession.Ordinal, Buffer, _countof(Buffer)) ||
                !SendArenaPrivateClientMessage(
                    Participant.Controller, Buffer))
            {
                return false;
            }
            Participant.TournamentEndSent = true;
            return true;
        }

        void TrySendCachedTournamentNotification(
            FParticipant& Participant) noexcept
        {
            if (!Participant.TournamentBootstrapSent ||
                !Participant.TournamentIdentityCached ||
                Participant.TournamentNotificationSent ||
                Participant.TournamentPresentationFallbackOwned ||
                GSession.TournamentNotificationUnavailable ||
                !IsCurrentCanonicalArenaSession() ||
                !ArenaTelemetryPolicy::CanNotifyMatchEntryForPlaylist(
                    GSession.Playlist))
            {
                return;
            }

            const uint64_t CaptureGeneration =
                GCaptureStateGeneration.load(std::memory_order_acquire);
            if (!ArenaTelemetryPolicy::IsTournamentRuntimeEffectEnabled(
                    ArenaTelemetryPolicy::ETournamentRuntimeEffect::
                        SessionLocalPresentation,
                    GCaptureEnabled.load(std::memory_order_acquire)) ||
                Participant.TournamentCaptureGeneration !=
                    CaptureGeneration)
            {
                InvalidatePendingTournamentIdentity(Participant);
                return;
            }

            const ULONGLONG Now = GetTickCount64();
            if (Now < Participant.NextTournamentNotificationAtMs)
                return;

            const bool PrerequisitesReady =
                HasReadyMatchEntryChannel(Participant) &&
                HasLoadedAcknowledgedMatchEntryPawn(Participant);
            const bool WasReadinessObserved =
                Participant.TournamentReadinessObserved;
            if (!ArenaTelemetryPolicy::AdvanceMatchEntryReadiness(
                    PrerequisitesReady,
                    Now,
                    &Participant.TournamentReadinessObserved,
                    &Participant.TournamentReadinessStableSinceMs))
            {
                if (!WasReadinessObserved &&
                    Participant.TournamentReadinessObserved)
                {
                    SDK::DbgLog(
                        "[ArenaTelemetry] match-entry client ready; stabilizing account=%s savedHype=%d delayMs=%llu\n",
                        Participant.AccountId,
                        Participant.TournamentSavedHype,
                        static_cast<unsigned long long>(
                            ArenaTelemetryPolicy::
                                kMatchEntryReadinessStabilizationMs));
                }
                Participant.NextTournamentNotificationAtMs = Now + 100;
                return;
            }

            // Prime the native route while the player is still on spawn
            // island, but do not invoke ClientNotifyMatchEntered until the
            // aircraft/first-award trigger owns the fare. Readiness requires a
            // stable second on some releases; starting it only at the trigger
            // would always lose the bounded 250 ms handoff to the bridge.
            if (!ArenaTelemetryPolicy::
                    CanAttemptArenaMatchEntryNotification(
                        Participant.TournamentEntryFeeVisualRequested,
                        Participant.TournamentEntryFeeStageComplete))
            {
                return;
            }

            const auto Result = TryNotifyMatchEntered(
                Participant, Participant.TournamentIdentity);
            if (Result == ETournamentNotificationResult::NotReady)
            {
                // The HTTP result is still current. Retry only the cheap local
                // ownership/channel check instead of issuing another request.
                Participant.NextTournamentNotificationAtMs = Now + 100;
                return;
            }
            if (Result == ETournamentNotificationResult::Sent)
            {
                SDK::DbgLog(
                    "[ArenaTelemetry] notified match entry account=%s savedHype=%d event=%s window=%s\n",
                    Participant.AccountId,
                    Participant.TournamentSavedHype,
                    Participant.TournamentIdentity.EventId,
                    Participant.TournamentIdentity.EventWindowId);
                Participant.TournamentNotificationSent = true;
                Participant.TournamentIdentityCached = false;
                Participant.TournamentIdentity = {};
                Participant.NextTournamentNotificationAtMs = 0;
                Participant.TournamentReadinessObserved = false;
                Participant.TournamentReadinessStableSinceMs = 0;
                TryAdvanceArenaPresentation(Participant);
                return;
            }

            // Once an owning channel exists, a missing/mismatched function is
            // a release-level schema problem rather than a join-timing issue.
            GSession.TournamentNotificationUnavailable = true;
            SDK::DbgLog(
                "[ArenaTelemetry] match-entry notification unavailable; schema rejected\n");
            TryAdvanceArenaPresentation(Participant);
        }

        void QueueTournamentLookup(
            FParticipant& Participant,
            bool AllowEnding = false) noexcept
        {
            if (Participant.TournamentLookupQueued ||
                Participant.TournamentIdentityCached ||
                Participant.TournamentNotificationSent ||
                Participant.TournamentPresentationFallbackOwned ||
                !IsCurrentCanonicalArenaSession(AllowEnding) ||
                !ArenaTelemetryPolicy::IsTournamentRuntimeEffectEnabled(
                    ArenaTelemetryPolicy::ETournamentRuntimeEffect::
                        SessionLocalPresentation,
                    GCaptureEnabled.load(std::memory_order_acquire)) ||
                !Participant.AccountId[0])
            {
                return;
            }
            const ULONGLONG Now = GetTickCount64();
            if (Now < Participant.NextTournamentLookupAtMs)
                return;

            FTournamentLookup Lookup{};
            Lookup.Controller = Participant.Controller;
            Lookup.PlayerState = Participant.PlayerState;
            Lookup.SessionOrdinal = GSession.Ordinal;
            Lookup.RequestId = ++GNextTournamentLookupRequestId;
            if (!Lookup.RequestId)
                Lookup.RequestId = ++GNextTournamentLookupRequestId;
            Lookup.CaptureGeneration =
                GCaptureStateGeneration.load(std::memory_order_acquire);
            Lookup.FortniteVersion = VersionInfo.FortniteVersion;
            Lookup.SavedProgressionEnabled =
                GCaptureEnabled.load(std::memory_order_acquire);
            CopyText(Lookup.AccountId, Participant.AccountId);
            if (TryPushSpsc(
                    GTournamentLookupQueue,
                    GTournamentLookupWrite,
                    GTournamentLookupRead,
                    Lookup))
            {
                Participant.TournamentLookupQueued = true;
                Participant.TournamentLookupRequestId = Lookup.RequestId;
                Participant.TournamentCaptureGeneration =
                    Lookup.CaptureGeneration;
            }
            else
            {
                Participant.NextTournamentLookupAtMs = Now + 1000;
            }
        }

        void ServiceArenaPresentation(
            FParticipant& Participant,
            bool AllowEnding) noexcept
        {
            // Bootstrap establishes the client's exact session/score state.
            // Never invoke the native match-entry RPC before it: without that
            // correlation scope, the adjacent B repair could be shown twice.
            if (TrySendArenaBootstrap(Participant))
            {
                // Before the gameplay trigger this only prewarms the cached
                // native route. After the trigger it may send exactly once.
                TrySendCachedTournamentNotification(Participant);
                TryAdvanceArenaPresentation(Participant);
            }
            QueueTournamentLookup(Participant, AllowEnding);
        }

        void ApplyTournamentDeliveries() noexcept
        {
            FTournamentDelivery Delivery{};
            while (TryPopSpsc(
                GTournamentDeliveryQueue,
                GTournamentDeliveryWrite,
                GTournamentDeliveryRead,
                Delivery))
            {
                if (!GSession.Active ||
                    Delivery.SessionOrdinal != GSession.Ordinal)
                {
                    continue;
                }
                const size_t Index = FindParticipant(
                    Delivery.Controller, Delivery.PlayerState);
                if (Index == (std::numeric_limits<size_t>::max)())
                    continue;

                auto& Participant = GSession.Participants[Index];
                if (!Participant.TournamentLookupQueued ||
                    Participant.TournamentLookupRequestId !=
                        Delivery.RequestId)
                {
                    continue;
                }

                Participant.TournamentLookupQueued = false;
                Participant.TournamentLookupRequestId = 0;
                const uint64_t CaptureGeneration =
                    GCaptureStateGeneration.load(std::memory_order_acquire);
                if (Delivery.CaptureGeneration != CaptureGeneration ||
                    Participant.TournamentCaptureGeneration !=
                        Delivery.CaptureGeneration)
                {
                    InvalidatePendingTournamentIdentity(Participant);
                    continue;
                }
                if (!Delivery.Resolved ||
                    !Delivery.HasArenaEntryFee ||
                    !ArenaTelemetryPolicy::
                        IsTournamentRuntimeEffectEnabled(
                            ArenaTelemetryPolicy::
                                ETournamentRuntimeEffect::
                                    SessionLocalPresentation,
                            GCaptureEnabled.load(
                                std::memory_order_acquire)))
                {
                    InvalidatePendingTournamentIdentity(Participant);
                    Participant.NextTournamentLookupAtMs =
                        GetTickCount64() + 2000;
                    continue;
                }

                Participant.TournamentIdentity = Delivery.Identity;
                Participant.TournamentSavedHype = Delivery.SavedHype;
                Participant.TournamentEntryFee = Delivery.ArenaEntryFee;
                Participant.TournamentEntryFeeKnown =
                    Delivery.HasArenaEntryFee;
                Participant.TournamentSavingEnabled =
                    GCaptureEnabled.load(std::memory_order_acquire);
                if (!Participant.TournamentBootstrapSent)
                {
                    Participant.TournamentLocalHype = static_cast<int32>(
                        ArenaTelemetryPolicy::
                            ResolveSessionLocalStartingHype(
                                Participant.TournamentSavingEnabled,
                                Delivery.SavedHype));
                    Participant.TournamentLocalStateInitialized = true;
                }
                Participant.TournamentIdentityCached = true;
                Participant.TournamentCaptureGeneration =
                    Delivery.CaptureGeneration;
                Participant.NextTournamentNotificationAtMs = 0;
                ServiceArenaPresentation(Participant);
            }
        }

        void RegisterCurrentPlayers() noexcept
        {
            auto World = UWorld::GetWorld();
            auto Driver = World
                ? static_cast<UNetDriver*>(World->NetDriver)
                : nullptr;
            if (Driver)
            {
                for (int Index = 0;
                     Index < Driver->ClientConnections.Num();
                     ++Index)
                {
                    auto Connection =
                        Driver->ClientConnections[Index];
                    auto Controller = Connection &&
                        Connection->PlayerController
                            ? Connection->PlayerController->
                                Cast<AFortPlayerControllerAthena>()
                            : nullptr;
                    if (Controller)
                        RegisterPlayer(Controller);
                }
            }

            if (!GSession.GameMode ||
                !GSession.GameMode->HasAlivePlayers())
            {
                return;
            }
            for (auto Actor : GSession.GameMode->AlivePlayers)
            {
                auto Controller = Actor
                    ? Actor->Cast<AFortPlayerControllerAthena>()
                    : nullptr;
                if (Controller)
                    RegisterPlayer(Controller);
            }
        }

        bool TryCompleteArenaPresentationEnd() noexcept
        {
            if (!GSession.Active || !GSession.Ending)
                return true;

            auto* CurrentWorld = UWorld::GetWorld();
            if (!CurrentWorld || !CurrentWorld->AuthorityGameMode ||
                !CurrentWorld->GameState)
            {
                // A temporary world gap is retryable. Do not retire the only
                // state that can still deliver END when ownership reappears.
                return false;
            }
            if (CurrentWorld->AuthorityGameMode != GSession.GameMode ||
                CurrentWorld->GameState != GSession.GameState)
            {
                // Travel already retired this generation. The client resets
                // its exact owned widget on world change, so no terminal RPC
                // from the stale session may target the new world.
                return true;
            }
            if (!IsCurrentCanonicalArenaSession(true))
                return false;

            RegisterCurrentPlayers();
            bool Complete = true;
            for (auto& Participant : GSession.Participants)
            {
                if (Participant.TournamentEndSent)
                    continue;

                PopulateParticipantIdentity(Participant, true);
                QueueTournamentLookup(Participant, true);
                // Sending mutates each ordered stage only after ClientMessage
                // succeeds. A failed attempt therefore retries the same head
                // event and can neither duplicate nor overtake an award.
                TryAdvanceArenaPresentation(Participant);
                if (!TrySendArenaPresentationEnd(Participant))
                {
                    Complete = false;
                    continue;
                }
                WriteArenaVisualDiagnostic(
                    "[ArenaVisual] match end bridge sent account=%s "
                    "bootstrap=%d pending=%zu\n",
                    Participant.AccountId,
                    Participant.TournamentBootstrapSent ? 1 : 0,
                    Participant.PendingPresentationEventCount);
            }
            return Complete;
        }

        bool TryQueueStart() noexcept
        {
            if (!GSession.Active || GSession.StartQueued)
                return GSession.StartQueued;

            auto Message = BuildSessionMessage(
                EMessageKind::Start, nullptr);
            if (!QueueSessionMessage(Message))
                return false;
            GSession.StartQueued = true;
            SDK::DbgLog(
                "[ArenaTelemetry] match session queued id=%s playlist=%s revision=%llu\n",
                GSession.SessionId,
                GSession.Playlist,
                static_cast<unsigned long long>(
                    GCaptureRevision.load(
                        std::memory_order_acquire)));
            return true;
        }

        void CompleteQueuedEnd() noexcept
        {
            SDK::DbgLog(
                "[ArenaTelemetry] match end queued id=%s sequence=%llu\n",
                GSession.SessionId,
                static_cast<unsigned long long>(
                    GSession.NextSequence));
            const uint64_t Ordinal = GSession.Ordinal;
            GSession = {};
            GSession.Ordinal = Ordinal;
        }

        bool TryQueueAbort() noexcept
        {
            if (!GSession.Active || !GSession.Corrupted)
                return true;
            if (!GSession.StartQueued && !TryQueueStart())
                return false;

            auto Message = BuildSessionMessage(
                EMessageKind::Event, "session_abort");
            if (!QueueSessionMessage(Message))
                return false;

            SDK::DbgLog(
                "[ArenaTelemetry] match abort queued id=%s sequence=%llu\n",
                GSession.SessionId,
                static_cast<unsigned long long>(
                    GSession.NextSequence));
            const uint64_t Ordinal = GSession.Ordinal;
            GSession = {};
            GSession.Ordinal = Ordinal;
            return true;
        }

        bool TryQueueEnd() noexcept
        {
            if (!GSession.Active || !GSession.Ending)
                return true;
            if (GSession.Corrupted)
            {
                return TryQueueAbort();
            }
            if (!GSession.StartQueued && !TryQueueStart())
                return false;

            auto Message = BuildSessionMessage(
                EMessageKind::End, "match_end");
            if (!QueueSessionMessage(Message))
                return false;

            CompleteQueuedEnd();
            return true;
        }

        bool BeginMatchSession(
            AFortGameMode* GameMode,
            AFortGameStateAthena* GameState) noexcept
        {
            if (!GameMode || !GameState || GSession.Active)
                return false;

            const auto Playlist =
                AFortGameMode::GetActivePlaylist(GameState);
            if (!Playlist)
                return false;
            const std::string ObjectName =
                Playlist->Name.ToString().c_str();
            const char* CanonicalPath = nullptr;
            if (!ArenaTelemetryPolicy::ResolveCanonicalPlaylist(
                    ObjectName.c_str(),
                    VersionInfo.FortniteVersion,
                    &CanonicalPath))
            {
                return false;
            }

            GSession = {};
            GSession.Active = true;
            GSession.GameMode = GameMode;
            GSession.GameState = GameState;
            GSession.Ordinal = ++GNextSessionOrdinal;
            GSession.Participants.reserve(128);
            CopyText(GSession.Playlist, CanonicalPath);
            sprintf_s(
                GSession.SessionId,
                "match-%08llx",
                static_cast<unsigned long long>(GSession.Ordinal));

            // Keep the session and player snapshot even if the bounded ring
            // is momentarily full. Tick retries the start in order instead of
            // losing the entire match generation.
            TryQueueStart();
            RegisterCurrentPlayers();
            return true;
        }

        bool TryBeginPendingMatch() noexcept
        {
            if (GSession.Active || !GPendingMatchStart.Pending)
                return false;

            const FPendingMatchStart Pending = GPendingMatchStart;
            GPendingMatchStart = {};
            return BeginMatchSession(
                Pending.GameMode, Pending.GameState);
        }
    }

    int GetPlacementPointsForPlayersRemaining(
        int PlayersRemaining) noexcept
    {
        return ArenaTelemetryPolicy::GetPlacementPoints(
            PlayersRemaining);
    }

    bool ResolveCanonicalArenaPlaylist(
        const char* ActivePlaylistObjectName,
        double FortniteVersion,
        const char** CanonicalPath) noexcept
    {
        return ArenaTelemetryPolicy::ResolveCanonicalPlaylist(
            ActivePlaylistObjectName,
            FortniteVersion,
            CanonicalPath);
    }

    bool IsCanonicalArenaMatch(
        AFortGameMode* GameMode,
        AFortGameStateAthena* GameState) noexcept
    {
        if (!GameMode || !GameState)
            return false;
        const auto* Playlist =
            AFortGameMode::GetActivePlaylist(GameState);
        if (!Playlist)
            return false;
        const std::string ObjectName =
            Playlist->Name.ToString().c_str();
        return ArenaTelemetryPolicy::ResolveCanonicalPlaylist(
            ObjectName.c_str(), VersionInfo.FortniteVersion, nullptr);
    }

    bool NotifyTournamentEliminationVisual(
        AFortPlayerControllerAthena* KillerPlayerController,
        AFortPlayerStateAthena* KillerPlayerState,
        int CurrentPlacement) noexcept
    {
        static uint32 UnsupportedLogs = 0;
        static uint32 ProcessFailureLogs = 0;
        static uint32 SuccessLogs = 0;

        const bool IsCanonicalArena = GSession.Active;
        if (!KillerPlayerState ||
            !ArenaTelemetryPolicy::IsTournamentRuntimeEffectEnabled(
                ArenaTelemetryPolicy::ETournamentRuntimeEffect::
                    SessionLocalPresentation,
                GCaptureEnabled.load(std::memory_order_acquire)))
        {
            return false;
        }

        __try
        {
            const bool HasTeamKillScore =
                KillerPlayerState->HasTeamKillScore();
            const int AbsoluteTeamEliminations =
                ArenaTelemetryPolicy::ResolveTeamEliminationVisualValue(
                    HasTeamKillScore,
                    HasTeamKillScore
                        ? KillerPlayerState->TeamKillScore
                        : 0,
                    KillerPlayerState->GetEffectiveKillScore());
            const auto Visual =
                ArenaTelemetryPolicy::BuildTeamEliminationVisual(
                    AbsoluteTeamEliminations);
            const auto Result = TryNotifyTournamentStatVisual(
                KillerPlayerState, Visual, true);
            if (Result == ETournamentStatVisualResult::Sent)
            {
                if (SuccessLogs++ < 4)
                {
                    WriteArenaVisualDiagnostic(
                        "[ArenaVisual] elimination presentation sent absolute=%d version=%.2f\n",
                        Visual.AbsoluteValue,
                        VersionInfo.FortniteVersion);
                }
                return true;
            }

            if (ArenaTelemetryPolicy::
                    ShouldFallbackEliminationPresentation(
                        IsCanonicalArena, Result) &&
                KillerPlayerController && CurrentPlacement >= 0)
            {
                // The primary schema was rejected before ProcessEvent began.
                // Consequently this exact +20 placement notification cannot
                // duplicate a stat dispatch. A ProcessEvent fault deliberately
                // does not fall back because the one-way RPC may already have
                // been queued. The single durable +20 event remains
                // RecordCreditedElimination below the caller.
                const auto FallbackResult =
                    TryNotifyTournamentPlacementVisual(
                        KillerPlayerController,
                        CurrentPlacement,
                        kEliminationPoints);
                static uint32 FallbackLogs = 0;
                if (FallbackLogs++ < 8)
                {
                    WriteArenaVisualDiagnostic(
                        "[ArenaVisual] elimination fallback placement=%d points=%d primary=%s fallback=%s version=%.2f\n",
                        CurrentPlacement,
                        kEliminationPoints,
                        Result == ETournamentStatVisualResult::
                            ProcessEventFailed
                                ? "ProcessEvent-failed"
                                : "schema-unsupported",
                        FallbackResult == ETournamentStatVisualResult::Sent
                            ? "sent"
                            : (FallbackResult ==
                                ETournamentStatVisualResult::
                                    ProcessEventFailed
                                    ? "ProcessEvent-failed"
                                    : "schema-unsupported"),
                        VersionInfo.FortniteVersion);
                }
                if (FallbackResult == ETournamentStatVisualResult::Sent)
                    return true;
            }

            uint32* LogCount =
                Result == ETournamentStatVisualResult::ProcessEventFailed
                    ? &ProcessFailureLogs
                    : &UnsupportedLogs;
            if ((*LogCount)++ < 4)
            {
                WriteArenaVisualDiagnostic(
                    Result == ETournamentStatVisualResult::ProcessEventFailed
                        ? "[ArenaVisual] tournament stat ProcessEvent failed closed version=%.2f\n"
                        : "[ArenaVisual] tournament stat schema rejected version=%.2f\n",
                    VersionInfo.FortniteVersion);
            }
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            if (ProcessFailureLogs++ < 4)
            {
                WriteArenaVisualDiagnostic(
                    "[ArenaVisual] tournament stat reflection faulted closed version=%.2f\n",
                    VersionInfo.FortniteVersion);
            }
            return false;
        }
    }

    bool NotifyTournamentPlacementVisual(
        AFortPlayerControllerAthena* PlayerController,
        int Placement,
        int PointsEarned,
        bool* DedicatedRpcExpected,
        bool AllowTypedStatFallback) noexcept
    {
        if (DedicatedRpcExpected)
            *DedicatedRpcExpected = false;
        if (!PlayerController ||
            !ArenaTelemetryPolicy::IsTournamentRuntimeEffectEnabled(
                ArenaTelemetryPolicy::ETournamentRuntimeEffect::
                    SessionLocalPresentation,
                GCaptureEnabled.load(std::memory_order_acquire)) ||
            Placement <= 0 ||
            !ArenaTelemetryPolicy::IsValidPointsDelta(PointsEarned))
        {
            return false;
        }

        // Prefer the dedicated placement-points RPC whenever its live
        // reflected contract is present. Some clients accept a generic
        // PLACEMENT_STAT_INDEX update without presenting the placement award
        // badge, while this RPC drives that dedicated presentation path.
        const auto PrimaryResult = TryNotifyTournamentPlacementVisual(
            PlayerController, Placement, PointsEarned);
        if (DedicatedRpcExpected &&
            PrimaryResult != ETournamentStatVisualResult::SchemaUnsupported)
        {
            // ProcessEvent failures are intentionally ambiguous: the one-way
            // RPC may already be queued. Let the client observe the exact
            // payload before deciding whether its placement repair is needed.
            *DedicatedRpcExpected = true;
        }
        if (PrimaryResult == ETournamentStatVisualResult::Sent)
        {
            static uint32 PrimarySuccessLogs = 0;
            if (PrimarySuccessLogs++ < 8)
            {
                WriteArenaVisualDiagnostic(
                    "[ArenaVisual] placement dedicated sent placement=%d points=%d version=%.2f\n",
                    Placement,
                    PointsEarned,
                    VersionInfo.FortniteVersion);
            }
            return true;
        }

        // A one-way ProcessEvent may have queued the packet before faulting.
        // Stop there so a second RPC can never double-present this milestone.
        if (!ArenaTelemetryPolicy::ShouldFallbackPlacementToTypedStat(
                PrimaryResult))
        {
            static uint32 ProcessFailureLogs = 0;
            if (ProcessFailureLogs++ < 4)
            {
                WriteArenaVisualDiagnostic(
                    "[ArenaVisual] placement dedicated ProcessEvent failed closed placement=%d points=%d version=%.2f\n",
                    Placement,
                    PointsEarned,
                    VersionInfo.FortniteVersion);
            }
            return false;
        }

        // Placement milestones emitted by RecordPlacementMilestone have an
        // authoritative private client bridge. Do not also send a typed-stat
        // fallback there: it cannot be correlated by exact point payload and
        // may already render on older clients. Other callers retain the
        // legacy typed-stat fallback through the default argument.
        if (!AllowTypedStatFallback)
            return false;

        // The dedicated ABI was rejected before ProcessEvent began. Only in
        // that case use the separately reflected and validated typed-stat RPC.
        // This is presentation-only; RecordPlacementMilestone has already
        // staged the single durable placement event for this recipient.
        auto* PlayerState = PlayerController->HasPlayerState()
            ? static_cast<AFortPlayerStateAthena*>(
                PlayerController->PlayerState)
            : nullptr;
        const auto Visual =
            ArenaTelemetryPolicy::BuildPlacementVisual(Placement);
        const auto FallbackResult = TryNotifyTournamentStatVisual(
            PlayerState, Visual, false);
        static uint32 FallbackLogs = 0;
        if (FallbackLogs++ < 8)
        {
            WriteArenaVisualDiagnostic(
                "[ArenaVisual] placement typed fallback placement=%d points=%d result=%s version=%.2f\n",
                Placement,
                PointsEarned,
                FallbackResult == ETournamentStatVisualResult::Sent
                    ? "sent"
                    : (FallbackResult ==
                        ETournamentStatVisualResult::ProcessEventFailed
                            ? "ProcessEvent-failed"
                            : "schema-unsupported"),
                VersionInfo.FortniteVersion);
        }
        return FallbackResult == ETournamentStatVisualResult::Sent;
    }

    void NotifyArenaBusFareVisual(
        AFortGameMode* GameMode,
        AFortPlayerControllerAthena* PlayerController) noexcept
    {
        if (!IsGameThreadCall() || !GSession.Active ||
            GSession.Ending || GSession.Corrupted ||
            GameMode != GSession.GameMode || !PlayerController ||
            !IsCurrentCanonicalArenaSession() ||
            !ArenaTelemetryPolicy::IsTournamentRuntimeEffectEnabled(
                ArenaTelemetryPolicy::ETournamentRuntimeEffect::
                    SessionLocalPresentation,
                GCaptureEnabled.load(std::memory_order_acquire)))
        {
            return;
        }

        auto* PlayerState = PlayerController->HasPlayerState()
            ? static_cast<AFortPlayerStateAthena*>(
                PlayerController->PlayerState)
            : nullptr;
        const size_t ParticipantIndex =
            EnsureParticipant(PlayerController, PlayerState);
        if (ParticipantIndex ==
            (std::numeric_limits<size_t>::max)())
        {
            return;
        }
        auto& Participant = GSession.Participants[ParticipantIndex];
        if (Participant.TournamentEntryFeeVisualRequested ||
            Participant.TournamentEntryFeeStageComplete)
        {
            return;
        }

        Participant.TournamentEntryFeeVisualRequested = true;
        Participant.TournamentNotificationWaitStartedAtMs =
            GetTickCount64();
        static uint32 RequestLogs = 0;
        if (RequestLogs++ < 8)
        {
            WriteArenaVisualDiagnostic(
                "[ArenaVisual] bus fare trigger queued account=%s version=%.2f\n",
                Participant.AccountId,
                VersionInfo.FortniteVersion);
        }
        // A lookup may already have completed on spawn island. Now that the
        // aircraft edge owns presentation, invoke the cached native route and
        // emit the ordered B frame in the same game-thread pass.
        ServiceArenaPresentation(Participant);
    }

    void Tick() noexcept
    {
        if (!IsGameThreadCall())
            return;
        EnsureServerInstanceId();
        EnsureWorkerStarted();

        if (!GSession.Active)
        {
            // Retired-session deliveries can arrive after travel. Drain them
            // even without an active match so they cannot occupy the bounded
            // queue indefinitely.
            RecoverDroppedTournamentDeliveries();
            ApplyTournamentDeliveries();
            TryBeginPendingMatch();
            return;
        }
        if (!GSession.StartQueued && !TryQueueStart())
            return;

        if (GSession.Corrupted)
        {
            TryQueueAbort();
            return;
        }

        SynchronizeTournamentCaptureState();
        RecoverDroppedTournamentDeliveries();
        ApplyTournamentDeliveries();
        if (GSession.Ending)
        {
            if (TryCompleteArenaPresentationEnd() && TryQueueEnd())
                TryBeginPendingMatch();
            return;
        }
        for (auto& Participant : GSession.Participants)
        {
            QueueParticipantJoin(Participant);
            ServiceArenaPresentation(Participant);
        }
        if (GSession.Ending && TryQueueEnd())
            TryBeginPendingMatch();
    }

    void OnMatchStarted(
        AFortGameMode* GameMode,
        AFortGameStateAthena* GameState) noexcept
    {
        if (!IsGameThreadCall() || !GameMode || !GameState)
            return;
        EnsureServerInstanceId();
        EnsureWorkerStarted();

        if (GSession.Active && !GSession.Ending &&
            GSession.GameMode == GameMode &&
            GSession.GameState == GameState)
        {
            return;
        }
        if (GSession.Active)
        {
            GPendingMatchStart = {
                GameMode,
                GameState,
                true
            };
            GSession.Ending = true;
            // Make one safe terminal delivery attempt when this callback still
            // refers to the old world. A new generation must not be delayed;
            // world-reset cleanup is authoritative once it has already won.
            TryCompleteArenaPresentationEnd();
            if (!TryQueueEnd())
            {
                SDK::DbgLog(
                    "[ArenaTelemetry] could not close prior session before new generation\n");
            }
            if (!GSession.Active)
                TryBeginPendingMatch();
            return;
        }
        GPendingMatchStart = {};
        BeginMatchSession(GameMode, GameState);
    }

    void OnMatchEnded(
        AFortGameMode* GameMode,
        AFortGameStateAthena* GameState) noexcept
    {
        if (!IsGameThreadCall() || !GSession.Active)
            return;
        if ((GameMode && GSession.GameMode != GameMode) ||
            (GameState && GSession.GameState != GameState))
        {
            return;
        }
        if (GSession.Ending)
            return;

        // Give identities that became available during the final frame one
        // last registration pass only while this is still the current world
        // generation. A seamless-travel callback can carry retired pointers;
        // staged facts remain safe to commit without dereferencing them.
        auto CurrentWorld = UWorld::GetWorld();
        const bool CanSnapshotCurrentGeneration =
            CurrentWorld &&
            CurrentWorld->AuthorityGameMode == GSession.GameMode &&
            CurrentWorld->GameState == GSession.GameState;
        if (CanSnapshotCurrentGeneration)
        {
            RegisterCurrentPlayers();
            for (auto& Participant : GSession.Participants)
            {
                PopulateParticipantIdentity(Participant, true);
                QueueParticipantJoin(Participant);
            }
        }
        GSession.Ending = true;
        // Keep the generation alive while this world is still authoritative.
        // Tick retries bootstrap/fare/E/P/V and the exact terminal frame until
        // every connected participant has crossed the one-shot END barrier.
        if (TryCompleteArenaPresentationEnd())
            TryQueueEnd();
    }

    void RegisterPlayer(
        AFortPlayerControllerAthena* PlayerController) noexcept
    {
        if (!IsGameThreadCall() ||
            !GSession.Active ||
            !PlayerController)
        {
            return;
        }
        auto PlayerState = PlayerController->HasPlayerState()
            ? static_cast<AFortPlayerStateAthena*>(
                PlayerController->PlayerState)
            : nullptr;
        EnsureParticipant(PlayerController, PlayerState);
    }

    bool RecordCreditedElimination(
        AFortGameMode* GameMode,
        AFortPlayerStateAthena* KillerPlayerState,
        AFortPlayerStateAthena* VictimPlayerState,
        std::uint64_t VictimLifeId,
        bool MatchWasLive,
        bool VictimWasAliveParticipant) noexcept
    {
        if (!IsGameThreadCall() ||
            !GSession.Active ||
            GSession.Ending ||
            GSession.Corrupted ||
            !GSession.StartQueued ||
            GameMode != GSession.GameMode ||
            !IsCurrentCanonicalArenaSession() ||
            !KillerPlayerState ||
            KillerPlayerState == VictimPlayerState)
        {
            return false;
        }

        try
        {
            if (!ArenaTelemetryPolicy::ConsumeFinalizedVictimLife(
                    MatchWasLive,
                    VictimWasAliveParticipant,
                    VictimLifeId,
                    &GSession.CreditedVictimLives))
            {
                return false;
            }
        }
        catch (...)
        {
            MarkSessionCorrupted(
                "elimination_dedupe_allocation_failed");
            return false;
        }

        auto KillerController =
            FindController(KillerPlayerState);
        const size_t KillerIndex = EnsureParticipant(
            KillerController, KillerPlayerState);
        if (KillerIndex == (std::numeric_limits<size_t>::max)())
        {
            if (!KillerPlayerState->HasbIsABot() ||
                !KillerPlayerState->bIsABot)
            {
                MarkSessionCorrupted(
                    "missing_killer_registration");
            }
            return false;
        }

        size_t VictimIndex = (std::numeric_limits<size_t>::max)();
        if (VictimPlayerState)
        {
            VictimIndex = EnsureParticipant(
                FindController(VictimPlayerState),
                VictimPlayerState);
        }

        const auto& Killer = GSession.Participants[KillerIndex];
        if (!Killer.AccountId[0])
        {
            MarkSessionCorrupted(
                "missing_killer_identity");
            return false;
        }

        char KillerAccountId[256]{};
        char VictimAccountId[256]{};
        CopyText(KillerAccountId, Killer.AccountId);
        if (VictimIndex != (std::numeric_limits<size_t>::max)())
        {
            const auto& Victim =
                GSession.Participants[VictimIndex];
            if (Victim.AccountId[0])
                CopyText(VictimAccountId, Victim.AccountId);
            if (Victim.TeamResolved &&
                Killer.TeamResolved &&
                Victim.TeamIndex == Killer.TeamIndex)
            {
                // Do not persist friendly-fire/team-member deaths as Arena
                // eliminations even if another gameplay path supplied a
                // credited killer.
                return false;
            }
        }

        const int KillerTeamIndex = Killer.TeamIndex;
        const int KillerKills =
            KillerPlayerState->GetEffectiveKillScore();
        const int PlayersRemaining = GameMode->HasAlivePlayers()
            ? GameMode->AlivePlayers.Num()
            : 0;
        std::unordered_set<std::string> CreditedAccounts;
        size_t CreditedPlayers = 0;

        // The authored Arena rule is multiplicative over
        // TEAM_ELIMS_STAT_INDEX. Persist the +20 Hype for every joined human
        // on the killer's team, including a teammate who died earlier. Only
        // the killer recipient carries the personal elimination count.
        for (auto& Recipient : GSession.Participants)
        {
            if (!ArenaTelemetryPolicy::ShouldCreditTeamElimination(
                    KillerTeamIndex,
                    Recipient.TeamIndex))
            {
                continue;
            }
            if (!Recipient.JoinQueued &&
                !QueueParticipantJoin(Recipient))
            {
                MarkSessionCorrupted(
                    "missing_team_elimination_registration");
                return false;
            }
            if (!Recipient.AccountId[0])
            {
                MarkSessionCorrupted(
                    "missing_team_elimination_identity");
                return false;
            }
            if (!CreditedAccounts.insert(
                    Recipient.AccountId).second)
            {
                continue;
            }

            bool SavedProgressionStaged = true;
            if (ArenaTelemetryPolicy::ShouldStageSavedProgression(
                    GCaptureRevision.load(std::memory_order_acquire),
                    GCaptureEnabled.load(std::memory_order_acquire)))
            {
                auto Message = BuildSessionMessage(
                    EMessageKind::Event, "elimination");
                Message.Event.HasAccount = true;
                CopyText(
                    Message.Event.AccountId,
                    Recipient.AccountId);
                CopyText(
                    Message.Event.DisplayName,
                    Recipient.DisplayName);
                Message.Event.HasTeamIndex = true;
                Message.Event.TeamIndex = Recipient.TeamIndex;
                Message.Event.HasKillerAccount = true;
                CopyText(
                    Message.Event.KillerAccountId,
                    KillerAccountId);
                if (VictimAccountId[0])
                {
                    Message.Event.HasVictimAccount = true;
                    CopyText(
                        Message.Event.VictimAccountId,
                        VictimAccountId);
                }
                if (strcmp(
                        Recipient.AccountId,
                        KillerAccountId) == 0)
                {
                    Message.Event.HasKills = true;
                    Message.Event.Kills = KillerKills;
                }
                Message.Event.HasPlayersRemaining = true;
                Message.Event.PlayersRemaining = PlayersRemaining;
                Message.Event.HasPointsDelta = true;
                Message.Event.PointsDelta = kEliminationPoints;
                SavedProgressionStaged =
                    QueueSessionMessage(Message, true);
            }

            QueueOrSendArenaPresentationEvent(
                Recipient,
                ArenaTelemetryWire::EArenaPresentationEventKind::Elimination,
                kEliminationPoints,
                0,
                true);
            if (!SavedProgressionStaged)
                return false;
            ++CreditedPlayers;
        }

        if (!CreditedPlayers)
        {
            MarkSessionCorrupted(
                "missing_team_elimination_recipient");
            return false;
        }
        return true;
    }

    void RecordPointsAdjustment(
        AFortGameMode* GameMode,
        AFortPlayerControllerAthena* PlayerController,
        int PointsDelta) noexcept
    {
        if (!IsGameThreadCall() ||
            !GSession.Active ||
            GSession.Ending ||
            GSession.Corrupted ||
            !GSession.StartQueued ||
            GameMode != GSession.GameMode ||
            !PlayerController ||
            PointsDelta == 0)
        {
            return;
        }

        // Revision zero means the initial start handshake is still queued;
        // let that ordered handshake make the authoritative decision. Once a
        // capture policy has been observed, avoid producing command events
        // locally while capture is paused as well.
        if (GCaptureRevision.load(std::memory_order_acquire) > 0 &&
            !ArenaTelemetryPolicy::IsTournamentRuntimeEffectEnabled(
                ArenaTelemetryPolicy::ETournamentRuntimeEffect::
                    SavedProgression,
                GCaptureEnabled.load(std::memory_order_acquire)))
        {
            return;
        }
        if (!ArenaTelemetryPolicy::IsValidPointsDelta(PointsDelta))
        {
            MarkSessionCorrupted("manual_points_delta_out_of_range");
            return;
        }

        auto PlayerState = PlayerController->HasPlayerState()
            ? static_cast<AFortPlayerStateAthena*>(
                PlayerController->PlayerState)
            : nullptr;
        const size_t ParticipantIndex = EnsureParticipant(
            PlayerController, PlayerState);
        if (ParticipantIndex ==
            (std::numeric_limits<size_t>::max)())
        {
            MarkSessionCorrupted(
                "missing_manual_points_registration");
            return;
        }

        auto& Participant =
            GSession.Participants[ParticipantIndex];
        if (!Participant.JoinQueued &&
            !QueueParticipantJoin(Participant))
        {
            MarkSessionCorrupted(
                "missing_manual_points_identity");
            return;
        }

        auto Message = BuildSessionMessage(
            EMessageKind::Event, "manual_points_adjustment");
        Message.Event.HasAccount = true;
        CopyText(
            Message.Event.AccountId,
            Participant.AccountId);
        CopyText(
            Message.Event.DisplayName,
            Participant.DisplayName);
        Message.Event.HasTeamIndex = true;
        Message.Event.TeamIndex = Participant.TeamIndex;
        Message.Event.HasPlayersRemaining = true;
        Message.Event.PlayersRemaining =
            GameMode->HasAlivePlayers()
                ? GameMode->AlivePlayers.Num()
                : 0;
        Message.Event.HasPointsDelta = true;
        Message.Event.PointsDelta = PointsDelta;
        QueueSessionMessage(Message, true);
    }

    void RecordPlacementMilestone(
        AFortGameMode* GameMode,
        AFortGameStateAthena* GameState,
        int PlayersBeforeDeath,
        bool IsFinalizedLiveDeath,
        bool UseFallbackPresentation) noexcept
    {
        if (!IsGameThreadCall() ||
            !GSession.Active ||
            GSession.Ending ||
            GSession.Corrupted ||
            !GSession.StartQueued ||
            GameMode != GSession.GameMode ||
            GameState != GSession.GameState ||
            !IsCurrentCanonicalArenaSession() ||
            !GameMode ||
            !GameMode->HasAlivePlayers())
        {
            return;
        }

        const int PlayersRemaining =
            GameMode->AlivePlayers.Num();
        // The presentation transition begins at the progression cursor, not
        // necessarily the population immediately before this death. This is
        // what preserves every crossed authored tier in a small match or a
        // multi-removal frame; 101 is the intentional pre-progression sentinel.
        const int Points =
            ArenaTelemetryPolicy::ConsumeCrossedPlacementPoints(
                PlayersBeforeDeath,
                PlayersRemaining,
                IsFinalizedLiveDeath,
                &GSession.PlacementProgression);
        if (Points <= 0)
            return;

        // Deliberately mirror Magnesium's existing visible Arena award path:
        // thresholds are granted to controllers still in AlivePlayers, not
        // retroactively to an eliminated teammate in a surviving team.
        for (auto Actor : GameMode->AlivePlayers)
        {
            auto Controller = Actor
                ? Actor->Cast<AFortPlayerControllerAthena>()
                : nullptr;
            if (!Controller)
            {
                MarkSessionCorrupted(
                    "invalid_alive_player");
                break;
            }
            if (Controller->IsInRespawnCountdown())
            {
                continue;
            }
            auto PlayerState = Controller->HasPlayerState()
                ? static_cast<AFortPlayerStateAthena*>(
                    Controller->PlayerState)
                : nullptr;
            if (PlayerState &&
                PlayerState->HasbIsABot() &&
                PlayerState->bIsABot)
            {
                continue;
            }
            const size_t ParticipantIndex =
                EnsureParticipant(Controller, PlayerState);
            if (ParticipantIndex ==
                (std::numeric_limits<size_t>::max)())
            {
                MarkSessionCorrupted(
                    "missing_placement_registration");
                break;
            }

            auto& Participant =
                GSession.Participants[ParticipantIndex];
            if (!Participant.JoinQueued &&
                !QueueParticipantJoin(Participant))
            {
                MarkSessionCorrupted(
                    "missing_placement_registration");
                break;
            }
            if (!Participant.AccountId[0])
            {
                MarkSessionCorrupted(
                    "missing_placement_identity");
                break;
            }

            bool SavedProgressionStaged = true;
            if (ArenaTelemetryPolicy::ShouldStageSavedProgression(
                    GCaptureRevision.load(std::memory_order_acquire),
                    GCaptureEnabled.load(std::memory_order_acquire)))
            {
                auto Message = BuildSessionMessage(
                    EMessageKind::Event, "placement");
                Message.Event.HasAccount = true;
                CopyText(
                    Message.Event.AccountId,
                    Participant.AccountId);
                CopyText(
                    Message.Event.DisplayName,
                    Participant.DisplayName);
                Message.Event.HasTeamIndex = true;
                Message.Event.TeamIndex =
                    Participant.TeamIndex;
                Message.Event.HasPlacement = true;
                Message.Event.Placement = PlayersRemaining;
                Message.Event.HasPlayersRemaining = true;
                Message.Event.PlayersRemaining =
                    PlayersRemaining;
                Message.Event.HasPointsDelta = true;
                Message.Event.PointsDelta = Points;
                SavedProgressionStaged =
                    QueueSessionMessage(Message, true);
            }

            // Native TournamentStats normally owns presentation. Reflected
            // fallback is legal only when every bounded registration attempt
            // failed before ProcessEvent began, so it cannot overlap an
            // ambiguous or confirmed native modifier registration.
            bool DedicatedRpcExpected = false;
            if (UseFallbackPresentation)
            {
                NotifyTournamentPlacementVisual(
                    Controller, PlayersRemaining, Points,
                    &DedicatedRpcExpected, false);
            }

            QueueOrSendArenaPresentationEvent(
                Participant,
                PlayersRemaining == 1
                    ? ArenaTelemetryWire::
                        EArenaPresentationEventKind::Victory
                    : ArenaTelemetryWire::
                        EArenaPresentationEventKind::Placement,
                Points,
                PlayersRemaining,
                !UseFallbackPresentation || DedicatedRpcExpected);
            if (!SavedProgressionStaged)
                break;
        }
    }
}
