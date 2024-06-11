#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <filesystem>
#include <vector>
#include <source_location>

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <vk_mem_alloc.h>

namespace havk {

struct DeviceContext;
struct Swapchain;
struct PipelineBuilder;
struct CommandList;
struct Resource;
struct BufferDesc;
struct ImageDesc;

// Internal
struct DescriptorHeap;
struct SamplerDescriptorPool;
struct FileWatcher;

#define HAVK_NON_COPYABLE(name)            \
    name(const name&) = delete;            \
    name& operator=(const name&) = delete; \
    name(name&&) = default;                \
    name& operator=(name&&) = default;

struct ResourceDeleter {
    void operator()(Resource* r);
};

#define HAVK_FWD_RESOURCE_DEF(name) \
    struct name;                   \
    using name##Ptr = std::unique_ptr<name, ResourceDeleter>;

HAVK_FWD_RESOURCE_DEF(Pipeline);
HAVK_FWD_RESOURCE_DEF(GraphicsPipeline);
HAVK_FWD_RESOURCE_DEF(ComputePipeline);
HAVK_FWD_RESOURCE_DEF(Buffer);
HAVK_FWD_RESOURCE_DEF(Image);
using DeviceContextPtr = std::unique_ptr<DeviceContext>;

#undef HAVK_FWD_RESOURCE_DEF


// Managed resource with queued deletion.
struct Resource {
    HAVK_NON_COPYABLE(Resource);

    DeviceContext* Context;
    uint64_t LastUseTimestamp;

    Resource() = default;
    virtual ~Resource() {}
    virtual void Release();

    template<typename R, typename... Args>
    static auto make(DeviceContext* ctx, Args&&... args) {
        R* ptr = new R(std::forward<Args>(args)...);
        ptr->Context = ctx;
        return std::unique_ptr<R, ResourceDeleter>(ptr);
    }
};
inline void ResourceDeleter::operator()(Resource* r) { r->Release(); }

// A token that represents the asynchronous completion of a command list submission.
struct Future {
    HAVK_NON_COPYABLE(Future);

    DeviceContext* Context;
    uint64_t Timestamp;

    Future(DeviceContext* ctx, uint64_t ts) : Context(ctx), Timestamp(ts) {}

    void Wait(uint64_t timeoutNs = UINT64_MAX) const;
    bool Poll() const;
};

struct DeviceCreateParams {
    GLFWwindow* Window = nullptr;
    std::vector<const char*> RequiredInstanceExtensions;
    std::vector<const char*> RequiredDeviceExtensions;
    VkPhysicalDeviceFeatures RequiredFeatures;

    std::filesystem::path ShaderBasePath = "assets/shaders/";
    bool EnableShaderHotReload = true;

    bool EnableDebugLayers = true;
};

struct DeviceInfo {
    VkPhysicalDevice Handle = nullptr;
    VkPhysicalDeviceProperties Props;
    VkPhysicalDeviceFeatures Features;

    uint32_t MainQueueIdx = ~0u;
};
enum class LogLevel { Trace, Debug, Info, Warn, Error };

DeviceContextPtr Create(DeviceCreateParams pars);

struct DeviceContext {
    HAVK_NON_COPYABLE(DeviceContext);

    VkInstance Instance = nullptr;
    VkDevice Device = nullptr;
    VkQueue MainQueue = nullptr;
    VmaAllocator Allocator = nullptr;
    VkCommandPool CmdPool = nullptr;
    DeviceInfo PhysicalDeviceInfo;

    std::unique_ptr<Swapchain> Swapchain;
    std::unique_ptr<PipelineBuilder> PipeBuilder;

    std::unique_ptr<DescriptorHeap> DescriptorHeap;
    std::unique_ptr<SamplerDescriptorPool> SamplerDescPool;

    VkSemaphore QueueSemaphore;    // Timeline semaphore for MainQueue submissions
    uint64_t NextQueueTimestamp;   // MainQueue submission counter

    DeviceContext() = default;
    ~DeviceContext();

    BufferPtr CreateBuffer(const BufferDesc& desc);
    ImagePtr CreateImage(const ImageDesc& desc);

    // Executes commands recorded by the callback.
    Future Submit(std::function<void(CommandList)> cb);
    Future Submit(VkCommandBuffer cmdBuffer, VkSemaphore waitSemaphore, VkPipelineStageFlags waitMask, VkSemaphore signalSemaphore,
                  VkFence fence);

    void WaitDeviceIdle() { vkDeviceWaitIdle(Device); };

    // Returns timestamp of last completed main queue sumission.
    uint64_t GetQueueTimestamp() const;

    // Call this method periodically to flush the deletion queue and refresh shaders.
    void Tick();

    void EnqueueDeletion(Resource* ptr);

    void Log(LogLevel level, const char* message, ...);

private:
    friend DeviceContextPtr Create(DeviceCreateParams pars);

    VkDebugUtilsMessengerEXT _debugMessenger = nullptr;
    std::vector<Resource*> _deletionQueue;  // Unused resources to be deleted
    uint64_t _prevTickQueueTimestamp = 0;
};

using ImageHandle = uint32_t;
static inline constexpr uint32_t InvalidHandle = ~0u;


struct BufferDesc {
    uint64_t Size;
    VkBufferUsageFlags Usage;
    VmaAllocationCreateFlags VmaFlags = 0; // HOST_ACCESS_* implies CREATE_MAPPED
};
struct Buffer final : Resource {
    VkBuffer Handle;
    VmaAllocation Allocation = nullptr;

    uint64_t Size;
    VkBufferUsageFlags Usage;

    void* MappedData;
    VkDeviceAddress DeviceAddress;

    void Write(const void* src, uint64_t destOffset, size_t byteCount);

    // Needs to be called before reading from a mapped memory for memory types that are not HOST_COHERENT.
    void Invalidate(uint64_t destOffset = 0, uint64_t byteCount = VK_WHOLE_SIZE);

    // Needs to be called after writing to a mapped memory for memory types that are not HOST_COHERENT.
    void Flush(uint64_t destOffset = 0, uint64_t byteCount = VK_WHOLE_SIZE);

    ~Buffer();
};

// Buffer that will be updated by host and read by GPU every frame.
// This is effectively one device buffer and N staging host buffers, where N =frames in flight.
struct TransientBuffer final : Resource {
    // TODO
    // https://edw.is/learning-vulkan/ Handling dynamic data which needs to be uploaded every frame
};

struct ImageDesc {
    VkImageType Type = VK_IMAGE_TYPE_2D;
    VkFormat Format;
    VkSampleCountFlagBits NumSamples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags Usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    uint32_t Width, Height, Depth = 1;
    uint32_t NumLayers = 1;
    uint32_t NumLevels = VK_REMAINING_MIP_LEVELS;
    VkImageViewType ViewType = VK_IMAGE_VIEW_TYPE_MAX_ENUM; // MAX_ENUM = auto
};

struct Image final : Resource {
    VkImage Handle;
    VkImageView ViewHandle;
    VmaAllocation Allocation = nullptr; // Null if this is a swapchain image.
    ImageDesc Desc;

    ImageHandle DescriptorHandle = InvalidHandle;

    // Internal state tracking
    VkImageLayout CurrentLayout_ = VK_IMAGE_LAYOUT_UNDEFINED;

    ~Image() override;
    void Release() override;

    [[nodiscard]]
    Future Upload(const void* data, size_t dataSize, VkRect2D destRect = { 0, 0, ~0u, ~0u },
                  VkImageSubresourceLayers layers = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });

    // Wraps existing VkImage and view. Will take ownership of VkImageView but not of the actual image.
    static ImagePtr WrapSwapchainImage(DeviceContext* ctx, VkImage handle, VkImageView viewHandle, ImageDesc desc);

    static ImagePtr LoadFile(DeviceContext* ctx, std::string_view path, VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT,
                             VkFormat format = VK_FORMAT_R8G8B8A8_SRGB, uint32_t mipLevels = VK_REMAINING_MIP_LEVELS,
                             Future* uploadSync = nullptr);
    static ImagePtr LoadFilePanoramaToCube(DeviceContext* ctx, std::string_view path, VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT,
                                           Future* uploadSync = nullptr);
};

struct Swapchain {
    HAVK_NON_COPYABLE(Swapchain);

    DeviceContext* Context;
    GLFWwindow* Window;
    VkSurfaceKHR Surface = nullptr;
    VkSwapchainKHR Handle = nullptr;

    VkSurfaceCapabilitiesKHR SurfaceCaps = {};
    VkSurfaceFormatKHR SurfaceFormat = {};
    VkPresentModeKHR PresentMode = {};

    Swapchain(DeviceContext* ctx, GLFWwindow* window, VkSurfaceKHR surface);
    ~Swapchain();

    std::pair<Image*, CommandList> AcquireImage();
    void Present();

    // (Re-)creates swapchain and initialize properties.
    // Automatically called by AcquireImage().
    void Initialize();

    uint32_t GetImageCount() const { return _images.size(); }
    VkExtent2D GetSurfaceSize() const {
        auto& desc = _images[0].Target->Desc;
        return { desc.Width, desc.Height };
    }

private:
    struct SwcImage {
        VkSemaphore AvailableSemaphore;
        VkSemaphore RenderFinishedSemaphore;
        VkFence InFlightFence;
        VkCommandBuffer CmdBuffer;

        ImagePtr Target;
    };
    std::vector<SwcImage> _images;
    uint32_t _currSyncIdx = 0, _currImageIdx = 0; // not necessarily the same!

    void ReleaseSwapchain();
    VkExtent2D GetCurrentSurfaceSize();
};
struct AttachmentInfo {
    Image* Target = nullptr;
    VkAttachmentLoadOp LoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    VkAttachmentStoreOp StoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkClearValue ClearValue = { };
};
struct RenderingTarget {
    VkRect2D Region = { 0, 0, 0, 0 };
    std::vector<AttachmentInfo> Attachments;
    AttachmentInfo DepthAttachment;
    AttachmentInfo StencilAttachment;
};

struct UseBarrier {
    VkAccessFlags Access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkPipelineStageFlags Stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    static const UseBarrier ReadOnly, All, ComputeRead, ComputeReadWrite, GraphicsRead, GraphicsReadWrite; 
};
constexpr UseBarrier
    UseBarrier::ReadOnly = { VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT },
    UseBarrier::All = { VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT },
    UseBarrier::ComputeRead = { VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT },
    UseBarrier::ComputeReadWrite = { VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT },
    UseBarrier::GraphicsRead = { VK_ACCESS_MEMORY_READ_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT },
    UseBarrier::GraphicsReadWrite = { VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT }; 

// Wrapper for VkCommandBuffer. Lifetime is managed elsewhere.
struct CommandList {
    HAVK_NON_COPYABLE(CommandList);

    DeviceContext* Context;
    VkCommandBuffer Buffer;
    // Internal
    VkPipeline BoundPipeline_ = nullptr;

    CommandList(DeviceContext* ctx, VkCommandBuffer buffer) {
        Context = ctx;
        Buffer = buffer;
    }

    void BeginRendering(const RenderingTarget& targets, bool setViewport = false);
    void EndRendering() { vkCmdEndRendering(Buffer); }

    void SetViewport(VkViewport vp) { vkCmdSetViewport(Buffer, 0, 1, &vp); }
    void SetScissor(VkRect2D rect) { vkCmdSetScissor(Buffer, 0, 1, &rect); }

    void TransitionLayout(Image& image, VkImageLayout newLayout, VkPipelineStageFlags destStage,
                          VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, bool discardContents = false);

    void Barrier(VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                 VkAccessFlags srcAccess = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                 VkAccessFlags dstAccess = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);

    // Barrier, layout transition, mark use.
    ImageHandle GetDescriptorHandle(Image& image, UseBarrier barrier, VkImageLayout layout = VK_IMAGE_LAYOUT_MAX_ENUM);
    VkDeviceAddress GetDeviceAddress(havk::Buffer& buffer, UseBarrier barrier);

    void UpdateBuffer(havk::Buffer& buffer, VkDeviceSize destOffset, uint32_t dataSize, const void* data) {
        GetDeviceAddress(buffer, { VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT });
        vkCmdUpdateBuffer(Buffer, buffer.Handle, destOffset, dataSize, data);
    }
    void MarkUse(Resource& res) {
        res.LastUseTimestamp = Context->NextQueueTimestamp;
    }
};

struct PushConstantsPtr {
    const void* Ptr = nullptr;
    uint32_t Size = 0;

    template<typename T>
    constexpr PushConstantsPtr(const T& ref) : Ptr(&ref), Size(sizeof(T)) {}

    PushConstantsPtr() = default;
};

struct SamplerDescriptorSet {
    VkDescriptorSet Handle = nullptr;
    uint32_t PoolIdx = 0;
};

struct Pipeline : Resource {
    VkPipeline Handle;
    VkPipelineLayout LayoutHandle;
    SamplerDescriptorSet SamplerDescriptors;

    ~Pipeline();

    void Bind(CommandList& cmdList);

    void Push(CommandList& cmdList, PushConstantsPtr pc) {
        if (pc.Size > 0) {
            vkCmdPushConstants(cmdList.Buffer, LayoutHandle, VK_SHADER_STAGE_ALL, 0, pc.Size, pc.Ptr);
        }
    }

private:
    friend void MoveHandles(Pipeline* dest, Pipeline* src); // hot reload
    
    void Destroy();
};

// Blittable with VkDrawIndirectCommand. Only exists for more sensible defaults and naming.
struct DrawCommand {
    uint32_t NumVertices;
    uint32_t NumInstances = 1;
    uint32_t VertexOffset = 0;
    uint32_t InstanceOffset = 0;
};

// Blittable with VkDrawIndexedIndirectCommand, different strides.
struct DrawIndexedCommand {
    uint32_t NumIndices;
    uint32_t NumInstances = 1;
    uint32_t IndexOffset = 0;
    int32_t VertexOffset = 0;
    uint32_t InstanceOffset = 0;

    VkBuffer IndexBuffer = nullptr;
    VkIndexType IndexType = VK_INDEX_TYPE_UINT16;
};

struct GraphicsPipeline final : Pipeline {
    void Draw(CommandList& cmdList, const DrawCommand& cmd, PushConstantsPtr pc = {}) {
        Bind(cmdList);
        Push(cmdList, pc);
        vkCmdDraw(cmdList.Buffer, cmd.NumVertices, cmd.NumInstances, cmd.VertexOffset, cmd.NumInstances);
    }
    void DrawIndexed(CommandList& cmdList, const DrawIndexedCommand& cmd, PushConstantsPtr pushConstants = {}) {
        Bind(cmdList);
        Push(cmdList, pushConstants);

        vkCmdBindIndexBuffer(cmdList.Buffer, cmd.IndexBuffer, 0, cmd.IndexType);
        vkCmdDrawIndexed(cmdList.Buffer, cmd.NumIndices, cmd.NumInstances, cmd.IndexOffset, cmd.VertexOffset, cmd.InstanceOffset);
    }
};
struct ComputePipeline final : Pipeline {
    void Dispatch(CommandList& cmdList, VkExtent3D groupCount, PushConstantsPtr pc = {}) {
        Bind(cmdList);
        Push(cmdList, pc);
        vkCmdDispatch(cmdList.Buffer, groupCount.width, groupCount.height, groupCount.depth);
    }
};

struct ShaderCompileResult {
    HAVK_NON_COPYABLE(ShaderCompileResult);

    VkDevice Device = nullptr;
    std::vector<VkPipelineShaderStageCreateInfo> Stages;
    VkPipelineLayout Layout = nullptr;
    SamplerDescriptorSet SamplerDescriptors;

    // Diagnostics
    std::string InfoLog;
    bool Success = false;

    std::string SourceFile;
    std::vector<std::string> IncludedFiles;

    ShaderCompileResult(VkDevice device) { Device = device; }
    ~ShaderCompileResult();

    void AppendLog(std::string_view str) {
        if (str.empty()) return;
        InfoLog += str;
        InfoLog += "\n";
    }
};

struct GraphicsPipelineDesc;

struct ShaderCompileParams {
    std::vector<std::pair<std::string, std::string>> PrepDefs; // Pre-processor definitions
    std::string LinkSource;                                    // TODO: Source code of a module used for link-time specialization
};

struct PipelineBuilder {
    HAVK_NON_COPYABLE(PipelineBuilder);

    DeviceContext* Context;
    VkPipelineCache Cache = nullptr;
    std::filesystem::path BasePath;

    PipelineBuilder(DeviceContext* ctx, const std::filesystem::path& basePath, bool enableHotReload);
    ~PipelineBuilder();

    GraphicsPipelinePtr CreateGraphics(const std::string& shaderFilename, const GraphicsPipelineDesc& desc,
                                       const ShaderCompileParams& compilePars = {},
                                       const std::source_location& debugLoc = std::source_location::current());

    ComputePipelinePtr CreateCompute(const std::string& shaderFilename, const ShaderCompileParams& compilePars = {},
                                     const std::source_location& debugLoc = std::source_location::current());

    ShaderCompileResult Compile(std::string_view filename, const ShaderCompileParams& compilePars = {});

private:
    friend DeviceContext;
    friend Pipeline;

    struct HotReloadTracker;
    struct PipelineSourceInfo;

    std::unique_ptr<HotReloadTracker> _reloadTracker = nullptr;
    void* _slangSession = nullptr;  // IGlobalSession* - this is void* to avoid leaking slang.h

    // Check compile result and move handles to pipeline.
    void InitPipeline(Pipeline* pl, ShaderCompileResult& shader,
                      const ShaderCompileParams& compilePars,
                      const GraphicsPipelineDesc* graphicsDesc);

    void Refresh();

    // Called by pipeline destructor.
    void StopTracking(Pipeline* pl);
};

namespace BlendingModes {
    // finalColor = newColor
    constexpr VkPipelineColorBlendAttachmentState None = { .blendEnable = VK_FALSE, .colorWriteMask = 0xF };
    // finalColor.rgb = newAlpha * newColor + (1 - newAlpha) * oldColor
    // finalAlpha.a = srcAlpha
    constexpr VkPipelineColorBlendAttachmentState AlphaComposite = {
        .blendEnable = VK_TRUE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = 0xF,
    };
};  // namespace BlendingModes

// TODO: Consider splitting this stuff, no idea how useable this is.
struct GraphicsPipelineDesc {
    // Rasterizer
    VkPolygonMode PolygonMode = VK_POLYGON_MODE_FILL;
    VkCullModeFlags CullMode =  VK_CULL_MODE_BACK_BIT;
    VkFrontFace FrontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    bool EnableDepthClamp = false;

    // Input Assembly
    VkPrimitiveTopology Topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    bool EnablePrimitiveRestart = false;

    // Multisampling
    VkSampleCountFlagBits RasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    bool EnableSampleShading = false;

    // Depth
    bool EnableDepthTest = true;
    bool EnableDepthWrite = true;
    VkCompareOp DepthCompareOp = VK_COMPARE_OP_LESS;

    // Stencil
    bool EnableStencilTest = false;
    VkStencilOpState StencilFront = {};
    VkStencilOpState StencilBack = {};

    // Color Blending
    bool EnableColorLogicOp = false;
    VkLogicOp ColorLogicOp = VK_LOGIC_OP_COPY;
    std::vector<VkPipelineColorBlendAttachmentState> BlendStates; // Optional. Defaults to BlendingModes::None for all outputs.
    float BlendConstants[4] = { 0, 0, 0, 0 };

    // Outputs (attachments)
    std::vector<VkFormat> OutputFormats;
    VkFormat DepthFormat = VK_FORMAT_UNDEFINED;
    VkFormat StencilFormat = VK_FORMAT_UNDEFINED;
};

// Global functions

#define VK_CHECK(callExpr)                              \
    if (auto vkr__ = (callExpr); vkr__ != VK_SUCCESS) { \
        havk::ThrowResult(vkr__, #callExpr);             \
    }
void ThrowResult(VkResult result, const char* msg);

};  // namespace havk