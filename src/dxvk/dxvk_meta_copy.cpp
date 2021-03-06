#include "dxvk_meta_copy.h"

#include <dxvk_copy_color_1d.h>
#include <dxvk_copy_color_2d.h>
#include <dxvk_copy_color_ms.h>
#include <dxvk_copy_depth_1d.h>
#include <dxvk_copy_depth_2d.h>
#include <dxvk_copy_depth_ms.h>

#include <dxvk_resolve_vert.h>
#include <dxvk_resolve_geom.h>

namespace dxvk {

  DxvkMetaCopyRenderPass::DxvkMetaCopyRenderPass(
    const Rc<vk::DeviceFn>&   vkd,
    const Rc<DxvkImageView>&  dstImageView,
    const Rc<DxvkImageView>&  srcImageView,
          bool                discardDst)
  : m_vkd         (vkd),
    m_dstImageView(dstImageView),
    m_srcImageView(srcImageView),
    m_renderPass  (createRenderPass(discardDst)),
    m_framebuffer (createFramebuffer()) {

  }
  

  DxvkMetaCopyRenderPass::~DxvkMetaCopyRenderPass() {
    m_vkd->vkDestroyFramebuffer(m_vkd->device(), m_framebuffer, nullptr);
    m_vkd->vkDestroyRenderPass (m_vkd->device(), m_renderPass,  nullptr);
  }

  
  VkRenderPass DxvkMetaCopyRenderPass::createRenderPass(bool discard) const {
    auto aspect = m_dstImageView->info().aspect;

    std::array<VkSubpassDependency, 2> subpassDeps = {{
      { VK_SUBPASS_EXTERNAL, 0,
        m_dstImageView->imageInfo().stages,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0 },
      { 0, VK_SUBPASS_EXTERNAL,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        m_dstImageView->imageInfo().stages,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        m_dstImageView->imageInfo().access, 0 },
    }};
    
    VkAttachmentDescription attachment;
    attachment.flags            = 0;
    attachment.format           = m_dstImageView->info().format;
    attachment.samples          = m_dstImageView->imageInfo().sampleCount;
    attachment.loadOp           = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp          = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp    = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp   = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout    = m_dstImageView->imageInfo().layout;
    attachment.finalLayout      = m_dstImageView->imageInfo().layout;

    if (discard) {
      attachment.loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      attachment.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    
    VkAttachmentReference attachmentRef;
    attachmentRef.attachment    = 0;
    attachmentRef.layout        = (aspect & VK_IMAGE_ASPECT_COLOR_BIT)
      ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
      : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    
    VkSubpassDescription subpass;
    subpass.flags                   = 0;
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.inputAttachmentCount    = 0;
    subpass.pInputAttachments       = nullptr;
    subpass.colorAttachmentCount    = 0;
    subpass.pColorAttachments       = nullptr;
    subpass.pResolveAttachments     = nullptr;
    subpass.pDepthStencilAttachment = nullptr;
    subpass.preserveAttachmentCount = 0;
    subpass.pPreserveAttachments    = nullptr;

    if (aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
      subpass.colorAttachmentCount  = 1;
      subpass.pColorAttachments     = &attachmentRef;
    } else {
      subpass.pDepthStencilAttachment = &attachmentRef;
    }

    VkRenderPassCreateInfo info;
    info.sType                  = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.pNext                  = nullptr;
    info.flags                  = 0;
    info.attachmentCount        = 1;
    info.pAttachments           = &attachment;
    info.subpassCount           = 1;
    info.pSubpasses             = &subpass;
    info.dependencyCount        = subpassDeps.size();
    info.pDependencies          = subpassDeps.data();

    VkRenderPass result = VK_NULL_HANDLE;
    if (m_vkd->vkCreateRenderPass(m_vkd->device(), &info, nullptr, &result) != VK_SUCCESS)
      throw DxvkError("DxvkMetaCopyRenderPass: Failed to create render pass");
    return result;
  }


  VkFramebuffer DxvkMetaCopyRenderPass::createFramebuffer() const {
    VkImageView             dstViewHandle   = m_dstImageView->handle();
    VkImageSubresourceRange dstSubresources = m_dstImageView->subresources();
    VkExtent3D              dstExtent       = m_dstImageView->mipLevelExtent(0);

    VkFramebufferCreateInfo fboInfo;
    fboInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fboInfo.pNext           = nullptr;
    fboInfo.flags           = 0;
    fboInfo.renderPass      = m_renderPass;
    fboInfo.attachmentCount = 1;
    fboInfo.pAttachments    = &dstViewHandle;
    fboInfo.width           = dstExtent.width;
    fboInfo.height          = dstExtent.height;
    fboInfo.layers          = dstSubresources.layerCount;
    
    VkFramebuffer result = VK_NULL_HANDLE;
    if (m_vkd->vkCreateFramebuffer(m_vkd->device(), &fboInfo, nullptr, &result) != VK_SUCCESS)
      throw DxvkError("DxvkMetaMipGenRenderPass: Failed to create target framebuffer");
    return result;
  }


  DxvkMetaCopyObjects::DxvkMetaCopyObjects(const Rc<vk::DeviceFn>& vkd)
  : m_vkd         (vkd),
    m_sampler     (createSampler()),
    m_shaderVert  (createShaderModule(dxvk_resolve_vert)),
    m_shaderGeom  (createShaderModule(dxvk_resolve_geom)),
    m_color {
      createShaderModule(dxvk_copy_color_1d),
      createShaderModule(dxvk_copy_color_2d),
      createShaderModule(dxvk_copy_color_ms) },
    m_depth {
      createShaderModule(dxvk_copy_depth_1d),
      createShaderModule(dxvk_copy_depth_2d),
      createShaderModule(dxvk_copy_depth_ms) } {
    
  }


  DxvkMetaCopyObjects::~DxvkMetaCopyObjects() {
    for (const auto& pair : m_pipelines) {
      m_vkd->vkDestroyPipeline(m_vkd->device(), pair.second.pipeHandle, nullptr);
      m_vkd->vkDestroyPipelineLayout(m_vkd->device(), pair.second.pipeLayout, nullptr);
      m_vkd->vkDestroyDescriptorSetLayout (m_vkd->device(), pair.second.dsetLayout, nullptr);
      m_vkd->vkDestroyRenderPass(m_vkd->device(), pair.second.renderPass, nullptr);
    }

    m_vkd->vkDestroyShaderModule(m_vkd->device(), m_depth.fragMs, nullptr);
    m_vkd->vkDestroyShaderModule(m_vkd->device(), m_depth.frag2D, nullptr);
    m_vkd->vkDestroyShaderModule(m_vkd->device(), m_depth.frag1D, nullptr);
    m_vkd->vkDestroyShaderModule(m_vkd->device(), m_color.fragMs, nullptr);
    m_vkd->vkDestroyShaderModule(m_vkd->device(), m_color.frag2D, nullptr);
    m_vkd->vkDestroyShaderModule(m_vkd->device(), m_color.frag1D, nullptr);
    m_vkd->vkDestroyShaderModule(m_vkd->device(), m_shaderGeom,   nullptr);
    m_vkd->vkDestroyShaderModule(m_vkd->device(), m_shaderVert,   nullptr);
    
    m_vkd->vkDestroySampler(m_vkd->device(), m_sampler, nullptr);
  }


  VkFormat DxvkMetaCopyObjects::getCopyDestinationFormat(
          VkImageAspectFlags    dstAspect,
          VkImageAspectFlags    srcAspect,
          VkFormat              srcFormat) const {
    if (srcAspect == dstAspect)
      return srcFormat;
    
    if (dstAspect == VK_IMAGE_ASPECT_COLOR_BIT
     && srcAspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
      switch (srcFormat) {
        case VK_FORMAT_D16_UNORM:  return VK_FORMAT_R16_UNORM;
        case VK_FORMAT_D32_SFLOAT: return VK_FORMAT_R32_SFLOAT;
        default:                   return VK_FORMAT_UNDEFINED;
      }
    }

    if (dstAspect == VK_IMAGE_ASPECT_DEPTH_BIT
     && srcAspect == VK_IMAGE_ASPECT_COLOR_BIT) {
      switch (srcFormat) {
        case VK_FORMAT_R16_UNORM:  return VK_FORMAT_D16_UNORM;
        case VK_FORMAT_R32_SFLOAT: return VK_FORMAT_D32_SFLOAT;
        default:                   return VK_FORMAT_UNDEFINED;
      }
    }

    return VK_FORMAT_UNDEFINED;
  }


  DxvkMetaCopyPipeline DxvkMetaCopyObjects::getPipeline(
          VkImageViewType       viewType,
          VkFormat              dstFormat,
          VkSampleCountFlagBits dstSamples) {
    std::lock_guard<std::mutex> lock(m_mutex);

    DxvkMetaCopyPipelineKey key;
    key.viewType = viewType;
    key.format   = dstFormat;
    key.samples  = dstSamples;
    
    auto entry = m_pipelines.find(key);
    if (entry != m_pipelines.end())
      return entry->second;

    DxvkMetaCopyPipeline pipeline = createPipeline(key);
    m_pipelines.insert({ key, pipeline });
    return pipeline;
  }
  
  
  VkSampler DxvkMetaCopyObjects::createSampler() const {
    VkSamplerCreateInfo info;
    info.sType                  = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.pNext                  = nullptr;
    info.flags                  = 0;
    info.magFilter              = VK_FILTER_NEAREST;
    info.minFilter              = VK_FILTER_NEAREST;
    info.mipmapMode             = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU           = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV           = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW           = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.mipLodBias             = 0.0f;
    info.anisotropyEnable       = VK_FALSE;
    info.maxAnisotropy          = 1.0f;
    info.compareEnable          = VK_FALSE;
    info.compareOp              = VK_COMPARE_OP_ALWAYS;
    info.minLod                 = 0.0f;
    info.maxLod                 = 0.0f;
    info.borderColor            = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    info.unnormalizedCoordinates = VK_FALSE;
    
    VkSampler result = VK_NULL_HANDLE;
    if (m_vkd->vkCreateSampler(m_vkd->device(), &info, nullptr, &result) != VK_SUCCESS)
      throw DxvkError("DxvkMetaCopyObjects: Failed to create sampler");
    return result;
  }

  
  VkShaderModule DxvkMetaCopyObjects::createShaderModule(
    const SpirvCodeBuffer&          code) const {
    VkShaderModuleCreateInfo info;
    info.sType                  = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.pNext                  = nullptr;
    info.flags                  = 0;
    info.codeSize               = code.size();
    info.pCode                  = code.data();
    
    VkShaderModule result = VK_NULL_HANDLE;
    if (m_vkd->vkCreateShaderModule(m_vkd->device(), &info, nullptr, &result) != VK_SUCCESS)
      throw DxvkError("DxvkMetaCopyObjects: Failed to create shader module");
    return result;
  }

  
  DxvkMetaCopyPipeline DxvkMetaCopyObjects::createPipeline(
    const DxvkMetaCopyPipelineKey&  key) {
    DxvkMetaCopyPipeline pipeline;
    pipeline.renderPass = this->createRenderPass(key);
    pipeline.dsetLayout = this->createDescriptorSetLayout();
    pipeline.pipeLayout = this->createPipelineLayout(pipeline.dsetLayout);
    pipeline.pipeHandle = this->createPipelineObject(key, pipeline.pipeLayout, pipeline.renderPass);
    return pipeline;
  }


  VkRenderPass DxvkMetaCopyObjects::createRenderPass(
    const DxvkMetaCopyPipelineKey&  key) const {
    auto aspect = imageFormatInfo(key.format)->aspectMask;

    VkAttachmentDescription attachment;
    attachment.flags            = 0;
    attachment.format           = key.format;
    attachment.samples          = key.samples;
    attachment.loadOp           = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp          = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp    = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp   = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout    = VK_IMAGE_LAYOUT_GENERAL;
    attachment.finalLayout      = VK_IMAGE_LAYOUT_GENERAL;
    
    VkAttachmentReference attachmentRef;
    attachmentRef.attachment    = 0;
    attachmentRef.layout        = (aspect & VK_IMAGE_ASPECT_COLOR_BIT)
      ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
      : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    
    VkSubpassDescription subpass;
    subpass.flags                   = 0;
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.inputAttachmentCount    = 0;
    subpass.pInputAttachments       = nullptr;
    subpass.colorAttachmentCount    = 0;
    subpass.pColorAttachments       = nullptr;
    subpass.pResolveAttachments     = nullptr;
    subpass.pDepthStencilAttachment = nullptr;
    subpass.preserveAttachmentCount = 0;
    subpass.pPreserveAttachments    = nullptr;

    if (aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
      subpass.colorAttachmentCount  = 1;
      subpass.pColorAttachments     = &attachmentRef;
    } else {
      subpass.pDepthStencilAttachment = &attachmentRef;
    }

    VkRenderPassCreateInfo info;
    info.sType                  = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.pNext                  = nullptr;
    info.flags                  = 0;
    info.attachmentCount        = 1;
    info.pAttachments           = &attachment;
    info.subpassCount           = 1;
    info.pSubpasses             = &subpass;
    info.dependencyCount        = 0;
    info.pDependencies          = nullptr;

    VkRenderPass result = VK_NULL_HANDLE;
    if (m_vkd->vkCreateRenderPass(m_vkd->device(), &info, nullptr, &result) != VK_SUCCESS)
      throw DxvkError("DxvkMetaCopyObjects: Failed to create render pass");
    return result;
  }

  
  VkDescriptorSetLayout DxvkMetaCopyObjects::createDescriptorSetLayout() const {
    VkDescriptorSetLayoutBinding binding;
    binding.binding             = 0;
    binding.descriptorType      = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount     = 1;
    binding.stageFlags          = VK_SHADER_STAGE_FRAGMENT_BIT;
    binding.pImmutableSamplers  = &m_sampler;
    
    VkDescriptorSetLayoutCreateInfo info;
    info.sType                  = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.pNext                  = nullptr;
    info.flags                  = 0;
    info.bindingCount           = 1;
    info.pBindings              = &binding;
    
    VkDescriptorSetLayout result = VK_NULL_HANDLE;
    if (m_vkd->vkCreateDescriptorSetLayout(m_vkd->device(), &info, nullptr, &result) != VK_SUCCESS)
      throw DxvkError("DxvkMetaCopyObjects: Failed to create descriptor set layout");
    return result;
  }

  
  VkPipelineLayout DxvkMetaCopyObjects::createPipelineLayout(
          VkDescriptorSetLayout     descriptorSetLayout) const {
    VkPushConstantRange push;
    push.stageFlags             = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset                 = 0;
    push.size                   = sizeof(VkOffset2D);

    VkPipelineLayoutCreateInfo info;
    info.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.pNext                  = nullptr;
    info.flags                  = 0;
    info.setLayoutCount         = 1;
    info.pSetLayouts            = &descriptorSetLayout;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges    = &push;
    
    VkPipelineLayout result = VK_NULL_HANDLE;
    if (m_vkd->vkCreatePipelineLayout(m_vkd->device(), &info, nullptr, &result) != VK_SUCCESS)
      throw DxvkError("DxvkMetaCopyObjects: Failed to create pipeline layout");
    return result;
  }
  

  VkPipeline DxvkMetaCopyObjects::createPipelineObject(
    const DxvkMetaCopyPipelineKey&  key,
          VkPipelineLayout          pipelineLayout,
          VkRenderPass              renderPass) {
    auto aspect = imageFormatInfo(key.format)->aspectMask;

    std::array<VkPipelineShaderStageCreateInfo, 3> stages;
    
    VkPipelineShaderStageCreateInfo& vsStage = stages[0];
    vsStage.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vsStage.pNext               = nullptr;
    vsStage.flags               = 0;
    vsStage.stage               = VK_SHADER_STAGE_VERTEX_BIT;
    vsStage.module              = m_shaderVert;
    vsStage.pName               = "main";
    vsStage.pSpecializationInfo = nullptr;
    
    VkPipelineShaderStageCreateInfo& gsStage = stages[1];
    gsStage.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    gsStage.pNext               = nullptr;
    gsStage.flags               = 0;
    gsStage.stage               = VK_SHADER_STAGE_GEOMETRY_BIT;
    gsStage.module              = m_shaderGeom;
    gsStage.pName               = "main";
    gsStage.pSpecializationInfo = nullptr;
    
    VkPipelineShaderStageCreateInfo& psStage = stages[2];
    psStage.sType               = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    psStage.pNext               = nullptr;
    psStage.flags               = 0;
    psStage.stage               = VK_SHADER_STAGE_FRAGMENT_BIT;
    psStage.module              = VK_NULL_HANDLE;
    psStage.pName               = "main";
    psStage.pSpecializationInfo = nullptr;

    auto shaderSet = (aspect & VK_IMAGE_ASPECT_COLOR_BIT) ? &m_color : &m_depth;
    
    if (key.viewType == VK_IMAGE_VIEW_TYPE_1D)
      psStage.module = shaderSet->frag1D;
    else if (key.samples == VK_SAMPLE_COUNT_1_BIT)
      psStage.module = shaderSet->frag2D;
    else
      psStage.module = shaderSet->fragMs;
    
    std::array<VkDynamicState, 2> dynStates = {{
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
    }};
    
    VkPipelineDynamicStateCreateInfo dynState;
    dynState.sType              = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynState.pNext              = nullptr;
    dynState.flags              = 0;
    dynState.dynamicStateCount  = dynStates.size();
    dynState.pDynamicStates     = dynStates.data();
    
    VkPipelineVertexInputStateCreateInfo viState;
    viState.sType               = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    viState.pNext               = nullptr;
    viState.flags               = 0;
    viState.vertexBindingDescriptionCount   = 0;
    viState.pVertexBindingDescriptions      = nullptr;
    viState.vertexAttributeDescriptionCount = 0;
    viState.pVertexAttributeDescriptions    = nullptr;
    
    VkPipelineInputAssemblyStateCreateInfo iaState;
    iaState.sType               = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    iaState.pNext               = nullptr;
    iaState.flags               = 0;
    iaState.topology            = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    iaState.primitiveRestartEnable = VK_FALSE;
    
    VkPipelineViewportStateCreateInfo vpState;
    vpState.sType               = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vpState.pNext               = nullptr;
    vpState.flags               = 0;
    vpState.viewportCount       = 1;
    vpState.pViewports          = nullptr;
    vpState.scissorCount        = 1;
    vpState.pScissors           = nullptr;
    
    VkPipelineRasterizationStateCreateInfo rsState;
    rsState.sType               = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rsState.pNext               = nullptr;
    rsState.flags               = 0;
    rsState.depthClampEnable    = VK_TRUE;
    rsState.rasterizerDiscardEnable = VK_FALSE;
    rsState.polygonMode         = VK_POLYGON_MODE_FILL;
    rsState.cullMode            = VK_CULL_MODE_NONE;
    rsState.frontFace           = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rsState.depthBiasEnable     = VK_FALSE;
    rsState.depthBiasConstantFactor = 0.0f;
    rsState.depthBiasClamp          = 0.0f;
    rsState.depthBiasSlopeFactor    = 0.0f;
    rsState.lineWidth           = 1.0f;
    
    uint32_t msMask = 0xFFFFFFFF;
    VkPipelineMultisampleStateCreateInfo msState;
    msState.sType                 = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msState.pNext                 = nullptr;
    msState.flags                 = 0;
    msState.rasterizationSamples  = key.samples;
    msState.sampleShadingEnable   = key.samples != VK_SAMPLE_COUNT_1_BIT;
    msState.minSampleShading      = 1.0f;
    msState.pSampleMask           = &msMask;
    msState.alphaToCoverageEnable = VK_FALSE;
    msState.alphaToOneEnable      = VK_FALSE;
    
    VkPipelineColorBlendAttachmentState cbAttachment;
    cbAttachment.blendEnable         = VK_FALSE;
    cbAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cbAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    cbAttachment.colorBlendOp        = VK_BLEND_OP_ADD;
    cbAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cbAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cbAttachment.alphaBlendOp        = VK_BLEND_OP_ADD;
    cbAttachment.colorWriteMask      =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    
    VkPipelineColorBlendStateCreateInfo cbState;
    cbState.sType               = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cbState.pNext               = nullptr;
    cbState.flags               = 0;
    cbState.logicOpEnable       = VK_FALSE;
    cbState.logicOp             = VK_LOGIC_OP_NO_OP;
    cbState.attachmentCount     = 1;
    cbState.pAttachments        = &cbAttachment;
    
    for (uint32_t i = 0; i < 4; i++)
      cbState.blendConstants[i] = 0.0f;
    
    VkStencilOpState stencilOp;
    stencilOp.failOp            = VK_STENCIL_OP_KEEP;
    stencilOp.passOp            = VK_STENCIL_OP_KEEP;
    stencilOp.depthFailOp       = VK_STENCIL_OP_KEEP;
    stencilOp.compareOp         = VK_COMPARE_OP_ALWAYS;
    stencilOp.compareMask       = 0xFFFFFFFF;
    stencilOp.writeMask         = 0xFFFFFFFF;
    stencilOp.reference         = 0;
    
    VkPipelineDepthStencilStateCreateInfo dsState;
    dsState.sType               = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    dsState.pNext               = nullptr;
    dsState.flags               = 0;
    dsState.depthTestEnable     = VK_TRUE;
    dsState.depthWriteEnable    = VK_TRUE;
    dsState.depthCompareOp      = VK_COMPARE_OP_ALWAYS;
    dsState.depthBoundsTestEnable = VK_FALSE;
    dsState.stencilTestEnable   = VK_FALSE;
    dsState.front               = stencilOp;
    dsState.back                = stencilOp;
    dsState.minDepthBounds      = 0.0f;
    dsState.maxDepthBounds      = 1.0f;
    
    VkGraphicsPipelineCreateInfo info;
    info.sType                  = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext                  = nullptr;
    info.flags                  = 0;
    info.stageCount             = stages.size();
    info.pStages                = stages.data();
    info.pVertexInputState      = &viState;
    info.pInputAssemblyState    = &iaState;
    info.pTessellationState     = nullptr;
    info.pViewportState         = &vpState;
    info.pRasterizationState    = &rsState;
    info.pMultisampleState      = &msState;
    info.pColorBlendState       = (aspect & VK_IMAGE_ASPECT_COLOR_BIT) ? &cbState : nullptr;
    info.pDepthStencilState     = (aspect & VK_IMAGE_ASPECT_COLOR_BIT) ? nullptr : &dsState;
    info.pDynamicState          = &dynState;
    info.layout                 = pipelineLayout;
    info.renderPass             = renderPass;
    info.subpass                = 0;
    info.basePipelineHandle     = VK_NULL_HANDLE;
    info.basePipelineIndex      = -1;
    
    VkPipeline result = VK_NULL_HANDLE;
    if (m_vkd->vkCreateGraphicsPipelines(m_vkd->device(), VK_NULL_HANDLE, 1, &info, nullptr, &result) != VK_SUCCESS)
      throw DxvkError("DxvkMetaCopyObjects: Failed to create graphics pipeline");
    return result;
  }
  
}