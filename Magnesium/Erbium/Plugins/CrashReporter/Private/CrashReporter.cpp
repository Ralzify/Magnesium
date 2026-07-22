#include "pch.h"
#include "../Public/CrashReporter.h"
#include "../../../PlayerAI/Public/PlayerAIFaultGuard.h"
#include <TlHelp32.h>
#include <winternl.h>
#include <sstream>
#pragma comment(lib, "ntdll.lib")

void FreezeOtherThreads()
{
    auto thrHandle = GetCurrentThread();
    auto currentThr = GetThreadId(thrHandle);
    auto currentPrc = GetProcessIdOfThread(thrHandle);
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (h != INVALID_HANDLE_VALUE)
    {
        THREADENTRY32 te;
        te.dwSize = sizeof(te);
        if (Thread32First(h, &te))
    {
            do {
                if (te.dwSize >= FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) +
                    sizeof(te.th32OwnerProcessID))
    {
                    if (te.th32ThreadID != currentThr && te.th32OwnerProcessID == currentPrc)
                    {
                        auto thr = OpenThread(THREAD_ALL_ACCESS, false, te.th32ThreadID);

                        if (thr != INVALID_HANDLE_VALUE)
                        {
                            SuspendThread(thr);
                            CloseHandle(thr);
                        }
                    }
                }
                te.dwSize = sizeof(te);
            } while (Thread32Next(h, &te));
        }
        CloseHandle(h);
    }
}

DWORD FormatNtStatus(NTSTATUS nsCode, TCHAR** ppszMessage) 
{
    HMODULE ntdll = LoadLibraryA("ntdll.dll");

    if (ntdll == NULL) 
        return 0;

    DWORD outLen = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_FROM_HMODULE,
        ntdll, RtlNtStatusToDosError(nsCode), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)ppszMessage, 0, NULL);

    FreeLibrary(ntdll);

    return outLen;
}

// Set once the CRT starts running atexit handlers. The 31.41 Sockets/FName fault is raised from
// that teardown (its handler is reachable ONLY via atexit -- zero call/data xrefs image-wide), so
// it is a consequence of an exit already in progress, not the cause of one.
std::atomic<bool> GInCrtExit{ false };

// Registered LAST, so it runs FIRST (atexit is LIFO) -- ahead of the engine's own handlers.
// exit()'s caller is still on the stack at this point, so the backtrace names whoever asked the
// process to quit. That is the actual bug; the access violation is downstream noise.
static void OnCrtExit()
{
    GInCrtExit.store(true, std::memory_order_release);

    void* Frames[32]{};
    const USHORT Count = CaptureStackBackTrace(0, 32, Frames, nullptr);

    SDK::DbgLog("[EXIT] process exit started — %u frames (who called exit):\n", Count);
    for (USHORT i = 0; i < Count; ++i)
    {
        const auto Addr = (uintptr_t)Frames[i];
        if (Addr >= ImageBase && Addr < ImageBase + 0x14000000)
            SDK::DbgLog("[EXIT]   %p  (FortniteClient +0x%llX)\n", (void*)Addr,
                (unsigned long long)(Addr - ImageBase));
        else
            SDK::DbgLog("[EXIT]   %p\n", (void*)Addr);
    }
}

LONG WINAPI ErbiumUnhandledExceptionFilter(LPEXCEPTION_POINTERS ExceptionInfo)
{
    if ((ExceptionInfo->ExceptionRecord->ExceptionCode & 0x80000000) == 0 || (ExceptionInfo->ExceptionRecord->ExceptionCode & 0x30000000) != 0)
        return EXCEPTION_CONTINUE_SEARCH;

    // A PlayerAI SEH guard is active on this thread and will contain this
    // fault (feature degrade instead of a crash) - let the frame handler
    // run. All other exceptions are reported exactly as before.
    if (GPlayerAIGuardedNativeCallDepth > 0)
        return EXCEPTION_CONTINUE_SEARCH;

    // This address is used by some Fortnite builds as an intentional probe.
    // Leave it to the OS/game before suspending any threads.
    if ((ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
         ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        ExceptionInfo->ExceptionRecord->ExceptionInformation[1] == 0xFFFFF78000000900)
        return EXCEPTION_CONTINUE_SEARCH;

    // Faults raised while the CRT is already running its onexit table are collateral: the process
    // was on its way out before this happened, so the interesting failure is whatever requested
    // the exit (logged as [EXIT] below). Reporting here also means FreezeOtherThreads() suspends
    // threads mid-teardown, which can deadlock the shutdown outright.
    if (GInCrtExit.load(std::memory_order_acquire))
    {
        SDK::DbgLog("[CrashReporter] fault at %p during process exit — secondary, see the [EXIT] backtrace above\n",
            (void*)ExceptionInfo->ExceptionRecord->ExceptionAddress);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    FreezeOtherThreads();

    STACKFRAME64 stackFrame{};

    char symName[1024 * sizeof(TCHAR)];
    char symStorage[sizeof(IMAGEHLP_SYMBOL64) + sizeof(symName)];
    auto sym = (IMAGEHLP_SYMBOL64*)symStorage;

    auto currentPrc = GetCurrentProcess();
    auto currentThr = GetCurrentThread();
    DWORD64 displacement = 0;
    stackFrame.AddrPC.Offset = ExceptionInfo->ContextRecord->Rip;
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrStack.Offset = ExceptionInfo->ContextRecord->Rsp;
    stackFrame.AddrStack.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Offset = ExceptionInfo->ContextRecord->Rbp;
    stackFrame.AddrFrame.Mode = AddrModeFlat;

    SymCleanup(currentPrc);

    auto InitResult = SymInitialize(currentPrc, nullptr, true);

    if (!InitResult)
        printf("[CrashReporter] Failed to initialize symbol finder!");

    std::stringstream reportStream;
    
    reportStream << "[CrashReporter] Caught unhandled exception (Code: ";
    char code[9];

    switch (ExceptionInfo->ExceptionRecord->ExceptionCode)
    {
#define NAMED_EX(x) case x: reportStream << #x; break;

        NAMED_EX(EXCEPTION_ACCESS_VIOLATION);
        NAMED_EX(EXCEPTION_ARRAY_BOUNDS_EXCEEDED);
        NAMED_EX(EXCEPTION_BREAKPOINT);
        NAMED_EX(EXCEPTION_DATATYPE_MISALIGNMENT);
        NAMED_EX(EXCEPTION_FLT_DENORMAL_OPERAND);
        NAMED_EX(EXCEPTION_FLT_DIVIDE_BY_ZERO);
        NAMED_EX(EXCEPTION_FLT_INEXACT_RESULT);
        NAMED_EX(EXCEPTION_FLT_INVALID_OPERATION);
        NAMED_EX(EXCEPTION_FLT_OVERFLOW);
        NAMED_EX(EXCEPTION_FLT_STACK_CHECK);
        NAMED_EX(EXCEPTION_FLT_UNDERFLOW);
        NAMED_EX(EXCEPTION_ILLEGAL_INSTRUCTION);
        NAMED_EX(EXCEPTION_INT_DIVIDE_BY_ZERO);
        NAMED_EX(EXCEPTION_INT_OVERFLOW);
        NAMED_EX(EXCEPTION_INVALID_DISPOSITION);
        NAMED_EX(EXCEPTION_NONCONTINUABLE_EXCEPTION);
        NAMED_EX(EXCEPTION_PRIV_INSTRUCTION);
        NAMED_EX(EXCEPTION_SINGLE_STEP);
        NAMED_EX(EXCEPTION_STACK_OVERFLOW);

#undef NAMED_EX
    default:
        snprintf(code, 9, "%08x", ExceptionInfo->ExceptionRecord->ExceptionCode);
        reportStream << code;
    }
    reportStream << ")";

    if (ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || ExceptionInfo->ExceptionRecord->ExceptionCode == EXCEPTION_IN_PAGE_ERROR)
    {
        switch (ExceptionInfo->ExceptionRecord->ExceptionInformation[0])
        {
        case 0:
            reportStream << "\n- Trying to read ";
            break;
        case 1:
            reportStream << "\n- Trying to write ";
            break;
        case 8:
            reportStream << "\n- Trying to execute ";
            break;
        }
        char addr[19];
        snprintf(addr, 19, "0x%016llx", ExceptionInfo->ExceptionRecord->ExceptionInformation[1]);
        reportStream << addr;
    }
    reportStream << "\n\n";

    for (int frame = 0;; frame++)
    {
        auto contextCpy = *ExceptionInfo->ContextRecord;
        contextCpy.ContextFlags = CONTEXT_ALL;

        auto result = StackWalk
        (
            IMAGE_FILE_MACHINE_AMD64,
            currentPrc,
            currentThr,
            &stackFrame,
            ExceptionInfo->ContextRecord,
            NULL,
            SymFunctionTableAccess64,
            SymGetModuleBase64,
            NULL
        );

        if (result == false)
            break;

        char addr[19];
        snprintf(addr, 19, "0x%016llx", stackFrame.AddrPC.Offset);
        reportStream << addr;

        auto imageBase = SymGetModuleBase64(currentPrc, stackFrame.AddrPC.Offset);
        if (imageBase)
        {
            char path[1024];
            GetModuleFileNameA(HMODULE(imageBase), path, 1024);

            auto filteredPath = strrchr(path, '\\');

            char offsetStr[19];
            snprintf(offsetStr, 19, "0x%llx", stackFrame.AddrPC.Offset - imageBase);
            reportStream << " (" << (filteredPath ? filteredPath + 1 : path) << "+" << offsetStr << ")";
        }
        reportStream << ": ";

        sym->SizeOfStruct = sizeof(symStorage);
        sym->MaxNameLength = sizeof(symName);
        
        BOOL SymResult = SymGetSymFromAddr64(currentPrc, (ULONG64)stackFrame.AddrPC.Offset, &displacement, sym);
        if (SymResult == false || imageBase == ImageBase)
            reportStream << "[unknown]\n";
        else
        {
            UnDecorateSymbolName(sym->Name, (PSTR)symName, sizeof(symName), UNDNAME_COMPLETE);

            reportStream << sym->Name << "()";
            IMAGEHLP_LINE64	ImageHelpLine = { 0 };
            ImageHelpLine.SizeOfStruct = sizeof(ImageHelpLine);
            if (SymGetLineFromAddr64(currentPrc, (ULONG64)stackFrame.AddrPC.Offset, (::DWORD*)&displacement, &ImageHelpLine))
            {
                auto filteredName = strrchr(ImageHelpLine.FileName, '\\');
                reportStream << " [" << (filteredName ? filteredName + 1 : ImageHelpLine.FileName) << ":" << ImageHelpLine.LineNumber << "]";
            }
            reportStream << "\n";
        }
    }
    auto reportStr = reportStream.str();
    printf("%s", reportStr.c_str());
    // stdout is not captured in most launch configurations, which makes post-mortem diagnosis
    // guesswork. DbgLog fopen/fflush/fcloses per call, so this survives the crash.
    SDK::DbgLog("\n=== CRASH REPORT ===\n%s\n=== END CRASH REPORT ===\n", reportStr.c_str());

    Memcury::Util::CopyToClipboard(reportStr);
    SymCleanup(currentPrc);
    Sleep(3000);
    //while (true) {}
    TerminateProcess(GetCurrentProcess(), ExceptionInfo->ExceptionRecord->ExceptionCode);
    //ExitProcess(ExceptionInfo->ExceptionRecord->ExceptionCode);
    // If TerminateProcess unexpectedly fails, tell Windows to perform its normal
    // unhandled-exception termination instead of attempting to resume at the
    // faulting instruction.
    return EXCEPTION_EXECUTE_HANDLER;
}

namespace
{
    // CH5/UE5.5: an EOS lobby/session event broadcasts a multicast delegate whose invocation
    // list still holds a listener bound to an already-destroyed object. The engine's own
    // "entry is dead" check (cmp [entry+8], 0) does not catch it, so Broadcast dereferences a
    // dangling pointer to read the listener's vtable and faults.
    //
    // First attempt resumed at Broadcast's own "entry is dead" branch, which skips the entry and
    // continues the loop. That was not enough: the crash simply moved one frame deeper, into the
    // delegate invocation itself, reading the *same* dead address. So the dead object is reachable
    // from more than just the invocation-list slot, and skipping entries is whack-a-mole.
    //
    // Instead abandon the entire broadcast: resume at the function's own epilogue
    // (dec [rdi+0x14] -- release the reentrancy counter, restore, return). At the faulting
    // instruction rdi (the delegate) and r14 (the argument) are both still live and rsp is
    // unchanged since the prologue, so that path unwinds correctly. bpl is cleared first so the
    // epilogue skips the compaction call, which would walk the same poisoned list.
    //
    // This drops one EOS session/lobby notification per occurrence. That is acceptable here: the
    // private server does not use EOS, and the alternative is process death.
    //
    //   mov  rax, [rdi]              ; invocation list Data
    //   cmp  dword [rbx+rax+8], 0    ; entry empty?
    //   je   <bail>                  ; -> mov bpl,1 ; jmp loop_next   (needs-compaction path)
    //   mov  rcx, [rbx+rax]          ; bound object
    //   test rcx, rcx / je <bail>
    //   mov  rax, [rcx]              ; <-- faults here on a dangling listener
    //   mov  r8,  [rax+0x60]
    //
    // Broadcast is a template, so every instantiation gets its own copy (8 on 31.41, 1 on
    // 32.11); all of them are registered.
    struct FBroadcastSite { uintptr_t Fault; uintptr_t Bail; };
    std::vector<FBroadcastSite> GBroadcastSites;
    PVOID GVehHandle = nullptr;

    // --- FNamePool::Resolve guard -------------------------------------------------------------
    //
    // Second, unrelated background-thread fault, on the Sockets thread. FNamePool::Resolve is:
    //     BlockIdx = id >> 16;  Offset = id & 0xFFFF;
    //     return Blocks[BlockIdx] + Offset * 2;
    // with no bounds check. When a subsystem stringifies an uninitialised FName the BlockIdx is
    // out of range, Blocks[BlockIdx] reads past the array, and the caller faults decoding the
    // entry header. Observed reads: 0x15000, 0x55000, 0x100015000 -- same low bits (Offset*2),
    // garbage high bits (the bogus block base). Confirmed present in BOTH the dedicated and
    // listen-server models, so it is pre-existing and not caused by our netmode change.
    //
    // The decode idiom is  mov rbx,rax / movzx edx,[rax] / shr edx,6  (Len = Header >> 6) and
    // appears at ~40 call sites. The earlier delegate guard taught the lesson here: patching one
    // instruction just moves the fault, so every site of the class is registered.
    //
    // On a fault we point rax (and rbx, which the preceding instruction already copied) at a
    // zeroed entry: header 0 => Len 0 => the caller sees an empty name instead of dying.
    alignas(8) const uint8_t GEmptyNameEntry[16] = { 0 };
    std::vector<uintptr_t> GNameDecodeSites;

    LONG CALLBACK DelegateBroadcastGuard(EXCEPTION_POINTERS* Info)
    {
        // Decline everything that is not an exact match for a known faulting instruction.
        // A vectored handler sees first-chance exceptions, and some builds raise/catch access
        // violations on purpose, so this must never generalise.
        if (!Info || !Info->ExceptionRecord || !Info->ContextRecord ||
            Info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
            return EXCEPTION_CONTINUE_SEARCH;

        const auto Rip = (uintptr_t)Info->ContextRecord->Rip;
        for (const auto& Site : GBroadcastSites)
        {
            if (Rip != Site.Fault)
                continue;

            static int Logged = 0;
            if (Logged++ < 8)
                SDK::DbgLog("[CrashGuard] dangling delegate listener at %p (read %p) -> abandoning "
                            "broadcast via epilogue %p\n",
                    (void*)Rip, (void*)Info->ExceptionRecord->ExceptionInformation[1],
                    (void*)Site.Bail);

            // Clear bpl so the epilogue's "needs compaction" branch is not taken.
            Info->ContextRecord->Rbp &= ~0xFFull;
            Info->ContextRecord->Rip = Site.Bail;
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        for (auto Site : GNameDecodeSites)
        {
            if (Rip != Site)
                continue;

            static int NameLogged = 0;
            if (NameLogged++ < 8)
                SDK::DbgLog("[CrashGuard] bad FName entry at %p (resolved %p) -> substituting empty name\n",
                    (void*)Rip, (void*)Info->ExceptionRecord->ExceptionInformation[1]);

            // Re-executes the faulting load against a valid zeroed entry (Len 0).
            Info->ContextRecord->Rax = (DWORD64)(uintptr_t)GEmptyNameEntry;
            Info->ContextRecord->Rbx = (DWORD64)(uintptr_t)GEmptyNameEntry;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void InstallDelegateBroadcastGuard()
    {
        // NOTE: Register() runs before SDK::Init(), so VersionInfo is still zeroed here --
        // do not gate on EngineVersion. Gating is unnecessary anyway: the handler declines
        // every exception except an exact match on one of the instruction addresses resolved
        // below, and this byte sequence is a UE5-era encoding that will not match older builds.
        // Scanning is safe this early because it only needs the PE headers, not the SDK.
        // Escape hatch: drop a file named "magnesium_noguards.txt" next to the exe to run with the
        // VEH guards off. Needed to tell whether a guard is preventing a crash or causing a later
        // one -- abandoning a broadcast mid-flight leaves engine state the caller never expects.
        if (GetFileAttributesW(L"magnesium_noguards.txt") != INVALID_FILE_ATTRIBUTES)
        {
            SDK::DbgLog("[CrashGuard] disabled by magnesium_noguards.txt\n");
            return;
        }

        const char* Sig = "48 8B 07 83 7C 03 08 00 74 ? 48 8B 0C 03 48 85 C9 74 ? 48 8B 01 4C 8B 40 60";

        (void)Sig; // documented above; matched byte-wise below so every copy is found

        auto Section = Memcury::PE::Section::GetSection(".text");
        auto Start = Section.GetSectionStart().Get();
        auto End = Start + Section.GetSectionSize();
        const uint8_t Bytes[] = { 0x48,0x8B,0x07,0x83,0x7C,0x03,0x08,0x00,0x74,0x00,
                                  0x48,0x8B,0x0C,0x03,0x48,0x85,0xC9,0x74,0x00,
                                  0x48,0x8B,0x01,0x4C,0x8B,0x40,0x60 };
        const bool Wild[] = { false,false,false,false,false,false,false,false,false,true,
                              false,false,false,false,false,false,false,false,true,
                              false,false,false,false,false,false,false };
        constexpr size_t Len = sizeof(Bytes);

        for (auto P = Start; P + Len < End; ++P)
        {
            auto Ptr = (const uint8_t*)P;
            bool Match = true;
            for (size_t i = 0; i < Len; ++i)
                if (!Wild[i] && Ptr[i] != Bytes[i]) { Match = false; break; }
            if (!Match)
                continue;

            const auto Fault = P + 0x13;                     // the 'mov rax,[rcx]' that faults

            // Find this instantiation's epilogue: 'dec dword ptr [rdi+0x14]' (FF 4F 14), the
            // reentrancy-counter release that begins the clean return path.
            uintptr_t Epilogue = 0;
            for (uintptr_t i = 0; i < 0x120; ++i)
            {
                auto Q = (const uint8_t*)(Fault + i);
                if (Q[0] == 0xFF && Q[1] == 0x4F && Q[2] == 0x14)
                {
                    Epilogue = Fault + i;
                    break;
                }
            }
            if (!Epilogue)
                continue; // cannot unwind this one safely -- leave it alone

            GBroadcastSites.push_back({ Fault, Epilogue });
        }

        // The bare decode idiom (48 8B D8 / 0F B7 10 / C1 EA 06) occurs 64 times in .text --
        // arming all of them would convert genuine name-pool corruption into empty strings
        // engine-wide. Qualify it with the specific call site instead:
        //   8B 4E 02     mov  ecx,[rsi+2]        ; inner FNameEntryId of a numbered name
        //   E8 rel32     call FNamePool::Resolve
        //   48 8B CF     mov  rcx,rdi
        //   48 8B D8     mov  rbx,rax
        //   0F B7 10     movzx edx,[rax]         ; <- faults, at +0x0E
        //   C1 EA 06     shr  edx,6
        // That is unique (1 hit) on 31.41. On builds where it matches nothing, nothing is armed.
        const uint8_t NameBytes[] = { 0x8B,0x4E,0x02, 0xE8,0,0,0,0, 0x48,0x8B,0xCF,
                                      0x48,0x8B,0xD8, 0x0F,0xB7,0x10, 0xC1,0xEA,0x06 };
        const bool NameWild[]     = { 0,0,0,        0,1,1,1,1,      0,0,0,
                                      0,0,0,        0,0,0,          0,0,0 };
        for (auto P = Start; P + sizeof(NameBytes) < End; ++P)
        {
            auto Ptr = (const uint8_t*)P;
            bool Match = true;
            for (size_t i = 0; i < sizeof(NameBytes); ++i)
                if (!NameWild[i] && Ptr[i] != NameBytes[i]) { Match = false; break; }
            if (Match)
                GNameDecodeSites.push_back(P + 0x0E);
        }

        if (GBroadcastSites.empty() && GNameDecodeSites.empty())
        {
            SDK::DbgLog("[CrashGuard] no guard sites resolved; guard not installed\n");
            return;
        }

        GVehHandle = AddVectoredExceptionHandler(1, DelegateBroadcastGuard);
        SDK::DbgLog("[CrashGuard] armed: %zu delegate-broadcast site(s), %zu FName-decode site(s)\n",
            GBroadcastSites.size(), GNameDecodeSites.size());
    }
}

void FCrashReporter::Register()
{
    InstallDelegateBroadcastGuard();
    atexit(OnCrtExit);

    // A vectored handler runs on *first-chance* exceptions, before Fortnite's
    // own SEH frames. Older builds such as 6.21 intentionally raise/catch some
    // access violations while loading frontend data; treating those as fatal
    // froze every thread and killed the process. A top-level filter runs only
    // after normal frame-based handlers decline the exception.
    SetUnhandledExceptionFilter(ErbiumUnhandledExceptionFilter);
    SDK::DbgLog("[CrashReporter] installed top-level unhandled exception filter\n");
}
