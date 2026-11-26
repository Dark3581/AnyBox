#include "DepthPlanePass.hpp"

#include "AbxApi.hpp"
#include "GraphicsPipeline.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace abx::d3d12x {

namespace {

const std::uint32_t kPlaneVertexSpirv[] = {
#include "DepthPlanePassVertSpirv.inc"
};

const std::uint32_t kPlaneFragmentSpirv[] = {
#include "DepthPlanePassFragSpirv.inc"
};

std::mutex g_planeLock;

}

DepthPlanePass* DepthPlanePass::For(const VulkanDevice& device) {
    static DepthPlanePass* created = nullptr;
    static bool tried = false;
    const std::lock_guard guard(g_planeLock);
    if (tried) return created;
    tried = true;
    auto* pass = new DepthPlanePass(device);
    std::string error;
    if (!pass->Create(error)) {
        AbxLog("d3d12_x", "depth values written through unordered access views cannot be drawn into depth planes on this device, so such views stay null views: %s", error.c_str());
        delete pass;
        return nullptr;
    }
    created = pass;
    return created;
}

bool DepthPlanePass::Create(std::string& error) {
    const DeviceFunctions& vk = device_.Vk();
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    setInfo.bindingCount = 1;
    setInfo.pBindings = &binding;
    if (vk.createDescriptorSetLayout(device_.Handle(), &setInfo, nullptr, &setLayout_) != VK_SUCCESS) {
        setLayout_ = VK_NULL_HANDLE;
        error = "vkCreateDescriptorSetLayout failed for the depth plane pass";
        return false;
    }
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout_;
    if (vk.createPipelineLayout(device_.Handle(), &layoutInfo, nullptr, &layout_) != VK_SUCCESS) {
        layout_ = VK_NULL_HANDLE;
        vk.destroyDescriptorSetLayout(device_.Handle(), setLayout_, nullptr);
        error = "vkCreatePipelineLayout failed for the depth plane pass";
        return false;
    }
    VkShaderModuleCreateInfo vertexInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    vertexInfo.codeSize = sizeof(kPlaneVertexSpirv);
    vertexInfo.pCode = kPlaneVertexSpirv;
    VkShaderModuleCreateInfo fragmentInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    fragmentInfo.codeSize = sizeof(kPlaneFragmentSpirv);
    fragmentInfo.pCode = kPlaneFragmentSpirv;
    if (vk.createShaderModule(device_.Handle(), &vertexInfo, nullptr, &vertex_) != VK_SUCCESS) vertex_ = VK_NULL_HANDLE;
    if (vk.createShaderModule(device_.Handle(), &fragmentInfo, nullptr, &fragment_) != VK_SUCCESS) fragment_ = VK_NULL_HANDLE;
    if (vertex_ == VK_NULL_HANDLE || fragment_ == VK_NULL_HANDLE) {
        if (vertex_ != VK_NULL_HANDLE) vk.destroyShaderModule(device_.Handle(), vertex_, nullptr);
        if (fragment_ != VK_NULL_HANDLE) vk.destroyShaderModule(device_.Handle(), fragment_, nullptr);
        vk.destroyPipelineLayout(device_.Handle(), layout_, nullptr);
        vk.destroyDescriptorSetLayout(device_.Handle(), setLayout_, nullptr);
        error = "vkCreateShaderModule failed for the depth plane pass";
        return false;
    }
    return true;
}

bool DepthPlanePass::CreateSource(VkImageView values, Source& source, std::string& error) const {
    const DeviceFunctions& vk = device_.Vk();
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;
    if (vk.createDescriptorPool(device_.Handle(), &poolInfo, nullptr, &source.pool) != VK_SUCCESS) {
        source.pool = VK_NULL_HANDLE;
        error = "vkCreateDescriptorPool failed for the depth plane pass";
        return false;
    }
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = source.pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &setLayout_;
    if (vk.allocateDescriptorSets(device_.Handle(), &allocate, &source.set) != VK_SUCCESS) {
        DestroySource(source);
        error = "vkAllocateDescriptorSets failed for the depth plane pass";
        return false;
    }
    const VkDescriptorImageInfo image{VK_NULL_HANDLE, values, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = source.set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &image;
    vk.updateDescriptorSets(device_.Handle(), 1, &write, 0, nullptr);
    return true;
}

void DepthPlanePass::DestroySource(Source& source) const {
    if (source.pool != VK_NULL_HANDLE) device_.Vk().destroyDescriptorPool(device_.Handle(), source.pool, nullptr);
    source.pool = VK_NULL_HANDLE;
    source.set = VK_NULL_HANDLE;
}

VkPipeline DepthPlanePass::Pipeline(VkFormat depthFormat, bool stencil, std::string& error) {
    const std::lock_guard guard(lock_);
    const auto known = pipelines_.find({depthFormat, stencil});
    if (known != pipelines_.end()) return known->second;
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rasterization{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterization.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    depthStencil.maxDepthBounds = 1.0f;
    VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    std::vector<VkDynamicState> dynamicStates = GraphicsDynamicStates();
    dynamicStates.erase(std::remove(dynamicStates.begin(), dynamicStates.end(), VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE), dynamicStates.end());
    if (device_.RectangleFill()) dynamicStates.push_back(VK_DYNAMIC_STATE_POLYGON_MODE_EXT);
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.depthAttachmentFormat = depthFormat;
    rendering.stencilAttachmentFormat = stencil ? depthFormat : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamic;
    info.layout = layout_;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult result = device_.Vk().createGraphicsPipelines(device_.Handle(), VK_NULL_HANDLE, 1, &info, nullptr, &pipeline);
    if (result != VK_SUCCESS) {
        error = "vkCreateGraphicsPipelines failed for the depth plane pass with " + std::to_string(static_cast<int>(result));
        return VK_NULL_HANDLE;
    }
    pipelines_.emplace(std::make_pair(depthFormat, stencil), pipeline);
    return pipeline;
}

void DepthPlanePass::Record(VkCommandBuffer commands, VkPipeline pipeline, const Source& source, VkImageView depth, bool stencil, VkExtent2D extent, bool setFixedState) const {
    const DeviceFunctions& vk = device_.Vk();
    VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAttachment.imageView = depth;
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    const VkRenderingAttachmentInfo stencilAttachment = depthAttachment;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    rendering.renderArea = {{0, 0}, extent};
    rendering.layerCount = 1;
    rendering.pDepthAttachment = &depthAttachment;
    rendering.pStencilAttachment = stencil ? &stencilAttachment : nullptr;
    vk.cmdBeginRendering(commands, &rendering);
    vk.cmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vk.cmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &source.set, 0, nullptr);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, extent};
    vk.cmdSetViewportWithCount(commands, 1, &viewport);
    vk.cmdSetScissorWithCount(commands, 1, &scissor);
    vk.cmdSetPrimitiveTopology(commands, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vk.cmdSetPrimitiveRestartEnable(commands, VK_FALSE);
    if (device_.RectangleFill()) vk.cmdSetPolygonMode(commands, VK_POLYGON_MODE_FILL);
    if (setFixedState) {
        const float constants[4] = {};
        vk.cmdSetBlendConstants(commands, constants);
        vk.cmdSetStencilReference(commands, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    }
    vk.cmdDraw(commands, 3, 1, 0, 0);
    vk.cmdEndRendering(commands);
}

}
