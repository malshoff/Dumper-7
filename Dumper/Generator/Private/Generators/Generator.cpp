
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

	ObjectArray::Init();

	/* Dragon Ball FighterZ (UE4.17) — GNames override required because
	 * the auto-scan relies on kernel32.dll imports (EnterCriticalSection / InitializeSRWLock)
	 * which are stripped in this packed binary. Offset found via IDA: FName::StaticInit
	 * references TNameEntryArray global at qword_143F6BD08 (RVA 0x3F6BD08). */
	FName::Init(0x3F6BD08, FName::EOffsetOverrideType::GNames, false);
	std::cerr << "[DBFZ-DIAG] FName::Init() done\n";

	Off::Init();
	std::cerr << "[DBFZ-DIAG] Off::Init() done\n";

	/* DBFZ UE4.17 — override UEnum::Names if auto-finder failed.
	 * The game's TArray is 0x18 bytes {Data, AllocatorPad, Num, Max}.
	 * UEnum layout: UField(0x30) + FString CppType(0x18) → Names at 0x48. */
	if (Off::UEnum::Names < 0)
	{
		Off::UEnum::Names = 0x48;
		std::cerr << "[DBFZ-OVERRIDE] Off::UEnum::Names = 0x48\n";
	}

	PropertySizes::Init();
	std::cerr << "[DBFZ-DIAG] PropertySizes::Init() done\n";

	CALL_PLATFORM_SPECIFIC_FUNCTION(Off::InSDK::ProcessEvent::InitPE); // Must be at this position, relies on offsets initialized in Off::Init()
	std::cerr << "[DBFZ-DIAG] ProcessEvent::InitPE done\n";

	Off::InSDK::World::InitGWorld(); // Must be at this position, relies on offsets initialized in Off::Init()
	std::cerr << "[DBFZ-DIAG] InitGWorld done\n";

	Off::InSDK::Text::InitTextOffsets(); // Must be at this position, relies on offsets initialized in Off::InitPE()
	std::cerr << "[DBFZ-DIAG] InitTextOffsets done\n";

	InitSettings();
	std::cerr << "[DBFZ-DIAG] InitSettings done\n";
}

void Generator::InitInternal()
{
	std::cerr << "[InitInternal] Starting PackageManager::Init()...\n"; std::cerr.flush();
	// Initialize PackageManager with all packages, their names, structs, classes enums, functions and dependencies
	PackageManager::Init();
	std::cerr << "[InitInternal] PackageManager::Init() done.\n"; std::cerr.flush();

	std::cerr << "[InitInternal] Starting StructManager::Init()...\n"; std::cerr.flush();
	// Initialize StructManager with all structs and their names
	StructManager::Init();
	std::cerr << "[InitInternal] StructManager::Init() done.\n"; std::cerr.flush();
	
	std::cerr << "[InitInternal] Starting EnumManager::Init()...\n"; std::cerr.flush();
	// Initialize EnumManager with all enums and their names
	EnumManager::Init();
	std::cerr << "[InitInternal] EnumManager::Init() done.\n"; std::cerr.flush();
	
	std::cerr << "[InitInternal] Starting MemberManager::Init()...\n"; std::cerr.flush();
	// Initialized all Member-Name collisions
	MemberManager::Init();
	std::cerr << "[InitInternal] MemberManager::Init() done.\n"; std::cerr.flush();

	std::cerr << "[InitInternal] Starting PackageManager::PostInit()...\n"; std::cerr.flush();
	// Post-Initialize PackageManager after StructManager has been initialized. 'PostInit()' handles Cyclic-Dependencies detection
	PackageManager::PostInit();
	std::cerr << "[InitInternal] PackageManager::PostInit() done.\n"; std::cerr.flush();
}

bool Generator::SetupDumperFolder()
{
	std::error_code ec;

	std::string FolderName = (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName);

	FileNameHelper::MakeValidFileName(FolderName);

	DumperFolder = fs::path(Settings::Generator::SDKGenerationPath) / FolderName;

	if (fs::exists(DumperFolder, ec))
	{
		fs::path Old = DumperFolder.generic_string() + "_OLD";

		fs::remove_all(Old, ec);
		if (ec) std::cerr << "[SetupDumperFolder] remove_all failed: " << ec.message() << "\n";

		fs::rename(DumperFolder, Old, ec);
		if (ec) std::cerr << "[SetupDumperFolder] rename failed: " << ec.message() << "\n";
	}

	fs::create_directories(DumperFolder, ec);
	if (ec)
	{
		std::cerr << "[SetupDumperFolder] create_directories failed: " << ec.message() << "\n";
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
	std::error_code ec;

	FileNameHelper::MakeValidFileName(FolderName);
	FileNameHelper::MakeValidFileName(SubfolderName);

	OutFolder = DumperFolder / FolderName;
	OutSubFolder = OutFolder / SubfolderName;
			
	if (fs::exists(OutFolder, ec))
	{
		fs::path Old = OutFolder.generic_string() + "_OLD";

		fs::remove_all(Old, ec);
		if (ec) std::cerr << "[SetupFolders] remove_all failed: " << ec.message() << "\n";

		fs::rename(OutFolder, Old, ec);
		if (ec) std::cerr << "[SetupFolders] rename failed: " << ec.message() << "\n";
	}

	fs::create_directories(OutFolder, ec);
	if (ec)
	{
		std::cerr << "[SetupFolders] create_directories failed: " << ec.message() << "\n";
		return false;
	}

	if (!SubfolderName.empty())
	{
		fs::create_directories(OutSubFolder, ec);
		if (ec)
		{
			std::cerr << "[SetupFolders] create_directories(sub) failed: " << ec.message() << "\n";
			return false;
		}
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