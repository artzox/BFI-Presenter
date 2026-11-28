// Build on Linux: g++ -O1 -I../../../third_party/vulkan-headers crtcheck.cpp -lvulkan; run with a software Vulkan driver, e.g.
// VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./a.out
// Lavapipe check of the CRT hand-off passes: pipelines made as bfi_crt_vk.h makes them, the pattern run on a grey
// frame, and the result read back (scanlines: rows must differ).
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdint>
#include <string>
#include "../../shaders_spv.h"
#include "../../../dx/bfi_crt_gen.h"
#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); return 1; } } while (0)
VkDevice dev; VkPhysicalDevice pd;
uint32_t memType(uint32_t bits, VkMemoryPropertyFlags f) { VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i; return 0; }
VkShaderModule sm(const uint32_t *c, size_t n) { VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO }; ci.codeSize = n; ci.pCode = c; VkShaderModule m; if (vkCreateShaderModule(dev, &ci, nullptr, &m)) return 0; return m; }
VkPipeline cp(VkPipelineLayout pl, VkShaderModule m) { VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
  ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = m; ci.stage.pName = "main"; ci.layout = pl;
  VkPipeline p = 0; if (vkCreateComputePipelines(dev, 0, 1, &ci, nullptr, &p)) return 0; return p; }
struct Img { VkImage i; VkDeviceMemory m; VkImageView v; };
Img mk(VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags u) { Img r; VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO }; ci.imageType = VK_IMAGE_TYPE_2D; ci.format = f; ci.extent = { w, h, 1 };
  ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = u; vkCreateImage(dev, &ci, nullptr, &r.i);
  VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, r.i, &mr); VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; ai.allocationSize = mr.size; ai.memoryTypeIndex = memType(mr.memoryTypeBits, 0);
  vkAllocateMemory(dev, &ai, nullptr, &r.m); vkBindImageMemory(dev, r.i, r.m, 0); VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO }; vi.image = r.i; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = f;
  vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }; vkCreateImageView(dev, &vi, nullptr, &r.v); return r; }
int main()
{
  VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO }; app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ii = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO }; ii.pApplicationInfo = &app; VkInstance inst; CK(vkCreateInstance(&ii, nullptr, &inst));
  uint32_t n = 1; CK(vkEnumeratePhysicalDevices(inst, &n, &pd) < 0 ? VK_ERROR_UNKNOWN : VK_SUCCESS);
  float pr = 1; VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO }; qi.queueCount = 1; qi.pQueuePriorities = &pr;
  VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO }; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi; CK(vkCreateDevice(pd, &di, nullptr, &dev));
  VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);
  const uint32_t W = 384, H = 216;
  // crt.hlsl layout
  VkDescriptorSetLayoutBinding b[10] = {};
  for (int i = 0; i < 10; i++) { b[i].binding = i; b[i].descriptorCount = 1; b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    b[i].descriptorType = i <= 5 || i == 9 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : i == 6 ? VK_DESCRIPTOR_TYPE_SAMPLER : i == 7 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; }
  VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO }; dli.bindingCount = 10; dli.pBindings = b; VkDescriptorSetLayout dsl; CK(vkCreateDescriptorSetLayout(dev, &dli, nullptr, &dsl));
  VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO }; pli.setLayoutCount = 1; pli.pSetLayouts = &dsl; VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &pli, nullptr, &pl));
  const uint32_t *code[6] = { kCrtPattern0Spv, kCrtPattern1Spv, kCrtSharpenSpv, kCrtSoftenSpv, kCrtGrainRawSpv, kCrtTailSpv };
  const size_t bytes[6] = { sizeof(kCrtPattern0Spv), sizeof(kCrtPattern1Spv), sizeof(kCrtSharpenSpv), sizeof(kCrtSoftenSpv), sizeof(kCrtGrainRawSpv), sizeof(kCrtTailSpv) };
  VkPipeline pso[6];
  for (int i = 0; i < 6; i++) { VkShaderModule m = sm(code[i], bytes[i]); pso[i] = m ? cp(pl, m) : 0; printf("pass %d: module %s pipeline %s\n", i, m ? "ok" : "FAIL", pso[i] ? "ok" : "FAIL"); }
  // crtwarp pipeline: sets 0-3 as the BFI pass
  {
    VkDescriptorSetLayoutBinding b0[3] = {}; for (int i = 0; i < 3; i++) { b0[i].binding = i; b0[i].descriptorCount = 1; b0[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; b0[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; }
    VkDescriptorSetLayoutBinding b1[1] = {}; b1[0].descriptorCount = 1; b1[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; b1[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    VkDescriptorSetLayoutBinding b2[8] = {}; for (int i = 0; i < 8; i++) { b2[i].binding = i; b2[i].descriptorCount = 1; b2[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; b2[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; }
    VkDescriptorSetLayoutBinding b3[2] = {}; for (int i = 0; i < 2; i++) { b3[i].binding = i; b3[i].descriptorCount = 1; b3[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT; b3[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; }
    VkDescriptorSetLayout l[4]; VkDescriptorSetLayoutBinding *bb[4] = { b0, b1, b2, b3 }; uint32_t nb[4] = { 3, 1, 8, 2 };
    for (int k = 0; k < 4; k++) { VkDescriptorSetLayoutCreateInfo c = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO }; c.bindingCount = nb[k]; c.pBindings = bb[k]; CK(vkCreateDescriptorSetLayout(dev, &c, nullptr, &l[k])); }
    VkPushConstantRange pr2 = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 128 };
    VkPipelineLayoutCreateInfo p2 = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO }; p2.setLayoutCount = 4; p2.pSetLayouts = l; p2.pushConstantRangeCount = 1; p2.pPushConstantRanges = &pr2; VkPipelineLayout plw; CK(vkCreatePipelineLayout(dev, &p2, nullptr, &plw));
    VkShaderModule m = sm(kCrtWarpSpv, sizeof(kCrtWarpSpv)); printf("crtwarp: module %s pipeline %s\n", m ? "ok" : "FAIL", m && cp(plw, m) ? "ok" : "FAIL");
  }
  if (!pso[1]) return 1;
  // run the pattern (BEAM 1) on a mid-grey frame
  Img in = mk(VK_FORMAT_R8G8B8A8_UNORM, W, H, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  Img sh = mk(VK_FORMAT_R32_UINT, W, H, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  Img dm = mk(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  Img out = mk(VK_FORMAT_R8G8B8A8_UNORM, W, H, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
  VkBuffer ub, rb; VkDeviceMemory um, rm;
  auto mkb = [&](VkDeviceSize sz, VkBufferUsageFlags u, VkBuffer &bf, VkDeviceMemory &mm) { VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO }; bi.size = sz; bi.usage = u; vkCreateBuffer(dev, &bi, nullptr, &bf);
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, bf, &mr); VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO }; ai.allocationSize = mr.size; ai.memoryTypeIndex = memType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(dev, &ai, nullptr, &mm); vkBindBufferMemory(dev, bf, mm, 0); };
  mkb(4096, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, ub, um);
  mkb((VkDeviceSize)W * H * 4 * 2, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, rb, rm);
  float *p; vkMapMemory(dev, um, 0, VK_WHOLE_SIZE, 0, (void **)&p); memset(p, 0, 4096);
  for (int i = 0; i < kCrtParamN; i++) for (int j = 0; j < 4; j++) p[i * 4 + j] = kCrtParams[i].def[j];
  p[kCrtParamN * 4] = W; p[kCrtParamN * 4 + 1] = H;
  uint8_t *rp; vkMapMemory(dev, rm, 0, VK_WHOLE_SIZE, 0, (void **)&rp);
  for (uint32_t i = 0; i < W * H; i++) { rp[i * 4] = rp[i * 4 + 1] = rp[i * 4 + 2] = 160; rp[i * 4 + 3] = 255; }
  VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO }; si.magFilter = si.minFilter = VK_FILTER_LINEAR; VkSampler smp; vkCreateSampler(dev, &si, nullptr, &smp);
  VkDescriptorPoolSize ps[4] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 7 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 } };
  VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO }; dpi.maxSets = 1; dpi.poolSizeCount = 4; dpi.pPoolSizes = ps; VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpi, nullptr, &dp));
  VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO }; dai.descriptorPool = dp; dai.descriptorSetCount = 1; dai.pSetLayouts = &dsl; VkDescriptorSet ds; CK(vkAllocateDescriptorSets(dev, &dai, &ds));
  const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo im[10] = { { 0, in.v, G }, { 0, dm.v, G }, { 0, dm.v, G }, { 0, dm.v, G }, { 0, dm.v, G }, { 0, dm.v, G }, { smp, 0, G }, { 0, out.v, G }, {}, { 0, sh.v, G } };
  VkDescriptorBufferInfo bi = { ub, 0, (VkDeviceSize)kCrtSlots * 16 };
  VkWriteDescriptorSet wr[10] = {};
  for (int k = 0; k < 10; k++) { wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; wr[k].dstSet = ds; wr[k].dstBinding = k; wr[k].descriptorCount = 1; wr[k].descriptorType = b[k].descriptorType; if (k == 8) wr[k].pBufferInfo = &bi; else wr[k].pImageInfo = &im[k]; }
  vkUpdateDescriptorSets(dev, 10, wr, 0, nullptr);
  VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; VkCommandPool cpool; vkCreateCommandPool(dev, &pci, nullptr, &cpool);
  VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO }; cai.commandPool = cpool; cai.commandBufferCount = 1; VkCommandBuffer cb; vkAllocateCommandBuffers(dev, &cai, &cb);
  VkCommandBufferBeginInfo bgi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; vkBeginCommandBuffer(cb, &bgi);
  auto bar = [&](VkImage i, VkImageLayout a, VkImageLayout c2) { VkImageMemoryBarrier m = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER }; m.oldLayout = a; m.newLayout = c2; m.srcQueueFamilyIndex = m.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; m.image = i;
    m.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }; m.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; m.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &m); };
  bar(in.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL); bar(sh.i, VK_IMAGE_LAYOUT_UNDEFINED, G); bar(dm.i, VK_IMAGE_LAYOUT_UNDEFINED, G); bar(out.i, VK_IMAGE_LAYOUT_UNDEFINED, G);
  VkBufferImageCopy bc = {}; bc.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }; bc.imageExtent = { W, H, 1 };
  vkCmdCopyBufferToImage(cb, rb, in.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc); bar(in.i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, G);
  VkClearColorValue zc = {}; VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }; (void)zc; (void)rr;
  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pso[1]); uint32_t off = 0; vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 1, &off);
  vkCmdDispatch(cb, (W + 7) / 8, (H + 7) / 8, 1);
  bar(out.i, G, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  vkCmdCopyImageToBuffer(cb, out.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &bc);
  vkEndCommandBuffer(cb);
  VkSubmitInfo sub = { VK_STRUCTURE_TYPE_SUBMIT_INFO }; sub.commandBufferCount = 1; sub.pCommandBuffers = &cb; CK(vkQueueSubmit(q, 1, &sub, 0)); vkQueueWaitIdle(q);
  printf("rows (x=100, R G B):\n");
  for (uint32_t y = 100; y < 112; y++) { uint8_t *px = rp + (y * W + 100) * 4; printf("  y=%u: %3d %3d %3d\n", y, px[0], px[1], px[2]); }
  printf("cols (y=100):"); for (uint32_t x = 100; x < 112; x++) { uint8_t *px = rp + (100 * W + x) * 4; printf(" %d/%d/%d", px[0], px[1], px[2]); } printf("\n");
  return 0;
}
