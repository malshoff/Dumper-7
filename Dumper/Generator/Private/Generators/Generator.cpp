 
#include "Generators/Generator.h"
#include "Managers/StructManager.h"
#include "Managers/EnumManager.h"
#include "Managers/MemberManager.h"
#include "Managers/PackageManager.h"

#include "HashStringTable.h"
#include "Utils.h"

#include "Platform.h"
#include "Json/json.hpp"

#include <fstream>

inline void InitSettings()
{
	Settings::InitWeakObjectPtrSettings();
	Settings::InitLargeWorldCoordinateSettings();

	Settings::InitObjectPtrPropertySettings();
	Settings::InitArrayDimSizeSettings();
}

// SEH wrapper for void functions — __try can't coexist with C++ destructors
__declspec(noinline) static void SEH_Call(void(*func)(), const char* name)
{
	__try {
		func();
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		char msg[128];
		snprintf(msg, sizeof(msg), "[Goals] %s crashed, skipping\n", name);
		OutputDebugStringA(msg);
	}
}

// Probe FField memory to find FField::Name offset
// Inner function that does the hexdump with SEH - no C++ objects
__declspec(noinline) static void SEH_ProbeFFieldInner(uint8_t* cp, uintptr_t moduleBase)
{
	struct RawFString { wchar_t* Data; int NumElements; int MaxElements; };
	auto realAppend = reinterpret_cast<void(*)(const void*, RawFString&)>(
		moduleBase + 0x1376990
	);

	// Hexdump
	for (int row = 0; row < 0x80; row += 0x10) {
		char line[80];
		int pos = snprintf(line, sizeof(line), "  +%02X:", row);
		for (int col = 0; col < 0x10; col++) {
			if (col == 8) line[pos++] = ' ';
			uint8_t byte = 0;
			__try { byte = cp[row + col]; } __except(EXCEPTION_EXECUTE_HANDLER) { byte = 0xCC; }
			pos += snprintf(line + pos, sizeof(line) - pos, " %02X", byte);
		}
		line[pos] = 0;
		std::cerr << line << "\n";
	}
	
	// Probe each 4-byte aligned offset for valid FName
	std::cerr << "[FFIELD] Probing FName at each offset:\n";
	for (int off = 0x08; off < 0x70; off += 4) {
		int32 compIdx = 0;
		__try { compIdx = *(int32*)(cp + off); } __except(EXCEPTION_EXECUTE_HANDLER) { continue; }
		
		if (compIdx <= 0 || compIdx > 0x200000) continue;
		
		wchar_t nameBuf[128];
		memset(nameBuf, 0, sizeof(nameBuf));
		RawFString nameStr = { nameBuf, 0, 128 };
		int32 fnamePair[2] = { compIdx, 0 };
		
		__try {
			realAppend(&fnamePair, nameStr);
			if (nameStr.NumElements > 1 && nameStr.NumElements < 64) {
				char result[128];
				int rpos = snprintf(result, sizeof(result), "  +0x%02X: CompIdx=%d -> \"", off, compIdx);
				for (int c = 0; c < nameStr.NumElements - 1 && c < 32; c++) {
					char ch = (char)nameStr.Data[c];
					result[rpos++] = (ch >= 32 && ch < 127) ? ch : '?';
				}
				result[rpos++] = '"';
				result[rpos] = 0;
				std::cerr << result << "\n";
			}
		} __except(EXCEPTION_EXECUTE_HANDLER) {
			// Not a valid FName
		}
	}
	std::cerr.flush();
}

// Outer wrapper that finds the FField pointer using C++ UE wrappers, then delegates to inner
static void SEH_ProbeFFieldName()
{
	auto guidStruct = ObjectArray::FindStructFast("Guid");
	if (!guidStruct) { std::cerr << "[FFIELD] Guid struct not found\n"; return; }
	
	uint8_t* cp = (uint8_t*)guidStruct.GetChildProperties().GetAddress();
	if (!cp || Platform::IsBadReadPtr(cp)) { std::cerr << "[FFIELD] ChildProperties null/bad\n"; return; }
	
	std::cerr << "[FFIELD] Guid.ChildProperties @" << (void*)cp << " hexdump:\n";
	SEH_ProbeFFieldInner(cp, Platform::GetModuleBase());
}

// Separate function for SEH diagnostic — __try can't coexist with C++ destructors
__declspec(noinline) static void DiagTestAppendString()
{
	uint32_t testFName[2] = { 0, 0 }; // CompIdx=0 (None), Number=0
	wchar_t buf[256];
	memset(buf, 0, sizeof(buf));

	// Build a raw FString on the stack (no C++ objects = SEH compatible)
	struct RawFString { wchar_t* Data; int NumElements; int MaxElements; };
	RawFString testStr = { buf, 0, 256 };

	auto realAppend = reinterpret_cast<void(*)(const void*, RawFString&)>(
		Platform::GetModuleBase() + 0x1376990
	);

	__try {
		realAppend(&testFName, testStr);
		// Log result
		std::cerr << "[DIAG] AppendString(CompIdx=0) returned. NumElements=" << testStr.NumElements << " str=\"";
		if (testStr.NumElements > 0 && testStr.Data)
			std::wcerr << testStr.Data;
		std::cerr << "\"\n";
	} __except(EXCEPTION_EXECUTE_HANDLER) {
		std::cerr << "[DIAG] AppendString(CompIdx=0) CRASHED! code=0x" 
		          << std::hex << GetExceptionCode() << std::dec << "\n";
	}
	std::cerr.flush();
}

void Generator::InitEngineCore()
{
	/* manual override */
	//ObjectArray::Init(/*GObjects*/, /*Layout = Default*/); // FFixedUObjectArray (UEVersion < UE4.21)
	//ObjectArray::Init(/*GObjects*/, /*ChunkSize*/, /*Layout = Default*/); // FChunkedFixedUObjectArray (UEVersion >= UE4.21)

	//FName::Init(/*bForceGNames = false*/);
	//FName::Init(/*AppendString, FName::EOffsetOverrideType::AppendString*/);
	//FName::Init(/*ToString, FName::EOffsetOverrideType::ToString*/);
	//FName::Init(/*GNames, FName::EOffsetOverrideType::GNames, true/false*/);
 
	//Off::InSDK::ProcessEvent::InitPE(/*PEIndex*/);

	/* Back4Blood (requires manual GNames override) */
	//InitObjectArrayDecryption([](void* ObjPtr) -> uint8* { return reinterpret_cast<uint8*>(uint64(ObjPtr) ^ 0x8375); });

	/* Multiversus [Unsupported, weird GObjects-struct] */
	//InitObjectArrayDecryption([](void* ObjPtr) -> uint8* { return reinterpret_cast<uint8*>(uint64(ObjPtr) ^ 0x1B5DEAFD6B4068C); });

	/* ===== Goals.exe: Encrypted GObjects =====
	 *
	 * Auto-detection finds the WRONG GObjects (shader params at 0xADD28E8).
	 * Real GObjects is at 0xAD0D390 with SIMD-encrypted chunk pointers.
	 * FUObjectItem size is 0x18 (24 bytes), not 0x10.
	 * The game's indexer function at RVA 0x1416860 decrypts everything.
	 *
	 * Strategy: Let auto-detect run (sets layout offsets for Num/Max),
	 * then override GObjects pointer and ByIndex to use game's function.
	 */

	ObjectArray::Init();

	// Save Num from auto-detected (wrong) address before overriding
	int32 savedNum = ObjectArray::Num();
	std::cerr << "[Goals] Auto-detected Num=" << std::dec << savedNum << "\n"; std::cerr.flush();

	// --- Step 1: Override GObjects to the REAL address ---
	uintptr_t moduleBase = Platform::GetModuleBase();
	uint8* realGObjects = reinterpret_cast<uint8*>(moduleBase + 0xAD0D390);
	ObjectArray::SetGObjectsRaw(realGObjects);
	Off::InSDK::ObjArray::GObjects = 0xAD0D390;

	// The encrypted struct has no readable Num field — use saved or hardcoded count
	ObjectArray::SetNumOverride(savedNum > 0 ? savedNum : 0x40000);
	std::cerr << "[Goals] Overrode GObjects to 0x" << std::hex << (void*)realGObjects 
	          << " NumOverride=" << std::dec << ObjectArray::Num() << "\n"; std::cerr.flush();

	// --- Step 2: Set correct FUObjectItem layout ---
	ObjectArray::SetFUObjectItemSize(0x18);
	ObjectArray::SetFUObjectItemOffset(0x08);  // Flags at +0, UObject* at +8

	// --- Step 3: Hook ByIndex to game's encrypted indexer ---
	// sub_141416860 signature: __int64 __fastcall (GUObjectArray* a1, int index)
	// Returns pointer to the FUObjectItem (24 bytes). UObject* is at offset 0.
	using GameIndexByFn = uint8_t* (__fastcall*)(void* arrayBase, int index);
	static GameIndexByFn GameIndexBy = reinterpret_cast<GameIndexByFn>(moduleBase + 0x1416860);
	static uint8* capturedGObjects = realGObjects;

	ObjectArray::SetCustomByIndex([](void* /*ObjectsArray*/, int32 Index, uint32 /*FUObjectItemSize*/, uint32 FUObjectItemOffset, uint32 /*PerChunk*/) -> void*
	{
		if (Index < 0)
			return nullptr;

		uint8_t* itemPtr = GameIndexBy(capturedGObjects, Index);
		if (!itemPtr || Platform::IsBadReadPtr(itemPtr))
			return nullptr;

		// FUObjectItem layout: [flags 8B][UObject* 8B][...]
		void* objPtr = *reinterpret_cast<void**>(itemPtr + FUObjectItemOffset);
		if (!objPtr || Platform::IsBadReadPtr(objPtr))
			return nullptr;

		// Validate UObject has a readable VTable (first pointer must be valid)
		void* vtable = *reinterpret_cast<void**>(objPtr);
		if (!vtable || Platform::IsBadReadPtr(vtable))
			return nullptr;

		return objPtr;
	});
	std::cerr << "[Goals] Hooked ByIndex to game's encrypted indexer at RVA 0x1416860\n"; std::cerr.flush();

	// --- Step 4: Quick sanity check ---
	{
		void* obj0 = ObjectArray::GetByIndex(0).GetAddress();
		void* obj1 = ObjectArray::GetByIndex(1).GetAddress();
		void* obj5 = ObjectArray::GetByIndex(5).GetAddress();
		std::cerr << "[Goals] Obj[0]=" << obj0 << " Obj[1]=" << obj1 << " Obj[5]=" << obj5 << "\n"; std::cerr.flush();

		if (!obj0 || Platform::IsBadReadPtr(obj0)) {
			std::cerr << "[Goals] ERROR: Obj[0] is invalid after hook! Aborting.\n"; std::cerr.flush();
		}
	}

	/* Goals.exe: FNamePool uses UE5.5 sharded layout, auto-detection fails.
	   AppendString at RVA 0x1376990; FNamePool at RVA 0xAC3E7C0. */

	// Override AppendString
	FName::Init((int32)0x1376990, FName::EOffsetOverrideType::AppendString);

	// Set GNames offset so SetGNamesWithoutCommitting returns early
	Off::InSDK::NameArray::GNames = (int32)0xAC3E7C0;
	Settings::Internal::bUseNamePool = true;

	// --- DIAGNOSTIC: test AppendString before proceeding ---
	DiagTestAppendString();

	// --- GOALS FIELD PATCHING ---
	// From IDA analysis: the game's UObject fields are at non-standard offsets:
	//   ClassPrivate at +0x40 (SIMD encrypted) — must decrypt to +0x10 for finders
	//   NamePrivate  at +0x50 (raw, unencrypted FName) — finders discover naturally
	//   OuterPrivate at +0x58 (raw, unencrypted pointer) — finders discover naturally
	// IMPORTANT: Only patch +0x10 (ClassPrivate). Do NOT touch +0x18/+0x20 as the
	// game's integrity system detects those modifications and crashes.
	{
		// SIMD decryption for ClassPrivate (key from xmmword_14840BFF0)
		const uint8_t xorKey[8] = {0x37, 0x38, 0xED, 0xAE, 0xF4, 0xF2, 0x17, 0xFA};
		auto simdDecrypt = [&](uint64_t encrypted) -> uint64_t {
			uint64_t keyVal = *(uint64_t*)xorKey;
			uint64_t xored = encrypted ^ keyVal;
			uint16_t words[4];
			memcpy(words, &xored, 8);
			uint16_t shuffled[4] = {
				words[(0x93 >> 0) & 3], words[(0x93 >> 2) & 3],
				words[(0x93 >> 4) & 3], words[(0x93 >> 6) & 3],
			};
			uint16_t result[4];
			for (int j = 0; j < 4; j++)
				result[j] = (shuffled[j] >> 4) ^ (shuffled[j] << 12);
			uint64_t out;
			memcpy(&out, result, 8);
			return out;
		};

		int total = ObjectArray::Num();
		int patched = 0;
		int errors = 0;
		
		std::cerr << "[Goals] Patching " << total << " objects (ClassPrivate only to +0x10)...\n";
		std::cerr.flush();
		
		for (int i = 0; i < total; i++) {
			__try {
				void* obj = ObjectArray::GetByIndex(i).GetAddress();
				if (!obj || Platform::IsBadReadPtr(obj)) continue;
				
				uint8_t* b = (uint8_t*)obj;
				
				// Decrypt ClassPrivate from +0x40, write to +0x10
				// Name(+0x50) and Outer(+0x58) are unencrypted — finders discover them
				uint64_t encClass = *(uint64_t*)(b + 0x40);
				uint64_t decClass = simdDecrypt(encClass);
				*(uint64_t*)(b + 0x10) = decClass;
				
				patched++;
			} __except(EXCEPTION_EXECUTE_HANDLER) {
				errors++;
			}
		}
		
		std::cerr << "[Goals] Patched " << patched << "/" << total << " objects";
		if (errors > 0) std::cerr << " (" << errors << " errors)";
		std::cerr << "\n";
		
		// Verify first 5 objects
		for (int i = 0; i < 5; i++) {
			void* obj = ObjectArray::GetByIndex(i).GetAddress();
			if (!obj) continue;
			uint8_t* b = (uint8_t*)obj;
			uint64_t cls = *(uint64_t*)(b + 0x10);
			int32 nameIdx = *(int32*)(b + 0x50);
			void* outer = *(void**)(b + 0x58);
			std::cerr << "[Goals]   Obj[" << i << "] Class=0x" << std::hex << cls 
				<< " NameIdx=" << std::dec << nameIdx 
				<< " Outer=" << outer << "\n";
		}
		std::cerr.flush();
	}

	Off::Init();
	
	// --- DIAGNOSTIC: hexdump FField to find FField::Name ---
	if (Off::UStruct::ChildProperties > 0 && Off::FField::Name < 0) {
		SEH_ProbeFFieldName();
	}
	
	// Guard: only run PropertySizes if FField system is fully initialized
	if (Off::FField::Name > 0) {
		PropertySizes::Init();
	} else {
		std::cerr << "[Goals] SKIPPING PropertySizes::Init (FField::Name not found)\n";
	}

	static auto initPE = []() { CALL_PLATFORM_SPECIFIC_FUNCTION(Off::InSDK::ProcessEvent::InitPE); };
	SEH_Call(+initPE, "InitPE");
	SEH_Call(Off::InSDK::World::InitGWorld, "InitGWorld");
	SEH_Call(Off::InSDK::Text::InitTextOffsets, "InitTextOffsets");
	SEH_Call(InitSettings, "InitSettings");
}

void Generator::InitInternal()
{
	// Initialize PackageManager with all packages, their names, structs, classes enums, functions and dependencies
	PackageManager::Init();

	// Initialize StructManager with all structs and their names
	StructManager::Init();
	
	// Initialize EnumManager with all enums and their names
	EnumManager::Init();
	
	// Initialized all Member-Name collisions
	MemberManager::Init();

	// Post-Initialize PackageManager after StructManager has been initialized. 'PostInit()' handles Cyclic-Dependencies detection
	PackageManager::PostInit();
}

bool Generator::SetupDumperFolder()
{
	try
	{
		std::string FolderName = (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName);

		FileNameHelper::MakeValidFileName(FolderName);

		DumperFolder = fs::path(Settings::Generator::SDKGenerationPath) / FolderName;

		if (fs::exists(DumperFolder))
		{
			fs::path Old = DumperFolder.generic_string() + "_OLD";

			fs::remove_all(Old);

			fs::rename(DumperFolder, Old);
		}

		fs::create_directories(DumperFolder);
	}
	catch (const std::filesystem::filesystem_error& fe)
	{
		std::cerr << "Could not create required folders! Info: \n";
		std::cerr << fe.what() << std::endl;
		return false;
	}

	return true;
}

bool Generator::SetupFolders(std::string& FolderName, fs::path& OutFolder)
{
	fs::path Dummy;
	std::string EmptyName = "";
	return SetupFolders(FolderName, OutFolder, EmptyName, Dummy);
}

bool Generator::SetupFolders(std::string& FolderName, fs::path& OutFolder, std::string& SubfolderName, fs::path& OutSubFolder)
{
	FileNameHelper::MakeValidFileName(FolderName);
	FileNameHelper::MakeValidFileName(SubfolderName);

	try
	{
		OutFolder = DumperFolder / FolderName;
		OutSubFolder = OutFolder / SubfolderName;
				
		if (fs::exists(OutFolder))
		{
			fs::path Old = OutFolder.generic_string() + "_OLD";

			fs::remove_all(Old);

			fs::rename(OutFolder, Old);
		}

		fs::create_directories(OutFolder);

		if (!SubfolderName.empty())
			fs::create_directories(OutSubFolder);
	}
	catch (const std::filesystem::filesystem_error& fe)
	{
		std::cerr << "Could not create required folders! Info: \n";
		std::cerr << fe.what() << std::endl;
		return false;
	}

	return true;
}


void DumpEditorOnlyMetadata(const fs::path& DumperFolder)
{
	if (Off::FField::EditorOnlyMetadata == -1)
		return;

	nlohmann::json MetadataJson;
	MetadataJson["GameName"] = Settings::Generator::GameName;
	MetadataJson["GameVersion"] = Settings::Generator::GameVersion;

	for (UEObject Obj : ObjectArray())
	{
		if (!Obj.IsA(EClassCastFlags::Struct))
			continue;

		UEStruct Struct = Obj.Cast<UEStruct>();

		auto ChildProperties = Struct.GetProperties();

		if (ChildProperties.empty())
			continue;

		auto& StructMembers = MetadataJson[Struct.GetCppName()];

		for (UEProperty Prop : Struct.GetProperties())
		{
			auto& Entries = StructMembers[Prop.GetValidName()];

			for (const auto& [Key, Value] : Prop.Cast<UEFField>().GetMetaData())
			{
				if (Key.empty() && Value.empty())
					continue;

				Entries[Key] = Value;
			}
		}
	}

	std::ofstream MetadataFile(DumperFolder / "Metadata.json");
	MetadataFile << MetadataJson.dump(4);
}