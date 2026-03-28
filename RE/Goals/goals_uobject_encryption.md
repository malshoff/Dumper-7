# Goals.exe — UObject Encryption & SDK Generation Bypass

## Overview

Goals.exe (UE5.5+) implements a custom **SIMD-based field encryption** scheme on `UObject` and
an **enlarged object layout** for both `UObject` and `FField` that prevents standard SDK dumpers
from resolving field offsets. This document details the complete bypass chain implemented in
Dumper-7 to produce a working SDK dump.

**Result:** Full SDK generated in ~145 seconds covering all 32,758 objects.

---

## Table of Contents

1. [GObjects Obfuscation](#1-gobjects-obfuscation)
2. [UObject Field Encryption](#2-uobject-field-encryption)
3. [UObject Memory Layout](#3-uobject-memory-layout)
4. [FField Memory Layout](#4-ffield-memory-layout)
5. [SIMD Decryption Algorithm](#5-simd-decryption-algorithm)
6. [Bypass Implementation](#6-bypass-implementation)
7. [Dumper-7 Modifications](#7-dumper-7-modifications)
8. [Discovered Offsets](#8-discovered-offsets)
9. [Key IDA Functions](#9-key-ida-functions)
10. [Lessons Learned](#10-lessons-learned)

---

## 1. GObjects Obfuscation

### Custom Indexer

The game replaces the standard `GObjects` array indexing with a custom function at
**RVA `0x1416860`** (`sub_141416860`). This function:

1. Uses **FNV-1a hashing** to compute an index
2. Looks up objects through an encrypted/indirect table
3. Returns a fully resolved `UObject*` pointer

### Override Strategy

```cpp
// Redirect Dumper-7 to the real GObjects base 
// (offset 0xADD28E8 is the FChunkedFixedUObjectArray found by auto-detection)
ObjectArray::SetGObjectsAddress(0x00007FF61312D390);  // Runtime VA
ObjectArray::SetNumOverride(32758);
ObjectArray::SetByIndexHook(sub_141416860);            // Game's custom indexer
```

The `ByIndex` hook calls the game's own indexer to resolve `UObject*` pointers, bypassing
whatever obfuscation exists in the array layout.

---

## 2. UObject Field Encryption

### Discovery Process

**Step 1:** Initial hexdump of Obj[0] showed constant encrypted padding at `+0x10` through `+0x3F`:
```
+0x10: 42 62 86 A6 C2 E2 0E 2E  42 62 86 A6 C2 E2 1E 3E
+0x20: 42 62 86 A6 C2 E2 0E 2E  42 62 86 A6 C2 E2 3E 5E
+0x30: 42 A2 86 E6 C2 22 0E 6E  00 00 00 00 00 00 00 00
```
The repeating byte pattern `42 62 86 A6 C2 E2 ...` across objects is a dead giveaway
for encrypted constant values (likely encrypted NULLs/zeros).

**Step 2:** Located the game's `GetFullName` function (`sub_1415A4A80` / `sub_14159C910`)
which reads from `+0x40`, applies SIMD decryption, and uses the result as `ClassPrivate`.

**Step 3:** Identified that `NamePrivate` at `+0x50` and `OuterPrivate` at `+0x58` are
**unencrypted** — stored as raw values.

**Step 4:** Confirmed ClassPrivate at `+0x40` is encrypted with the SIMD algorithm
(XOR → shuffle → rotate). Decrypting it yields valid `UClass*` pointers.

### Fields Affected

| Field | Real Offset | Encryption | Standard UE5 Offset |
|-------|------------|------------|---------------------|
| `ObjectFlags` | `+0x08` | None | `+0x08` |
| `InternalIndex` | `+0x0C` | None | `+0x0C` |
| `ClassPrivate` | `+0x40` | **SIMD encrypted** | `+0x10` |
| `NamePrivate` | `+0x50` | None (raw FName) | `+0x18` |
| `OuterPrivate` | `+0x58` | None (raw pointer) | `+0x20` |

### Integrity Protection

**CRITICAL:** Writing decrypted data to `+0x18` or `+0x20` causes the game to crash
after ~15 seconds. The game has periodic integrity checks on the `+0x10`–`+0x3F` region
(or the game re-reads those bytes and double-decrypts).

**Safe:** Writing the decrypted `ClassPrivate` to `+0x10` does NOT trigger the integrity
check. This offset is within the encrypted padding zone but appears not to be validated.

---

## 3. UObject Memory Layout

### Standard UE5 Layout (0x28 bytes)

```
+0x00: VTable*              (8 bytes)
+0x08: EObjectFlags         (4 bytes)
+0x0C: InternalIndex        (4 bytes)
+0x10: ClassPrivate*        (8 bytes)
+0x18: NamePrivate (FName)  (8 bytes)
+0x20: OuterPrivate*        (8 bytes)
Total: 0x28 bytes
```

### Goals.exe Layout (~0x60 bytes)

```
+0x00: VTable*              (8 bytes)
+0x08: EObjectFlags         (4 bytes)
+0x0C: InternalIndex        (4 bytes)
+0x10: [encrypted padding]  (48 bytes — contains encrypted Class/Outer/Name copies?)
+0x40: ClassPrivate*        (8 bytes, SIMD encrypted)
+0x48: [unknown]            (8 bytes, encrypted — decrypts to non-pointer)
+0x50: NamePrivate (FName)  (8 bytes, unencrypted)
+0x58: OuterPrivate*        (8 bytes, unencrypted)
Total: ~0x60 bytes
```

The enlarged UObject (0x60 vs 0x28) pushes all derived struct offsets higher by ~0x38 bytes,
causing `UStruct`, `UClass`, etc. fields to appear at much higher offsets than standard.

---

## 4. FField Memory Layout

### Discovery Process

After all UObject offsets resolved, the `FField::Name` offset finder failed because:
1. The auto-finder's search range was too narrow (`< 0x40`)
2. Even with increased range (`< 0x80`), the `GetName()` code path through Dumper-7's
   `NameArray` doesn't match the raw `AppendString` behavior

**Diagnostic hex dump** of `FGuid`'s first `ChildProperty` FField revealed:

```
+00: 80 C0 83 10 F6 7F 00 00  ← VTable*
+08: 00 00 00 00 00 00 00 00  ← zeros (padding)
+10: 00 00 00 00 00 00 00 00  ← zeros
+18: 00 00 00 00 00 00 00 00  ← zeros
+20: 00 00 00 00 00 00 00 00  ← zeros
+28: 00 00 00 00 00 00 00 00  ← zeros
+30: A0 C5 12 13 F6 7F 00 00  ← FFieldClass* (ClassPrivate)
+38: 41 91 41 94 F4 7F 00 00  ← Owner (tagged pointer, bit 0 = isUObject flag)
+40: 80 3A 69 E7 42 01 00 00  ← FField* Next
+48: 2E 1B 00 00 00 00 00 00  ← FName (CompIdx=6958 → "A", Number=0)
+50: 45 00 00 00 00 00 00 00  ← FlagsPrivate (0x45)
+58: 01 00 00 00 04 00 00 00  ← Property-specific data starts
```

**FName probe confirmation:** Calling `AppendString(CompIdx=6958)` returned `"A"` — the
expected name for FGuid's first field.

### Confirmed FField Layout

| Field | Offset | Standard UE5 | Notes |
|-------|--------|-------------|-------|
| VTable | `+0x00` | `+0x00` | Same |
| Padding | `+0x08` | — | 40 bytes of zeros |
| ClassPrivate | `+0x30` | `+0x08` | FFieldClass* (unencrypted) |
| Owner | `+0x38` | `+0x10` | Tagged pointer (bit 0 = type flag) |
| Next | `+0x40` | `+0x20` | FField* (unencrypted) |
| NamePrivate | `+0x48` | `+0x28` | FName (unencrypted) |
| FlagsPrivate | `+0x50` | `+0x30` | uint32 flags |

---

## 5. SIMD Decryption Algorithm

### Assembly (from sub_14159C910)

```asm
vmovq   xmm1, qword ptr [rdi+40h]        ; Load encrypted 8 bytes
vpxor   xmm2, xmm1, cs:xmmword_14840BFF0 ; XOR with key
vpshuflw xmm3, xmm2, 93h                  ; Shuffle low words
vpsrlw  xmm1, xmm3, 4                     ; Shift right 4
vpsllw  xmm0, xmm3, 0Ch                   ; Shift left 12
vpxor   xmm1, xmm0, xmm1                 ; Rotate right 4 per word
vmovq   rdx, xmm1                         ; Result
```

### XOR Key

Located at `xmmword_14840BFF0`:
```
37 38 ED AE F4 F2 17 FA  (repeated for 128-bit)
```
As uint64: `0xFA17F2F4AEED3837`

### C++ Implementation

```cpp
uint64_t simd_decrypt(uint64_t encrypted) {
    // Step 1: XOR with key
    const uint64_t key = 0xFA17F2F4AEED3837ULL;
    uint64_t xored = encrypted ^ key;
    
    // Step 2: VPSHUFLW with immediate 0x93
    // 0x93 = 10_01_00_11 → [w3, w0, w1, w2]
    uint16_t words[4];
    memcpy(words, &xored, 8);
    uint16_t shuffled[4] = {
        words[(0x93 >> 0) & 3],  // words[3]
        words[(0x93 >> 2) & 3],  // words[0] 
        words[(0x93 >> 4) & 3],  // words[1]
        words[(0x93 >> 6) & 3],  // words[2]
    };
    
    // Step 3: Rotate each word right by 4 bits
    uint16_t result[4];
    for (int i = 0; i < 4; i++)
        result[i] = (shuffled[i] >> 4) ^ (shuffled[i] << 12);
    
    uint64_t out;
    memcpy(&out, result, 8);
    return out;
}
```

### Algorithm Summary

```
encrypted → XOR(key) → VPSHUFLW(0x93) → ROR16(4) → decrypted
```

Only `ClassPrivate` at UObject `+0x40` uses this encryption. `NamePrivate` and `OuterPrivate`
are stored unencrypted.

---

## 6. Bypass Implementation

### Strategy Overview

```
┌─────────────────────────────────────────────────────┐
│  1. Hook GObjects ByIndex → game's custom indexer   │
│  2. Decrypt ClassPrivate (+0x40 → +0x10) per object │
│  3. Let finders discover Name=+0x50, Outer=+0x58    │
│  4. Override FField::Name → 0x48 (hardcoded)        │
│  5. Increase all search range caps for enlarged obj  │
│  6. Guard against crashes with SEH wrappers          │
└─────────────────────────────────────────────────────┘
```

### Why NOT Patch +0x18/+0x20

Initial approach: copy NamePrivate and OuterPrivate to standard offsets (+0x18, +0x20).
**This caused game crash after ~15 seconds** — the game's integrity system validates
the `+0x10`–`+0x3F` region periodically.

**Solution:** Only write to `+0x10` (ClassPrivate). Leave Name and Outer at their real
offsets. Dumper-7's offset finders naturally discover `+0x50` and `+0x58` when they
scan for valid FName/pointer patterns, since the encrypted padding at `+0x18`/`+0x20`
fails validation (huge ComparisonIndex values).

### Why FField::Name Auto-Detection Fails

The `FindFFieldNameOffset` function:
1. Iterates offsets from `FField::Owner` to max
2. At each offset, calls `UEFField::GetName()` → `NameArray::GetName(CompIdx)`
3. Checks if the name is "A"/"D" (Guid) or "X"/"Z" (Vector)

Dumper-7's `NameArray::GetName()` goes through a different code path than raw
`AppendString` — it accesses the `FNamePool` via Dumper-7's internal `NameArray`
wrapper, which may not handle the game's sharded pool layout correctly. Meanwhile,
calling `AppendString` directly works perfectly.

**Fix:** Hardcode `FField::Name = 0x48` when auto-detection fails.

---

## 7. Dumper-7 Modifications

### Files Modified

#### `Generator.cpp` — InitEngineCore
- **GObjects override:** Set base address, count (32758), and ByIndex hook
- **FName::AppendString override:** RVA `0x1376990`
- **ClassPrivate decryption:** SIMD decrypt from `+0x40`, write to `+0x10`
- **FField diagnostic:** Hexdump + FName probe when FField::Name fails
- **SEH guards:** All post-Init calls wrapped in SEH handlers

#### `Offsets.cpp` — Off::Init
- **FField::Name fallback:** `if (OffsetNotFound) → Off::FField::Name = 0x48`

#### `OffsetFinder.cpp` — Search Ranges
All search caps increased for the enlarged 0x60-byte UObject:

| Finder | Old Max | New Max | Reason |
|--------|---------|---------|--------|
| `FindUObjectOuterOffset` | `0x50` | `0x80` | Outer at 0x58 |
| `FindUFieldNextOffset` | `0x60` | `0xA0` | UField::Next pushed past 0x60 |
| `FindChildPropertiesOffset` | `0x80` | `0x120` | Search starts at 0x88 (Children+8) |
| `FindFFieldNextOffset` | `0x48` | `0x80` | FField::Next at 0x40 |
| `FindFFieldNameOffset` loop | `0x40` | `0x80` | FField::Name at 0x48 |
| `FindNameOffsetForSomeClass` | `0x40` | `0x80` | Generic name search |

#### `ObjectArray.h` — Public Setters
Added `SetGObjectsAddress`, `SetByIndexHook`, `SetNumOverride`, `SetFUObjectItemSize`
for external override of the encrypted GObjects system.

---

## 8. Discovered Offsets

### UObject Hierarchy

```
Off::UObject::Flags:              0x08
Off::UObject::Index:              0x0C
Off::UObject::Class:              0x10    (patched: decrypted from +0x40)
Off::UObject::Name:               0x50    (real location, auto-discovered)
Off::UObject::Outer:              0x58    (real location, auto-discovered)

Off::UStruct::StructBaseChain:    0x68
Off::UStruct::SuperStruct:        0x78
Off::UStruct::Children:           0x80
Off::UStruct::ChildProperties:    0x88
Off::UStruct::Size:               0x90
Off::UStruct::MinAlignment:       0x94

Off::UField::Next:                0x60

Off::UClass::CastFlags:           0x110
Off::UClass::ClassDefaultObject:  0x148
Off::UClass::ImplementedInterfaces: 0x218

Off::UEnum::Names:                0x80

Off::UFunction::FunctionFlags:    0xE8
Off::UFunction::ExecFunction:     0x110
```

### FField / FProperty

```
Off::FField::Class:               0x30
Off::FField::Next:                0x40
Off::FField::Name:                0x48    (manual override)
Off::FField::Flags:               0x50    (computed: Name + FNameSize)

Off::FFieldClass::CastFlags:      0x18

Off::Property::ArrayDim:          0x58
Off::Property::ElementSize:       0x5C
Off::Property::PropertyFlags:     0x60
Off::Property::Offset_Internal:   0x6C

UPropertySize:                    0x98
Off::BoolProperty::Base:          0x98
Off::ObjectProperty::PropertyClass: 0x98
Off::ByteProperty::Enum:          0x98
Off::StructProperty::Struct:      0x98
Off::EnumProperty::Base:          0x98
Off::DelegateProperty::SignatureFunction: 0x98
Off::ArrayProperty::Inner:        0xA0
Off::SetProperty::ElementProp:    0x98
Off::MapProperty::Base:           0x98
```

### SDK Generation

```
Off::InSDK::ObjArray::FUObjectItemSize: 0x10
Off::InSDK::UDataTable::RowMap:   0x68
PE-Offset:                        0x157CCF0
PE-Index:                         0x53
```

---

## 9. Key IDA Functions

| RVA | Name | Purpose |
|-----|------|---------|
| `0x1416860` | `GObjects::GetByIndex` | Custom FNV-1a indexed object resolver |
| `0x1376990` | `FName::AppendString` | Resolves FName → string via sharded pool |
| `0x1376750` | `FNameEntry::AppendNameToString` | Reads entry header + copies string |
| `0x1372300` | `FNamePool::FNamePool` | Pool constructor with hardcoded names |
| `0x159C910` | `SIMD_DecryptClassPtr` | XOR+shuffle+rotate decryption for ClassPrivate |
| `0x15A4A80` | `UObject::GetFullName` | Reveals encrypted field layout |
| `0x13FD6E0` | `FField::FindPropertyByName?` | Uses same SIMD key for pointer validation |
| `0x157CCF0` | `ProcessEvent` | Virtual function for blueprint event dispatch |

### Key Global Variables

| RVA | Name | Type |
|-----|------|------|
| `0xAC3E7C0` | `FNamePool` | Sharded name pool (stru_14AC3E7C0) |
| `0xAC3E559` | `bPoolInitialized` | FNamePool init guard flag |
| `0xADD28E8` | `GObjects` | FChunkedFixedUObjectArray |
| `0x840BFF0` | `XOR_Key` | `xmmword_14840BFF0` = `37 38 ED AE F4 F2 17 FA` |

---

## 10. Lessons Learned

### Anti-Tamper Observations

- The game validates UObject memory at `+0x18` and `+0x20` periodically (~15 second interval)
- Writing to `+0x10` (within the encrypted padding zone) is NOT detected
- The only safe approach: decrypt ClassPrivate to an unused offset, leave Name/Outer at
  their real locations

### Search Range Discipline

Dumper-7's offset finders use hardcoded max search ranges (0x40, 0x50, 0x60, 0x80) based
on standard UE object sizes. Games with enlarged objects (custom padding, encryption fields)
push real offsets past these caps. **Always increase caps proportionally to the object size
increase.**

### FField::Name Auto-Detection Limitation

Dumper-7's `FindFFieldNameOffset` relies on `NameArray::GetName()` matching the engine's
`AppendString` output. For games with sharded FNamePools or non-standard pool layouts,
the internal NameArray may not correctly resolve all ComparisonIndices, even though raw
`AppendString` works. **Hexdump + raw probe is the reliable diagnostic.**

### Methodology

1. Start with hexdump of object memory to identify patterns
2. Find the game's `GetFullName`/`GetName` to reveal which offsets are actually used
3. Identify encryption by looking for SIMD instructions (vpxor, vpshuflw, vpsrlw/vpsllw)
4. Test with raw `AppendString` calls before trusting Dumper-7's internal name resolution
5. Only patch the minimum memory necessary — test for integrity checks
6. When auto-detection fails, use hexdump probes and hardcode the discovered values
