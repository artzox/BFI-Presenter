// ============================================================================
// BFI Presenter -- inline hooks for x64 (shared): the DX add-on's InsnLength, HookByJumpChain and InlineHook64,
// used by the Vulkan layer to hook DLSS (NGX) calls. Copyright (C) 2026 artzox -- GPL-3.0-or-later.
// ============================================================================
#pragma once
#ifdef _WIN64
// Length of one x64 instruction (the common prologue subset); -1 if not
// supported. *ripDisp gets the offset of a RIP-relative displacement, or -1.
static int InsnLength(const BYTE *p, int *ripDisp)
{
    *ripDisp = -1;
    int i = 0;
    bool opsize = false, rexw = false;
    while (p[i] == 0x66 || p[i] == 0xF2 || p[i] == 0xF3) { if (p[i] == 0x66) opsize = true; i++; }
#ifdef _WIN64
    if ((p[i] & 0xF0) == 0x40) { rexw = (p[i] & 8) != 0; i++; }
#else
    if ((p[i] & 0xF0) == 0x40) return i + 1;                       // x86: inc / dec reg
#endif
    const BYTE op = p[i++];
    int imm = 0;
    bool modrm = false;
    if (op >= 0x50 && op <= 0x5F) return i;                       // push / pop
    if (op == 0x90 || op == 0xC3 || op == 0xCC) return i;
    if (op == 0x6A) return i + 1;
    if (op == 0x68) return i + 4;
    if (op >= 0xB8 && op <= 0xBF) return i + (rexw ? 8 : 4);
    if (op == 0x0F)
    {
        const BYTE op2 = p[i++];
        if (op2 == 0x1F || op2 == 0x10 || op2 == 0x11 || op2 == 0x28 || op2 == 0x29 || op2 == 0xB6 || op2 == 0xB7 ||
            op2 == 0xBE || op2 == 0xBF || (op2 >= 0x40 && op2 <= 0x4F) || op2 == 0xAF)
            modrm = true;
        else return -1;
    }
    else switch (op)
    {
    case 0x01: case 0x03: case 0x09: case 0x0B: case 0x21: case 0x23: case 0x29: case 0x2B: case 0x31: case 0x33:
    case 0x39: case 0x3B: case 0x63: case 0x84: case 0x85: case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8D:
    case 0xD1: case 0xD3: case 0xFF: case 0xF7:
        modrm = true;
        if (op == 0xF7) return -1;                                 // test imm form varies: not needed
        break;
    case 0x80: case 0x83: case 0xC0: case 0xC1: case 0xC6: case 0x6B: modrm = true; imm = 1; break;
    case 0x81: case 0xC7: case 0x69: modrm = true; imm = opsize ? 2 : 4; break;
    default: return -1;                                            // branches and anything unusual
    }
    if (!modrm) return -1;
    const BYTE m = p[i++];
    const int mod = m >> 6, rm = m & 7;
    if (mod != 3 && rm == 4)
    {
        const BYTE sib = p[i++];
        if (mod == 0 && (sib & 7) == 5) i += 4;
    }
#ifdef _WIN64
    if (mod == 0 && rm == 5) { *ripDisp = i; i += 4; }
#else
    if (mod == 0 && rm == 5) i += 4;                                 // x86: absolute address, copies as is
#endif
    else if (mod == 1) i += 1;
    else if (mod == 2) i += 4;
    return i + imm;
}

// target is changed to a relay to hook, and the returned "original" runs the bytes before the jump and
// then jumps where it used to go, so the earlier hook keeps working behind ours.
static void *HookByJumpChain(BYTE *code, size_t at, void *hook, char *why, size_t whyLen)
{
    int32_t rel;
    memcpy(&rel, code + at + 1, 4);
    BYTE *dest = code + at + 5 + rel;
    BYTE *page = nullptr;
    const uintptr_t base = (uintptr_t)code & ~(uintptr_t)0xFFFF;
    for (uintptr_t d = 0x10000; d < 0x40000000 && !page; d += 0x10000)
    {
        page = (BYTE *)VirtualAlloc((void *)(base - d), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!page) page = (BYTE *)VirtualAlloc((void *)(base + d), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    }
    if (!page) { snprintf(why, whyLen, "no memory near the function"); return nullptr; }
    // page+0: the original -- the bytes before the jump, then jmp [rip] -> dest
    memcpy(page, code, at);
    BYTE *j = page + at;
    j[0] = 0xFF; j[1] = 0x25; j[2] = j[3] = j[4] = j[5] = 0;
    const uint64_t d64 = (uint64_t)(uintptr_t)dest;
    memcpy(j + 6, &d64, 8);
    // page+64: relay to the hook
    BYTE *relay = page + 64;
    relay[0] = 0xFF; relay[1] = 0x25; relay[2] = relay[3] = relay[4] = relay[5] = 0;
    const uint64_t h64 = (uint64_t)(uintptr_t)hook;
    memcpy(relay + 6, &h64, 8);
    const int64_t nrel = (int64_t)(uintptr_t)relay - ((int64_t)(uintptr_t)code + (int64_t)at + 5);
    if (nrel < INT32_MIN || nrel > INT32_MAX) { VirtualFree(page, 0, MEM_RELEASE); snprintf(why, whyLen, "relay out of range"); return nullptr; }
    DWORD old;
    VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), page, 128);
    if (!VirtualProtect(code + at + 1, 4, PAGE_EXECUTE_READWRITE, &old)) { snprintf(why, whyLen, "can't write the function"); return nullptr; }
    const int32_t n32 = (int32_t)nrel;
    InterlockedExchange((volatile LONG *)(code + at + 1), (LONG)n32);   // not always aligned; x64 tolerates it
    VirtualProtect(code + at + 1, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, at + 5);
    snprintf(why, whyLen, "chained behind the jump at +%zu", at);
    return page;
}

// Patches target's first instructions with a jump to hook; returns a callable copy
// of the original start (nullptr if the start can't be relocated).
static void *InlineHook64(void *target, void *hook, char *why, size_t whyLen)
{
    BYTE *code = (BYTE *)target;
    int ins[16], rip[16], n = 0;
    size_t len = 0;
    while (len < 14)
    {
        int rd;
        // A jump already at the start (only plain instructions before it, no RIP-relative ones): chain behind it.
        if (code[len] == 0xE9 && len <= 8)
        {
            bool plain = true;
            for (int k = 0; k < n; k++) if (rip[k] >= 0) plain = false;
            if (plain) return HookByJumpChain(code, len, hook, why, whyLen);
        }
        const int l = InsnLength(code + len, &rd);
        if (l <= 0 || n >= 16) { snprintf(why, whyLen, "unsupported instruction at +%zu (%02X)", len, code[len]); return nullptr; }
        ins[n] = (int)len; rip[n] = rd; n++;
        len += (size_t)l;
    }
    BYTE *tramp = nullptr;
    const uintptr_t base = (uintptr_t)target & ~(uintptr_t)0xFFFF;
    for (uintptr_t d = 0x10000; d < 0x40000000 && !tramp; d += 0x10000)
    {
        tramp = (BYTE *)VirtualAlloc((void *)(base - d), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (!tramp) tramp = (BYTE *)VirtualAlloc((void *)(base + d), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    }
    if (!tramp) { snprintf(why, whyLen, "no memory near the function"); return nullptr; }
    memcpy(tramp, code, len);
    for (int k = 0; k < n; k++)
    {
        if (rip[k] < 0) continue;
        const int at = ins[k] + rip[k];
        int32_t disp;
        memcpy(&disp, code + at, 4);
        const int64_t abs = (int64_t)(uintptr_t)code + at + 4 + disp;
        const int64_t nd  = abs - ((int64_t)(uintptr_t)tramp + at + 4);
        if (nd < INT32_MIN || nd > INT32_MAX) { VirtualFree(tramp, 0, MEM_RELEASE); snprintf(why, whyLen, "displacement out of range"); return nullptr; }
        const int32_t nd32 = (int32_t)nd;
        memcpy(tramp + at, &nd32, 4);
    }
    BYTE *j = tramp + len;
    j[0] = 0xFF; j[1] = 0x25; j[2] = j[3] = j[4] = j[5] = 0;
    const uint64_t back = (uint64_t)(uintptr_t)(code + len);
    memcpy(j + 6, &back, 8);
    DWORD old;
    VirtualProtect(tramp, 4096, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), tramp, len + 14);
    BYTE patch[48];
    patch[0] = 0xFF; patch[1] = 0x25; patch[2] = patch[3] = patch[4] = patch[5] = 0;
    const uint64_t to = (uint64_t)(uintptr_t)hook;
    memcpy(patch + 6, &to, 8);
    for (size_t i = 14; i < len; i++) patch[i] = 0x90;
    if (!VirtualProtect(code, len, PAGE_EXECUTE_READWRITE, &old)) { snprintf(why, whyLen, "can't write the function"); return nullptr; }
    memcpy(code, patch, len);
    VirtualProtect(code, len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), code, len);
    return tramp;
}
#endif
