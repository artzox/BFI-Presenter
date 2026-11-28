// ============================================================================
// BFI Presenter -- Vulkan layer: CRT-Standalone's mask and scanlines after frame warp (CrtStandaloneHandoff, 1.0.54 test)
//
// Copyright (C) 2026 artzox -- https://github.com/artzox/BFI-Presenter
// SPDX-License-Identifier: GPL-3.0-or-later
//
// As the DX add-on (dx/bfi_crt.h): frame warp moves the finished frame, CRT-Standalone's fine mask and scanlines
// included, by different amounts over the screen -- curved bands of moire (Uncharted with frame interpolation). With
// BFI_PRESENTER_HANDOFF=1 in its preprocessor definitions, CRT-Standalone stops its main pass before the mask; the
// presenter draws the rest after frame warp, before the strobe, with the shader's own code and settings.
//
//   Game's thread, reshade_finish_effects (bfi_presenter_vk.cpp, crtvk::on_finish): the shader's switches and settings,
//   and its halation, glow and wide glow textures copied (ReShade's, this frame's) into crtCur.
//   Game's thread, its present (GameFrame): those copies into the frame's staging slot (crtSlot), with the frame.
//   Presenter, for each new picture (a strobe cycle's dark refresh reuses it), on its compute queue:
//     crtwarp.comp  the frame as the BFI pass would take it (moved by frame warp) into W, and where each pixel came from into S
//     crt.hlsl      CRT-Standalone's pattern (W -> A), then the passes after it that are on: sharpen, soften, grain's
//                   first half (-> G), and grain's second half, noise floor, gamut and corners in one (-> F)
//     the BFI pass reads F instead of the frame (set 2 binding 6, flags & 524288).
// The DX add-on compiles CRT-Standalone's code with its switches; the layer has no compiler with it, so its passes are
// compiled when it is built with every switch on, and a switch that is off is given its off value instead (a strength
// of 0: the shader's own test then leaves the picture as it is). ENABLE_BEAM_MODULATION changes the code: two builds.
// Not yet as the DX add-on: the CRT picture made with the frame on the game's queue (here every new picture is drawn by
// the presenter), what is drawn after the effects shown without the pattern (ReShade's window gets the mask too), and
// the realtime compute queue only (the graphics path draws without the hand-off). 64-bit, PIPELINE 0, ENABLE_SCANLINE_SS 0,
// 8-bit frames (the passes are 8-bit, as ReShade's back buffer CRT-Standalone draws into for such a game).
// ============================================================================
#pragma once
#include "../dx/bfi_crt_gen.h"   // CRT-Standalone's settings read (kCrtParams); its code is compiled into shaders_spv.h

namespace crtvk
{
// Game's thread -> presenter
static std::mutex lk;
static float params[kCrtSlots * 4] = {};        // one float4 per setting (kCrtParams), then the frame size
static int   beam = 1;                          // ENABLE_BEAM_MODULATION
static int   refH = 0;                          // SCANLINE_REFERENCE_HEIGHT
static std::atomic<int> state{0};               // 0 off, 1 the shader isn't handing off, 2 handing off, -1 not supported

static int ParamIndex(const char *name)
{
    for (int i = 0; i < kCrtParamN; i++) if (!strcmp(kCrtParams[i].name, name)) return i;
    return -1;
}
static float ParamOf(const float *cp, const char *name)
{
    const int i = ParamIndex(name);
    return i >= 0 ? cp[i * 4] : 1.0f;
}
// The presenter: this frame's settings, with the frame size (w, h).
static void ParamsFor(float *out, float w, float h, int *beamOut)
{
    int rh;
    {
        std::lock_guard<std::mutex> l(lk);
        memcpy(out, params, sizeof(params));
        *beamOut = beam;
        rh = refH;
    }
    out[kCrtParamN * 4 + 0] = w;
    out[kCrtParamN * 4 + 1] = h;
    out[kCrtParamN * 4 + 2] = 0.0f;                 // what is drawn after the effects: not told apart yet
    // SCANLINE_REFERENCE_HEIGHT (compiled as 0): the width it would give at this height
    const int sw = ParamIndex("crt_scanline_width");
    if (rh > 0 && sw >= 0) out[sw * 4] *= h / (float)rh;
}
// The passes after the pattern: drawn where they change the picture (as the DX add-on's PostOn)
static bool SharpenOn(const float *cp) { return ParamOf(cp, "crt_sharpen_strength") >= 0.001f; }
static bool SoftenOn(const float *cp)  { return ParamOf(cp, "crt_soften_strength") >= 0.001f; }
static bool GrainOn(const float *cp)   { return ParamOf(cp, "crt_grain_intensity") > 0.001f; }

// ---- Game's thread, at its present: this frame's halation and glow copies into the staging slot ----
static void DestroySlots(DeviceData *d, Swapchain *sc)
{
    for (int k = 0; k < 3; k++)
    {
        if (sc->crtCur[k]) d->DestroyImage(d->device, sc->crtCur[k], nullptr);
        if (sc->crtCurMem[k]) d->FreeMemory(d->device, sc->crtCurMem[k], nullptr);
        sc->crtCur[k] = VK_NULL_HANDLE; sc->crtCurMem[k] = VK_NULL_HANDLE; sc->crtCurHas[k] = false;
        for (uint32_t s = 0; s < kStage; s++)
        {
            if (sc->crtSlotView[k][s]) d->DestroyImageView(d->device, sc->crtSlotView[k][s], nullptr);
            if (sc->crtSlot[k][s]) d->DestroyImage(d->device, sc->crtSlot[k][s], nullptr);
            if (sc->crtSlotMem[k][s]) d->FreeMemory(d->device, sc->crtSlotMem[k][s], nullptr);
            sc->crtSlotView[k][s] = VK_NULL_HANDLE; sc->crtSlot[k][s] = VK_NULL_HANDLE; sc->crtSlotMem[k][s] = VK_NULL_HANDLE;
        }
    }
    for (uint32_t s = 0; s < kStage; s++) sc->crtSlotOk[s] = false;
    sc->crtCurOk = false; sc->crtSlotsMade = false;
}
static void CopyAtPresent(DeviceData *d, Swapchain *sc, VkCommandBuffer cb, uint32_t slot)
{
    if (slot >= kStage) return;
    const bool have = sc->crtCurOk.exchange(false);
    if (!have || state.load() != 2) { sc->crtSlotOk[slot] = false; return; }
    if (!sc->crtSlotsMade)
    {
        // the slots' copies, once per swap chain (their sizes and formats are the shader's, known from its first frame)
        VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D; ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL; ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.sharingMode = d->families.size() > 1 ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
        ici.queueFamilyIndexCount = d->families.size() > 1 ? (uint32_t)d->families.size() : 0;
        ici.pQueueFamilyIndices = d->families.size() > 1 ? d->families.data() : nullptr;
        bool ok = true;
        for (int k = 0; ok && k < 3; k++)
        {
            if (!sc->crtCurHas[k]) continue;            // compiled out of the shader: a stand-in is bound
            ici.format = sc->crtFmt[k]; ici.extent = { sc->crtW[k], sc->crtH[k], 1 };
            for (uint32_t s = 0; ok && s < kStage; s++)
            {
                ok = CreateImageMem(d, ici, &sc->crtSlot[k][s], &sc->crtSlotMem[k][s]);
                VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
                vci.image = sc->crtSlot[k][s]; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = ici.format;
                vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                ok = ok && d->CreateImageView(d->device, &vci, nullptr, &sc->crtSlotView[k][s]) == VK_SUCCESS;
            }
        }
        sc->crtSlotsMade = true;
        sc->crtSlotsFailed = !ok;
        sc->crtSlotsInit = false;
        sc->crtSlotGen++;
        if (!ok) LOGW("BFI Presenter: CRT hand-off: the textures for CRT-Standalone's halation and glow could not be made -- off for this swap chain");
    }
    if (sc->crtSlotsFailed) { sc->crtSlotOk[slot] = false; return; }
    for (int k = 0; k < 3; k++)
    {
        if (!sc->crtCurHas[k] || !sc->crtSlot[k][slot]) continue;
        VkImageMemoryBarrier b = ImgBarrier(sc->crtSlot[k][slot], sc->crtSlotsInit ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkImageCopy ic = {};
        ic.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        ic.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        ic.extent = { sc->crtW[k], sc->crtH[k], 1 };
        d->CmdCopyImage(cb, sc->crtCur[k], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->crtSlot[k][slot], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
        b = ImgBarrier(sc->crtSlot[k][slot], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
        d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    // the other slots' copies were never written: in SHADER_READ_ONLY from here (UNDEFINED -> it, contents undefined
    // until written; read only once their slot is ok)
    if (!sc->crtSlotsInit)
    {
        std::vector<VkImageMemoryBarrier> bs;
        for (int k = 0; k < 3; k++)
            for (uint32_t s = 0; s < kStage; s++)
                if (s != slot && sc->crtSlot[k][s])
                    bs.push_back(ImgBarrier(sc->crtSlot[k][s], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0));
        if (!bs.empty())
            d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, (uint32_t)bs.size(), bs.data());
        sc->crtSlotsInit = true;
    }
    sc->crtSlotOk[slot] = true;
}
} // namespace crtvk

// ---- Presenter: the passes ----
struct CrtGpu
{
    // W the frame as shown, S where each pixel came from (r32ui: two halves), A B in turn, G grain's first half, F final;
    // [6] a 1x1 stand-in for a texture compiled out of the shader
    static const int kW = 0, kS = 1, kA = 2, kB = 3, kG = 4, kF = 5, kDummy = 6, kImgN = 7;
    VkImage        img[kImgN] = {};
    VkDeviceMemory mem[kImgN] = {};
    VkImageView    view[kImgN] = {};
    uint32_t       w = 0, h = 0;
    VkSampler      lin = VK_NULL_HANDLE;
    // crtwarp.comp: the BFI pass's sets 0-2 and push constants, set 3 its two outputs
    VkDescriptorSetLayout dslCW = VK_NULL_HANDLE;
    VkPipelineLayout      plCW = VK_NULL_HANDLE;
    VkShaderModule        csCW = VK_NULL_HANDLE;
    VkPipeline            psoCW = VK_NULL_HANDLE;
    // crt.hlsl: one set layout (0-5 sampled, 6 sampler, 7 storage out, 8 settings (dynamic offset), 9 S)
    VkDescriptorSetLayout dslP = VK_NULL_HANDLE;
    VkPipelineLayout      plP = VK_NULL_HANDLE;
    static const int kPat0 = 0, kPat1 = 1, kSharpen = 2, kSoften = 3, kGrain = 4, kTail = 5, kPassN = 6;
    VkShaderModule        cs[kPassN] = {};
    VkPipeline            pso[kPassN] = {};
    VkDescriptorPool      dp = VK_NULL_HANDLE;
    VkDescriptorSet       dsCW = VK_NULL_HANDLE;
    VkDescriptorSet       dsPat[kStage] = {};             // W, S, the slot's halation and glow -> A
    VkDescriptorSet       dsSharpen = VK_NULL_HANDLE;      // A -> B
    VkDescriptorSet       dsSoften[2] = {};                // [0] A -> B, [1] B -> A
    VkDescriptorSet       dsGrain[2] = {};                 // A / B -> G
    VkDescriptorSet       dsTail[2] = {};                  // A / B with G -> F
    // the settings: one block per real image (its command buffer isn't recorded again before its fence)
    VkBuffer       ubo = VK_NULL_HANDLE;
    VkDeviceMemory uboMem = VK_NULL_HANDLE;
    uint8_t       *uboPtr = nullptr;
    uint32_t       uboStride = 0;
    bool     imgOk = false, ok = false, inGeneral = false;
    uint32_t slotGenSeen = ~0u;
    // the last picture drawn (a strobe cycle's dark refresh, or a repeat, reuses it)
    uint64_t keyId = 0; float keyWarp = 0.0f; uint32_t keySrc = 0, keyFlags = 0; int keyPrev = -2, keyPick = -1;

    bool MakeImg(DeviceData *d, int i, VkFormat f, uint32_t iw, uint32_t ih, VkImageUsageFlags use)
    {
        VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = f; ici.extent = { iw, ih, 1 }; ici.mipLevels = 1; ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL; ici.usage = use;
        if (!CreateImageMem(d, ici, &img[i], &mem[i])) return false;
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = img[i]; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = f;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        return d->CreateImageView(d->device, &vci, nullptr, &view[i]) == VK_SUCCESS;
    }
    // Before frame warp's set 2 is written (its binding 6 is F): the textures, about 230 MB at 4K.
    bool InitImages(DeviceData *d, uint32_t fw, uint32_t fh)
    {
        w = fw; h = fh;
        const VkImageUsageFlags su = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        bool p = true;
        for (int i = 0; p && i < kImgN; i++)
        {
            if (i == kS) p = MakeImg(d, i, VK_FORMAT_R32_UINT, fw, fh, su);
            else if (i == kDummy) p = MakeImg(d, i, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, su);
            else p = MakeImg(d, i, VK_FORMAT_R8G8B8A8_UNORM, fw, fh, su);
        }
        VkSamplerCreateInfo smi = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        smi.magFilter = smi.minFilter = VK_FILTER_LINEAR;
        smi.addressModeU = smi.addressModeV = smi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        p = p && d->CreateSampler(d->device, &smi, nullptr, &lin) == VK_SUCCESS;
        imgOk = p;
        inGeneral = false;
        if (!p) Release(d);
        return p;
    }
    VkImageView FinalView() const { return imgOk ? view[kF] : VK_NULL_HANDLE; }

    void WriteP(DeviceData *d, VkDescriptorSet set, VkImageView in, VkImageView halo, VkImageView glow, VkImageView wide,
                VkImageView prev, VkImageView grain, VkImageView out)
    {
        const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL, R = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkImageView dm = view[kDummy];
        VkDescriptorImageInfo ii[10] = {
            { VK_NULL_HANDLE, in ? in : dm, G }, { VK_NULL_HANDLE, halo ? halo : dm, halo ? R : G }, { VK_NULL_HANDLE, glow ? glow : dm, glow ? R : G },
            { VK_NULL_HANDLE, wide ? wide : dm, wide ? R : G }, { VK_NULL_HANDLE, prev ? prev : dm, G }, { VK_NULL_HANDLE, grain ? grain : dm, G },
            { lin, VK_NULL_HANDLE, G }, { VK_NULL_HANDLE, out, G }, {}, { VK_NULL_HANDLE, view[kS], G } };
        VkDescriptorBufferInfo bi = { ubo, 0, (VkDeviceSize)(kCrtSlots * 16) };
        VkWriteDescriptorSet wr[10] = {};
        for (int k = 0; k < 10; k++)
        {
            wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[k].dstSet = set; wr[k].dstBinding = (uint32_t)k; wr[k].descriptorCount = 1;
            wr[k].descriptorType = k <= 5 || k == 9 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : k == 6 ? VK_DESCRIPTOR_TYPE_SAMPLER :
                                   k == 7 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            if (k == 8) wr[k].pBufferInfo = &bi; else wr[k].pImageInfo = &ii[k];
        }
        d->UpdateDescriptorSets(d->device, 10, wr, 0, nullptr);
    }
    // After the realtime compute queue's resources (the BFI pass's set layouts and push constants).
    bool InitPasses(DeviceData *d, VkDescriptorSetLayout dslC, VkDescriptorSetLayout dslN, VkDescriptorSetLayout dslW, uint32_t pcSize, uint32_t nReal)
    {
        if (!imgOk || !d->CreateComputePipelines || !d->CmdDispatch) return false;
        VkDevice dev = d->device;
        bool p = true;
        {
            VkDescriptorSetLayoutBinding b[2] = {};
            for (int i = 0; i < 2; i++) { b[i].binding = (uint32_t)i; b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; }
            VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            dli.bindingCount = 2; dli.pBindings = b;
            p = d->CreateDescriptorSetLayout(dev, &dli, nullptr, &dslCW) == VK_SUCCESS;
            VkDescriptorSetLayout sl[4] = { dslC, dslN, dslW, dslCW };
            VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, pcSize };
            VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pli.setLayoutCount = 4; pli.pSetLayouts = sl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
            p = p && d->CreatePipelineLayout(dev, &pli, nullptr, &plCW) == VK_SUCCESS;
            csCW = p ? MakeShader(d, kCrtWarpSpv, sizeof(kCrtWarpSpv)) : VK_NULL_HANDLE;
            psoCW = csCW ? MakeComputePipeline(d, plCW, csCW) : VK_NULL_HANDLE;
            p = psoCW != VK_NULL_HANDLE;
            if (!p) LOGW("BFI Presenter: CRT hand-off: its warp pass could not be made");
        }
        if (p)
        {
            VkDescriptorSetLayoutBinding b[10] = {};
            for (int i = 0; i < 10; i++)
            {
                b[i].binding = (uint32_t)i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
                b[i].descriptorType = i <= 5 || i == 9 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : i == 6 ? VK_DESCRIPTOR_TYPE_SAMPLER :
                                      i == 7 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            }
            VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            dli.bindingCount = 10; dli.pBindings = b;
            p = d->CreateDescriptorSetLayout(dev, &dli, nullptr, &dslP) == VK_SUCCESS;
            VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pli.setLayoutCount = 1; pli.pSetLayouts = &dslP;
            p = p && d->CreatePipelineLayout(dev, &pli, nullptr, &plP) == VK_SUCCESS;
            const uint32_t *code[kPassN] = { kCrtPattern0Spv, kCrtPattern1Spv, kCrtSharpenSpv, kCrtSoftenSpv, kCrtGrainRawSpv, kCrtTailSpv };
            const size_t bytes[kPassN] = { sizeof(kCrtPattern0Spv), sizeof(kCrtPattern1Spv), sizeof(kCrtSharpenSpv), sizeof(kCrtSoftenSpv),
                                           sizeof(kCrtGrainRawSpv), sizeof(kCrtTailSpv) };
            for (int i = 0; p && i < kPassN; i++)
            {
                cs[i] = MakeShader(d, code[i], bytes[i]);
                pso[i] = cs[i] ? MakeComputePipeline(d, plP, cs[i]) : VK_NULL_HANDLE;
                p = pso[i] != VK_NULL_HANDLE;
                if (!p) LOGW("BFI Presenter: CRT hand-off: its pass %d (pattern, pattern, sharpen, soften, grain, tail) could not be made", i);
            }
        }
        if (p)
        {
            // the settings, one block per real image
            VkPhysicalDeviceProperties pp = {};
            d->inst->GetPhysicalDeviceProperties(d->phys, &pp);
            const uint32_t al = (uint32_t)std::max<VkDeviceSize>(16, pp.limits.minUniformBufferOffsetAlignment);
            uboStride = (kCrtSlots * 16 + al - 1) / al * al;
            VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = (VkDeviceSize)uboStride * std::max<uint32_t>(nReal, 1) ; bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            p = d->CreateBuffer(dev, &bci, nullptr, &ubo) == VK_SUCCESS;
            if (p)
            {
                VkMemoryRequirements mr; d->GetBufferMemoryRequirements(dev, ubo, &mr);
                VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
                ai.allocationSize = mr.size;
                ai.memoryTypeIndex = FindMemType(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                p = ai.memoryTypeIndex != UINT32_MAX && d->AllocateMemory(dev, &ai, nullptr, &uboMem) == VK_SUCCESS &&
                    d->BindBufferMemory(dev, ubo, uboMem, 0) == VK_SUCCESS &&
                    d->MapMemory(dev, uboMem, 0, VK_WHOLE_SIZE, 0, (void **)&uboPtr) == VK_SUCCESS;
            }
        }
        if (p)
        {
            const uint32_t nSets = 1 + kStage + 1 + 2 + 2 + 2;
            VkDescriptorPoolSize ps[4] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 + nSets }, { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 7 * nSets },
                                           { VK_DESCRIPTOR_TYPE_SAMPLER, nSets }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, nSets } };
            VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            dpi.maxSets = nSets; dpi.poolSizeCount = 4; dpi.pPoolSizes = ps;
            p = d->CreateDescriptorPool(dev, &dpi, nullptr, &dp) == VK_SUCCESS;
            std::vector<VkDescriptorSetLayout> l(nSets, dslP);
            l[0] = dslCW;
            std::vector<VkDescriptorSet> sets(nSets, VK_NULL_HANDLE);
            VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dai.descriptorPool = dp; dai.descriptorSetCount = nSets; dai.pSetLayouts = l.data();
            p = p && d->AllocateDescriptorSets(dev, &dai, sets.data()) == VK_SUCCESS;
            if (p)
            {
                uint32_t k = 0;
                dsCW = sets[k++];
                for (uint32_t s = 0; s < kStage; s++) dsPat[s] = sets[k++];
                dsSharpen = sets[k++];
                dsSoften[0] = sets[k++]; dsSoften[1] = sets[k++];
                dsGrain[0] = sets[k++]; dsGrain[1] = sets[k++];
                dsTail[0] = sets[k++]; dsTail[1] = sets[k++];
                VkDescriptorImageInfo ii[2] = { { VK_NULL_HANDLE, view[kW], VK_IMAGE_LAYOUT_GENERAL }, { VK_NULL_HANDLE, view[kS], VK_IMAGE_LAYOUT_GENERAL } };
                VkWriteDescriptorSet wr[2] = {};
                for (int i = 0; i < 2; i++)
                {
                    wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[i].dstSet = dsCW; wr[i].dstBinding = (uint32_t)i;
                    wr[i].descriptorCount = 1; wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wr[i].pImageInfo = &ii[i];
                }
                d->UpdateDescriptorSets(dev, 2, wr, 0, nullptr);
                for (uint32_t s = 0; s < kStage; s++) WriteP(d, dsPat[s], view[kW], VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, view[kA]);
                WriteP(d, dsSharpen, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, view[kA], VK_NULL_HANDLE, view[kB]);
                WriteP(d, dsSoften[0], VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, view[kA], VK_NULL_HANDLE, view[kB]);
                WriteP(d, dsSoften[1], VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, view[kB], VK_NULL_HANDLE, view[kA]);
                WriteP(d, dsGrain[0], VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, view[kA], VK_NULL_HANDLE, view[kG]);
                WriteP(d, dsGrain[1], VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, view[kB], VK_NULL_HANDLE, view[kG]);
                WriteP(d, dsTail[0], VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, view[kA], view[kG], view[kF]);
                WriteP(d, dsTail[1], VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, view[kB], view[kG], view[kF]);
            }
        }
        ok = p;
        slotGenSeen = ~0u; keyId = 0;
        return p;
    }

    // The slots' halation and glow copies (made on the game's thread at its present): the pattern's sets pointed at them.
    // Only while no slot is ok for the hand-off yet (made once per swap chain), so no set written here is in use.
    void SeeSlots(DeviceData *d, Swapchain *sc)
    {
        const uint32_t g = sc->crtSlotGen.load();
        if (g == slotGenSeen) return;
        slotGenSeen = g;
        for (uint32_t s = 0; s < kStage; s++)
            WriteP(d, dsPat[s], view[kW], sc->crtSlotView[0][s], sc->crtSlotView[1][s], sc->crtSlotView[2][s], VK_NULL_HANDLE, VK_NULL_HANDLE, view[kA]);
    }

    static void Barrier(DeviceData *d, VkCommandBuffer cb, VkPipelineStageFlags from, VkPipelineStageFlags to)
    {
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        d->CmdPipelineBarrier(cb, from, to, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    void Pass(DeviceData *d, VkCommandBuffer cb, int which, VkDescriptorSet set, uint32_t uboOff)
    {
        d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pso[which]);
        d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, plP, 0, 1, &set, 1, &uboOff);
        d->CmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
        Barrier(d, cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    // The CRT picture for this refresh into F (false: not drawn -- the BFI pass shows the frame as usual).
    // c: this refresh's constants (frame warp as the BFI pass will use it); sets 0-2 as the BFI pass binds them.
    template <class C>
    bool Record(DeviceData *d, VkCommandBuffer cb, Swapchain *sc, const C &c, uint32_t pick, uint64_t pickId, int prev, uint32_t ring,
                VkPipelineLayout plC, VkDescriptorSet set0, VkDescriptorSet set1, VkDescriptorSet set2)
    {
        if (!ok || pick >= kStage || !set2) return false;
        SeeSlots(d, sc);
        const uint32_t keyF = c.flags & (65536u | 16384u | 32768u | 4096u | 8192u);
        if (pickId == keyId && c.warp == keyWarp && c.warpSrc == keySrc && keyF == keyFlags && prev == keyPrev && (int)pick == keyPick)
            return true;                                  // drawn already (this cycle's other refresh)
        if (!inGeneral)
        {
            VkImageMemoryBarrier b[kImgN];
            for (int i = 0; i < kImgN; i++)
                b[i] = ImgBarrier(img[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, kImgN, b);
            inGeneral = true;
        }
        // an earlier refresh's BFI pass read F, its passes wrote the rest
        Barrier(d, cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);   // (the compute queue only)
        // the frame as shown
        C cw = c;
        cw.flags &= ~524288u;
        d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, psoCW);
        VkDescriptorSet sets[4] = { set0, set1, set2, dsCW };
        d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, plCW, 0, 4, sets, 0, nullptr);
        d->CmdPushConstants(cb, plCW, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cw), &cw);
        d->CmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
        Barrier(d, cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        (void)plC;
        // the settings
        float *cp = (float *)(uboPtr + (size_t)ring * uboStride);
        int bm = 1;
        crtvk::ParamsFor(cp, (float)w, (float)h, &bm);
        const uint32_t off = ring * uboStride;
        // the pattern, then the passes after it that are on
        Pass(d, cb, bm ? kPat1 : kPat0, dsPat[pick], off);
        int at = 0;                                       // 0: the picture is in A, 1: in B
        if (crtvk::SharpenOn(cp)) { Pass(d, cb, kSharpen, dsSharpen, off); at = 1; }
        if (crtvk::SoftenOn(cp)) { Pass(d, cb, kSoften, dsSoften[at], off); at = 1 - at; }
        if (crtvk::GrainOn(cp)) Pass(d, cb, kGrain, dsGrain[at], off);
        Pass(d, cb, kTail, dsTail[at], off);              // always: it writes F
        Barrier(d, cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        keyId = pickId; keyWarp = c.warp; keySrc = c.warpSrc; keyFlags = keyF; keyPrev = prev; keyPick = (int)pick;
        return true;
    }

    void Release(DeviceData *d)
    {
        VkDevice dev = d->device;
        for (int i = 0; i < kPassN; i++)
        {
            if (pso[i]) d->DestroyPipeline(dev, pso[i], nullptr);
            if (cs[i]) d->DestroyShaderModule(dev, cs[i], nullptr);
            pso[i] = VK_NULL_HANDLE; cs[i] = VK_NULL_HANDLE;
        }
        if (psoCW) d->DestroyPipeline(dev, psoCW, nullptr);
        if (csCW) d->DestroyShaderModule(dev, csCW, nullptr);
        if (plCW) d->DestroyPipelineLayout(dev, plCW, nullptr);
        if (dslCW) d->DestroyDescriptorSetLayout(dev, dslCW, nullptr);
        if (plP) d->DestroyPipelineLayout(dev, plP, nullptr);
        if (dslP) d->DestroyDescriptorSetLayout(dev, dslP, nullptr);
        if (dp) d->DestroyDescriptorPool(dev, dp, nullptr);
        psoCW = VK_NULL_HANDLE; csCW = VK_NULL_HANDLE; plCW = VK_NULL_HANDLE; dslCW = VK_NULL_HANDLE; plP = VK_NULL_HANDLE;
        dslP = VK_NULL_HANDLE; dp = VK_NULL_HANDLE;
        if (uboPtr) d->UnmapMemory(dev, uboMem);
        if (ubo) d->DestroyBuffer(dev, ubo, nullptr);
        if (uboMem) d->FreeMemory(dev, uboMem, nullptr);
        ubo = VK_NULL_HANDLE; uboMem = VK_NULL_HANDLE; uboPtr = nullptr;
        for (int i = 0; i < kImgN; i++)
        {
            if (view[i]) d->DestroyImageView(dev, view[i], nullptr);
            if (img[i]) d->DestroyImage(dev, img[i], nullptr);
            if (mem[i]) d->FreeMemory(dev, mem[i], nullptr);
            view[i] = VK_NULL_HANDLE; img[i] = VK_NULL_HANDLE; mem[i] = VK_NULL_HANDLE;
        }
        if (lin) d->DestroySampler(dev, lin, nullptr);
        lin = VK_NULL_HANDLE;
        imgOk = ok = inGeneral = false;
    }
};
