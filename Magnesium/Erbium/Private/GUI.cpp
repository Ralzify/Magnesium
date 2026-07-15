#include "pch.h"
#include "../Public/GUI.h"
#include <d3d11.h>
#include "../../ImGui/imgui.h"
#include "../../ImGui/imgui_stdlib.h"
#include "../../ImGui/imgui_impl_win32.h"
#include "../../ImGui/imgui_impl_dx11.h"
#include "../Public/Calendar.h"
#include "../Public/Configuration.h"
#include "../Public/Events.h"
#include "../Public/Misc.h"
#include "../../FortniteGame/Public/BattleRoyaleGamePhaseLogic.h"
#include "../../FortniteGame/Public/BuildingSMActor.h"
#include "../../Engine/Public/NetDriver.h"
#include "../../FortniteGame/Public/FortPhysicsPawn.h"
#include "../PlayerAI/Public/MagnesiumPlayerAISettings.h"
#include "../../Engine/Public/Texture.h"
#include <sstream>
#include <fstream>
#include <string>
#include <atomic>
#include <Windows.h>
#include <Shellapi.h>
#include <chrono>
#include <algorithm>
#include <unordered_set>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#pragma warning(push)
#pragma warning(disable: 4996) // stb_image_write uses sprintf in the HDR path
#include "stb_image_write.h"
#pragma warning(pop)
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
#include "EmbeddedImage.h"
#include "Icon.h" // ATLAS logo (ported menu branding)

#pragma comment(lib, "d3d11.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

UINT g_ResizeWidth = 0, g_ResizeHeight = 0;

ID3D11ShaderResourceView* g_EmbedTexture = nullptr;
int EmbedWidth = 0;
int EmbedHeight = 0;

// Ported ATLAS-style menu logo
ID3D11ShaderResourceView* g_LogoTexture = nullptr;
int g_LogoW = 0, g_LogoH = 0;

// Grey/dark theme accent (highlights, active tab, checkmarks, sliders) - soft cool silver
#define ACCENT_R 0.780f
#define ACCENT_G 0.820f
#define ACCENT_B 0.910f
static ImVec4 Accent(float a = 1.f) { return ImVec4(ACCENT_R, ACCENT_G, ACCENT_B, a); }
static ImVec4 AccentDk(float a = 1.f) { return ImVec4(0.50f, 0.54f, 0.64f, a); } // darker for active

bool LoadTextureFromMemory(const unsigned char* buffer, int buffer_size, ID3D11Device* d3dDevice, ID3D11ShaderResourceView** out_srv, int* out_width, int* out_height)
{
    int image_width = 0;
    int image_height = 0;
    unsigned char* image_data = stbi_load_from_memory(buffer, buffer_size, &image_width, &image_height, NULL, 4);

    if (image_data == NULL)
        return false;

    // Full mip chain + GPU-generated mips so large source images downscale smoothly
    // to small on-screen sizes (e.g. the 1024px logo) instead of looking pixelated.
    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = image_width;
    desc.Height = image_height;
    desc.MipLevels = 0; // 0 => allocate a full mip chain
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    desc.CPUAccessFlags = 0;

    ID3D11Texture2D* pTexture = NULL;
    d3dDevice->CreateTexture2D(&desc, NULL, &pTexture);
    if (pTexture == NULL)
    {
        stbi_image_free(image_data);
        return false;
    }

    ID3D11DeviceContext* ctx = NULL;
    d3dDevice->GetImmediateContext(&ctx);
    ctx->UpdateSubresource(pTexture, 0, NULL, image_data, image_width * 4, 0);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    ZeroMemory(&srvDesc, sizeof(srvDesc));
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = (UINT)-1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    d3dDevice->CreateShaderResourceView(pTexture, &srvDesc, out_srv);
    if (*out_srv)
        ctx->GenerateMips(*out_srv);
    if (ctx)
        ctx->Release();
    pTexture->Release();

    *out_width = image_width;
    *out_height = image_height;

    stbi_image_free(image_data);

    return true;
}

// Upload an already-decoded RGBA8 buffer to a texture on our device. Sibling of
// LoadTextureFromMemory minus the stb decode; used for extracted map pixels.
bool CreateTextureFromRGBA8(const unsigned char* rgba, int width, int height, ID3D11Device* d3dDevice, ID3D11ShaderResourceView** out_srv)
{
    if (!rgba || width <= 0 || height <= 0 || !d3dDevice)
        return false;

    D3D11_TEXTURE2D_DESC desc;
    ZeroMemory(&desc, sizeof(desc));
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 0; // full mip chain, GPU-generated (smooth downscale to ~260px)
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    desc.CPUAccessFlags = 0;

    ID3D11Texture2D* pTexture = NULL;
    d3dDevice->CreateTexture2D(&desc, NULL, &pTexture);
    if (pTexture == NULL)
        return false;

    ID3D11DeviceContext* ctx = NULL;
    d3dDevice->GetImmediateContext(&ctx);
    ctx->UpdateSubresource(pTexture, 0, NULL, rgba, width * 4, 0);

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
    ZeroMemory(&srvDesc, sizeof(srvDesc));
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = (UINT)-1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    d3dDevice->CreateShaderResourceView(pTexture, &srvDesc, out_srv);
    if (*out_srv)
        ctx->GenerateMips(*out_srv);
    if (ctx)
        ctx->Release();
    pTexture->Release();
    return true;
}

// ============================================================================
//  Custom Safe Zone interactive map editor support.
//  The GUI runs on its own standalone D3D11 device with no bridge into the game
//  renderer, so the in-game minimap UTexture2D can't be handed to ImGui directly.
//  We best-effort read its mip0 pixels out of CPU memory, decode to RGBA8, and
//  upload to *our* device (dumping a PNG next to the DLL for reuse).
// ============================================================================
namespace SafeZoneMap
{
    // Half-extent of the Athena/Apollo world map in UE units (cm). Corner samples
    // were ~+/-135345 on both axes; treat the map as a symmetric square.

    auto Chapter1 = VersionInfo.FortniteVersion < 11.00 || std::floor(VersionInfo.FortniteVersion) == 27;
	auto Chapter2 = VersionInfo.FortniteVersion >= 11.00 && VersionInfo.FortniteVersion < 19.00;
	auto Chapter3 = VersionInfo.FortniteVersion >= 19.00 && VersionInfo.FortniteVersion < 23.00;
	auto Chapter4 = VersionInfo.FortniteVersion >= 23.00 && VersionInfo.FortniteVersion < 27.00;
	auto Chapter5 = VersionInfo.FortniteVersion >= 28.00;

    constexpr float kWorldExtent = 135345.f; // make this chapter-dependant, currently set at the chapter2 & chapter3 value, need to update for chapter1 & chapter4

    static inline float Clamp(float v, float lo, float hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // Canvas-local pixel (origin top-left, y down) -> UE world (cm).
    // World X is the vertical axis (top = +, bottom = -), flipped vs screen-y.
    // World Y is the horizontal axis (left = -, right = +).
    static inline void PixelToWorld(float lx, float ly, float side, float& worldX, float& worldY)
    {
        worldY = kWorldExtent * (2.f * lx / side - 1.f);
        worldX = kWorldExtent * (1.f - 2.f * ly / side);
    }

    // UE world (cm) -> canvas-local pixel (add the canvas rect-min for screen pos).
    static inline void WorldToPixel(float worldX, float worldY, float side, float& lx, float& ly)
    {
        lx = (side * 0.5f) * (1.f + worldY / kWorldExtent);
        ly = (side * 0.5f) * (1.f - worldX / kWorldExtent);
    }

    static inline float RadiusToPixels(float radiusCm, float side) { return radiusCm * (side / (2.f * kWorldExtent)); }
    static inline float PixelsToRadius(float radiusPx, float side) { return radiusPx * (2.f * kWorldExtent / side); }

    // Shade the part of rect [rmin,rmax] that lies OUTSIDE the circle (c, radius)
    // as an angular fan of quads from the circle edge out to the rect boundary.
    static void FillOutsideCircle(ImDrawList* dl, const ImVec2& rmin, const ImVec2& rmax,
                                  const ImVec2& c, float radius, ImU32 col)
    {
        const int N = 96;
        const float TwoPi = 6.28318530718f;
        auto rayToRect = [&](float dx, float dy) -> float
        {
            float t = 1e30f, tt;
            if (dx >  1e-6f) { tt = (rmax.x - c.x) / dx; if (tt < t) t = tt; }
            else if (dx < -1e-6f) { tt = (rmin.x - c.x) / dx; if (tt < t) t = tt; }
            if (dy >  1e-6f) { tt = (rmax.y - c.y) / dy; if (tt < t) t = tt; }
            else if (dy < -1e-6f) { tt = (rmin.y - c.y) / dy; if (tt < t) t = tt; }
            return t < 0.f ? 0.f : t;
        };
        for (int i = 0; i < N; ++i)
        {
            float a0 = (float)i / N * TwoPi, a1 = (float)(i + 1) / N * TwoPi;
            float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
            float t0 = rayToRect(c0, s0), t1 = rayToRect(c1, s1);
            float r0 = radius < t0 ? radius : t0;  // clamp so we never overshoot the rect
            float r1 = radius < t1 ? radius : t1;
            ImVec2 in0(c.x + c0 * r0, c.y + s0 * r0), in1(c.x + c1 * r1, c.y + s1 * r1);
            ImVec2 out0(c.x + c0 * t0, c.y + s0 * t0), out1(c.x + c1 * t1, c.y + s1 * t1);
            dl->AddQuadFilled(in0, in1, out1, out0, col);
        }
    }

    // Manual override for tuning on a specific engine version (0 = auto-detect).
    static uint32_t g_PlatformDataOffsetOverride = 0;

    // Stable low EPixelFormat values (append-only in UE, so identical across the
    // whole version range). Minimaps are PF_DXT1; PF_B8G8R8A8 covers uncompressed.
    enum : int32 { PF_B8G8R8A8 = 2, PF_DXT1 = 5, PF_DXT3 = 6, PF_DXT5 = 7 };

    static bool IsKnownFormat(int32 f) { return f == PF_B8G8R8A8 || f == PF_DXT1 || f == PF_DXT3 || f == PF_DXT5; }
    static size_t FormatBytes(int32 f, int w, int h)
    {
        const size_t px = (size_t)w * h;
        if (f == PF_B8G8R8A8) return px * 4; // 32bpp uncompressed
        if (f == PF_DXT1)     return px / 2; // BC1: 8 bytes / 16 px
        return px;                            // BC2/BC3: 16 bytes / 16 px
    }

    // True only if every byte of [p, p+bytes) is committed and readable.
    static bool IsReadable(const void* p, size_t bytes)
    {
        if (!p || bytes == 0) return false;
        const uintptr_t start = (uintptr_t)p;
        if (start < 0x10000) return false;               // null / low reserved region
        if (start + bytes < start) return false;         // address-space wraparound => bogus ptr
        if (start > 0x7FFFFFFFFFFFull) return false;      // above x64 user address space
        MEMORY_BASIC_INFORMATION mbi{};
        const uint8_t* cur = (const uint8_t*)p;
        const uint8_t* end = cur + bytes;
        while (cur < end)
        {
            if (VirtualQuery(cur, &mbi, sizeof(mbi)) == 0) return false;
            if (mbi.State != MEM_COMMIT) return false;
            if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || mbi.Protect == 0) return false;
            cur = (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
        }
        return true;
    }

    // Bytes of the committed region from p to the end of its VirtualQuery block.
    // Restricted to MEM_PRIVATE: texture pixel buffers are FMemory::Malloc heap
    // allocations. MEM_IMAGE (the EXE) and MEM_MAPPED (pak files) regions also
    // pass a plain "readable" test and previously produced garbage textures
    // (2MB of program code decoded as DXT1 == colored static; see 10.40/27.11).
    static size_t RegionSize(const void* p)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!p || (uintptr_t)p < 0x10000 || VirtualQuery(p, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) return 0;
        if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || mbi.Protect == 0) return 0;
        if (mbi.Type != MEM_PRIVATE) return 0; // heap only: reject EXE image / mapped-file pointers
        size_t before = (const uint8_t*)p - (const uint8_t*)mbi.BaseAddress;
        return mbi.RegionSize > before ? (size_t)(mbi.RegionSize - before) : 0;
    }

    static bool IsPow2Dim(int32 v) { return v >= 64 && v <= 16384 && (v & (v - 1)) == 0; }

    // Reject "pixel data" pointers whose first 8 bytes are a code/vtable pointer
    // into a loaded module - the classic false positive (async-IO handles, linker
    // objects stored inside FByteBulkData) that decodes as colored static.
    static bool StartsWithImagePointer(const uint8_t* p)
    {
        if (!IsReadable(p, 8)) return false;
        const uint8_t* q = *(const uint8_t* const*)p;
        if (!q || (uintptr_t)q < 0x10000 || ((uintptr_t)q & 7)) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(q, &mbi, sizeof(mbi)) == 0) return false;
        return mbi.Type == MEM_IMAGE;
    }

    struct FPlatformData
    {
        const uint8_t* Ptr = nullptr;
        int32 SizeX = 0, SizeY = 0, PixelFormat = 0;
        uint32_t FoundAtOffset = 0;
    };

    // Scan the UTexture2D object for the non-reflected PlatformData pointer by
    // recognising FTexturePlatformData's header: {int32 SizeX, int32 SizeY,
    // uint32 PackedData/NumSlices, EPixelFormat PixelFormat}. Reading PixelFormat
    // from +0x0C (not a "first small int" probe, which grabs PackedData) both
    // validates the candidate and gives the correct format to decode.
    static bool DetectPlatformData(const void* tex, FPlatformData& out)
    {
        if (!IsReadable(tex, 0x220)) return false;
        const uint8_t* base = (const uint8_t*)tex;

        FPlatformData fallback; bool haveFallback = false;

        auto consider = [&](const uint8_t* P, uint32_t off) -> bool
        {
            if (!IsReadable(P, 0x20) || ((uintptr_t)P & 7)) return false;
            int32 sx = *(const int32*)(P + 0x0);
            int32 sy = *(const int32*)(P + 0x4);
            if (!IsPow2Dim(sx) || !IsPow2Dim(sy)) return false;
            // PixelFormat is at +0x0C (after two dims + PackedData); a couple of
            // older layouts keep it at +0x08. Take whichever is a known format.
            int32 pf0c = *(const int32*)(P + 0x0C);
            int32 pf08 = *(const int32*)(P + 0x08);
            int32 pf = IsKnownFormat(pf0c) ? pf0c : (IsKnownFormat(pf08) ? pf08 : 0);
            if (pf != 0) // strong match: real FTexturePlatformData with a known format
            {
                out.Ptr = P; out.SizeX = sx; out.SizeY = sy; out.PixelFormat = pf; out.FoundAtOffset = off;
                SDK::DbgLog("[SafeZoneMap] PlatformData @0x%X ptr=%p %dx%d PixelFormat=%d\n",
                    off, (const void*)P, sx, sy, pf);
                return true;
            }
            if (!haveFallback) { fallback = { P, sx, sy, 0, off }; haveFallback = true; }
            return false;
        };

        uint32_t start = g_PlatformDataOffsetOverride ? g_PlatformDataOffsetOverride : 0x10;
        uint32_t stop  = g_PlatformDataOffsetOverride ? g_PlatformDataOffsetOverride : 0x200;
        for (uint32_t off = start; off <= stop; off += 8)
        {
            const uint8_t* P = *(const uint8_t* const*)(base + off);
            if (consider(P, off)) return true;
            if (IsReadable(P, 8)) // UE5 pointer-to-pointer (PrivatePlatformData) variant
            {
                const uint8_t* Q = *(const uint8_t* const*)P;
                if (consider(Q, off)) return true;
            }
        }
        if (haveFallback) // no known-format match; fall back to first pow2 candidate
        {
            out = fallback;
            SDK::DbgLog("[SafeZoneMap] PlatformData (weak) @0x%X ptr=%p %dx%d (no known format)\n",
                out.FoundAtOffset, (const void*)out.Ptr, out.SizeX, out.SizeY);
            return true;
        }
        SDK::DbgLog("[SafeZoneMap] PlatformData not found in texture object\n");
        return false;
    }

    // From detected platform data, find mip0's resident pixel bytes (a pointer to
    // a committed region large enough for neededBytes).
    static const uint8_t* FindMip0Bytes(const FPlatformData& pd, size_t neededBytes)
    {
        for (uint32_t moff = 0x0C; moff <= 0x80; moff += 4) // the mips TArray {Data,Num,Max}
        {
            const uint8_t* arrAt = pd.Ptr + moff;
            if (!IsReadable(arrAt, 16)) continue;
            const uint8_t* data = *(const uint8_t* const*)(arrAt);
            int32 num = *(const int32*)(arrAt + 8);
            int32 max = *(const int32*)(arrAt + 12);
            if (num < 1 || num > 20 || max < num || !IsReadable(data, 8)) continue;

            // Element may be inline (FTexture2DMipMap) or a pointer (TIndirectArray).
            const uint8_t* mip0 = data;
            const uint8_t* asPtr = *(const uint8_t* const*)data;
            if (IsReadable(asPtr, 0x40))
            {
                int32 maybeSize = *(const int32*)asPtr;
                if (maybeSize == pd.SizeX || maybeSize == pd.SizeY) mip0 = asPtr;
            }
            if (!IsReadable(mip0, 0x60)) continue;

            for (uint32_t boff = 0x0; boff <= 0x50; boff += 8) // the bulk-data pointer
            {
                const uint8_t* cand = *(const uint8_t* const*)(mip0 + boff);
                if (RegionSize(cand) >= neededBytes && !StartsWithImagePointer(cand))
                {
                    SDK::DbgLog("[SafeZoneMap] mip0 bytes @mip+0x%X ptr=%p (need %zu, mips@pd+0x%X num=%d)\n",
                        boff, (const void*)cand, neededBytes, moff, num);
                    return cand;
                }
            }
        }
        SDK::DbgLog("[SafeZoneMap] mip0 resident bytes not found (need %zu)\n", neededBytes);
        return nullptr;
    }

    // Best-effort: extract the given minimap texture into an RGBA8 buffer.
    static bool ExtractToRGBA(const void* tex, std::vector<unsigned char>& rgba, int& outW, int& outH)
    {
        FPlatformData pd;
        if (!DetectPlatformData(tex, pd)) return false;

        const int w = pd.SizeX, h = pd.SizeY;
        const size_t pixels = (size_t)w * h;

        int fmt = pd.PixelFormat;
        const uint8_t* src = nullptr;

        // Known format -> look for exactly the mip that size (no size-guessing,
        // which is what previously decoded BC1 bytes as raw RGBA -> garbled).
        if (IsKnownFormat(fmt))
            src = FindMip0Bytes(pd, FormatBytes(fmt, w, h));

        if (!src) // unknown format or not found: probe by descending size class
        {
            const struct { int f; size_t need; } probes[] = {
                { PF_B8G8R8A8, pixels * 4 },
                { PF_DXT5,     pixels     },
                { PF_DXT1,     pixels / 2 },
            };
            for (auto& pr : probes)
            {
                const uint8_t* p = FindMip0Bytes(pd, pr.need);
                if (p) { src = p; fmt = pr.f; break; }
            }
            if (!src) return false;
        }

        rgba.assign(pixels * 4, 0);

        auto decodeBlocks = [&](int blockBytes, void(*dec)(const void*, void*, int))
        {
            const int bw = (w + 3) / 4, bh = (h + 3) / 4;
            const uint8_t* bp = src;
            for (int by = 0; by < bh; ++by)
                for (int bx = 0; bx < bw; ++bx, bp += blockBytes)
                {
                    unsigned char block[64]; // 4x4 RGBA
                    dec(bp, block, 4 * 4);
                    for (int py = 0; py < 4; ++py)
                    {
                        int y = by * 4 + py; if (y >= h) break;
                        for (int px = 0; px < 4; ++px)
                        {
                            int x = bx * 4 + px; if (x >= w) break;
                            unsigned char* d = &rgba[((size_t)y * w + x) * 4];
                            unsigned char* s = &block[(py * 4 + px) * 4];
                            d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
                        }
                    }
                }
        };

        if (fmt == PF_B8G8R8A8)
        {
            for (size_t i = 0; i < pixels; ++i)
            {
                const uint8_t* s = src + i * 4;
                unsigned char* d = &rgba[i * 4];
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; // BGRA -> RGBA
            }
        }
        else if (fmt == PF_DXT1)                 { decodeBlocks(BCDEC_BC1_BLOCK_SIZE, bcdec_bc1); }
        else if (fmt == PF_DXT3)                 { decodeBlocks(BCDEC_BC2_BLOCK_SIZE, bcdec_bc2); }
        else if (fmt == PF_DXT5)                 { decodeBlocks(BCDEC_BC3_BLOCK_SIZE, bcdec_bc3); }
        else return false;

        outW = w; outH = h;
        SDK::DbgLog("[SafeZoneMap] extracted %dx%d fmt=%d ok\n", w, h, fmt);
        return true;
    }

    // SEH guard: chasing unknown cross-version texture layouts can dereference a
    // bad pointer (e.g. the Rufus minimaps on 27.x). Catch the access violation
    // and fall back to the numeric editor instead of crashing the GUI thread.
    static bool ExtractToRGBA_Guarded(const void* tex, std::vector<unsigned char>& rgba, int& outW, int& outH)
    {
        __try
        {
            return ExtractToRGBA(tex, rgba, outW, outH);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            SDK::DbgLog("[SafeZoneMap] extraction faulted (SEH); skipping image\n");
            return false;
        }
    }

    // ------------------------------------------------------------------------
    // Game-thread load bridge. UE's loader (StaticLoadObject) is game-thread-
    // only: calling it from the GUI thread is what faulted on e.g. 17.30/27.x.
    // The GUI thread only POSTS a request; UNetDriver::TickFlush (game thread)
    // drains it, loads + extracts the pixels, and the GUI picks them up on a
    // later Acquire retry (the editor already re-polls every ~3s).
    // ------------------------------------------------------------------------
    enum class LoadState : int { Idle = 0, Requested, Ready, Failed, Consumed };
    static std::atomic<int> g_LoadState{ (int)LoadState::Idle };
    static std::vector<unsigned char> g_LoadedRGBA; // written under Requested, read under Ready
    static int g_LoadedW = 0, g_LoadedH = 0;

    static const UTexture2D* StaticLoadMinimapSEH(const wchar_t* path, const UClass* texClass)
    {
        __try
        {
            return (const UTexture2D*)SDK::StaticLoadObject(path, texClass);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            SDK::DbgLog("[SafeZoneMap] StaticLoadObject(%ls) faulted (SEH)\n", path);
            return nullptr;
        }
    }

    static int MinimapPathsForVersion(const wchar_t** out, int cap); // fwd
    static const UTexture2D* FindLoadedMinimapTexture(const wchar_t** paths, int np); // fwd

    // Called from the TickFlush hooks every server tick; a single atomic read
    // unless a request is actually pending.
    static void GameThreadTick()
    {
        if (g_LoadState.load(std::memory_order_acquire) != (int)LoadState::Requested)
            return;

        const wchar_t* paths[4];
        const int np = MinimapPathsForVersion(paths, 4);
        const UClass* texClass = UTexture2D::StaticClass();
        const UTexture2D* tex = nullptr;
        if (np && texClass && SDK::Offsets::StaticLoadObject)
            for (int i = 0; i < np && !tex; ++i)
            {
                tex = StaticLoadMinimapSEH(paths[i], texClass);
                SDK::DbgLog("[SafeZoneMap] game-thread StaticLoadObject(%ls) = %p\n", paths[i], (const void*)tex);
            }
        if (!tex) // the load may have registered it under a different outer/mount
            tex = FindLoadedMinimapTexture(paths, np);

        int w = 0, h = 0;
        if (tex && ExtractToRGBA_Guarded(tex, g_LoadedRGBA, w, h))
        {
            g_LoadedW = w; g_LoadedH = h;
            g_LoadState.store((int)LoadState::Ready, std::memory_order_release);
            return;
        }
        g_LoadedRGBA.clear();
        g_LoadState.store((int)LoadState::Failed, std::memory_order_release);
        SDK::DbgLog("[SafeZoneMap] game-thread minimap load failed\n");
    }

    // Candidate minimap object paths for the current engine version (find-only,
    // first hit wins). Paths are the full Package.ObjectName form. Some versions
    // ship the asset under more than one mount, so we list fallbacks.
    static int MinimapPathsForVersion(const wchar_t** out, int cap)
    {
        const float v = VersionInfo.FortniteVersion;
        int n = 0;
        auto add = [&](const wchar_t* p) { if (n < cap) out[n++] = p; };

        if (v < 11.00f)
            add(L"/Game/Athena/HUD/MiniMap/MiniMapAthena.MiniMapAthena");
        else if (v >= 13.00f && v < 14.00f) // C2S3 uses a season-specific texture
        {
            add(L"/Game/Athena/Apollo/Maps/UI/Apollo_Terrain_Minimap_S13_7.Apollo_Terrain_Minimap_S13_7");
            // Base asset fallback. NOTE: on some C2 builds the base texture is a
            // blanked placeholder (season art superseded it), so this mainly serves
            // to make Maps\Apollo_Terrain_Minimap.png a usable bundled fallback.
            add(L"/Game/Athena/Apollo/Maps/UI/Apollo_Terrain_Minimap.Apollo_Terrain_Minimap");
        }
        else if (v == 27.00f)
        {
            add(L"/Game/Athena/UI/Rufus/Capture_Iteration_Discovered_Rufus_01.Capture_Iteration_Discovered_Rufus_01");
            add(L"/Rufus/Game/UI/Capture_Iteration_Discovered_Rufus_01.Capture_Iteration_Discovered_Rufus_01");
        }
        else if (v == 27.10f)
        {
            add(L"/Game/Athena/UI/Rufus/Capture_Iteration_Discovered_Rufus_03.Capture_Iteration_Discovered_Rufus_03");
            add(L"/Rufus/Game/UI/Capture_Iteration_Discovered_Rufus_03.Capture_Iteration_Discovered_Rufus_03");
        }
        else if (v == 27.11f)
        {
            add(L"/Game/Athena/UI/Rufus/Capture_Iteration_Discovered_Rufus_04.Capture_Iteration_Discovered_Rufus_04");
            add(L"/Rufus/Game/UI/Capture_Iteration_Discovered_Rufus_04.Capture_Iteration_Discovered_Rufus_04");
        }
        else if ((v >= 11.00f && v < 27.00f) || v >= 28.00f)
            add(L"/Game/Athena/Apollo/Maps/UI/Apollo_Terrain_Minimap.Apollo_Terrain_Minimap");

        return n;
    }

    // Directory containing Magnesium.dll (not the host exe).
    static std::wstring ModuleDirW()
    {
        wchar_t buf[MAX_PATH] = {};
        DWORD n = GetModuleFileNameW(GetModuleHandleW(L"Magnesium.dll"), buf, MAX_PATH);
        std::wstring path(buf, n);
        size_t slash = path.find_last_of(L"\\/");
        return (slash == std::wstring::npos) ? L"." : path.substr(0, slash);
    }

    // Cache PNG next to Magnesium.dll, keyed by the EXACT version: Apollo seasons
    // reuse one object path but ship different art, so a coarser key would serve
    // one season's map for another.
    static std::wstring CachePathW()
    {
        wchar_t name[64];
        swprintf(name, 64, L"\\Magnesium_SafeZoneMap_%.2f.png", (double)VersionInfo.FortniteVersion);
        return ModuleDirW() + name;
    }

    static std::string WideToUtf8(const std::wstring& w)
    {
        if (w.empty()) return {};
        int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
        std::string s(len, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), len, nullptr, nullptr);
        return s;
    }

    // Scan the global object array for a loaded UTexture2D whose leaf name matches
    // one of the candidate paths' object names. Finds the texture regardless of its
    // outer/mount path (StaticFindObject needs the exact path, which varies), so it
    // succeeds where the hard-coded paths miss - as long as the texture is resident.
    // Rufus (ch1 remix) weekly map textures are matched by PREFIX: the resident
    // weekly index rarely matches the hard-coded one (e.g. 27.11 keeps Rufus_03
    // loaded, not Rufus_04), and any week's art is close enough for zone placement.
    static const UTexture2D* FindLoadedMinimapTexture(const wchar_t** paths, int np)
    {
        const UClass* texClass = UTexture2D::StaticClass();
        if (!texClass) return nullptr;

        // Exact-name candidates (fast FName compare) + prefix candidates (string).
        FName want[4]; int nn = 0;
        std::string prefixes[4]; int npre = 0;
        for (int i = 0; i < np; ++i)
        {
            const wchar_t* dot = wcsrchr(paths[i], L'.');
            std::wstring wleaf(dot ? dot + 1 : paths[i]);
            std::string nleaf(wleaf.begin(), wleaf.end()); // leaf names are ASCII

            // "..._Rufus_04" -> prefix "Capture_Iteration_Discovered_Rufus_"
            const std::string rufusTag = "_Rufus_";
            size_t rp = nleaf.rfind(rufusTag);
            if (rp != std::string::npos && npre < 4)
            {
                std::string pre = nleaf.substr(0, rp + rufusTag.size());
                bool dup = false;
                for (int k = 0; k < npre; ++k) if (prefixes[k] == pre) { dup = true; break; }
                if (!dup) prefixes[npre++] = pre;
            }
            if (nn < 4)
            {
                UEAllocatedString s = nleaf.c_str();
                UEAllocatedWString ws(s.begin(), s.end());
                want[nn++] = FName(ws);
            }
        }

        const UTexture2D* prefixHit = nullptr; // exact name wins over prefix match
        const int32 total = SDK::TUObjectArray::Num();
        for (int32 i = 0; i < total; ++i)
        {
            const UObject* obj = SDK::TUObjectArray::GetObjectByIndex(i);
            if (!obj) continue;
            bool hit = false;
            for (int k = 0; k < nn; ++k) if (obj->Name == want[k]) { hit = true; break; }
            if (hit)
            {
                if (obj->Class && obj->IsA(texClass))
                {
                    SDK::DbgLog("[SafeZoneMap] object-array scan found minimap texture @%p\n", (const void*)obj);
                    return (const UTexture2D*)obj;
                }
                continue;
            }
            if (npre == 0 || prefixHit) continue;
            if (!obj->Class || !obj->IsA(texClass)) continue; // ToString allocates; textures only
            std::string nm = obj->Name.ToString().c_str();
            for (int k = 0; k < npre; ++k)
            {
                if (nm.compare(0, prefixes[k].size(), prefixes[k]) != 0) continue;
                SDK::DbgLog("[SafeZoneMap] object-array scan prefix-matched '%s' @%p\n", nm.c_str(), (const void*)obj);
                prefixHit = (const UTexture2D*)obj;
                break;
            }
        }
        return prefixHit;
    }

    // One-shot diagnostic: report how many textures are resident and any with a
    // map-like name, so we can tell "wrong path" from "texture simply not loaded".
    static void DiagnosticLogTextures()
    {
        const UClass* texClass = UTexture2D::StaticClass();
        if (!texClass) { SDK::DbgLog("[SafeZoneMap] diag: no UTexture2D class\n"); return; }

        const int32 total = SDK::TUObjectArray::Num();
        int texCount = 0, logged = 0;
        for (int32 i = 0; i < total; ++i)
        {
            const UObject* obj = SDK::TUObjectArray::GetObjectByIndex(i);
            if (!obj || !obj->Class || !obj->IsA(texClass)) continue;
            ++texCount;
            if (logged >= 24) continue;
            std::string nm = obj->Name.ToString().c_str();
            if (nm.find("inimap") != std::string::npos || nm.find("errain") != std::string::npos ||
                nm.find("iscover") != std::string::npos || nm.find("apture") != std::string::npos ||
                nm.find("_Map") != std::string::npos || nm.find("Rufus") != std::string::npos)
            {
                SDK::DbgLog("[SafeZoneMap] diag: loaded texture '%s'\n", nm.c_str());
                ++logged;
            }
        }
        SDK::DbgLog("[SafeZoneMap] diag: %d UTexture2D resident, %d map-like\n", texCount, logged);
    }

    // Read a PNG file from disk and upload it to a texture.
    static bool LoadPngFile(const std::wstring& file, ID3D11Device* dev, ID3D11ShaderResourceView** outSrv, int* outW, int* outH)
    {
        std::ifstream f(file.c_str(), std::ios::binary);
        if (!f) return false;
        std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (data.empty()) return false;
        if (LoadTextureFromMemory((unsigned char*)data.data(), (int)data.size(), dev, outSrv, outW, outH))
        {
            SDK::DbgLog("[SafeZoneMap] loaded PNG %ls (%d bytes)\n", file.c_str(), (int)data.size());
            return true;
        }
        return false;
    }

    // User-supplied minimap PNGs (e.g. exported from FModel) in a "Maps" folder next
    // to the DLL. Server processes don't keep the base minimap texture resident, so
    // for most versions this is the real source. Checked most-specific first:
    //   Maps\<version>.png   (e.g. Maps\17.30.png) - exact per-version override
    //   Maps\<leaf>.png      (e.g. Maps\Apollo_Terrain_Minimap.png) - per-asset default
    static bool LoadBundledMinimap(const wchar_t** paths, int np, ID3D11Device* dev, ID3D11ShaderResourceView** outSrv, int* outW, int* outH)
    {
        const std::wstring dir = ModuleDirW();
        wchar_t vname[64];
        swprintf(vname, 64, L"\\Maps\\%.2f.png", (double)VersionInfo.FortniteVersion);
        if (LoadPngFile(dir + vname, dev, outSrv, outW, outH)) return true;
        SDK::DbgLog("[SafeZoneMap] no bundled PNG at %ls%ls\n", dir.c_str(), vname);

        for (int i = 0; i < np; ++i)
        {
            const wchar_t* dot = wcsrchr(paths[i], L'.');
            std::wstring leaf(dot ? dot + 1 : paths[i]);
            const std::wstring file = dir + L"\\Maps\\" + leaf + L".png";
            if (LoadPngFile(file, dev, outSrv, outW, outH)) return true;
            SDK::DbgLog("[SafeZoneMap] no bundled PNG at %ls\n", file.c_str());
        }
        return false;
    }

    // Try live extraction (and dump a PNG), else user-bundled PNGs, else cache.
    static bool Acquire(ID3D11Device* dev, ID3D11ShaderResourceView** outSrv, int* outW, int* outH)
    {
        const wchar_t* paths[4];
        const int np = MinimapPathsForVersion(paths, 4);
        if (np == 0)
        {
            SDK::DbgLog("[SafeZoneMap] no known minimap path for FN %.2f\n", VersionInfo.FortniteVersion);
            return false;
        }
        const std::wstring cacheW = CachePathW();

        // 1) Live extraction from the loaded UTexture2D. Use StaticFindObject
        // (find-only): FindObject<>() falls back to StaticLoadObject when the asset
        // isn't already loaded, and that native loader faults on some versions
        // (e.g. 17.30 / 27.x). If the minimap isn't resident we just skip the image.
        const UClass* texClass = UTexture2D::StaticClass();
        const UTexture2D* tex = nullptr;
        if (texClass)
            for (int i = 0; i < np && !tex; ++i)
            {
                tex = (const UTexture2D*)SDK::StaticFindObject(paths[i], texClass);
                SDK::DbgLog("[SafeZoneMap] StaticFindObject(%ls) = %p\n", paths[i], (const void*)tex);
            }
        if (!tex) // exact path missed; scan the object array by leaf name
        {
            tex = FindLoadedMinimapTexture(paths, np);
            static bool s_Diag = false;
            if (!tex && !s_Diag) { s_Diag = true; DiagnosticLogTextures(); }
        }
        if (tex)
        {
            std::vector<unsigned char> rgba; int w = 0, h = 0;
            if (ExtractToRGBA_Guarded(tex, rgba, w, h) && CreateTextureFromRGBA8(rgba.data(), w, h, dev, outSrv))
            {
                *outW = w; *outH = h;
                const std::string cacheU8 = WideToUtf8(cacheW);
                if (stbi_write_png(cacheU8.c_str(), w, h, 4, rgba.data(), w * 4))
                    SDK::DbgLog("[SafeZoneMap] dumped cache PNG -> %s\n", cacheU8.c_str());
                return true;
            }
        }
        else
        {
            SDK::DbgLog("[SafeZoneMap] FindObject(minimap) returned null\n");
        }

        // 2) Pixels produced by the game-thread load bridge (see GameThreadTick):
        // a previous Acquire posted a request, TickFlush loaded + extracted it.
        if (g_LoadState.load(std::memory_order_acquire) == (int)LoadState::Ready)
        {
            const bool ok = CreateTextureFromRGBA8(g_LoadedRGBA.data(), g_LoadedW, g_LoadedH, dev, outSrv);
            if (ok)
            {
                *outW = g_LoadedW; *outH = g_LoadedH;
                const std::string cacheU8 = WideToUtf8(cacheW);
                if (stbi_write_png(cacheU8.c_str(), g_LoadedW, g_LoadedH, 4, g_LoadedRGBA.data(), g_LoadedW * 4))
                    SDK::DbgLog("[SafeZoneMap] dumped cache PNG -> %s\n", cacheU8.c_str());
            }
            g_LoadedRGBA.clear(); g_LoadedRGBA.shrink_to_fit();
            g_LoadState.store((int)LoadState::Consumed, std::memory_order_release);
            if (ok) return true;
        }

        // 3) User-provided PNGs in Maps\ (FModel exports) - optional per-version
        // overrides for art the live path can't produce.
        if (LoadBundledMinimap(paths, np, dev, outSrv, outW, outH))
            return true;

        // 4) A PNG previously dumped by a successful live extraction.
        if (LoadPngFile(cacheW, dev, outSrv, outW, outH))
            return true;

        // 5) Nothing resident and no PNG on disk: ask the game thread to load the
        // asset properly (loading is game-thread-only; doing it here faulted on
        // 17.30/27.x). One request per session; the ~3s Acquire retry polls state.
        int expected = (int)LoadState::Idle;
        if (g_LoadState.compare_exchange_strong(expected, (int)LoadState::Requested, std::memory_order_acq_rel))
            SDK::DbgLog("[SafeZoneMap] posted game-thread load request\n");

        SDK::DbgLog("[SafeZoneMap] acquire failed; numeric fallback in use\n");
        return false;
    }
}

void GUI::SafeZoneMapGameTick()
{
    SafeZoneMap::GameThreadTick();
}

void Hyperlink(const char* label, const char* url)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 0.6f, 1.0f, 1.0f));
    ImGui::Text("%s", label);
    ImGui::PopStyleColor();

    if (ImGui::IsItemHovered())
    {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("Would you like to be redirected to this link?");
    }

    if (ImGui::IsItemClicked())
    {
        ShellExecuteA(0, "open", url, 0, 0, SW_SHOWNORMAL);
    }
}

void SmallSeparator(float Width, float Thickness = 1.0f)
{
    ImVec2 Pos = ImGui::GetCursorScreenPos();
    auto* Draw = ImGui::GetWindowDrawList();

    Draw->AddLine(Pos, ImVec2(Pos.x + Width, Pos.y), ImGui::GetColorU32(ImGuiCol_Separator), Thickness);

    ImGui::Dummy(ImVec2(Width, Thickness + 4));
}

static float ContentSectionWidth(float FallbackWidth)
{
    float Width = ImGui::GetContentRegionAvail().x;
    if (Width < FallbackWidth)
        Width = FallbackWidth;

    return Width;
}

static void SectionHeader(const char* Title, float Width)
{
    ImGui::Spacing();

    const float HeaderH = 28.f;
    const ImVec2 Pos = ImGui::GetCursorScreenPos();
    const ImVec2 End(Pos.x + Width, Pos.y + HeaderH);
    auto* Draw = ImGui::GetWindowDrawList();

    Draw->AddRectFilled(Pos, End, ImGui::GetColorU32(ImVec4(0.116f, 0.122f, 0.141f, 1.f)), 4.f);
    Draw->AddRectFilled(Pos, ImVec2(Pos.x + 3.f, End.y), ImGui::GetColorU32(Accent()), 4.f);

    ImGui::SetCursorScreenPos(ImVec2(Pos.x + 12.f, Pos.y + (HeaderH - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.82f, 0.84f, 0.90f, 1.f));
    ImGui::TextUnformatted(Title);
    ImGui::PopStyleColor();

    ImGui::SetCursorScreenPos(ImVec2(Pos.x, End.y + 8.f));
}

static void BeginSectionBody()
{
    ImGui::Indent(10.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.f, 7.f));
}

static void EndSectionBody()
{
    ImGui::PopStyleVar();
    ImGui::Unindent(10.f);
}

static bool LabeledSliderInt(const char* Label, const char* Id, int* Value, int Min, int Max, float Width)
{
    ImGui::TextUnformatted(Label);
    ImGui::SetNextItemWidth(Width);
    return ImGui::SliderInt(Id, Value, Min, Max);
}

static bool LabeledSliderFloat(const char* Label, const char* Id, float* Value, float Min, float Max, const char* Format, float Width)
{
    ImGui::TextUnformatted(Label);
    ImGui::SetNextItemWidth(Width);
    return ImGui::SliderFloat(Id, Value, Min, Max, Format);
}

static std::string FormatDurationSeconds(double Seconds)
{
    long long TotalSeconds = (long long)floor(Seconds);
    if (TotalSeconds < 0)
        TotalSeconds = 0;

    const long long Days = TotalSeconds / 86400;
    TotalSeconds %= 86400;
    const long long Hours = TotalSeconds / 3600;
    TotalSeconds %= 3600;
    const long long Minutes = TotalSeconds / 60;
    const long long RemainingSeconds = TotalSeconds % 60;

    std::string Result;
    auto AppendUnit = [&Result](long long Value, const char* Singular)
        {
            if (Value <= 0)
                return;

            if (!Result.empty())
                Result += " ";

            Result += std::to_string(Value);
            Result += " ";
            Result += Singular;
            if (Value != 1)
                Result += "s";
        };

    AppendUnit(Days, "Day");
    AppendUnit(Hours, "Hour");
    AppendUnit(Minutes, "Minute");

    if (Result.empty() || RemainingSeconds > 0)
        AppendUnit(RemainingSeconds, "Second");

    return Result;
}

// One full-width vertical tab in the left sidebar. Sets *activeUI to uiValue on click.
static void SidebarTab(const char* label, int uiValue, float yPos, float tabH, int* activeUI)
{
    ImGui::PushID(uiValue);
    const bool active = (*activeUI == uiValue);

    ImGui::SetCursorPos(ImVec2(0.f, yPos));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const ImVec2 size(ImGui::GetContentRegionAvail().x, tabH);

    if (ImGui::InvisibleButton("##tab", size))
        *activeUI = uiValue;
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active)
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(Accent(0.12f)));
    else if (hovered)
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), ImGui::GetColorU32(Accent(0.06f)));

    ImVec4 textCol = active ? Accent() : ImVec4(0.60f, 0.63f, 0.69f, 1.f);
    if (!active && hovered) textCol = ImVec4(0.85f, 0.87f, 0.92f, 1.f);

    const ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p.x + 26.f, p.y + (size.y - ts.y) * 0.5f), ImGui::GetColorU32(textCol), label);

    ImGui::PopID();
}

static const char* GetSelectedPlaylistModeName()
{
    switch (GUI::SelectedPlaylist)
    {
    case (int)Playlist::Solos:
        return "Solos";
    case (int)Playlist::Duos:
        return "Duos";
    case (int)Playlist::Trios:
        return "Trios";
    case (int)Playlist::Squads:
        return "Squads";
    case (int)Playlist::ZBSolos:
        return "Zero Build Solos";
    case (int)Playlist::ZBDuos:
        return "Zero Build Duos";
    case (int)Playlist::ZBTrios:
        return "Zero Build Trios";
    case (int)Playlist::ZBSquads:
        return "Zero Build Squads";
    case (int)Playlist::Playground:
        return "Playground";
    case (int)Playlist::Creative:
        return "Creative";
    case (int)Playlist::OneShotSolos:
        return "One Shot Solos";
    case (int)Playlist::OneShotDuos:
        return "One Shot Duos";
    case (int)Playlist::OneShotSquads:
        return "One Shot Squads";
    case (int)Playlist::SiphonSolos:
        return "Siphon Solos";
    case (int)Playlist::SiphonDuos:
        return "Siphon Duos";
    case (int)Playlist::SiphonSquads:
        return "Siphon Squads";
    case (int)Playlist::UnvSolos:
        return "Unvaulted Solos";
    case (int)Playlist::UnvDuos:
        return "Unvaulted Duos";
    case (int)Playlist::UnvTrios:
        return "Unvaulted Trios";
    case (int)Playlist::UnvSquads:
        return "Unvaulted Squads";
    case (int)Playlist::SlideSolos:
        return "Slide Solos";
    case (int)Playlist::SlideDuos:
        return "Slide Duos";
    case (int)Playlist::FILSolos:
        return "Floor Is Lava Solos";
    case (int)Playlist::FILDuos:
        return "Floor Is Lava Duos";
    case (int)Playlist::FILSquads:
        return "Floor Is Lava Squads";
    case (int)Playlist::TournamentSolos:
        return "Tournament Solos";
    case (int)Playlist::TournamentDuos:
        return "Tournament Duos";
    case (int)Playlist::TournamentTrios:
        return "Tournament Trios";
    case (int)Playlist::TournamentSquads:
        return "Tournament Squads";
    case (int)Playlist::ArenaSolos:
        return "Arena Solos";
    case (int)Playlist::ArenaDuos:
        return "Arena Duos";
    case (int)Playlist::ArenaTrios:
        return "Arena Trios";
    case (int)Playlist::ArenaSquads:
        return "Arena Squads";
    case (int)Playlist::ArenaZBSolos:
        return "Arena Zero Build Solos";
    case (int)Playlist::ArenaZBDuos:
        return "Arena Zero Build Duos";
    case (int)Playlist::ArenaZBTrios:
        return "Arena Zero Build Trios";
    case (int)Playlist::ArenaZBSquads:
        return "Arena Zero Build Squads";
    case (int)Playlist::Gav:
        return "Gav 1v1 Map";
    case (int)Playlist::Retrac1v1:
        return "Retrac 1v1 Map";
    case (int)Playlist::RetracTurtle:
        return "Retrac Turtle Fights";
    case (int)Playlist::RetracWater:
        return "Retrac Water Map";
    case (int)Playlist::TiltedZW:
        return "Tilted FFA";
    case (int)Playlist::OnlyUp:
        return "Only Up Map";
    case (int)Playlist::Twine1v1:
        return "Twine 1v1 Map";
    case (int)Playlist::Boxfight:
        return "Boxfights";
    case (int)Playlist::Backrooms:
        return "Backrooms Map";
    case (int)Playlist::Event:
        return "Event Playlist";
    case (int)Playlist::Custom:
        return "Custom";
    default:
        return "Unknown";
    }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam);
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}

auto WindowWidth = 800;
auto WindowHeight = 600;

inline std::vector<std::pair<AFortPlayerControllerAthena*, UNetConnection*>> AllControllers;

namespace TrickshotManager
{
    namespace fs = std::filesystem;

    inline fs::path GetDirectory()
    {
        char Path[MAX_PATH]{};
        if (FAILED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, Path)))
            return {};

        fs::path Directory = fs::path(Path) / "Magnesium" / "trickshots";
        std::error_code Error;
        fs::create_directories(Directory, Error);
        return Error ? fs::path{} : Directory;
    }

    inline std::string SanitizeName(const char* Name)
    {
        std::string Result = Name ? Name : "";
        Result.erase(Result.begin(), std::find_if(Result.begin(), Result.end(), [](unsigned char Character) { return !std::isspace(Character); }));
        Result.erase(std::find_if(Result.rbegin(), Result.rend(), [](unsigned char Character) { return !std::isspace(Character); }).base(), Result.end());

        for (auto& Character : Result)
        {
            if (Character == '<' || Character == '>' || Character == ':' || Character == '"' || Character == '/' ||
                Character == '\\' || Character == '|' || Character == '?' || Character == '*')
                Character = '_';
        }

        return Result;
    }

    inline std::string GetCurrentMapPath()
    {
        auto World = UWorld::GetWorld();
        return World ? FStringToStdString(UKismetSystemLibrary::GetPathName(World)) : "";
    }

    inline AFortPlayerControllerAthena* GetHostController()
    {
        auto World = UWorld::GetWorld();
        auto Driver = World ? static_cast<UNetDriver*>(World->NetDriver) : nullptr;
        if (!Driver)
            return nullptr;

        for (auto Connection : Driver->ClientConnections)
        {
            if (Connection && Connection->PlayerController)
                return static_cast<AFortPlayerControllerAthena*>(Connection->PlayerController);
        }
        return nullptr;
    }

    inline uint8 GuessAttachmentType(const std::string& ClassPath)
    {
        std::string Lower = ClassPath;
        std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](unsigned char Character) { return static_cast<char>(std::tolower(Character)); });
        if (Lower.find("wall") != std::string::npos)
            return 1;
        if (Lower.find("ceiling") != std::string::npos)
            return 2;
        if (Lower.find("stair") != std::string::npos)
            return 8;
        return 0;
    }

    inline std::vector<std::string> GetSavedNames()
    {
        std::vector<std::string> Names;
        auto Directory = GetDirectory();
        if (Directory.empty())
            return Names;

        std::error_code Error;
        for (const auto& Entry : fs::directory_iterator(Directory, Error))
        {
            if (Entry.is_regular_file() && Entry.path().extension() == ".json")
                Names.push_back(Entry.path().stem().string());
        }
        std::sort(Names.begin(), Names.end());
        return Names;
    }

    inline bool Delete(const std::string& Name, std::string& Message)
    {
        const auto Directory = GetDirectory();
        if (Name.empty() || Directory.empty())
        {
            Message = "Select a saved trickshot to delete.";
            return false;
        }

        std::error_code Error;
        const bool Removed = fs::remove(Directory / (Name + ".json"), Error);
        Message = Removed && !Error ? "Deleted " + Name + "." : "Could not delete the selected trickshot.";
        return Removed && !Error;
    }

    inline bool OpenDirectory(std::string& Message)
    {
        const auto Directory = GetDirectory();
        if (Directory.empty())
        {
            Message = "Could not open the trickshot folder.";
            return false;
        }

        const auto Result = ShellExecuteA(nullptr, "open", Directory.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        const bool Opened = reinterpret_cast<INT_PTR>(Result) > 32;
        if (!Opened)
            Message = "Could not open the trickshot folder.";
        return Opened;
    }

    inline bool Save(const char* Name, std::string& Message)
    {
        const std::string SafeName = SanitizeName(Name);
        const auto Directory = GetDirectory();
        if (SafeName.empty() || Directory.empty() || !UWorld::GetWorld())
        {
            Message = SafeName.empty() ? "Enter a trickshot name." : "Unable to access the trickshot folder or world.";
            return false;
        }

        nlohmann::json Root;
        Root["version"] = 1;
        Root["name"] = SafeName;
        Root["map"] = GetCurrentMapPath();
        Root["builds"] = nlohmann::json::array();

        TArray<ABuildingSMActor*> Builds;
        Utils::GetAll<ABuildingSMActor>(Builds);
        std::vector<ABuildingSMActor*> SavedBuilds;

        // Traps and other decos are represented as actors attached to a supporting
        // build. Trickshot files intentionally persist structural player builds only.
        std::unordered_set<ABuildingSMActor*> AttachedActors;
        for (auto Build : Builds)
        {
            if (!Build || !Build->HasAttachedBuildingActors())
                continue;
            for (auto Attached : Build->AttachedBuildingActors)
                if (Attached)
                    AttachedActors.insert(Attached);
        }
        for (auto Build : Builds)
        {
            if (!Build || !Build->bPlayerPlaced || Build->bDestroyed || AttachedActors.contains(Build))
                continue;

            SavedBuilds.push_back(Build);
        }

        for (auto Build : SavedBuilds)
        {

            const FVector Location = Build->K2_GetActorLocation();
            const FRotator Rotation = Build->K2_GetActorRotation();
            const std::string ClassPath = FStringToStdString(UKismetSystemLibrary::GetPathName(Build->Class));
            Root["builds"].push_back({
                { "class", ClassPath },
                { "location", { Location.X, Location.Y, Location.Z } },
                { "rotation", { Rotation.Pitch, Rotation.Yaw, Rotation.Roll } },
                { "level", Build->CurrentBuildingLevel },
                { "parent", -1 }
            });
        }
        Builds.Free();

        std::ofstream File(Directory / (SafeName + ".json"), std::ios::trunc);
        if (!File)
        {
            Message = "Could not create the trickshot file.";
            return false;
        }
        File << Root.dump(4);
        Message = "Saved " + std::to_string(Root["builds"].size()) + " player builds.";
        return true;
    }

    inline bool Load(const std::string& Name, std::string& Message)
    {
        const auto Directory = GetDirectory();
        auto Controller = GetHostController();
        if (Name.empty() || Directory.empty() || !Controller)
        {
            Message = Name.empty() ? "Select a saved trickshot." : "A connected player is required to load builds.";
            return false;
        }

        try
        {
            std::ifstream File(Directory / (Name + ".json"));
            nlohmann::json Root;
            if (!File || !(File >> Root) || !Root.contains("builds") || !Root["builds"].is_array())
            {
                Message = "The selected trickshot file is invalid.";
                return false;
            }

            if (Root.value("map", "") != GetCurrentMapPath())
            {
                Message = "This trickshot was saved on a different map.";
                return false;
            }

            struct PendingBuild { UClass* Class; FVector Location; FRotator Rotation; int Level; int Parent; uint8 AttachmentType; std::string ItemDefinition; };
            std::vector<PendingBuild> Pending;
            for (const auto& SavedBuild : Root["builds"])
            {
                // Backward compatibility: silently ignore traps/decos from earlier
                // experimental save files. Only structural builds are restored.
                if (SavedBuild.value("parent", -1) >= 0)
                    continue;
                const auto ClassPath = SavedBuild.at("class").get<std::string>();
                // Player building classes are already resident while their map is active. Do not use
                // StaticLoadObject here: its native signature varies by Fortnite version and can fault
                // on versions where the SDK's resolved loader does not match the running executable.
                UEAllocatedWString WideClassPath(ClassPath.begin(), ClassPath.end());
                UClass* Class = (UClass*)SDK::StaticFindObject(WideClassPath.c_str(), UClass::StaticClass());
                const auto& L = SavedBuild.at("location");
                const auto& R = SavedBuild.at("rotation");
                if (L.size() != 3 || R.size() != 3)
                    throw std::runtime_error("invalid building data");
                if (Class && (!Class->GetDefaultObj() || !Class->GetDefaultObj()->IsA(ABuildingSMActor::StaticClass())))
                    Class = nullptr;
                Pending.push_back({ Class, FVector(L[0].get<double>(), L[1].get<double>(), L[2].get<double>()),
                    FRotator(R[0].get<double>(), R[1].get<double>(), R[2].get<double>()), SavedBuild.value("level", 0),
                    SavedBuild.value("parent", -1), SavedBuild.value("attachmentType", GuessAttachmentType(ClassPath)),
                    SavedBuild.value("itemDefinition", "") });
            }

            TArray<ABuildingSMActor*> ExistingBuilds;
            Utils::GetAll<ABuildingSMActor>(ExistingBuilds);
            for (auto Build : ExistingBuilds)
                if (Build && Build->bPlayerPlaced)
                    Build->SilentDie(true);
            ExistingBuilds.Free();

            int Loaded = 0;
            int Skipped = 0;
            std::vector<ABuildingSMActor*> SpawnedBuilds(Pending.size(), nullptr);

            auto ApplyOwnership = [&](ABuildingSMActor* Build)
            {
                if (!Build)
                    return;
                Build->bPlayerPlaced = true;
                if (Controller->PlayerState)
                {
                    auto PlayerState = static_cast<AFortPlayerStateAthena*>(Controller->PlayerState);
                    if (PlayerState->HasTeamIndex())
                        Build->Team = PlayerState->TeamIndex;
                    if (Build->HasTeamIndex())
                        Build->TeamIndex = Build->Team;
                    if (Build->HasOwnerPersistentID() && PlayerState->HasWorldPlayerId())
                        Build->OwnerPersistentID = PlayerState->WorldPlayerId;
                    if (PlayerState->HasTeamIndex())
                        Build->SetTeam(PlayerState->TeamIndex);
                }
                Build->ForceNetUpdate();
            };

            // Structural supports must exist before Fortnite's native trap placement path runs.
            for (int Index = 0; Index < Pending.size(); ++Index)
            {
                const auto& SavedBuild = Pending[Index];
                if (SavedBuild.Parent >= 0)
                    continue;
                if (!SavedBuild.Class)
                {
                    ++Skipped;
                    continue;
                }
                auto Build = UWorld::SpawnActorUnfinished<ABuildingSMActor>(SavedBuild.Class, SavedBuild.Location, SavedBuild.Rotation, Controller);
                if (Build)
                {
                    Build->InitializeKismetSpawnedBuildingActor(Build, Controller, true, nullptr, false);
                    UWorld::FinishSpawnActor(Build, SavedBuild.Location, SavedBuild.Rotation);
                }
                if (!Build)
                {
                    ++Skipped;
                    continue;
                }
                int BuildingLevel = SavedBuild.Level;
                Build->CurrentBuildingLevel = BuildingLevel;
                Build->OnRep_CurrentBuildingLevel();
                ApplyOwnership(Build);
                SpawnedBuilds[Index] = Build;
                ++Loaded;
            }

            for (int Index = 0; Index < Pending.size(); ++Index)
            {
                const auto& SavedBuild = Pending[Index];
                if (SavedBuild.Parent < 0)
                    continue;
                if (!SavedBuild.Class || SavedBuild.Parent >= SpawnedBuilds.size() || !SpawnedBuilds[SavedBuild.Parent])
                {
                    ++Skipped;
                    continue;
                }
                UEAllocatedWString ItemDefinitionPath(SavedBuild.ItemDefinition.begin(), SavedBuild.ItemDefinition.end());
                auto Trap = ABuildingSMActor::SpawnSavedTrap(SavedBuild.Class, SavedBuild.Location, SavedBuild.Rotation,
                    SpawnedBuilds[SavedBuild.Parent], SavedBuild.AttachmentType, Controller,
                    SavedBuild.ItemDefinition.empty() ? nullptr : ItemDefinitionPath.c_str());
                if (!Trap)
                {
                    ++Skipped;
                    continue;
                }
                ApplyOwnership(Trap);
                SpawnedBuilds[Index] = Trap;
                ++Loaded;
            }

            // Preserve the supporting-build relationship used by traps. This lets ownership,
            // destruction, and replication continue to follow the restored parent build.
            for (int Index = 0; Index < Pending.size(); ++Index)
            {
                const int ParentIndex = Pending[Index].Parent;
                if (ParentIndex < 0 || ParentIndex >= SpawnedBuilds.size() || !SpawnedBuilds[Index] || !SpawnedBuilds[ParentIndex])
                    continue;
                if (SpawnedBuilds[ParentIndex]->HasAttachedBuildingActors())
                {
                    bool AlreadyAttached = false;
                    for (auto Attached : SpawnedBuilds[ParentIndex]->AttachedBuildingActors)
                        AlreadyAttached = AlreadyAttached || Attached == SpawnedBuilds[Index];
                    if (!AlreadyAttached)
                        SpawnedBuilds[ParentIndex]->AttachedBuildingActors.Add(SpawnedBuilds[Index]);
                }
            }
            Message = "Loaded " + std::to_string(Loaded) + " player builds.";
            if (Skipped > 0)
                Message += " Skipped " + std::to_string(Skipped) + " unavailable actors.";
            return true;
        }
        catch (const std::exception& Error)
        {
            Message = std::string("Failed to load trickshot: ") + Error.what();
            return false;
        }
    }
}

void GUI::Init()
{
    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASS wc{};
    wc.lpszClassName = L"ErbiumWC";
    wc.lpfnWndProc = WndProc;
    RegisterClass(&wc);

    wchar_t buffer[67];
    swprintf_s(buffer, VersionInfo.EngineVersion >= 5.0 ? L"Magnesium (FN %.2f, UE %.1f)" : (VersionInfo.FortniteVersion >= 5.00 || VersionInfo.FortniteVersion < 1.2 ? L"Magnesium (FN %.2f, UE %.2f)" : L"Magnesium (FN %.1f, UE %.2f)"), VersionInfo.FortniteVersion, VersionInfo.EngineVersion);
    auto hWnd = CreateWindow(wc.lpszClassName, buffer, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME, 100, 100, (int)(WindowWidth * main_scale), (int)(WindowHeight * main_scale), nullptr, nullptr, nullptr, nullptr);

    IDXGISwapChain* g_pSwapChain = nullptr;
    ID3D11Device* g_pd3dDevice = nullptr;
    ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;

    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    //createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return;

    LoadTextureFromMemory(embedded_image, sizeof(embedded_image), g_pd3dDevice, &g_EmbedTexture, &EmbedWidth, &EmbedHeight);
    LoadTextureFromMemory(Icon, sizeof(Icon), g_pd3dDevice, &g_LogoTexture, &g_LogoW, &g_LogoH);

    ID3D11RenderTargetView* g_mainRenderTargetView;

    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();

    ShowWindow(hWnd, SW_SHOWDEFAULT);
    UpdateWindow(hWnd);
    DWORD dwMyID = ::GetCurrentThreadId();
    DWORD dwCurID = ::GetWindowThreadProcessId(hWnd, NULL);
    AttachThreadInput(dwCurID, dwMyID, TRUE);
    SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
    SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOSIZE | SWP_NOMOVE);
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);
    SetActiveWindow(hWnd);
    AttachThreadInput(dwCurID, dwMyID, FALSE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.IniFilename = NULL;
    //io.DisplaySize = ImGui::GetMainViewport()->Size;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImFontConfig FontConfig;
    FontConfig.FontDataOwnedByAtlas = false;
    ImGui::GetIO().Fonts->AddFontFromMemoryTTF((void*)font, sizeof(font), 17.f, &FontConfig);

    auto& mStyle = ImGui::GetStyle();
    mStyle.WindowRounding = 0.f;
    mStyle.ItemSpacing = ImVec2(20, 6);
    mStyle.ItemInnerSpacing = ImVec2(8, 4);
    mStyle.FrameRounding = 4.5f;
    mStyle.GrabMinSize = 14.0f;
    mStyle.GrabRounding = 16.0f;
    mStyle.ScrollbarSize = 18.0f;
    mStyle.ScrollbarRounding = 16.0f;

    ImGuiStyle& style = mStyle;
    auto C = [](float r, float g, float b, float a = 1.f) { return ImVec4(r, g, b, a); };
    ImVec4* col = style.Colors;

    // Neutral graphite dark/grey theme (slight cool tint), ATLAS-style spread.
    col[ImGuiCol_WindowBg]             = C(0.090f, 0.094f, 0.106f, 1.00f); // graphite background
    col[ImGuiCol_ChildBg]              = C(0.090f, 0.094f, 0.106f, 1.00f);
    col[ImGuiCol_PopupBg]              = C(0.063f, 0.067f, 0.078f, 0.98f); // bar
    col[ImGuiCol_Border]               = C(0.196f, 0.204f, 0.227f, 1.00f);
    col[ImGuiCol_BorderShadow]         = C(0.f, 0.f, 0.f, 0.f);
    col[ImGuiCol_Text]                 = C(0.882f, 0.894f, 0.918f, 1.00f);
    col[ImGuiCol_TextDisabled]         = C(0.435f, 0.451f, 0.490f, 1.00f);
    col[ImGuiCol_TextSelectedBg]       = Accent(0.28f);
    col[ImGuiCol_FrameBg]              = C(0.137f, 0.145f, 0.165f, 1.00f);
    col[ImGuiCol_FrameBgHovered]       = C(0.176f, 0.184f, 0.208f, 1.00f);
    col[ImGuiCol_FrameBgActive]        = C(0.216f, 0.227f, 0.255f, 1.00f);
    col[ImGuiCol_TitleBg]              = C(0.063f, 0.067f, 0.078f, 1.00f);
    col[ImGuiCol_TitleBgActive]        = C(0.063f, 0.067f, 0.078f, 1.00f);
    col[ImGuiCol_TitleBgCollapsed]     = C(0.063f, 0.067f, 0.078f, 0.90f);
    col[ImGuiCol_MenuBarBg]            = C(0.063f, 0.067f, 0.078f, 1.00f);
    col[ImGuiCol_CheckMark]            = Accent();
    col[ImGuiCol_SliderGrab]           = Accent();
    col[ImGuiCol_SliderGrabActive]     = AccentDk();
    col[ImGuiCol_Button]               = C(0.157f, 0.165f, 0.188f, 1.00f);
    col[ImGuiCol_ButtonHovered]        = C(0.216f, 0.227f, 0.255f, 1.00f);
    col[ImGuiCol_ButtonActive]         = C(0.255f, 0.267f, 0.298f, 1.00f);
    col[ImGuiCol_Header]               = Accent(0.14f);
    col[ImGuiCol_HeaderHovered]        = Accent(0.22f);
    col[ImGuiCol_HeaderActive]         = Accent(0.30f);
    col[ImGuiCol_Separator]            = C(0.196f, 0.204f, 0.227f, 1.00f);
    col[ImGuiCol_SeparatorHovered]     = Accent(0.40f);
    col[ImGuiCol_SeparatorActive]      = Accent(0.70f);
    col[ImGuiCol_ScrollbarBg]          = C(0.063f, 0.067f, 0.078f, 0.40f);
    col[ImGuiCol_ScrollbarGrab]        = C(0.196f, 0.204f, 0.227f, 1.00f);
    col[ImGuiCol_ScrollbarGrabHovered] = C(0.255f, 0.267f, 0.298f, 1.00f);
    col[ImGuiCol_ScrollbarGrabActive]  = C(0.318f, 0.333f, 0.369f, 1.00f);
    col[ImGuiCol_Tab]                  = C(0.063f, 0.067f, 0.078f, 1.00f);
    col[ImGuiCol_TabHovered]           = Accent(0.18f);
    col[ImGuiCol_TabSelected]          = C(0.090f, 0.094f, 0.106f, 1.00f);
    col[ImGuiCol_PlotLines]            = Accent();
    col[ImGuiCol_PlotHistogram]        = Accent();
    //ImGui::StyleColorsDark();

    //ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;

    ImGui_ImplWin32_Init(hWnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    bool done = false;
    bool g_SwapChainOccluded = false;

    while (!done)
    {
        MSG msg;

        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }

        if (done)
            break;

        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            Sleep(10);
            continue;
        }

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            g_mainRenderTargetView->Release();

            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;

            ID3D11Texture2D* pBackBuffer;
            g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
            g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
            pBackBuffer->Release();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        if (FConfiguration::bAutoStartEvent && !FConfiguration::bEventStarted && FConfiguration::EventStartBaseTime > 0.f)
        {
            float CurrentTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
            if (CurrentTime >= FConfiguration::EventStartBaseTime + FConfiguration::EventStartTime)
            {
                printf("[Events] Auto-starting event at T=%.1f\n", CurrentTime);
                Events::StartEvent();
            }
        }

        main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(WindowWidth * main_scale, WindowHeight * main_scale), ImGuiCond_Always);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::Begin("Magnesium", nullptr, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();

        const float W = ImGui::GetWindowWidth();
        const float H = ImGui::GetWindowHeight();
        const ImVec2 wp = ImGui::GetWindowPos();
        const float TopBarH = 48.f;
        const float SidebarW = 150.f;

        int SelectedUI = 0;
        int hasEvent = 0;
        if (hasEvent == 0)
        {
            hasEvent = 1;
            for (auto& Event : Events::EventsArray)
            {
                if (Event.EventVersion != VersionInfo.FortniteVersion)
                    continue;
                hasEvent = 2;
            }
        }

        // ---- Top bar (#284a2c): logo + branding ----
        ImGui::GetWindowDrawList()->AddRectFilled(wp, ImVec2(wp.x + W, wp.y + TopBarH),
            ImGui::GetColorU32(ImVec4(0.063f, 0.067f, 0.078f, 1.f)));
        {
            const float LogoSize = 32.f;
            const float PadL = 14.f;
            ImGui::SetCursorPos(ImVec2(PadL, (TopBarH - LogoSize) * 0.5f));
            if (g_LogoTexture)
                ImGui::Image((void*)g_LogoTexture, ImVec2(LogoSize, LogoSize));
            else
                ImGui::Dummy(ImVec2(LogoSize, LogoSize));

            ImGui::SameLine(PadL + LogoSize + 10.f);
            const float TitleY = (TopBarH - ImGui::GetTextLineHeight()) * 0.5f;
            ImGui::SetCursorPosY(TitleY);
            ImGui::PushStyleColor(ImGuiCol_Text, Accent());
            ImGui::Text("MAGNESIUM");
            ImGui::PopStyleColor();
            ImGui::SameLine(0.f, 7.f);
            ImGui::SetCursorPosY(TitleY);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.62f, 0.68f, 1.f));
            ImGui::Text("| Gameserver");
            ImGui::PopStyleColor();
            ImGui::SameLine(0.f, 8.f);
            ImGui::SetCursorPosY(TitleY);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.46f, 0.48f, 0.54f, 1.f));
            ImGui::TextUnformatted("v2.1.0");
            ImGui::PopStyleColor();

            // FN / UE versions on the right, aligned to the visible viewport so they
            // never run off the window edge.
            char ver[48];
            snprintf(ver, sizeof(ver), "FN %.2f  \xC2\xB7  UE %.2f", VersionInfo.FortniteVersion, VersionInfo.EngineVersion);
            const float verW = ImGui::CalcTextSize(ver).x;
            const float rightEdge = ImGui::GetIO().DisplaySize.x;
            ImGui::SetCursorPos(ImVec2(rightEdge - verW - 18.f, TitleY));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.54f, 0.56f, 0.62f, 0.92f));
            ImGui::TextUnformatted(ver);
            ImGui::PopStyleColor();
        }

        // divider lines (drawn on top so child backgrounds don't cover them)
        {
            ImDrawList* fdl = ImGui::GetForegroundDrawList();
            const ImU32 lineCol = IM_COL32(50, 52, 58, 255);
            fdl->AddLine(ImVec2(wp.x, wp.y + TopBarH), ImVec2(wp.x + W, wp.y + TopBarH), lineCol, 1.f);
            fdl->AddLine(ImVec2(wp.x + SidebarW, wp.y + TopBarH), ImVec2(wp.x + SidebarW, wp.y + H), lineCol, 1.f);
        }

        // ---- Sidebar (#284a2c): vertical tabs replacing the old tab bar ----
        static int s_ActiveUI = 0;
        ImGui::SetCursorPos(ImVec2(0.f, TopBarH));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.063f, 0.067f, 0.078f, 1.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::BeginChild("##sidebar", ImVec2(SidebarW, H - TopBarH), false, ImGuiWindowFlags_NoScrollbar);
        {
            const float TabH = 38.f;
            const float TabsTop = 12.f;
            float yy = TabsTop;
            float activeY = -1.f;
            const bool inMatch = !FConfiguration::bReadyToStart;

            struct TabDef { const char* label; int ui; bool show; };
            TabDef tabs[] = {
                { "Match",      0, true },
                { "Lategame",   3, inMatch },
                { "Playlist",   1, inMatch },
                { "Creative",   5, inMatch && SelectedPlaylist == static_cast<int>(Playlist::Creative) },
                { "Custom Map", 6, inMatch && FConfiguration::bIsCustomMap },
                { "Player Bot", 4, inMatch },
                { "Players",    2, gsStatus >= Joinable },
                { "Trickshot",  7, FConfiguration::bEnableTrickshotTab },
                { "Credits",    8, true },
            };

            for (auto& t : tabs)
            {
                if (!t.show) continue;
                SidebarTab(t.label, t.ui, yy, TabH, &s_ActiveUI);
                if (s_ActiveUI == t.ui) activeY = yy + TabH * 0.5f;
                yy += TabH;
            }

            if (activeY < 0.f) { s_ActiveUI = 0; activeY = TabsTop + TabH * 0.5f; }

            static float s_IndY = -1.f;
            if (s_IndY < 0.f) s_IndY = activeY;
            float lerp = ImGui::GetIO().DeltaTime * 16.f; if (lerp > 1.f) lerp = 1.f;
            s_IndY += (activeY - s_IndY) * lerp;
            const ImVec2 sbPos = ImGui::GetWindowPos();
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImVec2(sbPos.x, sbPos.y + s_IndY - 9.f), ImVec2(sbPos.x + 3.f, sbPos.y + s_IndY + 9.f),
                ImGui::GetColorU32(Accent()), 2.f);

            // Primary CTA pinned to the bottom of the sidebar: start the server.
            if (gsStatus == NotReady && !FConfiguration::bReadyToStart)
            {
                const float bMargin = 12.f;
                const float bH = 40.f;
                const float bW = SidebarW - bMargin * 2.f;
                // Anchor to the visible viewport height (the window is taller than the
                // actual client area, so using H would push this off-screen).
                const float visH = ImGui::GetIO().DisplaySize.y;
                ImGui::SetCursorPos(ImVec2(bMargin, (visH - TopBarH) - bH - bMargin));
                const ImVec2 bp = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##startserver", ImVec2(bW, bH)))
                    FConfiguration::bReadyToStart = true;
                const bool bHov = ImGui::IsItemHovered();
                const bool bAct = ImGui::IsItemActive();
                ImDrawList* bdl = ImGui::GetWindowDrawList();
                const ImVec4 fillC = bAct ? ImVec4(0.62f, 0.66f, 0.78f, 1.f)
                                    : (bHov ? ImVec4(0.88f, 0.91f, 0.97f, 1.f) : Accent());
                bdl->AddRectFilled(bp, ImVec2(bp.x + bW, bp.y + bH), ImGui::GetColorU32(fillC), 6.f);

                const char* bLbl = "START SERVER";
                const ImVec2 bts = ImGui::CalcTextSize(bLbl);
                const ImVec2 tpos(bp.x + (bW - bts.x) * 0.5f, bp.y + (bH - bts.y) * 0.5f);
                const ImU32 tcol = IM_COL32(16, 18, 22, 255);
                bdl->AddText(tpos, tcol, bLbl);
                bdl->AddText(ImVec2(tpos.x + 1.f, tpos.y), tcol, bLbl); // faux-bold
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        SelectedUI = s_ActiveUI;

        // ---- Content panel (inset; transparent so the #32703b background shows) ----
        const float ContentPadX = 22.f;
        ImGui::SetCursorPos(ImVec2(SidebarW + ContentPadX, TopBarH + 14.f));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.f, 0.f, 0.f, 0.f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
        ImGui::BeginChild("##content", ImVec2((W - SidebarW) - ContentPadX * 2.f, (H - TopBarH) - 26.f), false);
        float Width = 260.0f;
        float Height = 0.0f;
        const float SectionWidth = ContentSectionWidth(Width);

        static char commandBuffer[1024] = { 0 };
        static char playlistBuffer[1024] = { 0 };
        switch (SelectedUI)
        {
        case 0:
        {
            SectionHeader("Match Information", SectionWidth);
            BeginSectionBody();

            ImGui::Text("- Status: ");
            ImGui::SameLine(0.0f, 0.0f);

            ImVec4 Color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // default white

            if (!FConfiguration::bReadyToStart)
            {
                Color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f); // gray
                ImGui::TextColored(Color, "Configuring...");
            }
            else if (gsStatus == NotReady)
            {
                Color = ImVec4(1.0f, 1.0f, 0.0f, 1.0f); // yellow
                ImGui::TextColored(Color, "Setting up the server...");
            }
            else if (gsStatus == Joinable)
            {
                Color = ImVec4(0.0f, 1.0f, 0.0f, 1.0f); // green
                ImGui::TextColored(Color, "Joinable!");
            }
            else if (gsStatus == StartedMatch)
            {
                Color = ImVec4(1.0f, 0.65f, 0.0f, 1.0f); // orange
                ImGui::TextColored(Color, "Match Started.");
            }
            else if (gsStatus == Ended)
            {
                Color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f); // red
                ImGui::TextColored(Color, "Match Ended.");
            }
            else
            {
                ImGui::TextColored(Color, "N/A");
            }

            ImGui::Text("- Mode: %s", GetSelectedPlaylistModeName());
            if (FConfiguration::bInfiniteRender)
            {
                ImGui::TextUnformatted("- Infinite Render: ");
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "Enabled");
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextUnformatted(" (only works on the last player to join!)");
            }
            ImGui::Text("- Server Port: %d", FConfiguration::Port);
            ImGui::Text("- Server Tick Rate: %.1f", FConfiguration::MaxTickRate);

            if (gsStatus >= Joinable)
            {
                auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;

                int AliveCount = 0;

                if (GameMode)
                    AliveCount = GameMode->AlivePlayers.Num();

                ImGui::Text("- Players: %d", AliveCount);

                const std::string Uptime = FormatDurationSeconds(GameMode ? UGameplayStatics::GetTimeSeconds(GameMode) : 0.0);
                ImGui::Text("- Uptime: %s", Uptime.c_str());

                static std::string LastElimStatusMessage;
                static std::chrono::high_resolution_clock::time_point AddMessageTime;

                if (!FConfiguration::ElimStatusMessage.empty() && FConfiguration::ElimStatusMessage != LastElimStatusMessage)
                {
                    LastElimStatusMessage = FConfiguration::ElimStatusMessage;
                    AddMessageTime = std::chrono::high_resolution_clock::now();
                }

                if (!LastElimStatusMessage.empty() && duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now() - AddMessageTime).count() < 120)
                {
                    ImVec4 KillerColor = ImVec4(0x4e / 255.f, 0x86 / 255.f, 0xa5 / 255.f, 1.0f);
                    ImVec4 EliminatedColor = ImVec4(0xa5 / 255.f, 0x56 / 255.f, 0x4c / 255.f, 1.0f);
                    ImVec4 WeaponColor = ImVec4(1.0f, 0.84f, 0.0f, 1.0f);

                    ImGui::TextUnformatted("- ");
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextColored(KillerColor, "%s", FConfiguration::ElimKillerName.c_str());
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextUnformatted(" eliminated ");
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextColored(EliminatedColor, "%s", FConfiguration::ElimEliminatedName.c_str());
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextUnformatted(" from ");
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::Text("%sm!", FConfiguration::ElimDistance.c_str());
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextUnformatted(" (");
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextColored(WeaponColor, "%s", FConfiguration::ElimWeaponName.c_str());
                    ImGui::SameLine(0.0f, 0.0f);

                    ImGui::TextUnformatted(")");

                    static bool bHasLogged = false;

                    if (!bHasLogged)
                    {
                        printf("- %s eliminated %s from %sm! (%s)\n", FConfiguration::ElimKillerName.c_str(), FConfiguration::ElimEliminatedName.c_str(), FConfiguration::ElimDistance.c_str(), FConfiguration::ElimWeaponName.c_str());
                        bHasLogged = true;
                    }
                }
                else
                {
                    LastElimStatusMessage.clear();
                    FConfiguration::ElimStatusMessage.clear();
                }
            }

            EndSectionBody();

            bool bIsGavMap = (SelectedPlaylist == static_cast<int>(Playlist::Gav));
            bool bIsEventPlaylist = (SelectedPlaylist == static_cast<int>(Playlist::Event));
            bool bIsRetrac1v1 = (SelectedPlaylist == static_cast<int>(Playlist::Retrac1v1));
            bool bIsRetracTurtle = (SelectedPlaylist == static_cast<int>(Playlist::RetracTurtle));
            bool bIsCreative = (SelectedPlaylist == static_cast<int>(Playlist::Creative));
            bool bIsOnlyUp = (SelectedPlaylist == static_cast<int>(Playlist::OnlyUp));
            bool bIsTiltedZW = (SelectedPlaylist == static_cast<int>(Playlist::TiltedZW));
            bool bIsTwine = (SelectedPlaylist == static_cast<int>(Playlist::Twine1v1));
            bool bIsBoxfight = (SelectedPlaylist == static_cast<int>(Playlist::Boxfight));
            bool bIsBackrooms = (SelectedPlaylist == static_cast<int>(Playlist::Backrooms));
            bool bShowsOnlyUpPreGameConfig = bIsOnlyUp && gsStatus < Joinable;
            bool bShowsDefaultPreGameConfig = !bIsGavMap && !bIsEventPlaylist && !bIsRetrac1v1 && !bIsRetracTurtle && !bIsCreative && !bIsOnlyUp && !bIsTiltedZW && !bIsTwine && !bIsBoxfight && !bIsBackrooms;
            const bool bEventStartsOnSpawnIsland =
                VersionInfo.FortniteVersion <= 4.50 ||
                VersionInfo.FortniteVersion == 6.21 ||
                VersionInfo.FortniteVersion == 7.20 ||
                VersionInfo.FortniteVersion == 7.30 ||
                VersionInfo.FortniteVersion == 8.51 ||
                VersionInfo.FortniteVersion == 9.40 ||
                VersionInfo.FortniteVersion == 9.41 ||
                VersionInfo.FortniteVersion == 10.40;
            bool bShowsEventBusControl = bIsEventPlaylist && bEventStartsOnSpawnIsland && gsStatus == Joinable;
            bool bShowsDefaultMatchSettings = bShowsDefaultPreGameConfig;

            if (gsStatus <= Joinable && (bShowsOnlyUpPreGameConfig || bShowsDefaultPreGameConfig || bShowsEventBusControl))
            {
                static bool bStartedBus = false;

                if (!bStartedBus)
                {
                    SectionHeader("Pre-Game Configuration", SectionWidth);
                    BeginSectionBody();

                    if (bShowsOnlyUpPreGameConfig)
                    {
                        ImGui::Checkbox("Disable Jump Fatigue", &FConfiguration::bDisableJumpFatigue);
                        ImGui::Checkbox("Player Has Pickaxe", &FConfiguration::bHasPickaxe);
                    }
                    else if (bShowsDefaultPreGameConfig)
                    {
                        if (gsStatus <= Joinable)
                        {
                            if (gsStatus < Joinable)
                            {
                                ImGui::Checkbox("Auto Bus Start", &FConfiguration::bAutoBusStart);

                                static bool bInitializedZone = false;

                                if (!bInitializedZone)
                                {
                                    FConfiguration::LateGameZone = FConfiguration::IsS27() ? 1 : 4;
                                    bInitializedZone = true;
                                }

                                if (VersionInfo.FortniteVersion == 19.20)
                                    FConfiguration::bAutoDump = false;

                                ImGui::Checkbox("Auto Dump Text", &FConfiguration::bAutoDump);

                                ImGui::Checkbox("Use Custom Map", &FConfiguration::bIsCustomMap);

                                ImGui::Checkbox("Enable Trickshot Tab", &FConfiguration::bEnableTrickshotTab);

                                static bool bInitializedConfig = false;

                                if (FConfiguration::bEnableTrickshotTab)
                                {
                                    if (!bInitializedConfig)
                                    {
                                        if (FConfiguration::bLateGame)
                                            FConfiguration::RandomizeKills = true;

                                        FConfiguration::RandomizeLevels = true;
                                        FConfiguration::bDisableJumpFatigue = true;
                                        FConfiguration::bAutoPauseTODM = true;

                                        if (IsArenaPlaylist() || IsTournamentPlaylist())
                                        {
                                            FConfiguration::RandomizeArenaPoints = true;
                                        }

                                        bInitializedConfig = true;
                                    }
                                }

                                if (FConfiguration::bAutoBusStart)
                                {
                                    LabeledSliderFloat("Bus Start Delay", "##bus-start-delay", &FConfiguration::BusStartDelay, 0.0f, 300.0f, "%.1f seconds", Width);
                                }

                                LabeledSliderFloat("Max Tick Rate", "##max-tick-rate", &FConfiguration::MaxTickRate, 5.0f, 180.0f, "%.1f seconds", Width);
                            }

                        }
                    }

                    if (gsStatus == Joinable && (bShowsDefaultPreGameConfig || bShowsEventBusControl))
                    {
                        ImGui::Spacing();

                        if (ImGui::Button("Start Bus Early", ImVec2(Width, Height)))
                        {
                            auto Time = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
                            auto WarmupDuration = 10.f;

                            auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
                            auto GameState = GameMode->GameState;

                            if (GameState->HasWarmupCountdownEndTime())
                            {
                                GameState->WarmupCountdownStartTime = Time;
                                GameState->WarmupCountdownEndTime = Time + WarmupDuration;
                                GameMode->WarmupCountdownDuration = WarmupDuration;
                                GameMode->WarmupEarlyCountdownDuration = WarmupDuration;
                            }

                            if (VersionInfo.FortniteVersion > 25.20)
                            {
                                auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(UWorld::GetWorld());

                                if (GamePhaseLogic->HasWarmupCountdownEndTime())
                                {
                                    GamePhaseLogic->WarmupCountdownStartTime = Time;
                                    GamePhaseLogic->WarmupCountdownEndTime = Time + WarmupDuration;
                                    GamePhaseLogic->WarmupCountdownDuration = WarmupDuration;
                                    GamePhaseLogic->WarmupEarlyCountdownDuration = WarmupDuration;
                                }
                            }

                            bStartedBus = true;
                        }
                    }

                    EndSectionBody();
                }
            }

            if (gsStatus == StartedMatch)
            {
                SectionHeader("Storm Control", SectionWidth);
                BeginSectionBody();

                if (!UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bPausedZone)
                {
                    if (ImGui::Button("Pause Safe Zone", ImVec2(Width, Height)))
                    {
                        UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bPausedZone = true;
                        auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
                        if (GameMode->HasbSafeZonePaused())
                            GameMode->bSafeZonePaused = true;
                        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"pausesafezone"), nullptr);
                    }
                }
                else
                {
                    if (ImGui::Button("Resume Safe Zone", ImVec2(Width, Height)))
                    {
                        UFortGameStateComponent_BattleRoyaleGamePhaseLogic::bPausedZone = false;
                        auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
                        if (GameMode->HasbSafeZonePaused())
                            GameMode->bSafeZonePaused = false;
                        UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"pausesafezone"), nullptr);
                    }
                }

                if (ImGui::Button("Skip Safe Zone", ImVec2(Width, Height)))
                {
                    auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;

                    if (GameMode->HasSafeZoneIndicator())
                    {
                        if (GameMode->SafeZoneIndicator)
                        {
                            GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
                            GameMode->SafeZoneIndicator->SafeZoneFinishShrinkTime = GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime + 0.05f;
                        }
                    }
                    else
                    {
                        auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(UWorld::GetWorld());

                        if (GamePhaseLogic->SafeZoneIndicator)
                        {
                            GamePhaseLogic->SafeZoneIndicator->SafeZoneStartShrinkTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
                            GamePhaseLogic->SafeZoneIndicator->SafeZoneFinishShrinkTime = GamePhaseLogic->SafeZoneIndicator->SafeZoneStartShrinkTime + 0.05f;
                        }
                    }

                    // UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"skipsafezone"), nullptr);
                }

                if (ImGui::Button("Start Shrinking Safe Zone", ImVec2(Width, Height)))
                {
                    auto GameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;

                    if (GameMode->HasSafeZoneIndicator())
                    {
                        if (GameMode->SafeZoneIndicator)
                            GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
                    }
                    else
                    {
                        auto GamePhaseLogic = UFortGameStateComponent_BattleRoyaleGamePhaseLogic::Get(UWorld::GetWorld());

                        if (GamePhaseLogic->SafeZoneIndicator)
                            GamePhaseLogic->SafeZoneIndicator->SafeZoneStartShrinkTime = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
                    }

                    // UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(L"startshrinksafezone"), nullptr);
                }

                EndSectionBody();
            }

            if (gsStatus >= Joinable)
            {
                SectionHeader("World Actions", SectionWidth);
                BeginSectionBody();

                if (ImGui::Button("Reset Player Builds", ImVec2(Width, Height)))
                {
                    TArray<ABuildingSMActor*> Builds;
                    Utils::GetAll<ABuildingSMActor>(Builds);

                    for (auto& Build : Builds)
                    {
                        if (Build && Build->bPlayerPlaced)
                            Build->SilentDie(true);
                    }

                    Builds.Free();
                }

                if (ImGui::Button("Destroy Floor Loot", ImVec2(Width, Height)))
                {
                    TArray<AActor*> Pickups;
                    Utils::GetAll<AActor>(AFortPickupAthena::StaticClass(), Pickups);

                    for (auto& Pickup : Pickups)
                    {
                        if (Pickup)
                            Pickup->K2_DestroyActor();
                    }

                    Pickups.Free();
                }

                EndSectionBody();

                if (SelectedPlaylist == (int)Playlist::Event && hasEvent == 2)
                {
                    bool bHasMatchingEvent = false;

                    for (auto& Event : Events::EventsArray)
                    {
                        const bool bMatchesEventPlaylist = !Event.PlaylistPath || wcscmp(FConfiguration::Playlist, Event.PlaylistPath) == 0;

                        if (Event.EventVersion == VersionInfo.FortniteVersion && bMatchesEventPlaylist)
                        {
                            bHasMatchingEvent = true;
                            break;
                        }
                    }

                    if (bHasMatchingEvent)
                    {
                        SectionHeader("Event", SectionWidth);
                        BeginSectionBody();

                        if (ImGui::Button("Start Event", ImVec2(Width, Height)))
                            Events::StartEvent();

                        EndSectionBody();
                    }
                }
            }

            const bool bCanShowGliderRedeploy = bShowsDefaultMatchSettings && gsStatus >= Joinable && gsStatus < Ended && VersionInfo.FortniteVersion > 5.41 && VersionInfo.FortniteVersion <= 16.00;
            const bool bCanShowRespawns = bShowsDefaultMatchSettings && ((VersionInfo.FortniteVersion >= 8.00 || gsStatus < Joinable) && VersionInfo.FortniteVersion > 2.50);

            if (bCanShowRespawns)
            {
                SectionHeader("Respawns", SectionWidth);
                BeginSectionBody();

                if (ImGui::Checkbox("Infinite Respawns", &FConfiguration::bForceRespawns))
                {
                    if (gsStatus >= Joinable)
                    {
                        auto RespawnPlaylist = FindObject<UFortPlaylistAthena>(FConfiguration::Playlist);
                        if (!RespawnPlaylist)
                            RespawnPlaylist = FindObject<UFortPlaylistAthena>(L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo");

                        if (RespawnPlaylist)
                        {
                            if (FConfiguration::bForceRespawns)
                            {
                                if (RespawnPlaylist->HasbRespawnInAir())
                                    RespawnPlaylist->bRespawnInAir = true;
                                if (RespawnPlaylist->HasRespawnHeight())
                                {
                                    RespawnPlaylist->RespawnHeight.Curve.CurveTable = nullptr;
                                    RespawnPlaylist->RespawnHeight.Curve.RowName = FName();
                                    RespawnPlaylist->RespawnHeight.Value = FConfiguration::RespawnHeight;
                                }
                                if (RespawnPlaylist->HasRespawnTime())
                                {
                                    RespawnPlaylist->RespawnTime.Curve.CurveTable = nullptr;
                                    RespawnPlaylist->RespawnTime.Curve.RowName = FName();
                                    RespawnPlaylist->RespawnTime.Value = FConfiguration::RespawnTime;
                                }

                                if (RespawnPlaylist->HasRespawnType())
                                {
                                    if (FConfiguration::PermanentRespawn)
                                        RespawnPlaylist->RespawnType = 1;
                                    else
                                        RespawnPlaylist->RespawnType = 2;
                                }
                            }
                            else
                            {
                                if (RespawnPlaylist->HasRespawnType())
                                    RespawnPlaylist->RespawnType = 0;
                            }
                        }
                    }
                }

                if (FConfiguration::bForceRespawns)
                {
                    ImGui::Indent(12.f);

                    if (ImGui::Checkbox("Storm Respawns", &FConfiguration::PermanentRespawn))
                    {
                        if (gsStatus >= Joinable)
                        {
                            auto RespawnPlaylist = FindObject<UFortPlaylistAthena>(FConfiguration::Playlist);
                            if (!RespawnPlaylist)
                                RespawnPlaylist = FindObject<UFortPlaylistAthena>(L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo");

                            if (RespawnPlaylist && RespawnPlaylist->HasRespawnType())
                                RespawnPlaylist->RespawnType = FConfiguration::PermanentRespawn ? 1 : 2;
                        }
                    }

                    ImGui::Checkbox("Keep Inventory on Respawn", &FConfiguration::bKeepInventory);
                    ImGui::Checkbox("Midzone Respawns", &FConfiguration::bMidZoneRespawning);

                    if (LabeledSliderInt("Respawn Time", "##respawn-time", &FConfiguration::RespawnTime, 1, 10, Width))
                    {
                        if (gsStatus >= Joinable)
                        {
                            auto RespawnPlaylist = FindObject<UFortPlaylistAthena>(FConfiguration::Playlist);
                            if (!RespawnPlaylist)
                                RespawnPlaylist = FindObject<UFortPlaylistAthena>(L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo");

                            if (RespawnPlaylist && RespawnPlaylist->HasRespawnTime())
                            {
                                RespawnPlaylist->RespawnTime.Curve.CurveTable = nullptr;
                                RespawnPlaylist->RespawnTime.Curve.RowName = FName();
                                RespawnPlaylist->RespawnTime.Value = FConfiguration::RespawnTime;
                            }
                        }
                    }

                    if (LabeledSliderInt("Respawn Height", "##respawn-height", &FConfiguration::RespawnHeight, 1000, 50000, Width))
                    {
                        if (gsStatus >= Joinable)
                        {
                            auto RespawnPlaylist = FindObject<UFortPlaylistAthena>(FConfiguration::Playlist);
                            if (!RespawnPlaylist)
                                RespawnPlaylist = FindObject<UFortPlaylistAthena>(L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo");

                            if (RespawnPlaylist && RespawnPlaylist->HasRespawnHeight())
                            {
                                RespawnPlaylist->RespawnHeight.Curve.CurveTable = nullptr;
                                RespawnPlaylist->RespawnHeight.Curve.RowName = FName();
                                RespawnPlaylist->RespawnHeight.Value = FConfiguration::RespawnHeight;
                            }
                        }
                    }

                    ImGui::Unindent(12.f);
                }

                EndSectionBody();
            }

            if (gsStatus >= Joinable)
            {
                SectionHeader("Gameplay Toggles", SectionWidth);
                BeginSectionBody();

                if (bCanShowGliderRedeploy)
                {
                    if (ImGui::Checkbox("Glider Redeploy", &FConfiguration::bGliderRedeploy))
                    {
                        auto GliderGameMode = (AFortGameMode*)UWorld::GetWorld()->AuthorityGameMode;
                        auto GliderGameState = GliderGameMode->GameState;
                        if (GliderGameState->HasDefaultGliderRedeployCanRedeploy())
                            GliderGameState->DefaultGliderRedeployCanRedeploy = FConfiguration::bGliderRedeploy ? 1.0f : 0.0f;
                    }
                }

                ImGui::Checkbox("Infinite Materials", &FConfiguration::bInfiniteMats);
                ImGui::Checkbox("Infinite Ammo", &FConfiguration::bInfiniteAmmo);
                ImGui::Checkbox("Toggle Cheat Commands", &FConfiguration::bEnableCheats);
                ImGui::Checkbox("Enable Trickshot Tab", &FConfiguration::bEnableTrickshotTab);
                ImGui::Checkbox("Siphon", &FConfiguration::bSiphon);

                if (FConfiguration::bSiphon)
                {
                    ImGui::Indent(12.f);

                    ImGui::TextUnformatted("Siphon Amount");
                    ImGui::SetNextItemWidth(Width);
                    ImGui::InputInt("##siphon-amount", &FConfiguration::SiphonAmount);

                    std::vector<const char*> SiphonAnimations = { "Default" };

                    if (VersionInfo.FortniteVersion >= 11.00)
                    {
                        SiphonAnimations.push_back("Slurp");
                        SiphonAnimations.push_back("Bandage Bazooka");
                    }

                    if (VersionInfo.FortniteVersion >= 12.50)
                    {
                        SiphonAnimations.push_back("Orange Paint");
                        SiphonAnimations.push_back("Purple Paint");
                    }

                    SiphonAnimations.push_back("Health Siphon");

                    if (VersionInfo.FortniteVersion >= 19.00)
                    {
                        SiphonAnimations.push_back("Med Mist");
                    }

                    if (VersionInfo.FortniteVersion >= 11.40)
                    {
                        SiphonAnimations.push_back("Upgrade Weapon");
                    }

                    if (FConfiguration::SiphonAnimType >= (int)SiphonAnimations.size())
                        FConfiguration::SiphonAnimType = 0;

                    ImGui::TextUnformatted("Siphon Animation");
                    ImGui::SetNextItemWidth(Width);
                    ImGui::Combo("##siphon-animation", &FConfiguration::SiphonAnimType, SiphonAnimations.data(), (int)SiphonAnimations.size());

                    ImGui::Unindent(12.f);
                }

                EndSectionBody();
            }

            if (gsStatus >= Joinable)
            {
                SectionHeader("Server Console", SectionWidth);
                BeginSectionBody();

                ImGui::SetNextItemWidth(Width);
                ImGui::InputText("##server-command", commandBuffer, IM_ARRAYSIZE(commandBuffer));

                if (ImGui::Button("Execute Console Command", ImVec2(Width, Height)))
                {
                    std::string str = commandBuffer;
                    auto wstr = std::wstring(str.begin(), str.end());

                    UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), FString(wstr.c_str()), nullptr);
                }

                EndSectionBody();
            }

            // "Start Server" button moved to the sidebar (bottom) for easy access.

            break;
        }
        case 1:
        {
            if (VersionInfo.FortniteVersion == 7.40 || VersionInfo.FortniteVersion == 14.40 || VersionInfo.FortniteVersion == 27.11 || VersionInfo.FortniteVersion == 30.00)
            {
                SectionHeader("Custom Playlists", SectionWidth);
                BeginSectionBody();

                if (VersionInfo.FortniteVersion == 7.40)
                {
                    ImGui::RadioButton("Backrooms Map", &SelectedPlaylist, (int)Playlist::Backrooms);
                }

                if (VersionInfo.FortniteVersion == 27.11)
                {
                    ImGui::RadioButton("Gav 1v1 Map", &SelectedPlaylist, (int)Playlist::Gav);
                    ImGui::RadioButton("Only Up Map", &SelectedPlaylist, (int)Playlist::OnlyUp);
                    ImGui::RadioButton("Tilted FFA", &SelectedPlaylist, (int)Playlist::TiltedZW);
                }

                if (VersionInfo.FortniteVersion == 14.40)
                {
                    ImGui::RadioButton("Retrac 1v1 Map", &SelectedPlaylist, (int)Playlist::Retrac1v1);
                    ImGui::RadioButton("Retrac Turtle Fights", &SelectedPlaylist, (int)Playlist::RetracTurtle);
                    //ImGui::RadioButton("Retrac Water Map", &SelectedPlaylist, (int)Playlist::RetracWater);
                    //ImGui::RadioButton("Twine 1v1 Map", &SelectedPlaylist, (int)Playlist::Twine1v1);
                }

                if (VersionInfo.FortniteVersion == 30.00)
                {
                    ImGui::RadioButton("Boxfights", &SelectedPlaylist, (int)Playlist::Boxfight);
                }

                EndSectionBody();
            }

            SectionHeader("Playlists", SectionWidth);
            BeginSectionBody();

            ImGui::RadioButton("Solos", &SelectedPlaylist, (int)Playlist::Solos);
            ImGui::RadioButton("Duos", &SelectedPlaylist, (int)Playlist::Duos);

            if (VersionInfo.FortniteVersion >= 7.40) // 7.30 content update idfk
            {
                ImGui::RadioButton("Trios", &SelectedPlaylist, (int)Playlist::Trios);
            }

            ImGui::RadioButton("Squads", &SelectedPlaylist, (int)Playlist::Squads);

            if (VersionInfo.FortniteVersion >= 20.00)
            {
                ImGui::RadioButton("Zero Build Solos", &SelectedPlaylist, (int)Playlist::ZBSolos);
                ImGui::RadioButton("Zero Build Duos", &SelectedPlaylist, (int)Playlist::ZBDuos);
                ImGui::RadioButton("Zero Build Trios", &SelectedPlaylist, (int)Playlist::ZBTrios);
                ImGui::RadioButton("Zero Build Squads", &SelectedPlaylist, (int)Playlist::ZBSquads);
            }

            if (VersionInfo.FortniteVersion >= 8.20)
            {
                ImGui::RadioButton("Arena Solos", &SelectedPlaylist, (int)Playlist::ArenaSolos);
                ImGui::RadioButton("Arena Duos", &SelectedPlaylist, (int)Playlist::ArenaDuos);
                ImGui::RadioButton("Arena Trios", &SelectedPlaylist, (int)Playlist::ArenaTrios);
                ImGui::RadioButton("Arena Squads", &SelectedPlaylist, (int)Playlist::ArenaSquads);

                if (VersionInfo.FortniteVersion >= 20.00)
                {
                    ImGui::RadioButton("Arena Zero Build Solos", &SelectedPlaylist, (int)Playlist::ArenaZBSolos);
                    ImGui::RadioButton("Arena Zero Build Duos", &SelectedPlaylist, (int)Playlist::ArenaZBDuos);
                    ImGui::RadioButton("Arena Zero Build Trios", &SelectedPlaylist, (int)Playlist::ArenaZBTrios);
                    ImGui::RadioButton("Arena Zero Build Squads", &SelectedPlaylist, (int)Playlist::ArenaZBSquads);
                }
            }

            if (VersionInfo.FortniteVersion >= 6.10)
            {
                ImGui::RadioButton("Tournament Solos", &SelectedPlaylist, (int)Playlist::TournamentSolos);
                ImGui::RadioButton("Tournament Duos", &SelectedPlaylist, (int)Playlist::TournamentDuos);
                ImGui::RadioButton("Tournament Trios", &SelectedPlaylist, (int)Playlist::TournamentTrios);
                ImGui::RadioButton("Tournament Squads", &SelectedPlaylist, (int)Playlist::TournamentSquads);
            }

            if (VersionInfo.FortniteVersion >= 7.10)
            {
                ImGui::RadioButton("One Shot Solos", &SelectedPlaylist, (int)Playlist::OneShotSolos);
                ImGui::RadioButton("One Shot Duos", &SelectedPlaylist, (int)Playlist::OneShotDuos);
                ImGui::RadioButton("One Shot Squads", &SelectedPlaylist, (int)Playlist::OneShotSquads);
                ImGui::RadioButton("Siphon Solos", &SelectedPlaylist, (int)Playlist::SiphonSolos);
                ImGui::RadioButton("Siphon Duos", &SelectedPlaylist, (int)Playlist::SiphonDuos);
                ImGui::RadioButton("Siphon Squads", &SelectedPlaylist, (int)Playlist::SiphonSquads);
                ImGui::RadioButton("Unvaulted Solos", &SelectedPlaylist, (int)Playlist::UnvSolos);
                ImGui::RadioButton("Unvaulted Duos", &SelectedPlaylist, (int)Playlist::UnvDuos);
                ImGui::RadioButton("Unvaulted Trios", &SelectedPlaylist, (int)Playlist::UnvTrios);
                ImGui::RadioButton("Unvaulted Squads", &SelectedPlaylist, (int)Playlist::UnvSquads);
                ImGui::RadioButton("Slide Solos", &SelectedPlaylist, (int)Playlist::SlideSolos);
                ImGui::RadioButton("Slide Duos", &SelectedPlaylist, (int)Playlist::SlideDuos);
            }

            if ((VersionInfo.FortniteVersion >= 8.20 && VersionInfo.FortniteVersion <= 10.40) || VersionInfo.FortniteVersion >= 15.20)
            {
                ImGui::RadioButton("Floor Is Lava Solos", &SelectedPlaylist, (int)Playlist::FILSolos);
                ImGui::RadioButton("Floor Is Lava Duos", &SelectedPlaylist, (int)Playlist::FILDuos);
                ImGui::RadioButton("Floor Is Lava Squads", &SelectedPlaylist, (int)Playlist::FILSquads);
            }

            if (VersionInfo.FortniteVersion >= 4.5 && VersionInfo.FortniteVersion < 11.31)
            {
                ImGui::RadioButton("Playground", &SelectedPlaylist, (int)Playlist::Playground);
            }

            if (VersionInfo.FortniteVersion >= 7.00)
            {
                ImGui::RadioButton("Creative ", &SelectedPlaylist, (int)Playlist::Creative);
            }

            for (auto& Event : Events::EventsArray)
            {
                if (Event.EventVersion == VersionInfo.FortniteVersion)
                {
                    ImGui::RadioButton("Event Playlist", &SelectedPlaylist, (int)Playlist::Event);
                }
            }

            ImGui::RadioButton("Custom", &SelectedPlaylist, (int)Playlist::Custom);

            EndSectionBody();

            switch (SelectedPlaylist)
            {
            case (int)Playlist::Solos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo";
                break;
            }
            case (int)Playlist::Duos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Playlist_DefaultDuo.Playlist_DefaultDuo";
                break;
            }
            case (int)Playlist::Trios:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Trios/Playlist_Trios.Playlist_Trios";
                break;
            }
            case (int)Playlist::Squads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Playlist_DefaultSquad.Playlist_DefaultSquad";
                break;
            }
            case (int)Playlist::ZBSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Playlist_NoBuildBR_Solo.Playlist_NoBuildBR_Solo";
                break;
            }
            case (int)Playlist::ZBDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Playlist_NoBuildBR_Duo.Playlist_NoBuildBR_Duo";
                break;
            }
            case (int)Playlist::ZBTrios:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Playlist_NoBuildBR_Trio.Playlist_NoBuildBR_Trio";
                break;
            }
            case (int)Playlist::ZBSquads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Playlist_NoBuildBR_Squad.Playlist_NoBuildBR_Squad";
                break;
            }
            case (int)Playlist::ArenaZBSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Competitive/Playlist_ShowdownAlt_NoBuildBR_Solo.Playlist_ShowdownAlt_NoBuildBR_Solo";
                break;
            }
            case (int)Playlist::ArenaZBDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Competitive/Playlist_ShowdownAlt_NoBuildBR_Duos.Playlist_ShowdownAlt_NoBuildBR_Duos";
                break;
            }
            case (int)Playlist::ArenaZBTrios:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Competitive/Playlist_ShowdownAlt_NoBuildBR_Trios.Playlist_ShowdownAlt_NoBuildBR_Trios";
                break;
            }
            case (int)Playlist::ArenaZBSquads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/NoBuildBR/Competitive/Playlist_ShowdownAlt_NoBuildBR_Squads.Playlist_ShowdownAlt_NoBuildBR_Squads";
                break;
            }
            case (int)Playlist::OneShotSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Low/Playlist_Low_Solo.Playlist_Low_Solo";
                break;
            }
            case (int)Playlist::OneShotDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Low/Playlist_Low_Duos.Playlist_Low_Duos";
                break;
            }
            case (int)Playlist::OneShotSquads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Low/Playlist_Low_Squad.Playlist_Low_Squad";
                break;
            }
            case (int)Playlist::SiphonSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Vamp/Playlist_Vamp_Solo.Playlist_Vamp_Solo";
                break;
            }
            case (int)Playlist::SiphonDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Vamp/Playlist_Vamp_Duos.Playlist_Vamp_Duos";
                break;
            }
            case (int)Playlist::SiphonSquads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Vamp/Playlist_Vamp_Squad.Playlist_Vamp_Squad";
                break;
            }
            case (int)Playlist::UnvSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Unvaulted/Playlist_Unvaulted_Solo.Playlist_Unvaulted_Solo";
                break;
            }
            case (int)Playlist::UnvDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Unvaulted/Playlist_Unvaulted_Duos.Playlist_Unvaulted_Duos";
                break;
            }
            case (int)Playlist::UnvTrios:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Unvaulted/Playlist_Unvaulted_Trios.Playlist_Unvaulted_Trios";
                break;
            }
            case (int)Playlist::UnvSquads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Unvaulted/Playlist_Unvaulted_Squads.Playlist_Unvaulted_Squads";
                break;
            }
            case (int)Playlist::SlideSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Slide/Playlist_Slide_Solo.Playlist_Slide_Solo";
                break;
            }
            case (int)Playlist::SlideDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Slide/Playlist_Slide_Duos.Playlist_Slide_Duos";
                break;
            }
            case (int)Playlist::TournamentSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_Showdown_Solo.Playlist_Showdown_Solo";
                break;
            }
            case (int)Playlist::TournamentDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_Showdown_Duos.Playlist_Showdown_Duos";
                break;
            }
            case (int)Playlist::TournamentTrios:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_Showdown_Trios.Playlist_Showdown_Trios";
                break;
            }
            case (int)Playlist::TournamentSquads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_Showdown_Squads.Playlist_Showdown_Squads";
                break;
            }
            case (int)Playlist::ArenaSolos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Solo.Playlist_ShowdownAlt_Solo";
                break;
            }
            case (int)Playlist::ArenaDuos:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Duos.Playlist_ShowdownAlt_Duos";
                break;
            }
            case (int)Playlist::ArenaTrios:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Trios.Playlist_ShowdownAlt_Trios";
                break;
            }
            case (int)Playlist::ArenaSquads:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Showdown/Playlist_ShowdownAlt_Squads.Playlist_ShowdownAlt_Squads";
                break;
            }
            case (int)Playlist::FILSolos:
            {
                if (VersionInfo.FortniteVersion <= 10.40)
                    FConfiguration::Playlist = L"/Game/Athena/Playlists/Fill/Playlist_Fill_Solo.Playlist_Fill_Solo";
                else
                    FConfiguration::Playlist = L"/Melt/Playlists/Playlist_Melt_Solo.Playlist_Melt_Solo";
                break;
            }
            case (int)Playlist::FILDuos:
            {
                if (VersionInfo.FortniteVersion <= 10.40)
                    FConfiguration::Playlist = L"/Game/Athena/Playlists/Fill/Playlist_Fill_Duos.Playlist_Fill_Duos";
                else
                    FConfiguration::Playlist = L"/Melt/Playlists/Playlist_Melt_Duos.Playlist_Melt_Duos";
                break;
            }
            case (int)Playlist::FILSquads:
            {
                if (VersionInfo.FortniteVersion <= 10.40)
                    FConfiguration::Playlist = L"/Game/Athena/Playlists/Fill/Playlist_Fill_Squads.Playlist_Fill_Squads";
                else
                    FConfiguration::Playlist = L"/Melt/Playlists/Playlist_Melt_Squads.Playlist_Melt_Squads";
                break;
            }
            case (int)Playlist::Playground:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Playground/Playlist_Playground.Playlist_Playground";
                FConfiguration::bLateGame = false;
                break;
            }
            case (int)Playlist::Creative:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Creative/Playlist_PlaygroundV2.Playlist_PlaygroundV2";
                FConfiguration::bLateGame = false;
                break;
            }
            case (int)Playlist::Gav:
            {
                FConfiguration::Playlist = L"/Game/Gav/Levels/GM_1v1/Playlist_Arena_DefaultSolo_Respawn.Playlist_Arena_DefaultSolo_Respawn";
                FConfiguration::bLateGame = false;
                break;
            }
            case (int)Playlist::Retrac1v1:
            {
				FConfiguration::Playlist = L"/Buddy/Playlist/Playlist_Retrac_1v1.Playlist_Retrac_1v1";
                FConfiguration::bLateGame = false;
                break;
            }
            case (int)Playlist::RetracTurtle:
            {
				FConfiguration::Playlist = L"/Buddy/Playlist/Playlist_Retrac_Turtle.Playlist_Retrac_Turtle";
                FConfiguration::bLateGame = false;
                break;
            }
            case (int)Playlist::RetracWater:
            {
				FConfiguration::Playlist = L"/Game/Retrac/Playlists/Playlist_ShowdownAlt_Solo_Retrac.Playlist_ShowdownAlt_Solo_Retrac";
                FConfiguration::bLateGame = false;
                break;
            }
            case (int)Playlist::TiltedZW:
            {
                FConfiguration::Playlist = L"/Game/Jett/TiltedZW/Playlist_TiltedZW_Jett.Playlist_TiltedZW_Jett";
                FConfiguration::bLateGame = false;
                break;
			}
            case (int)Playlist::OnlyUp:
            {
                FConfiguration::Playlist = L"/Game/Jett/Playlist_OnlyUp_Jett.Playlist_OnlyUp_Jett";
                FConfiguration::bLateGame = false;
                break;
			}
            case (int)Playlist::Twine1v1:
            {
                FConfiguration::Playlist = L"/Buddy/Playlists/Playlist_1v1Twine.Playlist_1v1Twine";
                FConfiguration::bLateGame = false;
                break;
			}
            case (int)Playlist::Boxfight:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Respawn/Playlist_Respawn_Solo.Playlist_Respawn_Solo";
                FConfiguration::bLateGame = false;
                break;
			}
            case (int)Playlist::Backrooms:
            {
                FConfiguration::Playlist = L"/Game/Athena/Playlists/Playlist_DefaultSolo.Playlist_DefaultSolo";
                FConfiguration::bLateGame = false;
                break;
            }
            case (int)Playlist::Event:
            {
                for (auto& Event : Events::EventsArray)
                {
                    if (Event.EventVersion == VersionInfo.FortniteVersion)
                    {
                        if (Event.PlaylistPath != nullptr)
                            FConfiguration::Playlist = Event.PlaylistPath;

                        FConfiguration::bLateGame = false;
                        break;
                    }
                }
                break;
            }
            case (int)Playlist::Custom:
            {
                break;
            }
            default:
            {
                break;
            }
            }

            if (SelectedPlaylist == (int)Playlist::Custom)
            {
                SectionHeader("Custom Playlist", SectionWidth);
                BeginSectionBody();

                ImGui::SetNextItemWidth(Width);
                ImGui::InputText("##custom-playlist", playlistBuffer, IM_ARRAYSIZE(playlistBuffer));

                if (ImGui::Button("Set Playlist", ImVec2(Width, Height)))
                {
                    std::string str = playlistBuffer;
                    static std::wstring persistentWstr;
                    persistentWstr = std::wstring(str.begin(), str.end());

                    FConfiguration::Playlist = persistentWstr.c_str();
                }

                EndSectionBody();
            }

            if (SelectedPlaylist == (int)Playlist::Event)
            {
                SectionHeader("Event Settings", SectionWidth);
                BeginSectionBody();

                ImGui::Checkbox("Auto Start Event", &FConfiguration::bAutoStartEvent);

                if (FConfiguration::bAutoStartEvent)
                {
                    LabeledSliderFloat("Event Start Time", "##event-start-time", &FConfiguration::EventStartTime, 30.0f, 300.0f, "%.1f seconds", Width);
                }

                EndSectionBody();
            }

            break;
        }
        case 2:
        {
            auto World = UWorld::GetWorld();

            if (!World) 
                break;

            static int InspectedPlayerIdx = -1;
            static bool bIsInspecting = false;

            UObject* NetDriver = World->NetDriver;

            if (!NetDriver) 
                break;

            UNetDriver* Driver = static_cast<UNetDriver*>(NetDriver);

            auto& ClientConnections = Driver->ClientConnections;
            AllControllers.clear();

            for (int i = 0; i < ClientConnections.Num(); i++)
            {
                auto Connection = ClientConnections[i];

                if (!Connection || !Connection->PlayerController) 
                    continue;

                AllControllers.push_back(std::make_pair((AFortPlayerControllerAthena*)Connection->PlayerController, Connection));
            }

            if (!bIsInspecting)
            {
                const std::string Header = "Players Connected: " + std::to_string(AllControllers.size());
                SectionHeader(Header.c_str(), SectionWidth);
                BeginSectionBody();

                for (int i = 0; i < AllControllers.size(); i++)
                {
                    auto& CurrentPair = AllControllers[i];
                    auto CurrentPlayerState = CurrentPair.first->PlayerState;

                    if (!CurrentPlayerState)
                    {
                        printf("PlayerState is null!\n");
                        continue;
                    }

                    auto Connection = CurrentPair.second;

                    std::string ButtonLabel = GUI::GetPlayerName(CurrentPlayerState, Connection);
                    auto RequestURL = GUI::GetRequestURL(Connection);

                    if (RequestURL && RequestURL->Data && RequestURL->NumElements)
                    {
                        auto RequestURLStr = RequestURL->ToString();
                        std::size_t pos = RequestURLStr.find("Name=");

                        if (pos != std::string::npos)
                        {
                            std::size_t end_pos = RequestURLStr.find('?', pos);
                            if (end_pos != std::string::npos)
                                ButtonLabel = RequestURLStr.substr(pos + 5, end_pos - pos - 5);
                            else
                                ButtonLabel = RequestURLStr.substr(pos + 5);
                        }
                    }

                    if (ButtonLabel.empty())
                        ButtonLabel = GUI::GetPlayerNameFromConnection(Connection);

                    if (ButtonLabel.empty())
                        ButtonLabel = std::string("Player ") + std::to_string(i + 1);

                    if (ImGui::Button(ButtonLabel.c_str(), ImVec2(Width, Height)))
                    {
                        InspectedPlayerIdx = i;
                        bIsInspecting = true;
                    }
                }

                EndSectionBody();
            }
            else
            {
                if (InspectedPlayerIdx >= AllControllers.size())
                {
                    bIsInspecting = false;
                    break;
                }

                auto TargetPC = AllControllers[InspectedPlayerIdx].first;
                auto TargetPS = TargetPC->PlayerState;
                auto TargetPawn = (AFortPlayerPawnAthena*)TargetPC->Pawn;

                if (!TargetPC || !TargetPawn || !TargetPS)
                {
                    bIsInspecting = false;
                    break;
				}

                if (ImGui::Button("Back", ImVec2(Width, Height)))
                {
                    bIsInspecting = false;
                    break;
                }

                SectionHeader("Player Information", SectionWidth);
                BeginSectionBody();

                auto InspectedPlayerState = (AFortPlayerStateAthena*)AllControllers[InspectedPlayerIdx].first->PlayerState;
                auto InspectedConnection = AllControllers[InspectedPlayerIdx].second;

                std::string DisplayName = GUI::GetPlayerName(InspectedPlayerState, InspectedConnection);

                if (DisplayName.empty())
                    DisplayName = std::string("Player ") + std::to_string(InspectedPlayerIdx + 1);

                ImGui::TextUnformatted("Inspecting Player: ");
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(ImVec4(1.f, 1.f, 0.f, 1.0f), DisplayName.c_str());

				ImGui::Text("Join Order: #%d", InspectedPlayerIdx + 1);

                //ImGui::Text("Ping: %f ms", TargetPS->GetPingInMilliseconds()); // ig it js doesn't exist on some versions

                ImGui::Text("Kills: %d", TargetPS->HasKillScore() ? TargetPS->KillScore : TargetPS->Kills);

                ImGui::TextUnformatted("Health: ");
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(ImVec4(0.372f, 0.792f, 0.255f, 1.0f), "%.0f", TargetPawn->GetHealth());
                ImGui::TextUnformatted("Shield: ");
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::TextColored(ImVec4(0.278f, 0.612f, 0.945f, 1.0f), "%.0f", TargetPawn->GetShield());

                EndSectionBody();

                SectionHeader("Player Actions", SectionWidth);
                BeginSectionBody();

                if (ImGui::Button("Copy Player's Location", ImVec2(Width, Height)))
                {
                    auto Location = TargetPawn->K2_GetActorLocation();

                    Memcury::Util::CopyToClipboard(std::to_string(Location.X) + " " + std::to_string(Location.Y) + " " + std::to_string(Location.Z));
				}

                if (ImGui::Button("Teleport All Players", ImVec2(Width, Height)))
                    AFortPlayerControllerAthena::TeleportAllPlayersTo(TargetPC);

                if (ImGui::Button("Regenerate Health & Shield", ImVec2(Width, Height)))
                {
                    TargetPawn->SetHealth(TargetPawn->GetMaxHealth());
                    TargetPawn->SetShield(TargetPawn->GetMaxShield());

                    auto Handle = TargetPS->AbilitySystemComponent->MakeEffectContext();
                    FGameplayTag Tag;
                    static auto Cue = FName(L"GameplayCue.Shield.PotionConsumed");
                    Tag.TagName = Cue;
                    auto PredictionKey = (FPredictionKey*)malloc(FPredictionKey::Size());
                    memset((PBYTE)PredictionKey, 0, FPredictionKey::Size());
                    TargetPS->AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded(Tag, *PredictionKey, Handle);
                    TargetPS->AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted(Tag, *PredictionKey, Handle);
                    free(PredictionKey);
                }

                if (ImGui::Button("Rift Player", ImVec2(Width, Height)))
                {
					auto Loc = TargetPawn->K2_GetActorLocation();

                    static auto RiftClass = FindObject<UClass>(L"/Game/Athena/Items/Consumables/RiftItem/BGA_RiftPortal_Item_Athena.BGA_RiftPortal_Item_Athena_C");

                    auto Rift = UWorld::SpawnActor<UClass>(RiftClass, Loc, {});

                    auto Actor = (AActor*)Rift;
                    Actor->ForceNetUpdate();

                    Actor->K2_DestroyActor();
				}

                auto& Health = TargetPC->MyFortPawn->HealthSet->Health;
                float MinValue = 100.f;

                bool bIsGodded = false;

                if (VersionInfo.FortniteVersion >= 21)
                {
                    if (TargetPC->Pawn)
                        bIsGodded = (TargetPC->Pawn->bCanBeDamaged == false);
                }
                else
                {
                    bIsGodded = (Health.Minimum == MinValue);
                }

                if (bIsGodded)
                {
                    if (ImGui::Button("Ungod Player", ImVec2(Width, Height)))
                    {
                        if (VersionInfo.FortniteVersion >= 21)
                        {
                            TargetPC->Pawn->bCanBeDamaged = true;
                        }
                        else
                        {
                            Health.Minimum = 0.f;
                            TargetPC->MyFortPawn->HealthSet->OnRep_Health(Health);
                        }
                    }
                }
                else
                {
                    if (ImGui::Button("God Player", ImVec2(Width, Height)))
                    {
                        if (VersionInfo.FortniteVersion >= 21)
                        {
                            TargetPC->Pawn->bCanBeDamaged = false;

                            float MaxHealth = TargetPawn->GetMaxHealth();
                            float MaxShield = TargetPawn->GetMaxShield();

                            TargetPawn->SetHealth(MaxHealth);
                            TargetPawn->SetShield(MaxShield);

                            auto Handle = TargetPS->AbilitySystemComponent->MakeEffectContext();
                            FGameplayTag Tag;
                            static auto Cue = FName(L"GameplayCue.Shield.PotionConsumed");
                            Tag.TagName = Cue;
                            auto PredictionKey = (FPredictionKey*)malloc(FPredictionKey::Size());
                            memset((PBYTE)PredictionKey, 0, FPredictionKey::Size());
                            TargetPS->AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded(Tag, *PredictionKey, Handle);
                            TargetPS->AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted(Tag, *PredictionKey, Handle);
                            free(PredictionKey);
                        }
                        else
                        {
                            if (Health.Minimum != MinValue)
                            {
                                Health.Minimum = MinValue;
                                TargetPC->MyFortPawn->HealthSet->OnRep_Health(Health);

                                float MaxHealth = TargetPawn->GetMaxHealth();
                                float MaxShield = TargetPawn->GetMaxShield();

                                TargetPawn->SetHealth(MaxHealth);
                                TargetPawn->SetShield(MaxShield);

                                auto Handle = TargetPS->AbilitySystemComponent->MakeEffectContext();
                                FGameplayTag Tag;
                                static auto Cue = FName(L"GameplayCue.Shield.PotionConsumed");
                                Tag.TagName = Cue;
                                auto PredictionKey = (FPredictionKey*)malloc(FPredictionKey::Size());
                                memset((PBYTE)PredictionKey, 0, FPredictionKey::Size());
                                TargetPS->AbilitySystemComponent->NetMulticast_InvokeGameplayCueAdded(Tag, *PredictionKey, Handle);
                                TargetPS->AbilitySystemComponent->NetMulticast_InvokeGameplayCueExecuted(Tag, *PredictionKey, Handle);
                                free(PredictionKey);
                            }
                            else
                            {
                                Health.Minimum = 0.f;
                                TargetPC->MyFortPawn->HealthSet->OnRep_Health(Health);
                            }
                        }
                    }
                }

                if (ImGui::Button("Eliminate Player", ImVec2(Width, Height)))
                {
                    TargetPC->ServerSuicide();
                    bIsInspecting = false;
                }

                if (ImGui::Button("Kick Player", ImVec2(Width, Height)))
                {
                    TargetPC->ServerReturnToMainMenu("You have been kicked from the game by the host.");
					bIsInspecting = false;
                }

                ImGui::Spacing();
                ImGui::Spacing();

                static float LaunchX = 0.f;
                static float LaunchY = 0.f;
                static float LaunchZ = 0.f;

                ImGui::SetNextItemWidth(Width);
                ImGui::InputFloat("Launch X", &LaunchX);

                ImGui::SetNextItemWidth(Width);
                ImGui::InputFloat("Launch Y", &LaunchY);

                ImGui::SetNextItemWidth(Width);
                ImGui::InputFloat("Launch Z", &LaunchZ);

                if (ImGui::Button("Launch Player", ImVec2(Width, Height)))
                {
                    FVector LaunchVelocity = FVector(LaunchX, LaunchY, LaunchZ);
                    TargetPawn->LaunchCharacterJump(LaunchVelocity, false, nullptr, true);
                }

                ImGui::Spacing();
                ImGui::Spacing();

                static char WID[256] = {};
				static int Amount = 1.f;

                ImGui::SetNextItemWidth(Width);
                ImGui::InputText("Item To Give", WID, IM_ARRAYSIZE(WID));

                ImGui::SetNextItemWidth(Width);
                ImGui::InputInt("Amount To Give", &Amount);

                if (ImGui::Button("Give Item To Player", ImVec2(Width, Height)))
                {
                    if (WID[0] != '\0')
                    {
                        std::string ItemID = WID;
                        auto ItemDefinition = FindObject<UFortItemDefinition>(UEAllocatedWString(ItemID.begin(), ItemID.end()));

                        if (!ItemDefinition)
                            ItemDefinition = TUObjectArray::FindObject<UFortItemDefinition>(ItemID.c_str());

                        int32 Count = Amount;

                        if (Count <= 0)
							Count = ItemDefinition->GetMaxStackSize();

                        FVector FinalLoc = TargetPawn ? TargetPawn->K2_GetActorLocation() : FVector();

                        FVector ForwardVector = TargetPawn ? TargetPawn->GetActorForwardVector() : FVector();
                        ForwardVector.Z = 0.0f;
                        ForwardVector.Normalize();

                        const float RandomAngleVariation = ((float)rand() * 0.00109866634f) - 18.f;
                        const float FinalAngle = RandomAngleVariation * 0.017453292519943295f;

                        FinalLoc.X += cos(FinalAngle) * 100.f;
                        FinalLoc.Y += sin(FinalAngle) * 100.f;

                        auto Pickup = AFortInventory::SpawnPickup(FinalLoc, ItemDefinition, Count, 0, EFortPickupSourceTypeFlag::GetOther(), EFortPickupSpawnSource::GetUnset(), TargetPawn);

                        if (TargetPawn && Pickup)
                            TargetPawn->ServerHandlePickup(Pickup, Pickup->PickupLocationData.FlyTime, FVector(), true);
                    }
				}

                ImGui::Spacing();
                ImGui::Spacing();

                static std::string nameStr;
                ImGui::SetNextItemWidth(Width);
                ImGui::InputText("New Name", &nameStr);

                if (ImGui::Button("Change Player's Name", ImVec2(Width, Height)))
                {
                    if (!nameStr.empty())
                    {
                        std::wstring nameW(nameStr.begin(), nameStr.end());
                        FString NewName = FString(nameW.c_str());

                        if (!TargetPC)
                            return;

                        TargetPC->ServerChangeName(NewName);
                        TargetPS->OnRep_PlayerName();
                        //nameStr.clear();
                    }
                }

                EndSectionBody();
            }

            break;
        }
        case 3:
        {
            SectionHeader("Lategame Options", SectionWidth);
            BeginSectionBody();

            if (gsStatus < StartedMatch)
            {
                bool bIsCustomMap = SelectedPlaylist == static_cast<int>(Playlist::Gav)
                    || SelectedPlaylist == static_cast<int>(Playlist::Retrac1v1)
                    || SelectedPlaylist == static_cast<int>(Playlist::RetracTurtle)
                    || SelectedPlaylist == static_cast<int>(Playlist::RetracWater)
                    || SelectedPlaylist == static_cast<int>(Playlist::TiltedZW)
                    || SelectedPlaylist == static_cast<int>(Playlist::OnlyUp)
                    || SelectedPlaylist == static_cast<int>(Playlist::Twine1v1)
                    || SelectedPlaylist == static_cast<int>(Playlist::Boxfight)
                    || SelectedPlaylist == static_cast<int>(Playlist::Backrooms)
                    || SelectedPlaylist == static_cast<int>(Playlist::Event);

                // Lategame skips the phases PlayerAI plays through, so it is
                // forced off and greyed out while Enable AIs is on.
                const bool bLockLateGame = bIsCustomMap || MagnesiumPlayerAISettings::bEnableAIs;
                if (bLockLateGame)
                    FConfiguration::bLateGame = false;

                ImGui::BeginDisabled(bLockLateGame);
                ImGui::Checkbox("Late Game", &FConfiguration::bLateGame);
                ImGui::EndDisabled();

                if (FConfiguration::bLateGame)
                {
                    if (VersionInfo.FortniteVersion > 2.50)
                        ImGui::Checkbox("Use Moving Bus", &FConfiguration::bMovingBus);

                    ImGui::Checkbox("Use Long Zone", &FConfiguration::bLateGameLongZone);
                    ImGui::Checkbox("Use Versionized Lategame Loadouts", &FConfiguration::bUseVersionizedLoadout);
                    ImGui::Checkbox("Use Custom Lategame Loadout", &FConfiguration::bUseCustomLoadout);
                    ImGui::Checkbox("Custom Safe Zone", &FConfiguration::bCustomSafeZone);

                    if (!FConfiguration::bCustomSafeZone)
                        LabeledSliderInt("Starting Zone", "##starting-zone", &FConfiguration::LateGameZone, 1, 7, Width);
                    else
                    {
                        // Interactive minimap: click to place the zone center, drag
                        // out to set the radius. The numeric inputs below stay live as
                        // a fine-tune and as a fallback if the map image is unavailable.
                        static ID3D11ShaderResourceView* s_MapSRV = nullptr;
                        static int s_MapW = 0, s_MapH = 0;
                        static int s_RetryIn = 0;
                        if (!s_MapSRV) // retry until the minimap texture becomes resident
                        {
                            if (s_RetryIn <= 0)
                            {
                                SafeZoneMap::Acquire(g_pd3dDevice, &s_MapSRV, &s_MapW, &s_MapH);
                                s_RetryIn = 180; // ~3s between attempts
                            }
                            else --s_RetryIn;
                        }

                        const float E = SafeZoneMap::kWorldExtent;

                        if (s_MapSRV)
                        {
                            const float S = Width * 1.8f; // square canvas
                            const ImVec2 origin = ImGui::GetCursorScreenPos();
                            ImGui::Image((void*)s_MapSRV, ImVec2(S, S));
                            ImGui::SetCursorScreenPos(origin); // overlay input on the same rect
                            ImGui::InvisibleButton("##mapcanvas", ImVec2(S, S));
                            const ImVec2 r0 = ImGui::GetItemRectMin();

                            // Press => set center; drag => set radius from that center.
                            if (ImGui::IsItemActivated())
                            {
                                const ImVec2 m = ImGui::GetIO().MousePos;
                                float wx, wy;
                                SafeZoneMap::PixelToWorld(m.x - r0.x, m.y - r0.y, S, wx, wy);
                                FConfiguration::CustomSafeZoneCenter.X = SafeZoneMap::Clamp(wx, -E, E);
                                FConfiguration::CustomSafeZoneCenter.Y = SafeZoneMap::Clamp(wy, -E, E);
                            }
                            if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
                            {
                                float lx, ly;
                                SafeZoneMap::WorldToPixel((float)FConfiguration::CustomSafeZoneCenter.X,
                                                          (float)FConfiguration::CustomSafeZoneCenter.Y, S, lx, ly);
                                const ImVec2 m = ImGui::GetIO().MousePos;
                                const float dx = m.x - (r0.x + lx), dy = m.y - (r0.y + ly);
                                const float dpx = sqrtf(dx * dx + dy * dy);
                                FConfiguration::CustomSafeZoneRadius =
                                    SafeZoneMap::Clamp(SafeZoneMap::PixelsToRadius(dpx, S), 500.f, 100000.f);
                            }

                            // Overlay the stored center + radius every frame (also when idle).
                            ImDrawList* dl = ImGui::GetWindowDrawList();
                            float lx, ly;
                            SafeZoneMap::WorldToPixel((float)FConfiguration::CustomSafeZoneCenter.X,
                                                      (float)FConfiguration::CustomSafeZoneCenter.Y, S, lx, ly);
                            const ImVec2 c(r0.x + lx, r0.y + ly);
                            const float rpx = SafeZoneMap::RadiusToPixels(FConfiguration::CustomSafeZoneRadius, S);
                            // Storm shading: everything outside the safe circle is purple (~0.5 alpha).
                            SafeZoneMap::FillOutsideCircle(dl, r0, ImVec2(r0.x + S, r0.y + S), c, rpx, IM_COL32(140, 40, 200, 128));
                            dl->AddCircleFilled(c, rpx, IM_COL32(90, 160, 255, 40), 96);
                            dl->AddCircle(c, rpx, IM_COL32(130, 200, 255, 230), 96, 2.f);
                            // Center marker (dot).
                            dl->AddCircleFilled(c, 4.f, IM_COL32(255, 255, 255, 235));
                            dl->AddCircle(c, 4.f, IM_COL32(20, 30, 60, 200), 0, 1.5f);

                            ImGui::Spacing();
                        }
                        else
                        {
                            ImGui::TextDisabled("Map image unavailable - set coordinates manually.");
                        }

                        // Numeric readout + fine-tune (always available).
                        float cx = (float)FConfiguration::CustomSafeZoneCenter.X;
                        float cy = (float)FConfiguration::CustomSafeZoneCenter.Y;

                        ImGui::SetNextItemWidth(Width);
                        if (ImGui::InputFloat("Center X", &cx)) FConfiguration::CustomSafeZoneCenter.X = cx;
                        ImGui::SetNextItemWidth(Width);
                        if (ImGui::InputFloat("Center Y", &cy)) FConfiguration::CustomSafeZoneCenter.Y = cy;

                        float RadiusMeters = FConfiguration::CustomSafeZoneRadius / 100.f;
                        if (LabeledSliderFloat("Radius", "##custom-sz-radius", &RadiusMeters, 5.f, 1000.f, "%.0f m", Width))
                            FConfiguration::CustomSafeZoneRadius = RadiusMeters * 100.f;
                    }
                }
            }

            EndSectionBody();

            if (gsStatus < StartedMatch && FConfiguration::bLateGame)
            {
            static char PrimaryWeaponBuffer[256] = { 0 };
            static char SecondaryWeaponBuffer[256] = { 0 };
            static char TertiaryWeaponBuffer[256] = { 0 };
            static char QuaternaryWeaponBuffer[256] = { 0 };
            static char QuinaryWeaponBuffer[256] = { 0 };
            static char TrapsBuffer[256] = { 0 };

            static int PrimaryAmountBuffer = 1;
            static int SecondaryAmountBuffer = 1;
            static int TertiaryAmountBuffer = 1;
            static int QuaternaryAmountBuffer = 1;
            static int QuinaryAmountBuffer = 1;
            static int TrapsAmountBuffer = 6;

            static bool bBuffersInitialized = false;
            static std::string LoadoutStatusMessage;
            static std::chrono::high_resolution_clock::time_point StatusMessageTime;
            static std::string ApplyLoadoutStatusMessage;
            static std::chrono::high_resolution_clock::time_point ApplyStatusMessageTime;

            if (FConfiguration::bUseCustomLoadout)
            {
                if (!bBuffersInitialized)
                {
                    strcpy_s(PrimaryWeaponBuffer, TCHAR_TO_UTF8(*FConfiguration::Primary));
                    strcpy_s(SecondaryWeaponBuffer, TCHAR_TO_UTF8(*FConfiguration::Secondary));
                    strcpy_s(TertiaryWeaponBuffer, TCHAR_TO_UTF8(*FConfiguration::Tertiary));
                    strcpy_s(QuaternaryWeaponBuffer, TCHAR_TO_UTF8(*FConfiguration::Quaternary));
                    strcpy_s(QuinaryWeaponBuffer, TCHAR_TO_UTF8(*FConfiguration::Quinary));
                    strcpy_s(TrapsBuffer, TCHAR_TO_UTF8(*FConfiguration::Traps));

                    PrimaryAmountBuffer = FConfiguration::PrimaryAmount;
                    SecondaryAmountBuffer = FConfiguration::SecondaryAmount;
                    TertiaryAmountBuffer = FConfiguration::TertiaryAmount;
                    QuaternaryAmountBuffer = FConfiguration::QuaternaryAmount;
                    QuinaryAmountBuffer = FConfiguration::QuinaryAmount;
                    TrapsAmountBuffer = FConfiguration::TrapsAmount;

                    bBuffersInitialized = true;
                }

                SectionHeader("Custom Loadout Slots", SectionWidth);
                BeginSectionBody();

                ImGui::PushItemWidth(Width);
                ImGui::InputText("Slot 1", PrimaryWeaponBuffer, sizeof(PrimaryWeaponBuffer));
                ImGui::InputInt("Slot 1 Amount", &PrimaryAmountBuffer);

                ImGui::InputText("Slot 2", SecondaryWeaponBuffer, sizeof(SecondaryWeaponBuffer));
                ImGui::InputInt("Slot 2 Amount", &SecondaryAmountBuffer);

                ImGui::InputText("Slot 3", TertiaryWeaponBuffer, sizeof(TertiaryWeaponBuffer));
                ImGui::InputInt("Slot 3 Amount", &TertiaryAmountBuffer);

                ImGui::InputText("Slot 4", QuaternaryWeaponBuffer, sizeof(QuaternaryWeaponBuffer));
                ImGui::InputInt("Slot 4 Amount", &QuaternaryAmountBuffer);

                ImGui::InputText("Slot 5", QuinaryWeaponBuffer, sizeof(QuinaryWeaponBuffer));
                ImGui::InputInt("Slot 5 Amount", &QuinaryAmountBuffer);

                ImGui::InputText("Trap", TrapsBuffer, sizeof(TrapsBuffer));
                ImGui::InputInt("Trap Amount", &TrapsAmountBuffer);
                ImGui::PopItemWidth();

                if (ImGui::Button("Apply Loadout", ImVec2(Width, Height)))
                {
                    FConfiguration::Primary = FString(PrimaryWeaponBuffer);
                    FConfiguration::Secondary = FString(SecondaryWeaponBuffer);
                    FConfiguration::Tertiary = FString(TertiaryWeaponBuffer);
                    FConfiguration::Quaternary = FString(QuaternaryWeaponBuffer);
                    FConfiguration::Quinary = FString(QuinaryWeaponBuffer);
                    FConfiguration::Traps = FString(TrapsBuffer);

                    FConfiguration::PrimaryAmount = PrimaryAmountBuffer;
                    FConfiguration::SecondaryAmount = SecondaryAmountBuffer;
                    FConfiguration::TertiaryAmount = TertiaryAmountBuffer;
                    FConfiguration::QuaternaryAmount = QuaternaryAmountBuffer;
                    FConfiguration::QuinaryAmount = QuinaryAmountBuffer;
                    FConfiguration::TrapsAmount = TrapsAmountBuffer;

                    printf("Saved current loadout.\n");
                    ApplyLoadoutStatusMessage = "Loadout saved successfully!";
                    ApplyStatusMessageTime = std::chrono::high_resolution_clock::now();

                    if (!ApplyLoadoutStatusMessage.empty())
                    {
                        auto Elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now() - StatusMessageTime).count();

                        if (Elapsed < 5)
                        {
                            ImVec4 StatusColor = (ApplyLoadoutStatusMessage.find("Failed.") != std::string::npos) ? ImVec4(1.0f, 0.0f, 0.0f, 1.0f) : ImVec4(0.0f, 1.0f, 0.0f, 1.0f);

                            ImGui::TextColored(StatusColor, "%s", LoadoutStatusMessage.c_str());
                        }
                        else
                        {
                            ApplyLoadoutStatusMessage.clear();
                        }
                    }
                }

                EndSectionBody();

                SectionHeader("Save/Load Loadout", SectionWidth);
                BeginSectionBody();

                if (ImGui::Button("Save Loadout to File", ImVec2(Width, Height)))
                {
                    if (LoadoutManager::SaveLoadout(PrimaryWeaponBuffer, PrimaryAmountBuffer, SecondaryWeaponBuffer, SecondaryAmountBuffer, TertiaryWeaponBuffer, TertiaryAmountBuffer, QuaternaryWeaponBuffer, QuaternaryAmountBuffer, QuinaryWeaponBuffer, QuinaryAmountBuffer, TrapsBuffer, TrapsAmountBuffer))
                    {
                        LoadoutStatusMessage = "Loadout saved successfully!";
                        printf("Loadout saved to: %s\n", LoadoutManager::GetLoadoutFilePath().c_str());
                    }
                    else
                    {
                        LoadoutStatusMessage = "Failed to save loadout!";
                    }

                    StatusMessageTime = std::chrono::high_resolution_clock::now();
                }

                if (ImGui::Button("Load Loadout from File", ImVec2(Width, Height)))
                {
                    if (LoadoutManager::LoadLoadout(PrimaryWeaponBuffer, PrimaryAmountBuffer, SecondaryWeaponBuffer, SecondaryAmountBuffer, TertiaryWeaponBuffer, TertiaryAmountBuffer, QuaternaryWeaponBuffer, QuaternaryAmountBuffer, QuinaryWeaponBuffer, QuinaryAmountBuffer, TrapsBuffer, TrapsAmountBuffer))
                    {
                        LoadoutStatusMessage = "Loadout loaded successfully!";
                        printf("Loadout loaded from: %s\n", LoadoutManager::GetLoadoutFilePath().c_str());
                    }
                    else
                    {
                        LoadoutStatusMessage = "Failed to load loadout! File may not exist.";
                    }

                    StatusMessageTime = std::chrono::high_resolution_clock::now();
                }

                if (!LoadoutStatusMessage.empty())
                {
                    auto Elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::high_resolution_clock::now() - StatusMessageTime).count();

                    if (Elapsed < 5)
                    {
                        ImVec4 StatusColor = (LoadoutStatusMessage.find("Failed") != std::string::npos) ? ImVec4(1.0f, 0.0f, 0.0f, 1.0f) : ImVec4(0.0f, 1.0f, 0.0f, 1.0f);

                        ImGui::TextColored(StatusColor, "%s", LoadoutStatusMessage.c_str());
                    }
                    else
                    {
                        LoadoutStatusMessage.clear();
                    }
                }

                EndSectionBody();
            }
            } // gsStatus < StartedMatch

            break;
        }
        case 4:
        {
            SectionHeader("AI Players", SectionWidth);
            BeginSectionBody();

            ImGui::Checkbox("Enable AIs (EXPERIMENTAL)", &MagnesiumPlayerAISettings::bEnableAIs);

            if (MagnesiumPlayerAISettings::bEnableAIs)
            {
                if (FConfiguration::bLateGame)
                    FConfiguration::bLateGame = false;
            }

            EndSectionBody();

            SectionHeader("Bot Stats", SectionWidth);
            BeginSectionBody();

            ImGui::PushItemWidth(Width);
            ImGui::InputInt("Bot Health", &FConfiguration::BotHealth);
            ImGui::InputInt("Bot Shield", &FConfiguration::BotShield);
            ImGui::PopItemWidth();

            EndSectionBody();

            SectionHeader("Bot Names", SectionWidth);
            BeginSectionBody();

            ImGui::Checkbox("Use Custom Bot Names", &FConfiguration::UseCustomBotNames);

            if (FConfiguration::UseCustomBotNames)
            {
                static char BotNameBuffer[64] = {};

                if (BotNameBuffer[0] == '\0' && !FConfiguration::BotName.empty())
                    strncpy_s(BotNameBuffer, sizeof(BotNameBuffer), FConfiguration::BotName.c_str(), _TRUNCATE);

                ImGui::PushItemWidth(Width);
                ImGui::InputText("Bot Name", BotNameBuffer, sizeof(BotNameBuffer));
                ImGui::PopItemWidth();

                if (ImGui::Button("Apply Bot Name", ImVec2(Width, Height)))
                {
                    FConfiguration::BotName = BotNameBuffer;
                }
            }

            EndSectionBody();

            break;
        }
        case 5:
        {
            SectionHeader("Creative Plot", SectionWidth);
            BeginSectionBody();

            ImGui::RadioButton("Temperate Island", &SelectedPlot, (int)Plot::Temperate);
            ImGui::RadioButton("Meadow Island", &SelectedPlot, (int)Plot::Meadow);
            ImGui::RadioButton("Arctic Island", &SelectedPlot, (int)Plot::Arctic);
            ImGui::RadioButton("Frosty Fortress", &SelectedPlot, (int)Plot::Fortress);
            ImGui::RadioButton("Ice Lake Island", &SelectedPlot, (int)Plot::IceLake);
            ImGui::RadioButton("Canyon Island", &SelectedPlot, (int)Plot::Canyon);
            ImGui::RadioButton("Arid Island", &SelectedPlot, (int)Plot::Arid);
            ImGui::RadioButton("Wasteland Island", &SelectedPlot, (int)Plot::Wasteland);
            ImGui::RadioButton("Tropical Island", &SelectedPlot, (int)Plot::Tropical);
            ImGui::RadioButton("River Edge Island", &SelectedPlot, (int)Plot::RiverEdge);
            ImGui::RadioButton("Volcano Island", &SelectedPlot, (int)Plot::Volcano);
            ImGui::RadioButton("Sandbar Island", &SelectedPlot, (int)Plot::Sandbar);
            ImGui::RadioButton("Caldera Island", &SelectedPlot, (int)Plot::Caldera);
            ImGui::RadioButton("Kevin Floating Islands", &SelectedPlot, (int)Plot::Kevin);
            ImGui::RadioButton("Black Glass Island", &SelectedPlot, (int)Plot::BlackGlass);
            ImGui::RadioButton("Grid Island", &SelectedPlot, (int)Plot::Grid);
            ImGui::RadioButton("The Block", &SelectedPlot, (int)Plot::Block);
            ImGui::RadioButton("Grassy Hill Island", &SelectedPlot, (int)Plot::GrassyHill);
            ImGui::RadioButton("Shoreline Island", &SelectedPlot, (int)Plot::Shoreline);
            ImGui::RadioButton("Archipelago Island", &SelectedPlot, (int)Plot::Archipelago);
            ImGui::RadioButton("Horseshoe Island", &SelectedPlot, (int)Plot::Horseshoe);
            ImGui::RadioButton("The Shark", &SelectedPlot, (int)Plot::Shark);
            ImGui::RadioButton("Floating Island Hub", &SelectedPlot, (int)Plot::FloatingHub);
            ImGui::RadioButton("Fortilla Island", &SelectedPlot, (int)Plot::Fortilla);
            ImGui::RadioButton("Debris Island", &SelectedPlot, (int)Plot::Debris);
            ImGui::RadioButton("Custom", &SelectedPlot, (int)Plot::Custom);

            EndSectionBody();

            switch (SelectedPlot)
            {
            case (int)Plot::Temperate:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Temperate_Medium.Temperate_Medium";
                break;
            }
            case (int)Plot::Meadow:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/FlatGrass_Large.FlatGrass_Large";
                break;
            }
            case (int)Plot::Arctic:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Arctic_Medium.Arctic_Medium";
                break;
            }
            case (int)Plot::Fortress:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Arctic_Competitive_Medium1.Arctic_Competitive_Medium1";
                break;
            }
            case (int)Plot::IceLake:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/IceLake_Large.IceLake_Large";
                break;
            }
            case (int)Plot::Canyon:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Desert_Large.Desert_Large";
                break;
            }
            case (int)Plot::Arid:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Pandora_Large.Pandora_Large";
                break;
            }
            case (int)Plot::Wasteland:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Desert_Large_02.Desert_Large_02";
                break;
            }
            case (int)Plot::Tropical:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Desert_Large_02.Desert_Large_02";
                break;
            }
            case (int)Plot::RiverEdge:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Military_Medium.Military_Medium";
                break;
            }
            case (int)Plot::Volcano:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Volcano_Large.Volcano_Large";
                break;
            }
            case (int)Plot::Sandbar:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Sandbar_Large.Sandbar_Large";
                break;
            }
            case (int)Plot::Caldera:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Volcano_Large_02.Volcano_Large_02";
                break;
            }
            case (int)Plot::Kevin:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Kevin_Large.Kevin_Large";
                break;
            }
            case (int)Plot::BlackGlass:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/BlackGlass_Medium.BlackGlass_Medium";
                break;
            }
            case (int)Plot::Grid:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/FlatGrid_Large.FlatGrid_Large";
                break;
            }
            case (int)Plot::Block:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/TheBlock_Season7.TheBlock_Season7";
                break;
            }
            case (int)Plot::GrassyHill:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/TheHub_01.TheHub_01";
                break;
            }
            case (int)Plot::Shoreline:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/FlatGrass_LargeV2.FlatGrass_LargeV2";
                break;
            }
            case (int)Plot::Archipelago:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/SandBar_LargeV2.SandBar_LargeV2";
                break;
            }
            case (int)Plot::Horseshoe:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Escape_Large.Escape_Large";
                break;
            }
            case (int)Plot::Shark:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Shark_Large.Shark_Large";
                break;
            }
            case (int)Plot::Fortilla:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Arena_Large_01.Arena_Large_01";
                break;
            }
            case (int)Plot::Debris:
            {
                FConfiguration::CreativePlot = L"/Game/Playgrounds/Items/Plots/Arena_Large_02.Arena_Large_02";
                break;
            }
            case (int)Plot::Custom:
            {
                break;
            }
            default:
            {
                break;
            }
            }

            break;
        }
        case 6:
        {
            SectionHeader("Custom Map Configuration", SectionWidth);
            BeginSectionBody();

			ImGui::Checkbox("One Kill Ends Game", &FConfiguration::AutoEndGame);

            EndSectionBody();

            SectionHeader("Custom Map", SectionWidth);
            BeginSectionBody();

            ImGui::RadioButton("Athena Faceoff", &SelectedMap, (int)Map::Faceoff);
            ImGui::RadioButton("Papaya (Party Royale)", &SelectedMap, (int)Map::Papaya);
            ImGui::RadioButton("The Combine", &SelectedMap, (int)Map::Crucible);
            ImGui::RadioButton("Flat Grid", &SelectedMap, (int)Map::FlatGrid);
            ImGui::RadioButton("Prop Hunt", &SelectedMap, (int)Map::PropHunt);

            EndSectionBody();

            switch (SelectedMap)
            {
            case (int)Map::Papaya:
            {
                FConfiguration::CustomMap = L"open /Game/Athena/Apollo/Maps/Special/Papaya/Apollo_Papaya";
                break;
            }
            case (int)Map::Crucible:
            {
                FConfiguration::CustomMap = L"open /Game/Athena/Maps/Crucible/Athena_Crucible";
                break;
            }
            case (int)Map::TutorialMap:
            {
                FConfiguration::CustomMap = L"open /Game/Athena/Maps/Tutorial/Athena_Tutorial_Map_A";
                break;
            }
            case (int)Map::EmptyTest:
            {
                FConfiguration::CustomMap = L"open /Game/Athena/Maps/Athena_EmptyTest";
                break;
            }
            case (int)Map::Faceoff:
            {
                FConfiguration::CustomMap = L"open /Game/Athena/Maps/Athena_Faceoff";
                break;
            }
            case (int)Map::Playground:
            {
                FConfiguration::CustomMap = L"open /Game/Athena/Maps/Athena_Playground";
                break;
            }
            case (int)Map::DADBRO:
            {
                FConfiguration::CustomMap = L"open /Game/Athena/Playlists/DADBRO/Athena_DADBRO_Apollo_Island";
                break;
            }
            case (int)Map::Kevin:
            {
                FConfiguration::CustomMap = L"open /Game/Creative/Maps/Islands/105x105/Kevin/Kevin_Floating_Island_105x105_Mesh";
                break;
            }
            case (int)Map::FlatGrid:
            {
                FConfiguration::CustomMap = L"open /Game/Creative/Maps/Islands/105x105/FlatGrid/FlatGrid_Island_105x105";
                break;
            }
            case (int)Map::PropHunt:
            {
                FConfiguration::CustomMap = L"open /Game/Creative/Maps/Islands/105x105/Loki/Loki_Island_105x105_M";
                break;
            }
            default:
            {
                break;
            }
            }

            break;
        }
        case 7:
        {
            SectionHeader("Trickshot Customization", SectionWidth);
            BeginSectionBody();

            if (gsStatus < Joinable)
            {
                ImGui::Checkbox("Toggle Swag Lines", &FConfiguration::bUseWinLines);

                if (VersionInfo.FortniteVersion <= 23.50)
                    ImGui::Checkbox("Toggle Infinite Render", &FConfiguration::bInfiniteRender);

                if ((IsArenaPlaylist() || IsTournamentPlaylist()) && FConfiguration::bLateGame && !FConfiguration::bForceRespawns)
                    ImGui::Checkbox("Randomize Arena Points", &FConfiguration::RandomizeArenaPoints);

                if (FConfiguration::bLateGame)
                    ImGui::Checkbox("Randomize Kills", &FConfiguration::RandomizeKills);

                ImGui::Checkbox("Randomize Levels", &FConfiguration::RandomizeLevels);

                ImGui::Checkbox("Disable Jump Fatigue", &FConfiguration::bDisableJumpFatigue);
            }

            //ImGui::Checkbox("Make Projectiles Rideable (WIP)", &FConfiguration::bRideableProjectiles);

            if (VersionInfo.FortniteVersion >= 19.00)
                ImGui::Checkbox("Toggle Crown Slomo", &FConfiguration::bCrownSlomo);

            if (VersionInfo.FortniteVersion >= 23.20 && VersionInfo.FortniteVersion < 25.20)
                ImGui::Checkbox("Negate Velocity on Win", &FConfiguration::bCancelVelocityOnWin);

            //ImGui::Checkbox("Down But Not Out (DBNO)", &FConfiguration::bEnableDBNO);

            ImGui::Checkbox("Auto Pause TODM", &FConfiguration::bAutoPauseTODM);

            if (FConfiguration::bAutoPauseTODM)
            {
                LabeledSliderInt("Time Of Day", "##time-of-day", &FConfiguration::TODMTime, 1, 24, Width);
            }

            if (FConfiguration::bRideableProjectiles)
            {
                static char ProjClassBuffer[512] = {};

                ImGui::InputText("Class", ProjClassBuffer, IM_ARRAYSIZE(ProjClassBuffer));

                if (ImGui::Button("Apply", ImVec2(Width, Height)))
                {
                    if (ProjClassBuffer[0] != '\0')
                    {
                        std::string ProjClass = ProjClassBuffer;
                        UClass* ProjectileClass = (UClass*)SDK::StaticLoadObject(UEAllocatedWString(ProjClass.begin(), ProjClass.end()).c_str(), SDK::UClass::StaticClass());

                        if (ProjectileClass)
                        {
                            static auto ProjectileBaseClass = FindClass("FortProjectileBase");

                            if (ProjectileClass->IsA(ProjectileBaseClass))
                            {
                                auto DefaultObject = ProjectileClass->GetDefaultObj();

                                if (DefaultObject)
                                {
                                    static auto CapsuleComponentOffset = ProjectileClass->GetOffset("CapsuleComponent");

                                    if (CapsuleComponentOffset != -1)
                                    {
                                        auto CapsuleComponent = GetFromOffset<UPrimitiveComponent*>(DefaultObject, CapsuleComponentOffset);

                                        if (CapsuleComponent)
                                        {
                                            static auto CanCharacterStepUpOnOffset = CapsuleComponent->GetOffset("bCanCharacterStepUpOn");

                                            if (CanCharacterStepUpOnOffset != -1)
                                            {
                                                GetFromOffset<bool>(CapsuleComponent, CanCharacterStepUpOnOffset) = true;
                                            }

                                            static auto WalkableSlopeOverrideOffset = CapsuleComponent->GetOffset("WalkableSlopeOverride");

                                            if (WalkableSlopeOverrideOffset != -1)
                                            {
                                                auto& SlopeOverride = GetFromOffset<FWalkableSlopeOverride>(CapsuleComponent, WalkableSlopeOverrideOffset);
                                                SlopeOverride.WalkableSlopeBehavior = EWalkableSlopeBehavior::WalkableSlope_Increase;
                                                SlopeOverride.WalkableSlopeAngle = 90.0f;
                                            }

                                            static auto CollisionEnabledOffset = CapsuleComponent->GetOffset("CollisionEnabled");

                                            if (CollisionEnabledOffset != -1)
                                            {
                                                GetFromOffset<ECollisionEnabled>(CapsuleComponent, CollisionEnabledOffset) = ECollisionEnabled::QueryOnly;
                                            }

                                            static auto SetCollisionEnabledFunc = CapsuleComponent->Class->GetFunction("SetCollisionEnabled");

                                            if (SetCollisionEnabledFunc)
                                            {
                                                struct { ECollisionEnabled NewType; } Params;
                                                Params.NewType = ECollisionEnabled::QueryOnly;
                                                CapsuleComponent->ProcessEvent(SetCollisionEnabledFunc, &Params);
                                            }

                                            static auto SetCollisionResponseToChannelFunc = CapsuleComponent->Class->GetFunction("SetCollisionResponseToChannel");

                                            if (SetCollisionResponseToChannelFunc)
                                            {
                                                struct { uint8 Channel; ECollisionResponse NewResponse; } Params;
                                                Params.Channel = 1;
                                                Params.NewResponse = ECollisionResponse::ECR_Block;
                                                CapsuleComponent->ProcessEvent(SetCollisionResponseToChannelFunc, &Params);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            EndSectionBody();

            SectionHeader("Trickshot Presets", SectionWidth);
            BeginSectionBody();

            static char TrickshotName[128]{};
            static std::vector<std::string> SavedTrickshots;
            static int SelectedTrickshot = -1;
            static std::string TrickshotMessage;
            static bool InitializedTrickshotList = false;

            if (!InitializedTrickshotList)
            {
                SavedTrickshots = TrickshotManager::GetSavedNames();
                InitializedTrickshotList = true;
            }

            ImGui::SetNextItemWidth(Width);
            ImGui::InputTextWithHint("##trickshot-name", "Trickshot Name", TrickshotName, IM_ARRAYSIZE(TrickshotName));

            ImGui::SetNextItemWidth(Width);
            const char* Preview = SelectedTrickshot >= 0 && SelectedTrickshot < SavedTrickshots.size()
                ? SavedTrickshots[SelectedTrickshot].c_str() : "Select Saved Trickshot";
            if (ImGui::BeginCombo("##saved-trickshots", Preview))
            {
                for (int Index = 0; Index < SavedTrickshots.size(); ++Index)
                {
                    const bool Selected = Index == SelectedTrickshot;
                    if (ImGui::Selectable(SavedTrickshots[Index].c_str(), Selected))
                        SelectedTrickshot = Index;
                    if (Selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            const float ButtonWidth = (Width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
            if (ImGui::Button("Save", ImVec2(ButtonWidth, Height)))
            {
                if (TrickshotManager::Save(TrickshotName, TrickshotMessage))
                {
                    const std::string SavedName = TrickshotManager::SanitizeName(TrickshotName);
                    SavedTrickshots = TrickshotManager::GetSavedNames();
                    auto It = std::find(SavedTrickshots.begin(), SavedTrickshots.end(), SavedName);
                    SelectedTrickshot = It == SavedTrickshots.end() ? -1 : static_cast<int>(std::distance(SavedTrickshots.begin(), It));
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Load", ImVec2(ButtonWidth, Height)))
            {
                TrickshotManager::Load(SelectedTrickshot >= 0 && SelectedTrickshot < SavedTrickshots.size()
                    ? SavedTrickshots[SelectedTrickshot] : "", TrickshotMessage);
            }

            if (ImGui::Button("Delete", ImVec2(ButtonWidth, Height)))
            {
                const std::string SelectedName = SelectedTrickshot >= 0 && SelectedTrickshot < SavedTrickshots.size()
                    ? SavedTrickshots[SelectedTrickshot] : "";
                if (TrickshotManager::Delete(SelectedName, TrickshotMessage))
                {
                    SavedTrickshots = TrickshotManager::GetSavedNames();
                    SelectedTrickshot = SavedTrickshots.empty() ? -1 : (std::min)(SelectedTrickshot, static_cast<int>(SavedTrickshots.size()) - 1);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Open Folder", ImVec2(ButtonWidth, Height)))
                TrickshotManager::OpenDirectory(TrickshotMessage);

            if (!TrickshotMessage.empty())
                ImGui::TextWrapped("%s", TrickshotMessage.c_str());

            EndSectionBody();

            break;
        }
        case 8:
        {
            auto rule = []()
            {
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float w = ImGui::GetContentRegionAvail().x;
                dl->AddLine(ImVec2(p.x, p.y + 1.f), ImVec2(p.x + w, p.y + 1.f), ImGui::GetColorU32(Accent(0.30f)), 1.f);
                ImGui::Dummy(ImVec2(0.f, 9.f));
            };
            auto credit = [](const char* name, const char* role, const char* url)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, Accent(0.95f));
                ImGui::TextUnformatted(name);
                ImGui::PopStyleColor();
                if (url && url[0])
                {
                    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (ImGui::IsItemClicked()) ShellExecuteA(0, "open", url, 0, 0, SW_SHOWNORMAL);
                }
                ImGui::SameLine(0.f, 8.f);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.60f, 0.66f, 1.f));
                ImGui::Text("- %s", role);
                ImGui::PopStyleColor();
                ImGui::Spacing();
            };

            // Header: logo + title
            if (g_LogoTexture)
            {
                ImGui::Image((void*)g_LogoTexture, ImVec2(44.f, 44.f));
                ImGui::SameLine(0.f, 12.f);
            }
            ImGui::BeginGroup();
            ImGui::PushStyleColor(ImGuiCol_Text, Accent());
            ImGui::TextUnformatted("MAGNESIUM");
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.54f, 0.56f, 0.62f, 1.f));
            ImGui::TextUnformatted("Gameserver  -  v2.1.0");
            ImGui::PopStyleColor();
            ImGui::EndGroup();

            ImGui::Dummy(ImVec2(0.f, 14.f));

            ImGui::PushStyleColor(ImGuiCol_Text, Accent(0.85f));
            ImGui::TextUnformatted("CORE");
            ImGui::PopStyleColor();
            rule();
            credit("Erbium", "Base of the project", "https://github.com/plooshi/Erbium");

            const bool anyContrib = FConfiguration::bInfiniteRender
                || SelectedPlaylist == static_cast<int>(Playlist::Gav)
                || SelectedPlaylist == static_cast<int>(Playlist::Retrac1v1)
                || SelectedPlaylist == static_cast<int>(Playlist::RetracTurtle)
                || SelectedPlaylist == static_cast<int>(Playlist::OnlyUp)
                || SelectedPlaylist == static_cast<int>(Playlist::TiltedZW);

            if (anyContrib)
            {
                ImGui::Dummy(ImVec2(0.f, 10.f));
                ImGui::PushStyleColor(ImGuiCol_Text, Accent(0.85f));
                ImGui::TextUnformatted("CONTRIBUTORS");
                ImGui::PopStyleColor();
                rule();

                if (FConfiguration::bInfiniteRender)
                    credit("Sweefy / Milxnor", "Infinite Render research", "https://x.com/Sweefyyy");
                if (SelectedPlaylist == static_cast<int>(Playlist::Gav))
                    credit("Gav", "Maker of the 27.11 1v1 map", "https://github.com/gavbowersdomain/27.11-Mods/tree/main/Mods/1v1");
                if (SelectedPlaylist == static_cast<int>(Playlist::Retrac1v1) || SelectedPlaylist == static_cast<int>(Playlist::RetracTurtle))
                    credit("Retrac", "Creator of the 1v1 & Turtle Fight maps", "https://discord.gg/retrac");
                if (SelectedPlaylist == static_cast<int>(Playlist::OnlyUp) || SelectedPlaylist == static_cast<int>(Playlist::TiltedZW))
                    credit("Jett", "Maker of the Only Up & Tilted FFA maps", "https://discord.com/channels/1469866169635962884/1473850399994806362/1473850399994806362");
            }

            ImGui::Dummy(ImVec2(0.f, 18.f));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.42f, 0.44f, 0.50f, 1.f));
            ImGui::TextUnformatted("Thank you for using Magnesium.");
            ImGui::PopStyleColor();

            break;
        }
        default:
        {
            break;
        }
        }

        ImGui::Dummy(ImVec2(0.0f, 40.0f));
        ImGui::EndChild();      // content panel
        ImGui::PopStyleVar();   // content WindowPadding
        ImGui::PopStyleColor(); // content ChildBg
        ImGui::End();           // window


        ImGui::Render();
        const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    if (g_EmbedTexture)
        g_EmbedTexture->Release();

    g_pSwapChain->Release();
    g_pd3dDeviceContext->Release();
    g_pd3dDevice->Release();
    DestroyWindow(hWnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);
    TerminateProcess(GetCurrentProcess(), 0);
}
