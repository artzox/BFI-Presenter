// Frame warp test for the Vulkan layer (Linux, any Vulkan driver; lavapipe works): the layer's own warp passes
// (vulkan/bfi_warp_vk.h, shaders/warp.comp) and its BFI compute shader (bfi.comp, rgba8) on the synthetic frames of
// make_frames.py -- the same sequence and outputs as warp_test.cpp (DX), so check.py reads both.
//   g++ -O2 -std=c++17 -I ../../third_party/vulkan-headers vk_warp_test.cpp -o vk_warp_test -ldl && ./vk_warp_test
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

static const uint32_t kStage = 7;
#include "../../vulkan/shaders_spv.h"

#define DEV_FUNCS(X) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) X(AllocateMemory) \
    X(FreeMemory) X(CreateImageView) X(DestroyImageView) X(CreateSampler) X(DestroySampler) X(CreateDescriptorSetLayout) \
    X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) \
    X(UpdateDescriptorSets) X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateShaderModule) X(DestroyShaderModule) \
    X(CreateComputePipelines) X(DestroyPipeline) X(CmdDispatch) X(CmdBindPipeline) X(CmdBindDescriptorSets) \
    X(CmdPushConstants) X(CmdPipelineBarrier) X(CreateBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) \
    X(MapMemory) X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) X(CreateCommandPool) X(AllocateCommandBuffers) \
    X(BeginCommandBuffer) X(EndCommandBuffer) X(QueueSubmit) X(QueueWaitIdle) X(GetDeviceQueue) X(ResetCommandBuffer)
struct DeviceData
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem = {};
#define X(n) PFN_vk##n n = nullptr;
    DEV_FUNCS(X)
#undef X
};
static uint32_t FindMemType(DeviceData *d, uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < d->mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (d->mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
static bool CreateImageMem(DeviceData *d, const VkImageCreateInfo &ici, VkImage *img, VkDeviceMemory *mem)
{
    if (d->CreateImage(d->device, &ici, nullptr, img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    d->GetImageMemoryRequirements(d->device, *img, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = FindMemType(d, mr.memoryTypeBits, 0);
    if (d->AllocateMemory(d->device, &ai, nullptr, mem) != VK_SUCCESS) return false;
    return d->BindImageMemory(d->device, *img, *mem, 0) == VK_SUCCESS;
}
static VkImageMemoryBarrier ImgBarrier(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = src; b.dstAccessMask = dst; b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    return b;
}
static VkShaderModule MakeShader(DeviceData *d, const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = bytes; ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    d->CreateShaderModule(d->device, &ci, nullptr, &m);
    return m;
}
static VkPipeline MakeComputePipeline(DeviceData *d, VkPipelineLayout pl, VkShaderModule cs)
{
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = cs; ci.stage.pName = "main";
    ci.layout = pl;
    VkPipeline p = VK_NULL_HANDLE;
    if (d->CreateComputePipelines(d->device, VK_NULL_HANDLE, 1, &ci, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    return p;
}
#include "../../vulkan/bfi_warp_vk.h"
#include "gen_constants_vk.h"   // struct Constants, extracted from vulkan/bfi_presenter_vk.cpp by run_vk.sh

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAILED %s: %d (line %d)\n", #x, (int)r_, __LINE__); return 1; } } while (0)

int main()
{
    const uint32_t W = 1920, H = 1080;
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib) { printf("no libvulkan\n"); return 1; }
    auto gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    auto createInstance = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    VkInstance inst;
    CHECK(createInstance(&ici, nullptr, &inst));
    auto enumPhys = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    auto getMem = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    auto getQf = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    auto getProps = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    auto createDevice = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
    auto gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    uint32_t n = 1;
    VkPhysicalDevice phys;
    enumPhys(inst, &n, &phys);
    VkPhysicalDeviceProperties pp; getProps(phys, &pp);
    printf("device: %s\n", pp.deviceName);
    uint32_t nq = 0; getQf(phys, &nq, nullptr);
    std::vector<VkQueueFamilyProperties> qf(nq); getQf(phys, &nq, qf.data());
    uint32_t fam = 0;
    for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { fam = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = fam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    DeviceData D; DeviceData *d = &D;
    CHECK(createDevice(phys, &dci, nullptr, &D.device));
    getMem(phys, &D.mem);
#define X(n) D.n = (PFN_vk##n)gdpa(D.device, "vk" #n); if (!D.n) { printf("missing vk%s\n", #n); return 1; }
    DEV_FUNCS(X)
#undef X
    VkDevice dev = D.device;
    VkQueue q; D.GetDeviceQueue(dev, fam, 0, &q);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pci.queueFamilyIndex = fam;
    VkCommandPool pool; CHECK(D.CreateCommandPool(dev, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cai.commandPool = pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    VkCommandBuffer cb; CHECK(D.AllocateCommandBuffers(dev, &cai, &cb));
    auto Begin = [&]() { D.ResetCommandBuffer(cb, 0); VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; D.BeginCommandBuffer(cb, &bi); };
    auto Run = [&]() { D.EndCommandBuffer(cb); VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; si.commandBufferCount = 1; si.pCommandBuffers = &cb; D.QueueSubmit(q, 1, &si, VK_NULL_HANDLE); D.QueueWaitIdle(q); };

    // host buffer for uploads and downloads
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = (VkDeviceSize)W * H * 8; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer hb; CHECK(D.CreateBuffer(dev, &bci, nullptr, &hb));
    VkMemoryRequirements mr; D.GetBufferMemoryRequirements(dev, hb, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size; mai.memoryTypeIndex = FindMemType(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory hm; CHECK(D.AllocateMemory(dev, &mai, nullptr, &hm)); D.BindBufferMemory(dev, hb, hm, 0);
    uint8_t *hp; D.MapMemory(dev, hm, 0, VK_WHOLE_SIZE, 0, (void **)&hp);

    // staging ring: frames f0 (slot 0) and f1 (slot 1); the other slots hold f1 too (unused)
    VkImage stage[kStage]; VkDeviceMemory sm[kStage]; VkImageView sv[kStage];
    for (uint32_t i = 0; i < kStage; i++)
    {
        VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM; ii.extent = { W, H, 1 };
        ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!CreateImageMem(d, ii, &stage[i], &sm[i])) { printf("stage image failed\n"); return 1; }
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = stage[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        CHECK(D.CreateImageView(dev, &vi, nullptr, &sv[i]));
        FILE *f = fopen(i == 0 ? "f0.rgba" : "f1.rgba", "rb");
        if (!f || fread(hp, 1, (size_t)W * H * 4, f) != (size_t)W * H * 4) { printf("frames missing: run make_frames.py\n"); return 1; }
        fclose(f);
        Begin();
        VkImageMemoryBarrier b = ImgBarrier(stage[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        D.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy bic = {}; bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; bic.imageExtent = { W, H, 1 };
        D.CmdCopyBufferToImage(cb, hb, stage[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
        b = ImgBarrier(stage[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        D.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        Run();
    }

    // the BFI compute pass (bfi.comp, rgba8): set 0 = src (nearest) + dst + APL value, set 1 = srcNew, set 2 = frame warp's
    WarpVk warp;
    if (!warp.CreateLayout(d)) { printf("warp layout failed\n"); return 1; }
    VkSamplerCreateInfo smi = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    smi.magFilter = smi.minFilter = VK_FILTER_NEAREST;
    smi.addressModeU = smi.addressModeV = smi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler nearest; CHECK(D.CreateSampler(dev, &smi, nullptr, &nearest));
    VkImage out, apl; VkDeviceMemory om, am; VkImageView ov, av;
    {
        VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM; ii.extent = { W, H, 1 };
        ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        CreateImageMem(d, ii, &out, &om);
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = out; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ii.format; vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        CHECK(D.CreateImageView(dev, &vi, nullptr, &ov));
        ii.format = VK_FORMAT_R32_SFLOAT; ii.extent = { 1, 1, 1 }; ii.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        CreateImageMem(d, ii, &apl, &am);
        vi.image = apl; vi.format = ii.format;
        CHECK(D.CreateImageView(dev, &vi, nullptr, &av));
    }
    VkDescriptorSetLayout dslC, dslN;
    {
        VkDescriptorSetLayoutBinding b[3] = {};
        b[0] = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        b[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        b[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
        VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        dli.bindingCount = 3; dli.pBindings = b;
        CHECK(D.CreateDescriptorSetLayout(dev, &dli, nullptr, &dslC));
        dli.bindingCount = 1;
        CHECK(D.CreateDescriptorSetLayout(dev, &dli, nullptr, &dslN));
    }
    VkDescriptorSetLayout sl[3] = { dslC, dslN, warp.dslW };
    VkPushConstantRange pr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Constants) };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.setLayoutCount = 3; pli.pSetLayouts = sl; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pr;
    VkPipelineLayout plC; CHECK(D.CreatePipelineLayout(dev, &pli, nullptr, &plC));
    VkPipeline pso = MakeComputePipeline(d, plC, MakeShader(d, kBfiComp8Spv, sizeof(kBfiComp8Spv)));
    if (!pso) { printf("BFI pipeline failed\n"); return 1; }
    VkDescriptorPool dp;
    {
        VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * kStage }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 * kStage } };
        VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpi.maxSets = 2 * kStage; dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
        CHECK(D.CreateDescriptorPool(dev, &dpi, nullptr, &dp));
    }
    VkDescriptorSet dsC[2], dsN[2];
    for (int i = 0; i < 2; i++)
    {
        VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dai.descriptorPool = dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &dslC;
        CHECK(D.AllocateDescriptorSets(dev, &dai, &dsC[i]));
        dai.pSetLayouts = &dslN;
        CHECK(D.AllocateDescriptorSets(dev, &dai, &dsN[i]));
        VkDescriptorImageInfo ii[4] = { { nearest, sv[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, { VK_NULL_HANDLE, ov, VK_IMAGE_LAYOUT_GENERAL },
                                        { VK_NULL_HANDLE, av, VK_IMAGE_LAYOUT_GENERAL }, { nearest, sv[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL } };
        VkWriteDescriptorSet w[4] = {};
        for (int k = 0; k < 4; k++)
        {
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[k].dstSet = k < 3 ? dsC[i] : dsN[i];
            w[k].dstBinding = k < 3 ? (uint32_t)k : 0;
            w[k].descriptorCount = 1;
            w[k].descriptorType = (k == 1 || k == 2) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[k].pImageInfo = &ii[k];
        }
        D.UpdateDescriptorSets(dev, 4, w, 0, nullptr);
    }
    if (!warp.Init(d, sv, W, H) || !warp.ok) { printf("warp.Init failed (ready %d ok %d)\n", warp.ready, warp.ok); return 1; }
    printf("warp levels: "); for (int l = 0; l < WarpVk::kLevels; l++) printf("%ux%u ", warp.w[l], warp.h[l]); printf("\n");
    Begin();
    {
        VkImageMemoryBarrier b[2] = { ImgBarrier(out, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT),
                                      ImgBarrier(apl, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_READ_BIT) };
        D.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
    }
    Run();

    auto Download = [&](VkImage img, VkImageLayout lay, uint32_t w, uint32_t h, size_t bpp, const char *name)
    {
        Begin();
        VkImageMemoryBarrier b = ImgBarrier(img, lay, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        D.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy bic = {}; bic.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; bic.imageExtent = { w, h, 1 };
        D.CmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hb, 1, &bic);
        b = ImgBarrier(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, lay, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        D.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        Run();
        FILE *o = fopen(name, "wb"); fwrite(hp, 1, (size_t)w * h * bpp, o); fclose(o);
    };

    // the presenter's sequence: frame 1 shown, frame 2 shown, then frame 2 shown half a frame and a frame ahead
    const float shares[] = { 0.0f, 0.5f, 1.0f };
    for (int pass = 0; pass < 4; pass++)
    {
        const uint32_t pick = pass == 0 ? 0u : 1u;
        const uint64_t pickId = pick + 100;
        const double pickT = 1.0 + 0.0185 * pick;
        Constants c = {};
        c.frames = 2; c.method = 2; c.gain = 0.5f; c.knee = 0.75f; c.ceilLin = 1.0f; c.outType = 0;
        uint64_t old = 0;
        const double ahead = pass == 0 ? 0.0 : shares[pass - 1];
        if (ahead > 0.0 && (old = warp.PairFor(pickId, pickT)) != 0) c.warp = (float)(ahead / (double)(pickId - old));
        auto t0 = std::chrono::steady_clock::now();
        Begin();
        bool touched = false;
        warp.ToGeneral(d, cb);
        if (pickId != warp.id[warp.cur]) { warp.RecordLuma(d, cb, pick, pickId, pickT, false); touched = true; }
        if (c.warp != 0.0f && (warp.flowNew != pickId || warp.flowOld != old)) { warp.RecordFlow(d, cb); touched = true; printf("pass %d: motion measured\n", pass); }
        if (touched) warp.ToShade(d, cb);
        VkDescriptorSet ws = warp.dsW[pick][c.warp != 0.0f ? 0u : pick];
        D.CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pso);
        D.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, plC, 0, 1, &dsC[pick], 0, nullptr);
        D.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, plC, 1, 1, &dsN[pick], 0, nullptr);
        D.CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, plC, 2, 1, &ws, 0, nullptr);
        D.CmdPushConstants(cb, plC, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(c), &c);
        D.CmdDispatch(cb, (W + 7) / 8, (H + 7) / 8, 1);
        Run();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        printf("pass %d: frame %llu warp %.3f  (%.0f ms on the software GPU)\n", pass, (unsigned long long)pickId, c.warp, ms);
        char name[64]; snprintf(name, sizeof(name), "out%d.rgba", pass);
        Download(out, VK_IMAGE_LAYOUT_GENERAL, W, H, 4, name);
    }
    Download(warp.img[WarpVk::Out()], VK_IMAGE_LAYOUT_GENERAL, warp.w[WarpVk::kOut], warp.h[WarpVk::kOut], 8, "flow.f16");
    printf("flow %ux%u done\n", warp.w[WarpVk::kOut], warp.h[WarpVk::kOut]);
    return 0;
}
