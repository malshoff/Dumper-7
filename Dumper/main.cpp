#include <Windows.h>
#include <iostream>
#include <chrono>
#include <fstream>

#include "Generators/CppGenerator.h"
#include "Generators/MappingGenerator.h"
#include "Generators/IDAMappingGenerator.h"
#include "Generators/DumpspaceGenerator.h"

#include "Generators/Generator.h"

enum class EFortToastType : uint8
{
        Default                        = 0,
        Subdued                        = 1,
        Impactful                      = 2,
        EFortToastType_MAX             = 3,
};

// ============================================================================
// SEH wrappers — MSVC forbids __try in functions with C++ objects that need
// destructors (error C2712). These thin wrappers isolate the SEH from C++ land.
// ============================================================================
static void DoGetGameInfo()
{
	FString Name;
	FString Version;
	UEClass Kismet = ObjectArray::FindClassFast("KismetSystemLibrary");
	UEFunction GetGameName = Kismet.GetFunction("KismetSystemLibrary", "GetGameName");
	UEFunction GetEngineVersion = Kismet.GetFunction("KismetSystemLibrary", "GetEngineVersion");

	Kismet.ProcessEvent(GetGameName, &Name);
	Kismet.ProcessEvent(GetEngineVersion, &Version);

	Settings::Generator::GameName = Name.ToString();
	Settings::Generator::GameVersion = Version.ToString();
}

static DWORD TryGetGameInfo()
{
	__try
	{
		DoGetGameInfo();
		return 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return GetExceptionCode();
	}
}

typedef void (*GeneratorFunc)();

// C++ try/catch wrapper to capture exception messages
static void RunGeneratorWithCppCatch(GeneratorFunc fn, const char* name)
{
	try
	{
		fn();
	}
	catch (const std::exception& e)
	{
		std::cerr << "[RunDump] *** C++ EXCEPTION in " << name << ": " << e.what() << " ***\n";
	}
	catch (...)
	{
		std::cerr << "[RunDump] *** UNKNOWN C++ EXCEPTION in " << name << " ***\n";
	}
}

// SEH wrapper as fallback for non-C++ exceptions (access violations, etc.)
static DWORD TryRunGenerator(GeneratorFunc fn)
{
	__try
	{
		fn();
		return 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return GetExceptionCode();
	}
}

static DWORD TryRunDump();

// ============================================================================
// RunDump — Runs the entire SDK generation synchronously.
// ============================================================================
static void RunDump()
{
	// --- File-based logging instead of AllocConsole ---
	namespace fs = std::filesystem;
	fs::create_directories("C:\\Dumper-7");
	std::ofstream LogFile("C:\\Dumper-7\\dump.log");
	auto* OldCerrBuf = std::cerr.rdbuf(LogFile.rdbuf());

	std::cerr << "Started Generation [Dumper-7]!\n";

	Settings::Config::Load();

	if (Settings::Config::SleepTimeout > 0)
	{
		std::cerr << "Sleeping for " << Settings::Config::SleepTimeout << "ms...\n";
		Sleep(Settings::Config::SleepTimeout);
	}

	auto DumpStartTime = std::chrono::high_resolution_clock::now();

	std::cerr << "[RunDump] Calling InitEngineCore...\n";
	std::cerr.flush();
	DWORD ecCode = TryRunGenerator([]() { Generator::InitEngineCore(); });
	if (ecCode != 0)
	{
		std::cerr << "[RunDump] *** EXCEPTION in InitEngineCore: 0x" << std::hex << ecCode << std::dec << " ***\n";
		std::cerr << "[RunDump] Cannot continue without engine core. Aborting.\n";
		std::cerr.rdbuf(OldCerrBuf);
		LogFile.close();
		return;
	}
	std::cerr << "[RunDump] InitEngineCore done.\n";

	std::cerr << "[RunDump] Calling InitInternal...\n";
	std::cerr.flush();
	DWORD iiCode = TryRunGenerator([]() { Generator::InitInternal(); });
	if (iiCode != 0)
	{
		std::cerr << "[RunDump] *** EXCEPTION in InitInternal: 0x" << std::hex << iiCode << std::dec << " ***\n";
		std::cerr << "[RunDump] Continuing despite InitInternal failure...\n";
	}
	else
	{
		std::cerr << "[RunDump] InitInternal done.\n";
	}

	// --- ProcessEvent to get game name/version ---
	if (Settings::Generator::GameName.empty() && Settings::Generator::GameVersion.empty())
	{
		std::cerr << "[RunDump] Getting GameName/Version via ProcessEvent...\n";
		std::cerr.flush();

		DWORD code = TryGetGameInfo();
		if (code != 0)
		{
			std::cerr << "[RunDump] *** EXCEPTION in ProcessEvent: 0x" << std::hex << code << std::dec << " ***\n";
			std::cerr << "[RunDump] Continuing without game name/version...\n";
			Settings::Generator::GameName = "Unknown";
			Settings::Generator::GameVersion = "0.0";
		}
	}

	std::cerr << "GameName: " << Settings::Generator::GameName << "\n";
	std::cerr << "GameVersion: " << Settings::Generator::GameVersion << "\n\n";
	std::cerr << "FolderName: " << (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName) << "\n\n";

	// --- Run each generator with individual exception handling ---
	struct GenEntry { GeneratorFunc fn; const char* name; };
	GenEntry generators[] = {
		{ []() { Generator::Generate<CppGenerator>(); },         "CppGenerator" },
		{ []() { Generator::Generate<MappingGenerator>(); },     "MappingGenerator" },
		{ []() { Generator::Generate<IDAMappingGenerator>(); },  "IDAMappingGenerator" },
		{ []() { Generator::Generate<DumpspaceGenerator>(); },   "DumpspaceGenerator" },
	};

	for (auto& g : generators)
	{
		std::cerr << "[RunDump] Running " << g.name << "...\n";
		std::cerr.flush();
		RunGeneratorWithCppCatch(g.fn, g.name);
	}

	auto DumpFinishTime = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double, std::milli> DumpTime = DumpFinishTime - DumpStartTime;
	std::cerr << "\n\nGenerating SDK took (" << DumpTime.count() << "ms)\n\n\n";

	// Restore cerr and close the log file
	std::cerr.rdbuf(OldCerrBuf);
	LogFile.close();
}

// Top-level SEH wrapper (no C++ objects here)
static DWORD TryRunDump()
{
	__try
	{
		RunDump();
		return 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return GetExceptionCode();
	}
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
	{
		DWORD code = TryRunDump();
		if (code != 0)
		{
			// Crash log fallback — cerr redirect is likely dead
			FILE* f = nullptr;
			fopen_s(&f, "C:\\Dumper-7\\crash.log", "w");
			if (f) {
				fprintf(f, "Dumper-7 FATAL EXCEPTION: 0x%08X\n", code);
				fclose(f);
			}
		}
		break;
	}
	}

	return TRUE;
}

