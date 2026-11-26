// NVN masked color and stencil clears on Vulkan.
#include <tuple>

#include "nvn_core.h"

namespace nvn {
namespace {

const uint32_t kVertex[] =
#include "clear_vert.inc"
;
const uint32_t kFloatFragment[] =
#include "clear_vec4.inc"
;
const uint32_t kSignedFragment[] =
#include "clear_ivec4.inc"
;
const uint32_t kUnsignedFragment[] =
#include "clear_uvec4.inc"
;

using ClearKey = std::tuple<VkFormat, VkSampleCountFlagBits, VkColorComponentFlags, ClearKind>;
std::map<ClearKey, VkPipeline> g_clearPipelines;
VkPipelineLayout g_clearLayout = VK_NULL_HANDLE;
VkShaderModule g_vertex = VK_NULL_HANDLE;
VkShaderModule g_fragments[3] = {};

template <size_t N>
VkShaderModule Load(const uint32_t (&code)[N]) {
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = sizeof code;
  info.pCode = code;
  VkShaderModule shader = VK_NULL_HANDLE;
  return g_vk.f.vkCreateShaderModule(g_vk.dev, &info, nullptr, &shader) == VK_SUCCESS ? shader : VK_NULL_HANDLE;
}

}  // namespace

VkPipeline GetMaskedClearPipeline(VkFormat format, VkSampleCountFlagBits samples, VkColorComponentFlags mask,
                                  ClearKind kind, VkPipelineLayout& layout) {
  auto& vk = g_vk.f;
  const bool stencil = kind == ClearKind::Stencil;
  const ClearKey key{format, samples, stencil ? 0u : mask, kind};
  auto existing = g_clearPipelines.find(key);
  if (existing != g_clearPipelines.end()) {
    layout = g_clearLayout;
    return existing->second;
  }
  if (!g_clearLayout) {
    const VkPushConstantRange value{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &value;
    if (vk.vkCreatePipelineLayout(g_vk.dev, &info, nullptr, &g_clearLayout) != VK_SUCCESS) return VK_NULL_HANDLE;
  }
  layout = g_clearLayout;
  if (!g_vertex) g_vertex = Load(kVertex);
  VkShaderModule fragment = VK_NULL_HANDLE;
  if (!stencil) {
    auto& cached = g_fragments[(int)kind];
    if (!cached) {
      switch (kind) {
        case ClearKind::Float: cached = Load(kFloatFragment); break;
        case ClearKind::Signed: cached = Load(kSignedFragment); break;
        case ClearKind::Unsigned: cached = Load(kUnsignedFragment); break;
        case ClearKind::Stencil: break;
      }
    }
    fragment = cached;
  }
  if (!g_vertex || (!stencil && !fragment)) return VK_NULL_HANDLE;

  const VkPipelineShaderStageCreateInfo stages[] = {
    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, g_vertex, "main"},
    {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main"},
  };
  const VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = samples;
  VkPipelineColorBlendAttachmentState attachment{};
  attachment.colorWriteMask = mask;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = 1;
  blend.pAttachments = &attachment;
  VkPipelineDepthStencilStateCreateInfo depthStencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  depthStencil.stencilTestEnable = VK_TRUE;
  depthStencil.front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xff, 0xff, 0};
  depthStencil.back = depthStencil.front;
  const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                          VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK};
  VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = stencil ? 4 : 2;
  dynamic.pDynamicStates = dynamicStates;
  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  if (stencil) rendering.stencilAttachmentFormat = format;
  else {
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
  }
  VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pipeline.pNext = &rendering;
  pipeline.stageCount = stencil ? 1 : 2;
  pipeline.pStages = stages;
  pipeline.pVertexInputState = &input;
  pipeline.pInputAssemblyState = &assembly;
  pipeline.pViewportState = &viewport;
  pipeline.pRasterizationState = &raster;
  pipeline.pMultisampleState = &multisample;
  if (stencil) pipeline.pDepthStencilState = &depthStencil;
  else pipeline.pColorBlendState = &blend;
  pipeline.pDynamicState = &dynamic;
  pipeline.layout = layout;
  VkPipeline result = VK_NULL_HANDLE;
  if (vk.vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pipeline, nullptr, &result) != VK_SUCCESS) return VK_NULL_HANDLE;
  g_clearPipelines.emplace(key, result);
  return result;
}

void DestroyMaskedClearPipelines() {
  auto& vk = g_vk.f;
  for (const auto& entry : g_clearPipelines) vk.vkDestroyPipeline(g_vk.dev, entry.second, nullptr);
  g_clearPipelines.clear();
  if (g_clearLayout) vk.vkDestroyPipelineLayout(g_vk.dev, g_clearLayout, nullptr);
  g_clearLayout = VK_NULL_HANDLE;
  if (g_vertex) vk.vkDestroyShaderModule(g_vk.dev, g_vertex, nullptr);
  g_vertex = VK_NULL_HANDLE;
  for (auto& fragment : g_fragments) {
    if (fragment) vk.vkDestroyShaderModule(g_vk.dev, fragment, nullptr);
    fragment = VK_NULL_HANDLE;
  }
}

}  // namespace nvn
