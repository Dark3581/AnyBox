#pragma once

#include "VulkanDevice.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace abx::d3d12x {

constexpr std::uint32_t kMaxColorTargets = 8;

struct GraphicsPipelineState {
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    bool stripCut = false;
    VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
    VkCullModeFlags cullMode = VK_CULL_MODE_BACK_BIT;
    VkFrontFace frontFace = VK_FRONT_FACE_CLOCKWISE;
    bool depthClamp = false;
    bool conservative = false;
    bool depthBias = false;
    float depthBiasConstant = 0.0f;
    float depthBiasClamp = 0.0f;
    float depthBiasSlope = 0.0f;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    std::uint32_t sampleMask = 0xFFFFFFFFu;
    bool alphaToCoverage = false;
    bool depthTest = false;
    bool depthWrite = false;
    VkCompareOp depthCompare = VK_COMPARE_OP_ALWAYS;
    bool stencilTest = false;
    VkStencilOpState stencilFront{};
    VkStencilOpState stencilBack{};
    bool logicOpEnable = false;
    VkLogicOp logicOp = VK_LOGIC_OP_COPY;
    std::vector<VkPipelineColorBlendAttachmentState> blend;
    std::vector<VkFormat> colorFormats;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkFormat stencilFormat = VK_FORMAT_UNDEFINED;
    std::set<std::string> features;
};

const std::vector<VkDynamicState>& GraphicsDynamicStates();

struct GraphicsStage {
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_VERTEX_BIT;
    VkShaderModule module = VK_NULL_HANDLE;
    std::uint32_t subgroupSize = 0;
};

struct VertexInput {
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
};

std::string GraphicsStateKey(const GraphicsPipelineState& state, const VertexInput& input);

VkResult CreateGraphicsPipeline(const VulkanDevice& device, VkPipelineCache cache, const GraphicsPipelineState& state, const std::vector<GraphicsStage>& stages,
                                const VertexInput& input, VkPipelineCreateFlags flags, VkPipeline* pipeline, std::string& error);

}
