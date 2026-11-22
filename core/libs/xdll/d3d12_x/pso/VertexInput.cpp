#include "VertexInput.hpp"

#include "GraphicsState.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace abx::d3d12x {

namespace {

constexpr UINT kInputSlots = 32;
constexpr UINT kMaxInputElements = 32;

bool SameSemantic(const char* a, const std::string& b) {
    std::size_t index = 0;
    for (; a[index] != '\0' && index < b.size(); ++index) {
        if (std::toupper(static_cast<unsigned char>(a[index])) != std::toupper(static_cast<unsigned char>(b[index]))) {
            return false;
        }
    }
    return a[index] == '\0' && index == b.size();
}

const char* TypeName(InputType type) {
    switch (type) {
    case InputType::Float:
        return "float";
    case InputType::Uint:
        return "uint";
    case InputType::Sint:
        return "sint";
    }
    return "unknown";
}

struct SlotState {
    bool used = false;
    D3D12_INPUT_CLASSIFICATION classification = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
    UINT end = 0;
};

}

std::optional<std::uint32_t> VertexFormatBytes(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
        return 16u;
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
        return 12u;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
        return 8u;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SNORM:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return 4u;
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SNORM:
    case DXGI_FORMAT_R16_SINT:
        return 2u;
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM:
    case DXGI_FORMAT_R8_SINT:
        return 1u;
    default:
        return std::nullopt;
    }
}

InputType VertexFormatType(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R8_UINT:
        return InputType::Uint;
    case DXGI_FORMAT_R32G32B32A32_SINT:
    case DXGI_FORMAT_R32G32B32_SINT:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_R8_SINT:
        return InputType::Sint;
    default:
        return InputType::Float;
    }
}

std::optional<VertexInput> TranslateInputLayout(const D3D12_INPUT_LAYOUT_DESC& layout, const std::vector<ShaderInput>& inputs, std::string& error) {
    if (layout.NumElements > kMaxInputElements) {
        error = std::to_string(layout.NumElements) + " input elements are more than " + std::to_string(kMaxInputElements);
        return std::nullopt;
    }
    if (layout.NumElements != 0 && layout.pInputElementDescs == nullptr) {
        error = "the input layout names elements but has no element array";
        return std::nullopt;
    }
    std::array<SlotState, kInputSlots> slots{};
    std::vector<bool> matched(inputs.size(), false);
    VertexInput result;
    for (UINT index = 0; index < layout.NumElements; ++index) {
        const D3D12_INPUT_ELEMENT_DESC& element = layout.pInputElementDescs[index];
        if (element.SemanticName == nullptr) {
            error = "input element " + std::to_string(index) + " has no semantic name";
            return std::nullopt;
        }
        const std::string name = std::string(element.SemanticName) + std::to_string(element.SemanticIndex);
        if (element.InputSlot >= kInputSlots) {
            error = "input element " + name + " uses slot " + std::to_string(element.InputSlot);
            return std::nullopt;
        }
        const std::optional<VkFormat> format = VulkanFormat(element.Format);
        const std::optional<std::uint32_t> bytes = VertexFormatBytes(element.Format);
        if (!format || !bytes) {
            error = "input element " + name + " has format " + std::to_string(static_cast<int>(element.Format)) + ", which has no Vulkan vertex mapping";
            return std::nullopt;
        }
        if (element.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA) {
            if (element.InstanceDataStepRate != 0) {
                error = "per-vertex input element " + name + " has instance step rate " + std::to_string(element.InstanceDataStepRate);
                return std::nullopt;
            }
        } else if (element.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA) {
            if (element.InstanceDataStepRate != 1) {
                error = "input element " + name + " has instance step rate " + std::to_string(element.InstanceDataStepRate) + ", and only 1 is supported";
                return std::nullopt;
            }
        } else {
            error = "input element " + name + " has classification " + std::to_string(static_cast<int>(element.InputSlotClass));
            return std::nullopt;
        }
        SlotState& slot = slots[element.InputSlot];
        if (slot.used && slot.classification != element.InputSlotClass) {
            error = "slot " + std::to_string(element.InputSlot) + " mixes per-vertex and per-instance elements";
            return std::nullopt;
        }
        UINT offset = element.AlignedByteOffset;
        if (offset == D3D12_APPEND_ALIGNED_ELEMENT) {
            const UINT alignment = (std::min)(4u, *bytes);
            offset = (slot.end + alignment - 1) / alignment * alignment;
        }
        if (offset > 2047) {
            error = "input element " + name + " starts at byte " + std::to_string(offset) + ", past the 2047 every Vulkan device supports";
            return std::nullopt;
        }
        slot.used = true;
        slot.classification = element.InputSlotClass;
        slot.end = offset + *bytes;
        for (std::size_t input = 0; input < inputs.size(); ++input) {
            if (inputs[input].index != element.SemanticIndex || !SameSemantic(element.SemanticName, inputs[input].semantic)) {
                continue;
            }
            if (matched[input]) {
                error = "the input layout names " + name + " twice";
                return std::nullopt;
            }
            if (VertexFormatType(element.Format) != inputs[input].type) {
                error = "the vertex shader reads " + name + " as " + TypeName(inputs[input].type) + ", but the input layout gives it format " +
                        std::to_string(static_cast<int>(element.Format));
                return std::nullopt;
            }
            matched[input] = true;
            result.attributes.push_back({inputs[input].location, element.InputSlot, *format, offset});
        }
    }
    for (std::size_t input = 0; input < inputs.size(); ++input) {
        if (!matched[input]) {
            error = "the input layout has no element for " + inputs[input].semantic + std::to_string(inputs[input].index) + ", which the vertex shader reads";
            return std::nullopt;
        }
    }
    for (UINT slot = 0; slot < kInputSlots; ++slot) {
        if (slots[slot].used) {
            const VkVertexInputRate rate = slots[slot].classification == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
            result.bindings.push_back({slot, 0, rate});
        }
    }
    std::sort(result.attributes.begin(), result.attributes.end(), [](const VkVertexInputAttributeDescription& a, const VkVertexInputAttributeDescription& b) { return a.location < b.location; });
    return result;
}

}
