#pragma once

#include <filesystem>
#include <iostream>

#include "Unreal/ObjectArray.h"
#include "Managers/DependencyManager.h"
#include "Managers/MemberManager.h"
#include "HashStringTable.h"


namespace fs = std::filesystem;

void DumpEditorOnlyMetadata(const fs::path& DumperFolder);

template<typename GeneratorType>
concept GeneratorImplementation = requires(GeneratorType t)
{
    /* Require static variables of type */
    GeneratorType::PredefinedMembers;
    requires(std::same_as<decltype(GeneratorType::PredefinedMembers), PredefinedMemberLookupMapType>);

    GeneratorType::MainFolderName;
    requires(std::same_as<decltype(GeneratorType::MainFolderName), std::string>);
    GeneratorType::SubfolderName;
    requires(std::same_as<decltype(GeneratorType::SubfolderName), std::string>);

    GeneratorType::MainFolder;
    requires(std::same_as<decltype(GeneratorType::MainFolder), fs::path>);
    GeneratorType::Subfolder;
    requires(std::same_as<decltype(GeneratorType::Subfolder), fs::path>);
    
    /* Require static functions */
    GeneratorType::Generate();

    GeneratorType::InitPredefinedMembers();
    GeneratorType::InitPredefinedFunctions();
};

class Generator
{
private:
    friend class GeneratorTest;

private:
    static inline fs::path DumperFolder;
    static inline bool bDumpedGObjects = false;
	static inline bool bDumepdEditorOnlyMetadata = false;

public:
    static void InitEngineCore();
    static void InitInternal();

private:
    static bool SetupDumperFolder();

    static bool SetupFolders(std::string& FolderName, fs::path& OutFolder);
    static bool SetupFolders(std::string& FolderName, fs::path& OutFolder, std::string& SubfolderName, fs::path& OutSubFolder);

public:
    template<GeneratorImplementation GeneratorType>
    static void Generate() 
    { 
        if (DumperFolder.empty())
        {
            std::cerr << "[DBG] Gen: SetupDumperFolder...\n" << std::flush;
            if (!SetupDumperFolder())
                return;
            std::cerr << "[DBG] Gen: SetupDumperFolder done\n" << std::flush;

            if (!bDumpedGObjects)
            {
                bDumpedGObjects = true;
                std::cerr << "[DBG] Gen: DumpObjects...\n" << std::flush;
                try { ObjectArray::DumpObjects(DumperFolder); }
                catch (const std::exception& e) { std::cerr << "[WARN] DumpObjects threw: " << e.what() << "\n" << std::flush; }
                catch (...) { std::cerr << "[WARN] DumpObjects threw unknown exception\n" << std::flush; }
                std::cerr << "[DBG] Gen: DumpObjects done\n" << std::flush;

                if (Settings::Internal::bUseFProperty)
                {
                    std::cerr << "[DBG] Gen: DumpObjectsWithProperties...\n" << std::flush;
                    try { ObjectArray::DumpObjectsWithProperties(DumperFolder); }
                    catch (const std::exception& e) { std::cerr << "[WARN] DumpObjectsWithProperties threw: " << e.what() << "\n" << std::flush; }
                    catch (...) { std::cerr << "[WARN] DumpObjectsWithProperties threw unknown\n" << std::flush; }
                    std::cerr << "[DBG] Gen: DumpObjectsWithProperties done\n" << std::flush;
                }
            }

            if (!bDumepdEditorOnlyMetadata)
            {
                bDumepdEditorOnlyMetadata = true;
                std::cerr << "[DBG] Gen: DumpEditorOnlyMetadata...\n" << std::flush;
                try { DumpEditorOnlyMetadata(DumperFolder); }
                catch (const std::exception& e) { std::cerr << "[WARN] DumpEditorOnlyMetadata threw: " << e.what() << "\n" << std::flush; }
                catch (...) { std::cerr << "[WARN] DumpEditorOnlyMetadata threw unknown\n" << std::flush; }
                std::cerr << "[DBG] Gen: DumpEditorOnlyMetadata done\n" << std::flush;
            }
        }

        std::cerr << "[DBG] Gen: SetupFolders...\n" << std::flush;
        if (!SetupFolders(GeneratorType::MainFolderName, GeneratorType::MainFolder, GeneratorType::SubfolderName, GeneratorType::Subfolder))
            return;
        std::cerr << "[DBG] Gen: SetupFolders done\n" << std::flush;

        std::cerr << "[DBG] Gen: InitPredefinedMembers...\n" << std::flush;
        GeneratorType::InitPredefinedMembers();
        std::cerr << "[DBG] Gen: InitPredefinedMembers done\n" << std::flush;

        std::cerr << "[DBG] Gen: InitPredefinedFunctions...\n" << std::flush;
        GeneratorType::InitPredefinedFunctions();
        std::cerr << "[DBG] Gen: InitPredefinedFunctions done\n" << std::flush;

        MemberManager::SetPredefinedMemberLookupPtr(&GeneratorType::PredefinedMembers);

        GeneratorType::Generate();
    };
};
