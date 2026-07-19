#pragma once
#include "Memcury.h"
#include <array>
#include <cstdio>
#include <cstdarg>
#include <string>
#pragma comment(lib, "version.lib")

namespace SDK
{
	// Crash-proof diagnostic log: opens/flushes/closes per call so nothing is lost to
	// buffering or to the stdout->stdout.log redirect that happens after SDK::Init.
	// Writes to magnesium_debug.log in the game's working directory.
	inline void DbgLog(const char* Fmt, ...)
	{
		FILE* f = nullptr;
		if (fopen_s(&f, "magnesium_debug.log", "a") != 0 || !f)
			return;
		va_list args;
		va_start(args, Fmt);
		vfprintf(f, Fmt, args);
		va_end(args);
		fflush(f);
		fclose(f);
	}

	// Signature-free version detection fallback. When the version-getter function
	// can't be located (new/obfuscated build), read the engine version from the
	// exe's VERSIONINFO resource and the Fortnite version + CL from the build string
	// in .rdata, then reconstruct the string the normal parser expects.
	inline std::wstring GetBuildStringFromMemory()
	{
		std::wstring engineVer;
		wchar_t path[MAX_PATH];
		if (GetModuleFileNameW(GetModuleHandleW(nullptr), path, MAX_PATH))
		{
			DWORD dummy = 0;
			DWORD sz = GetFileVersionInfoSizeW(path, &dummy);
			if (sz)
			{
				std::string buf(sz, '\0');
				if (GetFileVersionInfoW(path, 0, sz, buf.data()))
				{
					VS_FIXEDFILEINFO* ffi = nullptr;
					UINT len = 0;
					if (VerQueryValueW(buf.data(), L"\\", (LPVOID*)&ffi, &len) && ffi)
						engineVer = std::to_wstring(HIWORD(ffi->dwFileVersionMS)) + L"." + std::to_wstring(LOWORD(ffi->dwFileVersionMS)) + L".0";
				}
			}
		}
		if (engineVer.empty())
			return L"";

		// wide "Fortnite+Release-" in .rdata
		auto ref = Memcury::Scanner::FindPattern("46 00 6F 00 72 00 74 00 6E 00 69 00 74 00 65 00 2B 00 52 00 65 00 6C 00 65 00 61 00 73 00 65 00 2D 00", false, true);
		if (!ref.Get())
			return L"";

		std::wstring s = (const wchar_t*)ref.Get(); // "Fortnite+Release-32.11[-CL-38202817]"
		auto rp = s.find(L"Release-");
		if (rp == std::wstring::npos)
			return L"";
		std::wstring rest = s.substr(rp + 8); // "32.11..."
		size_t d = 0;
		while (d < rest.size() && (iswdigit(rest[d]) || rest[d] == L'.'))
			d++;
		std::wstring fnVer = rest.substr(0, d);
		if (fnVer.empty() || !iswdigit(fnVer[0]))
			return L"";

		// Real CL is only used to gate the pre-2.5 "Cert" table; a synthetic value
		// above that threshold is safe for modern builds.
		return engineVer + L"-99999999+++Fortnite+Release-" + fnVer;
	}
	struct FVersionInfo
	{
		double EngineVersion = 0.f;
		double FortniteVersion = 0.f;
	};
	struct FStringNoOps
	{
		wchar_t* Data;
		int32_t NumElements;
		int32_t MaxElements;
	};
	inline FVersionInfo VersionInfo{};

	namespace Offsets
	{
		inline uint64_t Realloc = 0;
		inline uint64_t AppendString = 0;
		inline uint64_t ToString = 0;
		inline uint64_t ProcessEventVft = 0;
		inline uint64_t GObjectsChunked = 0;
		inline uint64_t GObjectsUnchunked = 0;
		inline uint64_t Step = 0;
		inline uint64_t StepExplicitProperty = 0;
		inline uint64_t GetInterfaceAddress = 0;
		inline uint64_t StaticFindObject = 0;
		inline uint64_t StaticLoadObject = 0;
		inline uint64_t FNameConstructor = 0;
		inline uint64_t SpawnActor = 0;

		inline uint32_t Offset_Internal = 0;
		inline uint32_t ElementSize = 0;
		inline uint32_t PropertyFlags = 0;
		inline uint32_t PropertiesSize = 0;
		inline uint32_t Super = 0;
		inline uint32_t FieldMask = 0;
		inline uint32_t Children = 0;
		inline uint32_t FField_Next = 0;
		inline uint32_t FField_Name = 0;
		inline uint32_t ExecFunction = 0;
		inline uint32_t FFrame_PropertyChainForCompiledIn = 0;
		inline uint32_t FFrame_CurrentNativeFunction = 0;
		inline uint32_t FFrame_Next = 0;

		// FN 32.11+ (UE 5.5) obfuscation: GObjects ptr, FUObjectItem->Object,
		// and NumElements are stored encrypted as ~ROR16(value ^ key). Keys are
		// per-build random constants, resolved by scanning at init (see Init()).
		inline bool     bEncryptedObjects = false;
		inline uint32_t EncObjArrayKey = 0;  // GObjects chunk-array pointer key
		inline uint32_t EncObjNumKey = 0;    // NumElements key
		inline uint32_t EncObjItemKey = 0;   // FUObjectItem->Object key

		// FN 32.11+ also encrypts reflection linked-list Next pointers (~ROR16(x^key))
		// and the FProperty byte-offset field (~(x^key)). Keys resolved by scanning.
		inline uint32_t EncFieldNextKey = 0;  // UField::Next / FField::Next pointer key
		inline uint32_t EncPropOffsetKey = 0; // FProperty Offset_Internal key
		inline uint32_t ChildProperties = 0;  // UStruct FField (property) chain head
		inline bool bEncChildProperties = false; // FN 32.11: ChildProperties head is ~ROR16(x^EncFieldNextKey) encrypted
	}

	extern void UpdateNumElemsPerChunk();
	extern void InitializeProcessEventVft(uintptr_t);

	inline void Init()
	{
		FStringNoOps OutVar{}; // zero-init: Data stays null if no getter runs

		auto GetEngineVersionMethod1 = Memcury::Scanner::FindPattern("40 53 48 83 EC 20 48 8B D9 E8 ? ? ? ? 48 8B C8 41 B8 04 ? ? ? 48 8B D3");
		if (!GetEngineVersionMethod1.Get())
			GetEngineVersionMethod1 = Memcury::Scanner::FindPattern("48 89 5C 24 ? 57 48 83 EC ? 65 48 8B 04 25 ? ? ? ? 48 8B D9 B9 ? ? ? ? 48 8B 10 8B 04 11 39 05 ? ? ? ? 7E ? 48 8D 0D ? ? ? ? E8 ? ? ? ? 83 3D ? ? ? ? ? 75 ? 48 8D 3D ? ? ? ? 48 8B CF E8 ? ? ? ? 48 8D 0D ? ? ? ? 48 89 3D ? ? ? ? E8 ? ? ? ? 48 8D 0D ? ? ? ? E8 ? ? ? ? 48 8B 0D ? ? ? ? 41 B8");

		if (auto GetEngineVersion = (FStringNoOps * (*)(FStringNoOps * _Out)) GetEngineVersionMethod1.Get())
		{
			GetEngineVersion(&OutVar);
		}
		else
		{
			auto GetEngineVersionMethod2 = Memcury::Scanner::FindPattern("40 53 48 83 EC ? 33 C0 49 8B D8 48 39 42 ? 0F 95 C0 48 01 42 ? E8 ? ? ? ? 41 B8");
			auto CopyOfMethod2 = GetEngineVersionMethod2;

			auto GetStorage = (void* (*)()) GetEngineVersionMethod2.RelativeOffset(23).Get();
			auto GetEngineVersion2 = (void (*)(void*, FStringNoOps*, int)) CopyOfMethod2.RelativeOffset(42).Get();

			if (GetStorage && GetEngineVersion2) // guard: don't call null on builds where the pattern misses
				GetEngineVersion2(GetStorage(), &OutVar, 4); // no idea why 4 but sure
		}

		std::wstring BuildString = OutVar.Data ? OutVar.Data : L"";
		if (BuildString.empty()) // getter not found (e.g. UE5.5/32.11) — use signature-free fallback
			BuildString = GetBuildStringFromMemory();
		DbgLog("BuildString=\"%ws\"\n", BuildString.c_str());
		std::wstring EngineVersion = BuildString.substr(0, BuildString.find(L'-'));
		std::wstring FortniteCL = BuildString.substr(BuildString.find(L'-') + 1, BuildString.find(L'+') - BuildString.find(L'-') - 1);

		if (EngineVersion == L"4.26.1")
			EngineVersion = L"4.27.0";

		if (EngineVersion.find_first_of(L'.') != EngineVersion.find_last_of(L'.'))
			EngineVersion.erase(EngineVersion.rfind(L'.'));

		auto FortniteCLNum = std::stoull(FortniteCL);

		VersionInfo.EngineVersion = std::stod(EngineVersion);
		// these builds were just called "Cert"
		if (FortniteCLNum < 3901517)
		{
			switch (FortniteCLNum)
			{
			case 3668626:
				VersionInfo.FortniteVersion = 1.64;
				break;
			case 3681159:
				VersionInfo.FortniteVersion = 1.7;
				break;
			case 3700114:
				VersionInfo.FortniteVersion = 1.72;
				break;
			case 3724489:
				VersionInfo.FortniteVersion = 1.8;
				break;
			case 3729133:
				VersionInfo.FortniteVersion = 1.81;
				break;
			case 3741772:
				VersionInfo.FortniteVersion = 1.82;
				break;
			case 3757339:
				VersionInfo.FortniteVersion = 1.9;
				break;
			case 3775276:
				VersionInfo.FortniteVersion = 1.91;
				break;
			case 3790078:
				VersionInfo.FortniteVersion = 1.10;
				break;
			case 3807424:
				VersionInfo.FortniteVersion = 1.11;
				break;
			case 3825894:
				VersionInfo.FortniteVersion = 2.1;
				break;
			case 3841827:
				VersionInfo.FortniteVersion = 2.2;
				break;
			case 3847564:
				VersionInfo.FortniteVersion = 2.3;
				break;
			case 3858292:
				VersionInfo.FortniteVersion = 2.4;
				break;
			case 3870737:
				VersionInfo.FortniteVersion = 2.42;
				break;
			case 3889387:
				VersionInfo.FortniteVersion = 2.5;
				break;
			}
		}
		else 
			VersionInfo.FortniteVersion = std::stod(BuildString.substr(BuildString.rfind(L'-') + 1));

		bUE51 = VersionInfo.FortniteVersion >= 24.00;

		DbgLog("=== SDK::Init FN=%.2f EV=%.2f  [build " __DATE__ " " __TIME__ "] ===\n", VersionInfo.FortniteVersion, VersionInfo.EngineVersion);

		// FN 32.11+ encrypts the object array. Resolve the per-build keys and the
		// GObjects struct base by scanning the inlined decrypt idioms. If all parts
		// resolve, bEncryptedObjects gates the decryption in Core.h's TUObjectArray.
		if (VersionInfo.FortniteVersion >= 32.00)
		{
			// mov r32, ArrayKey ; mov r9, [rip+GObjectsBase] ; xor r9, rax
			auto ArrayDec = Memcury::Scanner::FindPattern("B8 ? ? ? ? 4C 8B 0D ? ? ? ? 4C 33 C8");
			if (ArrayDec.Get())
			{
				Offsets::EncObjArrayKey = *(uint32_t*)(ArrayDec.Get() + 1);
				Offsets::GObjectsChunked = ArrayDec.RelativeOffset(8).Get(); // FChunkedFixedUObjectArray base
			}

			// mov eax, [rip+Num] ; xor eax, NumKey ; not eax
			auto NumDec = Memcury::Scanner::FindPattern("8B 05 ? ? ? ? 35 ? ? ? ? F7 D0");
			if (NumDec.Get())
			{
				Offsets::EncObjNumKey = *(uint32_t*)(NumDec.Get() + 7);
				if (!Offsets::GObjectsChunked)
					Offsets::GObjectsChunked = NumDec.RelativeOffset(2).Get() - 0x14; // NumElements is base+0x14
			}

			// mov r10, [r?+0x10] ; mov eax, ItemKey ; xor r10, rax ; ror r10, 0x10 ; not r10
			auto ItemDec = Memcury::Scanner::FindPattern("4C 8B ? 10 B8 ? ? ? ? 4C 33 D0 49 C1 CA 10 49 F7 D2");
			if (ItemDec.Get())
				Offsets::EncObjItemKey = *(uint32_t*)(ItemDec.Get() + 5);

			// mov eax, [r?+0x64] ; xor eax, PropOffsetKey ; not eax
			auto PropOffDec = Memcury::Scanner::FindPattern("8B ? 64 35 ? ? ? ? F7 D0");
			if (PropOffDec.Get())
				Offsets::EncPropOffsetKey = *(uint32_t*)(PropOffDec.Get() + 4);

			// mov ecx, FieldNextKey ; lea rdx,[rsp+0x30] ; mov rax,[r8+0x10] ; xor rax, rcx
			auto FieldNextDec = Memcury::Scanner::FindPattern("B9 ? ? ? ? 48 8D 54 24 30 49 8B 40 10 48 33 C1");
			if (FieldNextDec.Get())
				Offsets::EncFieldNextKey = *(uint32_t*)(FieldNextDec.Get() + 1);

			Offsets::bEncryptedObjects = Offsets::GObjectsChunked && Offsets::EncObjArrayKey
				&& Offsets::EncObjNumKey && Offsets::EncObjItemKey;
		}

		Offsets::Realloc = Memcury::Scanner::FindPattern("48 89 5C 24 08 48 89 74 24 10 57 48 83 EC ? 48 8B F1 41 8B D8 48 8B 0D ? ? ? ?").Get();

		auto SRef = Memcury::Scanner::FindStringRef("ForwardShadingQuality_");
		constexpr std::array<const char*, 5> sigs =
		{
			"48 8D ? ? 48 8D ? ? E8",
			"48 8D ? ? ? 48 8D ? ? E8",
			"48 8D ? ? 49 8B ? E8",
			"48 8D ? ? ? 49 8B ? E8",
			"48 8D ? ? 48 8B ? E8"
		};

		for (auto& sig : sigs)
		{
			auto Scanner = SRef;
			Scanner.ScanFor(sig, true, 0, 1, (VersionInfo.EngineVersion == 5.0 || VersionInfo.EngineVersion == 5.1) ? 0x100 : 0x50);

			if (Scanner.Get() != SRef.Get())
			{
				auto p2b = Memcury::ASM::pattern2bytes(sig);

				Offsets::AppendString = Scanner.RelativeOffset((uint32_t)p2b.size()).Get();
				break;
			}
		}

		if (!Offsets::AppendString || VersionInfo.EngineVersion > 5.3) // i dk what ver they inlined it on
		{
			Offsets::ToString = Memcury::Scanner::FindPattern("48 8B C4 48 89 58 ? 48 89 70 ? 48 89 78 ? 55 41 54 41 55 41 56 41 57 48 8D A8 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 45 33 ED 48 8B FA 4C 89 2A").Get();

			if (!Offsets::ToString)
				Offsets::ToString = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 ? ? ? ? 33 ED 48 8B FA 48 89 2A 48 89 6A ? 8B 19").Get();
		}

		uintptr_t addr = 0;

		if (VersionInfo.FortniteVersion < 14.00)
			addr = Memcury::Scanner::FindStringRef(L"AccessNoneNoContext").ScanFor({ 0x40, 0x55 }, true, 0, 1, 2000).Get();
		else if (floor(VersionInfo.FortniteVersion) == 27)
			addr = Memcury::Scanner::FindPattern("40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ? ? ? ? 48 8D 6C 24 ? 48 89 9D ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C5 48 89 85 ? ? ? ? 45 33 E4 4C 89 45 ? 4D 8B F8").Get();
		else if (VersionInfo.EngineVersion == 5.2 || (std::floor(VersionInfo.FortniteVersion) == 24 && VersionInfo.FortniteVersion >= 24.30))
			addr = Memcury::Scanner::FindPattern("40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC ? ? ? ? 48 8D 6C 24 ? 48 89 9D ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C5 48 89 85 ? ? ? ? 45 33 F6").Get();
		else if (VersionInfo.FortniteVersion >= 23.00)
		{
			addr = Memcury::Scanner::FindPattern("48 85 C9 0F 85 ? ? ? ? F7 87 ? ? ? ? ? ? ? ? ? 8B ?").ScanFor({ 0x40, 0x55 }, false).Get();
			if (!addr)
				addr = Memcury::Scanner::FindPattern("41 FF 92 ? ? ? ? E9 ? ? ? ? 49 8B C8").ScanFor({ 0x40, 0x55 }, false).Get();
		}
		else
			addr = Memcury::Scanner::FindStringRef(L"UMeshNetworkComponent::ProcessEvent: Invalid mesh network node type: %s", true, 0, VersionInfo.FortniteVersion >= 19.00).ScanFor({ 0xE8 }, true, VersionInfo.FortniteVersion < 19.00 ? 1 : 3, VersionInfo.FortniteVersion == 15.50 ? 7 : 0, 2000).RelativeOffset(1).Get();

		if (Offsets::bEncryptedObjects)
		{
			// GObjectsChunked already points at the encrypted array base (resolved above).
		}
		else if (VersionInfo.EngineVersion >= 4.21)
		{
			if (VersionInfo.FortniteVersion <= 6.01)
				UpdateNumElemsPerChunk();

			Offsets::GObjectsChunked = Memcury::Scanner::FindPattern(VersionInfo.FortniteVersion <= 6.02 ? "48 8B 05 ? ? ? ? 48 8B 0C C8 48 8D 04 D1" : "48 8B 05 ? ? ? ? 48 8B 0C C8 48 8B 04 D1").RelativeOffset(3).Get();
		}
		else
		{
			auto Addr = Memcury::Scanner::FindPattern("48 8B 05 ? ? ? ? 48 8D 14 C8 EB 03 49 8B D6 8B 42 08 C1 E8 1D A8 01 0F 85 ? ? ? ? F7 86 ? ? ? ? ? ? ? ?", false);
			if (!Addr.Get())
				Addr = Memcury::Scanner::FindPattern("48 8B 05 ? ? ? ? 48 8D 1C C8 81 4B ? ? ? ? ? 49 63 76 30", false);

			Offsets::GObjectsUnchunked = Addr.RelativeOffset(3).Get();
		}

		Offsets::Step = Memcury::Scanner::FindPattern("48 8B 41 20 4C 8B D2 48 8B D1 44 0F B6 08 48 FF").Get();
		if (!Offsets::Step)
			Offsets::Step = Memcury::Scanner::FindPattern("48 8B 41 ? 4C 8B DA 44 0F B6 08").Get();

		if (VersionInfo.EngineVersion >= 5.4 || VersionInfo.EngineVersion == 5.2)
			Offsets::StepExplicitProperty = Memcury::Scanner::FindPattern("41 8B 40 ? 4D 8B C8 48 0F BA E0").Get();
		else if (VersionInfo.EngineVersion == 5.3)
			Offsets::StepExplicitProperty = Memcury::Scanner::FindPattern("48 8B C4 48 89 58 ? 48 89 68 ? 48 89 70 ? 48 89 78 ? 41 54 41 56 41 57 48 83 EC ? 41 8B 40 ? 49 8B D8 48 8B F2").Get();
		else if (VersionInfo.FortniteVersion >= 20.20)
			Offsets::StepExplicitProperty = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 41 8B 40 ? 49 8B D8 48 8B F2").Get();
		else
			Offsets::StepExplicitProperty = Memcury::Scanner::FindPattern("41 8B 40 ? 4D 8B C8").Get();

		if (VersionInfo.EngineVersion <= 4.21)
			Offsets::GetInterfaceAddress = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 33 FF 48 8B DA 48 8B F1 48").Get();
		else
		{
			Offsets::GetInterfaceAddress = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 33 DB 48 8B FA 48 8B F1 48 85 D2 0F 84 ? ? ? ? 8B 82 ? ? ? ? C1 E8").Get();

			if (!Offsets::GetInterfaceAddress)
			{
				Offsets::GetInterfaceAddress = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 33 DB 48 8B FA 48 8B F1 48 85 D2 74 ? F7 82").Get();

				if (!Offsets::GetInterfaceAddress)
				{
					Offsets::GetInterfaceAddress = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 33 DB 48 8B FA 48 8B F1 48 85 D2 0F 84 ? ? ? ? F7 82").Get();

					if (!Offsets::GetInterfaceAddress)
						Offsets::GetInterfaceAddress = Memcury::Scanner::FindPattern("48 8B C4 48 89 58 ? 48 89 68 ? 48 89 70 ? 48 89 78 ? 41 56 48 83 EC ? 33 DB 48 8B FA 48 8B E9").Get();

					if (!Offsets::GetInterfaceAddress)
						Offsets::GetInterfaceAddress = Memcury::Scanner::FindPattern("4C 8B DC 49 89 5B ? 49 89 73 ? 57 48 83 EC ? 33 DB 48 8B FA 48 8B F1").Get();

					if (!Offsets::GetInterfaceAddress)
						Offsets::GetInterfaceAddress = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 74 24 ? 57 48 81 EC ? ? ? ? 33 DB 48 8B FA").Get();
				}
			}
		}

		if (VersionInfo.EngineVersion >= 5.3)
		{
			Offsets::StaticFindObject = Memcury::Scanner::FindPattern("48 89 74 24 ? 48 89 7C 24 ? 55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC ? 4C 8B E9 48 8D 4D").Get();

			if (!Offsets::StaticFindObject)
				Offsets::StaticFindObject = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 74 24 ? 55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC ? 4C 8B E9").Get();

			if (!Offsets::StaticFindObject)
				Offsets::StaticFindObject = Memcury::Scanner::FindPattern("48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 48 83 FA FF").Get();
		}
		else if (VersionInfo.EngineVersion == 5.2)
			Offsets::StaticFindObject = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 55 41 56 41 57 48 8B EC 48 83 EC ? 33 DB 4C 8B F9").Get();
		else if (VersionInfo.EngineVersion >= 5.1)
		{
			Offsets::StaticFindObject = Memcury::Scanner::FindPattern("40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 33 F6 4C 8B E1 48 83 CB", false).Get();

			if (!Offsets::StaticFindObject)
				Offsets::StaticFindObject = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 74 24 ? 55 57 41 56 48 8B EC 48 83 EC 60 33 DB 4C 8B F1 48 8D 4D E8 41 8A F1", false).Get();

			if (!Offsets::StaticFindObject)
				Offsets::StaticFindObject = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 55 41 56 41 57 48 8B EC 48 83 EC 60 33 DB 4C 8B F9 48 8D 4D E8 45").Get();
		}
		else if (VersionInfo.EngineVersion == 5.0)
		{
			Offsets::StaticFindObject = Memcury::Scanner::FindPattern("40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 45 33 F6 4C 8B E1 45 0F B6 E9 49 8B F8 41 8B C6", false).Get();

			if (!Offsets::StaticFindObject)
			{
				Offsets::StaticFindObject = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 74 24 ? 4C 89 64 24 ? 55 41 55 41 57 48 8B EC 48 83 EC 60 45 8A E1 4C 8B E9 48 83 FA").Get();

				if (!Offsets::StaticFindObject)
				{
					Offsets::StaticFindObject = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 74 24 ? 4C 89 64 24 ? 55 41 55 41 57 48 8B EC 48 83 EC 50 4C 8B E9").Get();

					if (!Offsets::StaticFindObject)
						Offsets::StaticFindObject = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 7C 24 ? 4C 89 64 24 ? 55 41 56 41 57 48 8B EC 48 83 EC 60 33 FF 4C 8B E1 48 8D 4D E8 45 8A").Get();
				}
			}
		}
		else if (VersionInfo.EngineVersion >= 4.27)
		{
			if (floor(VersionInfo.FortniteVersion) == 18)
				Offsets::StaticFindObject = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 60 45 33 ED 45 8A F9 44 38 2D ? ? ? ? 49 8B F8 48 8B").Get();
			else if (VersionInfo.FortniteVersion == 16.50)
				Offsets::StaticFindObject = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 60 45 33 ED 45 8A F9 44 38 2D ? ? ? ? 49 8B F8 48 8B F2 4C 8B E1").Get();
			
			if (!Offsets::StaticFindObject)
				Offsets::StaticFindObject = Memcury::Scanner::FindPattern("40 55 53 57 41 54 41 55 41 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85").Get();
		}
		else if (VersionInfo.EngineVersion == 4.16)
			Offsets::StaticFindObject = Memcury::Scanner::FindPattern("4C 8B DC 57 48 81 EC ? ? ? ? 80 3D ? ? ? ? ? 49 89 6B F0 49 89 73 E8").Get();
		else if (VersionInfo.EngineVersion == 4.19)
		{
			Offsets::StaticFindObject = Memcury::Scanner::FindPattern("48 89 5C 24 ? 48 89 74 24 ? 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC 60 80 3D ? ? ? ? ? 45 0F B6 F1 49 8B F8").Get();

			if (!Offsets::StaticFindObject)
				Offsets::StaticFindObject = Memcury::Scanner::FindPattern("4C 8B DC 49 89 5B 08 49 89 6B 18 49 89 73 20 57 41 56 41 57 48 83 EC 60 80 3D").Get();
		}
		else
		{
			auto sRef = Memcury::Scanner::FindStringRef(L"Illegal call to StaticFindObject() while serializing object data!", false, 1).Get();

			for (int i = 0; i < 1000; i++)
			{
				auto Ptr = (uint8_t*)(sRef - i);

				if (*Ptr == 0x48 && *(Ptr + 1) == 0x89 && *(Ptr + 2) == 0x5C)
				{
					Offsets::StaticFindObject = uint64_t(Ptr);
					break;
				}
			}
		}

		if (VersionInfo.EngineVersion >= 5.4)
		{
			Offsets::StaticLoadObject = Memcury::Scanner::FindPattern("40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 8B 85 ? ? ? ? 33 FF 8B 35").Get();

			if (!Offsets::StaticLoadObject)
				Offsets::StaticLoadObject = Memcury::Scanner::FindPattern("40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? B8 ? ? ? ? E8 ? ? ? ? 48 2B E0 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 8B 85").Get();

			if (!Offsets::StaticLoadObject)
				Offsets::StaticLoadObject = Memcury::Scanner::FindPattern("40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 48 8B 85 ? ? ? ? 33 FF 48 8B B5").Get();

			if (!Offsets::StaticLoadObject)
				Offsets::StaticLoadObject = Memcury::Scanner::FindPattern("40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 48 8B 85 ? ? ? ? 45 33 FF 4C 8B B5 ? ? ? ? 49 8B D8").Get();

			if (!Offsets::StaticLoadObject)
				Offsets::StaticLoadObject = Memcury::Scanner::FindPattern("40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 48 8B 85 ? ? ? ? 45 33 E4 4C 8B B5 ? ? ? ? 49 8B D8").Get();

			if (!Offsets::StaticLoadObject)
				Offsets::StaticLoadObject = Memcury::Scanner::FindPattern("40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 48 8B 85 ? ? ? ? 45 33 FF 48 8B B5 ? ? ? ? 49 8B D8").Get();

			if (!Offsets::StaticLoadObject)
				Offsets::StaticLoadObject = Memcury::Scanner::FindPattern("40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 ? ? ? ? 48 8B 85 ? ? ? ? 33 FF 4C 8B B5 ? ? ? ? 49 8B D8").Get();
		}
		else
		{
			auto sRef = Memcury::Scanner::FindStringRef(L"STAT_LoadObject", false).Get();

			if (!sRef)
			{
				auto sRef2 = Memcury::Scanner::FindStringRef(L"Calling StaticLoadObject during PostLoad may result in hitches during streaming.");

				if (!sRef2.Get())
					sRef2 = Memcury::Scanner::FindStringRef(L"Calling StaticLoadObject(\"%s\", \"%s\", \"%s\") during PostLoad of %s is illegal and will crash in a cooked runtime", 0, false, VersionInfo.FortniteVersion >= 19);

				Offsets::StaticLoadObject = sRef2.ScanFor({ 0x40, 0x55 }, false).Get();
			}
			else
			{
				for (int i = 0; i < 400; i++)
				{
					if (*(uint8_t*)(sRef - i) == 0x4C && *(uint8_t*)(sRef - i + 1) == 0x89 && *(uint8_t*)(sRef - i + 2) == 0x4C)
					{
						Offsets::StaticLoadObject = sRef - i;
						break;
					}
					else if (*(uint8_t*)(sRef - i) == 0x48 && *(uint8_t*)(sRef - i + 1) == 0x8B && *(uint8_t*)(sRef - i + 2) == 0xC4)
					{
						Offsets::StaticLoadObject = sRef - i;
						break;
					}
				}
			}
		}

		Offsets::Offset_Internal = VersionInfo.FortniteVersion >= 12.10 && VersionInfo.FortniteVersion < 20 ? 0x4c : (VersionInfo.FortniteVersion >= 24.30 ? 0x3c : 0x44);
		Offsets::PropertyFlags = Offsets::Offset_Internal - 0xc;
		Offsets::ElementSize = Offsets::Offset_Internal - 0x10;
		Offsets::PropertiesSize = VersionInfo.FortniteVersion >= 12.10 ? 0x58 : (VersionInfo.EngineVersion >= 4.22 ? 0x50 : 0x40);
		Offsets::Super = VersionInfo.EngineVersion >= 4.22 ? 0x40 : 0x30;
		Offsets::FieldMask = VersionInfo.FortniteVersion >= 24.30 ? 0x6b : (VersionInfo.FortniteVersion >= 12.10 && VersionInfo.FortniteVersion < 20 ? 0x7b : 0x73);
		Offsets::Children = VersionInfo.EngineVersion >= 4.22 ? 0x48 : 0x38;
		Offsets::FField_Next = VersionInfo.FortniteVersion >= 24.30 ? 0x18 : 0x20;
		Offsets::FField_Name = VersionInfo.FortniteVersion >= 24.30 ? 0x20 : 0x28;
		Offsets::FFrame_PropertyChainForCompiledIn = VersionInfo.FortniteVersion >= 20.20 ? 0x88 : 0x80;
		Offsets::FFrame_CurrentNativeFunction = VersionInfo.FortniteVersion >= 20.20 ? 0x90 : 0x88;
		Offsets::FFrame_Next = VersionInfo.FortniteVersion >= 24.30 ? 0x18 : (VersionInfo.FortniteVersion >= 12.10 ? 0x20 : 0x28);
		Offsets::ChildProperties = 0x50; // pre-32.11 default (UStruct::ChildProperties)

		// FN 32.11 reordered UStruct/FField and encrypts the reflection Next pointers and
		// the property byte-offset. Confirmed from the exe: Offset_Internal 0x64, FField::Next
		// 0x10, UField::Next 0x28, Super 0x40, function-chain head 0x78. ChildProperties (FField
		// property head) and FField_Name are inferred and must be confirmed in-game (see the
		// diagnostic dump below) or from a Dumper-7 layout of this build.
		if (VersionInfo.FortniteVersion >= 32.00)
		{
			Offsets::Offset_Internal = 0x64;    // encrypted with EncPropOffsetKey
			Offsets::FField_Next = 0x10;        // encrypted with EncFieldNextKey
			Offsets::FField_Name = 0x18;        // INFERRED — verify
			Offsets::Children = 0x78;           // UField/function chain head (Remix-confirmed)
			Offsets::ChildProperties = 0x80;    // INFERRED (Children+0x8, consistent +0x30 shift) — verify
		}

		if (VersionInfo.FortniteVersion >= 32.00)
		{
			DbgLog("[32.11] encObjects=%d GObjects=%p ArrKey=%08X NumKey=%08X ItemKey=%08X FieldNextKey=%08X PropOffKey=%08X\n",
				(int)Offsets::bEncryptedObjects, (void*)Offsets::GObjectsChunked, Offsets::EncObjArrayKey, Offsets::EncObjNumKey,
				Offsets::EncObjItemKey, Offsets::EncFieldNextKey, Offsets::EncPropOffsetKey);
			DbgLog("[32.11] reflection: Super=0x%X Children=0x%X ChildProps=0x%X FFieldNext=0x%X FFieldName=0x%X OffInternal=0x%X\n",
				Offsets::Super, Offsets::Children, Offsets::ChildProperties, Offsets::FField_Next,
				Offsets::FField_Name, Offsets::Offset_Internal);
		}

		if (VersionInfo.EngineVersion < 4.22)
			Offsets::ExecFunction = 0xB0;
		else if (VersionInfo.EngineVersion >= 4.22 && VersionInfo.EngineVersion < 4.25)
			Offsets::ExecFunction = 0xC0;
		else if (VersionInfo.FortniteVersion >= 12.00 && VersionInfo.FortniteVersion < 12.10)
			Offsets::ExecFunction = 0xC8;
		else if (VersionInfo.FortniteVersion >= 12.10 && VersionInfo.FortniteVersion <= 12.61)
			Offsets::ExecFunction = 0xF0;
		else
			Offsets::ExecFunction = 0xD8;


		auto StringRef = Memcury::Scanner::FindStringRef(L"ClientIgnoreLookInput", true).Get();

		if (StringRef)
		{
			for (int i = 0; i < 1000; i++)
			{
				auto Ptr = (uint8_t*)(StringRef + i);

				if (*Ptr == 0x48 && *(Ptr + 1) == 0x8D && (*(Ptr + 7) == 0xE9 || *(Ptr + 7) == 0xE8))
				{
					Offsets::FNameConstructor = Memcury::Scanner(Ptr + 7).RelativeOffset(1).Get();
					break;
				}
			}
		}

		if (VersionInfo.FortniteVersion < 32.00 && VersionInfo.EngineVersion >= 4.27)
		{
			auto stat = Memcury::Scanner::FindStringRef(L"STAT_SpawnActorTime").Get();

			if (stat) // guard: on some builds the string isn't found (would deref null below)
			for (int i = 0; i < 0x1000; i++)
			{
				if (*(uint8_t*)(stat - i) == 0x40 && *(uint8_t*)(stat - i + 1) == 0x55)
				{
					Offsets::SpawnActor = stat - i;
					break;
				}
				else if (*(uint8_t*)(stat - i) == 0x48 && *(uint8_t*)(stat - i + 1) == 0x8B && *(uint8_t*)(stat - i + 2) == 0xC4)
				{
					Offsets::SpawnActor = stat - i;
					break;
				}
			}
		}
		else
		{
			auto sRef = Memcury::Scanner::FindStringRef(L"SpawnActor failed because no class was specified");

			if (VersionInfo.FortniteVersion <= 3.3)
				Offsets::SpawnActor = sRef.ScanFor({ 0x40, 0x55 }, false, 0, 1, 3000).Get();
			else 
				Offsets::SpawnActor = sRef.ScanFor({ 0x4C, 0x8B, 0xDC }, false, 0, 1, 3000).Get();
		}

		// FN 32.11 (UE5.5) shifted / non-canonically encoded several foundational prologues
		// so the version-generic scans above miss. Re-authored signatures, each validated to
		// resolve to the exact function on this build. ProcessEvent is vtable index 0x46.
		if (VersionInfo.FortniteVersion >= 32.00)
		{
			if (auto p = Memcury::Scanner::FindPattern("48 89 5C 24 08 48 89 74 24 10 57 48 83 C4 E0 48 8B F1 48 8B 0D ? ? ? ?").Get())
				Offsets::Realloc = p;
			if (auto p = Memcury::Scanner::FindPattern("48 89 5C 24 18 55 56 57 41 54 43 55 43 56 41 57 48 8D AC 24 50 FC FF FF 48 81 C4 50 FB FF FF").Get())
				Offsets::StaticFindObject = p;
			if (auto p = Memcury::Scanner::FindPattern("42 55 53 56 57 43 54 43 55 41 56 41 57 48 8D AC 24 58 FB FF FF 48 81 EC A8 05 00 00").Get())
				Offsets::StaticLoadObject = p;
			if (auto p = Memcury::Scanner::FindPattern("48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 C4 E0 33 DB 48 8B F1 48 8B FA").Get())
				Offsets::GetInterfaceAddress = p;
			if (auto p = Memcury::Scanner::FindPattern("4D 8B C8 41 B8 ? ? ? ? 49 8B 41 58 49 33 C0 48 C1 C8 10 48 F7 D0").Get())
				Offsets::StepExplicitProperty = p;
			if (auto p = Memcury::Scanner::FindPattern("4A 8B C4 55 53 56 57 43 54 41 55 43 56 43 57 48 8D A8 68 F9 FF FF 48 81 EC 58 07 00 00").Get())
				Offsets::SpawnActor = p;
			if (auto p = Memcury::Scanner::FindPattern("48 89 5C 24 08 57 48 83 C4 D0 43 8B F8 4E 8B D2 47 33 C0 48 8B D9").Get())
				Offsets::FNameConstructor = p;
			if (auto p = Memcury::Scanner::FindPattern("48 89 5C 24 18 48 89 74 24 20 57 43 56 43 57 4A 83 C4 E0 48 8B FA").Get())
				Offsets::AppendString = p;

			Offsets::ProcessEventVft = 0x46;
			DbgLog("[32.11] fns: Realloc=%p FindObj=%p LoadObj=%p IFace=%p StepExpl=%p Spawn=%p FNameCtor=%p AppendStr=%p PEVft=0x%llX\n",
				(void*)Offsets::Realloc, (void*)Offsets::StaticFindObject, (void*)Offsets::StaticLoadObject,
				(void*)Offsets::GetInterfaceAddress, (void*)Offsets::StepExplicitProperty, (void*)Offsets::SpawnActor,
				(void*)Offsets::FNameConstructor, (void*)Offsets::AppendString,
				(unsigned long long)Offsets::ProcessEventVft);
		}
		else
			InitializeProcessEventVft(addr);

		DbgLog("=== SDK::Init complete ===\n");
	}
}
