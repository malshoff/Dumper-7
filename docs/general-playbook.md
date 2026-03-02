# Dumper-7 Playbook: Porting to Complex / Stripped UE Games

This document captures the exact steps, debugging techniques, and fixes required to get Dumper-7 working for **Dragon Ball FighterZ (DBFZ)** — a UE4.17 game with a stripped/packed binary. Use this as a reference when targeting other games that fail the automatic offset-finding heuristics.

---

## Overview

| Item | Value |
|---|---|
| Game | Dragon Ball FighterZ |
| Engine | UE 4.17.2 (custom fork "RED140" by Arc System Works) |
| Binary | `RGame-Win64-Shipping.exe` (packed, imports stripped) |
| Injection | Manual DLL mapping via KMDF kernel driver |
| SDK Output | 586 package files, all generators completed |

### Key Issues Resolved

| Problem | Root Cause | Fix |
|---|---|---|
| GNames not found | Kernel32 imports stripped (no `EnterCriticalSection`/`InitializeSRWLock`) | Hardcode GNames offset from IDA |
| `UEnum::Names = -1` | Auto-finder enums (`ENetRole` etc.) not present in binary | Hardcode `0x48` after fixing TArray |
| `ULevel::Actors = -1` | TArray struct size mismatch | Fix TArray padding |
| `UClass::ImplementedInterfaces = -1` | TArray struct size mismatch | Fix TArray padding |
| ProcessEvent crash | Timing issue with manual DLL mapping | Resolved after TArray fix |
| CppGenerator crash (`0xe06d7363`) | C++ exceptions not caught in manually mapped DLLs | Replace `try/catch` with `std::error_code` overloads |
| **All TArray-dependent offsets wrong** | **Game uses 0x18-byte TArrays** (8-byte allocator pad) | **Add `AllocatorPad[8]` to TArray struct** |

---

## Step-by-Step Playbook

### Phase 1: Initial Attempt — Identify What Fails

1. **Build and inject Dumper-7 with default settings.** Add diagnostic `std::cerr` logging around each init stage in `Generator::InitEngineCore()`:
   ```cpp
   Off::Init();
   std::cerr << "[DIAG] Off::Init() done\n";
   PropertySizes::Init();
   std::cerr << "[DIAG] PropertySizes::Init() done\n";
   // etc.
   ```

2. **Collect the output.** Look for:
   - Offsets showing `0x-1` (auto-finder failed)
   - Exception codes (`0xe06d7363` = C++ exception, `0xc0000005` = access violation)
   - Which generators crash vs. succeed

3. **Categorize failures:**
   - **GNames/GObjects not found** → go to Phase 2
   - **Multiple offsets = `-1`** → go to Phase 3 (TArray mismatch)
   - **Individual offset = `-1`** → go to Phase 4 (specific offset fix)
   - **C++ exceptions in generators** → go to Phase 5 (manual mapping issues)

### Phase 2: Finding GNames/GObjects in IDA

The auto-scan finds GNames by looking for `kernel32.dll` imports (`EnterCriticalSection` or `InitializeSRWLock`). If the binary is packed/stripped, these won't exist.

#### Finding GNames

1. **Search for `FName::StaticInit`** in IDA:
   - Search strings for `"FName::StaticInit"` or `"LogInit"` 
   - Find the function that references this string — it's `FName::Init()`

2. **In `FName::Init()`, look for the pattern:**
   ```
   lea rax, qword_XXXXXXXXX   ; this is the TNameEntryArray global
   ```
   The `qword_` address is your GNames RVA.

3. **Alternative: search for `FNameEntry` access patterns.** Look for functions doing:
   ```
   mov rax, cs:GNames_ptr
   mov rcx, [rax+rcx*8]        ; array index
   ```

4. **Override in `Generator.cpp`:**
   ```cpp
   FName::Init(0x3F6BD08, FName::EOffsetOverrideType::GNames, false);
   ```

#### Finding GObjects

GObjects is typically found automatically. If not, search IDA for `FUObjectArray` patterns:
```
lea rax, qword_XXXXXXXXX   ; FFixedUObjectArray global
```

### Phase 3: Diagnosing TArray Size Mismatch (Critical!)

**This is the most impactful issue.** If multiple TArray-dependent offsets all fail (`UEnum::Names`, `ULevel::Actors`, `UClass::ImplementedInterfaces`), the game likely uses a non-standard TArray layout.

#### Symptoms
- 3+ offsets show `0x-1` simultaneously  
- All rely on TArray scanning
- The game compiles with a custom allocator or custom engine fork

#### Diagnosis: Hex Dump UEnum Objects

Add diagnostic code to `InitEngineCore()` after `Off::Init()`:

```cpp
if (Off::UEnum::Names < 0) {
    for (auto Obj : ObjectArray()) {
        if (!Obj.IsA(EClassCastFlags::Enum)) continue;
        
        std::string name = Obj.GetName();
        const uint8_t* base = reinterpret_cast<const uint8_t*>(Obj.GetAddress());
        std::cerr << "ENUM \"" << name << "\" @ " << (void*)base << "\n";
        
        // Dump raw bytes 0x30..0x70
        for (int row = 0x30; row < 0x70; row += 0x10) {
            char line[128];
            int pos = snprintf(line, sizeof(line), "  +0x%02X: ", row);
            for (int col = 0; col < 0x10; col++)
                pos += snprintf(line + pos, sizeof(line)-pos, "%02X ", base[row+col]);
            std::cerr << line << "\n";
        }
        if (++count > 3) break;
    }
}
```

#### Analysis: What to Look For

**Standard TArray (0x10 bytes):**
```
+0x00: [Data pointer, 8 bytes] [Num, 4 bytes] [Max, 4 bytes]
```

**Extended TArray (0x18 bytes — DBFZ pattern):**
```
+0x00: [Data pointer, 8 bytes] [AllocatorPad, 8 bytes] [Num, 4 bytes] [Max, 4 bytes]
```

To identify the pattern, look at UEnum objects after `UField::Next` (offset 0x28):

```
Standard UE4:         UField(0x30) + FString(0x10) + Names TArray(0x10)
DBFZ (0x18 TArray):   UField(0x30) + FString(0x18) + Names TArray(0x18)
```

**Key indicators:** Find a non-CDO enum and check:
- At 0x30: Look for a heap pointer (FString Data). The Num/Max of FString should match the CppType string length (e.g., `"RED::EArcadeRank"` = 18 chars → Num=18).
- Find where an int32 pair matches expected enum entry counts. If it's at `+0x10` past the pointer instead of `+0x08`, you have the 0x18-byte TArray.

**DBFZ Example:**
```
+0x30: [FString Data ptr]    +0x38: [pad 0]    +0x40: [Num=18, Max=24]  ← CppType "RED::EArcadeRank"
+0x48: [Names Data ptr]      +0x50: [pad 0]    +0x58: [Num=8, Max=8]    ← 8 enum entries
```

#### Fix: Modify `UnrealContainers.h`

In `UC::TArray`:
```cpp
protected:
    ArrayElementType* Data;
    uint8 AllocatorPad[0x8];  // Extended TArray: {Data, Pad, Num, Max} = 0x18
    int32 NumElements;
    int32 MaxElements;
```

Update the default constructor:
```cpp
TArray() : Data(nullptr), AllocatorPad{}, NumElements(0), MaxElements(0) {}
```

Update `static_assert` sizes at the bottom of the file:
```cpp
static_assert(sizeof(TArray<int32>) == 0x18, "TArray has a wrong size!");
static_assert(sizeof(TSet<int32>)   == 0x58, "TSet has a wrong size!");
static_assert(sizeof(TMap<int32, int32>) == 0x58, "TMap has a wrong size!");
```

### Phase 4: Hardcoding Individual Missing Offsets

After fixing TArray, if specific offsets still show `-1`:

#### `UEnum::Names`
The auto-finder searches for `ENetRole`, `ETraceTypeQuery`, `EAlphaBlendOption`, `EUpdateRateShiftBucket`. If none exist in the game:

1. Calculate from UE4 class layout:
   - UField ends at `0x30` (UObject 0x28 + Next 0x8)
   - FString CppType occupies `sizeof(TArray)` bytes (0x10 standard, 0x18 extended)
   - Names starts after CppType

2. Override in `InitEngineCore()` after `Off::Init()`:
   ```cpp
   if (Off::UEnum::Names < 0) {
       Off::UEnum::Names = 0x48; // 0x30 + 0x18 (extended FString)
   }
   ```

#### `ULevel::Actors`
Auto-finder needs a live `ULevel` instance. If none loaded during init:
- This only matters for the **generated SDK at runtime**, not for dump generation itself
- Can be found via IDA: search for `"OwningWorld"` string and find the property registration function
- The function registers `OwningWorld` at a known offset; `Actors` is typically nearby

#### `UClass::ImplementedInterfaces`
Auto-finder searches for `Interface_AssetUserData` in `ActorComponent`. If not found:
- For UE4.17, typical offset is around `0x128`–`0x268` depending on TArray sizes
- With 0x18 TArrays, the offset shifts upward

### Phase 5: Manual Mapping Issues

When injecting via manual DLL mapping (not LoadLibrary), standard C++ exception handling is broken because:
- The C runtime's exception dispatch tables aren't registered
- `try/catch` for C++ exceptions (`0xe06d7363`) silently fails

#### Fix: Replace `try/catch` with SEH or Error Codes

In `Generator::SetupDumperFolder()` and `Generator::SetupFolders()`:
```cpp
// BEFORE (broken in manually mapped DLLs):
try {
    fs::create_directories(path);
} catch (const std::filesystem::filesystem_error& e) {
    // never catches
}

// AFTER (works everywhere):
std::error_code ec;
fs::create_directories(path, ec);
if (ec) {
    std::cerr << "Failed: " << ec.message() << "\n";
}
```

#### Additional Manual Mapping Fix: Thread-Safe Static Init

If you get access violations during CRT init, disable MSVC's thread-safe static initialization:
- Add `/Zc:threadSafeInit-` to compiler flags in the `.vcxproj`
- This prevents the CRT from using TLS internals that fail in manually mapped DLLs

### Phase 6: Verification

After all fixes, verify the dump:

1. **All offsets resolved** (no `0x-1` in log)
2. **All generators complete** (CppGenerator, MappingGenerator, IDAMappingGenerator, DumpspaceGenerator)
3. **Game name/version retrieved** via ProcessEvent
4. **EnumsInfo.json has populated members** (not empty `[]` arrays)
5. **CppSDK has package files** (hundreds for a typical game)

---

## Files Modified (DBFZ)

| File | Change |
|---|---|
| `Dumper/Engine/Public/Unreal/UnrealContainers.h` | Added `AllocatorPad[8]` to TArray, updated static_asserts |
| `Dumper/Generator/Private/Generators/Generator.cpp` | GNames override, UEnum::Names override, diagnostic logging, SEH filesystem fixes |
| `.vcxproj` | Added `/Zc:threadSafeInit-` compiler flag |

---

## IDA Analysis Reference

### Useful Searches

| Target | IDA Search Strategy |
|---|---|
| GNames | String `"FName::StaticInit"` → follow xref → find `lea rax, qword_XXX` |
| ProcessEvent | String `"Script call stack"` or look for virtual function at vtable index 0x3F |
| ULevel properties | String `"OwningWorld"` → find property registration function |
| UEnum layout | Hex dump live UEnum objects from GObjects array (see Phase 3) |

### DBFZ-Specific Offsets Found

```
GObjects:            0x3F75030   (FFixedUObjectArray)
GNames:              0x3F6BD08   (TNameEntryArray, manually found)
ProcessEvent:        0xB95790    (vtable index 0x3F)
GWorld:              0x40A9FF8
UEnum::Names:        0x48        (manually calculated)
ULevel::Actors:      0xD0        (auto-found after TArray fix)
ImplementedInterfaces: 0x268     (auto-found after TArray fix)
TArray size:         0x18        (non-standard, has 8-byte allocator pad)
```

---

## Quick Checklist for New Games

- [ ] Does the initial dump produce any `0x-1` offsets?
- [ ] Are GNames/GObjects found automatically?
- [ ] **Are multiple TArray-dependent offsets failing?** → Check TArray size via hex dump
- [ ] Is the binary packed/stripped? → May need manual IDA analysis for GNames
- [ ] Is injection via manual mapping? → Fix C++ exceptions and thread-safe statics
- [ ] Do enums have populated members in EnumsInfo.json?
- [ ] Do all 4 generators complete without exceptions?
