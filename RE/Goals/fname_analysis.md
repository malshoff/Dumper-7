# Goals.exe — Reverse Engineering Notes

## Game Info
- **Binary:** `Goals.exe` (not `GOALS.exe` — that's the launcher)
- **Engine:** Unreal Engine 5.5+
- **Architecture:** x86-64, little-endian
- **Image base:** `0x140000000`

---

## Key Offsets (RVA from image base)

| Symbol | RVA | VA (base 0x140000000) | Signature |
|--------|-----|-----------------------|-----------|
| **FNamePool (GNames)** | `0xAC3E7C0` | `0x14AC3E7C0` | N/A (global variable) |
| **FNamePool::FNamePool (constructor)** | `0x1372300` | `0x141372300` | `48 89 5C 24 ? 48 89 74 24 ? 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC ? 33 C0` |
| **FName::AppendString** | `0x1376990` | `0x141376990` | `48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 80 3D ? ? ? ? ? 48 8B F2 ? ? 48 8B F9` |
| **FNameEntry::AppendNameToString** | `0x1376750` | `0x141376750` | (called by AppendString) |
| **FNamePool init guard** | `0xAC3E559` | `0x14AC3E559` | N/A (byte flag) |
| **GObjects (FChunkedFixedUObjectArray)** | `0xADD28E8` | `0x14ADD28E8` | Found by Dumper-7 auto-detection |
| **FUObjectItemSize** | `0x10` (16 bytes) | — | — |
| **XOR key (NOT FName encryption)** | — | `0x14840BFF0` | `37 38 ED AE F4 F2 17 FA` (repeated 16 bytes) |

---

## FNamePool Layout (UE5.5 Sharded)

This game uses a **sharded FNamePool** rather than the older flat chunk-based layout. Dumper-7's
`NameArray::InitializeNamePool` validation fails because it expects the older structure.

### FName::AppendString Decompilation

```cpp
// sub_141376990 — FName::AppendString(const FName* Name, FString& Out)
__int64 __fastcall FName_AppendString(unsigned int *a1, __int64 a2)
{
  unsigned int CompIdx = *a1;    // Full 32-bit ComparisonIndex
  
  _QWORD* Pool;
  if ( bPoolInitialized )        // byte_14AC3E559
  {
    Pool = &FNamePool;           // stru_14AC3E7C0
  }
  else
  {
    Pool = FNamePool_Constructor(&FNamePool);
    bPoolInitialized = 1;
  }
  
  // Block lookup: HIWORD = block index, LOWORD = slot offset (stride 2)
  // Entry address = Pool[HIWORD(CompIdx) + 2] + LOWORD(CompIdx) * 2
  result = FNameEntry_AppendNameToString(
    Pool[HIWORD(CompIdx) + 2] + 2 * (unsigned __int16)CompIdx,
    a2
  );
  
  // Append "_N" suffix if Number != 0
  if ( a1[1] )    // FName::Number at offset +4
  {
    FString_AppendChar(a2, '_');
    return FString_AppendInt(a2, a1[1] - 1);
  }
  return result;
}
```

### Pool Structure

```
FNamePool (at RVA 0xAC3E7C0):
  +0x00: [unknown header data]
  +0x10: QWORD Block[0] pointer    <-- Pool[2] = first block
  +0x18: QWORD Block[1] pointer    <-- Pool[3] = second block
  +0x20: ... more blocks ...
```

Each block is an array of FNameEntry objects accessed with stride=2 bytes.

### FNameEntry Header

```
uint16 Header:
  bit 0:       bIsWide (0=ANSI, 1=UTF-16)
  bits [6..15]: NameLength (Header >> 6)
  
  String data follows immediately after header (offset +2 from entry start)
```

### Dumper-7 Override

Since the sharded pool layout breaks auto-detection, use:
```cpp
FName::Init((int32)0x1376990, FName::EOffsetOverrideType::AppendString);
Off::InSDK::NameArray::GNames = (int32)0xAC3E7C0;
Settings::Internal::bUseNamePool = true;
```

---

## Investigation: XOR Sequence at sub_1413FD6E0 (NOT FName Encryption)

### Context

During investigation, a function at `0x1413FD6E0` was found containing SSE XOR/shuffle/rotate
operations on `[rdi+0x40]`. Initial hypothesis: **FName encryption**. This was later **disproved**.

### The Assembly

```asm
vmovq   xmm1, qword ptr [rdi+40h]            ; Load 8 bytes from object+0x40
vpxor   xmm2, xmm1, cs:xmmword_14840BFF0     ; XOR with key
vpshuflw xmm3, xmm2, 93h                      ; Shuffle low words
vpsrlw  xmm1, xmm3, 4                         ; Shift right 4
vpsllw  xmm0, xmm3, 0Ch                       ; Shift left 12
vpxor   xmm1, xmm0, xmm1                     ; = rotate right 4 per word
vmovq   rdx, xmm1                             ; Result → rdx
```

**XOR Key:** `0xFA17F2F4AEED3837` (at `xmmword_14840BFF0`: `37 38 ED AE F4 F2 17 FA`)

### Why It's NOT FName Encryption

The result `rdx` is used as a **POINTER**, not a 4-byte index:
```cpp
// rdx is used to access [rdx+112] and [rdx+104] — pointer arithmetic
if ( (int)v10 > *(_DWORD *)(rdx + 112) || ... *(_QWORD *)(*(_QWORD *)(rdx + 104) + 8 * v10) != v9 )
```

This is **pointer obfuscation** for owner/hierarchy validation in `FField::GetFName`, NOT
FName ComparisonIndex encryption. The field at `[obj+0x40]` in FField is an obfuscated pointer,
not the FName member.

### Decryption Algorithm (For Reference)

If needed in the future for the actual obfuscated pointer field:
```cpp
uint64_t decrypt_pointer_field(uint64_t enc) {
    enc ^= 0xFA17F2F4AEED3837ULL;
    
    uint16_t w[4] = {
        (uint16_t)(enc), (uint16_t)(enc >> 16),
        (uint16_t)(enc >> 32), (uint16_t)(enc >> 48)
    };
    // vpshuflw 0x93: [w0,w1,w2,w3] → [w3,w0,w1,w2]
    uint16_t s[4] = { w[3], w[0], w[1], w[2] };
    
    // Rotate each word right by 4
    for (int i = 0; i < 4; i++)
        s[i] = (s[i] << 12) ^ (s[i] >> 4);
    
    return (uint64_t)s[0] | ((uint64_t)s[1] << 16) |
           ((uint64_t)s[2] << 32) | ((uint64_t)s[3] << 48);
}
```

---

## How Offsets Were Found

### FNamePool Global (0xAC3E7C0)

1. Searched strings for `"None"` — not directly useful (IL2CPP-style search won't work)
2. Found `FNamePool::FNamePool` constructor (`sub_141372300`) which stores hardcoded FNames
   (`"None"`, `"ByteProperty"`, `"IntProperty"`, etc.)
3. The constructor is called with `&unk_14AC3E7C0` — this is the FNamePool global
4. Confirmed by AppendString: `v5 = &stru_14AC3E7C0;`

### FName::AppendString (0x1376990)

1. Dumper-7's auto-detection found it via the `"ForwardShadingQuality_"` string reference
2. Confirmed by manual analysis: function reads CompIdx, indexes into FNamePool, calls
   `FNameEntry::AppendNameToString`
3. Cross-referenced: called from `sub_14130D5D0` (decompilation shows standard UE5 FName
   string conversion pattern)

### GObjects (0xADD28E8)

Found automatically by Dumper-7's `ObjectArray::Init()` pattern scan.

---

## Current Issue: Access Violation in Off::Init()

### Symptoms

- AppendString is correctly overridden (log shows `Manual-Override: FName::AppendString --> Offset 0x1376990`)
- GObjects found successfully
- Crash `0xc0000005` occurs during `Off::Init()` after all overrides are applied
- Crash occurs regardless of FName configuration (auto-detect, manual override, with/without
  decryption wrapper)

### Likely Causes (Under Investigation)

1. **FString memory allocator mismatch** — `FFreableString` uses `malloc`, but the game's
   `TArray::ResizeGrow` (`sub_1411A29E0`) calls UE's `FMemory::Realloc` (`sub_1411DC370`).
   If the buffer needs reallocation, UE's allocator may crash on a non-UE pointer. However,
   pre-allocated 1024 elements should prevent any reallocation for typical FName lengths.

2. **FNamePool not initialized at injection time** — The pool guard byte (`byte_14AC3E559`)
   may be 0, causing AppendString to call the constructor. If the constructor has dependencies
   not yet initialized, it could crash.

3. **UObject layout detection failure** — `Off::Init()` uses pattern matching to find UObject
   field offsets. If the game uses non-standard UObject layout, reading wrong offsets could
   produce invalid FName pointers passed to AppendString.
   
4. **Off::Init() crash unrelated to FName** — Some offset finders in `Off::Init()` may access
   invalid memory during pattern matching, unrelated to FName resolution.

### Diagnostic Added

A `__try/__except` block tests `AppendString(CompIdx=0)` before `Off::Init()`. If this
crashes, the issue is with AppendString itself. If it succeeds, the crash is in the offset
finders.

---

## FString Memory Layout (Verified)

```
TArray<wchar_t> / FString:
  +0x00: wchar_t* Data       (8 bytes, pointer to buffer)
  +0x08: int32 NumElements   (4 bytes)
  +0x0C: int32 MaxElements   (4 bytes)
  Total: 16 bytes
```

This matches both Dumper-7's `FFreableString` and the game's internal FString usage
(confirmed by `sub_1411C56B0` accessing `a1+0`, `a1+8`, `a1+12`).

---

## Related Functions

| VA | Name | Purpose |
|----|------|---------|
| `0x141372300` | `FNamePool::FNamePool` | Pool constructor, initializes hardcoded names |
| `0x141376990` | `FName::AppendString` | Resolves FName → string, appends to FString |
| `0x141376750` | `FNameEntry::AppendNameToString` | Reads entry header + copies string data |
| `0x1411C56B0` | `FString::Append` | Appends wchar_t buffer to FString |
| `0x1411A29E0` | `TArray::ResizeGrow` | Ensures capacity, may call FMemory::Realloc |
| `0x1411DC370` | `FMemory::Realloc` (inner) | UE memory reallocation |
| `0x14123ACB0` | `FMemory::QuantizeSize` | Returns aligned allocation size |
| `0x1411DBDA0` | `FString::Init` | Initializes FString with capacity |
| `0x1413FD6E0` | `FField::FindPropertyByName?` | Uses XOR-obfuscated pointer (NOT FName encryption) |
| `0x14145D520` | `UObject::GetOuter?` | Called before XOR sequence |
| `0x14130D5D0` | UE internal (large func) | Calls AppendString with standard FName args |
