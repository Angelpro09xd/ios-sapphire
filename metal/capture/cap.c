/* Headless turnip driver: run under drm-shim + FD_RD_DUMP to capture the
 * exact a6xx command streams of a clear, a textured blended quad and a blit.
 * Loads the ICD directly (no Vulkan loader on the build host). */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "quad.vert.h"
#include "quad.frag.h"

#define W 256
#define H 256
#define CK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "%s = %d\n", #x, r_); exit(1); } } while (0)

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst; static VkDevice dev; static VkPhysicalDevice pd;
#define I(n) PFN_##n n = (PFN_##n)gipa(inst, #n)

static uint32_t memtype(uint32_t bits) {
    I(vkGetPhysicalDeviceMemoryProperties);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags &
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            return i;
    fprintf(stderr, "no host-visible memory type\n"); exit(1);
}

static VkImage image(VkFormat f, VkImageUsageFlags u, void **map, VkSubresourceLayout *lay) {
    I(vkCreateImage); I(vkGetImageMemoryRequirements); I(vkAllocateMemory);
    I(vkBindImageMemory); I(vkMapMemory); I(vkGetImageSubresourceLayout);
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
        .format = f, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR, .usage = u,
        .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED };
    VkImage im; CK(vkCreateImage(dev, &ci, NULL, &im));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, im, &mr);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size, .memoryTypeIndex = memtype(mr.memoryTypeBits) };
    VkDeviceMemory m; CK(vkAllocateMemory(dev, &ai, NULL, &m));
    CK(vkBindImageMemory(dev, im, m, 0));
    CK(vkMapMemory(dev, m, 0, VK_WHOLE_SIZE, 0, map));
    VkImageSubresource sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
    vkGetImageSubresourceLayout(dev, im, &sr, lay);
    return im;
}

static VkImageView view(VkImage im, VkFormat f) {
    I(vkCreateImageView);
    VkImageViewCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im,
        .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = f,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView v; CK(vkCreateImageView(dev, &ci, NULL, &v)); return v;
}

static void barrier(VkCommandBuffer cb, VkImage im, VkImageLayout from, VkImageLayout to) {
    I(vkCmdPipelineBarrier);
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
}

int main(int argc, char **argv) {
    const char *icd = argc > 1 ? argv[1] : "libvulkan_freedreno.so";
    void *h = dlopen(icd, RTLD_NOW); if (!h) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
    uint32_t ver = 5; PFN_vkVoidFunction neg = (PFN_vkVoidFunction)dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
    if (neg) ((VkResult (*)(uint32_t *))neg)(&ver);

    I(vkCreateInstance);
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    CK(vkCreateInstance(&ici, NULL, &inst));
    I(vkEnumeratePhysicalDevices); I(vkGetPhysicalDeviceProperties);
    uint32_t n = 1; CK(vkEnumeratePhysicalDevices(inst, &n, &pd)); if (!n) { fprintf(stderr, "no GPU\n"); return 1; }
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp); printf("device: %s\n", pp.deviceName);

    float prio = 1;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = 1 };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f13,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    I(vkCreateDevice); CK(vkCreateDevice(pd, &dci, NULL, &dev));
    I(vkGetDeviceQueue); VkQueue q; vkGetDeviceQueue(dev, 0, 0, &q);

    /* the framebuffer format of the phone (BGRA8 linear) and an RGBA8 "IOSurface" */
    const VkFormat RT = VK_FORMAT_B8G8R8A8_UNORM, TX = VK_FORMAT_R8G8B8A8_UNORM;
    void *rtmap, *txmap, *dsmap; VkSubresourceLayout rtl, txl, dsl;
    VkImage rt = image(RT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &rtmap, &rtl);
    VkImage tx = image(TX, VK_IMAGE_USAGE_SAMPLED_BIT, &txmap, &txl);
    VkImage ds = image(RT, VK_IMAGE_USAGE_TRANSFER_DST_BIT, &dsmap, &dsl);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
        ((uint32_t *)((char *)txmap + y * txl.rowPitch))[x] = 0xff000000u | (uint32_t)y << 8 | (uint32_t)x;
    printf("pitches rt %llu tx %llu\n", (unsigned long long)rtl.rowPitch, (unsigned long long)txl.rowPitch);
    VkImageView rtv = view(rt, RT), txv = view(tx, TX);

    I(vkCreateSampler);
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    VkSampler smp; CK(vkCreateSampler(dev, &sci, NULL, &smp));

    I(vkCreateDescriptorSetLayout); I(vkCreatePipelineLayout); I(vkCreateDescriptorPool);
    I(vkAllocateDescriptorSets); I(vkUpdateDescriptorSets);
    VkDescriptorSetLayoutBinding b0 = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b0 };
    VkDescriptorSetLayout dsl0; CK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl0));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl0, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
    VkDescriptorPool dp; CK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl0 };
    VkDescriptorSet set; CK(vkAllocateDescriptorSets(dev, &dsai, &set));
    VkDescriptorImageInfo dii = { smp, txv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &dii };
    vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);

    I(vkCreateShaderModule); I(vkCreateGraphicsPipelines);
    VkShaderModuleCreateInfo smv = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(quad_vert), .pCode = quad_vert };
    VkShaderModuleCreateInfo smf = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof(quad_frag), .pCode = quad_frag };
    VkShaderModule vs, fs; CK(vkCreateShaderModule(dev, &smv, NULL, &vs)); CK(vkCreateShaderModule(dev, &smf, NULL, &fs));
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    VkViewport vp = { 0, 0, W, H, 0, 1 }; VkRect2D sc = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1 };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    /* premultiplied source-over, what CoreAnimation uses for every layer */
    VkPipelineColorBlendAttachmentState cba = { .blendEnable = 1,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE, .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
    VkPipelineRenderingCreateInfo prci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &RT };
    VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &prci,
        .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps,
        .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl };
    VkPipeline pipe; CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));

    I(vkCreateCommandPool); I(vkAllocateCommandBuffers); I(vkBeginCommandBuffer); I(vkEndCommandBuffer);
    I(vkCmdBeginRendering); I(vkCmdEndRendering); I(vkCmdBindPipeline); I(vkCmdBindDescriptorSets);
    I(vkCmdPushConstants); I(vkCmdDraw); I(vkCmdBlitImage); I(vkCreateFence); I(vkQueueSubmit); I(vkWaitForFences);
    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandPool cp; CK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer c; CK(vkAllocateCommandBuffers(dev, &cbai, &c));
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CK(vkBeginCommandBuffer(c, &cbbi));
    barrier(c, tx, VK_IMAGE_LAYOUT_PREINITIALIZED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(c, rt, VK_IMAGE_LAYOUT_PREINITIALIZED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo att = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = rtv,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE, .clearValue.color.float32 = { 0.0f, 0.0f, 0.5f, 1.0f } };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = sc, .layerCount = 1,
        .colorAttachmentCount = 1, .pColorAttachments = &att };
    vkCmdBeginRendering(c, &ri);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, NULL);
    float pcv[8] = { -0.5f, -0.5f, 0.5f, 0.5f, 1, 1, 1, 1 };
    vkCmdPushConstants(c, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pcv);
    vkCmdDraw(c, 4, 1, 0, 0);
    /* CAP_DRAWS=n: more quads, each a different rect and tint, to see what one more layer costs */
    int draws = getenv("CAP_DRAWS") ? atoi(getenv("CAP_DRAWS")) : 1;
    for (int i = 1; i < draws; i++) {
        float p2[8] = { -0.9f + 0.1f * i, -0.9f, -0.6f + 0.1f * i, -0.6f, 0.5f, 0.5f, 0.5f, 0.5f };
        vkCmdPushConstants(c, pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, p2);
        vkCmdDraw(c, 4, 1, 0, 0);
    }
    vkCmdEndRendering(c);
    barrier(c, rt, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(c, ds, VK_IMAGE_LAYOUT_PREINITIALIZED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit bl = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { 0, 0, 0 }, { W, H, 1 } },
                       { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { 0, 0, 0 }, { W, H, 1 } } };
    vkCmdBlitImage(c, rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ds, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
    CK(vkEndCommandBuffer(c));
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fe; CK(vkCreateFence(dev, &fci, NULL, &fe));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &c };
    CK(vkQueueSubmit(q, 1, &si, fe));
    CK(vkWaitForFences(dev, 1, &fe, 1, 2000000000ull));
    printf("submitted; rt[32,32] = %08x (zero under drm-shim)\n", ((uint32_t *)((char *)rtmap + 32 * rtl.rowPitch))[32]);
    return 0;
}
