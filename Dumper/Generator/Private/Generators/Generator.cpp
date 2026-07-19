
#include "Generators/Generator.h"
#include "Managers/StructManager.h"
#include "Managers/EnumManager.h"
#include "Managers/MemberManager.h"
#include "Managers/PackageManager.h"

#include "HashStringTable.h"
#include "Utils.h"

#include "Platform.h"
#include "Json/json.hpp"
#include "Unreal/UnrealTypes.h"

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

	std::cerr << "[DBG] ObjectArray::Init...\n" << std::flush;
	ObjectArray::Init();
	std::cerr << "[DBG] ObjectArray::Init done\n" << std::flush;

	std::cerr << "[DBG] FName::Init...\n" << std::flush;
	/* AppendString crashes at runtime for this game (game-side allocator/TLS issue).
	 * Force the GNames/FNamePool lookup path instead. */
	FName::Init_Windows(/*bForceGNames =*/true);

	/* Null AppendString and verify ToStr is NameArray-backed. */
	const bool bGNamesOK = FName::ForceGNamesMode();
	std::cerr << std::format("[DBG] FName::Init done. GNamesOK={}\n", (int)bGNamesOK) << std::flush;

	if (!bGNamesOK)
	{
		std::cerr << "[FATAL] GNames not initialized - cannot resolve FNames. Aborting.\n" << std::flush;
		return;
	}

	std::cerr << "[DBG] Off::Init...\n" << std::flush;
	Off::Init();
	std::cerr << "[DBG] Off::Init done\n" << std::flush;

	std::cerr << "[DBG] PropertySizes::Init...\n" << std::flush;
	PropertySizes::Init();
	std::cerr << "[DBG] PropertySizes::Init done\n" << std::flush;

	std::cerr << "[DBG] InitPE...\n" << std::flush;
	CALL_PLATFORM_SPECIFIC_FUNCTION(Off::InSDK::ProcessEvent::InitPE); // Must be at this position, relies on offsets initialized in Off::Init()
	std::cerr << "[DBG] InitPE done\n" << std::flush;

	std::cerr << "[DBG] InitGWorld...\n" << std::flush;
	Off::InSDK::World::InitGWorld(); // Must be at this position, relies on offsets initialized in Off::Init()
	std::cerr << "[DBG] InitGWorld done\n" << std::flush;

	std::cerr << "[DBG] InitTextOffsets...\n" << std::flush;
	Off::InSDK::Text::InitTextOffsets(); // Must be at this position, relies on offsets initialized in Off::InitPE()
	std::cerr << "[DBG] InitTextOffsets done\n" << std::flush;

	std::cerr << "[DBG] InitSettings...\n" << std::flush;
	InitSettings();
	std::cerr << "[DBG] InitSettings done\n" << std::flush;
}

void Generator::InitInternal()
{
	// Initialize PackageManager with all packages, their names, structs, classes enums, functions and dependencies
	std::cerr << "[DBG] PackageManager::Init...\n" << std::flush;
	PackageManager::Init();
	std::cerr << "[DBG] PackageManager::Init done\n" << std::flush;

	// Initialize StructManager with all structs and their names
	std::cerr << "[DBG] StructManager::Init...\n" << std::flush;
	StructManager::Init();
	std::cerr << "[DBG] StructManager::Init done\n" << std::flush;
	
	// Initialize EnumManager with all enums and their names
	std::cerr << "[DBG] EnumManager::Init...\n" << std::flush;
	EnumManager::Init();
	std::cerr << "[DBG] EnumManager::Init done\n" << std::flush;
	
	// Initialized all Member-Name collisions
	std::cerr << "[DBG] MemberManager::Init...\n" << std::flush;
	MemberManager::Init();
	std::cerr << "[DBG] MemberManager::Init done\n" << std::flush;

	// Post-Initialize PackageManager after StructManager has been initialized. 'PostInit()' handles Cyclic-Dependencies detection
	std::cerr << "[DBG] PackageManager::PostInit...\n" << std::flush;
	PackageManager::PostInit();
	std::cerr << "[DBG] PackageManager::PostInit done\n" << std::flush;
}

bool Generator::SetupDumperFolder()
{
	try
	{
		std::cerr << "[DBG] SetupDumperFolder: building FolderName...\n" << std::flush;
		std::string FolderName = (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName);
		std::cerr << "[DBG] SetupDumperFolder: FolderName='" << FolderName << "'\n" << std::flush;

		FileNameHelper::MakeValidFileName(FolderName);
		std::cerr << "[DBG] SetupDumperFolder: MakeValidFileName -> '" << FolderName << "'\n" << std::flush;

		DumperFolder = fs::path(Settings::Generator::SDKGenerationPath) / FolderName;
		std::cerr << "[DBG] SetupDumperFolder: DumperFolder='" << DumperFolder.string() << "'\n" << std::flush;

		if (fs::exists(DumperFolder))
		{
			std::cerr << "[DBG] SetupDumperFolder: folder exists, renaming to _OLD...\n" << std::flush;
			fs::path Old = DumperFolder.generic_string() + "_OLD";

			fs::remove_all(Old);
			std::cerr << "[DBG] SetupDumperFolder: remove_all done\n" << std::flush;

			fs::rename(DumperFolder, Old);
			std::cerr << "[DBG] SetupDumperFolder: rename done\n" << std::flush;
		}

		std::cerr << "[DBG] SetupDumperFolder: create_directories...\n" << std::flush;
		fs::create_directories(DumperFolder);
		std::cerr << "[DBG] SetupDumperFolder: create_directories done\n" << std::flush;
	}
	catch (const std::filesystem::filesystem_error& fe)
	{
		std::cerr << "[SetupDumperFolder] filesystem_error: " << fe.what() << "\n" << std::flush;
		DumperFolder.clear();
		return false;
	}
	catch (const std::exception& e)
	{
		std::cerr << "[SetupDumperFolder] std::exception: " << e.what() << "\n" << std::flush;
		DumperFolder.clear();
		return false;
	}
	catch (...)
	{
		std::cerr << "[SetupDumperFolder] unknown exception!\n" << std::flush;
		DumperFolder.clear();
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