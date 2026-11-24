/*
Copyright (c) 2026 Jørgen Lind

  Permission is hereby granted, free of charge, to any person obtaining a copy of
  this software and associated documentation files (the "Software"), to deal in
  the Software without restriction, including without limitation the rights to
  use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
  of the Software, and to permit persons to whom the Software is furnished to do
  so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
  THE SOFTWARE.
*/

// The WebGPU renderer (WEB_PLAN.md, part 3), on webgpu.h. In the browser it is emdawnwebgpu over
// navigator.gpu; built against Dawn it will be the native one too. It draws what the Vulkan renderer
// draws, with the same shader translated to WGSL: one instanced quad per prim, one draw per run.
// WebGPU has no push constants, so each run's constants sit in their own 256-byte slot of a uniform
// buffer, bound at a dynamic offset. The canvas offers no sRGB format, so the shader encodes.

#include "ui_renderer.h"

#if defined(__EMSCRIPTEN__)

#include <algorithm>
#include <cmath>
#include <coroutine>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <webgpu/webgpu.h>

#include "shaders/ui_wgsl.h"

namespace vui::ui::detail
{
namespace
{
WGPUStringView view_of(const char *text)
{
  return WGPUStringView{text, WGPU_STRLEN};
}

std::string text_of(WGPUStringView view)
{
  if (!view.data)
    return {};
  return view.length == WGPU_STRLEN ? std::string(view.data) : std::string(view.data, view.length);
}

// The shader's Push block: what the Vulkan renderer pushes (ui_vulkan.cpp, push_t).
struct push_t
{
  float origin[2];
  float scale[2];
  float target[2];
  float encode_srgb;
  float zoom;
};
static_assert(sizeof(push_t) == 32);
constexpr std::uint64_t push_stride = 256;

// A WebGPU request that answers through a callback, awaited on the loop: the callback may come at
// any time, from the browser's own event loop, so it posts the resumption to the vio loop.
template <typename T>
struct answer_t
{
  vio::event_loop_t *loop = nullptr;
  std::optional<T> value;
  std::string message;
  std::coroutine_handle<> waiting;

  [[nodiscard]] bool await_ready() const noexcept
  {
    return value.has_value();
  }
  void await_suspend(std::coroutine_handle<> handle) noexcept
  {
    waiting = handle;
  }
  T await_resume()
  {
    return std::move(*value);
  }
  void arrive(T result, std::string text)
  {
    value = std::move(result);
    message = std::move(text);
    if (auto handle = std::exchange(waiting, {}))
      loop->run_in_loop([handle] { handle.resume(); });
  }
};

class webgpu_renderer_t final : public renderer_t
{
public:
  ~webgpu_renderer_t() override
  {
    release_textures();
    if (_bind_group)
      wgpuBindGroupRelease(_bind_group);
    if (_prims)
      wgpuBufferRelease(_prims);
    if (_pushes)
      wgpuBufferRelease(_pushes);
    if (_sampler)
      wgpuSamplerRelease(_sampler);
    if (_pipeline)
      wgpuRenderPipelineRelease(_pipeline);
    if (_layout)
      wgpuPipelineLayoutRelease(_layout);
    if (_group_layout)
      wgpuBindGroupLayoutRelease(_group_layout);
    if (_queue)
      wgpuQueueRelease(_queue);
    if (_surface)
      wgpuSurfaceRelease(_surface);
    if (_device)
      wgpuDeviceRelease(_device);
    if (_adapter)
      wgpuAdapterRelease(_adapter);
    if (_instance)
      wgpuInstanceRelease(_instance);
  }

  vio::task_t<bool> create(window_t &window, std::string &why)
  {
    auto &loop = *window.impl()->display.loop;
    _instance = wgpuCreateInstance(nullptr);
    if (!_instance)
    {
      why = "WebGPU is not available in this browser";
      co_return false;
    }
    const auto native = window.native();
    _selector = static_cast<const char *>(native.handle);
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas{};
    canvas.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
    canvas.selector = view_of(_selector.c_str());
    WGPUSurfaceDescriptor surface_info{};
    surface_info.nextInChain = &canvas.chain;
    _surface = wgpuInstanceCreateSurface(_instance, &surface_info);
    if (!_surface)
    {
      why = "no WebGPU surface for " + _selector;
      co_return false;
    }

    answer_t<WGPUAdapter> adapter{&loop};
    WGPURequestAdapterOptions options{};
    options.featureLevel = WGPUFeatureLevel_Core;
    options.powerPreference = WGPUPowerPreference_HighPerformance;
    options.compatibleSurface = _surface;
    WGPURequestAdapterCallbackInfo adapter_callback{};
    adapter_callback.mode = WGPUCallbackMode_AllowSpontaneous;
    adapter_callback.callback = [](WGPURequestAdapterStatus status, WGPUAdapter made, WGPUStringView message, void *answer, void *) {
      static_cast<answer_t<WGPUAdapter> *>(answer)->arrive(status == WGPURequestAdapterStatus_Success ? made : nullptr, text_of(message));
    };
    adapter_callback.userdata1 = &adapter;
    wgpuInstanceRequestAdapter(_instance, &options, adapter_callback);
    _adapter = co_await adapter;
    if (!_adapter)
    {
      why = "no WebGPU adapter: " + adapter.message;
      co_return false;
    }

    WGPUAdapterInfo info{};
    if (wgpuAdapterGetInfo(_adapter, &info) == WGPUStatus_Success)
    {
      _description = text_of(info.description);
      if (_description.empty())
        _description = text_of(info.vendor) + " " + text_of(info.architecture);
      wgpuAdapterInfoFreeMembers(info);
    }

    answer_t<WGPUDevice> device{&loop};
    WGPUDeviceDescriptor device_info{};
    device_info.label = view_of("vui");
    device_info.uncapturedErrorCallbackInfo.callback = [](WGPUDevice const *, WGPUErrorType type, WGPUStringView message, void *, void *) {
      std::fprintf(stderr, "vui: WebGPU error %d: %s\n", int(type), text_of(message).c_str());
    };
    device_info.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
    device_info.deviceLostCallbackInfo.callback = [](WGPUDevice const *, WGPUDeviceLostReason reason, WGPUStringView message, void *self, void *) {
      if (reason != WGPUDeviceLostReason_Destroyed && reason != WGPUDeviceLostReason_CallbackCancelled)
        std::fprintf(stderr, "vui: WebGPU device lost (%d): %s\n", int(reason), text_of(message).c_str());
      static_cast<webgpu_renderer_t *>(self)->_lost = true;
    };
    device_info.deviceLostCallbackInfo.userdata1 = this;
    WGPURequestDeviceCallbackInfo device_callback{};
    device_callback.mode = WGPUCallbackMode_AllowSpontaneous;
    device_callback.callback = [](WGPURequestDeviceStatus status, WGPUDevice made, WGPUStringView message, void *answer, void *) {
      static_cast<answer_t<WGPUDevice> *>(answer)->arrive(status == WGPURequestDeviceStatus_Success ? made : nullptr, text_of(message));
    };
    device_callback.userdata1 = &device;
    wgpuAdapterRequestDevice(_adapter, &device_info, device_callback);
    _device = co_await device;
    if (!_device)
    {
      why = "no WebGPU device: " + device.message;
      co_return false;
    }
    _queue = wgpuDeviceGetQueue(_device);

    WGPUSurfaceCapabilities capabilities{};
    if (wgpuSurfaceGetCapabilities(_surface, _adapter, &capabilities) != WGPUStatus_Success || capabilities.formatCount == 0)
    {
      why = "the canvas offers no format";
      co_return false;
    }
    _format = capabilities.formats[0];
    wgpuSurfaceCapabilitiesFreeMembers(capabilities);
    // The canvas formats are UNORM; an sRGB one, should a browser offer it, needs no encoding.
    _encode_srgb = _format != WGPUTextureFormat_BGRA8UnormSrgb && _format != WGPUTextureFormat_RGBA8UnormSrgb;

    if (!make_pipeline(why))
      co_return false;
    co_return true;
  }

  bool render(const display_list_t &list) override
  {
    if (_lost)
      return false;
    const extent_t target = list.framebuffer;
    if (target.width <= 0 || target.height <= 0)
      return true;
    if (target != _configured)
    {
      WGPUSurfaceConfiguration config{};
      config.device = _device;
      config.format = _format;
      config.usage = WGPUTextureUsage_RenderAttachment;
      config.width = std::uint32_t(target.width);
      config.height = std::uint32_t(target.height);
      config.alphaMode = WGPUCompositeAlphaMode_Opaque;
      config.presentMode = WGPUPresentMode_Fifo;
      wgpuSurfaceConfigure(_surface, &config);
      _configured = target;
    }
    WGPUSurfaceTexture current{};
    wgpuSurfaceGetCurrentTexture(_surface, &current);
    if (current.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal && current.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal)
    {
      if (current.texture)
        wgpuTextureRelease(current.texture);
      _configured = {};
      return true;
    }
    if (!upload_atlas(_coverage, list.atlas, WGPUTextureFormat_R8Unorm) || !upload_atlas(_images, list.images, WGPUTextureFormat_RGBA8UnormSrgb))
    {
      wgpuTextureRelease(current.texture);
      return false;
    }
    if (!_bind_group && (!ensure_pushes(1) || !bind()))
      return false;

    // Prims and each run's constants, in two writes.
    const std::uint64_t prim_bytes = list.prims.size_bytes();
    if (prim_bytes > 0)
    {
      if (!ensure_prims(prim_bytes))
        return false;
      wgpuQueueWriteBuffer(_queue, _prims, 0, list.prims.data(), prim_bytes);
    }
    const float scale_x = list.logical.width > 0 ? float(target.width) / float(list.logical.width) : list.scale;
    const float scale_y = list.logical.height > 0 ? float(target.height) / float(list.logical.height) : list.scale;
    if (!list.runs.empty())
    {
      if (!ensure_pushes(list.runs.size()))
        return false;
      _push_bytes.assign(list.runs.size() * push_stride, 0);
      for (std::size_t i = 0; i < list.runs.size(); ++i)
      {
        const auto &run = list.runs[i];
        push_t push{};
        push.origin[0] = run.x;
        push.origin[1] = run.y;
        push.scale[0] = scale_x;
        push.scale[1] = scale_y;
        push.target[0] = float(target.width);
        push.target[1] = float(target.height);
        push.encode_srgb = _encode_srgb ? 1.0f : 0.0f;
        push.zoom = run.scale;
        std::memcpy(&_push_bytes[i * push_stride], &push, sizeof(push));
      }
      wgpuQueueWriteBuffer(_queue, _pushes, 0, _push_bytes.data(), _push_bytes.size());
    }

    WGPUTextureView view = wgpuTextureCreateView(current.texture, nullptr);
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(_device, nullptr);
    WGPURenderPassColorAttachment attachment{};
    attachment.view = view;
    attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    attachment.loadOp = WGPULoadOp_Clear;
    attachment.storeOp = WGPUStoreOp_Store;
    const auto &bg = list.background;
    attachment.clearValue = _encode_srgb ? WGPUColor{bg.r, bg.g, bg.b, bg.a} : WGPUColor{to_linear(bg.r), to_linear(bg.g), to_linear(bg.b), bg.a};
    WGPURenderPassDescriptor pass_info{};
    pass_info.colorAttachmentCount = 1;
    pass_info.colorAttachments = &attachment;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_info);
    if (!list.runs.empty() && prim_bytes > 0)
    {
      wgpuRenderPassEncoderSetPipeline(pass, _pipeline);
      wgpuRenderPassEncoderSetVertexBuffer(pass, 0, _prims, 0, prim_bytes);
      for (std::size_t i = 0; i < list.runs.size(); ++i)
      {
        const auto &run = list.runs[i];
        // A clipping box's cut, in pixels and inside the framebuffer (G50).
        const float x0 = std::clamp(std::floor(run.clip[0] * scale_x), 0.0f, float(target.width));
        const float y0 = std::clamp(std::floor(run.clip[1] * scale_y), 0.0f, float(target.height));
        const float x1 = std::clamp(std::ceil(run.clip[2] * scale_x), 0.0f, float(target.width));
        const float y1 = std::clamp(std::ceil(run.clip[3] * scale_y), 0.0f, float(target.height));
        if (x1 <= x0 || y1 <= y0 || run.count == 0)
          continue;
        wgpuRenderPassEncoderSetScissorRect(pass, std::uint32_t(x0), std::uint32_t(y0), std::uint32_t(x1 - x0), std::uint32_t(y1 - y0));
        const std::uint32_t offset = std::uint32_t(i * push_stride);
        wgpuRenderPassEncoderSetBindGroup(pass, 0, _bind_group, 1, &offset);
        wgpuRenderPassEncoderDraw(pass, 6, run.count, 0, run.first);
      }
    }
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuQueueSubmit(_queue, 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
    wgpuTextureViewRelease(view);
    wgpuTextureRelease(current.texture);
    // The canvas shows what was drawn when the browser's task ends; there is nothing to present.
    return true;
  }

  [[nodiscard]] std::string describe() const override
  {
    return "webgpu: " + (_description.empty() ? std::string("browser adapter") : _description);
  }

private:
  struct texture_t
  {
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
    extent_t extent;
    std::uint64_t generation = 0;
    bool current = false;
  };

  static double to_linear(float c)
  {
    return c <= 0.04045f ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
  }

  bool make_pipeline(std::string &why)
  {
    WGPUShaderSourceWGSL source{};
    source.chain.sType = WGPUSType_ShaderSourceWGSL;
    source.code = view_of(shaders::ui_wgsl);
    WGPUShaderModuleDescriptor module_info{};
    module_info.nextInChain = &source.chain;
    module_info.label = view_of("ui.slang");
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(_device, &module_info);
    if (!module)
    {
      why = "the WGSL shader did not compile";
      return false;
    }

    WGPUBindGroupLayoutEntry entries[4]{};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    entries[0].texture.sampleType = WGPUTextureSampleType_Float;
    entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    entries[1].texture.sampleType = WGPUTextureSampleType_Float;
    entries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    entries[2].sampler.type = WGPUSamplerBindingType_Filtering;
    entries[3].binding = 3;
    entries[3].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    entries[3].buffer.type = WGPUBufferBindingType_Uniform;
    entries[3].buffer.hasDynamicOffset = true;
    entries[3].buffer.minBindingSize = sizeof(push_t);
    WGPUBindGroupLayoutDescriptor group_info{};
    group_info.entryCount = 4;
    group_info.entries = entries;
    _group_layout = wgpuDeviceCreateBindGroupLayout(_device, &group_info);
    WGPUPipelineLayoutDescriptor layout_info{};
    layout_info.bindGroupLayoutCount = 1;
    layout_info.bindGroupLayouts = &_group_layout;
    _layout = wgpuDeviceCreatePipelineLayout(_device, &layout_info);

    WGPUVertexAttribute attributes[6]{};
    attributes[0] = {nullptr, WGPUVertexFormat_Float32x4, offsetof(prim_t, x), 0};
    attributes[1] = {nullptr, WGPUVertexFormat_Float32x4, offsetof(prim_t, radii), 1};
    attributes[2] = {nullptr, WGPUVertexFormat_Float32x4, offsetof(prim_t, fill), 2};
    attributes[3] = {nullptr, WGPUVertexFormat_Float32x4, offsetof(prim_t, border_color), 3};
    attributes[4] = {nullptr, WGPUVertexFormat_Float32x2, offsetof(prim_t, border_width), 4};
    attributes[5] = {nullptr, WGPUVertexFormat_Uint32, offsetof(prim_t, kind), 5};
    WGPUVertexBufferLayout buffer{};
    buffer.stepMode = WGPUVertexStepMode_Instance;
    buffer.arrayStride = sizeof(prim_t);
    buffer.attributeCount = 6;
    buffer.attributes = attributes;

    // Premultiplied alpha: the shader multiplies, the blender only composites.
    WGPUBlendState blend{};
    blend.color = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha};
    blend.alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_OneMinusSrcAlpha};
    WGPUColorTargetState color{};
    color.format = _format;
    color.blend = &blend;
    color.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fragment{};
    fragment.module = module;
    fragment.entryPoint = view_of("fragmentMain");
    fragment.targetCount = 1;
    fragment.targets = &color;

    WGPURenderPipelineDescriptor pipeline_info{};
    pipeline_info.label = view_of("vui prims");
    pipeline_info.layout = _layout;
    pipeline_info.vertex.module = module;
    pipeline_info.vertex.entryPoint = view_of("vertexMain");
    pipeline_info.vertex.bufferCount = 1;
    pipeline_info.vertex.buffers = &buffer;
    pipeline_info.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pipeline_info.primitive.frontFace = WGPUFrontFace_CCW;
    pipeline_info.primitive.cullMode = WGPUCullMode_None;
    pipeline_info.multisample.count = 1;
    pipeline_info.multisample.mask = 0xFFFFFFFFu;
    pipeline_info.fragment = &fragment;
    _pipeline = wgpuDeviceCreateRenderPipeline(_device, &pipeline_info);
    wgpuShaderModuleRelease(module);
    if (!_pipeline)
    {
      why = "the render pipeline could not be made";
      return false;
    }

    WGPUSamplerDescriptor sampler_info{};
    sampler_info.addressModeU = WGPUAddressMode_ClampToEdge;
    sampler_info.addressModeV = WGPUAddressMode_ClampToEdge;
    sampler_info.addressModeW = WGPUAddressMode_ClampToEdge;
    sampler_info.magFilter = WGPUFilterMode_Linear;
    sampler_info.minFilter = WGPUFilterMode_Linear;
    sampler_info.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    sampler_info.lodMinClamp = 0;
    sampler_info.lodMaxClamp = 32;
    sampler_info.maxAnisotropy = 1;
    _sampler = wgpuDeviceCreateSampler(_device, &sampler_info);
    return true;
  }

  // The atlas's texture as `atlas` needs it, made again if it grew, and what changed written in.
  bool upload_atlas(texture_t &t, const atlas_view_t &atlas, WGPUTextureFormat format)
  {
    const bool empty = atlas.extent.width <= 0 || atlas.extent.height <= 0 || atlas.pixels.empty();
    const extent_t wanted = empty ? extent_t{1, 1} : atlas.extent;
    bool whole = !t.current || atlas.generation != t.generation;
    if (!t.texture || wanted != t.extent)
    {
      if (t.view)
        wgpuTextureViewRelease(t.view);
      if (t.texture)
      {
        wgpuTextureDestroy(t.texture);
        wgpuTextureRelease(t.texture);
      }
      WGPUTextureDescriptor info{};
      info.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
      info.dimension = WGPUTextureDimension_2D;
      info.size = {std::uint32_t(wanted.width), std::uint32_t(wanted.height), 1};
      info.format = format;
      info.mipLevelCount = 1;
      info.sampleCount = 1;
      t.texture = wgpuDeviceCreateTexture(_device, &info);
      if (!t.texture)
        return false;
      t.view = wgpuTextureCreateView(t.texture, nullptr);
      t.extent = wanted;
      whole = true;
      // The bind group named the old texture: make it again before drawing.
      if (_bind_group)
      {
        wgpuBindGroupRelease(_bind_group);
        _bind_group = nullptr;
      }
    }
    t.current = true;
    t.generation = atlas.generation;
    if (empty)
      return true;
    const rect_t region = whole ? rect_t{0, 0, wanted.width, wanted.height} : atlas.dirty;
    if (region.width <= 0 || region.height <= 0)
      return true;
    // Written straight from the atlas's own rows: writeTexture has no row alignment to meet.
    const std::size_t texel = std::size_t(std::max(atlas.bytes_per_texel, 1));
    const std::size_t row = std::size_t(atlas.extent.width) * texel;
    const std::size_t start = std::size_t(region.y) * row + std::size_t(region.x) * texel;
    const std::size_t bytes = std::size_t(region.height - 1) * row + std::size_t(region.width) * texel;
    WGPUTexelCopyTextureInfo destination{};
    destination.texture = t.texture;
    destination.origin = {std::uint32_t(region.x), std::uint32_t(region.y), 0};
    destination.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferLayout layout{};
    layout.bytesPerRow = std::uint32_t(row);
    layout.rowsPerImage = std::uint32_t(region.height);
    const WGPUExtent3D size{std::uint32_t(region.width), std::uint32_t(region.height), 1};
    wgpuQueueWriteTexture(_queue, &destination, atlas.pixels.data() + start, bytes, &layout, &size);
    return true;
  }

  bool ensure_prims(std::uint64_t bytes)
  {
    if (_prims && _prims_capacity >= bytes)
      return true;
    if (_prims)
    {
      wgpuBufferDestroy(_prims);
      wgpuBufferRelease(_prims);
    }
    _prims_capacity = 64 * 1024;
    while (_prims_capacity < bytes)
      _prims_capacity *= 2;
    WGPUBufferDescriptor info{};
    info.usage = WGPUBufferUsage_Vertex | WGPUBufferUsage_CopyDst;
    info.size = _prims_capacity;
    _prims = wgpuDeviceCreateBuffer(_device, &info);
    return _prims != nullptr;
  }

  bool ensure_pushes(std::size_t runs)
  {
    const std::uint64_t bytes = std::uint64_t(runs) * push_stride;
    if (_pushes && _pushes_capacity >= bytes)
      return true;
    if (_pushes)
    {
      wgpuBufferDestroy(_pushes);
      wgpuBufferRelease(_pushes);
    }
    _pushes_capacity = 64 * push_stride;
    while (_pushes_capacity < bytes)
      _pushes_capacity *= 2;
    WGPUBufferDescriptor info{};
    info.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    info.size = _pushes_capacity;
    _pushes = wgpuDeviceCreateBuffer(_device, &info);
    return _pushes != nullptr && bind();
  }

  bool bind()
  {
    if (!_coverage.view || !_images.view || !_pushes)
      return true;
    if (_bind_group)
      wgpuBindGroupRelease(_bind_group);
    WGPUBindGroupEntry entries[4]{};
    entries[0].binding = 0;
    entries[0].textureView = _coverage.view;
    entries[1].binding = 1;
    entries[1].textureView = _images.view;
    entries[2].binding = 2;
    entries[2].sampler = _sampler;
    entries[3].binding = 3;
    entries[3].buffer = _pushes;
    entries[3].size = sizeof(push_t);
    WGPUBindGroupDescriptor info{};
    info.layout = _group_layout;
    info.entryCount = 4;
    info.entries = entries;
    _bind_group = wgpuDeviceCreateBindGroup(_device, &info);
    return _bind_group != nullptr;
  }

  void release_textures()
  {
    for (auto *t : {&_coverage, &_images})
    {
      if (t->view)
        wgpuTextureViewRelease(t->view);
      if (t->texture)
        wgpuTextureRelease(t->texture);
      *t = {};
    }
  }

  WGPUInstance _instance = nullptr;
  WGPUSurface _surface = nullptr;
  WGPUAdapter _adapter = nullptr;
  WGPUDevice _device = nullptr;
  WGPUQueue _queue = nullptr;
  WGPUBindGroupLayout _group_layout = nullptr;
  WGPUPipelineLayout _layout = nullptr;
  WGPURenderPipeline _pipeline = nullptr;
  WGPUSampler _sampler = nullptr;
  WGPUBuffer _prims = nullptr;
  std::uint64_t _prims_capacity = 0;
  WGPUBuffer _pushes = nullptr;
  std::uint64_t _pushes_capacity = 0;
  std::vector<std::uint8_t> _push_bytes;
  WGPUBindGroup _bind_group = nullptr;
  texture_t _coverage;
  texture_t _images;
  WGPUTextureFormat _format = WGPUTextureFormat_BGRA8Unorm;
  extent_t _configured;
  std::string _selector;
  std::string _description;
  bool _encode_srgb = true;
  bool _lost = false;
};
} // namespace

vio::task_t<std::unique_ptr<renderer_t>> make_webgpu_renderer(window_t &window, std::string *why)
{
  auto renderer = std::make_unique<webgpu_renderer_t>();
  std::string reason;
  if (!co_await renderer->create(window, reason))
  {
    *why = reason;
    co_return nullptr;
  }
  co_return renderer;
}

// No Vulkan in a browser: the toolkit asks, and is told so.
std::unique_ptr<renderer_t> make_vulkan_renderer(window_t &, const std::optional<vulkan_device_t> &, std::string &why)
{
  why = "no Vulkan in a browser";
  return nullptr;
}

std::optional<offscreen_image_t> render_offscreen_vulkan(std::span<const display_list_t>, std::string &why)
{
  why = "no Vulkan in a browser";
  return std::nullopt;
}
} // namespace vui::ui::detail

#else

namespace vui::ui::detail
{
vio::task_t<std::unique_ptr<renderer_t>> make_webgpu_renderer(window_t &, std::string *why)
{
  // Native WebGPU through Dawn is phase A1 of WEB_PLAN.md.
  *why = "this build has no WebGPU";
  co_return nullptr;
}
} // namespace vui::ui::detail

#endif
