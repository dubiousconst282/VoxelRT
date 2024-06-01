#include "Havk.h"

#include <unordered_map>

#include <slang.h>
#include <slang-com-ptr.h>

namespace havk {

void Pipeline::Bind(CommandList& cmdList) {
    if (cmdList.BoundPipeline_ == Handle) return;
    cmdList.BoundPipeline_ = Handle;

    auto bindPt = dynamic_cast<GraphicsPipeline*>(this) ? VK_PIPELINE_BIND_POINT_GRAPHICS : VK_PIPELINE_BIND_POINT_COMPUTE;
    vkCmdBindPipeline(cmdList.Buffer, bindPt, Handle);
    vkCmdBindDescriptorSets(cmdList.Buffer, bindPt, LayoutHandle, 0, 1, &Context->DescriptorHeap->Set, 0, nullptr);
}
Pipeline::~Pipeline() {
    vkDestroyPipelineLayout(Context->Device, LayoutHandle, nullptr);
    vkDestroyPipeline(Context->Device, Handle, nullptr);
}

PipelineBuilder::PipelineBuilder(DeviceContext* ctx, const std::filesystem::path& basePath, bool enableHotReload) {
    Context = ctx;
    BasePath = basePath;

    if (enableHotReload) {
        _watcher = std::make_unique<FileWatcher>(BasePath);
    }
    slang::createGlobalSession((slang::IGlobalSession**)&_slangSession);
}

PipelineBuilder::~PipelineBuilder() {
    ((slang::IGlobalSession*)_slangSession)->Release();
}

GraphicsPipelinePtr PipelineBuilder::CreateGraphics(const std::string& shaderFilename, const GraphicsPipelineDesc& desc,
                                                    const ShaderCompileParams& compilePars) {
    ShaderCompileResult shader = Compile(shaderFilename, compilePars);
    if (!shader.InfoLog.empty()) {
        Context->Log(shader.Success ? LogLevel::Debug : LogLevel::Error, "Compile log for shader '%s':\n'%s'", shaderFilename.data(), shader.InfoLog.data());
    }
    auto pipe = Resource::make<GraphicsPipeline>(Context);

    pipe->LayoutHandle = shader.Layout;
    shader.Layout = nullptr;  // take ownership

    // Vertex input should be implemented via vertex pulling - death to fixed function pipeline.
    VkPipelineVertexInputStateCreateInfo vertexInputCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 0,
        .pVertexBindingDescriptions = nullptr,
        .vertexAttributeDescriptionCount = 0,
        .pVertexAttributeDescriptions = nullptr,
    };
    VkPipelineInputAssemblyStateCreateInfo inputAssemblyCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = desc.Topology,
        .primitiveRestartEnable = desc.EnablePrimitiveRestart,
    };
    VkPipelineViewportStateCreateInfo viewportCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo rasterizerCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = desc.EnableDepthClamp,
        .polygonMode = desc.PolygonMode,
        .cullMode = desc.CullMode,
        .frontFace = desc.FrontFace,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisampleCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = desc.RasterizationSamples,
        .sampleShadingEnable = desc.EnableSampleShading,
    };
    VkPipelineDepthStencilStateCreateInfo depthStencilCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = desc.EnableDepthTest,
        .depthWriteEnable = desc.EnableDepthWrite,
        .depthCompareOp = desc.DepthCompareOp,
        .stencilTestEnable = desc.EnableStencilTest,
        .front = desc.StencilFront,
        .back = desc.StencilBack,
    };

    std::vector<VkPipelineColorBlendAttachmentState> blendStates = desc.BlendStates;
    if (blendStates.size() == 0) {
        for (uint32_t i = 0; i < desc.OutputFormats.size(); i++) {
            blendStates.push_back(BlendingModes::None);
        }
    }
    VkPipelineColorBlendStateCreateInfo blendCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = desc.EnableColorLogicOp,
        .logicOp = desc.ColorLogicOp,
        .attachmentCount = (uint32_t)blendStates.size(),
        .pAttachments = blendStates.data(),
    };
    std::memcpy(blendCI.blendConstants, desc.BlendConstants, sizeof(desc.BlendConstants));

    VkDynamicState dynamicStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicStateCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = std::size(dynamicStates),
        .pDynamicStates = dynamicStates,
    };

    const VkPipelineRenderingCreateInfo renderingCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = (uint32_t)desc.OutputFormats.size(),
        .pColorAttachmentFormats = desc.OutputFormats.data(),
        .depthAttachmentFormat = desc.DepthFormat,
        .stencilAttachmentFormat = desc.StencilFormat,
    };

    VkGraphicsPipelineCreateInfo pipelineCI = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &renderingCI,
        .stageCount = (uint32_t)shader.Stages.size(),
        .pStages = shader.Stages.data(),
        .pVertexInputState = &vertexInputCI,
        .pInputAssemblyState = &inputAssemblyCI,
        .pViewportState = &viewportCI,
        .pRasterizationState = &rasterizerCI,
        .pMultisampleState = &multisampleCI,
        .pDepthStencilState = &depthStencilCI,
        .pColorBlendState = &blendCI,
        .pDynamicState = &dynamicStateCI,
        .layout = pipe->LayoutHandle,
    };
    VK_CHECK(vkCreateGraphicsPipelines(Context->Device, Cache, 1, &pipelineCI, nullptr, &pipe->Handle));

    return pipe;
}

ComputePipelinePtr PipelineBuilder::CreateCompute(const std::string& shaderFilename, const ShaderCompileParams& compilePars) {
    ShaderCompileResult shader = Compile(shaderFilename, compilePars);
    if (!shader.InfoLog.empty()) {
        Context->Log(shader.Success ? LogLevel::Debug : LogLevel::Error, "Compile log for shader '%s':\n'%s'", shaderFilename.data(), shader.InfoLog.data());
    }
    if (shader.Stages.size() != 1) {
        throw std::runtime_error("Compute shader must have exactly one entry point.");
    }
    auto pipe = Resource::make<ComputePipeline>(Context);

    pipe->LayoutHandle = shader.Layout;
    shader.Layout = nullptr;  // take ownership

    VkComputePipelineCreateInfo pipelineCI = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = shader.Stages[0],
        .layout = pipe->LayoutHandle,
    };
    VK_CHECK(vkCreateComputePipelines(Context->Device, Cache, 1, &pipelineCI, nullptr, &pipe->Handle));

    return pipe;
}

void PipelineBuilder::Refresh() {
    if (_watcher == nullptr) return;

    std::vector<std::filesystem::path> changedFiles;
    _watcher->PollChanges(changedFiles);

    for (auto& filePath : changedFiles) {
        for (auto& pipeline : _trackedPipelines) {
            if (pipeline.IncludedFiles.contains(filePath.string())) {
                // Recompile(shader);
            }
        }
    }
}

static VkShaderStageFlagBits GetVkStage(SlangStage stage);
static VkDescriptorType GetDescriptorType(SlangTypeKind kind);
static void ParseConstSampler(slang::UserAttribute* attr, VkSamplerCreateInfo* info);

ShaderCompileResult PipelineBuilder::Compile(std::string_view filename, const ShaderCompileParams& pars) {
    Context->Log(LogLevel::Debug, "Begin compile shader '%s'", filename.data());
    ShaderCompileResult result(Context->Device);
    result.Filename = filename;

    auto globalSession = (slang::IGlobalSession*)_slangSession;

    std::vector<slang::CompilerOptionEntry> opts;
    opts.push_back({ slang::CompilerOptionName::DebugInformation, { .intValue0 = SLANG_DEBUG_INFO_LEVEL_MAXIMAL } });
    opts.push_back({ slang::CompilerOptionName::Capability, { .intValue0 =  globalSession->findCapability("spirv_1_6") } });

    std::string basePath = BasePath.string();
    const char* searchPaths[] = { basePath.data() };

    std::vector<slang::PreprocessorMacroDesc> prepMacros;
    for (auto& def : pars.PrepDefs) {
        prepMacros.push_back({ .name = def.first.data(), .value = def.second.data() });
    }

    slang::TargetDesc targetDesc = {
        .format = SLANG_SPIRV,
        .profile = globalSession->findProfile("glsl_460"),
        .flags = SLANG_TARGET_FLAG_GENERATE_SPIRV_DIRECTLY,
        .forceGLSLScalarBufferLayout = true,
        .compilerOptionEntries = opts.data(),
        .compilerOptionEntryCount = (uint32_t)opts.size(),
    };
    slang::SessionDesc sessionDesc = {
        .targets = &targetDesc,
        .targetCount = 1,
        .searchPaths = searchPaths,
        .searchPathCount = 1,
        .preprocessorMacros = prepMacros.data(),
        .preprocessorMacroCount = (uint32_t)prepMacros.size(),
        .compilerOptionEntries = opts.data(),
        .compilerOptionEntryCount = (uint32_t)opts.size(),
    };

    // Parsing
    Slang::ComPtr<slang::ISession> session;
    globalSession->createSession(sessionDesc, session.writeRef());

    Slang::ComPtr<slang::IBlob> diagnostics;
    slang::IModule* module = session->loadModule(filename.data(), diagnostics.writeRef());

    if (diagnostics) {
        result.AppendLog(std::string_view((const char*)diagnostics->getBufferPointer(), diagnostics->getBufferSize()));
    }
    if (!module) {
        return result;
    }

    // Compositing
    std::vector<slang::IComponentType*> components = { module };
    std::vector<Slang::ComPtr<slang::IEntryPoint>> entryPoints;

    for (int32_t i = 0; i < module->getDefinedEntryPointCount(); i++) {
        module->getDefinedEntryPoint(i, entryPoints.emplace_back().writeRef());
        components.push_back(entryPoints.back());
    }
    Slang::ComPtr<slang::IComponentType> program;
    session->createCompositeComponentType(components.data(), (SlangInt)components.size(), program.writeRef(), diagnostics.writeRef());

    if (diagnostics) {
        result.AppendLog(std::string_view((const char*)diagnostics->getBufferPointer(), diagnostics->getBufferSize()));
    }
    if (!program) {
        return result;
    }

    // Linking
    // TODO: support for https://shader-slang.com/slang/user-guide/link-time-specialization
    Slang::ComPtr<slang::IComponentType> linkedProgram;
    Slang::ComPtr<ISlangBlob> diagnosticBlob;
    program->link(linkedProgram.writeRef(), diagnosticBlob.writeRef());

    slang::ProgramLayout* layout = linkedProgram->getLayout();

    for (uint32_t i = 0; i < layout->getEntryPointCount(); i++) {
        // Would be nicer to GENERATE_WHOLE_PROGRAM here but that seems to be not yet supported by the ISession API:
        // https://github.com/shader-slang/slang/issues/3974
        Slang::ComPtr<slang::IBlob> kernelBlob;
        linkedProgram->getEntryPointCode(i, 0, kernelBlob.writeRef(), diagnostics.writeRef());

        if (diagnostics) {
            result.AppendLog(std::string_view((const char*)diagnostics->getBufferPointer(), diagnostics->getBufferSize()));
        }
        if (!kernelBlob) {
            return result;
        }

        slang::EntryPointReflection* entryReflect = layout->getEntryPointByIndex(i);

        // Create vulkan module
        VkShaderModuleCreateInfo moduleCI = {
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
            .codeSize = (uint32_t)kernelBlob->getBufferSize(),
            .pCode = (uint32_t*)kernelBlob->getBufferPointer(),
        };

        VkShaderModule vkModule;
        VK_CHECK(vkCreateShaderModule(Context->Device, &moduleCI, nullptr, &vkModule));

        result.Stages.push_back({
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = GetVkStage(entryReflect->getStage()),
            .module = vkModule,
            .pName = "main",  // strdup(entryReflect->getName()),
        });
    }

    VkPushConstantRange pcRange = { .stageFlags = VK_SHADER_STAGE_ALL, .offset = 0, .size = 0 };
    std::vector<VkSampler> samplers;
    std::vector<VkDescriptorSetLayoutBinding> samplerBindings;

    for (uint32_t j = 0; j < layout->getParameterCount(); j++) {
        slang::VariableLayoutReflection* par = layout->getParameterByIndex(j);
        slang::TypeLayoutReflection* typeLayout = par->getTypeLayout();
        auto cat = par->getCategory();

        switch (par->getCategory()) {
            case slang::ParameterCategory::PushConstantBuffer: {
                if (pcRange.size != 0) {
                    result.AppendLog("Error: only a single push constant parameter is supported.");
                    return result;
                }
                pcRange.size = typeLayout->getElementTypeLayout()->getSize();
                Context->Log(LogLevel::Trace, "PC %s %d bytes", par->getName(), pcRange.size);
                break;
            }
            case slang::ParameterCategory::DescriptorTableSlot: {
                uint32_t index = par->getBindingIndex();
                uint32_t space = par->getBindingSpace() + par->getOffset(SLANG_PARAMETER_CATEGORY_SUB_ELEMENT_REGISTER_SPACE);

                Context->Log(LogLevel::Trace, "Par %s %d: %d-%d", par->getName(), typeLayout->getKind(), index, space);

                if (typeLayout->getKind() == slang::TypeReflection::Kind::SamplerState) {
                   // assert(space == 1);
                    VkSamplerCreateInfo info = {
                        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                        .magFilter = VK_FILTER_LINEAR,
                        .minFilter = VK_FILTER_LINEAR,
                        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                        .minLod = 0.0f,
                        .maxLod = VK_LOD_CLAMP_NONE,
                    };
                    slang::UserAttribute* descAttr = par->getVariable()->findUserAttributeByName(globalSession, "SamplerDesc");
                    if (descAttr) {
                        ParseConstSampler(descAttr, &info);
                    }

                    VkSampler& sampler = samplers.emplace_back(Context->DescriptorHeap->GetSampler(info));
                    samplerBindings.push_back({
                        .binding = index,
                        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                        .descriptorCount = 1,
                        .stageFlags = VK_SHADER_STAGE_ALL,
                        .pImmutableSamplers = &sampler,
                    });
                    break;
                } else if (memcmp(par->getName(), "havk__", 6) == 0) {
                    assert(space == 0);
                    break;
                }
                [[fallthrough]];
            }
            default: {
                result.AppendLog(std::string("warning: unhandled shader parameter '") + par->getName() + "'");
                break;
            }
        }
    }

    std::vector<VkDescriptorSetLayout> descLayouts;
    descLayouts.push_back(Context->DescriptorHeap->SetLayout);

    VkDescriptorSetLayoutCreateInfo samplerLayoutCI = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = (uint32_t)samplerBindings.size(),
        .pBindings = samplerBindings.data(),
    };
    // VK_CHECK(vkCreateDescriptorSetLayout(Context->Device, &samplerLayoutCI, nullptr, &descLayouts.emplace_back()));

    VkPipelineLayoutCreateInfo layoutCI = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = (uint32_t)descLayouts.size(),
        .pSetLayouts = descLayouts.data(),
        .pushConstantRangeCount = pcRange.size != 0 ? 1u : 0,
        .pPushConstantRanges = &pcRange,
    };
    VK_CHECK(vkCreatePipelineLayout(Context->Device, &layoutCI, nullptr, &result.Layout));

    if (_watcher != nullptr) {
        for (int32_t i = 0; i < session->getLoadedModuleCount(); i++) {
            slang::IModule* loadedModule = session->getLoadedModule(i);
            Context->Log(LogLevel::Trace, "Shader '%s' loads '%s'", filename.data(), loadedModule->getFilePath());
        }
    }
    // vkDestroyDescriptorSetLayout(Context->Device, descLayouts.front(), nullptr);

    result.Success = true;
    return result;
}
ShaderCompileResult::~ShaderCompileResult() {
    for (auto& stage : Stages) {
        vkDestroyShaderModule(Device, stage.module, nullptr);
        // free((void*)stage.pName);  // from strdup()
    }
    if (Layout != nullptr) {
        vkDestroyPipelineLayout(Device, Layout, nullptr);
    }
}

static void ParseConstSampler(slang::UserAttribute* attr, VkSamplerCreateInfo* info) {
    int magFilter, minFilter, mipFilter;
    int wrap, compareOp;
    // Can't get enum value out of UserAttribute yet: 
    attr->getArgumentValueInt(0, &magFilter);
    attr->getArgumentValueInt(1, &minFilter);
    attr->getArgumentValueInt(2, &mipFilter);
    attr->getArgumentValueInt(3, &wrap);
    attr->getArgumentValueFloat(4, &info->maxAnisotropy);
    attr->getArgumentValueInt(5, &compareOp);

    if (info->maxAnisotropy >= 1.0f) {
        info->anisotropyEnable = VK_TRUE;
    }

    const VkFilter filters[] = {
        VK_FILTER_NEAREST,
        VK_FILTER_LINEAR,
    };
    const VkSamplerAddressMode wrapModes[] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
    };
    const VkCompareOp compareOps[] = {
        VK_COMPARE_OP_NEVER,         VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_NOT_EQUAL,
        VK_COMPARE_OP_GREATER,       VK_COMPARE_OP_LESS,  VK_COMPARE_OP_GREATER_OR_EQUAL,
        VK_COMPARE_OP_LESS_OR_EQUAL,
    };

    info->addressModeU = wrapModes[wrap];
    info->addressModeV = wrapModes[wrap];
    info->addressModeW = wrapModes[wrap];

    info->magFilter = filters[magFilter];
    info->minFilter = filters[minFilter];
    info->mipmapMode = (VkSamplerMipmapMode)filters[mipFilter];

    info->compareOp = compareOps[compareOp];
    info->compareEnable = compareOp != 0;
}

static VkShaderStageFlagBits GetVkStage(SlangStage stage) {
    switch (stage) {
        case SLANG_STAGE_VERTEX: return VK_SHADER_STAGE_VERTEX_BIT;
        case SLANG_STAGE_GEOMETRY: return VK_SHADER_STAGE_GEOMETRY_BIT;
        case SLANG_STAGE_FRAGMENT: return VK_SHADER_STAGE_FRAGMENT_BIT;
        case SLANG_STAGE_COMPUTE: return VK_SHADER_STAGE_COMPUTE_BIT;
        case SLANG_STAGE_RAY_GENERATION: return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
        case SLANG_STAGE_INTERSECTION: return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
        case SLANG_STAGE_ANY_HIT: return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
        case SLANG_STAGE_CLOSEST_HIT: return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
        case SLANG_STAGE_MISS: return VK_SHADER_STAGE_MISS_BIT_KHR;
        case SLANG_STAGE_CALLABLE: return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
        case SLANG_STAGE_MESH: return VK_SHADER_STAGE_MESH_BIT_EXT;
        default:
            throw std::runtime_error("Unsupported shader stage");
            // case SLANG_STAGE_HULL: return VK_SHADER_STAGE_HULL;
            // case SLANG_STAGE_DOMAIN: return VK_SHADER_STAGE_DOMAIN_BIT;
            // case SLANG_STAGE_AMPLIFICATION: return VK_SHADER_STAGE_;
    }
}
static VkDescriptorType GetDescriptorType(SlangTypeKind kind) {
    switch (kind) {
        case SLANG_TYPE_KIND_CONSTANT_BUFFER: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        case SLANG_TYPE_KIND_SAMPLER_STATE: return VK_DESCRIPTOR_TYPE_SAMPLER;
        case SLANG_TYPE_KIND_SHADER_STORAGE_BUFFER: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        default: throw std::runtime_error("Unsupported binding type");
    }
}

};  // namespace havk