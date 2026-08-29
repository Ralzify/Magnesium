#include "pch.h"
#include "../Public/CrashReporter.h"
#include "../../../Support/Public/FaultGuard.h"
#include "../../../Public/Configuration.h"
#include <sstream>

namespace
{
    constexpr size_t MaxCrashFrames = 256;
    constexpr size_t MaxStackAddressesToInspect = 128;
    constexpr LONG MaxFirstChanceReports = 4;

    struct FCrashArtifactPaths
    {
        wchar_t TextPath[MAX_PATH]{};
        wchar_t DumpPath[MAX_PATH]{};
    };

    wchar_t GCrashOutputDirectory[MAX_PATH]{};
    uintptr_t GReporterModuleStart = 0;
    uintptr_t GReporterModuleEnd = 0;
    volatile LONG GCrashArtifactSequence = 0;
    volatile LONG GUnhandledCaptureStarted = 0;
    volatile LONG GEventCrashCaptureStarted = 0;
    volatile LONG GFirstChanceReportCount = 0;
    thread_local bool GWritingFirstChanceReport = false;

    bool EnsureDirectory(const wchar_t* Directory)
    {
        if (!Directory || !*Directory)
            return false;
        if (CreateDirectoryW(Directory, nullptr))
            return true;
        if (GetLastError() != ERROR_ALREADY_EXISTS)
            return false;

        const DWORD Attributes = GetFileAttributesW(Directory);
        return Attributes != INVALID_FILE_ATTRIBUTES &&
            (Attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    void InitializeCrashOutputDirectory()
    {
        wchar_t CurrentDirectory[MAX_PATH]{};
        const DWORD CurrentDirectoryLength = GetCurrentDirectoryW(
            static_cast<DWORD>(std::size(CurrentDirectory)), CurrentDirectory);
        if (CurrentDirectoryLength > 0 && CurrentDirectoryLength < std::size(CurrentDirectory))
        {
            _snwprintf_s(GCrashOutputDirectory, std::size(GCrashOutputDirectory), _TRUNCATE,
                L"%ls\\MagnesiumCrashes", CurrentDirectory);
            if (EnsureDirectory(GCrashOutputDirectory))
                return;
        }

        wchar_t LocalAppData[MAX_PATH]{};
        const DWORD LocalAppDataLength = GetEnvironmentVariableW(L"LOCALAPPDATA", LocalAppData,
            static_cast<DWORD>(std::size(LocalAppData)));
        if (LocalAppDataLength > 0 && LocalAppDataLength < std::size(LocalAppData))
        {
            wchar_t MagnesiumDirectory[MAX_PATH]{};
            _snwprintf_s(MagnesiumDirectory, std::size(MagnesiumDirectory), _TRUNCATE,
                L"%ls\\Magnesium", LocalAppData);
            if (EnsureDirectory(MagnesiumDirectory))
            {
                _snwprintf_s(GCrashOutputDirectory, std::size(GCrashOutputDirectory), _TRUNCATE,
                    L"%ls\\Crashes", MagnesiumDirectory);
                if (EnsureDirectory(GCrashOutputDirectory))
                    return;
            }
        }

        wcscpy_s(GCrashOutputDirectory, std::size(GCrashOutputDirectory), L".");
    }

    void InitializeReporterModuleRange()
    {
        HMODULE ReporterModule = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(
                    reinterpret_cast<uintptr_t>(&FCrashReporter::Register)), &ReporterModule) ||
            !ReporterModule)
        {
            return;
        }

        auto DosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(ReporterModule);
        if (DosHeader->e_magic != IMAGE_DOS_SIGNATURE)
            return;

        auto NtHeaders = reinterpret_cast<IMAGE_NT_HEADERS*>(
            reinterpret_cast<uintptr_t>(ReporterModule) + DosHeader->e_lfanew);
        if (NtHeaders->Signature != IMAGE_NT_SIGNATURE ||
            NtHeaders->OptionalHeader.SizeOfImage == 0)
        {
            return;
        }

        GReporterModuleStart = reinterpret_cast<uintptr_t>(ReporterModule);
        GReporterModuleEnd = GReporterModuleStart + NtHeaders->OptionalHeader.SizeOfImage;
    }

    bool IsReportableException(const LPEXCEPTION_POINTERS ExceptionInfo)
    {
        if (!ExceptionInfo || !ExceptionInfo->ExceptionRecord || !ExceptionInfo->ContextRecord)
        {
            return false;
        }

        const DWORD ExceptionCode = ExceptionInfo->ExceptionRecord->ExceptionCode;
        if ((ExceptionCode & 0x80000000) == 0 || (ExceptionCode & 0x30000000) != 0 ||
            GGuardedNativeCallDepth > 0)
        {
            return false;
        }

        if ((ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
             ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
            ExceptionInfo->ExceptionRecord->NumberParameters > 1 &&
            ExceptionInfo->ExceptionRecord->ExceptionInformation[1] == 0xFFFFF78000000900)
        {
            return false;
        }

        return true;
    }

    bool IsReporterAddress(const uintptr_t Address)
    {
        return GReporterModuleStart && Address >= GReporterModuleStart &&
            Address < GReporterModuleEnd;
    }

    bool IsMagnesiumRelatedException(const LPEXCEPTION_POINTERS ExceptionInfo)
    {
        if (!ExceptionInfo || !ExceptionInfo->ContextRecord)
            return false;
        if (IsReporterAddress(ExceptionInfo->ContextRecord->Rip))
            return true;

        const uintptr_t StackPointer = ExceptionInfo->ContextRecord->Rsp;
        MEMORY_BASIC_INFORMATION StackRegion{};
        if (!StackPointer || !VirtualQuery(reinterpret_cast<const void*>(StackPointer),
                &StackRegion, sizeof(StackRegion)) || StackRegion.State != MEM_COMMIT ||
            (StackRegion.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
        {
            return false;
        }

        const uintptr_t RegionEnd = reinterpret_cast<uintptr_t>(StackRegion.BaseAddress) +
            StackRegion.RegionSize;
        if (RegionEnd <= StackPointer)
            return false;

        const size_t AvailableAddresses = (RegionEnd - StackPointer) / sizeof(uintptr_t);
        const size_t AddressesToInspect = (std::min)(AvailableAddresses,
                MaxStackAddressesToInspect);
        auto StackAddresses = reinterpret_cast<const uintptr_t*>(StackPointer);
        for (size_t Index = 0;
             Index < AddressesToInspect;
             ++Index)
        {
            if (IsReporterAddress(StackAddresses[Index]))
                return true;
        }

        return false;
    }

    bool IsFatalEventException(const LPEXCEPTION_POINTERS ExceptionInfo)
    {
        if (!ExceptionInfo || !ExceptionInfo->ExceptionRecord)
            return false;

        switch (ExceptionInfo->ExceptionRecord->ExceptionCode)
        {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
            return true;
        default:
            return false;
        }
    }

    void BuildCrashArtifactPaths(const wchar_t* Kind, FCrashArtifactPaths& Paths)
    {
        SYSTEMTIME LocalTime{};
        GetLocalTime(&LocalTime);
        const LONG Sequence = InterlockedIncrement(&GCrashArtifactSequence);
        wchar_t Stem[160]{};
        _snwprintf_s(Stem, std::size(Stem), _TRUNCATE, L"Magnesium-%04u%02u%02u-%02u%02u%02u-"
            L"p%lu-t%lu-%ld-%ls", LocalTime.wYear, LocalTime.wMonth, LocalTime.wDay,
            LocalTime.wHour, LocalTime.wMinute, LocalTime.wSecond, GetCurrentProcessId(),
            GetCurrentThreadId(), Sequence, Kind);
        _snwprintf_s(Paths.TextPath, std::size(Paths.TextPath), _TRUNCATE, L"%ls\\%ls.txt",
            GCrashOutputDirectory, Stem);
        _snwprintf_s(Paths.DumpPath, std::size(Paths.DumpPath), _TRUNCATE, L"%ls\\%ls.dmp",
            GCrashOutputDirectory, Stem);
    }

    bool WriteFileBytes(const HANDLE File, const char* Data, size_t DataSize)
    {
        size_t WrittenTotal = 0;
        while (WrittenTotal < DataSize)
        {
            const DWORD Remaining = static_cast<DWORD>((std::min)(DataSize - WrittenTotal,
                    static_cast<size_t>(MAXDWORD)));
            DWORD Written = 0;
            if (!WriteFile(File, Data + WrittenTotal, Remaining, &Written, nullptr) || Written == 0)
            {
                return false;
            }
            WrittenTotal += Written;
        }
        return true;
    }

    bool AppendCrashText(const wchar_t* Path, const char* Text, size_t TextLength)
    {
        const HANDLE File = CreateFileW(Path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (File == INVALID_HANDLE_VALUE)
            return false;

        const bool bWritten = WriteFileBytes(File, Text, TextLength);
        FlushFileBuffers(File);
        CloseHandle(File);
        return bWritten;
    }

    void WriteBasicCrashHeader(const wchar_t* Path, const char* Kind,
        const LPEXCEPTION_POINTERS ExceptionInfo)
    {
        const auto Record = ExceptionInfo->ExceptionRecord;
        const bool bMemoryFault = (Record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
             Record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) && Record->NumberParameters > 1;
        const unsigned long long FaultOperation = bMemoryFault ? Record->ExceptionInformation[0]
            : 0;
        const unsigned long long FaultAddress = bMemoryFault ? Record->ExceptionInformation[1] : 0;
        char Header[4096]{};
        const int HeaderLength = _snprintf_s(Header, std::size(Header), _TRUNCATE,
            "Magnesium crash report\r\n"
            "Kind: %s\r\n"
            "ExceptionCode: 0x%08lX\r\n"
            "ExceptionAddress: %p\r\n"
            "ProcessId: %lu\r\n"
            "ThreadId: %lu\r\n"
            "RIP: 0x%016llX\r\n"
            "RSP: 0x%016llX\r\n"
            "RBP: 0x%016llX\r\n"
            "RAX: 0x%016llX\r\n"
            "RBX: 0x%016llX\r\n"
            "RCX: 0x%016llX\r\n"
            "RDX: 0x%016llX\r\n"
            "RSI: 0x%016llX\r\n"
            "RDI: 0x%016llX\r\n"
            "R8:  0x%016llX\r\n"
            "R9:  0x%016llX\r\n"
            "R10: 0x%016llX\r\n"
            "R11: 0x%016llX\r\n"
            "R12: 0x%016llX\r\n"
            "R13: 0x%016llX\r\n"
            "R14: 0x%016llX\r\n"
            "R15: 0x%016llX\r\n"
            "MemoryFault: %d\r\n"
            "MemoryOperation: %llu\r\n"
            "MemoryAddress: 0x%016llX\r\n\r\n", Kind, Record->ExceptionCode,
            Record->ExceptionAddress, GetCurrentProcessId(), GetCurrentThreadId(),
            ExceptionInfo->ContextRecord->Rip, ExceptionInfo->ContextRecord->Rsp,
            ExceptionInfo->ContextRecord->Rbp, ExceptionInfo->ContextRecord->Rax,
            ExceptionInfo->ContextRecord->Rbx, ExceptionInfo->ContextRecord->Rcx,
            ExceptionInfo->ContextRecord->Rdx, ExceptionInfo->ContextRecord->Rsi,
            ExceptionInfo->ContextRecord->Rdi, ExceptionInfo->ContextRecord->R8,
            ExceptionInfo->ContextRecord->R9, ExceptionInfo->ContextRecord->R10,
            ExceptionInfo->ContextRecord->R11, ExceptionInfo->ContextRecord->R12,
            ExceptionInfo->ContextRecord->R13, ExceptionInfo->ContextRecord->R14,
            ExceptionInfo->ContextRecord->R15, static_cast<int>(bMemoryFault), FaultOperation,
            FaultAddress);
        if (HeaderLength > 0)
            AppendCrashText(Path, Header, HeaderLength);
    }

    bool WriteMiniDump(const wchar_t* Path, const LPEXCEPTION_POINTERS ExceptionInfo,
        const MINIDUMP_TYPE PreferredDumpType, DWORD& Error)
    {
        Error = ERROR_SUCCESS;
        const HANDLE DumpFile = CreateFileW(Path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (DumpFile == INVALID_HANDLE_VALUE)
        {
            Error = GetLastError();
            return false;
        }

        MINIDUMP_EXCEPTION_INFORMATION DumpException{};
        DumpException.ThreadId = GetCurrentThreadId();
        DumpException.ExceptionPointers = ExceptionInfo;
        DumpException.ClientPointers = FALSE;
        BOOL bWritten = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), DumpFile,
            PreferredDumpType, &DumpException, nullptr, nullptr);
        if (!bWritten)
        {
            SetFilePointer(DumpFile, 0, nullptr, FILE_BEGIN);
            SetEndOfFile(DumpFile);
            bWritten = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), DumpFile,
                MiniDumpNormal, &DumpException, nullptr, nullptr);
        }
        if (!bWritten)
            Error = GetLastError();

        FlushFileBuffers(DumpFile);
        CloseHandle(DumpFile);
        if (!bWritten)
            DeleteFileW(Path);
        return bWritten != FALSE;
    }

    void AppendDumpStatus(const wchar_t* TextPath, const wchar_t* DumpPath, const bool bDumpWritten,
        const DWORD DumpError)
    {
        char Status[1024]{};
        const int StatusLength = _snprintf_s(Status, std::size(Status), _TRUNCATE,
            "MinidumpWritten: %d\r\n"
            "MinidumpError: %lu\r\n"
            "MinidumpPath: %ls\r\n\r\n", static_cast<int>(bDumpWritten), DumpError, DumpPath);
        if (StatusLength > 0)
            AppendCrashText(TextPath, Status, StatusLength);
    }

    void WriteInitialCrashArtifacts(const wchar_t* Kind, const char* KindText,
        const LPEXCEPTION_POINTERS ExceptionInfo, FCrashArtifactPaths& Paths,
        const bool bCaptureReferencedMemory)
    {
        BuildCrashArtifactPaths(Kind, Paths);
        WriteBasicCrashHeader(Paths.TextPath, KindText, ExceptionInfo);
        DWORD DumpError = ERROR_SUCCESS;
        MINIDUMP_TYPE DumpType = static_cast<MINIDUMP_TYPE>(MiniDumpNormal |
            MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        if (bCaptureReferencedMemory)
        {
            DumpType = static_cast<MINIDUMP_TYPE>(DumpType | MiniDumpWithDataSegs |
                MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory);
        }
        const bool bDumpWritten = WriteMiniDump(Paths.DumpPath, ExceptionInfo, DumpType, DumpError);
        AppendDumpStatus(Paths.TextPath, Paths.DumpPath, bDumpWritten, DumpError);
    }

    LONG WINAPI ErbiumFirstChanceExceptionHandler(LPEXCEPTION_POINTERS ExceptionInfo)
    {
        if (!IsReportableException(ExceptionInfo) || GWritingFirstChanceReport)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        const bool bEventException =
            FConfiguration::bEventStarted.load(std::memory_order_acquire) &&
            IsFatalEventException(ExceptionInfo);
        if (bEventException)
        {
            if (InterlockedCompareExchange(&GEventCrashCaptureStarted, 1, 0) != 0)
            {
                return EXCEPTION_CONTINUE_SEARCH;
            }
        }
        else
        {
            if (!IsMagnesiumRelatedException(ExceptionInfo))
                return EXCEPTION_CONTINUE_SEARCH;
            const LONG ReportIndex = InterlockedIncrement(&GFirstChanceReportCount);
            if (ReportIndex > MaxFirstChanceReports)
                return EXCEPTION_CONTINUE_SEARCH;
        }

        GWritingFirstChanceReport = true;
        FCrashArtifactPaths Paths{};
        WriteInitialCrashArtifacts(bEventException ? L"event-active" : L"first-chance",
            bEventException ? "fatal exception while event active"
                : "first-chance Magnesium-related exception", ExceptionInfo, Paths,
            bEventException);
        SDK::DbgLog("[CrashReporter] %s artifact text=%ls dump=%ls "
            "code=0x%08lX\n", bEventException ? "event-active" : "first-chance", Paths.TextPath,
            Paths.DumpPath, ExceptionInfo->ExceptionRecord->ExceptionCode);
        GWritingFirstChanceReport = false;
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

LONG WINAPI ErbiumUnhandledExceptionFilter(LPEXCEPTION_POINTERS ExceptionInfo)
{
    if (!IsReportableException(ExceptionInfo))
        return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedCompareExchange(&GUnhandledCaptureStarted, 1, 0) != 0)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    FCrashArtifactPaths Paths{};
    WriteInitialCrashArtifacts(L"unhandled", "unhandled exception", ExceptionInfo, Paths, false);

    STACKFRAME64 StackFrame{};
    CONTEXT ContextCopy = *ExceptionInfo->ContextRecord;
    ContextCopy.ContextFlags = CONTEXT_ALL;
    StackFrame.AddrPC.Offset = ContextCopy.Rip;
    StackFrame.AddrPC.Mode = AddrModeFlat;
    StackFrame.AddrStack.Offset = ContextCopy.Rsp;
    StackFrame.AddrStack.Mode = AddrModeFlat;
    StackFrame.AddrFrame.Offset = ContextCopy.Rbp;
    StackFrame.AddrFrame.Mode = AddrModeFlat;

    auto CurrentProcess = GetCurrentProcess();
    auto CurrentThread = GetCurrentThread();
    SymCleanup(CurrentProcess);
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    const BOOL bSymbolsInitialized = SymInitialize(CurrentProcess, nullptr, TRUE);

    std::stringstream ReportStream;
    ReportStream << "Stack trace:\n";
    char SymbolName[1024]{};
    char SymbolStorage[sizeof(IMAGEHLP_SYMBOL64) + sizeof(SymbolName)]{};
    auto Symbol = reinterpret_cast<IMAGEHLP_SYMBOL64*>(SymbolStorage);
    Symbol->SizeOfStruct = sizeof(IMAGEHLP_SYMBOL64);
    Symbol->MaxNameLength = sizeof(SymbolName);

    for (size_t FrameIndex = 0;
         FrameIndex < MaxCrashFrames;
         ++FrameIndex)
    {
        const BOOL bWalked = StackWalk64(IMAGE_FILE_MACHINE_AMD64, CurrentProcess, CurrentThread,
            &StackFrame, &ContextCopy, nullptr, SymFunctionTableAccess64, SymGetModuleBase64,
            nullptr);
        if (!bWalked || !StackFrame.AddrPC.Offset)
            break;

        char Address[19]{};
        snprintf(Address, sizeof(Address), "0x%016llx", StackFrame.AddrPC.Offset);
        ReportStream << Address;

        const DWORD64 ModuleBase = SymGetModuleBase64(CurrentProcess, StackFrame.AddrPC.Offset);
        if (ModuleBase)
        {
            char ModulePath[1024]{};
            GetModuleFileNameA(reinterpret_cast<HMODULE>(ModuleBase), ModulePath,
                sizeof(ModulePath));
            const char* FileName = strrchr(ModulePath, '\\');
            char Offset[19]{};
            snprintf(Offset, sizeof(Offset), "0x%llx", StackFrame.AddrPC.Offset - ModuleBase);
            ReportStream << " (" << (FileName ? FileName + 1 : ModulePath) << "+" << Offset << ")";
        }
        ReportStream << ": ";

        DWORD64 SymbolDisplacement = 0;
        const BOOL bSymbolResolved = bSymbolsInitialized && SymGetSymFromAddr64(CurrentProcess,
                StackFrame.AddrPC.Offset, &SymbolDisplacement, Symbol);
        if (!bSymbolResolved || ModuleBase == ImageBase)
        {
            ReportStream << "[unknown]\n";
            continue;
        }

        UnDecorateSymbolName(Symbol->Name, SymbolName, sizeof(SymbolName), UNDNAME_COMPLETE);
        ReportStream << SymbolName << "()";
        IMAGEHLP_LINE64 ImageLine{};
        ImageLine.SizeOfStruct = sizeof(ImageLine);
        DWORD LineDisplacement = 0;
        if (SymGetLineFromAddr64(CurrentProcess, StackFrame.AddrPC.Offset, &LineDisplacement,
                &ImageLine))
        {
            const char* FileName = strrchr(ImageLine.FileName, '\\');
            ReportStream << " [" << (FileName ? FileName + 1 : ImageLine.FileName)
                << ":" << ImageLine.LineNumber << "]";
        }
        ReportStream << "\n";
    }

    const std::string Report = ReportStream.str();
    AppendCrashText(Paths.TextPath, Report.c_str(), Report.size());
    printf("%s", Report.c_str());
    fflush(stdout);
    Memcury::Util::CopyToClipboard(Report);
    if (bSymbolsInitialized)
        SymCleanup(CurrentProcess);
    SDK::DbgLog("[CrashReporter] unhandled artifacts text=%ls dump=%ls "
        "code=0x%08lX\n", Paths.TextPath, Paths.DumpPath,
        ExceptionInfo->ExceptionRecord->ExceptionCode);
    TerminateProcess(GetCurrentProcess(), ExceptionInfo->ExceptionRecord->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

void FCrashReporter::Register()
{
    InitializeCrashOutputDirectory();
    InitializeReporterModuleRange();
    const PVOID VectoredHandler = AddVectoredExceptionHandler(1, ErbiumFirstChanceExceptionHandler);
    SetUnhandledExceptionFilter(ErbiumUnhandledExceptionFilter);
    SDK::DbgLog("[CrashReporter] handlers installed output=%ls "
        "module=%p-%p vectored=%p\n", GCrashOutputDirectory,
        reinterpret_cast<void*>(GReporterModuleStart), reinterpret_cast<void*>(GReporterModuleEnd),
        VectoredHandler);
}
