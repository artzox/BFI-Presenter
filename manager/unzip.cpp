// BFI Presenter Manager -- minimal zip reader (stored and deflate) for zips appended to an exe
// (ReShade's setup carries its DLLs that way). Copyright (C) 2026 artzox. SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace bfi {

namespace {

struct Bits {
    const uint8_t *p, *end;
    uint32_t buf = 0;
    int n = 0;
    bool bad = false;
    int Get(int count)
    {
        while (n < count)
        {
            if (p >= end) { bad = true; return 0; }
            buf |= (uint32_t)*p++ << n;
            n += 8;
        }
        int v = (int)(buf & ((1u << count) - 1));
        buf >>= count;
        n -= count;
        return v;
    }
};

struct Huff { uint16_t count[16]; uint16_t sym[320]; };

bool Build(Huff &h, const uint8_t *lens, int num)
{
    memset(h.count, 0, sizeof(h.count));
    for (int i = 0; i < num; i++) h.count[lens[i]]++;
    h.count[0] = 0;
    uint16_t offs[16];
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h.count[i];
    for (int i = 0; i < num; i++) if (lens[i]) h.sym[offs[lens[i]]++] = (uint16_t)i;
    return true;
}

int Decode(Bits &b, const Huff &h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++)
    {
        code |= b.Get(1);
        int count = h.count[len];
        if (code - count < first) return h.sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (b.bad) return -1;
    }
    return -1;
}

const uint16_t kLenBase[] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
const uint8_t kLenExtra[] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
const uint16_t kDistBase[] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                               4097, 6145, 8193, 12289, 16385, 24577 };
const uint8_t kDistExtra[] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

bool Codes(Bits &b, std::string &out, const Huff &lit, const Huff &dist)
{
    for (;;)
    {
        int s = Decode(b, lit);
        if (s < 0) return false;
        if (s < 256) out += (char)s;
        else if (s == 256) return true;
        else
        {
            s -= 257;
            if (s >= 29) return false;
            int len = kLenBase[s] + b.Get(kLenExtra[s]);
            int d = Decode(b, dist);
            if (d < 0 || d >= 30) return false;
            size_t back = kDistBase[d] + b.Get(kDistExtra[d]);
            if (back > out.size() || b.bad) return false;
            size_t from = out.size() - back;
            for (int i = 0; i < len; i++) out += out[from + i];
        }
    }
}

bool Inflate(const uint8_t *src, size_t n, std::string &out, size_t expect)
{
    Bits b{ src, src + n };
    out.clear();
    out.reserve(expect);
    int last;
    do
    {
        last = b.Get(1);
        int type = b.Get(2);
        if (type == 0)
        {
            b.buf = 0; b.n = 0;                 // to a byte boundary
            if (b.end - b.p < 4) return false;
            unsigned len = b.p[0] | (b.p[1] << 8);
            b.p += 4;
            if ((size_t)(b.end - b.p) < len) return false;
            out.append((const char *)b.p, len);
            b.p += len;
        }
        else if (type == 1)
        {
            static Huff lit, dist;
            static bool init = false;
            if (!init)
            {
                uint8_t l[288];
                for (int i = 0; i < 144; i++) l[i] = 8;
                for (int i = 144; i < 256; i++) l[i] = 9;
                for (int i = 256; i < 280; i++) l[i] = 7;
                for (int i = 280; i < 288; i++) l[i] = 8;
                Build(lit, l, 288);
                uint8_t d[30];
                for (int i = 0; i < 30; i++) d[i] = 5;
                Build(dist, d, 30);
                init = true;
            }
            if (!Codes(b, out, lit, dist)) return false;
        }
        else if (type == 2)
        {
            int hlit = b.Get(5) + 257, hdist = b.Get(5) + 1, hclen = b.Get(4) + 4;
            static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            uint8_t cl[19] = {};
            for (int i = 0; i < hclen; i++) cl[order[i]] = (uint8_t)b.Get(3);
            Huff clh;
            Build(clh, cl, 19);
            uint8_t lens[320] = {};
            for (int i = 0; i < hlit + hdist;)
            {
                int s = Decode(b, clh);
                if (s < 0) return false;
                if (s < 16) lens[i++] = (uint8_t)s;
                else
                {
                    int rep = 0;
                    uint8_t v = 0;
                    if (s == 16) { if (!i) return false; v = lens[i - 1]; rep = 3 + b.Get(2); }
                    else if (s == 17) rep = 3 + b.Get(3);
                    else rep = 11 + b.Get(7);
                    if (i + rep > hlit + hdist) return false;
                    while (rep--) lens[i++] = v;
                }
            }
            Huff lit, dist;
            Build(lit, lens, hlit);
            Build(dist, lens + hlit, hdist);
            if (!Codes(b, out, lit, dist)) return false;
        }
        else return false;
        if (b.bad) return false;
    } while (!last);
    return true;
}

uint32_t Rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t Rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

} // namespace

// Extracts the named entry (case-insensitive, any folder) from a zip, also one appended to another file.
bool ZipExtract(const std::string &data, const std::string &name, std::string &out)
{
    const uint8_t *d = (const uint8_t *)data.data();
    size_t n = data.size();
    if (n < 22) return false;
    // End of central directory: the last "PK\5\6" within the final 64 KB
    size_t eocd = std::string::npos;
    for (size_t i = n - 22 + 1; i-- > (n > 65557 ? n - 65557 : 0);)
        if (d[i] == 'P' && d[i + 1] == 'K' && d[i + 2] == 5 && d[i + 3] == 6) { eocd = i; break; }
    if (eocd == std::string::npos) return false;
    uint32_t cdSize = Rd32(d + eocd + 12), cdOff = Rd32(d + eocd + 16);
    uint16_t entries = Rd16(d + eocd + 10);
    if (cdSize > eocd) return false;
    // data in front of the zip (the exe) shifts every offset
    size_t shift = eocd - cdSize - cdOff;
    size_t p = cdOff + shift;
    auto lower = [](std::string s) { for (auto &c : s) c = (char)tolower((unsigned char)c); return s; };
    std::string want = lower(name);
    for (unsigned e = 0; e < entries && p + 46 <= n; e++)
    {
        if (Rd32(d + p) != 0x02014b50) return false;
        uint16_t method = Rd16(d + p + 10), fnLen = Rd16(d + p + 28), exLen = Rd16(d + p + 30), cmLen = Rd16(d + p + 32);
        uint32_t csize = Rd32(d + p + 20), usize = Rd32(d + p + 24), lho = Rd32(d + p + 42);
        std::string fn((const char *)d + p + 46, fnLen);
        p += 46 + fnLen + exLen + cmLen;
        std::string base = lower(fn.substr(fn.find_last_of("/\\") == std::string::npos ? 0 : fn.find_last_of("/\\") + 1));
        if (base != want) continue;
        size_t lh = lho + shift;
        if (lh + 30 > n || Rd32(d + lh) != 0x04034b50) return false;
        size_t start = lh + 30 + Rd16(d + lh + 26) + Rd16(d + lh + 28);
        if (start + csize > n) return false;
        if (method == 0) { out.assign((const char *)d + start, csize); return true; }
        if (method == 8) return Inflate(d + start, csize, out, usize) && out.size() == usize;
        return false;
    }
    return false;
}

} // namespace bfi
