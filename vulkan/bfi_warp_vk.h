// ============================================================================
// BFI Presenter -- frame warp, Vulkan layer side: the motion between the last two frames, measured on the GPU
//
// Copyright (C) 2026 artzox -- https://github.com/artzox/BFI-Presenter
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The same passes as the DX add-on (dx/bfi_warp.h explains them), as compute shaders (shaders/warp.comp):
//   1. Each new frame, once: its brightness at 1/4 size and four more levels each half the size (a pyramid); the
//      pyramid of the frame shown before is kept.
//   2. Only when a frame is about to be shown moved: the motion from the frame before, coarse to fine down to 1/8,
//      then averaged over the whole picture at 1/32 (what the BFI shader reads, set 2 binding 0).
// The BFI shader (shaders/bfi_shade.glsl) takes each pixel from further back along that motion, except what stands
// still on the screen (compared with the frame shown before, set 2 binding 1).
//
// Included by bfi_presenter_vk.cpp after its helpers (CreateImageMem, MakeShader, MakeComputePipeline, ImgBarrier).
// Every image is rgba16f in GENERAL layout (storage and sampled), used only by the queue the presenter records on.
// ============================================================================
#pragma once

struct WarpPush { float texelX, texelY, texel2X, texel2Y; uint32_t mode; };

struct WarpVk
{
    static const int kLevels = 5;                     // 1/4, 1/8, 1/16, 1/32, 1/64 of the game frame
    static const int kLast = 1;                       // motion refined down to 1/8
    static const int kOut = 3;                        // evened out at 1/32: what the BFI shader reads
    // images: luma[p][l] = 0..9, flow[l] = 10..14, out = 15
    static const int kImgs = 2 * kLevels + kLevels + 1;
    VkImage        img[kImgs] = {};
    VkDeviceMemory mem[kImgs] = {};
    VkImageView    view[kImgs] = {};
    uint32_t       w[kLevels] = {}, h[kLevels] = {};
    uint32_t       srcW = 0, srcH = 0;
    VkSampler      lin = VK_NULL_HANDLE;              // linear, clamp
    // passes
    VkDescriptorSetLayout dslP = VK_NULL_HANDLE;
    VkPipelineLayout      plP = VK_NULL_HANDLE;
    VkShaderModule        cs[4] = {};
    VkPipeline            pso[4] = {};                // luma, down, flow, smooth
    VkDescriptorPool      dpP = VK_NULL_HANDLE;
    VkDescriptorSet       setLuma[2][kStage] = {};    // ta = the game frame in slot s, dst = luma[p][0]
    VkDescriptorSet       setDown[2][kLevels] = {};   // l >= 1: ta = luma[p][l-1], dst = luma[p][l]
    VkDescriptorSet       setFlow[2][kLevels] = {};   // newer pyramid p: ta = luma[p][l], tb = luma[1-p][l], tc = flow[l+1], dst = flow[l]
    VkDescriptorSet       setSmooth = VK_NULL_HANDLE; // ta = flow[kLast], dst = out
    // the BFI shader's set 2: flow (out), the frame shown before (prev slot), this frame with the linear sampler (pick slot)
    VkDescriptorSetLayout dslW = VK_NULL_HANDLE;
    VkDescriptorPool      dpW = VK_NULL_HANDLE;
    VkDescriptorSet       dsW[kStage][kStage] = {};   // [pick][prev]
    // state
    bool     ready = false;                           // set 2 usable (always needed by the BFI pipelines)
    bool     ok = false;                              // the passes too
    bool     inGeneral = false;                       // images moved to GENERAL
    int      cur = 0;                                 // which pyramid holds the newest frame
    uint64_t id[2] = {};
    double   t[2] = {};
    uint64_t flowNew = 0, flowOld = 0;
    // Frame interpolation (1.0.54 test): the newer frame's motion pushed forward to the moment shown (shaders/ipsplat.comp),
    // at the game motion texture's size: two r32ui landing textures and the resolved motion (set 2 binding 6).
    VkImage        ipImg[3] = {};
    VkDeviceMemory ipMem[3] = {};
    VkImageView    ipView[3] = {};
    uint32_t       ipW = 0, ipH = 0;
    VkDescriptorSetLayout dslI = VK_NULL_HANDLE;
    VkPipelineLayout      plI = VK_NULL_HANDLE;
    static const int      kIpPasses = 4;
    VkShaderModule        csI[kIpPasses] = {};
    VkPipeline            psoI[kIpPasses] = {};       // clear, land, write, fill (s2)
    VkDescriptorPool      dpI = VK_NULL_HANDLE;
    VkDescriptorSet       setI[kStage] = {};          // gFlow = the game motion texture of slot s
    bool     ipOk = false;
    uint64_t ipLastId = 0; float ipLastBack = -1.0f;

    static int Luma(int p, int l) { return p * kLevels + l; }
    static int Flow(int l) { return 2 * kLevels + l; }
    static int Out() { return 3 * kLevels; }

    // Set 2's layout: made before the BFI pipelines' layouts, which include it.
    bool CreateLayout(DeviceData *d)
    {
        VkDescriptorSetLayoutBinding b[8] = {};
        for (int i = 0; i < 8; i++)
        {
            b[i].binding = (uint32_t)i;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        dli.bindingCount = 8;                          // binding 7: the CRT hand-off's picture (bfi_crt_vk.h, 1.0.54); binding 6: frame interpolation's motion at the moment shown; binding 3: the game's own motion for the frame (gvk); 4, 5: the frames
                                                       // from before ReShade's effects (FrameWarpStillBeforeEffects)
        dli.pBindings = b;
        return d->CreateDescriptorSetLayout(d->device, &dli, nullptr, &dslW) == VK_SUCCESS;
    }

    bool MakeImgFmt(DeviceData *d, VkFormat fmt, VkImageUsageFlags use, uint32_t iw, uint32_t ih, VkImage *im, VkDeviceMemory *mm, VkImageView *vw)
    {
        VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt; ici.extent = { iw, ih, 1 };
        ici.mipLevels = 1; ici.arrayLayers = 1; ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL; ici.usage = use;
        if (!CreateImageMem(d, ici, im, mm)) return false;
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = *im; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        return d->CreateImageView(d->device, &vci, nullptr, vw) == VK_SUCCESS;
    }
    // Frame interpolation's passes (optional: without them the search in the BFI shader is used).
    bool InitSplat(DeviceData *d, const VkImageView *gameFlowView)
    {
        if (!gameFlowView || !d->CreateComputePipelines || !d->CmdDispatch) return false;
        for (uint32_t i = 0; i < kStage; i++) if (!gameFlowView[i]) return false;
        VkDevice dev = d->device;
        bool p = MakeImgFmt(d, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT, ipW, ipH, &ipImg[0], &ipMem[0], &ipView[0]) &&
                 MakeImgFmt(d, VK_FORMAT_R32_UINT, VK_IMAGE_USAGE_STORAGE_BIT, ipW, ipH, &ipImg[1], &ipMem[1], &ipView[1]);
        VkDescriptorSetLayoutBinding b[4] = {};
        for (int i = 0; i < 4; i++)
        {
            b[i].binding = (uint32_t)i;
            b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        dli.bindingCount = 4; dli.pBindings = b;
        p = p && d->CreateDescriptorSetLayout(dev, &dli, nullptr, &dslI) == VK_SUCCESS;
        VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 16 };
        VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount = 1; pli.pSetLayouts = &dslI; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
        p = p && d->CreatePipelineLayout(dev, &pli, nullptr, &plI) == VK_SUCCESS;
        const uint32_t *code[kIpPasses] = { kIpSplat0Spv, kIpSplat1Spv, kIpSplat2Spv, kIpSplat3Spv };
        const size_t bytes[kIpPasses] = { sizeof(kIpSplat0Spv), sizeof(kIpSplat1Spv), sizeof(kIpSplat2Spv), sizeof(kIpSplat3Spv) };
        for (int i = 0; p && i < kIpPasses; i++)
        {
            csI[i] = MakeShader(d, code[i], bytes[i]);
            psoI[i] = csI[i] ? MakeComputePipeline(d, plI, csI[i]) : VK_NULL_HANDLE;
            p = psoI[i] != VK_NULL_HANDLE;
        }
        if (p)
        {
            VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kStage }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 * kStage } };
            VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            dpi.maxSets = kStage; dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
            p = d->CreateDescriptorPool(dev, &dpi, nullptr, &dpI) == VK_SUCCESS;
            VkDescriptorSetLayout l[kStage];
            for (uint32_t i = 0; i < kStage; i++) l[i] = dslI;
            VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dai.descriptorPool = dpI; dai.descriptorSetCount = kStage; dai.pSetLayouts = l;
            p = p && d->AllocateDescriptorSets(dev, &dai, setI) == VK_SUCCESS;
            for (uint32_t sl = 0; p && sl < kStage; sl++)
            {
                VkDescriptorImageInfo ii[4] = { { lin, gameFlowView[sl], VK_IMAGE_LAYOUT_GENERAL }, { VK_NULL_HANDLE, ipView[0], VK_IMAGE_LAYOUT_GENERAL },
                                                { VK_NULL_HANDLE, ipView[1], VK_IMAGE_LAYOUT_GENERAL }, { VK_NULL_HANDLE, ipView[2], VK_IMAGE_LAYOUT_GENERAL } };
                VkWriteDescriptorSet wr[4] = {};
                for (int k = 0; k < 4; k++)
                {
                    wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[k].dstSet = setI[sl]; wr[k].dstBinding = (uint32_t)k;
                    wr[k].descriptorCount = 1; wr[k].pImageInfo = &ii[k];
                    wr[k].descriptorType = k == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                }
                d->UpdateDescriptorSets(dev, 4, wr, 0, nullptr);
            }
        }
        return p;
    }

    bool MakeImage(DeviceData *d, int i, uint32_t iw, uint32_t ih)
    {
        VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        ici.extent = { iw, ih, 1 };
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!CreateImageMem(d, ici, &img[i], &mem[i])) return false;
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = img[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        return d->CreateImageView(d->device, &vci, nullptr, &view[i]) == VK_SUCCESS;
    }

    void WriteSet(DeviceData *d, VkDescriptorSet set, VkImageView a, VkImageLayout la, VkImageView b, VkImageLayout lb,
                  VkImageView c, VkImageLayout lc, VkImageView dst)
    {
        VkDescriptorImageInfo ii[4] = { { lin, a, la }, { lin, b, lb }, { lin, c, lc }, { VK_NULL_HANDLE, dst, VK_IMAGE_LAYOUT_GENERAL } };
        VkWriteDescriptorSet wr[4] = {};
        for (int k = 0; k < 4; k++)
        {
            wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[k].dstSet = set;
            wr[k].dstBinding = (uint32_t)k;
            wr[k].descriptorCount = 1;
            wr[k].descriptorType = k < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            wr[k].pImageInfo = &ii[k];
        }
        d->UpdateDescriptorSets(d->device, dst ? 4 : 3, wr, 0, nullptr);
    }

    // stageView: the staging ring's views (SHADER_READ_ONLY_OPTIMAL). Set 2 is required (false = the BFI pipelines
    // can't be used); the passes are optional (ok).
    bool Init(DeviceData *d, const VkImageView *stageView, const VkImageView *gameFlowView, const VkImageView *preView, uint32_t frameW, uint32_t frameH,
              VkImageView crtFinal = VK_NULL_HANDLE)
    {
        VkDevice dev = d->device;
        srcW = frameW; srcH = frameH;
        w[0] = (frameW + 3) / 4; h[0] = (frameH + 3) / 4;
        for (int l = 1; l < kLevels; l++) { w[l] = (w[l - 1] + 1) / 2; h[l] = (h[l - 1] + 1) / 2; }
        VkSamplerCreateInfo smi = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        smi.magFilter = smi.minFilter = VK_FILTER_LINEAR;
        smi.addressModeU = smi.addressModeV = smi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        smi.maxLod = 0.0f;
        bool r = d->CreateSampler(dev, &smi, nullptr, &lin) == VK_SUCCESS;
        r = r && MakeImage(d, Out(), w[kOut], h[kOut]);
        ipW = (frameW + 1) / 2; ipH = (frameH + 1) / 2;   // as the game motion texture (Swapchain gW, gH)
        const bool ipImgOk = r && MakeImgFmt(d, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                             ipW, ipH, &ipImg[2], &ipMem[2], &ipView[2]);
        if (r)
        {
            VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 * kStage * kStage };
            VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            dpi.maxSets = kStage * kStage;
            dpi.poolSizeCount = 1;
            dpi.pPoolSizes = &ps;
            r = d->CreateDescriptorPool(dev, &dpi, nullptr, &dpW) == VK_SUCCESS;
            VkDescriptorSetLayout l[kStage * kStage];
            for (uint32_t i = 0; i < kStage * kStage; i++) l[i] = dslW;
            VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dai.descriptorPool = dpW;
            dai.descriptorSetCount = kStage * kStage;
            dai.pSetLayouts = l;
            r = r && d->AllocateDescriptorSets(dev, &dai, &dsW[0][0]) == VK_SUCCESS;
            for (uint32_t a = 0; r && a < kStage; a++)
                for (uint32_t b = 0; b < kStage; b++)
                {
                    WriteSet(d, dsW[a][b], view[Out()], VK_IMAGE_LAYOUT_GENERAL, stageView[b], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             stageView[a], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_NULL_HANDLE);
                    // the game's motion texture of the frame shown (the measured motion as a stand-in where there is none)
                    VkDescriptorImageInfo gi = { lin, gameFlowView && gameFlowView[a] ? gameFlowView[a] : view[Out()], VK_IMAGE_LAYOUT_GENERAL };
                    VkWriteDescriptorSet gw = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    gw.dstSet = dsW[a][b]; gw.dstBinding = 3; gw.descriptorCount = 1;
                    gw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; gw.pImageInfo = &gi;
                    d->UpdateDescriptorSets(d->device, 1, &gw, 0, nullptr);
                    // the frames from before ReShade's effects (the stage frames where there are none: read only with flags & 16384)
                    VkDescriptorImageInfo pi[2] = { { lin, preView && preView[a] ? preView[a] : stageView[a], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
                                                    { lin, preView && preView[b] ? preView[b] : stageView[b], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
                    VkWriteDescriptorSet pw[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
                    for (int k = 0; k < 2; k++)
                    {
                        pw[k].dstSet = dsW[a][b]; pw[k].dstBinding = 4 + (uint32_t)k; pw[k].descriptorCount = 1;
                        pw[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; pw[k].pImageInfo = &pi[k];
                    }
                    d->UpdateDescriptorSets(d->device, 2, pw, 0, nullptr);
                    VkDescriptorImageInfo si = { lin, ipImgOk ? ipView[2] : view[Out()], VK_IMAGE_LAYOUT_GENERAL };
                    VkWriteDescriptorSet sw = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    sw.dstSet = dsW[a][b]; sw.dstBinding = 6; sw.descriptorCount = 1;
                    sw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; sw.pImageInfo = &si;
                    d->UpdateDescriptorSets(d->device, 1, &sw, 0, nullptr);
                    // the CRT hand-off's picture (read only with flags & 524288; the evened-out motion as a stand-in without it)
                    VkDescriptorImageInfo ci = { lin, crtFinal ? crtFinal : view[Out()], VK_IMAGE_LAYOUT_GENERAL };
                    VkWriteDescriptorSet cw = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
                    cw.dstSet = dsW[a][b]; cw.dstBinding = 7; cw.descriptorCount = 1;
                    cw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; cw.pImageInfo = &ci;
                    d->UpdateDescriptorSets(d->device, 1, &cw, 0, nullptr);
                }
        }
        ready = r;
        if (!r) return false;

        // the passes (optional)
        bool p = d->CreateComputePipelines && d->CmdDispatch && frameW >= 64 && frameH >= 64;
        for (int pp = 0; p && pp < 2; pp++)
            for (int l = 0; p && l < kLevels; l++) p = MakeImage(d, Luma(pp, l), w[l], h[l]);
        for (int l = 0; p && l < kLevels; l++) p = MakeImage(d, Flow(l), w[l], h[l]);
        if (p)
        {
            VkDescriptorSetLayoutBinding b[4] = {};
            for (int i = 0; i < 4; i++)
            {
                b[i].binding = (uint32_t)i;
                b[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                b[i].descriptorCount = 1;
                b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            }
            VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
            dli.bindingCount = 4;
            dli.pBindings = b;
            p = d->CreateDescriptorSetLayout(dev, &dli, nullptr, &dslP) == VK_SUCCESS;
            VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(WarpPush) };
            VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
            pli.setLayoutCount = 1;
            pli.pSetLayouts = &dslP;
            pli.pushConstantRangeCount = 1;
            pli.pPushConstantRanges = &pr;
            p = p && d->CreatePipelineLayout(dev, &pli, nullptr, &plP) == VK_SUCCESS;
        }
        if (p)
        {
            const uint32_t *code[4] = { kWarpLumaSpv, kWarpDownSpv, kWarpFlowSpv, kWarpSmoothSpv };
            const size_t bytes[4] = { sizeof(kWarpLumaSpv), sizeof(kWarpDownSpv), sizeof(kWarpFlowSpv), sizeof(kWarpSmoothSpv) };
            for (int i = 0; p && i < 4; i++)
            {
                cs[i] = MakeShader(d, code[i], bytes[i]);
                pso[i] = cs[i] ? MakeComputePipeline(d, plP, cs[i]) : VK_NULL_HANDLE;
                p = pso[i] != VK_NULL_HANDLE;
            }
        }
        if (p)
        {
            const uint32_t nSets = 2 * kStage + 2 * kLevels + 2 * kLevels + 1;
            VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * nSets }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, nSets } };
            VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            dpi.maxSets = nSets;
            dpi.poolSizeCount = 2;
            dpi.pPoolSizes = ps;
            p = d->CreateDescriptorPool(dev, &dpi, nullptr, &dpP) == VK_SUCCESS;
            std::vector<VkDescriptorSetLayout> l(nSets, dslP);
            std::vector<VkDescriptorSet> sets(nSets, VK_NULL_HANDLE);
            VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            dai.descriptorPool = dpP;
            dai.descriptorSetCount = nSets;
            dai.pSetLayouts = l.data();
            p = p && d->AllocateDescriptorSets(dev, &dai, sets.data()) == VK_SUCCESS;
            if (p)
            {
                uint32_t k = 0;
                const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL, S = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                for (int pp = 0; pp < 2; pp++)
                    for (uint32_t s = 0; s < kStage; s++)
                    {
                        setLuma[pp][s] = sets[k++];
                        WriteSet(d, setLuma[pp][s], stageView[s], S, stageView[s], S, stageView[s], S, view[Luma(pp, 0)]);
                    }
                for (int pp = 0; pp < 2; pp++)
                    for (int lv = 0; lv < kLevels; lv++)
                    {
                        setDown[pp][lv] = sets[k++];
                        const int src = Luma(pp, lv > 0 ? lv - 1 : 0);
                        WriteSet(d, setDown[pp][lv], view[src], G, view[src], G, view[src], G, view[Luma(pp, lv)]);
                    }
                for (int pp = 0; pp < 2; pp++)
                    for (int lv = 0; lv < kLevels; lv++)
                    {
                        setFlow[pp][lv] = sets[k++];
                        const VkImageView coarser = lv + 1 < kLevels ? view[Flow(lv + 1)] : view[Luma(pp, lv)];
                        WriteSet(d, setFlow[pp][lv], view[Luma(pp, lv)], G, view[Luma(1 - pp, lv)], G, coarser, G, view[Flow(lv)]);
                    }
                setSmooth = sets[k++];
                WriteSet(d, setSmooth, view[Flow(kLast)], G, view[Flow(kLast)], G, view[Flow(kLast)], G, view[Out()]);
            }
        }
        ok = p;
        ipOk = ipImgOk && InitSplat(d, gameFlowView);
        ipLastId = 0; ipLastBack = -1.0f;
        cur = 0; id[0] = id[1] = 0; t[0] = t[1] = 0.0; flowNew = flowOld = 0; inGeneral = false;
        return true;
    }

    void Release(DeviceData *d)
    {
        VkDevice dev = d->device;
        for (int i = 0; i < 4; i++)
        {
            if (pso[i]) d->DestroyPipeline(dev, pso[i], nullptr);
            if (cs[i]) d->DestroyShaderModule(dev, cs[i], nullptr);
            pso[i] = VK_NULL_HANDLE; cs[i] = VK_NULL_HANDLE;
        }
        if (dpP) d->DestroyDescriptorPool(dev, dpP, nullptr);
        if (plP) d->DestroyPipelineLayout(dev, plP, nullptr);
        if (dslP) d->DestroyDescriptorSetLayout(dev, dslP, nullptr);
        if (dpW) d->DestroyDescriptorPool(dev, dpW, nullptr);
        if (dslW) d->DestroyDescriptorSetLayout(dev, dslW, nullptr);
        dpP = VK_NULL_HANDLE; plP = VK_NULL_HANDLE; dslP = VK_NULL_HANDLE; dpW = VK_NULL_HANDLE; dslW = VK_NULL_HANDLE;
        for (int i = 0; i < kImgs; i++)
        {
            if (view[i]) d->DestroyImageView(dev, view[i], nullptr);
            if (img[i]) d->DestroyImage(dev, img[i], nullptr);
            if (mem[i]) d->FreeMemory(dev, mem[i], nullptr);
            view[i] = VK_NULL_HANDLE; img[i] = VK_NULL_HANDLE; mem[i] = VK_NULL_HANDLE;
        }
        for (int i = 0; i < kIpPasses; i++)
        {
            if (psoI[i]) d->DestroyPipeline(dev, psoI[i], nullptr);
            if (csI[i]) d->DestroyShaderModule(dev, csI[i], nullptr);
            psoI[i] = VK_NULL_HANDLE; csI[i] = VK_NULL_HANDLE;
        }
        for (int i = 0; i < 3; i++)
        {
            if (ipView[i]) d->DestroyImageView(dev, ipView[i], nullptr);
            if (ipImg[i]) d->DestroyImage(dev, ipImg[i], nullptr);
            if (ipMem[i]) d->FreeMemory(dev, ipMem[i], nullptr);
            ipView[i] = VK_NULL_HANDLE; ipImg[i] = VK_NULL_HANDLE; ipMem[i] = VK_NULL_HANDLE;
        }
        if (dpI) d->DestroyDescriptorPool(dev, dpI, nullptr);
        if (plI) d->DestroyPipelineLayout(dev, plI, nullptr);
        if (dslI) d->DestroyDescriptorSetLayout(dev, dslI, nullptr);
        dpI = VK_NULL_HANDLE; plI = VK_NULL_HANDLE; dslI = VK_NULL_HANDLE;
        ipOk = false;
        if (lin) d->DestroySampler(dev, lin, nullptr);
        lin = VK_NULL_HANDLE;
        ready = ok = false;
    }

    void Forget() { id[0] = id[1] = 0; flowNew = flowOld = 0; ipLastId = 0; }

    // The frame the motion of frameId would be measured from (as in DX): 0 = none usable.
    uint64_t PairFor(uint64_t frameId, double frameT) const
    {
        const uint64_t oldId = frameId == id[cur] ? id[1 - cur] : id[cur];
        const double oldT = frameId == id[cur] ? t[1 - cur] : t[cur];
        if (!ok || !oldId || frameId <= oldId || frameId - oldId > 4) return 0;
        if (frameT - oldT > 0.25 || frameT < oldT) return 0;
        return oldId;
    }

    // First use in a command buffer: every image to GENERAL (contents undefined; the pyramids are rebuilt as frames come).
    void ToGeneral(DeviceData *d, VkCommandBuffer cb)
    {
        if (inGeneral) return;
        VkImageMemoryBarrier b[kImgs + 3];
        uint32_t n = 0;
        for (int i = 0; i < kImgs; i++)
            if (img[i]) b[n++] = ImgBarrier(img[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                                            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        for (int i = 0; i < 3; i++)   // frame interpolation's (the resolved one is in set 2 from the start)
            if (ipImg[i]) b[n++] = ImgBarrier(ipImg[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                                              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              0, 0, nullptr, 0, nullptr, n, b);
        inGeneral = true;
    }
    void Between(DeviceData *d, VkCommandBuffer cb)
    {
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    }
    void Pass(DeviceData *d, VkCommandBuffer cb, int which, VkDescriptorSet set, const WarpPush &wp, uint32_t tw, uint32_t th)
    {
        d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pso[which]);
        d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, plP, 0, 1, &set, 0, nullptr);
        d->CmdPushConstants(cb, plP, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(wp), &wp);
        d->CmdDispatch(cb, (tw + 7) / 8, (th + 7) / 8, 1);
        Between(d, cb);
    }

    // A new frame is about to be shown for the first time: its brightness pyramid (the one before is kept).
    void RecordLuma(DeviceData *d, VkCommandBuffer cb, uint32_t slot, uint64_t frameId, double frameT, bool linear)
    {
        ToGeneral(d, cb);
        cur = 1 - cur;
        id[cur] = frameId; t[cur] = frameT;
        WarpPush wp = {};
        wp.texelX = 1.0f / (float)w[0]; wp.texelY = 1.0f / (float)h[0];
        wp.texel2X = 1.0f / (float)srcW; wp.texel2Y = 1.0f / (float)srcH;
        wp.mode = linear ? 1u : 0u;
        Pass(d, cb, 0, setLuma[cur][slot], wp, w[0], h[0]);
        for (int l = 1; l < kLevels; l++)
        {
            wp = {};
            wp.texelX = 1.0f / (float)w[l]; wp.texelY = 1.0f / (float)h[l];
            Pass(d, cb, 1, setDown[cur][l], wp, w[l], h[l]);
        }
    }

    // The motion from the frame before to the newest, coarse to fine, then evened out. Once per pair of frames.
    void RecordFlow(DeviceData *d, VkCommandBuffer cb)
    {
        ToGeneral(d, cb);
        const int top = kLevels - 1;
        for (int l = top; l >= kLast; l--)
        {
            WarpPush wp = {};
            wp.texelX = 1.0f / (float)w[l]; wp.texelY = 1.0f / (float)h[l];
            wp.mode = (l < top ? 1u : 0u) | (l == kLast ? 2u : 0u);
            if (l < top) { wp.texel2X = 1.0f / (float)w[l + 1]; wp.texel2Y = 1.0f / (float)h[l + 1]; }
            Pass(d, cb, 2, setFlow[cur][l], wp, w[l], h[l]);
        }
        WarpPush wp = {};
        wp.texelX = 1.0f / (float)w[kOut]; wp.texelY = 1.0f / (float)h[kOut];
        wp.texel2X = 1.0f / (float)w[kLast]; wp.texel2Y = 1.0f / (float)h[kLast];
        wp.mode = 1u;
        Pass(d, cb, 3, setSmooth, wp, w[kOut], h[kOut]);
        flowNew = id[cur]; flowOld = id[1 - cur];
    }

    // Frame interpolation: the motion of the frame in `slot` pushed forward to the moment shown (back = 1 - f). Once per
    // frame and moment (a strobe cycle's dark refresh reuses it).
    bool RecordSplat(DeviceData *d, VkCommandBuffer cb, uint32_t slot, uint64_t frameId, float back)
    {
        if (!ipOk || slot >= kStage) return false;
        if (frameId == ipLastId && back == ipLastBack) return true;
        ToGeneral(d, cb);
        // the BFI pass of an earlier refresh read the resolved motion; the passes below write it again
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              0, 1, &mb, 0, nullptr, 0, nullptr);
        struct { float back; uint32_t pad[3]; } pc = { back, { 0, 0, 0 } };
        for (int i = 0; i < kIpPasses; i++)
        {
            d->CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, psoI[i]);
            d->CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, plI, 0, 1, &setI[slot], 0, nullptr);
            d->CmdPushConstants(cb, plI, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            d->CmdDispatch(cb, (ipW + 7) / 8, (ipH + 7) / 8, 1);
            Between(d, cb);
        }
        ipLastId = frameId; ipLastBack = back;
        return true;
    }

    // After the passes, before the BFI pass reads the motion.
    void ToShade(DeviceData *d, VkCommandBuffer cb)
    {
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        d->CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              0, 1, &mb, 0, nullptr, 0, nullptr);
    }
};
