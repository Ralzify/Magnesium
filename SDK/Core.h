#pragma once
#include <tuple>
using namespace UC;

typedef uint64_t uint64;
typedef uint32_t uint32;
typedef uint16_t uint16;
typedef uint8_t uint8;
typedef int64_t int64;
typedef int32_t int32;
typedef int16_t int16;
typedef int8_t int8;
inline uint64_t ImageBase = *(uint64_t*)(__readgsqword(0x60) + 0x10);

namespace SDK
{
	// FN 32.11+ stores GObjects/Object/reflection-Next pointers as ~ROR16(value ^ key).
	__forceinline uint64_t DecryptObjPtr(uint64_t Encrypted, uint32_t Key)
	{
		uint64_t v = Encrypted ^ (uint64_t)Key;
		v = (v >> 16) | (v << 48); // ROR 16
		return ~v;
	}

	// FN 32.11+ stores the FProperty byte-offset as ~(value ^ key).
	__forceinline uint32_t DecryptPropOffset(uint32_t Raw)
	{
		if (Offsets::bEncryptedObjects && Offsets::EncPropOffsetKey)
			return ~(Raw ^ Offsets::EncPropOffsetKey);
		return Raw;
	}

	// Reliable "is this range committed & readable" check via VirtualQuery (IsBadReadPtr lies).
	inline bool MemReadable(const void* p, size_t n)
	{
		if (!p)
			return false;
		auto addr = (const uint8_t*)p;
		auto end = addr + n;
		while (addr < end)
		{
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(addr, &mbi, sizeof(mbi)))
				return false;
			if (mbi.State != MEM_COMMIT)
				return false;
			DWORD prot = mbi.Protect & 0xFF;
			if (!(prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)))
				return false;
			if (mbi.Protect & PAGE_GUARD)
				return false;
			addr = (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
		}
		return true;
	}

	class FName
	{
	public:
		int32 ComparisonIndex;
		int32 Number;

		FName(int32 InComparisonIndex = 0, int32 InNumber = 0) noexcept
			: ComparisonIndex(InComparisonIndex)
		{
			if (VersionInfo.FortniteVersion < 20.00)
				Number = InNumber;
		}

		FName(const wchar_t* String)
		{
			auto& FName__Ctor = (void(*&)(FName*, const wchar_t*, int)) Offsets::FNameConstructor;

			FName__Ctor(this, String, 1);
		}

		FName(UEAllocatedWString String)
		{
			auto& FName__Ctor = (void(*&)(FName*, const wchar_t*, int)) Offsets::FNameConstructor;

			FName__Ctor(this, String.c_str(), 1);
		}

		FName(FString String)
		{
			auto& FName__Ctor = (void(*&)(FName*, const wchar_t*, int)) Offsets::FNameConstructor;

			FName__Ctor(this, String.CStr(), 1);
		}

		bool IsValid() const
		{
			// Encrypted FName indices (FN 32.11+) aren't ordered, so ">0" is wrong; None is still 0.
			if (Offsets::bEncryptedObjects)
				return ComparisonIndex != 0;
			return ComparisonIndex > 0;
		}

		UEAllocatedString ToString() const
		{
			if (!Offsets::AppendString)
			{
				if (IsBadReadPtr((void*)this))
					return "";
				FString TempString(1024);

				auto ToString = (void(*&)(const FName*, FString&)) Offsets::ToString;
				ToString(this, TempString);

				UEAllocatedString OutputString = TempString.ToString();
				TempString.Free();

				return OutputString;
			}

			thread_local FString TempString(1024);

			auto AppendString = (void(*&)(const FName*, FString&)) Offsets::AppendString;
			AppendString(this, TempString);

			UEAllocatedString OutputString = TempString.ToString();
			TempString.Clear();

			return OutputString;
		}

		UEAllocatedString ToSDKString() const
		{
			UEAllocatedString OutputString = ToString();

			size_t pos = OutputString.rfind('/');

			if (pos == UEAllocatedString::npos)
				return OutputString;

			return OutputString.substr(pos + 1);
		}

		UEAllocatedWString ToWString() const
		{
			if (!Offsets::AppendString)
			{
				if (IsBadReadPtr((void*)this))
					return L"";
				FString TempString(1024);

				auto ToString = (void(*&)(const FName*, FString&)) Offsets::ToString;
				ToString(this, TempString);

				UEAllocatedWString OutputString = TempString.ToWString();
				TempString.Free();

				return OutputString;
			}

			thread_local FString TempString(1024);

			auto AppendString = (void(*&)(const FName*, FString&)) Offsets::AppendString;
			AppendString(this, TempString);

			UEAllocatedWString OutputString = TempString.ToWString();
			TempString.Clear();

			return OutputString;
		}

		UEAllocatedWString ToSDKWString() const
		{
			UEAllocatedWString OutputString = ToWString();

			size_t pos = OutputString.rfind('/');

			if (pos == UEAllocatedWString::npos)
				return OutputString;

			return OutputString.substr(pos + 1);
		}

		std::string ToUtf8() const
		{
			UEAllocatedWString Wide = ToWString();

			if (Wide.empty())
				return {};

			int size = WideCharToMultiByte(CP_UTF8, 0, Wide.c_str(), -1, nullptr, 0, nullptr, nullptr);

			std::string Out(size - 1, '\0');

			WideCharToMultiByte(CP_UTF8, 0, Wide.c_str(), -1, Out.data(), size, nullptr, nullptr);

			return Out;
		}

		bool operator==(const FName& Other) const
		{
			return ComparisonIndex == Other.ComparisonIndex && (VersionInfo.FortniteVersion >= 20.00 || Number == Other.Number);
		}
		bool operator!=(const FName& Other) const
		{
			return ComparisonIndex != Other.ComparisonIndex || (VersionInfo.FortniteVersion >= 20.00 ? false : Number != Other.Number);
		}

		bool operator<(const FName& Other) const
		{
			return ComparisonIndex == Other.ComparisonIndex ? (VersionInfo.FortniteVersion < 20.00 && Number < Other.Number) : ComparisonIndex < Other.ComparisonIndex;
		}

		operator bool() const {
			return IsValid();
		}
	};

	class ParamPair
	{
	public:
		UEAllocatedString ParamName;
		void* Value;

		template <typename ValueType>
		ParamPair(UEAllocatedString Name, ValueType Val)
		{
			ParamName = Name;
			// really scuffed way to make this work, just using & gives the same address for each param
			Value = FMemory::Malloc(sizeof(ValueType));
			memcpy(Value, &Val, sizeof(ValueType));
		}
	};

	template <typename _Ot>
	__forceinline _Ot& GetFromOffset(void* Obj, uint32 Offset)
	{
		return *(_Ot*)(__int64(Obj) + Offset);
	}

	template <typename _Ot>
	__forceinline _Ot& GetFromOffset(const void* Obj, uint32 Offset)
	{
		return *(_Ot*)(__int64(Obj) + Offset);
	}

	template <typename _Ot>
	__forceinline _Ot* GetPtrFromOffset(const void* Obj, uint32 Offset)
	{
		return (_Ot*)(__int64(Obj) + Offset);
	}

	class FStructBaseChain
	{
	public:
		FStructBaseChain()
			: StructBaseChainArray(nullptr)
			, NumStructBasesInChainMinusOne(-1)
		{
		}
		~FStructBaseChain()
		{
			delete[] StructBaseChainArray;
		}

		FStructBaseChain(const FStructBaseChain&) = delete;
		FStructBaseChain& operator=(const FStructBaseChain&) = delete;

		__forceinline bool IsChildOfUsingStructArray(const FStructBaseChain& Parent) const
		{
			int32 NumParentStructBasesInChainMinusOne = Parent.NumStructBasesInChainMinusOne;
			return NumParentStructBasesInChainMinusOne <= NumStructBasesInChainMinusOne && StructBaseChainArray[NumParentStructBasesInChainMinusOne] == &Parent;
		}

	private:
		FStructBaseChain** StructBaseChainArray;
		int32 NumStructBasesInChainMinusOne;

		friend class UStruct;
	};

	// FN 32.11 reordered UObject: Name and ObjectFlags are swapped (0x08 <-> 0x18) and the
	// internal index is encrypted (~(x ^ 0x693FB2C4)). These proxies live at Magnesium's
	// historical member offsets but read/write the version-correct location, so every
	// existing Obj->Name / Obj->Index / Obj->ObjectFlags call site keeps working unchanged.
	struct FObjFlagsProxy // Magnesium offset 0x08
	{
		int32 _raw;
		__forceinline int32* ptr() const
		{
			return VersionInfo.FortniteVersion >= 32.00 ? (int32*)((char*)this + 0x10) : (int32*)&_raw;
		}
		__forceinline operator int32() const { return *ptr(); }
		__forceinline FObjFlagsProxy& operator=(int32 v) { *ptr() = v; return *this; }
		__forceinline FObjFlagsProxy& operator&=(int32 v) { *ptr() &= v; return *this; }
		__forceinline FObjFlagsProxy& operator|=(int32 v) { *ptr() |= v; return *this; }
	};
	struct FObjIndexProxy // Magnesium offset 0x0C
	{
		int32 _raw;
		__forceinline operator int32() const
		{
			return VersionInfo.FortniteVersion >= 32.00 ? ~(_raw ^ 0x693FB2C4) : _raw;
		}
	};
	struct FNameProxy // Magnesium offset 0x18
	{
		FName _raw;
		__forceinline FName& ref() const
		{
			return VersionInfo.FortniteVersion >= 32.00 ? *(FName*)((char*)this - 0x10) : *(FName*)&_raw;
		}
		__forceinline operator FName() const { return ref(); }
		__forceinline UEAllocatedString ToString() const { return ref().ToString(); }
		__forceinline UEAllocatedWString ToWString() const { return ref().ToWString(); }
		__forceinline UEAllocatedString ToSDKString() const { return ref().ToSDKString(); }
		__forceinline UEAllocatedWString ToSDKWString() const { return ref().ToSDKWString(); }
		__forceinline std::string ToUtf8() const { return ref().ToUtf8(); }
		__forceinline bool IsValid() const { return ref().IsValid(); }
		__forceinline bool operator==(const FName& o) const { return ref() == o; }
		__forceinline bool operator!=(const FName& o) const { return ref() != o; }
		__forceinline FNameProxy& operator=(const FName& o) { ref() = o; return *this; }
	};

	class UObject
	{
	public:
		void** Vft;
		FObjFlagsProxy ObjectFlags;
		FObjIndexProxy Index;
		class UClass* Class;
		FNameProxy Name;
		UObject* Outer;

	public:
		const class UField* GetProperty(const char* Name, uint64_t CastFlags = 0) const;

		bool IsDefaultObject() const
		{
			return (int32)ObjectFlags & 0x10;
		}

		__declspec(noinline) uint32 GetOffset(const char* Name, uint64_t CastFlags = 0) const
		{
			auto Prop = GetProperty(Name, CastFlags);
			if (!Prop) return -1;
			return DecryptPropOffset(GetFromOffset<uint32>(Prop, Offsets::Offset_Internal));
		}

		bool IsA(const class UClass* Clss) const
		{
			if (!this || !Clss)
				return false;

			if (VersionInfo.EngineVersion >= 4.22)
			{
				auto& BaseChain = GetFromOffset<FStructBaseChain>(Class, 0x30);
				auto& BaseChainOther = GetFromOffset<FStructBaseChain>(Clss, 0x30);

				return BaseChain.IsChildOfUsingStructArray(BaseChainOther);
			}

			for (auto _Clss = Class; _Clss; _Clss = GetFromOffset<UClass*>(_Clss, 0x30))
			{
				if (_Clss == Clss) return true;
			}

			return false;
		}

		template <class T>
		bool IsA() const
		{
			return IsA(T::StaticClass());
		}

		template <class T>
		T* Cast(const class UClass* Clss = T::StaticClass()) const
		{
			return IsA(Clss) ? (T*)this : nullptr;
		}

		class UFunction* GetFunction(const char* Name) const;
		class UFunction* GetFunction(FName Name) const;

		void ProcessEvent(class UFunction* Function, void* Params) const
		{
			((void(*&)(const UObject*, class UFunction*, void*)) Vft[Offsets::ProcessEventVft])(this, Function, Params);
		}

		template <typename Ret = void, typename... Args>
		Ret Call(UFunction* Function, Args&&... args) const;

		static const UClass* StaticClass();

		const class IInterface* GetInterface(const class UClass*) const;

		void AddToRoot() const;
	};

	class UField : public UObject
	{
	public:
		const UField* FField_GetNext() const
		{
			if (Offsets::bEncryptedObjects && Offsets::EncFieldNextKey)
			{
				uint64_t Enc = GetFromOffset<uint64_t>(this, Offsets::FField_Next);
				return Enc ? (const UField*)DecryptObjPtr(Enc, Offsets::EncFieldNextKey) : nullptr;
			}
			return GetFromOffset<UField*>(this, Offsets::FField_Next);
		}

		FName& FField_GetName() const
		{
			return GetFromOffset<FName>(this, Offsets::FField_Name);
		}

		const UField* GetNext() const
		{
			if (Offsets::bEncryptedObjects && Offsets::EncFieldNextKey)
			{
				uint64_t Enc = GetFromOffset<uint64_t>(this, 0x28);
				return Enc ? (const UField*)DecryptObjPtr(Enc, Offsets::EncFieldNextKey) : nullptr;
			}
			return GetFromOffset<UField*>(this, 0x28);
		}

		FName& GetName() const
		{
			// A UField is a UObject, so its name lives at the (reordered on 32.11) UObject::Name offset.
			return GetFromOffset<FName>(this, VersionInfo.FortniteVersion >= 32.00 ? 0x08 : 0x18);
		}

		const uint8 GetFieldMask() const
		{
			return GetFromOffset<uint8>(this, Offsets::FieldMask);
		}
	};

	class UStruct : public UField
	{
	public:
		const UStruct* GetSuper() const
		{
			return GetFromOffset<UStruct*>(this, Offsets::Super);
		}

		const int32 GetPropertiesSize() const
		{
			return GetFromOffset<int32>(this, Offsets::PropertiesSize);
		}

		const UField* GetChildProperties() const
		{
			uint32 off = Offsets::ChildProperties ? Offsets::ChildProperties : 0x50;
			if (Offsets::bEncChildProperties && Offsets::EncFieldNextKey)
			{
				uint64_t Enc = GetFromOffset<uint64_t>(this, off);
				return Enc ? (const UField*)DecryptObjPtr(Enc, Offsets::EncFieldNextKey) : nullptr;
			}
			return GetFromOffset<UField*>(this, off);
		}

		const UField* GetChildren() const
		{
			return GetFromOffset<UField*>(this, Offsets::Children);
		}


		const UField* GetProperty(const char* Name, uint64_t CastFlags = 0) const;

		uint32_t GetOffset(const char* Name, uint64_t CastFlags = 0) const
		{
			auto Prop = GetProperty(Name, CastFlags);
			if (!Prop)
				return -1;

			return DecryptPropOffset(GetFromOffset<uint32>(Prop, Offsets::Offset_Internal));
		}
	};

	class UClass : public UStruct
	{
	public:
		uint64_t GetCastFlags() const;
		static const UClass* StaticClass();

		UObject* GetDefaultObj() const;
	};

	__declspec(noinline) inline const UField* UStruct::GetProperty(const char* Name, uint64_t CastFlags) const
	{
		UEAllocatedString s = Name;
		UEAllocatedWString ws(s.begin(), s.end());
		auto PropName = FName(ws);

		int superGuard = 0;
		for (const UStruct* Clss = this; Clss && superGuard++ < 4096; Clss = (const UStruct*)Clss->GetSuper())
		{
			int fieldGuard = 0;
			if (VersionInfo.FortniteVersion >= 12.10)
			{
				for (const UField* Prop = Clss->GetChildProperties(); Prop && fieldGuard++ < 100000; Prop = Prop->FField_GetNext())
				{
					// FN 32.11 reorders/encrypts FField::ClassPrivate and FFieldClass::CastFlags, so
					// dereferencing Prop+0x8 -> +0x10 for the cast-flag filter reads garbage/encrypted
					// memory and faults. ChildProperties only holds properties (unique names within a
					// class), so a name-only match is correct — skip the filter on 32.11.
					if (CastFlags != 0 && VersionInfo.FortniteVersion < 32.00)
					{
						auto FieldClass = *(void**)(__int64(Prop) + 0x8);
						auto FieldFlags = *(uint64_t*)(__int64(FieldClass) + 0x10);

						if ((FieldFlags & CastFlags) == 0)
							continue;
					}

					if (Prop->FField_GetName() == PropName)
						return Prop;
				}
			}
			else
			{
				for (const UField* Prop = Clss->GetChildren(); Prop && fieldGuard++ < 100000; Prop = Prop->GetNext())
				{
					if ((CastFlags == 0 || Prop->Class->GetCastFlags() & CastFlags) && Prop->GetName() == PropName)
						return Prop;
				}
			}
		}

		return nullptr;
	}

	inline const UField* UObject::GetProperty(const char* Name, uint64_t CastFlags) const
	{
		return Class->GetProperty(Name, CastFlags);
	}

	__declspec(noinline) inline UFunction* UObject::GetFunction(const char* Name) const
	{
		UEAllocatedString s = Name;
		UEAllocatedWString ws(s.begin(), s.end());
		auto PropName = FName(ws);

		// Children() is the UFunction chain, so a name match is sufficient. Requiring
		// GetCastFlags (CASTCLASS_UFunction) breaks on 32.11 where that offset isn't resolved yet.
		int superGuard = 0;
		for (const UStruct* Clss = Class; Clss && superGuard++ < 4096; Clss = (const UStruct*)Clss->GetSuper())
		{
			int fieldGuard = 0;
			for (const UField* Prop = Clss->GetChildren(); Prop && fieldGuard++ < 100000; Prop = Prop->GetNext())
				if ((VersionInfo.FortniteVersion >= 32.00 || Prop->Class->GetCastFlags() & 0x80000) && Prop->GetName() == PropName)
					return (UFunction*)Prop;
		}

		return nullptr;
		//return (UFunction*)GetProperty(Name, 0x80000);
	}


	__declspec(noinline) inline UFunction* UObject::GetFunction(FName Name) const
	{
		int superGuard = 0;
		for (const UStruct* Clss = Class; Clss && superGuard++ < 4096; Clss = (const UStruct*)Clss->GetSuper())
		{
			int fieldGuard = 0;
			for (const UField* Prop = Clss->GetChildren(); Prop && fieldGuard++ < 100000; Prop = Prop->GetNext())
				if ((VersionInfo.FortniteVersion >= 32.00 || Prop->Class->GetCastFlags() & 0x80000) && Prop->GetName() == Name)
					return (UFunction*)Prop;
		}

		return nullptr;
		//return (UFunction*)GetProperty(Name, 0x80000);
	}

	class UFunction : public UStruct
	{
	public:
		void*& GetNativeFunc() const
		{
			return GetFromOffset<void*>(this, Offsets::ExecFunction);
		}

		void SetNativeFunc(void* NewFunc) const
		{
			GetNativeFunc() = NewFunc;
		}

		__declspec(property(get = GetNativeFunc, put = SetNativeFunc))
			void* ExecFunction;


		void* GetImpl() const
		{
			if (!this)
				return nullptr;

			auto setnzAddr = Memcury::Scanner(GetNativeFunc()).ScanFor({ 0x0F, 0x95 }).Get();

			for (int i = 0; i < 0x200; i++)
			{
				auto Ptr = (uint8_t*)(setnzAddr + i);

				if (*Ptr == 0xe9 || *Ptr == 0xe8)
					return Memcury::Scanner(Ptr).RelativeOffset(1).GetAs<void*>();
			}
			return nullptr;

		}

		void Call(const UObject* obj, void* Params)
		{
			if (this)
				obj->ProcessEvent(this, Params);
		}

		void Call(const UObject* obj, UEAllocatedVector<ParamPair> Params)
		{
			//if (this) 
				//obj->ProcessEvent(this, CreateParams(Params));
		}

		void operator()(const UObject* obj, void* Params)
		{
			return Call(obj, Params);
		}

		uint32 GetVTableIndex() const
		{
			if (!this)
				return -1;

			auto ValidateName = Name.ToString() + "_Validate";
			auto ValidateRef = Memcury::Scanner::FindStringRef(UEAllocatedWString(ValidateName.begin(), ValidateName.end()).c_str(), false);

			auto Addr = ValidateRef.Get();

			if (!Addr)
			{
				// FN 32.11: GetNativeFunc() may read a reordered offset and return garbage; only
				// scan from it if it's a readable code pointer, else bail (never fault).
				auto nf = (uintptr_t)GetNativeFunc();
				if (nf && MemReadable((void*)nf, 0x10))
					Addr = Memcury::Scanner(nf).ScanFor({ 0x0F, 0x95 }).Get();
			}

			if (Addr && MemReadable((void*)Addr, 0x10))
				for (int i = 0; i < 2000; i++)
				{
					if (!MemReadable((void*)(Addr + i), 6))
						break;
					if (*((uint8*)Addr + i) == 0xFF && (*((uint8*)Addr + i + 1) == 0x90 || *((uint8*)Addr + i + 1) == 0x93 || *((uint8*)Addr + i + 1) == 0xA0))
					{
						auto VTIndex = *(uint32_t*)(Addr + i + 2);

						return VTIndex / 8;
					}
				}

			return -1;
		}

		static const UClass* StaticClass();

		struct Param
		{
			//UEAllocatedString Name;
			uint32 Offset;
			uint64 PropertyFlags;
			uint32 ElementSize;
		};
		class Params
		{
		public:
			UEAllocatedVector<Param> NameOffsetMap;
			uint32 Size;
		};


		struct ParamNamed
		{
			UEAllocatedString Name;
			uint32 Offset;
			uint64 PropertyFlags;
			uint32 ElementSize;
		};
		class ParamsNamed
		{
		public:
			UEAllocatedVector<ParamNamed> NameOffsetMap;
			uint32 Size;
		};

		Params GetParams() const
		{
			Params p{};

			if (VersionInfo.FortniteVersion >= 12.10)
				for (const UField* _Pr = GetChildProperties(); _Pr; _Pr = _Pr->FField_GetNext())
					p.NameOffsetMap.push_back({ DecryptPropOffset(GetFromOffset<uint32>(_Pr, Offsets::Offset_Internal)), GetFromOffset<uint64>(_Pr, Offsets::PropertyFlags), GetFromOffset<uint32>(_Pr, Offsets::ElementSize) });
			else
				for (const UField* _Pr = GetChildren(); _Pr; _Pr = _Pr->GetNext())
					p.NameOffsetMap.push_back({ DecryptPropOffset(GetFromOffset<uint32>(_Pr, Offsets::Offset_Internal)), GetFromOffset<uint64>(_Pr, Offsets::PropertyFlags), GetFromOffset<uint32>(_Pr, Offsets::ElementSize) });

			p.Size = GetPropertiesSize();
			return p;
		}


		ParamsNamed GetParamsNamed() const
		{
			ParamsNamed p{};

			if (VersionInfo.FortniteVersion >= 12.10)
				for (const UField* _Pr = GetChildProperties(); _Pr; _Pr = _Pr->FField_GetNext())
					p.NameOffsetMap.push_back({ _Pr->FField_GetName().ToSDKString(), DecryptPropOffset(GetFromOffset<uint32>(_Pr, Offsets::Offset_Internal)), GetFromOffset<uint64>(_Pr, Offsets::PropertyFlags), GetFromOffset<uint32>(_Pr, Offsets::ElementSize) });
			else
				for (const UField* _Pr = GetChildren(); _Pr; _Pr = _Pr->GetNext())
					p.NameOffsetMap.push_back({ _Pr->GetName().ToSDKString(), DecryptPropOffset(GetFromOffset<uint32>(_Pr, Offsets::Offset_Internal)), GetFromOffset<uint64>(_Pr, Offsets::PropertyFlags), GetFromOffset<uint32>(_Pr, Offsets::ElementSize) });

			p.Size = GetPropertiesSize();
			return p;
		}
	};

	template <typename Ret, typename... Args>
	Ret UObject::Call(UFunction* Function, Args&&... args) const
	{

		if (!Function)
			return Ret();

		// fast paths
		if constexpr (sizeof...(args) == 0 && std::is_void_v<Ret>)
			return ProcessEvent(Function, nullptr);

		if constexpr (sizeof...(args) == 1 && std::is_void_v<Ret>)
		{
			return ProcessEvent(Function, (void*)std::addressof((std::get<0>(std::forward_as_tuple(args...)))));
		}

		if constexpr (sizeof...(args) == 0 && !std::is_void_v<Ret>)
		{
			Ret ret{};

			ProcessEvent(Function, &ret);

			return ret;
		}

		auto Params = Function->GetParams();

		// FN 32.11 encrypts/relocates the FProperty ElementSize/PropertyFlags fields and the UStruct
		// PropertiesSize, so the reflection-derived sizes/flags are unreliable (and Malloc(garbage)
		// crashes). ProcessEvent uses the UFunction's own ParmsSize internally, so a fixed over-alloc
		// is safe; copy each arg by its true C++ size to the correctly-resolved Offset, and skip the
		// PropertyFlags-based out-param filter/copy-back (boot calls are in-param/void).
		const bool bFN32 = VersionInfo.FortniteVersion >= 32.00;
		const size_t allocSize = bFN32 ? 0x1000 : Params.Size;
		auto Mem = FMemory::Malloc(allocSize);
		memset((PBYTE)Mem, 0, allocSize);

		size_t i = 0;
		([&]
			{
				if (i >= Params.NameOffsetMap.size())
					return;

				auto& Param = Params.NameOffsetMap[i];

				if (!bFN32 && (((Param.PropertyFlags & 0x100) != 0 && (Param.PropertyFlags & 0x8000000) == 0) || (Param.PropertyFlags & 0x400) != 0))
				{
					i++;
					return;
				}

				const auto& Arg = args;

				const size_t copySize = bFN32 ? sizeof(std::remove_reference_t<decltype(args)>) : Param.ElementSize;
				memcpy(PBYTE(__int64(Mem) + Param.Offset), (const PBYTE)&Arg, copySize);
				i++;
			}(), ...);

		ProcessEvent(Function, Mem);

		if (!bFN32)
		{
			i = 0;
			([&] {
				if (i >= Params.NameOffsetMap.size())
					return;

				auto& Param = Params.NameOffsetMap[i];

				if (((Param.PropertyFlags & 0x100) == 0 && (Param.PropertyFlags & 0x8000000) == 0) || (Param.PropertyFlags & 0x400) != 0)
				{
					i++;
					return;
				}

				const auto& Arg = args;

				if constexpr (std::is_pointer_v<std::remove_reference_t<decltype(args)>>)
				{
					if (Arg != nullptr)
						memcpy((PBYTE)Arg, (const PBYTE)(__int64(Mem) + Param.Offset), Param.ElementSize);
				}
				i++;
				}(), ...);
		}

		if constexpr (!std::is_void_v<Ret>)
		{
			Ret ret{};
			for (auto& Param : Params.NameOffsetMap)
			{
				if ((Param.PropertyFlags & 0x400) == 0)
					continue;

				memcpy((PBYTE)&ret, (const PBYTE)(__int64(Mem) + Param.Offset), Param.ElementSize);
				break;
			}

			FMemory::Free(Mem);
			return ret;
		}

		FMemory::Free(Mem);
	}


	struct FUObjectItem final
	{
	public:
		class UObject* Object;
		int32 Flags;
		int32 ClusterRootIndex;
		int32 SerialNumber;

		// On encrypted builds (FN 32.11+) the live pointer is at +0x10 and the
		// reachability flags word is at +0x4 (this struct's plain layout is unused there).
		const UObject* GetObject() const
		{
			if (Offsets::bEncryptedObjects)
			{
				uint64_t Enc = *(const uint64_t*)((const uint8_t*)this + 0x10);
				return Enc ? (const UObject*)DecryptObjPtr(Enc, Offsets::EncObjItemKey) : nullptr;
			}
			return Object;
		}

		int32 GetFlags() const
		{
			return Offsets::bEncryptedObjects ? *(const int32*)((const uint8_t*)this + 0x4) : Flags;
		}

		int32& SerialRef()
		{
			if (Offsets::bEncryptedObjects)
				return *(int32*)((uint8_t*)this + 0x8); // SerialNumber sits at +0x8 on encrypted builds
			return SerialNumber;
		}
	};

	class TUObjectArrayUnchunked
	{
	private:
		FUObjectItem* Objects;
		const int32 MaxElements;
		const int32 NumElements;

	public:
		inline int Num() const
		{
			return NumElements;
		}

		inline int Max() const
		{
			return MaxElements;
		}

		inline FUObjectItem* GetItemByIndex(const int32 Index) const
		{
			if (Index < 0 || Index > NumElements)
				return nullptr;

			return Objects + Index;
		}
	};

	class TUObjectArrayChunked
	{
	public:
		static inline auto NumElementsPerChunk = 0x10000;
	private:
		FUObjectItem** Objects;
		FUObjectItem* PreAllocatedObjects;
		const int32 MaxElements;
		const int32 NumElements;
		const int32 MaxChunks;
		const int32 NumChunks;

	public:
		inline int Num() const
		{
			if (Offsets::bEncryptedObjects)
				return ~(*(const uint32_t*)&NumElements ^ Offsets::EncObjNumKey);
			return NumElements;
		}

		inline int Max() const
		{
			return MaxElements;
		}

		inline FUObjectItem* GetItemByIndex(const int32 Index) const
		{
			if (Index < 0 || Index > Num())
				return nullptr;

			const int32 ChunkIndex = Index / NumElementsPerChunk;
			const int32 ChunkOffset = Index % NumElementsPerChunk;

			FUObjectItem** Chunks = Objects;
			if (Offsets::bEncryptedObjects)
				Chunks = (FUObjectItem**)DecryptObjPtr((uint64_t)Objects, Offsets::EncObjArrayKey);

			return Chunks[ChunkIndex] + ChunkOffset;
		}
	};

	class TUObjectArray
	{
	public:
		static const int32 Num()
		{
			auto GObjectsChunked = (TUObjectArrayChunked*&)Offsets::GObjectsChunked;
			auto GObjectsUnchunked = (TUObjectArrayUnchunked*&)Offsets::GObjectsUnchunked;
			return GObjectsChunked ? GObjectsChunked->Num() : GObjectsUnchunked->Num();
		}

		static const int32 Max()
		{
			auto GObjectsChunked = (TUObjectArrayChunked*&)Offsets::GObjectsChunked;
			auto GObjectsUnchunked = (TUObjectArrayUnchunked*&)Offsets::GObjectsUnchunked;
			return GObjectsChunked ? GObjectsChunked->Max() : GObjectsUnchunked->Max();
		}

		static FUObjectItem* GetItemByIndex(const int32 Index)
		{
			auto GObjectsChunked = (TUObjectArrayChunked*&)Offsets::GObjectsChunked;
			auto GObjectsUnchunked = (TUObjectArrayUnchunked*&)Offsets::GObjectsUnchunked;
			return GObjectsChunked ? GObjectsChunked->GetItemByIndex(Index) : GObjectsUnchunked->GetItemByIndex(Index);
		}

		static const UObject* GetObjectByIndex(const int32 Index)
		{
			const FUObjectItem* Item = GetItemByIndex(Index);
			return Item ? Item->GetObject() : nullptr;
		}

		static const UObject* FindObject(const char* Name, uint64 TypeFlags = 0, const UClass* TargetClass = nullptr)
		{
			UEAllocatedString s = Name;
			UEAllocatedWString ws(s.begin(), s.end());
			auto ObjName = FName(ws);

			// Unreachable/pending-kill mask differs on encrypted builds (matches the game's own check).
			const int32 SkipFlags = Offsets::bEncryptedObjects ? 0x10200000 : 0x20;

			for (int i = 0; i < Num(); i++)
			{
				const FUObjectItem* Item = GetItemByIndex(i);
				if (!Item || (Item->GetFlags() & SkipFlags))
					continue;

				const UObject* Obj = Item->GetObject();
				// On 32.11 ClassCastFlags is encrypted/unresolved, so skip the cast-flag pre-filter
				// and rely on the name (+ optional TargetClass) match instead.
				const bool typeOk = (TypeFlags == 0) || Offsets::bEncryptedObjects || (Obj && Obj->Class && (Obj->Class->GetCastFlags() & TypeFlags));
				if (Obj && Obj->Class && typeOk && Obj->Name == ObjName && (!TargetClass || Obj->IsA(TargetClass)))
					return Obj;
			}
			return nullptr;
		}

		template <typename _Et = UObject>
		static const _Et* FindObject(const char* Name, uint64 TypeFlags = 0, const UClass* TargetClass = _Et::StaticClass())
		{
			return (const _Et*)FindObject(Name, TypeFlags, TargetClass);
		}

		template <typename _Et = UObject>
		static const _Et* FindObject(const std::string& Name, uint64 TypeFlags = 0, const UClass* TargetClass = _Et::StaticClass())
		{
			return FindObject<_Et>(Name.c_str(), TypeFlags, TargetClass);
		}

		static const UObject* FindFirstObject(const char* Name)
		{
			UClass* TargetClass = (UClass*)FindObject(Name, 0x20);

			if (TargetClass)
				for (int i = 0; i < Num(); i++)
				{
					const UObject* Obj = GetObjectByIndex(i);
					if (Obj && !Obj->IsDefaultObject() && Obj->IsA(TargetClass))
						return Obj;
				}

			return nullptr;
		}
	};


	inline void UObject::AddToRoot() const
	{
		if (!this)
			return;

		auto Item = (FUObjectItem*)TUObjectArray::GetItemByIndex(Index);

		if (Item)
		{
			if (Offsets::bEncryptedObjects)
				*(int32*)((uint8_t*)Item + 0x4) |= 1 << 30; // flags word is at +0x4 on encrypted builds
			else
				Item->Flags |= 1 << 30;
		}
	}

	inline const UClass* FindClass(const char* Name)
	{
		return (UClass*)TUObjectArray::FindObject(Name, 0x20);
	}

	inline const UObject* DefaultObjImpl(const char* Name)
	{
		auto TargetClass = FindClass(Name);
		for (int i = 0; i < TUObjectArray::Num(); i++)
		{
			const UObject* Obj = TUObjectArray::GetObjectByIndex(i);

			if (Obj && Obj->IsDefaultObject() && Obj->Class == TargetClass)
				return Obj;
		}
		return nullptr;
	}

	inline const UObject* DefaultObjImpl(const UClass* TargetClass, const char* Name)
	{
		for (int i = 0; i < TUObjectArray::Num(); i++)
		{
			const UObject* Obj = TUObjectArray::GetObjectByIndex(i);

			if (Obj && Obj->IsDefaultObject() && Obj->Class == TargetClass)
				return Obj;
		}
		return nullptr;
	}


	inline uint64_t UClass::GetCastFlags() const
	{
		// Detect the ClassCastFlags offset once and cache it. The old code re-scanned the whole
		// object array on every call when it couldn't find the offset (Offset stayed 0) — on 32.11
		// the range was too small (UStruct grew), which froze the server. Now: cache via Tried,
		// widen the range, and guard reads so a bad offset can never fault.
		static int32 Offset = 0;
		static bool Tried = false;
		if (!Tried)
		{
			Tried = true;
			auto ClassObj = TUObjectArray::FindObject("Class");
			auto ActorObj = TUObjectArray::FindObject("Actor");
			if (ClassObj && ActorObj)
			{
				for (int i = 0x28; i < 0x400; i += 4)
				{
					if (!MemReadable((const char*)ClassObj + i, 8) || !MemReadable((const char*)ActorObj + i, 8))
						break;
					if (*(uint64_t*)(__int64(ClassObj) + i) == 0x29 && *(uint64_t*)(__int64(ActorObj) + i) == 0x1000000000)
					{
						Offset = i;
						break;
					}
				}
			}
			DbgLog("[32.11] GetCastFlags offset resolved = 0x%X\n", Offset);
		}

		return Offset ? *(uint64_t*)(__int64(this) + Offset) : 0;
	}

	inline UObject* UClass::GetDefaultObj() const
	{
		if (!this)
			return nullptr;

		static int32 Offset = 0;
		if (Offset == 0)
		{
			auto ClassClass = FindClass("Class");
			auto ActorClass = FindClass("Actor");
			auto ClassObj = DefaultObjImpl(ClassClass, "Class");
			auto ActorObj = DefaultObjImpl(ActorClass, "Actor");
			for (int i = 0x28; i < 0x1a0; i += 4)
			{
				if (*(UObject**)(__int64(ClassClass) + i) == ClassObj && *(UObject**)(__int64(ActorClass) + i) == ActorObj)
				{
					Offset = i;
					break;
				}
			}
		}

		return *(UObject**)(__int64(this) + Offset);
	}

	class UEnum : public UField
	{
	public:
		int64 GetValue(const char* EnumMemberName) const
		{
			if (!this)
				return -1;

			auto Names = *(TArray<TPair<FName, int64>>*)(__int64(this) + 0x40);

			for (int i = 0; i < Names.Num(); i++)
			{
				auto& Pair = Names[i];
				auto& Name = Pair.Key();
				auto& Value = Pair.Value();

				if (Name.ComparisonIndex)
				{
					auto str = Name.ToString();
					auto colcolIdx = str.find_last_of("::");

					auto RealName = colcolIdx == -1 ? str : str.substr(colcolIdx + 1);

					if (RealName == EnumMemberName)
						return Value;
				}
			}

			return -1;
		}
	};

	inline const UStruct* FindStruct(const char* Name)
	{
		return (UStruct*)TUObjectArray::FindObject(Name, 0x10);
	}

	inline const UEnum* FindEnum(const char* Name)
	{
		return (UEnum*)TUObjectArray::FindObject(Name, 0x4);
	}

	inline const UClass* UFunction::StaticClass()
	{
		static const SDK::UClass* _storage = nullptr;

		if (!_storage)
			_storage = SDK::FindClass("Function");

		return _storage;
	}


	inline const UClass* UClass::StaticClass()
	{
		static const SDK::UClass* _storage = nullptr;

		if (!_storage)
			_storage = SDK::FindClass("Class");

		return _storage;
	}

	inline const UClass* UObject::StaticClass()
	{
		static const SDK::UClass* _storage = nullptr;

		if (!_storage)
			_storage = SDK::FindClass("Object");

		return _storage;
	}

	inline int StartingSerial = 676767676; // scuffed
	class FWeakObjectPtr
	{
	public:
		int32                                         ObjectIndex;                                       // 0x0000(0x0004)(NOT AUTO-GENERATED PROPERTY)
		int32                                         ObjectSerialNumber;                                // 0x0004(0x0004)(NOT AUTO-GENERATED PROPERTY)


		FWeakObjectPtr(int32 Index = -1, int32 SerialNumber = 0)
			: ObjectIndex(Index), ObjectSerialNumber(SerialNumber)
		{
		}

		FWeakObjectPtr(const UObject* Object)
		{
			if (Object)
			{
				ObjectIndex = Object->Index;
				auto Item = TUObjectArray::GetItemByIndex(Object->Index);

				if (Item->SerialRef() == 0)
					Item->SerialRef() = StartingSerial++;

				ObjectSerialNumber = Item->SerialRef();

			}
			else
			{
				ObjectIndex = -1;
				ObjectSerialNumber = 0;
			}
		}


	public:
		const UObject* Get() const
		{
			if (!this)
				return nullptr;

			if (ObjectIndex < 0 || ObjectSerialNumber == 0)
				return nullptr;

			auto Item = TUObjectArray::GetItemByIndex(ObjectIndex);

			if (!Item || Item->SerialRef() != ObjectSerialNumber)
				return nullptr;

			return Item->GetObject();
		}
		const UObject* operator->() const
		{
			return Get();
		}

		bool operator==(const FWeakObjectPtr& Other) const
		{
			return ObjectIndex == Other.ObjectIndex;
		}

		bool operator!=(const FWeakObjectPtr& Other) const
		{
			return ObjectIndex != Other.ObjectIndex;
		}

		bool operator==(const class UObject* Other) const
		{
			return ObjectIndex == Other->Index;
		}

		bool operator!=(const class UObject* Other) const
		{
			return ObjectIndex != Other->Index;
		}
	};

	template<typename UEType>
	class TWeakObjectPtr : public FWeakObjectPtr
	{
	public:
		TWeakObjectPtr(int32 Index = 0, int32 SerialNumber = 0)
			: FWeakObjectPtr(Index, SerialNumber)
		{
		}

		TWeakObjectPtr(UEType* Obj)
			: FWeakObjectPtr(Obj)
		{
		}

		UEType* Get() const
		{
			return (UEType*)FWeakObjectPtr::Get();
		}

		UEType* operator->() const
		{
			return (UEType*)FWeakObjectPtr::Get();
		}
	};

	template<typename TObjectID>
	class TPersistentObjectPtr
	{
	public:
		FWeakObjectPtr                                WeakPtr;
		int32                                         TagAtLastTest;
		TObjectID                                     ObjectID;

	public:
		const UObject* Get() const
		{
			return WeakPtr.Get();
		}
		const UObject* operator->() const
		{
			return WeakPtr.Get();
		}
	};

	struct FSoftObjectPath
	{
	public:
		class FName AssetPathName;
		class FString SubPathString;
	};


	__forceinline static const UObject* StaticFindObject(const wchar_t* ObjectPath, const UClass* Class)
	{
		auto StaticFindObjectInternal = (UObject * (*)(const UClass*, UObject*, const wchar_t*, bool)) SDK::Offsets::StaticFindObject;
		return StaticFindObjectInternal(Class, nullptr, ObjectPath, false);
	}

	__forceinline static const UObject* StaticLoadObject(const wchar_t* ObjectPath, const UClass* InClass, UObject* Outer = nullptr)
	{
		auto StaticLoadObjectInternal = (UObject * (*)(const UClass*, UObject*, const wchar_t*, const wchar_t*, uint32_t, UObject*, bool, void*)) SDK::Offsets::StaticLoadObject;
		return StaticLoadObjectInternal(InClass, Outer, ObjectPath, nullptr, 0, nullptr, false, nullptr);
	}

	static const UObject* FindObject(const wchar_t* ObjectPath, const UClass* Class)
	{
		auto Object = StaticFindObject(ObjectPath, Class);
		return Object ? Object : StaticLoadObject(ObjectPath, Class);
	}

	template <typename _Ot>
	static const _Ot* FindObject(const wchar_t* ObjectPath, const UClass* Class = _Ot::StaticClass())
	{
		return (const _Ot*)FindObject(ObjectPath, Class);
	}

	template <typename _Ot>
	static const _Ot* FindObject(UEAllocatedWString ObjectPath, const UClass* Class = _Ot::StaticClass())
	{
		return (const _Ot*)FindObject(ObjectPath.c_str(), Class);
	}

	template <typename _Ot>
	static const _Ot* FindObject(UEAllocatedString ObjectPath, const UClass* Class = _Ot::StaticClass())
	{
		return (const _Ot*)FindObject(std::wstring(ObjectPath.begin(), ObjectPath.end()).c_str(), Class);
	}

	template <typename _Ot>
	static const _Ot* FindObject(const char* ObjectPath, const UClass* Class = _Ot::StaticClass())
	{
		return FindObject<_Ot>(UEAllocatedString(ObjectPath), Class);
	}


	class FSoftObjectPtr : public TPersistentObjectPtr<FSoftObjectPath>
	{
	public:
		const UObject* InternalGet(const UClass* Class)
		{
			if (!this)
				return nullptr;

			auto Object = WeakPtr.Get();

			if (!Object)
			{
				const UObject* Ret = nullptr;

				if (VersionInfo.EngineVersion <= 4.16)
				{
					auto AssetLongPathname = *(FString*)(__int64(this) + offsetof(FSoftObjectPtr, ObjectID));

					if (AssetLongPathname.Num() > 0)
						WeakPtr = Ret = FindObject(AssetLongPathname.CStr(), Class);
				}
				else if (VersionInfo.FortniteVersion >= 23)
				{
					auto& PackageName = *(FName*)(__int64(this) + (VersionInfo.EngineVersion < 5.3 ? 0x10 : 0x8));
					auto& AssetName = *(FName*)(__int64(this) + (VersionInfo.EngineVersion < 5.3 ? 0x14 : 0xC));
					auto& SubPathString = *(FString*)(__int64(this) + (VersionInfo.EngineVersion < 5.3 ? 0x18 : 0x10));

					if (PackageName.ComparisonIndex > 0)
					{
						auto FullPath = PackageName.ToWString();
						if (AssetName.ComparisonIndex > 0)
							FullPath += L"." + AssetName.ToWString();
						if (SubPathString.Num() > 0)
							FullPath += L":" + SubPathString.ToWString();

						WeakPtr = Ret = FindObject(FullPath.c_str(), Class);
					}
				}
				else if (ObjectID.AssetPathName.ComparisonIndex > 0)
				{
					auto FullPath = ObjectID.AssetPathName.ToWString();
					if (ObjectID.SubPathString.Num() > 0)
						FullPath += L":" + ObjectID.SubPathString.ToWString();

					WeakPtr = Ret = FindObject(FullPath.c_str(), Class);
				}

				return Ret;
			}

			return Object;
		}

		static uint32_t Size()
		{
			return VersionInfo.EngineVersion >= 5.3 ? 0x20 : sizeof(FSoftObjectPtr);
		}
	};

	template<typename UEType>
	class TSoftObjectPtr : public FSoftObjectPtr
	{
	public:
		TSoftObjectPtr()
		{
		}

		TSoftObjectPtr(UEType* Obj)
		{
			WeakPtr = FWeakObjectPtr(Obj);
		}

		const UEType* Get()
		{
			return (UEType*)InternalGet(UEType::StaticClass());
			//return static_cast<const UEType*>(TPersistentObjectPtr::Get());
		}
		const UEType* operator->()
		{
			return Get();
		}
		operator const UEType* ()
		{
			return Get();
		}
	};

	template<typename UEType>
	class TSoftClassPtr : public FSoftObjectPtr
	{
	public:
		TSoftClassPtr()
		{
		}

		TSoftClassPtr(UClass* Obj)
		{
			WeakPtr = FWeakObjectPtr(Obj);
		}

		UClass* Get()
		{
			return (UEType*)InternalGet(UClass::StaticClass());
			//return static_cast<UClass*>(TPersistentObjectPtr::Get());
		}
		UClass* operator->()
		{
			return Get();
		}
		operator const UClass* ()
		{
			return Get();
		}
	};

	class IInterface : public UObject
	{
	};

	class FScriptInterface
	{
	public:
		const UObject* ObjectPointer = nullptr;
		const IInterface* InterfacePointer = nullptr;
	};

	template<class InterfaceType>
	class TScriptInterface : public FScriptInterface
	{
	};

	inline const IInterface* UObject::GetInterface(const UClass* Class) const
	{
		if (!Offsets::GetInterfaceAddress)
			return nullptr;

		return ((const IInterface * (*)(const UObject*, const UClass*)) Offsets::GetInterfaceAddress)(this, Class);
	}

	inline void UpdateNumElemsPerChunk()
	{
		TUObjectArrayChunked::NumElementsPerChunk = 0x10400;
	}

	// Empirically resolves the four encrypted/reordered reflection offsets on the LIVE client by
	// matching the known layouts of small stable structs (Guid: A/B/C/D @ 0/4/8/0xC, Vector: X/Y/Z
	// @ 0/8/0x10, Color: B/G/R/A @ 0/1/2/3). For each it locates UStruct::ChildProperties,
	// FField::Name, FField::Next and FProperty::Offset_Internal and detects whether the Next pointer
	// and the byte-offset are encrypted. Fully MemReadable-gated so it can never fault. On success it
	// writes the resolved values into Offsets:: so GetProperty/GetOffset work for every class.
	// Returns true if it resolved from the given struct.
	inline bool ResolveReflFromStruct(const uint8_t* strct, const int32* nameIdxs,
		const uint32* byteOffs, int nProps, const char* dbgName)
	{
		if (!strct || nProps < 2)
			return false;

		for (uint32_t CP = 0x40; CP <= 0x220; CP += 8) // ChildProperties (plain FField* head)
		{
			if (!MemReadable(strct + CP, 8))
				continue;
			uint64_t rawHead = *(const uint64_t*)(strct + CP);
			if (!rawHead)
				continue;

			// The ChildProperties head may be stored plain or ~ROR16(x^key) encrypted (FN 32.11
			// encrypts FField/property pointers but leaves UObject* like Children plain).
			for (int he = 0; he < 2; he++)
			{
			const uint8_t* head = (he == 0) ? (const uint8_t*)rawHead
				: (Offsets::EncFieldNextKey ? (const uint8_t*)DecryptObjPtr(rawHead, Offsets::EncFieldNextKey) : nullptr);
			const bool headEnc = (he == 1);
			if (!head || !MemReadable(head, 0x88))
				continue;

			for (uint32_t NM = 0x08; NM <= 0x3C; NM += 4) // FField::Name (FName, 4-byte index)
			{
				int32 hn = *(const int32*)(head + NM);
				int headProp = -1;
				for (int k = 0; k < nProps; k++)
					if (hn == nameIdxs[k]) { headProp = k; break; }
				if (headProp < 0)
					continue; // head's name isn't one of our known props — wrong CP/NM

				for (uint32_t NX = 0x08; NX <= 0x38; NX += 8) // FField::Next (enc or plain)
				{
					if (!MemReadable(head + NX, 8))
						continue;
					uint64_t raw = *(const uint64_t*)(head + NX);
					if (!raw)
						continue;

					// Try the encrypted interpretation first (matches FField_GetNext's default path),
					// then plain. The correct one lands on a sibling FField with a *different* known name.
					const uint8_t* nxt = nullptr;
					bool nextEnc = false;
					if (Offsets::EncFieldNextKey)
					{
						const uint8_t* dec = (const uint8_t*)DecryptObjPtr(raw, Offsets::EncFieldNextKey);
						if (MemReadable(dec, 0x88))
						{
							int32 n2 = *(const int32*)(dec + NM);
							for (int k = 0; k < nProps; k++)
								if (n2 == nameIdxs[k] && k != headProp) { nxt = dec; nextEnc = true; break; }
						}
					}
					if (!nxt && MemReadable((const uint8_t*)raw, 0x88))
					{
						const uint8_t* pl = (const uint8_t*)raw;
						int32 n2 = *(const int32*)(pl + NM);
						for (int k = 0; k < nProps; k++)
							if (n2 == nameIdxs[k] && k != headProp) { nxt = pl; break; }
					}
					if (!nxt)
						continue;

					// Walk the whole chain and collect (field ptr, which prop) pairs.
					const uint8_t* fields[8]; int fprop[8]; int cnt = 0;
					const uint8_t* cur = head;
					for (int step = 0; step < nProps + 2 && cur && MemReadable(cur, 0x88); step++)
					{
						int32 cn = *(const int32*)(cur + NM);
						int pk = -1;
						for (int k = 0; k < nProps; k++)
							if (cn == nameIdxs[k]) { pk = k; break; }
						if (pk < 0 || cnt >= 8)
							break;
						fields[cnt] = cur; fprop[cnt] = pk; cnt++;
						if (!MemReadable(cur + NX, 8))
							break;
						uint64_t r = *(const uint64_t*)(cur + NX);
						if (!r) { cur = nullptr; break; }
						cur = nextEnc ? (const uint8_t*)DecryptObjPtr(r, Offsets::EncFieldNextKey)
							: (const uint8_t*)r;
					}
					if (cnt < 2)
						continue;

					// Find Offset_Internal: the uint32 field whose value == the known byte-offset for
					// every collected property (checked both plain and ~(v^key) encrypted).
					for (uint32_t OI = 0x38; OI <= 0x94; OI += 4)
					{
						bool plainOk = true, encOk = (Offsets::EncPropOffsetKey != 0), anyNonZero = false;
						for (int i = 0; i < cnt; i++)
						{
							if (!MemReadable(fields[i] + OI, 4)) { plainOk = encOk = false; break; }
							uint32 rv = *(const uint32*)(fields[i] + OI);
							uint32 want = byteOffs[fprop[i]];
							if (want) anyNonZero = true;
							if (rv != want) plainOk = false;
							if ((~(rv ^ Offsets::EncPropOffsetKey)) != want) encOk = false;
						}
						if (!anyNonZero || (!plainOk && !encOk))
							continue;

						Offsets::ChildProperties = CP;
						Offsets::FField_Name = NM;
						Offsets::FField_Next = NX;
						Offsets::Offset_Internal = OI;
						Offsets::bEncChildProperties = headEnc;

						// Resolve ElementSize (FProperty): the uint32 that equals each property's
						// element size (== the spacing between consecutive members). Then PropertyFlags
						// is the uint64 immediately after it (standard FProperty sub-layout).
						{
							uint32 expES = (nProps >= 2) ? (byteOffs[1] - byteOffs[0]) : 4;
							if (expES == 0) expES = 4;
							auto checkES = [&](uint32_t ES) -> bool {
								for (int i = 0; i < cnt; i++)
									if (!MemReadable(fields[i] + ES, 4) || *(const uint32*)(fields[i] + ES) != expES)
										return false;
								return true;
							};
							// Prefer the standard-relative slot (Offset_Internal-0x10); else scan.
							if (OI >= 0x40 && checkES(OI - 0x10)) { Offsets::ElementSize = OI - 0x10; Offsets::PropertyFlags = OI - 0xc; }
							else for (uint32_t ES = 0x30; ES < OI; ES += 4)
								if (checkES(ES)) { Offsets::ElementSize = ES; Offsets::PropertyFlags = ES + 4; break; }
						}
						// NOTE: never clear EncFieldNextKey here — the (working) function chain
						// GetNext() at 0x28 relies on it; the property FField::Next uses the same key.
						if (plainOk && !encOk)
							Offsets::EncPropOffsetKey = 0; // offset is stored plain
						DbgLog("[reflres] via %s (props=%d): ChildProperties=0x%X(%s) FField::Name=0x%X "
							"FField::Next=0x%X(%s) Offset_Internal=0x%X(%s)\n",
							dbgName, cnt, CP, headEnc ? "enc" : "plain", NM, NX, nextEnc ? "enc" : "plain",
							OI, (plainOk && !encOk) ? "plain" : "enc");
						return true;
					}
				}
			}
			}
		}
		return false;
	}

	// Drives ResolveReflFromStruct across Guid/Vector/Color and reports the outcome.
	inline void FindReflectionOffsets()
	{
		auto guidObj = TUObjectArray::FindObject("Guid");
		auto vecObj = TUObjectArray::FindObject("Vector");
		auto colorObj = TUObjectArray::FindObject("Color");
		DbgLog("--- reflection offset resolver --- Guid=%p Vector=%p Color=%p\n",
			(void*)guidObj, (void*)vecObj, (void*)colorObj);

		const int32 gIdx[4] = { FName(L"A").ComparisonIndex, FName(L"B").ComparisonIndex,
			FName(L"C").ComparisonIndex, FName(L"D").ComparisonIndex };
		const uint32 gOff[4] = { 0x0, 0x4, 0x8, 0xC };
		const int32 vIdx[3] = { FName(L"X").ComparisonIndex, FName(L"Y").ComparisonIndex,
			FName(L"Z").ComparisonIndex };
		const uint32 vOff[3] = { 0x0, 0x8, 0x10 };
		const int32 cIdx[4] = { FName(L"B").ComparisonIndex, FName(L"G").ComparisonIndex,
			FName(L"R").ComparisonIndex, FName(L"A").ComparisonIndex };
		const uint32 cOff[4] = { 0x0, 0x1, 0x2, 0x3 };
		DbgLog("  name idxs A=%08X B=%08X C=%08X D=%08X X=%08X Y=%08X Z=%08X\n",
			gIdx[0], gIdx[1], gIdx[2], gIdx[3], vIdx[0], vIdx[1], vIdx[2]);

		bool ok = false;
		if (guidObj) ok = ResolveReflFromStruct((const uint8_t*)guidObj, gIdx, gOff, 4, "Guid");
		if (!ok && vecObj) ok = ResolveReflFromStruct((const uint8_t*)vecObj, vIdx, vOff, 3, "Vector");
		if (!ok && colorObj) ok = ResolveReflFromStruct((const uint8_t*)colorObj, cIdx, cOff, 4, "Color");

		// Resolve UStruct::PropertiesSize (the ProcessEvent params Malloc size). Guid=0x10,
		// Vector=0x18, Color=0x4 give a unique triple match.
		if (ok && guidObj && vecObj && colorObj)
		{
			const uint8_t* g = (const uint8_t*)guidObj; const uint8_t* v = (const uint8_t*)vecObj; const uint8_t* c = (const uint8_t*)colorObj;
			for (uint32_t PS = 0x40; PS <= 0x90; PS += 4)
			{
				if (!MemReadable(g + PS, 4) || !MemReadable(v + PS, 4) || !MemReadable(c + PS, 4)) continue;
				if (*(const uint32*)(g + PS) == 0x10 && *(const uint32*)(v + PS) == 0x18 && *(const uint32*)(c + PS) == 0x4)
				{
					Offsets::PropertiesSize = PS;
					break;
				}
			}
		}

		if (ok)
		{
			DbgLog("[reflres] RESOLVED. ChildProperties=0x%X FField::Name=0x%X "
				"FField::Next=0x%X Offset_Internal=0x%X ElementSize=0x%X PropertyFlags=0x%X PropertiesSize=0x%X\n",
				Offsets::ChildProperties, Offsets::FField_Name, Offsets::FField_Next,
				Offsets::Offset_Internal, Offsets::ElementSize, Offsets::PropertyFlags, Offsets::PropertiesSize);

			// Ground-truth dump: the resolved first-property FField (raw + prop-offset-decrypted) and
			// the Guid struct region, so ElementSize/PropertyFlags/PropertiesSize can be read exactly.
			if (guidObj)
			{
				const uint8_t* gs = (const uint8_t*)guidObj;
				uint64_t rawH = MemReadable(gs + Offsets::ChildProperties, 8) ? *(const uint64_t*)(gs + Offsets::ChildProperties) : 0;
				const uint8_t* h = Offsets::bEncChildProperties
					? (const uint8_t*)DecryptObjPtr(rawH, Offsets::EncFieldNextKey) : (const uint8_t*)rawH;
				DbgLog("[dump] Guid prop0 FField=%p (nameIdx@0x20=%08X):\n", (void*)h,
					MemReadable(h, 0x24) ? *(const int32*)(h + 0x20) : 0);
				for (uint32_t o = 0x00; h && o < 0x70; o += 4)
					if (MemReadable(h + o, 4))
						DbgLog("   f+0x%X raw=%08X decOff=%08X\n", o, *(const uint32*)(h + o),
							~(*(const uint32*)(h + o) ^ Offsets::EncPropOffsetKey));
				DbgLog("[dump] Guid struct @0x40..0x90 (looking for PropertiesSize=0x10):\n");
				for (uint32_t o = 0x40; o <= 0x90; o += 4)
					if (MemReadable(gs + o, 4))
						DbgLog("   g+0x%X=%08X\n", o, *(const uint32*)(gs + o));
			}
		}
		else
		{
			DbgLog("[reflres] FAILED to resolve from any known struct — offsets left at defaults\n");
			// Failure diagnostic: for Guid, dump each candidate head (decrypted with EncFieldNextKey)
			// and flag any int32 that matches a known Guid/Vector name index, so the true
			// ChildProperties/FField::Name can be read straight from the log next round.
			if (guidObj && Offsets::EncFieldNextKey)
			{
				auto isName = [&](int32 v) -> const char* {
					if (v == gIdx[0]) return "A"; if (v == gIdx[1]) return "B";
					if (v == gIdx[2]) return "C"; if (v == gIdx[3]) return "D";
					if (v == vIdx[0]) return "X"; if (v == vIdx[1]) return "Y";
					if (v == vIdx[2]) return "Z"; return nullptr; };
				const uint8_t* g = (const uint8_t*)guidObj;
				for (uint32_t CP = 0x40; CP <= 0x220; CP += 8)
				{
					if (!MemReadable(g + CP, 8)) continue;
					uint64_t rawHead = *(const uint64_t*)(g + CP);
					if (!rawHead) continue;
					const uint8_t* dec = (const uint8_t*)DecryptObjPtr(rawHead, Offsets::EncFieldNextKey);
					const uint8_t* pl = (const uint8_t*)rawHead;
					for (int e = 0; e < 2; e++)
					{
						const uint8_t* h = e ? dec : pl;
						if (!MemReadable(h, 0x40)) continue;
						char mark[96] = {}; int ml = 0;
						for (uint32_t j = 0x08; j <= 0x2C; j += 4)
						{
							const char* n = isName(*(const int32*)(h + j));
							if (n && ml < 80) ml += sprintf_s(mark + ml, sizeof(mark) - ml, " @0x%X='%s'", j, n);
						}
						DbgLog("  cand guid+0x%X %s->%p : 08=%08X 0C=%08X 10=%08X 14=%08X 18=%08X 1C=%08X 20=%08X 24=%08X 28=%08X%s\n",
							CP, e ? "dec" : "pl", (void*)h,
							*(const int32*)(h+0x08),*(const int32*)(h+0x0C),*(const int32*)(h+0x10),*(const int32*)(h+0x14),
							*(const int32*)(h+0x18),*(const int32*)(h+0x1C),*(const int32*)(h+0x20),*(const int32*)(h+0x24),
							*(const int32*)(h+0x28), mark);
					}
				}
			}
		}
	}

	// Diagnostic: dump the raw object-array state so a wrong key/offset is visible in the log
	// instead of just crashing. Safe to call before anything else touches the array.
	inline void DumpObjArrayDiag()
	{
		int num = TUObjectArray::Num();
		uint64_t rawObj = Offsets::GObjectsChunked ? *(unsigned long long*)Offsets::GObjectsChunked : 0ull;
		uint64_t chunks = Offsets::bEncryptedObjects ? DecryptObjPtr(rawObj, Offsets::EncObjArrayKey) : rawObj;
		DbgLog("--- ObjArray diag --- Num=%d (0x%X) GObjects=%p rawObjectsField=%016llX chunksPtr=%016llX chunk0=%016llX\n",
			num, num, (void*)Offsets::GObjectsChunked, rawObj, chunks,
			(chunks && !IsBadReadPtr((void*)chunks, 8)) ? *(unsigned long long*)chunks : 0ull);
		if (num <= 0 || num > 50000000)
		{
			DbgLog("Num is insane — GObjects/NumKey wrong; aborting diag\n");
			return;
		}
		for (int i = 0; i < 6; i++)
		{
			auto item = TUObjectArray::GetItemByIndex(i);
			const UObject* obj = TUObjectArray::GetObjectByIndex(i);
			DbgLog("[%d] item=%p obj=%p", i, (void*)item, (void*)obj);
			if (obj && !IsBadReadPtr((void*)obj, 0x30))
				DbgLog(" flags=%08X class=%p name=%s", (int32)obj->ObjectFlags,
					(void*)obj->Class, obj->Name.ToString().c_str());
			DbgLog("\n");
		}
		auto fnClass = FName(L"Class");
		auto fnActor = FName(L"Actor");
		DbgLog("FName(Class).idx=0x%08X FName(Actor).idx=0x%08X\n", fnClass.ComparisonIndex, fnActor.ComparisonIndex);
		auto clsObj = TUObjectArray::FindObject("Class");
		auto actObj = TUObjectArray::FindObject("Actor");
		DbgLog("FindObject(Class)=%p FindObject(Actor)=%p\n", (void*)clsObj, (void*)actObj);

		// Find the ClassCastFlags offset: Class metaclass holds 0x29, Actor holds 0x1000000000.
		// Also dump u64s that differ, to spot the field even if the exact constants changed.
		if (clsObj && actObj)
		{
			for (uint32_t i = 0x40; i < 0x300; i += 8)
			{
				if (!MemReadable((const char*)clsObj + i, 8) || !MemReadable((const char*)actObj + i, 8))
					continue;
				uint64_t cv = *(const uint64_t*)((const char*)clsObj + i);
				uint64_t av = *(const uint64_t*)((const char*)actObj + i);
				if (cv == 0x29 || av == 0x1000000000 || (av & 0x1000000000))
					DbgLog("  CastFlags? @0x%X  Class=%016llX Actor=%016llX\n", i, cv, av);
			}
		}

		// --- Reflection probe (bounded so it can never hang) ---
		auto actorCls = (const UStruct*)clsObj; // 'Actor' UClass resolved above
		actorCls = (const UStruct*)actObj;
		if (actorCls && !IsBadReadPtr((void*)actorCls, 0x100))
		{
			DbgLog("--- reflection probe: Actor UClass=%p Super(0x40)=%p ---\n",
				(void*)actorCls, *(void**)((const char*)actorCls + 0x40));

			// Function chain: Children(0x78) walked via UField::Next(0x28, encrypted)
			DbgLog("Children@0x78=%p\n", *(void**)((const char*)actorCls + 0x78));
			const UField* f = actorCls->GetChildren();
			for (int n = 0; n < 6 && f && !IsBadReadPtr((void*)f, 0x30); n++)
			{
				DbgLog("  fn[%d]=%p name=%s\n", n, (void*)f, f->GetName().ToString().c_str());
				f = f->GetNext();
			}

		}
		FindReflectionOffsets();

		// FStructBaseChain (used by IsA) sanity — check the pointer at Class+0x30 without calling IsA.
		if (actObj && MemReadable((const char*)actObj + 0x30, 0x10))
		{
			auto chainArr = *(const void* const*)((const char*)actObj + 0x30);
			int32 num = *(const int32*)((const char*)actObj + 0x38);
			DbgLog("StructBaseChain@0x30: arr=%p (readable=%d) numMinusOne=%d\n",
				(void*)chainArr, (int)MemReadable(chainArr, 8), num);
		}
		// Engine/world lookup that GetWorld depends on (IsA base-chain confirmed valid above).
		auto fortEngine = TUObjectArray::FindFirstObject("FortEngine");
		DbgLog("FindFirstObject(FortEngine)=%p\n", (void*)fortEngine);

		// Self-test: replicate GetWorld()'s GameViewport->World chain purely via GetOffset so the log
		// confirms the reflection fix end-to-end before the real start path runs (line 80 of dllmain).
		if (fortEngine)
		{
			uint32 gvOff = ((const UObject*)fortEngine)->GetOffset("GameViewport");
			DbgLog("[selftest] GetOffset(GameViewport)=0x%X\n", gvOff);
			if (gvOff != (uint32)-1 && gvOff < 0x2000 && MemReadable((const char*)fortEngine + gvOff, 8))
			{
				auto gv = *(const UObject* const*)((const char*)fortEngine + gvOff);
				DbgLog("[selftest] GameViewport=%p\n", (void*)gv);
				if (gv && MemReadable(gv, 0x10))
				{
					uint32 wOff = gv->GetOffset("World");
					DbgLog("[selftest] GetOffset(World)=0x%X\n", wOff);
					if (wOff != (uint32)-1 && wOff < 0x2000 && MemReadable((const char*)gv + wOff, 8))
						DbgLog("[selftest] World=%p (GetWorld should work)\n",
							*(void* const*)((const char*)gv + wOff));
				}
			}
		}
		DbgLog("--- diag end ---\n");
	}

	inline void InitializeProcessEventVft(uintptr_t PEAddr)
	{
		auto DefaultObj = UObject::StaticClass()->GetDefaultObj();

		if (DefaultObj)
			for (int i = 0; i < 0x100; i++)
				if (__int64(DefaultObj->Vft[i]) == PEAddr)
				{
					Offsets::ProcessEventVft = i;
					break;
				}
	}
}

#undef  PI
#define PI 					(3.1415926535897932f)
#define SMALL_NUMBER		(1.e-8f)
#define KINDA_SMALL_NUMBER	(1.e-4f)
#define BIG_NUMBER			(3.4e+38f)
#define EULERS_NUMBER       (2.71828182845904523536f)

// Copied from float.h
#define MAX_FLT 3.402823466e+38F

static int32 GSRandSeed;

struct FPlatformMath
{
	static FORCEINLINE uint32 CountLeadingZeros(uint32 Value)
	{
		// Use BSR to return the log2 of the integer
		unsigned long Log2;
		if (_BitScanReverse(&Log2, Value) != 0)
		{
			return 31 - Log2;
		}

		return 32;
	}
	static FORCEINLINE uint32 CountTrailingZeros(uint32 Value)
	{
		if (Value == 0)
		{
			return 32;
		}
		unsigned long BitIndex;	// 0-based, where the LSB is 0 and MSB is 31
		_BitScanForward(&BitIndex, Value);	// Scans from LSB to MSB
		return BitIndex;
	}
	static FORCEINLINE uint32 CeilLogTwo(uint32 Arg)
	{
		int32 Bitmask = ((int32)(CountLeadingZeros(Arg) << 26)) >> 31;
		return (32 - CountLeadingZeros(Arg - 1)) & (~Bitmask);
	}
	static FORCEINLINE uint32 RoundUpToPowerOfTwo(uint32 Arg)
	{
		return 1 << CeilLogTwo(Arg);
	}

	template< class T >
	static FORCEINLINE T Square(const T A)
	{
		return A * A;
	}

	template< class T >
	static FORCEINLINE T Clamp(const T X, const T Min, const T Max)
	{
		return X < Min ? Min : X < Max ? X : Max;
	}

	template< class T, class U >
	static FORCEINLINE T Lerp(const T& A, const T& B, const U& Alpha)
	{
		return (T)(A + Alpha * (B - A));
	}

	/** Divides two integers and rounds up */
	template <class T>
	static FORCEINLINE T DivideAndRoundUp(T Dividend, T Divisor)
	{
		return (Dividend + Divisor - 1) / Divisor;
	}

	/** Divides two integers and rounds down */
	template <class T>
	static FORCEINLINE T DivideAndRoundDown(T Dividend, T Divisor)
	{
		return Dividend / Divisor;
	}

	/** Divides two integers and rounds to nearest */
	template <class T>
	static FORCEINLINE T DivideAndRoundNearest(T Dividend, T Divisor)
	{
		return (Dividend >= 0)
			? (Dividend + Divisor / 2) / Divisor
			: (Dividend - Divisor / 2 + 1) / Divisor;
	}


	template <typename T>
	static FORCEINLINE bool IsPowerOfTwo(T Value)
	{
		return ((Value & (Value - 1)) == (T)0);
	}


	// Math Operations

	/** Returns highest of 3 values */
	template< class T >
	static FORCEINLINE T Max3(const T A, const T B, const T C)
	{
		return Max(Max(A, B), C);
	}

	/** Returns lowest of 3 values */
	template< class T >
	static FORCEINLINE T Min3(const T A, const T B, const T C)
	{
		return Min(Min(A, B), C);
	}

	// Returns e^Value
	static FORCEINLINE float Exp(float Value) { return expf(Value); }
	// Returns 2^Value
	static FORCEINLINE float Exp2(float Value) { return powf(2.f, Value); /*exp2f(Value);*/ }
	static FORCEINLINE float Loge(float Value) { return logf(Value); }
	static FORCEINLINE float LogX(float Base, float Value) { return Loge(Value) / Loge(Base); }
	// 1.0 / Loge(2) = 1.4426950f
	static FORCEINLINE float Log2(float Value) { return Loge(Value) * 1.4426950f; }

	static FORCEINLINE float Sin(float Value) { return sinf(Value); }
	static FORCEINLINE float Asin(float Value) { return asinf((Value < -1.f) ? -1.f : ((Value < 1.f) ? Value : 1.f)); }
	static FORCEINLINE float Sinh(float Value) { return sinhf(Value); }
	static FORCEINLINE float Cos(float Value) { return cosf(Value); }
	static FORCEINLINE float Acos(float Value) { return acosf((Value < -1.f) ? -1.f : ((Value < 1.f) ? Value : 1.f)); }
	static FORCEINLINE float Tan(float Value) { return tanf(Value); }
	static FORCEINLINE float Atan(float Value) { return atanf(Value); }

	// Note:  We use FASTASIN_HALF_PI instead of HALF_PI inside of FastASin(), since it was the value that accompanied the minimax coefficients below.
	// It is important to use exactly the same value in all places inside this function to ensure that FastASin(0.0f) == 0.0f.
	// For comparison:
	//		HALF_PI				== 1.57079632679f == 0x3fC90FDB
	//		FASTASIN_HALF_PI	== 1.5707963050f  == 0x3fC90FDA

	static FORCEINLINE float Sqrt(float Value) { return sqrtf(Value); }
	static FORCEINLINE float Pow(float A, float B) { return powf(A, B); }

	/** Computes a fully accurate inverse square root */
	static FORCEINLINE float InvSqrt(float F)
	{
		return 1.0f / sqrtf(F);
	}

	/** Computes a faster but less accurate inverse square root */
	static FORCEINLINE float InvSqrtEst(float F)
	{
		return InvSqrt(F);
	}

	/** Return true if value is NaN (not a number). */
	static FORCEINLINE bool IsNaN(float A)
	{
		return ((*(uint32*)&A) & 0x7FFFFFFF) > 0x7F800000;
	}
	/** Return true if value is finite (not NaN and not Infinity). */
	static FORCEINLINE bool IsFinite(float A)
	{
		return ((*(uint32*)&A) & 0x7F800000) != 0x7F800000;
	}
	static FORCEINLINE bool IsNegativeFloat(const float& A)
	{
		return ((*(uint32*)&A) >= (uint32)0x80000000); // Detects sign bit.
	}

	static FORCEINLINE bool IsNegativeDouble(const double& A)
	{
		return ((*(uint64*)&A) >= (uint64)0x8000000000000000); // Detects sign bit.
	}

	/**
	 * Computes the base 2 logarithm for a 64-bit value that is greater than 0.
	 * The result is rounded down to the nearest integer.
	 *
	 * @param Value		The value to compute the log of
	 * @return			Log2 of Value. 0 if Value is 0.
	 */
	static FORCEINLINE uint64 FloorLog2_64(uint64 Value)
	{
		uint64 pos = 0;
		if (Value >= 1ull << 32) { Value >>= 32; pos += 32; }
		if (Value >= 1ull << 16) { Value >>= 16; pos += 16; }
		if (Value >= 1ull << 8) { Value >>= 8; pos += 8; }
		if (Value >= 1ull << 4) { Value >>= 4; pos += 4; }
		if (Value >= 1ull << 2) { Value >>= 2; pos += 2; }
		if (Value >= 1ull << 1) { pos += 1; }
		return (Value == 0) ? 0 : pos;
	}

	// Conversion Functions

	/**
	 * Converts radians to degrees.
	 * @param	RadVal			Value in radians.
	 * @return					Value in degrees.
	 */
	template<class T>
	static FORCEINLINE auto RadiansToDegrees(T const& RadVal) -> decltype(RadVal* (180.f / PI))
	{
		return RadVal * (180.f / PI);
	}

	/**
	 * Converts degrees to radians.
	 * @param	DegVal			Value in degrees.
	 * @return					Value in radians.
	 */
	template<class T>
	static FORCEINLINE auto DegreesToRadians(T const& DegVal) -> decltype(DegVal* (PI / 180.f))
	{
		return DegVal * (PI / 180.f);
	}

	static FORCEINLINE int32 RoundToInt(float F)
	{
		// Note: the x2 is to workaround the rounding-to-nearest-even-number issue when the fraction is .5
		return _mm_cvt_ss2si(_mm_set_ss(F + F + 0.5f)) >> 1;
	}

	static FORCEINLINE float RoundToFloat(float F)
	{
		return (float)RoundToInt(F);
	}

	static FORCEINLINE int32 FloorToInt(float F)
	{
		return _mm_cvt_ss2si(_mm_set_ss(F + F - 0.5f)) >> 1;
	}

	static FORCEINLINE float FloorToFloat(float F)
	{
		return (float)FloorToInt(F);
	}

	static FORCEINLINE float GridSnap(float Location, float Grid)
	{
		if (Grid == 0.f)	return Location;
		else
		{
			return FloorToFloat((Location + 0.5f * Grid) / Grid) * Grid;
		}
	}

	/** Returns a random integer between 0 and RAND_MAX, inclusive */
	static FORCEINLINE int32 Rand() { return rand(); }

	/** Seeds global random number functions Rand() and FRand() */
	static FORCEINLINE void RandInit(int32 Seed) { srand(Seed); }

	/** Returns a random float between 0 and 1, inclusive. */
	static FORCEINLINE float FRand() { return Rand() / (float)RAND_MAX; }

	static void SRandInit(int32 Seed)
	{
		GSRandSeed = Seed;
	}

	static int32 GetRandSeed()
	{
		return GSRandSeed;
	}
};

typedef FPlatformMath FMath;
