#include "GraphicsPipeline.hpp"

#include <cstring>

namespace abx::d3d12x {

const std::vector<VkDynamicState>& GraphicsDynamicStates() {
    static const std::vector<VkDynamicState> states{
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT,
        VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
        VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE,
    };
    return states;
}

std::string GraphicsStateKey(const GraphicsPipelineState& state, const VertexInput& input) {
    std::string key;
    const auto add = [&key](std::uint64_t value) {
        key += std::to_string(value);
        key += ',';
    };
    const auto addFloat = [&add](float value) {
        std::uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        add(bits);
    };
    const auto addStencil = [&add](const VkStencilOpState& face) {
        add(face.failOp);
        add(face.passOp);
        add(face.depthFailOp);
        add(face.compareOp);
        add(face.compareMask);
        add(face.writeMask);
    };
    add(state.topology);
    add(state.stripCut);
    add(state.polygonMode);
    add(state.cullMode);
    add(state.frontFace);
    add(state.depthClamp);
    add(state.depthBias);
    addFloat(state.depthBiasConstant);
    addFloat(state.depthBiasClamp);
    addFloat(state.depthBiasSlope);
    add(state.samples);
    add(state.sampleMask);
    add(state.alphaToCoverage);
    add(state.depthTest);
    add(state.depthWrite);
    add(state.depthCompare);
    add(state.stencilTest);
    addStencil(state.stencilFront);
    addStencil(state.stencilBack);
    add(state.logicOpEnable);
    add(state.logicOp);
    key += "b";
    for (const VkPipelineColorBlendAttachmentState& blend : state.blend) {
        add(blend.blendEnable);
        add(blend.srcColorBlendFactor);
        add(blend.dstColorBlendFactor);
        add(blend.colorBlendOp);
        add(blend.srcAlphaBlendFactor);
        add(blend.dstAlphaBlendFactor);
        add(blend.alphaBlendOp);
        add(blend.colorWriteMask);
    }
    key += "f";
    for (const VkFormat format : state.colorFormats) {
        add(format);
    }
    add(state.depthFormat);
    add(state.stencilFormat);
    key += "v";
    for (const VkVertexInputBindingDescription& binding : input.bindings) {
        add(binding.binding);
        add(binding.inputRate);
    }
    key += "a";
    for (const VkVertexInputAttributeDescription& attribute : input.attributes) {
        add(attribute.location);
        add(attribute.binding);
        add(attribute.format);
        add(attribute.offset);
    }
    return key;
}

VkResult CreateGraphicsPipeline(const VulkanDevice& device, VkPipelineCache cache, const GraphicsPipelineState& state, const std::vector<GraphicsStage>& stages,
                                const VertexInput& input, VkPipelineCreateFlags flags, VkPipeline* pipeline, std::string& error) {
    *pipeline = VK_NULL_HANDLE;
    for (const std::string& feature : state.features) {
        if (!device.HasFeature(feature)) {
            error = "the pipeline state needs " + feature + ", which the device does not offer";
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    if (state.colorFormats.size() > kMaxColorTargets || state.blend.size() != state.colorFormats.size()) {
        error = "the pipeline state has " + std::to_string(state.colorFormats.size()) + " color targets and " + std::to_string(state.blend.size()) + " blend states";
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    std::vector<VkPipelineShaderStageCreateInfo> stageInfos(stages.size());
    std::vector<VkPipelineShaderStageRequiredSubgroupSizeCreateInfo> subgroups(stages.size());
    bool hasVertex = false;
    for (std::size_t index = 0; index < stages.size(); ++index) {
        VkPipelineShaderStageCreateInfo& info = stageInfos[index];
        info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        info.stage = stages[index].stage;
        info.module = stages[index].module;
        info.pName = "main";
        hasVertex = hasVertex || stages[index].stage == VK_SHADER_STAGE_VERTEX_BIT;
        if (stages[index].subgroupSize != 0 && device.CanRequireSubgroupSize(stages[index].subgroupSize, stages[index].stage)) {
            subgroups[index].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO;
            subgroups[index].requiredSubgroupSize = stages[index].subgroupSize;
            info.pNext = &subgroups[index];
        }
    }
    if (!hasVertex) {
        error = "the pipeline has no vertex shader";
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = static_cast<std::uint32_t>(input.bindings.size());
    vertexInput.pVertexBindingDescriptions = input.bindings.empty() ? nullptr : input.bindings.data();
    vertexInput.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(input.attributes.size());
    vertexInput.pVertexAttributeDescriptions = input.attributes.empty() ? nullptr : input.attributes.data();
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = state.topology;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rasterization{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterization.depthClampEnable = state.depthClamp ? VK_TRUE : VK_FALSE;
    rasterization.polygonMode = state.polygonMode;
    rasterization.cullMode = state.cullMode;
    rasterization.frontFace = state.frontFace;
    rasterization.depthBiasEnable = state.depthBias ? VK_TRUE : VK_FALSE;
    rasterization.depthBiasConstantFactor = state.depthBiasConstant;
    rasterization.depthBiasClamp = state.depthBiasClamp;
    rasterization.depthBiasSlopeFactor = state.depthBiasSlope;
    rasterization.lineWidth = 1.0f;
    const VkSampleMask sampleMask = state.sampleMask;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = state.samples;
    multisample.pSampleMask = &sampleMask;
    multisample.alphaToCoverageEnable = state.alphaToCoverage ? VK_TRUE : VK_FALSE;
    VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depthStencil.depthTestEnable = state.depthTest ? VK_TRUE : VK_FALSE;
    depthStencil.depthWriteEnable = state.depthWrite ? VK_TRUE : VK_FALSE;
    depthStencil.depthCompareOp = state.depthCompare;
    depthStencil.stencilTestEnable = state.stencilTest ? VK_TRUE : VK_FALSE;
    depthStencil.front = state.stencilFront;
    depthStencil.back = state.stencilBack;
    depthStencil.maxDepthBounds = 1.0f;
    VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.logicOpEnable = state.logicOpEnable ? VK_TRUE : VK_FALSE;
    colorBlend.logicOp = state.logicOp;
    colorBlend.attachmentCount = static_cast<std::uint32_t>(state.blend.size());
    colorBlend.pAttachments = state.blend.empty() ? nullptr : state.blend.data();
    const std::vector<VkDynamicState>& dynamicStates = GraphicsDynamicStates();
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = static_cast<std::uint32_t>(state.colorFormats.size());
    rendering.pColorAttachmentFormats = state.colorFormats.empty() ? nullptr : state.colorFormats.data();
    rendering.depthAttachmentFormat = state.depthFormat;
    rendering.stencilAttachmentFormat = state.stencilFormat;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rendering};
    info.flags = flags;
    info.stageCount = static_cast<std::uint32_t>(stageInfos.size());
    info.pStages = stageInfos.data();
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &inputAssembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &rasterization;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depthStencil;
    info.pColorBlendState = &colorBlend;
    info.pDynamicState = &dynamic;
    info.layout = device.TitlePipelineLayout();
    const VkResult result = device.Vk().createGraphicsPipelines(device.Handle(), cache, 1, &info, nullptr, pipeline);
    if (result != VK_SUCCESS) {
        *pipeline = VK_NULL_HANDLE;
        if (result != VK_PIPELINE_COMPILE_REQUIRED) {
            error = "vkCreateGraphicsPipelines failed with " + std::to_string(static_cast<int>(result));
        }
    }
    return result;
}

}
