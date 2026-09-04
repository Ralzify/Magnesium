#include "pch.h"

#include "../Public/AssetHotfixBaselineSampler.h"

#include <bit>
#include <charconv>
#include <cmath>
#include <limits>

namespace AssetHotfixBaselineSampler
{
namespace
{
	constexpr std::size_t kMaximumAssetPathBytes = 2048;
	constexpr std::size_t kMaximumRowNameBytes = 512;
	constexpr std::size_t kMaximumColumnNameBytes = 256;
	constexpr std::size_t kMaximumWideTokenCharacters = 2048;
	constexpr std::size_t kMaximumCellCharacters = 4096;
	constexpr std::size_t kMaximumSnapshotCharacters = 1024 * 1024;
	constexpr int32 kMaximumRows = 4096;
	constexpr std::size_t kMaximumParameters = 16;
	constexpr std::size_t kMaximumFieldWalk = 32;
	constexpr uint32 kMaximumParameterBytes = 0x100;
	constexpr int32 kDestroyedObjectFlags = 0x01800000;

	constexpr uint64 kCpfParm = 0x80;
	constexpr uint64 kCpfOutParm = 0x100;
	constexpr uint64 kCpfReturnParm = 0x400;

	constexpr wchar_t kDataTableClassPath[] =
		L"/Script/Engine.DataTable";
	constexpr wchar_t kCurveTableClassPath[] =
		L"/Script/Engine.CurveTable";
	constexpr wchar_t kFunctionClassPath[] =
		L"/Script/CoreUObject.Function";
	constexpr wchar_t kLibraryClassPath[] =
		L"/Script/Engine.DataTableFunctionLibrary";
	constexpr wchar_t kLibraryDefaultObjectPath[] =
		L"/Script/Engine.Default__DataTableFunctionLibrary";
	constexpr wchar_t kGetRowNamesPath[] =
		L"/Script/Engine.DataTableFunctionLibrary.GetDataTableRowNames";
	constexpr wchar_t kGetColumnPath[] =
		L"/Script/Engine.DataTableFunctionLibrary.GetDataTableColumnAsString";
	constexpr wchar_t kEvaluateCurvePath[] =
		L"/Script/Engine.DataTableFunctionLibrary.EvaluateCurveTableRow";

	struct FRawArray
	{
		void* Data = nullptr;
		int32 Num = 0;
		int32 Max = 0;
	};
	static_assert(sizeof(FRawArray) == sizeof(TArray<FName>));
	static_assert(sizeof(FRawArray) == sizeof(FString));

	struct FExpectedParameter
	{
		const wchar_t* Name = nullptr;
		uint32 ElementSize = 0;
		uint32 Alignment = 1;
		uint64 RequiredFlags = 0;
		uint64 ForbiddenFlags = 0;
	};

	struct FFunctionSchema
	{
		UFunction* Function = nullptr;
		uint32 ParamsSize = 0;
		std::array<uint32, kMaximumParameters> Offsets{};
	};

	struct FRuntimeObjects
	{
		UObject* LibraryDefaultObject = nullptr;
		UClass* FunctionClass = nullptr;
		UClass* LibraryClass = nullptr;
		UClass* AssetClass = nullptr;
		UObject* Asset = nullptr;
	};

	struct FDataSnapshot
	{
		FRawArray RowNames{};
		FRawArray Values{};
		bool RowNamesOwned = false;
		bool ValuesOwned = false;
	};

	class FFixedWriter final
	{
	public:
		explicit FFixedWriter(FSampleResult& Result) noexcept
			: Out(Result)
		{
		}

		bool Append(std::string_view Value) noexcept
		{
			if (Value.size() > Capacity() - Length)
				return false;
			memcpy(Out.RestorationDirective.data() + Length,
				Value.data(), Value.size());
			Length += Value.size();
			return true;
		}

		bool AppendWide(const wchar_t* Value, std::size_t LengthWide) noexcept
		{
			if (!Value || LengthWide > kMaximumCellCharacters ||
				LengthWide > static_cast<std::size_t>(
					(std::numeric_limits<int>::max)()))
			{
				return false;
			}
			const int Required = WideCharToMultiByte(
				CP_UTF8, WC_ERR_INVALID_CHARS, Value,
				static_cast<int>(LengthWide), nullptr, 0, nullptr, nullptr);
			if (Required <= 0 ||
				static_cast<std::size_t>(Required) > Capacity() - Length)
			{
				return false;
			}
			if (Required > 0 && WideCharToMultiByte(
					CP_UTF8, WC_ERR_INVALID_CHARS, Value,
					static_cast<int>(LengthWide),
					Out.RestorationDirective.data() + Length,
					Required, nullptr, nullptr) != Required)
			{
				return false;
			}
			Length += static_cast<std::size_t>(Required);
			return true;
		}

		bool Finish() noexcept
		{
			if (Length >= Out.RestorationDirective.size())
				return false;
			Out.RestorationDirective[Length] = '\0';
			Out.RestorationDirectiveLength = Length;
			return true;
		}

	private:
		std::size_t Capacity() const noexcept
		{
			return Out.RestorationDirective.size() - 1;
		}

		FSampleResult& Out;
		std::size_t Length = 0;
	};

	bool IsReadableProtection(DWORD Protection) noexcept
	{
		const DWORD Access = Protection & 0xff;
		return Access == PAGE_READONLY ||
			Access == PAGE_READWRITE ||
			Access == PAGE_WRITECOPY ||
			Access == PAGE_EXECUTE_READ ||
			Access == PAGE_EXECUTE_READWRITE ||
			Access == PAGE_EXECUTE_WRITECOPY;
	}

	bool IsExecutableProtection(DWORD Protection) noexcept
	{
		const DWORD Access = Protection & 0xff;
		return Access == PAGE_EXECUTE ||
			Access == PAGE_EXECUTE_READ ||
			Access == PAGE_EXECUTE_READWRITE ||
			Access == PAGE_EXECUTE_WRITECOPY;
	}

	bool IsMemoryRange(
		const void* Address,
		std::size_t Size,
		bool RequireExecutable) noexcept
	{
		if (!Address || Size == 0)
			return false;
		const uintptr_t Begin = reinterpret_cast<uintptr_t>(Address);
		if (Begin > (std::numeric_limits<uintptr_t>::max)() - Size)
			return false;
		const uintptr_t End = Begin + Size;
		uintptr_t Cursor = Begin;
		while (Cursor < End)
		{
			MEMORY_BASIC_INFORMATION Region{};
			if (VirtualQuery(reinterpret_cast<const void*>(Cursor),
					&Region, sizeof(Region)) != sizeof(Region) ||
				Region.State != MEM_COMMIT ||
				(Region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0 ||
				(RequireExecutable
					? !IsExecutableProtection(Region.Protect)
					: !IsReadableProtection(Region.Protect)))
			{
				return false;
			}
			const uintptr_t RegionBegin =
				reinterpret_cast<uintptr_t>(Region.BaseAddress);
			if (RegionBegin >
				(std::numeric_limits<uintptr_t>::max)() - Region.RegionSize)
			{
				return false;
			}
			const uintptr_t RegionEnd = RegionBegin + Region.RegionSize;
			if (RegionEnd <= Cursor)
				return false;
			Cursor = (std::min)(RegionEnd, End);
		}
		return true;
	}

	bool IsReadableRange(const void* Address, std::size_t Size) noexcept
	{
		return IsMemoryRange(Address, Size, false);
	}

	template <typename T>
	bool TryRead(const void* Address, T& Out) noexcept
	{
		Out = {};
		if (!IsReadableRange(Address, sizeof(T)))
			return false;
		memcpy(&Out, Address, sizeof(T));
		return true;
	}

	const uint8* AddOffset(const void* Base, uint32 Offset) noexcept
	{
		const uintptr_t Value = reinterpret_cast<uintptr_t>(Base);
		if (!Base || Value >
			(std::numeric_limits<uintptr_t>::max)() - Offset)
		{
			return nullptr;
		}
		return reinterpret_cast<const uint8*>(Value + Offset);
	}

	bool IsLiveObject(const UObject* Object) noexcept
	{
		if (!Object)
			return false;
		__try
		{
			const int32 Index = Object->Index;
			if (Index < 0 || Index >= TUObjectArray::Num())
				return false;
			const FUObjectItem* Item =
				TUObjectArray::GetItemByIndex(Index);
			return Item && Item->Object == Object && Object->Class &&
				(Item->Flags & 0x20) == 0 &&
				(Object->ObjectFlags & kDestroyedObjectFlags) == 0;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	const UObject* GuardedStaticFindObject(
		const wchar_t* Path,
		const UClass* Class) noexcept
	{
		__try
		{
			return StaticFindObject(Path, Class);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return nullptr;
		}
	}

	bool GuardedIsA(
		const UObject* Object,
		const UClass* Class) noexcept
	{
		__try
		{
			return Object && Class && Object->IsA(Class);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool GuardedConstructName(
		FName* Out,
		const wchar_t* Text) noexcept
	{
		__try
		{
			auto Constructor = reinterpret_cast<
				void(*)(FName*, const wchar_t*, int)>(
				Offsets::FNameConstructor);
			Constructor(Out, Text, 1);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool GuardedProcessEvent(
		const UObject* Object,
		UFunction* Function,
		void* Params) noexcept
	{
		__try
		{
			Object->ProcessEvent(Function, Params);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool GuardedFree(void* Allocation) noexcept
	{
		__try
		{
			FMemory::Free(Allocation);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	bool NamesEqual(const FName& Left, const FName& Right) noexcept
	{
		return Left == Right;
	}

	bool IsSafeToken(
		std::string_view Value,
		std::size_t Maximum) noexcept
	{
		if (Value.empty() || Value.size() > Maximum)
			return false;
		for (const unsigned char Character : Value)
		{
			if (Character == 0 || Character == ';' ||
				Character == '\r' || Character == '\n')
			{
				return false;
			}
		}
		return true;
	}

	bool ConvertUtf8Token(
		std::string_view Input,
		wchar_t* Out,
		std::size_t Capacity) noexcept
	{
		if (!Out || Capacity < 2 || Input.empty() ||
			Input.size() > static_cast<std::size_t>(
				(std::numeric_limits<int>::max)()))
		{
			return false;
		}
		const int Converted = MultiByteToWideChar(
			CP_UTF8, MB_ERR_INVALID_CHARS, Input.data(),
			static_cast<int>(Input.size()), Out,
			static_cast<int>(Capacity - 1));
		if (Converted <= 0 ||
			static_cast<std::size_t>(Converted) >= Capacity)
		{
			return false;
		}
		Out[Converted] = L'\0';
		return true;
	}

	bool ValidateEntryPoints() noexcept
	{
		if (!Offsets::StaticFindObject || !Offsets::FNameConstructor ||
			!Offsets::Realloc || Offsets::ProcessEventVft == 0 ||
			Offsets::ProcessEventVft > 512)
		{
			return false;
		}
		return IsMemoryRange(
			reinterpret_cast<const void*>(Offsets::StaticFindObject), 1, true) &&
			IsMemoryRange(
				reinterpret_cast<const void*>(Offsets::FNameConstructor), 1, true) &&
			IsMemoryRange(
				reinterpret_cast<const void*>(Offsets::Realloc), 1, true);
	}

	bool ValidateProcessEventTarget(const UObject* Object) noexcept
	{
		void** Vft = nullptr;
		if (!IsLiveObject(Object) || !TryRead(Object, Vft) || !Vft ||
			!IsReadableRange(Vft,
				(Offsets::ProcessEventVft + 1) * sizeof(void*)))
		{
			return false;
		}
		void* Target = nullptr;
		if (!TryRead(Vft + Offsets::ProcessEventVft, Target))
			return false;
		return IsMemoryRange(Target, 1, true);
	}

	bool ResolveRuntimeObjects(
		EAssetKind Kind,
		std::string_view AssetPath,
		FRuntimeObjects& Out,
		ESampleStatus& Status) noexcept
	{
		Out = {};
		Status = ESampleStatus::ReflectionUnavailable;
		if (!ValidateEntryPoints())
		{
			Status = ESampleStatus::LookupUnavailable;
			return false;
		}

		Out.FunctionClass = const_cast<UClass*>(reinterpret_cast<
			const UClass*>(GuardedStaticFindObject(kFunctionClassPath, nullptr)));
		Out.LibraryClass = const_cast<UClass*>(reinterpret_cast<
			const UClass*>(GuardedStaticFindObject(kLibraryClassPath, nullptr)));
		Out.AssetClass = const_cast<UClass*>(reinterpret_cast<const UClass*>(
			GuardedStaticFindObject(
				Kind == EAssetKind::DataTable
					? kDataTableClassPath : kCurveTableClassPath,
				nullptr)));
		Out.LibraryDefaultObject = const_cast<UObject*>(
			GuardedStaticFindObject(kLibraryDefaultObjectPath, Out.LibraryClass));
		if (!IsLiveObject(Out.FunctionClass) ||
			!IsLiveObject(Out.LibraryClass) ||
			!IsLiveObject(Out.AssetClass) ||
			!IsLiveObject(Out.LibraryDefaultObject) ||
			!GuardedIsA(Out.LibraryDefaultObject, Out.LibraryClass) ||
			!ValidateProcessEventTarget(Out.LibraryDefaultObject))
		{
			return false;
		}

		std::array<wchar_t, kMaximumWideTokenCharacters + 2> Direct{};
		if (!ConvertUtf8Token(AssetPath, Direct.data(), Direct.size()))
		{
			Status = ESampleStatus::InvalidDirective;
			return false;
		}
		Out.Asset = const_cast<UObject*>(
			GuardedStaticFindObject(Direct.data(), Out.AssetClass));

		// Some config paths name the package only. Try exactly one deterministic
		// object-name form, never a global object scan or asset load.
		if (!Out.Asset)
		{
			const std::size_t PathLength = wcslen(Direct.data());
			const wchar_t* LastSlash = wcsrchr(Direct.data(), L'/');
			const wchar_t* LastDot = wcsrchr(Direct.data(), L'.');
			if (LastSlash && (!LastDot || LastDot < LastSlash) &&
				LastSlash[1] != L'\0')
			{
				const std::size_t LeafLength =
					PathLength - static_cast<std::size_t>(
						LastSlash + 1 - Direct.data());
				if (PathLength + 1 + LeafLength < Direct.size())
				{
					Direct[PathLength] = L'.';
					memcpy(Direct.data() + PathLength + 1, LastSlash + 1,
						(LeafLength + 1) * sizeof(wchar_t));
					Out.Asset = const_cast<UObject*>(
						GuardedStaticFindObject(
							Direct.data(), Out.AssetClass));
				}
			}
		}

		if (!Out.Asset || !IsLiveObject(Out.Asset))
		{
			Status = ESampleStatus::AssetNotResident;
			return false;
		}
		if (!GuardedIsA(Out.Asset, Out.AssetClass))
		{
			Status = ESampleStatus::TypeMismatch;
			return false;
		}
		return true;
	}

	bool TryGetPropertyHead(
		const UFunction* Function,
		bool Modern,
		const UField*& Out) noexcept
	{
		Out = nullptr;
		const uint32 HeadOffset = Modern ? 0x50 : Offsets::Children;
		if (HeadOffset == 0 || HeadOffset > 0x200)
			return false;
		return TryRead(AddOffset(Function, HeadOffset), Out);
	}

	bool TryReadProperty(
		const UField* Property,
		bool Modern,
		FName& Name,
		const UField*& Next,
		uint32& Offset,
		uint32& ElementSize,
		uint64& Flags) noexcept
	{
		Name = {};
		Next = nullptr;
		Offset = 0;
		ElementSize = 0;
		Flags = 0;
		const uint32 NameOffset = Modern ? Offsets::FField_Name : 0x18;
		const uint32 NextOffset = Modern ? Offsets::FField_Next : 0x28;
		if (!NameOffset || !NextOffset || !Offsets::Offset_Internal ||
			!Offsets::ElementSize || !Offsets::PropertyFlags)
		{
			return false;
		}
		const std::size_t MetadataSize = (std::max)({
			static_cast<std::size_t>(NameOffset) + sizeof(FName),
			static_cast<std::size_t>(NextOffset) + sizeof(void*),
			static_cast<std::size_t>(Offsets::Offset_Internal) + sizeof(uint32),
			static_cast<std::size_t>(Offsets::ElementSize) + sizeof(uint32),
			static_cast<std::size_t>(Offsets::PropertyFlags) + sizeof(uint64) });
		if (MetadataSize > 0x400 ||
			!IsReadableRange(Property, MetadataSize))
		{
			return false;
		}
		return TryRead(AddOffset(Property, NameOffset), Name) &&
			TryRead(AddOffset(Property, NextOffset), Next) &&
			TryRead(AddOffset(Property, Offsets::Offset_Internal), Offset) &&
			TryRead(AddOffset(Property, Offsets::ElementSize), ElementSize) &&
			TryRead(AddOffset(Property, Offsets::PropertyFlags), Flags);
	}

	bool TryBuildSchemaMode(
		UFunction* Function,
		const FExpectedParameter* Expected,
		const FName* ExpectedNames,
		std::size_t ExpectedCount,
		bool Modern,
		FFunctionSchema& Out) noexcept
	{
		Out = {};
		if (!Function || !Expected || !ExpectedNames || ExpectedCount == 0 ||
			ExpectedCount > kMaximumParameters || !Offsets::PropertiesSize)
		{
			return false;
		}

		int32 SignedParamsSize = 0;
		if (!TryRead(AddOffset(Function, Offsets::PropertiesSize),
				SignedParamsSize) || SignedParamsSize <= 0 ||
			SignedParamsSize > static_cast<int32>(kMaximumParameterBytes))
		{
			return false;
		}
		const uint32 ParamsSize = static_cast<uint32>(SignedParamsSize);

		const UField* Property = nullptr;
		if (!TryGetPropertyHead(Function, Modern, Property) || !Property)
			return false;
		std::array<const UField*, kMaximumFieldWalk> Seen{};
		std::array<bool, kMaximumParameters> Matched{};
		std::size_t Walked = 0;
		std::size_t ParameterCount = 0;
		while (Property)
		{
			if (Walked >= Seen.size())
				return false;
			for (std::size_t Index = 0; Index < Walked; ++Index)
			{
				if (Seen[Index] == Property)
					return false;
			}
			Seen[Walked++] = Property;

			FName Name{};
			const UField* Next = nullptr;
			uint32 Offset = 0;
			uint32 ElementSize = 0;
			uint64 Flags = 0;
			if (!TryReadProperty(Property, Modern, Name, Next,
					Offset, ElementSize, Flags))
			{
				return false;
			}
			Property = Next;
			if ((Flags & kCpfParm) == 0)
				continue;
			++ParameterCount;
			if (ParameterCount > ExpectedCount || Offset >= ParamsSize ||
				ElementSize == 0 || ElementSize > ParamsSize - Offset)
			{
				return false;
			}

			bool Found = false;
			for (std::size_t Index = 0; Index < ExpectedCount; ++Index)
			{
				if (!NamesEqual(Name, ExpectedNames[Index]))
					continue;
				if (Matched[Index] ||
					ElementSize != Expected[Index].ElementSize ||
					(Expected[Index].Alignment > 1 &&
						Offset % Expected[Index].Alignment != 0) ||
					(Flags & Expected[Index].RequiredFlags) !=
						Expected[Index].RequiredFlags ||
					(Flags & Expected[Index].ForbiddenFlags) != 0)
				{
					return false;
				}
				Matched[Index] = true;
				Out.Offsets[Index] = Offset;
				Found = true;
				break;
			}
			if (!Found)
				return false;
		}

		if (ParameterCount != ExpectedCount)
			return false;
		for (std::size_t Index = 0; Index < ExpectedCount; ++Index)
		{
			if (!Matched[Index])
				return false;
		}
		Out.Function = Function;
		Out.ParamsSize = ParamsSize;
		return true;
	}

	bool TryBuildSchema(
		UFunction* Function,
		const FExpectedParameter* Expected,
		std::size_t ExpectedCount,
		FFunctionSchema& Out) noexcept
	{
		Out = {};
		if (!IsLiveObject(Function) || !Expected || ExpectedCount == 0 ||
			ExpectedCount > kMaximumParameters ||
			!Offsets::FNameConstructor)
		{
			return false;
		}
		std::array<FName, kMaximumParameters> Names{};
		for (std::size_t Index = 0; Index < ExpectedCount; ++Index)
		{
			if (!Expected[Index].Name ||
				!GuardedConstructName(&Names[Index], Expected[Index].Name))
			{
				return false;
			}
		}

		FFunctionSchema Legacy{};
		FFunctionSchema Modern{};
		const bool HasLegacy = TryBuildSchemaMode(
			Function, Expected, Names.data(), ExpectedCount, false, Legacy);
		const bool HasModern = TryBuildSchemaMode(
			Function, Expected, Names.data(), ExpectedCount, true, Modern);
		if (HasLegacy == HasModern)
			return false;
		Out = HasModern ? Modern : Legacy;
		return true;
	}

	UFunction* ResolveFunction(
		const wchar_t* Path,
		const FRuntimeObjects& Objects) noexcept
	{
		UFunction* Function = const_cast<UFunction*>(reinterpret_cast<
			const UFunction*>(GuardedStaticFindObject(Path, Objects.FunctionClass)));
		return IsLiveObject(Function) &&
			GuardedIsA(Function, Objects.FunctionClass)
			? Function : nullptr;
	}

	bool WriteParam(
		std::array<uint8, kMaximumParameterBytes>& Params,
		const FFunctionSchema& Schema,
		std::size_t Parameter,
		const void* Value,
		std::size_t Size) noexcept
	{
		if (!Value || Parameter >= Schema.Offsets.size())
			return false;
		const uint32 Offset = Schema.Offsets[Parameter];
		if (Offset > Schema.ParamsSize || Size > Schema.ParamsSize - Offset ||
			Offset > Params.size() || Size > Params.size() - Offset)
		{
			return false;
		}
		memcpy(Params.data() + Offset, Value, Size);
		return true;
	}

	bool ReadParam(
		const std::array<uint8, kMaximumParameterBytes>& Params,
		const FFunctionSchema& Schema,
		std::size_t Parameter,
		void* Value,
		std::size_t Size) noexcept
	{
		if (!Value || Parameter >= Schema.Offsets.size())
			return false;
		const uint32 Offset = Schema.Offsets[Parameter];
		if (Offset > Schema.ParamsSize || Size > Schema.ParamsSize - Offset ||
			Offset > Params.size() || Size > Params.size() - Offset)
		{
			return false;
		}
		memcpy(Value, Params.data() + Offset, Size);
		return true;
	}

	bool InvokeRowNames(
		const FRuntimeObjects& Objects,
		const FFunctionSchema& Schema,
		FRawArray& OutNames,
		bool& Owned) noexcept
	{
		OutNames = {};
		Owned = false;
		std::array<uint8, kMaximumParameterBytes> Params{};
		if (!WriteParam(Params, Schema, 0, &Objects.Asset,
				sizeof(Objects.Asset)) ||
			!ValidateProcessEventTarget(Objects.LibraryDefaultObject))
		{
			return false;
		}
		const bool Called = GuardedProcessEvent(
			Objects.LibraryDefaultObject, Schema.Function, Params.data());
		ReadParam(Params, Schema, 1, &OutNames, sizeof(OutNames));
		Owned = Called;
		return Called;
	}

	bool InvokeColumn(
		const FRuntimeObjects& Objects,
		const FFunctionSchema& Schema,
		const FName& Column,
		FRawArray& OutValues,
		bool& Owned) noexcept
	{
		OutValues = {};
		Owned = false;
		std::array<uint8, kMaximumParameterBytes> Params{};
		if (!WriteParam(Params, Schema, 0, &Objects.Asset,
				sizeof(Objects.Asset)) ||
			!WriteParam(Params, Schema, 1, &Column, sizeof(Column)) ||
			!ValidateProcessEventTarget(Objects.LibraryDefaultObject))
		{
			return false;
		}
		const bool Called = GuardedProcessEvent(
			Objects.LibraryDefaultObject, Schema.Function, Params.data());
		ReadParam(Params, Schema, 2, &OutValues, sizeof(OutValues));
		Owned = Called;
		return Called;
	}

	bool IsStructurallyValidArray(
		const FRawArray& Array,
		std::size_t ElementSize,
		int32 Maximum) noexcept
	{
		if (!ElementSize || Array.Num < 0 || Array.Max < Array.Num ||
			Array.Max > Maximum)
		{
			return false;
		}
		if (Array.Num == 0)
			return Array.Data == nullptr || Array.Max >= 0;
		const std::size_t Count = static_cast<std::size_t>(Array.Num);
		return Array.Data &&
			Count <= (std::numeric_limits<std::size_t>::max)() / ElementSize &&
			IsReadableRange(Array.Data, Count * ElementSize);
	}

	bool TryGetWideString(
		const FString& Value,
		const wchar_t*& Data,
		std::size_t& Length) noexcept
	{
		Data = nullptr;
		Length = 0;
		if (Value.NumElements < 0 || Value.MaxElements < Value.NumElements ||
			Value.MaxElements > static_cast<int32>(kMaximumCellCharacters + 1))
		{
			return false;
		}
		if (Value.NumElements == 0)
			return Value.Data == nullptr;
		if (!Value.Data || Value.NumElements < 1 ||
			!IsReadableRange(Value.Data,
				static_cast<std::size_t>(Value.NumElements) * sizeof(wchar_t)) ||
			Value.Data[Value.NumElements - 1] != L'\0')
		{
			return false;
		}
		for (int32 Index = 0; Index < Value.NumElements - 1; ++Index)
		{
			if (Value.Data[Index] == L'\0')
				return false;
		}
		Data = Value.Data;
		Length = static_cast<std::size_t>(Value.NumElements - 1);
		return true;
	}

	void ReleaseArray(FRawArray& Array, bool Strings, bool Owned) noexcept
	{
		if (!Owned || !Offsets::Realloc || !Array.Data)
		{
			Array = {};
			return;
		}
		const std::size_t ElementSize =
			Strings ? sizeof(FString) : sizeof(FName);
		if (!IsStructurallyValidArray(Array, ElementSize, kMaximumRows))
		{
			// Ownership was returned through an unproven ABI. Leaking a bounded
			// anomalous result is safer than freeing a forged/invalid pointer.
			Array = {};
			return;
		}
		if (Strings)
		{
			auto* Values = static_cast<FString*>(Array.Data);
			for (int32 Index = 0; Index < Array.Num; ++Index)
			{
				const wchar_t* Data = nullptr;
				std::size_t Length = 0;
				if (!TryGetWideString(Values[Index], Data, Length))
				{
					Array = {};
					return;
				}
			}
			for (int32 Index = 0; Index < Array.Num; ++Index)
			{
				if (Values[Index].Data)
					GuardedFree(Values[Index].Data);
			}
		}
		GuardedFree(Array.Data);
		Array = {};
	}

	void ReleaseSnapshot(FDataSnapshot& Snapshot) noexcept
	{
		ReleaseArray(Snapshot.Values, true, Snapshot.ValuesOwned);
		ReleaseArray(Snapshot.RowNames, false, Snapshot.RowNamesOwned);
		Snapshot = {};
	}

	bool ValidateSnapshot(const FDataSnapshot& Snapshot) noexcept
	{
		if (!IsStructurallyValidArray(
				Snapshot.RowNames, sizeof(FName), kMaximumRows) ||
			!IsStructurallyValidArray(
				Snapshot.Values, sizeof(FString), kMaximumRows) ||
			Snapshot.RowNames.Num <= 0 ||
			Snapshot.RowNames.Num != Snapshot.Values.Num)
		{
			return false;
		}
		const auto* Names = static_cast<const FName*>(Snapshot.RowNames.Data);
		const auto* Values = static_cast<const FString*>(Snapshot.Values.Data);
		std::size_t TotalCharacters = 0;
		for (int32 Index = 0; Index < Snapshot.RowNames.Num; ++Index)
		{
			if (!Names[Index].IsValid())
				return false;
			const wchar_t* Data = nullptr;
			std::size_t Length = 0;
			if (!TryGetWideString(Values[Index], Data, Length) ||
				Length > kMaximumSnapshotCharacters - TotalCharacters)
			{
				return false;
			}
			TotalCharacters += Length;
		}
		return true;
	}

	bool SnapshotsEqual(
		const FDataSnapshot& Left,
		const FDataSnapshot& Right) noexcept
	{
		if (Left.RowNames.Num != Right.RowNames.Num ||
			Left.Values.Num != Right.Values.Num)
		{
			return false;
		}
		const auto* LeftNames = static_cast<const FName*>(Left.RowNames.Data);
		const auto* RightNames = static_cast<const FName*>(Right.RowNames.Data);
		const auto* LeftValues = static_cast<const FString*>(Left.Values.Data);
		const auto* RightValues = static_cast<const FString*>(Right.Values.Data);
		for (int32 Index = 0; Index < Left.RowNames.Num; ++Index)
		{
			if (!NamesEqual(LeftNames[Index], RightNames[Index]))
				return false;
			const wchar_t* LeftData = nullptr;
			const wchar_t* RightData = nullptr;
			std::size_t LeftLength = 0;
			std::size_t RightLength = 0;
			if (!TryGetWideString(LeftValues[Index], LeftData, LeftLength) ||
				!TryGetWideString(RightValues[Index], RightData, RightLength) ||
				LeftLength != RightLength ||
				(LeftLength != 0 && memcmp(LeftData, RightData,
					LeftLength * sizeof(wchar_t)) != 0))
			{
				return false;
			}
		}
		return true;
	}

	bool InvokeSnapshot(
		const FRuntimeObjects& Objects,
		const FFunctionSchema& RowSchema,
		const FFunctionSchema& ColumnSchema,
		const FName& Column,
		FDataSnapshot& Out) noexcept
	{
		Out = {};
		if (!InvokeRowNames(Objects, RowSchema,
				Out.RowNames, Out.RowNamesOwned) ||
			!InvokeColumn(Objects, ColumnSchema, Column,
				Out.Values, Out.ValuesOwned))
		{
			return false;
		}
		return true;
	}

	bool BuildDataRestoration(
		const FParsedRowUpdateDirective& Directive,
		const FString& Value,
		FSampleResult& Out) noexcept
	{
		const wchar_t* Wide = nullptr;
		std::size_t WideLength = 0;
		if (!TryGetWideString(Value, Wide, WideLength) || WideLength == 0)
			return false;
		for (std::size_t Index = 0; Index < WideLength; ++Index)
		{
			if (Wide[Index] == L';' || Wide[Index] == L'\r' ||
				Wide[Index] == L'\n')
			{
				return false;
			}
		}
		FFixedWriter Writer(Out);
		return Writer.Append("+DataTable=") &&
			Writer.Append(Directive.AssetPath) &&
			Writer.Append(";RowUpdate;") &&
			Writer.Append(Directive.RowName) && Writer.Append(";") &&
			Writer.Append(Directive.ColumnName) && Writer.Append(";") &&
			Writer.AppendWide(Wide, WideLength) && Writer.Finish();
	}

	bool SampleDataTable(
		const FParsedRowUpdateDirective& Directive,
		const FRuntimeObjects& Objects,
		const FName& Row,
		const FName& Column,
		FSampleResult& Out) noexcept
	{
		static constexpr FExpectedParameter RowParameters[] =
		{
			{ L"Table", sizeof(void*), alignof(void*), kCpfParm,
				kCpfOutParm | kCpfReturnParm },
			{ L"OutRowNames", sizeof(FRawArray), alignof(void*),
				kCpfParm | kCpfOutParm, kCpfReturnParm }
		};
		static constexpr FExpectedParameter ColumnParameters[] =
		{
			{ L"DataTable", sizeof(void*), alignof(void*), kCpfParm,
				kCpfOutParm | kCpfReturnParm },
			{ L"PropertyName", sizeof(FName), alignof(FName), kCpfParm,
				kCpfOutParm | kCpfReturnParm },
			{ L"ReturnValue", sizeof(FRawArray), alignof(void*),
				kCpfParm | kCpfOutParm | kCpfReturnParm, 0 }
		};

		UFunction* RowFunction = ResolveFunction(kGetRowNamesPath, Objects);
		UFunction* ColumnFunction = ResolveFunction(kGetColumnPath, Objects);
		FFunctionSchema RowSchema{};
		FFunctionSchema ColumnSchema{};
		if (!RowFunction || !ColumnFunction ||
			!TryBuildSchema(RowFunction, RowParameters,
				_countof(RowParameters), RowSchema) ||
			!TryBuildSchema(ColumnFunction, ColumnParameters,
				_countof(ColumnParameters), ColumnSchema))
		{
			Out.Status = ESampleStatus::ReflectionUnavailable;
			return false;
		}

		FDataSnapshot First{};
		FDataSnapshot Second{};
		const bool Invoked = InvokeSnapshot(
			Objects, RowSchema, ColumnSchema, Column, First) &&
			InvokeSnapshot(
				Objects, RowSchema, ColumnSchema, Column, Second);
		if (!Invoked)
		{
			ReleaseSnapshot(Second);
			ReleaseSnapshot(First);
			Out.Status = ESampleStatus::Faulted;
			return false;
		}
		if (!ValidateSnapshot(First) || !ValidateSnapshot(Second) ||
			!SnapshotsEqual(First, Second))
		{
			ReleaseSnapshot(Second);
			ReleaseSnapshot(First);
			Out.Status = ESampleStatus::UnstableSnapshot;
			return false;
		}

		const auto* Names = static_cast<const FName*>(First.RowNames.Data);
		const auto* Values = static_cast<const FString*>(First.Values.Data);
		int32 MatchIndex = -1;
		int32 MatchCount = 0;
		for (int32 Index = 0; Index < First.RowNames.Num; ++Index)
		{
			if (NamesEqual(Names[Index], Row))
			{
				MatchIndex = Index;
				++MatchCount;
			}
		}
		if (MatchCount != 1 || MatchIndex < 0)
		{
			ReleaseSnapshot(Second);
			ReleaseSnapshot(First);
			Out.Status = ESampleStatus::TargetRowNotFound;
			return false;
		}

		const bool Built = BuildDataRestoration(
			Directive, Values[MatchIndex], Out);
		ReleaseSnapshot(Second);
		ReleaseSnapshot(First);
		if (!Built)
		{
			Out = {};
			Out.Status = ESampleStatus::UnsafeRestorationValue;
			return false;
		}
		Out.Semantics = ERestorationSemantics::Exact;
		Out.Status = ESampleStatus::Success;
		return true;
	}

	bool InvokeCurve(
		const FRuntimeObjects& Objects,
		const FFunctionSchema& Schema,
		const FName& Row,
		float Time,
		uint8& Result,
		float& Value) noexcept
	{
		std::array<uint8, kMaximumParameterBytes> Params{};
		FRawArray EmptyContext{};
		Result = 0xff;
		Value = std::numeric_limits<float>::quiet_NaN();
		if (!WriteParam(Params, Schema, 0, &Objects.Asset,
				sizeof(Objects.Asset)) ||
			!WriteParam(Params, Schema, 1, &Row, sizeof(Row)) ||
			!WriteParam(Params, Schema, 2, &Time, sizeof(Time)) ||
			!WriteParam(Params, Schema, 3, &Result, sizeof(Result)) ||
			!WriteParam(Params, Schema, 4, &Value, sizeof(Value)) ||
			!WriteParam(Params, Schema, 5, &EmptyContext,
				sizeof(EmptyContext)) ||
			!ValidateProcessEventTarget(Objects.LibraryDefaultObject) ||
			!GuardedProcessEvent(
				Objects.LibraryDefaultObject, Schema.Function, Params.data()))
		{
			return false;
		}
		return ReadParam(Params, Schema, 3, &Result, sizeof(Result)) &&
			ReadParam(Params, Schema, 4, &Value, sizeof(Value));
	}

	bool SampleCurveTable(
		const FParsedRowUpdateDirective& Directive,
		const FRuntimeObjects& Objects,
		const FName& Row,
		FSampleResult& Out) noexcept
	{
		static constexpr FExpectedParameter Parameters[] =
		{
			{ L"CurveTable", sizeof(void*), alignof(void*), kCpfParm,
				kCpfOutParm | kCpfReturnParm },
			{ L"RowName", sizeof(FName), alignof(FName), kCpfParm,
				kCpfOutParm | kCpfReturnParm },
			{ L"InXY", sizeof(float), alignof(float), kCpfParm,
				kCpfOutParm | kCpfReturnParm },
			{ L"OutResult", sizeof(uint8), alignof(uint8),
				kCpfParm | kCpfOutParm, kCpfReturnParm },
			{ L"OutXY", sizeof(float), alignof(float),
				kCpfParm | kCpfOutParm, kCpfReturnParm },
			{ L"ContextString", sizeof(FRawArray), alignof(void*), kCpfParm,
				kCpfOutParm | kCpfReturnParm }
		};

		float Time = 0.0f;
		const char* const Begin = Directive.ColumnName.data();
		const char* const End = Begin + Directive.ColumnName.size();
		const auto Parsed = std::from_chars(Begin, End, Time);
		if (Parsed.ec != std::errc{} || Parsed.ptr != End ||
			!std::isfinite(Time))
		{
			Out.Status = ESampleStatus::InvalidDirective;
			return false;
		}

		UFunction* Function = ResolveFunction(kEvaluateCurvePath, Objects);
		FFunctionSchema Schema{};
		if (!Function || !TryBuildSchema(Function, Parameters,
				_countof(Parameters), Schema))
		{
			Out.Status = ESampleStatus::ReflectionUnavailable;
			return false;
		}

		uint8 FirstResult = 0xff;
		uint8 SecondResult = 0xff;
		float FirstValue = 0.0f;
		float SecondValue = 0.0f;
		if (!InvokeCurve(Objects, Schema, Row, Time,
				FirstResult, FirstValue) ||
			!InvokeCurve(Objects, Schema, Row, Time,
				SecondResult, SecondValue))
		{
			Out.Status = ESampleStatus::Faulted;
			return false;
		}
		if (FirstResult != 0 || SecondResult != 0 ||
			!std::isfinite(FirstValue) || !std::isfinite(SecondValue))
		{
			Out.Status = ESampleStatus::EvaluationFailed;
			return false;
		}
		if (std::bit_cast<uint32>(FirstValue) !=
			std::bit_cast<uint32>(SecondValue))
		{
			Out.Status = ESampleStatus::UnstableSnapshot;
			return false;
		}

		char Value[64]{};
		const auto Encoded = std::to_chars(
			Value, Value + sizeof(Value), FirstValue,
			std::chars_format::general,
			std::numeric_limits<float>::max_digits10);
		if (Encoded.ec != std::errc{})
		{
			Out.Status = ESampleStatus::UnsafeRestorationValue;
			return false;
		}
		FFixedWriter Writer(Out);
		if (!Writer.Append("+CurveTable=") ||
			!Writer.Append(Directive.AssetPath) ||
			!Writer.Append(";RowUpdate;") ||
			!Writer.Append(Directive.RowName) || !Writer.Append(";") ||
			!Writer.Append(Directive.ColumnName) || !Writer.Append(";") ||
			!Writer.Append(std::string_view(Value, Encoded.ptr)) ||
			!Writer.Finish())
		{
			Out = {};
			Out.Status = ESampleStatus::UnsafeRestorationValue;
			return false;
		}
		Out.Semantics = ERestorationSemantics::BehavioralNonStructural;
		Out.Status = ESampleStatus::Success;
		return true;
	}
}

bool TrySampleResidentBaseline(
	const FParsedRowUpdateDirective& Directive,
	FSampleResult& Out) noexcept
{
	Out = {};
	if (Directive.Kind == EAssetKind::DataTable)
	{
		// The public functions return row names and column values as separate
		// arrays. Until the reflected ArrayProperty inner types and their ordering
		// contract are proven on this runtime, pairing them could restore the wrong
		// row. Fail closed instead of running whole-column exports per candidate.
		Out.Status = ESampleStatus::ReflectionUnavailable;
		return false;
	}
	if (Directive.AssetPath.size() > kMaximumAssetPathBytes ||
		Directive.RowName.size() > kMaximumRowNameBytes ||
		Directive.ColumnName.size() > kMaximumColumnNameBytes)
	{
		Out.Status = ESampleStatus::BoundsExceeded;
		return false;
	}
	if (!IsSafeToken(Directive.AssetPath, kMaximumAssetPathBytes) ||
		!IsSafeToken(Directive.RowName, kMaximumRowNameBytes) ||
		!IsSafeToken(Directive.ColumnName, kMaximumColumnNameBytes) ||
		Directive.AssetPath.front() != '/')
	{
		Out.Status = ESampleStatus::InvalidDirective;
		return false;
	}

	std::array<wchar_t, kMaximumRowNameBytes + 1> WideRow{};
	std::array<wchar_t, kMaximumColumnNameBytes + 1> WideColumn{};
	if (!ConvertUtf8Token(
			Directive.RowName, WideRow.data(), WideRow.size()) ||
		(Directive.Kind == EAssetKind::DataTable &&
			!ConvertUtf8Token(
				Directive.ColumnName, WideColumn.data(), WideColumn.size())))
	{
		Out.Status = ESampleStatus::InvalidDirective;
		return false;
	}

	FRuntimeObjects Objects{};
	ESampleStatus ResolveStatus = ESampleStatus::ReflectionUnavailable;
	if (!ResolveRuntimeObjects(
			Directive.Kind, Directive.AssetPath, Objects, ResolveStatus))
	{
		Out.Status = ResolveStatus;
		return false;
	}

	FName Row{};
	FName Column{};
	if (!GuardedConstructName(&Row, WideRow.data()) ||
		(Directive.Kind == EAssetKind::DataTable &&
			!GuardedConstructName(&Column, WideColumn.data())))
	{
		Out.Status = ESampleStatus::Faulted;
		return false;
	}

	if (Directive.Kind == EAssetKind::DataTable)
		return SampleDataTable(Directive, Objects, Row, Column, Out);
	if (Directive.Kind == EAssetKind::CurveTable)
		return SampleCurveTable(Directive, Objects, Row, Out);

	Out.Status = ESampleStatus::InvalidDirective;
	return false;
}
}
