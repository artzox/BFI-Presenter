// Runs the presenter's real shaders and dx/bfi_warp.h's passes on two frames and writes the warped result.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include "gen_presenter.h"
#include "../../dx/bfi_warp.h"
typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
#define CHECK(x) do { HRESULT _h = (x); if (FAILED(_h)) { printf("FAILED 0x%08lX: %s (line %d)\n", (unsigned long)_h, #x, __LINE__); fflush(stdout); exit(1); } } while (0)
static ID3D12Device *dev; static ID3D12CommandQueue *q; static ID3D12CommandAllocator *al; static ID3D12GraphicsCommandList *L;
static ID3D12Fence *fence; static UINT64 fv; static HANDLE ev;
static void Run()
{
    CHECK(L->Close());
    ID3D12CommandList *ls[] = { L }; q->ExecuteCommandLists(1, ls);
    q->Signal(fence, ++fv); fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, INFINITE);
    CHECK(dev->GetDeviceRemovedReason());
    CHECK(al->Reset()); CHECK(L->Reset(al, nullptr));
}
static ID3DBlob *Comp(PFN_D3DCompile c, const char *src, size_t n, const char *e, const char *t)
{
    ID3DBlob *o = nullptr, *er = nullptr;
    HRESULT hr = c(src, n, "x", nullptr, nullptr, e, t, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &o, &er);
    if (FAILED(hr)) { printf("compile %s: %s\n", e, er ? (char *)er->GetBufferPointer() : "?"); exit(1); }
    return o;
}
static ID3D12Resource *Tex(UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st)
{
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.Format = f; rd.SampleDesc.Count = 1; rd.Flags = fl;
    ID3D12Resource *r = nullptr; CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r)));
    return r;
}
static ID3D12Resource *Buf(UINT64 n, D3D12_HEAP_TYPE t, D3D12_RESOURCE_STATES st)
{
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = t;
    D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = n; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource *r = nullptr; CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, st, nullptr, IID_PPV_ARGS(&r)));
    return r;
}
static void Upload(ID3D12Resource *tex, const char *file, UINT w, UINT h)   // tex in COMMON, left in COMMON
{
    std::vector<uint8_t> d((size_t)w * h * 4);
    FILE *f = fopen(file, "rb"); if (!f || fread(d.data(), 1, d.size(), f) != d.size()) { printf("no %s\n", file); exit(1); } fclose(f);
    const UINT pitch = (w * 4 + 255) & ~255u;
    ID3D12Resource *up = Buf((UINT64)pitch * h, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t *p = nullptr; CHECK(up->Map(0, nullptr, (void **)&p));
    for (UINT y = 0; y < h; y++) memcpy(p + (size_t)y * pitch, d.data() + (size_t)y * w * 4, (size_t)w * 4);
    up->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    dst.pResource = tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = up; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM; src.PlacedFootprint.Footprint.Width = w;
    src.PlacedFootprint.Footprint.Height = h; src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = pitch;
    D3D12_RESOURCE_BARRIER b = WarpGpu::Tr(tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    L->ResourceBarrier(1, &b);
    L->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b = WarpGpu::Tr(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    L->ResourceBarrier(1, &b);
    Run(); up->Release();
}
// tex in `st`; writes raw bytes (bpp bytes per pixel)
static void Download(ID3D12Resource *tex, D3D12_RESOURCE_STATES st, UINT w, UINT h, DXGI_FORMAT f, UINT bpp, const char *file)
{
    const UINT pitch = (w * bpp + 255) & ~255u;
    ID3D12Resource *rb = Buf((UINT64)pitch * h, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
    src.pResource = tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = f; dst.PlacedFootprint.Footprint.Width = w; dst.PlacedFootprint.Footprint.Height = h;
    dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = pitch;
    D3D12_RESOURCE_BARRIER b = WarpGpu::Tr(tex, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
    L->ResourceBarrier(1, &b);
    L->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b = WarpGpu::Tr(tex, D3D12_RESOURCE_STATE_COPY_SOURCE, st);
    L->ResourceBarrier(1, &b);
    Run();
    uint8_t *p = nullptr; D3D12_RANGE rr = { 0, (SIZE_T)pitch * h }; CHECK(rb->Map(0, &rr, (void **)&p));
    FILE *o = fopen(file, "wb");
    for (UINT y = 0; y < h; y++) fwrite(p + (size_t)y * pitch, 1, (size_t)w * bpp, o);
    fclose(o); rb->Unmap(0, nullptr); rb->Release();
}
int main(int argc, char **argv)
{
    const UINT W = 1920, H = 1080;
    const int finest = argc > 1 ? atoi(argv[1]) : 1;
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    PFN_D3DCompile compile = (PFN_D3DCompile)(void *)GetProcAddress(dc, "D3DCompile");
    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)));
    D3D12_COMMAND_QUEUE_DESC qd = {}; CHECK(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));
    CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al)));
    CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&L)));
    CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))); ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);

    ID3DBlob *vs = Comp(compile, kShader, sizeof(kShader) - 1, "VSMain", "vs_5_0");
    ID3DBlob *ps = Comp(compile, kShader, sizeof(kShader) - 1, "PSMain", "ps_5_0");
    ID3DBlob *pw[kWarpShN];
    for (int i = 0; i < kWarpShN; i++) pw[i] = Comp(compile, kWarpShader, sizeof(kWarpShader) - 1, kWarpEntry[i], "ps_5_0");
    printf("shaders compiled\n"); fflush(stdout);

    // the presenter's root signature
    D3D12_DESCRIPTOR_RANGE range[5] = {};
    D3D12_ROOT_PARAMETER params[6] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = sizeof(Constants) / 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    for (int i = 0; i < 5; i++)
    {
        range[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; range[i].NumDescriptors = 1; range[i].BaseShaderRegister = (UINT)i;
        params[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1 + i].DescriptorTable.NumDescriptorRanges = 1; params[1 + i].DescriptorTable.pDescriptorRanges = &range[i];
        params[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX; samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {}; rsd.NumParameters = 6; rsd.pParameters = params; rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &samp;
    ID3DBlob *rsb = nullptr, *rse = nullptr; ID3D12RootSignature *rs = nullptr;
    CHECK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &rse));
    CHECK(dev->CreateRootSignature(0, rsb->GetBufferPointer(), rsb->GetBufferSize(), IID_PPV_ARGS(&rs)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = rs; pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() }; pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL; pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE; pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso = nullptr; CHECK(dev->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso)));

    const UINT kStage = 2;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = kStage + WarpGpu::kSrvCount;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12DescriptorHeap *srvHeap = nullptr, *rtvHeap = nullptr; CHECK(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap)));
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors = 1; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    CHECK(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)));
    const UINT ss = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ID3D12Resource *stage[kStage];
    for (UINT i = 0; i < kStage; i++)
    {
        stage[i] = Tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
        D3D12_CPU_DESCRIPTOR_HANDLE sh = srvHeap->GetCPUDescriptorHandleForHeapStart(); sh.ptr += (SIZE_T)i * ss;
        dev->CreateShaderResourceView(stage[i], nullptr, sh);
    }
    ID3D12Resource *out = Tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
    dev->CreateRenderTargetView(out, nullptr, rtvHeap->GetCPUDescriptorHandleForHeapStart());
    Upload(stage[0], "f0.rgba", W, H);
    Upload(stage[1], "f1.rgba", W, H);

    WarpGpu warp;
    if (!warp.Init(dev, rs, vs, pw, srvHeap, kStage, W, H)) { printf("warp.Init failed\n"); return 1; }
    warp.finest = finest;
    printf("warp levels: "); for (int l = 0; l < WarpGpu::kLevels; l++) printf("%ux%u ", warp.w[l], warp.h[l]); printf(" finest %d\n", finest); fflush(stdout);

    auto srvOf = [&](UINT i) { D3D12_GPU_DESCRIPTOR_HANDLE g = srvHeap->GetGPUDescriptorHandleForHeapStart(); g.ptr += (UINT64)i * ss; return g; };
    // the presenter's sequence for two frames, then refreshes of frame 2 shown moved
    const float shares[] = { 0.0f, 0.5f, 1.0f };
    for (int pass = 0; pass < 4; pass++)
    {
        const UINT pick = pass == 0 ? 0u : 1u;
        const UINT64 pickId = pick + 100;
        const double pickT = 1.0 + 0.0185 * pick;
        Constants c = {};
        c.frames = 2; c.method = 2; c.gain = 0.5f; c.knee = 0.75f; c.ceilLin = 1.0f; c.srcPerOutX = c.srcPerOutY = 1.0f;
        UINT64 old = 0;
        const double ahead = pass == 0 ? 0.0 : shares[pass - 1];
        if (ahead > 0.0 && (old = warp.PairFor(pickId, pickT)) != 0) c.warp = (float)(ahead / (double)(pickId - old));
        D3D12_RESOURCE_BARRIER b = WarpGpu::Tr(stage[pick], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        L->ResourceBarrier(1, &b);
        if (c.warp != 0.0f) { b = WarpGpu::Tr(stage[0], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); L->ResourceBarrier(1, &b); }
        L->SetDescriptorHeaps(1, &srvHeap);
        L->SetGraphicsRootSignature(rs);
        L->SetPipelineState(pso);
        L->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &c, 0);
        L->SetGraphicsRootDescriptorTable(1, srvOf(pick));
        L->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        bool touched = false;
        if (pickId != warp.id[warp.cur]) { warp.RecordLuma(L, srvOf(pick), pickId, pickT, false); touched = true; }
        if (c.warp != 0.0f && (warp.flowNew != pickId || warp.flowOld != old)) { warp.RecordFlow(L); touched = true; printf("pass %d: motion measured\n", pass); }
        if (touched) { L->SetPipelineState(pso); L->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &c, 0); L->SetGraphicsRootDescriptorTable(1, srvOf(pick)); }
        L->SetGraphicsRootDescriptorTable(4, warp.FlowSrv());
        L->SetGraphicsRootDescriptorTable(5, srvOf(c.warp != 0.0f ? 0u : pick));
        L->SetGraphicsRootDescriptorTable(2, srvOf(pick));
        L->SetGraphicsRootDescriptorTable(3, srvOf(pick));
        D3D12_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 }; D3D12_RECT sc = { 0, 0, (LONG)W, (LONG)H };
        L->RSSetViewports(1, &vp); L->RSSetScissorRects(1, &sc);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        L->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        L->DrawInstanced(3, 1, 0, 0);
        b = WarpGpu::Tr(stage[pick], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        L->ResourceBarrier(1, &b);
        if (c.warp != 0.0f) { b = WarpGpu::Tr(stage[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON); L->ResourceBarrier(1, &b); }
        LARGE_INTEGER t0, t1, fq; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
        Run();
        QueryPerformanceCounter(&t1);
        printf("pass %d: frame %llu warp %.3f  (%.0f ms on the software GPU)\n", pass, (unsigned long long)pickId, c.warp,
               (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart); fflush(stdout);
        char name[64]; snprintf(name, sizeof(name), "out%d.rgba", pass);
        Download(out, D3D12_RESOURCE_STATE_RENDER_TARGET, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, 4, name);
    }
    Download(warp.flowOut[finest], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, warp.w[finest], warp.h[finest], DXGI_FORMAT_R16G16B16A16_FLOAT, 8, "flow.f16");
    Download(warp.luma[warp.cur][0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, warp.w[0], warp.h[0], DXGI_FORMAT_R16_FLOAT, 2, "luma0.f16");
    printf("flow %ux%u done\n", warp.w[finest], warp.h[finest]);
    warp.Release();
    return 0;
}
