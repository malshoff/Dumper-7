#include <Windows.h>
#include <iostream>
#include <chrono>
#include <fstream>
#include <format>

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

/* Volatile globals tracking the last object processed inside FindObjectFast —
   defined in ObjectArray.cpp and printed here if a crash occurs. */
extern volatile int32_t  g_FindObjectFast_LastIdx;
extern volatile uint64_t g_FindObjectFast_LastAddr;
extern volatile int32_t  g_FindObjectFast_LastCompIdx;
extern volatile int32_t  g_FindObjectFast_Total;

/* Volatile package name tracker — updated before each package is generated.
   Defined in CppGenerator.cpp, extern'd here for the crash handler. */
extern volatile char g_CppGen_CurrentPackage[256];

/* Volatile enum tracker for MappingGenerator crash reporting. */
extern volatile int32  g_MapGen_LastEnumIdx;
extern volatile uint64 g_MapGen_LastEnumAddr;
extern volatile int32  g_MapGen_EnumCount;

/* Last C++ exception message captured before SEH rethrow. */
static char g_CppGen_ExceptionMsg[512] = { '\0' };

/* Records exception message, then rethrows so the SEH wrapper can catch it.
   Must NOT be inlined so __try in SafeGenerateCpp has no C++ locals. */
__declspec(noinline) static void TryGenerateCpp()
{
	try
	{
		Generator::Generate<CppGenerator>();
	}
	catch (const std::exception& e)
	{
		/* Print directly — don't rely on buffer surviving the rethrow */
		std::cerr << "[TryGenerateCpp] std::exception caught: " << e.what() << "\n" << std::flush;
		strncpy_s(g_CppGen_ExceptionMsg, e.what(), sizeof(g_CppGen_ExceptionMsg) - 1);
		throw;
	}
	catch (...)
	{
		std::cerr << "[TryGenerateCpp] unknown exception caught (not std::exception)\n" << std::flush;
		strncpy_s(g_CppGen_ExceptionMsg, "(unknown C++ exception type)", sizeof(g_CppGen_ExceptionMsg) - 1);
		throw;
	}
}

/* __try cannot be used in a function with C++ object unwinding (C2712).
   These thin wrappers have no local C++ objects so SEH is allowed. */
static DWORD SafeInitEngineCore()
{
	__try
	{
		Generator::InitEngineCore();
		return 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return GetExceptionCode();
	}
}

static DWORD SafeInitInternal()
{
	__try
	{
		Generator::InitInternal();
		return 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return GetExceptionCode();
	}
}

static DWORD SafeGenerateCpp()
{
	__try
	{
		TryGenerateCpp();
		return 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return GetExceptionCode();
	}
}

__declspec(noinline) static void TryGenerateMapping()
{
	try
	{
		Generator::Generate<MappingGenerator>();
	}
	catch (const std::exception& e)
	{
		std::cerr << "[TryGenerateMapping] std::exception: " << e.what() << "\n" << std::flush;
		strncpy_s(g_CppGen_ExceptionMsg, e.what(), sizeof(g_CppGen_ExceptionMsg) - 1);
		throw;
	}
	catch (...)
	{
		std::cerr << "[TryGenerateMapping] unknown exception\n" << std::flush;
		strncpy_s(g_CppGen_ExceptionMsg, "(unknown)", sizeof(g_CppGen_ExceptionMsg) - 1);
		throw;
	}
}

static DWORD SafeGenerateMapping()
{
	__try
	{
		TryGenerateMapping();
		return 0;
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		return GetExceptionCode();
	}
}

DWORD MainThread(HMODULE Module)
{
	AllocConsole();
	FILE* Dummy;
	freopen_s(&Dummy, "CONOUT$", "w", stderr);
	freopen_s(&Dummy, "CONIN$", "r", stdin);

	std::cerr << "Started Generation [Dumper-7]!\n";

	Settings::Config::Load();

	if (Settings::Config::SleepTimeout > 0)
	{
		std::cerr << "Sleeping for " << Settings::Config::SleepTimeout << "ms...\n";
		Sleep(Settings::Config::SleepTimeout);
	}

	auto DumpStartTime = std::chrono::high_resolution_clock::now();

	if (DWORD ExCode = SafeInitEngineCore())
	{
		std::cerr << std::format("\n[CRASH] InitEngineCore SEH exception code: 0x{:X}\n", ExCode) << std::flush;
		std::cerr << std::format("[CRASH] FindObjectFast last state: obj[{}]/{} addr=0x{:X} CompIdx={}\n",
			(int32_t)g_FindObjectFast_LastIdx,
			(int32_t)g_FindObjectFast_Total,
			(uint64_t)g_FindObjectFast_LastAddr,
			(int32_t)g_FindObjectFast_LastCompIdx) << std::flush;
		std::cerr << "Check the last [DBG] line above to find which step crashed.\nPress F6 to exit.\n" << std::flush;
		while (true) { if (GetAsyncKeyState(VK_F6) & 1) { FreeConsole(); FreeLibraryAndExitThread(Module, 0); } Sleep(100); }
		return 0;
	}

	if (DWORD ExCode = SafeInitInternal())
	{
		std::cerr << std::format("\n[CRASH] InitInternal SEH exception code: 0x{:X}\n", ExCode) << std::flush;
		std::cerr << "Press F6 to exit.\n" << std::flush;
		while (true) { if (GetAsyncKeyState(VK_F6) & 1) { FreeConsole(); FreeLibraryAndExitThread(Module, 0); } Sleep(100); }
		return 0;
	}

	if (Settings::Generator::GameName.empty() && Settings::Generator::GameVersion.empty())
	{
		// Only Possible in Main()
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

	std::cerr << "GameName: " << Settings::Generator::GameName << "\n";
	std::cerr << "GameVersion: " << Settings::Generator::GameVersion << "\n\n";

	std::cerr << "FolderName: " << (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName) << "\n\n";

	std::cerr << "[DBG] Generate<CppGenerator>...\n" << std::flush;
	if (DWORD ExCode = SafeGenerateCpp())
	{
		std::cerr << std::format("\n[CRASH] Generate<CppGenerator> SEH exception code: 0x{:X}\n", ExCode) << std::flush;
		std::cerr << std::format("[CRASH] Last package: {}\n", (const char*)g_CppGen_CurrentPackage) << std::flush;
		if (g_CppGen_ExceptionMsg[0])
			std::cerr << std::format("[CRASH] Exception message: {}\n", g_CppGen_ExceptionMsg) << std::flush;
		std::cerr << "Check last [DBG] CppGen: line above.\nPress F6 to exit.\n" << std::flush;
		while (true) { if (GetAsyncKeyState(VK_F6) & 1) { FreeConsole(); FreeLibraryAndExitThread(Module, 0); } Sleep(100); }
		return 0;
	}
	std::cerr << "[DBG] Generate<CppGenerator> done\n" << std::flush;

	std::cerr << "[DBG] Generate<MappingGenerator>...\n" << std::flush;
	if (DWORD ExCode = SafeGenerateMapping())
	{
		std::cerr << std::format("\n[CRASH] Generate<MappingGenerator> SEH code: 0x{:X}\n", ExCode) << std::flush;
		if (g_CppGen_ExceptionMsg[0])
			std::cerr << std::format("[CRASH] Exception: {}\n", g_CppGen_ExceptionMsg) << std::flush;
		std::cerr << std::format("[CRASH] MappingGen last enum: idx={} addr=0x{:X} total_done={}\n",
			(int32_t)g_MapGen_LastEnumIdx, (uint64_t)g_MapGen_LastEnumAddr, (int32_t)g_MapGen_EnumCount) << std::flush;
		std::cerr << "Press F6 to exit.\n" << std::flush;
		while (true) { if (GetAsyncKeyState(VK_F6) & 1) { FreeConsole(); FreeLibraryAndExitThread(Module, 0); } Sleep(100); }
		return 0;
	}
	std::cerr << "[DBG] Generate<MappingGenerator> done\n" << std::flush;

	std::cerr << "[DBG] Generate<IDAMappingGenerator>...\n" << std::flush;
	Generator::Generate<IDAMappingGenerator>();
	std::cerr << "[DBG] Generate<IDAMappingGenerator> done\n" << std::flush;

	std::cerr << "[DBG] Generate<DumpspaceGenerator>...\n" << std::flush;
	Generator::Generate<DumpspaceGenerator>();
	std::cerr << "[DBG] Generate<DumpspaceGenerator> done\n" << std::flush;

	auto DumpFinishTime = std::chrono::high_resolution_clock::now();

	std::chrono::duration<double, std::milli> DumpTime = DumpFinishTime - DumpStartTime;

	std::cerr << "\n\nGenerating SDK took (" << DumpTime.count() << "ms)\n\n\n";

	while (true)
	{
		if (GetAsyncKeyState(VK_F6) & 1)
		{
			fclose(stderr);
			if (Dummy) fclose(Dummy);
			FreeConsole();

			FreeLibraryAndExitThread(Module, 0);
		}

		Sleep(100);
	}

	return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		CreateThread(0, 0, (LPTHREAD_START_ROUTINE)MainThread, hModule, 0, 0);
		break;
	}

	return TRUE;
}
